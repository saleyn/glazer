// Float-to-text for the encoders (glz::fp): shortest round-trip formatting of
// finite doubles, a C++ port of the scalar
// path of zmij 1.0.23 (https://github.com/dtolnay/zmij, MIT License, copyright
// David Tolnay). The arithmetic, tables and output layout follow the Rust code;
// only the x86-64 SIMD digit paths are replaced by their scalar equivalents,
// which produce the same digits.
//
// Usage
// -----
//   char buf[32];
//   size_t n = glz::fp::ftoa(1.5, buf);   // buf[0..n) == "1.5"
//
// The function writes the text into buf and returns its length. It does not
// write a terminator. buf must hold at least 32 bytes. Nothing is allocated.
//
// Output examples (checked against zmij 1.0.23):
//   ftoa(1.5)                 -> "1.5"
//   ftoa(0.0)                 -> "0.0"
//   ftoa(-0.0)                -> "-0.0"
//   ftoa(-2.5e-5)             -> "-0.000025"
//   ftoa(1e20)                -> "1e+20"
//   ftoa(1e-7)                -> "1e-7"
//   ftoa(5e-324)              -> "5e-324"
//
// Notes for callers
//   - Only finite values are accepted. NaN and infinity have no JSON form, so
//     the encoder checks for them first and writes its own text.
//   - Integral values in fixed notation keep a ".0" suffix ("2.0"), and
//     exponent notation is used outside the range 1e-5 to 1e16. Exponents are
//     written without padding ("1e+20", not "1e+020").
//   - Output round-trips: parsing the text returns the same double.
//
// Example: writing a JSON number
//   char buf[32];
//   size_t n = glz::fp::ftoa(value, buf);
//   out.append(buf, n);
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace glz::fp {

namespace detail {

using u128 = unsigned __int128;

struct uint128 {
  uint64_t hi;
  uint64_t lo;
};

constexpr uint64_t umul128_hi64(uint64_t x, uint64_t y) {
  return uint64_t((u128(x) * y) >> 64);
}

// (x * y + c) >> 64
constexpr uint64_t umul128_add_hi64(uint64_t x, uint64_t y, uint64_t c) {
  return uint64_t((u128(x) * y + c) >> 64);
}

constexpr uint128 umul192_hi128(uint64_t x_hi, uint64_t x_lo, uint64_t y) {
  const u128 p = u128(x_hi) * y;
  const uint64_t p_lo = uint64_t(p);
  const uint64_t lo = p_lo + uint64_t((u128(x_lo) * y) >> 64);  // wrapping
  return {uint64_t(p >> 64) + uint64_t(lo < p_lo), lo};
}

// x / 10 for x <= 2^62
constexpr uint64_t div10(uint64_t x) {
  constexpr uint64_t DIV10_SIG64 = (uint64_t(1) << 63) / 5 + 1;
  return umul128_hi64(x, DIV10_SIG64);
}

// floor(log10(2^bin_exp)) if regular, floor(log10(3/4 * 2^bin_exp)) otherwise.
constexpr int32_t compute_dec_exp(int32_t bin_exp, bool regular) {
  constexpr int32_t LOG10_3_OVER_4_SIG = 131072;
  constexpr int32_t LOG10_2_SIG = 315653;
  constexpr int32_t LOG10_2_EXP = 20;
  return (bin_exp * LOG10_2_SIG - int32_t(!regular) * LOG10_3_OVER_4_SIG) >> LOG10_2_EXP;
}

constexpr uint8_t compute_exp_shift(int32_t bin_exp, int32_t dec_exp) {
  constexpr int32_t LOG2_POW10_SIG = 217707;
  constexpr int32_t LOG2_POW10_EXP = 16;
  const int32_t pow10_bin_exp = (-dec_exp * LOG2_POW10_SIG) >> LOG2_POW10_EXP;
  return uint8_t(bin_exp + pow10_bin_exp + 1);
}

constexpr size_t EXTRA_SHIFT = 6;
constexpr int32_t EXP_OFFSET = 1075;  // f64: bias 1023 + 52 significand bits
constexpr int32_t DEC_EXP_MIN = -293;
constexpr size_t NUM_POW10S = 618;

inline constexpr std::array<uint64_t, 28> POW10_MINOR = {
    0x8000000000000000ull, 0xa000000000000000ull, 0xc800000000000000ull,
    0xfa00000000000000ull, 0x9c40000000000000ull, 0xc350000000000000ull,
    0xf424000000000000ull, 0x9896800000000000ull, 0xbebc200000000000ull,
    0xee6b280000000000ull, 0x9502f90000000000ull, 0xba43b74000000000ull,
    0xe8d4a51000000000ull, 0x9184e72a00000000ull, 0xb5e620f480000000ull,
    0xe35fa931a0000000ull, 0x8e1bc9bf04000000ull, 0xb1a2bc2ec5000000ull,
    0xde0b6b3a76400000ull, 0x8ac7230489e80000ull, 0xad78ebc5ac620000ull,
    0xd8d726b7177a8000ull, 0x878678326eac9000ull, 0xa968163f0a57b400ull,
    0xd3c21bcecceda100ull, 0x84595161401484a0ull, 0xa56fa5b99019a5c8ull,
    0xcecb8f27f4200f3aull,
};

inline constexpr std::array<uint128, 23> POW10_MAJOR = {{
    {0xaf8e5410288e1b6full, 0x07ecf0ae5ee44ddaull},  // -303
    {0xb1442798f49ffb4aull, 0x99cd11cfdf41779dull},  // -275
    {0xb2fe3f0b8599ef07ull, 0x861fa7e6dcb4aa15ull},  // -247
    {0xb4bca50b065abe63ull, 0x0fed077a756b53aaull},  // -219
    {0xb67f6455292cbf08ull, 0x1a3bc84c17b1d543ull},  // -191
    {0xb84687c269ef3bfbull, 0x3d5d514f40eea742ull},  // -163
    {0xba121a4650e4ddebull, 0x92f34d62616ce413ull},  // -135
    {0xbbe226efb628afeaull, 0x890489f70a55368cull},  // -107
    {0xbdb6b8e905cb600full, 0x5400e987bbc1c921ull},  //  -79
    {0xbf8fdb78849a5f96ull, 0xde98520472bdd034ull},  //  -51
    {0xc16d9a0095928a27ull, 0x75b7053c0f178294ull},  //  -23
    {0xc350000000000000ull, 0x0000000000000000ull},  //    5
    {0xc5371912364ce305ull, 0x6c28000000000000ull},  //   33
    {0xc722f0ef9d80aad6ull, 0x424d3ad2b7b97ef6ull},  //   61
    {0xc913936dd571c84cull, 0x03bc3a19cd1e38eaull},  //   89
    {0xcb090c8001ab551cull, 0x5cadf5bfd3072cc6ull},  //  117
    {0xcd036837130890a1ull, 0x36dba887c37a8c10ull},  //  145
    {0xcf02b2c21207ef2eull, 0x94f967e45e03f4bcull},  //  173
    {0xd106f86e69d785c7ull, 0xe13336d701beba52ull},  //  201
    {0xd31045a8341ca07cull, 0x1ede48111209a051ull},  //  229
    {0xd51ea6fa85785631ull, 0x552a74227f3ea566ull},  //  257
    {0xd732290fbacaf133ull, 0xa97c177947ad4096ull},  //  285
    {0xd94ad8b1c7380874ull, 0x18375281ae7822bcull},  //  313
}};

inline constexpr std::array<uint32_t, 20> POW10_FIXUPS = {
    0x0a4e363f, 0x00001840, 0x00006400, 0x24200040, 0x00000000,
    0x0c000000, 0x82c81380, 0x5e4ce01f, 0xd730f60f, 0x0000001b,
    0x00000000, 0xcdf7fffc, 0x6e8201d8, 0x40cd3fd1, 0xdb642501,
    0x00000d0d, 0x14042400, 0x53713840, 0x11781db4, 0x00000000,
};

// 128-bit significand of 10^(i + DEC_EXP_MIN) rounded down (Dougall Johnson's method).
constexpr uint128 compute_pow10(uint32_t i) {
  constexpr uint32_t STRIDE = POW10_MINOR.size();
  const uint64_t m = POW10_MINOR[(i + 10) % STRIDE];
  const uint128 h = POW10_MAJOR[(i + 10) / STRIDE];

  const uint64_t h1 = umul128_hi64(h.lo, m);
  const uint64_t c0 = h.lo * m;
  const uint64_t c1 = h1 + h.hi * m;
  const uint64_t c2 = uint64_t(c1 < h1) + umul128_hi64(h.hi, m);

  uint128 result = (c2 >> 63) != 0 ? uint128{c2, c1}
                                   : uint128{(c2 << 1) | (c1 >> 63), (c1 << 1) | (c0 >> 63)};
  result.lo -= (POW10_FIXUPS[i >> 5] >> (i & 31)) & 1;
  return result;
}

inline constexpr auto POW10_TABLE = [] {
  std::array<uint64_t, NUM_POW10S * 2> t{};
  for (uint32_t i = 0; i < NUM_POW10S; ++i) {
    const uint128 r = compute_pow10(i);
    t[i * 2] = r.hi;
    t[i * 2 + 1] = r.lo;
  }
  return t;
}();

// Significand of 10^dec_exp_arg, where dec_exp_arg is -dec_exp - 1 as in zmij.
inline uint128 pow10_significand(int32_t dec_exp_arg) {
  const size_t i = size_t(dec_exp_arg - DEC_EXP_MIN);
  return {POW10_TABLE[i * 2], POW10_TABLE[i * 2 + 1]};
}

// Shift per raw binary exponent, with EXTRA_SHIFT added.
inline constexpr auto EXP_SHIFTS = [] {
  std::array<uint8_t, 2048> t{};
  for (int32_t raw_exp = 0; raw_exp < 2048; ++raw_exp) {
    int32_t bin_exp = raw_exp - EXP_OFFSET;
    if (raw_exp == 0) bin_exp += 1;
    const int32_t dec_exp = compute_dec_exp(bin_exp, true);
    t[size_t(raw_exp)] = uint8_t(compute_exp_shift(bin_exp, dec_exp + 1) + uint8_t(EXTRA_SHIFT));
  }
  return t;
}();

constexpr uint64_t THRESHOLD = 1000000000000000ull;        // 10^15, for f64
constexpr uint64_t BIASED_HALF = (uint64_t(1) << 63) + 6;  // +6 from zmij's verify.py
constexpr int32_t MAX_DIGITS10 = 17;

struct ToDecimal {
  int64_t sig;
  int32_t exp;
  uint8_t last_digit;
  bool has_last_digit;
};

inline ToDecimal to_decimal(uint64_t bin_sig, int64_t raw_exp, bool regular) {
  const int32_t bin_exp = int32_t(raw_exp - EXP_OFFSET);

  if (!regular) {
    const int32_t dec_exp       = compute_dec_exp(bin_exp, false);
    const uint8_t shift         = uint8_t(compute_exp_shift(bin_exp, dec_exp + 1) + uint8_t(EXTRA_SHIFT));
    const uint128 pow10         = pow10_significand(-dec_exp - 1);
    const uint128 p             = umul192_hi128(pow10.hi, pow10.lo, bin_sig << shift);

    uint64_t        integral    = p.hi >> EXTRA_SHIFT;
    const uint64_t  fractional  = (p.hi << (64 - EXTRA_SHIFT)) | (p.lo >> EXTRA_SHIFT);

    const uint64_t  half_ulp    = pow10.hi >> (EXTRA_SHIFT + 1 - shift);
    const bool      round_up    = half_ulp > UINT64_MAX - fractional;
    const bool      round_down  = (half_ulp >> 1) > fractional;
    integral += uint64_t(round_up);

    int32_t         digit       = int32_t(umul128_add_hi64(fractional, 10, (uint64_t(1) << 63) - 1));
    const int32_t   lo          = int32_t(umul128_add_hi64(fractional - (half_ulp >> 1), 10, ~uint64_t(0)));
    if (digit < lo) digit       = lo;
    return {int64_t(integral), dec_exp, uint8_t(digit), !(round_up || round_down)};
  }

  const int32_t  dec_exp        = compute_dec_exp(bin_exp, true);
  const uint8_t  shift          = EXP_SHIFTS[size_t(raw_exp)];
  const uint64_t even           = 1 - (bin_sig & 1);

  const uint128  pow10          = pow10_significand(-dec_exp - 1);
  const uint128  p              = umul192_hi128(pow10.hi, pow10.lo, bin_sig << shift);

  uint64_t       integral       = p.hi >> EXTRA_SHIFT;
  const uint64_t fractional     = (p.hi << (64 - EXTRA_SHIFT)) | (p.lo >> EXTRA_SHIFT);

  const uint64_t half_ulp       = (pow10.hi >> (EXTRA_SHIFT + 1 - shift)) + even;
  const bool     round_up       = uint64_t(fractional + half_ulp) < fractional;
  const bool     round_down     = half_ulp > fractional;
  integral += uint64_t(round_up);

  int32_t digit = int32_t(umul128_add_hi64(fractional, 10, BIASED_HALF));
  if (fractional == (uint64_t(1) << 62)) digit = 2;  // round 2.5 to 2
  return {int64_t(integral), dec_exp, uint8_t(digit), !(round_up || round_down)};
}

// Count of bytes up to the most significant nonzero byte (zmij's count_trailing_nonzeros).
constexpr size_t count_nonzero_bytes(uint64_t x) {
  const uint64_t  v = (x << 1) | 1;
  const int clz = v == 0 ? 64 : __builtin_clzll(v);
  return size_t(70 - clz) / 8;
}

// Eight digits, as bytes 0..9 in big-endian order, with the length in bytes.
struct Bcd8 {
  uint64_t bcd;
  size_t   len;
};

inline Bcd8 to_bcd8(uint64_t abcdefgh) {
  constexpr uint64_t DIV10K_SIG = (uint64_t(1) << 40) / 10000 + 1;
  constexpr uint64_t NEG10K     = (uint64_t(1) << 32) - 10000;
  constexpr uint64_t DIV100_SIG = (uint64_t(1) << 19) / 100 + 1;
  constexpr uint64_t NEG100     = (uint64_t(1) << 16) - 100;
  constexpr uint64_t DIV10_SIG  = (uint64_t(1) << 10) / 10 + 1;
  constexpr uint64_t NEG10      = (uint64_t(1) << 8)  - 10;

  const uint64_t abcd_efgh = abcdefgh + NEG10K * ((abcdefgh * DIV10K_SIG) >> 40);
  const uint64_t ab_cd_ef_gh =
      abcd_efgh + NEG100 * (((abcd_efgh * DIV100_SIG) >> 19) & 0x7f0000007full);
  const uint64_t a_b_c_d_e_f_g_h =
      ab_cd_ef_gh + NEG10 * (((ab_cd_ef_gh * DIV10_SIG) >> 10) & 0xf000f000f000full);
  const uint64_t bcd = __builtin_bswap64(a_b_c_d_e_f_g_h);
  return {bcd, count_nonzero_bytes(bcd)};
}

// Sixteen ASCII digits of value (< 10^16) in memory order, and the digit count.
struct Digits {
  char digits[16];
  size_t num_digits;
};

inline Digits to_digits_64(uint64_t value) {
  constexpr uint64_t ZEROS = 0x0101010101010101ull * uint64_t('0');
  const uint32_t hi        = uint32_t(value / 100000000);
  const uint32_t lo        = uint32_t(value % 100000000);
  const Bcd8     hi_bcd    = to_bcd8(hi);
  Digits r{};
  const uint64_t hi_digits = hi_bcd.bcd + ZEROS;
  std::memcpy(r.digits, &hi_digits, 8);
  if (lo == 0) {
    std::memcpy(r.digits + 8, &ZEROS, 8);
    r.num_digits = hi_bcd.len;
    return r;
  }
  const Bcd8      lo_bcd    = to_bcd8(lo);
  const uint64_t  lo_digits = lo_bcd.bcd + ZEROS;
  std::memcpy(r.digits + 8, &lo_digits, 8);
  r.num_digits = 8 + lo_bcd.len;
  return r;
}

}  // namespace detail

// Writes the shortest round-trip decimal form of a finite double to buffer,
// which must hold at least 32 bytes. Returns the length of the text (no
// terminator). See the usage notes at the top of this file. Matches
// zmij::Buffer::format_finite. Negative zero gives "-0.0".
inline size_t ftoa(double value, char* buffer)
{
  using namespace detail;
  uint64_t bits;
  std::memcpy(&bits, &value, 8);
  const int64_t bin_exp = int64_t((bits >> 52) & 0x7ff);
  const uint64_t bin_sig = bits & ((uint64_t(1) << 52) - 1);
  const bool negative = (bits >> 63) != 0;

  char* out = buffer;
  *out = '-';
  out += size_t(negative);

  ToDecimal dec;
  if (bin_exp == 0) {
    if (bin_sig == 0) {
      out[0] = '0';
      out[1] = '.';
      out[2] = '0';
      return size_t(out - buffer) + 3;
    }
    dec = to_decimal(bin_sig, 1, true);
    int64_t dec_sig = dec.sig * 10 + (-int64_t(dec.has_last_digit) & int64_t(dec.last_digit));
    int32_t dec_exp = dec.exp;
    while (dec_sig < int64_t(THRESHOLD)) {
      dec_sig *= 10;
      dec_exp -= 1;
    }
    const uint64_t d = div10(uint64_t(dec_sig));
    const int64_t last_digit = dec_sig - int64_t(d) * 10;
    dec = {int64_t(d), dec_exp, uint8_t(last_digit), last_digit != 0};
  } else {
    dec = to_decimal(bin_sig | (uint64_t(1) << 52), bin_exp, bin_sig != 0);
  }

  bool has_last_digit = dec.has_last_digit;
  const bool has_extra_digit = dec.sig >= int64_t(THRESHOLD);
  int32_t dec_exp = dec.exp + MAX_DIGITS10 - 2 + int32_t(has_extra_digit);

  // Write significand.
  const Digits dig = to_digits_64(uint64_t(dec.sig));
  std::memcpy(out + has_extra_digit, dig.digits, 16);
  out[has_extra_digit + 16] = char('0' + dec.last_digit);
  const size_t length = size_t(has_extra_digit) +
                        (has_last_digit ? 16 + 1 : dig.num_digits) - 1;

  if (dec_exp >= -5 && dec_exp <= 15) {
    if (int32_t(length) - 1 <= dec_exp) {
      // 1234e7 -> 12340000000.0
      std::memmove(out, out + 1, length);
      std::memset(out + length, '0', size_t(dec_exp + 3 - int32_t(length)));
      out[dec_exp + 1] = '.';
      return size_t(out - buffer) + size_t(dec_exp + 3);
    } else if (dec_exp >= 0) {
      // 1234e-2 -> 12.34
      std::memmove(out, out + 1, size_t(dec_exp + 1));
      out[dec_exp + 1] = '.';
      return size_t(out - buffer) + length + 1;
    } else {
      // 1234e-6 -> 0.001234
      const size_t zeros = size_t(1 - dec_exp);
      std::memmove(out + zeros, out + 1, length);
      std::memset(out, '0', zeros);
      out[1] = '.';
      return size_t(out - buffer) + zeros + length;
    }
  }

  // Exponential notation: 1234e30 -> 1.234e33
  out[0] = out[1];
  out[1] = '.';
  out += length + size_t(length > 1);

  // Exponent: 'e', sign, then the digits of |dec_exp| without padding.
  out[0] = 'e';
  out[1] = dec_exp >= 0 ? '+' : '-';
  uint32_t e = uint32_t(dec_exp >= 0 ? dec_exp : -dec_exp);
  char* p = out + 2;
  if (e >= 100) {
    *p++ = char('0' + e / 100);
    e %= 100;
    *p++ = char('0' + e / 10);
    *p++ = char('0' + e % 10);
  } else if (e >= 10) {
    *p++ = char('0' + e / 10);
    *p++ = char('0' + e % 10);
  } else {
    *p++ = char('0' + e);
  }
  return size_t(p - buffer);
}

}  // namespace glz::fp
