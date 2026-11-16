# CSV

## Usage

`decode/1,2` decodes an RFC 4180 CSV document to `#{headers => nil|[...],
data => Rows}`, where `Rows` is a list of rows, each row a list of binary
fields by default:

**Erlang**

**Erlang**

```erlang
1> glazer_csv:decode(<<"name,age\nAlice,30\nBob,25\n">>).
#{headers => nil,
  data    => [[<<"name">>,<<"age">>],[<<"Alice">>,<<"30">>],[<<"Bob">>,<<"25">>]]}

2> glazer_csv:encode([[<<"name">>, <<"age">>], [<<"Alice">>, 30]]).
<<"name,age\r\nAlice,30\r\n">>
```

**Elixir**

```elixir
iex> Glazer.CSV.decode!("name,age\nAlice,30\nBob,25\n")
%{headers: nil, data: [["name", "age"], ["Alice", "30"], ["Bob", "25"]]}

iex> Glazer.CSV.encode!([["name", "age"], ["Alice", "30"], ["Bob", "25"]])
"name,age\r\nAlice,30\r\nBob,25\r\n"
```

With the `headers` option, the first row is captured as column names in
`headers` and each subsequent row decodes to a map when combined with
`{return, map}`; `encode/2` with `headers` does the reverse, deriving the
header row from the first map's keys:

**Erlang**

```erlang
1> glazer_csv:decode(<<"name,age\nAlice,30\n">>, [headers, {return, map}]).
#{headers => [<<"name">>,<<"age">>],
  data    => [#{<<"name">> => <<"Alice">>, <<"age">> => <<"30">>}]}

2> glazer_csv:encode([#{<<"name">> => <<"Alice">>, <<"age">> => 30}], [headers]).
<<"age,name\r\n30,Alice\r\n">>
```

**Elixir**

```elixir
iex> Glazer.CSV.decode!("name,age\nAlice,30\n", [:headers, {:return, :map}])
%{headers: ["name", "age"], data: [%{"name" => "Alice", "age" => "30"}]}

iex> Glazer.CSV.encode!([%{"name" => "Alice", "age" => 30}], [:headers])
"age,name\r\n30,Alice\r\n"
```

Fields containing the delimiter, a double quote, or a line break are
quoted automatically on encode (with embedded quotes doubled), and
unquoted on decode. The delimiter defaults to `,` and can be changed via
`{delimiter, Char}`; the encoded line ending defaults to `\r\n` per
RFC 4180 and can be changed to `\n` via `{line_ending, lf}`.

## Streaming

For input that arrives in chunks — e.g. reading a large document
incrementally, or consuming CSV from a
socket or file — `decode_start/3` provides a small stateful
wrapper that buffers partial input and decodes each CSV record as soon
as it's complete, without re-parsing bytes you've already seen.
`decode_continue/2` returns `{Value, Acc, Rest::binary()}` if a `Value` is
complete and parsed, or `{continue, State}` if the value cannot be parsed given
the available data. Once the input is exhausted, call `stream_eof/1` to flush
any trailing bare scalar (numbers, strings, etc. have no closing
delimiter of their own) and surface an error if the buffer holds an
incomplete value:

**Erlang**

```erlang
1> {[<<"a">>,<<"b">>], ok, Bin1} = glazer_csv:decode_start(~"a,b\n1,2\n3,", ok, []).
%% Bin = <<"1,2\n3,">>
2> {[<<"1">>,<<"2">>], ok, Bin2} = glazer_csv:decode_start(Bin1, ok, []).
%% Bin = <<"3,">>
3> {continue, State} = glazer_csv:decode_start(Bin2, ok, []).
4> glazer_csv:decode_continue(~"4\n", State).
{[<<"3">>,<<"4">>],ok,<<>>}
```

**Elixir**

```elixir
iex> {["a", "b"], :ok, bin1} = Glazer.CSV.decode_start("a,b\n1,2\n3,", :ok, [])
iex> {["1", "2"], :ok, bin2} = Glazer.CSV.decode_start(bin1, :ok, [])
iex> {:continue, state} = Glazer.CSV.decode_start(bin2, :ok, [])
iex> Glazer.CSV.decode_continue("4\n", state)
{["3", "4"], :ok, ""}
```

`glazer_csv:decode_start/3` accepts the same options as `decode/2` (e.g.
`{keys, atom}`, `use_nil`) and applies them to every decoded value.

## CSV decode options

| Option | Description |
|---|---|
| `{delimiter, Char}` | Field delimiter (default `$,`) |
| `headers` | Treat the first row as column names (shorthand for `{headers, binary}`) |
| `{headers, [Name, ...]}` | Use the given list of atoms or binaries as column names; the first data row is **not** consumed as a header |
| `{headers, binary}` | First row is binary column names (same as bare `headers`) |
| `{headers, string}` | Alias for `{headers, binary}` |
| `{headers, atom}` | First row → atom column names (via `binary_to_atom/2`-equivalent) |
| `{headers, existing_atom}` | First row → existing-atom column names, falling back to binaries for unknown atoms |
| `{headers, charlist}` | First row → column names as lists of Unicode codepoints |
| `{return, list}` | Data rows are lists of field values (default) |
| `{return, tuple}` | Data rows are tuples of field values |
| `{return, map}` | Data rows are maps keyed by column names; requires `headers` or `{headers, ...}`. Raises `duplicate_header` on duplicate column names |
| `{fields, Specs}` | Convert each column's field from a binary, positionally — see [Field type conversion](#field-type-conversion) |
| `{skip, N}` | Skip the first `N` data rows (after any header row) |
| `{skip, {From, To}}` | Process only data rows `From..To` (1-based inclusive); equivalent to `{skip, From-1}` plus `{limit, To-From+1}` |
| `{limit, N}` | Process at most `N` data rows (after skipping) |
| `{null, Atom}` | Use `Atom` as the value produced by `on_failure => null` (default `null`) |
| `copy_strings` | Always allocate a fresh binary for each decoded field, instead of a zero-copy sub-binary of the input (see [Performance Optimization Details](performance.md#performance-optimization-details)) |

## Field type conversion

The `{fields, Specs}` decode option converts each column's field from a
binary to the given Erlang type. `Specs` is a list applied positionally —
the Nth spec applies to the Nth column, regardless of whether `headers` is
set. Columns beyond the end of `Specs` are left as binaries.

**Erlang**

```erlang
1> glazer_csv:decode(<<"name,age,active,joined\nAlice,30,true,2024-01-15T10:30:00Z\n">>,
..                    [headers, {fields, [binary, integer, boolean,
..                                         {datetime, <<"%Y-%m-%dT%H:%M:%SZ">>}]}]).
[#{<<"name">> => <<"Alice">>, <<"age">> => 30, <<"active">> => true,
   <<"joined">> => 1705314600}]
```

**Elixir**

```elixir
iex> Glazer.CSV.decode!("name,age,active,joined\nAlice,30,true,2024-01-15T10:30:00Z\n",
...>   [:headers, {:fields, [:binary, :integer, :boolean, {:datetime, "%Y-%m-%dT%H:%M:%SZ"}]}])
%{headers: ["name", "age", "active", "joined"], data: [["Alice", 30, true, 1705314600]]}
```

Each element of `Specs` is either a `Type` directly, or a map
`#{type => Type, default => Term, on_failure => OnFailure}` for more
control (see below). `Type` is one of:

| Type | Description |
|---|---|
| `integer` | Parse the field as an integer |
| `{float, Precision}` | Parse the field as a float, rounded to `Precision` decimal digits |
| `boolean` | Parse `"true"`/`"false"` (any case) as `true`/`false` |
| `{datetime, InputFormat}` | Parse with a `strptime`-like format string and convert to Unix epoch seconds (UTC) |
| `binary` | Leave the field as a binary (default) |
| `charlist` | Convert the field to a list of Unicode code points |
| `existing_atom` | Convert to an existing atom, falling back to a binary if no such atom exists |
| `{atom, ExistingAtoms}` | Convert to an atom only if the field's text matches (and exists as) one of `ExistingAtoms`, falling back to a binary otherwise |

`InputFormat` supports the directives `%Y %y %m %d %H %M %S %f %z` (and
`%%` for a literal `%`); any other character must match the input
literally, and a space matches a run of one-or-more whitespace characters.
`%z` accepts `Z`, `+HHMM`, or `+HH:MM`-style offsets; fractional seconds
(`%f`) are parsed but discarded. The result is always in UTC.

### `default` and `on_failure`

Using the map form `#{type => Type, default => Term, on_failure => OnFailure}`:

- `default` (when given) is used in place of the converted value whenever
  the raw CSV field is empty.
- `on_failure` controls what happens when a *non-empty* field fails to
  convert to `Type` (default `binary`):

  | `on_failure` | Behavior |
  |---|---|
  | `binary` | Leave the field as the original binary (default) |
  | `raise` | Raise `{invalid_field_value, Row, Column}` (1-based), or return `{error, Reason}` from `try_decode/2` |
  | `default` | Use the spec's `default` value (falls back to `binary` if no `default` is given) |
  | `null` | Use the configured null term: `{null, Atom}` if given, otherwise the library-wide null term (see [Null term configuration](json.md#null-term-configuration) and `{null, Atom}` below) |

**Erlang**

```erlang
1> glazer_csv:decode(<<"1\nbad\n">>,
..                    [{fields, [#{type => integer, on_failure => raise}]}]).
** exception error: {invalid_field_value,2,1}

2> glazer_csv:decode(<<"1\nbad\n">>,
..                    [{fields, [#{type => integer, default => 0, on_failure => default}]}]).
[[1],[0]]

3> glazer_csv:decode(<<"1\nbad\n">>,
..                    [{null, nil},
..                     {fields, [#{type => integer, on_failure => null}]}]).
[[1],[nil]]
```

**Elixir**

```elixir
iex> Glazer.CSV.decode!("1\nbad\n", [{:fields, [%{type: :integer, on_failure: :raise}]}])
** (ErlangError) Erlang error: {:invalid_field_value, 2, 1}

iex> Glazer.CSV.decode!("1\nbad\n", [{:fields, [%{type: :integer, default: 0, on_failure: :default}]}])
%{headers: nil, data: [[1], [0]]}

iex> Glazer.CSV.decode!("1\nbad\n", [{:null, nil}, {:fields, [%{type: :integer, on_failure: :null}]}])
%{headers: nil, data: [[1], [nil]]}
```

`{null, Atom}` only affects `on_failure => null` for that call. Without
it, `on_failure => null` falls back to the library-wide null term — `null`
by default, or whatever atom is configured via the
[Null term configuration](json.md#null-term-configuration)
application env var (`{glazer, [{null, Atom}]}`).

## CSV Encode options

| Option | Description |
|---|---|
| `{delimiter, Char}` | Field delimiter (default `$,`) |
| `headers` | Input is a list of maps; the first map's keys become the header row, and subsequent maps are encoded as rows in that column order (missing keys produce empty fields) |
| `{headers, [Name, ...]}` | Input is a list of maps; uses the given list of atoms or binaries (matching the maps' key type) as the column order and header row, instead of deriving it from the first map's keys (missing keys produce empty fields) |
| `{line_ending, lf \| crlf}` | Line terminator (default `crlf`, per RFC 4180) |

## API

All functions below are in `glazer_csv`.

| Function | Description |
|---|---|
| `decode/1`, `decode/2` | Decode a CSV binary or iolist to a list of rows (or maps with `headers`) |
| `try_decode/1`, `try_decode/2` | Decode CSV, returning `{ok, Rows}` or `{error, Reason}` instead of raising |
| `encode/1`, `encode/2` | Encode a list of rows (or maps with `headers`) to a CSV binary; raises `{encode_error, {Msg, Term}}` on failure |
| `read_file/1`, `read_file/2` | Read a file and decode its contents as CSV |
| `write_file/2`, `write_file/3` | Encode rows to CSV and write them to a file |
| `stream_decoder/0`, `stream_decoder/1` | Create an incremental CSV decode state for chunked input |
| `stream_feed/2` | Feed a chunk to a CSV stream decoder, returning completed rows |
| `stream_eof/1` | Flush a CSV stream decoder at end-of-input |

## Benchmarking CSV

```sh
$ make bench-csv
===> Running benchmarks (optimization: O3 - PGO)

(numbers in µs)
CSV               small (1.3K)          medium (130.9K)         large (3433.1K)
                decode     encode       decode     encode       decode     encode
-----------------------------------------------------------------------------------
glazer            8.3      4.1      769.2    400.7    45725.1  18175.5
rusty_csv        27.3      n/a     1282.7      n/a    47439.2      n/a
nimble_csv       40.5     34.7     5123.6   6509.5    375516.5  247815.2
csv              65.2    198.1    11383.0  26980.5    TIMEOUT  TIMEOUT
erl_csv          35.9     35.1     2587.6   3623.7    115074.7  143284.3
```

### glazer vs rusty_csv

> Note: [`rusty_csv`](https://hex.pm/packages/rusty_csv) is a Rust NIF (via
`rustler`) and the closest performance comparison for glazer's CSV
decoder — both use SIMD (AVX2/SSE2) to scan for delimiters/quotes and
return zero-copy sub-binaries for unescaped fields. It's excluded from the
default `make bench-csv` table above because it can't be `deps.get`'d
alongside `yaml_rustler` (incompatible `rustler` version constraints — see
the `BENCH_SET` note in the Makefile); run it explicitly with
`make bench-csv BENCH_SET=csv`:
>
```sh
$ make bench-csv BENCH_SET=csv
```
>
> The benchmarking table above has the merged results of running with `BENCH_SET=csv`
and without.
>
> (`rusty_csv` has no CSV encoder, so its encode column is `n/a`.)

Decode is within a few percent either way across file sizes — small-input
overhead favors glazer (no per-call Rust/NIF marshalling layer beyond
`rustler`'s own), and medium/large decode is close to a tie, with the
remainder being run-to-run noise rather than a structural gap. Profiling
glazer's large-file decode (3.4 MB / 25K rows / 150K fields) by
incrementally stubbing out parts of the pipeline shows where the time
actually goes:

| Stage | Share of decode time |
|---|---|
| SIMD scan (find delimiters/quotes) | ~7% |
| `enif_make_sub_binary` per field | ~31% |
| `enif_make_list_from_array` per row | ~26% |
| Remaining bookkeeping (field/row vectors, outer list) | ~34% |

Scanning is a small fraction of the total; the dominant cost is the NIF
term-construction calls inherent to the `[[field, ...], ...]` row-of-lists
shape both libraries return — `rusty_csv` pays the same `enif_make_sub_binary`
and list-construction costs per field/row, just batched at the end of a
two-phase scan-then-extract design instead of interleaved during scanning
like glazer. There's no scanning-strategy change available that would close
the remaining gap without changing the output term shape itself (e.g.
`{return, tuple}`, which avoids rebuilding a list per row).
