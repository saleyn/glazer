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
#include <unordered_set>
#include <vector>

#include <erl_nif.h>

namespace glz {

//-----------------------------------------------------------------------------
// Validation mode
//-----------------------------------------------------------------------------

enum class ValidationMode {
  FAIL_FAST = 0,    // Return immediately on first error
  COLLECT_ALL = 1   // Collect all errors
};

//-----------------------------------------------------------------------------
// Schema opcodes (bytecode instructions)
//-----------------------------------------------------------------------------

enum class SchemaOpcode : uint8_t {
  // Metadata
  SCHEMA_END = 0,

  // Type checking
  CHECK_TYPE = 1,

  // Numeric constraints
  CHECK_MINIMUM = 2,
  CHECK_MAXIMUM = 3,
  CHECK_EXCLUSIVE_MINIMUM = 4,
  CHECK_EXCLUSIVE_MAXIMUM = 5,
  CHECK_MULTIPLE_OF = 6,

  // String constraints
  CHECK_MIN_LENGTH = 7,
  CHECK_MAX_LENGTH = 8,
  CHECK_PATTERN = 9,

  // Array constraints
  CHECK_MIN_ITEMS = 10,
  CHECK_MAX_ITEMS = 11,
  CHECK_UNIQUE_ITEMS = 12,
  CHECK_ITEMS = 13,

  // Object constraints
  CHECK_MIN_PROPERTIES = 14,
  CHECK_MAX_PROPERTIES = 15,
  CHECK_REQUIRED = 16,
  CHECK_PROPERTIES = 17,
  CHECK_ADDITIONAL_PROPERTIES = 18,

  // Logical operators
  CHECK_NOT = 19,
  CHECK_ANY_OF = 20,
  CHECK_ONE_OF = 21,
  CHECK_ALL_OF = 22,

  // References
  CHECK_REF = 23,

  // Conditionals
  CHECK_IF_THEN_ELSE = 24,

  // Format validation
  CHECK_FORMAT = 25,

  // Advanced
  CHECK_PREFIX_ITEMS = 26,
  CHECK_DEPENDENT_REQUIRED = 27,
  CHECK_DEPENDENT_SCHEMAS = 28,
};

//-----------------------------------------------------------------------------
// Error reasons
//-----------------------------------------------------------------------------

enum class ErrorReason : uint8_t {
  TYPE_MISMATCH = 1,
  VALUE_TOO_SMALL = 2,
  VALUE_TOO_LARGE = 3,
  NOT_MULTIPLE_OF = 4,
  STRING_TOO_SHORT = 5,
  STRING_TOO_LONG = 6,
  PATTERN_MISMATCH = 7,
  MISSING_REQUIRED = 8,
  EXTRA_PROPERTIES = 9,
  TOO_FEW_ITEMS = 10,
  TOO_MANY_ITEMS = 11,
  DUPLICATE_ITEM = 12,
  TOO_FEW_PROPERTIES = 13,
  TOO_MANY_PROPERTIES = 14,
  SCHEMA_MATCHED = 15,  // negation failed
  NO_SCHEMA_MATCHED = 16,
  MULTIPLE_SCHEMAS_MATCHED = 17,
  ALL_SCHEMAS_FAILED = 18,
  ITEMS_VALIDATION_FAILED = 19,
  UNRESOLVED_REF = 20,
  CIRCULAR_REFERENCE = 21,
  THEN_VALIDATION_FAILED = 22,
  ELSE_VALIDATION_FAILED = 23,
  FORMAT_INVALID = 24,
  DEPENDENT_KEYS_MISSING = 25,
  DEPENDENT_SCHEMA_FAILED = 26,
};

//-----------------------------------------------------------------------------
// Compiled schema representation
//-----------------------------------------------------------------------------

struct CompiledSchema {
  std::vector<uint8_t> bytecode;
  std::vector<std::shared_ptr<std::regex>> patterns;
  std::vector<std::shared_ptr<CompiledSchema>> refs;  // Referenced schemas for $ref
  std::unordered_set<std::string> definitions;        // Definition names
  size_t total_constraints = 0;
  std::string definition_name;  // Empty if root schema
};

//-----------------------------------------------------------------------------
// Validation error
//-----------------------------------------------------------------------------

struct ValidationError {
  std::vector<ERL_NIF_TERM> path;  // [key1, 0, key2, ...] pointing to error location
  ErrorReason reason;
  ERL_NIF_TERM reason_detail;  // Additional detail (expected type, min value, etc.)
  ERL_NIF_TERM actual_value;   // The value that failed validation
};

//-----------------------------------------------------------------------------
// Validation context — shared state during recursive validation
//-----------------------------------------------------------------------------

struct ValidationContext {
  ErlNifEnv* m_env;
  ValidationMode m_mode = ValidationMode::FAIL_FAST;
  SmallVec<ValidationError, 16> m_errors;  // Zero heap allocation for typical error lists
  size_t m_max_errors = 1;
  size_t m_max_depth = 128;
  size_t m_current_depth = 0;
  std::chrono::steady_clock::time_point m_start_time;
  std::chrono::milliseconds m_timeout_ms{0};
  bool m_timed_out = false;

  // Check if timeout exceeded (check every N validations to avoid overhead)
  bool is_timeout_exceeded() {
    if (m_timeout_ms.count() == 0) return false;
    auto elapsed = std::chrono::steady_clock::now() - m_start_time;
    if (elapsed > m_timeout_ms) {
      m_timed_out = true;
      return true;
    }
    return false;
  }

  // Helper: push error to list
  void add_error(const ValidationError& err) {
    if (m_mode == ValidationMode::FAIL_FAST) {
      // Only store first error
      if (m_errors.empty()) {
        m_errors.push_back(err);
      }
    } else {
      // Collect all errors up to max_errors
      if (m_errors.size() < m_max_errors) {
        m_errors.push_back(err);
      }
    }
  }

  // Helper: have we hit max errors?
  bool should_stop() {
    if (m_mode == ValidationMode::FAIL_FAST && !m_errors.empty()) return true;
    if (m_mode == ValidationMode::COLLECT_ALL && m_errors.size() >= m_max_errors) return true;
    return false;
  }
};

//-----------------------------------------------------------------------------
// Type checking
//-----------------------------------------------------------------------------

// JSON type bitmask
enum class JSONType : uint8_t {
  NULL_VAL = 1 << 0,
  BOOLEAN  = 1 << 1,
  INTEGER  = 1 << 2,
  FLOAT    = 1 << 3,
  STRING   = 1 << 4,
  ARRAY    = 1 << 5,
  OBJECT   = 1 << 6,
};

inline JSONType term_to_type(ErlNifEnv* env, ERL_NIF_TERM term) {
  // null is represented as atom 'null' or 'nil'
  if (enif_is_atom(env, term))       return JSONType::NULL_VAL;
  if (enif_is_empty_list(env, term)) return JSONType::ARRAY;
  if (enif_is_list(env, term))       return JSONType::ARRAY;
  if (enif_is_map(env, term))        return JSONType::OBJECT;
  if (enif_is_binary(env, term))     return JSONType::STRING;

  // Check for boolean: true/false atoms
  if (enif_is_atom(env, term)) {
    // Could be true, false, null, or other atom
    char buf[16];
    if (enif_get_atom(env, term, buf, sizeof(buf), ERL_NIF_LATIN1)) {
      if (strcmp(buf, "true") == 0 || strcmp(buf, "false") == 0) {
        return JSONType::BOOLEAN;
      }
    }
    return JSONType::NULL_VAL;  // Other atoms treated as null
  }

  // Integer vs Float
  long long_val;
  if (enif_get_int64(env, term, &long_val)) return JSONType::INTEGER;

  uint64_t uval;
  if (enif_get_uint64(env, term, &uval)) return JSONType::INTEGER;

  double dbl_val;
  if (enif_get_double(env, term, &dbl_val)) return JSONType::FLOAT;

  return JSONType::NULL_VAL;
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
