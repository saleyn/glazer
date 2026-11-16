// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Entry points shared between the NIF translation units. Each nif_*.cpp file
// defines the functions it owns; glazer_nif.cpp builds the function table and
// nif_load() from these declarations. Keep the list in sync with nif_funcs.
//-----------------------------------------------------------------------------
#pragma once

#include <erl_nif.h>

namespace glz {

// nif_json.cpp
ERL_NIF_TERM nif_json_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_json_scan(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_json_try_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_json_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_json_encode_ndjson(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_json_minify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_json_prettify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

// nif_yaml.cpp
ERL_NIF_TERM nif_yaml_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_yaml_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

// nif_csv.cpp
ERL_NIF_TERM nif_csv_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_csv_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

// nif_jq.cpp
ERL_NIF_TERM nif_json_query(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_compile_path(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

// glazer_nif.cpp
ERL_NIF_TERM nif_info(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_encode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_try_decode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

// nif_schema.cpp (defined in glazer_json_schema_nif.hxx)
ERL_NIF_TERM nif_compile_schema(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_validate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
ERL_NIF_TERM nif_validate_term(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
void schema_resource_cleanup(ErlNifEnv* env, void* obj);

} // namespace glz
