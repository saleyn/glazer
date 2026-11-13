// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema compiler implementation
//-----------------------------------------------------------------------------

#include <algorithm>
#include <cstring>
#include <regex>
#include <sstream>

#include "glazer_schema_compiler.hpp"

namespace glz {

std::pair<bool, CompiledSchema> SchemaCompiler::compile(
  ERL_NIF_TERM schema_term,
  std::string& error_msg
) {
  m_bytecode.clear();
  m_patterns.clear();
  m_total_constraints = 0;

  if (!enif_is_map(m_env, schema_term)) {
    error_msg = "schema must be a map";
    return {false, CompiledSchema{}};
  }

  if (!compile_schema_term(schema_term, error_msg)) {
    return {false, CompiledSchema{}};
  }

  // Add END marker
  emit_opcode(SchemaOpcode::SCHEMA_END);

  CompiledSchema result;
  result.bytecode = m_bytecode;
  result.patterns = m_patterns;
  result.total_constraints = m_total_constraints;
  return {true, result};
}

bool SchemaCompiler::compile_schema_term(ERL_NIF_TERM schema_term, std::string& error_msg) {
  // Iterate over all keys in schema map
  ERL_NIF_TERM map_iter = enif_make_new_map(m_env);
  int map_size = 0;
  enif_get_map_size(m_env, schema_term, &map_size);

  ERL_NIF_TERM key, value;
  int idx = 0;
  while (enif_get_map_key(m_env, schema_term, idx++, &key, &value)) {
    std::string key_str;
    if (!get_string(key, key_str)) {
      continue;
    }

    // Hash-based keyword dispatch (first 8 bytes for speed)
    uint64_t keyword_hash = str_to_u64(key_str.c_str());
    switch (keyword_hash) {
      case str_to_u64("type"):
        if (!compile_type(value, error_msg)) return false;
        break;
      case str_to_u64("minimum"):
        compile_minimum(value);
        break;
      case str_to_u64("maximum"):
        compile_maximum(value);
        break;
      case str_to_u64("pattern"):
        compile_pattern(value, error_msg);
        break;
      case str_to_u64("minItems"):
        compile_min_items(value);
        break;
      case str_to_u64("maxItems"):
        compile_max_items(value);
        break;
      case str_to_u64("items\0\0\0\0"):  // "items" + 4 null bytes = 8 bytes
        if (!compile_items(value, error_msg)) return false;
        break;
      case str_to_u64("required"):
        if (!compile_required(value, error_msg)) return false;
        break;
      case str_to_u64("not\0\0\0\0\0"):  // "not" + 5 null bytes = 8 bytes
        if (!compile_not(value, error_msg)) return false;
        break;
      case str_to_u64("anyOf\0\0\0"):  // "anyOf" + 3 null bytes = 8 bytes
        if (!compile_any_of(value, error_msg)) return false;
        break;
      case str_to_u64("oneOf\0\0\0"):  // "oneOf" + 3 null bytes = 8 bytes
        if (!compile_one_of(value, error_msg)) return false;
        break;
      case str_to_u64("allOf\0\0\0"):  // "allOf" + 3 null bytes = 8 bytes
        if (!compile_all_of(value, error_msg)) return false;
        break;
      case str_to_u64("$ref\0\0\0\0"):  // "$ref" + 4 null bytes = 8 bytes
        if (!compile_ref(value, error_msg)) return false;
        break;
      case str_to_u64("format\0\0"):  // "format" + 2 null bytes = 8 bytes
        compile_format(value);
        break;
      // For longer keywords (>8 bytes), fall back to string comparison
      default:
        if (key_str == "exclusiveMinimum") {
          compile_exclusive_minimum(value);
        } else if (key_str == "exclusiveMaximum") {
          compile_exclusive_maximum(value);
        } else if (key_str == "multipleOf") {
          compile_multiple_of(value);
        } else if (key_str == "minLength") {
          compile_min_length(value);
        } else if (key_str == "maxLength") {
          compile_max_length(value);
        } else if (key_str == "uniqueItems") {
          compile_unique_items(value);
        } else if (key_str == "minProperties") {
          compile_min_properties(value);
        } else if (key_str == "maxProperties") {
          compile_max_properties(value);
        } else if (key_str == "properties") {
          if (!compile_properties(value, error_msg)) return false;
        } else if (key_str == "additionalProperties") {
          if (!compile_additional_properties(value, error_msg)) return false;
        }
        // Else: ignore metadata keywords (title, description, default, examples, $defs, etc.)
        break;
    }

  }

  return true;
}

// String literal hashing for fast type matching (< 8 bytes each)
inline constexpr uint64_t str_to_u64(const char* s) {
  uint64_t result = 0;
  for (int i = 0; s[i] && i < 8; ++i) {
    result = (result << 8) | static_cast<uint8_t>(s[i]);
  }
  return result;
}

bool SchemaCompiler::compile_type(ERL_NIF_TERM type_term, std::string& error_msg) {
  std::string type_str;
  if (!get_string(type_term, type_str)) {
    error_msg = "type must be a string";
    return false;
  }

  uint8_t type_mask = 0;

  if (type_str.length() <= 8) {
    auto type_hash = str_to_u64(type_str.c_str());

    switch (type_hash) {
      case str_to_u64("null"):
        type_mask = static_cast<uint8_t>(JSONType::NULL_VAL);
        break;
      case str_to_u64("boolean"):
        type_mask = static_cast<uint8_t>(JSONType::BOOLEAN);
        break;
      case str_to_u64("integer"):
        type_mask = static_cast<uint8_t>(JSONType::INTEGER);
        break;
      case str_to_u64("number"):
        type_mask = static_cast<uint8_t>(JSONType::FLOAT) | static_cast<uint8_t>(JSONType::INTEGER);
        break;
      case str_to_u64("string"):
        type_mask = static_cast<uint8_t>(JSONType::STRING);
        break;
      case str_to_u64("array"):
        type_mask = static_cast<uint8_t>(JSONType::ARRAY);
        break;
      case str_to_u64("object"):
        type_mask = static_cast<uint8_t>(JSONType::OBJECT);
        break;
      default:
        error_msg = StrBuf{"unknown type: ", type_str};  // Implicit conversion
        return false;
    }
  } else {
    error_msg = StrBuf{"invalid type string (too long): ", type_str};  // Implicit conversion
    return false;
  }

  emit_opcode(SchemaOpcode::CHECK_TYPE);
  emit_varint(type_mask);
  ++m_total_constraints;
  return true;
}

void SchemaCompiler::compile_minimum(ERL_NIF_TERM val_term) {
  int64_t min_val;
  if (get_int64(val_term, min_val)) {
    emit_opcode(SchemaOpcode::CHECK_MINIMUM);
    emit_varint(static_cast<uint64_t>(min_val));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_maximum(ERL_NIF_TERM val_term) {
  int64_t max_val;
  if (get_int64(val_term, max_val)) {
    emit_opcode(SchemaOpcode::CHECK_MAXIMUM);
    emit_varint(static_cast<uint64_t>(max_val));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_exclusive_minimum(ERL_NIF_TERM val_term) {
  int64_t min_val;
  if (get_int64(val_term, min_val)) {
    emit_opcode(SchemaOpcode::CHECK_EXCLUSIVE_MINIMUM);
    emit_varint(static_cast<uint64_t>(min_val));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_exclusive_maximum(ERL_NIF_TERM val_term) {
  int64_t max_val;
  if (get_int64(val_term, max_val)) {
    emit_opcode(SchemaOpcode::CHECK_EXCLUSIVE_MAXIMUM);
    emit_varint(static_cast<uint64_t>(max_val));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_multiple_of(ERL_NIF_TERM val_term) {
  int64_t factor;
  if (get_int64(val_term, factor) && factor > 0) {
    emit_opcode(SchemaOpcode::CHECK_MULTIPLE_OF);
    emit_varint(static_cast<uint64_t>(factor));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_min_length(ERL_NIF_TERM len_term) {
  int64_t len;
  if (get_int64(len_term, len) && len >= 0) {
    emit_opcode(SchemaOpcode::CHECK_MIN_LENGTH);
    emit_varint(static_cast<uint64_t>(len));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_max_length(ERL_NIF_TERM len_term) {
  int64_t len;
  if (get_int64(len_term, len) && len >= 0) {
    emit_opcode(SchemaOpcode::CHECK_MAX_LENGTH);
    emit_varint(static_cast<uint64_t>(len));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_pattern(ERL_NIF_TERM pattern_term, std::string& error_msg) {
  std::string pattern_str;
  if (!get_string(pattern_term, pattern_str)) {
    return;
  }

  try {
    auto regex = std::make_shared<std::regex>(pattern_str, std::regex::optimize);
    emit_opcode(SchemaOpcode::CHECK_PATTERN);
    emit_varint(m_patterns.size());
    m_patterns.push_back(regex);
    ++m_total_constraints;
  } catch (const std::regex_error& e) {
    error_msg = StrBuf{"invalid regex pattern: ", e.what()}.str();
  }
}

void SchemaCompiler::compile_min_items(ERL_NIF_TERM len_term) {
  int64_t len;
  if (get_int64(len_term, len) && len >= 0) {
    emit_opcode(SchemaOpcode::CHECK_MIN_ITEMS);
    emit_varint(static_cast<uint64_t>(len));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_max_items(ERL_NIF_TERM len_term) {
  int64_t len;
  if (get_int64(len_term, len) && len >= 0) {
    emit_opcode(SchemaOpcode::CHECK_MAX_ITEMS);
    emit_varint(static_cast<uint64_t>(len));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_unique_items(ERL_NIF_TERM unique_term) {
  // Check if unique_term is true
  char buf[8];
  if (enif_get_atom(m_env, unique_term, buf, sizeof(buf), ERL_NIF_LATIN1)) {
    if (strcmp(buf, "true") == 0) {
      emit_opcode(SchemaOpcode::CHECK_UNIQUE_ITEMS);
      ++m_total_constraints;
    }
  }
}

void SchemaCompiler::compile_min_properties(ERL_NIF_TERM len_term) {
  int64_t len;
  if (get_int64(len_term, len) && len >= 0) {
    emit_opcode(SchemaOpcode::CHECK_MIN_PROPERTIES);
    emit_varint(static_cast<uint64_t>(len));
    ++m_total_constraints;
  }
}

void SchemaCompiler::compile_max_properties(ERL_NIF_TERM len_term) {
  int64_t len;
  if (get_int64(len_term, len) && len >= 0) {
    emit_opcode(SchemaOpcode::CHECK_MAX_PROPERTIES);
    emit_varint(static_cast<uint64_t>(len));
    ++m_total_constraints;
  }
}

bool SchemaCompiler::compile_required(ERL_NIF_TERM required_term, std::string& error_msg) {
  // required_term should be a list
  if (!enif_is_list(m_env, required_term)) {
    error_msg = "required must be a list";
    return false;
  }

  std::vector<std::string> required_fields;
  ERL_NIF_TERM head, tail = required_term;
  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    std::string field_name;
    if (!get_string(head, field_name)) {
      error_msg = "required field name must be string";
      return false;
    }
    required_fields.push_back(field_name);
  }

  if (!required_fields.empty()) {
    emit_opcode(SchemaOpcode::CHECK_REQUIRED);
    emit_varint(required_fields.size());
    for (const auto& field : required_fields) {
      emit_string(field);
    }
    ++m_total_constraints;
  }

  return true;
}

bool SchemaCompiler::compile_items(ERL_NIF_TERM items_term, std::string& error_msg) {
  // TODO: Task 6 — Implement array items schema compilation
  // items_term can be:
  //   - schema: validate each array element against this schema
  //   - array of schemas: validate via prefixItems (Task 13)
  // For now: emit placeholder
  if (enif_is_map(m_env, items_term)) {
    emit_opcode(SchemaOpcode::CHECK_ITEMS);
    size_t schema_offset = m_bytecode.size();
    emit_varint(schema_offset);  // Placeholder: schema bytecode offset
    ++m_total_constraints;
  }
  return true;
}

bool SchemaCompiler::compile_properties(ERL_NIF_TERM props_term, std::string& error_msg) {
  // TODO: Task 7 — Implement object properties schema compilation
  // props_term is a map: key => schema
  // For each property, recursively compile its schema
  // For now: emit placeholder
  if (enif_is_map(m_env, props_term)) {
    emit_opcode(SchemaOpcode::CHECK_PROPERTIES);
    size_t schema_offset = m_bytecode.size();
    emit_varint(schema_offset);  // Placeholder: properties schemas offset
    ++m_total_constraints;
  }
  return true;
}

bool SchemaCompiler::compile_additional_properties(ERL_NIF_TERM ap_term, std::string& error_msg) {
  // TODO: Task 7 — Handle additionalProperties
  // ap_term can be:
  //   - true (default): allow any additional properties
  //   - false: reject any additional properties
  //   - schema: validate additional properties against schema
  // For now: emit placeholder only if false
  char buf[8];
  if (enif_get_atom(m_env, ap_term, buf, sizeof(buf), ERL_NIF_LATIN1)) {
    if (strcmp(buf, "false") == 0) {
      emit_opcode(SchemaOpcode::CHECK_ADDITIONAL_PROPERTIES);
      emit_varint(0);  // Placeholder: no additional properties allowed
      ++m_total_constraints;
    }
  }
  return true;
}

bool SchemaCompiler::compile_not(ERL_NIF_TERM not_term, std::string& error_msg) {
  // Task 8: NOT logic — recursively compile NOT schema
  // not_term is a schema; term must NOT match this schema
  if (!enif_is_map(m_env, not_term)) {
    error_msg = StrBuf{"not keyword value must be a schema object"};
    return false;
  }

  emit_opcode(SchemaOpcode::CHECK_NOT);
  size_t not_schema_offset = m_bytecode.size();
  emit_varint(not_schema_offset);  // Placeholder: will be filled after compilation

  // Recursively compile NOT schema
  if (!compile_schema_term(not_term, error_msg)) return false;

  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_any_of(ERL_NIF_TERM any_of_term, std::string& error_msg) {
  // TODO: Task 10 — Implement anyOf composition
  // any_of_term is a list of schemas
  // Term must match at least one schema (short-circuit on first match)
  // For now: skip
  return true;
}

bool SchemaCompiler::compile_one_of(ERL_NIF_TERM one_of_term, std::string& error_msg) {
  // TODO: Task 10 — Implement oneOf composition
  // one_of_term is a list of schemas
  // Term must match exactly one schema (validate all, count matches)
  // For now: skip
  return true;
}

bool SchemaCompiler::compile_all_of(ERL_NIF_TERM all_of_term, std::string& error_msg) {
  // TODO: Task 10 — Implement allOf composition
  // all_of_term is a list of schemas
  // Term must match all schemas (validate sequentially, accumulate errors)
  // For now: skip
  return true;
}

bool SchemaCompiler::compile_ref(ERL_NIF_TERM ref_term, std::string& error_msg) {
  // TODO: Task 11 — Implement schema references
  // ref_term is a string like "#/$defs/foo" or "other.json#/properties/bar"
  // Must resolve reference and compile referenced schema
  // Requires cycle detection
  // For now: skip
  return true;
}

bool SchemaCompiler::compile_if_then_else(
  ERL_NIF_TERM if_term,
  ERL_NIF_TERM then_term,
  ERL_NIF_TERM else_term,
  std::string& error_msg
) {
  // TODO: Task 12 — Implement conditional validation
  // if_term: condition schema
  // then_term: schema to validate if condition matches
  // else_term: schema to validate if condition doesn't match
  // For now: skip
  return true;
}

void SchemaCompiler::compile_format(ERL_NIF_TERM format_term) {
  // Task 9: Format validation — emit CHECK_FORMAT opcode with format string
  std::string format_str;
  if (!get_string(format_term, format_str)) {
    return;  // Invalid format term, skip
  }

  // Emit format validation opcode
  emit_opcode(SchemaOpcode::CHECK_FORMAT);
  emit_string(format_str);  // Emit format string (email, uri, date, etc.)
  ++m_total_constraints;
}

}  // namespace glz
