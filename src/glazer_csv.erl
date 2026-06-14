-module(glazer_csv).
-moduledoc """
Fast CSV encoding and decoding using the glaze C++ library.

By default `null`s (e.g. produced by `on_failure => null`) are represented
as the atom `null`. To change it application-wide, set the `null` env key
in your config:
```
{glazer, [{null, nil}]}.
```

## Features

- RFC 4180 CSV encoding/decoding via `decode/1,2` and `encode/1,2`, with
  optional header-row support
- Per-column field type conversion (`{fields, Specs}`), including
  integers, floats, booleans, datetimes, atoms, and strings (binaries)
- Incremental/streaming CSV decoding via `stream_decoder/0,1`,
  `stream_feed/2`, `stream_eof/1`
- Configurable representation of CSV `null` values
- `read_file/1,2` and `write_file/2,3` helpers for decoding/encoding
  directly to/from a file

See also [https://github.com/stephenberry/glaze]
""".
-export([decode/1, decode/2, try_decode/1, try_decode/2,
         encode/1, encode/2,
         read_file/1, read_file/2, write_file/2, write_file/3,
         stream_decoder/0, stream_decoder/1, stream_feed/2, stream_eof/1]).

-doc """
A single column's target type for the `{fields, Specs}` CSV decode option:

- `integer`            - parse as an integer
- `{float, Precision}` - parse as a float, rounded to `Precision` decimal
  digits
- `boolean`            - parse `"true"`/`"false"` (any case) as `true`/
  `false`
- `{datetime, InputFormat}` - parse using a `strptime`-like format string
  (`%Y %m %d %H %M %S %f %z` and literals; `%z` accepts `Z`, `+HHMM`, or
  `+HH:MM`), converting the result to Unix epoch seconds (UTC)
- `binary`              - leave as a binary (default)
- `charlist`            - convert to a list of Unicode code points
- `existing_atom`       - convert to an existing atom, falling back to a
  binary if no such atom exists
- `{atom, ExistingAtoms}` - convert to an atom only if the field's text
  matches (and exists as) one of `ExistingAtoms`, falling back to a binary
  otherwise
""".
-type field_type() ::
    integer
  | {float, non_neg_integer()}
  | boolean
  | {datetime, binary()}
  | binary
  | charlist
  | existing_atom
  | {atom, ExistingAtoms :: [atom()]}.

-doc """
Controls what happens when a non-empty field fails to convert to the
requested `field_type()` (default `binary`):

- `binary`  - leave the field as the original binary (default)
- `raise`   - raise (or return `{error, Reason}` from `try_decode/2`)
  `{invalid_field_value, Row, Column}` (1-based)
- `default` - use the spec's `default` value (falls back to `binary` if no
  `default` is given)
- `null`    - use the configured null term: `{null_term, Atom}` if given,
  otherwise the library-wide `null` term (see the `null` application env
  var, [Null term configuration](#null-term-configuration))
""".
-type field_on_failure() :: binary | raise | default | null.

-doc """
A single element of the `{fields, Specs}` CSV decode option: either a
`field_type()` directly, or a map for more control:

- `type`       - the `field_type()` to convert the field to
- `default`    - used in place of the converted value whenever the raw CSV
  field is empty
- `on_failure` - see `field_on_failure/0` (default `binary`)
""".
-type field_spec() ::
    field_type()
  | #{type        := field_type(),
      default     => term(),
      on_failure  => field_on_failure()}.

-doc """
How the header row should be represented when using `{headers, Type}`:

- `atom`          - column names are converted to atoms (via `binary_to_atom/2`-equivalent)
- `existing_atom` - column names are converted to existing atoms (binaries if not found)
- `binary`        - column names are kept as binaries (default)
- `string`        - alias for `binary`
- `charlist`      - column names are converted to lists of Unicode codepoints
""".
-type headers_type() :: atom | existing_atom | binary | string | charlist.

-doc """
A single CSV decode option.  See `t:decode_opts/0` for the full reference
table of all available options and their effects.
""".
-type decode_opt() ::
    {delimiter, char()}
  | headers
  | {headers, [atom() | binary()] | headers_type()}
  | {fields, [field_spec()]}
  | {null_term, atom()}
  | {return, list | map | tuple}
  | {skip, non_neg_integer() | {pos_integer(), pos_integer()}}
  | {limit, pos_integer()}.

-doc """
CSV decode options:

| Option | Description |
|--------|-------------|
| `{delimiter, Char}` | Field delimiter character (default `$,`) |
| `headers` | Treat the first row as column names (shorthand for `{headers, binary}`) |
| `{headers, [Name, ...]}` | Use the given list of atoms or binaries as column names; the first data row is **not** consumed as a header |
| `{headers, binary}` | First row → binary column names (same as bare `headers`) |
| `{headers, string}` | Alias for `{headers, binary}` |
| `{headers, atom}` | First row → atom column names (via `binary_to_atom/2`-equivalent) |
| `{headers, existing_atom}` | First row → existing-atom column names (fall back to binary for unknown atoms) |
| `{headers, charlist}` | First row → column names as lists of Unicode codepoints |
| `{return, list}` | Data rows are lists of field values (default) |
| `{return, tuple}` | Data rows are tuples of field values |
| `{return, map}` | Data rows are maps keyed by column names; requires `headers` or `{headers, ...}`. Raises `duplicate_header` on duplicate column names |
| `{fields, Specs}` | Per-column type conversion, applied positionally; see `field_spec/0` |
| `{skip, N}` | Skip the first `N` data rows (after any header row) |
| `{skip, {From, To}}` | Process only data rows `From..To` (1-based inclusive); equivalent to `{skip, From-1}` plus `{limit, To-From+1}` |
| `{limit, N}` | Process at most `N` data rows (after skipping) |
| `{null_term, Atom}` | Atom to use for `on_failure => null`; overrides the library-wide `null` env var |
""".
-type decode_opts() :: [decode_opt()].

-doc """
The result of a successful CSV decode: a map with two keys.

- `headers` - `nil` when the `headers` option was not given; otherwise a list
  of column names (binaries by default, atoms with `{headers, atom}` or
  `{headers, existing_atom}`)
- `data`    - list of data rows; each row is a list of field values by default,
  a tuple of field values with `{return, tuple}`, or a map keyed by the
  column names when both `headers` and `{return, map}` are given
""".
-type csv_result() :: #{
    headers := nil | [binary() | atom()],
    data    := [[term()]] | [tuple()] | [map()]
}.

-doc """
Error reasons returned by `try_decode/1,2` or raised by `decode/1,2`:

- `unterminated_quoted_field` — input ended inside a `"..."` field with no
  closing quote
- `duplicate_header` — two columns share the same name and `{return, map}`
  was requested (map keys must be unique)
- `{invalid_field_value, Row, Column}` — a field at the given 1-based
  row/column position failed to convert to the type requested by
  `{fields, Specs}` with `on_failure => raise`
""".
-type decode_error() ::
    unterminated_quoted_field
  | duplicate_header
  | {invalid_field_value, Row :: pos_integer(), Column :: pos_integer()}.

-doc """
A single CSV encode option.  See `t:encode_opts/0` for descriptions of all
available options.
""".
-type encode_opt() ::
    {delimiter, char()}
  | headers
  | {headers, [atom() | binary()]}
  | {line_ending, lf | crlf}.

-doc """
CSV encode options:

- `{delimiter, Char}`  - field delimiter (default `$,`)
- `headers`            - input is a list of maps; the first map's keys
  become the header row, and subsequent maps are encoded as rows in that
  column order (missing keys produce empty fields)
- `{headers, [Name, ...]}` - input is a list of maps; uses the given list of
  atoms or binaries (matching the maps' key type) as the column order and
  header row, instead of deriving it from the first map's keys (missing
  keys produce empty fields)
- `{line_ending, lf | crlf}` - line terminator (default `crlf`, per RFC 4180)
""".
-type encode_opts() :: [encode_opt()].

-export_type([decode_opt/0, decode_opts/0, encode_opt/0, encode_opts/0, decode_error/0,
               csv_result/0, headers_type/0, field_type/0, field_spec/0, field_on_failure/0,
               scan_state/0, stream_decoder/0]).

-doc """
Resumable state of the incremental row-boundary scanner used inside a
`t:stream_decoder/0`.  Carries the current byte offset and a flag
indicating whether the scanner is currently inside a quoted field.
Exposed so the state can be serialised or inspected; normal usage does
not require direct access to this type.
""".
-type scan_state() :: {non_neg_integer(), boolean()}.

-record(stream_decoder, {
  opts    = []        :: decode_opts(),
  buffer  = <<>>      :: binary(),
  header  = undefined :: binary() | undefined,
  state   = {0, false} :: scan_state()
}).

-doc """
Opaque handle for incremental CSV decoding.  Created by
`stream_decoder/0,1` and threaded through successive `stream_feed/2`
calls; call `stream_eof/1` to flush any remaining buffered bytes at the
end of the input.
""".
-opaque stream_decoder() :: #stream_decoder{}.

-doc """
Decode a CSV binary or iolist.

Returns a `t:csv_result/0` map `#{headers => nil, data => Rows}` where
`Rows` is a list of rows, each row a list of binary fields.  With the
`headers` option the first row is captured as column names in `headers`
instead of appearing in `data`.
Raises `Reason :: t:decode_error/0` on invalid input.

## Examples

```erlang
1> glazer_csv:decode(<<"a,b\n1,2\n3,4\n">>).
#{headers => nil, data => [[<<"a">>,<<"b">>],[<<"1">>,<<"2">>],[<<"3">>,<<"4">>]]}

2> glazer_csv:decode(<<>>).
#{headers => nil, data => []}

3> glazer_csv:decode(<<"\"hello, world\",42\n">>).
#{headers => nil, data => [[<<"hello, world">>,<<"42">>]]}
```
""".
-spec decode(binary() | iolist()) -> csv_result().
decode(Input) ->
  decode(Input, []).

-doc """
Decode a CSV binary or iolist with options (see `t:decode_opts/0`).
Returns a `t:csv_result/0`.
Raises `Reason :: t:decode_error/0` on invalid input.

## Examples

```erlang
%% First row as binary column names
1> glazer_csv:decode(<<"name,age\nAlice,30\nBob,25\n">>, [headers]).
#{headers => [<<"name">>,<<"age">>],
  data    => [[<<"Alice">>,<<"30">>],[<<"Bob">>,<<"25">>]]}

%% Explicit column names — no header row expected in the data
2> glazer_csv:decode(<<"Alice,30\n">>, [{headers, [name, age]}, {return, map}]).
#{headers => [name,age], data => [#{age => <<"30">>, name => <<"Alice">>}]}

%% Per-column type conversion
3> glazer_csv:decode(<<"Alice,30\n">>, [{fields, [binary, integer]}]).
#{headers => nil, data => [[<<"Alice">>,30]]}

%% Semi-colon delimiter, skip first 2 rows, limit to 3
4> glazer_csv:decode(<<"h1;h2\nr1a;r1b\nr2a;r2b\nr3a;r3b\nr4a;r4b\n">>,
                     [{delimiter, $;}, headers, {skip, 1}, {limit, 2}]).
#{headers => [<<"h1">>,<<"h2">>],
  data    => [[<<"r2a">>,<<"r2b">>],[<<"r3a">>,<<"r3b">>]]}

%% Rows as maps with atom keys
5> glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, existing_atom}, {return, map}]).
#{headers => [a,b], data => [#{a => <<"1">>, b => <<"2">>}]}

%% Rows as tuples
6> glazer_csv:decode(<<"a,b\n1,2\n">>, [{return, tuple}]).
#{headers => nil, data => [{<<"a">>,<<"b">>},{<<"1">>,<<"2">>}]}
```
""".
-spec decode(binary() | iolist(), decode_opts()) -> csv_result().
decode(Input, Opts) ->
  case try_decode(Input, Opts) of
    {ok,    Result} -> Result;
    {error, Reason} -> error(Reason)
  end.

-doc """
Decode a CSV binary or iolist, returning `{ok, Result}` or
`{error, Reason}` instead of raising.
`Result` is a `t:csv_result/0`; `Reason` is a `t:decode_error/0`.

## Examples

```erlang
1> glazer_csv:try_decode(<<"a,b\n1,2\n">>).
{ok, #{headers => nil, data => [[<<"a">>,<<"b">>],[<<"1">>,<<"2">>]]}}

2> glazer_csv:try_decode(<<"\"unterminated">>).
{error, unterminated_quoted_field}
```
""".
-spec try_decode(binary() | iolist()) -> {ok, csv_result()} | {error, decode_error()}.
try_decode(Input) ->
  glazer:csv_try_decode(Input).

-doc """
Decode a CSV binary or iolist with options (see `t:decode_opts/0`),
returning `{ok, Result}` or `{error, Reason}` instead of raising.
`Result` is a `t:csv_result/0`; `Reason` is a `t:decode_error/0`.

## Examples

```erlang
1> glazer_csv:try_decode(<<"name,age\nAlice,30\n">>, [headers]).
{ok, #{headers => [<<"name">>,<<"age">>], data => [[<<"Alice">>,<<"30">>]]}}

2> glazer_csv:try_decode(<<"x">>,
                         [{fields, [#{type => integer, on_failure => raise}]}]).
{error, {invalid_field_value, 1, 1}}
```
""".
-spec try_decode(binary() | iolist(), decode_opts()) ->
  {ok, csv_result()} | {error, decode_error()}.
try_decode(Input, Opts) ->
  glazer:csv_try_decode(Input, Opts).

-doc """
Encode a list of rows to a CSV binary.

Each row is a list of fields (binaries, atoms, integers, or floats).
Fields containing the delimiter, a double quote, or a line break are
quoted per RFC 4180, with embedded quotes doubled.

## Examples

```erlang
1> glazer_csv:encode([[<<"a">>, <<"b">>], [1, 2]]).
<<"a,b\r\n1,2\r\n">>

2> glazer_csv:encode([[<<"hello, world">>, <<"say \"hi\"">>]]).
<<"\"hello, world\",\"say \"\"hi\"\"\"\r\n">>

3> glazer_csv:encode([]).
<<>>
```
""".
-spec encode([[term()]] | [map()]) -> binary().
encode(Data) ->
  glazer:csv_encode(Data).

-doc """
Encode a list of rows to a CSV binary, with options.

With the `headers` option, `Data` is a list of maps: the first map's keys
become the header row (in iteration order), and each map is encoded as a
row in that column order.

## Examples

```erlang
%% Maps to CSV with a header row
1> glazer_csv:encode([#{<<"name">> => <<"Alice">>, <<"age">> => 30}], [headers]).
<<"age,name\r\n30,Alice\r\n">>

%% Maps to CSV with an explicit column order
2> glazer_csv:encode([#{<<"name">> => <<"Alice">>, <<"age">> => 30}],
                     [{headers, [<<"name">>, <<"age">>]}]).
<<"name,age\r\nAlice,30\r\n">>

%% Semicolon delimiter with LF line endings
3> glazer_csv:encode([[<<"a">>, <<"b">>], [1, 2]],
                     [{delimiter, $;}, {line_ending, lf}]).
<<"a;b\n1;2\n">>
```
""".
-spec encode([[term()]] | [map()], encode_opts()) -> binary().
encode(Data, Opts) ->
  glazer:csv_encode(Data, Opts).

-doc """
Read `Filename` and decode its contents as CSV.

Raises `Reason::decode_error()` if the file's contents aren't valid CSV, or
a binary `"Filename: Reason"` message (see `file:format_error/1`) if the
file can't be read.

## Examples

```erlang
%% File contains: name,age\nAlice,30\n
1> glazer_csv:read_file("data.csv").
#{headers => nil, data => [[<<"name">>,<<"age">>],[<<"Alice">>,<<"30">>]]}

2> glazer_csv:read_file("missing.csv").
** exception error: <<"missing.csv: no such file or directory">>
```
""".
-spec read_file(file:name_all()) -> csv_result().
read_file(Filename) ->
  read_file(Filename, []).

-doc """
Read `Filename` and decode its contents as CSV, with decode options
(see `decode/2`).

## Examples

```erlang
%% File contains: name,age\nAlice,30\nBob,25\n
1> glazer_csv:read_file("data.csv", [headers, {return, map}]).
#{headers => [<<"name">>,<<"age">>],
  data    => [#{<<"age">> => <<"30">>,  <<"name">> => <<"Alice">>},
              #{<<"age">> => <<"25">>,  <<"name">> => <<"Bob">>}]}

2> glazer_csv:read_file("data.csv", [headers, {fields, [binary, integer]}]).
#{headers => [<<"name">>,<<"age">>], data => [[<<"Alice">>,30],[<<"Bob">>,25]]}
```
""".
-spec read_file(file:name_all(), decode_opts()) -> csv_result().
read_file(Filename, Opts) ->
  case file:read_file(Filename) of
    {ok, Bin}       -> decode(Bin, Opts);
    {error, Reason} -> error(glazer:format_error("~ts: ~ts", [Filename,file:format_error(Reason)]))
  end.

-doc """
Encode `Data` to CSV and write it to `Filename`, overwriting any existing
file.

Raises a binary `"Filename: Reason"` message (see `file:format_error/1`)
if the file can't be written.

## Examples

```erlang
1> glazer_csv:write_file("out.csv", [[<<"name">>,<<"age">>],[<<"Alice">>,30]]).
ok

2> glazer_csv:write_file("/read-only/out.csv", []).
** exception error: <<"/read-only/out.csv: permission denied">>
```
""".
-spec write_file(file:name_all(), [[term()]] | [map()]) -> ok.
write_file(Filename, Data) ->
  write_file(Filename, Data, []).

-doc """
Encode `Data` to CSV with encode options (see `encode/2`) and write it to
`Filename`, overwriting any existing file.

## Examples

```erlang
%% Write maps as CSV with a header row and LF line endings
1> glazer_csv:write_file("out.csv",
                         [#{<<"name">> => <<"Alice">>, <<"score">> => 99}],
                         [headers, {line_ending, lf}]).
ok

%% Write with a semicolon delimiter
2> glazer_csv:write_file("out.csv",
                         [[<<"a">>, <<"b">>], [1, 2]],
                         [{delimiter, $;}]).
ok
```
""".
-spec write_file(file:name_all(), [[term()]] | [map()], encode_opts()) -> ok.
write_file(Filename, Data, Opts) ->
  case file:write_file(Filename, encode(Data, Opts)) of
    ok              -> ok;
    {error, Reason} -> error(glazer:format_error("~ts: ~ts", [Filename, file:format_error(Reason)]))
  end.

%%%----------------------------------------------------------------------------
%%% CSV streaming / incremental decode
%%%----------------------------------------------------------------------------

-doc """
Create a new incremental decoder for feeding CSV in chunks (e.g. from a
socket or file), useful when the whole input isn't available up front.

Each complete row is decoded as soon as its terminating line break is seen,
via [`decode/2`](`decode/2`) on that single row. Only the *row
boundary detection* is incremental — a small byte-scanner tracks whether
the cursor is inside a quoted field across chunks, so that `\n`/`\r\n`
inside quoted fields doesn't end a row.

With the `headers` option, the first complete row is captured as the header;
no row is emitted for it. Passes the same options as `decode/2` to every
row decode internally (see `stream_decoder/1` to supply options).

## Examples

```erlang
1> D0 = glazer_csv:stream_decoder(),
   {Rows1, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2\n3,">>),
   Rows1.
[[<<"a">>,<<"b">>],[<<"1">>,<<"2">>]]

2> {Rows2, D2} = glazer_csv:stream_feed(D1, <<"4\n">>),
   Rows2.
[[<<"3">>,<<"4">>]]

3> glazer_csv:stream_eof(D2).
{ok, []}
```
""".
-spec stream_decoder() -> stream_decoder().
stream_decoder() ->
  stream_decoder([]).

-doc """
Create a new incremental CSV decoder, passing `Opts` through to every
internal [`decode/2`](`decode/2`) call.

All options from `decode/2` are accepted except `{skip, ...}` and
`{limit, ...}`, which are ignored in streaming mode (the caller controls
which rows to process by consuming the output of `stream_feed/2`).

When `{headers, [List]}` is given, the explicit header names are
pre-populated and no header row is consumed from the stream.

## Examples

```erlang
%% Headers option: first row captured, data rows returned as field lists
1> D0 = glazer_csv:stream_decoder([headers]),
   {Rows, D1} = glazer_csv:stream_feed(D0, <<"name,age\nAlice,30\n">>),
   Rows.
[[<<"Alice">>,<<"30">>]]

%% Explicit headers + map output
2> D0 = glazer_csv:stream_decoder([{headers, [name, age]}, {return, map}]),
   {Rows, _D1} = glazer_csv:stream_feed(D0, <<"Alice,30\n">>),
   Rows.
[#{age => <<"30">>, name => <<"Alice">>}]

%% Semicolon delimiter
3> D0 = glazer_csv:stream_decoder([{delimiter, $;}]),
   {Rows, _D1} = glazer_csv:stream_feed(D0, <<"a;b\n1;2\n">>),
   Rows.
[[<<"a">>,<<"b">>],[<<"1">>,<<"2">>]]
```
""".
-spec stream_decoder(decode_opts()) -> stream_decoder().
stream_decoder(Opts) when is_list(Opts) ->
  %% {headers, [ExplicitList]}: encode the list as a CSV header row so the
  %% streaming machinery can prepend it to each data-row decode call, exactly
  %% as if that row had been read from the stream.
  Delim = proplists:get_value(delimiter, Opts, $,),
  {Header, Opts2} = case proplists:get_value(headers, Opts) of
    List when is_list(List) ->
      Encoded = encode([List], [{delimiter, Delim}]),
      %% encode/2 always appends CRLF; strip it to get a bare header line.
      HeaderBin = binary:part(Encoded, 0, byte_size(Encoded) - 2),
      %% Normalise {headers, List} → bare `headers` atom for internal decode calls.
      Opts3 = [headers | [O || O <- Opts, case O of {headers, _} -> false; _ -> true end]],
      {HeaderBin, Opts3};
    _ ->
      {undefined, Opts}
  end,
  #stream_decoder{opts = Opts2, header = Header}.

-doc """
Feed a chunk of bytes into the decoder, returning any complete CSV rows
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`decode/2`](`decode/2`) if a row that
the scanner deemed complete fails to decode.

## Examples

```erlang
%% Rows split across two feed calls
1> D0 = glazer_csv:stream_decoder(),
   {Rows1, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,">>),
   Rows1.
[[<<"a">>,<<"b">>]]

2> {Rows2, D2} = glazer_csv:stream_feed(D1, <<"2\n">>),
   Rows2.
[[<<"1">>,<<"2">>]]

3> glazer_csv:stream_eof(D2).
{ok, []}

%% Typical socket-reading loop
loop(Socket, D0) ->
  case gen_tcp:recv(Socket, 0) of
    {ok, Chunk} ->
      {Rows, D1} = glazer_csv:stream_feed(D0, Chunk),
      handle_rows(Rows),
      loop(Socket, D1);
    {error, closed} ->
      case glazer_csv:stream_eof(D0) of
        {ok, Trailing}  -> handle_rows(Trailing);
        {error, Reason} -> handle_truncated_stream(Reason)
      end
  end.
```
""".
-spec stream_feed(stream_decoder(), binary() | iolist()) ->
  {[[term()]] | [tuple()] | [map()], stream_decoder()}.
stream_feed(#stream_decoder{buffer = Buf} = D, Chunk) ->
  NewBuf = iolist_to_binary([Buf, Chunk]),
  stream_drain(D#stream_decoder{buffer = NewBuf}, []).

stream_drain(#stream_decoder{buffer = Buf, state = St} = D, Acc) ->
  case scan_row(Buf, St) of
    {complete, End, RestStart} ->
      <<RowBin:End/binary, _:(RestStart-End)/binary, Rest/binary>> = Buf,
      {Row, D1} = stream_decode_row(D, RowBin),
      D2 = D1#stream_decoder{buffer = Rest, state = {0, false}},
      case Row of
        skip -> stream_drain(D2, Acc);
        _    -> stream_drain(D2, [Row | Acc])
      end;
    {incomplete, NewSt} ->
      {lists:reverse(Acc), D#stream_decoder{state = NewSt}}
  end.

%% Decode a single complete row (without its line terminator). Returns `skip`
%% for a blank line (matching decode/2, which skips blank lines), or for
%% the header row when the `headers` option is set (it's captured but not
%% emitted as a row).
stream_decode_row(#stream_decoder{opts = Opts, header = undefined} = D, RowBin) ->
  case is_blank(RowBin) of
    true -> {skip, D};
    false ->
      %% has_headers/1 recognises both bare `headers` and {headers, Type}.
      case has_headers(Opts) of
        true  -> {skip, D#stream_decoder{header = RowBin}};
        false ->
          SafeOpts = stream_safe_opts(strip_headers_opt(Opts)),
          #{data := [Row]} = decode(RowBin, SafeOpts),
          {Row, D}
      end
  end;
stream_decode_row(#stream_decoder{opts = Opts, header = Header} = D, RowBin) ->
  case is_blank(RowBin) of
    true -> {skip, D};
    false ->
      #{data := [Row]} = decode(<<Header/binary, "\n", RowBin/binary>>,
                                 stream_safe_opts(Opts)),
      {Row, D}
  end.

%% True when the opts specify that the first data row should be treated as headers.
has_headers(Opts) ->
  proplists:get_value(headers, Opts) =/= undefined.

%% Strip {headers, ...} and bare `headers` from opts (for no-header row decodes).
strip_headers_opt(Opts) ->
  [O || O <- Opts, case O of
    headers      -> false;
    {headers, _} -> false;
    _            -> true
  end].

%% Strip {skip, ...} and {limit, ...}: meaningless inside per-row streaming decodes.
stream_safe_opts(Opts) ->
  [O || O <- Opts, case O of
    {skip,  _} -> false;
    {limit, _} -> false;
    _          -> true
  end].

-doc """
Signal end-of-stream: decode any remaining buffered bytes as a final row
(useful when the input doesn't end with a trailing line break).

Returns `{ok, Rows}` with zero or one trailing row, or `{error, Reason}` if
the remaining bytes don't form a valid row.

## Examples

```erlang
%% Input without a trailing newline
1> D0 = glazer_csv:stream_decoder(),
   {Rows1, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2">>),
   Rows1.
[[<<"a">>,<<"b">>]]

2> glazer_csv:stream_eof(D1).
{ok, [[<<"1">>,<<"2">>]]}

%% Input ending with a newline — nothing left at EOF
3> D0 = glazer_csv:stream_decoder(),
   {_Rows, D1} = glazer_csv:stream_feed(D0, <<"a,b\n">>),
   glazer_csv:stream_eof(D1).
{ok, []}

%% Unterminated quoted field surfaces here
4> D0 = glazer_csv:stream_decoder(),
   {[], D1} = glazer_csv:stream_feed(D0, <<"\"unterminated">>),
   glazer_csv:stream_eof(D1).
{error, unterminated_quoted_field}
```
""".
-spec stream_eof(stream_decoder()) ->
  {ok, [[term()]] | [tuple()] | [map()]} | {error, term()}.
stream_eof(#stream_decoder{buffer = Buf} = D) ->
  case is_blank(Buf) of
    true  -> {ok, []};
    false ->
      try stream_decode_row(D, Buf) of
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
-spec scan_row(binary(), scan_state()) ->
  {complete, non_neg_integer(), non_neg_integer()} | {incomplete, scan_state()}.
scan_row(Bin, {Pos, InQuotes}) ->
  scan_row(Bin, byte_size(Bin), Pos, InQuotes).

scan_row(Bin, Size, Pos, InQuotes) when Pos < Size ->
  case binary:at(Bin, Pos) of
    $" ->
      scan_row(Bin, Size, Pos + 1, not InQuotes);
    $\n when not InQuotes ->
      {complete, Pos, Pos + 1};
    $\r when not InQuotes, Pos + 1 < Size ->
      case binary:at(Bin, Pos + 1) of
        $\n -> {complete, Pos, Pos + 2};
        _   -> scan_row(Bin, Size, Pos + 1, InQuotes)
      end;
    $\r when not InQuotes ->
      %% lone `\r` at the end of the buffer: might be the start of `\r\n`,
      %% so wait for more data before deciding
      {incomplete, {Pos, InQuotes}};
    _ ->
      scan_row(Bin, Size, Pos + 1, InQuotes)
  end;
scan_row(_Bin, _Size, Pos, InQuotes) ->
  {incomplete, {Pos, InQuotes}}.

%% True if `Bin` is empty or contains only whitespace.
is_blank(Bin) ->
  lists:all(fun(B) -> B =:= $\s orelse B =:= $\t orelse B =:= $\r orelse B =:= $\n end,
            binary_to_list(Bin)).
