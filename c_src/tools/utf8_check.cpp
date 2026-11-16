// Checks glz::utf8::valid and first_invalid against the scalar reference:
// every 2- and 3-byte sequence at several offsets inside a 16-byte-aligned
// block, plus random byte strings. Build with -march=native (SSSE3).
//   g++ -std=c++2b -O2 -march=native -Ic_src c_src/tools/utf8_check.cpp -o utf8_check
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <random>
#include <vector>
#include "glazer_utf8_validate.hpp"

namespace u = glz::utf8;

static long failures = 0;

static void check(const std::vector<unsigned char>& v) {
  const char*  s    = reinterpret_cast<const char*>(v.data());
  const bool   ref  = u::valid_scalar(s, v.size());
  const bool   fast = u::valid(s, v.size());
  const size_t bad  = u::first_invalid(s, v.size());
  if (ref != fast || ref != (bad == u::npos)) {
    if (failures < 10) {
      std::printf("mismatch len=%zu ref=%d fast=%d first_invalid=%zu bytes:", v.size(), ref, fast, bad);
      for (unsigned char c : v) std::printf(" %02X", c);
      std::printf("\n");
    }
    ++failures;
  }
}

int main() {
  // Exhaustive 2-byte and 3-byte sequences, placed at offsets 0, 7 and 13 so
  // they cross the 16-byte block boundary in different ways. Prefix and suffix
  // are ASCII so the surrounding bytes do not interfere.
  long count = 0;
  for (int off : {0, 7, 13}) {
    for (int a = 0; a < 256; ++a)
      for (int b = 0; b < 256; ++b) {
        std::vector<unsigned char> v(48, 'x');
        v[off] = (unsigned char)a; v[off + 1] = (unsigned char)b;
        check(v); ++count;
      }
  }
  for (int a = 0; a < 256; ++a) {
    for (int b = 0; b < 256; b += 1) {
      for (int c = 0; c < 256; ++c) {
        std::vector<unsigned char> v(40, 'x');
        v[14] = (unsigned char)a; v[15] = (unsigned char)b; v[16] = (unsigned char)c;
        check(v); ++count;
      }
    }
  }
  // Random byte strings, biased toward lead and continuation bytes.
  std::mt19937 rng(12345);
  const unsigned char pool[] = {0x00, 'a', 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2,
                                0xDF, 0xE0, 0xE1, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF4, 0xF5, 0xFF};
  for (int iter = 0; iter < 3000000; ++iter) {
    const size_t n = rng() % 80;
    std::vector<unsigned char> v(n);
    for (auto& c : v) c = (rng() % 3 == 0) ? (unsigned char)(rng() & 0xFF) : pool[rng() % sizeof(pool)];
    check(v); ++count;
  }
  // Valid multi-byte text, then one corrupted byte at each position.
  const char*  text = "héllo wörld — 日本語 𝄞 ok";
  const size_t tn   = std::strlen(text);
  for (size_t pos = 0; pos < tn; ++pos)
    for (int corrupt : {0x80, 0xC0, 0xFF, 0x00}) {
      std::vector<unsigned char> v(reinterpret_cast<const unsigned char*>(text),
                                   reinterpret_cast<const unsigned char*>(text) + tn);
      v[pos] = (unsigned char)corrupt;
      check(v); ++count;
    }
  // Valid text from random code points (all planes, surrogates excluded), with
  // occasional corruption. Long runs cross the 16-byte blocks in every way.
  for (int iter = 0; iter < 200000; ++iter) {
    std::vector<unsigned char> v;
    const size_t cps = rng() % 40;
    for (size_t k = 0; k < cps; ++k) {
      uint32_t cp;
      do { cp = rng() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
      if (cp < 0x80) v.push_back((unsigned char)cp);
      else if (cp < 0x800) { v.push_back(0xC0 | (cp >> 6)); v.push_back(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) { v.push_back(0xE0 | (cp >> 12)); v.push_back(0x80 | ((cp >> 6) & 0x3F)); v.push_back(0x80 | (cp & 0x3F)); }
      else { v.push_back(0xF0 | (cp >> 18)); v.push_back(0x80 | ((cp >> 12) & 0x3F)); v.push_back(0x80 | ((cp >> 6) & 0x3F)); v.push_back(0x80 | (cp & 0x3F)); }
    }
    check(v); ++count;
    if (!v.empty() && rng() % 2) {  // corrupt one byte, then check again
      v[rng() % v.size()] = pool[rng() % sizeof(pool)];
      check(v); ++count;
    }
    if (!v.empty() && rng() % 4 == 0) {  // truncate inside a sequence
      v.resize(v.size() - 1 - (rng() % std::min<size_t>(3, v.size())));
      check(v); ++count;
    }
  }
  std::printf("cases=%ld failures=%ld\n", count, failures);
  return failures != 0;
}
