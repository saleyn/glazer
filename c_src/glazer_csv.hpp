// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// CSV decoding/encoding (RFC 4180-style), producing/consuming native Erlang
// terms in a single pass.
//
// Decoding:
//   - Without `headers`: a list of rows, each row a list of binary fields.
//   - With `headers`: the first row becomes the column names, and each
//     subsequent row decodes to a map (or proplist) keyed by those names.
//
// Encoding accepts the same shapes: a list of rows (each row a list of
// binaries/atoms/integers/floats), or — with `headers` — a list of maps
// whose values are written out in the given column order.
//
// Quoting follows RFC 4180: a field is quoted if it contains the delimiter,
// a quote character, or a line break; embedded quotes are doubled.
//-----------------------------------------------------------------------------
#pragma once

#include <cctype>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "fast_float.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_common.hpp"

namespace glz {

//-----------------------------------------------------------------------------
// Per-column field type conversion (`{fields, [Type, ...]}` decode option)
//-----------------------------------------------------------------------------

enum class CSVFieldType {
  none,          // leave as binary (default)
  integer,
  floatp,        // {float, Precision}
  boolean,
  datetime,      // {datetime, InputFormat}
  binary,
  charlist,
  existing_atom,
  atom_list,     // {atom, ExistingAtoms}
};

// What to do with a field that fails to convert to the requested type.
enum class CSVOnFailure {
  binary,         // leave the original binary (default)
  raise,          // fail the whole decode with {invalid_field_value, Row, Col}
  default_value,  // use CSVFieldSpec::default_term
  null,           // use the configured null term (am_null)
};

struct CSVFieldSpec {
  CSVFieldType type        = CSVFieldType::none;
  int          precision   = 0; // for floatp
  int64_t      scale       = 1; // for floatp
  std::string  format;          // for datetime (strptime-like)
  std::vector<std::string> atoms; // for {atom, ExistingAtoms}
  CSVOnFailure on_failure   = CSVOnFailure::binary;
  ERL_NIF_TERM default_term = 0;  // for empty fields, or on_failure == default_value
  bool         has_default  = false;
};

//-----------------------------------------------------------------------------
// Options
//-----------------------------------------------------------------------------

enum class CSVReturnKind { list, map, tuple };

struct CSVDecodeOpts {
  char delimiter            = ',';
  bool headers              = false;         // bare `headers` atom or {headers, ...}
  bool hdr_atom             = false;         // {headers, atom}
  bool hdr_existing_atom    = false;         // {headers, existing_atom}
  bool hdr_charlist         = false;         // {headers, charlist}
  CSVReturnKind return_kind = CSVReturnKind::list; // {return, list | map | tuple}
  bool explicit_headers     = false;         // {headers, [List]}: no header row in data
  ERL_NIF_TERM null_term    = 0;
  std::vector<ERL_NIF_TERM> header_list;     // populated when explicit_headers is true
  std::vector<CSVFieldSpec> fields;
  size_t skip_rows          = 0;             // {skip, N}: skip first N data rows
  size_t limit              = 0;             // {limit, N}: max data rows (0 = unlimited)
  // When true, fields are copied into fresh binaries instead of referencing
  // the input via sub-binary. Use this when decoded fields will outlive the
  // input binary by a large margin: without it, one live field keeps the
  // entire input buffer from being collected.
  bool copy_strings         = false;
};

// Parses a `field_type()`:
//   integer | {float, Precision} | boolean | {datetime, InputFormat}
//   | binary | charlist | existing_atom | {atom, ExistingAtoms :: list(atom())}
static bool parse_csv_field_type(ErlNifEnv* env, ERL_NIF_TERM term, CSVFieldSpec& spec)
{
  if (enif_is_identical(term, AM_INTEGER))       { spec.type = CSVFieldType::integer;       return true; }
  if (enif_is_identical(term, AM_BOOLEAN))       { spec.type = CSVFieldType::boolean;       return true; }
  if (enif_is_identical(term, AM_BINARY))        { spec.type = CSVFieldType::binary;        return true; }
  if (enif_is_identical(term, AM_CHARLIST))      { spec.type = CSVFieldType::charlist;      return true; }
  if (enif_is_identical(term, AM_EXISTING_ATOM)) { spec.type = CSVFieldType::existing_atom; return true; }

  int arity; const ERL_NIF_TERM* tp;
  if (!enif_get_tuple(env, term, &arity, &tp) || arity != 2) return false;

  if (enif_is_identical(tp[0], AM_FLOAT)) {
    int precision;
    if (!enif_get_int(env, tp[1], &precision) || precision < 0) return false;
    spec.type      = CSVFieldType::floatp;
    spec.precision = precision;
    spec.scale     = glz::power(10LL, static_cast<size_t>(precision));
    return true;
  }

  if (enif_is_identical(tp[0], AM_DATETIME)) {
    ErlNifBinary bin;
    if (!enif_inspect_binary(env, tp[1], &bin)) return false;
    spec.type   = CSVFieldType::datetime;
    spec.format.assign(reinterpret_cast<const char*>(bin.data), bin.size);
    return true;
  }

  if (enif_is_identical(tp[0], AM_ATOM)) {
    if (!enif_is_list(env, tp[1])) return false;
    std::vector<std::string> atoms;
    ERL_NIF_TERM ahead, atail = tp[1];
    char buf[256];
    while (enif_get_list_cell(env, atail, &ahead, &atail)) {
      std::string_view sv;
      if (!atom_to_sv(env, ahead, buf, sizeof(buf), sv)) return false;
      atoms.emplace_back(sv);
    }
    spec.type  = CSVFieldType::atom_list;
    spec.atoms = std::move(atoms);
    return true;
  }

  return false;
}

// Parses a single element of `{fields, [FieldType, ...]}`, where:
//   FieldType :: field_type()
//             | #{type => field_type(), default => Term, on_failure => binary | raise | default | null}
//   field_type() :: integer | {float, Precision} | boolean | {datetime, InputFormat}
//   | binary | charlist | existing_atom | {atom, ExistingAtoms :: list(atom())}
static bool parse_csv_field_spec(ErlNifEnv* env, ERL_NIF_TERM term, CSVFieldSpec& spec)
{
  if (!enif_is_map(env, term))
    return parse_csv_field_type(env, term, spec);

  ERL_NIF_TERM type_term;
  if (!enif_get_map_value(env, term, AM_TYPE, &type_term)) return false;
  if (!parse_csv_field_type(env, type_term, spec)) return false;

  ERL_NIF_TERM default_term;
  if (enif_get_map_value(env, term, AM_DEFAULT, &default_term)) {
    spec.default_term = default_term;
    spec.has_default  = true;
  }

  ERL_NIF_TERM on_failure_term;
  if (enif_get_map_value(env, term, AM_ON_FAILURE, &on_failure_term)) {
    if      (enif_is_identical(on_failure_term, AM_BINARY))  spec.on_failure = CSVOnFailure::binary;
    else if (enif_is_identical(on_failure_term, AM_RAISE))   spec.on_failure = CSVOnFailure::raise;
    else if (enif_is_identical(on_failure_term, AM_DEFAULT)) spec.on_failure = CSVOnFailure::default_value;
    else if (enif_is_identical(on_failure_term, AM_NULL))    spec.on_failure = CSVOnFailure::null;
    else return false;
  }

  return true;
}

static bool parse_csv_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, CSVDecodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if (enif_is_identical(head, AM_HEADERS))      { opts.headers      = true; continue; }
    if (enif_is_identical(head, AM_COPY_STRINGS)) { opts.copy_strings = true; continue; }

    int arity; const ERL_NIF_TERM* tp;
    if (!enif_get_tuple(env, head, &arity, &tp) || arity != 2) continue;

    if (enif_is_identical(tp[0], AM_DELIMITER)) {
      int cp;
      if (!enif_get_int(env, tp[1], &cp) || cp <= 0 || cp > 0x7F) return false;
      opts.delimiter = static_cast<char>(cp);
    } else if (enif_is_identical(tp[0], AM_NULL_TERM)) {
      if (!enif_is_atom(env, tp[1])) return false;
      opts.null_term = tp[1];
    } else if (enif_is_identical(tp[0], AM_RETURN)) {
      if      (enif_is_identical(tp[1], AM_MAP))   opts.return_kind = CSVReturnKind::map;
      else if (enif_is_identical(tp[1], AM_TUPLE)) opts.return_kind = CSVReturnKind::tuple;
      else if (enif_is_identical(tp[1], AM_LIST))  opts.return_kind = CSVReturnKind::list;
    } else if (enif_is_identical(tp[0], AM_HEADERS)) {
      // {headers, [List]}  — explicit column names, no header row in data
      // {headers, binary | string} — 1st row → binary keys
      // {headers, atom}            — 1st row → atom keys
      // {headers, existing_atom}   — 1st row → existing-atom keys
      // {headers, charlist}        — 1st row → charlist keys
      if (enif_is_list(env, tp[1])) {
        opts.headers          = true;
        opts.explicit_headers = true;
        ERL_NIF_TERM hhead, htail = tp[1];
        while (enif_get_list_cell(env, htail, &hhead, &htail))
          opts.header_list.push_back(hhead);
      } else if (enif_is_identical(tp[1], AM_BINARY) || enif_is_identical(tp[1], AM_STRING)) {
        opts.headers           = true;
        opts.hdr_atom          = false;
        opts.hdr_existing_atom = false;
        opts.hdr_charlist      = false;
      } else if (enif_is_identical(tp[1], AM_ATOM)) {
        opts.headers           = true;
        opts.hdr_atom          = true;
      } else if (enif_is_identical(tp[1], AM_EXISTING_ATOM)) {
        opts.headers           = true;
        opts.hdr_existing_atom = true;
      } else if (enif_is_identical(tp[1], AM_CHARLIST)) {
        opts.headers           = true;
        opts.hdr_charlist      = true;
      }
    } else if (enif_is_identical(tp[0], AM_SKIP)) {
      // {skip, N} or {skip, {From, To}} (1-based, inclusive)
      ErlNifUInt64 n;
      if (enif_get_uint64(env, tp[1], &n)) {
        opts.skip_rows = static_cast<size_t>(n);
      } else {
        int arity2; const ERL_NIF_TERM* tp2;
        if (enif_get_tuple(env, tp[1], &arity2, &tp2) && arity2 == 2) {
          ErlNifUInt64 from, to;
          if (enif_get_uint64(env, tp2[0], &from) && enif_get_uint64(env, tp2[1], &to)
              && from >= 1 && to >= from) {
            opts.skip_rows = static_cast<size_t>(from - 1);
            opts.limit     = static_cast<size_t>(to - from + 1);
          }
        }
      }
    } else if (enif_is_identical(tp[0], AM_LIMIT)) {
      ErlNifUInt64 n;
      if (enif_get_uint64(env, tp[1], &n))
        opts.limit = static_cast<size_t>(n);
    } else if (enif_is_identical(tp[0], AM_FIELDS)) {
      if (!enif_is_list(env, tp[1])) return false;
      ERL_NIF_TERM fhead, ftail = tp[1];
      std::vector<CSVFieldSpec> fields;
      while (enif_get_list_cell(env, ftail, &fhead, &ftail)) {
        CSVFieldSpec spec;
        if (!parse_csv_field_spec(env, fhead, spec)) return false;
        fields.push_back(spec);
      }
      opts.fields = std::move(fields);
    }
  }

  if (opts.return_kind == CSVReturnKind::map && !opts.headers)
    opts.return_kind = CSVReturnKind::list;

  return true;
}

struct CSVEncodeOpts {
  char delimiter        = ',';
  bool headers          = false;
  bool explicit_headers = false;          // {headers, [List]}: explicit column order
  std::vector<ERL_NIF_TERM> header_list;  // populated when explicit_headers is true
  std::string_view line_ending = "\r\n";
};

static bool parse_csv_encode_opts(ErlNifEnv* env, ERL_NIF_TERM list, CSVEncodeOpts& opts)
{
  ERL_NIF_TERM head, tail = list;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if (enif_is_identical(head, AM_HEADERS)) { opts.headers = true; continue; }

    int arity; const ERL_NIF_TERM* tp;
    if (!enif_get_tuple(env, head, &arity, &tp) || arity != 2) continue;

    if (enif_is_identical(tp[0], AM_DELIMITER)) {
      int cp;
      if (!enif_get_int(env, tp[1], &cp) || cp <= 0 || cp > 0x7F) return false;
      opts.delimiter = static_cast<char>(cp);
    } else if (enif_is_identical(tp[0], AM_LINE_ENDING)) {
      if      (enif_is_identical(tp[1], AM_LF))   opts.line_ending = "\n";
      else if (enif_is_identical(tp[1], AM_CRLF)) opts.line_ending = "\r\n";
      else return false;
    } else if (enif_is_identical(tp[0], AM_HEADERS)) {
      // {headers, [Names]} — explicit column order/names for map-row encoding
      if (!enif_is_list(env, tp[1])) return false;
      opts.headers          = true;
      opts.explicit_headers = true;
      ERL_NIF_TERM hhead, htail = tp[1];
      while (enif_get_list_cell(env, htail, &hhead, &htail))
        opts.header_list.push_back(hhead);
    }
  }
  return true;
}

//-----------------------------------------------------------------------------
// SIMD helpers — field scanners used by both decoder and encoder.
//
// find_field_end:   first of delimiter | '\n' | '\r'  (unquoted-field boundary)
// find_csv_special: first of delimiter | '"' | '\n' | '\r'  (needs-quoting check)
// find_byte (glazer_common.hpp): first '"'            (quoted-field / encoder)
//
// find_field_end and find_csv_special cascade AVX2 → SSE2 → scalar.
// find_byte is the shared single-byte scanner from glazer_common.hpp.
//-----------------------------------------------------------------------------

static const char* find_field_end(const char* p, const char* end, char delim) noexcept
{
#if defined(__ARM_NEON__)
  {
    const uint8x16_t vd = vdupq_n_u8(static_cast<uint8_t>(delim));
    const uint8x16_t vn = vdupq_n_u8('\n');
    const uint8x16_t vr = vdupq_n_u8('\r');
    while (p + 16 <= end) {
      uint8x16_t v   = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      uint8x16_t hit = vorrq_u8(vorrq_u8(vceqq_u8(v, vd), vceqq_u8(v, vn)), vceqq_u8(v, vr));
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
    const __m256i vd = _mm256_set1_epi8(delim);
    const __m256i vn = _mm256_set1_epi8('\n');
    const __m256i vr = _mm256_set1_epi8('\r');
    while (p + 32 <= end) {
      __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, vd), _mm256_cmpeq_epi8(v, vn)),
        _mm256_cmpeq_epi8(v, vr)));
      if (mask) return p + __builtin_ctz(mask);
      p += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i vd = _mm_set1_epi8(delim);
    const __m128i vn = _mm_set1_epi8('\n');
    const __m128i vr = _mm_set1_epi8('\r');
    while (p + 16 <= end) {
      __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, vd), _mm_cmpeq_epi8(v, vn)),
        _mm_cmpeq_epi8(v, vr)));
      if (mask) return p + __builtin_ctz(mask);
      p += 16;
    }
  }
#endif
  while (p < end && *p != delim && *p != '\n' && *p != '\r') ++p;
  return p;
}

// Finds the first byte in [p, end) that requires the field to be quoted:
// the delimiter, a double-quote, a newline, or a carriage return.
static const char* find_csv_special(const char* p, const char* end, char delim) noexcept
{
#if defined(__ARM_NEON__)
  {
    const uint8x16_t vd = vdupq_n_u8(static_cast<uint8_t>(delim));
    const uint8x16_t vq = vdupq_n_u8('"');
    const uint8x16_t vn = vdupq_n_u8('\n');
    const uint8x16_t vr = vdupq_n_u8('\r');
    while (p + 16 <= end) {
      uint8x16_t v   = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      uint8x16_t hit = vorrq_u8(vorrq_u8(vceqq_u8(v, vd), vceqq_u8(v, vq)),
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
    const __m256i vd = _mm256_set1_epi8(delim);
    const __m256i vq = _mm256_set1_epi8('"');
    const __m256i vn = _mm256_set1_epi8('\n');
    const __m256i vr = _mm256_set1_epi8('\r');
    while (p + 32 <= end) {
      __m256i  v    = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, vd), _mm256_cmpeq_epi8(v, vq)),
        _mm256_cmpeq_epi8(v, vn)), _mm256_cmpeq_epi8(v, vr)));
      if (mask) return p + __builtin_ctz(mask);
      p += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
    const __m128i vd = _mm_set1_epi8(delim);
    const __m128i vq = _mm_set1_epi8('"');
    const __m128i vn = _mm_set1_epi8('\n');
    const __m128i vr = _mm_set1_epi8('\r');
    while (p + 16 <= end) {
      __m128i  v    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
      unsigned mask = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, vd), _mm_cmpeq_epi8(v, vq)),
        _mm_cmpeq_epi8(v, vn)), _mm_cmpeq_epi8(v, vr)));
      if (mask) return p + __builtin_ctz(mask);
      p += 16;
    }
  }
#endif
  while (p < end && *p != delim && *p != '"' && *p != '\n' && *p != '\r') ++p;
  return p;
}

//-----------------------------------------------------------------------------
// CSVDecoder
//-----------------------------------------------------------------------------

struct CSVDecoder {
  ErlNifEnv*           m_env;
  const CSVDecodeOpts& m_opts;
  const char*          m_beg;
  const char*          m_p;
  const char*          m_end;
  ERL_NIF_TERM         m_input_bin; // original binary term — used for zero-copy sub_binary

  CSVDecoder(ErlNifEnv* e, const CSVDecodeOpts& o, const char* data, size_t size,
             ERL_NIF_TERM input_bin)
    : m_env(e), m_opts(o), m_beg(data), m_p(data), m_end(data + size), m_input_bin(input_bin) {}

  static bool is_eol(char c) { return c == '\n' || c == '\r'; }

  void skip_eol() {
    if (m_p < m_end && *m_p == '\r') ++m_p;
    if (m_p < m_end && *m_p == '\n') ++m_p;
  }

  // Makes a field term from a span of the input that needs no unescaping.
  // Default (copy_strings == false): a zero-copy sub-binary referencing the
  // original input — no allocation, but the input binary stays alive as
  // long as any sub-binary referencing it does.
  // With copy_strings == true: always allocates a fresh binary, allowing the
  // GC to reclaim the input buffer independently of the decoded results.
  ERL_NIF_TERM make_raw_field_term(std::string_view sv)
  {
    return make_span_term(m_env, m_input_bin, m_beg, m_end, sv, m_opts.copy_strings);
  }

  // Unescapes doubled quotes by copying the runs *between* them in bulk,
  // rather than byte-by-byte — for a field with a handful of escaped quotes
  // this is a handful of memcpy calls instead of one push_back per byte.
  // Writes directly into the destination Erlang binary (sized exactly to
  // the unescaped length up front) instead of building an intermediate
  // std::string and copying it again via make_binary: one allocation and
  // one pass over the bytes instead of two of each.
  ERL_NIF_TERM make_field_term(std::string_view sv, bool has_quote_escape)
  {
    if (!has_quote_escape) return make_raw_field_term(sv);

    size_t escapes = 0;
    for (size_t i = 0; i + 1 < sv.size(); ++i)
      if (sv[i] == '"' && sv[i+1] == '"') { ++escapes; ++i; }

    auto [term, dst] = make_binary(m_env, sv.size() - escapes);
    size_t start = 0, out = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
      if (sv[i] == '"' && i + 1 < sv.size() && sv[i+1] == '"') {
        size_t n = i + 1 - start;
        memcpy(dst + out, sv.data() + start, n); // include first quote
        out += n;
        start = i + 2; // skip the second quote
        ++i;
      }
    }
    memcpy(dst + out, sv.data() + start, sv.size() - start);
    return term;
  }

  // Reads one field starting at m_p. Advances m_p past the field (not past
  // the following delimiter/EOL). Sets `eol`/`eof` flags for the caller and
  // `more_fields` to false if the field was the last one on the line.
  ERL_NIF_TERM read_field(bool& ok)
  {
    ok = true;
    if (m_p < m_end && *m_p == '"') {
      ++m_p; // opening quote
      const char* start = m_p;
      bool has_escape = false;
      for (;;) {
        // SIMD: jump directly to the next '"' (or end) in one scan.
        m_p = find_byte(m_p, m_end, '"');
        if (m_p >= m_end) { ok = false; return 0; } // unterminated quoted field
        if (m_p + 1 < m_end && m_p[1] == '"') { has_escape = true; m_p += 2; continue; }
        break; // closing quote
      }
      std::string_view sv(start, static_cast<size_t>(m_p - start));
      ++m_p; // closing quote
      return make_field_term(sv, has_escape);
    }

    // SIMD: advance past bytes that are not the delimiter, '\n', or '\r'.
    const char* start = m_p;
    m_p = find_field_end(m_p, m_end, m_opts.delimiter);
    return make_raw_field_term(std::string_view(start, static_cast<size_t>(m_p - start)));
  }

  // Reads one record (row) into `fields`. Returns false at end of input with
  // no fields read (clean EOF). On a malformed quoted field, sets `err`.
  template<size_t N>
  bool read_record(SmallTermVec<N>& fields, bool& err)
  {
    err = false;
    fields.set_size(0);
    if (m_p >= m_end) return false;

    // Skip blank lines.
    while (m_p < m_end && is_eol(*m_p)) skip_eol();
    if (m_p >= m_end) return false;

    for (;;) {
      bool ok;
      auto field = read_field(ok);
      if (!ok) [[unlikely]] { err = true; return false; }
      fields.push_back(field);

      if (m_p < m_end && *m_p == m_opts.delimiter) { ++m_p; continue; }
      if (m_p < m_end && is_eol(*m_p)) { skip_eol(); }
      break;
    }
    return true;
  }

  // Attempts the type-specific conversion of `sv` per `spec`. Returns 0 on a
  // value that doesn't match the requested type.
  ERL_NIF_TERM try_convert(std::string_view sv, const CSVFieldSpec& spec)
  {
    switch (spec.type) {
      case CSVFieldType::integer: {
        if (sv.empty()) return 0;
        ERL_NIF_TERM r = glz::BigInt::decode(m_env, sv.data(), sv.data() + sv.size());
        return r;
      }
      case CSVFieldType::floatp: {
        if (sv.empty())
          return 0;
        double d;
        auto [ep, ec] = glz::fast_float::from_chars(sv.data(), sv.data() + sv.size(), d);
        if (ec != std::errc{} || ep != sv.data() + sv.size()) [[unlikely]]
          return 0;
        d = std::round(d * spec.scale) / spec.scale;
        return enif_make_double(m_env, d);
      }
      case CSVFieldType::boolean: {
        if (sv == "true"  || sv == "TRUE"  || sv == "True")  return AM_TRUE;
        if (sv == "false" || sv == "FALSE" || sv == "False") return AM_FALSE;
        return 0;
      }
      case CSVFieldType::datetime: {
        auto secs = datetime::parse(sv, spec.format);
        if (!secs) [[unlikely]] return 0;
        return enif_make_int64(m_env, *secs);
      }
      case CSVFieldType::charlist: {
        std::vector<ERL_NIF_TERM> cps;
        cps.reserve(sv.size());
        const char* p = sv.data();
        const char* e = p + sv.size();
        while (p < e) cps.push_back(enif_make_uint(m_env, decode_utf8(p, e)));
        return enif_make_list_from_array(m_env, cps.data(), unsigned(cps.size()));
      }
      case CSVFieldType::existing_atom: {
        ERL_NIF_TERM t;
        return enif_make_existing_atom_len(m_env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1)
             ? t : 0;
      }
      case CSVFieldType::atom_list: {
        for (auto& name : spec.atoms) {
          if (sv == name) {
            ERL_NIF_TERM t;
            return enif_make_existing_atom_len(m_env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1)
                 ? t : 0;
          }
        }
        return 0;
      }
      default:
        return 0;
    }
  }

  // Converts one field term (a binary) per `spec`. Returns 0 to signal
  // {invalid_field_value, Row, Col} (only when `spec.on_failure == raise`);
  // otherwise always returns a valid term, falling back per `on_failure`.
  ERL_NIF_TERM convert_field(ERL_NIF_TERM term, const CSVFieldSpec& spec)
  {
    if (spec.type == CSVFieldType::none || spec.type == CSVFieldType::binary)
      return term;

    ErlNifBinary bin;
    if (!enif_inspect_binary(m_env, term, &bin))
      return 0;

    auto sv = std::string_view(reinterpret_cast<const char*>(bin.data), bin.size);

    if (sv.empty() && spec.has_default)
      return spec.default_term;

    ERL_NIF_TERM r = try_convert(sv, spec);
    if (r) [[likely]] return r;

    switch (spec.on_failure) {
      case CSVOnFailure::binary:        return term;
      case CSVOnFailure::raise:         return 0;
      case CSVOnFailure::default_value: return spec.has_default ? spec.default_term : term;
      case CSVOnFailure::null:          return m_opts.null_term;
    }
    return term;
  }

  // Converts `fields` in place per `m_opts.fields` (positional by column
  // index; columns beyond the spec list, or with the `none`/default spec,
  // are left as binaries). Returns the 1-based column index of the first
  // field that fails to convert, or 0 on success.
  template<size_t N>
  size_t convert_fields(SmallTermVec<N>& fields)
  {
    size_t n = std::min(fields.size(), m_opts.fields.size());
    for (size_t i = 0; i < n; ++i) {
      auto converted = convert_field(fields[i], m_opts.fields[i]);
      if (!converted) [[unlikely]]
        return i + 1;
      fields.data()[i] = converted;
    }
    return 0;
  }

  ERL_NIF_TERM make_header_key(ERL_NIF_TERM bin_term)
  {
    if (!m_opts.hdr_atom && !m_opts.hdr_existing_atom && !m_opts.hdr_charlist)
      return bin_term;

    ErlNifBinary bin;
    if (!enif_inspect_binary(m_env, bin_term, &bin)) return bin_term;
    auto sv = std::string_view(reinterpret_cast<const char*>(bin.data), bin.size);

    if (m_opts.hdr_atom)
      return enif_make_atom_len(m_env, sv.data(), sv.size());

    if (m_opts.hdr_existing_atom) {
      ERL_NIF_TERM t;
      return enif_make_existing_atom_len(m_env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1)
           ? t : bin_term;
    }

    // charlist: decode UTF-8 bytes to a list of Unicode codepoints
    std::vector<ERL_NIF_TERM> cps;
    const char* p   = sv.data();
    const char* end = p + sv.size();
    while (p < end)
      cps.push_back(enif_make_uint(m_env, decode_utf8(p, end)));
    return enif_make_list_from_array(m_env, cps.data(), unsigned(cps.size()));
  }

  // Decodes the entire input.
  // Returns:
  //   - success: {true, #{headers => nil|[binary()|atom()], data => [[term()]]|[map()]}}
  //   - failure: {false, atom | binary}
  //
  // When `headers` is set the first row is extracted as the `headers` value
  // (applying `{headers, atom|existing_atom|charlist}` if requested). Subsequent rows are
  // emitted as field lists (default), as tuples when {return, tuple} is set,
  // or as maps keyed by the header names when {return, map} is also set.
  // duplicate_header is returned if a header row contains duplicate column
  // names and map output is requested.
  std::tuple<bool, ERL_NIF_TERM> decode()
  {
    SmallTermVec<64> fields;
    SmallTermVec<64> header;
    std::vector<ERL_NIF_TERM> rows;
    // Conservative row-count estimate (4 B/row minimum, e.g. "a,b\n") avoids
    // the ~15 doubling reallocations a 25K-row file would otherwise pay as
    // `rows` grows one push_back at a time; an overestimate only wastes
    // unused capacity, never time, so erring high for short rows is fine.
    rows.reserve((m_end - m_beg) / 4);
    size_t row_num  = 0;
    bool err = false;
    auto rec = read_record(fields, err);

    if (!rec) [[unlikely]]
      goto DONE;

    // Populate header: explicit list beats reading a header row from the data.
    if (m_opts.explicit_headers) {
      for (auto h : m_opts.header_list)
        header.push_back(h);
      // Do NOT consume a data row as the header.
    } else if (m_opts.headers) {
      for (auto field : fields)
        header.push_back(make_header_key(field));
      rec = read_record(fields, err);
    }

    // Skip the requested number of leading data rows.
    {
      size_t skipped = 0;
      while (rec && skipped < m_opts.skip_rows) {
        rec = read_record(fields, err);
        ++skipped;
      }
    }

    // Data rows: maps, tuples, or field lists per {return, ...} (map implies
    // headers — enforced by parse_csv_decode_opts). Dispatch on return_kind
    // once here rather than per-record inside the loop below.
    switch (m_opts.return_kind) {
      case CSVReturnKind::map:
        while (rec && (m_opts.limit == 0 || row_num < m_opts.limit)) {
          ++row_num;
          if (!m_opts.fields.empty()) {
            if (size_t col = convert_fields(fields))
              return std::make_tuple(
                      false,
                      enif_make_tuple3(m_env, AM_INVALID_FIELD_VALUE,
                                       enif_make_uint64(m_env, row_num),
                                       enif_make_uint64(m_env, col)));
          }
          ERL_NIF_TERM map = fields.to_erl_map(m_env, header);
          if (!map) [[unlikely]]
            return std::make_tuple(false, AM_DUPLICATE_HEADER);
          rows.push_back(map);
          rec = read_record(fields, err);
        }
        break;

      case CSVReturnKind::tuple:
        while (rec && (m_opts.limit == 0 || row_num < m_opts.limit)) {
          ++row_num;
          if (!m_opts.fields.empty()) {
            if (size_t col = convert_fields(fields))
              return std::make_tuple(
                      false,
                      enif_make_tuple3(m_env, AM_INVALID_FIELD_VALUE,
                                       enif_make_uint64(m_env, row_num),
                                       enif_make_uint64(m_env, col)));
          }
          rows.push_back(fields.to_erl_tuple(m_env));
          rec = read_record(fields, err);
        }
        break;

      case CSVReturnKind::list:
        while (rec && (m_opts.limit == 0 || row_num < m_opts.limit)) {
          ++row_num;
          if (!m_opts.fields.empty()) {
            if (size_t col = convert_fields(fields))
              return std::make_tuple(
                      false,
                      enif_make_tuple3(m_env, AM_INVALID_FIELD_VALUE,
                                       enif_make_uint64(m_env, row_num),
                                       enif_make_uint64(m_env, col)));
          }
          rows.push_back(fields.to_erl_list(m_env));
          rec = read_record(fields, err);
        }
        break;
    }

  DONE:
    if (err) [[unlikely]]
      return std::make_tuple(false, AM_UNTERMINATED_QUOTED_FIELD);

    ERL_NIF_TERM headers_term = m_opts.headers
      ? enif_make_list_from_array(m_env, header.data(), unsigned(header.size()))
      : AM_NIL;
    ERL_NIF_TERM data_term =
      enif_make_list_from_array(m_env, rows.data(), unsigned(rows.size()));

    ERL_NIF_TERM keys[2] = { AM_HEADERS, AM_DATA };
    ERL_NIF_TERM vals[2] = { headers_term, data_term };
    ERL_NIF_TERM result;
    enif_make_map_from_arrays(m_env, keys, vals, 2, &result);

    return std::make_tuple(true, result);
  }
};

//-----------------------------------------------------------------------------
// Encoder
//-----------------------------------------------------------------------------

struct CSVEncoder {
  ErlNifEnv*           m_env;
  const CSVEncodeOpts& m_opts;
  OutBuf&              m_out;
  const char*          m_err;
  ERL_NIF_TERM         m_err_term;

  CSVEncoder(ErlNifEnv* e, const CSVEncodeOpts& o, OutBuf& out)
    : m_env(e), m_opts(o), m_out(out), m_err(""), m_err_term(AM_NIL) {}

  void push_field_raw(std::string_view sv)
  {
    const char* p   = sv.data();
    const char* end = p + sv.size();

    // SIMD: check whether quoting is needed at all.
    if (find_csv_special(p, end, m_opts.delimiter) == end) { m_out.push(sv); return; }

    // Field contains a special byte — wrap in double quotes and double any
    // embedded '"' characters. SIMD locates each '"' for bulk-copy runs.
    m_out.push('"');
    for (;;) {
      const char* q = find_byte(p, end, '"');
      m_out.push(p, q - p);    // bulk-copy bytes up to (not including) the quote
      if (q >= end) break;
      m_out.push("\"\"", 2);   // double the embedded quote
      p = q + 1;
    }
    m_out.push('"');
  }

  // Encodes one field term (binary, atom, integer, or float). Returns false
  // for unsupported term types.
  bool encode_field(ERL_NIF_TERM term)
  {
    switch (enif_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, term, &bin)) [[unlikely]]
          return false;
        push_field_raw({reinterpret_cast<const char*>(bin.data), bin.size});
        return true;
      }
      case ERL_NIF_TERM_TYPE_INTEGER:
        return glz::BigInt::encode(m_env, term, m_out);

      case ERL_NIF_TERM_TYPE_FLOAT: {
        double d;
        if (!enif_get_double(m_env, term, &d)) [[unlikely]]
          return false;
        char buf[32];
        auto [e, ec] = std::to_chars(buf, buf+32, d, std::chars_format::general);
        if (ec != std::errc{}) [[unlikely]]
          return false;
        m_out.push(buf, e - buf);
        return true;
      }
      case ERL_NIF_TERM_TYPE_ATOM: {
        char buf[256];
        std::string_view sv;
        if (!atom_to_sv(m_env, term, buf, sizeof(buf), sv)) [[unlikely]]
          return false;
        push_field_raw(sv);
        return true;
      }
      default:
        return false;
    }
  }

  bool encode_row(ERL_NIF_TERM row)
  {
    ERL_NIF_TERM head, tail = row;
    bool first = true;
    while (enif_get_list_cell(m_env, tail, &head, &tail)) {
      if (!first) m_out.push(m_opts.delimiter);
      first = false;
      if (!encode_field(head)) [[unlikely]]
        return error("cannot encode CSV field", head);
    }
    if (!enif_is_empty_list(m_env, tail)) [[unlikely]]
      return error("cannot encode improper list as CSV row", tail);
    m_out.push(m_opts.line_ending);
    return true;
  }

  bool encode_map_row(ERL_NIF_TERM map, const std::vector<ERL_NIF_TERM>& header)
  {
    for (size_t i = 0; i < header.size(); ++i) {
      if (i > 0) m_out.push(m_opts.delimiter);
      ERL_NIF_TERM val;
      if (!enif_get_map_value(m_env, map, header[i], &val)) continue; // missing key -> empty field
      if (!encode_field(val)) [[unlikely]]
        return error("cannot encode CSV field", val);
    }
    m_out.push(m_opts.line_ending);
    return true;
  }

  // `term` is a list of rows (each a list of fields), or — with `headers` —
  // a list of maps.
  bool encode(ERL_NIF_TERM term)
  {
    if (!enif_is_list(m_env, term)) { m_err = "expected a list of rows"; return false; }

    if (enif_is_empty_list(m_env, term)) return true;

    if (m_opts.headers) {
      std::vector<ERL_NIF_TERM> header;

      if (m_opts.explicit_headers) {
        // Use the explicitly given column order/names.
        header = m_opts.header_list;
      } else {
        // Determine column order from the first row's map keys.
        ERL_NIF_TERM head, tail = term;
        enif_get_list_cell(m_env, tail, &head, &tail);
        if (enif_term_type(m_env, head) != ERL_NIF_TERM_TYPE_MAP) [[unlikely]]
          return error("headers option requires rows to be maps", head);

        ERL_NIF_TERM key, val;
        auto it = MapIterator::create(m_env, head);
        if (!it) [[unlikely]]
          return error("failed to create a map iterator", AM_NIL);
        while (it->get_pair(&key, &val)) {
          header.push_back(key);
          it->next();
        }
      }

      // Emit header row.
      for (size_t i = 0; i < header.size(); ++i) {
        if (i > 0) m_out.push(m_opts.delimiter);
        if (!encode_field(header[i])) [[unlikely]]
          return error("cannot encode CSV header", header[i]);
      }
      m_out.push(m_opts.line_ending);

      ERL_NIF_TERM row, rest = term;
      while (enif_get_list_cell(m_env, rest, &row, &rest)) {
        if (enif_term_type(m_env, row) != ERL_NIF_TERM_TYPE_MAP) [[unlikely]]
          return error("headers option requires rows to be maps", row);
        if (!encode_map_row(row, header)) [[unlikely]]
          return false;
      }
      return true;
    }

    ERL_NIF_TERM row, rest = term;
    while (enif_get_list_cell(m_env, rest, &row, &rest)) {
      if (!enif_is_list(m_env, row)) [[unlikely]]
        return error("expected each row to be a list", row);
      if (!encode_row(row)) [[unlikely]]
        return false;
    }
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
