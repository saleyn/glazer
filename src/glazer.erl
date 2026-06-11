-module(glazer).
-moduledoc """
Fast JSON encoding and decoding using the glaze C++ library.

By default JSON `null` is represented as the atom `null`. To change it
application-wide, set the `null` env key in your config:
```
{glazer, [{null, nil}]}.
```

See also [https://github.com/stephenberry/glaze]
""".
-export([json_decode/1, json_decode/2, json_try_decode/1, json_try_decode/2,
         json_encode/1, json_encode/2, json_minify/1, json_prettify/1,
         encode_integer/1, decode_integer/1, try_decode_integer/1,
         json_scan/1, json_scan/2,
         json_stream_decoder/0, json_stream_decoder/1, json_stream_feed/2, json_stream_eof/1,
         yaml_decode/1, yaml_decode/2, yaml_try_decode/1, yaml_try_decode/2,
         yaml_encode/1, yaml_encode/2,
         csv_decode/1, csv_decode/2, csv_try_decode/1, csv_try_decode/2,
         csv_encode/1, csv_encode/2,
         csv_stream_decoder/0, csv_stream_decoder/1, csv_stream_feed/2, csv_stream_eof/1]).

-on_load(init/0).

-define(LIBNAME, glazer).
-define(NOT_LOADED_ERROR,
  erlang:nif_error({not_loaded, [{module, ?MODULE}, {line, ?LINE}]})).

-type decode_opt() ::
    object_as_tuple
  | use_nil
  | {null_term, atom()}
  | {keys, atom | existing_atom | binary}
  | dedupe_keys.

-doc """
Decode options:

- `object_as_tuple`     - decode JSON objects as `{[{K, V}]}` proplists rather than maps
- `use_nil`             - use the atom `nil` for JSON null
- `{null_term, Atom}`   - use `Atom` for JSON null
- `{keys, atom}`        - decode object keys as atoms
- `{keys, existing_atom}` - decode keys as existing atoms, fall back to binary
- `{keys, binary}`      - decode keys as binaries (default)
- `dedupe_keys`         - with `object_as_tuple`, eliminate duplicate object
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
- `force_utf8`        - fix invalid UTF-8 sequences before encoding
- `use_nil`           - encode the atom `nil` as JSON `null`
- `{null_term, Atom}` - encode `Atom` as JSON `null`
""".
-type encode_opts() :: [encode_opt()].

-type yaml_decode_opt() ::
    use_nil
  | {null_term, atom()}
  | {keys, atom | existing_atom | binary}
  | yaml_1_1_bools.

-doc """
YAML decode options:

- `use_nil`             - use the atom `nil` for YAML `null`/`~`/empty values
- `{null_term, Atom}`   - use `Atom` for YAML `null`/`~`/empty values
- `{keys, atom}`        - decode mapping keys as atoms
- `{keys, existing_atom}` - decode mapping keys as existing atoms, fall back to binary
- `{keys, binary}`      - decode mapping keys as binaries (default)
- `yaml_1_1_bools`      - additionally treat `yes`/`no`/`on`/`off` (and case
  variants) as booleans, per the YAML 1.1 core schema. By default (YAML 1.2
  core schema) only `true`/`false` are recognized as booleans.
""".
-type yaml_decode_opts() :: [yaml_decode_opt()].

-type yaml_encode_opt() ::
    use_nil
  | {null_term, atom()}.

-doc """
YAML encode options:

- `use_nil`           - treat the atom `nil` as YAML `null`
- `{null_term, Atom}` - treat `Atom` as YAML `null`
""".
-type yaml_encode_opts() :: [yaml_encode_opt()].

-type csv_decode_opt() ::
    {delimiter, char()}
  | headers
  | {keys, atom | existing_atom | binary}.

-doc """
CSV decode options:

- `{delimiter, Char}`   - field delimiter (default `$,`)
- `headers`             - treat the first row as column names and decode
  each subsequent row as a map keyed by those names, instead of returning
  every row as a list of fields
- `{keys, atom}`        - with `headers`, decode column names as atoms
- `{keys, existing_atom}` - with `headers`, decode column names as existing
  atoms, falling back to binaries for unknown atoms
- `{keys, binary}`      - with `headers`, decode column names as binaries
  (default)
""".
-type csv_decode_opts() :: [csv_decode_opt()].

-type csv_encode_opt() ::
    {delimiter, char()}
  | headers
  | {line_ending, lf | crlf}.

-doc """
CSV encode options:

- `{delimiter, Char}`  - field delimiter (default `$,`)
- `headers`            - input is a list of maps; the first map's keys
  become the header row, and subsequent maps are encoded as rows in that
  column order (missing keys produce empty fields)
- `{line_ending, lf | crlf}` - line terminator (default `crlf`, per RFC 4180)
""".
-type csv_encode_opts() :: [csv_encode_opt()].

-export_type([decode_opts/0, encode_opts/0, yaml_decode_opts/0, yaml_encode_opts/0,
               csv_decode_opts/0, csv_encode_opts/0]).

-type scan_state() :: tuple().

-record(json_stream_decoder, {
  opts   = []        :: decode_opts(),
  buffer = <<>>      :: binary(),
  state  = undefined :: scan_state() | undefined
}).

-opaque json_stream_decoder() :: #json_stream_decoder{}.
-export_type([json_stream_decoder/0]).

-type csv_scan_state() :: {non_neg_integer(), boolean()}.

-record(csv_stream_decoder, {
  opts    = []        :: csv_decode_opts(),
  buffer  = <<>>      :: binary(),
  header  = undefined :: [binary()] | undefined,
  state   = {0, false} :: csv_scan_state()
}).

-opaque csv_stream_decoder() :: #csv_stream_decoder{}.
-export_type([csv_stream_decoder/0]).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").
-endif.

init() ->
  NullVal = application:get_env(?LIBNAME, null, null),
  is_atom(NullVal) orelse erlang:error("glazer: option 'null' must be an atom"),
  SoName  =
    case code:priv_dir(?LIBNAME) of
      {error, bad_name} ->
        case code:which(?MODULE) of
          Filename when is_list(Filename) ->
            Dir = filename:dirname(filename:dirname(Filename)),
            filename:join([Dir, "priv", ?LIBNAME]);
          _ ->
            filename:join("../priv", ?LIBNAME)
        end;
      Dir ->
        filename:join(Dir, ?LIBNAME)
    end,
  erlang:load_nif(SoName, [{null, NullVal}]).

-doc """
Decode a JSON binary or iolist to an Erlang term. JSON objects are returned as
maps (default). Raises `{parse_error, Msg}` on invalid input.
""".
-spec json_decode(binary() | iolist()) -> term().
json_decode(Input) ->
  case json_try_decode(Input) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

-doc """
Decode a JSON binary or iolist to an Erlang term with options.
Raises `{parse_error, Reason}` on invalid input.
""".
-spec json_decode(binary() | iolist(), decode_opts()) -> term().
json_decode(Input, Opts) ->
  case json_try_decode(Input, Opts) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

-doc """
Decode a JSON binary or iolist, returning `{ok, Term}` or
`{error, Reason}` instead of raising.
""".
-spec json_try_decode(binary() | iolist()) -> {ok, term()} | {error, binary()}.
json_try_decode(_Input) ->
  ?NOT_LOADED_ERROR.

-doc """
Decode a JSON binary or iolist with options, returning `{ok, Term}` or
`{error, Reason}` instead of raising.
""".
-spec json_try_decode(binary() | iolist(), decode_opts()) -> {ok, term()} | {error, binary()}.
json_try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc """
Decode a YAML binary or iolist to an Erlang term. YAML mappings are returned
as maps (default). Raises `{parse_error, Reason}` on invalid input.
""".
-spec yaml_decode(binary() | iolist()) -> term().
yaml_decode(Input) ->
  case yaml_try_decode(Input) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error(Reason)
  end.

-doc """
Decode a YAML binary or iolist to an Erlang term with options.
Raises `{parse_error, Msg}` on invalid input.
""".
-spec yaml_decode(binary() | iolist(), yaml_decode_opts()) -> term().
yaml_decode(Input, Opts) ->
  case yaml_try_decode(Input, Opts) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

-doc """
Decode a YAML binary or iolist, returning `{ok, Term}` or
`{error, Msg}` instead of raising.
""".
-spec yaml_try_decode(binary() | iolist()) -> {ok, term()} | {error, binary()}.
yaml_try_decode(_Input) ->
  ?NOT_LOADED_ERROR.

-doc """
Decode a YAML binary or iolist with options, returning `{ok, Term}` or
`{error, Msg}` instead of raising.
""".
-spec yaml_try_decode(binary() | iolist(), yaml_decode_opts()) -> {ok, term()} | {error, binary()}.
yaml_try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc """
Encode an Erlang term to a YAML binary in block style (2-space indentation,
sequences at the same indentation as the mapping key that owns them).
""".
-spec yaml_encode(term()) -> binary().
yaml_encode(Data) ->
  yaml_encode(Data, []).

-doc "Encode an Erlang term to a YAML binary in block style with options.".
-spec yaml_encode(term(), yaml_encode_opts()) -> binary().
yaml_encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc """
Decode a CSV binary or iolist to a list of rows.

By default each row is a list of binary fields. With the `headers` option,
the first row is used as column names and each subsequent row is decoded
as a map. Raises `unterminated_quoted_field` or `duplicate_header` on
invalid input.
""".
-spec csv_decode(binary() | iolist()) -> [[binary()]] | [#{binary() => binary()}].
csv_decode(Input) ->
  csv_decode(Input, []).

-doc """
Decode a CSV binary or iolist to a list of rows, with options.
Raises `Reason::atom()` (`unterminated_quoted_field` or `duplicate_header`)
on invalid input.
""".
-spec csv_decode(binary() | iolist(), csv_decode_opts()) -> [[binary()]] | [map()].
csv_decode(Input, Opts) ->
  case csv_try_decode(Input, Opts) of
    {ok,    Rows}   -> Rows;
    {error, Reason} -> error(Reason)
  end.

-doc """
Decode a CSV binary or iolist, returning `{ok, Rows}` or
`{error, Reason}` instead of raising, where `Reason` is
`unterminated_quoted_field` or `duplicate_header`.
""".
-spec csv_try_decode(binary() | iolist()) -> {ok, [[binary()]]} | {error, atom()}.
csv_try_decode(Input) ->
  csv_try_decode(Input, []).

-doc """
Decode a CSV binary or iolist with options, returning `{ok, Rows}` or
`{error, Reason}` instead of raising, where `Reason` is
`unterminated_quoted_field` or `duplicate_header`.
""".
-spec csv_try_decode(binary() | iolist(), csv_decode_opts()) ->
  {ok, [[binary()]] | [map()]} | {error, atom()}.
csv_try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc """
Encode a list of rows to a CSV binary.

Each row is a list of fields (binaries, atoms, integers, or floats).
Fields containing the delimiter, a double quote, or a line break are
quoted per RFC 4180, with embedded quotes doubled.
""".
-spec csv_encode([[term()]] | [map()]) -> binary().
csv_encode(Data) ->
  csv_encode(Data, []).

-doc """
Encode a list of rows to a CSV binary, with options.

With the `headers` option, `Data` is a list of maps: the first map's keys
become the header row (in iteration order), and each map is encoded as a
row in that column order.
""".
-spec csv_encode([[term()]] | [map()], csv_encode_opts()) -> binary().
csv_encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

%%%----------------------------------------------------------------------------
%%% CSV streaming / incremental decode
%%%----------------------------------------------------------------------------

-doc """
Create a new incremental decoder for feeding CSV in chunks (e.g. from a
socket or file), useful when the whole input isn't available up front.

Each complete row is decoded as soon as its terminating line break is seen,
via [`csv_decode/2`](`csv_decode/2`) on that single row. Only the *row
boundary detection* is incremental — a small byte-scanner tracks whether
the cursor is inside a quoted field across chunks, so that `\n`/`\r\n`
inside quoted fields doesn't end a row.

With the `headers` option, the first complete row is captured as the header
and used to decode every subsequent row as a map; no row is emitted for the
header itself.

## Example

```erlang
1> D0 = glazer:csv_stream_decoder(),
2> {Rows1, D1} = glazer:csv_stream_feed(D0, <<"a,b\n1,2\n3,">>),
3> Rows1.
[[<<"a">>,<<"b">>],[<<"1">>,<<"2">>]]
4> {Rows2, D2} = glazer:csv_stream_feed(D1, <<"4\n">>),
5> Rows2.
[[<<"3">>,<<"4">>]]
6> glazer:csv_stream_eof(D2).
{ok, []}
```
""".
-spec csv_stream_decoder() -> csv_stream_decoder().
csv_stream_decoder() ->
  csv_stream_decoder([]).

-doc """
Create a new incremental CSV decoder, passing `Opts` through to every
[`csv_decode/2`](`csv_decode/2`) call.
""".
-spec csv_stream_decoder(csv_decode_opts()) -> csv_stream_decoder().
csv_stream_decoder(Opts) when is_list(Opts) ->
  #csv_stream_decoder{opts = Opts}.

-doc """
Feed a chunk of bytes into the decoder, returning any complete CSV rows
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`csv_decode/2`](`csv_decode/2`) if a row that
the scanner deemed complete fails to decode.

## Example

```erlang
loop(Socket, D0) ->
  case gen_tcp:recv(Socket, 0) of
    {ok, Chunk} ->
      {Rows, D1} = glazer:csv_stream_feed(D0, Chunk),
      handle_rows(Rows),
      loop(Socket, D1);
    {error, closed} ->
      case glazer:csv_stream_eof(D0) of
        {ok, Trailing}  -> handle_rows(Trailing);
        {error, Reason} -> handle_truncated_stream(Reason)
      end
  end.
```
""".
-spec csv_stream_feed(csv_stream_decoder(), binary() | iolist()) ->
  {[[binary()]] | [map()], csv_stream_decoder()}.
csv_stream_feed(#csv_stream_decoder{buffer = Buf} = D, Chunk) ->
  NewBuf = iolist_to_binary([Buf, Chunk]),
  csv_stream_drain(D#csv_stream_decoder{buffer = NewBuf}, []).

csv_stream_drain(#csv_stream_decoder{buffer = Buf, state = St} = D, Acc) ->
  case csv_scan_row(Buf, St) of
    {complete, End, RestStart} ->
      <<RowBin:End/binary, _:(RestStart-End)/binary, Rest/binary>> = Buf,
      {Row, D1} = csv_stream_decode_row(D, RowBin),
      D2 = D1#csv_stream_decoder{buffer = Rest, state = {0, false}},
      case Row of
        skip -> csv_stream_drain(D2, Acc);
        _    -> csv_stream_drain(D2, [Row | Acc])
      end;
    {incomplete, NewSt} ->
      {lists:reverse(Acc), D#csv_stream_decoder{state = NewSt}}
  end.

%% Decode a single complete row (without its line terminator). Returns `skip`
%% for a blank line (matching csv_decode/2, which skips blank lines), or for
%% the header row when the `headers` option is set (it's captured but not
%% emitted as a row).
csv_stream_decode_row(#csv_stream_decoder{opts = Opts, header = undefined} = D, RowBin) ->
  case is_blank(RowBin) of
    true -> {skip, D};
    false ->
      case proplists:get_bool(headers, Opts) of
        true  -> {skip, D#csv_stream_decoder{header = RowBin}};
        false ->
          [Row] = csv_decode(RowBin, Opts -- [headers]),
          {Row, D}
      end
  end;
csv_stream_decode_row(#csv_stream_decoder{opts = Opts, header = Header} = D, RowBin) ->
  case is_blank(RowBin) of
    true -> {skip, D};
    false ->
      [Row] = csv_decode(<<Header/binary, "\n", RowBin/binary>>, Opts),
      {Row, D}
  end.

-doc """
Signal end-of-stream: decode any remaining buffered bytes as a final row
(useful when the input doesn't end with a trailing line break).

Returns `{ok, Rows}` with zero or one trailing row, or `{error, Reason}` if
the remaining bytes don't form a valid row.

## Example

```erlang
1> D0 = glazer:csv_stream_decoder(),
2> {Rows1, D1} = glazer:csv_stream_feed(D0, <<"a,b\n1,2">>),
3> Rows1.
[[<<"a">>,<<"b">>]]
4> glazer:csv_stream_eof(D1).
{ok, [[<<"1">>,<<"2">>]]}
```
""".
-spec csv_stream_eof(csv_stream_decoder()) ->
  {ok, [[binary()]] | [map()]} | {error, term()}.
csv_stream_eof(#csv_stream_decoder{buffer = Buf} = D) ->
  case is_blank(Buf) of
    true  -> {ok, []};
    false ->
      try csv_stream_decode_row(D, Buf) of
        {skip, _D1} -> {ok, []};
        {Row,  _D1} -> {ok, [Row]}
      catch
        error:Reason -> {error, Reason}
      end
  end.

%% Resumable scan for the next CSV row terminator (`\n` or `\r\n`) outside of
%% quoted fields, starting from `State = {Pos, InQuotes}`.
%%
%% Returns `{complete, End, RestStart}` where `binary:part(Bin, 0, End)` is
%% the row (without its terminator) and `RestStart` is the offset of the
%% first byte after the terminator, or `{incomplete, NewState}` if `Bin`
%% doesn't yet contain a complete row.
-spec csv_scan_row(binary(), csv_scan_state()) ->
  {complete, non_neg_integer(), non_neg_integer()} | {incomplete, csv_scan_state()}.
csv_scan_row(Bin, {Pos, InQuotes}) ->
  csv_scan_row(Bin, byte_size(Bin), Pos, InQuotes).

csv_scan_row(Bin, Size, Pos, InQuotes) when Pos < Size ->
  case binary:at(Bin, Pos) of
    $" ->
      csv_scan_row(Bin, Size, Pos + 1, not InQuotes);
    $\n when not InQuotes ->
      {complete, Pos, Pos + 1};
    $\r when not InQuotes, Pos + 1 < Size ->
      case binary:at(Bin, Pos + 1) of
        $\n -> {complete, Pos, Pos + 2};
        _   -> csv_scan_row(Bin, Size, Pos + 1, InQuotes)
      end;
    $\r when not InQuotes ->
      %% lone `\r` at the end of the buffer: might be the start of `\r\n`,
      %% so wait for more data before deciding
      {incomplete, {Pos, InQuotes}};
    _ ->
      csv_scan_row(Bin, Size, Pos + 1, InQuotes)
  end;
csv_scan_row(_Bin, _Size, Pos, InQuotes) ->
  {incomplete, {Pos, InQuotes}}.

-doc "Encode an Erlang term to a JSON binary.".
-spec json_encode(term()) -> binary().
json_encode(Data) ->
  json_encode(Data, []).

-doc "Encode an Erlang term to a JSON binary with options.".
-spec json_encode(term(), encode_opts()) -> binary().
json_encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc "Minify a JSON binary or iolist, removing all unnecessary whitespace.".
-spec json_minify(binary() | iolist()) -> binary().
json_minify(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Pretty-print a JSON binary or iolist with two-space indentation.".
-spec json_prettify(binary() | iolist()) -> binary().
json_prettify(_Input) ->
  ?NOT_LOADED_ERROR.

-doc """
Encode an integer to its JSON string representation.
Raises `badarg` if `Int` is not an integer.
""".
-spec encode_integer(integer()) -> binary().
encode_integer(_Int) ->
  ?NOT_LOADED_ERROR.

-doc """
Decode a JSON number string to an integer.
Raises `invalid_number_format` on invalid input.
""".
-spec decode_integer(binary() | iolist()) -> integer().
decode_integer(NumberString) ->
  case try_decode_integer(NumberString) of
    {ok,    Int}    -> Int;
    {error, Reason} -> error(Reason)
  end.

-doc """
Decode a JSON number string to an integer, returning `{ok, Int}` or
`{error, invalid_number_format}` instead of raising.
""".
-spec try_decode_integer(binary() | iolist()) -> {ok, integer()} | {error, invalid_number_format}.
try_decode_integer(_NumberString) ->
  ?NOT_LOADED_ERROR.

-doc """
Locate the end of the next complete top-level JSON value in `Bin`, without
decoding it.

Returns:

- `{complete, EndOffset}` - a complete value spans `binary:part(Bin, 0,
  EndOffset)`; the rest of `Bin` (if any) is left over for the next call
- `{incomplete, ScanState}` - `Bin` doesn't yet contain a complete value;
  feed more data via `json_scan/2` once it's available, passing the *entire
  unconsumed remainder* (this `Bin`, with new bytes appended) plus
  `ScanState`

This is the low-level primitive behind [`json_stream_feed/2`](`json_stream_feed/2`);
most callers should use the `stream_*` API instead.

## Example

Slicing off complete values from a buffer of concatenated JSON:

```erlang
1> Buf0 = <<"{\"a\":1} {\"b\":2}">>,
2> {complete, End1} = glazer:json_scan(Buf0).
{complete, 7}
3> <<Val1:End1/binary, Buf1/binary>> = Buf0,
4> Val1.
<<"{\"a\":1}">>
5> Buf1.
<<" {\"b\":2}">>
6> {complete, End2} = glazer:json_scan(Buf1).
{complete, 8}
```

Resuming a scan once more bytes arrive:

```erlang
1> {incomplete, S0} = glazer:json_scan(<<"{\"a\":">>).
{incomplete, {6,1,false,false,true,false}}
2> glazer:json_scan(<<"{\"a\":1}">>, S0).
{complete, 7}
```
""".
-spec json_scan(binary() | iolist()) -> {complete, non_neg_integer()} | {incomplete, scan_state()}.
json_scan(_Bin) ->
  ?NOT_LOADED_ERROR.

-doc """
Resume scanning `Bin` (the unconsumed remainder plus newly-appended bytes)
from `ScanState`.
""".
-spec json_scan(binary() | iolist(), scan_state()) -> {complete, non_neg_integer()} | {incomplete, scan_state()}.
json_scan(_Bin, _ScanState) ->
  ?NOT_LOADED_ERROR.

%%%----------------------------------------------------------------------------
%%% Streaming / incremental decode
%%%----------------------------------------------------------------------------

-doc """
Create a new incremental decoder for feeding JSON in chunks (e.g. from a
socket or file), useful when a complete document isn't available up front
or when a stream contains a sequence of concatenated/whitespace-separated
JSON values (e.g. newline-delimited JSON).

Decoding itself is **not** incremental — each complete top-level value is
still decoded in a single pass via [`json_decode/2`](`json_decode/2`) using the
library's fast whole-buffer decoder. Only the *boundary detection* (finding
where one value ends and the next begins) is incremental, via a small
byte-scanner that tracks nesting/string state across chunks.

## Example

```erlang
1> D0 = glazer:json_stream_decoder(),
2> {Vals1, D1} = glazer:json_stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
3> Vals1.
[#{<<"a">> => 1}]
4> {Vals2, _D2} = glazer:json_stream_feed(D1, <<"2}">>),
5> Vals2.
[#{<<"b">> => 2}]
```
""".
-spec json_stream_decoder() -> json_stream_decoder().
json_stream_decoder() ->
  json_stream_decoder([]).

-doc """
Create a new incremental decoder, passing `Opts` through to every
[`json_decode/2`](`json_decode/2`) call.
""".
-spec json_stream_decoder(decode_opts()) -> json_stream_decoder().
json_stream_decoder(Opts) when is_list(Opts) ->
  #json_stream_decoder{opts = Opts}.

-doc """
Feed a chunk of bytes into the decoder, returning any complete JSON values
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`json_decode/2`](`json_decode/2`) (e.g.
`Reason`) if a value that the scanner deemed complete fails
to decode.

## Example

Call `json_stream_feed/2` for each chunk received from the source while more
data may still arrive, and [`json_stream_eof/1`](`json_stream_eof/1`) once the source
is exhausted to flush any trailing value:

```erlang
loop(Socket, D0) ->
  case gen_tcp:recv(Socket, 0) of
    {ok, Chunk} ->
      {Vals, D1} = glazer:json_stream_feed(D0, Chunk),
      handle_values(Vals),
      loop(Socket, D1);
    {error, closed} ->
      case glazer:json_stream_eof(D0) of
        {ok, Trailing}  -> handle_values(Trailing);
        {error, Reason} -> handle_truncated_stream(Reason)
      end
  end.
```

The same decoder fits naturally into a `gen_server` driving an
active-mode socket: keep the `json_stream_decoder()` in the process state,
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
  {ok, #state{socket = Socket, decoder = glazer:json_stream_decoder()}}.

handle_info({tcp, Socket, Data}, #state{socket = Socket, decoder = D0} = State) ->
  {Vals, D1} = glazer:json_stream_feed(D0, Data),
  lists:foreach(fun handle_value/1, Vals),
  inet:setopts(Socket, [{active, once}]),
  {noreply, State#state{decoder = D1}};

handle_info({tcp_closed, Socket}, #state{socket = Socket, decoder = D0} = State) ->
  case glazer:json_stream_eof(D0) of
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
-spec json_stream_feed(json_stream_decoder(), binary() | iolist()) -> {[term()], json_stream_decoder()}.
json_stream_feed(#json_stream_decoder{buffer = Buf} = D, Chunk) ->
  NewBuf = iolist_to_binary([Buf, Chunk]),
  json_stream_drain(D#json_stream_decoder{buffer = NewBuf, state = undefined}, []).

json_stream_drain(#json_stream_decoder{buffer = Buf, opts = Opts, state = St} = D, Acc) ->
  ScanResult = case St of
    undefined -> json_scan(Buf);
    _         -> json_scan(Buf, St)
  end,
  case ScanResult of
    {complete, End} ->
      <<ValueBin:End/binary, Rest/binary>> = Buf,
      Term = json_decode(ValueBin, Opts),
      json_stream_drain(D#json_stream_decoder{buffer = Rest, state = undefined}, [Term | Acc]);
    {incomplete, NewSt} ->
      {lists:reverse(Acc), D#json_stream_decoder{state = NewSt}}
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
1> D0 = glazer:json_stream_decoder(),
2> {Vals1, D1} = glazer:json_stream_feed(D0, <<"123">>),
3> Vals1.
[]
4> glazer:json_stream_eof(D1).
{ok, [123]}
```

A stream that ends mid-value (e.g. a dropped connection) yields an error
instead of silently dropping the partial data:

```erlang
1> D0 = glazer:json_stream_decoder(),
2> {Vals1, D1} = glazer:json_stream_feed(D0, <<"{\"a\":1, \"b\":">>),
3> Vals1.
[]
4> glazer:json_stream_eof(D1).
{error, _Reason}
```
""".
-spec json_stream_eof(json_stream_decoder()) -> {ok, [term()]} | {error, term()}.
json_stream_eof(#json_stream_decoder{buffer = Buf, opts = Opts}) ->
  case is_blank(Buf) of
    true  -> {ok, []};
    false ->
      try json_decode(Buf, Opts) of
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
%%% Tests
%%%----------------------------------------------------------------------------
-ifdef(EUNIT).

encode_test_() ->
  [
    ?_assertEqual(<<"null">>,        json_encode(null)),
    ?_assertEqual(<<"null">>,        json_encode(nil, [use_nil])),
    ?_assertEqual(<<"true">>,        json_encode(true)),
    ?_assertEqual(<<"false">>,       json_encode(false)),
    ?_assertEqual(<<"1">>,           json_encode(1)),
    ?_assertEqual(<<"1.5">>,         json_encode(1.5)),
    ?_assertEqual(<<"\"hello\"">>,   json_encode(<<"hello">>)),
    ?_assertEqual(<<"[1,2,3]">>,     json_encode([1, 2, 3])),
    ?_assertEqual(<<"{}">>,          json_encode(#{})),
    ?_assertMatch(<<"{", _/binary>>, json_encode(#{<<"a">> => 1}))
  ].

decode_test_() ->
  [
    ?_assertEqual(null,             json_decode(<<"null">>)),
    ?_assertEqual(nil,              json_decode(<<"null">>, [use_nil])),
    ?_assertEqual(true,             json_decode(<<"true">>)),
    ?_assertEqual(false,            json_decode(<<"false">>)),
    ?_assertEqual(1,                json_decode(<<"1">>)),
    ?_assertEqual(1.5,              json_decode(<<"1.5">>)),
    ?_assertEqual(<<"hello">>,      json_decode(<<"\"hello\"">>)),
    ?_assertEqual([1, 2, 3],        json_decode(<<"[1,2,3]">>)),
    ?_assertEqual(#{<<"a">> => 1},  json_decode(<<"{\"a\":1}">>)),
    ?_assertEqual({[{<<"a">>, 1}]}, json_decode(<<"{\"a\":1}">>, [object_as_tuple])),
    ?_assertEqual(null,             json_decode(<<"null">>, [{null_term, null}])),
    ?_assertEqual(my_null,          json_decode(<<"null">>, [{null_term, my_null}]))
  ].

roundtrip_test_() ->
  Vals = [null, true, false, 0, 1, -1, 1.5, <<"hello">>, [], [1, 2, 3],
          #{<<"a">> => 1, <<"b">> => [1, 2]},
          #{<<"nested">> => #{<<"x">> => true}}],
  [?_assertEqual(V, json_decode(json_encode(V))) || V <- Vals].

minify_test_() ->
  [
    ?_assertEqual(<<"[1,2,3]">>,   json_minify(<<"[ 1, 2, 3 ]">>)),
    ?_assertEqual(<<"{\"a\":1}">>, json_minify(<<" { \"a\" : 1 } ">>))
  ].

prettify_test_() ->
  [
    ?_assertMatch(<<"[\n", _/binary>>,  json_prettify(<<"[1,2,3]">>)),
    ?_assertMatch(<<"{\n", _/binary>>,  json_prettify(<<"{\"a\":1}">>))
  ].

keys_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1},  json_decode(<<"{\"a\":1}">>)),
    ?_assertEqual(#{<<"a">> => 1},  json_decode(<<"{\"a\":1}">>, [{keys, binary}])),
    ?_assertEqual(#{a => 1},        json_decode(<<"{\"a\":1}">>, [{keys, atom}])),
    ?_assertEqual(#{a => 1},        json_decode(<<"{\"a\":1}">>, [{keys, existing_atom}])),
    %% existing_atom falls back to a binary for keys with no matching atom
    ?_assertEqual(#{<<"no_such_atom_in_glazer_test_suite_xyz">> => 1},
                  json_decode(<<"{\"no_such_atom_in_glazer_test_suite_xyz\":1}">>,
                         [{keys, existing_atom}]))
  ].

uescape_test_() ->
  [
    %% U+00E9 (é), UTF-8: 0xC3 0xA9
    ?_assertEqual(<<"\"\\u00e9\"">>, json_encode(<<16#C3, 16#A9>>, [uescape])),
    %% Without uescape, UTF-8 bytes pass through unescaped
    ?_assertEqual(<<"\"", 16#C3, 16#A9, "\"">>, json_encode(<<16#C3, 16#A9>>)),
    %% U+1F600 (emoji, outside the BMP) encodes as a surrogate pair
    ?_assertEqual(<<"\"\\ud83d\\ude00\"">>, json_encode(<<16#F0,16#9F,16#98,16#80>>, [uescape])),
    %% Round-trips back to the original UTF-8 binary
    ?_assertEqual(<<16#C3, 16#A9>>, json_decode(json_encode(<<16#C3, 16#A9>>, [uescape])))
  ].

force_utf8_test_() ->
  [
    %% Invalid byte sequences are sanitized to U+FFFD (UTF-8: EF BF BD)
    ?_assertEqual(<<"\"", 16#EF, 16#BF, 16#BD, 16#EF, 16#BF, 16#BD, "a\"">>,
                  json_encode(<<16#FF, 16#FE, $a>>, [force_utf8])),
    %% Without force_utf8, invalid bytes pass through verbatim
    ?_assertEqual(<<"\"", 16#FF, 16#FE, "a\"">>, json_encode(<<16#FF, 16#FE, $a>>)),
    %% Valid UTF-8 is left untouched
    ?_assertEqual(<<"\"", 16#C3, 16#A9, "\"">>, json_encode(<<16#C3, 16#A9>>, [force_utf8]))
  ].

pretty_test_() ->
  [
    ?_assertEqual(<<"{\n   \"a\": 1\n}">>, json_encode(#{<<"a">> => 1}, [pretty])),
    ?_assertEqual(<<"[\n   1,\n   2\n]">>, json_encode([1, 2], [pretty])),
    ?_assertEqual(#{<<"a">> => 1}, json_decode(json_encode(#{<<"a">> => 1}, [pretty])))
  ].

null_term_encode_test_() ->
  [
    ?_assertEqual(<<"null">>, json_encode(null)),
    ?_assertEqual(<<"null">>, json_encode(nil, [{null_term, nil}])),
    ?_assertEqual(<<"null">>, json_encode(undefined, [{null_term, undefined}])),
    ?_assertEqual(<<"null">>, json_encode(null, [{null_term, undefined}]))
  ].

object_as_tuple_test_() ->
  [
    ?_assertEqual({[]}, json_decode(<<"{}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}, {<<"b">>, 2}]},
                  json_decode(<<"{\"a\":1,\"b\":2}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, {[{<<"b">>, true}]}}]},
                  json_decode(<<"{\"a\":{\"b\":true}}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}]},
                  json_decode(json_encode({[{<<"a">>, 1}]}), [object_as_tuple]))
  ].

numbers_test_() ->
  [
    ?_assertEqual(0,        json_decode(<<"0">>)),
    ?_assertEqual(-1,       json_decode(<<"-1">>)),
    ?_assertEqual(-1.5,     json_decode(<<"-1.5">>)),
    ?_assertEqual(1.0e10,   json_decode(<<"1.0e10">>)),
    ?_assertEqual(1.0e-10,  json_decode(<<"1.0e-10">>)),
    ?_assertEqual(<<"-1">>, json_encode(-1)),
    ?_assertEqual(<<"0">>,  json_encode(0))
  ].

iolist_input_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1}, json_decode([<<"{\"a\":">>, <<"1}">>])),
    ?_assertEqual(#{<<"a">> => 1}, json_decode([<<"{">>, [<<"\"a\":1">>], <<"}">>])),
    ?_assertEqual(<<"{\"a\":1}">>, json_minify([<<"{ \"a\"">>, <<": 1 }">>]))
  ].

decode_error_test_() ->
  [
    ?_assertError(_, json_decode(<<"">>)),
    ?_assertError(_, json_decode(<<"{\"a\":}">>)),
    ?_assertError(_, json_decode(<<"{\"a\":1">>)),
    ?_assertError(_, json_decode(<<"[1, 2">>)),
    ?_assertError(_, json_decode(<<"not json">>))
  ].

bigint_test_() ->
  Big =  123456789012345678901234567890,
  Neg = -Big,
  [
    ?_assertEqual(<<"123456789012345678901234567890">>,  encode_integer(Big)),
    ?_assertEqual(<<"-123456789012345678901234567890">>, encode_integer(Neg)),
    ?_assertError(badarg,                                encode_integer(<<"not an integer">>)),
    ?_assertEqual({ok, Big},                             try_decode_integer(<<"123456789012345678901234567890">>)),
    ?_assertEqual({ok, Neg},                             try_decode_integer(<<"-123456789012345678901234567890">>)),
    ?_assertEqual({ok, 123},                             try_decode_integer(<<"123">>)),
    ?_assertEqual(Big,                                   decode_integer(<<"123456789012345678901234567890">>)),
    ?_assertEqual(123,                                   decode_integer(<<"123">>)),
    ?_assertError(invalid_number_format,                 decode_integer(<<"not a number">>)),
    ?_assertEqual({error, invalid_number_format},        try_decode_integer(<<"not a number">>)),
    ?_assertEqual(Big,                                   json_decode(<<"123456789012345678901234567890">>)),
    ?_assertEqual(<<"123456789012345678901234567890">>,  json_encode(Big)),
    ?_assertEqual(Big,                                   json_decode(json_encode(Big)))
  ].

scan_test_() ->
  [
    ?_assertEqual({complete, 7},  json_scan(<<"{\"a\":1}">>)),
    ?_assertEqual({complete, 7},  json_scan(<<"{\"a\":1}  {\"b\":2}">>)),
    ?_assertEqual({complete, 13}, json_scan(<<"[1,2,[3,4],5]rest">>)),
    ?_assertMatch({incomplete, _}, json_scan(<<"{\"a\":">>)),
    ?_assertMatch({incomplete, _}, json_scan(<<"123">>)),

    %% resume across a chunk boundary: caller passes the whole buffer + new
    %% bytes along with the previously-returned state
    ?_test(begin
      Part1 = <<"{\"a\":">>,
      Part2 = <<"1}">>,
      {incomplete, S1} = json_scan(Part1),
      ?assertEqual({complete, 7}, json_scan(<<Part1/binary, Part2/binary>>, S1))
    end),

    %% an escape sequence straddling the chunk boundary is tracked correctly
    ?_test(begin
      Chunk1 = <<"{\"k\":\"ab\\">>,
      Chunk2 = <<"\"cd\"}">>,
      {incomplete, S2} = json_scan(Chunk1),
      Whole = <<Chunk1/binary, Chunk2/binary>>,
      ?assertEqual({complete, byte_size(Whole)}, json_scan(Whole, S2))
    end)
  ].

json_stream_decoder_test_() ->
  [
    %% values split across feed/2 calls
    ?_test(begin
      D0 = json_stream_decoder(),
      {[#{<<"a">> := 1}], D1} = json_stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
      {[#{<<"b">> := 2}], _D2} = json_stream_feed(D1, <<"2}">>)
    end),

    %% byte-at-a-time JSON feeding decodes every line
    ?_test(begin
      Doc = <<"{\"x\":1}\n{\"y\":[1,2,3]}\n{\"z\":\"hi\"}\n">>,
      {Vals, DLast} = lists:foldl(
        fun(B, {Acc, D}) ->
          {V, D2} = json_stream_feed(D, <<B>>),
          {Acc ++ V, D2}
        end, {[], json_stream_decoder()}, binary_to_list(Doc)),
      {ok, []} = json_stream_eof(DLast),
      ?assertEqual([#{<<"x">> => 1},
                    #{<<"y">> => [1, 2, 3]},
                    #{<<"z">> => <<"hi">>}], Vals)
    end),

    %% a trailing bare scalar is only resolved at end-of-stream
    ?_test(begin
      D0 = json_stream_decoder(),
      {[], D1} = json_stream_feed(D0, <<"   42">>),
      ?assertEqual({ok, [42]}, json_stream_eof(D1))
    end),

    %% trailing whitespace at EOF yields no extra value
    ?_test(begin
      D0 = json_stream_decoder(),
      {[#{<<"a">> := 1}], D1} = json_stream_feed(D0, <<"{\"a\":1}\n">>),
      ?assertEqual({ok, []}, json_stream_eof(D1))
    end),

    %% decode options are threaded through to every decoded value
    ?_test(begin
      D0 = json_stream_decoder([{keys, atom}]),
      {[#{a := 1}], _D1} = json_stream_feed(D0, <<"{\"a\":1}">>)
    end),

    %% malformed trailing bytes surface as an error from json_stream_eof/1
    ?_test(begin
      D0 = json_stream_decoder(),
      {[], D1} = json_stream_feed(D0, <<"{\"a\":">>),
      ?assertMatch({error, _}, json_stream_eof(D1))
    end)
  ].

csv_stream_decoder_test_() ->
  [
    %% rows split across feed/2 calls
    ?_test(begin
      D0 = csv_stream_decoder(),
      {[[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]], D1} =
        csv_stream_feed(D0, <<"a,b\n1,2\n3,">>),
      {[[<<"3">>, <<"4">>]], _D2} = csv_stream_feed(D1, <<"4\n">>),
      ok
    end),

    %% byte-at-a-time feeding decodes every row
    ?_test(begin
      Doc = <<"a,b\n1,2\n3,4\n">>,
      {Rows, DLast} = lists:foldl(
        fun(B, {Acc, D}) ->
          {R, D2} = csv_stream_feed(D, <<B>>),
          {Acc ++ R, D2}
        end, {[], csv_stream_decoder()}, binary_to_list(Doc)),
      ?assertEqual({ok, []}, csv_stream_eof(DLast)),
      ?assertEqual([[<<"a">>, <<"b">>],
                     [<<"1">>, <<"2">>],
                     [<<"3">>, <<"4">>]], Rows)
    end),

    %% a trailing row with no terminator is only resolved at end-of-stream
    ?_test(begin
      D0 = csv_stream_decoder(),
      {[[<<"a">>, <<"b">>]], D1} = csv_stream_feed(D0, <<"a,b\n1,2">>),
      ?assertEqual({ok, [[<<"1">>, <<"2">>]]}, csv_stream_eof(D1))
    end),

    %% trailing newline at EOF yields no extra row
    ?_test(begin
      D0 = csv_stream_decoder(),
      {[[<<"a">>, <<"b">>]], D1} = csv_stream_feed(D0, <<"a,b\n">>),
      ?assertEqual({ok, []}, csv_stream_eof(D1))
    end),

    %% blank lines are skipped, matching csv_decode/2
    ?_test(begin
      D0 = csv_stream_decoder(),
      {Rows, D1} = csv_stream_feed(D0, <<"a,b\n\n1,2\n">>),
      ?assertEqual([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]], Rows),
      ?assertEqual({ok, []}, csv_stream_eof(D1))
    end),

    %% a quoted field containing an embedded newline doesn't end the row
    ?_test(begin
      D0 = csv_stream_decoder(),
      {Rows, D1} = csv_stream_feed(D0, <<"a,\"b\nc\"\nd,e\n">>),
      ?assertEqual([[<<"a">>, <<"b\nc">>], [<<"d">>, <<"e">>]], Rows),
      ?assertEqual({ok, []}, csv_stream_eof(D1))
    end),

    %% a quoted field containing a doubled (escaped) quote doesn't end the row
    ?_test(begin
      D0 = csv_stream_decoder(),
      {Rows, D1} = csv_stream_feed(D0, <<"a,\"b\"\"c\"\n">>),
      ?assertEqual([[<<"a">>, <<"b\"c">>]], Rows),
      ?assertEqual({ok, []}, csv_stream_eof(D1))
    end),

    %% the headers option decodes subsequent rows as maps, with no row
    %% emitted for the header itself
    ?_test(begin
      D0 = csv_stream_decoder([headers]),
      {Rows, D1} = csv_stream_feed(D0, <<"a,b\n1,2\n3,4\n">>),
      ?assertEqual([#{<<"a">> => <<"1">>, <<"b">> => <<"2">>},
                     #{<<"a">> => <<"3">>, <<"b">> => <<"4">>}], Rows),
      ?assertEqual({ok, []}, csv_stream_eof(D1))
    end),

    %% decode options are threaded through to every decoded row
    ?_test(begin
      D0 = csv_stream_decoder([headers, {keys, atom}]),
      {[#{a := <<"1">>, b := <<"2">>}], _D1} = csv_stream_feed(D0, <<"a,b\n1,2\n">>)
    end),

    %% malformed trailing bytes surface as an error from csv_stream_eof/1
    ?_test(begin
      D0 = csv_stream_decoder(),
      {[], D1} = csv_stream_feed(D0, <<"\"unterminated">>),
      ?assertMatch({error, unterminated_quoted_field}, csv_stream_eof(D1))
    end)
  ].

-endif.
