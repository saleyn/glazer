// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Format validators for the JSON Schema `format` keyword (assertion mode).
//
// Implemented formats: date-time, time (RFC 3339, with leap seconds and the
// RFC 9557 bracketed suffixes), email (RFC 5321 mailbox, ASCII), idn-email
// (RFC 6531 mailbox with an IDNA2008 domain), hostname (RFC 1123), idn-hostname
// (IDNA2008), ipv6 (RFC 4291 with embedded IPv4), uri (RFC 3986 absolute URI),
// uuid (RFC 4122 canonical form), and the other formats listed in check().
// Format names not listed in check() are treated as annotations by the caller.
//
// Hand-written parsers rather than regular expressions: each rule is a few
// lines, and no matching engine or backtracking is involved.
//-----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string_view>

#include "glazer_idna.hpp"
#include "glazer_common.hpp"
#include "glazer_utf8.hpp"

namespace glz::formats {

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool is_unreserved(char c) { return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~'; }
inline bool is_sub_delim(char c)  {
  return c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' ||
         c == '*' || c == '+' || c == ',' || c == ';' || c == '=';
}

// Fixed-width decimal field: exactly `n` digits, value in [lo, hi]
inline bool fixed_number(std::string_view s, size_t n, int lo, int hi, int& out) {
  if (s.size() != n) return false;
  int v = 0;
  for (char c : s) {
    if (!is_digit(c)) return false;
    v = v * 10 + (c - '0');
  }
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

inline bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
inline int days_in_month(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && is_leap(y)) ? 29 : d[m - 1];
}

// YYYY-MM-DD with a valid day for the month and year
inline bool full_date(std::string_view s, int& y, int& m, int& d) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  if (!fixed_number(s.substr(0, 4), 4, 0, 9999, y)) return false;
  if (!fixed_number(s.substr(5, 2), 2, 1, 12, m)) return false;
  if (!fixed_number(s.substr(8, 2), 2, 1, days_in_month(y, m), d)) return false;
  return true;
}

// Move a date by one day in either direction (used for leap-second checks)
inline void shift_day(int& y, int& m, int& d, int delta) {
  d += delta;
  if (d < 1) {
    if (--m < 1) { m = 12; --y; }
    d = days_in_month(y, m);
  } else if (d > days_in_month(y, m)) {
    d = 1;
    if (++m > 12) { m = 1; ++y; }
  }
}

// time-numoffset / "Z": returns the offset in minutes east of UTC
inline bool time_offset(std::string_view s, int& minutes) {
  if (s == "Z" || s == "z") { minutes = 0; return true; }
  if (s.size() != 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':') return false;
  int hh = 0, mm = 0;
  if (!fixed_number(s.substr(1, 2), 2, 0, 23, hh)) return false;
  if (!fixed_number(s.substr(4, 2), 2, 0, 59, mm)) return false;
  minutes = (hh * 60 + mm) * (s[0] == '-' ? -1 : 1);
  return true;
}

// full-time: hh:mm:ss[.frac] followed by a required offset. A leap second (60)
// is valid only when the UTC time is 23:59:60 on 30 June or 31 December.
inline bool full_time(std::string_view s, int y, int m, int d, bool allow_leap = true) {
  if (s.size() < 9 || s[2] != ':' || s[5] != ':') return false;
  int hh = 0, mm = 0, ss = 0;
  if (!fixed_number(s.substr(0, 2), 2, 0, 23, hh)) return false;
  if (!fixed_number(s.substr(3, 2), 2, 0, 59, mm)) return false;
  if (!fixed_number(s.substr(6, 2), 2, 0, 60, ss)) return false;
  size_t i = 8;
  if (s[i] == '.') {
    ++i;
    size_t start = i;
    while (i < s.size() && is_digit(s[i])) ++i;
    if (i == start) return false;
  }
  int offset = 0;
  if (!time_offset(s.substr(i), offset)) return false;

  if (ss == 60) {
    if (!allow_leap) return false;
    // Convert to UTC and check the leap-second rule on that clock
    int utc = hh * 60 + mm - offset;
    int uy = y, um = m, ud = d;
    if (utc < 0) { utc += 1440; shift_day(uy, um, ud, -1); }
    else if (utc >= 1440) { utc -= 1440; shift_day(uy, um, ud, 1); }
    bool last_moment = (utc == 23 * 60 + 59);
    bool june_or_dec = (um == 6 && ud == 30) || (um == 12 && ud == 31);
    if (!(last_moment && june_or_dec)) return false;
  }
  return true;
}

// RFC 9557 suffixes: [tzname] or [+hh:mm] (optionally critical "!"), then any
// number of [key=value] tags (optionally critical "!")
inline bool date_time_suffixes(std::string_view s) {
  size_t i = 0;
  auto bracket_body = [&](size_t& pos, std::string_view& body) {
    if (pos >= s.size() || s[pos] != '[') return false;
    size_t close = s.find(']', pos);
    if (close == std::string_view::npos) return false;
    body = s.substr(pos + 1, close - pos - 1);
    pos = close + 1;
    return true;
  };
  if (i < s.size() && s[i] == '[' && s.find('=', i) > s.find(']', i)) {
    std::string_view body;
    size_t pos = i;
    if (!bracket_body(pos, body)) return false;
    if (!body.empty() && body[0] == '!') body.remove_prefix(1);
    bool numeric = !body.empty() && (body[0] == '+' || body[0] == '-');
    if (numeric) {
      int off = 0;
      if (!time_offset(body, off)) return false;
    } else {
      if (body.empty() || !(is_alpha(body[0]) || body[0] == '_')) return false;
      size_t component = 0;
      for (char c : body) {
        if (c == '/') { component = 0; continue; }
        if (!(is_alpha(c) || is_digit(c) || c == '_' || c == '+' || c == '-')) return false;
        if (++component > 14) return false;
      }
    }
    i = pos;
  }
  while (i < s.size()) {
    std::string_view body;
    size_t pos = i;
    if (!bracket_body(pos, body)) return false;
    if (!body.empty() && body[0] == '!') body.remove_prefix(1);
    size_t eq = body.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= body.size()) return false;
    // RFC 9557 keys and values are lowercase letters, digits, '-' and '_'
    for (size_t k = 0; k < body.size(); ++k) {
      char c = body[k];
      if (k == eq) continue;
      bool lower = (c >= 'a' && c <= 'z');
      if (!(lower || is_digit(c) || c == '-' || c == '_')) return false;
    }
    i = pos;
  }
  return true;
}

// RFC 3339 date-time with the RFC 9557 suffixes
inline bool is_date_time(std::string_view s) {
  if (s.size() < 20 || (s[10] != 'T' && s[10] != 't')) return false;
  int y = 0, m = 0, d = 0;
  if (!full_date(s.substr(0, 10), y, m, d)) return false;
  // find where the time ends: the offset is "Z"/"z" or +/-hh:mm
  size_t i = 11;
  size_t j = i;
  while (j < s.size() && s[j] != 'Z' && s[j] != 'z' && s[j] != '+' && s[j] != '-') ++j;
  if (j >= s.size()) return false;
  if (s[j] != 'Z' && s[j] != 'z') {
    if (j + 6 > s.size()) return false;
    j += 6;
  } else {
    j += 1;
  }
  if (j > s.size()) return false;
  if (!full_time(s.substr(i, j - i), y, m, d)) return false;
  return date_time_suffixes(s.substr(j));
}

inline bool is_time(std::string_view s) {
  // RFC 3339 full-time without a date: leap seconds are not valid times
  return full_time(s, 2000, 1, 1, false);
}

// date format (RFC 3339 full-date): YYYY-MM-DD with a valid day for the month
inline bool is_date(std::string_view s) {
  int y = 0, m = 0, d = 0;
  return full_date(s, y, m, d);
}

// Dotted-quad IPv4 (RFC 3986 dec-octet: no leading zeros)
inline bool ipv4_literal(std::string_view s) {
  int parts = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '.') {
      std::string_view octet = s.substr(start, i - start);
      if (octet.empty() || octet.size() > 3) return false;
      if (octet.size() > 1 && octet[0] == '0') return false;
      int v = 0;
      for (char c : octet) {
        if (!is_digit(c)) return false;
        v = v * 10 + (c - '0');
      }
      if (v > 255) return false;
      ++parts;
      start = i + 1;
    }
  }
  return parts == 4;
}

// ipv4 format (RFC 2673): exactly four dotted decimal octets, no leading zeros
inline bool is_ipv4(std::string_view s) {
  int parts = 0;
  for (char c : s) if (c == '.') ++parts;
  return parts == 3 && ipv4_literal(s);
}

// RFC 4291 IPv6 address with optional trailing dotted-quad IPv4
inline bool is_ipv6(std::string_view s) {
  if (s.empty()) return false;
  size_t dc = s.find("::");
  if (dc != std::string_view::npos && s.find("::", dc + 1) != std::string_view::npos) return false;

  // Count the 16-bit groups in a ':'-separated run. An embedded IPv4 counts as 2.
  auto count_groups = [](std::string_view part, bool allow_ipv4_tail, int& groups) -> bool {
    groups = 0;
    if (part.empty()) return true;
    size_t start = 0;
    while (true) {
      size_t colon = part.find(':', start);
      std::string_view g = part.substr(start, colon == std::string_view::npos ? std::string_view::npos : colon - start);
      bool last = (colon == std::string_view::npos);
      if (g.empty()) return false;
      if (g.find('.') != std::string_view::npos) {
        if (!last || !allow_ipv4_tail || !ipv4_literal(g)) return false;
        groups += 2;
      } else {
        if (g.size() > 4) return false;
        for (char c : g)
          if (!is_hex(c)) return false;
        groups += 1;
      }
      if (last) break;
      start = colon + 1;
    }
    return true;
  };

  if (dc == std::string_view::npos) {
    int g = 0;
    return count_groups(s, true, g) && g == 8;
  }

  std::string_view left = s.substr(0, dc);
  std::string_view right = s.substr(dc + 2);
  int gl = 0, gr = 0;
  // An IPv4 tail is only allowed on the right of "::"
  if (!count_groups(left, false, gl)) return false;
  if (!count_groups(right, true, gr)) return false;
  // "::" stands for one or more zero groups
  return gl + gr <= 7;
}

// ---- idn-hostname (IDNA2008) ------------------------------------------------

inline bool is_idn_hostname(std::string_view s) { return idna::valid_idn_domain(s); }

// ---- uuid (RFC 4122 / RFC 9562 canonical form) -----------------------------

// 8-4-4-4-12 hexadecimal digits with hyphens, case-insensitive
inline bool is_uuid(std::string_view s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    bool hyphen_pos = (i == 8 || i == 13 || i == 18 || i == 23);
    if (hyphen_pos) {
      if (s[i] != '-') return false;
    } else if (!is_hex(s[i])) {
      return false;
    }
  }
  return true;
}

// ---- email -----------------------------------------------------------------

inline bool is_atext(char c) {
  return is_alpha(c) || is_digit(c) ||
         c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
         c == '*' || c == '+' || c == '-' || c == '/' || c == '=' || c == '?'  ||
         c == '^' || c == '_' || c == '`' || c == '{' || c == '|' || c == '}'  ||
         c == '~';
}

inline bool printable_ascii(char c) { return c >= 0x20 && c <= 0x7E; }

// RFC 1123 hostname label: 1-63 letters, digits or hyphens, not at either end
inline bool hostname_label(std::string_view l) {
  if (l.empty() || l.size() > 63) return false;
  if (l.front() == '-' || l.back() == '-') return false;
  for (char c : l)
    if (!(is_alpha(c) || is_digit(c) || c == '-')) return false;
  return true;
}

inline bool is_hostname(std::string_view s) { return idna::valid_ascii_domain(s); }

// RFC 5321 mailbox (email) or, with idn set, RFC 6531 internationalized mailbox
// (idn-email): the local part may hold UTF-8 (any code point >= U+0080) and the
// domain is an IDNA2008 idn-hostname instead of an ASCII hostname.
inline bool is_mailbox(std::string_view s, bool idn) {
  if (s.size() > 254) return false;
  if (idn && !valid_utf8(s)) return false;
  size_t at = s.rfind('@');
  if (at == std::string_view::npos || at == 0 || at + 1 >= s.size()) return false;
  std::string_view local = s.substr(0, at);
  std::string_view domain = s.substr(at + 1);
  auto non_ascii = [](char c) { return static_cast<unsigned char>(c) >= 0x80; };

  // local-part: quoted-string or dot-atom
  if (local.front() == '"') {
    if (local.size() < 2 || local.back() != '"') return false;
    std::string_view body = local.substr(1, local.size() - 2);
    for (size_t i = 0; i < body.size(); ++i) {
      char c = body[i];
      if (c == '\\') {
        if (i + 1 >= body.size() || !printable_ascii(body[i + 1])) return false;
        ++i;   // quoted-pair
      } else if (c == '"' || !(printable_ascii(c) || (idn && non_ascii(c)))) {
        return false;
      }
    }
  } else {
    if (local.size() > 64) return false;
    bool atom_start = true;
    for (char c : local) {
      if (c == '.') {
        if (atom_start) return false;   // leading or doubled dot
        atom_start = true;
      } else if (is_atext(c) || (idn && non_ascii(c))) {
        atom_start = false;
      } else {
        return false;
      }
    }
    if (atom_start) return false;       // trailing dot
  }

  // domain: domain-literal or hostname
  if (domain.front() == '[') {
    if (domain.size() < 3 || domain.back() != ']') return false;
    std::string_view lit = domain.substr(1, domain.size() - 2);
    if (lit.size() > 5 && (lit[0] == 'I' || lit[0] == 'i') && (lit[1] == 'P' || lit[1] == 'p') &&
        (lit[2] == 'V' || lit[2] == 'v') && (lit[3] == '6') && lit[4] == ':') {
      return is_ipv6(lit.substr(5));
    }
    return ipv4_literal(lit);
  }
  return idn ? is_idn_hostname(domain) : is_hostname(domain);
}

inline bool is_email(std::string_view s) { return is_mailbox(s, false); }

inline bool is_idn_email(std::string_view s) { return is_mailbox(s, true); }

// ---- URI / IRI references (RFC 3986 / RFC 3987) -------------------------------

// RFC 3987 ucschar and iprivate code points
inline bool is_ucschar(uint32_t c) {
  return (c >= 0xA0    && c <= 0xD7FF)  || (c >= 0xF900  && c <= 0xFDCF)  || (c >= 0xFDF0  && c <= 0xFFEF)  ||
         (c >= 0x10000 && c <= 0x1FFFD) || (c >= 0x20000 && c <= 0x2FFFD) || (c >= 0x30000 && c <= 0x3FFFD) ||
         (c >= 0x40000 && c <= 0x4FFFD) || (c >= 0x50000 && c <= 0x5FFFD) || (c >= 0x60000 && c <= 0x6FFFD) ||
         (c >= 0x70000 && c <= 0x7FFFD) || (c >= 0x80000 && c <= 0x8FFFD) || (c >= 0x90000 && c <= 0x9FFFD) ||
         (c >= 0xA0000 && c <= 0xAFFFD) || (c >= 0xB0000 && c <= 0xBFFFD) || (c >= 0xC0000 && c <= 0xCFFFD) ||
         (c >= 0xD0000 && c <= 0xDFFFD) || (c >= 0xE1000 && c <= 0xEFFFD);
}

inline bool is_iprivate(uint32_t c) {
  return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD);
}

enum class UriPart { Userinfo, RegName, Path, Query, Fragment };

// One component of a URI or IRI. Non-ASCII is accepted only in IRI mode (and
// private-use characters only in query and fragment). Percent-encoding is checked.
inline bool uri_component(std::string_view s, UriPart part, bool iri, bool allow_question) {
  for (size_t i = 0; i < s.size();) {
    auto c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      if (!iri) return false;
      auto p = s.data() + i;
      auto e = s.data() + s.size();
      uint32_t cp = glz::decode_utf8(p, e);
      if (cp == 0xFFFD) return false;
      bool private_ok = (part == UriPart::Query || part == UriPart::Fragment) && is_iprivate(cp);
      if (!(is_ucschar(cp) || private_ok)) return false;
      i = static_cast<size_t>(p - s.data());
      continue;
    }
    if (c == '%') {
      if (i + 2 >= s.size() || !is_hex(s[i + 1]) || !is_hex(s[i + 2])) return false;
      i += 3;
      continue;
    }
    bool ok = is_unreserved(c) || is_sub_delim(c);
    switch (part) {
      case UriPart::Userinfo: ok = ok || c == ':'; break;
      case UriPart::RegName: break;
      case UriPart::Path: ok = ok || c == ':' || c == '@' || c == '/'; break;
      case UriPart::Query:
      case UriPart::Fragment:
        ok = ok || c == ':' || c == '@' || c == '/' || (allow_question && c == '?');
        break;
    }
    if (!ok) return false;
    ++i;
  }
  return true;
}

// IPvFuture: "v" 1*HEXDIG "." 1*(unreserved / sub-delims / ":")
inline bool ipv_future(std::string_view s) {
  if (s.size() < 4 || (s[0] != 'v' && s[0] != 'V')) return false;
  size_t dot = s.find('.');
  if (dot == std::string_view::npos || dot < 2 || dot + 1 >= s.size()) return false;
  for (size_t i = 1; i < dot; ++i)
    if (!is_hex(s[i])) return false;
  for (size_t i = dot + 1; i < s.size(); ++i) {
    char c = s[i];
    if (!(is_unreserved(c) || is_sub_delim(c) || c == ':')) return false;
  }
  return true;
}

// authority = [ userinfo "@" ] host [ ":" port ]
inline bool uri_authority(std::string_view a, bool iri) {
  size_t at = a.rfind('@');
  if (at != std::string_view::npos) {
    if (!uri_component(a.substr(0, at), UriPart::Userinfo, iri, false)) return false;
    a = a.substr(at + 1);
  }
  size_t close = a.rfind(']');
  size_t colon = a.rfind(':');
  if (colon != std::string_view::npos && (close == std::string_view::npos || colon > close)) {
    for (size_t i = colon + 1; i < a.size(); ++i)
      if (!is_digit(a[i])) return false;
    a = a.substr(0, colon);
  }
  if (!a.empty() && a.front() == '[') {
    if (a.size() < 3 || a.back() != ']') return false;
    std::string_view inner = a.substr(1, a.size() - 2);
    return is_ipv6(inner) || ipv_future(inner);
  }
  // reg-name; dotted-quad IPv4 addresses are also reg-names here
  return uri_component(a, UriPart::RegName, iri, false);
}

// URI (absolute, scheme required), URI-reference (relative references allowed),
// and the IRI variants when iri is true.
inline bool uri_generic(std::string_view s, bool iri, bool absolute_only) {
  size_t scheme_end = 0;
  bool has_scheme = false;
  if (!s.empty() && is_alpha(s[0])) {
    size_t i = 1;
    while (i < s.size() && (is_alpha(s[i]) || is_digit(s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) ++i;
    if (i < s.size() && s[i] == ':') {
      has_scheme = true;
      scheme_end = i;
    }
  }
  if (absolute_only && !has_scheme) return false;

  std::string_view rest = has_scheme ? s.substr(scheme_end + 1) : s;

  std::string_view frag;
  bool has_frag = false;
  size_t hash = rest.find('#');
  if (hash != std::string_view::npos) {
    frag = rest.substr(hash + 1);
    rest = rest.substr(0, hash);
    has_frag = true;
  }
  std::string_view query;
  bool has_query = false;
  size_t qm = rest.find('?');
  if (qm != std::string_view::npos) {
    query = rest.substr(qm + 1);
    rest = rest.substr(0, qm);
    has_query = true;
  }
  if (has_query && !uri_component(query, UriPart::Query, iri, true)) return false;
  if (has_frag && !uri_component(frag, UriPart::Fragment, iri, true)) return false;

  if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
    std::string_view after = rest.substr(2);
    size_t slash = after.find('/');
    if (!uri_authority(after.substr(0, slash), iri)) return false;
    if (slash != std::string_view::npos && !uri_component(after.substr(slash), UriPart::Path, iri, false))
      return false;
    return true;
  }
  if (!has_scheme && !rest.empty() && rest[0] != '/') {
    // path-noscheme: the first segment cannot contain ':'
    size_t slash = rest.find('/');
    if (rest.substr(0, slash).find(':') != std::string_view::npos) return false;
  }
  return uri_component(rest, UriPart::Path, iri, false);
}

inline bool is_uri(std::string_view s) { return uri_generic(s, false, true); }
inline bool is_uri_reference(std::string_view s) { return uri_generic(s, false, false); }
inline bool is_iri(std::string_view s) { return uri_generic(s, true, true); }
inline bool is_iri_reference(std::string_view s) { return uri_generic(s, true, false); }

// ---- JSON Pointer (RFC 6901) and relative JSON Pointer ----------------------

inline bool is_json_pointer(std::string_view s) {
  if (s.empty()) return true;
  if (s[0] != '/') return false;
  for (size_t i = 1; i < s.size(); ++i) {
    if (s[i] == '~') {
      if (i + 1 >= s.size() || (s[i + 1] != '0' && s[i + 1] != '1')) return false;
      ++i;
    }
  }
  return true;
}

// non-negative-integer, then "#" alone or a JSON Pointer
inline bool is_relative_json_pointer(std::string_view s) {
  size_t i = 0;
  if (i >= s.size() || !is_digit(s[i])) return false;
  if (s[i] == '0') {
    ++i;
  } else {
    while (i < s.size() && is_digit(s[i])) ++i;
  }
  if (i == s.size()) return true;
  if (s[i] == '#') return i + 1 == s.size();
  return is_json_pointer(s.substr(i));
}

// ---- duration (ISO 8601 as used by JSON Schema) ----------------------------

// "P" then Y M D (date) and/or T then H M S (time), in that order, with the
// units present forming one contiguous run; or "P<n>W" on its own.
inline bool is_duration(std::string_view s) {
  if (s.size() < 2 || s[0] != 'P') return false;
  size_t i = 1;
  auto digits = [&](size_t& pos) {
    size_t start = pos;
    while (pos < s.size() && is_digit(s[pos])) ++pos;
    return pos > start;
  };
  bool in_time = false;
  int last = -1, count = 0;
  while (i < s.size()) {
    if (s[i] == 'T') {
      if (in_time) return false;
      in_time = true;
      ++i;
      if (i == s.size()) return false;   // "T" must be followed by a time unit
      continue;
    }
    if (!digits(i) || i >= s.size()) return false;
    char unit = s[i++];
    if (unit == 'W' && !in_time) {
      return count == 0 && i == s.size();   // weeks are only valid on their own
    }
    int idx = -1;
    if (!in_time) {
      idx = unit == 'Y' ? 0 : unit == 'M' ? 1 : unit == 'D' ? 2 : -1;
    } else {
      idx = unit == 'H' ? 3 : unit == 'M' ? 4 : unit == 'S' ? 5 : -1;
    }
    if (idx < 0 || idx <= last) return false;
    if (count > 0 && idx != last + 1) return false;   // skipped a unit
    last = idx;
    ++count;
  }
  return count > 0;
}

// ---- uri-template (RFC 6570) ------------------------------------------------

// varspec list inside one {...} expression
inline bool template_expression(std::string_view body) {
  size_t i = 0;
  const size_t n = body.size();
  if (n > 0 && std::string_view("+#./;?&").find(body[0]) != std::string_view::npos) ++i;
  if (i == n) return false;   // empty expression

  auto varchar_at = [&](size_t& k) {
    if (k >= n) return false;
    if (body[k] == '%') {
      if (k + 2 >= n || !is_hex(body[k + 1]) || !is_hex(body[k + 2])) return false;
      k += 3;
      return true;
    }
    if (is_alpha(body[k]) || is_digit(body[k]) || body[k] == '_') {
      ++k;
      return true;
    }
    return false;
  };
  auto varname = [&]() {
    if (!varchar_at(i)) return false;
    while (true) {
      while (varchar_at(i)) {}
      if (i < n && body[i] == '.') {
        ++i;
        if (!varchar_at(i)) return false;   // no leading, trailing or doubled dots
        continue;
      }
      return true;
    }
  };

  while (true) {
    if (!varname()) return false;
    if (i < n && body[i] == ':') {
      ++i;
      if (i >= n || body[i] == '0' || !is_digit(body[i])) return false;   // 1..9999, no leading zero
      int digits = 0;
      while (i < n && is_digit(body[i])) {
        if (++digits > 4) return false;
        ++i;
      }
    } else if (i < n && body[i] == '*') {
      ++i;
    }
    if (i == n) return true;
    if (body[i] != ',') return false;
    ++i;
    if (i == n) return false;   // trailing comma
  }
}

inline bool is_uri_template(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '{') {
      size_t close = s.find('}', i);
      if (close == std::string_view::npos) return false;
      std::string_view body = s.substr(i + 1, close - i - 1);
      if (body.find('{') != std::string_view::npos) return false;
      if (!template_expression(body)) return false;
      i = close + 1;
      continue;
    }
    if (c == '}') return false;
    if (c == '%') {
      if (i + 2 >= s.size() || !is_hex(s[i + 1]) || !is_hex(s[i + 2])) return false;
      i += 3;
      continue;
    }
    if (c >= 0x80) {
      const char* p = s.data() + i;
      const char* e = s.data() + s.size();
      uint32_t cp = glz::decode_utf8(p, e);
      if (cp == 0xFFFD) return false;
      i = static_cast<size_t>(p - s.data());
      continue;
    }
    // literals exclude controls, space and these ASCII characters
    if (c < 0x20 || c == 0x7F || c == ' ' || c == '"' || c == '<' || c == '>' ||
        c == '\\' || c == '^' || c == '`' || c == '|') {
      return false;
    }
    ++i;
  }
  return true;
}

// Entry point: returns false only for a value the named format rejects.
// Unknown formats and non-string instances are not checked.
inline bool check(std::string_view format, std::string_view text, bool& known) {
  known = true;
  switch (str_to_u64(format)) {
    case str_to_u64("date-time"):
      if (!str_cmp_tail8(format, "date-time")) break;
      return is_date_time(text);
    case str_to_u64("time"):
      if (!str_cmp_tail8(format, "time")) break;
      return is_time(text);
    case str_to_u64("email"):
      if (!str_cmp_tail8(format, "email")) break;
      return is_email(text);
    case str_to_u64("idn-email"):
      if (!str_cmp_tail8(format, "idn-email")) break;
      return is_idn_email(text);
    case str_to_u64("hostname"):
      if (!str_cmp_tail8(format, "hostname")) break;
      return is_hostname(text);
    case str_to_u64("ipv4"):
      if (!str_cmp_tail8(format, "ipv4")) break;
      return is_ipv4(text);
    case str_to_u64("ipv6"):
      if (!str_cmp_tail8(format, "ipv6")) break;
      return is_ipv6(text);
    case str_to_u64("date"):
      if (!str_cmp_tail8(format, "date")) break;
      return is_date(text);
    case str_to_u64("uri"):
      if (!str_cmp_tail8(format, "uri")) break;
      return is_uri(text);
    case str_to_u64("uri-reference"):
      if (!str_cmp_tail8(format, "uri-reference")) break;
      return is_uri_reference(text);
    case str_to_u64("iri"):
      if (!str_cmp_tail8(format, "iri")) break;
      return is_iri(text);
    case str_to_u64("iri-reference"):
      if (!str_cmp_tail8(format, "iri-reference")) break;
      return is_iri_reference(text);
    case str_to_u64("uuid"):
      if (!str_cmp_tail8(format, "uuid")) break;
      return is_uuid(text);
    case str_to_u64("idn-hostname"):
      if (!str_cmp_tail8(format, "idn-hostname")) break;
      return is_idn_hostname(text);
    case str_to_u64("duration"):
      if (!str_cmp_tail8(format, "duration")) break;
      return is_duration(text);
    case str_to_u64("json-pointer"):
      if (!str_cmp_tail8(format, "json-pointer")) break;
      return is_json_pointer(text);
    case str_to_u64("relative-json-pointer"):
      if (!str_cmp_tail8(format, "relative-json-pointer")) break;
      return is_relative_json_pointer(text);
    case str_to_u64("uri-template"):
      if (!str_cmp_tail8(format, "uri-template")) break;
      return is_uri_template(text);
    default:
      break;
  }
  known = false;
  return true;
}


} // namespace glz::formats
