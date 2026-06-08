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
sources.

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
glazejson     14160.9   4774.0    13771.9   7049.2       15.7     12.0       14.1      7.4        1.1      1.9
torque        15895.8   4613.0    12699.7   5822.6       15.4     11.6       16.6      7.1        3.6      2.2
simdjsone     15764.0   8182.0    17115.5  12931.4       25.7     27.3       15.4     20.1        2.0      4.4
jiffy         35805.8   4622.6    45737.4   8915.5       41.7     21.8       29.6     11.8        6.5      3.0
jason         27518.9  12206.7    37930.1  22581.0       56.1     28.7       29.5     20.8        4.1      5.1
thoas         27267.8  13443.9    38068.6  23706.7       47.5     36.5       35.4     21.3        6.0      3.1
euneus        26325.0  11579.8    28975.9  21150.8       48.2     25.1       21.9     14.3        8.8      2.7
json          25733.0  11468.9    28398.7  20347.3       41.4     31.7       37.6      9.9        6.6      2.3
```

(requires the `bench`/`dev` Mix dependencies — see `mix.exs`).

## Testing

```sh
make test
```

runs the EUnit test suite via `rebar3 eunit`.

## License

MIT License — see [LICENSE](LICENSE) for details.
