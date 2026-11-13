// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validation engine — recursive validator with fail-fast and
// collect-all modes.
//
// Processes pre-compiled bytecode schema against Erlang terms.
// Handles error path tracking and constraint checking.
//-----------------------------------------------------------------------------
#pragma once

#include <cassert>
#include <cstring>

#include <erl_nif.h>

#include "glazer_schema.hpp"

namespace glz {

class SchemaValidator {
 public:
  SchemaValidator(ErlNifEnv* env, const CompiledSchema& schema)
    : m_env(env), m_schema(schema) {}

  // Validate a term against schema with given options
  // Returns: {success, result_term}
  std::pair<bool, ERL_NIF_TERM> validate(
    ERL_NIF_TERM term,
    ValidationMode mode,
    size_t max_depth,
    size_t max_errors,
    int timeout_ms
  );

 private:
  ErlNifEnv* m_env;
  const CompiledSchema& m_schema;

  // Recursively validate a value against schema bytecode
  bool validate_value(
    ERL_NIF_TERM term,
    size_t bytecode_offset,
    ValidationContext& ctx
  );

  // Validate type constraint
  bool check_type(ERL_NIF_TERM term, uint8_t type_mask, ValidationContext& ctx);

  // Numeric constraints
  bool check_minimum(ERL_NIF_TERM term, int64_t min_val, ValidationContext& ctx);
  bool check_maximum(ERL_NIF_TERM term, int64_t max_val, ValidationContext& ctx);
  bool check_exclusive_minimum(ERL_NIF_TERM term, int64_t min_val, ValidationContext& ctx);
  bool check_exclusive_maximum(ERL_NIF_TERM term, int64_t max_val, ValidationContext& ctx);
  bool check_multiple_of(ERL_NIF_TERM term, int64_t factor, ValidationContext& ctx);

  // String constraints
  bool check_min_length(ERL_NIF_TERM term, size_t min_len, ValidationContext& ctx);
  bool check_max_length(ERL_NIF_TERM term, size_t max_len, ValidationContext& ctx);
  bool check_pattern(ERL_NIF_TERM term, size_t pattern_idx, ValidationContext& ctx);

  // Array constraints
  bool check_min_items(ERL_NIF_TERM term, size_t min_items, ValidationContext& ctx);
  bool check_max_items(ERL_NIF_TERM term, size_t max_items, ValidationContext& ctx);
  bool check_unique_items(ERL_NIF_TERM term, ValidationContext& ctx);
  bool check_items(ERL_NIF_TERM term, size_t items_schema_offset, ValidationContext& ctx);

  // Object constraints
  bool check_min_properties(ERL_NIF_TERM term, size_t min_props, ValidationContext& ctx);
  bool check_max_properties(ERL_NIF_TERM term, size_t max_props, ValidationContext& ctx);
  bool check_required(ERL_NIF_TERM term, const std::vector<std::string>& required_fields, ValidationContext& ctx);
  bool check_properties(ERL_NIF_TERM term, size_t props_schema_offset, ValidationContext& ctx);
  bool check_additional_properties(ERL_NIF_TERM term, ERL_NIF_TERM ap_schema, ValidationContext& ctx);

  // Logical operators
  bool check_not(ERL_NIF_TERM term, size_t not_schema_offset, ValidationContext& ctx);
  bool check_any_of(ERL_NIF_TERM term, const std::vector<size_t>& schemas, ValidationContext& ctx);
  bool check_one_of(ERL_NIF_TERM term, const std::vector<size_t>& schemas, ValidationContext& ctx);
  bool check_all_of(ERL_NIF_TERM term, const std::vector<size_t>& schemas, ValidationContext& ctx);

  // Helper: get int64 value from term
  bool get_number(ERL_NIF_TERM term, int64_t& out);
  bool get_number_double(ERL_NIF_TERM term, double& out);

  // Helper: get string from binary or atom
  bool get_string(ERL_NIF_TERM term, std::string& out);

  // Helper: make error tuple
  ERL_NIF_TERM make_error_tuple(const ValidationError& err);

  // Helper: make error list
  ERL_NIF_TERM make_error_list(const std::vector<ValidationError>& errors);

  // Read varint from bytecode
  bool read_varint(size_t& offset, uint64_t& out);
};

}  // namespace glz

// Include implementation at end of header
#include "glazer_schema_validator.hxx"