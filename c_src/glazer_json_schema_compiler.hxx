// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema compiler - implementation
//
// This file contains method implementations for SchemaCompiler class.
// It is included at the end of glazer_schema_compiler.hpp within namespace glz.
// Do not include this directly.
//-----------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <unordered_set>
#include <cstdint>
#include <cctype>
#include <regex>

#include "glazer_json_schema_compiler.hpp"

namespace glz {

std::pair<bool, CompiledSchema> SchemaCompiler::compile(
  ERL_NIF_TERM schema_term,
  std::string& error_msg
) {
  m_programs.clear();
  m_bytecode.clear();
  m_patterns.clear();
  m_numbers.clear();
  m_track_annotations = false;
  m_pattern_cache.clear();
  m_documents.clear();
  m_anchors.clear();
  m_ref_programs.clear();
  m_dynamic_anchors.clear();
  m_dynamic_names.clear();
  m_aliases.clear();
  m_skip_id = false;
  m_program_docs.clear();
  m_base.clear();
  m_total_constraints = 0;

  if (!is_schema_term(schema_term)) {
    error_msg = "schema must be a map";
    return {false, CompiledSchema{}};
  }

  m_root_term = schema_term;
  m_documents[""] = schema_term;
  ERL_NIF_TERM root_id;
  std::string root_doc;
  if (lookup_str(schema_term, "$id", root_id) && get_string(root_id, root_doc) &&
      !root_doc.empty() && root_doc[0] != '#') {
    root_doc = resolve_uri("", root_doc.substr(0, root_doc.find('#')));
    m_documents[root_doc] = schema_term;   // the root is also reachable by its $id
  }
  m_base = root_doc;
  for (const auto& [uri, doc] : m_external) {
    if (!is_schema_term(doc)) {
      error_msg = StrBuf{"document must be a schema object: ", uri}.str();
      return {false, CompiledSchema{}};
    }
    // A document served under a retrieval URL that declares its own $id is
    // known by that $id; the URL becomes an alias for it
    std::string canonical = uri;
    ERL_NIF_TERM own;
    std::string own_id;
    if (lookup_str(doc, "$id", own) && get_string(own, own_id) && !own_id.empty() && own_id[0] != '#')
      canonical = resolve_uri(uri, own_id.substr(0, own_id.find('#')));
    m_documents[canonical] = doc;
    m_documents[uri] = doc;
    if (canonical != uri)
      m_aliases[uri] = canonical;
  }

  // Index embedded resources and anchors in every document before compiling
  prescan(schema_term, "");
  for (const auto& [uri, doc] : m_external)
    prescan(doc, m_aliases.count(uri) ? m_aliases[uri] : uri);

  // Program 0 is the root; "#" names it
  m_ref_programs["#"] = 0;
  m_ref_programs[root_doc + "#"] = 0;
  new_program();
  m_skip_id = !root_doc.empty();   // the root's own $id is already in m_base
  if (!compile_into(0, schema_term, error_msg))
    return {false, CompiledSchema{}};

  // Dynamic anchors are compiled only when a $dynamicRef names them. Compiling
  // one can reference further names, so iterate to a fixed point.
  std::vector<std::pair<DynamicAnchorDef, size_t>> anchor_programs;
  std::unordered_set<std::string> compiled;
  bool progress = true;
  while (progress) {
    progress = false;
    auto defs = m_dynamic_anchors;   // compile_target does not add to this list
    for (const auto& def : defs) {
      std::string key = def.doc + "#" + def.name;
      if (compiled.count(key) || !m_dynamic_names.count(def.name))
        continue;
      size_t idx = 0;
      if (!compile_target(key, def.term, def.doc, idx, error_msg))
        return {false, CompiledSchema{}};
      compiled.insert(key);
      anchor_programs.emplace_back(def, idx);
      progress = true;
    }
  }

  // Lay the programs out back to back; offsets resolve program indices
  CompiledSchema result;
  result.program_offsets.reserve(m_programs.size());
  for (const auto& program : m_programs) {
    result.program_offsets.push_back(result.bytecode.size());
    result.bytecode.insert(result.bytecode.end(), program.begin(), program.end());
  }
  for (size_t i = 0; i < m_programs.size(); ++i)
    result.resource_of[result.program_offsets[i]] = m_program_docs[i];
  for (const auto& [def, idx] : anchor_programs)
    result.dynamic_anchors[def.doc][def.name] = result.program_offsets[idx];
  result.patterns = m_patterns;
  result.numbers = m_numbers;
  result.track_annotations = m_track_annotations;
  result.total_constraints = m_total_constraints;
  return {true, result};
}

size_t SchemaCompiler::new_program() {
  m_programs.emplace_back();
  m_program_docs.push_back(m_base);
  return m_programs.size() - 1;
}

bool SchemaCompiler::compile_program(
  ERL_NIF_TERM schema_term,
  size_t& index,
  std::string& error_msg
) {
  index = new_program();
  return compile_into(index, schema_term, error_msg);
}

bool SchemaCompiler::compile_into(size_t index, ERL_NIF_TERM schema_term, std::string& error_msg) {
  // A schema that declares its own $id is the root of that resource
  ERL_NIF_TERM id_term;
  std::string id;
  if (lookup_str(schema_term, "$id", id_term) && get_string(id_term, id) && !id.empty() && id[0] != '#')
    m_program_docs[index] = m_skip_id ? m_base : resolve_uri(m_base, id.substr(0, id.find('#')));

  // Emit into a fresh buffer so nested programs don't interleave with this one
  std::vector<uint8_t> saved = std::move(m_bytecode);
  m_bytecode.clear();

  bool ok = true;
  if (enif_is_identical(schema_term, AM_TRUE)) {
    // true accepts everything: an empty program
  } else if (enif_is_identical(schema_term, AM_FALSE)) {
    // false accepts nothing: an empty anyOf always fails
    emit_opcode(SchemaOpcode::CHECK_ANY_OF);
    emit_varint(0);
    ++m_total_constraints;
  } else {
    ok = compile_schema_term(schema_term, error_msg);
  }
  if (ok)
    emit_opcode(SchemaOpcode::SCHEMA_END);

  m_programs[index] = std::move(m_bytecode);
  m_bytecode = std::move(saved);
  return ok;
}

bool SchemaCompiler::compile_schema_term(ERL_NIF_TERM schema_term, std::string& error_msg) {
  // An embedded resource ($id) starts a new base document for its keywords
  struct BaseScope {
    std::string& base;
    std::string saved;
    BaseScope(std::string& b) : base(b), saved(b) {}
    ~BaseScope() { base = saved; }
  } base_scope(m_base);
  ERL_NIF_TERM id_term;
  std::string id;
  bool skip_id = m_skip_id;
  m_skip_id = false;
  if (lookup_str(schema_term, "$id", id_term) && get_string(id_term, id) && !id.empty() && id[0] != '#' && !skip_id)
    m_base = resolve_uri(m_base, id.substr(0, id.find('#')));

  // unevaluated* must see the annotations of every applicator, so they are
  // compiled after the rest of this schema's keywords
  ERL_NIF_TERM uneval_props = 0, uneval_items = 0;

  auto iter_opt = MapIterator::create(m_env, schema_term);
  if (!iter_opt) {
    error_msg = "failed to iterate schema map";
    return false;
  }

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string kw;
    if (!get_string(key, kw))
      continue;  // Non-string keys are not keywords

    // Dispatch on the first 8 bytes of the keyword (str_to_u64). A key does not
    // identify a name longer than 8 bytes, so each case also checks the rest:
    // str_cmp_tail8 requires a keyword of at most 8 bytes for a short name, and
    // the bytes after the first 8 for a longer one. Without it, an unknown
    // keyword such as "maxItemsX" would dispatch as "maxItems". The shared-prefix
    // cases compare the full name in turn.
    switch (str_to_u64(kw)) {
      case str_to_u64("type"):
        if (!str_cmp_tail8(kw, "type")) break;
        if (!compile_type(value, error_msg)) return false;
        break;
      case str_to_u64("minimum"):
        if (!str_cmp_tail8(kw, "minimum")) break;
        if (!compile_minimum(value, error_msg)) return false;
        break;
      case str_to_u64("maximum"):
        if (!str_cmp_tail8(kw, "maximum")) break;
        if (!compile_maximum(value, error_msg)) return false;
        break;
      case str_to_u64("exclusiveMinimum"):   // shares its first 8 bytes with the names below; compared in full
        if (str_cmp_tail8(kw, "exclusiveMinimum")) {
          if (!compile_exclusive_minimum(value, error_msg)) return false;
        } else if (str_cmp_tail8(kw, "exclusiveMaximum")) {
          if (!compile_exclusive_maximum(value, error_msg)) return false;
        }
        break;
      case str_to_u64("multipleOf"):
        if (!str_cmp_tail8(kw, "multipleOf")) break;
        if (!compile_multiple_of(value, error_msg)) return false;
        break;
      case str_to_u64("minLength"):
        if (!str_cmp_tail8(kw, "minLength")) break;
        compile_min_length(value);
        break;
      case str_to_u64("maxLength"):
        if (!str_cmp_tail8(kw, "maxLength")) break;
        compile_max_length(value);
        break;
      case str_to_u64("pattern"):
        if (!str_cmp_tail8(kw, "pattern")) break;
        if (!compile_pattern(value, error_msg)) return false;
        break;
      case str_to_u64("minItems"):
        if (!str_cmp_tail8(kw, "minItems")) break;
        compile_min_items(value);
        break;
      case str_to_u64("maxItems"):
        if (!str_cmp_tail8(kw, "maxItems")) break;
        compile_max_items(value);
        break;
      case str_to_u64("uniqueItems"):
        if (!str_cmp_tail8(kw, "uniqueItems")) break;
        compile_unique_items(value);
        break;
      case str_to_u64("items"):
        if (!str_cmp_tail8(kw, "items")) break;
        if (!compile_items(value, schema_term, error_msg)) return false;
        break;
      case str_to_u64("minProperties"):
        if (!str_cmp_tail8(kw, "minProperties")) break;
        compile_min_properties(value);
        break;
      case str_to_u64("maxProperties"):
        if (!str_cmp_tail8(kw, "maxProperties")) break;
        compile_max_properties(value);
        break;
      case str_to_u64("required"):
        if (!str_cmp_tail8(kw, "required")) break;
        if (!compile_required(value, error_msg)) return false;
        break;
      case str_to_u64("properties"):
        if (!str_cmp_tail8(kw, "properties")) break;
        if (!compile_properties(value, error_msg)) return false;
        break;
      case str_to_u64("additionalProperties"):
        if (!str_cmp_tail8(kw, "additionalProperties")) break;
        if (!compile_additional_properties(value, schema_term, error_msg)) return false;
        break;
      case str_to_u64("not"):
        if (!str_cmp_tail8(kw, "not")) break;
        if (!compile_not(value, error_msg)) return false;
        break;
      case str_to_u64("format"):
        if (!str_cmp_tail8(kw, "format")) break;
        if (!compile_format(value, error_msg)) return false;
        break;
      case str_to_u64("anyOf"):
        if (!str_cmp_tail8(kw, "anyOf")) break;
        if (!compile_schema_choice(SchemaOpcode::CHECK_ANY_OF, value, "anyOf", error_msg)) return false;
        break;
      case str_to_u64("oneOf"):
        if (!str_cmp_tail8(kw, "oneOf")) break;
        if (!compile_schema_choice(SchemaOpcode::CHECK_ONE_OF, value, "oneOf", error_msg)) return false;
        break;
      case str_to_u64("allOf"):
        if (!str_cmp_tail8(kw, "allOf")) break;
        if (!compile_schema_choice(SchemaOpcode::CHECK_ALL_OF, value, "allOf", error_msg)) return false;
        break;
      case str_to_u64("$ref"):
        if (!str_cmp_tail8(kw, "$ref")) break;
        if (!compile_ref(value, error_msg)) return false;
        break;
      case str_to_u64("if"): {
        if (!str_cmp_tail8(kw, "if")) break;
        // then/else only mean something alongside if; they are compiled here
        ERL_NIF_TERM then_term = 0, else_term = 0;
        lookup_keyword(schema_term, AM_THEN, "then", then_term);
        lookup_keyword(schema_term, AM_ELSE, "else", else_term);
        if (!compile_if_then_else(value, then_term, else_term, error_msg)) return false;
        break;
      }
      case str_to_u64("prefixItems"):
        if (!str_cmp_tail8(kw, "prefixItems")) break;
        if (!compile_prefix_items(value, error_msg)) return false;
        break;
      case str_to_u64("dependentRequired"):   // shares its first 8 bytes with the names below; compared in full
        if (str_cmp_tail8(kw, "dependentRequired")) {
          if (!compile_dependent_required(value, error_msg)) return false;
        } else if (str_cmp_tail8(kw, "dependentSchemas")) {
          if (!compile_dependent_schemas(value, error_msg)) return false;
        }
        break;
      case str_to_u64("const"):
        if (!str_cmp_tail8(kw, "const")) break;
        if (!compile_const(value, error_msg)) return false;
        break;
      case str_to_u64("enum"):
        if (!str_cmp_tail8(kw, "enum")) break;
        if (!compile_enum(value, error_msg)) return false;
        break;
      case str_to_u64("contains"):
        if (!str_cmp_tail8(kw, "contains")) break;
        if (!compile_contains(value, schema_term, error_msg)) return false;
        break;
      case str_to_u64("patternProperties"):
        if (!str_cmp_tail8(kw, "patternProperties")) break;
        if (!compile_pattern_properties(value, error_msg)) return false;
        break;
      case str_to_u64("propertyNames"):
        if (!str_cmp_tail8(kw, "propertyNames")) break;
        if (!compile_property_names(value, error_msg)) return false;
        break;
      case str_to_u64("$dynamicRef"):
        if (!str_cmp_tail8(kw, "$dynamicRef")) break;
        if (!compile_dynamic_ref(value, error_msg)) return false;
        break;
      case str_to_u64("unevaluatedProperties"):   // shares its first 8 bytes with the names below; compared in full
        if (str_cmp_tail8(kw, "unevaluatedProperties"))
          uneval_props = value;
        else if (str_cmp_tail8(kw, "unevaluatedItems"))
          uneval_items = value;
        break;
    }
    // Annotations (title, description, $schema, $defs, $dynamicAnchor, ...) carry no constraint
  }

  if (uneval_props != 0 && !compile_unevaluated(SchemaOpcode::CHECK_UNEVALUATED_PROPERTIES, uneval_props, error_msg))
    return false;
  if (uneval_items != 0 && !compile_unevaluated(SchemaOpcode::CHECK_UNEVALUATED_ITEMS, uneval_items, error_msg))
    return false;

  return true;
}

bool SchemaCompiler::type_mask_of(const std::string& type_str, uint8_t& type_mask) const {
  switch (str_to_u64(type_str)) {
    case str_to_u64("null"):
      type_mask = static_cast<uint8_t>(JSONType::NULL_VAL);
      return true;
    case str_to_u64("boolean"):
      type_mask = static_cast<uint8_t>(JSONType::BOOLEAN);
      return true;
    case str_to_u64("integer"):
      type_mask = static_cast<uint8_t>(JSONType::INTEGER);
      return true;
    case str_to_u64("number"):
      type_mask = static_cast<uint8_t>(JSONType::FLOAT) | static_cast<uint8_t>(JSONType::INTEGER);
      return true;
    case str_to_u64("string"):
      type_mask = static_cast<uint8_t>(JSONType::STRING);
      return true;
    case str_to_u64("array"):
      type_mask = static_cast<uint8_t>(JSONType::ARRAY);
      return true;
    case str_to_u64("object"):
      type_mask = static_cast<uint8_t>(JSONType::OBJECT);
      return true;
    default:
      break;
  }
  return false;
}

bool SchemaCompiler::compile_type(ERL_NIF_TERM type_term, std::string& error_msg) {
  uint8_t type_mask = 0;

  if (enif_is_list(m_env, type_term)) {
    // "type": ["string", "null"] accepts any of the listed types
    unsigned int n = 0;
    if (!enif_get_list_length(m_env, type_term, &n) || n == 0) {
      error_msg = "type array must not be empty";
      return false;
    }
    ERL_NIF_TERM head, tail = type_term;
    while (enif_get_list_cell(m_env, tail, &head, &tail)) {
      std::string name;
      uint8_t mask = 0;
      if (!get_string(head, name) || !type_mask_of(name, mask)) {
        error_msg = StrBuf{"unknown type: ", name}.str();
        return false;
      }
      type_mask |= mask;
    }
  } else {
    std::string type_str;
    if (!get_string(type_term, type_str)) {
      error_msg = "type must be a string or an array of strings";
      return false;
    }
    if (!type_mask_of(type_str, type_mask)) {
      error_msg = StrBuf{"unknown type: ", type_str}.str();
      return false;
    }
  }

  emit_opcode(SchemaOpcode::CHECK_TYPE);
  emit_varint(type_mask);
  ++m_total_constraints;
  return true;
}

// A numeric keyword value: an exact integer (any size) or a double
bool SchemaCompiler::get_number(ERL_NIF_TERM term, Number& out) {
  BigNum big;
  if (BigNum::from_term(m_env, term, big)) {
    out = make_integer(std::move(big));
    return true;
  }
  double d = 0;
  if (get_double(term, d)) {
    out = d;
    return true;
  }
  return false;
}

// Stores a number once and returns its index in the numbers table
size_t SchemaCompiler::intern_number(Number value) {
  m_numbers.push_back(std::move(value));
  return m_numbers.size() - 1;
}

bool SchemaCompiler::compile_number_keyword(SchemaOpcode op, ERL_NIF_TERM val_term,
                                            const char* keyword, std::string& error_msg) {
  Number value;
  if (!get_number(val_term, value)) {
    error_msg = std::string(keyword) + " must be a number";
    return false;
  }
  emit_opcode(op);
  emit_varint(intern_number(std::move(value)));
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_minimum(ERL_NIF_TERM val_term, std::string& error_msg) {
  return compile_number_keyword(SchemaOpcode::CHECK_MINIMUM, val_term, "minimum", error_msg);
}

bool SchemaCompiler::compile_maximum(ERL_NIF_TERM val_term, std::string& error_msg) {
  return compile_number_keyword(SchemaOpcode::CHECK_MAXIMUM, val_term, "maximum", error_msg);
}

bool SchemaCompiler::compile_exclusive_minimum(ERL_NIF_TERM val_term, std::string& error_msg) {
  return compile_number_keyword(SchemaOpcode::CHECK_EXCLUSIVE_MINIMUM, val_term, "exclusiveMinimum", error_msg);
}

bool SchemaCompiler::compile_exclusive_maximum(ERL_NIF_TERM val_term, std::string& error_msg) {
  return compile_number_keyword(SchemaOpcode::CHECK_EXCLUSIVE_MAXIMUM, val_term, "exclusiveMaximum", error_msg);
}

bool SchemaCompiler::compile_multiple_of(ERL_NIF_TERM val_term, std::string& error_msg) {
  return compile_number_keyword(SchemaOpcode::CHECK_MULTIPLE_OF, val_term, "multipleOf", error_msg);
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

bool SchemaCompiler::intern_regex(const std::string& pattern_str, size_t& pattern_idx, std::string& error_msg) {
  auto it = m_pattern_cache.find(pattern_str);
  if (it != m_pattern_cache.end()) {
    pattern_idx = it->second;
    return true;
  }
#if !defined(GLAZER_HAVE_PCRE2)
  // SRELL has no match limit, so a nested quantifier (e.g. "(a+)+") could run
  // for an unbounded time. Refuse it in this build; PCRE2 bounds the match.
  // Applies to schema patterns only: the built-in formats are fixed and bounded.
  if (has_nested_quantifier(pattern_str)) {
    error_msg = "invalid regex pattern: nested quantifier not allowed in this build (SRELL has no match limit)";
    return false;
  }
#endif
  try {
    auto regex = std::make_shared<SchemaRegex>(pattern_str);
    pattern_idx = m_patterns.size();
    m_patterns.push_back(regex);
    m_pattern_cache[pattern_str] = pattern_idx;
    return true;
  } catch (const SchemaRegexError& e) {
    error_msg = StrBuf{"invalid regex pattern: ", e.what()}.str();
    return false;
  }
}

bool SchemaCompiler::compile_pattern(ERL_NIF_TERM pattern_term, std::string& error_msg) {
  std::string pattern_str;
  if (!get_string(pattern_term, pattern_str)) {
    error_msg = "pattern must be a string";
    return false;
  }
  size_t idx = 0;
  if (!intern_regex(pattern_str, idx, error_msg))
    return false;
  emit_opcode(SchemaOpcode::CHECK_PATTERN);
  emit_varint(idx);
  ++m_total_constraints;
  return true;
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

bool SchemaCompiler::compile_items(
  ERL_NIF_TERM items_term,
  ERL_NIF_TERM schema_term,
  std::string& error_msg
) {
  if (!is_schema_term(items_term)) {
    error_msg = "items must be a schema object";
    return false;
  }

  // Positions covered by prefixItems are not checked by items
  size_t start = 0;
  ERL_NIF_TERM prefix;
  unsigned int prefix_len = 0;
  if (lookup_keyword(schema_term, AM_PREFIX_ITEMS, "prefixItems", prefix) &&
      enif_get_list_length(m_env, prefix, &prefix_len))
    start = prefix_len;

  size_t idx = 0;
  if (!compile_program(items_term, idx, error_msg)) return false;

  emit_opcode(SchemaOpcode::CHECK_ITEMS);
  emit_varint(idx);
  emit_varint(start);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_properties(ERL_NIF_TERM props_term, std::string& error_msg) {
  if (!enif_is_map(m_env, props_term)) {
    error_msg = "properties must be a schema object";
    return false;
  }

  // Compile every property schema first (nested programs), then emit the table
  std::vector<std::pair<std::string, size_t>> entries;
  auto iter_opt = MapIterator::create(m_env, props_term);
  if (!iter_opt) {
    error_msg = "failed to iterate properties map";
    return false;
  }

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string name;
    if (!get_string(key, name)) {
      error_msg = "property names must be strings or atoms";
      return false;
    }
    size_t idx = 0;
    if (!compile_program(value, idx, error_msg)) return false;
    entries.emplace_back(std::move(name), idx);
  }

  // Map iteration order is not something to depend on: validate properties in
  // name order so fail-fast reports the same error on every run.
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  emit_opcode(SchemaOpcode::CHECK_PROPERTIES);
  emit_varint(entries.size());
  for (const auto& [name, idx] : entries) {
    emit_string(name);
    emit_varint(idx);
  }
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_additional_properties(
  ERL_NIF_TERM ap_term,
  ERL_NIF_TERM schema_term,
  std::string& error_msg
) {
  // true imposes nothing; its annotation still marks every extra key evaluated
  bool allow_all = enif_is_identical(ap_term, AM_TRUE);

  bool is_schema = enif_is_map(m_env, ap_term);
  if (!allow_all && !is_schema && !enif_is_identical(ap_term, AM_FALSE)) {
    error_msg = "additionalProperties must be a boolean or schema object";
    return false;
  }

  // Declared property names come from the sibling `properties` keyword
  std::vector<std::string> names;
  ERL_NIF_TERM props;
  if (lookup_keyword(schema_term, AM_PROPERTIES, "properties", props) && enif_is_map(m_env, props)) {
    auto iter_opt = MapIterator::create(m_env, props);
    if (!iter_opt) {
      error_msg = "failed to iterate properties map";
      return false;
    }
    ERL_NIF_TERM key, value;
    for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
      std::string name;
      if (get_string(key, name)) names.push_back(std::move(name));
    }
  }

  // Keys matching a patternProperties pattern are not additional
  std::vector<size_t> pattern_idxs;
  ERL_NIF_TERM pattern_props;
  if (lookup_keyword(schema_term, AM_PATTERN_PROPERTIES, "patternProperties", pattern_props) && enif_is_map(m_env, pattern_props)) {
    auto iter_opt = MapIterator::create(m_env, pattern_props);
    if (!iter_opt) {
      error_msg = "failed to iterate patternProperties map";
      return false;
    }
    ERL_NIF_TERM key, value;
    for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
      std::string pattern;
      size_t idx = 0;
      if (!get_string(key, pattern) || !intern_regex(pattern, idx, error_msg)) {
        if (error_msg.empty()) error_msg = "patternProperties names must be strings";
        return false;
      }
      pattern_idxs.push_back(idx);
    }
  }

  size_t schema_idx = 0;
  if (is_schema && !compile_program(ap_term, schema_idx, error_msg)) return false;
  (void)allow_all;

  // Layout: kind (0 = forbidden, 1 = schema), names (sorted, for binary search),
  // patterns, [schema program]
  std::sort(names.begin(), names.end());
  emit_opcode(SchemaOpcode::CHECK_ADDITIONAL_PROPERTIES);
  emit_varint(allow_all ? 2 : (is_schema ? 1 : 0));
  emit_varint(names.size());
  for (const auto& name : names)
    emit_string(name);
  emit_varint(pattern_idxs.size());
  for (size_t idx : pattern_idxs)
    emit_varint(idx);
  if (is_schema)
    emit_varint(schema_idx);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_not(ERL_NIF_TERM not_term, std::string& error_msg) {
  if (!is_schema_term(not_term)) {
    error_msg = "not keyword value must be a schema object";
    return false;
  }

  size_t idx = 0;
  if (!compile_program(not_term, idx, error_msg)) return false;

  emit_opcode(SchemaOpcode::CHECK_NOT);
  emit_varint(idx);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_schema_list(ERL_NIF_TERM list_term, std::vector<size_t>& out, std::string& error_msg) {
  ERL_NIF_TERM head, tail = list_term;
  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    if (!is_schema_term(head)) {
      error_msg = "subschema must be a schema object";
      return false;
    }
    size_t idx = 0;
    if (!compile_program(head, idx, error_msg)) return false;
    out.push_back(idx);
  }
  return true;
}

bool SchemaCompiler::compile_schema_choice(
  SchemaOpcode op,
  ERL_NIF_TERM list_term,
  const char* keyword,
  std::string& error_msg
) {
  unsigned int len = 0;
  if (!enif_is_list(m_env, list_term) || !enif_get_list_length(m_env, list_term, &len) || len == 0) {
    error_msg = StrBuf{keyword, " must be a non-empty array of schemas"}.str();
    return false;
  }

  std::vector<size_t> progs;
  if (!compile_schema_list(list_term, progs, error_msg)) return false;

  emit_opcode(op);
  emit_varint(progs.size());
  for (size_t idx : progs)
    emit_varint(idx);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_any_of(ERL_NIF_TERM any_of_term, std::string& error_msg) {
  return compile_schema_choice(SchemaOpcode::CHECK_ANY_OF, any_of_term, "anyOf", error_msg);
}

bool SchemaCompiler::compile_one_of(ERL_NIF_TERM one_of_term, std::string& error_msg) {
  return compile_schema_choice(SchemaOpcode::CHECK_ONE_OF, one_of_term, "oneOf", error_msg);
}

bool SchemaCompiler::compile_all_of(ERL_NIF_TERM all_of_term, std::string& error_msg) {
  return compile_schema_choice(SchemaOpcode::CHECK_ALL_OF, all_of_term, "allOf", error_msg);
}

bool SchemaCompiler::lookup_str(ERL_NIF_TERM map, const std::string& name, ERL_NIF_TERM& out) {
  ERL_NIF_TERM bin_key;
  unsigned char* p = enif_make_new_binary(m_env, name.size(), &bin_key);
  if (!name.empty())
    std::memcpy(p, name.data(), name.size());
  if (enif_get_map_value(m_env, map, bin_key, &out))
    return true;

  // Atom keys: look up existing atoms only, so arbitrary names never create atoms
  ERL_NIF_TERM atom_key;
  if (enif_make_existing_atom_len(m_env, name.data(), name.size(), &atom_key, ERL_NIF_UTF8))
    return enif_get_map_value(m_env, map, atom_key, &out);
  return false;
}

std::string SchemaCompiler::percent_decode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

std::string SchemaCompiler::resolve_uri(const std::string& base, const std::string& ref) {
  // Already absolute (has a scheme)?
  // scheme ":" where scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )  (RFC 3986)
  size_t colon = ref.find(':');
  if (colon != std::string::npos && colon > 0 &&
      std::isalpha(static_cast<unsigned char>(ref[0]))) {
    bool scheme = true;
    for (size_t i = 1; i < colon && scheme; ++i) {
      unsigned char ch = static_cast<unsigned char>(ref[i]);
      scheme = std::isalnum(ch) || ch == '+' || ch == '-' || ch == '.';
    }
    if (scheme)
      return ref;
  }
  if (base.empty())
    return ref;
  if (ref.empty())
    return base.substr(0, base.find('#'));

  // Split base into scheme://authority, path and query (fragment dropped)
  std::string b = base.substr(0, base.find('#'));
  std::string prefix, path;
  size_t scheme_end = b.find("://");
  size_t path_start = std::string::npos;
  if (scheme_end != std::string::npos) {
    path_start = b.find('/', scheme_end + 3);
    prefix = path_start == std::string::npos ? b : b.substr(0, path_start);
    path = path_start == std::string::npos ? "" : b.substr(path_start);
  } else {
    path = b;
  }

  if (ref[0] == '#')
    return b + ref;
  if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/')
    return b.substr(0, b.find(':') + 1) + ref;
  if (ref[0] == '/')
    return prefix + ref;

  // Merge with the base directory, then drop the last path segment
  std::string dir = path.substr(0, path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
  if (dir.empty() && scheme_end != std::string::npos)
    dir = "/";
  std::string merged = dir + ref;

  // Remove dot segments
  std::vector<std::string> segments;
  size_t pos = 0;
  while (pos <= merged.size()) {
    size_t slash = merged.find('/', pos);
    if (slash == std::string::npos) slash = merged.size();
    std::string seg = merged.substr(pos, slash - pos);
    if (seg == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (seg != ".") {
      segments.push_back(seg);
    }
    pos = slash + 1;
  }
  std::string joined;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i) joined += '/';
    joined += segments[i];
  }
  if (!merged.empty() && merged[0] == '/' && (joined.empty() || joined[0] != '/'))
    joined = "/" + joined;
  return prefix + joined;
}

void SchemaCompiler::prescan(ERL_NIF_TERM node, const std::string& base) {
  // Only schema positions can declare identifiers; values under enum, const or
  // unknown keywords are data. Descend through the keywords that hold subschemas.
  static const std::unordered_set<std::string> single_schema = {
    "items", "additionalProperties", "unevaluatedItems", "unevaluatedProperties",
    "not", "if", "then", "else", "contains", "propertyNames"};
  static const std::unordered_set<std::string> schema_list = {"allOf", "anyOf", "oneOf", "prefixItems"};
  static const std::unordered_set<std::string> schema_map = {
    "properties", "patternProperties", "$defs", "definitions", "dependentSchemas"};

  if (enif_is_list(m_env, node)) {
    ERL_NIF_TERM head, tail = node;
    while (enif_get_list_cell(m_env, tail, &head, &tail))
      prescan(head, base);
    return;
  }
  if (!enif_is_map(m_env, node))
    return;

  std::string current = base;
  ERL_NIF_TERM value;
  std::string text;

  if (lookup_str(node, "$id", value) && get_string(value, text) && !text.empty() && text[0] != '#') {
    current = resolve_uri(base, text.substr(0, text.find('#')));
    m_documents.emplace(current, node);   // first definition wins
  }
  if (lookup_str(node, "$anchor", value) && get_string(value, text))
    m_anchors.emplace(current + "#" + text, node);
  if (lookup_str(node, "$dynamicAnchor", value) && get_string(value, text)) {
    m_anchors.emplace(current + "#" + text, node);
    m_dynamic_anchors.push_back(DynamicAnchorDef{text, current, node});
  }

  auto iter_opt = MapIterator::create(m_env, node);
  if (!iter_opt)
    return;
  ERL_NIF_TERM key, child;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &child); iter.next()) {
    std::string kw;
    if (!get_string(key, kw))
      continue;
    if (single_schema.count(kw) || schema_list.count(kw)) {
      prescan(child, current);
    } else if (schema_map.count(kw) && enif_is_map(m_env, child)) {
      auto entries = MapIterator::create(m_env, child);
      if (!entries) continue;
      ERL_NIF_TERM ekey, evalue;
      for (auto& e = entries.value(); e.get_pair(&ekey, &evalue); e.next())
        prescan(evalue, current);
    }
  }
}
bool SchemaCompiler::walk_pointer(ERL_NIF_TERM doc, const std::string& pointer, ERL_NIF_TERM& out) {
  out = doc;
  if (pointer.empty())
    return true;
  if (pointer[0] != '/')
    return false;

  size_t pos = 1;
  while (pos <= pointer.size()) {
    size_t slash = pointer.find('/', pos);
    if (slash == std::string::npos)
      slash = pointer.size();

    // Unescape ~1 -> '/' first, then ~0 -> '~' (RFC 6901 order)
    std::string token = pointer.substr(pos, slash - pos);
    std::string decoded;
    for (size_t i = 0; i < token.size(); ++i) {
      if (token[i] == '~' && i + 1 < token.size() && token[i + 1] == '1') {
        decoded += '/';
        ++i;
      } else if (token[i] == '~' && i + 1 < token.size() && token[i + 1] == '0') {
        decoded += '~';
        ++i;
      } else {
        decoded += token[i];
      }
    }

    if (enif_is_map(m_env, out)) {
      if (!lookup_str(out, decoded, out))
        return false;
    } else if (enif_is_list(m_env, out)) {
      if (decoded.empty() || decoded.find_first_not_of("0123456789") != std::string::npos)
        return false;
      size_t want = std::stoull(decoded);
      ERL_NIF_TERM head, tail = out;
      size_t i = 0;
      bool found = false;
      while (enif_get_list_cell(m_env, tail, &head, &tail)) {
        if (i++ == want) {
          out = head;
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    } else {
      return false;
    }

    pos = slash + 1;
  }
  return true;
}

bool SchemaCompiler::resolve_ref(
  const std::string& ref,
  std::string& doc,
  std::string& memo,
  ERL_NIF_TERM& target,
  std::string& error_msg
) {
  std::string full = resolve_uri(m_base, ref);
  size_t hash = full.find('#');
  doc = (hash == std::string::npos) ? full : full.substr(0, hash);
  auto alias = m_aliases.find(doc);
  if (alias != m_aliases.end())
    doc = alias->second;
  std::string frag = (hash == std::string::npos) ? "" : percent_decode(full.substr(hash + 1));

  auto document = m_documents.find(doc);
  if (document == m_documents.end()) {
    error_msg = StrBuf{"unresolved $ref (document not provided): ", ref}.str();
    return false;
  }

  memo = doc + "#" + frag;
  if (frag.empty() || frag[0] == '/') {
    if (!walk_pointer(document->second, frag, target)) {
      error_msg = StrBuf{"unresolved $ref: ", ref}.str();
      return false;
    }
    return true;
  }

  // Plain-name fragment: an anchor
  auto anchor = m_anchors.find(memo);
  if (anchor == m_anchors.end()) {
    error_msg = StrBuf{"unresolved $ref anchor: ", ref}.str();
    return false;
  }
  target = anchor->second;
  return true;
}
bool SchemaCompiler::compile_target(
  const std::string& memo,
  ERL_NIF_TERM term,
  const std::string& doc,
  size_t& index,
  std::string& error_msg
) {
  auto known = m_ref_programs.find(memo);
  if (known != m_ref_programs.end()) {
    index = known->second;
    return true;
  }
  if (enif_is_identical(term, m_root_term)) {
    m_ref_programs[memo] = 0;
    index = 0;
    return true;
  }
  if (!is_schema_term(term)) {
    error_msg = "$ref must point to a schema object";
    return false;
  }

  // Reserve before compiling so recursive references find this slot
  std::string saved = m_base;
  m_base = doc;
  index = new_program();
  m_ref_programs[memo] = index;
  ERL_NIF_TERM own;
  std::string own_id;
  m_skip_id = lookup_str(term, "$id", own) && get_string(own, own_id) && !own_id.empty() && own_id[0] != '#';
  bool ok = compile_into(index, term, error_msg);
  m_skip_id = false;
  m_base = saved;
  return ok;
}

bool SchemaCompiler::compile_ref(ERL_NIF_TERM ref_term, std::string& error_msg) {
  std::string ref;
  if (!get_string(ref_term, ref)) {
    error_msg = "$ref must be a string";
    return false;
  }

  std::string doc, memo;
  ERL_NIF_TERM target = 0;
  if (!resolve_ref(ref, doc, memo, target, error_msg))
    return false;

  size_t idx = 0;
  if (!compile_target(memo, target, doc, idx, error_msg))
    return false;

  emit_opcode(SchemaOpcode::CHECK_REF);
  emit_varint(idx);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_dynamic_ref(ERL_NIF_TERM ref_term, std::string& error_msg) {
  // (names are recorded below, once the reference is known to be dynamic)
  std::string ref;
  if (!get_string(ref_term, ref)) {
    error_msg = "$dynamicRef must be a string";
    return false;
  }

  // Static fallback: the same resolution as $ref
  std::string doc, memo;
  ERL_NIF_TERM target = 0;
  if (!resolve_ref(ref, doc, memo, target, error_msg))
    return false;
  size_t fallback = 0;
  if (!compile_target(memo, target, doc, fallback, error_msg))
    return false;

  size_t hash = ref.find('#');
  std::string frag = (hash == std::string::npos) ? "" : ref.substr(hash + 1);

  // Dynamic resolution applies only when the static target itself declares a
  // $dynamicAnchor of that name; otherwise the reference behaves like $ref
  ERL_NIF_TERM dyn_term;
  std::string dyn_name;
  bool target_is_dynamic = lookup_str(target, "$dynamicAnchor", dyn_term) &&
                           get_string(dyn_term, dyn_name) && dyn_name == percent_decode(frag);

  if (frag.empty() || frag[0] == '/' || !target_is_dynamic) {
    emit_opcode(SchemaOpcode::CHECK_REF);
    emit_varint(fallback);
  } else {
    emit_opcode(SchemaOpcode::CHECK_DYNAMIC_REF);
    emit_varint(fallback);
    emit_string(frag);
    m_dynamic_names.insert(percent_decode(frag));
  }
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_unevaluated(SchemaOpcode op, ERL_NIF_TERM value, std::string& error_msg) {
  m_track_annotations = true;
  // true marks everything evaluated (kind 2); false forbids unevaluated members;
  // a schema validates them
  if (enif_is_identical(value, AM_TRUE)) {
    emit_opcode(op);
    emit_varint(2);
    ++m_total_constraints;
    return true;
  }

  if (enif_is_identical(value, AM_FALSE)) {
    emit_opcode(op);
    emit_varint(0);
    ++m_total_constraints;
    return true;
  }

  if (!enif_is_map(m_env, value)) {
    error_msg = "unevaluated keyword must be a boolean or schema object";
    return false;
  }

  size_t idx = 0;
  if (!compile_program(value, idx, error_msg)) return false;
  emit_opcode(op);
  emit_varint(1);
  emit_varint(idx);
  ++m_total_constraints;
  return true;
}
bool SchemaCompiler::compile_if_then_else(
  ERL_NIF_TERM if_term,
  ERL_NIF_TERM then_term,
  ERL_NIF_TERM else_term,
  std::string& error_msg
) {
  if (!is_schema_term(if_term)) {
    error_msg = "if must be a schema object";
    return false;
  }

  size_t if_idx = 0;
  if (!compile_program(if_term, if_idx, error_msg)) return false;

  // Branch codes: 0 = absent, otherwise program index + 1
  size_t then_code = 0, else_code = 0;
  if (then_term != 0) {
    if (!is_schema_term(then_term)) {
      error_msg = "then must be a schema object";
      return false;
    }
    size_t idx = 0;
    if (!compile_program(then_term, idx, error_msg)) return false;
    then_code = idx + 1;
  }
  if (else_term != 0) {
    if (!is_schema_term(else_term)) {
      error_msg = "else must be a schema object";
      return false;
    }
    size_t idx = 0;
    if (!compile_program(else_term, idx, error_msg)) return false;
    else_code = idx + 1;
  }

  emit_opcode(SchemaOpcode::CHECK_IF_THEN_ELSE);
  emit_varint(if_idx);
  emit_varint(then_code);
  emit_varint(else_code);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_prefix_items(ERL_NIF_TERM list_term, std::string& error_msg) {
  if (!enif_is_list(m_env, list_term)) {
    error_msg = "prefixItems must be an array of schemas";
    return false;
  }

  std::vector<size_t> progs;
  if (!compile_schema_list(list_term, progs, error_msg)) return false;

  emit_opcode(SchemaOpcode::CHECK_PREFIX_ITEMS);
  emit_varint(progs.size());
  for (size_t idx : progs)
    emit_varint(idx);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_dependent_required(ERL_NIF_TERM map_term, std::string& error_msg) {
  if (!enif_is_map(m_env, map_term)) {
    error_msg = "dependentRequired must be an object";
    return false;
  }

  std::vector<std::pair<std::string, std::vector<std::string>>> deps;
  auto iter_opt = MapIterator::create(m_env, map_term);
  if (!iter_opt) {
    error_msg = "failed to iterate dependentRequired";
    return false;
  }

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string name;
    if (!get_string(key, name)) {
      error_msg = "dependentRequired names must be strings or atoms";
      return false;
    }
    if (!enif_is_list(m_env, value)) {
      error_msg = "dependentRequired values must be arrays";
      return false;
    }
    std::vector<std::string> required;
    ERL_NIF_TERM head, tail = value;
    while (enif_get_list_cell(m_env, tail, &head, &tail)) {
      std::string field;
      if (!get_string(head, field)) {
        error_msg = "dependentRequired entries must be strings or atoms";
        return false;
      }
      required.push_back(std::move(field));
    }
    deps.emplace_back(std::move(name), std::move(required));
  }

  emit_opcode(SchemaOpcode::CHECK_DEPENDENT_REQUIRED);
  emit_varint(deps.size());
  for (const auto& [name, required] : deps) {
    emit_string(name);
    emit_varint(required.size());
    for (const auto& field : required)
      emit_string(field);
  }
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_dependent_schemas(ERL_NIF_TERM map_term, std::string& error_msg) {
  if (!enif_is_map(m_env, map_term)) {
    error_msg = "dependentSchemas must be an object";
    return false;
  }

  std::vector<std::pair<std::string, size_t>> entries;
  auto iter_opt = MapIterator::create(m_env, map_term);
  if (!iter_opt) {
    error_msg = "failed to iterate dependentSchemas";
    return false;
  }

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string name;
    if (!get_string(key, name)) {
      error_msg = "dependentSchemas names must be strings or atoms";
      return false;
    }
    size_t idx = 0;
    if (!is_schema_term(value) || !compile_program(value, idx, error_msg)) {
      if (error_msg.empty()) error_msg = "dependentSchemas values must be schema objects";
      return false;
    }
    entries.emplace_back(std::move(name), idx);
  }

  emit_opcode(SchemaOpcode::CHECK_DEPENDENT_SCHEMAS);
  emit_varint(entries.size());
  for (const auto& [name, idx] : entries) {
    emit_string(name);
    emit_varint(idx);
  }
  ++m_total_constraints;
  return true;
}

void SchemaCompiler::push_varint(std::vector<uint8_t>& out, uint64_t value) {
  uint8_t buf[10];
  size_t len = encode_varint(value, buf);
  out.insert(out.end(), buf, buf + len);
}

bool SchemaCompiler::encode_json(ERL_NIF_TERM term, std::vector<uint8_t>& out, std::string& error_msg) {
  // Tags: T true, F false, N null, i int64, b bignum, d double, s string, a array, o object
  if (enif_is_identical(term, AM_TRUE)) { out.push_back('T'); return true; }
  if (enif_is_identical(term, AM_FALSE)) { out.push_back('F'); return true; }

  long lv = 0;
  if (enif_get_int64(m_env, term, &lv)) {
    int64_t v = static_cast<int64_t>(lv);
    out.push_back('i');
    uint8_t raw[sizeof(v)];
    std::memcpy(raw, &v, sizeof(v));
    out.insert(out.end(), raw, raw + sizeof(v));
    return true;
  }
  // Integers beyond int64: sign, then the magnitude as little-endian bytes
  BigNum big;
  if (BigNum::from_term(m_env, term, big)) {
    out.push_back('b');
    out.push_back(big.negative ? 1 : 0);
    std::vector<uint8_t> bytes;
    for (uint32_t limb : big.mag)
      for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<uint8_t>(limb >> (8 * k)));
    while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
    push_varint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
    return true;
  }

  double dv = 0;
  if (enif_get_double(m_env, term, &dv)) {
    out.push_back('d');
    uint8_t raw[sizeof(dv)];
    std::memcpy(raw, &dv, sizeof(dv));
    out.insert(out.end(), raw, raw + sizeof(dv));
    return true;
  }

  ErlNifBinary bin;
  if (enif_inspect_binary(m_env, term, &bin)) {
    out.push_back('s');
    push_varint(out, bin.size);
    out.insert(out.end(), bin.data, bin.data + bin.size);
    return true;
  }

  if (enif_is_list(m_env, term)) {
    unsigned int n = 0;
    enif_get_list_length(m_env, term, &n);
    out.push_back('a');
    push_varint(out, n);
    ERL_NIF_TERM head, tail = term;
    while (enif_get_list_cell(m_env, tail, &head, &tail)) {
      if (!encode_json(head, out, error_msg)) return false;
    }
    return true;
  }

  if (enif_is_map(m_env, term)) {
    size_t n = 0;
    enif_get_map_size(m_env, term, &n);
    out.push_back('o');
    push_varint(out, n);
    auto iter_opt = MapIterator::create(m_env, term);
    if (!iter_opt) {
      error_msg = "failed to iterate object value";
      return false;
    }
    ERL_NIF_TERM key, value;
    for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
      std::string name;
      if (!get_string(key, name)) {
        error_msg = "object keys must be strings or atoms";
        return false;
      }
      push_varint(out, name.size());
      out.insert(out.end(), name.begin(), name.end());
      if (!encode_json(value, out, error_msg)) return false;
    }
    return true;
  }

  if (enif_is_atom(m_env, term)) { out.push_back('N'); return true; }

  error_msg = "unsupported value in const/enum";
  return false;
}

bool SchemaCompiler::compile_const(ERL_NIF_TERM value, std::string& error_msg) {
  std::vector<uint8_t> encoded;
  if (!encode_json(value, encoded, error_msg)) return false;
  emit_opcode(SchemaOpcode::CHECK_CONST);
  emit_varint(encoded.size());
  emit_bytes(encoded.data(), encoded.size());
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_enum(ERL_NIF_TERM value, std::string& error_msg) {
  unsigned int n = 0;
  if (!enif_is_list(m_env, value) || !enif_get_list_length(m_env, value, &n)) {
    error_msg = "enum must be an array";
    return false;
  }
  emit_opcode(SchemaOpcode::CHECK_ENUM);
  emit_varint(n);
  ERL_NIF_TERM head, tail = value;
  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    std::vector<uint8_t> encoded;
    if (!encode_json(head, encoded, error_msg)) return false;
    emit_varint(encoded.size());
    emit_bytes(encoded.data(), encoded.size());
  }
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_contains(ERL_NIF_TERM value, ERL_NIF_TERM schema_term, std::string& error_msg) {
  if (!is_schema_term(value)) {
    error_msg = "contains must be a schema object";
    return false;
  }

  // minContains defaults to 1; maxContains is optional (encoded as max + 1, 0 = none)
  uint64_t min_count = 1, max_code = 0;
  ERL_NIF_TERM term;
  double dv = 0;
  if (lookup_keyword(schema_term, AM_MIN_CONTAINS, "minContains", term) && get_double(term, dv)) {
    if (dv < 0 || dv != std::trunc(dv)) { error_msg = "minContains must be a non-negative integer"; return false; }
    min_count = static_cast<uint64_t>(dv);
  }
  if (lookup_keyword(schema_term, AM_MAX_CONTAINS, "maxContains", term) && get_double(term, dv)) {
    if (dv < 0 || dv != std::trunc(dv)) { error_msg = "maxContains must be a non-negative integer"; return false; }
    max_code = static_cast<uint64_t>(dv) + 1;
  }

  size_t idx = 0;
  if (!compile_program(value, idx, error_msg)) return false;
  emit_opcode(SchemaOpcode::CHECK_CONTAINS);
  emit_varint(idx);
  emit_varint(min_count);
  emit_varint(max_code);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_pattern_properties(ERL_NIF_TERM map_term, std::string& error_msg) {
  if (!enif_is_map(m_env, map_term)) {
    error_msg = "patternProperties must be an object";
    return false;
  }

  std::vector<std::pair<size_t, size_t>> entries;   // pattern index, program index
  auto iter_opt = MapIterator::create(m_env, map_term);
  if (!iter_opt) {
    error_msg = "failed to iterate patternProperties";
    return false;
  }
  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string pattern;
    if (!get_string(key, pattern)) {
      error_msg = "patternProperties names must be strings";
      return false;
    }
    size_t pidx = 0, prog = 0;
    if (!intern_regex(pattern, pidx, error_msg)) return false;
    if (!is_schema_term(value) || !compile_program(value, prog, error_msg)) {
      if (error_msg.empty()) error_msg = "patternProperties values must be schema objects";
      return false;
    }
    entries.emplace_back(pidx, prog);
  }

  emit_opcode(SchemaOpcode::CHECK_PATTERN_PROPERTIES);
  emit_varint(entries.size());
  for (const auto& [pidx, prog] : entries) {
    emit_varint(pidx);
    emit_varint(prog);
  }
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_property_names(ERL_NIF_TERM value, std::string& error_msg) {
  if (!is_schema_term(value)) {
    error_msg = "propertyNames must be a schema object";
    return false;
  }
  size_t idx = 0;
  if (!compile_program(value, idx, error_msg)) return false;
  emit_opcode(SchemaOpcode::CHECK_PROPERTY_NAMES);
  emit_varint(idx);
  ++m_total_constraints;
  return true;
}

bool SchemaCompiler::compile_format(ERL_NIF_TERM format_term, std::string& error_msg) {
  // Asserted by default; format_assertion false makes it an annotation
  if (!m_format_assertion)
    return true;
  std::string format_str;
  if (!get_string(format_term, format_str)) {
    return true;
  }

  // A format named in the formats option is checked against its pattern
  auto custom = m_custom_formats.find(format_str);
  if (custom != m_custom_formats.end()) {
    size_t idx = 0;
    if (!intern_regex(custom->second, idx, error_msg))
      return false;
    emit_opcode(SchemaOpcode::CHECK_CUSTOM_FORMAT);
    emit_varint(idx);
    ++m_total_constraints;
    return true;
  }

  emit_opcode(SchemaOpcode::CHECK_FORMAT);
  emit_string(format_str);
  ++m_total_constraints;
  return true;
}

} // namespace glz