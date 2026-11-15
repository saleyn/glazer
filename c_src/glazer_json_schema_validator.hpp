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

#include "glazer_common.hpp"
#include "glazer_json_schema.hpp"

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
  // Binary key terms for property names taken from the bytecode, reused across
  // the objects of one validation (names are looked up again per element)
  KeyCache m_names;
  // The binary term for a property name, from the cache when possible
  ERL_NIF_TERM name_term(std::string_view name);

  // Recursively validate a value against schema bytecode
  bool validate_value(
    ERL_NIF_TERM term,
    size_t bytecode_offset,
    ValidationContext& ctx
  );

  // Validate type constraint
  bool check_type(ERL_NIF_TERM term, uint8_t type_mask, ValidationContext& ctx);

  // Numeric constraints
  bool compare_to_bound(ERL_NIF_TERM term, const Number& bound, int& cmp);
  bool check_minimum(ERL_NIF_TERM term, const Number& min_val, ValidationContext& ctx);
  bool check_maximum(ERL_NIF_TERM term, const Number& max_val, ValidationContext& ctx);
  bool check_exclusive_minimum(ERL_NIF_TERM term, const Number& min_val, ValidationContext& ctx);
  bool check_exclusive_maximum(ERL_NIF_TERM term, const Number& max_val, ValidationContext& ctx);
  bool integer_multiple(ERL_NIF_TERM term, const Number& factor, bool& multiple);
  bool check_multiple_of(ERL_NIF_TERM term, const Number& factor, ValidationContext& ctx);

  // Numeric bound as an Erlang term: integral bounds stay integers

  // String constraints
  bool check_min_length(ERL_NIF_TERM term, size_t min_len, ValidationContext& ctx);
  bool check_max_length(ERL_NIF_TERM term, size_t max_len, ValidationContext& ctx);
  bool check_pattern(ERL_NIF_TERM term, size_t pattern_idx, ValidationContext& ctx);
  bool check_custom_format(ERL_NIF_TERM term, size_t pattern_idx, ValidationContext& ctx);

  // Array constraints
  bool check_min_items(ERL_NIF_TERM term, size_t min_items, ValidationContext& ctx);
  bool check_max_items(ERL_NIF_TERM term, size_t max_items, ValidationContext& ctx);
  bool check_unique_items(ERL_NIF_TERM term, ValidationContext& ctx);
  bool check_items(ERL_NIF_TERM term, size_t items_schema_offset, size_t start, ValidationContext& ctx);
  bool check_prefix_items(ERL_NIF_TERM term, const std::vector<size_t>& offsets, ValidationContext& ctx);

  // Object constraints
  bool check_min_properties(ERL_NIF_TERM term, size_t min_props, ValidationContext& ctx);
  bool check_max_properties(ERL_NIF_TERM term, size_t max_props, ValidationContext& ctx);
  bool check_required(ERL_NIF_TERM term, const std::vector<std::string_view>& required_fields, ValidationContext& ctx);
  bool get_map_field(ERL_NIF_TERM map, std::string_view name, ERL_NIF_TERM* out);
  bool check_properties(
    ERL_NIF_TERM term,
    const std::vector<std::pair<std::string_view, size_t>>& props,  // name -> program offset
    ValidationContext& ctx
  );
  bool check_additional_properties(
    ERL_NIF_TERM term,
    int mode,                           // 0: extra keys forbidden, 1: schema, 2: allowed (but evaluated)
    const std::vector<std::string_view>& declared,
    const std::vector<size_t>& patterns,  // keys matching these are not additional
    size_t schema_offset,               // used when has_schema
    ValidationContext& ctx
  );

  bool check_pattern_properties(
    ERL_NIF_TERM term,
    const std::vector<std::pair<size_t, size_t>>& entries,  // pattern index -> program offset
    ValidationContext& ctx
  );
  bool check_property_names(ERL_NIF_TERM term, size_t schema_offset, ValidationContext& ctx);
  bool check_contains(ERL_NIF_TERM term, size_t schema_offset, size_t min_count, size_t max_count,
                      bool has_max, ValidationContext& ctx);
  bool check_const(ERL_NIF_TERM term, size_t offset, size_t length, ValidationContext& ctx);
  bool check_enum(ERL_NIF_TERM term, const std::vector<std::pair<size_t, size_t>>& values,
                  ValidationContext& ctx);

  // Compare an Erlang term with a value serialized by the compiler
  bool equal_encoded(ERL_NIF_TERM term, const uint8_t* buf, size_t len, size_t& pos);
  // JSON equality between two Erlang terms (numbers compare by value)
  bool terms_equal(ERL_NIF_TERM a, ERL_NIF_TERM b);
  // Read a varint from a buffer; false on truncation
  static bool read_varint_at(const uint8_t* buf, size_t len, size_t& pos, uint64_t& out);

  // Logical operators
  bool check_not(ERL_NIF_TERM term, size_t not_schema_offset, ValidationContext& ctx);
  bool check_any_of(ERL_NIF_TERM term, const std::vector<size_t>& schemas, ValidationContext& ctx);
  bool check_one_of(ERL_NIF_TERM term, const std::vector<size_t>& schemas, ValidationContext& ctx);
  bool check_all_of(ERL_NIF_TERM term, const std::vector<size_t>& schemas, ValidationContext& ctx);
  bool check_if_then_else(ERL_NIF_TERM term, size_t if_off, size_t then_off, size_t else_off, ValidationContext& ctx);

  // Dependencies
  bool check_dependent_required(
    ERL_NIF_TERM term,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& deps,
    ValidationContext& ctx
  );
  bool check_dependent_schemas(
    ERL_NIF_TERM term,
    const std::vector<std::pair<std::string, size_t>>& deps,
    ValidationContext& ctx
  );

  // Does term satisfy the program at offset? Errors are discarded. When it does,
  // its annotations are merged into `annotations` (may be null).
  bool matches(ERL_NIF_TERM term, size_t offset, ValidationContext& ctx, Evaluated* annotations);

  // Validate a child instance: its keywords record into their own annotations
  bool validate_child(ERL_NIF_TERM term, size_t offset, ValidationContext& ctx);

  // Validate an applicator ($ref, allOf, if/then/else, ...): it has its own
  // annotations, which are merged into the parent instance's annotations
  bool validate_applicator(ERL_NIF_TERM term, size_t offset, ValidationContext& ctx);

  static void merge_evaluated(Evaluated& into, const Evaluated& from);

  bool check_unevaluated_properties(ERL_NIF_TERM term, bool forbid, size_t schema_offset, ValidationContext& ctx);
  bool check_unevaluated_items(ERL_NIF_TERM term, bool forbid, size_t schema_offset, ValidationContext& ctx);

  // Read a program index from bytecode and return its start offset
  bool read_program(size_t& bytecode_offset, size_t& out_offset);

  static constexpr size_t NO_BRANCH = static_cast<size_t>(-1);

  // Helper: get int64 value from term
  bool get_number_double(ERL_NIF_TERM term, double& out);

  // Helper: get string from binary or atom
  bool get_string(ERL_NIF_TERM term, std::string& out);

  // Helper: make error tuple
  ERL_NIF_TERM make_error_tuple(const ValidationError& err);

  // Helper: error body {path, Path, {Reason, Detail}} shared by both forms
  ERL_NIF_TERM make_error_body(const ValidationError& err);

  // Helper: make error list (works with std::vector or SmallVec)
  template<typename ErrorVec>
  ERL_NIF_TERM make_error_list(const ErrorVec& errors);

  // Read varint from bytecode
  bool read_varint(size_t& offset, uint64_t& out);
  bool read_double(size_t& offset, double& out);
  bool read_string(size_t& offset, std::string& out);
  bool key_view(ERL_NIF_TERM key, std::string& scratch, std::string_view& out);
  // A string as a view into the bytecode (valid while the schema is alive)
  bool read_string_view(size_t& offset, std::string_view& out);

  // Build an Erlang binary from a std::string
  ERL_NIF_TERM binary_term(std::string_view s);
};

// Forward declaration for format validator (defined in .hxx)
bool check_format_value(ErlNifEnv* env, ERL_NIF_TERM term, std::string_view format);

}  // namespace glz

// Include implementation at end of header
#include "glazer_json_schema_validator.hxx"