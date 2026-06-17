// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON-specific decode/encode/scan implementation.
//
// Decode: hand-rolled recursive-descent parser — zero-copy over raw input,
//         produces Erlang terms in a single pass (no intermediate tree).
// Encode: direct Erlang-term to JSON writer with a stack-allocated output
//         buffer (no intermediate generic_u64 tree).
//-----------------------------------------------------------------------------
#pragma once

#include <array>
#include <cassert>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <erl_nif.h>
#if defined(__ARM_NEON__)
#  include <arm_neon.h>
#endif

#include "fast_float.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_common.hpp"

namespace glz {

//-----------------------------------------------------------------------------
// Options
//-----------------------------------------------------------------------------

struct DecodeOpts {
  bool         object_as_tuple     = false;
  ERL_NIF_TERM null_term           = 0;
  bool         label_atom          = false;
  bool         label_existing_atom = false;
  bool         dedupe_keys         = false;
  // When true, unescaped strings are copied into fresh binaries instead of
  // referencing the input via sub-binary.  Use this when decoded strings
  // will outlive the input binary by a large margin: without it, one live
  // string keeps the entire input buffer from being collected.
  bool         copy_strings        = false;
};

struct EncodeOpts {
  bool         pretty     = false;
  bool         uescape    = false;
  bool         force_utf8 = false;
  ERL_NIF_TERM null_term  = 0;
};

//-----------------------------------------------------------------------------
// Option parsing
//-----------------------------------------------------------------------------

static bool parse_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, DecodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_OBJECT_AS_TUPLE))  opts.object_as_tuple = true;
    else if (enif_is_identical(head, AM_USE_NIL))          opts.null_term       = AM_NIL;
    else if (enif_is_identical(head, AM_DEDUPE_KEYS))      opts.dedupe_keys     = true;
    else if (enif_is_identical(head, AM_COPY_STRINGS))     opts.copy_strings    = true;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2) {
        if (enif_is_identical(tp[0], AM_NULL_TERM) && enif_is_atom(env, tp[1]))
          opts.null_term = tp[1];
        else if (enif_is_identical(tp[0], AM_KEYS) || enif_is_identical(tp[0], AM_LABEL_ATOM)) {
          if      (enif_is_identical(tp[1], AM_LABEL_ATOM))          opts.label_atom = true;
          else if (enif_is_identical(tp[1], AM_LABEL_EXISTING_ATOM)) opts.label_existing_atom = true;
          else if (enif_is_identical(tp[1], AM_LABEL_BINARY))        { opts.label_atom = false; opts.label_existing_atom = false; }
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
    if      (enif_is_identical(head, AM_PRETTY))     opts.pretty     = true;
    else if (enif_is_identical(head, AM_USE_NIL))    opts.null_term  = AM_NIL;
    else if (enif_is_identical(head, AM_UESCAPE))    opts.uescape    = true;
    else if (enif_is_identical(head, AM_FORCE_UTF8)) opts.force_utf8 = true;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2)
        if (enif_is_identical(tp[0], AM_NULL_TERM) && enif_is_atom(env, tp[1]))
          opts.null_term = tp[1];
    }
  }
  return true;
}

//-----------------------------------------------------------------------------
// Zero-copy JSON decoder — parses raw bytes, emits Erlang terms directly
//-----------------------------------------------------------------------------

struct Decoder {
  ErlNifEnv*        m_env;
  const DecodeOpts& m_opts;
  const char*       m_beg;  // start of input (for error reporting)
  const char*       m_p;    // current position
  const char*       m_end;
  ERL_NIF_TERM      m_input_bin; // original binary term — used for zero-copy sub_binary
  KeyCache          m_key_cache;
  bool              m_use_key_cache;
  unsigned          m_depth = 0;
  std::string       m_err;

  // Below this input size, documents rarely repeat enough keys to amortize
  // the cache's lookup-scan cost — skip it entirely (helps small payloads
  // like RPC messages, where glazer otherwise loses ground to torque).
  static constexpr size_t KEY_CACHE_MIN_SIZE = 2048;

  // parse_value/parse_array/parse_object recurse on each nesting level, so
  // an unbounded depth can overflow the C stack and crash the whole VM.
  // This cap is well within the default thread stack size with room to
  // spare for the rest of each frame, across compilers (gcc/clang) and
  // under AddressSanitizer (whose redzones and shadow-memory checks inflate
  // each frame considerably compared to a normal build).
  static constexpr unsigned MAX_DEPTH = 256;

  Decoder(ErlNifEnv* e, const DecodeOpts& o, const char* data, size_t size,
          ERL_NIF_TERM input_bin)
    : m_env(e), m_opts(o), m_beg(data), m_p(data), m_end(data + size),
      m_input_bin(input_bin),
      m_use_key_cache(size >= KEY_CACHE_MIN_SIZE) {}

  // Increments the shared depth counter for the lifetime of a parse_array /
  // parse_object call, so every return path (including early `return 0`)
  // restores it.
  struct DepthGuard {
    explicit DepthGuard(Decoder* d) : d(d) { ++d->m_depth; }
    ~DepthGuard() { --d->m_depth; }

    bool check() const {
      if (d->m_depth > MAX_DEPTH) [[unlikely]] {
        d->m_err = "exceeded maximum nesting depth";
        return false;
      }
      ++d->m_p;
      d->skip_ws();
      return true;
    }
  private:
    Decoder* d;
  };

  // ---- whitespace ----
  static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  void skip_ws() {
    // Fast path: minified JSON has structural whitespace only rarely (often
    // none at all). Check the first byte before paying for an 8-byte load
    // and SWAR bit-twiddling — avoids that cost on the overwhelmingly common
    // "no whitespace here" case.
    if (m_p >= m_end || !is_ws(*m_p)) return;
    while (m_p + 8 <= m_end) {
      uint64_t w;
      memcpy(&w, m_p, 8);
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
        m_p += __builtin_ctzll(cleared) >> 3;
#else
        while (is_ws(*m_p)) ++m_p;
#endif
        return;
      }
      m_p += 8;
    }
    while (m_p < m_end && is_ws(*m_p)) ++m_p;
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

  // Advance m_p past bytes that are neither '"' nor '\' using the widest
  // SIMD tier available (AVX2: 32 B, SSE2: 16 B), then SWAR (8 B).
  // On return m_p is at the first potential special byte or in the scalar
  // cleanup zone (fewer than one chunk-width from m_end).
  void bulk_skip_to_special() noexcept {
#if defined(__AVX2__)
    {
      const __m256i vq = _mm256_set1_epi8('"');
      const __m256i vb = _mm256_set1_epi8('\\');
      while (m_p + 32 <= m_end) {
        __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(m_p));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(
          _mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vb)));
        if (mask) { m_p += __builtin_ctz(mask); return; }
        m_p += 32;
      }
    }
#endif
#if defined(__SSE2__)
    {
      const __m128i vq = _mm_set1_epi8('"');
      const __m128i vb = _mm_set1_epi8('\\');
      while (m_p + 16 <= m_end) {
        __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m_p));
        unsigned mask = (unsigned)_mm_movemask_epi8(
          _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vb)));
        if (mask) { m_p += __builtin_ctz(mask); return; }
        m_p += 16;
      }
    }
#endif
    // SWAR fallback (8 B/iter): leaves m_p at the word boundary before a hit.
    while (m_p + 8 <= m_end) {
      uint64_t w;
      memcpy(&w, m_p, 8);
      if (has_byte(w, '"') | has_byte(w, '\\')) break;
      m_p += 8;
    }
  }

  // Returns false on error; sets p past the closing quote.
  // If has_escape is set the caller must unescape before using as binary.
  // Bulk-scans with SIMD (AVX2→SSE2→SWAR); scalar fallback only for the
  // few bytes around each '"' or '\'.
  bool read_string_raw(const char*& begin_out, size_t& len_out, bool& has_escape)
  {
    if (m_p >= m_end || *m_p != '"')
      return false;

    ++m_p;  // skip opening quote
    const char* s = m_p;
    has_escape = false;
    for (;;) {
      bulk_skip_to_special();
      while (m_p < m_end) {
        char c = *m_p;
        if (c == '"') { begin_out = s; len_out = m_p - s; ++m_p; return true; }
        if (c == '\\') [[unlikely]] { has_escape = true; ++m_p; if (m_p < m_end) ++m_p; break; }
        ++m_p;
      }
      if (m_p >= m_end) return false; // unterminated string
    }
  }

  // Unescape a JSON string into buf, return view of result.
  // Only called when has_escape is true. std::string is used deliberately:
  // escaped strings are rare in practice (real payloads have ~0), so the
  // buffer is almost never written. If used OutBuf, it would reserve 4 KB on
  // the stack unconditionally; std::string pays nothing until the first actual
  // write.
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
  // Default (copy_strings == false): unescaped strings are returned as
  // sub-binaries of the original input — zero allocation, but the input
  // binary stays alive as long as any sub-binary referencing it does.
  // With copy_strings == true: always allocates a fresh binary, allowing the
  // GC to reclaim the input buffer independently of the decoded results.
  ERL_NIF_TERM make_string_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    if (!has_escape) [[likely]]
      return make_span_term(m_env, m_input_bin, m_beg, m_end, std::string_view(s, len), m_opts.copy_strings);
    return make_binary(m_env, unescape(s, len, buf));
  }

  // Make a key term (binary / atom / existing_atom).
  ERL_NIF_TERM make_key_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    if (m_opts.label_atom) {
      std::string_view sv = has_escape ? unescape(s, len, buf) : std::string_view(s, len);
      return enif_make_atom_len(m_env, sv.data(), sv.size());
    }
    if (m_opts.label_existing_atom) {
      std::string_view sv = has_escape ? unescape(s, len, buf) : std::string_view(s, len);
      ERL_NIF_TERM t;
      // enif_make_existing_atom_len avoids the std::string copy the old code paid
      return enif_make_existing_atom_len(m_env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1)
           ? t : make_binary(m_env, sv);
    }
    // Binary keys: reuse cached terms for repeated keys (raw, unescaped only —
    // escapes are rare for keys and not worth complicating the cache for).
    // Only worthwhile for larger documents — see KEY_CACHE_MIN_SIZE.
    if (!has_escape && m_use_key_cache) {
      uint32_t h = KeyCache::hash_of(s, len);
      if (ERL_NIF_TERM cached = m_key_cache.lookup(s, len, h))
        return cached;
      auto term = make_binary(m_env, std::string_view(s, len));
      m_key_cache.insert(s, len, h, term);
      return term;
    }
    return make_string_term(s, len, has_escape, buf);
  }

  // ---- number parsing ----
  ERL_NIF_TERM parse_number()
  {
    const char* start = m_p;
    bool neg = (*m_p == '-');
    if (neg) ++m_p;

    // Integer part
    while (m_p < m_end && *m_p >= '0' && *m_p <= '9') ++m_p;

    bool is_float = false;
    if (m_p < m_end && *m_p == '.') { is_float = true; ++m_p; while (m_p < m_end && *m_p >= '0' && *m_p <= '9') ++m_p; }
    if (m_p < m_end && (*m_p == 'e' || *m_p == 'E')) {
      is_float = true;
      if  (++m_p < m_end && (*m_p == '+' || *m_p == '-')) ++m_p;
      while (m_p < m_end &&  *m_p >= '0' && *m_p <= '9')  ++m_p;
    }

    if (is_float) {
      double d;
      // std::from_chars for floating-point isn't available on all platforms
      // (e.g. older Apple libc++), so use vendored fast_float here.
      auto [ep, ec] = glz::fast_float::from_chars(start, m_p, d);
      if (ec != std::errc{}) return 0;
      return enif_make_double(m_env, d);
    }

    // Integer: try int64/uint64 first, bigint fallback
    if (neg) {
      int64_t v = 0;
      auto [ep, ec] = std::from_chars(start + 1, m_p, v);
      if (ec == std::errc{})
        return enif_make_int64(m_env, -v);
      // Could be uint64_t range negative? no — fall through to bigint
    } else {
      uint64_t v = 0;
      auto [ep, ec] = std::from_chars(start, m_p, v);
      if (ec == std::errc{})
        return v <= uint64_t(INT64_MAX) ? enif_make_int64(m_env, int64_t(v))
                                        : enif_make_uint64(m_env, v);
    }
    // Bigint
    ERL_NIF_TERM r = glz::BigInt::decode(m_env, start, m_p);
    return r ? r : (ERL_NIF_TERM)0;
  }

  // ---- core value parser ----
  ERL_NIF_TERM parse_value(std::string& scratch)
  {
    skip_ws();
    if (m_p >= m_end) [[unlikely]]
      return 0;
    auto c = *m_p;
    switch (c) {
      case '"': {
        const char* s; size_t len; bool has_escape;
        if (!read_string_raw(s, len, has_escape)) return 0;
        return make_string_term(s, len, has_escape, scratch);
      }

      case '{': return parse_object(scratch);
      case '[': return parse_array(scratch);

      case 't':
        if (m_p + 4 <= m_end && memcmp(m_p, "true", 4) == 0) {
          m_p += 4;
          return AM_TRUE;
        }
        return 0;
      case 'f':
        if (m_p + 5 <= m_end && memcmp(m_p, "false", 5) == 0) {
          m_p += 5;
          return AM_FALSE;
        }
        return 0;
      case 'n':
        if (m_p + 4 <= m_end && memcmp(m_p, "null", 4) == 0) {
          m_p += 4;
          return m_opts.null_term;
        }
        return 0;

      default:
        // The expression checks if the number is in []'-','0'-'9'] range:
        return ((unsigned char)(c - '0') <= 9 || c == '-') ? parse_number() : 0;
    }
  }

  ERL_NIF_TERM parse_array(std::string& scratch)
  {
    assert(*m_p == '[');
    DepthGuard guard(this);
    if (!guard.check()) [[unlikely]] return 0;

    SmallTermVec<16> items;
    if (m_p < m_end && *m_p == ']') {
      ++m_p;
      return enif_make_list_from_array(m_env, nullptr, 0);
    }

    for (;;) {
      auto v = parse_value(scratch);
      if (!v) [[unlikely]] return 0;
      items.push_back(v);
      skip_ws();
      if (m_p >= m_end) [[unlikely]] return 0;
      if (*m_p == ']')  { ++m_p; break; }
      if (*m_p != ',')  [[unlikely]] return 0;
      ++m_p;
    }
    return enif_make_list_from_array(m_env, items.data(), unsigned(items.size()));
  }

  // Remove earlier duplicate {Key, Val} pairs in-place, keeping each key's
  // *last* occurrence (and its position). O(n^2) but objects are typically
  // small (SmallTermVec inline capacity is 32) so this is cheaper than a
  // hash set for the common case. Used for the object_as_tuple path, which
  // has no map-based shortcut.
  template <size_t N>
  static void dedupe_pairs_last(SmallTermVec<N>& pairs, ErlNifEnv* env)
  {
    size_t n = pairs.size();
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) {
      int arity_i; const ERL_NIF_TERM* tp_i;
      enif_get_tuple(env, pairs.data()[i], &arity_i, &tp_i);
      bool dup = false;
      for (size_t j = i + 1; j < n; ++j) {
        int arity_j; const ERL_NIF_TERM* tp_j;
        enif_get_tuple(env, pairs.data()[j], &arity_j, &tp_j);
        if (enif_compare(tp_i[0], tp_j[0]) == 0) { dup = true; break; }
      }
      if (!dup)
        pairs.data()[out++] = pairs.data()[i];
    }
    pairs.set_size(out);
  }

  ERL_NIF_TERM parse_object(std::string& scratch)
  {
    assert(*m_p == '{');
    DepthGuard guard(this);
    if (!guard.check()) [[unlikely]] return 0;

    if (m_opts.object_as_tuple) {
      SmallTermVec<32> pairs;

      if (m_p < m_end && *m_p == '}') { ++m_p;
        return enif_make_tuple1(m_env, enif_make_list_from_array(m_env, nullptr, 0)); }

      for (;;) {
        if (m_p >= m_end || *m_p != '"') [[unlikely]] return 0;

        const char* ks;
        size_t      kl;
        bool        ke;

        if (!read_string_raw(ks, kl, ke)) [[unlikely]] return 0;

        auto key = make_key_term(ks, kl, ke, scratch);
        skip_ws();

        if (m_p >= m_end || *m_p != ':') [[unlikely]] return 0;
        ++m_p;

        auto val = parse_value(scratch);
        if (!val) [[unlikely]] return 0;

        pairs.push_back(enif_make_tuple2(m_env, key, val));
        skip_ws();

        if (m_p >= m_end) [[unlikely]] return 0;

        if (*m_p == '}') { ++m_p; break; }
        if (*m_p != ',') [[unlikely]] return 0;
        ++m_p;
        skip_ws();
      }
      if (m_opts.dedupe_keys)
        dedupe_pairs_last(pairs, m_env);

      return enif_make_tuple1(m_env, pairs.to_erl_list(m_env));
    }

    // Map path
    SmallTermVec<32> ks, vs;

    if (m_p < m_end && *m_p == '}') {
      ++m_p;
      return enif_make_new_map(m_env);
    }

    for (;;) {
      if (m_p >= m_end || *m_p != '"') [[unlikely]] return 0;

      const char* kstr;
      size_t      klen;
      bool        kesc;

      if (!read_string_raw(kstr, klen, kesc))
        [[unlikely]] return 0;

      auto key = make_key_term(kstr, klen, kesc, scratch);
      skip_ws();

      if (m_p >= m_end || *m_p != ':') [[unlikely]] return 0;
      ++m_p;

      auto val = parse_value(scratch);
      if (!val) [[unlikely]] return 0;

      ks.push_back(key); vs.push_back(val);
      skip_ws();

      if (m_p >= m_end) [[unlikely]] return 0;
      if (*m_p == '}') { ++m_p; break; }
      if (*m_p != ',') [[unlikely]] return 0;
      ++m_p;
      skip_ws();
    }

    [[maybe_unused]] auto map = vs.to_erl_map<true>(m_env, ks);
    assert(map);
    return map;
  }

  // Always returns {ok, Term} | {error, Msg}.
  // Raising vs. non-raising behaviour is the Erlang caller's responsibility.
  std::tuple<bool, ERL_NIF_TERM> decode(const char* data, size_t size)
  {
    m_p = data; m_end = data + size; m_beg = data;
    std::string scratch;
    ERL_NIF_TERM result = parse_value(scratch);
    if (result) skip_ws();
    if (result && m_p == m_end) [[likely]]
      return std::make_tuple(true, result);

    std::string msg = m_err.empty()
      ? "JSON parse error at offset " + std::to_string(m_p - m_beg)
      : m_err + " at offset " + std::to_string(m_p - m_beg);
    return std::make_tuple(false, make_binary(m_env, msg));
  }
};

//-----------------------------------------------------------------------------
// Value-boundary scanner — finds where the next complete top-level JSON value
// ends in a (possibly partial) buffer, without building any Erlang terms.
//
// This underpins incremental/streaming decode: callers buffer raw bytes,
// repeatedly ask the scanner "is there a complete value yet?", and once one
// is found, slice it off and hand it to the existing whole-buffer `decode`.
// The scanner never allocates and never inspects string contents beyond
// quote/escape bytes, so it stays cheap even on huge inputs.
//-----------------------------------------------------------------------------

struct ScanState {
  uint64_t pos        = 0;      // byte offset into the buffer to resume scanning at
  uint32_t depth      = 0;      // current [ ]/{ } nesting depth
  bool     in_string  = false;  // currently inside a "..." (top-level or nested)
  bool     escape     = false;  // previous byte inside a string was an unconsumed backslash
  bool     started    = false;  // have we seen the first non-ws byte of the value yet?
  bool     scalar     = false;  // value-so-far is a bare scalar (number/literal), not { or [

  static ScanState initial() { return ScanState{}; }
};

struct Scanner {
  const char* m_beg;
  const char* m_p;
  const char* m_end;

  // `resume_pos` is where to start scanning from (0 for a fresh scan, or
  // ScanState::pos when continuing — the caller passes the full buffer each
  // time, so previously-scanned bytes must be skipped rather than re-walked).
  Scanner(const char* data, size_t size, size_t resume_pos)
    : m_beg(data), m_p(data + std::min(resume_pos, size)), m_end(data + size) {}

  static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  // Scans from `p` using/updating `st`.
  //   returns true  + sets `value_end` to one-past-the-last-byte of the value, if complete
  //   returns false (value_end untouched) if the buffer ran out mid-value (st updated to resume)
  bool scan(ScanState& st, const char*& value_end)
  {
    // Skip leading whitespace before the value starts.
    if (!st.started) {
      while (m_p < m_end && is_ws(*m_p)) ++m_p;
      if (m_p >= m_end) return false;
    }

    while (m_p < m_end) {
      char c = *m_p;

      if (st.in_string) {
        if (st.escape)  { st.escape = false; ++m_p; continue; }
        if (c == '\\')  { st.escape = true;  ++m_p; continue; }
        if (c == '"')   { st.in_string = false; ++m_p;
                          if (st.depth == 0 && st.scalar) { value_end = m_p; return true; }
                          continue;
                        }
        ++m_p;
        continue;
      }

      switch (c) {
        case '"':
          st.in_string = true;
          if (!st.started) { st.started = true; st.scalar = true; }
          ++m_p;
          break;

        case '{':
        case '[':
          st.started = true;
          st.scalar  = false;
          ++st.depth;
          ++m_p;
          break;

        case '}':
        case ']':
          if (st.depth == 0) { value_end = m_p; return true; } // stray close — treat as boundary
          --st.depth;
          ++m_p;
          if (st.depth == 0) { value_end = m_p; return true; }
          break;

        default:
          if (st.depth == 0) {
            if (is_ws(c)) {
              if (st.started && st.scalar) { value_end = m_p; return true; }
              ++m_p;
            } else if (!st.started) {
              // start of a bare scalar: number, true/false/null
              st.started = true;
              st.scalar  = true;
              ++m_p;
            } else if (st.scalar) {
              ++m_p;
            } else {
              // garbage after a completed container value
              value_end = m_p;
              return true;
            }
          } else {
            ++m_p; // inside a container: commas, colons, scalar bytes — just consume
          }
          break;
      }
    }

    // Ran out of input — record where to resume from on the next call. Note
    // that even a "complete-looking" bare scalar at the buffer boundary is
    // ambiguous (more digits/letters could follow in the next chunk), so we
    // always report incomplete here and let the caller feed more data or
    // signal EOF explicitly.
    st.pos = static_cast<uint64_t>(m_p - m_beg);
    return false;
  }
};

inline ERL_NIF_TERM scan_state_to_term(ErlNifEnv* env, const ScanState& st)
{
  return enif_make_tuple6(env,
    enif_make_uint64(env, st.pos),
    enif_make_uint(env, st.depth),
    st.in_string ? AM_TRUE : AM_FALSE,
    st.escape    ? AM_TRUE : AM_FALSE,
    st.started   ? AM_TRUE : AM_FALSE,
    st.scalar    ? AM_TRUE : AM_FALSE);
}

inline bool scan_state_from_term(ErlNifEnv* env, ERL_NIF_TERM term, ScanState& st)
{
  int arity; const ERL_NIF_TERM* tp;
  if (!enif_get_tuple(env, term, &arity, &tp) || arity != 6) return false;

  ErlNifUInt64 pos;
  unsigned     depth;
  if (!enif_get_uint64(env, tp[0], &pos))   return false;
  if (!enif_get_uint(env, tp[1], &depth))   return false;
  st.pos       = pos;
  st.depth     = depth;
  st.in_string = enif_is_identical(tp[2], AM_TRUE);
  st.escape    = enif_is_identical(tp[3], AM_TRUE);
  st.started   = enif_is_identical(tp[4], AM_TRUE);
  st.scalar    = enif_is_identical(tp[5], AM_TRUE);
  return true;
}

//-----------------------------------------------------------------------------
// Direct Erlang-term → JSON encoder (no intermediate generic_u64 tree)
//-----------------------------------------------------------------------------

// ESCAPE_TAB, NEEDS_ESCAPE_TAB, and EscapeEntry are defined in glazer_common.hpp.

// find_escape_pos is defined in glazer_common.hpp and shared with glazer_yaml.hpp.

// JSON-escape a UTF-8 byte sequence into out.
// Pre-reserves worst-case space (6 bytes per input byte + 2 quotes) in one
// shot, then writes into the already-reserved tail via raw pointer — no
// further ensure() calls inside the loop.  find_escape_pos bulk-skips clean
// runs (NEON/AVX2/SSE2/table); ESCAPE_TAB handles special bytes with a
// single indexed load + memcpy instead of a switch branch.
static void json_escape_string(std::string_view sv, OutBuf& out)
{
  // Worst case: every byte escapes to 6 chars (\uXXXX), plus 2 quotes.
  out.ensure(sv.size() * 6 + 2);

  char* dst       = out.m_data + out.m_len;
  *dst++          = '"';
  const char* p   = sv.data();
  const char* end = p + sv.size();

  while (p < end) {
    const char* special = find_escape_pos(p, end);
    size_t      run     = static_cast<size_t>(special - p);
    if (run) { memcpy(dst, p, run); dst += run; }
    p = special;
    if (p >= end) break;

    const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
    memcpy(dst, e.seq, e.len);
    dst += e.len;
  }

  *dst++ = '"';
  out.m_len = static_cast<size_t>(dst - out.m_data);
}

// JSON-escape a UTF-8 byte sequence, additionally escaping every non-ASCII
// code point as \uXXXX (uescape), and/or replacing invalid UTF-8 byte
// sequences with U+FFFD before escaping (force_utf8).
static void json_escape_string_unicode(std::string_view sv, OutBuf& out,
                                       bool uescape, bool force_utf8)
{
  out.push('"');
  const char* p   = sv.data();
  const char* end = p + sv.size();
  const char* run = p;

  while (p < end) {
    auto c = (unsigned char)*p;

    if (c < 0x80) [[likely]] {
      if (!NEEDS_ESCAPE_TAB[c]) [[likely]] {
        ++p;
      } else {
        if (p > run) out.push(run, p - run);
        switch (c) {
          case '"':  out.push("\\\"", 2); break;
          case '\\': out.push("\\\\", 2); break;
          case '\b': out.push("\\b",  2); break;
          case '\f': out.push("\\f",  2); break;
          case '\n': out.push("\\n",  2); break;
          case '\r': out.push("\\r",  2); break;
          case '\t': out.push("\\t",  2); break;
          default:   push_uescape(out,c); break;
        }
        ++p;
        run = p;
      }
      continue;
    }

    // Non-ASCII: decode the code point (sanitizing invalid sequences when
    // force_utf8 is set; otherwise pass invalid bytes through verbatim).
    if (p > run) out.push(run, p - run);

    const char* seq_start = p;
    uint32_t cp = decode_utf8(p, end);

    if (uescape) {
      push_uescape(out, cp);
    } else if (force_utf8 && cp == 0xFFFD && !(p - seq_start == 3 &&
               uint8_t(seq_start[0]) == 0xEF &&
               uint8_t(seq_start[1]) == 0xBF &&
               uint8_t(seq_start[2]) == 0xBD)) {
      // Invalid sequence sanitized to U+FFFD (and it wasn't already a
      // literal U+FFFD in the input) — emit the replacement character.
      out.push("\xEF\xBF\xBD", 3);
    } else {
      out.push(seq_start, p - seq_start);
    }
    run = p;
  }
  if (p > run) out.push(run, p - run);
  out.push('"');
}

struct JsonEncoder {
  ErlNifEnv*        m_env;
  const EncodeOpts& m_opts;
  OutBuf&           m_out;
  char              m_atom_buf[256]; // scratch for atom → string_view
  const char*       m_err;
  ERL_NIF_TERM      m_err_term;

  void escape_string(std::string_view sv)
  {
    if (m_opts.uescape || m_opts.force_utf8)
      json_escape_string_unicode(sv, m_out, m_opts.uescape, m_opts.force_utf8);
    else
      json_escape_string(sv, m_out);
  }

  bool encode(ERL_NIF_TERM term)
  {
    // Dispatch on the term's runtime type once — avoids the cascade of
    // enif_is_identical / enif_get_* probes that each cost a C call.
    switch (enif_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, term, &bin)) return false;
        escape_string({reinterpret_cast<const char*>(bin.data), bin.size});
        return true;
      }

      case ERL_NIF_TERM_TYPE_INTEGER:
        return glz::BigInt::encode(m_env, term, m_out);

      case ERL_NIF_TERM_TYPE_MAP: {
        m_out.push('{');
        auto iter = MapIterator::create(m_env, term);
        if (!iter) [[unlikely]] return false;
        ERL_NIF_TERM k, v;
        bool first = true;
        while (iter->get_pair(&k, &v)) {
          if (!first) m_out.push(',');
          first = false;
          if (!encode_key(k)) [[unlikely]] return error("cannot encode key", k);
          m_out.push(':');
          if (!encode(v))     [[unlikely]] return error("cannot encode value", v);
          iter->next();
        }
        m_out.push('}');
        return true;
      }

      case ERL_NIF_TERM_TYPE_LIST: {
        m_out.push('[');
        ERL_NIF_TERM h, t = term;
        bool first = true;
        while (enif_get_list_cell(m_env, t, &h, &t)) {
          if (!first) m_out.push(',');
          first = false;
          if (!encode(h)) [[unlikely]]
            return error("cannot encode list element", h);
        }
        if (!enif_is_empty_list(m_env, t)) [[unlikely]]  // improper list
          return error("improper list", t);
        m_out.push(']');
        return true;
      }

      case ERL_NIF_TERM_TYPE_ATOM: {
        if (enif_is_identical(term, m_opts.null_term)) { m_out.push("null", 4); return true; }
        if (enif_is_identical(term, AM_TRUE))  { m_out.push("true",  4); return true; }
        if (enif_is_identical(term, AM_FALSE)) { m_out.push("false", 5); return true; }
        if (enif_is_identical(term, AM_NULL))  { m_out.push("null",  4); return true; }
        if (enif_is_identical(term, AM_NIL))   { m_out.push("null",  4); return true; }
        std::string_view sv;
        if (!atom_to_sv(m_env, term, m_atom_buf, sizeof(m_atom_buf), sv)) [[unlikely]]
          return error("cannot convert atom to string", term);
        escape_string(sv);
        return true;
      }

      case ERL_NIF_TERM_TYPE_FLOAT: {
        double d;
        if (!enif_get_double(m_env, term, &d))
          return error("not a float", term);
        if (!std::isfinite(d)) {
          m_out.push("null", 4);
          return true;
        }
        // chars_format::general produces the shortest round-trip representation
        // (same as ryu's output), which is typically shorter than %.17g.
        char buf[32];
        auto [e, ec] = std::to_chars(buf, buf+32, d, std::chars_format::general);
        if (ec == std::errc{}) [[likely]] {
          bool has_dot = false;
          for (char* p = buf; p < e; ++p) {
            if (*p == '.' || *p == 'e' || *p == 'E') { has_dot = true; break; }
          }
          m_out.push(buf, e - buf);
          if (!has_dot) m_out.push(".0", 2);
        } else {
          // Fallback: should never happen for finite doubles, but be safe.
          int n = snprintf(buf, sizeof(buf), "%.17g", d);
          m_out.push(buf, n);
        }
        return true;
      }

      case ERL_NIF_TERM_TYPE_TUPLE: {
        // {[{K,V}...]} proplist → object
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(m_env, term, &arity, &tp);
        if (arity == 1 && enif_is_list(m_env, tp[0])) [[likely]] {
          m_out.push('{');
          ERL_NIF_TERM h, t = tp[0];
          bool first = true;
          while (enif_get_list_cell(m_env, t, &h, &t)) {
            int pa; const ERL_NIF_TERM* pp;
            if (!enif_get_tuple(m_env, h, &pa, &pp) || pa != 2) [[unlikely]]
              return error("not a tuple", h);
            if (!first) m_out.push(',');
            first = false;
            if (!encode_key(pp[0])) [[unlikely]]
              return error("cannot encode key", pp[0]);
            m_out.push(':');
            if (!encode(pp[1])) [[unlikely]]
              return error("cannot encode value", pp[1]);
          }
          m_out.push('}');
          return true;
        }
        return error("tuple is not an object", term);
      }

      default:
        return false;
    }
  }

  bool encode_key(ERL_NIF_TERM k)
  {
    ErlNifBinary bin;
    if (enif_inspect_binary(m_env, k, &bin)) {
      escape_string({reinterpret_cast<const char*>(bin.data), bin.size});
      return true;
    }
    if (enif_is_atom(m_env, k)) {
      std::string_view sv;
      if (!atom_to_sv(m_env, k, m_atom_buf, sizeof(m_atom_buf), sv)) return false;

      escape_string(sv);
      return true;
    }
    return false;
  }
private:
  template <int N>
  bool error(const char (&err)[N], ERL_NIF_TERM term) {
    m_err = err;
    m_err_term = term;
    return false;
  }
};

} // namespace glz
