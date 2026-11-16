// vim:ts=2:sw=2:et
#pragma once

#include <erl_nif.h>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>

namespace glz {

extern ERL_NIF_TERM AM_ENOMEM;
extern ERL_NIF_TERM AM_ERROR;
extern ERL_NIF_TERM AM_FALSE;
extern ERL_NIF_TERM AM_NIL;
extern ERL_NIF_TERM AM_UNDEFINED;
extern ERL_NIF_TERM AM_NULL;
extern ERL_NIF_TERM AM_OK;
extern ERL_NIF_TERM AM_TRUE;

// Decode option atoms
extern ERL_NIF_TERM AM_ATOM;
extern ERL_NIF_TERM AM_COPY_STRINGS;
extern ERL_NIF_TERM AM_DEDUPE_KEYS;
extern ERL_NIF_TERM AM_EXISTING_ATOM;
extern ERL_NIF_TERM AM_HAS_TRAILER;
extern ERL_NIF_TERM AM_KEYS;
extern ERL_NIF_TERM AM_LABEL_BINARY;
extern ERL_NIF_TERM AM_NULL_TERM;
extern ERL_NIF_TERM AM_OBJECT_AS_TUPLE;
extern ERL_NIF_TERM AM_RETURN_TRAILER;
extern ERL_NIF_TERM AM_USE_NIL;
extern ERL_NIF_TERM AM_VALIDATE_UTF8;
extern ERL_NIF_TERM AM_SKIP_UTF8_VALIDATION;

// Encode option atoms
extern ERL_NIF_TERM AM_PRETTY;
extern ERL_NIF_TERM AM_UESCAPE;
extern ERL_NIF_TERM AM_FORCE_UTF8;
extern ERL_NIF_TERM AM_ESCAPE_FWD_SLASH;

// YAML option atoms
extern ERL_NIF_TERM AM_YAML_1_1_BOOLS;

// CSV option atoms
extern ERL_NIF_TERM AM_BADARG;
extern ERL_NIF_TERM AM_BINARY;
extern ERL_NIF_TERM AM_BOOLEAN;
extern ERL_NIF_TERM AM_CHARLIST;
extern ERL_NIF_TERM AM_CRLF;
extern ERL_NIF_TERM AM_DATA;
extern ERL_NIF_TERM AM_DATETIME;
extern ERL_NIF_TERM AM_DEFAULT;
extern ERL_NIF_TERM AM_DELIMITER;
extern ERL_NIF_TERM AM_FIELDS;
extern ERL_NIF_TERM AM_FLOAT;
extern ERL_NIF_TERM AM_HEADERS;
extern ERL_NIF_TERM AM_INTEGER;
extern ERL_NIF_TERM AM_LF;
extern ERL_NIF_TERM AM_LIMIT;
extern ERL_NIF_TERM AM_LINE_ENDING;
extern ERL_NIF_TERM AM_LIST;
extern ERL_NIF_TERM AM_MAP;
extern ERL_NIF_TERM AM_ON_FAILURE;
extern ERL_NIF_TERM AM_RAISE;
extern ERL_NIF_TERM AM_RETURN;
extern ERL_NIF_TERM AM_SKIP;
extern ERL_NIF_TERM AM_STRING;
extern ERL_NIF_TERM AM_TUPLE;
extern ERL_NIF_TERM AM_TYPE;

// YAML special-float atoms (Erlang floats can't represent inf/nan)
extern ERL_NIF_TERM AM_INFINITY;
extern ERL_NIF_TERM AM_NAN;
extern ERL_NIF_TERM AM_NEG_INFINITY;

// Error atoms
extern ERL_NIF_TERM AM_ENCODE_ERROR;
extern ERL_NIF_TERM AM_INVALID_INPUT;
extern ERL_NIF_TERM AM_INVALID_NUMBER_FORMAT;

// scan/2 result atoms
extern ERL_NIF_TERM AM_COMPLETE;
extern ERL_NIF_TERM AM_INCOMPLETE;

// CSV error atoms
extern ERL_NIF_TERM AM_DUPLICATE_HEADER;
extern ERL_NIF_TERM AM_INVALID_FIELD_VALUE;
extern ERL_NIF_TERM AM_UNTERMINATED_QUOTED_FIELD;

// jq error atoms
extern ERL_NIF_TERM AM_JQ_COMPILE_ERROR;
extern ERL_NIF_TERM AM_JQ_DECODE_ERROR;
extern ERL_NIF_TERM AM_JQ_NOT_AVAILABLE;

// compile_path/1 path_step() atoms and error atoms
extern ERL_NIF_TERM AM_FIELD;
extern ERL_NIF_TERM AM_INDEX;
extern ERL_NIF_TERM AM_INVALID_PATH;
extern ERL_NIF_TERM AM_ITERATE;

// info/0 result atoms
extern ERL_NIF_TERM AM_APP_VERSION;
extern ERL_NIF_TERM AM_OPTIMIZATION;
extern ERL_NIF_TERM AM_PGO;
extern ERL_NIF_TERM AM_VERSION;

// json_schema
extern ERL_NIF_TERM AM_FAIL_FAST;
extern ERL_NIF_TERM AM_COLLECT_ALL_ERRORS;
extern ERL_NIF_TERM AM_MAX_DEPTH;
extern ERL_NIF_TERM AM_MAX_ERRORS;
extern ERL_NIF_TERM AM_TIMEOUT_MS;
extern ERL_NIF_TERM AM_THEN;
extern ERL_NIF_TERM AM_ELSE;
extern ERL_NIF_TERM AM_PREFIX_ITEMS;
extern ERL_NIF_TERM AM_PROPERTIES;
extern ERL_NIF_TERM AM_PATTERN_PROPERTIES;
extern ERL_NIF_TERM AM_MIN_CONTAINS;
extern ERL_NIF_TERM AM_MAX_CONTAINS;
extern ERL_NIF_TERM AM_REFS;
extern ERL_NIF_TERM AM_FORMAT_ASSERTION;
extern ERL_NIF_TERM AM_FORMATS;
extern ERL_NIF_TERM AM_SCHEMA;
extern ERL_NIF_TERM AM_PATH;
extern ERL_NIF_TERM AM_UNKNOWN;
extern ERL_NIF_TERM AM_TYPE_MISMATCH;
extern ERL_NIF_TERM AM_VALUE_TOO_SMALL;
extern ERL_NIF_TERM AM_VALUE_TOO_LARGE;
extern ERL_NIF_TERM AM_NOT_MULTIPLE_OF;
extern ERL_NIF_TERM AM_STRING_TOO_SHORT;
extern ERL_NIF_TERM AM_STRING_TOO_LONG;
extern ERL_NIF_TERM AM_PATTERN_MISMATCH;
extern ERL_NIF_TERM AM_MISSING_REQUIRED;
extern ERL_NIF_TERM AM_EXTRA_PROPERTIES;
extern ERL_NIF_TERM AM_TOO_FEW_ITEMS;
extern ERL_NIF_TERM AM_TOO_MANY_ITEMS;
extern ERL_NIF_TERM AM_DUPLICATE_ITEM;
extern ERL_NIF_TERM AM_TOO_FEW_PROPERTIES;
extern ERL_NIF_TERM AM_TOO_MANY_PROPERTIES;
extern ERL_NIF_TERM AM_SCHEMA_MATCHED;
extern ERL_NIF_TERM AM_NO_SCHEMA_MATCHED;
extern ERL_NIF_TERM AM_MULTIPLE_SCHEMAS_MATCHED;
extern ERL_NIF_TERM AM_ALL_SCHEMAS_FAILED;
extern ERL_NIF_TERM AM_ITEMS_VALIDATION_FAILED;
extern ERL_NIF_TERM AM_UNRESOLVED_REF;
extern ERL_NIF_TERM AM_CIRCULAR_REFERENCE;
extern ERL_NIF_TERM AM_THEN_VALIDATION_FAILED;
extern ERL_NIF_TERM AM_ELSE_VALIDATION_FAILED;
extern ERL_NIF_TERM AM_FORMAT_INVALID;
extern ERL_NIF_TERM AM_DEPENDENT_KEYS_MISSING;
extern ERL_NIF_TERM AM_DEPENDENT_SCHEMA_FAILED;
extern ERL_NIF_TERM AM_EXPECTED_STRING;
extern ERL_NIF_TERM AM_INVALID_TERM_TYPE;
extern ERL_NIF_TERM AM_MAX_DEPTH_EXCEEDED;
extern ERL_NIF_TERM AM_NOT_CONST;
extern ERL_NIF_TERM AM_NOT_IN_ENUM;
extern ERL_NIF_TERM AM_CONTAINS_FAILED;
extern ERL_NIF_TERM AM_TIMEOUT;
extern ERL_NIF_TERM AM_REGEX_ERROR;

// info/0 atoms
extern ERL_NIF_TERM AM_REGEX_ENGINE;
extern ERL_NIF_TERM AM_PCRE2;
extern ERL_NIF_TERM AM_SRELL;

// The runtime null value (configurable via NIF load)
extern ERL_NIF_TERM am_null;

struct DeadProcError : public std::exception {};

void init_atoms(ErlNifEnv* env);

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
