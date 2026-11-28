// Standalone JSON decode benchmark for glazer's C++ decoder, without the BEAM.
//
// Compiles the real JSONDecoder from glazer_json.hpp and links stub versions of
// the erl_nif term builders. Each stub returns a cheap handle instead of
// allocating an Erlang term, so the timed work is the parser itself: byte
// scanning, dispatch, the key cache, and the decoder's value/frame bookkeeping.
// Term construction in the BEAM is not measured here.
//
// Build and run (development tool; not part of the NIF):
//
//   clang++ -std=c++2b -O3 -march=native -DNDEBUG -Ic_src -I$ERTS_INCLUDE \
//     c_src/tools/json_parse_bench.cpp c_src/glazer_atoms.cpp -o json_parse_bench
//   ./json_parse_bench test/data/twitter.json test/data/openrtb.json
//
// Results are min-of-rounds per file, in microseconds per decode.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "glazer_json.hpp"

// ---------------------------------------------------------------------------
// Stub erl_nif term builders. A term is an index into a counter; nothing is
// stored, so these cost a function call and an increment each.
// ---------------------------------------------------------------------------

namespace {

// Scratch space for binaries built for strings and keys (enif_make_new_binary).
// In the BEAM these come from the process heap, a bump allocation with no libc
// call, so this is a bump pool reset between decodes rather than malloc/free.
// Requests beyond the pool fall back to malloc and are freed on reset.
constexpr size_t POOL_BYTES = size_t(64) << 20;
char*            g_pool = nullptr;
size_t           g_pool_used = 0;
std::vector<void*> g_overflow;
unsigned long      g_next_term = 1;

void* arena_alloc(size_t n) {
  n = (n + 7) & ~size_t(7);
  if (!g_pool) g_pool = static_cast<char*>(std::malloc(POOL_BYTES));
  if (g_pool_used + n <= POOL_BYTES) {
    void* p = g_pool + g_pool_used;
    g_pool_used += n;
    return p;
  }
  void* p = std::malloc(n ? n : 1);
  g_overflow.push_back(p);
  return p;
}

void arena_reset() {
  g_pool_used = 0;
  for (void* p : g_overflow) std::free(p);
  g_overflow.clear();
}

}  // namespace

extern "C" {

ERL_NIF_TERM enif_make_int64(ErlNifEnv*, ErlNifSInt64) { return g_next_term++; }
ERL_NIF_TERM enif_make_uint64(ErlNifEnv*, ErlNifUInt64) { return g_next_term++; }
ERL_NIF_TERM enif_make_double(ErlNifEnv*, double) { return g_next_term++; }
ERL_NIF_TERM enif_make_tuple(ErlNifEnv*, unsigned, ...) { return g_next_term++; }
ERL_NIF_TERM enif_make_new_map(ErlNifEnv*) { return g_next_term++; }
ERL_NIF_TERM enif_make_list_from_array(ErlNifEnv*, const ERL_NIF_TERM*, unsigned) { return g_next_term++; }
int enif_make_map_from_arrays(ErlNifEnv*, ERL_NIF_TERM*, ERL_NIF_TERM*, size_t, ERL_NIF_TERM* map_out) {
  *map_out = g_next_term++;
  return 1;
}
ERL_NIF_TERM enif_make_atom_len(ErlNifEnv*, const char*, size_t) { return g_next_term++; }
ERL_NIF_TERM enif_make_sub_binary(ErlNifEnv*, ERL_NIF_TERM, size_t, size_t) { return g_next_term++; }
unsigned char* enif_make_new_binary(ErlNifEnv*, size_t size, ERL_NIF_TERM* term) {
  *term = g_next_term++;
  return static_cast<unsigned char*>(arena_alloc(size));
}
ERL_NIF_TERM enif_make_binary(ErlNifEnv*, ErlNifBinary* bin) {
  return g_next_term++;
}
ERL_NIF_TERM enif_make_list(ErlNifEnv*, unsigned, ...) { return g_next_term++; }
ERL_NIF_TERM enif_make_badarg(ErlNifEnv*) { return g_next_term++; }
int enif_is_list(ErlNifEnv*, ERL_NIF_TERM) { return 1; }
int enif_is_empty_list(ErlNifEnv*, ERL_NIF_TERM) { return 0; }

ERL_NIF_TERM enif_make_atom(ErlNifEnv*, const char*) { return g_next_term++; }
ERL_NIF_TERM enif_make_int(ErlNifEnv*, int) { return g_next_term++; }
int enif_make_existing_atom_len(ErlNifEnv*, const char*, size_t, ERL_NIF_TERM* atom, ErlNifCharEncoding) {
  *atom = g_next_term++;
  return 1;
}
int enif_make_map_put(ErlNifEnv*, ERL_NIF_TERM, ERL_NIF_TERM, ERL_NIF_TERM, ERL_NIF_TERM* map_out) {
  *map_out = g_next_term++;
  return 1;
}
int enif_get_tuple(ErlNifEnv*, ERL_NIF_TERM, int*, const ERL_NIF_TERM**) { return 0; }
int enif_compare(ERL_NIF_TERM lhs, ERL_NIF_TERM rhs) { return lhs == rhs ? 0 : (lhs < rhs ? -1 : 1); }
size_t enif_binary_to_term(ErlNifEnv*, const unsigned char*, size_t, ERL_NIF_TERM*, unsigned int) { return 0; }
int enif_inspect_binary(ErlNifEnv*, ERL_NIF_TERM, ErlNifBinary*) { return 0; }

}  // extern "C"

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", path);
    std::exit(1);
  }
  return std::string(std::istreambuf_iterator<char>(in), {});
}

// One decode of `data`, returning whether it parsed.
static bool decode_once(ErlNifEnv* env, const std::string& data, const glz::JSONDecodeOpts& opts) {
  glz::JSONDecoder dec(env, opts, data.data(), data.size(), /*input_bin=*/1);
  auto [ok, term] = dec.decode(data.data(), data.size());
  (void)term;
  arena_reset();
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE...\n", argv[0]);
    return 1;
  }

  glz::init_atoms(nullptr);  // stubs ignore env; sets the AM_* globals
  glz::JSONDecodeOpts opts;
  opts.null_term = glz::AM_NULL;

  // PARSE_ITERS=N runs exactly N decodes per file and skips calibration. Use it
  // under callgrind, where wall-clock calibration would give each tool a
  // different N; instructions per decode are then Ir / N.
  const char* fixed = std::getenv("PARSE_ITERS");

  for (int i = 1; i < argc; ++i) {
    std::string data = read_file(argv[i]);
    if (fixed) {
      int n = std::atoi(fixed);
      for (int k = 0; k < n; ++k) {
        if (!decode_once(nullptr, data, opts)) {
          std::fprintf(stderr, "%s: decode failed\n", argv[i]);
          return 1;
        }
      }
      std::printf("%-28s %d decodes\n", argv[i], n);
      continue;
    }
    // Calibrate the iteration count to about 0.5 s of work per round.
    auto t0 = std::chrono::steady_clock::now();
    int probe = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)) {
      if (!decode_once(nullptr, data, opts)) {
        std::fprintf(stderr, "%s: decode failed\n", argv[i]);
        return 1;
      }
      ++probe;
    }
    int n = std::max(1, probe * 10);

    double best_us = 1e300;
    for (int round = 0; round < 7; ++round) {
      auto start = std::chrono::steady_clock::now();
      for (int k = 0; k < n; ++k) decode_once(nullptr, data, opts);
      auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
      best_us = std::min(best_us, us / n);
    }
    std::printf("%-28s %12.2f us/decode  (%zu bytes)\n", argv[i], best_us, data.size());
  }
  return 0;
}
