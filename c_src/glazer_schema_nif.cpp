// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validator NIF interface
//
// Exposes schema validation functions to Erlang:
//   - glazer_json:compile_schema/1
//   - glazer_json:validate/2,3
//   - glazer_json:validate_term/2,3
//   - glazer_json:validate_batch/2,3
//-----------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#include <erl_nif.h>

#include "glazer_atoms.hpp"
#include "glazer_common.hpp"
#include "glazer_schema.hpp"
#include "glazer_schema_compiler.hpp"
#include "glazer_schema_validator.hpp"

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

static ErlNifResourceType* SCHEMA_RESOURCE_TYPE = nullptr;

static void schema_resource_cleanup(ErlNifEnv* env, void* obj) {
  SchemaResource* res = static_cast<SchemaResource*>(obj);
  res->~SchemaResource();
}

//-----------------------------------------------------------------------------
// NIF: compile_schema/1
//
// Input:  Schema as map (decoded JSON)
// Output: {ok, {schema, OpaqueHandle}} | {error, ErrorMsg}
//-----------------------------------------------------------------------------

static ERL_NIF_TERM nif_compile_schema(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  if (argc != 1) {
    return enif_make_badarg(env);
  }

  ERL_NIF_TERM schema_term = argv[0];

  if (!enif_is_map(env, schema_term)) {
    return enif_make_tuple2(
      env,
      AM_ERROR,
      enif_make_string(env, "schema must be a map", ERL_NIF_LATIN1)
    );
  }

  SchemaCompiler compiler(env);
  std::string error_msg;
  auto [success, compiled_schema] = compiler.compile(schema_term, error_msg);

  if (!success) {
    return enif_make_tuple2(
      env,
      AM_ERROR,
      enif_make_string(env, error_msg.c_str(), ERL_NIF_LATIN1)
    );
  }

  // Allocate resource
  SchemaResource* res = static_cast<SchemaResource*>(
    enif_alloc_resource(SCHEMA_RESOURCE_TYPE, sizeof(SchemaResource))
  );
  if (!res) {
    return enif_make_tuple2(
      env,
      AM_ERROR,
      enif_make_string(env, "resource allocation failed", ERL_NIF_LATIN1)
    );
  }

  new (res) SchemaResource();
  res->schema = std::make_shared<CompiledSchema>(compiled_schema);

  ERL_NIF_TERM resource_term = enif_make_resource(env, res);
  enif_release_resource(res);

  return enif_make_tuple2(
    env,
    AM_OK,
    enif_make_tuple2(
      env,
      enif_make_atom(env, "schema"),
      resource_term
    )
  );
}

//-----------------------------------------------------------------------------
// Helper: parse validation options
//
// Options = [fail_fast | collect_all_errors | {max_depth, N} | {max_errors, N} | {timeout_ms, N}]
//-----------------------------------------------------------------------------

struct ValidationOptions {
  ValidationMode mode              = ValidationMode::FAIL_FAST;
  size_t max_depth                 = 128;
  size_t max_errors                = 1;
  int timeout_ms                   = 0;
};

static bool parse_validation_options(
  ErlNifEnv* env,
  ERL_NIF_TERM options_list,
  ValidationOptions& opts
) {
  ERL_NIF_TERM head, tail = options_list;

  while (enif_get_list_cell(env, tail, &head, &tail)) {
    if (enif_is_identical(head, enif_make_atom(env, "fail_fast"))) {
      opts.mode = ValidationMode::FAIL_FAST;
    } else if (enif_is_identical(head, enif_make_atom(env, "collect_all_errors"))) {
      opts.mode = ValidationMode::COLLECT_ALL;
    } else {
      int arity;
      const ERL_NIF_TERM* tuple_items;
      if (enif_get_tuple(env, head, &arity, &tuple_items) && arity == 2) {
        ERL_NIF_TERM key = tuple_items[0];
        ERL_NIF_TERM val = tuple_items[1];

        if (enif_is_identical(key, enif_make_atom(env, "max_depth"))) {
          long max_depth_val;
          if (enif_get_int64(env, val, &max_depth_val)) {
            opts.max_depth = static_cast<size_t>(max_depth_val);
          }
        } else if (enif_is_identical(key, enif_make_atom(env, "max_errors"))) {
          long max_errors_val;
          if (enif_get_int64(env, val, &max_errors_val)) {
            opts.max_errors = static_cast<size_t>(max_errors_val);
          }
        } else if (enif_is_identical(key, enif_make_atom(env, "timeout_ms"))) {
          long timeout_val;
          if (enif_get_int64(env, val, &timeout_val)) {
            opts.timeout_ms = static_cast<int>(timeout_val);
          }
        }
      }
    }
  }

  return true;
}

//-----------------------------------------------------------------------------
// NIF: validate/2,3
//
// Signature:
//   validate(CompiledSchema, Term)
//   validate(CompiledSchema, Term, Options)
//
// Input:  {schema, Resource}, term(), [validate_opt()]
// Output: ok | {error, {path, [...], reason}}  (fail-fast mode)
//         ok | {error, [{path, [...], reason}, ...]}  (collect_all mode)
//-----------------------------------------------------------------------------

static ERL_NIF_TERM nif_validate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  if (argc < 2 || argc > 3) {
    return enif_make_badarg(env);
  }

  ERL_NIF_TERM schema_term = argv[0];
  ERL_NIF_TERM data_term = argv[1];
  ValidationOptions opts;

  if (argc == 3) {
    if (!enif_is_list(env, argv[2])) {
      return enif_make_badarg(env);
    }
    if (!parse_validation_options(env, argv[2], opts)) {
      return enif_make_badarg(env);
    }
  }

  // Extract schema resource from {schema, Resource}
  int schema_arity;
  const ERL_NIF_TERM* schema_items;
  if (!enif_get_tuple(env, schema_term, &schema_arity, &schema_items) || schema_arity != 2) {
    return enif_make_badarg(env);
  }

  SchemaResource* res;
  if (!enif_get_resource(env, schema_items[1], SCHEMA_RESOURCE_TYPE, (void**)&res)) {
    return enif_make_badarg(env);
  }

  // Estimate validation work
  size_t estimated_work_us = (res->schema->total_constraints * 10) +
                              (static_cast<size_t>(opts.max_depth) * opts.max_errors);
  bool use_dirty_scheduler = (estimated_work_us >= 1000) ||
                              (opts.mode == ValidationMode::COLLECT_ALL && estimated_work_us >= 500);

  if (use_dirty_scheduler) {
    // Schedule on dirty CPU scheduler
    ERL_NIF_TERM dirty_argv[] = {schema_term, data_term, enif_make_list(env, 0)};
    // TODO: dispatch to dirty scheduler
    // For now, run inline and report work
  }

  // Run validation
  SchemaValidator validator(env, *res->schema);
  auto [success, result_term] = validator.validate(
    data_term,
    opts.mode,
    opts.max_depth,
    opts.max_errors,
    opts.timeout_ms
  );

  // Report work to scheduler
  update_reduction_count(env, estimated_work_us / 10);

  if (success) {
    return result_term;
  } else {
    return enif_make_tuple2(env, AM_ERROR, result_term);
  }
}

//-----------------------------------------------------------------------------
// NIF: validate_term/2,3
//
// Like validate/2,3 but takes schema as term (not compiled resource)
// Compiles schema inline then validates.
//
// Input:  schema_term, data_term, [validate_opt()]
// Output: ok | {error, ...}
//-----------------------------------------------------------------------------

static ERL_NIF_TERM nif_validate_term(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  if (argc < 2 || argc > 3) {
    return enif_make_badarg(env);
  }

  ERL_NIF_TERM schema_term = argv[0];
  ERL_NIF_TERM data_term = argv[1];
  ValidationOptions opts;

  if (argc == 3) {
    if (!enif_is_list(env, argv[2])) {
      return enif_make_badarg(env);
    }
    parse_validation_options(env, argv[2], opts);
  }

  // Compile schema inline
  SchemaCompiler compiler(env);
  std::string error_msg;
  auto [success, compiled_schema] = compiler.compile(schema_term, error_msg);

  if (!success) {
    return enif_make_tuple2(env, AM_ERROR, enif_make_string(env, error_msg.c_str(), ERL_NIF_LATIN1));
  }

  // Validate
  SchemaValidator validator(env, compiled_schema);
  auto [valid, result_term] = validator.validate(
    data_term,
    opts.mode,
    opts.max_depth,
    opts.max_errors,
    opts.timeout_ms
  );

  if (valid) {
    return result_term;
  } else {
    return enif_make_tuple2(env, AM_ERROR, result_term);
  }
}

//-----------------------------------------------------------------------------
// NIF registration
//-----------------------------------------------------------------------------

static int nif_load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info) {
  // Create resource type for compiled schemas
  SCHEMA_RESOURCE_TYPE = enif_open_resource_type(
    env,
    "glazer_json",
    "compiled_schema",
    schema_resource_cleanup,
    ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER,
    nullptr
  );

  if (!SCHEMA_RESOURCE_TYPE) {
    return -1;
  }

  return 0;
}

static int nif_unload(ErlNifEnv* env, void* priv_data) {
  return 0;
}

static int nif_reload(ErlNifEnv* env, void* priv_data, ERL_NIF_TERM load_info) {
  return 0;
}

static int nif_upgrade(ErlNifEnv* env, void* priv_data, void** new_priv_data, ERL_NIF_TERM load_info) {
  return 0;
}

static ErlNifFunc nif_funcs[] = {
  {"compile_schema", 1, nif_compile_schema, 0},
  {"validate", 2, nif_validate, 0},
  {"validate", 3, nif_validate, 0},
  {"validate_term", 2, nif_validate_term, 0},
  {"validate_term", 3, nif_validate_term, 0},
};

ERL_NIF_INIT(
  glazer_json,
  nif_funcs,
  nif_load,
  nif_reload,
  nif_upgrade,
  nif_unload
)

}  // namespace glz
