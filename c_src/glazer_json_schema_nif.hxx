// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validator NIF interface - implementation
//
// This file contains method implementations and is included at the end of
// glazer_schema_nif.hpp. Do not include this directly.
//-----------------------------------------------------------------------------
#pragma once

#include "glazer_json_schema_nif.hpp"

namespace glz {

// Cleanup function for schema resources
// SCHEMA_RESOURCE_TYPE is defined in glazer_nif.cpp's nif_load()

void schema_resource_cleanup(ErlNifEnv* env, void* obj) {
  SchemaResource* res = static_cast<SchemaResource*>(obj);
  res->~SchemaResource();
}

// Options for compile_schema/2:
//   [{refs, #{Uri => Schema}}]   documents that $ref may name by URI
//   [{format_assertion, true}]   assert `format` (default: annotation only)
// Returns false (and sets error) on a malformed option.
static bool apply_compile_options(
  ErlNifEnv* env,
  ERL_NIF_TERM opts,
  SchemaCompiler& compiler,
  std::string& error
) {
  if (!enif_is_list(env, opts)) {
    error = "options must be a list";
    return false;
  }

  ERL_NIF_TERM head, tail = opts;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    int arity;
    const ERL_NIF_TERM* items;
    if (!enif_get_tuple(env, head, &arity, &items) || arity != 2) {
      error = "unknown compile option";
      return false;
    }
    if (enif_is_identical(items[0], AM_FORMAT_ASSERTION)) {
      if (enif_is_identical(items[1], AM_TRUE))
        compiler.set_format_assertion(true);
      else if (enif_is_identical(items[1], AM_FALSE))
        compiler.set_format_assertion(false);
      else {
        error = "format_assertion must be true or false";
        return false;
      }
      continue;
    }
    if (enif_is_identical(items[0], AM_FORMATS)) {
      // formats: #{Name => Pattern}, both binaries
      if (!enif_is_map(env, items[1])) {
        error = "formats must be a map of name to pattern";
        return false;
      }
      ErlNifMapIterator it;
      if (!enif_map_iterator_create(env, items[1], &it, ERL_NIF_MAP_ITERATOR_FIRST)) {
        error = "formats must be a map of name to pattern";
        return false;
      }
      bool ok = true;
      ERL_NIF_TERM k, v;
      while (ok && enif_map_iterator_get_pair(env, &it, &k, &v)) {
        ErlNifBinary name, pattern;
        if (!enif_inspect_binary(env, k, &name) || !enif_inspect_binary(env, v, &pattern)) {
          error = "formats must map binary names to binary patterns";
          ok = false;
        } else {
          compiler.set_custom_format(
            std::string(reinterpret_cast<const char*>(name.data), name.size),
            std::string(reinterpret_cast<const char*>(pattern.data), pattern.size));
          enif_map_iterator_next(env, &it);
        }
      }
      enif_map_iterator_destroy(env, &it);
      if (!ok) return false;
      continue;
    }
    if (!enif_is_identical(items[0], AM_REFS)) {
      error = "unknown compile option";
      return false;
    }
    if (!enif_is_map(env, items[1])) {
      error = "refs must be a map of URI to schema";
      return false;
    }

    ERL_NIF_TERM key, value;
    ErlNifMapIterator it;
    if (!enif_map_iterator_create(env, items[1], &it, ERL_NIF_MAP_ITERATOR_FIRST)) {
      error = "refs must be a map of URI to schema";
      return false;
    }
    while (enif_map_iterator_get_pair(env, &it, &key, &value)) {
      ErlNifBinary bin;
      char atom_buf[256];
      std::string uri;
      if (enif_inspect_binary(env, key, &bin))
        uri.assign(reinterpret_cast<const char*>(bin.data), bin.size);
      else if (enif_get_atom(env, key, atom_buf, sizeof(atom_buf), ERL_NIF_UTF8))
        uri = atom_buf;
      else {
        enif_map_iterator_destroy(env, &it);
        error = "refs keys must be URI strings";
        return false;
      }
      compiler.add_document(uri, value);
      enif_map_iterator_next(env, &it);
    }
    enif_map_iterator_destroy(env, &it);
  }
  return true;
}

ERL_NIF_TERM nif_compile_schema(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  if (argc < 1 || argc > 2) {
    return enif_make_badarg(env);
  }

  ERL_NIF_TERM schema_term = argv[0];

  if (!enif_is_map(env, schema_term) &&
      !enif_is_identical(schema_term, AM_TRUE) &&
      !enif_is_identical(schema_term, AM_FALSE)) {
    return enif_make_tuple2(
      env,
      AM_ERROR,
      make_message_binary(env, "schema must be a map")
    );
  }

  SchemaCompiler compiler(env);
  std::string error_msg;
  if (argc == 2 && !apply_compile_options(env, argv[1], compiler, error_msg)) {
    return enif_make_tuple2(env, AM_ERROR, make_message_binary(env, error_msg));
  }

  auto [success, compiled_schema] = compiler.compile(schema_term, error_msg);

  if (!success) {
    return enif_make_tuple2(
      env,
      AM_ERROR,
      make_message_binary(env, error_msg)
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
      make_message_binary(env, "resource allocation failed")
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
      AM_SCHEMA,
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
    if (enif_is_identical(head, AM_FAIL_FAST))
      opts.mode = ValidationMode::FAIL_FAST;
    else if (enif_is_identical(head, AM_COLLECT_ALL_ERRORS))
      opts.mode = ValidationMode::COLLECT_ALL;
    else {
      int arity;
      const ERL_NIF_TERM* tuple_items;
      if (enif_get_tuple(env, head, &arity, &tuple_items) && arity == 2) {
        auto key = tuple_items[0];
        auto val = tuple_items[1];

        if (enif_is_identical(key, AM_MAX_DEPTH)) {
          ErlNifSInt64 max_depth_val;
          if (enif_get_int64(env, val, &max_depth_val))
            opts.max_depth = static_cast<size_t>(max_depth_val);
        } else if (enif_is_identical(key, AM_MAX_ERRORS)) {
          ErlNifSInt64 max_errors_val;
          if (enif_get_int64(env, val, &max_errors_val)) {
            opts.max_errors = static_cast<size_t>(max_errors_val);
            opts.max_errors_set = true;
          }
        } else if (enif_is_identical(key, AM_TIMEOUT_MS)) {
          ErlNifSInt64 timeout_val;
          if (enif_get_int64(env, val, &timeout_val))
            opts.timeout_ms = static_cast<int>(timeout_val);
        }
      }
    }
  }

  if (opts.mode == ValidationMode::COLLECT_ALL && !opts.max_errors_set)
    opts.max_errors = std::numeric_limits<size_t>::max();

  return true;
}

ERL_NIF_TERM nif_validate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  if (argc < 2 || argc > 3) [[unlikely]]
    return enif_make_badarg(env);

  ERL_NIF_TERM schema_term = argv[0];
  ERL_NIF_TERM data_term = argv[1];
  ValidationOptions opts;

  if (argc == 3) {
    if (!enif_is_list(env, argv[2])) [[unlikely]]
      return enif_make_badarg(env);
    if (!parse_validation_options(env, argv[2], opts)) [[unlikely]]
      return enif_make_badarg(env);
  }

  // Extract schema resource from {schema, Resource}
  int schema_arity;
  const ERL_NIF_TERM* schema_items;
  if (!enif_get_tuple(env, schema_term, &schema_arity, &schema_items) || schema_arity != 2)
    return enif_make_badarg(env);

  SchemaResource* res;
  if (!enif_get_resource(env, schema_items[1], SCHEMA_RESOURCE_TYPE, (void**)&res))
    return enif_make_badarg(env);

  // Estimate validation work
  size_t estimated_work_us = (res->schema->total_constraints * 10) +
                              (static_cast<size_t>(opts.max_depth) * std::min<size_t>(opts.max_errors, 1024));
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

  return success ? result_term : enif_make_tuple2(env, AM_ERROR, result_term);
}

ERL_NIF_TERM nif_validate_term(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]) {
  if (argc < 2 || argc > 3) {
    return enif_make_badarg(env);
  }

  ERL_NIF_TERM schema_term = argv[0];
  ERL_NIF_TERM data_term = argv[1];
  ValidationOptions opts;

  if (argc == 3) {
    if (!enif_is_list(env, argv[2])) [[unlikely]]
      return enif_make_badarg(env);
    parse_validation_options(env, argv[2], opts);
  }

  // Compile schema inline
  SchemaCompiler compiler(env);
  std::string error_msg;
  auto [success, compiled_schema] = compiler.compile(schema_term, error_msg);

  if (!success) [[unlikely]]
    return enif_make_tuple2(env, AM_ERROR, make_message_binary(env, error_msg));

  // Validate
  SchemaValidator validator(env, compiled_schema);
  auto [valid, result_term] = validator.validate(
    data_term,
    opts.mode,
    opts.max_depth,
    opts.max_errors,
    opts.timeout_ms
  );

  return valid ? result_term : enif_make_tuple2(env, AM_ERROR, result_term);
}

// NIF lifecycle functions defined in glazer_nif.cpp
// This file is only for the implementation of NIF functions that handle schema validation

} // namespace glz