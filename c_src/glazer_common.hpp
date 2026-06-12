// vim:ts=2:sw=2:et
// ---------------------------------------------------------------------------
// Shared utilities used by both the JSON and YAML decoders/encoders:
// growable term/byte buffers, the object-key cache, and UTF-8/atom helpers.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <erl_nif.h>

// ---------------------------------------------------------------------------
// Small inline-capacity buffer for term arrays built while parsing
// arrays/objects — avoids heap allocation for the common case (most
// containers have only a handful of elements).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Key cache — object/mapping keys repeat heavily within a document (e.g. a
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

// ---------------------------------------------------------------------------
// Atom / UTF-8 helpers shared by the JSON and YAML encoders
// ---------------------------------------------------------------------------

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
