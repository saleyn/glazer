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
- Incremental/streaming decoding of partial input (e.g. NDJSON over a
  socket) via `stream_decoder/0,1`, `stream_feed/2`, `stream_eof/1`
- Configurable representation of JSON `null` and JSON object keys
- `minify/1` and `prettify/1` helpers
- `read_file/1,2` and `write_file/2,3` helpers for decoding/encoding
  directly to/from a file
- `query/2,3`: run a [jq](https://jqlang.org/) filter over a JSON
  document, returning decoded Erlang terms (requires `glazer` to be built
  with `libjq` available)
""".
-export([decode/1, decode/2, try_decode/1, try_decode/2,
         encode/1, encode/2, minify/1, prettify/1,
         query/2, query/3,
         scan/1, scan/2,
         read_file/1, read_file/2, write_file/2, write_file/3,
         stream_decoder/0, stream_decoder/1, stream_feed/2, stream_eof/1]).

-type decode_opt() ::
    object_as_tuple
  | use_nil
  | {null_term, atom()}
  | {keys, atom | existing_atom | binary}
  | dedupe_keys.

-doc """
Decode options:

- `object_as_tuple`       - decode JSON objects as `{[{K, V}]}` proplists rather than maps
- `use_nil`               - use the atom `nil` for JSON null
- `{null_term, Atom}`     - use `Atom` for JSON null
- `{keys, atom}`          - decode object keys as atoms
- `{keys, existing_atom}` - decode keys as existing atoms, fall back to binary
- `{keys, binary}`        - decode keys as binaries (default)
- `dedupe_keys`           - with `object_as_tuple`, eliminate duplicate object
  keys from the resulting proplist, keeping the last occurrence's value
  (and position). Has no effect when objects are decoded as maps (the
  default) or with `{keys, atom | existing_atom}`: a JSON object with
  duplicate keys is always deduped (last value wins) when decoded to a map,
  since maps cannot represent duplicate keys.
""".
-type decode_opts() :: [decode_opt()].

-type encode_opt() ::
    pretty
  | uescape
  | force_utf8
  | use_nil
  | {null_term, atom()}.

-doc """
Encode options:

- `pretty`            - pretty-print the JSON output
- `uescape`           - escape non-ASCII characters as \\uXXXX sequences
- `force_utf8`        - replace invalid UTF-8 byte sequences with the
  Unicode replacement character (U+FFFD) before encoding. Without this
  option, invalid bytes in binaries are copied into the output verbatim,
  which can produce a result that is not valid UTF-8/JSON. A pre-existing
  literal U+FFFD in the input is left untouched (not double-replaced). When
  combined with `uescape`, the replacement character is further escaped to
  `\\ufffd`. This is an *encode*-only option: `decode/1,2` does not validate
  that JSON strings in the input are valid UTF-8 and copies bytes through
  as-is regardless of options
- `use_nil`           - encode the atom `nil` as JSON `null`
- `{null_term, Atom}` - encode `Atom` as JSON `null`
""".
-type encode_opts() :: [encode_opt()].

-type query_reason() ::
    enomem
  | jq_not_available
  | jq_decode_error
  | {jq_compile_error, binary()}
  | invalid_input
  | binary().

-export_type([decode_opt/0, decode_opts/0, encode_opt/0, encode_opts/0, query_reason/0,
               scan_state/0, stream_decoder/0]).

-type scan_state() :: tuple().

-record(stream_decoder, {
  opts   = []        :: decode_opts(),
  buffer = <<>>      :: binary(),
  state  = undefined :: scan_state() | undefined
}).

-opaque stream_decoder() :: #stream_decoder{}.

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
```
""".
-spec decode(binary() | iolist(), decode_opts()) -> term().
decode(Input, Opts) ->
  case try_decode(Input, Opts) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

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
```
""".
-spec try_decode(binary() | iolist(), decode_opts()) -> {ok, term()} | {error, binary()}.
try_decode(Input, Opts) ->
  glazer:json_try_decode(Input, Opts).

-doc """
Encode an Erlang term to a JSON binary.

## Examples

```erlang
1> glazer_json:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"{\"a\":1,\"b\":[true,null,3.5]}">>

2> glazer_json:encode(<<"hello">>).
<<"\"hello\"">>

3> glazer_json:encode(123456789012345678901234567890).
<<"123456789012345678901234567890">>
```
""".
-spec encode(term()) -> binary().
encode(Data) ->
  glazer:json_encode(Data).

-doc """
Encode an Erlang term to a JSON binary with options (see `t:encode_opts/0`).

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
```
""".
-spec encode(term(), encode_opts()) -> binary().
encode(Data, Opts) ->
  glazer:json_encode(Data, Opts).

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
Like `query/2`, but decodes each result term using `DecodeOpts`
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
query(Input, Filter, DecodeOpts) ->
  glazer:json_query(Input, Filter, DecodeOpts).

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
  glazer:json_scan(Bin).

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
Create a new incremental decoder for feeding JSON in chunks (e.g. from a
socket or file), useful when a complete document isn't available up front
or when a stream contains a sequence of concatenated/whitespace-separated
JSON values (e.g. newline-delimited JSON).

Decoding itself is **not** incremental — each complete top-level value is
still decoded in a single pass via [`decode/2`](`decode/2`) using the
library's fast whole-buffer decoder. Only the *boundary detection* (finding
where one value ends and the next begins) is incremental, via a small
byte-scanner that tracks nesting/string state across chunks.

## Example

```erlang
1> D0 = glazer_json:stream_decoder(),
2> {Vals1, D1} = glazer_json:stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
3> Vals1.
[#{<<"a">> => 1}]
4> {Vals2, _D2} = glazer_json:stream_feed(D1, <<"2}">>),
5> Vals2.
[#{<<"b">> => 2}]
```
""".
-spec stream_decoder() -> stream_decoder().
stream_decoder() ->
  stream_decoder([]).

-doc """
Create a new incremental decoder, passing `Opts` through to every
[`decode/2`](`decode/2`) call.
""".
-spec stream_decoder(decode_opts()) -> stream_decoder().
stream_decoder(Opts) when is_list(Opts) ->
  #stream_decoder{opts = Opts}.

-doc """
Feed a chunk of bytes into the decoder, returning any complete JSON values
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`decode/2`](`decode/2`) (e.g.
`Reason`) if a value that the scanner deemed complete fails
to decode.

## Example

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

The same decoder fits naturally into a `gen_server` driving an
active-mode socket: keep the `stream_decoder()` in the process state,
feed it from `handle_info({tcp, ...})`, and flush it on
`{tcp_closed, ...}`:

```erlang
-module(json_conn).
-behaviour(gen_server).
-export([start_link/1]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2]).

-record(state, {socket, decoder}).

start_link(Socket) ->
  gen_server:start_link(?MODULE, Socket, []).

init(Socket) ->
  inet:setopts(Socket, [{active, once}]),
  {ok, #state{socket = Socket, decoder = glazer_json:stream_decoder()}}.

handle_info({tcp, Socket, Data}, #state{socket = Socket, decoder = D0} = State) ->
  {Vals, D1} = glazer_json:stream_feed(D0, Data),
  lists:foreach(fun handle_value/1, Vals),
  inet:setopts(Socket, [{active, once}]),
  {noreply, State#state{decoder = D1}};

handle_info({tcp_closed, Socket}, #state{socket = Socket, decoder = D0} = State) ->
  case glazer_json:stream_eof(D0) of
    {ok, Trailing}  -> lists:foreach(fun handle_value/1, Trailing);
    {error, Reason} -> handle_truncated_stream(Reason)
  end,
  {stop, normal, State};

handle_info({tcp_error, Socket, Reason}, #state{socket = Socket} = State) ->
  {stop, Reason, State}.

handle_call(_Request, _From, State) -> {reply, ok, State}.
handle_cast(_Request, State)        -> {noreply, State}.

handle_value(Val) ->
  io:format("received: ~p~n", [Val]).
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
Signal end-of-stream: decode any remaining buffered bytes as a final value
(useful for a trailing bare scalar, e.g. a lone number or `true`/`null`,
which the scanner can't otherwise distinguish from a value that's still
being written to mid-chunk).

Returns `{ok, [Term]}` with zero or one trailing value, or `{error,
Reason}` if the remaining bytes don't form a complete value.

## Example

```erlang
1> D0 = glazer_json:stream_decoder(),
2> {Vals1, D1} = glazer_json:stream_feed(D0, <<"123">>),
3> Vals1.
[]
4> glazer_json:stream_eof(D1).
{ok, [123]}
```

A stream that ends mid-value (e.g. a dropped connection) yields an error
instead of silently dropping the partial data:

```erlang
1> D0 = glazer_json:stream_decoder(),
2> {Vals1, D1} = glazer_json:stream_feed(D0, <<"{\"a\":1, \"b\":">>),
3> Vals1.
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