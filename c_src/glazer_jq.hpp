// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// Optional jq-filter support for glazer:json_query/2.
//
// When libjq (https://github.com/jqlang/jq) and its headers are available at
// build time (GLAZER_HAVE_JQ defined by c_src/Makefile), this implements a
// jq filter over a JSON document: the input is parsed by libjq's `jv_parse`,
// the filter program is compiled and run via `jq_compile`/`jq_start`/
// `jq_next`, and each output value is serialized back to JSON text via
// `jv_dump_string` and re-decoded into an Erlang term using glazer's own
// JSON decoder (so decode options/null handling stay consistent with
// json_decode/2).
//
// When libjq is unavailable, json_query/2 returns {error, jq_not_available}.
//-----------------------------------------------------------------------------
#pragma once

#include <memory>
#include <string>

#include <erl_nif.h>

#include "glazer_atoms.hpp"
#include "glazer_json.hpp"

#ifdef GLAZER_HAVE_JQ
#include <jq.h>
#include <jv.h>

namespace glz {

struct JqSmartPtr : std::unique_ptr<jq_state, void(*)(jq_state*)> {
  JqSmartPtr() noexcept
    : unique_ptr(jq_init(), [](jq_state* jq) { jq_teardown(&jq); }) {}
};

// RAII wrapper around jv's refcounted value type: calls jv_free on
// destruction, and jv_copy when a copy is taken (jv's own copy semantics
// are refcount-based, so this mirrors how raw jv values are normally used).
class JvGuard {
public:
  // v is passed by value because jv itself is a small value type — it's a
  // struct wrapping a tagged pointer/refcounted handle (defined in jv.h),
  // not a heap-allocated object accessed through a pointer.
  // libjq's API passes and returns jv by value everywhere.
  explicit JvGuard(jv v) noexcept : m_v(v) {}
  JvGuard(const JvGuard& o) noexcept : m_v(jv_copy(o.m_v)) {}
  JvGuard(JvGuard&& o) noexcept : m_v(o.m_v) { o.m_v = jv_invalid(); }
  ~JvGuard() { jv_free(m_v); }

  JvGuard& operator=(const JvGuard&) = delete;

  JvGuard& operator=(JvGuard&& o) noexcept {
    if (this != &o) {
      jv_free(m_v);
      m_v   = o.m_v;
      o.m_v = jv_invalid();
    }
    return *this;
  }

  jv   get()    const noexcept { return m_v; }
  jv   release()      noexcept { jv val = m_v; m_v = jv_invalid(); return val; }
  bool valid()  const noexcept { return jv_is_valid(m_v); }

private:
  jv m_v;
};

inline ERL_NIF_TERM jq_query(ErlNifEnv* env, const ErlNifBinary& input,
                              const ErlNifBinary& filter, const DecodeOpts& opts)
{
  JqSmartPtr jq;
  if (!jq) [[unlikely]]
    return enif_make_tuple2(env, AM_ERROR, AM_ENOMEM);

  std::string err_msg;
  jq_set_error_cb(jq.get(), [](void* data, jv msg) {
    auto* out = static_cast<std::string*>(data);
    if (jv_get_kind(msg) != JV_KIND_STRING)
      msg = jv_dump_string(msg, 0);
    if (!out->empty())
      *out += "; ";
    *out += jv_string_value(msg);
    jv_free(msg);
  }, &err_msg);

  std::string filter_str(reinterpret_cast<const char*>(filter.data), filter.size);
  if (!jq_compile(jq.get(), filter_str.c_str()))
    return enif_make_tuple2(env, AM_ERROR,
      enif_make_tuple2(env, AM_JQ_COMPILE_ERROR, make_binary(env, err_msg)));

  std::string input_str(reinterpret_cast<const char*>(input.data), input.size);
  JvGuard input_val(jv_parse(input_str.c_str()));
  if (!input_val.valid())
    return enif_make_tuple2(env, AM_ERROR, AM_INVALID_INPUT);

  jq_start(jq.get(), input_val.release(), 0);

  std::vector<ERL_NIF_TERM> results;
  for (;;) {
    JvGuard result(jq_next(jq.get()));
    if (!result.valid()) {
      if (jv_invalid_has_msg(jv_copy(result.get()))) {
        JvGuard msg(jv_invalid_get_msg(result.release()));
        if (jv_get_kind(msg.get()) != JV_KIND_STRING)
          msg = JvGuard(jv_dump_string(msg.release(), 0));
        if (!err_msg.empty())
          err_msg += "; ";
        err_msg += jv_string_value(msg.get());
      }
      break;
    }

    JvGuard text(jv_dump_string(result.release(), 0));
    std::string_view sv(jv_string_value(text.get()), jv_string_length_bytes(jv_copy(text.get())));

    Decoder dec(env, opts, sv.data(), sv.size());
    auto [success, decoded] = dec.decode(sv.data(), sv.size());

    // jq's own JSON output should always be valid JSON; if re-decoding
    // ever fails, surface it rather than silently dropping the value.
    if (!success) [[unlikely]]
      return enif_make_tuple2(env, AM_ERROR, AM_JQ_DECODE_ERROR);

    results.push_back(decoded);
  }

  if (!err_msg.empty())
    return enif_make_tuple2(env, AM_ERROR, make_binary(env, err_msg));

  return enif_make_tuple2(env, AM_OK,
    enif_make_list_from_array(env, results.data(), static_cast<unsigned>(results.size())));
}

} // namespace glz

#else // !GLAZER_HAVE_JQ

namespace glz {

inline ERL_NIF_TERM jq_query(ErlNifEnv* env, const ErlNifBinary&, const ErlNifBinary&, const DecodeOpts&)
{
  return enif_make_tuple2(env, AM_ERROR, AM_JQ_NOT_AVAILABLE);
}

} // namespace glz

#endif // GLAZER_HAVE_JQ
