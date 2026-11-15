// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema compiler — converts schema AST to bytecode.
//
// Recursive descent over schema map/list, emits bytecode instructions
// and pre-compiles patterns.
//-----------------------------------------------------------------------------
#pragma once

#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

#include <erl_nif.h>

#include "glazer_atoms.hpp"
#include "glazer_json_schema.hpp"
#include "glazer_common.hpp"

namespace glz {

class SchemaCompiler {
public:
  SchemaCompiler(ErlNifEnv* env) : m_env(env) {}

  // Assert `format` (default true). Call before compile()
  void set_format_assertion(bool on) { m_format_assertion = on; }
  // A format name defined by the formats option: its value must match this pattern
  void set_custom_format(const std::string& name, const std::string& pattern) {
    m_custom_formats[name] = pattern;
  }

  // Register a document that $ref may name (uri -> schema). Call before compile().
  void add_document(const std::string& uri, ERL_NIF_TERM schema_term) {
    m_external.emplace_back(uri, schema_term);
  }

  // Compile a schema (map or list) to bytecode
  // Returns: {success, CompiledSchema} or {false, error_msg}
  std::pair<bool, CompiledSchema> compile(
    ERL_NIF_TERM schema_term,
    std::string& error_msg
  );

private:
  ErlNifEnv* m_env;
  std::vector<uint8_t> m_bytecode;                       // Program currently being emitted
  std::vector<std::vector<uint8_t>> m_programs;          // Programs, by index (0 = root)
  // Documents and anchors visible to $ref ("" is the root document)
  std::vector<std::pair<std::string, ERL_NIF_TERM>> m_external;
  std::unordered_map<std::string, ERL_NIF_TERM> m_documents;   // uri -> schema
  std::unordered_map<std::string, ERL_NIF_TERM> m_anchors;     // "uri#name" -> schema
  std::unordered_map<std::string, size_t> m_ref_programs;      // "uri#pointer" -> program
  bool m_format_assertion = true;    // `format` is asserted unless compile_schema sets it off
  std::string m_base;                                          // document being compiled
  std::vector<std::string> m_program_docs;                     // document of each program
  ERL_NIF_TERM m_root_term = 0;

  struct DynamicAnchorDef {
    std::string name;
    std::string doc;
    ERL_NIF_TERM term;
  };
  std::vector<DynamicAnchorDef> m_dynamic_anchors;
  std::unordered_set<std::string> m_dynamic_names;      // names used by compiled $dynamicRef
  std::unordered_map<std::string, std::string> m_aliases; // retrieval URL -> canonical ($id) URI
  bool m_skip_id = false;   // the next schema's $id is already resolved into m_base
  std::vector<std::shared_ptr<SchemaRegex>> m_patterns;
  std::vector<Number> m_numbers;
  bool m_track_annotations = false;   // an unevaluated* keyword was compiled   // numeric keyword values (see compile_number_keyword)
  std::unordered_map<std::string, size_t> m_pattern_cache;  // O(1) deduplication
  std::unordered_map<std::string, std::string> m_custom_formats;  // formats option
  size_t m_total_constraints = 0;

  // Get string value from Erlang binary or atom
  bool get_string(ERL_NIF_TERM term, std::string& out) {
    ErlNifBinary bin;
    if (enif_inspect_binary(m_env, term, &bin)) {
      out = std::string(reinterpret_cast<const char*>(bin.data), bin.size);
      return true;
    }

    char buf[256];
    if (enif_get_atom(m_env, term, buf, sizeof(buf), ERL_NIF_LATIN1)) {
      out = buf;
      return true;
    }

    return false;
  }

  // Get any JSON number (integer or float) as a double
  bool get_double(ERL_NIF_TERM term, double& out) {
    if (enif_get_double(m_env, term, &out))
      return true;
    long long_val;
    if (enif_get_int64(m_env, term, &long_val)) {
      out = static_cast<double>(long_val);
      return true;
    }
    return false;
  }

  // Get integer value
  bool get_int64(ERL_NIF_TERM term, int64_t& out) {
    long long_val;
    if (enif_get_int64(m_env, term, &long_val)) {
      out = long_val;
      return true;
    }

    double dbl;
    if (enif_get_double(m_env, term, &dbl)) {
      out = static_cast<int64_t>(dbl);
      return true;
    }

    return false;
  }

  // Look up a schema keyword in a map whose keys are atoms or binaries
  bool lookup_keyword(ERL_NIF_TERM map, ERL_NIF_TERM atom_key, const char* name, ERL_NIF_TERM& out_value) {
    if (enif_get_map_value(m_env, map, atom_key, &out_value))
      return true;
    ERL_NIF_TERM bin_key;
    unsigned char* p = enif_make_new_binary(m_env, strlen(name), &bin_key);
    std::memcpy(p, name, strlen(name));
    return enif_get_map_value(m_env, map, bin_key, &out_value);
  }

  // Compile type keyword
  bool compile_type(ERL_NIF_TERM type_term, std::string& error_msg);

  // Compile numeric constraints
  bool get_number(ERL_NIF_TERM term, Number& out);
  size_t intern_number(Number value);
  bool compile_number_keyword(SchemaOpcode op, ERL_NIF_TERM val_term, const char* keyword, std::string& error_msg);
  bool compile_minimum(ERL_NIF_TERM val_term, std::string& error_msg);
  bool compile_maximum(ERL_NIF_TERM val_term, std::string& error_msg);
  bool compile_exclusive_minimum(ERL_NIF_TERM val_term, std::string& error_msg);
  bool compile_exclusive_maximum(ERL_NIF_TERM val_term, std::string& error_msg);
  bool compile_multiple_of(ERL_NIF_TERM val_term, std::string& error_msg);

  // Compile string constraints
  void compile_min_length(ERL_NIF_TERM len_term);
  void compile_max_length(ERL_NIF_TERM len_term);
  bool compile_pattern(ERL_NIF_TERM pattern_term, std::string& error_msg);

  // Compile array constraints
  void compile_min_items(ERL_NIF_TERM len_term);
  void compile_max_items(ERL_NIF_TERM len_term);
  void compile_unique_items(ERL_NIF_TERM unique_term);
  bool compile_items(ERL_NIF_TERM items_term, ERL_NIF_TERM schema_term, std::string& error_msg);
  bool compile_prefix_items(ERL_NIF_TERM list_term, std::string& error_msg);

  // Schema positions accept a schema object or a boolean schema (true / false)
  bool is_schema_term(ERL_NIF_TERM term) const {
    return enif_is_map(m_env, term) || enif_is_identical(term, AM_TRUE) || enif_is_identical(term, AM_FALSE);
  }

  // Interned regular expression index (deduplicated per compilation)
  bool intern_regex(const std::string& pattern, size_t& index, std::string& error_msg);

  // Serialize a JSON value for const/enum comparison
  bool encode_json(ERL_NIF_TERM term, std::vector<uint8_t>& out, std::string& error_msg);
  void push_varint(std::vector<uint8_t>& out, uint64_t value);

  bool compile_const(ERL_NIF_TERM value, std::string& error_msg);

  // RFC 3986 reference resolution of `ref` against `base` ("" = no base)
  static std::string resolve_uri(const std::string& base, const std::string& ref);
  static std::string percent_decode(const std::string& text);
  bool compile_enum(ERL_NIF_TERM value, std::string& error_msg);
  bool compile_contains(ERL_NIF_TERM value, ERL_NIF_TERM schema_term, std::string& error_msg);
  bool compile_pattern_properties(ERL_NIF_TERM value, std::string& error_msg);
  bool compile_property_names(ERL_NIF_TERM value, std::string& error_msg);
  bool type_mask_of(const std::string& name, uint8_t& mask) const;

  // Compile object constraints
  void compile_min_properties(ERL_NIF_TERM len_term);
  void compile_max_properties(ERL_NIF_TERM len_term);
  bool compile_required(ERL_NIF_TERM required_term, std::string& error_msg);
  bool compile_properties(ERL_NIF_TERM props_term, std::string& error_msg);
  bool compile_additional_properties(ERL_NIF_TERM ap_term, ERL_NIF_TERM schema_term, std::string& error_msg);

  // Compile logical operators
  bool compile_not(ERL_NIF_TERM not_term, std::string& error_msg);
  bool compile_any_of(ERL_NIF_TERM any_of_term, std::string& error_msg);
  bool compile_one_of(ERL_NIF_TERM one_of_term, std::string& error_msg);
  bool compile_all_of(ERL_NIF_TERM all_of_term, std::string& error_msg);

  // Compile references
  bool compile_ref(ERL_NIF_TERM ref_term, std::string& error_msg);

  // Compile conditionals
  bool compile_if_then_else(
    ERL_NIF_TERM if_term,
    ERL_NIF_TERM then_term,
    ERL_NIF_TERM else_term,
    std::string& error_msg
  );

  // Compile format validation
  bool compile_format(ERL_NIF_TERM format_term, std::string& error_msg);

  // Compile a schema as a separate program; returns its index in m_programs
  bool compile_program(ERL_NIF_TERM schema_term, size_t& index, std::string& error_msg);

  // Compile the keywords of one schema object into the current program
  bool compile_schema_term(ERL_NIF_TERM schema_term, std::string& error_msg);

  // Index every $id, $anchor and $dynamicAnchor in a document
  void prescan(ERL_NIF_TERM node, const std::string& base);

  // Map lookup by name; keys may be binaries or existing atoms
  bool lookup_str(ERL_NIF_TERM map, const std::string& name, ERL_NIF_TERM& out);

  // Follow a JSON Pointer ("/a/b/0", with ~0 and ~1 escapes) from a document
  bool walk_pointer(ERL_NIF_TERM doc, const std::string& pointer, ERL_NIF_TERM& out);

  // Split a $ref into document, memo key and target schema
  bool resolve_ref(const std::string& ref, std::string& doc, std::string& memo,
                   ERL_NIF_TERM& target, std::string& error_msg);

  // Program index of a schema, compiling it once (cycles reuse the reserved slot)
  bool compile_target(const std::string& memo, ERL_NIF_TERM term, const std::string& doc,
                      size_t& index, std::string& error_msg);

  bool compile_dynamic_ref(ERL_NIF_TERM ref_term, std::string& error_msg);
  bool compile_unevaluated(SchemaOpcode op, ERL_NIF_TERM value, std::string& error_msg);

  // Compile a schema into an already-reserved program slot
  bool compile_into(size_t index, ERL_NIF_TERM schema_term, std::string& error_msg);

  // Reserve a program slot belonging to the document being compiled
  size_t new_program();

  // Compile each schema of an array into its own program
  bool compile_schema_list(ERL_NIF_TERM list_term, std::vector<size_t>& out, std::string& error_msg);

  // anyOf / oneOf / allOf: a non-empty array of schemas
  bool compile_schema_choice(SchemaOpcode op, ERL_NIF_TERM list_term, const char* keyword, std::string& error_msg);

  // dependentRequired / dependentSchemas
  bool compile_dependent_required(ERL_NIF_TERM map_term, std::string& error_msg);
  bool compile_dependent_schemas(ERL_NIF_TERM map_term, std::string& error_msg);

  // Bytecode emission helpers (optimized with append instead of insert for efficiency)
  void emit_opcode(SchemaOpcode op) {
    m_bytecode.push_back(static_cast<uint8_t>(op));
  }

  // Numeric bounds are stored as IEEE-754 doubles (8 bytes, little-endian)
  void emit_double(double val) {
    uint8_t buf[sizeof(double)];
    std::memcpy(buf, &val, sizeof(double));
    m_bytecode.insert(m_bytecode.end(), buf, buf + sizeof(double));
  }

  void emit_varint(uint64_t val) {
    uint8_t buf[10];
    size_t len = encode_varint(val, buf);
    // Use append instead of insert for better performance (O(1) vs O(n))
    m_bytecode.insert(m_bytecode.end(), buf, buf + len);
  }

  void emit_bytes(const uint8_t* data, size_t len) {
    // Pre-allocate if needed to reduce reallocations
    if (m_bytecode.capacity() < m_bytecode.size() + len)
      m_bytecode.reserve(m_bytecode.size() + len + 256);
    m_bytecode.insert(m_bytecode.end(), data, data + len);
  }

  void emit_string(const std::string& s) {
    emit_varint(s.size());
    if (m_bytecode.capacity() < m_bytecode.size() + s.size())
      m_bytecode.reserve(m_bytecode.size() + s.size() + 256);
    m_bytecode.insert(m_bytecode.end(), s.begin(), s.end());
  }
};

}  // namespace glz

// Include implementation at end of header
#include "glazer_json_schema_compiler.hxx"
