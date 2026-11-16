// C++ port of the parts of sonic-rs's push parser that differ from glazer's
// decoder, for measuring which algorithms account for the difference. Parse
// only, with a counting visitor (like json_parse_bench.cpp).
//
// Ported from deps/torque/native/sonic-rs/src (parser.rs, reader.rs,
// util/arch/x86_64.rs, sonic-number's SWAR integer path):
//   - skip_space: two-byte fast path, then a non-space bitmap for the 64-byte
//     chunk, reused by later calls until the chunk is consumed (PORT_BITMAP)
//   - string scan: 32-byte blocks with quote, backslash and control-character
//     masks, quote-first fast path (PORT_SIMD_STR; scalar when off)
//   - integers: 8-digit SWAR batches (PORT_SWAR; byte loop when off)
//   - object/array loops that take the next token's first byte from
//     skip_space and dispatch on it, with the compact ':' fast path
//
// Not ported: escape decoding (escapes are skipped, not unescaped), UTF-8
// validation, sonic's float algorithm (floats use glazer's fast_float), and
// the lazy/skip paths. Those are the parts not on the path a visitor parse of
// these files exercises, except escapes, which are a known gap.
//
// Build (ablations are compile-time):
//   g++ -std=c++2b -O3 -march=native -DNDEBUG -Ic_src \
//       -DPORT_BITMAP=1 -DPORT_SIMD_STR=1 -DPORT_SWAR=1 \
//       c_src/tools/sonic_port_bench.cpp -o sonic_port_bench
// Run: PARSE_ITERS=N ./sonic_port_bench FILE...   (or timed, without PARSE_ITERS)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include <immintrin.h>

#include "vendors/glazer_atof.hpp"

#ifndef PORT_BITMAP
#define PORT_BITMAP 1
#endif
#ifndef PORT_SIMD_STR
#define PORT_SIMD_STR 1
#endif
#ifndef PORT_SWAR
#define PORT_SWAR 1
#endif

namespace port {

// Counting visitor: every callback succeeds and counts one token.
struct Counter {
  uint64_t n = 0;
  bool dom_start() { return true; }
  bool dom_end() { return true; }
  bool null() { ++n; return true; }
  bool boolean(bool) { ++n; return true; }
  bool u64(uint64_t) { ++n; return true; }
  bool i64(int64_t) { ++n; return true; }
  bool f64(double) { ++n; return true; }
  bool str(const char*, size_t) { ++n; return true; }
  bool key(const char*, size_t) { ++n; return true; }
  bool obj_start() { ++n; return true; }
  bool obj_end(size_t) { return true; }
  bool arr_start() { ++n; return true; }
  bool arr_end(size_t) { return true; }
};

constexpr int MAX_DEPTH = 128;

static inline bool is_ws(unsigned c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Bit i is set when byte i of the 64 bytes at q is not JSON whitespace.
static inline uint64_t nonspace64(const char* q) {
#if defined(__AVX2__)
  const __m256i sp = _mm256_set1_epi8(' ');
  const __m256i tb = _mm256_set1_epi8('\t');
  const __m256i cr = _mm256_set1_epi8('\r');
  const __m256i lf = _mm256_set1_epi8('\n');
  auto ws = [&](__m256i x) {
    return _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(x, sp), _mm256_cmpeq_epi8(x, tb)),
                           _mm256_or_si256(_mm256_cmpeq_epi8(x, cr), _mm256_cmpeq_epi8(x, lf)));
  };
  __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q));
  __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q + 32));
  uint64_t lo = static_cast<uint32_t>(_mm256_movemask_epi8(ws(a)));
  uint64_t hi = static_cast<uint32_t>(_mm256_movemask_epi8(ws(b)));
  return ~(lo | (hi << 32));
#else
  uint64_t m = 0;
  for (int i = 0; i < 64; ++i)
    if (!is_ws(static_cast<unsigned char>(q[i]))) m |= 1ull << i;
  return m;
#endif
}

// 8 ASCII digits at p, as sonic-number's swar.rs checks them.
static inline bool is8digits(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  uint64_t a = v + 0x4646464646464646ull;
  uint64_t b = v - 0x3030303030303030ull;
  return ((a | b) & 0x8080808080808080ull) == 0;
}
static inline uint64_t parse8digits(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  v -= 0x3030303030303030ull;
  v = (v * 10 + (v >> 8)) & 0x00FF00FF00FF00FFull;
  v = (v * 100 + (v >> 16)) & 0x0000FFFF0000FFFFull;
  v = v * 10000 + (v >> 32);
  return v & 0xFFFFFFFFull;
}

template <class V>
struct Parser {
  const char* const beg;
  const char* p;
  const char* const end;
  V& v;
  uint64_t  nospace_bits = 0;  // non-space bitmap of the last 64-byte chunk
  // Offset from beg of that chunk. Starts at -64 so no lookup can hit the empty
  // cache before the first chunk is scanned (0 would treat bytes 0..63 as blank).
  intptr_t  nospace_start = -64;

  Parser(const char* b, size_t n, V& vis) : beg(b), p(b), end(b + n), v(vis) {}

  // Next non-whitespace byte, consumed; -1 at end of input.
  int skip_space() {
    if (p < end) { unsigned c = static_cast<unsigned char>(*p++); if (!is_ws(c)) return static_cast<int>(c); }
    if (p < end) { unsigned c = static_cast<unsigned char>(*p++); if (!is_ws(c)) return static_cast<int>(c); }
#if PORT_BITMAP
    {
      intptr_t off = (p - beg) - nospace_start;
      if (static_cast<uintptr_t>(off) < 64) {
        uint64_t m = nospace_bits & (~0ull << off);
        if (m) {
          const char* q = beg + nospace_start + __builtin_ctzll(m);
          p = q + 1;
          return static_cast<unsigned char>(*q);
        }
        p = beg + nospace_start + 64;
      }
    }
#endif
    while (end - p >= 64) {
      uint64_t m = nonspace64(p);
      if (m) {
#if PORT_BITMAP
        nospace_bits = m;
        nospace_start = p - beg;
#endif
        const char* q = p + __builtin_ctzll(m);
        p = q + 1;
        return static_cast<unsigned char>(*q);
      }
      p += 64;
    }
    while (p < end) {
      unsigned c = static_cast<unsigned char>(*p++);
      if (!is_ws(c)) return static_cast<int>(c);
    }
    return -1;
  }

  // p is just after the opening quote. Emits the string and consumes the closing quote.
  bool string_body(bool key) {
    const char* s0 = p;
    auto emit = [&](const char* s, size_t len) { key ? v.key(s, len) : v.str(s, len); };
#if PORT_SIMD_STR
    {
      const __m256i vq = _mm256_set1_epi8('"');
      const __m256i vb = _mm256_set1_epi8('\\');
      const __m256i vc = _mm256_set1_epi8(0x1f);
      while (end - p >= 32) {
        __m256i x  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        uint32_t qb = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, vq)));
        uint32_t bb = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, vb)));
        uint32_t ub = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_min_epu8(x, vc), x)));  // byte <= 0x1f
        if (qb | bb | ub) {
          unsigned qi = qb ? __builtin_ctz(qb) : 32u;
          unsigned bi = bb ? __builtin_ctz(bb) : 32u;
          unsigned ui = ub ? __builtin_ctz(ub) : 32u;
          if (qi < bi && qi < ui) {  // quote first: the fast path
            emit(s0, static_cast<size_t>(p + qi - s0));
            p += qi + 1;
            return true;
          }
          if (ui < bi) return false;  // control character before any escape or quote
          p += bi;                    // backslash first: finish in the scalar loop
          break;
        }
        p += 32;
      }
    }
#endif
    while (p < end) {
      unsigned c = static_cast<unsigned char>(*p);
      if (c == '"') {
        emit(s0, static_cast<size_t>(p - s0));
        ++p;
        return true;
      }
      if (c == '\\') {
        if (end - p < 2) return false;
        p += 2;
        continue;
      }
      if (c < 0x20) return false;
      ++p;
    }
    return false;
  }

  // p is just after the first character of a number (already consumed).
  bool number(unsigned first) {
    const char* start = p - 1;
    bool neg = first == '-';
    const char* dp = start + (neg ? 1 : 0);
    const char* q = dp;
#if PORT_SWAR
    uint64_t acc = 0;
    if (end - q >= 8 && is8digits(q)) {
      acc = parse8digits(q);
      q += 8;
      if (end - q >= 8 && is8digits(q)) {
        acc = acc * 100000000ull + parse8digits(q);
        q += 8;
      }
    }
#else
    uint64_t acc = 0;
#endif
    while (q < end && static_cast<unsigned>(*q - '0') <= 9) {
      acc = acc * 10 + static_cast<unsigned>(*q - '0');
      ++q;
    }
    bool isfloat = false;
    if (q < end && (*q == '.' || *q == 'e' || *q == 'E')) {
      isfloat = true;
      if (*q == '.') {
        ++q;
        while (q < end && static_cast<unsigned>(*q - '0') <= 9) ++q;
      }
      if (q < end && (*q == 'e' || *q == 'E')) {
        ++q;
        if (q < end && (*q == '+' || *q == '-')) ++q;
        while (q < end && static_cast<unsigned>(*q - '0') <= 9) ++q;
      }
    }
    size_t nd = static_cast<size_t>(q - dp);
    if (isfloat || nd == 0 || nd > 18) {
      double d = 0;
      auto r = glz::fp::from_chars(start, q, d);
      if (r.ec != std::errc{}) return false;
      v.f64(d);
    } else if (neg) {
      v.i64(-static_cast<int64_t>(acc));
    } else {
      v.u64(acc);
    }
    p = q;
    return true;
  }

  bool literal(unsigned c) {
    const char* rest = c == 't' ? "rue" : c == 'f' ? "alse" : "ull";
    size_t n = c == 'f' ? 4 : 3;
    if (static_cast<size_t>(end - p) < n || std::memcmp(p, rest, n) != 0) return false;
    p += n;
    if (c == 'n') v.null();
    else v.boolean(c == 't');
    return true;
  }

  bool colon() {
    if (p < end && *p == ':') { ++p; return true; }  // compact fast path
    return skip_space() == ':';
  }

  bool dispatch(int c, int depth) {
    switch (c) {
      case '-': case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        return number(static_cast<unsigned>(c));
      case '"': return string_body(false);
      case '{': return object(depth);
      case '[': return array(depth);
      case 't': case 'f': case 'n': return literal(static_cast<unsigned>(c));
      default: return false;
    }
  }

  bool object(int depth) {
    if (depth >= MAX_DEPTH) return false;
    v.obj_start();
    int c = skip_space();
    if (c == '}') { v.obj_end(0); return true; }
    if (c != '"') return false;
    size_t count = 0;
    for (;;) {
      if (!string_body(true)) return false;
      if (!colon()) return false;
      if (!dispatch(skip_space(), depth + 1)) return false;
      ++count;
      c = skip_space();
      if (c == '}') { v.obj_end(count); return true; }
      if (c != ',') return false;
      if (skip_space() != '"') return false;
    }
  }

  bool array(int depth) {
    if (depth >= MAX_DEPTH) return false;
    v.arr_start();
    int c = skip_space();
    if (c == ']') { v.arr_end(0); return true; }
    size_t count = 0;
    for (;;) {
      if (!dispatch(c, depth + 1)) return false;
      ++count;
      c = skip_space();
      if (c == ']') { v.arr_end(count); return true; }
      if (c != ',') return false;
      c = skip_space();
    }
  }

  bool parse() {
    v.dom_start();
    if (!dispatch(skip_space(), 0)) return false;
    v.dom_end();
    return skip_space() < 0;  // trailing content is an error
  }
};

}  // namespace port

static std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(1); }
  return std::string(std::istreambuf_iterator<char>(in), {});
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s FILE...\n", argv[0]); return 1; }
  const char* fixed = std::getenv("PARSE_ITERS");
  std::printf("config: bitmap=%d simd_str=%d swar=%d\n", PORT_BITMAP, PORT_SIMD_STR, PORT_SWAR);

  for (int i = 1; i < argc; ++i) {
    std::string data = read_file(argv[i]);
    auto parse_once = [&](uint64_t& n) {
      port::Counter c;
      port::Parser<port::Counter> ps(data.data(), data.size(), c);
      bool ok = ps.parse();
      n = c.n;
      return ok;
    };

    uint64_t tokens = 0;
    if (!parse_once(tokens)) { std::fprintf(stderr, "%s: parse failed\n", argv[i]); return 1; }

    if (fixed) {
      int n = std::atoi(fixed);
      for (int k = 0; k < n; ++k) { uint64_t t; parse_once(t); }
      std::printf("%-28s %d parses, %llu tokens\n", argv[i], n, (unsigned long long)tokens);
      continue;
    }

    auto t0 = std::chrono::steady_clock::now();
    int probe = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)) {
      uint64_t t; parse_once(t);
      ++probe;
    }
    int n = std::max(1, probe * 10);
    double best_us = 1e300;
    for (int round = 0; round < 7; ++round) {
      auto start = std::chrono::steady_clock::now();
      for (int k = 0; k < n; ++k) { uint64_t t; parse_once(t); }
      auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
      best_us = std::min(best_us, us / n);
    }
    std::printf("%-28s %12.2f us/parse  (%zu bytes, %llu tokens)\n", argv[i], best_us, data.size(),
                (unsigned long long)tokens);
  }
  return 0;
}
