-module(glazer_json).
-moduledoc """
Fast JSON encoding and decoding using the glaze C++ library.

By default `null`s are represented as the atom `null`. To change it
application-wide, set the `null` env key in your config:
```
{glazer, [{null, nil}]}.
```

## Features

- Decoding straight to Erlang terms: maps, lists, binaries, integers
  (including bignums), floats, booleans, and `null`
- Encoding Erlang terms straight to JSON, including big integers
- Incremental/streaming decoding via `decode_start/3` and `decode_continue/2`:
  parse one JSON value per call with unparsed remainder preserved; compatible
  with OTP `json:decode_start/3` and `json:decode_continue/2`
- Configurable representation of JSON `null` and JSON object keys
- `minify/1` and `prettify/1` helpers
- `read_file/1,2` and `write_file/2,3` helpers for decoding/encoding
  directly to/from a file
- `query/2,3`: run a [jq](https://jqlang.org/) filter over a JSON
  document, returning decoded Erlang terms (requires `glazer` to be built
  with `libjq` available)

## Elixir Protocol Support

When used from Elixir, `glazer_json:encode/2` can serve as the backend for
protocol implementations (e.g., `Jason.Encoder` or custom protocols). The
`encode_to_iodata/1,2` functions explicitly document the iodata return type
for use in protocol wrappers. See the README for an example of building a
custom Elixir protocol on top of Glazer's Erlang encoding functions.
""".
-export([decode/1, decode/2, try_decode/1, try_decode/2,
         encode/1, encode/2, try_encode/2,
         encode_ndjson/1, encode_ndjson/2, minify/1, prettify/1,
         query/2, query/3,
         scan/1, scan/2,
         read_file/1, read_file/2, write_file/2, write_file/3,
         stream_decoder/0, stream_decoder/1, stream_feed/2, stream_eof/1,
         decode_start/3, decode_continue/2,
         'decode!'/1, 'encode!'/1, 'encode_to_iodata!'/1,
         encode_to_iodata/1, encode_to_iodata/2]).

-deprecated({'encode!',           1, "use Glazer.JSON.encode!/1 instead"}).
-deprecated({'decode!',           1, "use Glazer.JSON.decode!/1 instead"}).
-deprecated({'encode_to_iodata!', 1, "use Glazer.JSON.encode_to_iodata!/1 instead"}).

-deprecated({stream_decoder,      0, "use decode_start/3"}).
-deprecated({stream_decoder,      1, "use decode_start/3"}).
-deprecated({stream_feed,         2, "use decode_continue/2"}).
-deprecated({stream_eof,          1, "use decode_continue/2"}).

-type decode_opt() ::
    object_as_tuple
  | use_nil
  | {null_term | null, atom()}
  | {keys, atom | existing_atom | binary}
  | dedupe_keys
  | copy_strings
  | return_trailer
  | validate_utf8
  | skip_utf8_validation.

-doc """
Decode options:

- `object_as_tuple`       - decode JSON objects as `{[{K, V}]}` proplists rather than maps
- `use_nil`               - use the atom `nil` for JSON null
- `{null, atom()}`        - use `Atom` for JSON null
- `{null_term, atom()}`   - same as `{null, atom()}`, (**DEPRECATED**)
- `{keys, atom}`          - decode object keys as atoms
- `{keys, existing_atom}` - decode keys as existing atoms, fall back to binary
- `{keys, binary}`        - decode keys as binaries (default)
- `dedupe_keys`           - with `object_as_tuple`, eliminate duplicate object
  keys from the resulting proplist, keeping the last occurrence's value
  (and position). Has no effect when objects are decoded as maps (the
  default) or with `{keys, atom | existing_atom}`: a JSON object with
  duplicate keys is always deduped (last value wins) when decoded to a map,
  since maps cannot represent duplicate keys.
- `copy_strings`          - always allocate a fresh binary for each decoded
  string value, rather than returning a sub-binary that references the
  original input. By default (without this option) unescaped strings are
  zero-copy sub-binaries of the input, which is faster but keeps the entire
  input binary alive in memory as long as any decoded string referencing it
  is reachable. Use `copy_strings` when decoded strings are long-lived and
  the input is large, to allow the GC to reclaim the input buffer
  independently.
- `return_trailer`        - allow (instead of rejecting) trailing
  non-whitespace data after the decoded value. On a match, the successful
  result is `{has_trailer, Term, Rest}` where `Rest` is the unconsumed
  remainder of the input as a zero-copy sub-binary. Without this option,
  trailing non-whitespace data after a complete value is a parse error.
  Useful for decoding one JSON value off the front of a buffer (e.g. a
  newline-delimited stream) without a separate `scan/1,2` pass to find
  where the value ends first.
- `validate_utf8`         - enable UTF-8 validation for JSON strings.
  When enabled, invalid UTF-8 sequences in string values or object keys
  cause a parse error instead of being silently accepted, matching the
  behavior of OTP's json module and other JSON parsers. By default,
  UTF-8 validation is disabled for backward compatibility and performance.
- `skip_utf8_validation`  - explicitly disable UTF-8 validation for JSON
  strings (this is the default behavior). This option is provided for
  clarity when UTF-8 validation behavior might be changed in future versions.
""".
-type decode_opts() :: [decode_opt()].

-type encode_opt() ::
    pretty
  | uescape
  | force_utf8
  | escape_fwd_slash
  | use_nil
  | {null_term | null, atom()}.

-doc """
Encode options:

- `pretty`              - pretty-print the JSON output
- `uescape`             - escape non-ASCII characters as \\uXXXX sequences
- `force_utf8`          - replace invalid UTF-8 byte sequences with the
  Unicode replacement character (U+FFFD) before encoding. Without this
  option, invalid bytes in binaries are copied into the output verbatim,
  which can produce a result that is not valid UTF-8/JSON. A pre-existing
  literal U+FFFD in the input is left untouched (not double-replaced). When
  combined with `uescape`, the replacement character is further escaped to
  `\\ufffd`. This is an *encode*-only option: for UTF-8 validation during
  decoding, use `validate_utf8` in decode options (disabled by default)
- `escape_fwd_slash`    - escape forward slashes (`/`) as `\\/` in JSON strings,
  which is valid per RFC 8259 §7. By default, forward slashes are not escaped
- `use_nil`             - encode the atom `nil` as JSON `null`
- `{null, atom()}`      - encode `Atom` as JSON null
- `{null_term, atom()}` - same as `{null, atom()}`, (**DEPRECATED**)
""".
-type encode_opts() :: [encode_opt()].

-type query_reason() ::
    enomem
  | jq_not_available
  | jq_decode_error
  | {jq_compile_error, binary()}
  | invalid_input
  | binary().

-export_type([decoders/0, stream_decoder/0, continuation_state/0]).

-type scan_state() :: {_, _, _, _, _, _} | undefined.

-record(stream_decoder, {
  opts   = []        :: decode_opts(),
  buffer = <<>>      :: binary(),
  state  = undefined :: scan_state()
}).

-opaque stream_decoder() :: #stream_decoder{}.

%% Decoders are an opaque type used in decoder_start/3

-opaque decoders() :: map() | decode_opts().

-record(decode_continuation, {
  buffer     :: binary(),       % Unparsed remaining data
  scan_state :: scan_state(),   % Scan state for resuming
  opts       :: decode_opts(),  % Decode options
  acc        :: term()          % User accumulator
}).

-opaque continuation_state() :: #decode_continuation{}.

-doc """
Decode a JSON binary or iolist to an Erlang term. JSON objects are returned as
maps (default). Raises `{parse_error, Msg}` on invalid input.

## Examples

```erlang
1> glazer_json:decode(<<"{\"a\":1,\"b\":[true,null,3.5]}">>).
#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}

2> glazer_json:decode(<<"not json">>).
** exception error: {parse_error,<<"...">>}
```
""".
-spec decode(binary() | iolist()) -> term().
decode(Input) ->
  case try_decode(Input) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

-doc """
Decode a JSON binary or iolist to an Erlang term with options
(see `t:decode_opts/0`). Raises `{parse_error, Reason}` on invalid input.

## Examples

```erlang
%% Object keys as atoms
1> glazer_json:decode(<<"{\"a\":1}">>, [{keys, atom}]).
#{a => 1}

%% JSON null as the atom `nil`
2> glazer_json:decode(<<"{\"a\":null}">>, [use_nil]).
#{<<"a">> => nil}

%% Objects as jiffy-style proplist tuples
3> glazer_json:decode(<<"{\"a\":1}">>, [object_as_tuple]).
{[{<<"a">>, 1}]}

%% Trailing data returned alongside the decoded value instead of erroring
4> glazer_json:decode(<<"1 2">>, [return_trailer]).
{has_trailer, 1, <<"2">>}
```
""".
-spec decode(binary() | iolist(), decode_opts()) -> term().
decode(Input, Opts) ->
  case try_decode(Input, Opts) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

-doc """
Decode a JSON binary to an Erlang term. Equivalent to `decode/1`, provided
for API parity with Elixir's `JSON.decode!/1`. Raises `{parse_error,
Reason}` on invalid input.

## NOTE

This function is deprecated. Use `Glazer.JSON.decode!/1` instead.

## Examples

```elixir
1> Glazer.JSON.decode!("{\"a\":1}").
%{"a" => 1}
```
""".
-spec 'decode!'(binary() | iolist()) -> term().
'decode!'(Input) ->
  decode(Input, [use_nil]).

-doc """
Decode a JSON binary or iolist, returning `{ok, Term}` or
`{error, Reason}` instead of raising.

## Examples

```erlang
1> glazer_json:try_decode(<<"{\"a\":1}">>).
{ok, #{<<"a">> => 1}}

2> glazer_json:try_decode(<<"not json">>).
{error, <<"...">>}
```
""".
-spec try_decode(binary() | iolist()) -> {ok, term()} | {error, binary()}.
try_decode(Input) ->
  glazer:json_try_decode(Input).

-doc """
Decode a JSON binary or iolist with options (see `t:decode_opts/0`),
returning `{ok, Term}` or `{error, Reason}` instead of raising.

## Examples

```erlang
1> glazer_json:try_decode(<<"{\"a\":1}">>, [{keys, atom}]).
{ok, #{a => 1}}

2> glazer_json:try_decode(<<"not json">>, [{keys, atom}]).
{error, <<"...">>}

%% Trailing data returned alongside the decoded value instead of erroring
3> glazer_json:try_decode(<<"1 2">>, [return_trailer]).
{ok, {has_trailer, 1, <<"2">>}}
```
""".
-spec try_decode(binary() | iolist(), decode_opts()) -> {ok, term()} | {error, binary()}.
try_decode(Input, Opts) ->
  glazer:json_try_decode(Input, Opts).

-doc """
Encode an Erlang term to a JSON binary.

Raises `{encode_error, {Msg, Term}}` if `Data` contains a value that
cannot be represented as JSON (e.g. an improper list, a pid, or an
unsupported tuple).

## Examples

```erlang
1> glazer_json:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"{\"a\":1,\"b\":[true,null,3.5]}">>

2> glazer_json:encode(<<"hello">>).
<<"\"hello\"">>

3> glazer_json:encode(123456789012345678901234567890).
<<"123456789012345678901234567890">>

4> glazer_json:encode([1|2]).
** exception error: {encode_error,{<<"improper list">>,2}}
```
""".
-spec encode(term()) -> binary().
encode(Data) ->
  glazer:json_encode(Data).

-doc """
Encode an Erlang term to a JSON binary with options (see `t:encode_opts/0`).

Raises `{encode_error, {Msg, Term}}` if `Data` contains a value that
cannot be represented as JSON.

## Examples

```erlang
%% Pretty-print with two-space indentation
1> glazer_json:encode(#{a => 1}, [pretty]).
<<"{\n  \"a\": 1\n}">>

%% Escape non-ASCII characters as \\uXXXX
2> glazer_json:encode(<<"héllo"/utf8>>, [uescape]).
<<"\"h\\u00e9llo\"">>

%% Encode the atom `nil` as JSON null
3> glazer_json:encode(#{<<"a">> => nil}, [use_nil]).
<<"{\"a\":null}">>

%% A binary with an invalid UTF-8 byte (0x80 is a lone continuation byte)
%% is copied through verbatim by default, yielding output that is not
%% valid UTF-8/JSON
4> glazer_json:encode(<<"a", 128, "b">>).
<<"\"a", 128, "b\"">>

%% force_utf8 replaces the invalid byte with U+FFFD (encoded as 0xEF 0xBF 0xBD)
5> glazer_json:encode(<<"a", 128, "b">>, [force_utf8]).
<<"\"a", 239, 191, 189, "b\"">>

%% force_utf8 + uescape further escapes the replacement character
6> glazer_json:encode(<<"a", 128, "b">>, [force_utf8, uescape]).
<<"\"a\\ufffdb\"">>

%% Escape forward slashes in JSON strings
7> glazer_json:encode(<<"https://example.com/path">>, [escape_fwd_slash]).
<<"\"https:\\/\\/example.com\\/path\"">>
```
""".
-spec encode(term(), encode_opts()) -> binary().
encode(Data, Opts) ->
  glazer:json_encode(Data, Opts).

-doc """
Same as `encode/2`, but returns {ok, term()} | {error, term()}
""".
-spec try_encode(term(), encode_opts()) -> {ok, binary()} | {error, badarg | term()}.
try_encode(Data, Opts) ->
  glazer:json_try_encode(Data, Opts).

-doc """
Encode a list of Erlang terms to newline-delimited JSON (NDJSON), with one
JSON value per line. Each value is encoded separately and followed by a newline.

NDJSON is a common interchange format where every line is an independent,
complete JSON value, allowing readers to consume the stream one line at a time.

Raises `{encode_error, {Msg, Term}}` if any value in the list contains a value
that cannot be represented as JSON (e.g. an improper list, a pid, or an
unsupported tuple).

## Examples

```erlang
1> glazer_json:encode_ndjson([#{<<"id">> => 1}, #{<<"id">> => 2}]).
<<"{\"id\":1}\n{\"id\":2}\n">>

2> glazer_json:encode_ndjson([]).
<<>>

3> glazer_json:encode_ndjson([1, <<"hello">>, true], [uescape]).
<<"1\n\"hello\"\ntrue\n">>
```
""".
-spec encode_ndjson([term()]) -> binary().
encode_ndjson(List) ->
  glazer:json_encode_ndjson(List).

-doc """
Encode a list of Erlang terms to newline-delimited JSON (NDJSON) with options
(see `t:encode_opts/0`).

## Examples

```erlang
%% With force_utf8 option
1> glazer_json:encode_ndjson([<<"héllo"/utf8>>], [force_utf8]).
<<"\"héllo\"\n"/utf8>>

%% With uescape option
2> glazer_json:encode_ndjson([<<"café"/utf8>>], [uescape]).
<<"\"caf\\u00e9\"\n">>
```
""".
-spec encode_ndjson([term()], encode_opts()) -> binary().
encode_ndjson(List, Opts) ->
  glazer:json_encode_ndjson(List, Opts).

-doc """
Encode an Erlang term to a JSON binary. Equivalent to `encode/1`, provided
for API parity with Elixir's `JSON.encode!/1`. Raises `{encode_error, Msg}`
if `Data` cannot be encoded.

## NOTE

This function is deprecated. Use `Glazer.JSON.encode!/1` instead.

## Examples

```elixir
1> Glazer.JSON.encode!(%{"a" => 1}).
<<"{\"a\":1}">>
```
""".
-spec 'encode!'(term()) -> binary().
'encode!'(Data) ->
  encode(Data, [use_nil]).

-doc """
Encode an Erlang term to JSON as iodata. Equivalent to `encode/1` (which
already returns a binary, itself valid iodata), provided for API parity with
Elixir's `JSON.encode_to_iodata!/1`. Raises `{encode_error, Msg}` if `Data`
cannot be encoded.

## NOTE

This function is deprecated. Use `Glazer.JSON.encode_to_iodata!/1` instead.

## Examples

```elixir
1> Glazer.JSON.encode_to_iodata!(%{"a" => 1}).
<<"{\"a\":1}">>
```
""".
-spec 'encode_to_iodata!'(term()) -> iodata().
'encode_to_iodata!'(Data) ->
  encode(Data, [use_nil]).

-doc """
Encode an Erlang term to JSON iodata.

This function is identical to `encode/1` but explicitly documents its return
type as `iodata()` for discoverability in protocol implementations (e.g., Elixir's
`Jason.Encoder`). Since `encode/1` already returns a binary (which is valid
iodata), this is a zero-overhead alias.

Raises `{encode_error, {Msg, Term}}` if `Data` contains a value that
cannot be represented as JSON.

This is particularly useful when integrating with Elixir protocol frameworks
that expect an `encode_to_iodata/2` callback matching the `Jason.Encoder`
interface.

## Examples

```erlang
1> glazer_json:encode_to_iodata(#{<<"a">> => 1}).
<<"{\"a\":1}">>

2> glazer_json:encode_to_iodata(123).
<<"123">>
```
""".
-spec encode_to_iodata(term()) -> iodata().
encode_to_iodata(Data) ->
  encode(Data).

-doc """
Encode an Erlang term to JSON iodata with options.

This function is identical to `encode/2` but explicitly documents its return
type as `iodata()` for discoverability in protocol implementations (e.g., Elixir's
`Jason.Encoder`). Since `encode/2` already returns a binary (which is valid
iodata), this is a zero-overhead alias.

Raises `{encode_error, {Msg, Term}}` if `Data` contains a value that
cannot be represented as JSON.

See `encode/2` for available options.

## Examples

```erlang
1> glazer_json:encode_to_iodata(#{a => 1}, [pretty]).
<<"{\n  \"a\": 1\n}">>

2> glazer_json:encode_to_iodata(<<"héllo"/utf8>>, [uescape]).
<<"\"h\\u00e9llo\"">>
```
""".
-spec encode_to_iodata(term(), encode_opts()) -> iodata().
encode_to_iodata(Data, Opts) ->
  encode(Data, Opts).

-doc """
Minify a JSON binary or iolist, removing all unnecessary whitespace.

## Examples

```erlang
1> glazer_json:minify(<<"{\n  \"a\": 1,\n  \"b\": [1, 2, 3]\n}">>).
<<"{\"a\":1,\"b\":[1,2,3]}">>
```
""".
-spec minify(binary() | iolist()) -> binary().
minify(Input) ->
  glazer:json_minify(Input).

-doc """
Pretty-print a JSON binary or iolist with indentation.

## Examples

```erlang
1> glazer_json:prettify(<<"{\"a\":1,\"b\":[1,2,3]}">>).
<<"{\n   \"a\": 1,\n   \"b\": [\n      1,\n      2,\n      3\n   ]\n}">>

2> glazer_json:prettify(<<"{}">>).
<<"{}">>
```
""".
-spec prettify(binary() | iolist()) -> binary().
prettify(Input) ->
  glazer:json_prettify(Input).

-doc """
Read `Filename` and decode its contents as JSON.

Raises `{parse_error, Reason}` if the file's contents aren't valid JSON, or
a binary `"Filename: Reason"` message (see `file:format_error/1`) if the
file can't be read.

## Example

```erlang
1> glazer_json:read_file("data.json").
#{<<"a">> => 1}
```
""".
-spec read_file(file:name_all()) -> term().
read_file(Filename) ->
  read_file(Filename, []).

-doc """
Read `Filename` and decode its contents as JSON, with decode options
(see `decode/2`).
""".
-spec read_file(file:name_all(), decode_opts()) -> term().
read_file(Filename, Opts) ->
  case file:read_file(Filename) of
    {ok, Bin}       -> decode(Bin, Opts);
    {error, Reason} -> error(glazer:format_error("~ts: ~ts", [Filename,file:format_error(Reason)]))
  end.

-doc """
Encode `Data` to JSON and write it to `Filename`, overwriting any existing
file.

Raises a binary `"Filename: Reason"` message (see `file:format_error/1`)
if the file can't be written.

## Example

```erlang
1> glazer_json:write_file("data.json", #{<<"a">> => 1}).
ok
```
""".
-spec write_file(file:name_all(), term()) -> ok.
write_file(Filename, Data) ->
  write_file(Filename, Data, []).

-doc """
Encode `Data` to JSON with encode options (see `encode/2`) and write it to
`Filename`, overwriting any existing file.
""".
-spec write_file(file:name_all(), term(), encode_opts()) -> ok.
write_file(Filename, Data, Opts) ->
  case file:write_file(Filename, encode(Data, Opts)) of
    ok              -> ok;
    {error, Reason} -> error(glazer:format_error("~ts: ~ts", [Filename, file:format_error(Reason)]))
  end.

-doc """
Run a [jq](https://jqlang.org/) `Filter` program against a JSON binary or
iolist `Input`, returning one Erlang term per value produced by the filter
(in the order they are emitted by jq).

Requires `glazer` to have been built against `libjq`; if `libjq` was not
available at build time, this returns `{error, jq_not_available}`.

A runtime error raised by the filter itself (e.g. via jq's `error/0,1`) is
returned as `{error, Msg}` where `Msg` is the binary message produced by jq.

## Examples

```erlang
1> glazer_json:query(<<"{\"a\":[1,2,3]}">>, <<".a[]">>).
{ok,[1,2,3]}

2> glazer_json:query(<<"{\"a\":1}">>, <<".b">>).
{ok,[null]}

3> glazer_json:query(<<"not json">>, <<".">>).
{error, invalid_input}
```
""".
-spec query(binary() | iolist(), binary() | iolist()) ->
  {ok, [term()]} | {error, query_reason()}.
query(Input, Filter) ->
  glazer:json_query(Input, Filter).

-doc """
Like `query/2`, but decodes each result term using `JSONDecodeOpts`
(see `decode/2`).

## Examples

```erlang
1> glazer_json:query(<<"{\"a\":[1,2,3]}">>, <<".a">>, [{keys, atom}]).
{ok, [[1,2,3]]}

2> glazer_json:query(<<"{\"a\":null}">>, <<".a">>, [use_nil]).
{ok, [nil]}
```
""".
-spec query(binary() | iolist(), binary() | iolist(), decode_opts()) ->
  {ok, [term()]} | {error, query_reason()}.
query(Input, Filter, JSONDecodeOpts) ->
  glazer:json_query(Input, Filter, JSONDecodeOpts).

-doc """
Locate the end of the next complete top-level JSON value in `Bin`, without
decoding it.

Returns:

- `{complete, EndOffset}` - a complete value spans `binary:part(Bin, 0,
  EndOffset)`; the rest of `Bin` (if any) is left over for the next call
- `{incomplete, ScanState}` - `Bin` doesn't yet contain a complete value;
  feed more data via `scan/2` once it's available, passing the *entire
  unconsumed remainder* (this `Bin`, with new bytes appended) plus
  `ScanState`

This is the low-level primitive behind [`stream_feed/2`](`stream_feed/2`);
most callers should use the `stream_*` API instead.

## Example

Slicing off complete values from a buffer of concatenated JSON:

```erlang
1> Buf0 = <<"{\"a\":1} {\"b\":2}">>,
2> {complete, End1} = glazer_json:scan(Buf0).
{complete, 7}
3> <<Val1:End1/binary, Buf1/binary>> = Buf0,
4> Val1.
<<"{\"a\":1}">>
5> Buf1.
<<" {\"b\":2}">>
6> {complete, End2} = glazer_json:scan(Buf1).
{complete, 8}
```

Resuming a scan once more bytes arrive:

```erlang
1> {incomplete, S0} = glazer_json:scan(<<"{\"a\":">>).
{incomplete, {6,1,false,false,true,false}}
2> glazer_json:scan(<<"{\"a\":1}">>, S0).
{complete, 7}
```
""".
-spec scan(binary() | iolist()) ->
  {complete, non_neg_integer()} | {incomplete, scan_state()}.
scan(Bin) ->
  scan(Bin, undefined).

-doc """
Resume scanning `Bin` (the unconsumed remainder plus newly-appended bytes)
from `ScanState`.

## Examples

```erlang
1> {incomplete, S0} = glazer_json:scan(<<"[1, 2,">>).
{incomplete, {6,1,false,false,true,false}}
2> glazer_json:scan(<<"[1, 2, 3]">>, S0).
{complete, 9}
```
""".
-spec scan(binary() | iolist(), scan_state()) ->
  {complete, non_neg_integer()} | {incomplete, scan_state()}.
scan(Bin, ScanState) ->
  glazer:json_scan(Bin, ScanState).

%%%----------------------------------------------------------------------------
%%% Streaming / incremental decode
%%%----------------------------------------------------------------------------

-doc """
**DEPRECATED**: Use [`decode_start/3`](`decode_start/3`) and
[`decode_continue/2`](`decode_continue/2`) instead.

Create a new incremental decoder for feeding JSON in chunks (e.g. from a
socket or file), useful when a complete document isn't available up front
or when a stream contains a sequence of concatenated/whitespace-separated
JSON values (e.g. newline-delimited JSON).

Decoding itself is **not** incremental — each complete top-level value is
still decoded in a single pass via [`decode/2`](`decode/2`) using the
library's fast whole-buffer decoder. Only the *boundary detection* (finding
where one value ends and the next begins) is incremental, via a small
byte-scanner that tracks nesting/string state across chunks.

> **Note**: This function parses ALL complete values in the input and returns
> them as a list. Use [`decode_start/3`](`decode_start/3`) if you need to parse
> one value at a time with the unparsed remainder preserved.

## Example

```erlang
1> D0 = glazer_json:stream_decoder(),
2> {[#{<<"a">> => 1}],  D1} = glazer_json:stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
3> {[#{<<"b">> => 2}], _D2} = glazer_json:stream_feed(D1, <<"2}">>),
```
""".
-spec stream_decoder() -> stream_decoder().
stream_decoder() ->
  stream_decoder([]).

-doc """
Create a new incremental decoder, passing `Opts` through to every
[`decode/2`](`decode/2`) call.

**DEPRECATED**: Use [`decode_start/3`](`decode_start/3`) instead, which accepts
decode options directly.
""".
-spec stream_decoder(decode_opts()) -> stream_decoder().
stream_decoder(Opts) when is_list(Opts) ->
  #stream_decoder{opts = Opts}.

-doc """
**DEPRECATED**: Use [`decode_continue/2`](`decode_continue/2`) instead for one-value-at-a-time
parsing with remainder preservation. Use `stream_feed/2` only if you need to
collect all complete values in a single chunk.

Feed a chunk of bytes into the decoder, returning any complete JSON values
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`decode/2`](`decode/2`) (e.g.
`Reason`) if a value that the scanner deemed complete fails
to decode.

> **Important Limitation**: This function parses ALL complete values and returns
> them as a list. If you need to parse one value at a time or preserve the
> unparsed remainder for incremental processing, use
> [`decode_start/3`](`decode_start/3`) and [`decode_continue/2`](`decode_continue/2`)
> instead, which return one value per call with the unparsed Rest buffer.

## Example (Deprecated Pattern)

Call `stream_feed/2` for each chunk received from the source while more
data may still arrive, and [`stream_eof/1`](`stream_eof/1`) once the source
is exhausted to flush any trailing value:

```erlang
loop(Socket, D0) ->
  case gen_tcp:recv(Socket, 0) of
    {ok, Chunk} ->
      {Vals, D1} = glazer_json:stream_feed(D0, Chunk),
      handle_values(Vals),
      loop(Socket, D1);
    {error, closed} ->
      case glazer_json:stream_eof(D0) of
        {ok, Trailing}  -> handle_values(Trailing);
        {error, Reason} -> handle_truncated_stream(Reason)
      end
  end.
```

## Better Alternative (Recommended)

Use [`decode_continue/2`](`decode_continue/2`) for cleaner streaming code:

```erlang
loop(Socket, State0) ->
  case gen_tcp:recv(Socket, 0) of
    {ok, Chunk} ->
      case glazer_json:decode_continue(Chunk, State0) of
        {Val, Acc, Rest} ->
          handle_value(Val),
          % To parse more values from Rest, call decode_start(Rest, Acc, [])
          loop(Socket, new_state_for_rest(Rest, Acc));
        {continue, State1} ->
          loop(Socket, State1)
      end;
    {error, closed} ->
      case glazer_json:decode_continue(end_of_input, State0) of
        {Val, _Acc, _Rest} -> handle_value(Val);
        {nil, _Acc, _Rest} -> ok;
        {continue, _}       -> handle_error("incomplete value at EOF")
      end
  end.
```
""".
-spec stream_feed(stream_decoder(), binary() | iolist()) -> {[term()], stream_decoder()}.
stream_feed(#stream_decoder{buffer = Buf} = D, Chunk) ->
  NewBuf = iolist_to_binary([Buf, Chunk]),
  stream_drain(D#stream_decoder{buffer = NewBuf, state = undefined}, []).

stream_drain(#stream_decoder{buffer = Buf, opts = Opts} = D, Acc) ->
  case scan(Buf) of
    {complete, End} ->
      <<ValueBin:End/binary, Rest/binary>> = Buf,
      Term = decode(ValueBin, Opts),
      stream_drain(D#stream_decoder{buffer = Rest, state = undefined}, [Term | Acc]);
    {incomplete, NewSt} ->
      {lists:reverse(Acc), D#stream_decoder{state = NewSt}}
  end.

-doc """
**DEPRECATED**: Use [`decode_continue/2`](`decode_continue/2`) with
`end_of_input` instead.

Signal end-of-stream: decode any remaining buffered bytes as a final value
(useful for a trailing bare scalar, e.g. a lone number or `true`/`null`,
which the scanner can't otherwise distinguish from a value that's still
being written to mid-chunk).

Returns `{ok, [Term]}` with zero or one trailing value, or `{error,
Reason}` if the remaining bytes don't form a complete value.

## Example (Deprecated)

```erlang
1> D0 = glazer_json:stream_decoder(),
2> {Vals1, D1} = glazer_json:stream_feed(D0, <<"123">>),
3> Vals1.
[]
4> glazer_json:stream_eof(D1).
{ok, [123]}
```

## Recommended Alternative

Use [`decode_continue/2`](`decode_continue/2`) with `end_of_input`:

```erlang
1> {continue, S0} = glazer_json:decode_start(<<"123">>, ok, []),
2> glazer_json:decode_continue(end_of_input, S0).
{123, ok, <<>>}
```

A stream that ends mid-value (e.g. a dropped connection) yields an error
instead of silently dropping the partial data (same behavior as deprecated version):

```erlang
1> {continue, S0} = glazer_json:decode_start(<<"{\"a\":1, \"b\":">>, ok, []),
2> glazer_json:decode_continue(end_of_input, S0).
** exception error: {parse_error, ...}
[]
4> glazer_json:stream_eof(D1).
{error, _Reason}
```
""".
-spec stream_eof(stream_decoder()) -> {ok, [term()]} | {error, term()}.
stream_eof(#stream_decoder{buffer = Buf, opts = Opts}) ->
  case is_blank(Buf) of
    true  -> {ok, []};
    false ->
      try decode(Buf, Opts) of
        Term -> {ok, [Term]}
      catch
        error:Reason -> {error, Reason}
      end
  end.

%% True if `Bin` is empty or contains only JSON whitespace (space, tab, CR, LF).
is_blank(Bin) ->
  lists:all(fun(B) -> B =:= $\s orelse B =:= $\t orelse B =:= $\r orelse B =:= $\n end,
            binary_to_list(Bin)).

%%%----------------------------------------------------------------------------
%%% Incremental decode with custom decoders (OTP json.erl compatibility)
%%%----------------------------------------------------------------------------

-doc """
Start incremental (streaming) JSON decoding.

This is the **recommended** function for streaming JSON. It parses exactly one
JSON value per call and returns the unparsed remainder in the `Rest` buffer.
Compatible with OTP `json:decode_start/3`.

Returns either:

- `{Result, Acc, Rest}` - a complete JSON value was decoded; `Rest` contains
  any unparsed data (next values, whitespace, etc.)
- `{continue, State}` - more data needed; feed via [`decode_continue/2`](`decode_continue/2`)
  or provide more data

The `Decoders` parameter is accepted for API compatibility with `json` module, however
it's primarily used for passing decoder options — custom decoder callbacks are silently
ignored and results always follow glazer's standard decoding. Pass an empty map `#{}`
or empty list `[]` for compatibility with `json:decode_start/3`. If passed a map, the
implementation converts it to a list, and only `{null: atom()}` value is
meaningful.

The `Acc` parameter is a user-provided accumulator that is returned unchanged
in the result, useful for passing context through the streaming parse.

## Examples

Parsing an incomplete value across chunks:

```erlang
1> {continue, State} = glazer_json:decode_start(<<"{\"a\":">>, ok, []),
2> glazer_json:decode_continue(<<"1}">>, State).
{#{<<"a">> => 1}, ok, <<>>}
```

A complete value in one call:

```erlang
1> glazer_json:decode_start(<<"123">>, my_acc, []).
{123, my_acc, <<>>}
```

Parsing multiple values from a single input:

```erlang
{[1], ok, Rest1} = glazer_json:decode_start(<<"[1][2][3]">>, ok, []),
% Rest1 = <<"[2][3]">>
{[2], ok, Rest2} = glazer_json:decode_start(Rest1, ok, []),
% Rest2 = <<"[3]">>
{[3], ok, <<>>} = glazer_json:decode_start(Rest2, ok, []).
```

Socket streaming with `decode_continue`:

```erlang
recv_json(Socket, State0) ->
  case gen_tcp:recv(Socket, 1024) of
    {ok, Chunk} ->
      case glazer_json:decode_continue(Chunk, State0) of
        {Value, Acc, _Rest} ->
          handle_value(Value),
          recv_json(Socket, State0);
        {continue, State1} ->
          recv_json(Socket, State1)
      end;
    {error, closed} ->
      case glazer_json:decode_continue(end_of_input, State0) of
        {Value, _Acc, _Rest} -> handle_value(Value);
        {nil, _Acc, _Rest}   -> ok;
        {continue, _}        -> handle_error("incomplete value")
      end
  end.
```
""".
-spec decode_start(binary() | iolist(), Acc :: term(), decoders()) ->
  {Result :: term(), Acc :: term(), Rest :: binary()} | {continue, continuation_state()}.
decode_start(Input, Acc, Decoders) when is_binary(Input) ->
  Opts = normalize_decoders(Decoders),
  decode_one_value(Input, Opts, Acc, undefined);
decode_start(Input, Acc, Decoders) when is_list(Input) ->
  Opts = normalize_decoders(Decoders),
  decode_one_value(iolist_to_binary(Input), Opts, Acc, undefined);
decode_start(_Input, _Acc, _Decoders) ->
  error(badarg).

-doc """
Resume incremental JSON decoding with new data or signal end of stream.

This is the companion to [`decode_start/3`](`decode_start/3`) for streaming scenarios
where JSON data arrives in chunks. Call with:

- A `binary()` or `iolist()` to feed more data and attempt to parse one value
- The atom `end_of_input` to signal no more data is coming (flushes buffered
  bare scalars like `123` that can't be distinguished from incomplete values)

Returns either:

- `{Result, Acc, Rest}` - a complete JSON value was decoded
- `{continue, State}` - more data needed; call again with more data or `end_of_input`
- `{nil, Acc, <<>>}` - when `end_of_input` is called with no buffered data

Raises `{parse_error, Reason}` if an incomplete value is at EOF.

## Examples

Parsing an incomplete value across chunks:

```erlang
1> {continue, S0} = glazer_json:decode_start(<<"{\"x\":">>, ok, []),
2> glazer_json:decode_continue(<<"1}">>, S0).
{#{<<"x">> => 1}, ok, <<>>}
```

A bare scalar that requires `end_of_input` to resolve:

```erlang
1> {continue, S0} = glazer_json:decode_start(<<"123">>, ok, []),
2> glazer_json:decode_continue(end_of_input, S0).
{123, ok, <<>>}
```

An incomplete value at EOF (error):

```erlang
1> {continue, S0} = glazer_json:decode_start(<<"{\"a\":">>, ok, []),
2> glazer_json:decode_continue(end_of_input, S0).
** exception error: {parse_error, ...}
```

## Socket Streaming Pattern

```erlang
-record(json_state, {cont_state, acc, handler}).

process_chunk(Chunk, #json_state{cont_state = S0} = State) ->
  case glazer_json:decode_continue(Chunk, S0) of
    {Value, Acc, _Rest} ->
      % Got one value, handler processes it
      handle_json(Value, State#json_state{acc = Acc});
    {continue, S1} ->
      % Need more data
      State#json_state{cont_state = S1};
    {nil, Acc, _Rest} ->
      % Empty data at EOF
      State#json_state{acc = Acc}
  end.

on_socket_close(#json_state{cont_state = S0} = State) ->
  case glazer_json:decode_continue(end_of_input, S0) of
    {Value, Acc, _Rest} ->
      handle_json(Value, State#json_state{acc = Acc});
    {continue, _} ->
      log_error("incomplete JSON at EOF");
    {error, Reason} ->
      log_error({parse_error, Reason})
  end.
```
""".
-spec decode_continue(binary() | iolist() | end_of_input, State :: continuation_state()) ->
  {Result :: term() | nil, Acc :: term(), Rest :: binary()} | {continue, continuation_state()}.
decode_continue(end_of_input, #decode_continuation{buffer = Buf, opts = Opts, acc = Acc}) ->
  %% Signal end of input: try to decode remaining buffer as final value
  case Buf of
    <<>> ->
      %% No data left
      {nil, Acc, <<>>};
    _ ->
      %% Try to decode remaining buffer as a complete value
      try decode(Buf, Opts) of
        Val -> {Val, Acc, <<>>}
      catch
        error:{parse_error, _Reason} ->
          %% Incomplete value at EOF
          error({parse_error, "incomplete value at end of stream"})
      end
  end;
decode_continue(Input, #decode_continuation{buffer = Buf, scan_state = ScanState, opts = Opts, acc = Acc})
  when is_binary(Input) ->
  %% Append new input to buffer and try to parse one value
  NewBuf = iolist_to_binary([Buf, Input]),
  decode_one_value(NewBuf, Opts, Acc, ScanState);
decode_continue(Input, #decode_continuation{buffer = Buf, scan_state = ScanState, opts = Opts, acc = Acc})
  when is_list(Input) ->
  %% Append new input to buffer and try to parse one value
  NewBuf = iolist_to_binary([Buf, iolist_to_binary(Input)]),
  decode_one_value(NewBuf, Opts, Acc, ScanState);
decode_continue(_Input, _State) ->
  error(badarg).

%% Helper: normalize decoders map/list to decode options
normalize_decoders(List) when is_list(List) ->
  List;
normalize_decoders(Map) when is_map(Map) ->
  maps:to_list(Map).

%% Core implementation: Parse exactly one JSON value from buffer
%% Returns either a complete value or a continuation state
decode_one_value(Buf, Opts, Acc, ScanState) ->
  case scan(Buf, ScanState) of
    {complete, End} ->
      %% Found complete value, extract and decode it
      <<ValueBin:End/binary, Rest/binary>> = Buf,
      Val = decode(ValueBin, Opts),
      {Val, Acc, Rest};
    {incomplete, NewScanState} ->
      %% Need more data, return continuation state
      {continue, #decode_continuation{
        buffer = Buf,
        scan_state = NewScanState,
        opts = Opts,
        acc = Acc
      }}
  end.
