// vim:ts=2:sw=2:et
// Erlang NIF registration: function table, load and lifecycle, info and
// integer helpers. Entry points are implemented in nif_*.cpp.
#include <erl_nif.h>

#include "glazer_nif_api.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_common.hpp"
#include "glazer_regex.hpp"

namespace glz {

// Schema resource type (initialized in nif_load)
ErlNifResourceType* SCHEMA_RESOURCE_TYPE = nullptr;

// NIF: info
//-----------------------------------------------------------------------------

#ifndef GLAZER_VERSION
#define GLAZER_VERSION "unknown"
#endif
#ifndef GLAZER_APP_VERSION
#define GLAZER_APP_VERSION "unknown"
#endif
#ifndef GLAZER_OPT_LEVEL
#define GLAZER_OPT_LEVEL "none"
#endif
#ifndef GLAZER_PGO
#define GLAZER_PGO 0
#endif

namespace {
  #if (GLAZER_PGO == 1)
  extern "C" __attribute__((visibility("default")))
  const char g_pgo_signature[] = "GLAZER_PGO_OPTIMIZED=1";
  #endif
  extern "C" __attribute__((visibility("default")))
  const char g_opt_level[]     = "GLAZER_OPT_LEVEL="   GLAZER_OPT_LEVEL;
  extern "C" __attribute__((visibility("default")))
  const char g_version[]       = "GLAZER_VERSION="     GLAZER_VERSION;
  extern "C" __attribute__((visibility("default")))
  const char g_app_version[]   = "GLAZER_APP_VERSION=" GLAZER_APP_VERSION;
}

ERL_NIF_TERM nif_info(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 0) [[unlikely]]
    return enif_make_badarg(env);

  // "none", "O1", "O2" or "O3", as a binary
  auto opt = [=]() { return make_binary(env, std::string_view(GLAZER_OPT_LEVEL)); };

  ERL_NIF_TERM keys[]   = { AM_VERSION, AM_APP_VERSION, AM_PGO, AM_OPTIMIZATION,
                            AM_REGEX_ENGINE };
  ERL_NIF_TERM values[] = {
    make_binary(env, std::string_view(GLAZER_VERSION)),
    make_binary(env, std::string_view(GLAZER_APP_VERSION)),
    GLAZER_PGO ? AM_TRUE : AM_FALSE,
    opt(),
    strcmp(SchemaRegex::engine(), "pcre2") == 0 ? AM_PCRE2 : AM_SRELL
  };

  ERL_NIF_TERM map;
  enif_make_map_from_arrays(env, keys, values, 5, &map);
  return map;
}

//-----------------------------------------------------------------------------
// NIF: encode_integer / try_decode_integer
//-----------------------------------------------------------------------------

ERL_NIF_TERM nif_encode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  glz::BigInt::StringOut out;
  if (glz::BigInt::encode(env, argv[0], out)) [[likely]]
    return make_binary(env, out.str);
  return enif_make_badarg(env);
}

ERL_NIF_TERM nif_try_decode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]] return enif_make_tuple2(env, AM_ERROR, AM_BADARG);
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_tuple2(env, AM_ERROR, AM_BADARG);
  auto r = glz::BigInt::decode(env,
    reinterpret_cast<const char*>(bin.data),
    reinterpret_cast<const char*>(bin.data) + bin.size);
  if (r) [[likely]]
    return enif_make_tuple2(env, AM_OK, r);
  return enif_make_tuple2(env, AM_ERROR, AM_INVALID_NUMBER_FORMAT);
}

//-----------------------------------------------------------------------------
// NIF lifecycle
//-----------------------------------------------------------------------------

static int nif_load(ErlNifEnv* env, void** /*priv_data*/, ERL_NIF_TERM load_info)
{
  init_atoms(env);

  if (!verify_fast_term_type(env)) {
    fprintf(stderr, "glazer: fast_term_type tag assumptions invalid on this OTP version\n");
    return -1;
  }

  ERL_NIF_TERM head, tail = load_info;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    int arity; const ERL_NIF_TERM* tp;
    if (enif_get_tuple(env, head, &arity, &tp) && arity == 2)
      if (enif_is_identical(tp[0], AM_NULL) && enif_is_atom(env, tp[1]))
        am_null = tp[1];
  }

  // Initialize schema resource type
  ErlNifResourceType* res_type = enif_open_resource_type(
    env,
    nullptr,
    "schema_resource",
    schema_resource_cleanup,
    ERL_NIF_RT_CREATE,
    nullptr
  );

  if (res_type) {
    SCHEMA_RESOURCE_TYPE = res_type;
  }
  // Note: Continue even if resource type creation fails;
  // schema validation features will still be available via NIFs

  return 0;
}

static ErlNifFunc nif_funcs[] = {
  // JSON decoding/encoding
  {"json_try_decode",    1, nif_json_try_decode,    0},
  {"json_try_decode",    2, nif_json_try_decode,    0},
  {"yaml_try_decode",    1, nif_yaml_try_decode,    0},
  {"yaml_try_decode",    2, nif_yaml_try_decode,    0},
  {"csv_try_decode",     1, nif_csv_try_decode,     0},
  {"csv_try_decode",     2, nif_csv_try_decode,     0},
  {"json_scan",          2, nif_json_scan,          0},
  {"json_encode",        1, nif_json_encode,        0},
  {"json_encode",        2, nif_json_encode,        0},
  {"json_try_encode",    2, nif_json_try_encode,    0},
  {"json_encode_ndjson", 1, nif_json_encode_ndjson, 0},
  {"json_encode_ndjson", 2, nif_json_encode_ndjson, 0},
  {"yaml_encode",        1, nif_yaml_encode,        0},
  {"yaml_encode",        2, nif_yaml_encode,        0},
  {"csv_encode",         1, nif_csv_encode,         0},
  {"csv_encode",         2, nif_csv_encode,         0},
  {"json_minify",        1, nif_json_minify,        0},
  {"json_prettify",      1, nif_json_prettify,      0},
  {"json_query",         2, nif_json_query,         0},
  {"json_query",         3, nif_json_query,         0},
  {"encode_integer",     1, nif_encode_integer,     0},
  {"try_decode_integer", 1, nif_try_decode_integer, 0},
  {"nif_compile_path",   1, nif_compile_path,       0},
  {"nif_info",           0, nif_info,               0},

  // Schema compilation and validation (JSON Schema documents, any term as data)
  {"compile_schema", 1, nif_compile_schema, 0},
  {"compile_schema", 2, nif_compile_schema, 0},
  {"schema_validate",       2, nif_validate,       0},
  {"schema_validate",       3, nif_validate,       0},
  {"schema_validate_term",  2, nif_validate_term,  0},
  {"schema_validate_term",  3, nif_validate_term,  0},
};

} // namespace glz

ERL_NIF_INIT(glazer, glz::nif_funcs, glz::nif_load, NULL, NULL, NULL)
