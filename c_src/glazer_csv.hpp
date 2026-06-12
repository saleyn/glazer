// vim:ts=2:sw=2:et
// ---------------------------------------------------------------------------
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
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_common.hpp"

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct CsvDecodeOpts {
  char delimiter           = ',';
  bool headers             = false;
  bool label_atom          = false;
  bool label_existing_atom = false;
};

static bool parse_csv_decode_opts(ErlNifEnv* env, ERL_NIF_TERM list, CsvDecodeOpts& opts)
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
    } else if (enif_is_identical(tp[0], AM_KEYS)) {
      if      (enif_is_identical(tp[1], AM_LABEL_ATOM))          opts.label_atom = true;
      else if (enif_is_identical(tp[1], AM_LABEL_EXISTING_ATOM)) opts.label_existing_atom = true;
      else if (enif_is_identical(tp[1], AM_LABEL_BINARY))        { opts.label_atom = false; opts.label_existing_atom = false; }
    }
  }
  return true;
}

struct CsvEncodeOpts {
  char        delimiter = ',';
  bool        headers   = false;
  std::string_view line_ending = "\r\n";
};

static bool parse_csv_encode_opts(ErlNifEnv* env, ERL_NIF_TERM list, CsvEncodeOpts& opts)
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
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct CsvDecoder {
  ErlNifEnv*           m_env;
  const CsvDecodeOpts& m_opts;
  const char*          m_p;
  const char*          m_end;

  CsvDecoder(ErlNifEnv* e, const CsvDecodeOpts& o, const char* data, size_t size)
    : m_env(e), m_opts(o), m_p(data), m_end(data + size) {}

  static bool is_eol(char c) { return c == '\n' || c == '\r'; }

  void skip_eol() {
    if (m_p < m_end && *m_p == '\r') ++m_p;
    if (m_p < m_end && *m_p == '\n') ++m_p;
  }

  // Unescapes doubled quotes by copying the runs *between* them in bulk,
  // rather than byte-by-byte — for a field with a handful of escaped quotes
  // this is a handful of memcpy calls instead of one push_back per byte.
  ERL_NIF_TERM make_field_term(std::string_view sv, bool has_quote_escape)
  {
    if (!has_quote_escape) return make_binary(m_env, sv);

    std::string buf;
    buf.reserve(sv.size());
    size_t start = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
      if (sv[i] == '"' && i + 1 < sv.size() && sv[i+1] == '"') {
        buf.append(sv.data() + start, i + 1 - start); // include first quote
        start = i + 2; // skip the second quote
        ++i;
      }
    }
    buf.append(sv.data() + start, sv.size() - start);
    return make_binary(m_env, buf);
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
        if (m_p >= m_end) { ok = false; return 0; } // unterminated quoted field
        if (*m_p == '"') {
          if (m_p + 1 < m_end && m_p[1] == '"') { has_escape = true; m_p += 2; continue; }
          break; // closing quote
        }
        ++m_p;
      }
      std::string_view sv(start, static_cast<size_t>(m_p - start));
      ++m_p; // closing quote
      return make_field_term(sv, has_escape);
    }

    const char* start = m_p;
    while (m_p < m_end && *m_p != m_opts.delimiter && !is_eol(*m_p)) ++m_p;
    return make_binary(m_env, std::string_view(start, static_cast<size_t>(m_p - start)));
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
      ERL_NIF_TERM field = read_field(ok);
      if (!ok) [[unlikely]] { err = true; return false; }
      fields.push_back(field);

      if (m_p < m_end && *m_p == m_opts.delimiter) { ++m_p; continue; }
      if (m_p < m_end && is_eol(*m_p)) { skip_eol(); }
      break;
    }
    return true;
  }

  ERL_NIF_TERM make_header_key(ERL_NIF_TERM bin_term)
  {
    if (!m_opts.label_atom && !m_opts.label_existing_atom) return bin_term;

    ErlNifBinary bin;
    if (!enif_inspect_binary(m_env, bin_term, &bin)) return bin_term;
    auto sv = std::string_view(reinterpret_cast<const char*>(bin.data), bin.size);

    if (m_opts.label_atom)
      return enif_make_atom_len(m_env, sv.data(), sv.size());

    ERL_NIF_TERM t;
    return enif_make_existing_atom_len(m_env, sv.data(), sv.size(), &t, ERL_NIF_LATIN1)
         ? t : bin_term;
  }

  // Decodes the entire input into a list of rows (maps if `headers`).
  // Returns:
  //   - success: {true, Result :: list(map) | list(list)}
  //   - failure: {false, Result :: atom | binary}
  std::tuple<bool, ERL_NIF_TERM> decode()
  {
    SmallTermVec<64> fields;
    SmallTermVec<64> header;
    std::vector<ERL_NIF_TERM> rows;

    bool err = false;
    auto rec = read_record(fields, err);

    if (!rec) [[unlikely]]
      goto DONE;

    // If `headers` is set, the first record becomes the column names.
    // Otherwise, treat it as a data row and decode it as usual.
    if (m_opts.headers) {
      for (auto field : fields)
        header.push_back(make_header_key(field));
      rec = read_record(fields, err);
    }

    if (m_opts.headers) {
      while (rec) {
        auto map = fields.to_erl_map(m_env, header);
        if (!map) [[unlikely]]
          return std::make_tuple(false, AM_DUPLICATE_HEADER);
        rows.push_back(map);
        rec = read_record(fields, err);
      }
    } else {
      do    { rows.push_back(fields.to_erl_list(m_env)); }
      while (read_record(fields, err));
    }

  DONE:
    if (err) [[unlikely]]
      return std::make_tuple(false, AM_UNTERMINATED_QUOTED_FIELD);

    return std::make_tuple(true,
      enif_make_list_from_array(m_env, rows.data(), unsigned(rows.size())));
  }
};

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct CsvEncoder {
  ErlNifEnv*           m_env;
  const CsvEncodeOpts& m_opts;
  OutBuf&              m_out;
  std::string          m_err;

  CsvEncoder(ErlNifEnv* e, const CsvEncodeOpts& o, OutBuf& out)
    : m_env(e), m_opts(o), m_out(out) {}

  void push_field_raw(std::string_view sv)
  {
    bool needs_quotes = false;
    for (char c : sv) {
      if (c == m_opts.delimiter || c == '"' || c == '\n' || c == '\r') { needs_quotes = true; break; }
    }
    if (!needs_quotes) { m_out.push(sv); return; }

    m_out.push('"');
    size_t start = 0;
    for (size_t i = 0; i < sv.size(); ++i) {
      if (sv[i] == '"') {
        m_out.push(sv.data() + start, i - start + 1);
        m_out.push('"'); // double the quote
        start = i + 1;
      }
    }
    m_out.push(sv.data() + start, sv.size() - start);
    m_out.push('"');
  }

  // Encodes one field term (binary, atom, integer, or float). Returns false
  // for unsupported term types.
  bool encode_field(ERL_NIF_TERM term)
  {
    switch (enif_term_type(m_env, term)) {
      case ERL_NIF_TERM_TYPE_BITSTRING: {
        ErlNifBinary bin;
        if (!enif_inspect_binary(m_env, term, &bin)) return false;
        push_field_raw({reinterpret_cast<const char*>(bin.data), bin.size});
        return true;
      }
      case ERL_NIF_TERM_TYPE_INTEGER:
        return glazer::BigInt::encode(m_env, term, m_out);

      case ERL_NIF_TERM_TYPE_FLOAT: {
        double d;
        if (!enif_get_double(m_env, term, &d)) return false;
        char buf[32];
        auto [e, ec] = std::to_chars(buf, buf+32, d, std::chars_format::general);
        if (ec != std::errc{}) return false;
        m_out.push(buf, e - buf);
        return true;
      }
      case ERL_NIF_TERM_TYPE_ATOM: {
        char buf[256];
        std::string_view sv;
        if (!atom_to_sv(m_env, term, buf, sizeof(buf), sv)) return false;
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
      if (!encode_field(head)) { m_err = "cannot encode CSV field"; return false; }
    }
    m_out.push(m_opts.line_ending);
    return true;
  }

  bool encode_map_row(ERL_NIF_TERM map, const std::vector<ERL_NIF_TERM>& header)
  {
    for (size_t i = 0; i < header.size(); ++i) {
      if (i > 0) m_out.push(m_opts.delimiter);
      ERL_NIF_TERM val;
      if (!enif_get_map_value(m_env, map, header[i], &val)) continue; // missing key -> empty field
      if (!encode_field(val)) { m_err = "cannot encode CSV field"; return false; }
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
      // Determine column order from the first row's map keys.
      ERL_NIF_TERM head, tail = term;
      enif_get_list_cell(m_env, tail, &head, &tail);
      if (enif_term_type(m_env, head) != ERL_NIF_TERM_TYPE_MAP) {
        m_err = "headers option requires rows to be maps";
        return false;
      }

      std::vector<ERL_NIF_TERM> header;
      ERL_NIF_TERM key, val;
      ErlNifMapIterator it;
      enif_map_iterator_create(m_env, head, &it, ERL_NIF_MAP_ITERATOR_FIRST);
      while (enif_map_iterator_get_pair(m_env, &it, &key, &val)) {
        header.push_back(key);
        enif_map_iterator_next(m_env, &it);
      }
      enif_map_iterator_destroy(m_env, &it);

      // Emit header row.
      for (size_t i = 0; i < header.size(); ++i) {
        if (i > 0) m_out.push(m_opts.delimiter);
        if (!encode_field(header[i])) { m_err = "cannot encode CSV header"; return false; }
      }
      m_out.push(m_opts.line_ending);

      ERL_NIF_TERM row, rest = term;
      while (enif_get_list_cell(m_env, rest, &row, &rest)) {
        if (enif_term_type(m_env, row) != ERL_NIF_TERM_TYPE_MAP) {
          m_err = "headers option requires rows to be maps";
          return false;
        }
        if (!encode_map_row(row, header)) return false;
      }
      return true;
    }

    ERL_NIF_TERM row, rest = term;
    while (enif_get_list_cell(m_env, rest, &row, &rest)) {
      if (!enif_is_list(m_env, row)) { m_err = "expected each row to be a list"; return false; }
      if (!encode_row(row)) return false;
    }
    return true;
  }
};
