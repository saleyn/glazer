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
#if defined(__AVX2__)
#  include <immintrin.h>
#elif defined(__SSE2__)
#  include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#endif

#include "vendors/glazer_atof.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_common.hpp"
#include "glazer_utf8_validate.hpp"
#include "vendors/glazer_ftoa.hpp"

// The encoder's string tail may do one 16-byte load that reads past the end of
// the string (never past a 4 KiB block, so never off a mapped page). Sanitizers
// flag that read, so under AddressSanitizer and MemorySanitizer the tail uses
// the bounded word path only. Define GLAZER_TAIL_BOUNDED to force it in any build.
#ifndef GLAZER_TAIL_BOUNDED
#  if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_MEMORY__)
#    define GLAZER_TAIL_BOUNDED 1
#  elif defined(__has_feature)
#    if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#      define GLAZER_TAIL_BOUNDED 1
#    endif
#  endif
#endif

// NEON movemask: returns a 16-bit mask where bit i is set if byte i is nonzero.
// Equivalent to SSE2 _mm_movemask_epi8 when applied to comparison results (0x00/0xFF).
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
static inline uint16_t neon_movemask(uint8x16_t v) {
  static const uint8_t pows[16] = {1,2,4,8,16,32,64,128, 1,2,4,8,16,32,64,128};
  uint8x16_t pw     = vld1q_u8(pows);
  uint8x16_t masked = vandq_u8(v, pw);
  uint8x8_t  lo     = vpadd_u8(vget_low_u8(masked), vget_high_u8(masked));
  lo = vpadd_u8(lo, lo);
  lo = vpadd_u8(lo, lo);
  return vget_lane_u16(vreinterpret_u16_u8(lo), 0);
}

// neon_movemask_highbit: extracts bit 7 of each byte (like SSE2 _mm_movemask_epi8
// on raw data, not comparison results). Used for ASCII detection.
static inline uint16_t neon_movemask_highbit(uint8x16_t v) {
  return neon_movemask(vshrq_n_u8(v, 7));
}

// Quick check: returns true if any byte in v has bit 7 set.
static inline bool neon_any_highbit(uint8x16_t v) {
  uint64x2_t h = vreinterpretq_u64_u8(vshrq_n_u8(v, 7));
  return (vgetq_lane_u64(h, 0) | vgetq_lane_u64(h, 1)) != 0;
}
#endif

namespace glz {

//-----------------------------------------------------------------------------
// Options
//-----------------------------------------------------------------------------

struct JSONDecodeOpts {
  bool         object_as_tuple     = false;
  ERL_NIF_TERM null_term           = 0;
  bool         hdr_atom          = false;
  bool         hdr_existing_atom = false;
  bool         dedupe_keys         = false;
  // When true, unescaped strings are copied into fresh binaries instead of
  // referencing the input via sub-binary.  Use this when decoded strings
  // will outlive the input binary by a large margin: without it, one live
  // string keeps the entire input buffer from being collected.
  bool         copy_strings        = false;
  // When true, trailing non-whitespace data after the decoded value is not
  // an error: decode() returns {has_trailer, Value, Rest} instead, letting
  // callers split a value off a buffer without a separate scan() pass.
  bool         return_trailer      = false;
  // When true, validate that JSON strings contain valid UTF-8 sequences.
  // On by default: invalid UTF-8 in a string is a decode error. Use
  // skip_utf8_validation to accept such bytes unchecked.
  bool         validate_utf8       = true;
};

struct JSONEncodeOpts {
  bool         pretty            = false;
  bool         uescape           = false;
  bool         force_utf8        = false;
  bool         escape_fwd_slash  = false;
  ERL_NIF_TERM null_term         = 0;
};

//-----------------------------------------------------------------------------
// Option parsing
//-----------------------------------------------------------------------------

inline bool parse_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, JSONDecodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_OBJECT_AS_TUPLE))      opts.object_as_tuple = true;
    else if (enif_is_identical(head, AM_USE_NIL))              opts.null_term       = AM_NIL;
    else if (enif_is_identical(head, AM_DEDUPE_KEYS))          opts.dedupe_keys     = true;
    else if (enif_is_identical(head, AM_COPY_STRINGS))         opts.copy_strings    = true;
    else if (enif_is_identical(head, AM_RETURN_TRAILER))       opts.return_trailer  = true;
    else if (enif_is_identical(head, AM_VALIDATE_UTF8))        opts.validate_utf8   = true;
    else if (enif_is_identical(head, AM_SKIP_UTF8_VALIDATION)) opts.validate_utf8   = false;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2) {
        const ERL_NIF_TERM opt = tp[0], val = tp[1];
        if ((enif_is_identical(opt, AM_NULL_TERM) ||
             enif_is_identical(opt, AM_NULL)) && enif_is_atom(env, val))
          opts.null_term = val;
        else if (enif_is_identical(opt, AM_KEYS) || enif_is_identical(opt, AM_ATOM)) {
          if      (enif_is_identical(val, AM_ATOM))           opts.hdr_atom = true;
          else if (enif_is_identical(val, AM_EXISTING_ATOM))  opts.hdr_existing_atom = true;
          else if (enif_is_identical(val, AM_LABEL_BINARY)) { opts.hdr_atom = false; opts.hdr_existing_atom = false; }
        }
      }
    }
  }
  return true;
}

inline bool parse_encode_opts(ErlNifEnv* env, ERL_NIF_TERM list, JSONEncodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_PRETTY))           opts.pretty           = true;
    else if (enif_is_identical(head, AM_USE_NIL))          opts.null_term        = AM_NIL;
    else if (enif_is_identical(head, AM_UESCAPE))          opts.uescape          = true;
    else if (enif_is_identical(head, AM_FORCE_UTF8))       opts.force_utf8       = true;
    else if (enif_is_identical(head, AM_ESCAPE_FWD_SLASH)) opts.escape_fwd_slash = true;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2) {
        const ERL_NIF_TERM opt = tp[0], val = tp[1];
        if ((enif_is_identical(opt, AM_NULL_TERM) ||
             enif_is_identical(opt, AM_NULL)) && enif_is_atom(env, val))
          opts.null_term = val;
      }
    }
  }
  return true;
}

//-----------------------------------------------------------------------------
// JSON \X single-character escape lookup — shared by JSON's unescape().
// table[c] == 0 means "not a recognized single-char escape" (covers '\uXXXX'
// and the default/pass-through case, both handled separately by the caller).
//-----------------------------------------------------------------------------
static constexpr auto JSON_ESCAPE_CHAR_TABLE = [] {
  std::array<char, 256> t{};
  t['"']  = '"';
  t['\\'] = '\\';
  t['/']  = '/';
  t['b']  = '\b';
  t['f']  = '\f';
  t['n']  = '\n';
  t['r']  = '\r';
  t['t']  = '\t';
  return t;
}();

//-----------------------------------------------------------------------------
// Zero-copy JSON decoder — parses raw bytes, emits Erlang terms directly
//-----------------------------------------------------------------------------

// Offset of the first invalid UTF-8 byte, or UTF8_OK when the input is valid.
// Validity is decided by the vector validator (glazer_utf8_validate.hpp); the offset
// is only computed when the input is invalid.
inline constexpr size_t UTF8_OK = static_cast<size_t>(-1);
inline size_t first_invalid_utf8(const char* s, size_t len)
{
  if (glz::utf8::valid(s, len)) return UTF8_OK;
  return glz::utf8::first_invalid(s, len);
}

struct JSONDecoder {
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
  static constexpr unsigned MAX_DEPTH = 128;

  JSONDecoder(ErlNifEnv*   e,    const JSONDecodeOpts& o,
              const char*  data, size_t size,
              ERL_NIF_TERM input_bin)
  : m_env(e), m_opts(o)
  , m_beg(data), m_p(data)
  , m_end(data + size)
  , m_input_bin(input_bin)
  , m_use_key_cache(size >= KEY_CACHE_MIN_SIZE)
  {}

  // Always returns {ok, Term} | {error, Msg}.
  // Raising vs. non-raising behaviour is the Erlang caller's responsibility.
  std::tuple<bool, ERL_NIF_TERM> decode(const char* data, size_t size)
  {
    // UTF-8 validation is done per-string instead of upfront to avoid
    // scanning the entire input twice.  Unescaped strings are validated in
    // make_string_term / make_key_term; escaped strings are validated
    // after unescape().
    m_p = data; m_end = data + size; m_beg = data;
    std::string scratch;
    ERL_NIF_TERM result = parse_value(scratch);
    if (result) skip_ws();
    if (result && m_p == m_end) [[likely]]
      return std::make_tuple(true, result);

    if (result && m_opts.return_trailer) {
      ERL_NIF_TERM rest = make_span_term(m_env, m_input_bin, m_beg, m_end,
                                          std::string_view(m_p, m_end - m_p), false);
      return std::make_tuple(true, enif_make_tuple3(m_env, AM_HAS_TRAILER, result, rest));
    }

    std::string msg = m_err.empty()
      ? "JSON parse error at offset " + std::to_string(m_p - m_beg)
      : m_err + " at offset " + std::to_string(m_p - m_beg);
    return std::make_tuple(false, make_binary(m_env, msg));
  }

private:
  ErlNifEnv*        m_env;
  const JSONDecodeOpts& m_opts;
  const char*       m_beg;  // start of input (for error reporting)
  const char*       m_p;    // current position
  const char*       m_end;
  ERL_NIF_TERM      m_input_bin; // original binary term — used for zero-copy sub_binary
  KeyCache          m_key_cache;
  bool              m_use_key_cache;
  // Shared per-call stacks for container members (see parse_object/parse_array).
  SmallTermVec<64>  m_stk_keys;
  SmallTermVec<64>  m_stk_vals;
  unsigned          m_depth = 0;
  bool              m_last_string_ascii = true;
  std::string       m_err;

  // Increments the shared depth counter for the lifetime of a parse_array /
  // parse_object call, so every return path (including early `return 0`)
  // restores it.
  struct DepthGuard {
    explicit DepthGuard(JSONDecoder* d) : d(d) { ++d->m_depth; }
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
    JSONDecoder* d;
  };

  // ---- whitespace ----
  static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  void skip_ws() {
    if (m_p >= m_end) return;
    const unsigned char c = static_cast<unsigned char>(*m_p);
    if (c > 0x20 || !is_ws(static_cast<char>(c))) return;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    {
      const uint8x16_t vsp = vdupq_n_u8(' ');
      const uint8x16_t vnl = vdupq_n_u8('\n');
      const uint8x16_t vtb = vdupq_n_u8('\t');
      const uint8x16_t vcr = vdupq_n_u8('\r');
      while (m_p + 16 <= m_end) {
        uint8x16_t v  = vld1q_u8(reinterpret_cast<const uint8_t*>(m_p));
        uint8x16_t ws = vorrq_u8(vorrq_u8(vceqq_u8(v, vsp), vceqq_u8(v, vnl)),
                                  vorrq_u8(vceqq_u8(v, vtb), vceqq_u8(v, vcr)));
        uint16_t mask = neon_movemask(ws);
        if (mask != 0xFFFF) { m_p += __builtin_ctz(~mask); return; }
        m_p += 16;
      }
    }
#elif defined(__SSE2__)
    const __m128i vsp = _mm_set1_epi8(' ');
    const __m128i vnl = _mm_set1_epi8('\n');
    const __m128i vtb = _mm_set1_epi8('\t');
    const __m128i vcr = _mm_set1_epi8('\r');
    while (m_p + 16 <= m_end) {
      __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m_p));
      __m128i ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, vsp), _mm_cmpeq_epi8(v, vnl)),
                                _mm_or_si128(_mm_cmpeq_epi8(v, vtb), _mm_cmpeq_epi8(v, vcr)));
      int mask = _mm_movemask_epi8(ws);
      if (mask != 0xFFFF) { m_p += __builtin_ctz(~mask); return; }
      m_p += 16;
    }
#endif
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
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    // NEON (16 B/iter), same tier as find_escape_pos on Apple Silicon/ARM64.
    {
      const uint8x16_t vq = vdupq_n_u8('"');
      const uint8x16_t vb = vdupq_n_u8('\\');
      while (m_p + 16 <= m_end) {
        uint8x16_t v   = vld1q_u8(reinterpret_cast<const uint8_t*>(m_p));
        uint8x16_t hit = vorrq_u8(vceqq_u8(v, vq), vceqq_u8(v, vb));
        uint64x2_t h64 = vreinterpretq_u64_u8(hit);
        uint64_t   lo  = vgetq_lane_u64(h64, 0);
        uint64_t   hi  = vgetq_lane_u64(h64, 1);
        if (lo | hi) {
          m_p += lo ? (__builtin_ctzll(lo) >> 3) : 8 + (__builtin_ctzll(hi) >> 3);
          return;
        }
        m_p += 16;
      }
    }
#endif
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
  // Scans to the first '"' or '\\' and reports which one it is: 1 = quote at
  // m_p, 2 = backslash at m_p, 0 = end of input first. The quote-vs-backslash
  // decision comes from the same SIMD masks that located the byte, so the
  // caller does not re-read it.
  int scan_special() noexcept {
#if defined(__AVX2__)
    {
      const __m256i vq = _mm256_set1_epi8('"');
      const __m256i vb = _mm256_set1_epi8('\\');
      while (m_p + 32 <= m_end) {
        __m256i  v  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(m_p));
        uint32_t mq = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vq));
        uint32_t mb = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vb));
        uint32_t m  = mq | mb;
        if (m) {
          unsigned i = __builtin_ctz(m);
          m_p += i;
          return ((mq >> i) & 1u) ? 1 : 2;
        }
        m_p += 32;
      }
    }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    {
      const uint8x16_t vq = vdupq_n_u8('"');
      const uint8x16_t vb = vdupq_n_u8('\\');
      while (m_p + 16 <= m_end) {
        uint8x16_t v  = vld1q_u8(reinterpret_cast<const uint8_t*>(m_p));
        uint16_t   mq = neon_movemask(vceqq_u8(v, vq));
        uint16_t   mb = neon_movemask(vceqq_u8(v, vb));
        uint16_t   m  = mq | mb;
        if (m) {
          unsigned i = __builtin_ctz(m);
          m_p += i;
          return ((mq >> i) & 1u) ? 1 : 2;
        }
        m_p += 16;
      }
    }
#elif defined(__SSE2__)
    {
      const __m128i vq = _mm_set1_epi8('"');
      const __m128i vb = _mm_set1_epi8('\\');
      while (m_p + 16 <= m_end) {
        __m128i  v  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m_p));
        unsigned mq = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vq));
        unsigned mb = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vb));
        unsigned m  = mq | mb;
        if (m) {
          unsigned i = __builtin_ctz(m);
          m_p += i;
          return ((mq >> i) & 1u) ? 1 : 2;
        }
        m_p += 16;
      }
    }
#endif
    while (m_p < m_end) {
      char c = *m_p;
      if (c == '"') return 1;
      if (c == '\\') return 2;
      ++m_p;
    }
    return 0;
  }

  bool read_string_raw(const char*& begin_out, size_t& len_out, bool& has_escape)
  {
    if (m_p >= m_end || *m_p != '"')
      return false;

    ++m_p;  // skip opening quote
    const char* s = m_p;
    has_escape = false;
    m_last_string_ascii = true;
    for (;;) {
#if defined(__AVX2__)
      {
        const __m256i vq = _mm256_set1_epi8('"');
        const __m256i vb = _mm256_set1_epi8('\\');
        __m256i vacc = _mm256_setzero_si256();
        while (m_p + 32 <= m_end) {
          __m256i  v  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(m_p));
          uint32_t mq = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vq));
          uint32_t mb = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vb));
          uint32_t m  = mq | mb;
          if (m) {
            unsigned i = __builtin_ctz(m);
            m_p += i;
            if ((mq >> i) & 1u) {
              vacc = _mm256_or_si256(vacc, v);
              m_last_string_ascii = (_mm256_movemask_epi8(vacc) == 0);
              begin_out = s;
              len_out = static_cast<size_t>(m_p - s);
              ++m_p;
              return true;
            }
            break;
          }
          vacc = _mm256_or_si256(vacc, v);
          m_p += 32;
        }
        m_last_string_ascii = (_mm256_movemask_epi8(vacc) == 0);
      }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
      {
        const uint8x16_t vq = vdupq_n_u8('"');
        const uint8x16_t vb = vdupq_n_u8('\\');
        uint8x16_t     vacc = vdupq_n_u8(0);
        while (m_p + 16 <= m_end) {
          uint8x16_t v  = vld1q_u8(reinterpret_cast<const uint8_t*>(m_p));
          uint16_t   mq = neon_movemask(vceqq_u8(v, vq));
          uint16_t   mb = neon_movemask(vceqq_u8(v, vb));
          uint16_t   m  = mq | mb;
          if (m) {
            unsigned i = __builtin_ctz(m);
            m_p += i;
            if ((mq >> i) & 1u) {
              vacc = vorrq_u8(vacc, v);
              m_last_string_ascii = !neon_any_highbit(vacc);
              begin_out = s;
              len_out = static_cast<size_t>(m_p - s);
              ++m_p;
              return true;
            }
            break;
          }
          vacc = vorrq_u8(vacc, v);
          m_p += 16;
        }
        m_last_string_ascii = !neon_any_highbit(vacc);
      }
#elif defined(__SSE2__) && !defined(__AVX2__)
      {
        const __m128i vq = _mm_set1_epi8('"');
        const __m128i vb = _mm_set1_epi8('\\');
        __m128i     vacc = _mm_setzero_si128();
        while (m_p + 16 <= m_end) {
          __m128i  v  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m_p));
          unsigned mq = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vq));
          unsigned mb = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vb));
          unsigned m  = mq | mb;
          if (m) {
            unsigned i = __builtin_ctz(m);
            m_p += i;
            if ((mq >> i) & 1u) {
              vacc = _mm_or_si128(vacc, v);
              m_last_string_ascii = (_mm_movemask_epi8(vacc) == 0);
              begin_out = s;
              len_out = static_cast<size_t>(m_p - s);
              ++m_p;
              return true;
            }
            break;
          }
          vacc = _mm_or_si128(vacc, v);
          m_p += 16;
        }
        m_last_string_ascii = (_mm_movemask_epi8(vacc) == 0);
      }
#endif
      while (m_p < m_end) {
        char c = *m_p;
        if (c == '"') {
          begin_out = s;
          len_out = static_cast<size_t>(m_p - s);
          ++m_p;
          return true;
        }
        if (c == '\\') break;
        if (static_cast<unsigned char>(c) > 0x7F) m_last_string_ascii = false;
        ++m_p;
      }

      if (m_p >= m_end) return false;
      if (*m_p != '\\') return false;
      has_escape = true;
      ++m_p;
      if (m_p < m_end) ++m_p;
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
    auto hex4 = [](const char* p) {
      int v = 0;
      for (int i = 0; i < 4; ++i) {
        int d = hex_digit_value(static_cast<unsigned char>(p[i]));
        if (d < 0) return -1;
        v = v * 16 + d;
      }
      return v;
    };
    while (s < end) {
      // Bulk-copy clean bytes up to the next backslash
      auto bs = static_cast<const char*>(memchr(s, '\\', static_cast<size_t>(end - s)));
      if (!bs) { buf.append(s, static_cast<size_t>(end - s)); break; }
      if (bs > s) buf.append(s, static_cast<size_t>(bs - s));
      s = bs + 1;
      if (s >= end) break;
      char ec = *s++;
      if (ec != 'u') {
        char rep = JSON_ESCAPE_CHAR_TABLE[static_cast<unsigned char>(ec)];
        buf += rep ? rep : ec;
        continue;
      }
      if (s + 4 > end) continue;
      int cp = hex4(s); s += 4;
      if (cp >= 0xD800 && cp <= 0xDBFF && s + 6 <= end && s[0] == '\\' && s[1] == 'u') {
        int lo = hex4(s + 2); s += 6;
        if (lo >= 0xDC00 && lo <= 0xDFFF)
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
      }
      if      (cp < 0x80)    { buf += (char)cp; }
      else if (cp < 0x800)   { buf += (char)(0xC0|(cp>>6)); buf += (char)(0x80|(cp&0x3F)); }
      else if (cp < 0x10000) {
        buf += (char)(0xE0|(cp>>12));
        buf += (char)(0x80|((cp>>6)&0x3F));
        buf += (char)(0x80|(cp&0x3F));
      } else {
        buf += (char)(0xF0|(cp>>18));
        buf += (char)(0x80|((cp>>12)&0x3F));
        buf += (char)(0x80|((cp>>6)&0x3F));
        buf += (char)(0x80|(cp&0x3F));
      }
    }
    return buf;
  }

  inline bool is_valid_utf8(const char* s, size_t len) { return glz::utf8::valid(s, len); }

  // Make an Erlang binary from a JSON string span (handles escapes).
  // Default (copy_strings == false): unescaped strings are returned as
  // sub-binaries of the original input — zero allocation, but the input
  // binary stays alive as long as any sub-binary referencing it does.
  // With copy_strings == true: always allocates a fresh binary, allowing the
  // GC to reclaim the input buffer independently of the decoded results.
  bool check_utf8(const char* s, size_t len) {
    if (!m_opts.validate_utf8) [[likely]] return true;
    if (m_last_string_ascii) [[likely]] return true;
    return first_invalid_utf8(s, len) == UTF8_OK;
  }

  __attribute__((always_inline))
  ERL_NIF_TERM make_string_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    if (!has_escape) [[likely]] {
      if (!check_utf8(s, len)) [[unlikely]] {
        m_err = "invalid UTF-8 in JSON string";
        return 0;
      }
      return make_span_term(m_env, m_input_bin, m_beg, m_end, std::string_view(s, len), m_opts.copy_strings);
    }

    std::string_view unescaped = unescape(s, len, buf);
    // m_last_string_ascii reflects the raw (escaped) bytes, not unescaped output
    if (m_opts.validate_utf8 && first_invalid_utf8(unescaped.data(), unescaped.size()) != UTF8_OK) {
      m_err = "invalid UTF-8 in JSON string";
      return 0;
    }
    return make_binary(m_env, unescaped);
  }

  __attribute__((always_inline))
  ERL_NIF_TERM make_key_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    if (!has_escape && m_use_key_cache) {
      uint64_t pre, tail;
      uint32_t h = KeyCache::hash_and_words(s, len, pre, tail);
      if (ERL_NIF_TERM cached = m_key_cache.lookup_with_words(s, len, h, pre, tail))
        return cached;
      if (!check_utf8(s, len)) {
        m_err = "invalid UTF-8 in JSON string";
        return 0;
      }
      std::string_view sv(s, len);
      auto term = make_binary(m_env, sv);
      m_key_cache.insert_with_words(s, len, h, pre, tail, term);
      return term;
    }

    std::string_view sv;
    if (has_escape) {
      sv = unescape(s, len, buf);
      if (m_opts.validate_utf8 && first_invalid_utf8(sv.data(), sv.size()) != UTF8_OK) {
        m_err = "invalid UTF-8 in JSON string";
        return 0;
      }
    } else {
      sv = std::string_view(s, len);
      if (!check_utf8(sv.data(), sv.size())) {
        m_err = "invalid UTF-8 in JSON string";
        return 0;
      }
    }

    if (m_opts.hdr_atom) {
      return enif_make_atom_len(m_env, sv.data(), sv.size());
    }
    if (m_opts.hdr_existing_atom) {
      ERL_NIF_TERM t;
      return enif_make_existing_atom_len(m_env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1)
           ? t : make_binary(m_env, sv);
    }
    return make_binary(m_env, sv);
  }

  // ---- number parsing ----
  ERL_NIF_TERM parse_number()
  {
    const char* start = m_p;
    bool neg = (*m_p == '-');
    if (neg) ++m_p;

    // Integer part. Digits are accumulated in this same pass, so integers do
    // not rescan them. At most 18 digits fit in uint64_t; longer runs take the
    // bigint path below.
    const char* digits = m_p;
    uint64_t acc = 0;
    while (m_p < m_end && *m_p >= '0' && *m_p <= '9') {
      acc = acc * 10 + static_cast<uint64_t>(*m_p - '0');
      ++m_p;
    }
    const size_t ndigits = static_cast<size_t>(m_p - digits);

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
      // (e.g. older Apple libc++), so use glazer_atof.hpp here.
      auto [ep, ec] = glz::fp::from_chars(start, m_p, d);
      if (ec != std::errc{}) return 0;
      return enif_make_double(m_env, d);
    }

    // Integer: 1..18 digits were accumulated above and always fit in int64.
    if (ndigits >= 1 && ndigits <= 18) [[likely]] {
      int64_t v = static_cast<int64_t>(acc);
      return enif_make_int64(m_env, neg ? -v : v);
    }
    // Empty or longer than 18 digits: bigint fallback (below).
    // Bigint
    ERL_NIF_TERM r = glz::BigInt::decode(m_env, start, m_p);
    return r ? r : (ERL_NIF_TERM)0;
  }

  // ---- value-type dispatch table ----
  enum ValueAction : uint8_t {
    VA_ERR = 0, VA_STR, VA_OBJ, VA_ARR,
    VA_TRUE, VA_FALSE, VA_NULL, VA_NUM
  };

  static constexpr auto make_value_action_table() {
    struct Table { uint8_t data[256] = {}; };
    Table t{};
    t.data[(unsigned char)'"'] = VA_STR;
    t.data[(unsigned char)'{'] = VA_OBJ;
    t.data[(unsigned char)'['] = VA_ARR;
    t.data[(unsigned char)'t'] = VA_TRUE;
    t.data[(unsigned char)'f'] = VA_FALSE;
    t.data[(unsigned char)'n'] = VA_NULL;
    t.data[(unsigned char)'-'] = VA_NUM;
    for (unsigned char c = '0'; c <= '9'; ++c) t.data[c] = VA_NUM;
    return t;
  };

  // ---- core value parser ----
  // A table lookup maps the first byte to a dense 0-7 enum, which the
  // compiler lowers to a jump table.  A direct switch on the char produced
  // a long branch chain (range 0x22-0x7B = 89 slots, too sparse for a
  // jump table) — the table indirection cuts ~24% of decode instructions.
  __attribute__((always_inline))
  ERL_NIF_TERM parse_value(std::string& scratch)
  {
    skip_ws();
    if (m_p >= m_end) [[unlikely]]
      return 0;

    static constexpr auto tbl = make_value_action_table();
    switch (tbl.data[(unsigned char)*m_p]) {
      case VA_STR: {
        const char* s; size_t len; bool has_escape;
        if (!read_string_raw(s, len, has_escape)) return 0;
        return make_string_term(s, len, has_escape, scratch);
      }

      case VA_OBJ: return parse_object(scratch);
      case VA_ARR: return parse_array(scratch);

      case VA_TRUE:
        if (m_p + 4 <= m_end && memcmp(m_p+1, "rue", 3) == 0) {
          m_p += 4;
          return AM_TRUE;
        }
        return 0;
      case VA_FALSE:
        if (m_p + 5 <= m_end && memcmp(m_p+1, "alse", 4) == 0) {
          m_p += 5;
          return AM_FALSE;
        }
        return 0;
      case VA_NULL:
        if (m_p + 4 <= m_end && memcmp(m_p+1, "ull", 3) == 0) {
          m_p += 4;
          return m_opts.null_term;
        }
        return 0;

      case VA_NUM:
        return parse_number();

      default:
        return 0;
    }
  }

  ERL_NIF_TERM parse_array(std::string& scratch)
  {
    assert(*m_p == '[');
    DepthGuard guard(this);
    if (!guard.check()) [[unlikely]] return 0;

    const size_t base = m_stk_vals.size();
    if (m_p < m_end && *m_p == ']') {
      ++m_p;
      return enif_make_list_from_array(m_env, nullptr, 0);
    }

    for (;;) {
      auto v = parse_value(scratch);
      if (!v) [[unlikely]] return 0;
      m_stk_vals.push_back(v);
      skip_ws();
      if (m_p >= m_end) [[unlikely]] return 0;
      if (*m_p == ']')  { ++m_p; break; }
      if (*m_p != ',')  [[unlikely]] return 0;
      ++m_p;
    }
    const size_t n = m_stk_vals.size() - base;
    ERL_NIF_TERM list = enif_make_list_from_array(m_env, m_stk_vals.data() + base, unsigned(n));
    m_stk_vals.set_size(base);
    return list;
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

  static constexpr size_t FLATMAP_LIMIT       = 32;
  static constexpr size_t MIN_ORDERED_MEMBERS = 4;

  // Pre-sort keys into Erlang binary term order so ERTS's
  // erts_validate_and_sort_flatmap does a linear pass instead of insertion sort.
  // Returns true if the permutation was applied (keys/vals reordered in-place).
  static bool presort_binary_keys(ErlNifEnv* env,
                                  ERL_NIF_TERM* keys, ERL_NIF_TERM* vals, size_t n)
  {
    struct KeyOrd { uint64_t prefix; const unsigned char* ptr; size_t len; };
    KeyOrd ords[FLATMAP_LIMIT];

    for (size_t i = 0; i < n; ++i) {
      ErlNifBinary bin;
      if (!enif_inspect_binary(env, keys[i], &bin)) return false;
      uint64_t pre = 0;
      size_t   cp  = bin.size < 8 ? bin.size : 8;
      memcpy(&pre, bin.data, cp);
      pre = __builtin_bswap64(pre);
      ords[i] = {pre, bin.data, bin.size};
    }

    // Check if already sorted.
    bool sorted = true;
    for (size_t i = 1; i < n; ++i) {
      auto& a = ords[i - 1];
      auto& b = ords[i];
      if (a.prefix < b.prefix) continue;
      if (a.prefix > b.prefix) { sorted = false; break; }
      int cmp = memcmp(a.ptr, b.ptr, a.len < b.len ? a.len : b.len);
      if (cmp < 0) continue;
      if (cmp > 0 || a.len > b.len) { sorted = false; break; }
    }
    if (sorted) return false;

    // Insertion sort on a permutation index.
    uint8_t perm[FLATMAP_LIMIT];
    for (size_t i = 0; i < n; ++i) {
      size_t j = i;
      while (j > 0) {
        auto& a = ords[perm[j - 1]];
        auto& b = ords[i];
        if (a.prefix < b.prefix) break;
        if (a.prefix == b.prefix) {
          int cmp = memcmp(a.ptr, b.ptr, a.len < b.len ? a.len : b.len);
          if (cmp < 0 || (cmp == 0 && a.len <= b.len)) break;
        }
        perm[j] = perm[j - 1];
        --j;
      }
      perm[j] = uint8_t(i);
    }

    ERL_NIF_TERM sk[FLATMAP_LIMIT], sv[FLATMAP_LIMIT];
    for (size_t i = 0; i < n; ++i) {
      sk[i] = keys[perm[i]];
      sv[i] = vals[perm[i]];
    }
    memcpy(keys, sk, n * sizeof(ERL_NIF_TERM));
    memcpy(vals, sv, n * sizeof(ERL_NIF_TERM));
    return true;
  }

  // Build a map from n parallel key/value terms. Same result as
  // SmallTermVec::to_erl_map: duplicate keys fall back to put-in-order (last wins),
  // and dedupe_keys rebuilds the map through put.
  ERL_NIF_TERM make_map(ERL_NIF_TERM* keys, ERL_NIF_TERM* vals, size_t n)
  {
    if (n >= MIN_ORDERED_MEMBERS && n <= FLATMAP_LIMIT &&
        !m_opts.hdr_atom && !m_opts.hdr_existing_atom) {
      presort_binary_keys(m_env, keys, vals, n);
    }

    ERL_NIF_TERM map = 0;
    if (!enif_make_map_from_arrays(m_env, keys, vals, unsigned(n), &map)) [[unlikely]] {
      map = enif_make_new_map(m_env);
      for (size_t i = 0; i < n; ++i) {
        ERL_NIF_TERM next;
        enif_make_map_put(m_env, map, keys[i], vals[i], &next);
        map = next;
      }
    }
    if (m_opts.dedupe_keys && map) {
      ERL_NIF_TERM deduped = enif_make_new_map(m_env);
      for (size_t i = 0; i < n; ++i) {
        ERL_NIF_TERM next;
        enif_make_map_put(m_env, deduped, keys[i], vals[i], &next);
        deduped = next;
      }
      return deduped;
    }
    return map;
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
        if (!key) [[unlikely]] return 0;

        if (m_p < m_end && *m_p == ':')
          ++m_p;
        else {
          skip_ws();
          if (m_p >= m_end || *m_p != ':') [[unlikely]] return 0;
          ++m_p;
        }

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

    const size_t base = m_stk_keys.size();
    const size_t vbase = m_stk_vals.size();

    if (m_p < m_end && *m_p == '}') {
      ++m_p;
      return enif_make_new_map(m_env);
    }

    for (;;) {
      const char* kstr;
      size_t      klen;
      bool        kesc;

      if (!read_string_raw(kstr, klen, kesc))
        [[unlikely]] return 0;

      auto key = make_key_term(kstr, klen, kesc, scratch);
      if (!key) [[unlikely]] return 0;
      m_stk_keys.push_back(key);

      if (m_p < m_end && *m_p == ':')
        ++m_p;
      else {
        skip_ws();
        if (m_p >= m_end || *m_p != ':') [[unlikely]] return 0;
        ++m_p;
      }

      auto val = parse_value(scratch);
      if (!val) [[unlikely]] return 0;
      m_stk_vals.push_back(val);
      skip_ws();

      if (m_p >= m_end) [[unlikely]] return 0;
      if (*m_p == '}') { ++m_p; break; }
      if (*m_p != ',') [[unlikely]] return 0;
      ++m_p;
      skip_ws();
    }

    const size_t n = m_stk_vals.size() - vbase;
    ERL_NIF_TERM map = make_map(m_stk_keys.data() + base, m_stk_vals.data() + vbase, n);
    m_stk_keys.set_size(base);
    m_stk_vals.set_size(vbase);
    return map;
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
  static const ERL_NIF_TERM s_atoms[] = {AM_FALSE, AM_TRUE};

  return enif_make_tuple6(env,
    enif_make_uint64(env, st.pos),
    enif_make_uint(env, st.depth),
    s_atoms[st.in_string],
    s_atoms[st.escape],
    s_atoms[st.started],
    s_atoms[st.scalar]
  );
}

inline bool scan_state_from_term(ErlNifEnv* env, ERL_NIF_TERM term, ScanState& st)
{
  int arity; const ERL_NIF_TERM* tp;
  if (enif_is_identical(term, AM_UNDEFINED)) return true;
  if (!enif_get_tuple(env, term, &arity, &tp) || arity != 6) return false;

  ErlNifUInt64 pos;
  unsigned     depth;
  if (!enif_get_uint64(env, tp[0], &pos)) return false;
  if (!enif_get_uint(env, tp[1], &depth)) return false;
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
inline void json_escape_string(std::string_view sv, OutBuf& out)
{
  out.ensure(sv.size() * 2 + 34);

  char* dst = out.m_data + out.m_len;
  *dst++ = '"';
  const char* p = sv.data();
  const char* end = p + sv.size();

  while (p < end) {
#if defined(__AVX2__)
    while (p + 32 <= end) {
      const __m256i  v      = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      const __m256i  vq     = _mm256_set1_epi8('"');
      const __m256i  vbs    = _mm256_set1_epi8('\\');
      const __m256i  vbias  = _mm256_set1_epi8(-128);
      const __m256i  vcmp   = _mm256_set1_epi8(-96);
      const __m256i  biased = _mm256_xor_si256(v, vbias);
      const uint32_t mask   = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
        _mm256_or_si256(_mm256_cmpgt_epi8(vcmp, biased), _mm256_cmpeq_epi8(v, vq)),
        _mm256_cmpeq_epi8(v, vbs)));
      if (mask) {
        const unsigned skip = __builtin_ctz(mask);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), v);
        dst += skip;
        p   += skip;
        const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
        memcpy(dst, e.seq, e.len);
        dst += e.len;
        continue;
      }
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), v);
      dst += 32;
      p += 32;
    }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    while (p + 16 <= end) {
      const uint8x16_t v      = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      const uint8x16_t vq     = vdupq_n_u8('"');
      const uint8x16_t vbs    = vdupq_n_u8('\\');
      const uint8x16_t vbias  = vdupq_n_u8(0x80);
      const int8x16_t  vcmp   = vdupq_n_s8(-96);
      const uint8x16_t biased = veorq_u8(v, vbias);
      const uint8x16_t hit    = vorrq_u8(
        vorrq_u8(vreinterpretq_u8_s8(vcgtq_s8(vcmp, vreinterpretq_s8_u8(biased))),
                 vceqq_u8(v, vq)),
        vceqq_u8(v, vbs));
      const uint16_t mask = neon_movemask(hit);
      if (mask) {
        const unsigned skip = __builtin_ctz(mask);
        vst1q_u8(reinterpret_cast<uint8_t*>(dst), v);
        dst += skip;
        p   += skip;
        const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
        memcpy(dst, e.seq, e.len);
        dst += e.len;
        continue;
      }
      vst1q_u8(reinterpret_cast<uint8_t*>(dst), v);
      dst += 16;
      p   += 16;
    }
#elif defined(__SSE2__)
    while (p + 16 <= end) {
      const __m128i  v      = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      const __m128i  vq     = _mm_set1_epi8('"');
      const __m128i  vbs    = _mm_set1_epi8('\\');
      const __m128i  vbias  = _mm_set1_epi8(-128);
      const __m128i  vcmp   = _mm_set1_epi8(-96);
      const __m128i  biased = _mm_xor_si128(v, vbias);
      const unsigned mask   = (unsigned)_mm_movemask_epi8(_mm_or_si128(
        _mm_or_si128(_mm_cmpgt_epi8(vcmp, biased), _mm_cmpeq_epi8(v, vq)),
        _mm_cmpeq_epi8(v, vbs)));
      if (mask) {
        const unsigned skip = __builtin_ctz(mask);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
        dst += skip;
        p   += skip;
        const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
        memcpy(dst, e.seq, e.len);
        dst += e.len;
        continue;
      }
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
      dst += 16;
      p   += 16;
    }
#endif
    if (p >= end) break;
    if (!NEEDS_ESCAPE_TAB[(unsigned char)*p]) {
      *dst++ = *p++;
      continue;
    }

    const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
    memcpy(dst, e.seq, e.len);
    dst += e.len;
  }

  *dst++ = '"';
  out.m_len = static_cast<size_t>(dst - out.m_data);
}

// Fused scan: validate UTF-8 + detect escape chars in one SIMD pass.
// No copy — scan only. Returns:
//  -1 = invalid UTF-8
//   0 = valid UTF-8, has escape chars (needs json_escape_string)
//   1 = valid UTF-8, no escape chars (safe for push_quoted)
// Replaces the separate first_invalid_utf8() + find_escape_pos() two-pass scan.
inline int json_scan_validate(const char* data, size_t len)
{
  const char* p   = data;
  const char* end = p + len;
  bool validated = false;
  bool has_escape = false;

#if defined(__AVX2__)
  while (p + 32 <= end) {
    const __m256i v      = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i vq     = _mm256_set1_epi8('"');
    const __m256i vbs    = _mm256_set1_epi8('\\');
    const __m256i vbias  = _mm256_set1_epi8(-128);
    const __m256i vcmp   = _mm256_set1_epi8(-96);
    const __m256i biased = _mm256_xor_si256(v, vbias);
    const uint32_t esc_mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
      _mm256_or_si256(_mm256_cmpgt_epi8(vcmp, biased), _mm256_cmpeq_epi8(v, vq)),
      _mm256_cmpeq_epi8(v, vbs)));
    const uint32_t hi_mask  = (uint32_t)_mm256_movemask_epi8(v);

    if (hi_mask && !validated) {
      if (first_invalid_utf8(data, len) != UTF8_OK)
        return -1;
      validated = true;
    }
    if (esc_mask) {
      if (validated) return 0;
      has_escape = true;
    }
    p += 32;
  }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  while (p + 16 <= end) {
    const uint8x16_t v      = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    const uint8x16_t vq     = vdupq_n_u8('"');
    const uint8x16_t vbs    = vdupq_n_u8('\\');
    const uint8x16_t vbias  = vdupq_n_u8(0x80);
    const int8x16_t  vcmp   = vdupq_n_s8(-96);
    const uint8x16_t biased = veorq_u8(v, vbias);
    const uint16_t esc_mask = neon_movemask(vorrq_u8(
      vorrq_u8(vreinterpretq_u8_s8(vcgtq_s8(vcmp, vreinterpretq_s8_u8(biased))),
               vceqq_u8(v, vq)),
      vceqq_u8(v, vbs)));
    const bool hi = neon_any_highbit(v);

    if (hi && !validated) {
      if (first_invalid_utf8(data, len) != UTF8_OK)
        return -1;
      validated = true;
    }
    if (esc_mask) {
      if (validated) return 0;
      has_escape = true;
    }
    p += 16;
  }
#elif defined(__SSE2__)
  while (p + 16 <= end) {
    const __m128i v      = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i vq     = _mm_set1_epi8('"');
    const __m128i vbs    = _mm_set1_epi8('\\');
    const __m128i vbias  = _mm_set1_epi8(-128);
    const __m128i vcmp   = _mm_set1_epi8(-96);
    const __m128i biased = _mm_xor_si128(v, vbias);
    const unsigned esc_mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(
      _mm_or_si128(_mm_cmpgt_epi8(vcmp, biased), _mm_cmpeq_epi8(v, vq)),
      _mm_cmpeq_epi8(v, vbs)));
    const unsigned hi_mask  = (unsigned)_mm_movemask_epi8(v);

    if (hi_mask && !validated) {
      if (first_invalid_utf8(data, len) != UTF8_OK)
        return -1;
      validated = true;
    }
    if (esc_mask) {
      if (validated) return 0;
      has_escape = true;
    }
    p += 16;
  }
#endif
  while (p < end) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c >= 0x80 && !validated) {
      if (first_invalid_utf8(data, len) != UTF8_OK)
        return -1;
      validated = true;
    }
    if (NEEDS_ESCAPE_TAB[c]) {
      if (validated) return 0;
      has_escape = true;
    }
    ++p;
  }
  return has_escape ? 0 : 1;
}

// JSON-escape a UTF-8 byte sequence with optional forward slash escaping
inline void json_escape_string_fwd_slash(std::string_view sv, OutBuf& out, bool escape_fwd_slash)
{
  out.ensure(sv.size() * 2 + 2);

  char* dst = out.m_data + out.m_len;
  *dst++ = '"';
  const char* p = sv.data();
  const char* end = p + sv.size();

  while (p < end) {
#if defined(__AVX2__)
    while (p + 32 <= end) {
      const __m256i v      = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      const __m256i vq     = _mm256_set1_epi8('"');
      const __m256i vbs    = _mm256_set1_epi8('\\');
      const __m256i vslash = _mm256_set1_epi8('/');
      const __m256i vbias  = _mm256_set1_epi8(-128);
      const __m256i vcmp   = _mm256_set1_epi8(-96);
      const __m256i biased = _mm256_xor_si256(v, vbias);
      const __m256i need   = _mm256_or_si256(
        _mm256_or_si256(_mm256_or_si256(_mm256_cmpgt_epi8(vcmp, biased), _mm256_cmpeq_epi8(v, vq)),
        _mm256_cmpeq_epi8(v, vbs)),
        escape_fwd_slash ? _mm256_cmpeq_epi8(v, vslash) : _mm256_setzero_si256());
      const uint32_t mask = (uint32_t)_mm256_movemask_epi8(need);
      if (mask) {
        const unsigned skip = __builtin_ctz(mask);
        memcpy(dst, p, skip);
        dst += skip;
        p   += skip;
        goto fwd_escape_one;
      }
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), v);
      dst += 32;
      p += 32;
    }
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    while (p + 16 <= end) {
      const uint8x16_t v      = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      const uint8x16_t vq     = vdupq_n_u8('"');
      const uint8x16_t vbs    = vdupq_n_u8('\\');
      const uint8x16_t vslash = vdupq_n_u8('/');
      const uint8x16_t vbias  = vdupq_n_u8(0x80);
      const int8x16_t  vcmp   = vdupq_n_s8(-96);
      const uint8x16_t biased = veorq_u8(v, vbias);
      const uint8x16_t need   = vorrq_u8(
        vorrq_u8(vorrq_u8(vreinterpretq_u8_s8(vcgtq_s8(vcmp, vreinterpretq_s8_u8(biased))),
                           vceqq_u8(v, vq)),
                 vceqq_u8(v, vbs)),
        escape_fwd_slash ? vceqq_u8(v, vslash) : vdupq_n_u8(0));
      const uint16_t mask = neon_movemask(need);
      if (mask) {
        const unsigned skip = __builtin_ctz(mask);
        memcpy(dst, p, skip);
        dst += skip;
        p   += skip;
        goto fwd_escape_one;
      }
      vst1q_u8(reinterpret_cast<uint8_t*>(dst), v);
      dst += 16;
      p += 16;
    }
#elif defined(__SSE2__)
    while (p + 16 <= end) {
      const __m128i v      = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      const __m128i vq     = _mm_set1_epi8('"');
      const __m128i vbs    = _mm_set1_epi8('\\');
      const __m128i vslash = _mm_set1_epi8('/');
      const __m128i vbias  = _mm_set1_epi8(-128);
      const __m128i vcmp   = _mm_set1_epi8(-96);
      const __m128i biased = _mm_xor_si128(v, vbias);
      const __m128i need   = _mm_or_si128(
        _mm_or_si128(_mm_or_si128(_mm_cmpgt_epi8(vcmp, biased), _mm_cmpeq_epi8(v, vq)),
        _mm_cmpeq_epi8(v, vbs)),
        escape_fwd_slash ? _mm_cmpeq_epi8(v, vslash) : _mm_setzero_si128());
      const unsigned mask = (unsigned)_mm_movemask_epi8(need);
      if (mask) {
        const unsigned skip = __builtin_ctz(mask);
        memcpy(dst, p, skip);
        dst += skip;
        p   += skip;
        goto fwd_escape_one;
      }
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
      dst += 16;
      p += 16;
    }
#endif
    if (p >= end) break;
    if (!NEEDS_ESCAPE_TAB[(unsigned char)*p] && !(escape_fwd_slash && *p == '/')) {
      *dst++ = *p++;
      continue;
    }
  fwd_escape_one:
    if (escape_fwd_slash && *p == '/') {
      *dst++ = '\\';
      *dst++ = '/';
      ++p;
    } else {
      const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
      memcpy(dst, e.seq, e.len);
      dst += e.len;
    }
  }

  *dst++ = '"';
  out.m_len = static_cast<size_t>(dst - out.m_data);
}

// JSON-escape a UTF-8 byte sequence, additionally escaping every non-ASCII
// code point as \uXXXX (uescape), and/or replacing invalid UTF-8 byte
// sequences with U+FFFD before escaping (force_utf8).
inline void json_escape_string_unicode(std::string_view sv, OutBuf& out,
                                       bool uescape, bool force_utf8, bool escape_fwd_slash = false)
{
  out.reserve(sv.size() * 6 + 2);
  out.push('"');
  const char* p   = sv.data();
  const char* end = p + sv.size();
  const char* run = p;

  while (p < end) {
    auto c = (unsigned char)*p;

    if (c < 0x80) [[likely]] {
      if (c == '/' && escape_fwd_slash) {
        if (p > run) out.push(run, p - run);
        out.push("\\/", 2);
        ++p;
        run = p;
      } else if (!NEEDS_ESCAPE_TAB[c]) [[likely]] {
        ++p;
      } else {
        if (p > run) out.push(run, p - run);
        const EscapeEntry& e = ESCAPE_TAB[c];
        out.push(e.seq, e.len);
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

//-----------------------------------------------------------------------------
// Word helpers for short strings (see escape_string). A byte needs escaping
// when it is below 0x20, or is '"' or '\\'. Each test is exact for "some byte
// matches". Unused lanes are padded with 'a', which needs no escape.
//-----------------------------------------------------------------------------
static inline uint64_t short_word_needs_escape(uint64_t v) {
  constexpr uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
  const uint64_t below_20 = (v - ones * 0x20) & ~v & highs;
  const uint64_t qv = v ^ (ones * '"');
  const uint64_t bv = v ^ (ones * '\\');
  const uint64_t q_zero = (qv - ones) & ~qv & highs;
  const uint64_t b_zero = (bv - ones) & ~bv & highs;
  return below_20 | q_zero | b_zero;
}
static inline uint64_t short_load(const char* p, size_t k) {  // k = 4 or 8
  if (k == 8) { uint64_t w; memcpy(&w, p, 8); return w; }
  uint32_t w; memcpy(&w, p, 4); return w;
}
// True when the n bytes (1 <= n <= 16) at p contain a byte that needs escaping.
static inline bool short_needs_escape(const char* p, size_t n) {
  if (n >= 8)
    return (short_word_needs_escape(short_load(p, 8)) | short_word_needs_escape(short_load(p + n - 8, 8))) != 0;
  if (n >= 4)
    return (short_word_needs_escape(short_load(p, 4) | 0x6161616100000000ull) |
            short_word_needs_escape(short_load(p + n - 4, 4) | 0x6161616100000000ull)) != 0;
  // n is 1..3: the first, middle and last bytes cover every position.
  const uint64_t w = static_cast<uint64_t>(static_cast<unsigned char>(p[0]))
                   | (static_cast<uint64_t>(static_cast<unsigned char>(p[n / 2])) << 8)
                   | (static_cast<uint64_t>(static_cast<unsigned char>(p[n - 1])) << 16)
                   | 0x6161616161000000ull;
  return short_word_needs_escape(w) != 0;
}
// True when any of the n bytes (1 <= n <= 16) has its high bit set.
static inline bool short_any_high_bit(const char* p, size_t n) {
  constexpr uint64_t highs = 0x8080808080808080ull;
  if (n >= 8)
    return ((short_load(p, 8) | short_load(p + n - 8, 8)) & highs) != 0;
  if (n >= 4)
    return ((short_load(p, 4) | short_load(p + n - 4, 4)) & 0x80808080ull) != 0;
  return ((static_cast<unsigned char>(p[0]) | static_cast<unsigned char>(p[n / 2]) |
           static_cast<unsigned char>(p[n - 1])) & 0x80) != 0;
}
// One test for 1..16 bytes: true when any byte needs escaping or has its high
// bit set (the two conditions short_needs_escape and short_any_high_bit check
// separately). The words are loaded once and the length is dispatched once.
static inline bool short_bad(const char* p, size_t n)
{
  constexpr uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
  auto bad = [&](uint64_t v) {
    const uint64_t qv = v ^ (ones * '"');
    const uint64_t bv = v ^ (ones * '\\');
    return (((v - ones * 0x20) & ~v) | ((qv - ones) & ~qv) | ((bv - ones) & ~bv) | v) & highs;
  };
  uint64_t a, b;
  if (n >= 8) {
    a = short_load(p, 8);
    b = short_load(p + n - 8, 8);
  } else if (n >= 4) {
    a = short_load(p, 4) | 0x6161616100000000ull;
    b = short_load(p + n - 4, 4) | 0x6161616100000000ull;
  } else {  // 1..3: first, middle and last bytes cover every position
    a = static_cast<uint64_t>(static_cast<unsigned char>(p[0]))
      | (static_cast<uint64_t>(static_cast<unsigned char>(p[n / 2])) << 8)
      | (static_cast<uint64_t>(static_cast<unsigned char>(p[n - 1])) << 16)
      | 0x6161616161000000ull;
    b = a;
  }
  return (bad(a) | bad(b)) != 0;
}

// Copy n (1 <= n <= 16) bytes with at most two moves of 4 or 8 bytes.
static inline void short_copy(char* dst, const char* src, size_t n) {
  if (n >= 8) {
    uint64_t a, b;
    memcpy(&a, src, 8);
    memcpy(&b, src + n - 8, 8);
    memcpy(dst, &a, 8);
    memcpy(dst + n - 8, &b, 8);
  } else if (n >= 4) {
    uint32_t a, b;
    memcpy(&a, src, 4);
    memcpy(&b, src + n - 4, 4);
    memcpy(dst, &a, 4);
    memcpy(dst + n - 4, &b, 4);
  } else {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
  }
}

struct JSONEncoder {
  JSONEncoder(ErlNifEnv* env, const JSONEncodeOpts& opts, OutBuf& out)
    : m_env(env), m_opts(opts), m_out(out), m_err(nullptr), m_err_term(0)
  {
    thread_local AtomNameCache tl_atom_cache;
    m_atom_cache = &tl_atom_cache;
  }

  ErlNifEnv*   env()       const { return m_env;      }
  const char*  err()       const { return m_err;      }
  ERL_NIF_TERM err_term()  const { return m_err_term; }

  // Output budget in bytes. The normal scheduler encodes with a budget; once
  // the output would exceed it, encoding stops and the NIF reruns the call on a
  // dirty scheduler (unlimited budget). SIZE_MAX means no limit.
  void set_budget(size_t budget) { m_budget = budget; }
  bool budget_exceeded() const   { return m_over_budget; }

  // A plain-ASCII binary with nothing to escape is written straight to the
  // output, in one pass that copies 16-byte blocks as it checks them. Anything
  // else (a byte that needs escaping, a high bit, or uescape/escape_fwd_slash)
  // returns false, and the caller takes the general path, which validates and
  // escapes. Valid in both value and key position. Output only changes on commit,
  // so a false return leaves the buffer as it was.
  bool emit_plain(const char* data, size_t n)
  {
    if (n == 0 || m_opts.uescape || m_opts.escape_fwd_slash) return false;
    if (n <= 16) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#if defined(GLAZER_TAIL_BOUNDED)
      constexpr bool short_wide_ok = false;
#else
      constexpr bool short_wide_ok = true;
#endif
      if (short_wide_ok && (reinterpret_cast<uintptr_t>(data) & 4095) <= 4096 - 16) {
        const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(data));
        const uint8x16_t special = vorrq_u8(
          vorrq_u8(vreinterpretq_u8_s8(vcgtq_s8(vdupq_n_s8(-96),
                     vreinterpretq_s8_u8(veorq_u8(v, vdupq_n_u8(0x80))))),
                   vceqq_u8(v, vdupq_n_u8('"'))),
          vorrq_u8(vceqq_u8(v, vdupq_n_u8('\\')), v));
        if (neon_movemask_highbit(special) & ((1u << n) - 1)) return false;
        m_out.ensure(n + 18);
        char* const base = m_out.tail();
        base[0] = '"';
        vst1q_u8(reinterpret_cast<uint8_t*>(base + 1), v);
        base[n + 1] = '"';
        m_out.commit(n + 2);
        return true;
      }
#elif defined(__SSE2__)
#if defined(GLAZER_TAIL_BOUNDED)
      constexpr bool short_wide_ok = false;
#else
      constexpr bool short_wide_ok = true;
#endif
      if (short_wide_ok && (reinterpret_cast<uintptr_t>(data) & 4095) <= 4096 - 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
        const __m128i special = _mm_or_si128(_mm_or_si128(_mm_cmpgt_epi8(_mm_set1_epi8(-96), _mm_xor_si128(v, _mm_set1_epi8(-128))),
                                                          _mm_cmpeq_epi8(v, _mm_set1_epi8('"'))),
                                             _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\\')), v));
        if (_mm_movemask_epi8(special) & ((1u << n) - 1)) return false;
        m_out.ensure(n + 18);
        char* const base = m_out.tail();
        base[0] = '"';
        _mm_storeu_si128(reinterpret_cast<__m128i*>(base + 1), v);
        base[n + 1] = '"';
        m_out.commit(n + 2);
        return true;
      }
#endif
      if (short_bad(data, n)) return false;
      m_out.ensure(n + 2);
      m_out.unsafe_push('"');
      short_copy(m_out.tail(), data, n);
      m_out.commit(n);
      m_out.unsafe_push('"');
      return true;
    }
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    m_out.ensure(n + 2 + 16);
    char* const base = m_out.tail();
    const uint8x16_t vq    = vdupq_n_u8('"');
    const uint8x16_t vbs   = vdupq_n_u8('\\');
    const uint8x16_t vbias = vdupq_n_u8(0x80);
    const int8x16_t  vcmp  = vdupq_n_s8(-96);
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
      const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
      const uint8x16_t special = vorrq_u8(
        vorrq_u8(vreinterpretq_u8_s8(vcgtq_s8(vcmp, vreinterpretq_s8_u8(veorq_u8(v, vbias)))),
                 vceqq_u8(v, vq)),
        vorrq_u8(vceqq_u8(v, vbs), v));
      if (neon_movemask_highbit(special) != 0) return false;
      vst1q_u8(reinterpret_cast<uint8_t*>(base + 1 + i), v);
    }
    const size_t r = n - i;
    if (r > 0) {
#if defined(GLAZER_TAIL_BOUNDED)
      constexpr bool wide_tail_ok = false;
#else
      constexpr bool wide_tail_ok = true;
#endif
      if (wide_tail_ok && (reinterpret_cast<uintptr_t>(data + i) & 4095) <= 4096 - 16) {
        const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
        const uint8x16_t special = vorrq_u8(
          vorrq_u8(vreinterpretq_u8_s8(vcgtq_s8(vdupq_n_s8(-96),
                     vreinterpretq_s8_u8(veorq_u8(v, vdupq_n_u8(0x80))))),
                   vceqq_u8(v, vdupq_n_u8('"'))),
          vorrq_u8(vceqq_u8(v, vdupq_n_u8('\\')), v));
        if (neon_movemask_highbit(special) & ((1u << r) - 1)) return false;
        vst1q_u8(reinterpret_cast<uint8_t*>(base + 1 + i), v);
      } else {
        if (short_needs_escape(data + i, r) || glz::utf8::short_any_high_bit(data + i, r)) return false;
        short_copy(base + 1 + i, data + i, r);
      }
    }
    base[0] = '"';
    base[n + 1] = '"';
    m_out.commit(n + 2);
    return true;
#elif defined(__AVX2__)
    m_out.ensure(n + 2 + 32);
    char* const base = m_out.tail();
    const __m256i vq    = _mm256_set1_epi8('"');
    const __m256i vbs   = _mm256_set1_epi8('\\');
    const __m256i vbias = _mm256_set1_epi8(-128);
    const __m256i vcmp  = _mm256_set1_epi8(-96);
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
      const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
      const __m256i special = _mm256_or_si256(_mm256_or_si256(_mm256_cmpgt_epi8(vcmp, _mm256_xor_si256(v, vbias)),
                                                              _mm256_cmpeq_epi8(v, vq)),
                                              _mm256_or_si256(_mm256_cmpeq_epi8(v, vbs), v));
      if (_mm256_movemask_epi8(special) != 0) return false;
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(base + 1 + i), v);
    }
    // Handle remaining 16..31 bytes with SSE2
    {
      const __m128i vq16    = _mm_set1_epi8('"');
      const __m128i vbs16   = _mm_set1_epi8('\\');
      const __m128i vbias16 = _mm_set1_epi8(-128);
      const __m128i vcmp16  = _mm_set1_epi8(-96);
      for (; i + 16 <= n; i += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
        const __m128i special = _mm_or_si128(_mm_or_si128(_mm_cmpgt_epi8(vcmp16, _mm_xor_si128(v, vbias16)),
                                                          _mm_cmpeq_epi8(v, vq16)),
                                             _mm_or_si128(_mm_cmpeq_epi8(v, vbs16), v));
        if (_mm_movemask_epi8(special) != 0) return false;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(base + 1 + i), v);
      }
    }
    const size_t r = n - i;
#elif defined(__SSE2__)
    m_out.ensure(n + 2 + 16);
    char* const base = m_out.tail();
    size_t i = 0;
    {
      const __m128i vq    = _mm_set1_epi8('"');
      const __m128i vbs   = _mm_set1_epi8('\\');
      const __m128i vbias = _mm_set1_epi8(-128);
      const __m128i vcmp  = _mm_set1_epi8(-96);
      for (; i + 16 <= n; i += 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
        const __m128i special = _mm_or_si128(_mm_or_si128(_mm_cmpgt_epi8(vcmp, _mm_xor_si128(v, vbias)),
                                                          _mm_cmpeq_epi8(v, vq)),
                                             _mm_or_si128(_mm_cmpeq_epi8(v, vbs), v));
        if (_mm_movemask_epi8(special) != 0) return false;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(base + 1 + i), v);
      }
    }
    const size_t r = n - i;
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    // already handled above
#elif defined(__SSE2__) || defined(__AVX2__)
    if (r > 0) {
#if defined(GLAZER_TAIL_BOUNDED)
      constexpr bool wide_tail_ok = false;
#else
      constexpr bool wide_tail_ok = true;
#endif
      if (wide_tail_ok && (reinterpret_cast<uintptr_t>(data + i) & 4095) <= 4096 - 16) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
        const __m128i special = _mm_or_si128(_mm_or_si128(_mm_cmpgt_epi8(_mm_set1_epi8(-96), _mm_xor_si128(v, _mm_set1_epi8(-128))),
                                                          _mm_cmpeq_epi8(v, _mm_set1_epi8('"'))),
                                             _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\\')), v));
        if (_mm_movemask_epi8(special) & ((1u << r) - 1)) return false;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(base + 1 + i), v);
      } else {
        if (short_needs_escape(data + i, r) || glz::utf8::short_any_high_bit(data + i, r)) return false;
        short_copy(base + 1 + i, data + i, r);
      }
    }
    base[0] = '"';
    base[n + 1] = '"';
    m_out.commit(n + 2);
    return true;
#else
    return false;
#endif
  }

  bool encode(ERL_NIF_TERM term)
  {
    if (over_budget(0)) [[unlikely]] return budget_error(term);
    switch (fast_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, term, &bin)) return false;
        if (over_budget(bin.size)) [[unlikely]] return budget_error(term);
        const char* data = reinterpret_cast<const char*>(bin.data);
        if (emit_plain(data, bin.size)) return true;
        // emit_plain failed — string has high-bit bytes or escape chars.
        // For the common case (no uescape, no force_utf8, no escape_fwd_slash):
        // fused single-pass UTF-8 validation + escape detection.
        if (!m_opts.uescape && !m_opts.escape_fwd_slash && !m_opts.force_utf8) {
          int rc = json_scan_validate(data, bin.size);
          if (rc < 0) [[unlikely]]
            return error("invalid UTF-8 in string", term);
          if (rc > 0) {
            m_out.push_quoted({data, bin.size});
            return true;
          }
          json_escape_string({data, bin.size}, m_out);
          return true;
        }
        // Slow path for special options.
        const bool valid_utf8 = first_invalid_utf8(data, bin.size) == UTF8_OK;
        if (!m_opts.force_utf8 && !valid_utf8) [[unlikely]]
          return error("invalid UTF-8 in string", term);
        return escape_string({data, bin.size});
      }

      case ERL_NIF_TERM_TYPE_INTEGER:
        return encode_integer(term);

      case ERL_NIF_TERM_TYPE_MAP:
        return encode_map(term);

      case ERL_NIF_TERM_TYPE_LIST:
        return encode_list(term);

      case ERL_NIF_TERM_TYPE_ATOM:
        return encode_atom(term);

      case ERL_NIF_TERM_TYPE_FLOAT:
        return encode_float(term);

      case ERL_NIF_TERM_TYPE_TUPLE: {
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(m_env, term, &arity, &tp);
        if (arity == 1 && enif_is_list(m_env, tp[0])) [[likely]]
          return encode_proplist_inline(tp[0]);
        return error("tuple is not an object", term);
      }

      default:
        return error("unsupported term type", term);
    }
  }

private:
  ErlNifEnv*            m_env;
  const JSONEncodeOpts& m_opts;
  OutBuf&               m_out;
  AtomNameCache*        m_atom_cache;
  char                  m_atom_buf[256]; // scratch for atom → string_view
  const char*           m_err;
  ERL_NIF_TERM          m_err_term;

  // Nesting depth of encode_map/encode_list. Same cap as the decoder, so a
  // term that decodes can always be encoded back, and so deep terms fail with
  // an error instead of exhausting the C stack.
  static constexpr unsigned MAX_DEPTH = 128;
  unsigned              m_depth = 0;


  struct DepthScope {
    unsigned& d;
    explicit DepthScope(unsigned& d) : d(d) { ++d; }
    ~DepthScope() { --d; }
  };

  size_t m_budget = SIZE_MAX;
  bool   m_over_budget = false;

  // True if writing `extra` more bytes would pass the budget; records it.
  bool over_budget(size_t extra) {
    if (m_out.view().size() + extra <= m_budget) return false;
    m_over_budget = true;
    return true;
  }
  bool budget_error(ERL_NIF_TERM term) { return error("output budget exceeded", term); }

  bool escape_string(std::string_view sv)
  {
    if (sv.empty()) {
      m_out.push("\"\"", 2);
      return true;
    }

    // Short strings with the default options: decide with word tests and copy
    // with overlapping moves. No per-byte loop, no branch per byte, no libc call.
    if (sv.size() <= 16 && !m_opts.uescape && !m_opts.escape_fwd_slash) {
      const char*  p = sv.data();
      const size_t n = sv.size();
      // With force_utf8, a string with a non-ASCII byte is left to the validating path.
      if (!short_needs_escape(p, n) && !(m_opts.force_utf8 && short_any_high_bit(p, n))) {
        m_out.ensure(n + 2);
        m_out.unsafe_push('"');
        short_copy(m_out.tail(), p, n);
        m_out.commit(n);
        m_out.unsafe_push('"');
        return true;
      }
    }

    auto p     = sv.data();
    auto end   = p + sv.size();
    auto force = m_opts.force_utf8 && first_invalid_utf8(p, sv.size()) != UTF8_OK;
    if (!m_opts.uescape && !force) {
      if (!m_opts.escape_fwd_slash) {
        if (find_escape_pos(p, end) == end) {
          m_out.push_quoted(sv);
          return true;
        }
      } else {
        const char* q = p;
        while (q < end && *q != '/' && !NEEDS_ESCAPE_TAB[(unsigned char)*q]) ++q;
        if (q == end) {
          m_out.push_quoted(sv);
          return true;
        }
      }
    }

    if (m_opts.uescape || force)
      json_escape_string_unicode(sv, m_out, m_opts.uescape, force, m_opts.escape_fwd_slash);
    else if (m_opts.escape_fwd_slash)
      json_escape_string_fwd_slash(sv, m_out, true);
    else
      json_escape_string(sv, m_out);
    return true;
  }

  // Atom names and charlist keys hold Latin-1 code points (0..255). Emit them as
  // UTF-8, so a code point such as 0xE9 becomes the two bytes C3 A9.
  static bool has_high_byte(const char* s, size_t n)
  {
    for (size_t i = 0; i < n; ++i)
      if (static_cast<unsigned char>(s[i]) >= 0x80) return true;
    return false;
  }

  bool escape_latin1(const char* s, size_t n)
  {
    if (!has_high_byte(s, n))
      return escape_string({s, n});
    std::string utf8;
    utf8.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c < 0x80) {
        utf8 += static_cast<char>(c);
      } else {
        utf8 += static_cast<char>(0xC0 | (c >> 6));
        utf8 += static_cast<char>(0x80 | (c & 0x3F));
      }
    }
    return escape_string(utf8);
  }

  bool append_escaped_latin1_atom(const char* s, size_t len)
  {
    if (has_high_byte(s, len))
      return escape_latin1(s, len);
    if (len == 0) {
      m_out.push("\"\"", 2);
      return true;
    }

    const char* end = s + len;
    if (!m_opts.escape_fwd_slash) {
      if (find_escape_pos(s, end) == end) {
        m_out.push_quoted({s, len});
        return true;
      }
    } else {
      const char* p = s;
      while (p < end && *p != '/' && !NEEDS_ESCAPE_TAB[(unsigned char)*p]) ++p;
      if (p == end) {
        m_out.push_quoted({s, len});
        return true;
      }
    }

    m_out.push('"');
    const char* p = s;
    while (p < end) {
      const char* run_start = p;
      while (p < end && !NEEDS_ESCAPE_TAB[(unsigned char)*p] && !(m_opts.escape_fwd_slash && *p == '/'))
        ++p;

      if (p > run_start)
        m_out.push(run_start, static_cast<size_t>(p - run_start));
      if (p >= end)
        break;

      if (m_opts.escape_fwd_slash && *p == '/') {
        m_out.push("\\/", 2);
        ++p;
        continue;
      }

      const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
      m_out.push(e.seq, e.len);
    }
    m_out.push('"');
    return true;
  }

  bool encode_integer(ERL_NIF_TERM term)
  {
    ErlNifSInt64 i;
    if (enif_get_int64(m_env, term, &i)) [[likely]] {
      m_out.ensure(20);
      m_out.commit(glz::BigInt::write_i64(m_out.tail(), i));
      return true;
    }
    return glz::BigInt::encode(m_env, term, m_out)
        || error("cannot encode integer", term);
  }

  bool encode_float(ERL_NIF_TERM term)
  {
    double d;
    if (!enif_get_double(m_env, term, &d)) [[unlikely]]
      return error("not a float", term);
    if (!std::isfinite(d)) [[unlikely]] {
      m_out.push("null", 4);
      return true;
    }

    char buf[32];
    m_out.push(buf, glz::fp::ftoa(d, buf));
    return true;
  }

  // Writes one object member key and its ':'.
  bool encode_member_key(ERL_NIF_TERM key)
  {
    // Fast path: atom keys with cached escaped representation (key + colon).
    if ((static_cast<uintptr_t>(key) & 0x3F) == 0x0B) {
      size_t cached_len;
      const char* cached = m_atom_cache->lookup(key, cached_len);
      if (cached) {
        m_out.push(cached, cached_len);
        return true;
      }
      // Cache miss: encode normally, then capture the output for caching.
      size_t before = m_out.view().size();
      if (!encode_atom_key(key)) [[unlikely]] return false;
      m_out.push(':');
      size_t after = m_out.view().size();
      size_t emitted = after - before;
      if (emitted < AtomNameCache::MAX_CACHED_LEN)
        m_atom_cache->insert(key, m_out.view().data() + before, emitted);
      return true;
    }
    if (!encode_key(key)) [[unlikely]] return false;
    m_out.push(':');
    return true;
  }

  bool encode_binary_key(ERL_NIF_TERM key, const char* data, size_t n)
  {
    if (emit_plain(data, n)) return true;
    if (!m_opts.uescape && !m_opts.escape_fwd_slash && !m_opts.force_utf8) {
      int rc = json_scan_validate(data, n);
      if (rc < 0) [[unlikely]]
        return error("invalid UTF-8 in string", key);
      if (rc > 0) {
        m_out.push_quoted({data, n});
        return true;
      }
      json_escape_string({data, n}, m_out);
      return true;
    }
    if (!m_opts.force_utf8 && first_invalid_utf8(data, n) != UTF8_OK) [[unlikely]]
      return error("invalid UTF-8 in string", key);
    return escape_string({data, n});
  }

  bool encode_key(ERL_NIF_TERM key)
  {
    switch (fast_term_type(m_env, key)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, key, &bin))
          return error("not a binary", key);
        return encode_binary_key(key, reinterpret_cast<const char*>(bin.data), bin.size);
      }
      case ERL_NIF_TERM_TYPE_ATOM:
        return encode_atom_key(key);
      case ERL_NIF_TERM_TYPE_INTEGER: {
        m_out.push('"');
        if (!glz::BigInt::encode(m_env, key, m_out))
          return error("cannot encode integer", key);
        m_out.push('"');
        return true;
      }
      case ERL_NIF_TERM_TYPE_LIST: {
        if (enif_is_empty_list(m_env, key))
          return escape_string("");

        ERL_NIF_TERM h, t = key;
        int i = 0;
        const int max_len = sizeof(m_atom_buf) - 1;
        while (enif_get_list_cell(m_env, t, &h, &t)) {
          unsigned int ch;
          if (!enif_get_uint(m_env, h, &ch) || ch > 255) [[unlikely]]
            return error("list key is not a charlist", key);
          m_atom_buf[i++] = static_cast<char>(ch);
          if (i > max_len) [[unlikely]]
            return error("charlist key is too long", key);
        }
        if (!enif_is_empty_list(m_env, t)) [[unlikely]]
          return error("list key is not a proper charlist", key);
        return escape_latin1(m_atom_buf, i);
      }
      default:
        return error("unsupported key type", key);
    }
  }

  inline bool encode_map(ERL_NIF_TERM term)
  {
    if (m_depth >= MAX_DEPTH) [[unlikely]] return error("exceeded maximum nesting depth", term);
    DepthScope depth_scope(m_depth);
    size_t map_size = 0;
    if (enif_get_map_size(m_env, term, &map_size)) [[likely]]
      m_out.reserve(map_size * 24 + 2);
    m_out.push('{');
    auto iter = MapIterator::create(m_env, term);
    if (!iter) [[unlikely]] return false;
    ERL_NIF_TERM k, v;
    bool first = true;
    while (iter->get_pair(&k, &v)) {
      if (!first) m_out.push(',');
      first = false;
      if (!encode_member_key(k)) [[unlikely]] return error("cannot encode key", k);
      if (!encode(v))            [[unlikely]] return error("cannot encode value", v);
      iter->next();
    }
    m_out.push('}');
    return true;
  }

  inline bool encode_list(ERL_NIF_TERM term)
  {
    if (m_depth >= MAX_DEPTH) [[unlikely]] return error("exceeded maximum nesting depth", term);
    DepthScope depth_scope(m_depth);
    unsigned list_len = 0;
    if (enif_get_list_length(m_env, term, &list_len)) [[likely]]
      m_out.reserve(static_cast<size_t>(list_len) * 12 + 2);
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

  bool encode_proplist_inline(ERL_NIF_TERM term)
  {
    if (m_depth >= MAX_DEPTH) [[unlikely]] return error("exceeded maximum nesting depth", term);
    DepthScope depth_scope(m_depth);
    unsigned list_len = 0;
    if (enif_get_list_length(m_env, term, &list_len)) [[likely]]
      m_out.reserve(static_cast<size_t>(list_len) * 24 + 2);
    m_out.push('{');
    ERL_NIF_TERM h, t = term;
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
    if (!enif_is_empty_list(m_env, t)) [[unlikely]]
      return error("improper list", t);
    m_out.push('}');
    return true;
  }

  bool encode_atom_key(ERL_NIF_TERM term) {
    // Object keys are always strings, even for atoms like `true`, `false`, or
    // `nil`. Treat them as names rather than raw JSON literals so the output is
    // always a valid quoted JSON object member.
    unsigned len = 0;
    if (!enif_get_atom_length(m_env, term, &len, ERL_NIF_LATIN1)) [[unlikely]]
      return error("cannot convert atom to string", term);

    if (len + 1 > sizeof(m_atom_buf)) {
      char* tmp = static_cast<char*>(malloc(len + 1));
      if (!tmp) return error("cannot convert atom to string", term);
      enif_get_atom(m_env, term, tmp, len + 1, ERL_NIF_LATIN1);
      bool ok = append_escaped_latin1_atom(tmp, len);
      free(tmp);
      return ok;
    }

    enif_get_atom(m_env, term, m_atom_buf, sizeof(m_atom_buf), ERL_NIF_LATIN1);
    return append_escaped_latin1_atom(m_atom_buf, len);
  }

  // Atoms are immediate terms, so comparing the words is the same as
  // enif_is_identical, without a call into the VM for each comparison.
  bool encode_atom(ERL_NIF_TERM term) {
    if (term == m_opts.null_term) { m_out.push("null", 4); return true; }
    if (term == AM_TRUE)  { m_out.push("true",  4); return true; }
    if (term == AM_FALSE) { m_out.push("false", 5); return true; }
    if (term == AM_NIL)   { m_out.push("null",  4); return true; }
    unsigned len = 0;
    if (!enif_get_atom_length(m_env, term, &len, ERL_NIF_LATIN1)) [[unlikely]]
      return error("cannot convert atom to string", term);

    if (len + 1 > sizeof(m_atom_buf)) [[unlikely]] {
      using SmartPtr = std::unique_ptr<char[], decltype(&free)>;

      SmartPtr tmp(static_cast<char*>(malloc(len + 1)), &free);
      if (!tmp) return error("cannot convert atom to string", term);
      enif_get_atom(m_env, term, tmp.get(), len + 1, ERL_NIF_LATIN1);
      return append_escaped_latin1_atom(tmp.get(), len);
    }

    enif_get_atom(m_env, term, m_atom_buf, sizeof(m_atom_buf), ERL_NIF_LATIN1);
    return append_escaped_latin1_atom(m_atom_buf, len);
  }

  // Each nesting level reports failure through here as the error unwinds.
  // Keep the first (innermost) error, so the real cause survives the unwind.
  template <int N>
  bool error(const char (&err)[N], ERL_NIF_TERM term) {
    if (!m_err) {
      m_err = err;
      m_err_term = term;
    }
    return false;
  }
};

} // namespace glz
