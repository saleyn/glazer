// vim:ts=2:sw=2:et
// ---------------------------------------------------------------------------
// Erlang NIF implementation of JSON encoding/decoding with a focus on speed
// and low overhead.
// ---------------------------------------------------------------------------
// Copyright: 2026 Sergey Aleynikov
//
// Implementation inspired by the [glaze](https://github.com/igormunkus/glaze)
// project, but rewritten from scratch to avoid the intermediate generic_u64
// tree.
//
// NIF registration and dirty-scheduler dispatch glue. The actual decode/
// encode/scan logic lives in glazer_json.hpp (and, in future, glazer_yaml.hpp).
// ---------------------------------------------------------------------------

#include <cassert>
#include <string>
#include <string_view>

#include <erl_nif.h>

#include "glazer_json_format.hpp"
#include "glazer_atoms.hpp"
#include "glazer_bigint.hpp"
#include "glazer_json.hpp"
#include "glazer_yaml.hpp"
#include "glazer_csv.hpp"

// ---------------------------------------------------------------------------
// Dirty-scheduler threshold — inputs larger than this are offloaded to a
// dirty CPU scheduler so they don't block normal scheduler threads.
// Small inputs run inline on a normal scheduler.
// consume_timeslice is not called: dirty schedulers ignore it, and small
// inputs on normal schedulers complete fast enough not to need yielding.
// ---------------------------------------------------------------------------

static constexpr size_t DIRTY_THRESHOLD = 8192;

// ---------------------------------------------------------------------------
// NIF: json_decode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_json_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  DecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  Decoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size);
  return dec.decode(reinterpret_cast<const char*>(bin.data), bin.size);
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
    if (bin.size < DIRTY_THRESHOLD)
      return do_json_decode(env, bin, argc, argv);
    sched_argv[0] = enif_make_binary(env, &bin);
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_json_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_json_try_decode_dirty, 2, sched_argv);
}

// ---------------------------------------------------------------------------
// NIF: yaml_decode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_yaml_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  YamlDecodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_yaml_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  YamlDecoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size);
  return dec.decode();
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
    if (bin.size < DIRTY_THRESHOLD)
      return do_yaml_decode(env, bin, argc, argv);
    sched_argv[0] = enif_make_binary(env, &bin);
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_yaml_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_yaml_try_decode_dirty, 2, sched_argv);
}

// ---------------------------------------------------------------------------
// NIF: csv_decode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_csv_decode(ErlNifEnv* env, const ErlNifBinary& bin, int argc, const ERL_NIF_TERM argv[])
{
  CsvDecodeOpts opts;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_csv_decode_opts(env, argv[1], opts)))
    return enif_make_badarg(env);
  CsvDecoder dec(env, opts, reinterpret_cast<const char*>(bin.data), bin.size);
  return dec.decode();
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
    if (bin.size < DIRTY_THRESHOLD)
      return do_csv_decode(env, bin, argc, argv);
    sched_argv[0] = enif_make_binary(env, &bin);
    sched_argv[1] = argc > 1 ? argv[1] : enif_make_list(env, 0);
  } else {
    return enif_make_badarg(env);
  }
  return enif_schedule_nif(env, "glazer_csv_decode", ERL_NIF_DIRTY_JOB_CPU_BOUND,
                           nif_csv_try_decode_dirty, 2, sched_argv);
}

// ---------------------------------------------------------------------------
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
// ---------------------------------------------------------------------------

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
    return enif_make_tuple2(env, AM_COMPLETE, enif_make_uint64(env, offset));
  }

  return enif_make_tuple2(env, AM_INCOMPLETE, scan_state_to_term(env, st));
}

// ---------------------------------------------------------------------------
// NIF: json_encode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_json_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  EncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  Encoder enc{env, opts, out};
  if (!enc.encode(argv[0]))
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        make_binary(env, std::string_view("cannot encode term to JSON"))));

  if (!opts.pretty)
    return make_binary(env, out.view());

  auto pretty_out = glz::prettify_json(out.view());
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

// ---------------------------------------------------------------------------
// NIF: yaml_encode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_yaml_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  YamlEncodeOpts opts;
  opts.null_term = am_null;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_yaml_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  YamlEncoder enc{env, opts, out};
  if (!enc.encode(argv[0]))
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        make_binary(env, std::string_view("cannot encode term to YAML"))));

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

// ---------------------------------------------------------------------------
// NIF: csv_encode
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_csv_encode(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  CsvEncodeOpts opts;
  if (argc == 2 && (!enif_is_list(env, argv[1]) || !parse_csv_encode_opts(env, argv[1], opts))) [[unlikely]]
    return enif_make_badarg(env);

  OutBuf out;
  CsvEncoder enc{env, opts, out};
  if (!enc.encode(argv[0]))
    return enif_raise_exception(env,
      enif_make_tuple2(env, AM_ENCODE_ERROR,
        make_binary(env, std::string_view("cannot encode term to CSV"))));

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

// ---------------------------------------------------------------------------
// NIF: json_minify / json_prettify
// ---------------------------------------------------------------------------

static ERL_NIF_TERM do_json_minify(ErlNifEnv* env, const ErlNifBinary& bin)
{
  std::string in(reinterpret_cast<const char*>(bin.data), bin.size);
  return make_binary(env, glz::minify_json(in));
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
  return make_binary(env, glz::prettify_json(in));
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

// ---------------------------------------------------------------------------
// NIF: encode_integer / try_decode_integer
// ---------------------------------------------------------------------------

static ERL_NIF_TERM nif_encode_integer(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
  if (argc != 1) [[unlikely]]
    return enif_make_badarg(env);
  glazer::BigInt::StringOut out;
  if (glazer::BigInt::encode(env, argv[0], out)) [[likely]]
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
  auto r = glazer::BigInt::decode(env,
    reinterpret_cast<const char*>(bin.data),
    reinterpret_cast<const char*>(bin.data) + bin.size);
  if (r) [[likely]]
    return enif_make_tuple2(env, AM_OK, r);
  return enif_make_tuple2(env, AM_ERROR, AM_INVALID_NUMBER_FORMAT);
}

// ---------------------------------------------------------------------------
// NIF lifecycle
// ---------------------------------------------------------------------------

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
  {"encode_integer",     1, nif_encode_integer,     0},
  {"try_decode_integer", 1, nif_try_decode_integer, 0},
};

ERL_NIF_INIT(glazer, nif_funcs, nif_load, NULL, NULL, NULL)
