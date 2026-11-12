// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validation engine implementation
//-----------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <unordered_set>

#include "glazer_schema_validator.hpp"

namespace glz {

std::pair<bool, ERL_NIF_TERM> SchemaValidator::validate(
  ERL_NIF_TERM term,
  ValidationMode mode,
  size_t max_depth,
  size_t max_errors,
  int timeout_ms
) {
  ValidationContext ctx;
  ctx.m_env = m_env;
  ctx.m_mode = mode;
  ctx.m_max_depth = max_depth;
  ctx.m_max_errors = max_errors;
  ctx.m_timeout_ms = std::chrono::milliseconds(timeout_ms);
  ctx.m_start_time = std::chrono::steady_clock::now();

  if (!validate_value(term, 0, ctx)) {
    // Validation succeeded (no errors)
    return {true, enif_make_atom(m_env, "ok")};
  }

  // Build error result
  if (ctx.m_errors.empty()) {
    return {true, enif_make_atom(m_env, "ok")};
  }

  if (mode == ValidationMode::FAIL_FAST) {
    // Return single error as tuple
    return {true, make_error_tuple(ctx.m_errors[0])};
  } else {
    // Return all errors as list
    return {true, make_error_list(ctx.m_errors)};
  }
}

bool SchemaValidator::validate_value(
  ERL_NIF_TERM term,
  size_t bytecode_offset,
  ValidationContext& ctx
) {
  // Check timeout
  if (ctx.is_timeout_exceeded()) {
    return true;  // Stop validation but don't report error
  }

  // Check recursion depth
  if (ctx.m_current_depth >= ctx.m_max_depth) {
    ValidationError err;
    err.reason = ErrorReason::TOO_FEW_ITEMS;  // Reuse as depth error
    ctx.add_error(err);
    return false;
  }

  // Process bytecode stream starting at bytecode_offset
  const uint8_t* bytecode = m_schema.bytecode.data();
  size_t bytecode_len = m_schema.bytecode.size();

  while (bytecode_offset < bytecode_len) {
    SchemaOpcode opcode = static_cast<SchemaOpcode>(bytecode[bytecode_offset++]);

    if (opcode == SchemaOpcode::SCHEMA_END) {
      break;
    }

    bool result = true;
    switch (opcode) {
      case SchemaOpcode::CHECK_TYPE: {
        uint64_t type_mask = 0;
        if (!read_varint(bytecode_offset, type_mask)) break;
        result = check_type(term, static_cast<uint8_t>(type_mask), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MINIMUM: {
        int64_t min_val = 0;
        uint64_t uval = 0;
        if (!read_varint(bytecode_offset, uval)) break;
        min_val = static_cast<int64_t>(uval);
        result = check_minimum(term, min_val, ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAXIMUM: {
        int64_t max_val = 0;
        uint64_t uval = 0;
        if (!read_varint(bytecode_offset, uval)) break;
        max_val = static_cast<int64_t>(uval);
        result = check_maximum(term, max_val, ctx);
        break;
      }
      case SchemaOpcode::CHECK_MIN_LENGTH: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_min_length(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAX_LENGTH: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_max_length(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_PATTERN: {
        uint64_t pattern_idx = 0;
        if (!read_varint(bytecode_offset, pattern_idx)) break;
        result = check_pattern(term, static_cast<size_t>(pattern_idx), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MIN_ITEMS: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_min_items(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAX_ITEMS: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_max_items(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MIN_PROPERTIES: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_min_properties(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAX_PROPERTIES: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_max_properties(term, static_cast<size_t>(len), ctx);
        break;
      }
      default:
        // Unknown opcode, skip
        break;
    }

    if (!result && ctx.m_mode == ValidationMode::FAIL_FAST) {
      return false;
    }

    if (ctx.should_stop()) {
      return false;
    }
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_type(ERL_NIF_TERM term, uint8_t type_mask, ValidationContext& ctx) {
  JSONType term_type = term_to_type(m_env, term);
  uint8_t term_type_mask = static_cast<uint8_t>(term_type);

  if ((term_type_mask & type_mask) == 0) {
    // Type mismatch
    ValidationError err;
    err.path = ctx.m_errors.empty() ? std::vector<ERL_NIF_TERM>{} : ctx.m_errors.back().path;
    err.reason = ErrorReason::TYPE_MISMATCH;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_minimum(ERL_NIF_TERM term, int64_t min_val, ValidationContext& ctx) {
  int64_t term_val = 0;
  if (!get_number(term, term_val)) {
    return true;  // Not a number, skip constraint
  }

  if (term_val < min_val) {
    ValidationError err;
    err.reason = ErrorReason::VALUE_TOO_SMALL;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_maximum(ERL_NIF_TERM term, int64_t max_val, ValidationContext& ctx) {
  int64_t term_val = 0;
  if (!get_number(term, term_val)) {
    return true;
  }

  if (term_val > max_val) {
    ValidationError err;
    err.reason = ErrorReason::VALUE_TOO_LARGE;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_exclusive_minimum(
  ERL_NIF_TERM term,
  int64_t min_val,
  ValidationContext& ctx
) {
  int64_t term_val = 0;
  if (!get_number(term, term_val)) {
    return true;
  }

  if (term_val <= min_val) {
    ValidationError err;
    err.reason = ErrorReason::VALUE_TOO_SMALL;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_exclusive_maximum(
  ERL_NIF_TERM term,
  int64_t max_val,
  ValidationContext& ctx
) {
  int64_t term_val = 0;
  if (!get_number(term, term_val)) {
    return true;
  }

  if (term_val >= max_val) {
    ValidationError err;
    err.reason = ErrorReason::VALUE_TOO_LARGE;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_multiple_of(ERL_NIF_TERM term, int64_t factor, ValidationContext& ctx) {
  int64_t term_val = 0;
  if (!get_number(term, term_val)) {
    return true;
  }

  if (factor != 0 && term_val % factor != 0) {
    ValidationError err;
    err.reason = ErrorReason::NOT_MULTIPLE_OF;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_min_length(ERL_NIF_TERM term, size_t min_len, ValidationContext& ctx) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin)) {
    return true;  // Not a string, skip
  }

  // Count UTF-8 characters (not bytes)
  size_t char_count = 0;
  const uint8_t* data = bin.data;
  const uint8_t* end = data + bin.size;
  while (data < end) {
    uint8_t byte = *data++;
    if ((byte & 0xC0) != 0x80) {  // Not a continuation byte
      char_count++;
    }
  }

  if (char_count < min_len) {
    ValidationError err;
    err.reason = ErrorReason::STRING_TOO_SHORT;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_max_length(ERL_NIF_TERM term, size_t max_len, ValidationContext& ctx) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin)) {
    return true;
  }

  // Count UTF-8 characters
  size_t char_count = 0;
  const uint8_t* data = bin.data;
  const uint8_t* end = data + bin.size;
  while (data < end) {
    uint8_t byte = *data++;
    if ((byte & 0xC0) != 0x80) {
      char_count++;
    }
  }

  if (char_count > max_len) {
    ValidationError err;
    err.reason = ErrorReason::STRING_TOO_LONG;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_pattern(
  ERL_NIF_TERM term,
  size_t pattern_idx,
  ValidationContext& ctx
) {
  if (pattern_idx >= m_schema.patterns.size()) {
    return true;  // Invalid pattern index, skip
  }

  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin)) {
    return true;
  }

  const auto& pattern = m_schema.patterns[pattern_idx];
  std::string text(reinterpret_cast<const char*>(bin.data), bin.size);

  if (!std::regex_search(text, *pattern)) {
    ValidationError err;
    err.reason = ErrorReason::PATTERN_MISMATCH;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_min_items(ERL_NIF_TERM term, size_t min_items, ValidationContext& ctx) {
  size_t list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) {
    return true;  // Not a list, skip
  }

  if (list_len < min_items) {
    ValidationError err;
    err.reason = ErrorReason::TOO_FEW_ITEMS;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_max_items(ERL_NIF_TERM term, size_t max_items, ValidationContext& ctx) {
  size_t list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) {
    return true;
  }

  if (list_len > max_items) {
    ValidationError err;
    err.reason = ErrorReason::TOO_MANY_ITEMS;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_unique_items(ERL_NIF_TERM term, ValidationContext& ctx) {
  ERL_NIF_TERM head, tail = term;
  std::unordered_set<std::string> seen;
  size_t index = 0;

  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    // Create simple canonical representation for duplicate detection
    // For identical terms, pointer comparison works; for values, use term encoding
    std::string repr = std::to_string(reinterpret_cast<intptr_t>(head));

    if (seen.find(repr) != seen.end()) {
      ValidationError err;
      err.path.push_back(enif_make_ulong(m_env, index));
      err.reason = ErrorReason::DUPLICATE_ITEM;
      err.actual_value = head;
      ctx.add_error(err);
      return false;
    }
    seen.insert(repr);
    ++index;
  }
  return true;
}

bool SchemaValidator::check_min_properties(
  ERL_NIF_TERM term,
  size_t min_props,
  ValidationContext& ctx
) {
  size_t map_size = 0;
  if (!enif_get_map_size(m_env, term, &map_size)) {
    return true;  // Not a map, skip
  }

  if (map_size < min_props) {
    ValidationError err;
    err.reason = ErrorReason::TOO_FEW_PROPERTIES;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::check_max_properties(
  ERL_NIF_TERM term,
  size_t max_props,
  ValidationContext& ctx
) {
  size_t map_size = 0;
  if (!enif_get_map_size(m_env, term, &map_size)) {
    return true;
  }

  if (map_size > max_props) {
    ValidationError err;
    err.reason = ErrorReason::TOO_MANY_PROPERTIES;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }
  return true;
}

bool SchemaValidator::get_number(ERL_NIF_TERM term, int64_t& out) {
  long long_val;
  if (enif_get_int64(m_env, term, &long_val)) {
    out = long_val;
    return true;
  }

  // Try bigint
  if (enif_is_big_integer(m_env, term)) {
    // For now, assume it fits in int64_t
    unsigned long ulong_val;
    if (enif_get_uint64(m_env, term, reinterpret_cast<uint64_t*>(&ulong_val))) {
      out = static_cast<int64_t>(ulong_val);
      return true;
    }
  }

  return false;
}

bool SchemaValidator::get_number_double(ERL_NIF_TERM term, double& out) {
  if (enif_get_double(m_env, term, &out)) {
    return true;
  }

  long long_val;
  if (enif_get_int64(m_env, term, &long_val)) {
    out = static_cast<double>(long_val);
    return true;
  }

  return false;
}

bool SchemaValidator::get_string(ERL_NIF_TERM term, std::string& out) {
  ErlNifBinary bin;
  if (enif_inspect_binary(m_env, term, &bin)) {
    out = std::string(reinterpret_cast<const char*>(bin.data), bin.size);
    return true;
  }
  return false;
}

ERL_NIF_TERM SchemaValidator::make_error_tuple(const ValidationError& err) {
  // {error, {path, PathList, Reason}}
  ERL_NIF_TERM path_list = enif_make_list(m_env, 0);
  for (const auto& p : err.path) {
    path_list = enif_make_list_cell(m_env, p, path_list);
  }

  ERL_NIF_TERM reason_tuple = enif_make_tuple2(
    m_env,
    enif_make_atom(m_env, "reason"),  // Placeholder
    err.reason_detail
  );

  return enif_make_tuple3(
    m_env,
    enif_make_atom(m_env, "error"),
    enif_make_tuple2(m_env, enif_make_atom(m_env, "path"), path_list),
    reason_tuple
  );
}

ERL_NIF_TERM SchemaValidator::make_error_list(const std::vector<ValidationError>& errors) {
  ERL_NIF_TERM error_list = enif_make_list(m_env, 0);
  for (const auto& err : errors) {
    error_list = enif_make_list_cell(m_env, make_error_tuple(err), error_list);
  }
  return enif_make_tuple2(m_env, enif_make_atom(m_env, "error"), error_list);
}

bool SchemaValidator::read_varint(size_t& offset, uint64_t& out) {
  if (offset >= m_schema.bytecode.size()) {
    return false;
  }
  const uint8_t* buf = m_schema.bytecode.data() + offset;
  size_t max_len = m_schema.bytecode.size() - offset;
  size_t bytes_read = decode_varint(buf, max_len, out);
  if (bytes_read == 0) {
    return false;
  }
  offset += bytes_read;
  return true;
}

// Constraint check stub implementations (deferred to Tasks 6-7)

bool SchemaValidator::check_items(
  ERL_NIF_TERM term,
  size_t items_schema_offset,
  ValidationContext& ctx
) {
  // Task 6: Recursively validate each array element against items schema
  size_t list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) {
    return true;  // Not an array, skip
  }

  ERL_NIF_TERM head, tail = term;
  size_t index = 0;

  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    // Check depth limit
    if (ctx.m_current_depth >= ctx.m_max_depth) {
      ValidationError err;
      err.reason = ErrorReason::TOO_FEW_ITEMS;  // Reuse for depth error
      ctx.add_error(err);
      return false;
    }

    // Push index to error path
    ctx.m_errors.back().path.push_back(enif_make_ulong(m_env, index));
    ++ctx.m_current_depth;

    // Recursively validate element against items schema
    bool item_valid = validate_value(head, items_schema_offset, ctx);

    // Pop depth and path
    --ctx.m_current_depth;
    if (!ctx.m_errors.empty()) {
      ctx.m_errors.back().path.pop_back();
    }

    if (!item_valid && ctx.m_mode == ValidationMode::FAIL_FAST) {
      return false;
    }

    ++index;
    if (ctx.should_stop()) break;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_properties(
  ERL_NIF_TERM term,
  size_t props_schema_offset,
  ValidationContext& ctx
) {
  // TODO: Implement in Task 7
  // For now: if term is object, validate each property against its schema
  return true;
}

bool SchemaValidator::check_required(
  ERL_NIF_TERM term,
  const std::vector<std::string>& required_fields,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term)) {
    return true;  // Not an object, skip
  }

  for (const auto& field : required_fields) {
    ERL_NIF_TERM key = enif_make_string(m_env, field.c_str(), ERL_NIF_LATIN1);
    ERL_NIF_TERM value;
    if (!enif_get_map_value(m_env, term, key, &value)) {
      // Required field missing
      ValidationError err;
      err.reason = ErrorReason::MISSING_REQUIRED;
      err.reason_detail = enif_make_string(m_env, field.c_str(), ERL_NIF_LATIN1);
      err.actual_value = term;
      ctx.add_error(err);
      if (ctx.m_mode == ValidationMode::FAIL_FAST) {
        return false;
      }
    }
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_additional_properties(
  ERL_NIF_TERM term,
  ERL_NIF_TERM ap_schema,
  ValidationContext& ctx
) {
  // TODO: Implement in Task 7
  // For now: if ap_schema is false, reject any properties not in properties list
  return true;
}

bool SchemaValidator::check_not(
  ERL_NIF_TERM term,
  size_t not_schema_offset,
  ValidationContext& ctx
) {
  // Task 8: Negation — term must NOT match the schema
  // Create temporary context to test if schema matches
  ValidationContext temp_ctx;
  temp_ctx.m_env = m_env;
  temp_ctx.m_mode = ValidationMode::FAIL_FAST;  // Just check if matches at all
  temp_ctx.m_max_depth = ctx.m_max_depth;
  temp_ctx.m_current_depth = ctx.m_current_depth;

  // Validate against NOT schema
  bool matches = validate_value(term, not_schema_offset, temp_ctx);

  // If temp validation succeeded with no errors, the schema matched (negation failed)
  if ((matches || temp_ctx.m_errors.empty()) && temp_ctx.m_errors.empty()) {
    ValidationError err;
    err.reason = ErrorReason::SCHEMA_MATCHED;  // Negation failed
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }

  // If temp validation had errors, schema didn't match (negation passed)
  return true;
}

bool SchemaValidator::check_any_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  // TODO: Implement in Task 10
  // For now: must match at least one schema
  return true;
}

bool SchemaValidator::check_one_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  // TODO: Implement in Task 10
  // For now: must match exactly one schema
  return true;
}

bool SchemaValidator::check_all_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  // TODO: Implement in Task 10
  // For now: must match all schemas
  return true;
}

// Format validation patterns (Task 9)
static const std::unordered_map<std::string, std::string> FORMAT_PATTERNS = {
  {"email",     R"(^[^\s@]+@[^\s@]+\.[^\s@]+$)"},
  {"ipv4",      R"(^(\d{1,3}\.){3}\d{1,3}$)"},
  {"ipv6",      R"(^(([0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4})$)"},
  {"uri",       R"(^[a-zA-Z][a-zA-Z0-9+\-.]*:)"},
  {"uuid",      R"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)"},
  {"hostname",  R"(^([a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?\.)*[a-zA-Z]{2,}$)"},
  {"date",      R"(^\d{4}-\d{2}-\d{2}$)"},
  {"time",      R"(^\d{2}:\d{2}:\d{2}$)"},
  {"date-time", R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(Z|[+-]\d{2}:\d{2}))$)"},
};

bool check_format_value(ErlNifEnv* env, ERL_NIF_TERM term, std::string_view format) {
  // Get string from term
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, term, &bin)) {
    return true;  // Not a string, skip format check
  }

  std::string text(reinterpret_cast<const char*>(bin.data), bin.size);

  // Find format pattern
  auto it = FORMAT_PATTERNS.find(std::string(format));
  if (it == FORMAT_PATTERNS.end()) {
    // Unknown format, just pass (could be custom)
    return true;
  }

  // Validate against pattern
  try {
    std::regex pattern(it->second, std::regex::optimize);
    return std::regex_match(text, pattern);
  } catch (const std::regex_error&) {
    // Regex error, just pass
    return true;
  }
}

}  // namespace glz
