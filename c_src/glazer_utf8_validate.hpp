// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// UTF-8 validation.
//
// utf8::valid(p, n) answers whether the n bytes at p are well-formed UTF-8 under
// the strict rules: no overlong forms, no UTF-16 surrogates (U+D800..U+DFFF),
// nothing above U+10FFFF, and no truncated sequence at the end.
//
// The vector paths use the three-lookup method from simdjson (Keiser and Lemire,
// "Validating UTF-8 In Less Than One Instruction Per Byte"): each byte is
// classified by a table lookup on its high nibble, its low nibble and the
// nibble of the byte before it, and the classes are combined per 16-byte block
// with the carried-over previous bytes. SSSE3 (pshufb) on x86 and NEON (tbl) on
// ARM64; other targets use the scalar check.
//
// utf8::first_invalid(p, n) returns the offset of the first byte that starts or
// continues an invalid sequence, or npos. It runs only when valid() has already
// said the input is invalid, so the error position costs nothing on valid input.
//-----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>  // uint8_t
#include <cstring>  // memcpy

#if defined(__SSSE3__)
#include <tmmintrin.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace glz::utf8 {

inline constexpr size_t npos = static_cast<size_t>(-1);

// Strict scalar check of one position; returns the length of the sequence
// starting at p (1..4), or 0 if no valid sequence starts there.
inline size_t sequence_length(const unsigned char* p, const unsigned char* e)
{
  const unsigned char c = *p;
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) {
    if (e - p < 2 || (p[1] & 0xC0) != 0x80) return 0;
    return 2;
  }
  if (c >= 0xE0 && c <= 0xEF) {
    if (e - p < 3) return 0;
    const unsigned char lo = (c == 0xE0) ? 0xA0 : 0x80;
    const unsigned char hi = (c == 0xED) ? 0x9F : 0xBF;
    if (p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80) return 0;
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (e - p < 4) return 0;
    const unsigned char lo = (c == 0xF0) ? 0x90 : 0x80;
    const unsigned char hi = (c == 0xF4) ? 0x8F : 0xBF;
    if (p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    return 4;
  }
  return 0;  // continuation byte, C0, C1, or F5..FF
}

// Reference scalar validator. Also the fallback where no vector path exists.
inline bool valid_scalar(const char* s, size_t n)
{
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* e = p + n;
  while (p < e) {
    if (*p < 0x80) { ++p; continue; }
    const size_t len = sequence_length(p, e);
    if (len == 0) return false;
    p += len;
  }
  return true;
}

// Offset of the first invalid byte, or npos when the input is valid.
inline size_t first_invalid(const char* s, size_t n)
{
  const unsigned char* b = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* p = b;
  const unsigned char* e = b + n;
  while (p < e) {
    if (*p < 0x80) { ++p; continue; }
    const size_t len = sequence_length(p, e);
    if (len == 0) return static_cast<size_t>(p - b);
    p += len;
  }
  return npos;
}

#if defined(__SSSE3__) || defined(__aarch64__)
namespace detail {

// Error flags, as in simdjson's table-driven validator.
enum : uint8_t {
  TOO_SHORT      = 1 << 0,
  TOO_LONG       = 1 << 1,
  OVERLONG_3     = 1 << 2,
  TOO_LARGE      = 1 << 3,
  SURROGATE      = 1 << 4,
  OVERLONG_2     = 1 << 5,
  TOO_LARGE_1000 = 1 << 6,
  OVERLONG_4     = 1 << 6,
  TWO_CONTS      = 1 << 7,
  CARRY          = TOO_SHORT | TOO_LONG | TWO_CONTS,
};

// Indexed by the high nibble of the first byte of a pair (the byte before).
alignas(16) constexpr uint8_t BYTE_1_HIGH[16] = {
  TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG, TOO_LONG,
  TWO_CONTS, TWO_CONTS, TWO_CONTS, TWO_CONTS,
  TOO_SHORT | OVERLONG_2,
  TOO_SHORT,
  TOO_SHORT | OVERLONG_3 | SURROGATE,
  TOO_SHORT | TOO_LARGE | TOO_LARGE_1000 | OVERLONG_4,
};

// Indexed by the low nibble of the first byte of a pair.
alignas(16) constexpr uint8_t BYTE_1_LOW[16] = {
  CARRY | OVERLONG_3 | OVERLONG_2 | OVERLONG_4,
  CARRY | OVERLONG_2,
  CARRY, CARRY,
  CARRY | TOO_LARGE,
  CARRY | TOO_LARGE | TOO_LARGE_1000,
  CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
  CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
  CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
  CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000 | SURROGATE,  // 0xED lead
  CARRY | TOO_LARGE | TOO_LARGE_1000, CARRY | TOO_LARGE | TOO_LARGE_1000,
};

// Indexed by the high nibble of the second byte of a pair (the current byte).
alignas(16) constexpr uint8_t BYTE_2_HIGH[16] = {
  TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT,
  TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE_1000 | OVERLONG_4,
  TOO_LONG | OVERLONG_2 | TWO_CONTS | OVERLONG_3 | TOO_LARGE,
  TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE,
  TOO_LONG | OVERLONG_2 | TWO_CONTS | SURROGATE | TOO_LARGE,
  TOO_SHORT, TOO_SHORT, TOO_SHORT, TOO_SHORT,
};


// Where the vector loop stopped at p, the scalar check resumes at the last
// non-continuation byte within the previous three bytes, so it starts on a
// sequence boundary.

#if defined(__SSSE3__)
inline bool valid_ssse3(const char* s, size_t n)
{
  const unsigned char* b = reinterpret_cast<const unsigned char*>(s);
  const __m128i tab_b1h = _mm_load_si128(reinterpret_cast<const __m128i*>(BYTE_1_HIGH));
  const __m128i tab_b1l = _mm_load_si128(reinterpret_cast<const __m128i*>(BYTE_1_LOW));
  const __m128i tab_b2h = _mm_load_si128(reinterpret_cast<const __m128i*>(BYTE_2_HIGH));
  const __m128i nib = _mm_set1_epi8(0x0F);
  const __m128i sign = _mm_set1_epi8(static_cast<char>(0x80));
  const __m128i high_bit = _mm_set1_epi8(static_cast<char>(0x80));
  const __m128i t2 = _mm_set1_epi8(static_cast<char>(0xDF ^ 0x80));   // >= 0xE0 after the sign flip
  const __m128i t3 = _mm_set1_epi8(static_cast<char>(0xEF ^ 0x80));   // >= 0xF0
  __m128i err = _mm_setzero_si128();
  __m128i prev = _mm_setzero_si128();
  bool prev_ascii = true;  // the block before the current one has no high bit set
  // One block of 16 bytes, classified against the bytes before it.
  auto block = [&](const __m128i in) {
    const bool ascii = _mm_movemask_epi8(in) == 0;
    // All-ASCII after all-ASCII cannot pend or start a bad sequence.
    if (ascii && prev_ascii) { prev = in; return; }
    prev_ascii = ascii;
    const __m128i prev1 = _mm_alignr_epi8(in, prev, 15);
    const __m128i prev2 = _mm_alignr_epi8(in, prev, 14);
    const __m128i prev3 = _mm_alignr_epi8(in, prev, 13);
    const __m128i hi1 = _mm_and_si128(_mm_srli_epi16(prev1, 4), nib);
    const __m128i lo1 = _mm_and_si128(prev1, nib);
    const __m128i hi2 = _mm_and_si128(_mm_srli_epi16(in, 4), nib);
    const __m128i special = _mm_and_si128(
        _mm_and_si128(_mm_shuffle_epi8(tab_b1h, hi1), _mm_shuffle_epi8(tab_b1l, lo1)),
        _mm_shuffle_epi8(tab_b2h, hi2));
    // A third byte (>= 0xE0 two back) or a fourth byte (>= 0xF0 three back)
    // must be followed by continuation bytes, so it sets the 0x80 bit here.
    const __m128i must23 = _mm_and_si128(
        _mm_or_si128(_mm_cmpgt_epi8(_mm_xor_si128(prev2, sign), t2),
                     _mm_cmpgt_epi8(_mm_xor_si128(prev3, sign), t3)),
        high_bit);
    err = _mm_or_si128(err, _mm_xor_si128(must23, special));
    prev = in;
  };
  size_t i = 0;
  // 64-byte ASCII runs after ASCII are skipped with one test.
  for (; i + 64 <= n; i += 64) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i + 16));
    const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i + 32));
    const __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i + 48));
    const __m128i any = _mm_or_si128(_mm_or_si128(a, c), _mm_or_si128(d, e));
    if (!prev_ascii || _mm_movemask_epi8(any) != 0) break;
    prev = e;
  }
  for (; i + 16 <= n; i += 16)
    block(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i)));
  // The rest, zero-padded. Zero is ASCII, so a sequence cut off at the end
  // meets a zero byte and is reported.
  alignas(16) unsigned char last[16] = {0};
  if (n > i) std::memcpy(last, b + i, n - i);
  block(_mm_load_si128(reinterpret_cast<const __m128i*>(last)));
  return _mm_movemask_epi8(_mm_cmpeq_epi8(err, _mm_setzero_si128())) == 0xFFFF;
}
#endif  // __SSSE3__

#if defined(__aarch64__)
inline bool valid_neon(const char* s, size_t n)
{
  const unsigned char* b = reinterpret_cast<const unsigned char*>(s);
  const uint8x16_t tab_b1h = vld1q_u8(BYTE_1_HIGH);
  const uint8x16_t tab_b1l = vld1q_u8(BYTE_1_LOW);
  const uint8x16_t tab_b2h = vld1q_u8(BYTE_2_HIGH);
  const uint8x16_t nib = vdupq_n_u8(0x0F);
  const uint8x16_t high_bit = vdupq_n_u8(0x80);
  const uint8x16_t t2 = vdupq_n_u8(0xDF);   // > 0xDF: third-byte lead
  const uint8x16_t t3 = vdupq_n_u8(0xEF);   // > 0xEF: fourth-byte lead
  uint8x16_t err = vdupq_n_u8(0);
  uint8x16_t prev = vdupq_n_u8(0);
  bool prev_ascii = true;
  auto block = [&](const uint8x16_t in) {
    const bool ascii = vmaxvq_u8(in) < 0x80;
    if (ascii && prev_ascii) { prev = in; return; }
    prev_ascii = ascii;
    const uint8x16_t prev1 = vextq_u8(prev, in, 15);
    const uint8x16_t prev2 = vextq_u8(prev, in, 14);
    const uint8x16_t prev3 = vextq_u8(prev, in, 13);
    const uint8x16_t hi1 = vandq_u8(vshrq_n_u8(prev1, 4), nib);
    const uint8x16_t lo1 = vandq_u8(prev1, nib);
    const uint8x16_t hi2 = vandq_u8(vshrq_n_u8(in, 4), nib);
    const uint8x16_t special = vandq_u8(
        vandq_u8(vqtbl1q_u8(tab_b1h, hi1), vqtbl1q_u8(tab_b1l, lo1)),
        vqtbl1q_u8(tab_b2h, hi2));
    const uint8x16_t must23 = vandq_u8(
        vorrq_u8(vcgtq_u8(prev2, t2), vcgtq_u8(prev3, t3)), high_bit);
    err = vorrq_u8(err, veorq_u8(must23, special));
    prev = in;
  };
  size_t i = 0;
  for (; i + 16 <= n; i += 16) block(vld1q_u8(b + i));
  alignas(16) unsigned char last[16] = {0};
  if (n > i) std::memcpy(last, b + i, n - i);
  block(vld1q_u8(last));
  return vmaxvq_u8(err) == 0;
}
#endif  // aarch64 NEON

}  // namespace detail
#endif  // any vector path

// True when any of the n bytes (n < 16) has its high bit set. Word loads that
// overlap cover every byte, so no byte loop is needed.
inline bool short_any_high_bit(const char* s, size_t n)
{
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  constexpr uint64_t highs = 0x8080808080808080ull;
  uint64_t a, b;
  if (n >= 8) {
    std::memcpy(&a, p, 8);
    std::memcpy(&b, p + n - 8, 8);
    return ((a | b) & highs) != 0;
  }
  if (n >= 4) {
    uint32_t c, d;
    std::memcpy(&c, p, 4);
    std::memcpy(&d, p + n - 4, 4);
    return ((c | d) & 0x80808080u) != 0;
  }
  if (n == 0) return false;
  return ((p[0] | p[n / 2] | p[n - 1]) & 0x80) != 0;  // n is 1..3
}

// True when the n bytes at s are well-formed UTF-8.
inline bool valid(const char* s, size_t n)
{
  if (n < 16) {
    // Plain ASCII short strings (most keys) need no further check.
    if (!short_any_high_bit(s, n)) return true;
    return valid_scalar(s, n);
  }
#if defined(__SSSE3__)
  return detail::valid_ssse3(s, n);
#elif defined(__aarch64__)
  return detail::valid_neon(s, n);
#else
  return valid_scalar(s, n);
#endif
}

}  // namespace glz::utf8
