// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Regular expressions for schema `pattern`, `patternProperties` and `format`.
//
// Engine, chosen at build time (c_src/Makefile):
//   1. PCRE2, when libpcre2-8 is installed (GLAZER_HAVE_PCRE2).
//   2. Otherwise SRELL, the header-only engine fetched into vendors/srell.hpp.
//
// Both run in UTF-8 mode, so \p{...} works. \d, \w and \s stay ASCII, as in
// ECMA-262 without the `u` flag.
//
// Why not std::regex
// ------------------
// The schema suite needs Unicode property classes (\p{Letter}), which
// std::regex does not support at all. Where std::regex does run, it is much
// slower than the alternatives. Measured on this machine (one run per cell,
// ns per match, lower is better):
//
//   pattern                 input         std::regex       PCRE2       SRELL
//   literal "needle"        10,000 chars     378,149      27,783       7,767
//   "^[a-z]+$"              10,000 chars   1,284,495      16,190      64,880
//   SSN "^\d{3}-\d{2}-\d{4}$"  200 chars       5,977         153          21
//   email-like              10,000 chars   1,501,890      16,631          76
//   "(a|b)*c$"              10,000 chars     988,190     184,782      55,047
//   "a+b"                   10,000 chars      59,410      11,805       1,575
//   \p{L}+                  10,000 chars   unsupported    57,555     144,510
//
// std::regex also has no safeguard against deep recursion on long inputs, and
// it is the slowest engine on every pattern it can run. Compile cost is
// 4-160 us per pattern for all three, which is negligible because schemas are
// compiled once and validated many times.
//
// Why PCRE2 first, SRELL second
// -----------------------------
// PCRE2 is the fastest engine for most inputs here, is packaged by every
// major distribution, and has a match limit (pcre2_set_match_limit) that can
// bound catastrophic backtracking. SRELL is a header-only fallback so the
// build does not require a system library. It is faster on short inputs and
// on some long ones, but slower on long anchored classes and on \p{L} (see
// the table). SRELL's header exposes no match or step limit.
//
// Why not a native matcher
// ------------------------
// A hand-written Pike VM over code points was built and tested (differential
// fuzzing against PCRE2: zero mismatches on 240,000 comparisons). It was
// removed because it was 5-30x slower than PCRE2 on typical patterns and
// about 10x slower than SRELL on long inputs. Its only advantage was linear
// time, which the match limit gives PCRE2 without a second engine.
//
// Known limit: nested quantifiers such as "^(([a-z])+.)+[A-Z]([a-z])+$" are
// exponential for backtracking engines. PCRE2 stops at its match limit and
// reports no match; SRELL's header exposes no such limit.
//-----------------------------------------------------------------------------
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(GLAZER_HAVE_PCRE2)
#  ifndef PCRE2_CODE_UNIT_WIDTH
#    define PCRE2_CODE_UNIT_WIDTH 8
#  endif
#  include <pcre2.h>
#else
#  include "vendors/srell.hpp"
#endif

namespace glz {

struct SchemaRegexError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ECMA-262 accepts General_Category value names (\p{Letter}); the engines take
// the short names (\p{L}). Translate the long names; other text is untouched.
inline std::string ecma_to_short_properties(const std::string& pattern) {
  static const std::unordered_map<std::string, std::string> aliases = {
    {"Letter", "L"}, {"Cased_Letter", "LC"}, {"Uppercase_Letter", "Lu"},
    {"Lowercase_Letter", "Ll"}, {"Titlecase_Letter", "Lt"}, {"Modifier_Letter", "Lm"},
    {"Other_Letter", "Lo"}, {"Mark", "M"}, {"Combining_Mark", "M"},
    {"Nonspacing_Mark", "Mn"}, {"Spacing_Mark", "Mc"}, {"Enclosing_Mark", "Me"},
    {"Number", "N"}, {"Decimal_Number", "Nd"}, {"Digit", "Nd"}, {"Letter_Number", "Nl"},
    {"Other_Number", "No"}, {"Punctuation", "P"}, {"punct", "P"},
    {"Connector_Punctuation", "Pc"}, {"Dash_Punctuation", "Pd"},
    {"Open_Punctuation", "Ps"}, {"Close_Punctuation", "Pe"},
    {"Initial_Punctuation", "Pi"}, {"Final_Punctuation", "Pf"},
    {"Other_Punctuation", "Po"}, {"Symbol", "S"}, {"Math_Symbol", "Sm"},
    {"Currency_Symbol", "Sc"}, {"Modifier_Symbol", "Sk"}, {"Other_Symbol", "So"},
    {"Separator", "Z"}, {"Space_Separator", "Zs"}, {"Line_Separator", "Zl"},
    {"Paragraph_Separator", "Zp"}, {"Other", "C"}, {"Control", "Cc"}, {"cntrl", "Cc"},
    {"Format", "Cf"}, {"Surrogate", "Cs"}, {"Private_Use", "Co"}, {"Unassigned", "Cn"},
  };

  std::string out;
  out.reserve(pattern.size());
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '\\' && i + 1 < pattern.size()) {
      char kind = pattern[i + 1];
      if ((kind == 'p' || kind == 'P') && i + 2 < pattern.size() && pattern[i + 2] == '{') {
        size_t close = pattern.find('}', i + 3);
        if (close != std::string::npos) {
          std::string name = pattern.substr(i + 3, close - (i + 3));
          for (const char* prefix : {"General_Category=", "gc="}) {
            std::string p(prefix);
            if (name.compare(0, p.size(), p) == 0) name = name.substr(p.size());
          }
          auto alias = aliases.find(name);
          out += pattern.substr(i, 3);   // \p{ or \P{
          out += (alias != aliases.end()) ? alias->second : name;
          out += '}';
          i = close;
          continue;
        }
      }
      out += pattern[i];
      out += pattern[i + 1];
      ++i;
      continue;
    }
    out += pattern[i];
  }
  return out;
}

// True when some quantified group contains a quantifier, e.g. "(a+)+" or
// "((x|y)*z){2,}". A backtracking engine without a match limit can take
// exponential time on such patterns. Escapes and character classes are skipped.
inline bool has_nested_quantifier(std::string_view p) {
  std::vector<bool> open;          // one entry per open group: body contains a quantifier
  bool prev_closed_quantified = false;   // the previous token closed a group that had a quantifier
  bool prev_closed = false;              // the previous token was ')'

  auto saturating = [](std::string_view digits) {
    size_t v = 0;
    for (char d : digits) v = (v > 100000) ? v : v * 10 + static_cast<size_t>(d - '0');
    return v;
  };

  for (size_t i = 0; i < p.size(); ++i) {
    char c = p[i];
    if (c == '\\') {                       // escape: skip the next character
      ++i;
      prev_closed = false;
      continue;
    }
    if (c == '[') {                          // character class
      ++i;
      if (i < p.size() && p[i] == '^') ++i;
      while (i < p.size() && p[i] != ']') {
        if (p[i] == '\\') ++i;
        ++i;
      }
      prev_closed = false;
      continue;
    }
    if (c == '(') {
      open.push_back(false);
      if (p.substr(i + 1, 2) == "?:") i += 2;
      prev_closed = false;
      continue;
    }
    if (c == ')') {
      if (open.empty()) { prev_closed = false; continue; }
      bool quantified = open.back();
      open.pop_back();
      if (quantified && !open.empty()) open.back() = true;
      prev_closed_quantified = quantified;
      prev_closed = true;
      continue;
    }

    // Quantifier? multi: it can repeat more than once
    bool quant = false, multi = false;
    if (c == '*' || c == '+') { quant = true; multi = true; }
    else if (c == '?') { quant = true; }
    else if (c == '{') {
      size_t j = i + 1, d1 = j;
      while (j < p.size() && p[j] >= '0' && p[j] <= '9') ++j;
      if (j > d1) {
        size_t min = saturating(p.substr(d1, j - d1)), max = min;
        bool unbounded = false;
        if (j < p.size() && p[j] == ',') {
          ++j;
          size_t d2 = j;
          while (j < p.size() && p[j] >= '0' && p[j] <= '9') ++j;
          if (j > d2) max = saturating(p.substr(d2, j - d2));
          else unbounded = true;
        }
        if (j < p.size() && p[j] == '}') {
          quant = true;
          multi = unbounded || max > 1;
          i = j;
        }
      }
    }
    if (!quant) { prev_closed = false; continue; }

    if (prev_closed && prev_closed_quantified && multi) return true;
    for (size_t g = 0; g < open.size(); ++g) open[g] = true;
    if (i + 1 < p.size() && p[i + 1] == '?') ++i;   // lazy suffix
    prev_closed = false;
  }
  return false;
}

// The outcome of matching a pattern. An engine error is not a mismatch: it is
// reported with the engine's code (PCRE2's negative error code; -1 for SRELL,
// which reports exceptions only).
struct RegexMatch {
  enum class Status : uint8_t { NoMatch, Match, Error };
  Status status = Status::NoMatch;
  int    code   = 0;
};

class SchemaRegex {
public:
  enum class Mode {
    Search,   // matches anywhere in the text (schema `pattern` semantics)
    Full      // must match the whole text (format semantics)
  };

  // Throws SchemaRegexError when the pattern is invalid
  explicit SchemaRegex(const std::string& pattern, Mode mode = Mode::Search);
  ~SchemaRegex();

  SchemaRegex(const SchemaRegex&) = delete;
  SchemaRegex& operator=(const SchemaRegex&) = delete;

  RegexMatch match(std::string_view text) const;

  // "pcre2" or "srell"
  static const char* engine();

private:
#if defined(GLAZER_HAVE_PCRE2)
  pcre2_code_8* m_code = nullptr;
#else
  Mode m_mode;  // read by match(): SRELL distinguishes full from search matching
  srell::u8cregex m_regex;
#endif
};

#if defined(GLAZER_HAVE_PCRE2)

inline SchemaRegex::SchemaRegex(const std::string& pattern, Mode mode) {
  std::string translated = ecma_to_short_properties(pattern);
  std::string source = (mode == Mode::Full) ? "\\A(?:" + translated + ")\\z" : translated;
  int error = 0;
  PCRE2_SIZE offset = 0;
  m_code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()), source.size(),
                         PCRE2_UTF, &error, &offset, nullptr);
  if (m_code)   // JIT when the library has it; otherwise matching stays interpreted
    pcre2_jit_compile(m_code, PCRE2_JIT_COMPLETE);
  if (!m_code) {
    PCRE2_UCHAR message[256];
    pcre2_get_error_message(error, message, sizeof(message));
    throw SchemaRegexError(std::string(reinterpret_cast<const char*>(message)) +
                           " at offset " + std::to_string(offset));
  }
}

inline SchemaRegex::~SchemaRegex() {
  if (m_code)
    pcre2_code_free(m_code);
}

inline RegexMatch SchemaRegex::match(std::string_view text) const {
  // Only the match result is needed, so one per-thread block with a single
  // capture slot serves every pattern (a match with more captures than slots
  // still reports rc >= 0). Thread-local: compiled patterns are shared.
  struct Scratch {
    pcre2_match_data_8* data = pcre2_match_data_create(1, nullptr);
    ~Scratch() { pcre2_match_data_free(data); }
  };
  static thread_local Scratch scratch;

  // Invalid UTF-8 cannot match (PCRE2 reports it as an error below)
  static const char empty[1] = {0};
  const char* data = text.empty() ? empty : text.data();
  int rc = pcre2_match(m_code, reinterpret_cast<PCRE2_SPTR>(data), text.size(), 0, 0, scratch.data, nullptr);
  if (rc >= 0)
    return {RegexMatch::Status::Match, 0};
  if (rc == PCRE2_ERROR_NOMATCH)
    return {RegexMatch::Status::NoMatch, 0};
  return {RegexMatch::Status::Error, rc};   // e.g. PCRE2_ERROR_MATCHLIMIT, PCRE2_ERROR_JIT_STACKLIMIT
}

inline const char* SchemaRegex::engine() { return "pcre2"; }

#else

inline SchemaRegex::SchemaRegex(const std::string& pattern, Mode mode) : m_mode(mode) {
  try {
    m_regex = srell::u8cregex(ecma_to_short_properties(pattern), srell::regex_constants::ECMAScript);
  } catch (const std::exception& e) {
    throw SchemaRegexError(e.what());
  }
}

inline SchemaRegex::~SchemaRegex() = default;

inline RegexMatch SchemaRegex::match(std::string_view text) const {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  try {
    bool found = m_mode == Mode::Full ? srell::regex_match(first, last, m_regex)
                                      : srell::regex_search(first, last, m_regex);
    return {found ? RegexMatch::Status::Match : RegexMatch::Status::NoMatch, 0};
  } catch (const std::exception&) {
    return {RegexMatch::Status::Error, -1};
  }
}

inline const char* SchemaRegex::engine() { return "srell"; }

#endif

} // namespace glz
