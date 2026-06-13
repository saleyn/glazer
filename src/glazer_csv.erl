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

-type decode_opt() ::
    {delimiter, char()}
  | headers
  | {keys, atom | existing_atom | binary}
  | {fields, [field_spec()]}
  | {null_term, atom()}.

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
- `{fields, Specs}`     - convert each column's field from a binary,
  positionally (the Nth spec applies to the Nth column, regardless of
  `headers`). Columns beyond the end of `Specs`, or given type `binary`,
  are left as binaries. See `field_spec/0` and `field_type/0` for
  the available types and the `default`/`on_failure` options
- `{null_term, Atom}`   - use `Atom` as the value produced by
  `on_failure => null`, overriding the library-wide `null` term for this
  call (default: the library-wide `null` term, see the `null` application
  env var)
""".
-type decode_opts() :: [decode_opt()].

-type decode_error() ::
    unterminated_quoted_field
  | duplicate_header
  | {invalid_field_value, Row :: pos_integer(), Column :: pos_integer()}.

-type encode_opt() ::
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
-type encode_opts() :: [encode_opt()].

-export_type([decode_opt/0, decode_opts/0, encode_opt/0, encode_opts/0, decode_error/0,
               field_type/0, field_spec/0, field_on_failure/0,
               scan_state/0, stream_decoder/0]).

-type scan_state() :: {non_neg_integer(), boolean()}.

-record(stream_decoder, {
  opts    = []        :: decode_opts(),
  buffer  = <<>>      :: binary(),
  header  = undefined :: binary() | undefined,
  state   = {0, false} :: scan_state()
}).

-opaque stream_decoder() :: #stream_decoder{}.

-doc """
Decode a CSV binary or iolist to a list of rows.

By default each row is a list of binary fields. With the `headers` option,
the first row is used as column names and each subsequent row is decoded
as a map. Raises `unterminated_quoted_field` or `duplicate_header` on
invalid input.
""".
-spec decode(binary() | iolist()) -> [[binary()]] | [#{binary() => binary()}].
decode(Input) ->
  decode(Input, []).

-doc """
Decode a CSV binary or iolist to a list of rows, with options.
Raises `Reason::decode_error()` on invalid input.
""".
-spec decode(binary() | iolist(), decode_opts()) -> [[binary()]] | [map()].
decode(Input, Opts) ->
  case try_decode(Input, Opts) of
    {ok,    Rows}   -> Rows;
    {error, Reason} -> error(Reason)
  end.

-doc """
Decode a CSV binary or iolist, returning `{ok, Rows}` or
`{error, Reason}` instead of raising, where `Reason` is a
`decode_error()`.
""".
-spec try_decode(binary() | iolist()) -> {ok, [[binary()]]} | {error, decode_error()}.
try_decode(Input) ->
  glazer:csv_try_decode(Input).

-doc """
Decode a CSV binary or iolist with options, returning `{ok, Rows}` or
`{error, Reason}` instead of raising, where `Reason` is a
`decode_error()`.
""".
-spec try_decode(binary() | iolist(), decode_opts()) ->
  {ok, [[binary()]] | [map()]} | {error, decode_error()}.
try_decode(Input, Opts) ->
  glazer:csv_try_decode(Input, Opts).

-doc """
Encode a list of rows to a CSV binary.

Each row is a list of fields (binaries, atoms, integers, or floats).
Fields containing the delimiter, a double quote, or a line break are
quoted per RFC 4180, with embedded quotes doubled.
""".
-spec encode([[term()]] | [map()]) -> binary().
encode(Data) ->
  glazer:csv_encode(Data).

-doc """
Encode a list of rows to a CSV binary, with options.

With the `headers` option, `Data` is a list of maps: the first map's keys
become the header row (in iteration order), and each map is encoded as a
row in that column order.
""".
-spec encode([[term()]] | [map()], encode_opts()) -> binary().
encode(Data, Opts) ->
  glazer:csv_encode(Data, Opts).

-doc """
Read `Filename` and decode its contents as CSV.

Raises `Reason::decode_error()` if the file's contents aren't valid CSV, or
a binary `"Filename: Reason"` message (see `file:format_error/1`) if the
file can't be read.

## Example

```erlang
1> glazer_csv:read_file("data.csv").
[[<<"a">>,<<"b">>],[<<"1">>,<<"2">>]]
```
""".
-spec read_file(file:name_all()) -> [[binary()]] | [map()].
read_file(Filename) ->
  read_file(Filename, []).

-doc """
Read `Filename` and decode its contents as CSV, with decode options
(see `decode/2`).
""".
-spec read_file(file:name_all(), decode_opts()) -> [[binary()]] | [map()].
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

## Example

```erlang
1> glazer_csv:write_file("data.csv", [[<<"a">>,<<"b">>],[1,2]]).
ok
```
""".
-spec write_file(file:name_all(), [[term()]] | [map()]) -> ok.
write_file(Filename, Data) ->
  write_file(Filename, Data, []).

-doc """
Encode `Data` to CSV with encode options (see `encode/2`) and write it to
`Filename`, overwriting any existing file.
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

With the `headers` option, the first complete row is captured as the header
and used to decode every subsequent row as a map; no row is emitted for the
header itself.

## Example

```erlang
1> D0 = glazer_csv:stream_decoder(),
2> {Rows1, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2\n3,">>),
3> Rows1.
[[<<"a">>,<<"b">>],[<<"1">>,<<"2">>]]
4> {Rows2, D2} = glazer_csv:stream_feed(D1, <<"4\n">>),
5> Rows2.
[[<<"3">>,<<"4">>]]
6> glazer_csv:stream_eof(D2).
{ok, []}
```
""".
-spec stream_decoder() -> stream_decoder().
stream_decoder() ->
  stream_decoder([]).

-doc """
Create a new incremental CSV decoder, passing `Opts` through to every
[`decode/2`](`decode/2`) call.
""".
-spec stream_decoder(decode_opts()) -> stream_decoder().
stream_decoder(Opts) when is_list(Opts) ->
  #stream_decoder{opts = Opts}.

-doc """
Feed a chunk of bytes into the decoder, returning any complete CSV rows
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`decode/2`](`decode/2`) if a row that
the scanner deemed complete fails to decode.

## Example

```erlang
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
  {[[binary()]] | [map()], stream_decoder()}.
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
      case proplists:get_bool(headers, Opts) of
        true  -> {skip, D#stream_decoder{header = RowBin}};
        false ->
          [Row] = decode(RowBin, Opts -- [headers]),
          {Row, D}
      end
  end;
stream_decode_row(#stream_decoder{opts = Opts, header = Header} = D, RowBin) ->
  case is_blank(RowBin) of
    true -> {skip, D};
    false ->
      [Row] = decode(<<Header/binary, "\n", RowBin/binary>>, Opts),
      {Row, D}
  end.

-doc """
Signal end-of-stream: decode any remaining buffered bytes as a final row
(useful when the input doesn't end with a trailing line break).

Returns `{ok, Rows}` with zero or one trailing row, or `{error, Reason}` if
the remaining bytes don't form a valid row.

## Example

```erlang
1> D0 = glazer_csv:stream_decoder(),
2> {Rows1, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2">>),
3> Rows1.
[[<<"a">>,<<"b">>]]
4> glazer_csv:stream_eof(D1).
{ok, [[<<"1">>,<<"2">>]]}
```
""".
-spec stream_eof(stream_decoder()) ->
  {ok, [[binary()]] | [map()]} | {error, term()}.
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
