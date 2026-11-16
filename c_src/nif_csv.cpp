// vim:ts=2:sw=2:et
// CSV NIFs: decode and encode.
#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <erl_nif.h>

#include "glazer_nif_api.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_csv.hpp"

namespace glz {
// NIF: csv_decode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM do_csv_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  CSVDecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_csv_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  CSVDecoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size, argv[0]);
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

ERL_NIF_TERM nif_csv_try_decode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
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
// NIF: csv_encode
//-----------------------------------------------------------------------------

static ERL_NIF_TERM nif_csv_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[]);

// Runs on the normal scheduler with an output budget; see nif_json.cpp.
static ERL_NIF_TERM do_csv_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[], size_t budget)
{
  CSVEncodeOpts opts;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_csv_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  try {
    thread_local OutBuf out;
    out.clear();
    CSVEncoder enc{env, opts, out};
    enc.set_budget(budget);
    if (!enc.encode(argv[0])) [[unlikely]] {
      if (enc.budget_exceeded())
        return enif_schedule_nif(env, "glazer_csv_encode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                                 nif_csv_encode_dirty, argc, argv);
      return enif_raise_exception(env,
        enif_make_tuple2(env, AM_ENCODE_ERROR,
          enif_make_tuple2(env, make_binary(env, std::string_view(enc.err())), enc.err_term())));
    }

    update_reduction_count(env, out.view().size());
    return make_binary(env, out.view());
  } catch (const OutOfMemory& e) {
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        enif_make_tuple2(env, make_binary(env, std::string_view("out of memory")), am_null)));
  }
}

static ERL_NIF_TERM nif_csv_encode_dirty(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  return do_csv_encode(env, argc, argv, SIZE_MAX);
}

ERL_NIF_TERM nif_csv_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc < 1 || argc > 2) [[unlikely]]
    return enif_make_badarg(env);
  return do_csv_encode(env, argc, argv, DIRTY_THRESHOLD);
}

//-----------------------------------------------------------------------------

} // namespace glz
