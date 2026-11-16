//------------------------------------------------------------------------------
// Author: Serge Aleynikov <saleyn at gmail dot com>
//------------------------------------------------------------------------------
// Highly optimized BigInt implementation that uses efficient algorithms
// similar to what the Erlang VM uses internally
//------------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <climits>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <erl_nif.h>

namespace glz {

struct BigInt {

  static inline ERL_NIF_TERM
  decode(ErlNifEnv* env, const char* begin, const char* end)
  {
    if (begin >= end) [[unlikely]]
      return 0;
    return parse_decimal_string(env, begin, end);
  }

  // Adapts std::string to the `push(char)` / `push(const char*, size_t)`
  // interface expected by `encode`.
  struct StringOut {
    std::string str;
    void push(char c)                  { str.push_back(c); }
    void push(const char* s, size_t n) { str.append(s, n); }
  };

  // Write the decimal representation of an integer term to `out` (anything
  // with `push(char)` / `push(const char*, size_t)`), handling the int64,
  // uint64, and arbitrary-precision bignum cases. Returns false if `term`
  // isn't an integer.
  // Decimal digits of v, two per table lookup, written to out (at least 20
  // bytes). Returns the length. Faster than std::to_chars for large values.
  static inline size_t write_u64(char* out, uint64_t v)
  {
    // Four digits per step from a table of the 10000 four-character groups,
    // built at compile time (40 KB). The final group of up to four digits is
    // taken from the same table, starting at the first nonzero digit.
    static constexpr auto D4 = [] {
      std::array<char, 40000> t{};
      for (int i = 0; i < 10000; ++i) {
        t[4 * i] = char('0' + i / 1000);
        t[4 * i + 1] = char('0' + i / 100 % 10);
        t[4 * i + 2] = char('0' + i / 10 % 10);
        t[4 * i + 3] = char('0' + i % 10);
      }
      return t;
    }();
    char  tmp[24];
    char* e = tmp + sizeof tmp;
    char* s = e;
    while (v >= 10000) {
      const unsigned r = unsigned(v % 10000);
      v /= 10000;
      s -= 4;
      std::memcpy(s, D4.data() + 4 * r, 4);
    }
    uint32_t w = uint32_t(v);  // below 10000
    if (w >= 1000) {
      s -= 4;
      std::memcpy(s, D4.data() + 4 * w, 4);
    } else if (w >= 100) {
      s -= 3;
      std::memcpy(s, D4.data() + 4 * w + 1, 3);
    } else if (w >= 10) {
      s -= 2;
      std::memcpy(s, D4.data() + 4 * w + 2, 2);
    } else {
      *--s = char('0' + w);
    }
    const size_t n = size_t(e - s);
    std::memcpy(out, s, n);
    return n;
  }

  // As write_u64, with a leading '-' for negative values (at least 20 bytes).
  static inline size_t write_i64(char* out, int64_t v)
  {
    if (v >= 0) return write_u64(out, uint64_t(v));
    out[0] = '-';
    return 1 + write_u64(out + 1, uint64_t(0) - uint64_t(v));
  }

  template <class Out>
  static inline bool encode(ErlNifEnv* env, ERL_NIF_TERM term, Out& out)
  {
    ErlNifSInt64 i;
    if (enif_get_int64(env, term, &i)) [[likely]] {
      char buf[24];
      out.push(buf, write_i64(buf, i));
      return true;
    }

    ErlNifUInt64 u;
    if (enif_get_uint64(env, term, &u)) {
      char buf[24];
      out.push(buf, write_u64(buf, u));
      return true;
    }

    // bigint — doesn't fit in 64 bits
    ErlNifBinary bin;
    if (!enif_term_to_binary(env, term, &bin)) return false;
    bool ok = binary_to_decimal(bin.data, bin.size, out);
    enif_release_binary(&bin);
    return ok;
  }

  // Build an integer term from little-endian 32-bit limbs and a sign
  static inline ERL_NIF_TERM term_from_magnitude(ErlNifEnv* env,
                                                 const std::vector<uint32_t>& limbs, bool negative) {
    return limbs_to_term(env, limbs, negative);
  }

private:

  static inline ERL_NIF_TERM parse_decimal_string(ErlNifEnv* env, const char* begin, const char* end) {
    size_t len = end - begin;
    bool negative = false;

    if (*begin == '-') { negative = true; ++begin; --len; }
    else if (*begin == '+')             { ++begin; --len; }

    if (len == 0) [[unlikely]] return 0;

    while (len > 1 && *begin == '0')    { ++begin; --len; }

    // Fast path: fits in a signed 64-bit integer
    if (len <= 18) {
      ErlNifSInt64 result = 0;
      for (size_t i = 0; i < len; ++i) {
        char c = begin[i];
        if (c < '0' || c > '9') [[unlikely]]
          return 0;
        if (result > (LLONG_MAX / 10))
          goto bigint;
        result = result * 10 + (c - '0');
      }
      return negative ? enif_make_int64(env, -result) : enif_make_int64(env, result);
    }

  bigint:
    return create_bigint(env, begin, len, negative);
  }

  // Decode: build limb array left-to-right in 9-digit chunks, then write
  // the external term format in one allocation.
  static ERL_NIF_TERM create_bigint(ErlNifEnv* env,
                                    const char* digits, size_t len, bool negative)
  {
    std::vector<uint32_t> limbs;
    limbs.reserve(len / 9 + 2);

    const char* ptr = digits;
    size_t remaining = len;

    while (remaining >= 9) {
      uint32_t chunk = 0;
      for (int i = 0; i < 9; ++i)
        chunk = chunk * 10 + (ptr[i] - '0');
      mul_add(limbs, 1000000000u, chunk);
      ptr += 9; remaining -= 9;
    }
    if (remaining > 0) {
      uint32_t chunk = 0, mul = 1;
      for (size_t i = 0; i < remaining; ++i) {
        chunk = chunk * 10 + (ptr[i] - '0');
        mul *= 10;
      }
      mul_add(limbs, mul, chunk);
    }

    return limbs_to_term(env, limbs, negative);
  }

  // Multiply limbs by `mul` then add `add` in one pass (avoids a second loop).
  static void mul_add(std::vector<uint32_t>& limbs, uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    for (auto& w : limbs) {
      auto v = static_cast<uint64_t>(w) * mul + carry;
      w = static_cast<uint32_t>(v);
      carry = v >> 32;
    }
    while (carry) {
      limbs.push_back(static_cast<uint32_t>(carry));
      carry >>= 32;
    }
  }

  // Write Erlang external bignum format directly from the limb array —
  // no intermediate `bytes` vector.
  static ERL_NIF_TERM limbs_to_term(
    ErlNifEnv* env,
    const std::vector<uint32_t>& limbs, bool negative)
  {
    if (limbs.empty()) return enif_make_int(env, 0);

    // Count significant bytes (trim trailing zero bytes of the last limb).
    size_t byte_len = limbs.size() * 4;
    {
      uint32_t top = limbs.back();
      if ((top >> 24) == 0) { --byte_len;
        if ((top >> 16) == 0) { --byte_len;
          if ((top >>  8) == 0) { --byte_len; }}}
    }

    // Header: 1 (version) + 1 (tag) + 1 or 4 (len) + 1 (sign) + byte_len (payload)
    bool small = byte_len <= 255;
    size_t hdr  = small ? 4 : 7;  // 131 + tag + len_bytes + sign
    std::vector<uint8_t> buf(hdr + byte_len);

    buf[0] = 131;
    if (small) {
      buf[1] = 'n';
      buf[2] = static_cast<uint8_t>(byte_len);
      buf[3] = negative ? 1 : 0;
    } else {
      buf[1] = 'o';
      buf[2] = static_cast<uint8_t>(byte_len >> 24);
      buf[3] = static_cast<uint8_t>(byte_len >> 16);
      buf[4] = static_cast<uint8_t>(byte_len >>  8);
      buf[5] = static_cast<uint8_t>(byte_len);
      buf[6] = negative ? 1 : 0;
    }

    // Copy limbs as little-endian bytes directly into the payload.
    // Use byte_len to bound the write: full limbs first, then the
    // partial last limb (byte_len may be < limbs.size()*4 after trimming).
    uint8_t* dst        = buf.data() + hdr;
    size_t   full_limbs = byte_len >> 2; // division by 4
    size_t   tail_bytes = byte_len & 3;  // remainder mod 4
    for (size_t i = 0; i < full_limbs; ++i) {
      uint32_t w = limbs[i];
      dst[0] = w & 0xFF;
      dst[1] = (w >>  8) & 0xFF;
      dst[2] = (w >> 16) & 0xFF;
      dst[3] = (w >> 24) & 0xFF;
      dst += 4;
    }
    if (tail_bytes) {
      uint32_t w = limbs[full_limbs];
      for (size_t j = 0; j < tail_bytes; ++j)
        *dst++ = (w >> (j * 8)) & 0xFF;
    }

    ERL_NIF_TERM result;
    if (!enif_binary_to_term(env, buf.data(), hdr + byte_len, &result, 0)) [[unlikely]]
      return 0;
    return result;
  }

  // Parses an Erlang external-term-format bignum (SMALL_BIG_EXT/LARGE_BIG_EXT)
  // and writes its decimal representation to `out`. Returns false on
  // malformed input.
  template <class Out>
  static bool binary_to_decimal(const uint8_t* data, size_t size, Out& out) {
    if (size < 4 || data[0] != 131) return false;

    const uint8_t* payload;
    size_t byte_len;
    bool negative;

    if (data[1] == 'n') {                            // SMALL_BIG_EXT
      if (size < 4) return false;
      byte_len = data[2];
      negative = data[3] != 0;
      payload  = data + 4;
    } else if (data[1] == 'o') {                     // LARGE_BIG_EXT
      if (size < 7) return false;
      byte_len = (size_t(data[2]) << 24) | (size_t(data[3]) << 16)
               | (size_t(data[4]) <<  8) |  size_t(data[5]);
      negative = data[6] != 0;
      payload  = data + 7;
    } else {
      return false;
    }

    if (byte_len == 0) { out.push('0'); return true; }
    if (payload + byte_len > data + size) return false;

    // Reinterpret payload as little-endian uint32 limbs (may have a partial
    // last limb if byte_len is not a multiple of 4).
    size_t n_full  = byte_len / 4;
    size_t n_extra = byte_len % 4;
    size_t n_limbs = n_full + (n_extra ? 1 : 0);

    std::vector<uint32_t> words(n_limbs);
    for (size_t i = 0; i < n_full; ++i) {
      const uint8_t* p = payload + i * 4;
      words[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8)
               | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    if (n_extra) {
      const uint8_t* p = payload + n_full * 4;
      uint32_t w = 0;
      for (size_t j = 0; j < n_extra; ++j) w |= uint32_t(p[j]) << (j * 8);
      words[n_full] = w;
    }

    // decimal digit count upper bound: ceil(byte_len * log10(256)) < byte_len * 2.41
    // Use (byte_len * 5 + 1) / 2 as a tight integer approximation.
    std::vector<char> digits;
    digits.reserve((byte_len * 5 + 1) / 2);

    constexpr uint32_t B = 1000000000u;
    while (!words.empty()) {
      uint64_t rem = 0;
      for (auto it = words.rbegin(); it != words.rend(); ++it) {
        uint64_t d = (rem << 32) | *it;
        *it = static_cast<uint32_t>(d / B);
        rem = d % B;
      }
      while (!words.empty() && words.back() == 0) words.pop_back();

      uint32_t r = static_cast<uint32_t>(rem);
      bool last = words.empty();
      for (int i = 0; i < 9; ++i) {
        digits.push_back('0' + (r % 10));
        r /= 10;
        if (r == 0 && last) break;
      }
    }

    if (negative) out.push('-');
    // digits are LSdigit-first; reverse to get the number, trimming leading zeros.
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::reverse(digits.begin(), digits.end());
    out.push(digits.data(), digits.size());
    return true;
  }
};

//------------------------------------------------------------------------------
// BigNum: an exact integer of any size, for comparisons and arithmetic where a
// double is not exact enough (schema bounds, equality, multipleOf). Values are
// a sign and little-endian 32-bit magnitude limbs with no high zero limbs.
//------------------------------------------------------------------------------
struct BigNum {
  bool negative = false;
  std::vector<uint32_t> mag;   // empty for zero; negative is false for zero

  bool is_zero() const { return mag.empty(); }

  void trim() {
    while (!mag.empty() && mag.back() == 0) mag.pop_back();
    if (mag.empty()) negative = false;
  }

  static void set_u64(std::vector<uint32_t>& m, uint64_t u) {
    m.clear();
    while (u) { m.push_back(static_cast<uint32_t>(u)); u >>= 32; }
  }

  static BigNum from_int64(int64_t v) {
    BigNum r;
    uint64_t u = v < 0 ? uint64_t(0) - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
    set_u64(r.mag, u);
    r.negative = v < 0;
    r.trim();
    return r;
  }

  // True when the value fits in an int64, which is then stored in out
  bool to_int64(int64_t& out) const {
    if (mag.size() > 2) return false;
    uint64_t u = 0;
    for (size_t i = mag.size(); i-- > 0;) u = (u << 32) | mag[i];
    if (negative) {
      if (u > static_cast<uint64_t>(INT64_MAX) + 1) return false;
      out = static_cast<int64_t>(uint64_t(0) - u);
    } else {
      if (u > static_cast<uint64_t>(INT64_MAX)) return false;
      out = static_cast<int64_t>(u);
    }
    return true;
  }

  // Approximate value, for the places that combine with floats
  double to_double() const {
    double r = 0;
    for (size_t i = mag.size(); i-- > 0;) r = r * 4294967296.0 + mag[i];
    return negative ? -r : r;
  }

  // Exact value of any Erlang integer term. False for non-integers, including
  // floats.
  static bool from_term(ErlNifEnv* env, ERL_NIF_TERM term, BigNum& out) {
    ErlNifSInt64 i;
    if (enif_get_int64(env, term, &i)) { out = from_int64(i); return true; }
    ErlNifUInt64 u;
    if (enif_get_uint64(env, term, &u)) {
      out = BigNum();
      set_u64(out.mag, u);
      return true;
    }
    double d;
    if (enif_get_double(env, term, &d) || !enif_is_number(env, term)) return false;
    ErlNifBinary bin;
    if (!enif_term_to_binary(env, term, &bin)) return false;
    bool ok = parse_external(bin.data, bin.size, out);
    enif_release_binary(&bin);
    return ok;
  }

  // The integer term for this value
  ERL_NIF_TERM to_term(ErlNifEnv* env) const {
    int64_t small;
    if (to_int64(small)) return enif_make_int64(env, small);
    return BigInt::term_from_magnitude(env, mag, negative);
  }

  // Exact value of a finite, integral double (false otherwise)
  static bool from_integral_double(double d, BigNum& out) {
    if (!std::isfinite(d) || d != std::trunc(d)) return false;
    out = BigNum();
    if (std::fabs(d) < 9.0e18) {
      out = from_int64(static_cast<int64_t>(d));
      return true;
    }
    int e = 0;
    double m = std::frexp(std::fabs(d), &e);          // |d| = m * 2^e, 0.5 <= m < 1
    auto mantissa = static_cast<uint64_t>(std::ldexp(m, 53));   // exact: 53 bits
    set_u64(out.mag, mantissa);
    shift_left(out.mag, static_cast<size_t>(e - 53)); // |d| >= 2^63, so e - 53 >= 0
    out.negative = d < 0;
    out.trim();
    return true;
  }

  static int compare_magnitude(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
      if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
  }

  // -1, 0 or 1
  static int compare(const BigNum& a, const BigNum& b) {
    if (a.negative != b.negative) return a.negative ? -1 : 1;
    int c = compare_magnitude(a.mag, b.mag);
    return a.negative ? -c : c;
  }

  // Exact comparison of an integer with a finite double: -1, 0 or 1
  static int compare_double(const BigNum& a, double d) {
    double t = std::trunc(d);
    BigNum bt;
    from_integral_double(t, bt);
    int c = compare(a, bt);
    if (c != 0) return c;
    if (d > t) return -1;   // a equals the whole part of d, and d has a positive fraction
    if (d < t) return 1;
    return 0;
  }

  // |a| mod d, for d > 0
  static uint64_t mod_u64(const BigNum& a, uint64_t d) {
    unsigned __int128 rem = 0;
    for (size_t i = a.mag.size(); i-- > 0;)
      rem = ((rem << 32) | a.mag[i]) % d;
    return static_cast<uint64_t>(rem);
  }

  // |a| mod |d|, for a non-zero d, by shift and subtract
  static BigNum mod(const BigNum& a, const BigNum& d) {
    BigNum r;
    for (size_t i = a.mag.size() * 32; i-- > 0;) {
      shift_left(r.mag, 1);
      if ((a.mag[i / 32] >> (i % 32)) & 1u) {
        if (r.mag.empty()) r.mag.push_back(1);
        else r.mag[0] |= 1u;
      }
      if (compare_magnitude(r.mag, d.mag) >= 0)
        subtract_magnitude(r.mag, d.mag);
    }
    r.negative = false;
    r.trim();
    return r;
  }

private:

  static void shift_left(std::vector<uint32_t>& m, size_t bits) {
    if (m.empty()) return;
    size_t words = bits / 32, rem = bits % 32;
    if (rem) {
      uint32_t carry = 0;
      for (auto& w : m) {
        uint32_t next = (w << rem) | carry;
        carry = w >> (32 - rem);
        w = next;
      }
      if (carry) m.push_back(carry);
    }
    m.insert(m.begin(), words, 0u);
  }

  // a -= b, for |a| >= |b|
  static void subtract_magnitude(std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      uint64_t bi = (i < b.size() ? b[i] : 0) + borrow;
      if (a[i] >= bi) { a[i] = static_cast<uint32_t>(a[i] - bi); borrow = 0; }
      else            { a[i] = static_cast<uint32_t>((uint64_t(1) << 32) + a[i] - bi); borrow = 1; }
    }
    while (!a.empty() && a.back() == 0) a.pop_back();
  }

  // Reads an Erlang external-format integer (SMALL_BIG_EXT or LARGE_BIG_EXT)
  static bool parse_external(const uint8_t* data, size_t size, BigNum& out) {
    if (size < 4 || data[0] != 131) return false;
    const uint8_t* payload;
    size_t byte_len;
    bool neg;
    if (data[1] == 'n') {
      byte_len = data[2];
      neg = data[3] != 0;
      payload = data + 4;
    } else if (data[1] == 'o') {
      if (size < 7) return false;
      byte_len = (size_t(data[2]) << 24) | (size_t(data[3]) << 16) | (size_t(data[4]) << 8) | size_t(data[5]);
      neg = data[6] != 0;
      payload = data + 7;
    } else {
      return false;
    }
    if (payload + byte_len > data + size) return false;
    out = BigNum();
    out.mag.assign((byte_len + 3) / 4, 0u);
    for (size_t i = 0; i < byte_len; ++i)
      out.mag[i / 4] |= uint32_t(payload[i]) << (8 * (i % 4));
    out.negative = neg;
    out.trim();
    return true;
  }
};

} // namespace glz
