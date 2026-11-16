// Standalone JSON encode benchmark for glazer's C++ encoder, without the BEAM.
//
// Builds Erlang-like terms (cons cells for lists, flat maps, binaries, atoms,
// integers, floats) from a JSON file and links an in-memory model of the
// erl_nif term API that the encoder calls. Terms are pointers to nodes, so the
// encoder walks lists, maps and atoms the way it does in the VM, with each API
// call costing a function call and a few loads. The encoder itself is the
// real JSONEncoder from glazer_json.hpp.
//
// Build and run (development tool; not part of the NIF):
//   g++ -std=c++2b -O3 -march=native -DNDEBUG -fPIC -Ic_src -I$ERTS_INCLUDE \
//       c_src/tools/json_encode_bench.cpp c_src/glazer_atoms.cpp -o json_encode_bench
//   PARSE_ITERS=N ./json_encode_bench FILE...   (fixed count, for callgrind)
//   ./json_encode_bench FILE...                 (timed, min of rounds)
//
// The budget is unlimited here, so the scheduler split (inline, then dirty) is
// not modeled; the encoder itself is the same code path either way.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "glazer_json.hpp"

namespace {

// ---------------------------------------------------------------------------
// Term model
// ---------------------------------------------------------------------------

enum class Kind : uint8_t { Int, Dbl, Bin, Atom, Nil, Cons, Map };

struct Node {
  Kind kind;
  int64_t i = 0;
  double d = 0;
  std::string s;                       // Bin contents, or Atom name
  Node* head = nullptr;                // Cons
  Node* tail = nullptr;                // Cons
  std::vector<Node*> keys, vals;       // Map (flat, insertion order)
};

// Arena of nodes for one harness run; terms stay valid for the process lifetime.
std::vector<std::unique_ptr<Node>> g_nodes;
std::map<std::string, Node*> g_atoms;
Node* g_nil = nullptr;  // the empty list, one shared node (set in main)

Node* mk(Kind k) {
  g_nodes.emplace_back(new Node{k});
  return g_nodes.back().get();
}
Node* atom(const std::string& name) {
  auto it = g_atoms.find(name);
  if (it != g_atoms.end()) return it->second;
  Node* n = mk(Kind::Atom);
  n->s = name;
  g_atoms[name] = n;
  return n;
}
inline ERL_NIF_TERM T(Node* n) { return static_cast<ERL_NIF_TERM>(reinterpret_cast<uintptr_t>(n)); }
inline Node* N(ERL_NIF_TERM t) { return reinterpret_cast<Node*>(static_cast<uintptr_t>(t)); }

// Bump arena for binaries the encoder creates (enif_make_new_binary).
char*  g_out = nullptr;
size_t g_out_used = 0;
constexpr size_t OUT_BYTES = size_t(16) << 20;

// ---------------------------------------------------------------------------
// JSON -> term
// ---------------------------------------------------------------------------

struct Builder {
  const char* p;
  const char* e;
  void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
  Node* value() {
    ws();
    if (*p == '{') {
      ++p;
      Node* m = mk(Kind::Map);
      ws();
      if (*p == '}') { ++p; return m; }
      for (;;) {
        ws();
        Node* k = string_node();
        ws(); ++p;  // ':'
        Node* v = value();
        m->keys.push_back(k);
        m->vals.push_back(v);
        ws();
        if (*p == ',') { ++p; continue; }
        ++p;  // '}'
        return m;
      }
    }
    if (*p == '[') {
      ++p;
      std::vector<Node*> items;
      ws();
      if (*p == ']') { ++p; return nil(); }
      for (;;) {
        items.push_back(value());
        ws();
        if (*p == ',') { ++p; continue; }
        ++p;  // ']'
        break;
      }
      Node* list = nil();
      for (auto it = items.rbegin(); it != items.rend(); ++it) {
        Node* c = mk(Kind::Cons);
        c->head = *it;
        c->tail = list;
        list = c;
      }
      return list;
    }
    if (*p == '"') return string_node();
    if (std::strncmp(p, "true", 4) == 0) { p += 4; return atom("true"); }
    if (std::strncmp(p, "false", 5) == 0) { p += 5; return atom("false"); }
    if (std::strncmp(p, "null", 4) == 0) { p += 4; return atom("null"); }
    const char* start = p;
    bool isf = false;
    if (*p == '-') ++p;
    while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
      if (*p == '.' || *p == 'e' || *p == 'E') isf = true;
      ++p;
    }
    std::string tok(start, p);
    Node* n;
    if (isf) { n = mk(Kind::Dbl); n->d = std::strtod(tok.c_str(), nullptr); }
    else { n = mk(Kind::Int); n->i = std::strtoll(tok.c_str(), nullptr, 10); }
    return n;
  }
  Node* nil() { return g_nil; }
  Node* string_node() {
    ++p;  // opening quote
    std::string out;
    while (p < e && *p != '"') {
      if (*p == '\\') {  // decode the escapes the encoder can meet in these files
        ++p;
        switch (*p) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            unsigned cp = static_cast<unsigned>(std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16));
            p += 4;
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            break;
          }
          default: out += *p;
        }
        ++p;
      } else {
        out += *p++;
      }
    }
    ++p;  // closing quote
    Node* n = mk(Kind::Bin);
    n->s = std::move(out);
    return n;
  }
};

// ---------------------------------------------------------------------------
// erl_nif stubs over the model
// ---------------------------------------------------------------------------

}  // namespace

extern "C" {

ErlNifEnv* g_env_dummy = nullptr;

ERL_NIF_TERM enif_make_atom(ErlNifEnv*, const char* name) { return T(atom(name)); }
ERL_NIF_TERM enif_make_int64(ErlNifEnv*, ErlNifSInt64 v) { Node* n = mk(Kind::Int); n->i = v; return T(n); }
ERL_NIF_TERM enif_make_uint64(ErlNifEnv*, ErlNifUInt64 v) { Node* n = mk(Kind::Int); n->i = static_cast<int64_t>(v); return T(n); }
ERL_NIF_TERM enif_make_double(ErlNifEnv*, double v) { Node* n = mk(Kind::Dbl); n->d = v; return T(n); }
ERL_NIF_TERM enif_make_int(ErlNifEnv*, int v) { Node* n = mk(Kind::Int); n->i = v; return T(n); }
ERL_NIF_TERM enif_make_new_map(ErlNifEnv*) { return T(mk(Kind::Map)); }
int enif_make_map_from_arrays(ErlNifEnv*, ERL_NIF_TERM* keys, ERL_NIF_TERM* vals, size_t n, ERL_NIF_TERM* out) {
  Node* m = mk(Kind::Map);
  for (size_t i = 0; i < n; ++i) { m->keys.push_back(N(keys[i])); m->vals.push_back(N(vals[i])); }
  *out = T(m);
  return 1;
}
int enif_make_map_put(ErlNifEnv*, ERL_NIF_TERM map, ERL_NIF_TERM k, ERL_NIF_TERM v, ERL_NIF_TERM* out) {
  Node* m = mk(Kind::Map);
  Node* src = N(map);
  m->keys = src->keys; m->vals = src->vals;
  m->keys.push_back(N(k)); m->vals.push_back(N(v));
  *out = T(m);
  return 1;
}
ERL_NIF_TERM enif_make_list_from_array(ErlNifEnv*, const ERL_NIF_TERM* items, unsigned n) {
  Node* list = g_nil;
  for (unsigned i = n; i-- > 0;) { Node* c = mk(Kind::Cons); c->head = N(items[i]); c->tail = list; list = c; }
  return T(list);
}
ERL_NIF_TERM enif_make_list(ErlNifEnv*, unsigned, ...) { return T(g_nil); }
ERL_NIF_TERM enif_make_tuple(ErlNifEnv*, unsigned, ...) { return T(mk(Kind::Map)); }
ERL_NIF_TERM enif_make_sub_binary(ErlNifEnv*, ERL_NIF_TERM, size_t, size_t) { return T(mk(Kind::Bin)); }
ERL_NIF_TERM enif_make_badarg(ErlNifEnv*) { return T(atom("badarg")); }
ERL_NIF_TERM enif_make_binary(ErlNifEnv*, ErlNifBinary*) { return T(mk(Kind::Bin)); }
unsigned char* enif_make_new_binary(ErlNifEnv*, size_t size, ERL_NIF_TERM* term) {
  // A fresh allocation per binary, as the VM makes large binaries. (A reused
  // arena would hide the cost of the result's memory.)
  *term = T(mk(Kind::Bin));
  return static_cast<unsigned char*>(std::malloc(size ? size : 1));
}
int enif_inspect_binary(ErlNifEnv*, ERL_NIF_TERM t, ErlNifBinary* bin) {
  Node* n = N(t);
  if (n->kind != Kind::Bin) return 0;
  bin->size = n->s.size();
  bin->data = reinterpret_cast<unsigned char*>(n->s.data());
  return 1;
}
int enif_get_int64(ErlNifEnv*, ERL_NIF_TERM t, ErlNifSInt64* ip) {
  Node* n = N(t);
  if (n->kind != Kind::Int) return 0;
  *ip = n->i;
  return 1;
}
int enif_get_uint64(ErlNifEnv*, ERL_NIF_TERM t, ErlNifUInt64* ip) {
  Node* n = N(t);
  if (n->kind != Kind::Int || n->i < 0) return 0;
  *ip = static_cast<ErlNifUInt64>(n->i);
  return 1;
}
int enif_get_double(ErlNifEnv*, ERL_NIF_TERM t, double* dp) {
  Node* n = N(t);
  if (n->kind != Kind::Dbl) return 0;
  *dp = n->d;
  return 1;
}
int enif_is_identical(ERL_NIF_TERM a, ERL_NIF_TERM b) { return a == b; }
int enif_get_atom_length(ErlNifEnv*, ERL_NIF_TERM t, unsigned* len, ErlNifCharEncoding) {
  Node* n = N(t);
  if (n->kind != Kind::Atom) return 0;
  *len = static_cast<unsigned>(n->s.size());
  return 1;
}
int enif_get_atom(ErlNifEnv*, ERL_NIF_TERM t, char* buf, unsigned len, ErlNifCharEncoding) {
  Node* n = N(t);
  if (n->kind != Kind::Atom || len < n->s.size() + 1) return 0;
  std::memcpy(buf, n->s.data(), n->s.size());
  buf[n->s.size()] = 0;
  return static_cast<int>(n->s.size() + 1);
}
int enif_get_list_length(ErlNifEnv*, ERL_NIF_TERM t, unsigned* len) {
  Node* n = N(t);
  unsigned k = 0;
  while (n->kind == Kind::Cons) { ++k; n = n->tail; }
  if (n->kind != Kind::Nil) return 0;
  *len = k;
  return 1;
}
int enif_get_list_cell(ErlNifEnv*, ERL_NIF_TERM t, ERL_NIF_TERM* head, ERL_NIF_TERM* tail) {
  Node* n = N(t);
  if (n->kind != Kind::Cons) return 0;
  *head = T(n->head);
  *tail = T(n->tail);
  return 1;
}
int enif_is_empty_list(ErlNifEnv*, ERL_NIF_TERM t) { return N(t)->kind == Kind::Nil; }
int enif_get_map_size(ErlNifEnv*, ERL_NIF_TERM t, size_t* size) {
  Node* n = N(t);
  if (n->kind != Kind::Map) return 0;
  *size = n->keys.size();
  return 1;
}
int enif_map_iterator_create(ErlNifEnv*, ERL_NIF_TERM map, ErlNifMapIterator* iter, ErlNifMapIteratorEntry) {
  Node* n = N(map);
  if (n->kind != Kind::Map) return 0;
  iter->idx = 0;
  iter->u.flat.ks = reinterpret_cast<ERL_NIF_TERM*>(n->keys.data());
  iter->u.flat.vs = reinterpret_cast<ERL_NIF_TERM*>(n->vals.data());
  iter->__spare__[0] = reinterpret_cast<void*>(n->keys.size());
  return 1;
}
int enif_map_iterator_get_pair(ErlNifEnv*, ErlNifMapIterator* iter, ERL_NIF_TERM* key, ERL_NIF_TERM* val) {
  size_t n = reinterpret_cast<size_t>(iter->__spare__[0]);
  if (iter->idx >= n) return 0;
  *key = reinterpret_cast<ERL_NIF_TERM*>(iter->u.flat.ks)[iter->idx];
  *val = reinterpret_cast<ERL_NIF_TERM*>(iter->u.flat.vs)[iter->idx];
  return 1;
}
int enif_map_iterator_next(ErlNifEnv*, ErlNifMapIterator* iter) { ++iter->idx; return 1; }
void enif_map_iterator_destroy(ErlNifEnv*, ErlNifMapIterator*) {}
int enif_consume_timeslice(ErlNifEnv*, int) { return 1; }
int enif_is_binary(ErlNifEnv*, ERL_NIF_TERM t) { return N(t)->kind == Kind::Bin; }
ErlNifTermType enif_term_type(ErlNifEnv*, ERL_NIF_TERM t) {
  switch (N(t)->kind) {
    case Kind::Int: return ERL_NIF_TERM_TYPE_INTEGER;
    case Kind::Dbl: return ERL_NIF_TERM_TYPE_FLOAT;
    case Kind::Bin: return ERL_NIF_TERM_TYPE_BITSTRING;
    case Kind::Atom: return ERL_NIF_TERM_TYPE_ATOM;
    case Kind::Nil: case Kind::Cons: return ERL_NIF_TERM_TYPE_LIST;
    case Kind::Map: return ERL_NIF_TERM_TYPE_MAP;
  }
  return ERL_NIF_TERM_TYPE_ATOM;
}
int enif_is_list(ErlNifEnv*, ERL_NIF_TERM t) { Kind k = N(t)->kind; return k == Kind::Cons || k == Kind::Nil; }
int enif_get_tuple(ErlNifEnv*, ERL_NIF_TERM, int* arity, const ERL_NIF_TERM** array) { *arity = 0; *array = nullptr; return 0; }
int enif_get_uint(ErlNifEnv*, ERL_NIF_TERM t, unsigned* ip) {
  Node* n = N(t);
  if (n->kind != Kind::Int || n->i < 0) return 0;
  *ip = static_cast<unsigned>(n->i);
  return 1;
}
int enif_term_to_binary(ErlNifEnv*, ERL_NIF_TERM, ErlNifBinary*) { return 0; }
void enif_release_binary(ErlNifBinary*) {}

}  // extern "C"

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(1); }
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// One encode, as the NIF does it: encode into an OutBuf, then copy into a binary.
static size_t encode_once(ERL_NIF_TERM term, const glz::JSONEncodeOpts& opts) {
  thread_local glz::OutBuf out;   // reused across calls, as the NIF does
  out.clear();
  glz::JSONEncoder enc{nullptr, opts, out};
  if (!enc.encode(term)) std::abort();
  ErlNifBinary dummy;
  ERL_NIF_TERM bin = enif_make_binary(nullptr, &dummy);
  unsigned char* p = enif_make_new_binary(nullptr, out.view().size(), &bin);
  std::memcpy(p, out.view().data(), out.view().size());
  std::free(p);   // the result is dropped at once, as the VM drops garbage
  return out.view().size();
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s FILE...\n", argv[0]); return 1; }
  g_nil = mk(Kind::Nil);
  glz::init_atoms(nullptr);  // sets the AM_* globals used by the encoder
  glz::JSONEncodeOpts opts;
  opts.null_term = glz::AM_NULL;
  const char* fixed = std::getenv("PARSE_ITERS");

  for (int i = 1; i < argc; ++i) {
    std::string data = read_file(argv[i]);
    Builder b{data.data(), data.data() + data.size()};
    ERL_NIF_TERM term = T(b.value());
    size_t out_size = encode_once(term, opts);  // no budget: large outputs take the normal path

    if (fixed) {
      int n = std::atoi(fixed);
      for (int k = 0; k < n; ++k) encode_once(term, opts);
      std::printf("%-28s %d encodes, %zu bytes out\n", argv[i], n, out_size);
      continue;
    }

    auto t0 = std::chrono::steady_clock::now();
    int probe = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)) { encode_once(term, opts); ++probe; }
    int n = std::max(1, probe * 10);
    double best_us = 1e300;
    for (int round = 0; round < 7; ++round) {
      auto start = std::chrono::steady_clock::now();
      for (int k = 0; k < n; ++k) encode_once(term, opts);
      auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
      best_us = std::min(best_us, us / n);
    }
    std::printf("%-28s %12.3f us/encode  (%zu bytes out)\n", argv[i], best_us, out_size);
  }
  return 0;
}
