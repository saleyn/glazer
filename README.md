# glazer

[![build](https://github.com/saleyn/glazer/actions/workflows/erlang.yaml/badge.svg)](https://github.com/saleyn/glazer/actions/workflows/erlang.yaml)
[![Hex.pm](https://img.shields.io/hexpm/v/glazer.svg)](https://hex.pm/packages/glazer)
[![Hex.pm](https://img.shields.io/hexpm/dt/glazer.svg)](https://hex.pm/packages/glazer)

Fast Erlang NIF JSON encoder/decoder with a hand-rolled recursive-descent
decoder and direct term-to-JSON encoder that produce/consume native Erlang
terms in a single pass. Inspired by the
[glaze](https://github.com/stephenberry/glaze) C++ library, `glazer` has
matured into a standalone implementation with no external C++ dependencies.

## Features

- Decoding straight to Erlang terms: maps, lists, binaries, integers
  (including bignums), floats, booleans, and `null`
- Encoding Erlang terms straight to JSON, including big integers
- Incremental/streaming decoding of partial input (e.g. NDJSON over a
  socket) via `stream_decoder/0,1`, `stream_feed/2`, `stream_eof/1`
- Configurable representation of JSON `null` and JSON object keys
- `minify/1` and `prettify/1` helpers
- Standalone big-integer encode/decode helpers
  (`encode_bigint/1`, `decode_bigint/1`)

## Installation

### Erlang

Add `glazer` to your `rebar.config` deps:

```erlang
{deps, [
  {glazer, "~> 0.1"}
]}.
```

Building the NIF requires a C++23 compiler (GCC 12+ or Clang 16+) and
`make`. There are no external C++ library dependencies — all C++ code is
self-contained in `c_src/`. A plain

```sh
make
```

builds `priv/glazer.so` and compiles the Erlang sources. For the fastest
possible binary, run a Profile-Guided Optimisation (PGO) build instead:

```sh
make optimize
```

This performs three steps automatically: compiles an instrumented binary,
runs the test suite to collect real branch-frequency data, then recompiles
with those profiles applied. The resulting `.so` typically outperforms a
plain `-O3` build by 5–15% on realistic JSON workloads.

### Elixir

Add `glazer` to your `mix.exs` deps:

```elixir
def deps do
  [
    {:glazer, "~> 0.1"}
  ]
end
```

Then fetch and compile:

```sh
make
```

`glazer` is an Erlang application with a Rebar-based C++ NIF build;
`mix` invokes the same top-level `Makefile`/`rebar3 compile` path
described above, so the same C++23 compiler requirement applies.
Once compiled, call it via the `:glazer` module from Elixir:

```elixir
iex> :glazer.decode(~s({"a":1,"b":[true,null,3.5]}))
%{"a" => 1, "b" => [true, :null, 3.5]}

iex> :glazer.encode(%{"a" => 1, "b" => [true, :null, 3.5]})
"{\"a\":1,\"b\":[true,null,3.5]}"
```

Use the `use_nil`/`{null_term, nil}` option (see [JSON `null`](#json-null)
below) to get idiomatic Elixir `nil` instead of the atom `:null`.

## Usage

```erlang
1> glazer:decode(<<"{\"a\":1,\"b\":[true,null,3.5]}">>).
#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}

2> glazer:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"{\"a\":1,\"b\":[true,null,3.5]}">>

3> glazer:encode(#{a => 1}, [pretty]).
<<"{\n  \"a\": 1\n}">>

4> glazer:minify(<<" { \"a\" : 1 } ">>).
{ok, <<"{\"a\":1}">>}

5> glazer:prettify(<<"{\"a\":1}">>).
{ok, <<"{\n  \"a\": 1\n}">>}
```

### Streaming

For input that arrives in chunks — e.g. reading a large document
incrementally, or consuming newline-delimited JSON (NDJSON) from a
socket or file — `stream_decoder/0,1` provides a small stateful
wrapper that buffers partial input and decodes each JSON value as soon
as it's complete, without re-parsing bytes you've already seen:

```erlang
1> D0 = glazer:stream_decoder(),
2> {Vals1, D1} = glazer:stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
3> Vals1.
[#{<<"a">> => 1}]

4> {Vals2, D2} = glazer:stream_feed(D1, <<"2}">>),
5> Vals2.
[#{<<"b">> => 2}]

6> glazer:stream_eof(D2).
{ok, []}
```

`stream_feed/2` returns the list of values completed by the chunk just
fed (possibly empty, possibly more than one if the chunk completes
several values) along with the updated decoder state to pass to the
next call. Once the input is exhausted, call `stream_eof/1` to flush
any trailing bare scalar (numbers, strings, etc. have no closing
delimiter of their own) and surface an error if the buffer holds an
incomplete value:

```erlang
1> D0 = glazer:stream_decoder(),
2> {[], D1} = glazer:stream_feed(D0, <<"   42">>),
3> glazer:stream_eof(D1).
{ok, [42]}
```

`stream_decoder/1` accepts the same options as `decode/2` (e.g.
`{keys, atom}`, `use_nil`) and applies them to every decoded value.

#### Efficiency

`stream_feed/2` only scans for value *boundaries* incrementally —
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

This makes `stream_feed/2` well suited to byte-at-a-time or
small-chunk feeding (e.g. consuming a `gen_tcp`/`gen_statem` socket
buffer as it fills) without the quadratic-rescan cost a naive
"concatenate and retry full decode" loop would incur on large or
slow-arriving documents.

Under the hood, `stream_feed/2` is built on `scan/1,2` — a low-level
primitive that scans a buffer for the byte offset where the next JSON
value ends (or reports that more input is needed) without doing a full
decode. It's exposed directly for callers that want to implement their
own framing/buffering strategy:

```erlang
1> glazer:scan(<<"{\"a\":1} {\"b\":2}">>).
{complete, 7}

2> glazer:scan(<<"{\"a\":">>).
{incomplete, ScanState}

3> glazer:scan(<<"{\"a\":1}">>, ScanState).
{complete, 7}
```

### JSON `null`

By default, JSON `null` decodes to (and `null` encodes from) the atom
`null`. This can be overridden:

- Application-wide, via the `null` environment key — set this once in
  your `sys.config` (or `rebar.config` `relx`/`shell` config) and every
  call uses it as the default:

  ```erlang
  {glazer, [{null, nil}]}
  ```

- Per call, with the `use_nil` shorthand or the `{null_term, Atom}`
  option (see [Options](#options) below). Per-call options always take
  precedence over the application-wide default.

### Big integers

JSON numbers that don't fit into a 64-bit integer are decoded as
Erlang big integers (and big integers are encoded back to their exact
decimal JSON representation):

```erlang
1> glazer:decode(<<"123456789012345678901234567890">>).
123456789012345678901234567890

2> glazer:encode(123456789012345678901234567890).
<<"123456789012345678901234567890">>
```

`encode_bigint/1` and `decode_bigint/1` expose the same conversion
routines directly, independent of JSON parsing/encoding:

```erlang
1> glazer:encode_bigint(123456789012345678901234567890).
{ok, <<"123456789012345678901234567890">>}

2> glazer:decode_bigint(<<"123456789012345678901234567890">>).
{ok, 123456789012345678901234567890}
```

## Options

### Decode options (`decode/2`)

| Option | Description |
|---|---|
| `return_maps` | Decode JSON objects as Erlang maps (default) |
| `object_as_tuple` | Decode JSON objects as `{[{Key, Value}]}` proplist tuples (jiffy-style) |
| `use_nil` | Use the atom `nil` for JSON `null` |
| `{null_term, Atom}` | Use `Atom` for JSON `null` |
| `{keys, atom}` | Decode object keys as atoms (via `binary_to_atom/2`-equivalent) |
| `{keys, existing_atom}` | Decode object keys as existing atoms, falling back to binaries for unknown atoms |
| `{keys, binary}` | Decode object keys as binaries (default) |

```erlang
1> glazer:decode(<<"{\"a\":1}">>, [object_as_tuple]).
{[{<<"a">>, 1}]}

2> glazer:decode(<<"{\"a\":1}">>, [{keys, atom}]).
#{a => 1}

3> glazer:decode(<<"null">>, [use_nil]).
nil

4> glazer:decode(<<"null">>, [{null_term, undefined}]).
undefined
```

### Encode options (`encode/2`)

| Option | Description |
|---|---|
| `pretty` | Pretty-print the JSON output with two-space indentation |
| `uescape` | Escape non-ASCII characters as `\uXXXX` sequences |
| `force_utf8` | Sanitize invalid UTF-8 byte sequences before encoding |
| `use_nil` | Encode the atom `nil` as JSON `null` |
| `{null_term, Atom}` | Encode `Atom` as JSON `null` |

```erlang
1> glazer:encode(#{a => 1}, [pretty]).
<<"{\n  \"a\": 1\n}">>

2> glazer:encode(<<"héllo"/utf8>>, [uescape]).
<<"\"h\\u00e9llo\"">>

3> glazer:encode(nil, [use_nil]).
<<"null">>
```

## API

| Function | Description |
|---|---|
| `decode/1`, `decode/2` | Decode a JSON binary or iolist to an Erlang term |
| `encode/1`, `encode/2` | Encode an Erlang term to a JSON binary |
| `minify/1` | Remove unnecessary whitespace from a JSON document |
| `prettify/1` | Pretty-print a JSON document with two-space indentation |
| `encode_bigint/1` | Encode an integer to its JSON decimal-string representation |
| `decode_bigint/1` | Decode a JSON number string to an Erlang integer |
| `scan/1`, `scan/2` | Scan a buffer for the end offset of the next complete JSON value |
| `stream_decoder/0`, `stream_decoder/1` | Create an incremental-decode state for chunked input |
| `stream_feed/2` | Feed a chunk to a stream decoder, returning completed values |
| `stream_eof/1` | Flush a stream decoder at end-of-input |

See the module's EDoc comments (`src/glazer.erl`) for full type
specs and details.

## Benchmarks

A comparison benchmark against other JSON libraries (`simdjsone`,
`jiffy`, `jason`, `thoas`, `euneus`, OTP's built-in `json`, and
`torque`) is available via:

```sh
$ make bench
Running benchmarks...

(numbers in µs)
               twitter (616.7K)     twitter2 (758.0K)     openrtb (1.2K)         esad (1.3K)         small (0.1K)
               decode   encode     decode   encode     decode   encode     decode   encode     decode   encode
---------------------------------------------------------------------------------------------------------------------
glazer         9517.1   3137.2    11433.2   6162.7       18.2     14.4       12.9      8.0        2.2      2.1
torque        10684.4   4054.6    12462.9   6447.4       18.4     14.2       13.3      8.7        5.6      2.2
simdjsone     11247.9   7240.1    18182.3  12374.3       28.7     30.7       16.2     21.8        2.3      4.2
jiffy         29433.9   4731.5    45569.0   8103.0       53.4     24.7       34.3     13.6        7.0      4.4
jason         21190.0  12235.7    36822.3  22125.9       49.2     39.2       27.8     22.0        4.1      3.9
thoas         21363.8  12791.1    37789.9  23072.4       54.2     38.6       37.9     22.9        3.4      3.9
euneus        20946.2  10733.7    28909.2  21062.5       48.3     32.4       25.6     19.3        7.5      2.5
json          20101.3  10567.5    27445.5  20646.1       45.6     32.3       25.4     10.3        9.0      3.1
```

(requires the `bench`/`dev` Mix dependencies — see `mix.exs`).

### Performance

`glazer` is roughly on par with `torque` (a Rust `sonic-rs` NIF) across
the benchmarked workloads — neither library is consistently faster, and the
gap on any given file/operation is typically within a few percent. Both sit
well ahead of the other contenders (`simdjsone`, `jiffy`, and the pure-Elixir
libraries `jason`, `thoas`, `euneus`, and OTP's built-in `json`).

Where `glazer` has an edge over `torque`:

- **No tuple-of-binaries intermediate representation.** `glazer` decodes
  straight to native Erlang terms (maps, lists, binaries, numbers) and
  encodes straight from them, in a single pass, with no generic JSON-tree
  staging step — minimizing allocation and copying on both the decode and
  encode paths.
- **Big integer support.** JSON numbers that overflow 64 bits decode to
  Erlang bignums (and encode back to their exact decimal form) — see
  [Big integers](#big-integers). `torque` does not support this.
- **Configurable `null` and object-key representation.** `null_term`/`use_nil`
  and `{keys, atom | existing_atom | binary}` let you tailor the decoded
  shape to your application without a post-processing pass.
- **`uescape`/`force_utf8` encode options** for `\uXXXX`-escaping non-ASCII
  output and sanitizing invalid UTF-8 — useful when targeting strict JSON
  consumers or transports that aren't UTF-8 clean.
- **Standalone `minify/1`/`prettify/1` and big-integer helpers**
  (`encode_bigint/1`/`decode_bigint/1`) that don't require a full
  decode/encode round-trip.
- **No external C++ dependencies.** The NIF is fully self-contained —
  no CMake, no `FetchContent`, no vendored third-party library to pull
  at build time — vs. `torque`'s reliance on a Rust toolchain and
  `sonic-rs`, which adds a second language/toolchain to the build.

### Performance optimizations

A few implementation techniques in `c_src/glaze_nif.cpp` account for most
of the gap over the slower contenders:

- **Single-pass, zero-copy decode/encode.** As noted above, there's no
  intermediate generic JSON tree — the decoder builds Erlang terms directly
  from the input bytes (string keys/values are views into the original
  binary whenever no escaping is needed) and the encoder writes JSON bytes
  directly from Erlang terms. This removes a whole staging
  allocate-and-copy pass that tree-based decoders pay for.

- **Inline, growable output buffer (`OutBuf`).** Encoding writes into a
  4 KB stack-allocated buffer first; only documents that exceed that spill
  to the heap, growing geometrically via `malloc`/`realloc` (the latter
  resizes in place when possible, avoiding a copy on every growth — a
  plain `new[]`/`delete[]` doubling strategy can't do this).

- **Key cache for repeated object keys (`KeyCache`).** Real-world JSON
  documents reuse the same small set of key strings heavily (e.g. a
  Twitter feed has ~13K key occurrences across only ~94 distinct keys).
  `KeyCache` is an open-addressed hash table (power-of-two size, linear
  probing, FNV-1a hash with a precomputed-hash fast-reject before the
  `memcmp`) that lets a repeated key reuse the same already-built
  `ERL_NIF_TERM` binary instead of paying `enif_make_new_binary` + `memcpy`
  again. It's only engaged for inputs above a size threshold
  (`KEY_CACHE_MIN_SIZE`), since small payloads (RPC-sized messages) rarely
  repeat keys enough to amortize the lookup cost.

- **Epoch-counter lazy clearing.** Both `KeyCache` and the scratch buffers
  it touches need to start "empty" on every decode call, but
  zero-initializing a multi-KB table for every single call — including
  tiny documents that never populate it — would cost more than the cache
  saves. Instead each cache entry carries a generation/`epoch` tag; a slot
  is considered live only if its `epoch` matches the cache's current
  `m_epoch` (itself seeded from a process-wide monotonically-increasing
  counter, so leftover garbage from a prior stack frame can never
  coincidentally look live). This makes cache construction effectively
  free, regardless of table size.

- **SWAR whitespace skipping.** `skip_ws` checks the next byte before
  paying for any wider load, then — for runs of whitespace — scans 8 bytes
  at a time using branch-free bit-twiddling ("SIMD within a register") to
  find the first non-whitespace byte, rather than testing one byte at a
  time. Minified JSON (the overwhelmingly common case) has little or no
  structural whitespace, so the single-byte fast path dominates in
  practice.

- **Table-driven string escaping with bulk copies.** JSON string escaping
  scans for runs of bytes that need no escaping (a precomputed 256-entry
  lookup table answers "does this byte need escaping?" in O(1)) and copies
  each run in one `memcpy`, falling into a per-byte switch only for the
  rare characters that actually need an escape sequence.

- **Fast integer formatting.** Integers are written to JSON using a
  lookup-table-based digit-pair algorithm (avoiding division for small
  values) with a vendored `lltoa` fallback for larger numbers — faster
  than routing every integer through `snprintf`.

## Testing

```sh
make test
```

runs the EUnit test suite via `rebar3 eunit`.

## License

MIT License — see [LICENSE](LICENSE) for details.
