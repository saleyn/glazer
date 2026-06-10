// vim:ts=2:sw=2:et
// ---------------------------------------------------------------------------
// Erlang NIF implementation of JSON encoding/decoding with a focus on speed
// and low overhead.
// ---------------------------------------------------------------------------
// Copyright: 2026 Sergey Aleynikov
//
// Implementation inspired by the [glaze](https://github.com/igormunkus/glaze)
// project, but rewritten from scratch to avoid the intermediate generic_u64
// tree.
//
// Decode: hand-rolled recursive-descent parser — zero-copy over raw input,
//         produces Erlang terms in a single pass (no intermediate tree).
// Encode: direct Erlang-term to JSON writer with a stack-allocated output
//         buffer (no intermediate generic_u64 tree).
// ---------------------------------------------------------------------------

#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "fast_float.hpp"
#include "glazer_json_format.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_lltoa.hpp"

// ---------------------------------------------------------------------------
// Dirty-scheduler threshold — inputs larger than this are offloaded to a
// dirty CPU scheduler so they don't block normal scheduler threads.
// Small inputs run inline on a normal scheduler.
// consume_timeslice is not called: dirty schedulers ignore it, and small
// inputs on normal schedulers complete fast enough not to need yielding.
// ---------------------------------------------------------------------------

static constexpr size_t DIRTY_THRESHOLD = 8192;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct DecodeOpts {
  bool         object_as_tuple     = false;
  ERL_NIF_TERM null_term           = 0;
  bool         label_atom          = false;
  bool         label_existing_atom = false;
  bool         dedupe_keys         = false;
};

struct EncodeOpts {
  bool         pretty     = false;
  bool         uescape    = false;
  bool         force_utf8 = false;
  ERL_NIF_TERM null_term  = 0;
};

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

static bool parse_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, DecodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_OBJECT_AS_TUPLE))  opts.object_as_tuple = true;
    else if (enif_is_identical(head, AM_USE_NIL))          opts.null_term       = AM_NIL;
    else if (enif_is_identical(head, AM_DEDUPE_KEYS))      opts.dedupe_keys     = true;
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
      memcpy(b, &digs[n], 2);
      return b + 2;
    }
    return util::lltoa(b, n);
  }

  inline char* i64toa(char* b, int64_t v) { return util::lltoa(b, v); }
}

// ---------------------------------------------------------------------------
// Small inline-capacity buffer for term arrays built while parsing
// arrays/objects — avoids heap allocation for the common case (most JSON
// objects/arrays have only a handful of elements).
// ---------------------------------------------------------------------------

template <size_t N>
struct SmallTermVec {
  ERL_NIF_TERM  m_inline[N];
  ERL_NIF_TERM* m_data = m_inline;
  size_t        m_len  = 0;
  size_t        m_cap  = N;

  ~SmallTermVec() { if (m_data != m_inline) delete[] m_data; }

  void push_back(ERL_NIF_TERM v) {
    if (m_len == m_cap) {
      size_t nc = m_cap * 2;
      ERL_NIF_TERM* nb = new ERL_NIF_TERM[nc];
      memcpy(nb, m_data, m_len * sizeof(ERL_NIF_TERM));
      if (m_data != m_inline) delete[] m_data;
      m_data = nb; m_cap = nc;
    }
    m_data[m_len++] = v;
  }

  ERL_NIF_TERM* data() const { return m_data; }
  size_t        size() const { return m_len;  }
  void          set_size(size_t n) { m_len = n; }
};

// ---------------------------------------------------------------------------
// Output buffer — 4 KB inline, grows to heap
// ---------------------------------------------------------------------------

struct OutBuf {
  static constexpr size_t INLINE = 4096;

  char   m_inline[INLINE];
  char*  m_data;
  size_t m_len;
  size_t m_cap;

  OutBuf() : m_data(m_inline), m_len(0), m_cap(INLINE) {}
  ~OutBuf() { if (m_data != m_inline) free(m_data); }

  void ensure(size_t need) {
    if (m_len + need <= m_cap) return;
    size_t nc = m_cap * 2;
    while (nc < m_len + need) nc *= 2;
    if (m_data == m_inline) {
      // Can't realloc a stack array — first spill to the heap requires a copy.
      std::unique_ptr<char[]> nb = std::unique_ptr<char[]>(static_cast<char*>(malloc(nc)));
      memcpy(nb.get(), m_data, m_len);
      m_data = nb.release();
    } else {
      // May resize in place (no copy) when the allocator can extend the block.
      m_data = static_cast<char*>(realloc(m_data, nc));
    }
    m_cap = nc;
  }

  void push(char c)                  { ensure(1); m_data[m_len++] = c; }
  void push(const char* s, size_t n) { ensure(n); memcpy(m_data + m_len, s, n); m_len += n; }
  void push(std::string_view sv)     { push(sv.data(), sv.size()); }

  std::string_view view() const      { return {m_data, m_len}; }

  operator std::string_view() const  { return view(); }
};

// ---------------------------------------------------------------------------
// Key cache — JSON object keys repeat heavily within a document (e.g. a
// twitter feed has ~13K key occurrences but only ~94 distinct strings).
// Caching the resulting binary term lets repeated keys reuse the same
// already-built ERL_NIF_TERM instead of paying enif_make_new_binary + memcpy
// each time. Linear scan is fine — distinct-key counts are small in practice,
// and a capped size keeps pathological documents (huge unique-key counts)
// from paying scan overhead for no benefit.
// ---------------------------------------------------------------------------

struct KeyCache {
  // Open-addressed, power-of-two-sized table with linear probing. Sized
  // larger than the expected distinct-key count (real documents have ~94
  // distinct keys per the comment above) to keep the load factor low and
  // probe sequences short.
  static constexpr size_t CAP  = 128;
  static constexpr size_t MASK = CAP - 1;

  // Lazily-cleared via an epoch counter rather than zero-initializing the
  // whole array up front: a slot is "live" only if its `epoch` matches the
  // cache's current `m_epoch`. This avoids paying ~3KB of memset on every
  // single decode call (including tiny ones that never touch the cache —
  // see Decoder::m_use_key_cache / KEY_CACHE_MIN_SIZE) merely to construct
  // the Decoder. `m_epoch` is seeded from a process-wide monotonic counter,
  // so leftover garbage from prior stack frames can never coincide with it
  // (it is always strictly less than every epoch handed out so far).
  struct Entry { const char* s; size_t len; uint32_t hash; uint32_t epoch; ERL_NIF_TERM term; };
  Entry    m_entries[CAP]; // intentionally left uninitialized — see m_epoch
  size_t   m_count = 0;
  uint32_t m_epoch;

  static_assert((CAP & MASK) == 0, "CAP must be a power of two");

  static uint32_t next_epoch() {
    static std::atomic<uint32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1; // never 0
  }

  KeyCache() : m_epoch(next_epoch()) {}

  // FNV-1a — cheap, decent distribution, computed once per key and reused
  // for both the lookup and (on a miss) the subsequent insert.
  static uint32_t hash_of(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
      h ^= static_cast<unsigned char>(s[i]);
      h *= 16777619u;
    }
    return h;
  }

  // Returns 0 if not cached or cache is full/bypassed (has_escape).
  // O(1) average: jump straight to the hash's home slot and linearly probe
  // only the (typically very short, given the low load factor) collision
  // chain — comparing the precomputed hash before len/memcmp.
  ERL_NIF_TERM lookup(const char* s, size_t len, uint32_t hash) const {
    for (size_t i = hash & MASK, probes = 0; probes < CAP; ++probes, i = (i + 1) & MASK) {
      const Entry& e = m_entries[i];
      if (e.epoch != m_epoch) return 0; // empty slot — key was never inserted
      if (e.hash == hash && e.len == len && memcmp(e.s, s, len) == 0)
        return e.term;
    }
    return 0;
  }

  void insert(const char* s, size_t len, uint32_t hash, ERL_NIF_TERM term) {
    if (m_count >= CAP) return;
    for (size_t i = hash & MASK;; i = (i + 1) & MASK) {
      if (m_entries[i].epoch != m_epoch) {
        m_entries[i] = {s, len, hash, m_epoch, term};
        ++m_count;
        return;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Zero-copy JSON decoder — parses raw bytes, emits Erlang terms directly
// ---------------------------------------------------------------------------

struct Decoder {
  ErlNifEnv*        m_env;
  const DecodeOpts& m_opts;
  const char*       m_beg;  // start of input (for error reporting)
  const char*       m_p;    // current position
  const char*       m_end;
  KeyCache          m_key_cache;
  bool              m_use_key_cache;

  // Below this input size, documents rarely repeat enough keys to amortize
  // the cache's lookup-scan cost — skip it entirely (helps small payloads
  // like RPC messages, where glazer otherwise loses ground to torque).
  static constexpr size_t KEY_CACHE_MIN_SIZE = 2048;

  Decoder(ErlNifEnv* e, const DecodeOpts& o, const char* data, size_t size)
    : m_env(e), m_opts(o), m_beg(data), m_p(data), m_end(data + size),
      m_use_key_cache(size >= KEY_CACHE_MIN_SIZE) {}

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
  // Returns false on error; sets p past the closing quote.
  // If has_escape is set the caller must unescape before using as binary.
  // Bulk-scans 8 bytes at a time with SWAR; scalar fallback only on the word
  // containing a '"' or '\', then returns to SWAR for the next clean run.
  bool read_string_raw(const char*& begin_out, size_t& len_out, bool& has_escape)
  {
    if (m_p >= m_end || *m_p != '"')
      return false;

    ++m_p;  // skip opening quote
    const char* s = m_p;
    has_escape = false;
    for (;;) {
      // SWAR phase: skip words with no special bytes.
      while (m_p + 8 <= m_end) {
        uint64_t w;
        memcpy(&w, m_p, 8);
        if (has_byte(w, '"') | has_byte(w, '\\')) break;
        m_p += 8;
      }
      // Scalar phase: advance byte-by-byte until we hit '"', '\', or end.
      while (m_p < m_end) {
        char c = *m_p;
        if (c == '"') { begin_out = s; len_out = m_p - s; ++m_p; return true; }
        if (c == '\\') { has_escape = true; ++m_p; if (m_p < m_end) ++m_p; break; }
        ++m_p;
        // If the next char is also not special, return to SWAR.
        if (m_p + 8 <= m_end) {
          uint64_t w; memcpy(&w, m_p, 8);
          if (!(has_byte(w, '"') | has_byte(w, '\\'))) { m_p += 8; break; }
        }
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
  // buf is scratch storage reused across calls.
  ERL_NIF_TERM make_string_term(const char* s, size_t len, bool has_escape, std::string& buf)
  {
    std::string_view sv = has_escape ? unescape(s, len, buf) : std::string_view(s, len);
    return make_binary(m_env, sv);
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
    ERL_NIF_TERM r = glazer::BigInt::decode(m_env, start, m_p);
    return r ? r : (ERL_NIF_TERM)0;
  }

  // ---- core value parser ----
  ERL_NIF_TERM parse_value(std::string& scratch)
  {
    skip_ws();
    if (m_p >= m_end)
      return 0;

    switch (*m_p) {
      case '"': {
        const char* s; size_t len; bool has_escape;
        if (!read_string_raw(s, len, has_escape)) return 0;
        return make_string_term(s, len, has_escape, scratch);
      }

      case '{': return parse_object(scratch);
      case '[': return parse_array(scratch);

      case 't':
        if (m_p + 4 <= m_end && memcmp(m_p, "true", 4) == 0)  { m_p += 4; return AM_TRUE;  } return 0;
      case 'f':
        if (m_p + 5 <= m_end && memcmp(m_p, "false", 5) == 0) { m_p += 5; return AM_FALSE; } return 0;
      case 'n':
        if (m_p + 4 <= m_end && memcmp(m_p, "null", 4) == 0)  { m_p += 4; return m_opts.null_term; } return 0;

      case '-': case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        return parse_number();

      default: return 0;
    }
  }

  ERL_NIF_TERM parse_array(std::string& scratch)
  {
    assert(*m_p == '[');
    ++m_p;
    skip_ws();

    SmallTermVec<16> items;
    if (m_p < m_end && *m_p == ']') { ++m_p; return enif_make_list_from_array(m_env, nullptr, 0); }

    while (m_p < m_end) {
      ERL_NIF_TERM v = parse_value(scratch);
      if (!v) return 0;
      items.push_back(v);
      skip_ws();
      if (m_p >= m_end) return 0;
      if (*m_p == ']') { ++m_p; break; }
      if (*m_p != ',') return 0;
      ++m_p;
    }
    return enif_make_list_from_array(m_env, items.data(), (unsigned)items.size());
  }

  // Build a map from parallel key/value arrays that may contain duplicate
  // keys, keeping the *last* value for each duplicate. enif_make_map_put
  // overwrites on collision, so a single left-to-right pass naturally
  // gives last-value-wins semantics with no explicit key comparison.
  static ERL_NIF_TERM make_map_dedup(ErlNifEnv* env, SmallTermVec<16>& ks, SmallTermVec<16>& vs)
  {
    ERL_NIF_TERM map = enif_make_new_map(env);
    for (size_t i = 0; i < ks.size(); ++i) {
      ERL_NIF_TERM next;
      enif_make_map_put(env, map, ks.data()[i], vs.data()[i], &next);
      map = next;
    }
    return map;
  }

  // Remove earlier duplicate {Key, Val} pairs in-place, keeping each key's
  // *last* occurrence (and its position). O(n^2) but objects are typically
  // small (SmallTermVec inline capacity is 16) so this is cheaper than a
  // hash set for the common case. Used for the object_as_tuple path, which
  // has no map-based shortcut.
  static void dedupe_pairs_last(SmallTermVec<16>& pairs, ErlNifEnv* env)
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
    ++m_p;
    skip_ws();

    if (m_opts.object_as_tuple) {
      SmallTermVec<16> pairs;

      if (m_p < m_end && *m_p == '}') { ++m_p;
        return enif_make_tuple1(m_env, enif_make_list_from_array(m_env, nullptr, 0)); }

      while (m_p < m_end) {
        if (*m_p != '"') return 0;

        const char* ks;
        size_t      kl;
        bool        ke;

        if (!read_string_raw(ks, kl, ke)) return 0;

        auto key = make_key_term(ks, kl, ke, scratch);
        skip_ws();

        if (m_p >= m_end || *m_p != ':') return 0;
        ++m_p;

        auto val = parse_value(scratch);
        if (!val) return 0;

        pairs.push_back(enif_make_tuple2(m_env, key, val));
        skip_ws();

        if (m_p >= m_end) return 0;

        if (*m_p == '}') { ++m_p; break; }
        if (*m_p != ',') return 0;
        ++m_p;
        skip_ws();
      }
      if (m_opts.dedupe_keys)
        dedupe_pairs_last(pairs, m_env);

      auto list = enif_make_list_from_array(m_env, pairs.data(), (unsigned)pairs.size());
      return enif_make_tuple1(m_env, list);
    }

    // Map path
    SmallTermVec<16> ks, vs;

    if (m_p < m_end && *m_p == '}') { ++m_p;
      ERL_NIF_TERM m; enif_make_map_from_arrays(m_env, nullptr, nullptr, 0, &m); return m; }

    while (m_p < m_end) {
      if (*m_p != '"') return 0;

      const char* kstr;
      size_t      klen;
      bool        kesc;

      if (!read_string_raw(kstr, klen, kesc))
        return 0;

      auto key = make_key_term(kstr, klen, kesc, scratch);
      skip_ws();

      if (m_p >= m_end || *m_p != ':') return 0;
      ++m_p;

      auto val = parse_value(scratch);
      if (!val) return 0;

      ks.push_back(key); vs.push_back(val);
      skip_ws();

      if (m_p >= m_end) return 0;
      if (*m_p == '}') { ++m_p; break; }
      if (*m_p != ',') return 0;
      ++m_p;
      skip_ws();
    }

    ERL_NIF_TERM map;
    return enif_make_map_from_arrays(m_env, ks.data(), vs.data(), (unsigned)ks.size(), &map)
         ? map
         : make_map_dedup(m_env, ks, vs); // Dedupe, keeping the last value for each key
  }

  // Always returns {ok, Term} | {error, {parse_error, Msg}}.
  // Raising vs. non-raising behaviour is the Erlang caller's responsibility.
  ERL_NIF_TERM decode(const char* data, size_t size)
  {
    m_p = data; m_end = data + size; m_beg = data;
    std::string scratch;
    ERL_NIF_TERM result = parse_value(scratch);
    if (!result) {
      std::string msg = "JSON parse error at offset " + std::to_string(m_p - m_beg);
      return enif_make_tuple2(m_env, AM_ERROR,
        enif_make_tuple2(m_env, AM_PARSE_ERROR, make_binary(m_env, msg)));
    }
    skip_ws();
    return enif_make_tuple2(m_env, AM_OK, result);
  }
};

// ---------------------------------------------------------------------------
// Value-boundary scanner — finds where the next complete top-level JSON value
// ends in a (possibly partial) buffer, without building any Erlang terms.
//
// This underpins incremental/streaming decode: callers buffer raw bytes,
// repeatedly ask the scanner "is there a complete value yet?", and once one
// is found, slice it off and hand it to the existing whole-buffer `decode`.
// The scanner never allocates and never inspects string contents beyond
// quote/escape bytes, so it stays cheap even on huge inputs.
// ---------------------------------------------------------------------------

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

// Write a JSON "\uXXXX" escape (6 bytes, lowercase hex) for a code unit
// cu <= 0xFFFF directly into dst, without going through snprintf.
static inline void write_uescape(char* dst, uint32_t cu)
{
  static constexpr char HEX[] = "0123456789abcdef";
  dst[0] = '\\';
  dst[1] = 'u';
  dst[2] = HEX[(cu >> 12) & 0xF];
  dst[3] = HEX[(cu >>  8) & 0xF];
  dst[4] = HEX[(cu >>  4) & 0xF];
  dst[5] = HEX[ cu        & 0xF];
}

// JSON-escape a UTF-8 byte sequence into out.
// Fast path: scan for runs of bytes that need no escaping and bulk-copy them;
// only fall into the per-byte switch for the rare escape characters.
static void json_escape_string(std::string_view sv, OutBuf& out)
{
  out.push('"');
  const char* p   = sv.data();
  const char* end = p + sv.size();
  const char* run = p;

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
        char esc[6]; write_uescape(esc, c);
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

// Emit a single Unicode code point as a JSON \uXXXX escape (or a surrogate
// pair for code points beyond the BMP).
static void push_uescape(OutBuf& out, uint32_t cp)
{
  char esc[6];
  if (cp <= 0xFFFF) {
    write_uescape(esc, cp);
    out.push(esc, 6);
  } else {
    cp -= 0x10000;
    uint32_t hi = 0xD800 + (cp >> 10);
    uint32_t lo = 0xDC00 + (cp & 0x3FF);
    write_uescape(esc, hi); out.push(esc, 6);
    write_uescape(esc, lo); out.push(esc, 6);
  }
}

// Decode one UTF-8 sequence starting at p (p < end). Returns the code point
// and advances p past the sequence. On invalid/truncated input, returns the
// Unicode replacement character (U+FFFD) and advances p by one byte.
static uint32_t decode_utf8(const char*& p, const char* end)
{
  auto c    = (unsigned char)*p;
  auto cont = [&](const char* q) {
    return q < end && ((unsigned char)*q & 0xC0) == 0x80;
  };

  if (c < 0x80) { ++p; return c; }

  if ((c & 0xE0) == 0xC0 && cont(p+1)) {
    uint32_t cp = (uint32_t(c & 0x1F) << 6) | (uint32_t((unsigned char)p[1]) & 0x3F);
    p += 2;
    return cp >= 0x80 ? cp : 0xFFFD;
  }
  if ((c & 0xF0) == 0xE0 && cont(p+1) && cont(p+2)) {
    auto cp = (uint32_t(c & 0x0F) << 12)
            | (uint32_t((unsigned char)p[1] & 0x3F) << 6)
            |  uint32_t((unsigned char)p[2] & 0x3F);
    p += 3;
    return (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) ? cp : 0xFFFD;
  }
  if ((c & 0xF8) == 0xF0 && cont(p+1) && cont(p+2) && cont(p+3)) {
    auto cp = (uint32_t(c & 0x07) << 18)
            | (uint32_t((unsigned char)p[1] & 0x3F) << 12)
            | (uint32_t((unsigned char)p[2] & 0x3F) << 6)
            |  uint32_t((unsigned char)p[3] & 0x3F);
    p += 4;
    return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : 0xFFFD;
  }

  ++p;
  return 0xFFFD;
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

    if (c < 0x80) {
      if (NEEDS_ESCAPE_TAB[c]) {
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
      } else {
        ++p;
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

struct Encoder {
  ErlNifEnv*        m_env;
  const EncodeOpts& m_opts;
  OutBuf&           m_out;
  char              m_atom_buf[256]; // scratch for atom → string_view

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

      case ERL_NIF_TERM_TYPE_INTEGER: {
        ErlNifSInt64 i;
        if (enif_get_int64(m_env, term, &i)) {
          char buf[22]; char* e = lltoa_impl::i64toa(buf, i);
          m_out.push(buf, e - buf);
          return true;
        }
        ErlNifUInt64 u;
        if (enif_get_uint64(m_env, term, &u)) {
          char buf[21]; char* e = util::lltoa(buf, u);
          m_out.push(buf, e - buf);
          return true;
        }
        // bigint — doesn't fit in 64 bits
        auto s = glazer::BigInt::encode(m_env, term);
        if (!s.empty()) { m_out.push(s); return true; }
        return false;
      }

      case ERL_NIF_TERM_TYPE_MAP: {
        m_out.push('{');
        ErlNifMapIterator iter;
        if (!enif_map_iterator_create(m_env, term, &iter, ERL_NIF_MAP_ITERATOR_FIRST))
          return false;
        ERL_NIF_TERM k, v;
        bool first = true;
        while (enif_map_iterator_get_pair(m_env, &iter, &k, &v)) {
          if (!first) m_out.push(',');
          first = false;
          if (!encode_key(k)) { enif_map_iterator_destroy(m_env, &iter); return false; }
          m_out.push(':');
          if (!encode(v))     { enif_map_iterator_destroy(m_env, &iter); return false; }
          enif_map_iterator_next(m_env, &iter);
        }
        enif_map_iterator_destroy(m_env, &iter);
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
          if (!encode(h)) return false;
        }
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
        if (!atom_to_sv(m_env, term, m_atom_buf, sizeof(m_atom_buf), sv)) return false;
        escape_string(sv);
        return true;
      }

      case ERL_NIF_TERM_TYPE_FLOAT: {
        double d;
        if (!enif_get_double(m_env, term, &d)) return false;
        if (!std::isfinite(d)) { m_out.push("null", 4); return true; }
        char buf[32];
        auto [e, ec] = std::to_chars(buf, buf+32, d, std::chars_format::general);
        if (ec == std::errc{}) {
          bool has_dot = false;
          for (char* p = buf; p < e; ++p) if (*p == '.' || *p == 'e' || *p == 'E') { has_dot = true; break; }
          m_out.push(buf, e - buf);
          if (!has_dot) m_out.push(".0", 2);
        } else {
          int n = snprintf(buf, sizeof(buf), "%.17g", d);
          m_out.push(buf, n);
        }
        return true;
      }

      case ERL_NIF_TERM_TYPE_TUPLE: {
        // {[{K,V}...]} proplist → object
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(m_env, term, &arity, &tp);
        if (arity == 1 && enif_is_list(m_env, tp[0])) {
          m_out.push('{');
          ERL_NIF_TERM h, t = tp[0];
          bool first = true;
          while (enif_get_list_cell(m_env, t, &h, &t)) {
            int pa; const ERL_NIF_TERM* pp;
            if (!enif_get_tuple(m_env, h, &pa, &pp) || pa != 2) return false;
            if (!first) m_out.push(',');
            first = false;
            if (!encode_key(pp[0])) return false;
            m_out.push(':');
            if (!encode(pp[1])) return false;
          }
          m_out.push('}');
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
};

// ---------------------------------------------------------------------------
// NIF: decode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  DecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  Decoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size);
  return dec.decode(reinterpret_cast<const char*>(bin.data), bin.size);
}

static ERL_NIF_TERM nif_decode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_decode(env, bin, argc, argv);
}

static ERL_NIF_TERM nif_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[2];
  if (enif_inspect_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_decode(env, bin, argc, argv);
    sched_argv[0] = argv[0];
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_decode(env, bin, argc, argv);
    sched_argv[0] = enif_make_binary(env, &bin);
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_decode_dirty, 2, sched_argv);
}

// ---------------------------------------------------------------------------
// NIF: scan — locate the end of the next complete top-level JSON value.
//
//   scan(Bin)            -> {complete, EndOffset} | {incomplete, State} | {error, Reason}
//   scan(Bin, State)     -> resumes scanning Bin (the *unconsumed remainder*
//                           from a prior {incomplete, State}, with new bytes
//                           appended) using the given State
//
// `EndOffset` is the byte offset, into `Bin`, one past the end of the value —
// i.e. binary:part(Bin, 0, EndOffset) is the complete value, and the rest is
// left over for the next call.
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_scan(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);

  ScanState st = ScanState::initial();
  if (argc == 2 && !scan_state_from_term(env, argv[1], st))
    return enif_make_badarg(env);

  const char* data = reinterpret_cast<const char*>(bin.data);
  Scanner scanner(data, bin.size, st.pos);

  const char* value_end = nullptr;
  if (scanner.scan(st, value_end)) {
    size_t offset = static_cast<size_t>(value_end - data);
    return enif_make_tuple2(env, AM_COMPLETE, enif_make_uint64(env, offset));
  }

  return enif_make_tuple2(env, AM_INCOMPLETE, scan_state_to_term(env, st));
}

// ---------------------------------------------------------------------------
// NIF: encode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  EncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  Encoder enc{env, opts, out};
  if (!enc.encode(argv[0]))
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        make_binary(env, std::string_view("cannot encode term to JSON"))));

  if (!opts.pretty)
    return make_binary(env, out.view());

  auto pretty_out = glz::prettify_json(out.view());
  return make_binary(env, pretty_out);
}

static ERL_NIF_TERM nif_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_encode(env, argc, argv);
}

static ERL_NIF_TERM nif_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) return enif_make_badarg(env);

  // Output size is unknown upfront; use input binary size as a proxy.
  // For non-binary terms (atoms, integers, short lists) always run inline.
  ErlNifBinary bin;
  if (enif_inspect_binary(env, argv[0], &bin) && bin.size >= DIRTY_THRESHOLD) {
    ERL_NIF_TERM sched_argv[2] = { argv[0], argc > 1 ? argv[1] : enif_make_list(env, 0) };
    return enif_schedule_nif(env, "glazer_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                             nif_encode_dirty, 2, sched_argv);
  }
  return do_encode(env, argc, argv);
}

// ---------------------------------------------------------------------------
// NIF: minify / prettify
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_minify(ErlNifEnv* env, const ErlNifBinary& bin)
{
  std::string in(reinterpret_cast<const char*>(bin.data), bin.size);
  return make_binary(env, glz::minify_json(in));
}

static ERL_NIF_TERM nif_minify_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_minify(env, bin);
}

static ERL_NIF_TERM nif_minify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[1];
  if (enif_inspect_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_minify(env, bin);
    sched_argv[0] = argv[0];
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_minify(env, bin);
    sched_argv[0] = enif_make_binary(env, &bin);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_minify", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_minify_dirty, 1, sched_argv);
}

static ERL_NIF_TERM do_prettify(ErlNifEnv* env, const ErlNifBinary& bin)
{
  std::string_view in(reinterpret_cast<const char*>(bin.data), bin.size);
  return make_binary(env, glz::prettify_json(in));
}

static ERL_NIF_TERM nif_prettify_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_prettify(env, bin);
}

static ERL_NIF_TERM nif_prettify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[1];
  if (enif_inspect_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_prettify(env, bin);
    sched_argv[0] = argv[0];
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_prettify(env, bin);
    sched_argv[0] = enif_make_binary(env, &bin);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_prettify", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_prettify_dirty, 1, sched_argv);
}

// ---------------------------------------------------------------------------
// NIF: encode_bigint / decode_bigint
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_encode_bigint(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  ErlNifSInt64 val;
  if (enif_get_int64(env, argv[0], &val)) {
    char buf[22]; char* e = lltoa_impl::i64toa(buf, val);
    return enif_make_tuple2(env, AM_OK, make_binary(env, std::string_view(buf, e - buf)));
  }
  auto s = glazer::BigInt::encode(env, argv[0]);
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
  auto r = glazer::BigInt::decode(env,
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
  {"try_decode",    1, nif_decode,        0},
  {"try_decode",    2, nif_decode,        0},
  {"scan",          1, nif_scan,          0},
  {"scan",          2, nif_scan,          0},
  {"encode",        1, nif_encode,        0},
  {"encode",        2, nif_encode,        0},
  {"minify",        1, nif_minify,        0},
  {"prettify",      1, nif_prettify,      0},
  {"encode_bigint", 1, nif_encode_bigint, 0},
  {"decode_bigint", 1, nif_decode_bigint, 0},
};

ERL_NIF_INIT(glazer, nif_funcs, nif_load, NULL, NULL, NULL)
