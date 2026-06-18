// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// YAML-specific decode implementation.
//
// Decode: hand-rolled recursive-descent block-style parser — produces
//         Erlang terms directly in a single pass, mirroring the philosophy
//         of the JSON decoder in glazer_json.hpp (no intermediate tree).
//
// Not yet implemented: tags (!!str etc.), multi-document streams, complex
// (collection) mapping keys, merge-key (<<) semantics, anchors on mapping
// keys, top-level (root-node) anchors/aliases.
//-----------------------------------------------------------------------------
#pragma once

#include <cassert>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>
#if defined(__ARM_NEON__)
#  include <arm_neon.h>
#endif

#include "fast_float.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_common.hpp"

namespace glz {

//-----------------------------------------------------------------------------
// SIMD helpers for the YAML decoder/encoder.
//
// find_break:      first '\n' or '\r'          (end-of-line scan)
// find_dq_special: first '"', '\', '\n', '\r'  (double-quoted scalar scan)
// find_byte (glazer_common.hpp): first '\''    (single-quoted scalar encoder)
//
// AVX2 (32 B/iter) → SSE2 (16 B/iter) → scalar fallback.
//-----------------------------------------------------------------------------

static const char* find_break(const char* p, const char* end) noexcept
{
#if defined(__ARM_NEON__)
  {
    const uint8x16_t vn = vdupq_n_u8('\n');
    const uint8x16_t vr = vdupq_n_u8('\r');
    while (p + 16 <= end) {
      uint8x16_t v   = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      uint8x16_t hit = vorrq_u8(vceqq_u8(v, vn), vceqq_u8(v, vr));
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
    const __m256i vn = _mm256_set1_epi8('\n');
    const __m256i vr = _mm256_set1_epi8('\r');
    while (p + 32 <= end) {
      __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, vn), _mm256_cmpeq_epi8(v, vr)));
      if (mask) return p + __builtin_ctz(mask);
      p += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i vn = _mm_set1_epi8('\n');
    const __m128i vr = _mm_set1_epi8('\r');
    while (p + 16 <= end) {
      __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      unsigned mask = (unsigned)_mm_movemask_epi8(
        _mm_or_si128(_mm_cmpeq_epi8(v, vn), _mm_cmpeq_epi8(v, vr)));
      if (mask) return p + __builtin_ctz(mask);
      p += 16;
    }
  }
#endif
  while (p < end && *p != '\n' && *p != '\r') ++p;
  return p;
}

// Returns a pointer to the first byte in [p, end) that is '"', '\', '\n',
// or '\r' — the four characters that require special handling inside a
// double-quoted YAML scalar.
static const char* find_dq_special(const char* p, const char* end) noexcept
{
#if defined(__ARM_NEON__)
  {
    const uint8x16_t vq  = vdupq_n_u8('"');
    const uint8x16_t vbs = vdupq_n_u8('\\');
    const uint8x16_t vn  = vdupq_n_u8('\n');
    const uint8x16_t vr  = vdupq_n_u8('\r');
    while (p + 16 <= end) {
      uint8x16_t v   = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      uint8x16_t hit = vorrq_u8(vorrq_u8(vceqq_u8(v, vq), vceqq_u8(v, vbs)),
                                 vorrq_u8(vceqq_u8(v, vn), vceqq_u8(v, vr)));
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
    const __m256i vq  = _mm256_set1_epi8('"');
    const __m256i vbs = _mm256_set1_epi8('\\');
    const __m256i vn  = _mm256_set1_epi8('\n');
    const __m256i vr  = _mm256_set1_epi8('\r');
    while (p + 32 <= end) {
      __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vbs)),
        _mm256_or_si256(_mm256_cmpeq_epi8(v, vn), _mm256_cmpeq_epi8(v, vr))));
      if (mask) return p + __builtin_ctz(mask);
      p += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i vq  = _mm_set1_epi8('"');
    const __m128i vbs = _mm_set1_epi8('\\');
    const __m128i vn  = _mm_set1_epi8('\n');
    const __m128i vr  = _mm_set1_epi8('\r');
    while (p + 16 <= end) {
      __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vbs)),
        _mm_or_si128(_mm_cmpeq_epi8(v, vn), _mm_cmpeq_epi8(v, vr))));
      if (mask) return p + __builtin_ctz(mask);
      p += 16;
    }
  }
#endif
  while (p < end && *p != '"' && *p != '\\' && *p != '\n' && *p != '\r') ++p;
  return p;
}

//-----------------------------------------------------------------------------
// Options
//-----------------------------------------------------------------------------

struct YamlDecodeOpts {
  ERL_NIF_TERM null_term     = 0;
  bool         label_atom    = false;
  bool         label_existing_atom = false;
  bool         yaml_1_1_bools = false;
  // When true, scalars are copied into fresh binaries instead of referencing
  // the input via sub-binary. Use this when decoded scalars will outlive the
  // input binary by a large margin: without it, one live scalar keeps the
  // entire input buffer from being collected.
  bool         copy_strings  = false;
};

static bool parse_yaml_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, YamlDecodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if      (enif_is_identical(head, AM_USE_NIL))        opts.null_term      = AM_NIL;
    else if (enif_is_identical(head, AM_YAML_1_1_BOOLS)) opts.yaml_1_1_bools = true;
    else if (enif_is_identical(head, AM_COPY_STRINGS))   opts.copy_strings   = true;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2) {
        if (enif_is_identical(tp[0], AM_NULL_TERM) && enif_is_atom(env, tp[1]))
          opts.null_term = tp[1];
        else if (enif_is_identical(tp[0], AM_KEYS)) {
          if      (enif_is_identical(tp[1], AM_LABEL_ATOM))          opts.label_atom = true;
          else if (enif_is_identical(tp[1], AM_LABEL_EXISTING_ATOM)) opts.label_existing_atom = true;
          else if (enif_is_identical(tp[1], AM_LABEL_BINARY))        { opts.label_atom = false; opts.label_existing_atom = false; }
        }
      }
    }
  }
  return true;
}

struct YamlEncodeOpts {
  ERL_NIF_TERM null_term = 0;
};

static bool parse_yaml_encode_opts(ErlNifEnv* env, ERL_NIF_TERM list, YamlEncodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if (enif_is_identical(head, AM_USE_NIL)) opts.null_term = AM_NIL;
    else {
      int arity; const ERL_NIF_TERM* tp;
      if (enif_get_tuple(env, head, &arity, &tp) && arity == 2)
        if (enif_is_identical(tp[0], AM_NULL_TERM) && enif_is_atom(env, tp[1]))
          opts.null_term = tp[1];
    }
  }
  return true;
}

//-----------------------------------------------------------------------------
// YAML block-style decoder
//-----------------------------------------------------------------------------

struct YamlDecoder {
  ErlNifEnv*            m_env;
  const YamlDecodeOpts& m_opts;
  const char*           m_beg;
  const char*           m_p;
  const char*           m_end;
  ERL_NIF_TERM          m_input_bin; // original binary term — used for zero-copy sub_binary
  KeyCache              m_key_cache;
  bool                  m_use_key_cache;
  unsigned              m_depth = 0;
  std::string           m_err;
  // &anchor bindings — terms live in m_env, which outlives the decoder.
  std::map<std::string, ERL_NIF_TERM, std::less<>> m_anchors;

  static constexpr size_t   KEY_CACHE_MIN_SIZE = 2048;
  static constexpr unsigned MAX_DEPTH = 256;

  YamlDecoder(ErlNifEnv* e, const YamlDecodeOpts& o, const char* data, size_t size,
              ERL_NIF_TERM input_bin)
    : m_env(e), m_opts(o), m_beg(data), m_p(data), m_end(data + size),
      m_input_bin(input_bin), m_use_key_cache(size >= KEY_CACHE_MIN_SIZE) {}

  struct DepthGuard {
    explicit DepthGuard(YamlDecoder* d) : d(d) { ++d->m_depth; }
    ~DepthGuard() { --d->m_depth; }
    bool ok() const { return d->m_depth <= MAX_DEPTH; }
  private:
    YamlDecoder* d;
  };

  // -------------------------------------------------------------------------
  // Low-level helpers
  // -------------------------------------------------------------------------

  static inline bool is_blank(char c) { return c == ' ' || c == '\t'; }
  static inline bool is_break(char c) { return c == '\n' || c == '\r'; }
  static inline bool is_flow_indicator(char c) {
    return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
  }

  bool at_end() const { return m_p >= m_end; }

  // Skip a single line break (\r\n, \r, or \n).
  void skip_break() {
    if (m_p < m_end && *m_p == '\r') ++m_p;
    if (m_p < m_end && *m_p == '\n') ++m_p;
  }

  // Skip blanks (space/tab) only.
  void skip_blanks() { while (m_p < m_end && is_blank(*m_p)) ++m_p; }

  // Skip a trailing comment (# ... ) to end of line. Caller has already
  // verified we're not inside a quoted scalar. A '#' only starts a comment
  // if it's at the start of the line or preceded by whitespace.
  void skip_comment_to_eol() {
    m_p = find_break(m_p, m_end);
  }

  // Skip blank lines and comment-only lines, and any line whose content
  // (after indentation) is empty or starts with '#'. Also tolerates
  // `---`/`...` document marker lines (single-document mode: no-ops).
  // Leaves m_p at the start of the next line that has real content, or at
  // m_end.
  void skip_blank_and_comment_lines() {
    for (;;) {
      const char* line_start = m_p;
      skip_blanks();
      if (at_end()) { m_p = line_start; return; }
      if (is_break(*m_p)) { skip_break(); continue; }
      if (*m_p == '#') { skip_comment_to_eol(); if (!at_end()) skip_break(); continue; }
      // Document markers at column 0 only.
      if (line_start == m_p && m_p + 3 <= m_end &&
          ((memcmp(m_p, "---", 3) == 0) || (memcmp(m_p, "...", 3) == 0))) {
        const char* after = m_p + 3;
        if (after >= m_end || is_blank(*after) || is_break(*after) || *after == '#') {
          m_p = after;
          skip_blanks();
          if (m_p < m_end && *m_p == '#') skip_comment_to_eol();
          if (!at_end()) skip_break();
          continue;
        }
      }
      // Real content — restore m_p to the start of this line so the caller
      // (peek_indent / parse_block / parse_mapping / parse_sequence) sees
      // the line's indentation.
      m_p = line_start;
      return;
    }
  }

  // Returns the indentation column (count of leading spaces; tabs are not
  // valid YAML indentation) of the current line without consuming it.
  // Assumes m_p is at the start of a line.
  size_t peek_indent() const {
    const char* p = m_p;
    size_t col = 0;
    while (p < m_end && *p == ' ') { ++p; ++col; }
    return col;
  }

  // -------------------------------------------------------------------------
  // Scalar filtering (Section 3.x algorithms, adapted from rapidyaml)
  // -------------------------------------------------------------------------

  // Folds line breaks within a multi-line plain/quoted scalar per the YAML
  // spec: every maximal run of N consecutive line breaks folds to a single
  // space if N == 1, or to (N-1) literal newlines if N > 1. `lines` are the
  // raw (already trailing-whitespace-trimmed for plain, or as-is for quoted)
  // per-line spans, i.e. `lines.size() - 1` is the total number of line
  // breaks; empty entries indicate runs of consecutive breaks. Plain-scalar
  // callers must strip trailing empty entries first (trailing breaks are not
  // part of a plain scalar); for quoted scalars leading/trailing runs fold
  // by the same rule (e.g. "a\n" -> "a ", "\n\na" -> "\na").
  template <class Container>
  static void fold_lines(const Container& lines, std::string& out) {
    out.clear();
    size_t n = lines.size();
    if (n == 0) return;
    out.append(std::string_view(lines[0]));
    size_t i = 1;
    while (i < n) {
      // One break reaches lines[i]; each *interior* empty entry skipped
      // extends the run by one more break. A trailing empty entry is the
      // run's end, not an extra break.
      size_t breaks = 1;
      while (i < n && lines[i].empty() && i + 1 < n) { ++breaks; ++i; }
      if (breaks == 1) out.push_back(' ');
      else             out.append(breaks - 1, '\n');
      out.append(std::string_view(lines[i]));
      ++i;
    }
  }

  // ---- Plain scalar (Section 3.1) -----------------------------------------
  //
  // Reads a plain (unquoted) scalar starting at m_p, which may span multiple
  // lines if continuation lines are indented more than `min_indent` and do
  // not look like new block-structure entries. Stops at: ': ' (mapping
  // value indicator), ' #' (comment), a line whose indentation is
  // <= min_indent, or end of input. Trailing whitespace on each line is
  // stripped before folding.
  std::string_view read_plain_scalar(size_t min_indent, std::string& scratch) {
    std::vector<std::string_view> lines;
    const char* line_begin = m_p;

    for (;;) {
      const char* seg_start = m_p;
      const char* last_nonblank = m_p;
      bool seg_has_content = false;

      bool stop = false;
      while (m_p < m_end && !is_break(*m_p)) {
        char c = *m_p;
        // ': ' or ':' at end of line ends the scalar (mapping key/value sep).
        if (c == ':' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1]))) {
          stop = true; break;
        }
        // ' #' (or '#' at start of scalar) starts a comment.
        if (c == '#' && (m_p == seg_start || is_blank(m_p[-1]))) {
          stop = true; break;
        }
        if (!is_blank(c)) { last_nonblank = m_p + 1; seg_has_content = true; }
        ++m_p;
      }

      if (seg_has_content || !lines.empty())
        lines.emplace_back(seg_start, last_nonblank - seg_start);

      if (stop) goto done;
      if (at_end()) break;
      // Peek ahead: is the next line a continuation?
      const char* save = m_p;
      skip_break();
      // Skip blank lines (these become extra folds handled by fold_lines
      // via empty entries).
      const char* resume = m_p;
      for (;;) {
        const char* probe = resume;
        size_t indent = 0;
        while (probe < m_end && *probe == ' ') { ++probe; ++indent; }
        if (probe < m_end && is_break(*probe)) {
          // blank line
          lines.emplace_back();
          resume = probe; skip_break_at(resume);
          continue;
        }
        if (probe >= m_end || indent <= min_indent) {
          m_p = save;
          goto done;
        }
        // Continuation line.
        m_p = probe;
        break;
      }
    }
  done:
    (void)line_begin;
    // Trailing blank lines (from blank-line probing that hit a dedent/EOF)
    // are not part of the scalar.
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty()) return std::string_view();
    if (lines.size() == 1) return lines[0];
    fold_lines(lines, scratch);
    return scratch;
  }

  // Helper for read_plain_scalar: advance `p` past a line break.
  static void skip_break_at(const char*& p) {
    if (*p == '\r') ++p;
    if (*p == '\n') ++p;
  }

  // ---- Single-quoted scalar (Section 3.2) ----------------------------------
  //
  // Reads '...' starting at m_p (m_p points at the opening '\''). On return,
  // m_p is past the closing quote. `''` is an escaped single quote. Line
  // breaks fold per fold_lines; trailing whitespace on each line before a
  // fold is stripped.
  bool read_single_quoted(std::string& out, size_t /*min_indent*/) {
    ++m_p; // skip opening quote
    std::vector<std::string> lines;
    std::string cur;

    for (;;) {
      if (at_end()) { m_err = "unterminated single-quoted scalar"; return false; }
      char c = *m_p;
      if (c == '\'') {
        if (m_p + 1 < m_end && m_p[1] == '\'') {
          cur.push_back('\'');
          m_p += 2;
          continue;
        }
        ++m_p; // closing quote
        break;
      }
      if (is_break(c)) {
        size_t e = cur.size();
        while (e > 0 && is_blank(cur[e-1])) --e;
        cur.resize(e);
        lines.push_back(std::move(cur));
        cur.clear();
        skip_break();
        while (m_p < m_end && *m_p == ' ') ++m_p;
        continue;
      }
      cur.push_back(c);
      ++m_p;
    }
    // Trailing blanks before the closing quote are content ('a  ' keeps both
    // spaces) — only blanks adjacent to a fold are stripped (per-break above).
    lines.push_back(std::move(cur));

    if (lines.size() == 1) { out = std::move(lines[0]); return true; }
    std::vector<std::string_view> views;
    views.reserve(lines.size());
    for (auto& l : lines) views.emplace_back(l);
    fold_lines(views, out);
    return true;
  }

  // ---- Double-quoted scalar (Section 3.3) ----------------------------------
  //
  // Reads "..." starting at m_p (pointing at opening '"'). Handles full
  // escape table and newline folding. On return, m_p is past the closing
  // quote.
  bool read_double_quoted(std::string& out, size_t min_indent) {
    (void)min_indent;
    ++m_p; // skip opening quote
    std::vector<std::string> lines;
    std::string cur;

    for (;;) {
      // SIMD: bulk-append all bytes up to the next '"', '\', '\n', or '\r'.
      {
        const char* safe = find_dq_special(m_p, m_end);
        if (safe > m_p) { cur.append(m_p, safe - m_p); m_p = safe; }
      }
      if (at_end()) { m_err = "unterminated double-quoted scalar"; return false; }
      char c = *m_p;
      if (c == '"') { ++m_p; break; }
      if (c == '\\') {
        ++m_p;
        if (at_end()) { m_err = "unterminated escape in double-quoted scalar"; return false; }
        char e = *m_p;
        switch (e) {
          case '0':  cur.push_back('\0'); ++m_p; break;
          case 'a':  cur.push_back('\a'); ++m_p; break;
          case 'b':  cur.push_back('\b'); ++m_p; break;
          case 't': case '\t': cur.push_back('\t'); ++m_p; break;
          case 'n':  cur.push_back('\n'); ++m_p; break;
          case 'v':  cur.push_back('\v'); ++m_p; break;
          case 'f':  cur.push_back('\f'); ++m_p; break;
          case 'r':  cur.push_back('\r'); ++m_p; break;
          case 'e':  cur.push_back('\x1b'); ++m_p; break;
          case ' ':  cur.push_back(' '); ++m_p; break;
          case '"':  cur.push_back('"'); ++m_p; break;
          case '/':  cur.push_back('/'); ++m_p; break;
          case '\\': cur.push_back('\\'); ++m_p; break;
          case 'N':  append_utf8(cur, 0x85);   ++m_p; break;       // NEL
          case '_':  append_utf8(cur, 0xA0);   ++m_p; break;       // NBSP
          case 'L':  append_utf8(cur, 0x2028); ++m_p; break;       // LS
          case 'P':  append_utf8(cur, 0x2029); ++m_p; break;       // PS
          case 'x': { uint32_t cp; if (!read_hex_escape(2, cp)) return false; append_utf8(cur, cp); break; }
          case 'u': { uint32_t cp; if (!read_hex_escape(4, cp)) return false; append_utf8(cur, cp); break; }
          case 'U': { uint32_t cp; if (!read_hex_escape(8, cp)) return false; append_utf8(cur, cp); break; }
          case '\n': case '\r':
            // Escaped line break: line-continuation, no fold (consumed below
            // along with leading whitespace of the next line).
            skip_break();
            while (m_p < m_end && is_blank(*m_p)) ++m_p;
            continue;
          default:
            m_err = "invalid escape sequence in double-quoted scalar";
            return false;
        }
        continue;
      }
      if (is_break(c)) {
        // strip trailing blanks already accumulated in cur for this line
        size_t e = cur.size();
        while (e > 0 && is_blank(cur[e-1])) --e;
        cur.resize(e);
        lines.push_back(std::move(cur));
        cur.clear();
        skip_break();
        while (m_p < m_end && *m_p == ' ') ++m_p;
        continue;
      }
      // Unreachable: find_dq_special() already stops at any of '"', '\', '\n', '\r'.
      cur.push_back(c);
      ++m_p;
    }
    // strip trailing blanks of the final segment too (only matters if a fold
    // follows immediately before closing quote, which is invalid YAML but be
    // lenient)
    lines.push_back(std::move(cur));

    if (lines.size() == 1) { out = std::move(lines[0]); return true; }
    std::vector<std::string_view> views;
    views.reserve(lines.size());
    for (auto& l : lines) views.emplace_back(l);
    fold_lines(views, out);
    return true;
  }

  bool read_hex_escape(int ndigits, uint32_t& out) {
    if (m_p + 1 + ndigits > m_end) { m_err = "truncated hex escape"; return false; }
    uint32_t v = 0;
    for (int i = 1; i <= ndigits; ++i) {
      int d = hex_digit_value(static_cast<unsigned char>(m_p[i]));
      if (d < 0) { m_err = "invalid hex digit in escape"; return false; }
      v = v * 16 + uint32_t(d);
    }
    m_p += 1 + ndigits;
    out = v;
    return true;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(char(cp));
    } else if (cp < 0x800) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  // ---- Block scalars (| literal, > folded) — Section 8.1 -------------------
  //
  // m_p points at the '|' or '>' header indicator. `parent_indent` is the
  // column of the construct owning the scalar (mapping key column, sequence
  // dash column, or 0 for a top-level document): content must be indented
  // strictly more. The header may carry a chomping indicator ('-' strip /
  // '+' keep / default clip) and an explicit indentation indicator (1-9,
  // relative to parent_indent), in either order, followed only by an
  // optional comment.
  ERL_NIF_TERM read_block_scalar(bool folded, size_t parent_indent) {
    ++m_p; // consume '|' or '>'
    int chomp     = 0;  // -1 strip, 0 clip, +1 keep
    int indicator = -1;
    while (m_p < m_end) {
      char c = *m_p;
      if      ((c == '-' || c == '+') && chomp == 0)  chomp = (c == '-') ? -1 : 1;
      else if (c >= '1' && c <= '9' && indicator < 0) indicator = c - '0';
      else break;
      ++m_p;
    }
    skip_blanks();
    if (!at_end() && *m_p == '#') skip_comment_to_eol();
    if (!at_end() && !is_break(*m_p)) { m_err = "invalid block scalar header"; return 0; }
    if (!at_end()) skip_break();

    size_t indent   = indicator > 0 ? parent_indent + size_t(indicator) : 0;
    bool   detected = indicator > 0;

    std::vector<std::string_view> lines;
    size_t breaks_after = 0; // line breaks since the last non-empty content line

    for (;;) {
      if (at_end()) break;
      const char* line_begin = m_p;
      const char* q = line_begin;
      size_t s = 0;
      while (q < m_end && *q == ' ') { ++q; ++s; }
      const char* r = q;
      while (r < m_end && is_blank(*r)) ++r; // tabs after the space indent
      if (r >= m_end || is_break(*r)) {
        // Blank line. Whitespace beyond the indent is content ("x\n    \n"
        // at indent 2 has a "  " content line); otherwise the line is empty.
        if (detected && size_t(r - line_begin) > indent) {
          lines.emplace_back(line_begin + indent, size_t(r - line_begin) - indent);
          breaks_after = 0;
        } else {
          lines.emplace_back();
        }
        m_p = r;
        if (!at_end()) { skip_break(); ++breaks_after; }
        continue;
      }
      // Non-blank line: detect / verify indentation.
      if (!detected) {
        if (s <= parent_indent) break; // not part of this scalar
        indent   = s;
        detected = true;
      } else if (s < indent) {
        break; // dedent ends the scalar
      }
      const char* eol = r;
      while (eol < m_end && !is_break(*eol)) ++eol;
      // Content is verbatim from the indent column, incl. extra leading
      // spaces (more-indented lines) and trailing blanks.
      lines.emplace_back(line_begin + indent, size_t(eol - line_begin) - indent);
      breaks_after = 0;
      m_p = eol;
      if (!at_end()) { skip_break(); ++breaks_after; }
    }

    while (!lines.empty() && lines.back().empty()) lines.pop_back();

    std::string body;
    if (!folded) {
      // Literal: verbatim lines joined by single newlines.
      for (size_t i = 0; i < lines.size(); ++i) {
        if (i) body.push_back('\n');
        body.append(lines[i]);
      }
    } else {
      // Folded: like plain-scalar folding, except breaks adjacent to
      // more-indented lines (leading whitespace after the indent) are
      // literal, and so are the empty lines between them.
      size_t i = 0, n = lines.size();
      while (i < n && lines[i].empty()) { body.push_back('\n'); ++i; }
      if (i < n) {
        body.append(lines[i]);
        bool prev_more = is_blank(lines[i].front());
        ++i;
        while (i < n) {
          size_t k = 0;
          while (i + k < n && lines[i + k].empty()) ++k;
          i += k;
          bool cur_more = is_blank(lines[i].front());
          // k empty entries bounded by content = k+1 breaks. Literal next
          // to more-indented lines; folded otherwise (1 break -> space,
          // k+1 breaks -> k newlines).
          if (prev_more || cur_more) body.append(k + 1, '\n');
          else if (k == 0)           body.push_back(' ');
          else                       body.append(k, '\n');
          body.append(lines[i]);
          prev_more = cur_more;
          ++i;
        }
      }
    }

    // Chomping: strip drops all trailing breaks, clip keeps at most one,
    // keep retains them all.
    if (chomp > 0)
      body.append(breaks_after, '\n');
    else if (chomp == 0 && breaks_after > 0 && !body.empty())
      body.push_back('\n');

    return make_binary(m_env, body);
  }

  // -------------------------------------------------------------------------
  // Anchors and aliases (&name / *name)
  // -------------------------------------------------------------------------

  // Reads the name following an '&' or '*' indicator at m_p. Names end at
  // whitespace, a line break, or a flow indicator.
  bool read_anchor_name(std::string& name) {
    bool alias = (*m_p == '*');
    ++m_p;
    const char* s = m_p;
    while (m_p < m_end && !is_blank(*m_p) && !is_break(*m_p) && !is_flow_indicator(*m_p))
      ++m_p;
    if (m_p == s) {
      m_err = alias ? "empty alias name" : "empty anchor name";
      return false;
    }
    name.assign(s, size_t(m_p - s));
    return true;
  }

  // Resolves a '*alias' at m_p to its anchored term. An alias to an anchor
  // whose node hasn't completed yet (i.e. a cycle — unrepresentable as an
  // Erlang term) is reported as undefined.
  ERL_NIF_TERM resolve_alias() {
    std::string name;
    if (!read_anchor_name(name)) return 0;
    auto it = m_anchors.find(name);
    if (it == m_anchors.end()) {
      m_err = "undefined alias '" + name + "'";
      return 0;
    }
    return it->second;
  }

  // -------------------------------------------------------------------------
  // Implicit scalar typing (Section 1.3 / YAML 1.2 core schema)
  // -------------------------------------------------------------------------

  ERL_NIF_TERM resolve_plain_scalar(std::string_view s) {
    if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL")
      return m_opts.null_term;
    if (s == "true"  || s == "True"  || s == "TRUE")  return AM_TRUE;
    if (s == "false" || s == "False" || s == "FALSE") return AM_FALSE;
    if (m_opts.yaml_1_1_bools) {
      if (s == "yes" || s == "Yes"   || s == "YES" ||
          s == "on"  || s == "On"    || s == "ON")
        return AM_TRUE;
      if (s == "no"  || s == "No"    || s == "NO" ||
          s == "off" || s == "Off"   || s == "OFF")
        return AM_FALSE;
    }
    if (s == ".inf"  || s == ".Inf"  || s == ".INF" || s == "+.inf" || s == "+.Inf" || s == "+.INF")
      return AM_INFINITY;
    if (s == "-.inf" || s == "-.Inf" || s == "-.INF")
      return AM_NEG_INFINITY;
    if (s == ".nan"  || s == ".NaN"  || s == ".NAN")
      return AM_NAN;

    if (ERL_NIF_TERM num = try_parse_number(s)) return num;

    return make_span_term(m_env, m_input_bin, m_beg, m_end, s, m_opts.copy_strings);
  }

  // Returns 0 if `s` doesn't look like a YAML core-schema int/float.
  ERL_NIF_TERM try_parse_number(std::string_view s) {
    if (s.empty()) return 0;
    const char* p = s.data();
    const char* end = p + s.size();
    const char* start = p;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); ++p; }
    if (p == end) return 0;

    // Hex / octal: 0x... / 0o...
    if (*p == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
      return parse_radix_int(p + 2, end, 16, neg);
    }
    if (*p == '0' && p + 1 < end && (p[1] == 'o' || p[1] == 'O')) {
      return parse_radix_int(p + 2, end, 8, neg);
    }

    // Scan digits / one dot / exponent to determine int vs float.
    const char* q = p;
    bool has_digit = false, has_dot = false, has_exp = false;
    while (q < end) {
      char c = *q;
      if (c >= '0' && c <= '9') { has_digit = true; ++q; continue; }
      if (c == '.' && !has_dot && !has_exp) { has_dot = true; ++q; continue; }
      if ((c == 'e' || c == 'E') && has_digit && !has_exp) {
        has_exp = true; ++q;
        if (q < end && (*q == '+' || *q == '-')) ++q;
        continue;
      }
      return 0; // not a number
    }
    if (!has_digit) return 0;

    if (!has_dot && !has_exp) {
      // Plain integer.
      if (neg) {
        int64_t v = 0;
        auto [ep, ec] = std::from_chars(start, end, v);
        if (ec == std::errc{} && ep == end) return enif_make_int64(m_env, v);
      } else {
        uint64_t v = 0;
        auto [ep, ec] = std::from_chars(start, end, v);
        if (ec == std::errc{} && ep == end)
          return v <= uint64_t(INT64_MAX) ? enif_make_int64(m_env, int64_t(v))
                                          : enif_make_uint64(m_env, v);
      }
      ERL_NIF_TERM r = glz::BigInt::decode(m_env, start, end);
      return r ? r : (ERL_NIF_TERM)0;
    }

    double d;
    auto [ep, ec] = glz::fast_float::from_chars(start, end, d);
    if (ec == std::errc{} && ep == end)
      return enif_make_double(m_env, d);
    return 0;
  }

  ERL_NIF_TERM parse_radix_int(const char* p, const char* end, int radix, bool neg) {
    if (p >= end) return 0;
    for (const char* q = p; q < end; ++q) {
      char c = *q;
      bool ok = (radix == 16) ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                               : (c >= '0' && c <= '7');
      if (!ok) return 0;
    }
    uint64_t v = 0;
    auto [ep, ec] = std::from_chars(p, end, v, radix);
    if (ec != std::errc{} || ep != end) return 0;
    int64_t sv = neg ? -int64_t(v) : int64_t(v);
    return enif_make_int64(m_env, sv);
  }

  // -------------------------------------------------------------------------
  // Block-style parsing
  // -------------------------------------------------------------------------

  // True iff `s` is a sub-span of the original input buffer — i.e. safe to
  // retain a pointer into (KeyCache::insert) rather than a transient local
  // buffer (a quoted-scalar `out` or a fold/unescape `scratch`) that is
  // destroyed when the caller returns.
  bool is_input_span(std::string_view s) const {
    return s.data() >= m_beg && s.data() + s.size() <= m_end;
  }

  // Make a key term honoring label_atom / label_existing_atom. Mirrors
  // Decoder::make_key_term in glazer_json.hpp: the key cache only retains
  // keys backed by the original input (mirrors JSON's has_escape bypass) —
  // `s` may instead be a quoted-scalar or line-folded scratch buffer local
  // to the caller, which must never be cached by pointer.
  ERL_NIF_TERM make_key_term(std::string_view s) {
    if (m_opts.label_atom)
      return enif_make_atom_len(m_env, s.data(), s.size());
    if (m_opts.label_existing_atom) {
      ERL_NIF_TERM t;
      return enif_make_existing_atom_len(m_env, s.data(), s.size(), &t, ERL_NIF_LATIN1)
           ? t : make_span_term(m_env, m_input_bin, m_beg, m_end, s, m_opts.copy_strings);
    }
    if (m_use_key_cache && is_input_span(s)) {
      uint32_t h = KeyCache::hash_of(s.data(), s.size());
      if (ERL_NIF_TERM cached = m_key_cache.lookup(s.data(), s.size(), h))
        return cached;
      auto term = make_span_term(m_env, m_input_bin, m_beg, m_end, s, m_opts.copy_strings);
      m_key_cache.insert(s.data(), s.size(), h, term);
      return term;
    }
    return make_span_term(m_env, m_input_bin, m_beg, m_end, s, m_opts.copy_strings);
  }

  // Parses a single scalar token at the current position (plain, single- or
  // double-quoted), returning the resolved Erlang term. `min_indent` bounds
  // multi-line plain scalar continuations. Sets m_err on failure (returns 0).
  ERL_NIF_TERM parse_scalar(size_t min_indent) {
    if (at_end()) return m_opts.null_term;
    char c = *m_p;
    std::string scratch;
    if (c == '\'') {
      std::string out;
      if (!read_single_quoted(out, min_indent)) return 0;
      return make_binary(m_env, out);
    }
    if (c == '"') {
      std::string out;
      if (!read_double_quoted(out, min_indent)) return 0;
      return make_binary(m_env, out);
    }
    std::string_view s = read_plain_scalar(min_indent, scratch);
    return resolve_plain_scalar(s);
  }

  // After reading a `key:` or `- ` marker, decide what follows on the rest
  // of this line:
  //   - nothing / only a comment -> value is on subsequent indented lines
  //     (nested block) or is null (empty)
  //   - a scalar / nested flow value -> parse it inline
  // `min_indent` is the indent column of the current container (mapping or
  // sequence) — used to bound plain scalar continuations.
  ERL_NIF_TERM parse_node(size_t min_indent) {
    skip_blanks();
    if (at_end() || is_break(*m_p) || *m_p == '#') {
      // Empty inline value -> look for a nested block on following lines.
      const char* save = m_p;
      if (!at_end() && *m_p == '#') skip_comment_to_eol();
      if (!at_end()) skip_break();
      skip_blank_and_comment_lines();
      if (at_end()) { m_p = save; return m_opts.null_term; }
      size_t next_indent = peek_indent();
      if (next_indent > min_indent) {
        // A block scalar header may sit on its own line ("a:\n  |\n  x").
        const char* q = m_p + next_indent;
        if (q < m_end && (*q == '|' || *q == '>')) {
          m_p = q;
          return read_block_scalar(*q == '>', min_indent);
        }
        return parse_block(next_indent);
      }
      // A block sequence is allowed at the same indentation as the mapping
      // key that owns it (e.g. "imp:\n- id: x"), per spec.
      if (next_indent == min_indent) {
        const char* q = m_p + next_indent;
        if (q < m_end && *q == '-' && (q + 1 >= m_end || is_blank(q[1]) || is_break(q[1])))
          return parse_sequence(next_indent);
      }
      m_p = save;
      return m_opts.null_term;
    }

    // Anchor: '&name' binds the node that follows; alias: '*name' reuses it.
    if (*m_p == '&') {
      std::string name;
      if (!read_anchor_name(name)) return 0;
      ERL_NIF_TERM v = parse_node(min_indent);
      if (!v) return 0;
      m_anchors[name] = v;
      return v;
    }
    if (*m_p == '*')
      return resolve_alias();

    // Block scalar ("a: |", "- >-", ...).
    if (*m_p == '|' || *m_p == '>')
      return read_block_scalar(*m_p == '>', min_indent);

    // Inline flow collection ("a: [1, 2]" / "- {k: v}").
    if (*m_p == '[' || *m_p == '{') {
      ERL_NIF_TERM v = parse_flow_node();
      if (!v) return 0;
      if (!check_flow_line_end()) return 0;
      return v;
    }

    // Inline value on the same line: a scalar (plain/quoted), a nested block
    // sequence ("- item"), or a nested block mapping ("key: value" — common
    // after a sequence dash, e.g. "- a: 1"). All of these start a nested
    // block at the current column.
    if (*m_p == '-' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1]))) {
      size_t col = size_t(m_p - line_start(m_p));
      return parse_block(col, /*at_line_start=*/false);
    }

    if (looks_like_mapping_line()) {
      size_t col = size_t(m_p - line_start(m_p));
      return parse_block(col, /*at_line_start=*/false);
    }

    return parse_scalar(min_indent);
  }

  // Returns a pointer to the start of the line containing `p`.
  const char* line_start(const char* p) const {
    while (p > m_beg && p[-1] != '\n') --p;
    return p;
  }

  // Parses a block node (mapping or sequence) whose entries are indented
  // exactly `indent` columns. Dispatches based on the first non-blank
  // character of the current line.
  //
  // If `at_line_start` is false, `m_p` is already positioned exactly at
  // column `indent` of the *current* line (e.g. right after a sequence
  // dash's "- " prefix, as in "- a: 1" or "- - 1") — the usual
  // skip-to-line-start / indentation check is skipped for the first entry.
  ERL_NIF_TERM parse_block(size_t indent, bool at_line_start = true) {
    DepthGuard guard(this);
    if (!guard.ok()) { m_err = "exceeded maximum nesting depth"; return 0; }

    const char* p;
    if (at_line_start) {
      skip_blank_and_comment_lines();
      if (at_end()) return m_opts.null_term;
      if (peek_indent() != indent) return m_opts.null_term;
      p = m_p + indent;
    } else {
      p = m_p;
    }

    // A flow collection on its own line ("a:\n  [1, 2]").
    if (p < m_end && (*p == '[' || *p == '{')) {
      m_p = p;
      ERL_NIF_TERM v = parse_flow_node();
      if (!v) return 0;
      if (!check_flow_line_end()) return 0;
      return v;
    }

    if (p < m_end && *p == '-' && (p + 1 >= m_end || is_blank(p[1]) || is_break(p[1])))
      return parse_sequence(indent, at_line_start);
    return parse_mapping(indent, at_line_start);
  }

  // Parses a block sequence: a run of `- item` lines all indented at `indent`.
  // See parse_block for the meaning of `at_line_start`.
  ERL_NIF_TERM parse_sequence(size_t indent, bool at_line_start = true) {
    SmallTermVec<16> items;
    bool first = !at_line_start;
    for (;;) {
      const char* line_start_p = m_p;
      if (!first) {
        skip_blank_and_comment_lines();
        line_start_p = m_p;
        if (at_end() || peek_indent() != indent) break;
        m_p += indent;
      }
      first = false;
      const char* p = m_p;
      if (!(p < m_end && *p == '-' && (p + 1 >= m_end || is_blank(p[1]) || is_break(p[1])))) {
        m_p = line_start_p;
        break;
      }
      m_p = p + 1; // consume '-'
      skip_blanks();

      // The item's indentation bound is the dash column: continuation lines
      // of plain scalars, nested blocks on following lines, and block
      // scalar content all only need to be indented past the dash
      // ("- foo\n  bar" folds; "- |\n  x" is content; "-\n a: 1" nests).
      ERL_NIF_TERM v = parse_node(indent);
      if (!v) return 0;
      items.push_back(v);
    }
    return enif_make_list_from_array(m_env, items.data(), unsigned(items.size()));
  }

  // Parses a block mapping: a run of `key: value` lines all indented at
  // `indent`. See parse_block for the meaning of `at_line_start`.
  ERL_NIF_TERM parse_mapping(size_t indent, bool at_line_start = true) {
    SmallTermVec<16> ks, vs;
    bool first = !at_line_start;
    for (;;) {
      const char* line_start_p = m_p;
      if (!first) {
        skip_blank_and_comment_lines();
        line_start_p = m_p;
        if (at_end() || peek_indent() != indent) break;
        m_p += indent;
      }
      first = false;
      if (m_p < m_end && *m_p == '-' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1])))
        { m_p = line_start_p; break; } // sequence item at this indent — not part of this mapping

      // Parse the key (plain or quoted scalar, up to ':').
      ERL_NIF_TERM key_term;
      std::string scratch;
      if (*m_p == '[' || *m_p == '{') {
        m_err = "flow collection keys are not supported";
        return 0;
      }
      if (*m_p == '\'') {
        std::string out;
        if (!read_single_quoted(out, indent)) return 0;
        key_term = make_key_term(out);
      } else if (*m_p == '"') {
        std::string out;
        if (!read_double_quoted(out, indent)) return 0;
        key_term = make_key_term(out);
      } else {
        std::string_view k = read_plain_key(indent);
        if (m_err.size()) return 0;
        key_term = make_key_term(k);
      }

      skip_blanks();
      if (at_end() || *m_p != ':') { m_err = "expected ':' after mapping key"; return 0; }
      ++m_p;

      ERL_NIF_TERM val = parse_node(indent);
      if (!val) return 0;

      ks.push_back(key_term);
      vs.push_back(val);
    }

    [[maybe_unused]] auto map = vs.to_erl_map<true>(m_env, ks);
    assert(map != 0);
    return map;
  }

  // Reads a plain scalar that is to be used as a mapping key: stops at the
  // first ':' followed by space/EOL/EOF (the key/value separator), without
  // the multi-line folding plain scalars otherwise allow (mapping keys are
  // single-line in block context for Step 1).
  std::string_view read_plain_key(size_t /*indent*/) {
    const char* seg_start = m_p;
    const char* last_nonblank = m_p;
    while (m_p < m_end && !is_break(*m_p)) {
      char c = *m_p;
      if (c == ':' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1])))
        break;
      if (c == '#' && (m_p == seg_start || is_blank(m_p[-1])))
        break;
      if (!is_blank(c)) last_nonblank = m_p + 1;
      ++m_p;
    }
    if (seg_start == last_nonblank) { m_err = "empty mapping key"; }
    return std::string_view(seg_start, last_nonblank - seg_start);
  }

  // -------------------------------------------------------------------------
  // Flow-style parsing ({...} mappings, [...] sequences)
  // -------------------------------------------------------------------------

  // Skips whitespace inside a flow collection: blanks, line breaks, and
  // comments (a '#' preceded by whitespace or at the start of input).
  void skip_flow_ws() {
    for (;;) {
      skip_blanks();
      if (at_end()) return;
      char c = *m_p;
      if (is_break(c)) { skip_break(); continue; }
      if (c == '#' && (m_p == m_beg || is_blank(m_p[-1]) || is_break(m_p[-1]))) {
        skip_comment_to_eol();
        continue;
      }
      return;
    }
  }

  // Reads a plain scalar in flow context. In addition to the block-context
  // terminators (': ', ' #'), flow indicators (',', '[', ']', '{', '}')
  // terminate the scalar, and ':' is a terminator when followed by a flow
  // indicator as well as by whitespace/end (so `a:1` stays one scalar, per
  // spec). May span lines, folded like block plain scalars.
  std::string_view read_flow_plain_scalar(std::string& scratch) {
    std::vector<std::string_view> lines;

    for (;;) {
      const char* seg_start = m_p;
      const char* last_nonblank = m_p;
      bool seg_has_content = false;

      bool stop = false;
      while (m_p < m_end && !is_break(*m_p)) {
        char c = *m_p;
        if (is_flow_indicator(c)) { stop = true; break; }
        if (c == ':' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1]) ||
                         is_flow_indicator(m_p[1]))) {
          stop = true; break;
        }
        if (c == '#' && (m_p == seg_start || is_blank(m_p[-1]))) {
          stop = true; break;
        }
        if (!is_blank(c)) { last_nonblank = m_p + 1; seg_has_content = true; }
        ++m_p;
      }

      if (seg_has_content)
        lines.emplace_back(seg_start, last_nonblank - seg_start);

      if (stop || at_end()) break;

      // Line break inside the scalar: fold. Blank lines become empty entries.
      skip_break();
      for (;;) {
        const char* probe = m_p;
        while (probe < m_end && is_blank(*probe)) ++probe;
        if (probe < m_end && is_break(*probe)) {
          lines.emplace_back();
          m_p = probe;
          skip_break();
          continue;
        }
        m_p = probe;
        break;
      }
      if (at_end()) break;
    }

    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.empty()) return std::string_view();
    if (lines.size() == 1) return lines[0];
    fold_lines(lines, scratch);
    return scratch;
  }

  // Verifies the start of a flow scalar isn't a block construct (block
  // collections and block scalars are not allowed inside flow collections
  // per spec — error rather than mis-parse). Returns false and sets m_err
  // if it is.
  bool check_no_block_in_flow() {
    char c = *m_p;
    if ((c == '-' || c == '?') &&
        (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1]))) [[unlikely]] {
      m_err = "block entries are not allowed in flow context";
      return false;
    }
    if (c != '|' && c != '>') [[likely]]
      return true;

    m_err = "block scalars are not allowed in flow context";
    return false;
  }

  // Parses a single node in flow context: a nested flow collection, a quoted
  // scalar, or a plain scalar. m_p may be at leading whitespace.
  ERL_NIF_TERM parse_flow_node() {
    skip_flow_ws();
    if (at_end()) [[unlikely]] { m_err = "unexpected end of input in flow context"; return 0; }
    char c = *m_p;
    if (c == '&') {
      std::string name;
      if (!read_anchor_name(name)) return 0;
      ERL_NIF_TERM v = parse_flow_node();
      if (!v) return 0;
      m_anchors[name] = v;
      return v;
    }
    if (c == '*') return resolve_alias();
    if (c == '[') return parse_flow_sequence();
    if (c == '{') return parse_flow_mapping();
    if (c == '\'') {
      std::string out;
      if (!read_single_quoted(out, 0)) return 0;
      return make_binary(m_env, out);
    }
    if (c == '"') {
      std::string out;
      if (!read_double_quoted(out, 0)) return 0;
      return make_binary(m_env, out);
    }
    if (!check_no_block_in_flow()) return 0;
    std::string scratch;
    std::string_view s = read_flow_plain_scalar(scratch);
    return resolve_plain_scalar(s);
  }

  // Parses one flow-sequence entry. Supports the single-pair mapping
  // shorthand `[key: value, ...]`, where the entry decodes to a one-entry
  // map (PyYAML-compatible: `[a: 1, b: 2]` -> [#{a=>1}, #{b=>2}]).
  ERL_NIF_TERM parse_flow_seq_entry() {
    char c = *m_p;
    // Anchored or aliased entries take the plain-node path (an anchored
    // single-pair `[&a k: v]` is not supported).
    if (c == '[' || c == '{' || c == '&' || c == '*') return parse_flow_node();

    bool quoted = (c == '\'' || c == '"');
    std::string qout;
    std::string scratch;
    std::string_view s;
    if (c == '\'') {
      if (!read_single_quoted(qout, 0)) return 0;
      s = qout;
    } else if (c == '"') {
      if (!read_double_quoted(qout, 0)) return 0;
      s = qout;
    } else {
      if (!check_no_block_in_flow()) return 0;
      s = read_flow_plain_scalar(scratch);
    }

    if (quoted) skip_blanks();
    if (m_p < m_end && *m_p == ':' &&
        (quoted || m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1]) ||
         is_flow_indicator(m_p[1]))) {
      // Single-pair mapping entry.
      ++m_p;
      skip_flow_ws();
      ERL_NIF_TERM val = m_opts.null_term;
      if (at_end()) [[unlikely]] { m_err = "unterminated flow sequence"; return 0; }
      if (*m_p != ',' && *m_p != ']') {
        val = parse_flow_node();
        if (!val) return 0;
      }
      ERL_NIF_TERM map = enif_make_new_map(m_env), next;
      enif_make_map_put(m_env, map, make_key_term(s), val, &next);
      return next;
    }

    return quoted ? make_binary(m_env, s) : resolve_plain_scalar(s);
  }

  // Parses a flow sequence `[a, b, ...]`. m_p is at the opening '['; on
  // success m_p is past the closing ']'. Trailing commas are allowed.
  ERL_NIF_TERM parse_flow_sequence() {
    DepthGuard guard(this);
    if (!guard.ok()) [[unlikely]] { m_err = "exceeded maximum nesting depth"; return 0; }

    ++m_p; // consume '['
    SmallTermVec<16> items;
    for (;;) {
      skip_flow_ws();
      if (at_end()) [[unlikely]] { m_err = "unterminated flow sequence"; return 0; }
      if (*m_p == ']') { ++m_p; break; }
      if (*m_p == ',') [[unlikely]] { m_err = "unexpected ',' in flow sequence"; return 0; }

      ERL_NIF_TERM v = parse_flow_seq_entry();
      if (!v) return 0;
      items.push_back(v);

      skip_flow_ws();
      if (at_end()) [[unlikely]] { m_err = "unterminated flow sequence"; return 0; }
      if (*m_p == ',') { ++m_p; continue; }
      if (*m_p == ']') [[likely]] { ++m_p; break; }
      m_err = "expected ',' or ']' in flow sequence";
      return 0;
    }
    return enif_make_list_from_array(m_env, items.data(), unsigned(items.size()));
  }

  // Parses a flow mapping `{k: v, ...}`. m_p is at the opening '{'; on
  // success m_p is past the closing '}'. Trailing commas are allowed; a key
  // without ': value' gets a null value (`{a, b}`).
  ERL_NIF_TERM parse_flow_mapping() {
    DepthGuard guard(this);
    if (!guard.ok()) [[unlikely]] { m_err = "exceeded maximum nesting depth"; return 0; }

    ++m_p; // consume '{'
    SmallTermVec<16> ks, vs;
    for (;;) {
      skip_flow_ws();
      if (at_end()) [[unlikely]] { m_err = "unterminated flow mapping"; return 0; }
      if (*m_p == '}') { ++m_p; break; }
      if (*m_p == ',') [[unlikely]] { m_err = "unexpected ',' in flow mapping"; return 0; }

      // Key: a plain or quoted scalar (collection/complex keys unsupported).
      char c = *m_p;
      ERL_NIF_TERM key_term;
      bool quoted = (c == '\'' || c == '"');
      if (c == '[' || c == '{') [[unlikely]] {
        m_err = "flow collection keys are not supported";
        return 0;
      } else if (c == '\'') {
        std::string out;
        if (!read_single_quoted(out, 0)) return 0;
        key_term = make_key_term(out);
      } else if (c == '"') {
        std::string out;
        if (!read_double_quoted(out, 0)) return 0;
        key_term = make_key_term(out);
      } else {
        if (!check_no_block_in_flow()) return 0;
        std::string scratch;
        std::string_view s = read_flow_plain_scalar(scratch);
        if (s.empty() && (at_end() || *m_p != ':')) [[unlikely]] {
          m_err = "expected mapping key in flow mapping";
          return 0;
        }
        key_term = make_key_term(s);
      }

      if (quoted) skip_blanks();
      ERL_NIF_TERM val = m_opts.null_term;
      if (m_p < m_end && *m_p == ':') {
        ++m_p;
        skip_flow_ws();
        if (at_end()) [[unlikely]] { m_err = "unterminated flow mapping"; return 0; }
        if (*m_p != ',' && *m_p != '}') {
          val = parse_flow_node();
          if (!val) return 0;
        }
      }
      ks.push_back(key_term);
      vs.push_back(val);

      skip_flow_ws();
      if (at_end()) [[unlikely]] { m_err = "unterminated flow mapping"; return 0; }
      if (*m_p == ',') { ++m_p; continue; }
      if (*m_p == '}') { ++m_p; break; }
      m_err = "expected ',' or '}' in flow mapping";
      return 0;
    }

    [[maybe_unused]] auto map = vs.to_erl_map<true>(m_env, ks);
    assert(map);
    return map;
  }

  // After an inline flow collection in block context, only blanks and a
  // comment may remain on the line.
  bool check_flow_line_end() {
    skip_blanks();
    if (!at_end() && *m_p == '#') skip_comment_to_eol();
    if (at_end() || is_break(*m_p)) [[likely]]
      return true;

    m_err = "unexpected content after flow collection";
    return false;
  }

  // -------------------------------------------------------------------------
  // Entry point
  // -------------------------------------------------------------------------

  std::tuple<bool, ERL_NIF_TERM> decode() {
    skip_blank_and_comment_lines();
    if (at_end()) [[unlikely]]
      return std::make_tuple(true, m_opts.null_term);

    size_t indent = peek_indent();
    ERL_NIF_TERM result;
    const char* p = m_p + indent;
    if (p < m_end && (*p == '&' || *p == '*')) {
      // Root-node anchors are useless in single-document mode (nothing can
      // alias the root before it completes) — reject rather than mis-parse.
      m_err = "top-level anchors/aliases are not supported";
      result = 0;
    }
    else if (p < m_end && (*p == '|' || *p == '>')) {
      // Top-level block scalar document.
      m_p = p;
      result = read_block_scalar(*p == '>', indent);
    }
    else if (p < m_end && (*p == '[' || *p == '{')) {
      // Top-level flow document.
      m_p = p;
      result = parse_flow_node();
    }
    else if (p < m_end && *p == '-' && (p + 1 >= m_end || is_blank(p[1]) || is_break(p[1])))
      result = parse_sequence(indent);
    else {
      // Could be a single top-level scalar document.
      m_p += indent;
      const char* save = m_p;
      // Try mapping first: if the line contains "key:" at top level.
      if (looks_like_mapping_line()) {
        m_p = save;
        result = parse_mapping(indent, /*at_line_start=*/false);
      } else {
        result = parse_scalar(indent);
        skip_blank_and_comment_lines();
      }
    }

    if (!result) [[unlikely]] {
      std::string msg = m_err.empty()
        ? ("YAML parse error at offset " + std::to_string(m_p - m_beg))
        : (m_err + " at offset " + std::to_string(m_p - m_beg));
      return std::make_tuple(false, make_binary(m_env, msg));
    }

    skip_blank_and_comment_lines();
    if (!at_end()) [[unlikely]] {
      std::string msg = "trailing content at offset " + std::to_string(m_p - m_beg);
      return std::make_tuple(false, make_binary(m_env, msg));
    }

    return std::make_tuple(true, result);
  }

  // Heuristic: does the current line, read as a plain/quoted scalar key,
  // contain a top-level "key:" / "key: value" separator (i.e. is this a
  // mapping key line)? Restores m_p.
  bool looks_like_mapping_line() {
    const char* save = m_p;
    bool found = false;
    if (m_p < m_end && (*m_p == '\'' || *m_p == '"')) {
      char q = *m_p; ++m_p;
      while (m_p < m_end && !is_break(*m_p)) {
        if (*m_p == q) {
          if (q == '\'' && m_p + 1 < m_end && m_p[1] == '\'') { m_p += 2; continue; }
          ++m_p;
          break;
        }
        ++m_p;
      }
      skip_blanks();
      if (m_p < m_end && *m_p == ':' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1])))
        found = true;
    } else {
      const char* seg_start = m_p;
      while (m_p < m_end && !is_break(*m_p)) {
        char c = *m_p;
        if (c == ':' && (m_p + 1 >= m_end || is_blank(m_p[1]) || is_break(m_p[1]))) {
          found = true; break;
        }
        if (c == '#' && (m_p == seg_start || is_blank(m_p[-1]))) break;
        ++m_p;
      }
    }
    m_p = save;
    return found;
  }
};

//-----------------------------------------------------------------------------
// Erlang-term -> YAML encoder (block style, 2-space indentation)
//
// Mirrors the JSON encoder's term-dispatch shape, but writes block-style
// YAML: nested mappings/sequences are indented two spaces per level, with
// sequences placed at the *same* indentation as the mapping key that owns
// them (the style produced by PyYAML/libyaml and accepted by the decoder
// above). Empty maps/sequences fall back to flow style (`{}` / `[]`) since
// block style has no representation for them.
//
// Scalars are emitted in plain style where unambiguous, single-quoted where
// quoting is needed but the content is plain UTF-8 with no control
// characters, and double-quoted (with full escaping) otherwise.
//-----------------------------------------------------------------------------

struct YamlEncoder {
  ErlNifEnv*            m_env;
  const YamlEncodeOpts& m_opts;
  OutBuf&               m_out;
  char                  m_atom_buf[256];
  const char*           m_err;
  ERL_NIF_TERM          m_err_term;

  // -------------------------------------------------------------------------
  // Scalar string analysis / emission
  // -------------------------------------------------------------------------

  // Returns true if `s`, written unquoted, would be re-read by the core
  // schema as something other than a string (null/bool/number/special
  // float), per resolve_plain_scalar's rules in the decoder above.
  //
  // Dispatches on the first character first: every literal this function
  // recognizes (and every numeric scalar) starts with one of
  // "~ntfyYNoO.+-0123456789", so plain words like "Alice" or "NYC" bail out
  // after a single branch instead of running the full literal/number checks.
  static bool looks_like_non_string_scalar(std::string_view s) {
    if (s.empty()) return true;
    auto c = s.front();
    switch (c) {
      case '~':
        return s == "~";
      case 'n': return s == "null"  || s == "no";
      case 'N': return s == "Null"  || s == "NULL" || s == "No" || s == "NO";
      case 't': return s == "true";
      case 'T': return s == "True"  || s == "TRUE";
      case 'f': return s == "false";
      case 'F': return s == "False" || s == "FALSE";
      case 'y': return s == "yes";
      case 'Y': return s == "Yes"   || s == "YES";
      case 'O': return s == "On"    || s == "ON" || s == "Off" || s == "OFF";
      case 'o': return s == "on"    || s == "off";
      case '.':
        return s == ".inf" || s == ".Inf" || s == ".INF" ||
               s == ".nan" || s == ".NaN" || s == ".NAN";
      case '+': case '-':
        if (s == "+.inf" || s == "+.Inf" || s == "+.INF" ||
            s == "-.inf" || s == "-.Inf" || s == "-.INF")
          return true;
        break;
      default:
        // Is c in range '0'-'9'?
        if ((unsigned char)(c - '0') <= 9)
          break;
        return false;
    }

    // Looks like a YAML core-schema int/float (decimal, hex, octal)?
    const char* p = s.data();
    const char* end = p + s.size();
    if (*p == '+' || *p == '-') ++p;
    if (p == end) return false;
    if (*p == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O')) {
      for (const char* q = p + 2; q < end; ++q)
        if (!std::isxdigit((unsigned char)*q)) return false;
      return p + 2 < end;
    }
    bool has_digit = false, has_dot = false, has_exp = false;
    for (const char* q = p; q < end; ++q) {
      char c = *q;
      if (c >= '0' && c <= '9') { has_digit = true; continue; }
      if (c == '.' && !has_dot && !has_exp) { has_dot = true; continue; }
      if ((c == 'e' || c == 'E') && has_digit && !has_exp) {
        has_exp = true;
        if (q + 1 < end && (q[1] == '+' || q[1] == '-')) ++q;
        continue;
      }
      return false;
    }
    return has_digit;
  }

  static inline bool is_indicator_char(char c) {
    static constexpr auto table = [] {
      std::array<bool, 256> t{};
      for (char ind : {'-','?',':',',','[',']','{','}','#','&','*','!','|','>','\'','"','%','@','`'})
        t[(unsigned char)ind] = true;
      return t;
    }();
    return table[(unsigned char)c];
  }

  // Returns true if `s` contains a byte that forces double-quoting (control
  // characters other than plain printable text — single-quoted scalars
  // cannot represent these).  Uses SIMD to scan for c < 0x20 (unsigned)
  // via the bias trick: (c ^ 0x80) < 0xA0 in signed comparison.
  static bool needs_double_quote(std::string_view s) noexcept {
    const char* p   = s.data();
    const char* end = p + s.size();
#if defined(__AVX2__)
    {
      const __m256i vbias = _mm256_set1_epi8(-128);
      const __m256i vcmp  = _mm256_set1_epi8(-96);   // 0xA0 as signed
      while (p + 32 <= end) {
        __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(
          _mm256_cmpgt_epi8(vcmp, _mm256_xor_si256(v, vbias)));
        if (mask) return true;
        p += 32;
      }
    }
#endif
#if defined(__SSE2__)
    {
      const __m128i vbias = _mm_set1_epi8(-128);
      const __m128i vcmp  = _mm_set1_epi8(-96);
      while (p + 16 <= end) {
        __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        unsigned mask = (unsigned)_mm_movemask_epi8(
          _mm_cmpgt_epi8(vcmp, _mm_xor_si128(v, vbias)));
        if (mask) return true;
        p += 16;
      }
    }
#endif
    while (p < end) { if ((unsigned char)*p < 0x20) return true; ++p; }
    return false;
  }

  // Returns true if `s` can be written as a YAML plain scalar without any
  // quoting, in the contexts this encoder uses (block mapping value / block
  // sequence item, single-line content only).
  static bool is_safe_plain_scalar(std::string_view s) {
    if (s.empty()) return false;
    if (looks_like_non_string_scalar(s)) return false;

    char first = s.front();
    if (is_indicator_char(first)) return false;
    // A leading '-', '?', ':' is always disallowed above for simplicity
    // (matches common emitters and avoids edge cases).
    if (first == ' ' || s.back() == ' ') return false;

    // Single pass: control chars force double-quoting; ": " / trailing ':'
    // and " #" are plain-scalar-ending sequences.
    for (size_t i = 0; i < s.size(); ++i) {
      unsigned char c = s[i];
      if (c < 0x20) return false;
      if (c == ':' && (i + 1 == s.size() || s[i+1] == ' ')) return false;
      if (c == '#' && i > 0 && s[i-1] == ' ') return false;
    }
    return true;
  }

  // Emit `s` single-quoted, doubling embedded single quotes.
  // SIMD locates each '\'' for bulk-copy runs between them.
  void emit_single_quoted(std::string_view s) {
    m_out.push('\'');
    const char* p   = s.data();
    const char* end = p + s.size();
    for (;;) {
      const char* q = find_byte(p, end, '\'');
      m_out.push(p, q - p);   // bulk-copy bytes up to (not including) the quote
      if (q >= end) break;
      m_out.push("''", 2);    // double the embedded single quote
      p = q + 1;
    }
    m_out.push('\'');
  }

  // Emit `s` double-quoted with full C-style escaping (the JSON-compatible
  // escape subset is always valid YAML).
  // Pre-reserves worst-case space then writes via raw pointer — same strategy
  // as json_escape_string in glazer_json.hpp.  ESCAPE_TAB replaces the switch.
  // Uses find_escape_pos (from glazer_json.hpp, defined below) which detects
  // all control chars < 0x20, '"', and '\' in bulk via NEON/AVX2/SSE2/table.
  void emit_double_quoted(std::string_view s) {
    m_out.ensure(s.size() * 6 + 2);
    char* dst       = m_out.m_data + m_out.m_len;
    *dst++          = '"';
    const char* p   = s.data();
    const char* end = p + s.size();
    while (p < end) {
      const char* special = find_escape_pos(p, end);
      size_t run = static_cast<size_t>(special - p);
      if (run) { memcpy(dst, p, run); dst += run; }
      p = special;
      if (p >= end) break;
      const EscapeEntry& e = ESCAPE_TAB[(unsigned char)*p++];
      memcpy(dst, e.seq, e.len);
      dst += e.len;
    }
    *dst++ = '"';
    m_out.m_len = static_cast<size_t>(dst - m_out.m_data);
  }

  // Emit a string scalar, choosing plain / single-quoted / double-quoted.
  void emit_string_scalar(std::string_view s) {
    if (is_safe_plain_scalar(s))    m_out.push(s);
    else if (needs_double_quote(s)) emit_double_quoted(s);
    else                             emit_single_quoted(s);
  }

  // -------------------------------------------------------------------------
  // Top-level entry point
  // -------------------------------------------------------------------------

  bool encode(ERL_NIF_TERM term) {
    if (!encode_node(term, 0)) return false;
    m_out.push('\n');
    return true;
  }

  // -------------------------------------------------------------------------
  // Node dispatch
  //
  // `indent` is the column of the container that owns the node currently
  // being encoded. Scalars are written inline; collections recurse with
  // indent+2 (mappings) or the same indent (sequences nested directly under
  // a mapping key, per the same-indent convention the decoder accepts).
  // -------------------------------------------------------------------------

  bool encode_node(ERL_NIF_TERM term, size_t indent) {
    switch (enif_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, term, &bin)) return false;
        emit_string_scalar({reinterpret_cast<const char*>(bin.data), bin.size});
        return true;
      }

      case ERL_NIF_TERM_TYPE_INTEGER:
        return glz::BigInt::encode(m_env, term, m_out);

      case ERL_NIF_TERM_TYPE_FLOAT:
        return encode_float(term);

      case ERL_NIF_TERM_TYPE_ATOM:
        return encode_atom(term);

      case ERL_NIF_TERM_TYPE_MAP: {
        size_t size;
        enif_get_map_size(m_env, term, &size);
        if (size == 0) { m_out.push("{}", 2); return true; }
        return encode_map(term, indent);
      }

      case ERL_NIF_TERM_TYPE_LIST:
        if (enif_is_empty_list(m_env, term)) { m_out.push("[]", 2); return true; }
        return encode_sequence(term, indent);

      case ERL_NIF_TERM_TYPE_TUPLE: {
        // {[{K,V}...]} proplist -> mapping
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(m_env, term, &arity, &tp);
        if (arity != 1 || !enif_is_list(m_env, tp[0])) [[unlikely]]
          return error("tuple is not an object", term);

        if (enif_is_empty_list(m_env, tp[0])) {
          m_out.push("{}", 2);
          return true;
        }
        return encode_proplist(tp[0], indent);
      }

      default:
        return error("unsupported term type", term);
    }
  }

  bool encode_float(ERL_NIF_TERM term) {
    double d;
    if (!enif_get_double(m_env, term, &d)) return false;
    if (std::isnan(d)) { m_out.push(".nan", 4); return true; }
    if (std::isinf(d)) {
      if (d < 0) m_out.push("-.inf", 5);
      else       m_out.push(".inf", 4);
      return true;
    }
    char buf[32];
    auto [e, ec] = std::to_chars(buf, buf+32, d, std::chars_format::general);
    if (ec == std::errc{}) [[likely]] {
      bool has_dot = false;
      for (char* p = buf; p < e; ++p)
        if (*p == '.' || *p == 'e' || *p == 'E') { has_dot = true; break; }
      m_out.push(buf, e - buf);
      if (!has_dot) m_out.push(".0", 2);
    } else {
      int n = snprintf(buf, sizeof(buf), "%.17g", d);
      m_out.push(buf, n);
    }
    return true;
  }

  bool encode_atom(ERL_NIF_TERM term) {
    if (m_opts.null_term && enif_is_identical(term, m_opts.null_term)) { m_out.push("null", 4); return true; }
    if (enif_is_identical(term, AM_TRUE))         { m_out.push("true",  4); return true; }
    if (enif_is_identical(term, AM_FALSE))        { m_out.push("false", 5); return true; }
    if (enif_is_identical(term, AM_NULL))         { m_out.push("null",  4); return true; }
    if (enif_is_identical(term, AM_NIL))          { m_out.push("null",  4); return true; }
    if (enif_is_identical(term, AM_INFINITY))     { m_out.push(".inf",  4); return true; }
    if (enif_is_identical(term, AM_NEG_INFINITY)) { m_out.push("-.inf", 5); return true; }
    if (enif_is_identical(term, AM_NAN))          { m_out.push(".nan",  4); return true; }
    std::string_view sv;
    if (!atom_to_sv(m_env, term, m_atom_buf, sizeof(m_atom_buf), sv)) return false;
    emit_string_scalar(sv);
    return true;
  }

  static void push_indent(OutBuf& out, size_t n) {
    static constexpr char SPACES[] = "                                ";  // 32 spaces
    while (n >= sizeof(SPACES) - 1) { out.push(SPACES, sizeof(SPACES) - 1); n -= sizeof(SPACES) - 1; }
    if (n) out.push(SPACES, n);
  }

  // Encodes the value of a mapping key at the given indent (the column of
  // the key itself). Scalars are written inline after "key: "; collections
  // start on the next line(s): nested mappings indent to indent+2, nested
  // sequences are placed at the same `indent` (same-indent convention).
  bool encode_map_value(ERL_NIF_TERM term, size_t indent) {
    switch (enif_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_MAP: {
        size_t size;
        enif_get_map_size(m_env, term, &size);
        if (size == 0) { m_out.push(" {}", 3); return true; }
        m_out.push('\n');
        return encode_map(term, indent + 2);
      }
      case ERL_NIF_TERM_TYPE_LIST: {
        if (enif_is_empty_list(m_env, term)) { m_out.push(" []", 3); return true; }
        m_out.push('\n');
        return encode_sequence(term, indent);
      }
      case ERL_NIF_TERM_TYPE_TUPLE: {
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(m_env, term, &arity, &tp);
        if (arity != 1 || !enif_is_list(m_env, tp[0])) [[unlikely]]
          return error("tuple is not an object", term);

        if (enif_is_empty_list(m_env, tp[0])) {
          m_out.push(" {}", 3);
          return true;
        }
        m_out.push('\n');
        return encode_proplist(tp[0], indent + 2);
      }
      default:
        m_out.push(' ');
        return encode_node(term, indent);
    }
  }

  // -------------------------------------------------------------------------
  // Mappings / sequences
  // -------------------------------------------------------------------------

  // `at_line_start` is false when the first entry continues a line already
  // begun by the caller (e.g. right after "- " for a mapping that is a
  // sequence item) — in that case the first entry's indent is skipped.
  bool encode_map(ERL_NIF_TERM term, size_t indent, bool at_line_start = true) {
    auto iter = MapIterator::create(m_env, term);
    if (!iter) return false;

    ERL_NIF_TERM k, v;
    bool first = true;
    while (iter->get_pair(&k, &v)) {
      if (!first || at_line_start) { if (!first) m_out.push('\n'); push_indent(m_out, indent); }
      first = false;
      if (!encode_key(k) || !encode_map_value(v, indent)) return false;
      iter->next();
    }
    return true;
  }

  bool encode_proplist(ERL_NIF_TERM list, size_t indent, bool at_line_start = true) {
    ERL_NIF_TERM h, t = list;
    bool first = true;
    while (enif_get_list_cell(m_env, t, &h, &t)) {
      int pa; const ERL_NIF_TERM* pp;
      if (!enif_get_tuple(m_env, h, &pa, &pp) || pa != 2) [[unlikely]]
        return error("proplist element is not a 2-tuple", h);
      if (!first || at_line_start) { if (!first) m_out.push('\n'); push_indent(m_out, indent); }
      first = false;
      if (!encode_key(pp[0]) || !encode_map_value(pp[1], indent)) return false;
    }
    if (!enif_is_empty_list(m_env, t)) [[unlikely]]
      return error("cannot encode improper list as proplist", t);
    return true;
  }

  bool encode_key(ERL_NIF_TERM k) {
    switch (enif_term_type(m_env, k)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, k, &bin)) return false;
        emit_string_scalar({reinterpret_cast<const char*>(bin.data), bin.size});
        break;
      }
      case ERL_NIF_TERM_TYPE_ATOM: {
        std::string_view sv;
        if (!atom_to_sv(m_env, k, m_atom_buf, sizeof(m_atom_buf), sv)) return false;
        emit_string_scalar(sv);
        break;
      }
      case ERL_NIF_TERM_TYPE_INTEGER:
        if (!glz::BigInt::encode(m_env, k, m_out)) return false;
        break;
      default:
        return error("unsupported map key type", k);
    }
    m_out.push(':');
    return true;
  }

  // Sequence items are placed at `indent` (the same column as the mapping
  // key that owns the sequence, or 0 at the document root), each starting
  // with "- ". Nested mappings under a "- " continue at indent+2.
  bool encode_sequence(ERL_NIF_TERM list, size_t indent) {
    ERL_NIF_TERM h, t = list;
    bool first = true;
    while (enif_get_list_cell(m_env, t, &h, &t)) {
      if (!first) m_out.push('\n');
      first = false;
      push_indent(m_out, indent);
      m_out.push("- ", 2);
      if (!encode_item(h, indent + 2)) return false;
    }
    if (!enif_is_empty_list(m_env, t)) [[unlikely]]
      return error("cannot encode improper list as sequence", t);
    return true;
  }

  // Encodes a sequence item, which sits right after "- " on the dash's line.
  bool encode_item(ERL_NIF_TERM term, size_t indent) {
    switch (enif_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_MAP: {
        size_t size;
        enif_get_map_size(m_env, term, &size);
        if (size == 0) { m_out.push("{}", 2); return true; }
        return encode_map(term, indent, false);
      }
      case ERL_NIF_TERM_TYPE_LIST: {
        if (enif_is_empty_list(m_env, term)) { m_out.push("[]", 2); return true; }
        // A nested sequence item: "- - ..." (the inner sequence's first
        // dash immediately follows this item's "- ").
        return encode_sequence_inline(term, indent);
      }
      case ERL_NIF_TERM_TYPE_TUPLE: {
        int arity; const ERL_NIF_TERM* tp;
        enif_get_tuple(m_env, term, &arity, &tp);
        if (arity == 1 && enif_is_list(m_env, tp[0])) {
          if (enif_is_empty_list(m_env, tp[0])) { m_out.push("{}", 2); return true; }
          return encode_proplist(tp[0], indent, false);
        }
        return false;
      }
      default:
        return encode_node(term, indent);
    }
  }

  // A sequence nested directly inside another sequence's item: the first
  // "- " has already been written by the caller; subsequent items continue
  // on their own line at `indent` ("- - first\n  - second").
  bool encode_sequence_inline(ERL_NIF_TERM list, size_t indent) {
    ERL_NIF_TERM h, t = list;
    bool first = true;
    while (enif_get_list_cell(m_env, t, &h, &t)) {
      if (!first) { m_out.push('\n'); push_indent(m_out, indent); }
      m_out.push("- ", 2);
      if (!encode_item(h, indent + 2)) return false;
      first = false;
    }
    if (!enif_is_empty_list(m_env, t)) [[unlikely]]
      return error("cannot encode improper list as sequence", t);
    return true;
  }

private:
  template <int N>
  bool error(const char (&err)[N], ERL_NIF_TERM term) {
    m_err      = err;
    m_err_term = term;
    return false;
  }
};

} // namespace glz
