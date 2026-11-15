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
  const uint32_t* first = kRunStart;
  const uint32_t* pos = std::upper_bound(first, first + kRuns, cp);
  uint32_t v = kRunValue[(pos - first) - 1];
  return Props{v & 7u, (v >> 3) & 7u, (v >> 6) & 7u, (v >> 9) & 1u, (v >> 10) & 15u, (v >> 14) & 1u};
}

// ---- RFC 3492 Punycode -----------------------------------------------------

inline constexpr uint32_t kBase = 36, kTmin = 1, kTmax = 26, kSkew = 38, kDamp = 700;
inline constexpr uint32_t kInitialBias = 72, kInitialN = 128;

inline uint32_t adapt(uint32_t delta, uint32_t numpoints, bool firsttime) {
  delta = firsttime ? delta / kDamp : delta / 2;
  delta += delta / numpoints;
  uint32_t k = 0;
  while (delta > ((kBase - kTmin) * kTmax) / 2) {
    delta /= kBase - kTmin;
    k += kBase;
  }
  return k + (kBase - kTmin + 1) * delta / (delta + kSkew);
}

inline char digit_char(uint32_t d) { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); }

inline bool digit_value(char c, uint32_t& d) {
  if (c >= 'a' && c <= 'z') d = static_cast<uint32_t>(c - 'a');
  else if (c >= 'A' && c <= 'Z') d = static_cast<uint32_t>(c - 'A');
  else if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0' + 26);
  else return false;
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
  uint32_t n = kInitialN, i = 0, bias = kInitialBias;
  while (pos < in.size()) {
    uint32_t oldi = i, w = 1;
    for (uint32_t k = kBase;; k += kBase) {
      if (pos >= in.size()) return false;
      uint32_t d = 0;
      if (!digit_value(in[pos++], d)) return false;
      if (d > (UINT32_MAX - i) / w) return false;
      i += d * w;
      uint32_t t = k <= bias ? kTmin : (k >= bias + kTmax ? kTmax : k - bias);
      if (d < t) break;
      if (w > UINT32_MAX / (kBase - t)) return false;
      w *= kBase - t;
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
  uint32_t n = kInitialN, delta = 0, bias = kInitialBias;
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
        for (uint32_t k = kBase;; k += kBase) {
          uint32_t t = k <= bias ? kTmin : (k >= bias + kTmax ? kTmax : k - bias);
          if (q < t) break;
          out += digit_char(t + (q - t) % (kBase - t));
          q = (q - t) / (kBase - t);
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

inline constexpr uint32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
inline constexpr uint32_t kLCount = 19, kVCount = 21, kTCount = 28;
inline constexpr uint32_t kNCount = kVCount * kTCount, kSCount = kLCount * kNCount;

// Canonical combining class (0 for starters)
inline uint32_t ccc_of(uint32_t cp) {
  const uint32_t* first = kCccStart;
  const uint32_t* pos = std::upper_bound(first, first + kCccRuns, cp);
  return kCccValue[(pos - first) - 1];
}

// Full canonical decomposition of one code point (Hangul is algorithmic)
inline void decompose_into(uint32_t cp, std::vector<uint32_t>& out) {
  if (cp >= kSBase && cp < kSBase + kSCount) {
    uint32_t s = cp - kSBase;
    out.push_back(kLBase + s / kNCount);
    out.push_back(kVBase + (s % kNCount) / kTCount);
    if (uint32_t t = s % kTCount) out.push_back(kTBase + t);
    return;
  }
  const uint32_t* end = kDecompCp + kDecompCount;
  const uint32_t* it = std::lower_bound(kDecompCp, end, cp);
  if (it != end && *it == cp) {
    size_t i = static_cast<size_t>(it - kDecompCp);
    for (uint32_t k = kDecompOff[i]; k < kDecompOff[i + 1]; ++k) out.push_back(kDecompSeq[k]);
    return;
  }
  out.push_back(cp);
}

// Primary composite of a starter and a following character, if any
inline bool compose_pair(uint32_t a, uint32_t b, uint32_t& c) {
  if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount) {
    c = kSBase + ((a - kLBase) * kVCount + (b - kVBase)) * kTCount;
    return true;
  }
  if (a >= kSBase && a < kSBase + kSCount && (a - kSBase) % kTCount == 0 && b > kTBase && b < kTBase + kTCount) {
    c = a + (b - kTBase);
    return true;
  }
  uint64_t key = (static_cast<uint64_t>(a) << 21) | b;
  const uint64_t* end = kCompKey + kCompCount;
  const uint64_t* it = std::lower_bound(kCompKey, end, key);
  if (it != end && *it == key) {
    c = kCompValue[it - kCompKey];
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
inline bool uts46_map(uint32_t cp, std::vector<uint32_t>& out) {
  const uint32_t* end = kUts46Cp + kUts46Count;
  const uint32_t* it = std::lower_bound(kUts46Cp, end, cp);
  if (it == end || *it != cp) {   // valid: not listed
    out.push_back(cp);
    return true;
  }
  size_t i = static_cast<size_t>(it - kUts46Cp);
  switch (kUts46Type[i]) {
    case UTS_IGNORED:
      return true;
    case UTS_MAPPED:
      for (uint32_t k = kUts46Off[i]; k < kUts46Off[i + 1]; ++k) out.push_back(kUts46Seq[k]);
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
