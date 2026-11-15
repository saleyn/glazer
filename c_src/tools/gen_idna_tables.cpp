// Generates c_src/glazer_idna_tables.hpp: IDNA2008 (RFC 5892, RFC 5893) lookup
// tables, the UTS #46 mapping table, and the NFC tables (UAX #15), from ICU's
// Unicode data. Development-time tool; not part of the NIF.
//
//   g++ -std=c++17 -O2 c_src/tools/gen_idna_tables.cpp -o gen_idna $(pkg-config --cflags --libs icu-uc)
//   ./gen_idna > c_src/glazer_idna_tables.hpp
//
// Why C++ and ICU, rather than Python or Erlang:
//   - ICU is the only source here for the UTS #46 mapping (per code point, via
//     icu::IDNA), the IDNA2008 derived properties (via the case, script and
//     joining data), and the canonical composition pairs (via Normalizer2).
//   - Python's unicodedata has NFC/NFD and bidi classes, but no UTS #46 mapping
//     and no joining types, and its Unicode version can differ from ICU's, so
//     the tables would not match what the rest of the pipeline assumes.
//     PyICU would fix that, but adds a Python dependency for one build step.
//   - Erlang's unicode module offers NFC/NFD only; it exposes no UTS #46 or
//     IDNA property data, so it cannot produce these tables.
//   - C++ uses the toolchain and icu-uc package the NIF build already needs,
//     so the generator adds no new dependency. The output records the Unicode
//     version it was built from; regenerate when ICU is upgraded.
#include <unicode/idna.h>
#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uscript.h>
#include <unicode/uversion.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

// Derived property values (RFC 5892 section 2)
enum Derived : uint32_t { DISALLOWED = 0, PVALID = 1, CONTEXTJ = 2, CONTEXTO = 3, UNASSIGNED = 4 };

// Script codes the contextual rules need
enum Script : uint32_t { SC_NONE = 0, SC_GREEK = 1, SC_HEBREW = 2, SC_HIRAGANA = 3, SC_KATAKANA = 4, SC_HAN = 5 };

// Compact bidi classes (RFC 5893 needs these)
enum Bidi : uint32_t {
  BD_L = 0, BD_R = 1, BD_AL = 2, BD_EN = 3, BD_ES = 4, BD_ET = 5, BD_AN = 6,
  BD_CS = 7, BD_NSM = 8, BD_BN = 9, BD_ON = 10, BD_B = 11, BD_S = 12, BD_WS = 13,
  BD_EXPLICIT = 14, BD_OTHER = 15
};

// RFC 5892 section 2.6 exceptions (the F list)
Derived exception(UChar32 cp, bool& found) {
  found = true;
  if (cp >= 0x0660 && cp <= 0x0669) return CONTEXTO;
  if (cp >= 0x06F0 && cp <= 0x06F9) return CONTEXTO;
  if (cp >= 0x3031 && cp <= 0x3035) return DISALLOWED;
  switch (cp) {
    case 0x00B7: return CONTEXTO;
    case 0x00DF: return PVALID;
    case 0x03C2: return PVALID;
    case 0x06FD: return PVALID;
    case 0x06FE: return PVALID;
    case 0x0F0B: return PVALID;
    case 0x3007: return PVALID;
    case 0x0375: return CONTEXTO;
    case 0x05F3: return CONTEXTO;
    case 0x05F4: return CONTEXTO;
    case 0x30FB: return CONTEXTO;
    case 0x0640: return DISALLOWED;
    case 0x07FA: return DISALLOWED;
    case 0x302E: return DISALLOWED;
    case 0x302F: return DISALLOWED;
    case 0x303B: return DISALLOWED;
    case 0x200C: return CONTEXTJ;
    case 0x200D: return CONTEXTJ;
    default: found = false; return DISALLOWED;
  }
}

// RFC 5892 section 2.4: NFKC(toCasefold(NFKC(cp))) differs from cp
bool unstable(UChar32 cp, const icu::Normalizer2* nfkc) {
  UErrorCode err = U_ZERO_ERROR;
  icu::UnicodeString s = icu::UnicodeString::fromUTF32(&cp, 1);
  icu::UnicodeString a = nfkc->normalize(s, err);
  a.foldCase(U_FOLD_CASE_DEFAULT);
  icu::UnicodeString b = nfkc->normalize(a, err);
  return b != s;
}

Derived derive(UChar32 cp, const icu::Normalizer2* nfkc) {
  if (static_cast<UCharCategory>(u_charType(cp)) == U_UNASSIGNED) return UNASSIGNED;
  // LDH (lowercase only; uppercase is caught by "unstable")
  if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') || cp == '-') return PVALID;
  bool found = false;
  Derived ex = exception(cp, found);
  if (found) return ex;
  if (unstable(cp, nfkc)) return DISALLOWED;
  if (u_hasBinaryProperty(cp, UCHAR_DEFAULT_IGNORABLE_CODE_POINT) || u_hasBinaryProperty(cp, UCHAR_WHITE_SPACE))
    return DISALLOWED;
  if (cp >= 0x20D0 && cp <= 0x20FF) return DISALLOWED;   // Combining Diacritical Marks for Symbols
  switch (static_cast<UCharCategory>(u_charType(cp))) {
    case U_LOWERCASE_LETTER: case U_UPPERCASE_LETTER: case U_OTHER_LETTER:
    case U_DECIMAL_DIGIT_NUMBER: case U_MODIFIER_LETTER: case U_NON_SPACING_MARK:
    case U_COMBINING_SPACING_MARK:
      return PVALID;
    default:
      return DISALLOWED;
  }
}

Script script_code(UChar32 cp) {
  UErrorCode err = U_ZERO_ERROR;
  UScriptCode sc = uscript_getScript(cp, &err);
  if (U_FAILURE(err)) return SC_NONE;
  switch (sc) {
    case USCRIPT_GREEK: return SC_GREEK;
    case USCRIPT_HEBREW: return SC_HEBREW;
    case USCRIPT_HIRAGANA: return SC_HIRAGANA;
    case USCRIPT_KATAKANA: return SC_KATAKANA;
    case USCRIPT_HAN: return SC_HAN;
    default: return SC_NONE;
  }
}

Bidi bidi_code(UChar32 cp) {
  switch (u_charDirection(cp)) {
    case U_LEFT_TO_RIGHT: return BD_L;
    case U_RIGHT_TO_LEFT: return BD_R;
    case U_RIGHT_TO_LEFT_ARABIC: return BD_AL;
    case U_EUROPEAN_NUMBER: return BD_EN;
    case U_EUROPEAN_NUMBER_SEPARATOR: return BD_ES;
    case U_EUROPEAN_NUMBER_TERMINATOR: return BD_ET;
    case U_ARABIC_NUMBER: return BD_AN;
    case U_COMMON_NUMBER_SEPARATOR: return BD_CS;
    case U_DIR_NON_SPACING_MARK: return BD_NSM;
    case U_BOUNDARY_NEUTRAL: return BD_BN;
    case U_OTHER_NEUTRAL: return BD_ON;
    case U_BLOCK_SEPARATOR: return BD_B;
    case U_SEGMENT_SEPARATOR: return BD_S;
    case U_WHITE_SPACE_NEUTRAL: return BD_WS;
    case U_LEFT_TO_RIGHT_EMBEDDING: case U_LEFT_TO_RIGHT_OVERRIDE:
    case U_RIGHT_TO_LEFT_EMBEDDING: case U_RIGHT_TO_LEFT_OVERRIDE:
    case U_POP_DIRECTIONAL_FORMAT: case U_LEFT_TO_RIGHT_ISOLATE:
    case U_RIGHT_TO_LEFT_ISOLATE: case U_FIRST_STRONG_ISOLATE:
    case U_POP_DIRECTIONAL_ISOLATE:
      return BD_EXPLICIT;
    default: return BD_OTHER;
  }
}

// Packed fields, low to high: derived (3), joining (3), script (3), virama (1),
// bidi (4), mark (1)
uint32_t pack(UChar32 cp, const icu::Normalizer2* nfkc) {
  uint32_t derived = derive(cp, nfkc);
  uint32_t joining = static_cast<uint32_t>(u_getIntPropertyValue(cp, UCHAR_JOINING_TYPE));
  uint32_t script = script_code(cp);
  uint32_t virama = u_getCombiningClass(cp) == 9 ? 1 : 0;
  uint32_t bidi = bidi_code(cp);
  UCharCategory gc = static_cast<UCharCategory>(u_charType(cp));
  uint32_t mark = (gc == U_NON_SPACING_MARK || gc == U_COMBINING_SPACING_MARK || gc == U_ENCLOSING_MARK) ? 1 : 0;
  return derived | (joining << 3) | (script << 6) | (virama << 9) | (bidi << 10) | (mark << 14);
}

} // namespace

int main() {
  UErrorCode err = U_ZERO_ERROR;
  const icu::Normalizer2* nfkc = icu::Normalizer2::getNFKCInstance(err);
  if (U_FAILURE(err)) { std::fprintf(stderr, "ICU: %s\n", u_errorName(err)); return 1; }

  UVersionInfo v;
  u_getUnicodeVersion(v);

  // Run-length encode the packed values over the whole code space
  std::vector<std::pair<uint32_t, uint32_t>> runs;   // (start, packed)
  uint32_t prev = 0xFFFFFFFFu;
  for (UChar32 cp = 0; cp <= 0x10FFFF; ++cp) {
    uint32_t p = pack(cp, nfkc);
    if (p != prev) {
      runs.emplace_back(static_cast<uint32_t>(cp), p);
      prev = p;
    }
  }

  std::printf("// AUTO-GENERATED by c_src/tools/gen_idna_tables.cpp -- do not edit.\n");
  std::printf("// Unicode %d.%d (ICU). IDNA2008 derived properties (RFC 5892) and the\n", v[0], v[1]);
  std::printf("// context data for RFC 5892 appendix A and RFC 5893.\n");
  std::printf("// Packed value, low to high bits: derived (3), joining (3), script (3),\n");
  std::printf("// virama (1), bidi (4), mark (1).\n");
  std::printf("#pragma once\n\n#include <cstdint>\n\nnamespace glz::idna {\n\n");
  std::printf("inline constexpr uint32_t kRunStart[] = {\n");
  for (auto& r : runs) std::printf("  0x%X,\n", r.first);
  std::printf("};\n\ninline constexpr uint32_t kRunValue[] = {\n");
  for (auto& r : runs) std::printf("  0x%X,\n", r.second);
  std::printf("};\n\ninline constexpr uint32_t kRuns = %zu;\n\n", runs.size());

  // ---- NFC data (UAX #15; RFC 5891 section 5.4) ----
  UErrorCode nerr = U_ZERO_ERROR;
  const icu::Normalizer2* nfd = icu::Normalizer2::getNFDInstance(nerr);
  if (U_FAILURE(nerr)) { std::fprintf(stderr, "ICU NFD: %s\n", u_errorName(nerr)); return 1; }

  // Canonical combining class, run-length encoded
  std::vector<std::pair<uint32_t, uint32_t>> ccc_runs;
  prev = 0xFFFFFFFFu;
  for (UChar32 cp = 0; cp <= 0x10FFFF; ++cp) {
    uint32_t c = u_getCombiningClass(cp);
    if (c != prev) {
      ccc_runs.emplace_back(static_cast<uint32_t>(cp), c);
      prev = c;
    }
  }

  // Full canonical decompositions (NFD). Hangul syllables are algorithmic and
  // are not listed; surrogates have no mapping.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> decomp;
  for (UChar32 cp = 0; cp <= 0x10FFFF; ++cp) {
    if (cp >= 0xAC00 && cp <= 0xD7A3) continue;
    if (cp >= 0xD800 && cp <= 0xDFFF) continue;
    icu::UnicodeString s1 = icu::UnicodeString::fromUTF32(&cp, 1);
    icu::UnicodeString d = nfd->normalize(s1, nerr);
    if (d == s1) continue;
    std::vector<uint32_t> seq;
    for (int32_t i = 0; i < d.length();) {
      UChar32 c = d.char32At(i);
      seq.push_back(static_cast<uint32_t>(c));
      i += (c >= 0x10000) ? 2 : 1;
    }
    decomp.emplace_back(static_cast<uint32_t>(cp), std::move(seq));
  }

  // Primary composites: one-level canonical pairs (a, b) -> c where
  // composePair(a, b) == c, so excluded and singleton mappings are left out
  const icu::Normalizer2* nfc = icu::Normalizer2::getNFCInstance(nerr);
  if (U_FAILURE(nerr)) { std::fprintf(stderr, "ICU NFC: %s\n", u_errorName(nerr)); return 1; }
  std::vector<std::pair<uint64_t, uint32_t>> comps;
  for (UChar32 cp = 0; cp <= 0x10FFFF; ++cp) {
    if (cp >= 0xAC00 && cp <= 0xD7A3) continue;
    if (cp >= 0xD800 && cp <= 0xDFFF) continue;
    icu::UnicodeString raw;
    if (!nfc->getRawDecomposition(cp, raw)) continue;
    if (raw.countChar32() != 2) continue;
    UChar32 a = raw.char32At(0);
    UChar32 b = raw.char32At((a >= 0x10000) ? 2 : 1);
    if (nfc->composePair(a, b) != cp) continue;
    comps.emplace_back((static_cast<uint64_t>(a) << 21) | static_cast<uint64_t>(b), static_cast<uint32_t>(cp));
  }
  std::sort(comps.begin(), comps.end());

  std::printf("inline constexpr uint32_t kCccStart[] = {\n");
  for (auto& r : ccc_runs) std::printf("  0x%X,\n", r.first);
  std::printf("};\n\ninline constexpr uint32_t kCccValue[] = {\n");
  for (auto& r : ccc_runs) std::printf("  %u,\n", r.second);
  std::printf("};\n\ninline constexpr uint32_t kCccRuns = %zu;\n\n", ccc_runs.size());

  // kDecompCp is sorted; the sequence for kDecompCp[i] is
  // kDecompSeq[kDecompOff[i] .. kDecompOff[i + 1])
  std::vector<uint32_t> seq_flat, offs;
  for (auto& d : decomp) {
    offs.push_back(static_cast<uint32_t>(seq_flat.size()));
    seq_flat.insert(seq_flat.end(), d.second.begin(), d.second.end());
  }
  offs.push_back(static_cast<uint32_t>(seq_flat.size()));
  std::printf("inline constexpr uint32_t kDecompCp[] = {\n");
  for (auto& d : decomp) std::printf("  0x%X,\n", d.first);
  std::printf("};\n\ninline constexpr uint32_t kDecompOff[] = {\n");
  for (auto o : offs) std::printf("  %u,\n", o);
  std::printf("};\n\ninline constexpr uint32_t kDecompSeq[] = {\n");
  for (auto c : seq_flat) std::printf("  0x%X,\n", c);
  std::printf("};\n\ninline constexpr uint32_t kDecompCount = %zu;\n\n", decomp.size());

  // Primary composites keyed by (first << 21) | second, sorted
  std::printf("inline constexpr uint64_t kCompKey[] = {\n");
  for (auto& c : comps) std::printf("  0x%llXULL,\n", static_cast<unsigned long long>(c.first));
  std::printf("};\n\ninline constexpr uint32_t kCompValue[] = {\n");
  for (auto& c : comps) std::printf("  0x%X,\n", c.second);
  std::printf("};\n\ninline constexpr uint32_t kCompCount = %zu;\n\n", comps.size());

  // ---- UTS #46 mapping (nontransitional, STD3 rules off). The IDNA2008 rules
  // are applied after mapping, so ASCII outside LDH is still rejected there.
  // Entries: code points that are ignored, mapped, or disallowed; all others
  // are valid and not listed.
  UErrorCode uerr = U_ZERO_ERROR;
  icu::IDNA* idna = icu::IDNA::createUTS46Instance(UIDNA_NONTRANSITIONAL_TO_UNICODE, uerr);
  if (U_FAILURE(uerr)) { std::fprintf(stderr, "ICU UTS46: %s\n", u_errorName(uerr)); return 1; }
  enum : uint32_t { UTS_MAPPED = 1, UTS_IGNORED = 2, UTS_DISALLOWED = 3 };
  struct Uts { uint32_t cp, type; std::vector<uint32_t> seq; };
  std::vector<Uts> uts;
  for (UChar32 cp = 0; cp <= 0x10FFFF; ++cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) { uts.push_back({static_cast<uint32_t>(cp), UTS_DISALLOWED, {}}); continue; }
    // Wrap the code point in '!' (a starter that neither composes nor maps), so
    // that empty-label and leading-mark errors do not hide the result
    icu::UnicodeString src = icu::UnicodeString::fromUTF32(&cp, 1);
    icu::UnicodeString wrapped = icu::UnicodeString(u'!').append(src).append(u'!');
    icu::UnicodeString dst;
    icu::IDNAInfo info;
    uerr = U_ZERO_ERROR;
    idna->nameToUnicode(wrapped, dst, info, uerr);
    if (U_FAILURE(uerr) || info.hasErrors()) { uts.push_back({static_cast<uint32_t>(cp), UTS_DISALLOWED, {}}); continue; }
    if (dst == wrapped) continue;   // valid, unchanged
    bool framed = dst.length() >= 2 && dst.charAt(0) == u'!' && dst.charAt(dst.length() - 1) == u'!';
    if (!framed) continue;          // composed with the wrapper: a valid mark
    icu::UnicodeString mid = dst.tempSubString(1, dst.length() - 2);
    if (mid.isEmpty()) { uts.push_back({static_cast<uint32_t>(cp), UTS_IGNORED, {}}); continue; }
    Uts e{static_cast<uint32_t>(cp), UTS_MAPPED, {}};
    for (int32_t i = 0; i < mid.length();) {
      UChar32 c = mid.char32At(i);
      e.seq.push_back(static_cast<uint32_t>(c));
      i += (c >= 0x10000) ? 2 : 1;
    }
    uts.push_back(std::move(e));
  }
  delete idna;

  std::vector<uint32_t> useq, uoff;
  for (auto& e : uts) {
    uoff.push_back(static_cast<uint32_t>(useq.size()));
    useq.insert(useq.end(), e.seq.begin(), e.seq.end());
  }
  uoff.push_back(static_cast<uint32_t>(useq.size()));
  std::printf("inline constexpr uint32_t kUts46Cp[] = {\n");
  for (auto& e : uts) std::printf("  0x%X,\n", e.cp);
  std::printf("};\n\ninline constexpr uint32_t kUts46Type[] = {\n");
  for (auto& e : uts) std::printf("  %u,\n", e.type);
  std::printf("};\n\ninline constexpr uint32_t kUts46Off[] = {\n");
  for (auto o : uoff) std::printf("  %u,\n", o);
  std::printf("};\n\ninline constexpr uint32_t kUts46Seq[] = {\n");
  for (auto c : useq) std::printf("  0x%X,\n", c);
  std::printf("};\n\ninline constexpr uint32_t kUts46Count = %zu;\n\n", uts.size());

  std::printf("} // namespace glz::idna\n");
  std::fprintf(stderr, "ccc runs: %zu, decompositions: %zu, composition pairs: %zu, uts46 entries: %zu\n",
               ccc_runs.size(), decomp.size(), comps.size(), uts.size());
  std::fprintf(stderr, "runs: %zu\n", runs.size());
  return 0;
}
