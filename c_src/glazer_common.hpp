// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Shared utilities used by both the JSON and YAML decoders/encoders:
// growable term/byte buffers, the object-key cache, and UTF-8/atom helpers.
//-----------------------------------------------------------------------------
#pragma once

#include "glazer_utf8.hpp"

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

//-----------------------------------------------------------------------------
// Inline term-type dispatch via BEAM tag bits (avoids indirect NIF API call).
// Falls back to enif_term_type for types not in the fast path.
//
// OTP term tagging (64-bit, stable since OTP 26):
//   Primary tag (bits 0–1): 0b01=list, 0b10=boxed, 0b11=immediate
//   Immediate1 (bits 0–3): 0xF=small_int, 0xB=immed2, 0x3=pid, 0x7=port
//   Immediate2 (bits 0–5): 0x0B=atom, 0x3B=nil (empty list)
//   Boxed header (low 6 bits of *ptr): 0x20/0x24=bitstring, 0x2C=map,
//     0x00=tuple, 0x08/0x0C=bigint, 0x18=float
//
// Boxed pointer extraction uses (x & ~7) rather than (x - 2) to strip both
// the primary tag (bit 1) and the literal tag (bit 2). The literal tag is
// active on platforms without ERTS_HAVE_OS_PHYSICAL_MEMORY_RESERVATION
// (notably Windows), where constant terms carry bit 2 set.
//-----------------------------------------------------------------------------
inline ErlNifTermType fast_term_type(ErlNifEnv* env, ERL_NIF_TERM term)
{
  auto x = static_cast<uintptr_t>(term);
  switch (x & 0x3) {
    case 0x1:
      return ERL_NIF_TERM_TYPE_LIST;
    case 0x3:
      switch (x & 0xF) {
        case 0xF: return ERL_NIF_TERM_TYPE_INTEGER;
        default:
          switch (x & 0x3F) {
            case 0x0B: return ERL_NIF_TERM_TYPE_ATOM;
            case 0x3B: return ERL_NIF_TERM_TYPE_LIST;
          }
      }
      break;
    case 0x2: {
      auto* ptr = reinterpret_cast<const uintptr_t*>(x & ~static_cast<uintptr_t>(0x7));
      switch (*ptr & 0x3F) {
        case 0x20: case 0x24: return ERL_NIF_TERM_TYPE_BITSTRING;
        case 0x2C:            return ERL_NIF_TERM_TYPE_MAP;
        case 0x00:            return ERL_NIF_TERM_TYPE_TUPLE;
        case 0x08: case 0x0C: return ERL_NIF_TERM_TYPE_INTEGER;
        case 0x18:            return ERL_NIF_TERM_TYPE_FLOAT;
      }
      break;
    }
  }
  return enif_term_type(env, term);
}

// Called from nif_load to verify fast_term_type tag-bit assumptions match
// the running OTP version. All terms allocated here live on the nif_load
// env and are garbage-collected when BEAM disposes of it after load returns.
inline bool verify_fast_term_type(ErlNifEnv* env)
{
  ERL_NIF_TERM atom   = enif_make_atom(env, "test");
  ERL_NIF_TERM small  = enif_make_int(env, 42);
  ERL_NIF_TERM nil    = enif_make_list(env, 0);
  ERL_NIF_TERM list   = enif_make_list(env, 1, small);
  ERL_NIF_TERM map    = enif_make_new_map(env);
  ERL_NIF_TERM flt    = enif_make_double(env, 3.14);
  ERL_NIF_TERM tup    = enif_make_tuple(env, 1, small);
  ERL_NIF_TERM bin;     enif_make_new_binary(env, 1, &bin);

  // Also test a non-empty map — literals from BEAM code may carry the
  // TAG_LITERAL_PTR bit (0x4) on platforms without
  // ERTS_HAVE_OS_PHYSICAL_MEMORY_RESERVATION (e.g. Windows).
  ERL_NIF_TERM map1;
  enif_make_map_put(env, map, atom, small, &map1);

  struct { ERL_NIF_TERM term; const char* name; } checks[] = {
    {atom,  "atom"},  {small, "small"}, {nil,  "nil"},  {list, "list"},
    {map,   "map"},   {map1,  "map1"},  {flt,  "float"},{tup,  "tuple"},
    {bin,   "binary"},
  };
  for (auto& c : checks) {
    auto expected = enif_term_type(env, c.term);
    auto got      = fast_term_type(env, c.term);
    if (got != expected) {
      fprintf(stderr, "glazer: fast_term_type mismatch for %s: "
              "expected %d, got %d (term=0x%llx)\n",
              c.name, (int)expected, (int)got,
              (unsigned long long)c.term);
      return false;
    }
  }
  return true;
}

// Switch key for a name: its first 8 bytes packed into an integer. Names that
// share their first 8 bytes share a key, so a case must still compare the full
// name when it matters (see the keyword and format dispatch).
// The bytes after the first 8, which str_to_u64 does not cover. Two names with
// equal keys are equal when their tails are equal too.
inline constexpr std::string_view str_tail8(std::string_view s) {
  return s.size() > 8 ? s.substr(8) : std::string_view{};
}

// For use after a switch on str_to_u64(kw), when kw's key equals that of the
// literal s: is kw the same name? A literal of at most 8 bytes is fully covered
// by its key, so kw only has to be no longer. A longer literal also compares the
// bytes after the first 8.
template <size_t N>
bool str_cmp_tail8(std::string_view kw, const char (&s)[N]) {
  if constexpr (N - 1 <= 8)
    return kw.size() <= 8;
  else
    return str_tail8(kw) == str_tail8(std::string_view(s, N - 1));
}

inline constexpr uint64_t str_to_u64(std::string_view s) {
  uint64_t result = 0;
  for (size_t i = 0; i < 8 && i < s.size(); ++i)
    result = (result << 8) | static_cast<uint8_t>(s[i]);
  return result;
}

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
// Dirty-scheduler threshold — inputs larger than this are offloaded to a
// dirty CPU scheduler so they don't block normal scheduler threads.
// Small inputs run inline on a normal scheduler; for those,
// update_reduction_count() (see glazer_common.hpp) reports the work done to
// the scheduler via enif_consume_timeslice so the BEAM updates the calling
// process's reduction count instead of charging a flat 1 reduction for the
// call. Dirty schedulers don't need this — they're not shared with regular
// processes the same way.
//-----------------------------------------------------------------------------

static constexpr size_t DIRTY_THRESHOLD = 4096;

// Nesting levels the size estimate walks. Deeper terms are reported as large,
// so they go to a dirty scheduler and the encoder rejects them with an error.
// Matches the encoder's nesting cap; keeps this recursion off the C stack limit.
static constexpr unsigned ESTIMATE_MAX_DEPTH = 128;

inline size_t estimate_json_term_size(ErlNifEnv* env, ERL_NIF_TERM term, unsigned depth = 0)
{
  if (depth >= ESTIMATE_MAX_DEPTH) [[unlikely]] return DIRTY_THRESHOLD + 1;

  if (enif_is_binary(env, term)) {
    ErlNifBinary bin;
    if (enif_inspect_binary(env, term, &bin))
      return bin.size * 2 + 8;
  }

  if (enif_is_list(env, term)) {
    ERL_NIF_TERM list = term;
    ERL_NIF_TERM head;
    size_t count = 0;
    size_t bytes = 0;
    while (enif_get_list_cell(env, list, &head, &list)) {
      ++count;
      bytes += estimate_json_term_size(env, head, depth + 1);
      if (count >= 256) return DIRTY_THRESHOLD + 1;
    }
    return bytes + count * 8;
  }

  if (enif_is_map(env, term)) {
    size_t size = 0;
    if (enif_get_map_size(env, term, &size))
      return size * 32;
  }

  if (enif_is_tuple(env, term)) {
    int arity = 0;
    const ERL_NIF_TERM* elems = nullptr;
    if (enif_get_tuple(env, term, &arity, &elems)) {
      size_t bytes = 0;
      for (int i = 0; i < arity; ++i)
        bytes += estimate_json_term_size(env, elems[i], depth + 1);
      return bytes + size_t(arity) * 12;
    }
  }

  return 0;
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

static constexpr size_t BYTES_PER_REDUCTION       = 20;
static constexpr size_t REDUCTION_COUNT           = 4000;
static constexpr size_t REDUCTION_CHECK_THRESHOLD = 512;

// Report the percentage of a timeslice consumed while processing `bytes`
// bytes, so the scheduler updates the process's reduction count instead of
// charging a flat 1 reduction for the NIF call. The return value of
// enif_consume_timeslice (1 if the process should yield/be preempted, 0
// otherwise) is normally used to drive cooperative scheduling for NIFs that
// process work in chunks across multiple calls. Here it is intentionally
// ignored: work that's long enough to need preemption is offloaded to a
// dirty scheduler instead, so this function is only reached for inline
// (small, sub-DIRTY_THRESHOLD) calls, where it serves purely to keep the
// reduction count accurate. Tiny payloads are skipped entirely.
inline void update_reduction_count([[maybe_unused]] ErlNifEnv* env, [[maybe_unused]] size_t bytes) {
#if ERL_NIF_MAJOR_VERSION > 2 || (ERL_NIF_MAJOR_VERSION == 2 && ERL_NIF_MINOR_VERSION >= 4)
  if (bytes < REDUCTION_CHECK_THRESHOLD) return;
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
  bool                empty()       const { return m_len == 0; }
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
    ERL_NIF_TERM map = 0;

    if (!enif_make_map_from_arrays(env, keys.data(), m_data, unsigned(n), &map)) [[unlikely]] {
      // ERTS rejects maps with duplicate keys. Fall back to the intended
      // JSON semantics: last occurrence wins. This is also used for the
      // object_as_tuple dedupe mode, so Dedupe controls the same behavior.
      map = enif_make_new_map(env);
      for (size_t i = 0; i < n; ++i) {
        ERL_NIF_TERM next;
        enif_make_map_put(env, map, keys.data()[i], m_data[i], &next);
        map = next;
      }
    }

    if (Dedupe && map) {
      // Explicit dedupe mode keeps the last value as well, but the fast-path
      // map constructor already returns a valid map when it succeeds, so we
      // only need a second pass when duplicate removal is required for an
      // already-constructed map that still contains repeated keys.
      ERL_NIF_TERM deduped = enif_make_new_map(env);
      for (size_t i = 0; i < n; ++i) {
        ERL_NIF_TERM next;
        enif_make_map_put(env, deduped, keys.data()[i], m_data[i], &next);
        deduped = next;
      }
      return deduped;
    }

    return map;
  }
};

//-----------------------------------------------------------------------------
// Generic small vector — stack allocation for small lists (ValidationError, etc)
//
// Optimizes common case where error lists have <16 errors: zero heap allocation
// for typical validation runs. Falls back to heap for larger collections.
//-----------------------------------------------------------------------------

template <typename T, size_t N>
struct SmallVec {
  T           m_inline[N];
  T*          m_data = m_inline;
  size_t      m_len  = 0;
  size_t      m_cap  = N;

  ~SmallVec() { if (m_data != m_inline) delete[] m_data; }

  void push_back(const T& v) {
    if (m_len == m_cap) [[unlikely]] {
      size_t nc = m_cap * 2;
      T* nb = new T[nc];
      std::move(m_data, m_data + m_len, nb);
      if (m_data != m_inline) delete[] m_data;
      m_data = nb;
      m_cap = nc;
    }
    m_data[m_len++] = v;
  }

  void push_back(T&& v) {
    if (m_len == m_cap) [[unlikely]] {
      size_t nc = m_cap * 2;
      T* nb = new T[nc];
      std::move(m_data, m_data + m_len, nb);
      if (m_data != m_inline) delete[] m_data;
      m_data = nb;
      m_cap = nc;
    }
    m_data[m_len++] = std::move(v);
  }

  const T* begin() const { return m_data; }
  const T* end()   const { return m_data + m_len; }
  T*       begin()       { return m_data; }
  T*       end()         { return m_data + m_len; }

  T&       back()       { return m_data[m_len - 1]; }
  const T& back() const { return m_data[m_len - 1]; }

  T&       operator[](size_t i)       { return m_data[i]; }
  const T& operator[](size_t i) const { return m_data[i]; }

  size_t size()  const { return m_len; }
  bool   empty() const { return m_len == 0; }
  void   clear()       { m_len = 0; }
};

//-----------------------------------------------------------------------------
// Hex digit lookup — shared by the JSON \uXXXX and YAML \xXX/\uXXXX/\UXXXXXXXX
// escape decoders. A 256-entry table turns the 3-branch chain
// ('0'-'9' / 'a'-'f' / 'A'-'F') into a single array index.
//-----------------------------------------------------------------------------
static constexpr auto HEX_DIGIT_VALUES = [] {
  std::array<int8_t, 256> t{};
  t.fill(-1);
  for (int i = 0; i <= 9; ++i) t['0' + i] = static_cast<int8_t>(i);
  for (int i = 0; i <  6; ++i) {
    t['a' + i] = static_cast<int8_t>(10 + i);
    t['A' + i] = static_cast<int8_t>(10 + i);
  }
  return t;
}();

inline int hex_digit_value(unsigned char c)
{
  return HEX_DIGIT_VALUES[c];
}

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
// Custom exception for allocation failures — zero overhead when not thrown
//-----------------------------------------------------------------------------
struct OutOfMemory : public std::exception {
  const char* what() const noexcept override { return "out of memory"; }
};

//-----------------------------------------------------------------------------
// Output buffer — 4 KB inline, grows to heap
//
// Exception-based error handling: allocation failures throw OutOfMemory() instead
// of silently failing (like the original code did). This forces explicit handling
// of OOM errors in the NIF wrappers.
//-----------------------------------------------------------------------------

struct OutBuf {
  static constexpr size_t INLINE = 4096;

  char   m_inline[INLINE];
  char*  m_data;
  size_t m_len;
  size_t m_cap;

  OutBuf() : m_data(m_inline), m_len(0), m_cap(INLINE) {}
  ~OutBuf() { if (m_data != m_inline) free(m_data); }

  void clear() noexcept { m_len = 0; }

  void reserve(size_t need) {
    if (need <= m_cap) [[likely]] return;
    size_t nc = m_cap;
    if (nc < 256) nc = 256;
    while (nc < need) nc *= 2;
    if (m_data == m_inline) [[unlikely]] {
      auto nb = static_cast<char*>(malloc(nc));
      if (!nb) [[unlikely]] throw OutOfMemory();
      memcpy(nb, m_data, m_len);
      m_data = nb;
    } else {
      auto resized = static_cast<char*>(realloc(m_data, nc));
      if (!resized) [[unlikely]] throw OutOfMemory();
      m_data = resized;
    }
    m_cap = nc;
  }

  void ensure(size_t need) {
    if (m_len + need <= m_cap) [[likely]] return;
    size_t nc = m_cap * 2;
    while (nc < m_len + need) nc *= 2;
    if (m_data == m_inline) [[unlikely]] {
      auto nb = static_cast<char*>(malloc(nc));
      if (!nb) [[unlikely]] throw OutOfMemory();
      memcpy(nb, m_data, m_len);
      m_data = nb;
    } else {
      auto resized = static_cast<char*>(realloc(m_data, nc));
      if (!resized) [[unlikely]] throw OutOfMemory();
      m_data = resized;
    }
    m_cap = nc;
  }

  void push(char c)                  { ensure(1); m_data[m_len++] = c; }
  void push(const char* s, size_t n) { ensure(n); memcpy(m_data + m_len, s, n); m_len += n; }
  void push(std::string_view sv)     { push(sv.data(), sv.size()); }

  void push_quoted(std::string_view sv) {
    const size_t n = sv.size();
    ensure(n + 2);
    char* dst = m_data + m_len;
    *dst++ = '"';
    if (n != 0) [[likely]]
      memcpy(dst, sv.data(), n);
    dst += n;
    *dst++ = '"';
    m_len = static_cast<size_t>(dst - m_data);
  }

  // Use functions below with caution: they do not check capacity and may
  // overwrite memory if used incorrectly. Call `ensure()` to make sure there is
  // enough space before calling these functions.
  void  unsafe_push(char c)                  { m_data[m_len++] = c; }
  // Write position for direct writes; commit(n) advances the length by n.
  char* tail()                               { return m_data + m_len; }
  void  commit(size_t n)                     { m_len += n; }
  void  unsafe_push(const char* s, size_t n) { memcpy(m_data + m_len, s, n); m_len += n; }
  void  unsafe_push(std::string_view sv)     { unsafe_push(sv.data(), sv.size()); }

  std::string_view view() const              { return {m_data, m_len}; }

  operator std::string_view() const          { return view(); }
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
  // Open-addressed, power-of-two-sized table with linear probing. 512 slots keep
  // the load low for documents with a few hundred distinct keys (twitter2 has 73
  // distinct keys, which collide often in 128 slots and mispredict on every probe).
  // Measured on twitter2 decode: 512 slots cut mispredictions by 18% over 128.
  static constexpr size_t CAP  = 512;
  static constexpr size_t MASK = CAP - 1;

  // Lazily-cleared via an epoch counter rather than zero-initializing the
  // whole array up front: a slot is "live" only if its `epoch` matches the
  // cache's current `m_epoch`. This avoids paying ~3KB of memset on every
  // single decode call (including tiny ones that never touch the cache —
  // see KEY_CACHE_MIN_SIZE) merely to construct the cache. `m_epoch` is
  // seeded from a process-wide monotonic counter, so leftover garbage from
  // prior stack frames can never coincide with it (it is always strictly
  // less than every epoch handed out so far).
  struct Entry { uint64_t pre; uint64_t tail; const char* s; size_t len; uint32_t hash; uint32_t epoch; ERL_NIF_TERM term; };
  Entry    m_entries[CAP]; // intentionally left uninitialized — see m_epoch
  size_t   m_count = 0;
  uint32_t m_epoch;

  static_assert((CAP & MASK) == 0, "CAP must be a power of two");

  static uint32_t next_epoch() {
    static std::atomic<uint32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1; // never 0
  }

  KeyCache() : m_epoch(next_epoch()) {}

  // First and last 8 key bytes, zero-padded for short keys. For keys of up to
  // 16 bytes these two words cover every byte, so equal words mean equal keys.
  static inline void key_words(const char* s, size_t len, uint64_t& pre, uint64_t& tail) {
    if (len >= 8) {
      memcpy(&pre, s, 8);
      memcpy(&tail, s + len - 8, 8);
    } else if (len >= 4) {
      // Two overlapping 4-byte loads cover every byte of a 4..7 byte key.
      uint32_t a, b;
      memcpy(&a, s, 4);
      memcpy(&b, s + len - 4, 4);
      pre = static_cast<uint64_t>(a) | (static_cast<uint64_t>(b) << 32);
      tail = pre;
    } else {
      pre = 0;
      for (size_t i = 0; i < len; ++i)
        pre |= static_cast<uint64_t>(static_cast<unsigned char>(s[i])) << (8 * i);
      tail = pre;
    }
  }

  static inline uint32_t hash_of(const char* s, size_t len) {
    uint64_t pre, tail;
    key_words(s, len, pre, tail);
    uint64_t h = pre * 0x9E3779B97F4A7C15ull ^ (tail + len) * 0xC2B2AE3D27D4EB4Full;
    return static_cast<uint32_t>(h >> 32);
  }

  // Compute hash + word pair in one call, avoiding double key_words().
  static inline uint32_t hash_and_words(const char* s, size_t len, uint64_t& pre_out, uint64_t& tail_out) {
    key_words(s, len, pre_out, tail_out);
    uint64_t h = pre_out * 0x9E3779B97F4A7C15ull ^ (tail_out + len) * 0xC2B2AE3D27D4EB4Full;
    return static_cast<uint32_t>(h >> 32);
  }

  // Returns 0 if not cached. Keys of up to 16 bytes are matched by their word
  // pair alone; longer keys are confirmed with memcmp.
  inline ERL_NIF_TERM lookup(const char* s, size_t len, uint32_t hash) const {
    uint64_t pre, tail;
    key_words(s, len, pre, tail);
    return lookup_with_words(s, len, hash, pre, tail);
  }

  inline ERL_NIF_TERM lookup_with_words(const char* s, size_t len, uint32_t hash,
                                        uint64_t pre, uint64_t tail) const {
    for (size_t i = hash & MASK, probes = 0; probes < CAP; ++probes, i = (i + 1) & MASK) {
      const Entry& e = m_entries[i];
      if (e.epoch != m_epoch) [[unlikely]] return 0;
      if (e.hash == hash && e.len == len && e.pre == pre && e.tail == tail &&
          (len <= 16 || memcmp(e.s, s, len) == 0)) [[likely]]
        return e.term;
    }
    return 0;
  }

  void insert(const char* s, size_t len, uint32_t hash, ERL_NIF_TERM term) {
    if (m_count >= CAP) return;
    uint64_t pre, tail;
    key_words(s, len, pre, tail);
    insert_with_words(s, len, hash, pre, tail, term);
  }

  void insert_with_words(const char* s, size_t len, uint32_t hash,
                          uint64_t pre, uint64_t tail, ERL_NIF_TERM term) {
    if (m_count >= CAP) return;
    for (size_t i = hash & MASK, probes = 0; probes < CAP; ++probes, i = (i + 1) & MASK) {
      if (m_entries[i].epoch != m_epoch) {
        m_entries[i] = {pre, tail, s, len, hash, m_epoch, term};
        ++m_count;
        return;
      }
    }
  }
};

//-----------------------------------------------------------------------------
// Atom-name cache for the JSON encoder — caches the escaped, quoted
// representation of atom keys (including the trailing ':') so that
// repeated atom keys cost a single memcpy instead of two NIF calls
// plus escaping.  Atoms are never GC'd, so the raw term value is a
// stable key.  The cache is designed to be thread_local and reused
// across encode calls.
//-----------------------------------------------------------------------------

struct AtomNameCache {
  static constexpr size_t CAP  = 512;
  static constexpr size_t MASK = CAP - 1;
  static constexpr size_t MAX_CACHED_LEN = 72; // "quoted_key": + NUL

  struct Entry {
    ERL_NIF_TERM atom;      // raw term value (atoms are immediates)
    uint16_t     len;       // length of cached escaped string
    char         buf[MAX_CACHED_LEN];
  };

  Entry  m_entries[CAP]{};  // zero-initialized: atom == 0 means empty
  size_t m_count = 0;

  static_assert((CAP & MASK) == 0, "CAP must be a power of two");

  // Returns the cached escaped key+colon string, or nullptr on miss.
  const char* lookup(ERL_NIF_TERM atom, size_t& len) const {
    size_t idx = (static_cast<size_t>(atom) >> 6) & MASK;
    for (size_t probes = 0; probes < 8; ++probes) {
      const Entry& e = m_entries[idx];
      if (e.atom == atom) { len = e.len; return e.buf; }
      if (e.atom == 0) return nullptr;
      idx = (idx + 1) & MASK;
    }
    return nullptr;
  }

  // Insert a cached escaped key+colon.  `data` must include quotes and colon.
  void insert(ERL_NIF_TERM atom, const char* data, size_t len) {
    if (m_count >= CAP / 2 || len >= MAX_CACHED_LEN) return;
    size_t idx = (static_cast<size_t>(atom) >> 6) & MASK;
    for (size_t probes = 0; probes < 8; ++probes) {
      Entry& e = m_entries[idx];
      if (e.atom == 0) {
        e.atom = atom;
        e.len  = static_cast<uint16_t>(len);
        memcpy(e.buf, data, len);
        ++m_count;
        return;
      }
      if (e.atom == atom) return; // already inserted
      idx = (idx + 1) & MASK;
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
      m_env    = o.m_env;
      m_iter   = o.m_iter;
      m_live   = o.m_live;
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
  static constexpr char     HEX[]    = "0123456789abcdef";
  static constexpr uint16_t PREFIX   = '\\' | ('u' << 8);
  memcpy(dst, &PREFIX, 2);

  uint32_t packed = (uint32_t(HEX[(cu >> 12) & 0xF])      ) |
                    (uint32_t(HEX[(cu >>  8) & 0xF]) <<  8) |
                    (uint32_t(HEX[(cu >>  4) & 0xF]) << 16) |
                    (uint32_t(HEX[ cu        & 0xF]) << 24);
  memcpy(dst + 2, &packed, 4);
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

static constexpr auto ESCAPE_TAB = []{
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
}();

// NEEDS_ESCAPE_TAB: quick bool check for find_escape_pos scalar fallback.
static constexpr auto NEEDS_ESCAPE_TAB = [] {
  std::array<bool, 256> tab{};
  for (int i = 0; i < 256; ++i) tab[i] = ESCAPE_TAB[i].len > 0;
  return tab;
}();


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
  [[maybe_unused]] const char* const start = p;  // used only by the SSE2 tail below
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
        vreinterpretq_u8_s8(vcgtq_s8(vreinterpretq_s8_u8(vcmp),
                                     vreinterpretq_s8_u8(biased))),
        vceqq_u8(v, vq)),
        vceqq_u8(v, vbs));
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
#if defined(__SSE2__)
  // Under 16 bytes left: one 16-byte window that ends at end. The bytes before p
  // were already scanned clean, so the first flagged byte in the window is at p
  // or later.
  if (end - start >= 16 && p < end) {
    const __m128i vq    = _mm_set1_epi8('"');
    const __m128i vbs   = _mm_set1_epi8('\\');
    const __m128i vbias = _mm_set1_epi8(-128);
    const __m128i vcmp  = _mm_set1_epi8(-96);
    const char* w = end - 16;
    __m128i  v      = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w));
    __m128i  biased = _mm_xor_si128(v, vbias);
    unsigned mask   = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(
      _mm_cmpgt_epi8(vcmp, biased),
      _mm_cmpeq_epi8(v, vq)),
      _mm_cmpeq_epi8(v, vbs)));
    return mask ? w + __builtin_ctz(mask) : end;
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
    const char* cend  = std::min(end, p+max_digits);
    out = 0;
    for (out = 0; p < cend && *p >= '0' && *p <= '9'; ++p)
      out = out * 10 + (*p - '0');
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
