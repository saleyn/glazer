// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validator NIF interface - implementation
//
// This file contains method implementations and is included at the end of
// glazer_schema_nif.hpp. Do not include this directly.
//-----------------------------------------------------------------------------
namespace glz {

// Cleanup function for schema resources
// SCHEMA_RESOURCE_TYPE is defined in glazer_nif.cpp's nif_load()

void schema_resource_cleanup(ErlNifEnv* env, void* obj) {
  SchemaResource* res = static_cast<SchemaResource*>(obj);
  res->~SchemaResource();
}

ERL_NIF_TERM nif_compile_schema(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
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

bool parse_validation_options(
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

ERL_NIF_TERM nif_validate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
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

ERL_NIF_TERM nif_validate_term(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
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

// NIF lifecycle functions defined in glazer_nif.cpp
// This file is only for the implementation of NIF functions that handle schema validation

} // namespace glz