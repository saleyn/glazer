// vim:ts=2:sw=2:et
// JSON NIFs: decode, scan, encode, ndjson, minify, prettify (hot path, kept in one unit).
#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "glazer_nif_api.hpp"
#include "glazer_json_format.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_json.hpp"

namespace glz {
//-----------------------------------------------------------------------------
// NIF: json_decode
//-----------------------------------------------------------------------------

static std::tuple<bool, ERL_NIF_TERM> do_json_try_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  JSONDecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_decode_opts(env, argv[1], opts))) [[unlikely]]
    return std::make_tuple(false, AM_BADARG);
  JSONDecoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size, argv[0]);
  auto result = dec.decode(reinterpret_cast<const char*>(bin.data), bin.size);
  update_reduction_count(env, bin.size);
  return result;
}

static ERL_NIF_TERM nif_json_try_decode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  ErlNifBinary bin;
  [[maybe_unused]] bool ok = enif_inspect_binary(env, argv[0], &bin);
  assert(ok);
  auto res = do_json_try_decode(env, bin, argc, argv);
  return make_tuple(env, res);
}

ERL_NIF_TERM nif_json_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_tuple2(env, AM_ERROR, AM_BADARG);

  ErlNifBinary bin;
  ERL_NIF_TERM sched_argv[2];
  if (enif_inspect_binary(env, argv[0], &bin)) [[likely]] {
    if (bin.size < DIRTY_THRESHOLD) {
      auto [success, res] = do_json_try_decode(env, bin, argc, argv);
      return enif_make_tuple2(env, success ? AM_OK : AM_ERROR, res);
    }
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
      auto [success, res] = do_json_try_decode(env, bin, argc, inline_argv);
      return enif_make_tuple2(env, success ? AM_OK : AM_ERROR, res);
    }
    sched_argv[0] = bin_term;
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_json_try_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_json_try_decode_dirty, 2, sched_argv);
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

ERL_NIF_TERM nif_json_scan(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  assert(argc == 2);

  ErlNifBinary bin;
  if (!enif_inspect_binary(env, argv[0], &bin) &&
      !enif_inspect_iolist_as_binary(env, argv[0], &bin))
    return enif_make_badarg(env);

  ScanState st = ScanState::initial();
  if (!scan_state_from_term(env, argv[1], st)) [[unlikely]]
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
// NIF: json_try_encode
//-----------------------------------------------------------------------------

// Encoders run first on the normal scheduler with an output budget of
// DIRTY_THRESHOLD bytes. If the output would pass the budget, the call is
// rescheduled on a dirty CPU scheduler with no budget. Output size is measured
// as it is produced, so no separate walk of the input term is needed.

static ERL_NIF_TERM nif_json_try_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM nif_json_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM nif_json_encode_ndjson_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

static ERL_NIF_TERM do_json_try_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[], size_t budget)
{
  JSONEncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_tuple2(env, AM_ERROR, AM_BADARG);

  // The size is measured while encoding: the normal run stops once the output
  // passes the budget and reruns on a dirty scheduler, which encodes without
  // a separate size pass. The buffer is thread-local, so it keeps its capacity.
  thread_local OutBuf out;
  out.clear();

  JSONEncoder enc{env, opts, out};
  enc.set_budget(budget);
  if (!enc.encode(argv[0])) [[unlikely]]
    return enc.budget_exceeded()
      ? enif_schedule_nif(env, "glazer_json_try_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                          nif_json_try_encode_dirty, argc, argv)
      : enif_make_tuple2(env, AM_ERROR,
          enif_make_tuple2(env, AM_ENCODE_ERROR,
            enif_make_tuple2(env, make_binary(env, std::string_view(enc.err())), enc.err_term())));

  if (!opts.pretty) {
    update_reduction_count(env, out.view().size());
    return enif_make_tuple2(env, AM_OK, make_binary(env, out.view()));
  }

  auto pretty_out = glz::prettify_json(out.view());
  update_reduction_count(env, pretty_out.size());
  return enif_make_tuple2(env, AM_OK, make_binary(env, pretty_out));
}

static ERL_NIF_TERM nif_json_try_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_json_try_encode(env, argc, argv, SIZE_MAX);
}

ERL_NIF_TERM nif_json_try_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_tuple(env, AM_ERROR, AM_BADARG);
  return do_json_try_encode(env, argc, argv, DIRTY_THRESHOLD);
}

//-----------------------------------------------------------------------------
// NIF: json_encode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[], size_t budget)
{
  JSONEncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  try {
    // The size is measured while encoding: the normal run stops once the output
    // passes the budget and reruns on a dirty scheduler, which encodes without
    // a separate size pass. The buffer is thread-local, so it keeps its capacity.
    thread_local OutBuf out;
    out.clear();

    JSONEncoder enc{env, opts, out};
    enc.set_budget(budget);
    if (!enc.encode(argv[0])) [[unlikely]] {
      if (enc.budget_exceeded())
        return enif_schedule_nif(env, "glazer_json_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                                 nif_json_encode_dirty, argc, argv);
      return enif_raise_exception(env,
        enif_make_tuple2(env, AM_ENCODE_ERROR,
          enif_make_tuple2(env, make_binary(env, std::string_view(enc.err())), enc.err_term())));
    }

    if (!opts.pretty) {
      update_reduction_count(env, out.view().size());
      return make_binary(env, out.view());
    }

    auto pretty_out = glz::prettify_json(out.view());
    update_reduction_count(env, pretty_out.size());
    return make_binary(env, pretty_out);
  } catch (const OutOfMemory& e) {
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        enif_make_tuple2(env, make_binary(env, std::string_view("out of memory")), am_null)));
  }
}

static ERL_NIF_TERM nif_json_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_json_encode(env, argc, argv, SIZE_MAX);
}

ERL_NIF_TERM nif_json_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);
  return do_json_encode(env, argc, argv, DIRTY_THRESHOLD);
}

//-----------------------------------------------------------------------------
// NIF: json_encode_ndjson — encode a list of terms as newline-delimited JSON
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_json_encode_ndjson(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[], size_t budget)
{
  if (!enif_is_list(env, argv[0])) [[unlikely]]
    return enif_make_badarg(env);

  JSONEncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  try {
    thread_local OutBuf out;
    out.clear();
    ERL_NIF_TERM list = argv[0];
    ERL_NIF_TERM head;

    // Encode each element, appending \n after each. Pretty printing is not
    // applied: each line is an independent JSON value.
    while (enif_get_list_cell(env, list, &head, &list)) {
      JSONEncoder enc{env, opts, out};
      enc.set_budget(budget);
      if (!enc.encode(head)) [[unlikely]] {
        if (enc.budget_exceeded())
          return enif_schedule_nif(env, "glazer_json_encode_ndjson", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                                   nif_json_encode_ndjson_dirty, argc, argv);
        return enif_raise_exception(env,
          enif_make_tuple2(env, AM_ENCODE_ERROR,
            enif_make_tuple2(env, make_binary(env, std::string_view(enc.err())), enc.err_term())));
      }
      out.push('\n');
    }

    // Check for proper list termination (should end with empty list)
    if (!enif_is_empty_list(env, list)) [[unlikely]]
      return enif_make_badarg(env);

    update_reduction_count(env, out.view().size());
    return make_binary(env, out.view());
  } catch (const OutOfMemory& e) {
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        enif_make_tuple2(env, make_binary(env, std::string_view("out of memory")), am_null)));
  }
}

static ERL_NIF_TERM nif_json_encode_ndjson_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_json_encode_ndjson(env, argc, argv, SIZE_MAX);
}

ERL_NIF_TERM nif_json_encode_ndjson(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);
  return do_json_encode_ndjson(env, argc, argv, DIRTY_THRESHOLD);
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

ERL_NIF_TERM nif_json_minify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
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

ERL_NIF_TERM nif_json_prettify(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
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

} // namespace glz
