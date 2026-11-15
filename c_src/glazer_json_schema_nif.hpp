// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validator NIF interface
//
// Exposes schema validation functions to Erlang:
//   - glazer_schema:compile_schema/1
//   - glazer_schema:validate/2,3
//   - glazer_schema:validate_term/2,3
//   - glazer_json:validate_batch/2,3
//
// This header is included by glazer_nif.cpp to register schema functions
// in the NIF module.
//-----------------------------------------------------------------------------

#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <erl_nif.h>

#include "glazer_atoms.hpp"
#include "glazer_common.hpp"
#include "glazer_json_schema.hpp"
#include "glazer_json_schema_compiler.hpp"
#include "glazer_json_schema_validator.hpp"

// Note: This file is included at the END of glazer_nif.cpp to ensure all
// schema functions are registered. The schema namespace declarations merge
// with the glz namespace in glazer_nif.cpp.

namespace glz {

//-----------------------------------------------------------------------------
// Dirty-scheduler threshold for schema validation
//-----------------------------------------------------------------------------

static constexpr size_t SCHEMA_DIRTY_THRESHOLD_BYTES = 16384;
static constexpr size_t SCHEMA_DIRTY_THRESHOLD_CONSTRAINTS = 100;

//-----------------------------------------------------------------------------
// Resource type for compiled schemas
//
// Holds a shared_ptr to CompiledSchema so it's reference-counted and
// automatically deleted when all Erlang references are released.
//-----------------------------------------------------------------------------

struct SchemaResource {
  std::shared_ptr<CompiledSchema> schema;
};

extern ErlNifResourceType* SCHEMA_RESOURCE_TYPE;

void schema_resource_cleanup(ErlNifEnv* env, void* obj);

//-----------------------------------------------------------------------------
// NIF function declarations
//-----------------------------------------------------------------------------

ERL_NIF_TERM nif_compile_schema(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_validate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_validate_term(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

//-----------------------------------------------------------------------------
// Validation options structure
//-----------------------------------------------------------------------------

struct ValidationOptions {
  ValidationMode mode              = ValidationMode::FAIL_FAST;
  size_t max_depth                 = 128;
  size_t max_errors                = 1;
  bool   max_errors_set            = false;  // explicit max_errors option given
  int timeout_ms                   = 0;
};

// Error messages cross into Erlang as binaries
inline ERL_NIF_TERM make_message_binary(ErlNifEnv* env, const std::string& msg) {
  ERL_NIF_TERM term;
  unsigned char* p = enif_make_new_binary(env, msg.size(), &term);
  std::memcpy(p, msg.data(), msg.size());
  return term;
}

bool parse_validation_options(
  ErlNifEnv* env,
  ERL_NIF_TERM options_list,
  ValidationOptions& opts
);

}  // namespace glz

// Include implementation at end of header
#include "glazer_json_schema_nif.hxx"