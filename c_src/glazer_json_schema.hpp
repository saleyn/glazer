// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validation engine — core data structures and interfaces.
//
// Supports both fail-fast and collect-all-errors validation modes.
// Schemas are compiled once to bytecode for efficient reuse.
//-----------------------------------------------------------------------------
#pragma once

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "glazer_bigint.hpp"
#include <vector>

#include <erl_nif.h>

#include "glazer_regex.hpp"

#include "glazer_atoms.hpp"

namespace glz {

//-----------------------------------------------------------------------------
// Validation mode
//-----------------------------------------------------------------------------

enum class ValidationMode {
  FAIL_FAST   = 0,  // Return immediately on first error
  COLLECT_ALL = 1   // Collect all errors
};

//-----------------------------------------------------------------------------
// Schema opcodes (bytecode instructions)
//-----------------------------------------------------------------------------

enum class SchemaOpcode : uint8_t {
  // Metadata
  SCHEMA_END                  = 0,

  // Type checking
  CHECK_TYPE                  = 1,

  // Numeric constraints
  CHECK_MINIMUM               = 2,
  CHECK_MAXIMUM               = 3,
  CHECK_EXCLUSIVE_MINIMUM     = 4,
  CHECK_EXCLUSIVE_MAXIMUM     = 5,
  CHECK_MULTIPLE_OF           = 6,

  // String constraints
  CHECK_MIN_LENGTH            = 7,
  CHECK_MAX_LENGTH            = 8,
  CHECK_PATTERN               = 9,

  // Array constraints
  CHECK_MIN_ITEMS             = 10,
  CHECK_MAX_ITEMS             = 11,
  CHECK_UNIQUE_ITEMS          = 12,
  CHECK_ITEMS                 = 13,

  // Object constraints
  CHECK_MIN_PROPERTIES        = 14,
  CHECK_MAX_PROPERTIES        = 15,
  CHECK_REQUIRED              = 16,
  CHECK_PROPERTIES            = 17,
  CHECK_ADDITIONAL_PROPERTIES = 18,

  // Logical operators
  CHECK_NOT                   = 19,
  CHECK_ANY_OF                = 20,
  CHECK_ONE_OF                = 21,
  CHECK_ALL_OF                = 22,

  // References
  CHECK_REF                   = 23,

  // Conditionals
  CHECK_IF_THEN_ELSE          = 24,

  // Format validation
  CHECK_FORMAT                = 25,

  // Advanced
  CHECK_PREFIX_ITEMS          = 26,
  CHECK_DEPENDENT_REQUIRED    = 27,
  CHECK_DEPENDENT_SCHEMAS     = 28,

  // Unevaluated keywords (run last, after the applicators they depend on)
  CHECK_UNEVALUATED_PROPERTIES = 29,
  CHECK_UNEVALUATED_ITEMS      = 30,

  // Dynamic references (resolved against the dynamic scope at validation time)
  CHECK_DYNAMIC_REF            = 31,

  // Values, arrays and property patterns
  CHECK_CONST                  = 32,
  CHECK_ENUM                   = 33,
  CHECK_CONTAINS               = 34,
  CHECK_PATTERN_PROPERTIES     = 35,
  CHECK_PROPERTY_NAMES         = 36,
  CHECK_CUSTOM_FORMAT          = 37,  // a format named by the formats option
};

//-----------------------------------------------------------------------------
// Error reasons
//-----------------------------------------------------------------------------

enum class ErrorReason : uint8_t {
  TYPE_MISMATCH               = 1,
  VALUE_TOO_SMALL             = 2,
  VALUE_TOO_LARGE             = 3,
  NOT_MULTIPLE_OF             = 4,
  STRING_TOO_SHORT            = 5,
  STRING_TOO_LONG             = 6,
  PATTERN_MISMATCH            = 7,
  MISSING_REQUIRED            = 8,
  EXTRA_PROPERTIES            = 9,
  TOO_FEW_ITEMS               = 10,
  TOO_MANY_ITEMS              = 11,
  DUPLICATE_ITEM              = 12,
  TOO_FEW_PROPERTIES          = 13,
  TOO_MANY_PROPERTIES         = 14,
  SCHEMA_MATCHED              = 15,  // negation failed
  NO_SCHEMA_MATCHED           = 16,
  MULTIPLE_SCHEMAS_MATCHED    = 17,
  ALL_SCHEMAS_FAILED          = 18,
  ITEMS_VALIDATION_FAILED     = 19,
  UNRESOLVED_REF              = 20,
  CIRCULAR_REFERENCE          = 21,
  THEN_VALIDATION_FAILED      = 22,
  ELSE_VALIDATION_FAILED      = 23,
  FORMAT_INVALID              = 24,
  DEPENDENT_KEYS_MISSING      = 25,
  DEPENDENT_SCHEMA_FAILED     = 26,
  EXPECTED_STRING             = 27,
  INVALID_TERM_TYPE           = 28,
  MAX_DEPTH_EXCEEDED          = 29,
  NOT_CONST                   = 30,
  NOT_IN_ENUM                 = 31,
  CONTAINS_FAILED             = 32,
  TIMEOUT                     = 33,  // validation stopped at timeout_ms
  REGEX_ERROR                 = 34,  // the regex engine failed (not a mismatch); detail is its code

  __COUNT__
};

std::string_view error_to_string(ErrorReason err) {
  static const std::string_view s_descriptions[] = {
    "",
    "type mismatch",
    "value too small",
    "value too large",
    "not multiple of",
    "string too short",
    "string too long",
    "pattern mismatch",
    "missing required",
    "extra properties",
    "too few items",
    "too many items",
    "duplicate item",
    "too few properties",
    "too many properties",
    "schema matched",
    "no schema matched",
    "multiple schemas matched",
    "all schemas failed",
    "items validation failed",
    "unresolved ref",
    "circular reference",
    "then validation failed",
    "else validation failed",
    "format invalid",
    "dependent keys missing",
    "dependent schema failed",
    "expected string",
    "invalid term type",
    "max depth exceeded",
    "not const",
    "not in enum",
    "contains failed",
    "timeout",
    "regex error",
  };
  return s_descriptions[uint8_t(err)];
}

//-----------------------------------------------------------------------------
// Compiled schema representation
//-----------------------------------------------------------------------------

// A numeric keyword's value: an integer that fits in int64, an exact integer
// of any size, or a double. Bounds and multipleOf factors are stored this way,
// so they compare exactly. An integer is always the int64 form when it fits.
using Number = std::variant<int64_t, BigNum, double>;

// The exact form of an integer: int64 when it fits, BigNum otherwise
inline Number make_integer(BigNum b) {
  int64_t small = 0;
  if (b.to_int64(small)) return small;
  return b;
}

// An integer Number as a BigNum (for comparisons with arbitrary integers)
inline BigNum number_as_big(const Number& n) {
  if (const auto* i = std::get_if<int64_t>(&n)) return BigNum::from_int64(*i);
  return std::get<BigNum>(n);
}

// The value as a double, for combining with floats
inline double number_approx(const Number& n) {
  if (const auto* i = std::get_if<int64_t>(&n)) return static_cast<double>(*i);
  if (const auto* b = std::get_if<BigNum>(&n))  return b->to_double();
  return std::get<double>(n);
}

inline ERL_NIF_TERM number_to_term(ErlNifEnv* env, const Number& n) {
  if (const auto* i = std::get_if<int64_t>(&n)) return enif_make_int64(env, *i);
  if (const auto* b = std::get_if<BigNum>(&n))  return b->to_term(env);
  return enif_make_double(env, std::get<double>(n));
}

struct CompiledSchema {
  using PatternsVec = std::vector<std::shared_ptr<SchemaRegex>>;
  using RefsVec     = std::vector<std::shared_ptr<CompiledSchema>>;
  using DefsSet     = std::unordered_set<std::string>;

  std::vector<uint8_t> bytecode;
  // Start offset in `bytecode` of each sub-program. Program 0 is the root
  // schema; nested schemas (items, properties, not, ...) are separate programs
  // referenced by index, so validators can jump to them without scanning.
  std::vector<size_t>  program_offsets;
  // Document (resource) each program belongs to, by program offset
  std::unordered_map<size_t, std::string> resource_of;
  // Document -> $dynamicAnchor name -> program offset of that anchored schema
  std::unordered_map<std::string, std::unordered_map<std::string, size_t>> dynamic_anchors;
  PatternsVec          patterns;
  std::vector<Number>  numbers;   // bounds and factors, referenced by index from the bytecode
  // True when some unevaluated* keyword needs the annotations of the walk
  // (which properties and items were evaluated). Otherwise annotation tracking
  // is skipped entirely.
  bool track_annotations = false;
  RefsVec              refs;                  // Referenced schemas for $ref
  DefsSet              definitions;           // Definition names
  size_t               total_constraints = 0;
  std::string          definition_name;       // Empty if root schema
};

//-----------------------------------------------------------------------------
// Validation error
//-----------------------------------------------------------------------------

struct ValidationError {
  using TermVec = std::vector<ERL_NIF_TERM>;

  TermVec      path;          // [key1, 0, key2, ...] pointing to error location
  ErrorReason  reason;
  ERL_NIF_TERM reason_detail; // Extra detail (expected type, min value, etc.)
  ERL_NIF_TERM actual_value;  // The value that failed validation
};

//-----------------------------------------------------------------------------
// Validation context — shared state during recursive validation
//-----------------------------------------------------------------------------

// Properties and array positions already evaluated for one instance. Filled by
// the keywords that apply to the instance, read by unevaluatedProperties/Items.
struct Evaluated {
  std::unordered_set<std::string> props;
  size_t items = 0;   // leading array positions evaluated; SIZE_MAX means all
  std::unordered_set<size_t> item_set;   // individual positions (e.g. matched by contains)
  bool all_props = false;                // an unevaluatedProperties: true marked every property
};

struct ValidationContext {
  using ErrorsVec    = SmallVec<ValidationError, 16>;
  using TimePoint    = std::chrono::steady_clock::time_point;
  using Milliseconds = std::chrono::milliseconds;

  ErlNifEnv*     m_env;
  ValidationMode m_mode          = ValidationMode::FAIL_FAST;
  ErrorsVec      m_errors;      // No heap allocation for typical error lists
  size_t         m_max_errors    = 1;
  size_t         m_max_depth     = 128;
  size_t         m_current_depth = 0;
  std::vector<ERL_NIF_TERM> m_path;  // Current location: keys/indices from the root
  TimePoint      m_start_time;
  Milliseconds   m_timeout_ms{0};
  bool           m_timed_out = false;

  // Annotations of the instance being validated (see Evaluated); null when
  // nothing downstream needs them
  struct Evaluated* m_eval = nullptr;

  // Resources entered so far, outermost first (program offsets), for $dynamicRef
  std::vector<size_t> m_dyn_scope;

  // Check if timeout exceeded (check every N validations to avoid overhead)
  bool is_timeout_exceeded() {
    if (m_timeout_ms.count() == 0)
      return false;

    auto elapsed = std::chrono::steady_clock::now() - m_start_time;
    if (elapsed <= m_timeout_ms)
      return false;

    m_timed_out = true;
    return true;
  }

  // Helper: push error to list
  // The error is stamped with the current traversal path.
  void add_error(ValidationError&& err) {
    // After the deadline, a sub-walk may have been cut short and "no errors"
    // or "no match" no longer means what it says; record nothing more.
    if (m_timed_out)
      return;
    err.path = m_path;
    if (m_mode == ValidationMode::FAIL_FAST) {
      // Only store first error
      if (m_errors.empty())
        m_errors.push_back(std::move(err));
    } else {
      // Collect all errors up to max_errors
      if (m_errors.size() < m_max_errors)
        m_errors.push_back(std::move(err));
    }
  }

  // Helper: have we hit max errors?
  bool should_stop() {
    return
      (m_mode == ValidationMode::FAIL_FAST   && !m_errors.empty()) ||
      (m_mode == ValidationMode::COLLECT_ALL && m_errors.size() >= m_max_errors);
  }

  // Helper: get last error path
  std::vector<ERL_NIF_TERM> get_last_path() const {
    return m_path;
  }
};

//-----------------------------------------------------------------------------
// Type checking
//-----------------------------------------------------------------------------

// JSON type bitmask
enum class JSONType : uint8_t {
  INVALID  = 0,

  NULL_VAL = 1 << 0,
  BOOLEAN  = 1 << 1,
  INTEGER  = 1 << 2,
  FLOAT    = 1 << 3,
  STRING   = 1 << 4,
  ARRAY    = 1 << 5,
  OBJECT   = 1 << 6,
};

inline JSONType term_to_type(ErlNifEnv* env, ERL_NIF_TERM term) {
  // Booleans are the atoms true/false; any other atom is treated as null
  // (the configured null atom, 'nil', or 'null').
  if (enif_is_atom(env, term)) {
    if (enif_is_identical(term, AM_TRUE) || enif_is_identical(term, AM_FALSE))
      return JSONType::BOOLEAN;
    return JSONType::NULL_VAL;
  }
  if (enif_is_empty_list(env, term)) return JSONType::ARRAY;
  if (enif_is_list(env, term))       return JSONType::ARRAY;
  if (enif_is_map(env, term))        return JSONType::OBJECT;
  if (enif_is_binary(env, term))     return JSONType::STRING;

  // Integer vs Float
  ErlNifSInt64 long_val;
  if (enif_get_int64(env, term, &long_val)) return JSONType::INTEGER;

  ErlNifUInt64 uval;
  if (enif_get_uint64(env, term, &uval)) return JSONType::INTEGER;

  // Integers of any size: a number that is not a float
  double any_double;
  if (enif_is_number(env, term) && !enif_get_double(env, term, &any_double))
    return JSONType::INTEGER;

  double dbl_val;
  if (enif_get_double(env, term, &dbl_val)) return JSONType::FLOAT;

  return JSONType::INVALID;  // not representable as JSON (tuple, pid, ref, ...)
}

//-----------------------------------------------------------------------------
// Efficient string buffer for error message building
//
// Allows efficient construction from multiple string_view arguments without
// intermediate allocations. Template parameter sets max size.
//
// Usage:
//   error_msg = StrBuf{"unknown type: ", type_str};
//   error_msg = StrBuf("invalid regex: ", pattern, " in schema");
//-----------------------------------------------------------------------------

template<size_t MaxSize = 256>
class StrBuf {
  std::array<char, MaxSize> m_buf{};
  size_t m_len = 0;

 public:
  // Initializer list constructor (preferred for readability)
  StrBuf(std::initializer_list<std::string_view> parts) {
    for (std::string_view part : parts) {
      append(part);
    }
  }

  // Variadic template constructor (alternative syntax)
  template<typename... Args>
  StrBuf(Args&&... args) {
    (append(args), ...);  // Fold expression: expand and append each arg
  }

  // Append a string_view
  void append(std::string_view sv) {
    size_t needed = m_len + sv.size();
    if (needed > MaxSize) {
      // Silently truncate to fit (better than throwing in low-level code)
      sv = sv.substr(0, MaxSize - m_len);
    }
    if (!sv.empty()) {
      std::memcpy(m_buf.data() + m_len, sv.data(), sv.size());
      m_len += sv.size();
    }
  }

  // Append a C string
  void append(const char* cstr) { if (cstr) append(std::string_view(cstr)); }
  // Append a std::string
  void append(const std::string& str) { append(std::string_view(str)); }

  // Implicit conversion to std::string (allows: std::string s = StrBuf{a, b, c})
  operator std::string() const { return std::string(m_buf.data(), m_len); }

  // Explicit conversion to std::string
  std::string str() const { return std::string(m_buf.data(), m_len); }

  std::string_view view() const { return std::string_view(m_buf.data(), m_len); }

  size_t size() const { return m_len; }

  bool empty() const { return m_len == 0; }

  const char* c_str() const {
    // Safe: m_buf is zero-initialized, so m_buf[m_len] = '\0' (or uninit if m_len == MaxSize)
    // For safety, ensure null termination when converting to C string
    const_cast<char*>(m_buf.data())[std::min(m_len, MaxSize - 1)] = '\0';
    return m_buf.data();
  }
};

//-----------------------------------------------------------------------------
// Varint encoding/decoding (LEB128-style)
//-----------------------------------------------------------------------------

inline size_t encode_varint(uint64_t v, uint8_t* buf) {
  size_t count = 0;
  while (v >= 128) {
    buf[count++] = (v & 0x7F) | 0x80;
    v >>= 7;
  }
  buf[count++] = v & 0x7F;
  return count;
}

inline size_t decode_varint(const uint8_t* buf, size_t max_len, uint64_t& out) {
  out = 0;
  size_t shift = 0;
  size_t count = 0;
  while (count < max_len) {
    uint8_t byte = buf[count++];
    out |= (uint64_t)(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      return count;
    }
    shift += 7;
  }
  return 0;  // Error: overflow
}

}  // namespace glz
