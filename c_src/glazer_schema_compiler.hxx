// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema compiler - implementation
//
// This file contains method implementations for SchemaCompiler class.
// It is included at the end of glazer_schema_compiler.hpp within namespace glz.
// Do not include this directly.
//-----------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <regex>

std::pair<bool, CompiledSchema> SchemaCompiler::compile(
  ERL_NIF_TERM schema_term,
  std::string& error_msg
) {
  m_bytecode.clear();
  m_patterns.clear();
  m_pattern_cache.clear();  // Clear deduplication cache
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

// String literal hashing for fast keyword matching (< 8 bytes each)
inline constexpr uint64_t str_to_u64_local(const char* s) {
  uint64_t result = 0;
  for (int i = 0; s[i] && i < 8; ++i) {
    result = (result << 8) | static_cast<uint8_t>(s[i]);
  }
  return result;
}

// Runtime version for dynamic strings
inline uint64_t str_to_u64_runtime(const char* s) {
  uint64_t result = 0;
  for (int i = 0; s[i] && i < 8; ++i) {
    result = (result << 8) | static_cast<uint8_t>(s[i]);
  }
  return result;
}

bool SchemaCompiler::compile_schema_term(ERL_NIF_TERM schema_term, std::string& error_msg) {
  auto iter_opt = MapIterator::create(m_env, schema_term);
  if (!iter_opt) {
    error_msg = "failed to iterate schema map";
    return false;
  }

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string key_str;
    if (!get_string(key, key_str)) {
      iter.next();
      continue;
    }

    // Hash-based keyword dispatch (first 8 bytes for speed)
    uint64_t keyword_hash = str_to_u64_local(key_str.c_str());
    switch (keyword_hash) {
      case str_to_u64_local("type"):
        if (!compile_type(value, error_msg)) return false;
        break;
      case str_to_u64_local("minimum"):
        compile_minimum(value);
        break;
      case str_to_u64_local("maximum"):
        compile_maximum(value);
        break;
      case str_to_u64_local("pattern"):
        compile_pattern(value, error_msg);
        break;
      case str_to_u64_local("minItems"):
        compile_min_items(value);
        break;
      case str_to_u64_local("maxItems"):
        compile_max_items(value);
        break;
      case str_to_u64_local("items"):
        if (!compile_items(value, error_msg)) return false;
        break;
      case str_to_u64_local("required"):
        if (!compile_required(value, error_msg)) return false;
        break;
      case str_to_u64_local("not"):
        if (!compile_not(value, error_msg)) return false;
        break;
      case str_to_u64_local("anyOf"):
        if (!compile_any_of(value, error_msg)) return false;
        break;
      case str_to_u64_local("oneOf"):
        if (!compile_one_of(value, error_msg)) return false;
        break;
      case str_to_u64_local("allOf"):
        if (!compile_all_of(value, error_msg)) return false;
        break;
      case str_to_u64_local("$ref"):
        if (!compile_ref(value, error_msg)) return false;
        break;
      case str_to_u64_local("format"):
        compile_format(value);
        break;
      // For longer keywords (>8 bytes), hash first 8 bytes, then hash suffix
      case str_to_u64_local("exclusiv"):  // exclusiveMinimum/exclusiveMaximum (15 bytes)
        if (key_str.size() == 15) {
          uint64_t suffix_hash = str_to_u64_runtime(key_str.c_str() + 8);
          switch (suffix_hash) {
            case str_to_u64_local("eMinimum"):
              compile_exclusive_minimum(value);
              break;
            case str_to_u64_local("eMaximum"):
              compile_exclusive_maximum(value);
              break;
            default:
              return false;
          }
        }
        break;
      case str_to_u64_local("multiple"):  // multipleOf (10 bytes)
        if (key_str.size() == 10 && key_str[8] == 'O' && key_str[9] == 'f') {
          compile_multiple_of(value);
        }
        break;
      case str_to_u64_local("minLengt"):  // minLength (9 bytes)
        if (key_str.size() == 9 && key_str[8] == 'h')
          compile_min_length(value);
        else
          return false;
        break;
      case str_to_u64_local("maxLengt"):  // maxLength (9 bytes)
        if (key_str.size() == 9 && key_str[8] == 'h')
          compile_max_length(value);
        else
          return false;
        break;
      case str_to_u64_local("uniqueIt"):  // uniqueItems (11 bytes)
        if (key_str.size() == 11 && key_str[8] == 'e' && key_str[9] == 'm' && key_str[10] == 's')
          compile_unique_items(value);
        else
          return false;
        break;
      case str_to_u64_local("minPrope"):  // minProperties (13 bytes)
        if (key_str.size() == 13 && key_str.compare(8, 5, "rties") == 0)
          compile_min_properties(value);
        else
          return false;
        break;
      case str_to_u64_local("maxPrope"):  // maxProperties (13 bytes)
        if (key_str.size() == 13 && key_str.compare(8, 5, "rties") == 0)
          compile_max_properties(value);
        else
          return false;
        break;
      case str_to_u64_local("properti"):  // properties (10 bytes)
        if (key_str.size() == 10 && key_str[8] == 'e' && key_str[9] == 's') {
          if (!compile_properties(value, error_msg))
            return false;
        }
        else
          return false;
        break;
      case str_to_u64_local("additio"):  // additionalProperties (20 bytes)
        if (key_str.size() == 20 && key_str.compare(8, 12, "nalProperties") == 0) {
          if (!compile_additional_properties(value, error_msg)) return false;
        }
        break;
    default:
      // Ignore metadata keywords (title, description, default, examples, $defs, etc.)
      break;
    }
  }

  return true;
}

bool SchemaCompiler::compile_type(ERL_NIF_TERM type_term, std::string& error_msg) {
  std::string type_str;
  if (!get_string(type_term, type_str)) {
    error_msg = "type must be a string";
    return false;
  }

  uint8_t type_mask = 0;

  if (type_str.length() <= 8) {
    uint64_t type_hash = str_to_u64_runtime(type_str.c_str());

    switch (type_hash) {
      case str_to_u64_local("null"):
        type_mask = static_cast<uint8_t>(JSONType::NULL_VAL);
        break;
      case str_to_u64_local("boolean"):
        type_mask = static_cast<uint8_t>(JSONType::BOOLEAN);
        break;
      case str_to_u64_local("integer"):
        type_mask = static_cast<uint8_t>(JSONType::INTEGER);
        break;
      case str_to_u64_local("number"):
        type_mask = static_cast<uint8_t>(JSONType::FLOAT) | static_cast<uint8_t>(JSONType::INTEGER);
        break;
      case str_to_u64_local("string"):
        type_mask = static_cast<uint8_t>(JSONType::STRING);
        break;
      case str_to_u64_local("array"):
        type_mask = static_cast<uint8_t>(JSONType::ARRAY);
        break;
      case str_to_u64_local("object"):
        type_mask = static_cast<uint8_t>(JSONType::OBJECT);
        break;
      default:
        error_msg = StrBuf{"unknown type: ", type_str};
        return false;
    }
  } else {
    error_msg = StrBuf{"invalid type string (too long): ", type_str};
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

  // Check cache for duplicate pattern — O(1) lookup
  auto it = m_pattern_cache.find(pattern_str);
  if (it != m_pattern_cache.end()) {
    // Pattern already compiled, reuse existing index
    emit_opcode(SchemaOpcode::CHECK_PATTERN);
    emit_varint(it->second);
    ++m_total_constraints;
    return;
  }

  // New pattern — compile and cache
  try {
    auto regex = std::make_shared<std::regex>(pattern_str, std::regex::optimize);
    size_t pattern_idx = m_patterns.size();
    emit_opcode(SchemaOpcode::CHECK_PATTERN);
    emit_varint(pattern_idx);
    m_patterns.push_back(regex);
    m_pattern_cache[pattern_str] = pattern_idx;
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
  char buf[16];
  std::string_view atom_sv;
  if (atom_to_sv(m_env, unique_term, buf, sizeof(buf), atom_sv)) {
    if (atom_sv == "true") {
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
  if (enif_is_map(m_env, items_term)) {
    emit_opcode(SchemaOpcode::CHECK_ITEMS);
    size_t schema_offset = m_bytecode.size();
    emit_varint(schema_offset);
    ++m_total_constraints;
  }
  return true;
}

bool SchemaCompiler::compile_properties(ERL_NIF_TERM props_term, std::string& error_msg) {
  if (enif_is_map(m_env, props_term)) {
    emit_opcode(SchemaOpcode::CHECK_PROPERTIES);
    size_t schema_offset = m_bytecode.size();
    emit_varint(schema_offset);
    ++m_total_constraints;
  }
  return true;
}

bool SchemaCompiler::compile_additional_properties(ERL_NIF_TERM ap_term, std::string& error_msg) {
  char buf[16];
  std::string_view atom_sv;
  if (atom_to_sv(m_env, ap_term, buf, sizeof(buf), atom_sv)) {
    if (atom_sv == "false") {
      emit_opcode(SchemaOpcode::CHECK_ADDITIONAL_PROPERTIES);
      emit_varint(0);
      ++m_total_constraints;
    }
  }
  return true;
}

bool SchemaCompiler::compile_not(ERL_NIF_TERM not_term, std::string& error_msg) {
  if (!enif_is_map(m_env, not_term)) {
    error_msg = StrBuf{"not keyword value must be a schema object"};
    return false;
  }

  emit_opcode(SchemaOpcode::CHECK_NOT);
  size_t not_schema_offset = m_bytecode.size();
  emit_varint(not_schema_offset);

  if (!compile_schema_term(not_term, error_msg)) return false;

  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_any_of(ERL_NIF_TERM any_of_term, std::string& error_msg) {
  return true;
}

bool SchemaCompiler::compile_one_of(ERL_NIF_TERM one_of_term, std::string& error_msg) {
  return true;
}

bool SchemaCompiler::compile_all_of(ERL_NIF_TERM all_of_term, std::string& error_msg) {
  return true;
}

bool SchemaCompiler::compile_ref(ERL_NIF_TERM ref_term, std::string& error_msg) {
  return true;
}

bool SchemaCompiler::compile_if_then_else(
  ERL_NIF_TERM if_term,
  ERL_NIF_TERM then_term,
  ERL_NIF_TERM else_term,
  std::string& error_msg
) {
  return true;
}

void SchemaCompiler::compile_format(ERL_NIF_TERM format_term) {
  std::string format_str;
  if (!get_string(format_term, format_str)) {
    return;
  }

  emit_opcode(SchemaOpcode::CHECK_FORMAT);
  emit_string(format_str);
  ++m_total_constraints;
}