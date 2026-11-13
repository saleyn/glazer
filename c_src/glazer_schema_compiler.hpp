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

#include <erl_nif.h>

#include "glazer_atoms.hpp"
#include "glazer_schema.hpp"

namespace glz {

class SchemaCompiler {
 public:
  SchemaCompiler(ErlNifEnv* env) : m_env(env) {}

  // Compile a schema (map or list) to bytecode
  // Returns: {success, CompiledSchema} or {false, error_msg}
  std::pair<bool, CompiledSchema> compile(
    ERL_NIF_TERM schema_term,
    std::string& error_msg
  );

 private:
  ErlNifEnv* m_env;
  std::vector<uint8_t> m_bytecode;
  std::vector<std::shared_ptr<std::regex>> m_patterns;
  std::unordered_map<std::string, size_t> m_pattern_cache;  // O(1) deduplication
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

  // Get map value by key
  bool get_map_value(
    ERL_NIF_TERM map,
    const char* key_str,
    ERL_NIF_TERM& out_value
  ) {
    ERL_NIF_TERM key = enif_make_atom(m_env, key_str);
    return enif_get_map_value(m_env, map, key, &out_value);
  }

  // Compile type keyword
  bool compile_type(ERL_NIF_TERM type_term, std::string& error_msg);

  // Compile numeric constraints
  void compile_minimum(ERL_NIF_TERM val_term);
  void compile_maximum(ERL_NIF_TERM val_term);
  void compile_exclusive_minimum(ERL_NIF_TERM val_term);
  void compile_exclusive_maximum(ERL_NIF_TERM val_term);
  void compile_multiple_of(ERL_NIF_TERM val_term);

  // Compile string constraints
  void compile_min_length(ERL_NIF_TERM len_term);
  void compile_max_length(ERL_NIF_TERM len_term);
  void compile_pattern(ERL_NIF_TERM pattern_term, std::string& error_msg);

  // Compile array constraints
  void compile_min_items(ERL_NIF_TERM len_term);
  void compile_max_items(ERL_NIF_TERM len_term);
  void compile_unique_items(ERL_NIF_TERM unique_term);
  bool compile_items(ERL_NIF_TERM items_term, std::string& error_msg);

  // Compile object constraints
  void compile_min_properties(ERL_NIF_TERM len_term);
  void compile_max_properties(ERL_NIF_TERM len_term);
  bool compile_required(ERL_NIF_TERM required_term, std::string& error_msg);
  bool compile_properties(ERL_NIF_TERM props_term, std::string& error_msg);
  bool compile_additional_properties(ERL_NIF_TERM ap_term, std::string& error_msg);

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
  void compile_format(ERL_NIF_TERM format_term);

  // Compile a complete schema recursively
  bool compile_schema_term(ERL_NIF_TERM schema_term, std::string& error_msg);

  // Bytecode emission helpers (optimized with append instead of insert for efficiency)
  void emit_opcode(SchemaOpcode op) {
    m_bytecode.push_back(static_cast<uint8_t>(op));
  }

  void emit_varint(uint64_t val) {
    uint8_t buf[10];
    size_t len = encode_varint(val, buf);
    // Use append instead of insert for better performance (O(1) vs O(n))
    m_bytecode.insert(m_bytecode.end(), buf, buf + len);
  }

  void emit_bytes(const uint8_t* data, size_t len) {
    // Pre-allocate if needed to reduce reallocations
    if (m_bytecode.capacity() < m_bytecode.size() + len) {
      m_bytecode.reserve(m_bytecode.size() + len + 256);
    }
    m_bytecode.insert(m_bytecode.end(), data, data + len);
  }

  void emit_string(const std::string& s) {
    emit_varint(s.size());
    if (m_bytecode.capacity() < m_bytecode.size() + s.size()) {
      m_bytecode.reserve(m_bytecode.size() + s.size() + 256);
    }
    m_bytecode.insert(m_bytecode.end(), s.begin(), s.end());
  }
};

}  // namespace glz

// Include implementation at end of header
#include "glazer_schema_compiler.hxx"
