// vim:ts=2:sw=2:et
// Erlang NIF binding to the glaze C++ JSON library
// https://github.com/stephenberry/glaze
//
// Decode: hand-rolled recursive-descent parser — zero-copy over raw input,
//         produces Erlang terms in a single pass (no intermediate generic_u64 tree).
// Encode: direct Erlang-term → JSON writer with a stack-allocated output buffer
//         (no intermediate generic_u64 tree).

#include <array>
#include <cassert>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "glaze/glaze.hpp"
#include "glaze_atoms.hpp"
#include "glaze_bigint.hpp"
#include "glaze_lltoa.hpp"

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct DecodeOpts {
  bool         return_maps           = true;
  bool         object_as_tuple       = false;
  ERL_NIF_TERM null_term             = 0;
  bool         label_atom            = false;
  bool         label_existing_atom   = false;
};

struct EncodeOpts {
  bool         pretty    = false;
  ERL_NIF_TERM null_term = 0;
};

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

static bool parse_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, DecodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_RETURN_MAPS))    opts.return_maps = true;
    else if (enif_is_identical(head, AM_OBJECT_AS_TUPLE)){ opts.object_as_tuple = true; opts.return_maps = false; }
    else if (enif_is_identical(head, AM_USE_NIL))        opts.null_term = AM_NIL;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2) {
        if (enif_is_identical(tp[0], AM_NULL_TERM) && enif_is_atom(env, tp[1]))
          opts.null_term = tp[1];
        else if (enif_is_identical(tp[0], AM_LABEL_ATOM)) {
          if      (enif_is_identical(tp[1], AM_LABEL_ATOM))          opts.label_atom = true;
          else if (enif_is_identical(tp[1], AM_LABEL_EXISTING_ATOM)) opts.label_existing_atom = true;
        }
      }
    }
  }
  return true;
}

static bool parse_encode_opts(ErlNifEnv* env, ERL_NIF_TERM list, EncodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_PRETTY))   opts.pretty = true;
    else if (enif_is_identical(head, AM_USE_NIL))  opts.null_term = AM_NIL;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2)
        if (enif_is_identical(tp[0], AM_NULL_TERM) && enif_is_atom(env, tp[1]))
          opts.null_term = tp[1];
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Fast integer → JSON digits  (lookup-table, no division on small values)
// Adapted from https://github.com/jeaiii/itoa (MIT)
// ---------------------------------------------------------------------------

namespace lltoa_impl {
  struct pair { char dd[2]; };
  static constexpr pair digs[100] = {
    {'0','0'},{'0','1'},{'0','2'},{'0','3'},{'0','4'},{'0','5'},{'0','6'},{'0','7'},{'0','8'},{'0','9'},
    {'1','0'},{'1','1'},{'1','2'},{'1','3'},{'1','4'},{'1','5'},{'1','6'},{'1','7'},{'1','8'},{'1','9'},
    {'2','0'},{'2','1'},{'2','2'},{'2','3'},{'2','4'},{'2','5'},{'2','6'},{'2','7'},{'2','8'},{'2','9'},
    {'3','0'},{'3','1'},{'3','2'},{'3','3'},{'3','4'},{'3','5'},{'3','6'},{'3','7'},{'3','8'},{'3','9'},
    {'4','0'},{'4','1'},{'4','2'},{'4','3'},{'4','4'},{'4','5'},{'4','6'},{'4','7'},{'4','8'},{'4','9'},
    {'5','0'},{'5','1'},{'5','2'},{'5','3'},{'5','4'},{'5','5'},{'5','6'},{'5','7'},{'5','8'},{'5','9'},
    {'6','0'},{'6','1'},{'6','2'},{'6','3'},{'6','4'},{'6','5'},{'6','6'},{'6','7'},{'6','8'},{'6','9'},
    {'7','0'},{'7','1'},{'7','2'},{'7','3'},{'7','4'},{'7','5'},{'7','6'},{'7','7'},{'7','8'},{'7','9'},
    {'8','0'},{'8','1'},{'8','2'},{'8','3'},{'8','4'},{'8','5'},{'8','6'},{'8','7'},{'8','8'},{'8','9'},
    {'9','0'},{'9','1'},{'9','2'},{'9','3'},{'9','4'},{'9','5'},{'9','6'},{'9','7'},{'9','8'},{'9','9'},
  };

  inline char* u64toa(char* b, uint64_t n)
  {
    if (n < 100) {
      if (n < 10) { b[0] = '0' + (char)n; return b + 1; }
      memcpy(b, &digs[n], 2); return b + 2;
    }
    return util::lltoa(b, n);
  }

  inline char* i64toa(char* b, int64_t v)
  {
    return util::lltoa(b, v);
  }
}

// ---------------------------------------------------------------------------
// Small inline-capacity buffer for term arrays built while parsing
// arrays/objects — avoids heap allocation for the common case (most JSON
// objects/arrays have only a handful of elements).
// ---------------------------------------------------------------------------

template <size_t N>
struct SmallTermVec {
  ERL_NIF_TERM  inline_[N];
  ERL_NIF_TERM* data_ = inline_;
  size_t        len_  = 0;
  size_t        cap_  = N;

  ~SmallTermVec() { if (data_ != inline_) delete[] data_; }

  void push_back(ERL_NIF_TERM v) {
    if (len_ == cap_) {
      size_t nc = cap_ * 2;
      ERL_NIF_TERM* nb = new ERL_NIF_TERM[nc];
      memcpy(nb, data_, len_ * sizeof(ERL_NIF_TERM));
      if (data_ != inline_) delete[] data_;
      data_ = nb; cap_ = nc;
    }
    data_[len_++] = v;
  }

  ERL_NIF_TERM* data() const { return data_; }
  size_t        size() const { return len_; }
};

// ---------------------------------------------------------------------------
// Output buffer — 4 KB inline, grows to heap
// ---------------------------------------------------------------------------

struct OutBuf {
  static constexpr size_t INLINE = 4096;

  char   inline_[INLINE];
  char*  data  = inline_;
  size_t len   = 0;
  size_t cap   = INLINE;

  ~OutBuf() { if (data != inline_) delete[] data; }

  void ensure(size_t need) {
    if (len + need <= cap) return;
    size_t nc = cap * 2;
    while (nc < len + need) nc *= 2;
    char* nb = new char[nc];
    memcpy(nb, data, len);
    if (data != inline_) delete[] data;
    data = nb; cap = nc;
  }

  void push(char c)              { ensure(1); data[len++] = c; }
  void push(const char* s, size_t n) { ensure(n); memcpy(data + len, s, n); len += n; }
  void push(std::string_view sv) { push(sv.data(), sv.size()); }

  std::string_view view() const { return {data, len}; }
};

// ---------------------------------------------------------------------------
// Zero-copy JSON decoder — parses raw bytes, emits Erlang terms directly
// ---------------------------------------------------------------------------

struct Decoder {
  ErlNifEnv*       env;
  const DecodeOpts& opts;
  const char*      beg;  // start of input (for error reporting)
  const char*      p;    // current position
  const char*      end;

  Decoder(ErlNifEnv* e, const DecodeOpts& o, const char* data, size_t size)
    : env(e), opts(o), beg(data), p(data), end(data + size) {}

  // ---- whitespace ----
  static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  void skip_ws() {
    while (p + 8 <= end) {
      uint64_t w;
      memcpy(&w, p, 8);
      // Any byte that is not one of ' ' \t \r \n stops the run.
      uint64_t non_ws = has_byte(w, ' ') | has_byte(w, '\t') | has_byte(w, '\r') | has_byte(w, '\n');
      // non_ws has a set high-bit at each position that *matches* one of the WS chars.
      // We want the first byte that does NOT match any — invert per-byte "is whitespace" mask.
      // Build the set of matched positions, then find first unmatched byte.
      uint64_t matched = non_ws;
      // A byte fully matches iff its top bit is set in `matched`. Find first byte where it's clear.
      uint64_t cleared = ~matched & 0x8080808080808080ULL;
      if (cleared) {
#if defined(__GNUC__) || defined(__clang__)
        p += __builtin_ctzll(cleared) >> 3;
#else
        while (is_ws(*p)) ++p;
#endif
        return;
      }
      p += 8;
    }
    while (p < end && is_ws(*p)) ++p;
  }

  // SWAR (SIMD-within-a-register) helpers: detect '"' or '\' anywhere within
  // an 8-byte word in a few branch-free ops. Classic bit-trick:
  // for byte b, ((b ^ pattern) - 0x01..) & ~(b ^ pattern) & 0x80.. is set
  // iff b == pattern's corresponding byte.
  static inline uint64_t has_byte(uint64_t w, uint8_t needle) {
    uint64_t pattern = 0x0101010101010101ULL * needle;
    uint64_t x = w ^ pattern;
    return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
  }

  // ---- string reading — returns view into raw input (no unescaping for pure-ASCII keys) ----
  // Returns false on error; sets p past the closing quote.
  // If has_escape is set the caller must unescape before using as binary.
  bool read_string_raw(const char*& begin_out, size_t& len_out, bool& has_escape)
  {
    if (p >= end || *p != '"') return false;
    ++p;  // skip opening quote
    const char* s = p;
    has_escape = false;
    while (p < end) {
      char c = *p;
      if (c == '"') { begin_out = s; len_out = p - s; ++p; return true; }
      if (c == '\\') { has_escape = true; ++p; if (p < end) ++p; }
      else ++p;
    }
    return false; // unterminated
  }

  // Unescape a JSON string into buf, return view of result.
  // Only called when has_escape is true.
  static std::string_view unescape(const char* s, size_t len, std::string& buf)
  {
    buf.clear();
    buf.reserve(len);
    const char* end = s + len;
    while (s < end) {
      char c = *s++;
      if (c != '\\') { buf += c; continue; }
      if (s >= end) break;
      switch (*s++) {
        case '"':  buf += '"';  break;
        case '\\': buf += '\\'; break;
        case '/':  buf += '/';  break;
        case 'b':  buf += '\b'; break;
        case 'f':  buf += '\f'; break;
        case 'n':  buf += '\n'; break;
        case 'r':  buf += '\r'; break;
        case 't':  buf += '\t'; break;
        case 'u': {
          if (s + 4 > end) break;
          auto hex4 = [](const char* p) {
            int v = 0;
            for (int i = 0; i < 4; ++i) {
              char c = p[i];
              int d = (c >= '0' && c <= '9') ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
              if (d < 0) return -1;
              v = v * 16 + d;
            }
            return v;
          };
          int cp = hex4(s); s += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && s + 6 <= end && s[0] == '\\' && s[1] == 'u') {
            int lo = hex4(s + 2); s += 6;
            if (lo >= 0xDC00 && lo <= 0xDFFF)
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          // Encode cp as UTF-8
          if (cp < 0x80) { buf += (char)cp; }
          else if (cp < 0x800) { buf += (char)(0xC0|(cp>>6)); buf += (char)(0x80|(cp&0x3F)); }
          else if (cp < 0x10000) {
            buf += (char)(0xE0|(cp>>12)); buf += (char)(0x80|((cp>>6)&0x3F)); buf += (char)(0x80|(cp&0x3F));
          } else {
            buf += (char)(0xF0|(cp>>18)); buf += (char)(0x80|((cp>>12)&0x3F));
            buf += (char)(0x80|((cp>>6)&0x3F)); buf += (char)(0x80|(cp&0x3F));
          }
          break;
        }
        default: buf += *(s-1); break;
      }
    }
    return buf;
  }

  // Make an Erlang binary from a JSON string span (handles escapes).
  // buf is scratch storage reused across calls.
  ERL_NIF_TERM make_string_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    std::string_view sv = has_escape ? unescape(s, len, buf) : std::string_view(s, len);
    return make_binary(env, sv);
  }

  // Make a key term (binary / atom / existing_atom).
  ERL_NIF_TERM make_key_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    if (opts.label_atom) {
      std::string_view sv = has_escape ? unescape(s, len, buf) : std::string_view(s, len);
      return enif_make_atom_len(env, sv.data(), sv.size());
    }
    if (opts.label_existing_atom) {
      std::string_view sv = has_escape ? unescape(s, len, buf) : std::string_view(s, len);
      ERL_NIF_TERM t;
      // enif_make_existing_atom_len avoids the std::string copy the old code paid
      if (enif_make_existing_atom_len(env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1))
        return t;
      return make_binary(env, sv);
    }
    return make_string_term(s, len, has_escape, buf);
  }

  // ---- number parsing ----
  ERL_NIF_TERM parse_number()
  {
    const char* start = p;
    bool neg = (*p == '-');
    if (neg) ++p;

    // Integer part
    while (p < end && *p >= '0' && *p <= '9') ++p;

    bool is_float = false;
    if (p < end && *p == '.') { is_float = true; ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
    if (p < end && (*p == 'e' || *p == 'E')) {
      is_float = true; ++p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      while (p < end && *p >= '0' && *p <= '9') ++p;
    }

    size_t len = p - start;

    if (is_float) {
      double d;
      auto [ep, ec] = std::from_chars(start, p, d);
      if (ec != std::errc{}) return 0;
      return enif_make_double(env, d);
    }

    // Integer: try int64/uint64 first, bigint fallback
    if (neg) {
      int64_t v;
      auto [ep, ec] = std::from_chars(start + 1, p, v);
      if (ec == std::errc{}) return enif_make_int64(env, -v);
      // Could be uint64_t range negative? no — fall through to bigint
    } else {
      uint64_t v;
      auto [ep, ec] = std::from_chars(start, p, v);
      if (ec == std::errc{}) {
        if (v <= (uint64_t)INT64_MAX) return enif_make_int64(env, (int64_t)v);
        return enif_make_uint64(env, v);
      }
    }
    // Bigint
    ERL_NIF_TERM r = glazejson::BigInt::decode(env, start, p);
    return r ? r : (ERL_NIF_TERM)0;
  }

  // ---- core value parser ----
  ERL_NIF_TERM parse_value(std::string& scratch)
  {
    skip_ws();
    if (p >= end) return 0;

    switch (*p) {
      case '"': {
        ++p;
        const char* s = p;
        bool has_escape = false;
        while (p < end) {
          if (*p == '"') { size_t len = p - s; ++p; return make_string_term(s, len, has_escape, scratch); }
          if (*p == '\\') { has_escape = true; ++p; if (p < end) ++p; }
          else ++p;
        }
        return 0;
      }

      case '{': return parse_object(scratch);
      case '[': return parse_array(scratch);

      case 't':
        if (p + 4 <= end && memcmp(p, "true", 4) == 0)  { p += 4; return AM_TRUE;  } return 0;
      case 'f':
        if (p + 5 <= end && memcmp(p, "false", 5) == 0) { p += 5; return AM_FALSE; } return 0;
      case 'n':
        if (p + 4 <= end && memcmp(p, "null", 4) == 0)  { p += 4; return opts.null_term; } return 0;

      case '-': case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        return parse_number();

      default: return 0;
    }
  }

  ERL_NIF_TERM parse_array(std::string& scratch)
  {
    assert(*p == '[');
    ++p;
    skip_ws();

    SmallTermVec<16> items;
    if (p < end && *p == ']') { ++p; return enif_make_list_from_array(env, nullptr, 0); }

    while (p < end) {
      ERL_NIF_TERM v = parse_value(scratch);
      if (!v) return 0;
      items.push_back(v);
      skip_ws();
      if (p >= end) return 0;
      if (*p == ']') { ++p; break; }
      if (*p != ',') return 0;
      ++p;
    }
    return enif_make_list_from_array(env, items.data(), (unsigned)items.size());
  }

  ERL_NIF_TERM parse_object(std::string& scratch)
  {
    assert(*p == '{');
    ++p;
    skip_ws();

    if (opts.object_as_tuple) {
      SmallTermVec<16> pairs;

      if (p < end && *p == '}') { ++p;
        return enif_make_tuple1(env, enif_make_list_from_array(env, nullptr, 0)); }

      while (p < end) {
        if (*p != '"') return 0;
        const char* ks; size_t kl; bool ke;
        if (!read_string_raw(ks, kl, ke)) return 0;
        ERL_NIF_TERM key = make_key_term(ks, kl, ke, scratch);
        skip_ws();
        if (p >= end || *p != ':') return 0; ++p;
        ERL_NIF_TERM val = parse_value(scratch);
        if (!val) return 0;
        pairs.push_back(enif_make_tuple2(env, key, val));
        skip_ws();
        if (p >= end) return 0;
        if (*p == '}') { ++p; break; }
        if (*p != ',') return 0; ++p;
        skip_ws();
      }
      ERL_NIF_TERM list = enif_make_list_from_array(env, pairs.data(), (unsigned)pairs.size());
      return enif_make_tuple1(env, list);
    }

    // Map path
    SmallTermVec<16> ks, vs;

    if (p < end && *p == '}') { ++p;
      ERL_NIF_TERM m; enif_make_map_from_arrays(env, nullptr, nullptr, 0, &m); return m; }

    while (p < end) {
      if (*p != '"') return 0;
      const char* kstr; size_t klen; bool kesc;
      if (!read_string_raw(kstr, klen, kesc)) return 0;
      ERL_NIF_TERM key = make_key_term(kstr, klen, kesc, scratch);
      skip_ws();
      if (p >= end || *p != ':') return 0; ++p;
      ERL_NIF_TERM val = parse_value(scratch);
      if (!val) return 0;
      ks.push_back(key); vs.push_back(val);
      skip_ws();
      if (p >= end) return 0;
      if (*p == '}') { ++p; break; }
      if (*p != ',') return 0; ++p;
      skip_ws();
    }

    ERL_NIF_TERM map;
    if (!enif_make_map_from_arrays(env, ks.data(), vs.data(), (unsigned)ks.size(), &map))
      return enif_raise_exception(env, AM_BADARG);
    return map;
  }

  ERL_NIF_TERM decode(const char* data, size_t size)
  {
    p = data; end = data + size; beg = data;
    std::string scratch;
    ERL_NIF_TERM result = parse_value(scratch);
    if (!result) {
      std::string msg = "JSON parse error at offset " + std::to_string(p - beg);
      return enif_raise_exception(env,
        enif_make_tuple2(env, AM_PARSE_ERROR, make_binary(env, msg)));
    }
    skip_ws();
    // trailing garbage is tolerated (matches glaze's prior behaviour)
    return result;
  }
};

// ---------------------------------------------------------------------------
// Direct Erlang-term → JSON encoder (no intermediate generic_u64 tree)
// ---------------------------------------------------------------------------

static bool atom_to_sv(ErlNifEnv* env, ERL_NIF_TERM atom, char* buf, size_t bufsz, std::string_view& out)
{
  unsigned len = 0;
  if (!enif_get_atom_length(env, atom, &len, ERL_NIF_LATIN1)) return false;
  if (len + 1 > bufsz) return false;
  enif_get_atom(env, atom, buf, len + 1, ERL_NIF_LATIN1);
  out = {buf, len};
  return true;
}

// Bytes that must be escaped in a JSON string: control chars, '"', '\'.
// Everything else (including all UTF-8 continuation/lead bytes) passes through.
static constexpr bool needs_escape(unsigned char c) {
  return c < 0x20 || c == '"' || c == '\\';
}

static constexpr std::array<bool, 256> build_needs_escape_tab() {
  std::array<bool, 256> tab{};
  for (int i = 0; i < 256; ++i) tab[i] = needs_escape((unsigned char)i);
  return tab;
}
static constexpr std::array<bool, 256> NEEDS_ESCAPE_TAB = build_needs_escape_tab();

// JSON-escape a UTF-8 byte sequence into out.
// Fast path: scan for runs of bytes that need no escaping and bulk-copy them;
// only fall into the per-byte switch for the rare escape characters.
static void json_escape_string(std::string_view sv, OutBuf& out)
{
  out.push('"');
  const char* p     = sv.data();
  const char* end   = p + sv.size();
  const char* run   = p;

  while (p < end) {
    unsigned char c = (unsigned char)*p;
    if (!NEEDS_ESCAPE_TAB[c]) { ++p; continue; }

    if (p > run) out.push(run, p - run);

    switch (c) {
      case '"':  out.push("\\\"", 2); break;
      case '\\': out.push("\\\\", 2); break;
      case '\b': out.push("\\b",  2); break;
      case '\f': out.push("\\f",  2); break;
      case '\n': out.push("\\n",  2); break;
      case '\r': out.push("\\r",  2); break;
      case '\t': out.push("\\t",  2); break;
      default: {
        char esc[7]; snprintf(esc, sizeof(esc), "\\u%04X", c);
        out.push(esc, 6);
        break;
      }
    }
    ++p;
    run = p;
  }
  if (p > run) out.push(run, p - run);
  out.push('"');
}

struct Encoder {
  ErlNifEnv*        env;
  const EncodeOpts& opts;
  OutBuf&           out;
  char              atom_buf[256]; // scratch for atom → string_view

  bool encode(ERL_NIF_TERM term)
  {
    // Dispatch on the term's runtime type once — avoids the cascade of
    // enif_is_identical / enif_get_* probes that each cost a C call.
    switch (enif_term_type(env, term)) {

      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(env, term, &bin)) return false;
        json_escape_string({reinterpret_cast<const char*>(bin.data), bin.size}, out);
        return true;
      }

      case ERL_NIF_TERM_TYPE_INTEGER: {
        ErlNifSInt64 i;
        if (enif_get_int64(env, term, &i)) {
          char buf[22]; char* e = lltoa_impl::i64toa(buf, i);
          out.push(buf, e - buf); return true;
        }
        ErlNifUInt64 u;
        if (enif_get_uint64(env, term, &u)) {
          char buf[21]; char* e = util::lltoa(buf, u);
          out.push(buf, e - buf); return true;
        }
        // bigint — doesn't fit in 64 bits
        auto s = glazejson::BigInt::encode(env, term);
        if (!s.empty()) { out.push(s); return true; }
        return false;
      }

      case ERL_NIF_TERM_TYPE_MAP: {
        out.push('{');
        ErlNifMapIterator iter;
        if (!enif_map_iterator_create(env, term, &iter, ERL_NIF_MAP_ITERATOR_FIRST))
          return false;
        ERL_NIF_TERM k, v;
        bool first = true;
        while (enif_map_iterator_get_pair(env, &iter, &k, &v)) {
          if (!first) out.push(',');
          first = false;
          if (!encode_key(k)) { enif_map_iterator_destroy(env, &iter); return false; }
          out.push(':');
          if (!encode(v))     { enif_map_iterator_destroy(env, &iter); return false; }
          enif_map_iterator_next(env, &iter);
        }
        enif_map_iterator_destroy(env, &iter);
        out.push('}');
        return true;
      }

      case ERL_NIF_TERM_TYPE_LIST: {
        out.push('[');
        ERL_NIF_TERM h, t = term;
        bool first = true;
        while (enif_get_list_cell(env, t, &h, &t)) {
          if (!first) out.push(',');
          first = false;
          if (!encode(h)) return false;
        }
        out.push(']');
        return true;
      }

      case ERL_NIF_TERM_TYPE_ATOM: {
        if (enif_is_identical(term, opts.null_term)) { out.push("null", 4); return true; }
        if (enif_is_identical(term, AM_TRUE))  { out.push("true",  4); return true; }
        if (enif_is_identical(term, AM_FALSE)) { out.push("false", 5); return true; }
        if (enif_is_identical(term, AM_NULL))  { out.push("null",  4); return true; }
        if (enif_is_identical(term, AM_NIL))   { out.push("null",  4); return true; }
        std::string_view sv;
        if (!atom_to_sv(env, term, atom_buf, sizeof(atom_buf), sv)) return false;
        json_escape_string(sv, out);
        return true;
      }

      case ERL_NIF_TERM_TYPE_FLOAT: {
        double d;
        if (!enif_get_double(env, term, &d)) return false;
        if (!std::isfinite(d)) { out.push("null", 4); return true; }
        char buf[32];
        auto [e, ec] = std::to_chars(buf, buf+32, d, std::chars_format::general);
        if (ec == std::errc{}) {
          bool has_dot = false;
          for (char* p = buf; p < e; ++p) if (*p == '.' || *p == 'e' || *p == 'E') { has_dot = true; break; }
          out.push(buf, e - buf);
          if (!has_dot) out.push(".0", 2);
        } else {
          int n = snprintf(buf, sizeof(buf), "%.17g", d);
          out.push(buf, n);
        }
        return true;
      }

      case ERL_NIF_TERM_TYPE_TUPLE: {
        // {[{K,V}...]} proplist → object
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(env, term, &arity, &tp);
        if (arity == 1 && enif_is_list(env, tp[0])) {
          out.push('{');
          ERL_NIF_TERM h, t = tp[0];
          bool first = true;
          while (enif_get_list_cell(env, t, &h, &t)) {
            int pa; const ERL_NIF_TERM* pp;
            if (!enif_get_tuple(env, h, &pa, &pp) || pa != 2) return false;
            if (!first) out.push(',');
            first = false;
            if (!encode_key(pp[0])) return false;
            out.push(':');
            if (!encode(pp[1])) return false;
          }
          out.push('}');
          return true;
        }
        return false;
      }

      default:
        return false;
    }
  }

  bool encode_key(ERL_NIF_TERM k)
  {
    ErlNifBinary bin;
    if (enif_inspect_binary(env, k, &bin)) {
      json_escape_string({reinterpret_cast<const char*>(bin.data), bin.size}, out);
      return true;
    }
    if (enif_is_atom(env, k)) {
      std::string_view sv;
      if (!atom_to_sv(env, k, atom_buf, sizeof(atom_buf), sv)) return false;
      json_escape_string(sv, out);
      return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// NIF: decode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) return enif_make_badarg(env);

  DecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);

  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);

  Decoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size);
  return dec.decode(reinterpret_cast<const char*>(bin.data), bin.size);
}

// ---------------------------------------------------------------------------
// NIF: encode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) return enif_make_badarg(env);

  EncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);

  OutBuf out;
  Encoder enc{env, opts, out};

  if (!enc.encode(argv[0]))
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        make_binary(env, std::string_view("cannot encode term to JSON"))));

  if (!opts.pretty)
    return make_binary(env, out.view());

  std::string pretty_in(out.view());
  std::string pretty_out = glz::prettify_json(pretty_in);
  return make_binary(env, pretty_out);
}

// ---------------------------------------------------------------------------
// NIF: minify / prettify
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_minify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) return enif_make_badarg(env);
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);
  std::string in(reinterpret_cast<const char*>(bin.data), bin.size);
  std::string out = glz::minify_json(in);
  return enif_make_tuple2(env, AM_OK, make_binary(env, out));
}

static ERL_NIF_TERM nif_prettify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) return enif_make_badarg(env);
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);
  std::string in(reinterpret_cast<const char*>(bin.data), bin.size);
  std::string out = glz::prettify_json(in);
  return enif_make_tuple2(env, AM_OK, make_binary(env, out));
}

// ---------------------------------------------------------------------------
// NIF: encode_bigint / decode_bigint
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_encode_bigint(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) return enif_make_badarg(env);
  ErlNifSInt64 val;
  if (enif_get_int64(env, argv[0], &val)) {
    char buf[22]; char* e = lltoa_impl::i64toa(buf, val);
    return enif_make_tuple2(env, AM_OK, make_binary(env, std::string_view(buf, e - buf)));
  }
  auto s = glazejson::BigInt::encode(env, argv[0]);
  if (s.empty())
    return enif_make_tuple2(env, AM_ERROR, make_binary(env, std::string_view("invalid_bigint")));
  return enif_make_tuple2(env, AM_OK, make_binary(env, s));
}

static ERL_NIF_TERM nif_decode_bigint(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) return enif_make_badarg(env);
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);
  auto r = glazejson::BigInt::decode(env,
    reinterpret_cast<const char*>(bin.data),
    reinterpret_cast<const char*>(bin.data) + bin.size);
  if (!r)
    return enif_make_tuple2(env, AM_ERROR, make_binary(env, std::string_view("invalid_number_format")));
  return enif_make_tuple2(env, AM_OK, r);
}

// ---------------------------------------------------------------------------
// NIF lifecycle
// ---------------------------------------------------------------------------

static int nif_load(ErlNifEnv* env, void** /*priv_data*/, ERL_NIF_TERM load_info)
{
  init_atoms(env);

  ERL_NIF_TERM head, tail = load_info;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    int arity; const ERL_NIF_TERM* tp;
    if (enif_get_tuple(env, head, &arity, &tp) && arity == 2)
      if (enif_is_identical(tp[0], AM_NULL) && enif_is_atom(env, tp[1]))
        am_null = tp[1];
  }
  return 0;
}

static ErlNifFunc nif_funcs[] = {
  {"decode",        1, nif_decode,        0},
  {"decode",        2, nif_decode,        0},
  {"encode",        1, nif_encode,        0},
  {"encode",        2, nif_encode,        0},
  {"minify",        1, nif_minify,        0},
  {"prettify",      1, nif_prettify,      0},
  {"encode_bigint", 1, nif_encode_bigint, 0},
  {"decode_bigint", 1, nif_decode_bigint, 0},
};

ERL_NIF_INIT(glazejson, nif_funcs, nif_load, NULL, NULL, NULL)
