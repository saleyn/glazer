// vim:ts=2:sw=2:et
#pragma once

#include <erl_nif.h>

static ERL_NIF_TERM AM_OK;
static ERL_NIF_TERM AM_ERROR;
static ERL_NIF_TERM AM_TRUE;
static ERL_NIF_TERM AM_FALSE;
static ERL_NIF_TERM AM_NULL;
static ERL_NIF_TERM AM_NIL;
static ERL_NIF_TERM AM_BADARG;
static ERL_NIF_TERM AM_ENOMEM;

// Decode option atoms
static ERL_NIF_TERM AM_RETURN_MAPS;
static ERL_NIF_TERM AM_OBJECT_AS_TUPLE;
static ERL_NIF_TERM AM_USE_NIL;
static ERL_NIF_TERM AM_NULL_TERM;
static ERL_NIF_TERM AM_LABEL_ATOM;
static ERL_NIF_TERM AM_LABEL_EXISTING_ATOM;
static ERL_NIF_TERM AM_LABEL_BINARY;

// Encode option atoms
static ERL_NIF_TERM AM_PRETTY;
static ERL_NIF_TERM AM_UESCAPE;
static ERL_NIF_TERM AM_FORCE_UTF8;

// Error atoms
static ERL_NIF_TERM AM_PARSE_ERROR;
static ERL_NIF_TERM AM_ENCODE_ERROR;

// The runtime null value (configurable via NIF load)
static ERL_NIF_TERM am_null;

struct DeadProcError : public std::exception {};

inline void init_atoms(ErlNifEnv* env)
{
  AM_OK               = enif_make_atom(env, "ok");
  AM_ERROR            = enif_make_atom(env, "error");
  AM_TRUE             = enif_make_atom(env, "true");
  AM_FALSE            = enif_make_atom(env, "false");
  AM_NULL             = enif_make_atom(env, "null");
  AM_NIL              = enif_make_atom(env, "nil");
  AM_BADARG           = enif_make_atom(env, "badarg");
  AM_ENOMEM           = enif_make_atom(env, "enomem");

  AM_RETURN_MAPS          = enif_make_atom(env, "return_maps");
  AM_OBJECT_AS_TUPLE      = enif_make_atom(env, "object_as_tuple");
  AM_USE_NIL              = enif_make_atom(env, "use_nil");
  AM_NULL_TERM            = enif_make_atom(env, "null_term");
  AM_LABEL_ATOM           = enif_make_atom(env, "atom");
  AM_LABEL_EXISTING_ATOM  = enif_make_atom(env, "existing_atom");
  AM_LABEL_BINARY         = enif_make_atom(env, "binary");

  AM_PRETTY       = enif_make_atom(env, "pretty");
  AM_UESCAPE      = enif_make_atom(env, "uescape");
  AM_FORCE_UTF8   = enif_make_atom(env, "force_utf8");

  AM_PARSE_ERROR  = enif_make_atom(env, "parse_error");
  AM_ENCODE_ERROR = enif_make_atom(env, "encode_error");

  am_null = AM_NULL;
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
