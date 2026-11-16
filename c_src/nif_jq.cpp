// vim:ts=2:sw=2:et
// jq NIFs: json_query and compile_path (optional, requires libjq).
#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "glazer_nif_api.hpp"
#include "glazer_atoms.hpp"
#include "glazer_json.hpp"
#include "glazer_jq.hpp"

namespace glz {
// NIF: json_query (jq filter, optional — requires libjq at build time)
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_query(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary input, filter;
  if (!enif_inspect_iolist_as_binary(env, argv[0], &input) ||
      !enif_inspect_iolist_as_binary(env, argv[1], &filter)) [[unlikely]]
    return enif_make_badarg(env);

  JSONDecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 3 && (!enif_is_list(env, argv[2]) || !parse_decode_opts(env, argv[2], opts))) [[unlikely]]
    return enif_make_badarg(env);

  return jq_query(env, input, filter, opts);
}

static ERL_NIF_TERM nif_json_query_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_json_query(env, argc, argv);
}

ERL_NIF_TERM nif_json_query(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 2 || argc > 3) [[unlikely]]
    return enif_make_badarg(env);

  ERL_NIF_TERM sched_argv[3];
  sched_argv[0] = argv[0];
  sched_argv[1] = argv[1];
  sched_argv[2] = argc > 2 ? argv[2] : enif_make_list(env, 0);
  return enif_schedule_nif(env, "glazer_json_query", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_json_query_dirty, 3, sched_argv);
}

//-----------------------------------------------------------------------------
// NIF: compile_path — parse a small subset of jq path syntax into a list of
// path_step() terms for glazer:find/2:
//
//   .              -> []
//   .foo, .foo.bar -> [{field, <<"foo">>}, ...]
//   .["foo bar"]   -> {field, <<"foo bar">>}
//   .[]            -> iterate
//   .[N], .[-N]    -> {index, N}
//
// Returns {ok, Path} or {error, {invalid_path, Filter}}.
//-----------------------------------------------------------------------------

namespace {

// Parse the contents of a '[...]' segment (the '[' has already been
// consumed). On success, advances `p` past the closing ']'.
bool parse_bracket(ErlNifEnv* env, const char*& p, const char* end, ERL_NIF_TERM& step)
{
  if (p == end) [[unlikely]]
    return false;

  if (*p == ']') {
    ++p;
    step = AM_ITERATE;
    return true;
  }

  if (*p == '"') {
    const char* q = ++p;
    while (q != end && *q != '"')
      ++q;
    if (q == end || q + 1 == end || q[1] != ']') [[unlikely]]
      return false;
    step = enif_make_tuple2(env, AM_FIELD, make_binary(env, std::string_view(p, q - p)));
    p = q + 2;
    return true;
  }

  // Integer index, possibly negative.
  const char* q = p;
  if (q != end && *q == '-')
    ++q;
  const char* digits_start = q;
  while (q != end && *q >= '0' && *q <= '9')
    ++q;
  if (q == digits_start || q == end || *q != ']') [[unlikely]]
    return false;

  long val = std::strtol(std::string(p, q - p).c_str(), nullptr, 10);
  step = enif_make_tuple2(env, AM_INDEX, enif_make_long(env, val));
  p = q + 1;
  return true;
}

// Parse a bare (unquoted) field name up to the next '.' or '['.
bool parse_name(ErlNifEnv* env, const char*& p, const char* end, ERL_NIF_TERM& step)
{
  const char* q = p;
  while (q != end && *q != '.' && *q != '[')
    ++q;
  if (q == p) [[unlikely]]
    return false;
  step = enif_make_tuple2(env, AM_FIELD, make_binary(env, std::string_view(p, q - p)));
  p = q;
  return true;
}

} // namespace

ERL_NIF_TERM nif_compile_path(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary filter;
  if (!enif_inspect_binary(env, argv[0], &filter) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &filter)) [[unlikely]]
    return enif_make_badarg(env);

  auto invalid = [&]() {
    return enif_make_tuple2(env, AM_ERROR,
      enif_make_tuple2(env, AM_INVALID_PATH, make_binary(env, filter.data ?
        std::string_view(reinterpret_cast<const char*>(filter.data), filter.size) :
        std::string_view())));
  };

  const char* p   = reinterpret_cast<const char*>(filter.data);
  const char* end = p + filter.size;

  if (p == end || *p != '.') [[unlikely]]
    return invalid();
  ++p;

  if (p == end)
    return enif_make_tuple2(env, AM_OK, enif_make_list(env, 0));

  std::vector<ERL_NIF_TERM> steps;
  while (p != end) {
    ERL_NIF_TERM step;
    if (*p == '[') {
      ++p;
      if (!parse_bracket(env, p, end, step)) [[unlikely]]
        return invalid();
    } else if (*p == '.') {
      ++p;
      continue;
    } else if (!parse_name(env, p, end, step)) [[unlikely]]
      return invalid();

    steps.push_back(step);
  }

  return enif_make_tuple2(env, AM_OK,
    enif_make_list_from_array(env, steps.data(), static_cast<unsigned>(steps.size())));
}

//-----------------------------------------------------------------------------

} // namespace glz
