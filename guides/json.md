# JSON

## Usage

**Erlang**

```erlang
1> glazer_json:decode(<<"{\"a\":1,\"b\":[true,null,3.5]}">>).
#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}

2> glazer_json:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"{\"a\":1,\"b\":[true,null,3.5]}">>

3> glazer_json:encode(#{a => 1}, [pretty]).
<<"{\n   \"a\": 1\n}">>

4> glazer_json:minify(<<" { \"a\" : 1 } ">>).
{ok, <<"{\"a\":1}">>}

5> glazer_json:prettify(<<"{\"a\":1}">>).
{ok, <<"{\n   \"a\": 1\n}">>}
```

**Elixir**

```elixir
iex> Glazer.JSON.decode(~s({"a":1,"b":[true,null,3.5]}))
{:ok, %{"a" => 1, "b" => [true, nil, 3.5]}}

iex> Glazer.JSON.encode!(%{"a" => 1, "b" => [true, nil, 3.5]})
"{\"a\":1,\"b\":[true,null,3.5]}"

iex> Glazer.JSON.encode!(%{a: 1}, [:pretty])
"{\n   \"a\": 1\n}"

iex> Glazer.JSON.minify!(~s( { "a" : 1 } ))
"{\"a\":1}"

iex> Glazer.JSON.prettify!(~s({"a":1}))
"{\n   \"a\": 1\n}"

iex> Glazer.JSON.decode!(<<"{\"a\":1,\"b\":[true,null,3.5]}">>)
%{"a" => 1, "b" => [true, nil, 3.5]}

iex> Glazer.JSON.encode!(%{"a" => 1, "b" => [true, nil, 3.5]})
"{\"a\":1,\"b\":[true,null,3.5]}"

iex> Glazer.JSON.encode!(%{a: 1}, [:pretty])
"{\n   \"a\": 1\n}"

iex> Glazer.JSON.minify!(" { \"a\" : 1 } ")
"{\"a\":1}"

iex> Glazer.JSON.prettify!("{\"a\":1}")
"{\n   \"a\": 1\n}"
```

## Streaming

For input that arrives in chunks — e.g. reading a large document
incrementally, or consuming newline-delimited JSON (NDJSON) from a
socket or file — `decode_start/3` provides a small stateful
wrapper that buffers partial input and decodes each JSON value as soon
as it's complete, without re-parsing bytes you've already seen.
`decode_continue/2` returns `{Value, Acc, Rest::binary()}` if a `Value` is
complete and parsed, or `{continue, State}` if the value cannot be parsed given
the available data. Once the input is exhausted, call `stream_eof/1` to flush
any trailing bare scalar (numbers, strings, etc. have no closing
delimiter of their own) and surface an error if the buffer holds an
incomplete value:

**Erlang**

```erlang
1> {continue, State} = glazer_json:decode_start(~"{\"a\":", ok, []).
2> {#{~"a" := 1}, ok, NextBin = ~"[2]"} = glazer_json:decode_continue(<<"1}[2]">>, State).
3> {[2], ok, ~""} = glazer_json:decode_start(NextBin, ok, []).
```

**Elixir**

```elixir
iex> {:continue, state} = Glazer.JSON.decode_start(~s({"a":), :ok, [])
iex> {%{"a" => 1}, :ok, "[2]"} = Glazer.JSON.decode_continue("1}[2]", state)
iex> {[2], :ok, ""} = Glazer.JSON.decode_start("[2]", :ok, [])
```

`glazer_json:decode_start/3` accepts the same options as `decode/2` (e.g.
`{keys, atom}`, `use_nil`) and applies them to every decoded value.

A typical read loop calls `decode_start/3` for each chunk while more data
may still arrive, and `decode_continue/2` once the socket closes to flush any
trailing value:

**Erlang**

```erlang
-spec recv_json(inet:socket(), list()) -> {ok, term(), binary()} | {error, term()}.
recv_json(Socket, Opts) ->
  loop(Socket, undefined, Opts).

loop(Socket, State, Opts) ->
  case gen_tcp:recv(Socket, 0) ->
    {ok, Chunk} ->
      case next_json(Chunk, State, Opts) of
        {Value, nil, Rest}   -> {ok, Value, Rest};
        {continue, NewState} -> loop(Socket, NewState, Opts)
      end;
    {error, Reason} ->
        {error, Reason}
  end.

next_json(Chunk, undefined, Opts) -> glazer_json:decode_start(Chunk, nil, Opts);
next_json(Chunk, State,    _Opts) -> glazer_json:decode_continue(Chunk, State).

```

**Elixir**

```elixir
@spec recv_json(:gen_tcp.socket(), keyword()) :: {:ok, term(), binary()} | {:error, term()}
def recv_json(socket, opts), do: loop(socket, :undefined, opts)

defp loop(socket, state, opts) do
  case :gen_tcp.recv(socket, 0) do
    {:ok, chunk} ->
      case next_json(chunk, state, opts) do
        {value, nil, rest} -> {:ok, value, rest}
        {:continue, new_state} -> loop(socket, new_state, opts)
      end

    {:error, reason} ->
      {:error, reason}
  end
end

defp next_json(chunk, :undefined, opts), do: Glazer.JSON.decode_start(chunk, nil, opts)
defp next_json(chunk, state, _opts), do: Glazer.JSON.decode_continue(chunk, state)
```

### Efficiency

`decode_start/3` only scans for value *boundaries* incrementally —
the scanner carries a small resumable cursor (`scan_state()`) that
remembers how far it has already looked (nesting depth, whether it's
inside a string, escape state, …), so each call to `scan/2` resumes
from where the previous one left off rather than re-walking the whole
buffer from byte zero. Once a complete value's end offset is known,
that slice is decoded exactly once via the same NIF-backed decoder
used by `decode/2` — there's no intermediate tokenization or tree
representation, and no byte is ever scanned or decoded twice. The only
buffering cost is concatenating newly-arrived chunks onto the
not-yet-complete tail of the input.

This makes `decode_start/3` well suited to byte-at-a-time or
small-chunk feeding (e.g. consuming a `gen_tcp`/`gen_statem` socket
buffer as it fills) without the quadratic-rescan cost a naive
"concatenate and retry full decode" loop would incur on large or
slow-arriving documents.

Under the hood, `decode_start/3` is built on `scan/1,2` — a low-level
primitive that scans a buffer for the byte offset where the next JSON
value ends (or reports that more input is needed) without doing a full
decode. It's exposed directly for callers that want to implement their
own framing/buffering strategy:

**Erlang**

```erlang
1> glazer_json:scan(<<"{\"a\":1} {\"b\":2}">>).
{complete, 7}

2> glazer_json:scan(<<"{\"a\":">>).
{incomplete, ScanState}

3> glazer_json:scan(<<"{\"a\":1}">>, ScanState).
{complete, 7}
```

**Elixir**

```elixir
iex> Glazer.JSON.scan!(~s({"a":1} {"b":2}))
{:complete, 7}

iex> {:incomplete, scan_state} = Glazer.JSON.scan!(~s({"a":))
iex> Glazer.JSON.scan!(~s({"a":1}), scan_state)
{:complete, 7}
```

`stream_decoder/0,1`, `stream_feed/2`, `stream_eof/1` and
`scan/1,2` are JSON-only — see [YAML streaming](#streaming) and
[CSV streaming](#streaming) below for the other formats.

## Null term configuration

By default, JSON/YAML `null` decodes to (and `null` encodes from) the atom
`null`, and this same atom is used as the default null term throughout the
library (e.g. for the CSV `on_failure => null` field option). This can be
overridden:

- Application-wide, via the `null` environment key — set this once in
  the application's config and every call uses it as the default:

  **Erlang** (`rebar.config`):
  ```erlang
  {glazer, [{null, nil}]}
  ```

  **Elixir** (`config.exs`):
  ```erlang
  config :glazer, null: nil
  ```

- Per call, with the `use_nil` shorthand or the `{null, Atom}`
  option (see [JSON decode options](#json-decode-options) below).
  Per-call options always take precedence over the application-wide
  default.

## JSON decode options

| Option | Description |
|---|---|
| `object_as_tuple` | Decode JSON objects as `{[{Key, Value}]}` proplist tuples (jiffy-style) instead of maps (default) |
| `use_nil` | Use the atom `nil` for JSON `null` |
| `{null, Atom}` | Use `Atom` for JSON `null` |
| `{keys, atom}` | Decode object keys as atoms (via `binary_to_atom/2`-equivalent) |
| `{keys, existing_atom}` | Decode object keys as existing atoms, falling back to binaries for unknown atoms |
| `{keys, binary}` | Decode object keys as binaries (default) |
| `dedupe_keys` | With `object_as_tuple`, eliminate duplicate object keys, keeping the last occurrence's value (and position) |
| `copy_strings` | Always allocate a fresh binary for each decoded string, instead of a zero-copy sub-binary of the input (see [Performance Optimization Details](performance.md#performance-optimization-details)) |
| `return_trailer` | Allow trailing non-whitespace data after the decoded value instead of rejecting it; on a match, return `{has_trailer, Term, Rest}` with `Rest` as a zero-copy sub-binary of the unconsumed remainder |
| `validate_utf8` | Validate UTF-8 in JSON strings (the default). Invalid UTF-8 is a parse error |
| `skip_utf8_validation` | Accept invalid UTF-8 in JSON strings and copy the bytes through unchanged |

**Erlang**

```erlang
1> glazer_json:decode(<<"{\"a\":1}">>, [object_as_tuple]).
{[{<<"a">>, 1}]}

2> glazer_json:decode(<<"{\"a\":1}">>, [{keys, atom}]).
#{a => 1}

3> glazer_json:decode(<<"null">>, [use_nil]).
nil

4> glazer_json:decode(<<"null">>, [{null, undefined}]).
undefined

5> glazer_json:decode(<<"{\"a\":1,\"a\":2}">>).
#{<<"a">> => 2}

6> glazer_json:decode(<<"{\"a\":1,\"a\":2}">>, [object_as_tuple]).
{[{<<"a">>, 1}, {<<"a">>, 2}]}

7> glazer_json:decode(<<"{\"a\":1,\"a\":2}">>, [object_as_tuple, dedupe_keys]).
{[{<<"a">>, 2}]}

8> glazer_json:decode(<<"1 2">>, [return_trailer]).
{has_trailer, 1, <<"2">>}
```

**Elixir**

```elixir
iex> Glazer.JSON.decode(~s({"a":1}), [:object_as_tuple])
{:ok, {[{"a", 1}]}}

iex> Glazer.JSON.decode(~s({"a":1}), keys: :atom)
{:ok, %{a: 1}}

iex> Glazer.JSON.decode("null")                      # nil is the default in Elixir
{:ok, nil}

iex> Glazer.JSON.decode(~s({"a":null}))
{:ok, %{"a" => nil}}

iex> Glazer.JSON.decode(~s({"a":1,"a":2}))
{:ok, %{"a" => 2}}

iex> Glazer.JSON.decode(~s({"a":1,"a":2}), [:object_as_tuple])
{:ok, {[{"a", 1}, {"a", 2}]}}

iex> Glazer.JSON.decode(~s({"a":1,"a":2}), [:object_as_tuple, :dedupe_keys])
{:ok, {[{"a", 2}]}}

iex> Glazer.JSON.decode("1 2", [:return_trailer])
{:ok, {:has_trailer, 1, "2"}}
```

> [!NOTE]
> A JSON object with duplicate keys cannot be represented as an Erlang map,
> so decoding to maps (the default) and `{keys, atom | existing_atom}` always
> dedupe duplicate keys, last value wins, regardless of `dedupe_keys`. With
> `object_as_tuple`, duplicate keys are preserved as-is unless `dedupe_keys`
> is given.

## JSON encode options

| Option | Description |
|---|---|
| `pretty` | Pretty-print the JSON output with two-space indentation |
| `uescape` | Escape non-ASCII characters as `\uXXXX` sequences |
| `force_utf8` | Replace invalid UTF-8 byte sequences with U+FFFD before encoding |
| `use_nil` | Encode the atom `nil` as JSON `null` |
| `{null, Atom}` | Encode `Atom` as JSON `null` |

**Erlang**

```erlang
1> glazer_json:encode(#{a => 1}, [pretty]).
<<"{\n   \"a\": 1\n}">>

2> glazer_json:encode(<<"héllo"/utf8>>, [uescape]).
<<"\"h\\u00e9llo\"">>

3> glazer_json:encode(nil, [use_nil]).
<<"null">>
```

**Elixir**

```elixir
iex> Glazer.JSON.encode!(%{a: 1}, [:pretty])
"{\n   \"a\": 1\n}"

iex> Glazer.JSON.encode!("héllo", [:uescape])
"\"h\\u00e9llo\""

iex> Glazer.JSON.encode!(nil)                        # nil is JSON null in Elixir
"null"
```

**Option `force_utf8`:**

> [!NOTE]
> Decoding validates UTF-8 by default: a JSON string with invalid UTF-8 is a
> parse error. Pass `skip_utf8_validation` to accept such bytes unchanged.
> `force_utf8` is an *encode*-only option; it replaces invalid bytes instead
> of raising.

Binaries may contain arbitrary bytes, including byte sequences that are not
valid UTF-8. By default, encoding such a binary (as a value or an object key)
raises an encode error, so the output is always valid UTF-8/JSON:

**Erlang**

```erlang
1> glazer_json:encode(<<"a", 128, "b">>).
** exception error: {encode_error,{<<"invalid UTF-8 in string">>,<<"a\x80b">>}}
```

With `force_utf8`, each invalid byte (or byte sequence) is replaced with the
Unicode replacement character `U+FFFD` (encoded as `0xEF 0xBF 0xBD`):

**Erlang**

```erlang
2> glazer_json:encode(<<"a", 128, "b">>, [force_utf8]).
<<"\"a", 239, 191, 189, "b\"">>
```

**Elixir**

```elixir
iex> Glazer.JSON.encode!(<<"a", 128, "b">>, [:force_utf8])
"\"a�b\""
```

A literal `U+FFFD` already present in the input is left untouched (it is
not re-replaced). Combining `force_utf8` with `uescape` further escapes the
replacement character as `\ufffd`:

**Erlang**

```erlang
3> glazer_json:encode(<<"a", 128, "b">>, [force_utf8, uescape]).
<<"\"a\\ufffdb\"">>
```

**Elixir**

```elixir
iex> Glazer.JSON.encode!(<<"a", 128, "b">>, [:force_utf8, :uescape])
"\"a\\ufffdb\""
```

## jq filter support

If [`libjq`](https://jqlang.org/) and its headers (`jq.h`/`jv.h`) are
available when `glazer` is built, `query/2,3` runs a jq filter
program against a JSON document and returns one Erlang term per value
produced by the filter (decoded using the same options as
`decode/2`):

**Erlang**

```erlang
1> glazer_json:query(<<"{\"a\":[1,2,3]}">>, <<".a[]">>).
{ok, [1, 2, 3]}

2> glazer_json:query(<<"{\"a\":1}">>, <<".b">>).
{ok, [null]}

3> glazer_json:query(<<"{\"a\":{\"b\":2}}">>, <<".">>, [{keys, atom}]).
{ok, [#{a => #{b => 2}}]}

4> glazer_json:query(<<"not json">>, <<".">>).
{error, invalid_input}

5> glazer_json:query(<<"{\"a\":1}">>, <<"bad syntax (((">>).
{error, jq_decode_error}
```

**Elixir**

```elixir
iex> Glazer.JSON.query!(~s({"a":[1,2,3]}), ".a[]")
{:ok, [1, 2, 3]}

iex> Glazer.JSON.query!(~s({"a":1}), ".b")
{:ok, [:null]}                                       # query keeps the default null atom

iex> Glazer.JSON.query!(~s({"a":{"b":2}}), ".", keys: :atom)
{:ok, [%{a: %{b: 2}}]}

iex> Glazer.JSON.query!("not json", ".")
{:error, :invalid_input}

iex> Glazer.JSON.query!(~s({"a":1}), "bad syntax (((")
{:error, {:jq_compile_error, _}}
```

If `libjq` was not available at build time, `query/2,3` returns
`{error, jq_not_available}`. Build detection is automatic — `make` probes
for `jq.h`/`libjq` and only enables this feature if found, so `glazer`
still builds and works without `libjq` installed.

## Elixir Protocol Support

`glazer_json` can serve as the backend for Elixir protocol implementations,
such as `Jason.Encoder` or custom protocols. This allows Elixir users to
filter struct fields at compile-time using the `@derive` annotation.

### Deriving encoders

`@derive {Glazer.JSON.Encoder, ...}` selects the fields of a struct that are encoded,
with `:only` or `:except`. Encoding then goes through the Erlang NIF. The derive
needs no wrapper module, and the field filtering happens at compile time.

```elixir
defmodule MyApp.User do
  @derive {Glazer.JSON.Encoder, only: [:id, :name, :email]}
  defstruct [:id, :name, :email, :password_hash]
end

user = %MyApp.User{id: 1, name: "Alice", email: "alice@example.com", password_hash: "***"}
Glazer.JSON.Encoder.encode(user, [])
# => "{\"id\":1,\"name\":\"Alice\",\"email\":\"alice@example.com\"}"
```

With Ecto, the derive goes on the schema, and sensitive fields are left out:

```elixir
defmodule MyApp.GustSecrets do
  use Ecto.Schema

  @derive {Glazer.JSON.Encoder, only: [:id, :name, :value_type, :inserted_at, :updated_at]}
  schema "gust_secrets" do
    field :name, :string
    field :value, Gust.Encrypted.Binary, redact: true
    field :value_type, Ecto.Enum, values: [:string, :json]

    timestamps()
  end
end
```

To encode a map directly, use `Glazer.JSON.encode!/2`, as in the examples above.

If an application needs `Jason.encode!/1` to encode a struct, derive `Jason.Encoder` as
usual. Jason's own implementation handles it. Glazer's `Jason.Encoder` fallback applies only
when Jason is not loaded (see [Disabling the `Jason.Encoder` fallback](#disabling-the-jasonencoder-fallback)).

### API functions for protocol implementations

The following functions are explicitly named to match protocol conventions:

- `encode_to_iodata/1` - encode a term to JSON iodata (identical to `encode/1`)
- `encode_to_iodata/2` - encode a term to JSON iodata with options (identical to `encode/2`)

These are aliases for discoverability; they help protocol wrapper code match
the naming convention used in `Jason.Encoder` and similar frameworks.

### Disabling the `Jason.Encoder` fallback

When Jason is not loaded, Glazer defines a small `Jason.Encoder` protocol so that
`@derive {Jason.Encoder, ...}` still compiles. To stop Glazer from defining it,
set the flag in your config and recompile:

```elixir
# config/config.exs
config :glazer, jason_encoder_fallback: false
```

The flag is read at compile time by Elixir code (`Application.compile_env`), so it
applies to Mix builds only; it has no Erlang form. Run `mix compile --force` after changing it.
With the flag off, `@derive {Jason.Encoder, ...}` needs Jason as a dependency.
The default is `true`.

## Elixir's Phoenix `json_library()` compliance

Phoenix supports a pluggable `:json_library` configuration
(see [phoenix](https://phoenix.hexdocs.pm/1.8.8/Phoenix.html#json_library/0))
that lets applications swap in an alternative JSON implementation for
Phoenix's JSON API module by configuring a module that exports:

- `decode!/1`
- `encode!/1`
- `encode_to_iodata!/1`

`Glazer.JSON` exports these, so `glazer_json` can be configured directly
as a `json_library()`. To match Elixir's `JSON` module, where `null` decodes
to/from `nil` rather than the atom `:null`, these three functions automatically
apply `use_nil` — no extra configuration is needed:

```elixir
config :phoenix, :json_library, Glazer.JSON
```

**Elixir**

```elixir
iex> Glazer.JSON.decode!(~s({"a":1,"b":null}))
%{"a" => 1, "b" => nil}

iex> Glazer.JSON.encode!(%{"a" => 1, "b" => nil})
"{\"a\":1,\"b\":null}"

iex> Glazer.JSON.encode_to_iodata!(%{"a" => 1, "b" => nil})
"{\"a\":1,\"b\":null}"

iex> Glazer.JSON.decode!(~s({"a":null}))
%{"a" => nil}

iex> Glazer.JSON.encode!(%{"a" => nil})
"{\"a\":null}"
```

## API

**Erlang**

All functions below are in `glazer_json`:

| Function | Description |
|---|---|
| `decode/1`, `decode/2` | Decode a JSON binary or iolist to an Erlang term |
| `try_decode/1`, `try_decode/2` | Decode a JSON binary or iolist, returning `{ok, Term}` or `{error, {parse_error, Msg}}` instead of raising |
| `encode/1`, `encode/2` | Encode an Erlang term to a JSON binary; raises `{encode_error, {Msg, Term}}` on failure |
| `minify/1` | Remove unnecessary whitespace from a JSON document |
| `prettify/1` | Pretty-print a JSON document with two-space indentation |
| `read_file/1`, `read_file/2` | Read a file and decode its contents as JSON |
| `write_file/2`, `write_file/3` | Encode a term to JSON and write it to a file |
| `scan/1`, `scan/2` | Scan a buffer for the end offset of the next complete JSON value |
| `stream_decoder/0`, `stream_decoder/1` | Create an incremental-decode state for chunked input |
| `stream_feed/2` | Feed a chunk to a stream decoder, returning completed values |
| `stream_eof/1` | Flush a stream decoder at end-of-input |
| `query/2`, `query/3` | Run a [jq](https://jqlang.org/) filter over a JSON document, returning `{ok, [Term]}` (requires `libjq`) |

**Elixir**

`Glazer.JSON` exports the following functions:

| Function | Description |
|---|---|
| `decode/1`, `decode/2` | Decode a JSON binary or iolist, returning `{:ok, term}` or `{:error, reason}`. `null` becomes `nil` |
| `decode!/1` | Decode a JSON binary or iolist, raising `Glazer.ParseError` on invalid input |
| `encode!/1`, `encode!/2` | Encode an Elixir term to a JSON binary, raising `Glazer.ParseError` if it cannot be encoded |
| `encode_to_iodata!/1` | Encode an Elixir term to JSON as iodata, raising `ArgumentError` if it cannot be encoded |
| `encode_ndjson/1`, `encode_ndjson/2` | Encode a list of terms as newline-delimited JSON |
| `minify!/1` | Remove all unnecessary whitespace from a JSON binary or iolist |
| `prettify!/1` | Pretty-print a JSON binary or iolist with indentation |
| `read_file!/1`, `read_file!/2` | Read a file and decode its contents as JSON |
| `write_file!/2`, `write_file!/3` | Encode a term to JSON and write it to a file, overwriting any existing file |
| `query!/2`, `query!/3` | Run a [jq](https://jqlang.org/) filter over a JSON document, returning one term per filter result |
| `scan!/1`, `scan!/2` | Locate the end of the next complete top-level JSON value without decoding it |
| `decode_start/2`, `decode_start/3` | Start incremental (streaming) decoding of a JSON value |
| `decode_continue/2` | Resume incremental decoding with more data, or signal end of input with `:end_of_input` |

## Benchmarking JSON

A comparison benchmark against other JSON libraries (`simdjsone`,
`jiffy`, `jason`, `thoas`, `euneus`, OTP's built-in `json`, and
`torque`) is available via:

```sh
$ make bench-json
===> Running benchmarks (optimization: O3 - PGO)

(numbers in µs)
JSON           twitter (616.7K)   twitter2 (758.0K)     openrtb (1.2K)       esad (1.3K)         small (0.1K)
               decode   encode     decode   encode     decode   encode     decode   encode     decode   encode
----------------------------------------------------------------------------------------------------------------
glazer          833.5    326.7     1057.3    581.6        1.7      1.3        1.2      0.7        0.3      0.3
torque          771.7    310.2     1150.1    544.5        1.8      1.4        1.1      0.8        0.3      0.3
jiffy          2004.8    706.8     2096.3   1734.0        3.2      3.0        2.0      1.6        0.4      0.5
json           2753.8   2636.3     4015.3   4994.2        5.3      5.8        2.6      3.5        0.8      0.8
euneus         2914.6   2523.7     4269.4   5074.8        6.4      5.5        3.1      3.8        1.0      0.9
thoas          3686.2   3919.1     5963.2   7693.4        7.2      7.8        4.9      5.6        0.8      0.9
jason          3911.8   3553.4     6069.0   6959.3        7.2      7.0        4.7      5.3        0.9      0.9
===> Running benchmarks (optimization: O3 - PGO)
===> Process niceness: 0 (lower is higher priority)
```

(requires the `bench`/`dev` Mix dependencies — see `mix.exs`).

To further illustrate performance of `glazer`, here's a benchmark comparison to
some leading Golang libraries (the lower numbers the better):

```sh
$ test/bench-go/run.sh

numbers in µs)
GO-JSON           twitter (616.7K)   twitter2 (758.0K)     openrtb (1.2K)       esad (1.3K)         small (0.1K)
                  decode   encode     decode   encode     decode   encode     decode   encode     decode   encode
-------------------------------------------------------------------------------------------------------------------
sonic             2301.0   1184.0     3812.0   1986.0       12.0      7.0        5.0      3.0        1.0      0.0
goccy/go-json     3594.0   3103.0     5963.0   5490.0       19.0     13.0       11.0      7.0        2.0      1.0
jsoniter          4029.0   1832.0     6655.0   3012.0       16.0     11.0        8.0      6.0        1.0      1.0
jsony                n/a   3866.0        n/a   6913.0        n/a     15.0        n/a      9.0        n/a      1.0
stdlib/json       6751.0   4048.0     8328.0   7894.0       19.0     16.0       12.0      8.0        2.0      2.0
```
