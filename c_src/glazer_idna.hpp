// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// IDNA2008 label validation (RFC 5890-5893) for the hostname and idn-hostname
// formats.
//
//   - RFC 5892 derived properties and the appendix A contextual rules
//     (ZWJ/ZWNJ, MIDDLE DOT, GREEK KERAIA, HEBREW GERESH/GERSHAYIM, KATAKANA
//     MIDDLE DOT, Arabic-Indic digits), from glazer_idna_tables.hpp (generated).
//   - RFC 5893 bidi rule, across the domain: when any label is RTL (contains R,
//     AL or AN), every label must satisfy rules 1-6.
//   - RFC 3492 Punycode, used to decode and check A-labels (xn--...).
//   - UTS #46 mapping (generated table), applied to internationalized names
//     before the IDNA2008 rules, as the idn-hostname format expects.
//
//   - RFC 5891 section 5.4 NFC requirement: a U-label must be unchanged by NFC
//     normalization (UAX #15), using the decomposition, combining class and
//     composition tables in glazer_idna_tables.hpp (generated).
//-----------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "glazer_idna_tables.hpp"
#include "glazer_utf8.hpp"

namespace glz::idna {

enum : uint32_t { DISALLOWED = 0, PVALID = 1, CONTEXTJ = 2, CONTEXTO = 3, UNASSIGNED = 4 };
enum : uint32_t { SC_NONE = 0, SC_GREEK = 1, SC_HEBREW = 2, SC_HIRAGANA = 3, SC_KATAKANA = 4, SC_HAN = 5 };
enum : uint32_t {
  BD_L = 0, BD_R = 1, BD_AL = 2, BD_EN = 3, BD_ES = 4, BD_ET = 5, BD_AN = 6,
  BD_CS = 7, BD_NSM = 8, BD_BN = 9, BD_ON = 10, BD_B = 11, BD_S = 12, BD_WS = 13,
  BD_EXPLICIT = 14, BD_OTHER = 15
};
// ICU's joining types, as stored in the table
enum : uint32_t { JT_NONE = 0, JT_CAUSING = 1, JT_DUAL = 2, JT_LEFT = 3, JT_RIGHT = 4, JT_TRANSPARENT = 5 };

struct Props {
  uint32_t derived, joining, script, virama, bidi, mark;
};

inline Props props(uint32_t cp) {
  const uint32_t* first = s_run_start;
  const uint32_t* pos = std::upper_bound(first, first + s_runs, cp);
  uint32_t v = s_run_value[(pos - first) - 1];
  return Props{v & 7u, (v >> 3) & 7u, (v >> 6) & 7u, (v >> 9) & 1u, (v >> 10) & 15u, (v >> 14) & 1u};
}

// ---- RFC 3492 Punycode -----------------------------------------------------

inline constexpr uint32_t s_base = 36, s_t_min = 1, s_t_max = 26, s_skew = 38, s_damp = 700;
inline constexpr uint32_t s_initial_bias = 72, s_initial_n = 128;

inline uint32_t adapt(uint32_t delta, uint32_t numpoints, bool firsttime) {
  delta = firsttime ? delta / s_damp : delta / 2;
  delta += delta / numpoints;
  uint32_t k = 0;
  while (delta > ((s_base - s_t_min) * s_t_max) / 2) {
    delta /= s_base - s_t_min;
    k += s_base;
  }
  return k + (s_base - s_t_min + 1) * delta / (delta + s_skew);
}

inline char digit_char(uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); }

// Punycode digit values (RFC 3492 section 5): a-z and A-Z are 0-25, 0-9 are
// 26-35. Every other byte maps to 0xFF.
namespace detail {
  constexpr std::array<uint8_t, 256> make_digit_value_table() {
    std::array<uint8_t, 256> t{};
    for (auto& v : t) v = 0xFF;
    for (uint32_t c = 'a'; c <= 'z'; ++c) t[c] = static_cast<uint8_t>(c - 'a');
    for (uint32_t c = 'A'; c <= 'Z'; ++c) t[c] = static_cast<uint8_t>(c - 'A');
    for (uint32_t c = '0'; c <= '9'; ++c) t[c] = static_cast<uint8_t>(c - '0' + 26);
    return t;
  }
}  // namespace detail

inline constexpr std::array<uint8_t, 256> s_digit_value = detail::make_digit_value_table();

inline bool digit_value(char c, uint32_t& d) {
  const uint8_t v = s_digit_value[static_cast<unsigned char>(c)];
  if (v == 0xFF) return false;
  d = v;
  return true;
}

// Decodes the part of an A-label after "xn--". False on malformed input.
inline bool punycode_decode(std::string_view in, std::vector<uint32_t>& out) {
  out.clear();
  size_t b = in.rfind('-');
  if (b != std::string_view::npos) {
    for (size_t j = 0; j < b; ++j) {
      if (static_cast<unsigned char>(in[j]) >= 0x80) return false;
      out.push_back(static_cast<unsigned char>(in[j]));
    }
  }
  size_t pos = (b == std::string_view::npos) ? 0 : b + 1;
  uint32_t n = s_initial_n, i = 0, bias = s_initial_bias;
  while (pos < in.size()) {
    uint32_t oldi = i, w = 1;
    for (uint32_t k = s_base;; k += s_base) {
      if (pos >= in.size()) return false;
      uint32_t d = 0;
      if (!digit_value(in[pos++], d)) return false;
      if (d > (UINT32_MAX - i) / w) return false;
      i += d * w;
      uint32_t t = k <= bias ? s_t_min : (k >= bias + s_t_max ? s_t_max : k - bias);
      if (d < t) break;
      if (w > UINT32_MAX / (s_base - t)) return false;
      w *= s_base - t;
    }
    uint32_t len = static_cast<uint32_t>(out.size()) + 1;
    bias = adapt(i - oldi, len, oldi == 0);
    if (i / len > UINT32_MAX - n) return false;
    n += i / len;
    i %= len;
    if (n > 0x10FFFF) return false;
    out.insert(out.begin() + i, n);
    ++i;
  }
  return true;
}

inline std::string punycode_encode(const std::vector<uint32_t>& cps) {
  std::string out;
  uint32_t b = 0;
  for (uint32_t cp : cps)
    if (cp < 0x80) { out += static_cast<char>(cp); ++b; }
  uint32_t h = b;
  if (b > 0) out += '-';
  uint32_t n = s_initial_n, delta = 0, bias = s_initial_bias;
  while (h < cps.size()) {
    uint32_t m = UINT32_MAX;
    for (uint32_t cp : cps)
      if (cp >= n && cp < m) m = cp;
    delta += (m - n) * (h + 1);
    n = m;
    for (uint32_t cp : cps) {
      if (cp < n) ++delta;
      if (cp == n) {
        uint32_t q = delta;
        for (uint32_t k = s_base;; k += s_base) {
          uint32_t t = k <= bias ? s_t_min : (k >= bias + s_t_max ? s_t_max : k - bias);
          if (q < t) break;
          out += digit_char(t + (q - t) % (s_base - t));
          q = (q - t) / (s_base - t);
        }
        out += digit_char(q);
        bias = adapt(delta, h + 1, h == b);
        delta = 0;
        ++h;
      }
    }
    ++delta;
    ++n;
  }
  return out;
}

// ---- NFC (UAX #15) -----------------------------------------------------------

inline constexpr uint32_t s_s_base = 0xAC00, s_l_base = 0x1100, s_v_base = 0x1161, s_t_base = 0x11A7;
inline constexpr uint32_t s_l_count = 19, s_v_count = 21, s_t_count = 28;
inline constexpr uint32_t s_n_count = s_v_count * s_t_count, s_s_count = s_l_count * s_n_count;

// Canonical combining class (0 for starters)
inline uint32_t ccc_of(uint32_t cp) {
  const uint32_t* first = s_ccc_start;
  const uint32_t* pos = std::upper_bound(first, first + s_ccc_runs, cp);
  return s_ccc_value[(pos - first) - 1];
}

// Full canonical decomposition of one code point (Hangul is algorithmic)
inline void decompose_into(uint32_t cp, std::vector<uint32_t>& out) {
  if (cp >= s_s_base && cp < s_s_base + s_s_count) {
    uint32_t s = cp - s_s_base;
    out.push_back(s_l_base + s / s_n_count);
    out.push_back(s_v_base + (s % s_n_count) / s_t_count);
    if (uint32_t t = s % s_t_count) out.push_back(s_t_base + t);
    return;
  }
  const uint32_t* end = s_decomp_cp + s_decomp_count;
  const uint32_t* it = std::lower_bound(s_decomp_cp, end, cp);
  if (it != end && *it == cp) {
    size_t i = static_cast<size_t>(it - s_decomp_cp);
    for (uint32_t k = s_decomp_off[i]; k < s_decomp_off[i + 1]; ++k) out.push_back(s_decomp_seq[k]);
    return;
  }
  out.push_back(cp);
}

// Primary composite of a starter and a following character, if any
inline bool compose_pair(uint32_t a, uint32_t b, uint32_t& c) {
  if (a >= s_l_base && a < s_l_base + s_l_count && b >= s_v_base && b < s_v_base + s_v_count) {
    c = s_s_base + ((a - s_l_base) * s_v_count + (b - s_v_base)) * s_t_count;
    return true;
  }
  if (a >= s_s_base && a < s_s_base + s_s_count && (a - s_s_base) % s_t_count == 0 && b > s_t_base && b < s_t_base + s_t_count) {
    c = a + (b - s_t_base);
    return true;
  }
  uint64_t key = (static_cast<uint64_t>(a) << 21) | b;
  const uint64_t* end = s_comp_key + s_comp_count;
  const uint64_t* it = std::lower_bound(s_comp_key, end, key);
  if (it != end && *it == key) {
    c = s_comp_value[it - s_comp_key];
    return true;
  }
  return false;
}

// NFC of a sequence of code points (UAX #15: decompose, reorder, compose)
inline std::vector<uint32_t> normalize_nfc(const std::vector<uint32_t>& in) {
  std::vector<uint32_t> d;
  d.reserve(in.size() * 2);
  for (uint32_t cp : in) decompose_into(cp, d);

  // canonical ordering: stable sort of each run of non-starters by class
  for (size_t i = 0; i < d.size();) {
    if (ccc_of(d[i]) == 0) { ++i; continue; }
    size_t j = i;
    while (j < d.size() && ccc_of(d[j]) != 0) ++j;
    std::stable_sort(d.begin() + i, d.begin() + j, [](uint32_t a, uint32_t b) { return ccc_of(a) < ccc_of(b); });
    i = j;
  }

  // canonical composition
  std::vector<uint32_t> out;
  out.reserve(d.size());
  size_t starter = SIZE_MAX;
  int64_t last_cc = -1;   // class of the last character kept after the starter
  for (uint32_t c : d) {
    uint32_t cc = ccc_of(c);
    if (starter != SIZE_MAX) {
      bool blocked = last_cc != -1 && last_cc >= static_cast<int64_t>(cc);
      uint32_t comp;
      if (!blocked && compose_pair(out[starter], c, comp)) {
        out[starter] = comp;
        continue;
      }
    }
    if (cc == 0) {
      starter = out.size();
      last_cc = -1;
    } else if (starter != SIZE_MAX) {
      last_cc = cc;
    }
    out.push_back(c);
  }
  return out;
}

// True when the sequence is unchanged by NFC normalization
inline bool is_nfc(const std::vector<uint32_t>& cps) {
  if (std::all_of(cps.begin(), cps.end(), [](uint32_t c) { return c < 0x80; })) return true;
  return normalize_nfc(cps) == cps;
}

// ---- label validation -------------------------------------------------------

inline bool is_rtl_class(uint32_t bd) { return bd == BD_R || bd == BD_AL || bd == BD_AN; }

// RFC 5893 rules 1-6 for one label, from the bidi classes of its code points
inline bool bidi_label_ok(const std::vector<uint32_t>& classes) {
  if (classes.empty()) return false;
  bool rtl = std::any_of(classes.begin(), classes.end(), is_rtl_class);
  // rule 1: the first character is L, R or AL
  uint32_t first = classes.front();
  if (first != BD_L && first != BD_R && first != BD_AL) return false;
  bool has_en = false, has_an = false;
  for (uint32_t bd : classes) {
    if (bd == BD_EN) has_en = true;
    if (bd == BD_AN) has_an = true;
  }
  // the last character that is not NSM
  uint32_t last = BD_NSM;
  for (size_t k = classes.size(); k-- > 0;)
    if (classes[k] != BD_NSM) { last = classes[k]; break; }
  if (rtl) {
    // rule 2: only R, AL, AN, EN, ES, CS, ET, ON, BN, NSM
    for (uint32_t bd : classes) {
      switch (bd) {
        case BD_R: case BD_AL: case BD_AN: case BD_EN: case BD_ES: case BD_CS:
        case BD_ET: case BD_ON: case BD_BN: case BD_NSM:
          break;
        default:
          return false;
      }
    }
    // rule 3: ends with R, AL, EN or AN
    if (last != BD_R && last != BD_AL && last != BD_EN && last != BD_AN) return false;
    // rule 4: EN and AN do not both occur
    return !(has_en && has_an);
  }
  // rule 5: only L, EN, ES, CS, ET, ON, BN, NSM
  for (uint32_t bd : classes) {
    switch (bd) {
      case BD_L: case BD_EN: case BD_ES: case BD_CS: case BD_ET: case BD_ON: case BD_BN: case BD_NSM:
        break;
      default:
        return false;
    }
  }
  // rule 6: ends with L or EN
  return last == BD_L || last == BD_EN;
}

// RFC 5893 is a rule for the whole domain: a Bidi domain (one with at least one
// RTL label) must have every label satisfy the rules, including LTR labels.
// Labels are given as code points (decoded for A-labels).
inline bool domain_bidi_ok(const std::vector<std::vector<uint32_t>>& labels) {
  bool bidi_domain = false;
  std::vector<std::vector<uint32_t>> classes;
  classes.reserve(labels.size());
  for (const auto& cps : labels) {
    std::vector<uint32_t> c;
    c.reserve(cps.size());
    for (uint32_t cp : cps) {
      uint32_t bd = props(cp).bidi;
      c.push_back(bd);
      if (is_rtl_class(bd)) bidi_domain = true;
    }
    classes.push_back(std::move(c));
  }
  if (!bidi_domain) return true;
  for (const auto& c : classes)
    if (!bidi_label_ok(c)) return false;
  return true;
}

// RFC 5892 appendix A contextual rules for CONTEXTJ and CONTEXTO code points
inline bool context_rule(const std::vector<uint32_t>& cps, size_t i) {
  uint32_t cp = cps[i];
  auto has_virama_before = [&]() { return i > 0 && props(cps[i - 1]).virama == 1; };
  switch (cp) {
    case 0x200D:   // ZWJ: preceded by virama
      return has_virama_before();
    case 0x200C: { // ZWNJ: preceded by virama, or joining context
      if (has_virama_before()) return true;
      // (L or D) T* ZWNJ T* (R or D)
      size_t a = i;
      bool before = false;
      while (a > 0) {
        uint32_t jt = props(cps[a - 1]).joining;
        if (jt == JT_TRANSPARENT) { --a; continue; }
        before = (jt == JT_LEFT || jt == JT_DUAL);
        break;
      }
      if (!before) return false;
      size_t b = i + 1;
      while (b < cps.size()) {
        uint32_t jt = props(cps[b]).joining;
        if (jt == JT_TRANSPARENT) { ++b; continue; }
        return jt == JT_RIGHT || jt == JT_DUAL;
      }
      return false;
    }
    case 0x00B7:   // MIDDLE DOT: between two 'l'
      return i > 0 && i + 1 < cps.size() && cps[i - 1] == 'l' && cps[i + 1] == 'l';
    case 0x0375:   // GREEK LOWER NUMERAL SIGN: followed by Greek
      return i + 1 < cps.size() && props(cps[i + 1]).script == SC_GREEK;
    case 0x05F3:   // HEBREW PUNCTUATION GERESH / GERSHAYIM: preceded by Hebrew
    case 0x05F4:
      return i > 0 && props(cps[i - 1]).script == SC_HEBREW;
    case 0x30FB: { // KATAKANA MIDDLE DOT: label has Hiragana, Katakana or Han
      for (size_t k = 0; k < cps.size(); ++k) {
        if (k == i) continue;
        uint32_t sc = props(cps[k]).script;
        if (sc == SC_HIRAGANA || sc == SC_KATAKANA || sc == SC_HAN) return true;
      }
      return false;
    }
    default:
      break;
  }
  if (cp >= 0x0660 && cp <= 0x0669) {   // Arabic-Indic digits: not with extended ones
    for (uint32_t c : cps) if (c >= 0x06F0 && c <= 0x06F9) return false;
    return true;
  }
  if (cp >= 0x06F0 && cp <= 0x06F9) {   // extended Arabic-Indic digits: not with the others
    for (uint32_t c : cps) if (c >= 0x0660 && c <= 0x0669) return false;
    return true;
  }
  return false;
}

// A U-label (decoded form) under IDNA2008
inline bool valid_u_label(const std::vector<uint32_t>& cps) {
  if (cps.empty()) return false;
  if (!is_nfc(cps)) return false;                                // RFC 5891 section 5.4
  if (props(cps.front()).mark) return false;                     // no leading combining mark
  if (cps.front() == '-' || cps.back() == '-') return false;     // no leading or trailing hyphen
  if (cps.size() >= 4 && cps[2] == '-' && cps[3] == '-') return false;   // no "--" in positions 3-4
  for (size_t i = 0; i < cps.size(); ++i) {
    switch (props(cps[i]).derived) {
      case PVALID:
        break;
      case CONTEXTJ:
      case CONTEXTO:
        if (!context_rule(cps, i)) return false;
        break;
      default:
        return false;
    }
  }
  return true;   // RFC 5893 is checked across the domain (domain_bidi_ok)
}

inline bool ascii_ci_prefix_xn(std::string_view label) {
  return label.size() >= 4 && (label[0] == 'x' || label[0] == 'X') && (label[1] == 'n' || label[1] == 'N') &&
         label[2] == '-' && label[3] == '-';
}

// An A-label (xn--...): must decode to a valid U-label that is non-ASCII and
// whose canonical Punycode is exactly the input (case-insensitively)
inline bool valid_a_label(std::string_view label, std::vector<uint32_t>* decoded = nullptr) {
  if (!ascii_ci_prefix_xn(label)) return false;
  std::string_view body = label.substr(4);
  if (body.empty()) return false;
  std::vector<uint32_t> cps;
  if (!punycode_decode(body, cps)) return false;
  bool non_ascii = std::any_of(cps.begin(), cps.end(), [](uint32_t c) { return c >= 0x80; });
  if (!non_ascii) return false;
  if (!valid_u_label(cps)) return false;
  std::string canonical = punycode_encode(cps);
  if (canonical.size() != body.size()) return false;
  for (size_t i = 0; i < body.size(); ++i) {
    char a = body[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (a != canonical[i]) return false;
  }
  if (decoded) *decoded = cps;
  return true;
}

// An ASCII label that is not an A-label: letters, digits and hyphens, not at
// either end (RFC 1123). The "--" rule of RFC 5891 applies to A- and U-labels
// only, so "ab--cd" is a valid plain label.
inline bool valid_ldh_label(std::string_view label) {
  if (label.empty() || label.size() > 63) return false;
  if (label.front() == '-' || label.back() == '-') return false;
  for (char c : label)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

// hostname (IDNA2008 ASCII form): every label is LDH or an A-label, 1-63 octets.
// The label's code points (decoded for an A-label) are returned in decoded.
inline bool valid_hostname_label(std::string_view label, std::vector<uint32_t>* decoded = nullptr) {
  if (label.size() > 63) return false;
  if (ascii_ci_prefix_xn(label)) return valid_a_label(label, decoded);
  if (!valid_ldh_label(label)) return false;
  if (decoded) decoded->assign(label.begin(), label.end());
  return true;
}

// ---- UTS #46 mapping (applied before the IDNA2008 rules) ---------------------
// Mapping data is in glazer_idna_tables.hpp (generated). ASCII outside LDH is
// not mapped away, so the IDNA2008 label rules still reject it.

enum : uint32_t { UTS_MAPPED = 1, UTS_IGNORED = 2, UTS_DISALLOWED = 3 };

// Appends the UTS 46 mapping of cp to out. False when cp is disallowed.
// Entries are ranges start..end, sorted and non-overlapping; a code point
// outside every range is valid and unchanged.
inline bool uts46_map(uint32_t cp, std::vector<uint32_t>& out) {
  const uint32_t* end = s_uts46_start + s_uts46_count;
  const uint32_t* it = std::upper_bound(s_uts46_start, end, cp);
  if (it == s_uts46_start) {   // below the first entry: valid
    out.push_back(cp);
    return true;
  }
  size_t i = static_cast<size_t>(it - s_uts46_start) - 1;
  if (cp > s_uts46_end[i]) {   // in a gap between entries: valid
    out.push_back(cp);
    return true;
  }
  switch (s_uts46_type[i]) {
    case UTS_IGNORED:
      return true;
    case UTS_MAPPED:
      for (uint32_t k = s_uts46_off[i]; k < s_uts46_off[i + 1]; ++k) out.push_back(s_uts46_seq[k]);
      return true;
    default:
      return false;
  }
}

// UTS 46 processing of a domain name: map every code point, normalize to NFC,
// then split at the dots (after mapping, the ideographic and fullwidth full
// stops are dots). False on invalid UTF-8 or a disallowed code point.
inline bool uts46_labels(std::string_view domain, std::vector<std::vector<uint32_t>>& labels) {
  if (!glz::valid_utf8(domain)) return false;
  std::vector<uint32_t> mapped;
  mapped.reserve(domain.size());
  const char* p = domain.data();
  const char* e = p + domain.size();
  while (p < e) {
    if (!uts46_map(glz::decode_utf8(p, e), mapped)) return false;
  }
  std::vector<uint32_t> nfc = normalize_nfc(mapped);
  labels.assign(1, {});
  for (uint32_t cp : nfc) {
    if (cp == '.') labels.emplace_back();
    else labels.back().push_back(cp);
  }
  return true;
}

// Internationalized domain name (idn-hostname, and the domain of idn-email):
// UTS 46 mapping and NFC, then each label is an ASCII LDH label, an A-label, or
// a U-label that passes the IDNA2008 rules. Each ASCII form is at most 63
// octets and the whole name at most 253.
inline bool valid_idn_domain(std::string_view domain) {
  std::vector<std::vector<uint32_t>> labels;
  if (!uts46_labels(domain, labels)) return false;
  size_t total = labels.size() - 1;   // dots between labels
  std::vector<std::vector<uint32_t>> decoded;
  decoded.reserve(labels.size());
  for (const auto& cps : labels) {
    bool ascii = std::all_of(cps.begin(), cps.end(), [](uint32_t c) { return c < 0x80; });
    std::string form;
    std::vector<uint32_t> code_points;
    if (ascii) {
      form.assign(cps.begin(), cps.end());
      if (!valid_hostname_label(form, &code_points)) return false;
    } else {
      if (cps.size() >= 4 && (cps[0] | 0x20) == 'x' && (cps[1] | 0x20) == 'n' && cps[2] == '-' && cps[3] == '-')
        return false;                   // an A-label must not mix with non-ASCII
      if (!valid_u_label(cps)) return false;
      code_points = cps;
      form = "xn--" + punycode_encode(cps);
      if (form.size() > 63) return false;
    }
    total += form.size();
    decoded.push_back(std::move(code_points));
  }
  return total <= 253 && domain_bidi_ok(decoded);
}

// hostname (ASCII, with A-labels): the same label and domain rules, with no mapping
inline bool valid_ascii_domain(std::string_view s) {
  if (s.empty() || s.size() > 253) return false;
  std::vector<std::vector<uint32_t>> decoded;
  size_t start = 0;
  while (true) {
    size_t dot = s.find('.', start);
    std::string_view label = s.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    decoded.emplace_back();
    if (!valid_hostname_label(label, &decoded.back())) return false;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return domain_bidi_ok(decoded);
}

} // namespace glz::idna
