// vim:ts=2:sw=2:et
#pragma once

#include <erl_nif.h>
#include <tuple>
#include <cstring>

namespace glz {

static ERL_NIF_TERM AM_ENOMEM;
static ERL_NIF_TERM AM_ERROR;
static ERL_NIF_TERM AM_FALSE;
static ERL_NIF_TERM AM_NIL;
static ERL_NIF_TERM AM_UNDEFINED;
static ERL_NIF_TERM AM_NULL;
static ERL_NIF_TERM AM_OK;
static ERL_NIF_TERM AM_TRUE;

// Decode option atoms
static ERL_NIF_TERM AM_ATOM;
static ERL_NIF_TERM AM_COPY_STRINGS;
static ERL_NIF_TERM AM_DEDUPE_KEYS;
static ERL_NIF_TERM AM_EXISTING_ATOM;
static ERL_NIF_TERM AM_HAS_TRAILER;
static ERL_NIF_TERM AM_KEYS;
static ERL_NIF_TERM AM_LABEL_BINARY;
static ERL_NIF_TERM AM_NULL_TERM;
static ERL_NIF_TERM AM_OBJECT_AS_TUPLE;
static ERL_NIF_TERM AM_RETURN_TRAILER;
static ERL_NIF_TERM AM_USE_NIL;
static ERL_NIF_TERM AM_VALIDATE_UTF8;
static ERL_NIF_TERM AM_SKIP_UTF8_VALIDATION;

// Encode option atoms
static ERL_NIF_TERM AM_PRETTY;
static ERL_NIF_TERM AM_UESCAPE;
static ERL_NIF_TERM AM_FORCE_UTF8;
static ERL_NIF_TERM AM_ESCAPE_FWD_SLASH;

// YAML option atoms
static ERL_NIF_TERM AM_YAML_1_1_BOOLS;

// CSV option atoms
static ERL_NIF_TERM AM_BADARG;
static ERL_NIF_TERM AM_BINARY;
static ERL_NIF_TERM AM_BOOLEAN;
static ERL_NIF_TERM AM_CHARLIST;
static ERL_NIF_TERM AM_CRLF;
static ERL_NIF_TERM AM_DATA;
static ERL_NIF_TERM AM_DATETIME;
static ERL_NIF_TERM AM_DEFAULT;
static ERL_NIF_TERM AM_DELIMITER;
static ERL_NIF_TERM AM_FIELDS;
static ERL_NIF_TERM AM_FLOAT;
static ERL_NIF_TERM AM_HEADERS;
static ERL_NIF_TERM AM_INTEGER;
static ERL_NIF_TERM AM_LF;
static ERL_NIF_TERM AM_LIMIT;
static ERL_NIF_TERM AM_LINE_ENDING;
static ERL_NIF_TERM AM_LIST;
static ERL_NIF_TERM AM_MAP;
static ERL_NIF_TERM AM_ON_FAILURE;
static ERL_NIF_TERM AM_RAISE;
static ERL_NIF_TERM AM_RETURN;
static ERL_NIF_TERM AM_SKIP;
static ERL_NIF_TERM AM_STRING;
static ERL_NIF_TERM AM_TUPLE;
static ERL_NIF_TERM AM_TYPE;

// YAML special-float atoms (Erlang floats can't represent inf/nan)
static ERL_NIF_TERM AM_INFINITY;
static ERL_NIF_TERM AM_NAN;
static ERL_NIF_TERM AM_NEG_INFINITY;

// Error atoms
static ERL_NIF_TERM AM_ENCODE_ERROR;
static ERL_NIF_TERM AM_INVALID_INPUT;
static ERL_NIF_TERM AM_INVALID_NUMBER_FORMAT;

// scan/2 result atoms
static ERL_NIF_TERM AM_COMPLETE;
static ERL_NIF_TERM AM_INCOMPLETE;

// CSV error atoms
static ERL_NIF_TERM AM_DUPLICATE_HEADER;
static ERL_NIF_TERM AM_INVALID_FIELD_VALUE;
static ERL_NIF_TERM AM_UNTERMINATED_QUOTED_FIELD;

// jq error atoms
static ERL_NIF_TERM AM_JQ_COMPILE_ERROR;
static ERL_NIF_TERM AM_JQ_DECODE_ERROR;
static ERL_NIF_TERM AM_JQ_NOT_AVAILABLE;

// compile_path/1 path_step() atoms and error atoms
static ERL_NIF_TERM AM_FIELD;
static ERL_NIF_TERM AM_INDEX;
static ERL_NIF_TERM AM_INVALID_PATH;
static ERL_NIF_TERM AM_ITERATE;

// info/0 result atoms
static ERL_NIF_TERM AM_APP_VERSION;
static ERL_NIF_TERM AM_OPTIMIZATION;
static ERL_NIF_TERM AM_PGO;
static ERL_NIF_TERM AM_VERSION;

// json_schema
static ERL_NIF_TERM AM_FAIL_FAST;
static ERL_NIF_TERM AM_COLLECT_ALL_ERRORS;
static ERL_NIF_TERM AM_MAX_DEPTH;
static ERL_NIF_TERM AM_MAX_ERRORS;
static ERL_NIF_TERM AM_TIMEOUT_MS;
static ERL_NIF_TERM AM_THEN;
static ERL_NIF_TERM AM_ELSE;
static ERL_NIF_TERM AM_PREFIX_ITEMS;
static ERL_NIF_TERM AM_PROPERTIES;
static ERL_NIF_TERM AM_PATTERN_PROPERTIES;
static ERL_NIF_TERM AM_MIN_CONTAINS;
static ERL_NIF_TERM AM_MAX_CONTAINS;
static ERL_NIF_TERM AM_REFS;
static ERL_NIF_TERM AM_FORMAT_ASSERTION;
static ERL_NIF_TERM AM_FORMATS;
static ERL_NIF_TERM AM_SCHEMA;
static ERL_NIF_TERM AM_PATH;
static ERL_NIF_TERM AM_UNKNOWN;
static ERL_NIF_TERM AM_TYPE_MISMATCH;
static ERL_NIF_TERM AM_VALUE_TOO_SMALL;
static ERL_NIF_TERM AM_VALUE_TOO_LARGE;
static ERL_NIF_TERM AM_NOT_MULTIPLE_OF;
static ERL_NIF_TERM AM_STRING_TOO_SHORT;
static ERL_NIF_TERM AM_STRING_TOO_LONG;
static ERL_NIF_TERM AM_PATTERN_MISMATCH;
static ERL_NIF_TERM AM_MISSING_REQUIRED;
static ERL_NIF_TERM AM_EXTRA_PROPERTIES;
static ERL_NIF_TERM AM_TOO_FEW_ITEMS;
static ERL_NIF_TERM AM_TOO_MANY_ITEMS;
static ERL_NIF_TERM AM_DUPLICATE_ITEM;
static ERL_NIF_TERM AM_TOO_FEW_PROPERTIES;
static ERL_NIF_TERM AM_TOO_MANY_PROPERTIES;
static ERL_NIF_TERM AM_SCHEMA_MATCHED;
static ERL_NIF_TERM AM_NO_SCHEMA_MATCHED;
static ERL_NIF_TERM AM_MULTIPLE_SCHEMAS_MATCHED;
static ERL_NIF_TERM AM_ALL_SCHEMAS_FAILED;
static ERL_NIF_TERM AM_ITEMS_VALIDATION_FAILED;
static ERL_NIF_TERM AM_UNRESOLVED_REF;
static ERL_NIF_TERM AM_CIRCULAR_REFERENCE;
static ERL_NIF_TERM AM_THEN_VALIDATION_FAILED;
static ERL_NIF_TERM AM_ELSE_VALIDATION_FAILED;
static ERL_NIF_TERM AM_FORMAT_INVALID;
static ERL_NIF_TERM AM_DEPENDENT_KEYS_MISSING;
static ERL_NIF_TERM AM_DEPENDENT_SCHEMA_FAILED;
static ERL_NIF_TERM AM_EXPECTED_STRING;
static ERL_NIF_TERM AM_INVALID_TERM_TYPE;
static ERL_NIF_TERM AM_MAX_DEPTH_EXCEEDED;
static ERL_NIF_TERM AM_NOT_CONST;
static ERL_NIF_TERM AM_NOT_IN_ENUM;
static ERL_NIF_TERM AM_CONTAINS_FAILED;
static ERL_NIF_TERM AM_TIMEOUT;
static ERL_NIF_TERM AM_REGEX_ERROR;

// info/0 atoms
static ERL_NIF_TERM AM_REGEX_ENGINE;
static ERL_NIF_TERM AM_PCRE2;
static ERL_NIF_TERM AM_SRELL;

// The runtime null value (configurable via NIF load)
static ERL_NIF_TERM am_null;

struct DeadProcError : public std::exception {};

inline void init_atoms(ErlNifEnv* env)
{
  AM_APP_VERSION               = enif_make_atom(env, "app_version");
  AM_ATOM                      = enif_make_atom(env, "atom");
  AM_BADARG                    = enif_make_atom(env, "badarg");
  AM_BINARY                    = enif_make_atom(env, "binary");
  AM_BOOLEAN                   = enif_make_atom(env, "boolean");
  AM_CHARLIST                  = enif_make_atom(env, "charlist");
  AM_COMPLETE                  = enif_make_atom(env, "complete");
  AM_COPY_STRINGS              = enif_make_atom(env, "copy_strings");
  AM_CRLF                      = enif_make_atom(env, "crlf");
  AM_DATA                      = enif_make_atom(env, "data");
  AM_DATETIME                  = enif_make_atom(env, "datetime");
  AM_DEDUPE_KEYS               = enif_make_atom(env, "dedupe_keys");
  AM_DEFAULT                   = enif_make_atom(env, "default");
  AM_DELIMITER                 = enif_make_atom(env, "delimiter");
  AM_DUPLICATE_HEADER          = enif_make_atom(env, "duplicate_header");
  AM_ENCODE_ERROR              = enif_make_atom(env, "encode_error");
  AM_ENOMEM                    = enif_make_atom(env, "enomem");
  AM_ERROR                     = enif_make_atom(env, "error");
  AM_ESCAPE_FWD_SLASH          = enif_make_atom(env, "escape_fwd_slash");
  AM_EXISTING_ATOM             = enif_make_atom(env, "existing_atom");
  AM_FALSE                     = enif_make_atom(env, "false");
  AM_FIELD                     = enif_make_atom(env, "field");
  AM_FIELDS                    = enif_make_atom(env, "fields");
  AM_FLOAT                     = enif_make_atom(env, "float");
  AM_FORCE_UTF8                = enif_make_atom(env, "force_utf8");
  AM_HAS_TRAILER               = enif_make_atom(env, "has_trailer");
  AM_HEADERS                   = enif_make_atom(env, "headers");
  AM_INCOMPLETE                = enif_make_atom(env, "incomplete");
  AM_INDEX                     = enif_make_atom(env, "index");
  AM_INFINITY                  = enif_make_atom(env, "infinity");
  AM_INTEGER                   = enif_make_atom(env, "integer");
  AM_INVALID_FIELD_VALUE       = enif_make_atom(env, "invalid_field_value");
  AM_INVALID_INPUT             = enif_make_atom(env, "invalid_input");
  AM_INVALID_NUMBER_FORMAT     = enif_make_atom(env, "invalid_number_format");
  AM_INVALID_PATH              = enif_make_atom(env, "invalid_path");
  AM_ITERATE                   = enif_make_atom(env, "iterate");
  AM_JQ_COMPILE_ERROR          = enif_make_atom(env, "jq_compile_error");
  AM_JQ_DECODE_ERROR           = enif_make_atom(env, "jq_decode_error");
  AM_JQ_NOT_AVAILABLE          = enif_make_atom(env, "jq_not_available");
  AM_KEYS                      = enif_make_atom(env, "keys");
  AM_LABEL_BINARY              = enif_make_atom(env, "binary");
  AM_LF                        = enif_make_atom(env, "lf");
  AM_LIMIT                     = enif_make_atom(env, "limit");
  AM_LINE_ENDING               = enif_make_atom(env, "line_ending");
  AM_LIST                      = enif_make_atom(env, "list");
  AM_MAP                       = enif_make_atom(env, "map");
  AM_NAN                       = enif_make_atom(env, "nan");
  AM_NEG_INFINITY              = enif_make_atom(env, "neg_infinity");
  AM_NIL                       = enif_make_atom(env, "nil");
  AM_NULL                      = enif_make_atom(env, "null");
  AM_NULL_TERM                 = enif_make_atom(env, "null_term");
  AM_OBJECT_AS_TUPLE           = enif_make_atom(env, "object_as_tuple");
  AM_OK                        = enif_make_atom(env, "ok");
  AM_ON_FAILURE                = enif_make_atom(env, "on_failure");
  AM_OPTIMIZATION              = enif_make_atom(env, "optimization");
  AM_PGO                       = enif_make_atom(env, "pgo");
  AM_PRETTY                    = enif_make_atom(env, "pretty");
  AM_RAISE                     = enif_make_atom(env, "raise");
  AM_RETURN                    = enif_make_atom(env, "return");
  AM_RETURN_TRAILER            = enif_make_atom(env, "return_trailer");
  AM_SKIP                      = enif_make_atom(env, "skip");
  AM_SKIP_UTF8_VALIDATION      = enif_make_atom(env, "skip_utf8_validation");
  AM_STRING                    = enif_make_atom(env, "string");
  AM_TRUE                      = enif_make_atom(env, "true");
  AM_TUPLE                     = enif_make_atom(env, "tuple");
  AM_TYPE                      = enif_make_atom(env, "type");
  AM_UESCAPE                   = enif_make_atom(env, "uescape");
  AM_UNDEFINED                 = enif_make_atom(env, "undefined");
  AM_UNTERMINATED_QUOTED_FIELD = enif_make_atom(env, "unterminated_quoted_field");
  AM_USE_NIL                   = enif_make_atom(env, "use_nil");
  AM_VALIDATE_UTF8             = enif_make_atom(env, "validate_utf8");
  AM_VERSION                   = enif_make_atom(env, "version");
  AM_YAML_1_1_BOOLS            = enif_make_atom(env, "yaml_1_1_bools");
  AM_FAIL_FAST                 = enif_make_atom(env, "fail_fast");
  AM_COLLECT_ALL_ERRORS        = enif_make_atom(env, "collect_all_errors");
  AM_MAX_DEPTH                 = enif_make_atom(env, "max_depth");
  AM_MAX_ERRORS                = enif_make_atom(env, "max_errors");
  AM_TIMEOUT_MS                = enif_make_atom(env, "timeout_ms");
  AM_THEN                      = enif_make_atom(env, "then");
  AM_ELSE                      = enif_make_atom(env, "else");
  AM_PREFIX_ITEMS              = enif_make_atom(env, "prefixItems");
  AM_PROPERTIES                = enif_make_atom(env, "properties");
  AM_PATTERN_PROPERTIES        = enif_make_atom(env, "patternProperties");
  AM_MIN_CONTAINS              = enif_make_atom(env, "minContains");
  AM_MAX_CONTAINS              = enif_make_atom(env, "maxContains");
  AM_REFS                      = enif_make_atom(env, "refs");
  AM_FORMAT_ASSERTION          = enif_make_atom(env, "format_assertion");
  AM_FORMATS                   = enif_make_atom(env, "formats");
  AM_SCHEMA                    = enif_make_atom(env, "schema");
  AM_PATH                      = enif_make_atom(env, "path");
  AM_UNKNOWN                   = enif_make_atom(env, "unknown");
  AM_TYPE_MISMATCH             = enif_make_atom(env, "type_mismatch");
  AM_VALUE_TOO_SMALL           = enif_make_atom(env, "value_too_small");
  AM_VALUE_TOO_LARGE           = enif_make_atom(env, "value_too_large");
  AM_NOT_MULTIPLE_OF           = enif_make_atom(env, "not_multiple_of");
  AM_STRING_TOO_SHORT          = enif_make_atom(env, "string_too_short");
  AM_STRING_TOO_LONG           = enif_make_atom(env, "string_too_long");
  AM_PATTERN_MISMATCH          = enif_make_atom(env, "pattern_mismatch");
  AM_MISSING_REQUIRED          = enif_make_atom(env, "missing_required");
  AM_EXTRA_PROPERTIES          = enif_make_atom(env, "extra_properties");
  AM_TOO_FEW_ITEMS             = enif_make_atom(env, "too_few_items");
  AM_TOO_MANY_ITEMS            = enif_make_atom(env, "too_many_items");
  AM_DUPLICATE_ITEM            = enif_make_atom(env, "duplicate_item");
  AM_TOO_FEW_PROPERTIES        = enif_make_atom(env, "too_few_properties");
  AM_TOO_MANY_PROPERTIES       = enif_make_atom(env, "too_many_properties");
  AM_SCHEMA_MATCHED            = enif_make_atom(env, "schema_matched");
  AM_NO_SCHEMA_MATCHED         = enif_make_atom(env, "no_schema_matched");
  AM_MULTIPLE_SCHEMAS_MATCHED  = enif_make_atom(env, "multiple_schemas_matched");
  AM_ALL_SCHEMAS_FAILED        = enif_make_atom(env, "all_schemas_failed");
  AM_ITEMS_VALIDATION_FAILED   = enif_make_atom(env, "items_validation_failed");
  AM_UNRESOLVED_REF            = enif_make_atom(env, "unresolved_ref");
  AM_CIRCULAR_REFERENCE        = enif_make_atom(env, "circular_reference");
  AM_THEN_VALIDATION_FAILED    = enif_make_atom(env, "then_validation_failed");
  AM_ELSE_VALIDATION_FAILED    = enif_make_atom(env, "else_validation_failed");
  AM_FORMAT_INVALID            = enif_make_atom(env, "format_invalid");
  AM_DEPENDENT_KEYS_MISSING    = enif_make_atom(env, "dependent_keys_missing");
  AM_DEPENDENT_SCHEMA_FAILED   = enif_make_atom(env, "dependent_schema_failed");
  AM_EXPECTED_STRING           = enif_make_atom(env, "expected_string");
  AM_INVALID_TERM_TYPE         = enif_make_atom(env, "invalid_term_type");
  AM_MAX_DEPTH_EXCEEDED        = enif_make_atom(env, "max_depth_exceeded");
  AM_NOT_CONST                 = enif_make_atom(env, "not_const");
  AM_NOT_IN_ENUM               = enif_make_atom(env, "not_in_enum");
  AM_TIMEOUT                   = enif_make_atom(env, "timeout");
  AM_REGEX_ERROR               = enif_make_atom(env, "regex_error");
  AM_REGEX_ENGINE              = enif_make_atom(env, "regex_engine");
  AM_PCRE2                     = enif_make_atom(env, "pcre2");
  AM_SRELL                     = enif_make_atom(env, "srell");
  AM_CONTAINS_FAILED           = enif_make_atom(env, "contains_failed");

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
