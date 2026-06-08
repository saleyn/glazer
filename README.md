# glazejson

[![build](https://github.com/saleyn/glazejson/actions/workflows/erlang.yaml/badge.svg)](https://github.com/saleyn/glazejson/actions/workflows/erlang.yaml)
[![Hex.pm](https://img.shields.io/hexpm/v/glazejson.svg)](https://hex.pm/packages/glazejson)
[![Hex.pm](https://img.shields.io/hexpm/dt/glazejson.svg)](https://hex.pm/packages/glazejson)

Fast Erlang NIF JSON encoder/decoder backed by the
[glaze](https://github.com/stephenberry/glaze) C++ library, with a
hand-rolled recursive-descent decoder and direct term-to-JSON encoder
that produce/consume native Erlang terms in a single pass.

## Features

- Decoding straight to Erlang terms: maps, lists, binaries, integers
  (including bignums), floats, booleans, and `null`
- Encoding Erlang terms straight to JSON, including big integers
- Configurable representation of JSON `null` and JSON object keys
- `minify/1` and `prettify/1` helpers
- Standalone big-integer encode/decode helpers
  (`encode_bigint/1`, `decode_bigint/1`)

## Installation

Add `glazejson` to your `rebar.config` deps:

```erlang
{deps, [glazejson]}.
```

Building the NIF requires a C++23 compiler (GCC 12+ or Clang 16+) and
CMake; the `glaze` C++ library is fetched automatically at build time
via CMake's `FetchContent`. The top-level `Makefile` wires the CMake
build into `rebar3 compile`, so a plain

```sh
rebar3 compile
```

is enough — it builds `priv/glazejson.so` and compiles the Erlang
sources.  Make sure you have a relatively recent C++ compiler version
installed.

## Usage

```erlang
1> glazejson:decode(<<"{\"a\":1,\"b\":[true,null,3.5]}">>).
#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}

2> glazejson:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"{\"a\":1,\"b\":[true,null,3.5]}">>

3> glazejson:encode(#{a => 1}, [pretty]).
<<"{\n  \"a\": 1\n}">>

4> glazejson:minify(<<" { \"a\" : 1 } ">>).
{ok, <<"{\"a\":1}">>}

5> glazejson:prettify(<<"{\"a\":1}">>).
{ok, <<"{\n  \"a\": 1\n}">>}
```

### JSON `null`

By default, JSON `null` decodes to (and `null` encodes from) the atom
`null`. This can be overridden:

- Application-wide, via the `null` environment key — set this once in
  your `sys.config` (or `rebar.config` `relx`/`shell` config) and every
  call uses it as the default:

  ```erlang
  {glazejson, [{null, nil}]}
  ```

- Per call, with the `use_nil` shorthand or the `{null_term, Atom}`
  option (see [Options](#options) below). Per-call options always take
  precedence over the application-wide default.

### Big integers

JSON numbers that don't fit into a 64-bit integer are decoded as
Erlang big integers (and big integers are encoded back to their exact
decimal JSON representation):

```erlang
1> glazejson:decode(<<"123456789012345678901234567890">>).
123456789012345678901234567890

2> glazejson:encode(123456789012345678901234567890).
<<"123456789012345678901234567890">>
```

`encode_bigint/1` and `decode_bigint/1` expose the same conversion
routines directly, independent of JSON parsing/encoding:

```erlang
1> glazejson:encode_bigint(123456789012345678901234567890).
{ok, <<"123456789012345678901234567890">>}

2> glazejson:decode_bigint(<<"123456789012345678901234567890">>).
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
1> glazejson:decode(<<"{\"a\":1}">>, [object_as_tuple]).
{[{<<"a">>, 1}]}

2> glazejson:decode(<<"{\"a\":1}">>, [{keys, atom}]).
#{a => 1}

3> glazejson:decode(<<"null">>, [use_nil]).
nil

4> glazejson:decode(<<"null">>, [{null_term, undefined}]).
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
1> glazejson:encode(#{a => 1}, [pretty]).
<<"{\n  \"a\": 1\n}">>

2> glazejson:encode(<<"héllo"/utf8>>, [uescape]).
<<"\"h\\u00e9llo\"">>

3> glazejson:encode(nil, [use_nil]).
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

See the module's EDoc comments (`src/glazejson.erl`) for full type
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
glazejson     10097.9   3947.9    14904.2   8186.0       17.4     12.5       14.8      8.7        1.3      1.5
torque        10151.7   4358.7    12899.5   6798.9       18.3     13.2       19.9      7.1        4.5      1.7
simdjsone     10345.9   7541.2    18973.3  13482.5       25.6     27.5       19.5     18.5        1.7      4.5
jiffy         30645.2   4347.6    51053.1   9500.1       50.0     28.6       32.2     19.0        7.4      4.2
jason         21005.7  12918.1    40277.2  25064.8       56.4     26.2       33.7     22.1        6.0      3.7
thoas         21151.4  13779.6    41390.0  25625.0       57.4     29.9       35.0     26.7        7.5      3.8
euneus        20488.9  12319.9    31853.9  25111.0       40.7     32.7       25.2     19.0        7.3      3.3
json          19887.1  11679.8    30902.8  24087.7       41.5     26.9       40.1     10.6        4.8      4.1
```

(requires the `bench`/`dev` Mix dependencies — see `mix.exs`).

### Performance

`glazejson` is roughly on par with `torque` (a Rust `sonic-rs` NIF) across
the benchmarked workloads — neither library is consistently faster, and the
gap on any given file/operation is typically within a few percent. Both sit
well ahead of the other contenders (`simdjsone`, `jiffy`, and the pure-Elixir
libraries `jason`, `thoas`, `euneus`, and OTP's built-in `json`).

Where `glazejson` has an edge over `torque`:

- **No tuple-of-binaries intermediate representation.** `glazejson` decodes
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
- **Built on [glaze](https://github.com/stephenberry/glaze)**, a mature,
  actively-maintained, header-only C++ JSON library — vs. `torque`'s
  reliance on a Rust toolchain and `sonic-rs`, which adds a second
  language/toolchain to the build.

## Testing

```sh
make test
```

runs the EUnit test suite via `rebar3 eunit`.

## License

MIT License — see [LICENSE](LICENSE) for details.
