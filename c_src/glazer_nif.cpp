// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Erlang NIF implementation of JSON encoding/decoding with a focus on speed
// and low overhead.
//-----------------------------------------------------------------------------
// Copyright: 2026 Sergey Aleynikov
//
// Implementation inspired by the [glaze](https://github.com/igormunkus/glaze)
// project, but rewritten from scratch to avoid the intermediate generic_u64
// tree.
//
// NIF registration and dirty-scheduler dispatch glue. The actual decode/
// encode/scan logic lives in glazer_json.hpp (and, in future, glazer_yaml.hpp).
//-----------------------------------------------------------------------------

#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "glazer_json_format.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_json.hpp"
#include "glazer_yaml.hpp"
#include "glazer_csv.hpp"
#include "glazer_jq.hpp"

namespace glz {

//-----------------------------------------------------------------------------
// Dirty-scheduler threshold — inputs larger than this are offloaded to a
// dirty CPU scheduler so they don't block normal scheduler threads.
// Small inputs run inline on a normal scheduler; for those,
// update_reduction_count() (see glazer_common.hpp) reports the work done to
// the scheduler via enif_consume_timeslice so the BEAM updates the calling
// process's reduction count instead of charging a flat 1 reduction for the
// call. Dirty schedulers don't need this — they're not shared with regular
// processes the same way.
//-----------------------------------------------------------------------------

static constexpr size_t DIRTY_THRESHOLD = 8192;

//-----------------------------------------------------------------------------
// NIF: json_decode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  DecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  Decoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size, argv[0]);
  auto result = make_tuple(env, dec.decode(reinterpret_cast<const char*>(bin.data), bin.size));
  update_reduction_count(env, bin.size);
  return result;
}

static ERL_NIF_TERM nif_json_try_decode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_json_decode(env, bin, argc, argv);
}

static ERL_NIF_TERM nif_json_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[2];
  if (enif_inspect_binary(env, argv[0], &bin)) [[likely]] {
    if (bin.size < DIRTY_THRESHOLD)
      return do_json_decode(env, bin, argc, argv);
    sched_argv[0] = argv[0];
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    // argv[0] is an iolist, not itself a binary term — enif_make_binary
    // materializes a real binary term so any zero-copy sub_binary taken
    // against it downstream (decode keeps a reference for the whole call)
    // is valid; passing the raw iolist term to enif_make_sub_binary would
    // be undefined behaviour.
    ERL_NIF_TERM bin_term = enif_make_binary(env, &bin);
    if (bin.size < DIRTY_THRESHOLD) {
      ERL_NIF_TERM inline_argv[2] = { bin_term, argc > 1 ? argv[1] : enif_make_list(env, 0) };
      return do_json_decode(env, bin, argc, inline_argv);
    }
    sched_argv[0] = bin_term;
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_json_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_json_try_decode_dirty, 2, sched_argv);
}

//-----------------------------------------------------------------------------
// NIF: yaml_decode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_yaml_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  YamlDecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_yaml_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  YamlDecoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size, argv[0]);
  auto result = make_tuple(env, dec.decode());
  update_reduction_count(env, bin.size);
  return result;
}

static ERL_NIF_TERM nif_yaml_try_decode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_yaml_decode(env, bin, argc, argv);
}

static ERL_NIF_TERM nif_yaml_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[2];
  if (enif_inspect_binary(env, argv[0], &bin)) [[likely]] {
    if (bin.size < DIRTY_THRESHOLD)
      return do_yaml_decode(env, bin, argc, argv);
    sched_argv[0] = argv[0];
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    // argv[0] is an iolist, not itself a binary term — enif_make_binary
    // materializes a real binary term so any zero-copy sub_binary taken
    // against it downstream is valid; passing the raw iolist term to
    // enif_make_sub_binary would be undefined behaviour.
    ERL_NIF_TERM bin_term = enif_make_binary(env, &bin);
    if (bin.size < DIRTY_THRESHOLD) {
      ERL_NIF_TERM inline_argv[2] = { bin_term, argc > 1 ? argv[1] : enif_make_list(env, 0) };
      return do_yaml_decode(env, bin, argc, inline_argv);
    }
    sched_argv[0] = bin_term;
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_yaml_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_yaml_try_decode_dirty, 2, sched_argv);
}

//-----------------------------------------------------------------------------
// NIF: csv_decode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_csv_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  CsvDecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_csv_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  CsvDecoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size, argv[0]);
  auto result = make_tuple(env, dec.decode());
  update_reduction_count(env, bin.size);
  return result;
}

static ERL_NIF_TERM nif_csv_try_decode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_csv_decode(env, bin, argc, argv);
}

static ERL_NIF_TERM nif_csv_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[2];
  if (enif_inspect_binary(env, argv[0], &bin)) [[likely]] {
    if (bin.size < DIRTY_THRESHOLD)
      return do_csv_decode(env, bin, argc, argv);
    sched_argv[0] = argv[0];
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    // argv[0] is an iolist, not itself a binary term — enif_make_binary
    // materializes a real binary term so any zero-copy sub_binary taken
    // against it downstream is valid; passing the raw iolist term to
    // enif_make_sub_binary would be undefined behaviour.
    ERL_NIF_TERM bin_term = enif_make_binary(env, &bin);
    if (bin.size < DIRTY_THRESHOLD) {
      ERL_NIF_TERM inline_argv[2] = { bin_term, argc > 1 ? argv[1] : enif_make_list(env, 0) };
      return do_csv_decode(env, bin, argc, inline_argv);
    }
    sched_argv[0] = bin_term;
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_csv_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_csv_try_decode_dirty, 2, sched_argv);
}

//-----------------------------------------------------------------------------
// NIF: scan — locate the end of the next complete top-level JSON value.
//
//   scan(Bin)            -> {complete, EndOffset} | {incomplete, State} | {error, Reason}
//   scan(Bin, State)     -> resumes scanning Bin (the *unconsumed remainder*
//                           from a prior {incomplete, State}, with new bytes
//                           appended) using the given State
//
// `EndOffset` is the byte offset, into `Bin`, one past the end of the value —
// i.e. binary:part(Bin, 0, EndOffset) is the complete value, and the rest is
// left over for the next call.
//-----------------------------------------------------------------------------

static ERL_NIF_TERM nif_json_scan(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);

  ScanState st = ScanState::initial();
  if (argc == 2 && !scan_state_from_term(env, argv[1], st))
    return enif_make_badarg(env);

  const char* data = reinterpret_cast<const char*>(bin.data);
  Scanner scanner(data, bin.size, st.pos);

  const char* value_end = nullptr;
  if (scanner.scan(st, value_end)) {
    size_t offset = static_cast<size_t>(value_end - data);
    update_reduction_count(env, offset);
    return enif_make_tuple2(env, AM_COMPLETE, enif_make_uint64(env, offset));
  }

  update_reduction_count(env, bin.size);
  return enif_make_tuple2(env, AM_INCOMPLETE, scan_state_to_term(env, st));
}

//-----------------------------------------------------------------------------
// NIF: json_encode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  EncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  JsonEncoder enc{env, opts, out};
  if (!enc.encode(argv[0])) [[unlikely]]
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        enif_make_tuple2(env, make_binary(env, std::string_view(enc.m_err)), enc.m_err_term)));

  if (!opts.pretty) {
    update_reduction_count(env, out.view().size());
    return make_binary(env, out.view());
  }

  auto pretty_out = glz::prettify_json(out.view());
  update_reduction_count(env, pretty_out.size());
  return make_binary(env, pretty_out);
}

static ERL_NIF_TERM nif_json_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_json_encode(env, argc, argv);
}

static ERL_NIF_TERM nif_json_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  // Output size is unknown upfront; use input binary size as a proxy.
  // For non-binary terms (atoms, integers, short lists) always run inline.
  ErlNifBinary bin;
  if (enif_inspect_binary(env, argv[0], &bin) && bin.size >= DIRTY_THRESHOLD) [[unlikely]] {
    ERL_NIF_TERM sched_argv[2] = { argv[0], argc > 1 ? argv[1] : enif_make_list(env, 0) };
    return enif_schedule_nif(env, "glazer_json_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                             nif_json_encode_dirty, 2, sched_argv);
  }
  return do_json_encode(env, argc, argv);
}

//-----------------------------------------------------------------------------
// NIF: yaml_encode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_yaml_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  YamlEncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_yaml_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  YamlEncoder enc{env, opts, out};
  if (!enc.encode(argv[0])) {
    if (enc.m_err)
      return enif_raise_exception(env,
        enif_make_tuple2(env, AM_ENCODE_ERROR,
          enif_make_tuple2(env, make_binary(env, std::string_view(enc.m_err)), enc.m_err_term)));
    return enif_raise_exception(env, AM_INVALID_INPUT);
  }

  update_reduction_count(env, out.view().size());
  return make_binary(env, out.view());
}

static ERL_NIF_TERM nif_yaml_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_yaml_encode(env, argc, argv);
}

static ERL_NIF_TERM nif_yaml_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  // Output size is unknown upfront; use input binary size as a proxy.
  ErlNifBinary bin;
  if (enif_inspect_binary(env, argv[0], &bin) && bin.size >= DIRTY_THRESHOLD) [[unlikely]] {
    ERL_NIF_TERM sched_argv[2] = { argv[0], argc > 1 ? argv[1] : enif_make_list(env, 0) };
    return enif_schedule_nif(env, "glazer_yaml_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                             nif_yaml_encode_dirty, 2, sched_argv);
  }
  return do_yaml_encode(env, argc, argv);
}

//-----------------------------------------------------------------------------
// NIF: csv_encode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_csv_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  CsvEncodeOpts opts;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_csv_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  CsvEncoder enc{env, opts, out};
  if (!enc.encode(argv[0])) [[unlikely]]
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        enif_make_tuple2(env, make_binary(env, std::string_view(enc.m_err)), enc.m_err_term)));

  update_reduction_count(env, out.view().size());
  return make_binary(env, out.view());
}

static ERL_NIF_TERM nif_csv_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_csv_encode(env, argc, argv);
}

static ERL_NIF_TERM nif_csv_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);

  ErlNifBinary bin;
  if (enif_inspect_binary(env, argv[0], &bin) && bin.size >= DIRTY_THRESHOLD) [[unlikely]] {
    ERL_NIF_TERM sched_argv[2] = { argv[0], argc > 1 ? argv[1] : enif_make_list(env, 0) };
    return enif_schedule_nif(env, "glazer_csv_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                             nif_csv_encode_dirty, 2, sched_argv);
  }
  return do_csv_encode(env, argc, argv);
}

//-----------------------------------------------------------------------------
// NIF: json_minify / json_prettify
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_minify(ErlNifEnv* env, const ErlNifBinary& bin)
{
  std::string in(reinterpret_cast<const char*>(bin.data), bin.size);
  auto result = make_binary(env, glz::minify_json(in));
  update_reduction_count(env, bin.size);
  return result;
}

static ERL_NIF_TERM nif_json_minify_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_json_minify(env, bin);
}

static ERL_NIF_TERM nif_json_minify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[1];
  if (enif_inspect_binary(env, argv[0], &bin)) [[likely]] {
    if (bin.size < DIRTY_THRESHOLD)
      return do_json_minify(env, bin);
    sched_argv[0] = argv[0];
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_json_minify(env, bin);
    sched_argv[0] = enif_make_binary(env, &bin);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_json_minify", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_json_minify_dirty, 1, sched_argv);
}

static ERL_NIF_TERM do_json_prettify(ErlNifEnv* env, const ErlNifBinary& bin)
{
  std::string_view in(reinterpret_cast<const char*>(bin.data), bin.size);
  auto result = make_binary(env, glz::prettify_json(in));
  update_reduction_count(env, bin.size);
  return result;
}

static ERL_NIF_TERM nif_json_prettify_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  return do_json_prettify(env, bin);
}

static ERL_NIF_TERM nif_json_prettify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[1];
  if (enif_inspect_binary(env, argv[0], &bin)) [[likely]] {
    if (bin.size < DIRTY_THRESHOLD)
      return do_json_prettify(env, bin);
    sched_argv[0] = argv[0];
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &bin)) {
    if (bin.size < DIRTY_THRESHOLD)
      return do_json_prettify(env, bin);
    sched_argv[0] = enif_make_binary(env, &bin);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_json_prettify", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_json_prettify_dirty, 1, sched_argv);
}

//-----------------------------------------------------------------------------
// NIF: json_query (jq filter, optional — requires libjq at build time)
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_query(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary input, filter;
  if (!enif_inspect_iolist_as_binary(env, argv[0], &input) ||
      !enif_inspect_iolist_as_binary(env, argv[1], &filter)) [[unlikely]]
    return enif_make_badarg(env);

  DecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 3 && (!enif_is_list(env, argv[2]) || !parse_decode_opts(env, argv[2], opts))) [[unlikely]]
    return enif_make_badarg(env);

  return jq_query(env, input, filter, opts);
}

static ERL_NIF_TERM nif_json_query_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_json_query(env, argc, argv);
}

static ERL_NIF_TERM nif_json_query(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
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

static ERL_NIF_TERM nif_compile_path(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
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
// NIF: encode_integer / try_decode_integer
//-----------------------------------------------------------------------------

static ERL_NIF_TERM nif_encode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  glz::BigInt::StringOut out;
  if (glz::BigInt::encode(env, argv[0], out)) [[likely]]
    return make_binary(env, out.str);
  return enif_make_badarg(env);
}

static ERL_NIF_TERM nif_try_decode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]] return enif_make_badarg(env);
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);
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

  ERL_NIF_TERM head, tail = load_info;
  while (enif_get_list_cell(env, tail, &head, &tail)) {
    int arity; const ERL_NIF_TERM* tp;
    if (enif_get_tuple(env, head, &arity, &tp) && arity == 2)
      if (enif_is_identical(tp[0], AM_NULL) && enif_is_atom(env, tp[1]))
        am_null = tp[1];
  }
  return 0;
}

static ErlNifFunc nif_funcs[] = {
  {"json_try_decode",    1, nif_json_try_decode,    0},
  {"json_try_decode",    2, nif_json_try_decode,    0},
  {"yaml_try_decode",    1, nif_yaml_try_decode,    0},
  {"yaml_try_decode",    2, nif_yaml_try_decode,    0},
  {"csv_try_decode",     1, nif_csv_try_decode,     0},
  {"csv_try_decode",     2, nif_csv_try_decode,     0},
  {"json_scan",          1, nif_json_scan,          0},
  {"json_scan",          2, nif_json_scan,          0},
  {"json_encode",        1, nif_json_encode,        0},
  {"json_encode",        2, nif_json_encode,        0},
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
};

} // namespace glz

ERL_NIF_INIT(glazer, glz::nif_funcs, glz::nif_load, NULL, NULL, NULL)
