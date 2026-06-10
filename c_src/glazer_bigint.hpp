//------------------------------------------------------------------------------
// Author: Serge Aleynikov <saleyn at gmail dot com>
//------------------------------------------------------------------------------
// Highly optimized BigInt implementation that uses efficient algorithms
// similar to what the Erlang VM uses internally
//------------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>
#include <cstring>
#include <algorithm>
#include <erl_nif.h>

namespace glazer {

struct BigInt {

  static ERL_NIF_TERM
  decode(ErlNifEnv* env, const char* begin, const char* end)
  {
    if (begin >= end) [[unlikely]]
      return 0;
    return parse_decimal_string(env, begin, end);
  }

  static std::string
  encode(ErlNifEnv* env, ERL_NIF_TERM term)
  {
    ErlNifSInt64 small_val;
    if (enif_get_int64(env, term, &small_val)) [[likely]]
      return int64_to_string(small_val);
    return bigint_to_string(env, term);
  }

private:

  static std::string int64_to_string(ErlNifSInt64 value) {
    if (value == 0) return "0";
    bool neg = value < 0;
    ErlNifUInt64 u = neg ? -static_cast<ErlNifUInt64>(value) : static_cast<ErlNifUInt64>(value);
    char buf[22];
    char* p = buf + sizeof(buf);
    do { *--p = '0' + (u % 10); u /= 10; } while (u);
    if (neg) *--p = '-';
    return std::string(p, buf + sizeof(buf) - p);
  }

  static ERL_NIF_TERM parse_decimal_string(ErlNifEnv* env, const char* begin, const char* end) {
    size_t len = end - begin;
    bool negative = false;

    if (*begin == '-') { negative = true; ++begin; --len; }
    else if (*begin == '+')             { ++begin; --len; }

    if (len == 0) [[unlikely]] return 0;

    while (len > 1 && *begin == '0') { ++begin; --len; }

    // Fast path: fits in a signed 64-bit integer
    if (len <= 18) {
      ErlNifSInt64 result = 0;
      for (size_t i = 0; i < len; ++i) {
        char c = begin[i];
        if (c < '0' || c > '9') [[unlikely]] return 0;
        if (result > (LLONG_MAX / 10)) goto bigint;
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
  static ERL_NIF_TERM limbs_to_term(ErlNifEnv* env,
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
    uint8_t* dst = buf.data() + hdr;
    size_t full_limbs = byte_len >> 2; // division by 4
    size_t tail_bytes = byte_len & 3;  // remainder mod 4
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
    if (!enif_binary_to_term(env, buf.data(), hdr + byte_len, &result, 0))
      return 0;
    return result;
  }

  // Encode: parse the external term in-place (no copy), then convert to decimal.
  static std::string bigint_to_string(ErlNifEnv* env, ERL_NIF_TERM term) {
    ErlNifBinary bin;
    if (!enif_term_to_binary(env, term, &bin)) return {};
    std::string result = binary_to_decimal(bin.data, bin.size);
    enif_release_binary(&bin);
    return result;
  }

  static std::string binary_to_decimal(const uint8_t* data, size_t size) {
    if (size < 4 || data[0] != 131) return {};

    const uint8_t* payload;
    size_t byte_len;
    bool negative;

    if (data[1] == 'n') {                            // SMALL_BIG_EXT
      if (size < 4) return {};
      byte_len = data[2];
      negative = data[3] != 0;
      payload  = data + 4;
    } else if (data[1] == 'o') {                     // LARGE_BIG_EXT
      if (size < 7) return {};
      byte_len = (size_t(data[2]) << 24) | (size_t(data[3]) << 16)
               | (size_t(data[4]) <<  8) |  size_t(data[5]);
      negative = data[6] != 0;
      payload  = data + 7;
    } else {
      return {};
    }

    if (byte_len == 0) return "0";
    if (payload + byte_len > data + size) return {};

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
    std::string result;
    result.reserve((byte_len * 5 + 1) / 2 + (negative ? 1 : 0));

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

    if (negative) result += '-';
    // digits are LSdigit-first; reverse to get the number, trimming leading zeros.
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::reverse(digits.begin(), digits.end());
    result.append(digits.begin(), digits.end());
    return result;
  }
};

} // namespace glazer
