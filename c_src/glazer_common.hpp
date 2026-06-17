// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Shared utilities used by both the JSON and YAML decoders/encoders:
// growable term/byte buffers, the object-key cache, and UTF-8/atom helpers.
//-----------------------------------------------------------------------------
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <erl_nif.h>
#if defined(__SSE2__)
#  include <immintrin.h>
#endif
#if defined(__ARM_NEON__)
#  include <arm_neon.h>
#endif

namespace glz {

// Calculate `a` raised to the power of `b`.
template <typename T>
inline T power(T a, size_t b) {
  if (a == 0) return 0;

  T result = 1;
  for (; b > 0; b >>= 1) {
    if (b & 1) result *= a; // If b is odd, multiply the base with the result
    a *= a;
  }
  return result;
}

//-----------------------------------------------------------------------------
// Reduction-count bookkeeping for NIFs that run on a normal (non-dirty)
// scheduler. Without this, the BEAM charges a NIF call a flat 1 reduction
// regardless of how much CPU it actually used, which can let
// CPU-heavy-but-inline calls starve other runnable processes on the same
// scheduler. enif_consume_timeslice(env, percent) tells the scheduler what
// percentage of a timeslice (1-100) was consumed so its reduction
// accounting reflects the real work done before returning to Erlang.
//-----------------------------------------------------------------------------

static constexpr size_t BYTES_PER_REDUCTION = 20;
static constexpr size_t REDUCTION_COUNT     = 4000;

// Report the percentage of a timeslice consumed while processing `bytes`
// bytes, so the scheduler updates the process's reduction count instead of
// charging a flat 1 reduction for the NIF call. The return value of
// enif_consume_timeslice (1 if the process should yield/be preempted, 0
// otherwise) is normally used to drive cooperative scheduling for NIFs that
// process work in chunks across multiple calls. Here it is intentionally
// ignored: work that's long enough to need preemption is offloaded to a
// dirty scheduler instead, so this function is only reached for inline
// (small, sub-DIRTY_THRESHOLD) calls, where it serves purely to keep the
// reduction count accurate.
inline void update_reduction_count([[maybe_unused]] ErlNifEnv* env, [[maybe_unused]] size_t bytes) {
#if ERL_NIF_MAJOR_VERSION > 2 || (ERL_NIF_MAJOR_VERSION == 2 && ERL_NIF_MINOR_VERSION >= 4)
  size_t reds = bytes / BYTES_PER_REDUCTION;
  int percent = static_cast<int>(reds * 100 / REDUCTION_COUNT);
  if (percent < 1)   percent = 1;
  if (percent > 100) percent = 100;
  (void)enif_consume_timeslice(env, percent);
#endif
}

//-----------------------------------------------------------------------------
// Small inline-capacity buffer for term arrays built while parsing
// arrays/objects — avoids heap allocation for the common case (most
// containers have only a handful of elements).
//-----------------------------------------------------------------------------

template <size_t N>
struct SmallTermVec {
  ERL_NIF_TERM  m_inline[N];
  ERL_NIF_TERM* m_data = m_inline;
  size_t        m_len  = 0;
  size_t        m_cap  = N;

  ~SmallTermVec() { if (m_data != m_inline) delete[] m_data; }

  void push_back(ERL_NIF_TERM v) {
    if (m_len == m_cap) [[unlikely]] {
      size_t nc = m_cap * 2;
      ERL_NIF_TERM* nb = new ERL_NIF_TERM[nc];
      memcpy(nb, m_data, m_len * sizeof(ERL_NIF_TERM));
      if (m_data != m_inline) delete[] m_data;
      m_data = nb; m_cap = nc;
    }
    m_data[m_len++] = v;
  }

  ERL_NIF_TERM operator[](size_t i) const { return m_data[i]; }

  const ERL_NIF_TERM* begin()       const { return m_data; }
  const ERL_NIF_TERM* end()         const { return m_data + m_len; }

  ERL_NIF_TERM*       begin()             { return m_data; }
  ERL_NIF_TERM*       end()               { return m_data + m_len; }

  ERL_NIF_TERM*       data()        const { return m_data; }
  size_t              size()        const { return m_len;  }
  void                set_size(size_t n)  { m_len = n; }

  ERL_NIF_TERM  to_erl_list(ErlNifEnv* env) const {
    return enif_make_list_from_array(env, m_data, unsigned(m_len));
  }

  ERL_NIF_TERM  to_erl_tuple(ErlNifEnv* env) const {
    return enif_make_tuple_from_array(env, m_data, unsigned(m_len));
  }

  // `this` holds values, `keys` holds the parallel array of keys.
  // Returns 0 on error (i.e. duplicate keys) or ERL_NIF_TERM on success.
  template <bool Dedupe = false, typename T = SmallTermVec<16>>
  ERL_NIF_TERM to_erl_map(ErlNifEnv* env, const T& keys) const {
    auto n = std::min(keys.size(), m_len);
    ERL_NIF_TERM map;
    if (!enif_make_map_from_arrays(env, keys.data(), m_data, unsigned(n), &map)) [[unlikely]]
      map = 0;

    if (Dedupe && !map) {
      // Dedupe, keeping last value for duplicate keys.
      map = enif_make_new_map(env);
      for (auto p = m_data, q = keys.data(), e = p+n; p != e; ++p, ++q) {
        ERL_NIF_TERM next;
        enif_make_map_put(env, map, *q, *p, &next);
        map = next;
      }
    }

    return map;
  }
};

//-----------------------------------------------------------------------------
// Zero-copy span term — shared by the JSON, YAML, and CSV decoders.
//
// Returns a sub-binary referencing `[beg, beg+end)` within `input_bin` when
// `copy_strings` is false (the default) and `sv` actually lies within
// `[beg, end)`: no allocation, but `input_bin` stays alive as long as any
// sub-binary referencing it is reachable. Falls back to copying — via
// make_binary — when `copy_strings` is true, or when `sv` does not point
// into the `[beg, end)` span (e.g. a scratch buffer used to fold/unescape
// multi-line scalars): callers may pass either a raw input slice or a
// locally-built buffer without needing to track which case applies.
//-----------------------------------------------------------------------------

inline ERL_NIF_TERM make_span_term(ErlNifEnv* env, ERL_NIF_TERM input_bin, const char* beg,
                                    const char* end, std::string_view sv, bool copy_strings)
{
  if (!copy_strings && sv.data() >= beg && sv.data() + sv.size() <= end)
    return enif_make_sub_binary(env, input_bin, static_cast<size_t>(sv.data() - beg), sv.size());
  auto [term, p] = make_binary(env, sv.size());
  memcpy(p, sv.data(), sv.size());
  return term;
}

//-----------------------------------------------------------------------------
// Output buffer — 4 KB inline, grows to heap
//-----------------------------------------------------------------------------

struct OutBuf {
  static constexpr size_t INLINE = 4096;

  char   m_inline[INLINE];
  char*  m_data;
  size_t m_len;
  size_t m_cap;

  OutBuf() : m_data(m_inline), m_len(0), m_cap(INLINE) {}
  ~OutBuf() { if (m_data != m_inline) free(m_data); }

  void ensure(size_t need) {
    if (m_len + need <= m_cap) [[likely]] return;
    size_t nc = m_cap * 2;
    while (nc < m_len + need) nc *= 2;
    if (m_data == m_inline) [[unlikely]] {
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

//-----------------------------------------------------------------------------
// Key cache — object/mapping keys repeat heavily within a document (e.g. a
// twitter feed has ~13K key occurrences but only ~94 distinct strings).
// Caching the resulting binary term lets repeated keys reuse the same
// already-built ERL_NIF_TERM instead of paying enif_make_new_binary + memcpy
// each time. Linear scan is fine — distinct-key counts are small in practice,
// and a capped size keeps pathological documents (huge unique-key counts)
// from paying scan overhead for no benefit.
//-----------------------------------------------------------------------------

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
  // see KEY_CACHE_MIN_SIZE) merely to construct the cache. `m_epoch` is
  // seeded from a process-wide monotonic counter, so leftover garbage from
  // prior stack frames can never coincide with it (it is always strictly
  // less than every epoch handed out so far).
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
      if (e.epoch != m_epoch) [[unlikely]] return 0; // empty slot — key was never inserted
      if (e.hash == hash && e.len == len && memcmp(e.s, s, len) == 0) [[likely]]
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

//-----------------------------------------------------------------------------
// RAII wrapper for ErlNifMapIterator — automatically calls
// enif_map_iterator_destroy on scope exit.
//
// Only constructible via the static factory:
//   auto iter = MapIterator::create(env, map);   // std::optional<MapIterator>
//   if (!iter) return false;
//   while (iter->get_pair(&k, &v)) { ...; iter->next(); }
//-----------------------------------------------------------------------------

struct MapIterator {
  MapIterator(const MapIterator&)            = delete;
  MapIterator& operator=(const MapIterator&) = delete;

  MapIterator(MapIterator&& o) noexcept
    : m_env(o.m_env), m_iter(o.m_iter), m_live(o.m_live)
  {
    o.m_live = false;
  }

  MapIterator& operator=(MapIterator&& o) noexcept
  {
    if (this != &o) {
      destroy();
      m_env  = o.m_env;
      m_iter = o.m_iter;
      m_live = o.m_live;
      o.m_live = false;
    }
    return *this;
  }

  ~MapIterator() { destroy(); }

  static std::optional<MapIterator> create(
    ErlNifEnv* env, ERL_NIF_TERM map,
    ErlNifMapIteratorEntry entry = ERL_NIF_MAP_ITERATOR_FIRST)
  {
    MapIterator it;
    if (!enif_map_iterator_create(env, map, &it.m_iter, entry))
      return std::nullopt;
    it.m_env  = env;
    it.m_live = true;
    return it;
  }

  bool get_pair(ERL_NIF_TERM* key, ERL_NIF_TERM* val)
  {
    return enif_map_iterator_get_pair(m_env, &m_iter, key, val);
  }

  void next() { enif_map_iterator_next(m_env, &m_iter); }

private:
  ErlNifEnv*        m_env;
  ErlNifMapIterator m_iter;
  bool              m_live{false};

  MapIterator() = default;

  void destroy()
  {
    if (m_live) {
      enif_map_iterator_destroy(m_env, &m_iter);
      m_live = false;
    }
  }
};

//-----------------------------------------------------------------------------
// Atom / UTF-8 helpers shared by the JSON and YAML encoders
//-----------------------------------------------------------------------------

inline bool atom_to_sv(ErlNifEnv* env, ERL_NIF_TERM atom, char* buf, size_t bufsz, std::string_view& out)
{
  unsigned len = 0;
  if (!enif_get_atom_length(env, atom, &len, ERL_NIF_LATIN1)) return false;
  if (len + 1 > bufsz) return false;
  enif_get_atom(env, atom, buf, len + 1, ERL_NIF_LATIN1);
  out = {buf, len};
  return true;
}

// Write a "\uXXXX" escape (6 bytes, lowercase hex) for a code unit
// cu <= 0xFFFF directly into dst, without going through snprintf.
inline void write_uescape(char* dst, uint32_t cu)
{
  static constexpr char HEX[] = "0123456789abcdef";
  dst[0] = '\\';
  dst[1] = 'u';
  dst[2] = HEX[(cu >> 12) & 0xF];
  dst[3] = HEX[(cu >>  8) & 0xF];
  dst[4] = HEX[(cu >>  4) & 0xF];
  dst[5] = HEX[ cu        & 0xF];
}

// Emit a single Unicode code point as a \uXXXX escape (or a surrogate pair
// for code points beyond the BMP).
inline void push_uescape(OutBuf& out, uint32_t cp)
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

// Dense escape table: for each byte, stores the escape sequence as a
// length-prefixed 7-byte payload.  len==0 means no escaping needed.
// Layout per entry: [len][c0][c1][c2][c3][c4][c5][c6]  (8 bytes total)
struct EscapeEntry { uint8_t len; char seq[7]; };
static constexpr std::array<EscapeEntry, 256> build_escape_tab() {
  std::array<EscapeEntry, 256> tab{};
  const char* hex = "0123456789abcdef";
  for (int i = 0; i < 0x20; ++i) {
    tab[i].len    = 6;
    tab[i].seq[0] = '\\'; tab[i].seq[1] = 'u';
    tab[i].seq[2] = '0';  tab[i].seq[3] = '0';
    tab[i].seq[4] = hex[(i >> 4) & 0xF];
    tab[i].seq[5] = hex[i & 0xF];
  }
  tab['\b'] = {2, {'\\','b'}};
  tab['\f'] = {2, {'\\','f'}};
  tab['\n'] = {2, {'\\','n'}};
  tab['\r'] = {2, {'\\','r'}};
  tab['\t'] = {2, {'\\','t'}};
  tab['"']  = {2, {'\\','"'}};
  tab['\\'] = {2, {'\\','\\'}};
  return tab;
}
static constexpr std::array<EscapeEntry, 256> ESCAPE_TAB = build_escape_tab();

// NEEDS_ESCAPE_TAB: quick bool check for find_escape_pos scalar fallback.
static constexpr std::array<bool, 256> build_needs_escape_tab() {
  std::array<bool, 256> tab{};
  for (int i = 0; i < 256; ++i) tab[i] = ESCAPE_TAB[i].len > 0;
  return tab;
}
static constexpr std::array<bool, 256> NEEDS_ESCAPE_TAB = build_needs_escape_tab();

// Decode one UTF-8 sequence starting at p (p < end). Returns the code point
// and advances p past the sequence. On invalid/truncated input, returns the
// Unicode replacement character (U+FFFD) and advances p by one byte.
inline uint32_t decode_utf8(const char*& p, const char* end)
{
  auto c    = (unsigned char)*p;
  auto cont = [&](const char* q) {
    return q < end && ((unsigned char)*q & 0xC0) == 0x80;
  };

  if (c < 0x80) [[likely]] { ++p; return c; }

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

//-----------------------------------------------------------------------------
// SIMD byte scanner — shared by the JSON, YAML, and CSV modules.
//
// find_byte: return a pointer to the first occurrence of `c` in [p, end),
//            or `end` if not found.
// Cascades AVX2 (32 B/iter) → SSE2 (16 B/iter) → scalar.
//-----------------------------------------------------------------------------

inline const char* find_byte(const char* p, const char* end, char c) noexcept
{
#if defined(__AVX2__)
  {
    const __m256i vc = _mm256_set1_epi8(c);
    while (p + 32 <= end) {
      __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vc));
      if (mask) return p + __builtin_ctz(mask);
      p += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i vc = _mm_set1_epi8(c);
    while (p + 16 <= end) {
      __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vc));
      if (mask) return p + __builtin_ctz(mask);
      p += 16;
    }
  }
#endif
  while (p < end && *p != c) ++p;
  return p;
}

// Return a pointer to the first byte in [p, end) that needs JSON/YAML escaping
// (control char < 0x20, '"', or '\').  Returns end if none found.
// Uses NEON (16 B/iter) → AVX2 (32 B/iter) → SSE2 (16 B/iter) → table.
// The bias trick converts unsigned c < 0x20 to a signed comparison:
// (c ^ 0x80) < 0xA0 (signed), which SSE2/NEON signed-compare handles.
inline const char* find_escape_pos(const char* p, const char* end) noexcept
{
#if defined(__ARM_NEON__)
  {
    const uint8x16_t vq    = vdupq_n_u8('"');
    const uint8x16_t vbs   = vdupq_n_u8('\\');
    const uint8x16_t vbias = vdupq_n_u8(0x80);
    const uint8x16_t vcmp  = vdupq_n_u8(0xA0);
    while (p + 16 <= end) {
      uint8x16_t v      = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      uint8x16_t biased = veorq_u8(v, vbias);
      uint8x16_t hit    = vorrq_u8(vorrq_u8(
        vcltq_u8(biased, vcmp), vceqq_u8(v, vq)), vceqq_u8(v, vbs));
      uint64x2_t h64 = vreinterpretq_u64_u8(hit);
      uint64_t   lo  = vgetq_lane_u64(h64, 0);
      uint64_t   hi  = vgetq_lane_u64(h64, 1);
      if (lo | hi) {
        if (lo) return p + (__builtin_ctzll(lo) >> 3);
        return p + 8 + (__builtin_ctzll(hi) >> 3);
      }
      p += 16;
    }
  }
#endif
#if defined(__AVX2__)
  {
    const __m256i vq    = _mm256_set1_epi8('"');
    const __m256i vbs   = _mm256_set1_epi8('\\');
    const __m256i vbias = _mm256_set1_epi8(-128);
    const __m256i vcmp  = _mm256_set1_epi8(-96);
    while (p + 32 <= end) {
      __m256i  v      = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      __m256i  biased = _mm256_xor_si256(v, vbias);
      uint32_t mask   = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(
        _mm256_cmpgt_epi8(vcmp, biased),
        _mm256_cmpeq_epi8(v, vq)),
        _mm256_cmpeq_epi8(v, vbs)));
      if (mask) return p + __builtin_ctz(mask);
      p += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i vq    = _mm_set1_epi8('"');
    const __m128i vbs   = _mm_set1_epi8('\\');
    const __m128i vbias = _mm_set1_epi8(-128);
    const __m128i vcmp  = _mm_set1_epi8(-96);
    while (p + 16 <= end) {
      __m128i  v      = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      __m128i  biased = _mm_xor_si128(v, vbias);
      unsigned mask   = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(
        _mm_cmpgt_epi8(vcmp, biased),
        _mm_cmpeq_epi8(v, vq)),
        _mm_cmpeq_epi8(v, vbs)));
      if (mask) return p + __builtin_ctz(mask);
      p += 16;
    }
  }
#endif
  while (p < end && !NEEDS_ESCAPE_TAB[(unsigned char)*p]) ++p;
  return p;
}

//-----------------------------------------------------------------------------
// Minimal strptime-like parser, used to turn a `{datetime, InputFormat}`
// field into Unix epoch seconds (UTC).
//
// Supported directives: %Y %y %m %d %H %M %S %f %z, and literal `%%`.
// Any other character in the format must match the input literally; a space
// in the format matches a run of one-or-more whitespace characters in the
// input (as with strptime).
//-----------------------------------------------------------------------------

namespace datetime {

  // Days since 1970-01-01 for the given proleptic-Gregorian civil date.
  // Howard Hinnant's `days_from_civil` algorithm (public domain).
  inline int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
  {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
  }

  inline bool parse_uint(const char*& p, const char* end, int max_digits, int& out)
  {
    const char* start = p;
    out = 0;
    while (p < end && (p - start) < max_digits && *p >= '0' && *p <= '9') {
      out = out * 10 + (*p - '0');
      ++p;
    }
    return p > start;
  }

  // Parses `input` according to `format` (a strptime-like format string) and
  // returns the corresponding Unix epoch time in seconds (UTC), or
  // `std::nullopt` if the input doesn't match the format.
  // NOTE: we could use std::get_time(), but it's locale-dependent and doesn't
  // support fractional seconds or timezone offsets, so we implement our own
  inline std::optional<int64_t> parse(std::string_view input, std::string_view format)
  {
    const char* p   = input.data();
    const char* end = p + input.size();
    const char* f   = format.data();
    const char* fe  = f + format.size();

    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
    bool have_date = false;
    int tz_offset_sec = 0;

    while (f < fe) {
      char fc = *f;

      if (fc == '%' && f + 1 < fe) {
        char spec = f[1];
        f += 2;
        switch (spec) {
          case 'Y': {
            int v;
            if (!parse_uint(p, end, 4, v)) return std::nullopt;
            year = v; have_date = true;
            break;
          }
          case 'y': {
            int v;
            if (!parse_uint(p, end, 2, v)) return std::nullopt;
            year = (v <= 68) ? 2000 + v : 1900 + v; have_date = true;
            break;
          }
          case 'm': {
            int v;
            if (!parse_uint(p, end, 2, v) || v < 1 || v > 12) return std::nullopt;
            month = v; have_date = true;
            break;
          }
          case 'd': {
            int v;
            if (!parse_uint(p, end, 2, v) || v < 1 || v > 31) return std::nullopt;
            day = v; have_date = true;
            break;
          }
          case 'H': {
            int v;
            if (!parse_uint(p, end, 2, v) || v > 23) return std::nullopt;
            hour = v;
            break;
          }
          case 'M': {
            int v;
            if (!parse_uint(p, end, 2, v) || v > 59) return std::nullopt;
            minute = v;
            break;
          }
          case 'S': {
            int v;
            if (!parse_uint(p, end, 2, v) || v > 60) return std::nullopt;
            second = v;
            break;
          }
          case 'f': {
            // Fractional seconds — consume digits, discard.
            int v;
            if (!parse_uint(p, end, 9, v)) return std::nullopt;
            break;
          }
          case 'z': {
            if (p < end && (*p == 'Z' || *p == 'z')) { ++p; tz_offset_sec = 0; break; }
            if (p >= end || (*p != '+' && *p != '-'))
              return std::nullopt;
            auto neg = *p == '-';
            ++p;
            int hh, mm = 0;
            if (!parse_uint(p, end, 2, hh))
              return std::nullopt;
            if (p < end && *p == ':') ++p;
            if (p < end && *p >= '0' && *p <= '9' && !parse_uint(p, end, 2, mm))
              return std::nullopt;
            tz_offset_sec = (hh * 3600 + mm * 60) * (neg ? -1 : 1);
            break;
          }
          case '%':
            if (p >= end || *p != '%')
              return std::nullopt;
            ++p;
            break;
          default:
            return std::nullopt;
        }
        continue;
      }

      if (fc == ' ') {
        if (p >= end || !std::isspace(static_cast<uint8_t>(*p))) [[unlikely]]
          return std::nullopt;
        while (p < end && std::isspace(static_cast<uint8_t>(*p))) ++p;
        ++f;
        continue;
      }

      if (p >= end || *p != fc) [[unlikely]]
        return std::nullopt;
      ++p; ++f;
    }

    if (p != end)   return std::nullopt; // trailing input not consumed by format
    if (!have_date) return std::nullopt;

    auto days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    auto secs = days * 86400 + hour * 3600 + minute * 60 + second - tz_offset_sec;
    return secs;
  }

} // namespace datetime

} // namespace glz
