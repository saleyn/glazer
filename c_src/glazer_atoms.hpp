// vim:ts=2:sw=2:et
#pragma once

#include <erl_nif.h>
#include <tuple>

namespace glz {

static ERL_NIF_TERM AM_OK;
static ERL_NIF_TERM AM_ERROR;
static ERL_NIF_TERM AM_TRUE;
static ERL_NIF_TERM AM_FALSE;
static ERL_NIF_TERM AM_NULL;
static ERL_NIF_TERM AM_NIL;
static ERL_NIF_TERM AM_ENOMEM;

// Decode option atoms
static ERL_NIF_TERM AM_OBJECT_AS_TUPLE;
static ERL_NIF_TERM AM_USE_NIL;
static ERL_NIF_TERM AM_NULL_TERM;
static ERL_NIF_TERM AM_KEYS;
static ERL_NIF_TERM AM_LABEL_ATOM;
static ERL_NIF_TERM AM_LABEL_EXISTING_ATOM;
static ERL_NIF_TERM AM_LABEL_BINARY;
static ERL_NIF_TERM AM_DEDUPE_KEYS;
static ERL_NIF_TERM AM_COPY_STRINGS;

// Encode option atoms
static ERL_NIF_TERM AM_PRETTY;
static ERL_NIF_TERM AM_UESCAPE;
static ERL_NIF_TERM AM_FORCE_UTF8;

// YAML option atoms
static ERL_NIF_TERM AM_YAML_1_1_BOOLS;

// CSV option atoms
static ERL_NIF_TERM AM_DELIMITER;
static ERL_NIF_TERM AM_HEADERS;
static ERL_NIF_TERM AM_DATA;
static ERL_NIF_TERM AM_RETURN;
static ERL_NIF_TERM AM_MAP;
static ERL_NIF_TERM AM_LIST;
static ERL_NIF_TERM AM_TUPLE;
static ERL_NIF_TERM AM_STRING;
static ERL_NIF_TERM AM_SKIP;
static ERL_NIF_TERM AM_LIMIT;
static ERL_NIF_TERM AM_LINE_ENDING;
static ERL_NIF_TERM AM_LF;
static ERL_NIF_TERM AM_CRLF;
static ERL_NIF_TERM AM_FIELDS;
static ERL_NIF_TERM AM_INTEGER;
static ERL_NIF_TERM AM_FLOAT;
static ERL_NIF_TERM AM_BOOLEAN;
static ERL_NIF_TERM AM_DATETIME;
static ERL_NIF_TERM AM_BINARY;
static ERL_NIF_TERM AM_CHARLIST;
static ERL_NIF_TERM AM_EXISTING_ATOM;
static ERL_NIF_TERM AM_ATOM;
static ERL_NIF_TERM AM_TYPE;
static ERL_NIF_TERM AM_DEFAULT;
static ERL_NIF_TERM AM_ON_FAILURE;
static ERL_NIF_TERM AM_RAISE;

// YAML special-float atoms (Erlang floats can't represent inf/nan)
static ERL_NIF_TERM AM_INFINITY;
static ERL_NIF_TERM AM_NEG_INFINITY;
static ERL_NIF_TERM AM_NAN;

// Error atoms
static ERL_NIF_TERM AM_ENCODE_ERROR;
static ERL_NIF_TERM AM_INVALID_NUMBER_FORMAT;
static ERL_NIF_TERM AM_INVALID_INPUT;

// scan/2 result atoms
static ERL_NIF_TERM AM_COMPLETE;
static ERL_NIF_TERM AM_INCOMPLETE;

// CSV error atoms
static ERL_NIF_TERM AM_DUPLICATE_HEADER;
static ERL_NIF_TERM AM_UNTERMINATED_QUOTED_FIELD;
static ERL_NIF_TERM AM_INVALID_FIELD_VALUE;

// jq error atoms
static ERL_NIF_TERM AM_JQ_NOT_AVAILABLE;
static ERL_NIF_TERM AM_JQ_COMPILE_ERROR;
static ERL_NIF_TERM AM_JQ_DECODE_ERROR;

// compile_path/1 path_step() atoms and error atoms
static ERL_NIF_TERM AM_FIELD;
static ERL_NIF_TERM AM_ITERATE;
static ERL_NIF_TERM AM_INDEX;
static ERL_NIF_TERM AM_INVALID_PATH;

// The runtime null value (configurable via NIF load)
static ERL_NIF_TERM am_null;

struct DeadProcError : public std::exception {};

inline void init_atoms(ErlNifEnv* env)
{
  AM_OK                        = enif_make_atom(env, "ok");
  AM_ERROR                     = enif_make_atom(env, "error");
  AM_TRUE                      = enif_make_atom(env, "true");
  AM_FALSE                     = enif_make_atom(env, "false");
  AM_NULL                      = enif_make_atom(env, "null");
  AM_NIL                       = enif_make_atom(env, "nil");
  AM_ENOMEM                    = enif_make_atom(env, "enomem");

  AM_OBJECT_AS_TUPLE           = enif_make_atom(env, "object_as_tuple");
  AM_USE_NIL                   = enif_make_atom(env, "use_nil");
  AM_NULL_TERM                 = enif_make_atom(env, "null_term");
  AM_KEYS                      = enif_make_atom(env, "keys");
  AM_LABEL_ATOM                = enif_make_atom(env, "atom");
  AM_LABEL_EXISTING_ATOM       = enif_make_atom(env, "existing_atom");
  AM_LABEL_BINARY              = enif_make_atom(env, "binary");
  AM_DEDUPE_KEYS               = enif_make_atom(env, "dedupe_keys");
  AM_COPY_STRINGS              = enif_make_atom(env, "copy_strings");

  AM_PRETTY                    = enif_make_atom(env, "pretty");
  AM_UESCAPE                   = enif_make_atom(env, "uescape");
  AM_FORCE_UTF8                = enif_make_atom(env, "force_utf8");

  AM_YAML_1_1_BOOLS            = enif_make_atom(env, "yaml_1_1_bools");

  AM_DELIMITER                 = enif_make_atom(env, "delimiter");
  AM_HEADERS                   = enif_make_atom(env, "headers");
  AM_DATA                      = enif_make_atom(env, "data");
  AM_RETURN                    = enif_make_atom(env, "return");
  AM_MAP                       = enif_make_atom(env, "map");
  AM_LIST                      = enif_make_atom(env, "list");
  AM_TUPLE                     = enif_make_atom(env, "tuple");
  AM_STRING                    = enif_make_atom(env, "string");
  AM_SKIP                      = enif_make_atom(env, "skip");
  AM_LIMIT                     = enif_make_atom(env, "limit");
  AM_LINE_ENDING               = enif_make_atom(env, "line_ending");
  AM_LF                        = enif_make_atom(env, "lf");
  AM_CRLF                      = enif_make_atom(env, "crlf");
  AM_FIELDS                    = enif_make_atom(env, "fields");
  AM_INTEGER                   = enif_make_atom(env, "integer");
  AM_FLOAT                     = enif_make_atom(env, "float");
  AM_BOOLEAN                   = enif_make_atom(env, "boolean");
  AM_DATETIME                  = enif_make_atom(env, "datetime");
  AM_BINARY                    = enif_make_atom(env, "binary");
  AM_CHARLIST                  = enif_make_atom(env, "charlist");
  AM_EXISTING_ATOM             = enif_make_atom(env, "existing_atom");
  AM_ATOM                      = enif_make_atom(env, "atom");
  AM_TYPE                      = enif_make_atom(env, "type");
  AM_DEFAULT                   = enif_make_atom(env, "default");
  AM_ON_FAILURE                = enif_make_atom(env, "on_failure");
  AM_RAISE                     = enif_make_atom(env, "raise");

  AM_INFINITY                  = enif_make_atom(env, "infinity");
  AM_NEG_INFINITY              = enif_make_atom(env, "neg_infinity");
  AM_NAN                       = enif_make_atom(env, "nan");

  AM_ENCODE_ERROR              = enif_make_atom(env, "encode_error");
  AM_INVALID_NUMBER_FORMAT     = enif_make_atom(env, "invalid_number_format");
  AM_INVALID_INPUT             = enif_make_atom(env, "invalid_input");

  AM_COMPLETE                  = enif_make_atom(env, "complete");
  AM_INCOMPLETE                = enif_make_atom(env, "incomplete");

  AM_DUPLICATE_HEADER          = enif_make_atom(env, "duplicate_header");
  AM_UNTERMINATED_QUOTED_FIELD = enif_make_atom(env, "unterminated_quoted_field");
  AM_INVALID_FIELD_VALUE       = enif_make_atom(env, "invalid_field_value");

  AM_JQ_NOT_AVAILABLE          = enif_make_atom(env, "jq_not_available");
  AM_JQ_COMPILE_ERROR          = enif_make_atom(env, "jq_compile_error");
  AM_JQ_DECODE_ERROR           = enif_make_atom(env, "jq_decode_error");

  AM_FIELD                     = enif_make_atom(env, "field");
  AM_ITERATE                   = enif_make_atom(env, "iterate");
  AM_INDEX                     = enif_make_atom(env, "index");
  AM_INVALID_PATH              = enif_make_atom(env, "invalid_path");

  am_null                      = AM_NULL;
}

inline std::tuple<ERL_NIF_TERM, unsigned char*>
make_binary(ErlNifEnv* env, size_t size)
{
  ERL_NIF_TERM term;
  auto p = enif_make_new_binary(env, size, &term);
  return std::make_tuple(term, p);
}

inline ERL_NIF_TERM make_binary(ErlNifEnv* env, std::string_view sv)
{
  auto [term, p] = make_binary(env, sv.size());
  memcpy(p, sv.data(), sv.size());
  return term;
}

inline ERL_NIF_TERM make_binary(ErlNifEnv* env, const std::string& s)
{
  return make_binary(env, std::string_view(s));
}

inline ERL_NIF_TERM make_tuple(ErlNifEnv* env, std::tuple<bool, ERL_NIF_TERM> result)
{
  auto [success, term] = result;
  return enif_make_tuple2(env, success ? AM_OK : AM_ERROR, term);
}

} // namespace glz
