// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validation engine - implementation
//
// This file contains method implementations for SchemaValidator class.
// It is included at the end of glazer_schema_validator.hpp within namespace glz.
// Do not include this directly.
//-----------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <regex>
#include <unordered_map>
#include <unordered_set>

std::pair<bool, ERL_NIF_TERM> SchemaValidator::validate(
  ERL_NIF_TERM   term,
  ValidationMode mode,
  size_t         max_depth,
  size_t         max_errors,
  int            timeout_ms
) {
  ValidationContext ctx;
  ctx.m_env        = m_env;
  ctx.m_mode       = mode;
  ctx.m_max_depth  = max_depth;
  ctx.m_max_errors = max_errors;
  ctx.m_timeout_ms = std::chrono::milliseconds(timeout_ms);
  ctx.m_start_time = std::chrono::steady_clock::now();

  if (!validate_value(term, 0, ctx)) {
    return {true, enif_make_atom(m_env, "ok")};
  }

  if (ctx.m_errors.empty())
    return {true, enif_make_atom(m_env, "ok")};

  if (mode == ValidationMode::FAIL_FAST)
    return {true, make_error_tuple(ctx.m_errors[0])};
  else
    return {true, make_error_list(ctx.m_errors)};
}

bool SchemaValidator::validate_value(
  ERL_NIF_TERM term,
  size_t bytecode_offset,
  ValidationContext& ctx
) {
  if (ctx.is_timeout_exceeded())
    return true;

  if (ctx.m_current_depth >= ctx.m_max_depth) {
    ValidationError err;
    err.reason = ErrorReason::TOO_FEW_ITEMS;
    ctx.add_error(err);
    return false;
  }

  const uint8_t* bytecode = m_schema.bytecode.data();
  size_t bytecode_len = m_schema.bytecode.size();

  while (bytecode_offset < bytecode_len) {
    SchemaOpcode opcode = static_cast<SchemaOpcode>(bytecode[bytecode_offset++]);

    if (opcode == SchemaOpcode::SCHEMA_END)
      break;

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
      case SchemaOpcode::CHECK_EXCLUSIVE_MINIMUM: {
        int64_t min_val = 0;
        uint64_t uval = 0;
        if (!read_varint(bytecode_offset, uval)) break;
        min_val = static_cast<int64_t>(uval);
        result = check_exclusive_minimum(term, min_val, ctx);
        break;
      }
      case SchemaOpcode::CHECK_EXCLUSIVE_MAXIMUM: {
        int64_t max_val = 0;
        uint64_t uval = 0;
        if (!read_varint(bytecode_offset, uval)) break;
        max_val = static_cast<int64_t>(uval);
        result = check_exclusive_maximum(term, max_val, ctx);
        break;
      }
      case SchemaOpcode::CHECK_MULTIPLE_OF: {
        int64_t factor = 0;
        uint64_t uval = 0;
        if (!read_varint(bytecode_offset, uval)) break;
        factor = static_cast<int64_t>(uval);
        result = check_multiple_of(term, factor, ctx);
        break;
      }
      case SchemaOpcode::CHECK_UNIQUE_ITEMS: {
        result = check_unique_items(term, ctx);
        break;
      }
      case SchemaOpcode::CHECK_ITEMS: {
        uint64_t items_schema_offset = 0;
        if (!read_varint(bytecode_offset, items_schema_offset)) break;
        result = check_items(term, static_cast<size_t>(items_schema_offset), ctx);
        break;
      }
      case SchemaOpcode::CHECK_REQUIRED: {
        uint64_t num_fields = 0;
        if (!read_varint(bytecode_offset, num_fields)) break;
        std::vector<std::string> required_fields;
        for (size_t i = 0; i < num_fields; ++i) {
          uint64_t field_len = 0;
          if (!read_varint(bytecode_offset, field_len)) break;
          std::string field(field_len, '\0');
          const uint8_t* bytecode = m_schema.bytecode.data();
          if (bytecode_offset + field_len > m_schema.bytecode.size()) break;
          std::memcpy(&field[0], bytecode + bytecode_offset, field_len);
          bytecode_offset += field_len;
          required_fields.push_back(field);
        }
        result = check_required(term, required_fields, ctx);
        break;
      }
      case SchemaOpcode::CHECK_PROPERTIES: {
        uint64_t props_schema_offset = 0;
        if (!read_varint(bytecode_offset, props_schema_offset)) break;
        result = check_properties(term, static_cast<size_t>(props_schema_offset), ctx);
        break;
      }
      case SchemaOpcode::CHECK_ADDITIONAL_PROPERTIES: {
        uint64_t ap_val = 0;
        if (!read_varint(bytecode_offset, ap_val)) break;
        ERL_NIF_TERM ap_schema = (ap_val == 0) ? AM_FALSE : AM_TRUE;
        result = check_additional_properties(term, ap_schema, ctx);
        break;
      }
      case SchemaOpcode::CHECK_NOT: {
        uint64_t not_schema_offset = 0;
        if (!read_varint(bytecode_offset, not_schema_offset)) break;
        result = check_not(term, static_cast<size_t>(not_schema_offset), ctx);
        break;
      }
      case SchemaOpcode::CHECK_FORMAT: {
        uint64_t format_len = 0;
        if (!read_varint(bytecode_offset, format_len)) break;
        std::string format(format_len, '\0');
        const uint8_t* bytecode = m_schema.bytecode.data();
        if (bytecode_offset + format_len > m_schema.bytecode.size()) break;
        std::memcpy(&format[0], bytecode + bytecode_offset, format_len);
        bytecode_offset += format_len;
        if (check_format_value(m_env, term, format)) {
          // Format check passed
          result = true;
        } else {
          ValidationError err;
          err.reason = ErrorReason::FORMAT_INVALID;
          err.actual_value = term;
          ctx.add_error(err);
          result = false;
        }
        break;
      }
      default:
        break;
    }

    if (!result && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;

    if (ctx.should_stop())
      return false;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_type(ERL_NIF_TERM term, uint8_t type_mask, ValidationContext& ctx) {
  JSONType term_type = term_to_type(m_env, term);
  uint8_t term_type_mask = static_cast<uint8_t>(term_type);

  if ((term_type_mask & type_mask) == 0) {
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
  if (!get_number(term, term_val))
    return true;

  if (term_val >= min_val)
    return true;

  ValidationError err;
  err.reason = ErrorReason::VALUE_TOO_SMALL;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_maximum(ERL_NIF_TERM term, int64_t max_val, ValidationContext& ctx) {
  int64_t term_val = 0;
  if (!get_number(term, term_val))
    return true;

  if (term_val <= max_val)
    return true;

  ValidationError err;
  err.reason = ErrorReason::VALUE_TOO_LARGE;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_exclusive_minimum(
  ERL_NIF_TERM term,
  int64_t min_val,
  ValidationContext& ctx
) {
  int64_t term_val = 0;
  if (!get_number(term, term_val))
    return true;

  if (term_val > min_val)
    return true;

  ValidationError err;
  err.reason = ErrorReason::VALUE_TOO_SMALL;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_exclusive_maximum(
  ERL_NIF_TERM term,
  int64_t max_val,
  ValidationContext& ctx
) {
  int64_t term_val = 0;
  if (!get_number(term, term_val))
    return true;

  if (term_val < max_val)
    return true;

  ValidationError err;
  err.reason = ErrorReason::VALUE_TOO_LARGE;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_multiple_of(ERL_NIF_TERM term, int64_t factor, ValidationContext& ctx) {
  int64_t term_val = 0;
  if (!get_number(term, term_val))
    return true;

  if (factor == 0 || term_val % factor == 0)
    return true;

  ValidationError err;
  err.reason = ErrorReason::NOT_MULTIPLE_OF;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_min_length(ERL_NIF_TERM term, size_t min_len, ValidationContext& ctx) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;

  size_t char_count = 0;
  const uint8_t* data = bin.data;
  const uint8_t* end = data + bin.size;
  while (data < end) {
    uint8_t byte = *data++;
    if ((byte & 0xC0) != 0x80)
      char_count++;
  }

  if (char_count >= min_len)
    return true;

  ValidationError err;
  err.reason = ErrorReason::STRING_TOO_SHORT;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_max_length(ERL_NIF_TERM term, size_t max_len, ValidationContext& ctx) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;

  size_t char_count = 0;
  const uint8_t* data = bin.data;
  const uint8_t* end = data + bin.size;
  while (data < end) {
    uint8_t byte = *data++;
    if ((byte & 0xC0) != 0x80)
      char_count++;
  }

  if (char_count <= max_len)
    return true;

  ValidationError err;
  err.reason = ErrorReason::STRING_TOO_LONG;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_pattern(
  ERL_NIF_TERM term,
  size_t pattern_idx,
  ValidationContext& ctx
) {
  if (pattern_idx >= m_schema.patterns.size())
    return true;

  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;

  const auto& pattern = m_schema.patterns[pattern_idx];
  std::string text(reinterpret_cast<const char*>(bin.data), bin.size);

  // Regex matching can be expensive — report work to scheduler
  update_reduction_count(m_env, bin.size);

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
  unsigned int list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) [[unlikely]]
    return true;

  if (list_len >= min_items)
    return true;

  ValidationError err;
  err.reason = ErrorReason::TOO_FEW_ITEMS;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_max_items(ERL_NIF_TERM term, size_t max_items, ValidationContext& ctx) {
  unsigned int list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) [[unlikely]]
    return true;

  if (list_len <= max_items)
    return true;

  ValidationError err;
  err.reason = ErrorReason::TOO_MANY_ITEMS;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_unique_items(ERL_NIF_TERM term, ValidationContext& ctx) {
  ERL_NIF_TERM head, tail = term;
  std::unordered_set<std::string> seen;
  size_t index = 0;

  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    // Use hex representation for term identity without conversion overhead
    char repr_buf[32];
    int len = snprintf(repr_buf, sizeof(repr_buf), "%p", reinterpret_cast<void*>(head));
    std::string repr(repr_buf, len);

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
  if (!enif_get_map_size(m_env, term, &map_size))
    return true;

  if (map_size >= min_props)
    return true;

  ValidationError err;
  err.reason = ErrorReason::TOO_FEW_PROPERTIES;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::check_max_properties(
  ERL_NIF_TERM term,
  size_t max_props,
  ValidationContext& ctx
) {
  size_t map_size = 0;
  if (!enif_get_map_size(m_env, term, &map_size))
    return true;

  if (map_size <= max_props)
    return true;

  ValidationError err;
  err.reason = ErrorReason::TOO_MANY_PROPERTIES;
  err.actual_value = term;
  ctx.add_error(err);
  return false;
}

bool SchemaValidator::get_number(ERL_NIF_TERM term, int64_t& out) {
  long long_val;
  if (enif_get_int64(m_env, term, &long_val)) {
    out = long_val;
    return true;
  }

  uint64_t ulong_val;
  if (enif_get_uint64(m_env, term, &ulong_val)) {
    out = static_cast<int64_t>(ulong_val);
    return true;
  }

  return false;
}

bool SchemaValidator::get_number_double(ERL_NIF_TERM term, double& out) {
  if (enif_get_double(m_env, term, &out))
    return true;

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
  ERL_NIF_TERM path_list = enif_make_list(m_env, 0);
  for (const auto& p : err.path)
    path_list = enif_make_list_cell(m_env, p, path_list);

  ERL_NIF_TERM reason_tuple = enif_make_tuple2(
    m_env,
    enif_make_atom(m_env, "reason"),
    err.reason_detail
  );

  return enif_make_tuple3(
    m_env,
    enif_make_atom(m_env, "error"),
    enif_make_tuple2(m_env, enif_make_atom(m_env, "path"), path_list),
    reason_tuple
  );
}

template<typename ErrorVec>
ERL_NIF_TERM SchemaValidator::make_error_list(const ErrorVec& errors) {
  ERL_NIF_TERM error_list = enif_make_list(m_env, 0);
  for (const auto& err : errors)
    error_list = enif_make_list_cell(m_env, make_error_tuple(err), error_list);

  return enif_make_tuple2(m_env, enif_make_atom(m_env, "error"), error_list);
}

bool SchemaValidator::read_varint(size_t& offset, uint64_t& out) {
  if (offset >= m_schema.bytecode.size())
    return false;

  const uint8_t* buf = m_schema.bytecode.data() + offset;
  size_t max_len = m_schema.bytecode.size() - offset;
  size_t bytes_read = decode_varint(buf, max_len, out);
  if (bytes_read == 0)
    return false;
  offset += bytes_read;
  return true;
}

bool SchemaValidator::check_items(
  ERL_NIF_TERM term,
  size_t items_schema_offset,
  ValidationContext& ctx
) {
  unsigned int list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0)
    return true;

  ERL_NIF_TERM head, tail = term;
  size_t index = 0;

  // Report array iteration work to scheduler
  update_reduction_count(m_env, list_len * 10);

  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    if (ctx.m_current_depth >= ctx.m_max_depth) {
      ValidationError err;
      err.reason = ErrorReason::TOO_FEW_ITEMS;
      ctx.add_error(err);
      return false;
    }

    ctx.m_errors.back().path.push_back(enif_make_ulong(m_env, index));
    ++ctx.m_current_depth;

    bool item_valid = validate_value(head, items_schema_offset, ctx);

    --ctx.m_current_depth;
    if (!ctx.m_errors.empty())
      ctx.m_errors.back().path.pop_back();

    if (!item_valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;

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
  return true;
}

bool SchemaValidator::check_required(
  ERL_NIF_TERM term,
  const std::vector<std::string>& required_fields,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  for (const auto& field : required_fields) {
    ERL_NIF_TERM key = enif_make_string(m_env, field.c_str(), ERL_NIF_LATIN1);
    ERL_NIF_TERM value;
    if (!enif_get_map_value(m_env, term, key, &value)) {
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
  return true;
}

bool SchemaValidator::check_not(
  ERL_NIF_TERM term,
  size_t not_schema_offset,
  ValidationContext& ctx
) {
  ValidationContext temp_ctx;
  temp_ctx.m_env = m_env;
  temp_ctx.m_mode = ValidationMode::FAIL_FAST;
  temp_ctx.m_max_depth = ctx.m_max_depth;
  temp_ctx.m_current_depth = ctx.m_current_depth;

  bool matches = validate_value(term, not_schema_offset, temp_ctx);

  if ((matches || temp_ctx.m_errors.empty()) && temp_ctx.m_errors.empty()) {
    ValidationError err;
    err.reason = ErrorReason::SCHEMA_MATCHED;
    err.actual_value = term;
    ctx.add_error(err);
    return false;
  }

  return true;
}

bool SchemaValidator::check_any_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  return true;
}

bool SchemaValidator::check_one_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  return true;
}

bool SchemaValidator::check_all_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  return true;
}

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
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, term, &bin)) {
    return true;
  }

  std::string text(reinterpret_cast<const char*>(bin.data), bin.size);

  auto it = FORMAT_PATTERNS.find(std::string(format));
  if (it == FORMAT_PATTERNS.end())
    return true;

  try {
    std::regex pattern(it->second, std::regex::optimize);
    return std::regex_match(text, pattern);
  } catch (const std::regex_error&) {
    return true;
  }
}