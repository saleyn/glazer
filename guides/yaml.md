# YAML

## Usage

`decode/1,2` decodes a YAML document to an Erlang term — mappings
become maps, sequences become lists, and scalars become the matching
Erlang type (binaries, numbers, booleans, or `null`):

**Erlang**

**Erlang**

```erlang
1> glazer_yaml:decode(<<"a: 1\nb:\n- true\n- null\n- 3.5\n">>).
#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}

2> glazer_yaml:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"a: 1\nb:\n- true\n- null\n- 3.5\n">>
```

**Elixir**

```elixir
iex> Glazer.YAML.decode!("a: 1\nb:\n  - true\n  - null\n  - 3.5\n")
%{"a" => 1, "b" => [true, :null, 3.5]}

iex> Glazer.YAML.decode!("a: 1\nb:\n  - true\n  - null\n  - 3.5\n", [:use_nil])
%{"a" => 1, "b" => [true, nil, 3.5]}

iex> Glazer.YAML.encode!(%{"a" => 1, "b" => [true, nil, 3.5]})
"a: 1\nb:\n- true\n- null\n- 3.5\n"
```

`encode/1,2` encodes an Erlang term to YAML in block style
(2-space indentation, sequences at the same indentation as the mapping
key that owns them). Raises `{encode_error, {Msg, Term}}` if the data
contains a value that cannot be represented as YAML.

## Streaming

There is no incremental YAML decoder. YAML's block styles have no
closing delimiter — a mapping or sequence simply ends at a dedent or
end-of-input — so there is no way to scan a partial buffer for "is this
value complete yet?" the way [`scan/1,2`](json.md#efficiency) does for
JSON's bracket-balanced syntax. Decode full YAML documents with
`decode/1,2` once they are fully buffered.

## YAML decode options

| Option | Description |
|---|---|
| `use_nil` | Use the atom `nil` for YAML `null`/`~`/empty values |
| `{null, Atom}` | Use `Atom` for YAML `null`/`~`/empty values |
| `{keys, atom}` | Decode mapping keys as atoms |
| `{keys, existing_atom}` | Decode mapping keys as existing atoms, falling back to binaries for unknown atoms |
| `{keys, binary}` | Decode mapping keys as binaries (default) |
| `yaml_1_1_bools` | Additionally treat `yes`/`no`/`on`/`off` (and case variants) as booleans, per the YAML 1.1 core schema. By default (YAML 1.2 core schema) only `true`/`false` are recognized as booleans |
| `copy_strings` | Always allocate a fresh binary for each decoded scalar, instead of a zero-copy sub-binary of the input for single-line plain scalars (see [Performance Optimization Details](performance.md#performance-optimization-details)) |

**Erlang**

```erlang
1> glazer_yaml:decode(<<"a: ~\n">>, [use_nil]).
#{<<"a">> => nil}

2> glazer_yaml:decode(<<"a: 1\n">>, [{keys, atom}]).
#{a => 1}

3> glazer_yaml:decode(<<"a: yes\n">>, [yaml_1_1_bools]).
#{<<"a">> => true}
```

**Elixir**

```elixir
iex> Glazer.YAML.decode!("a: ~\n", [:use_nil])
%{"a" => nil}

iex> Glazer.YAML.decode!("a: 1\n", keys: :atom)
%{a: 1}

iex> Glazer.YAML.decode!("a: yes\n", [:yaml_1_1_bools])
%{"a" => true}
```

## YAML encode options

| Option | Description |
|---|---|
| `use_nil` | Treat the atom `nil` as YAML `null` |
| `{null, Atom}` | Treat `Atom` as YAML `null` |

**Erlang**

```erlang
1> glazer_yaml:encode(#{<<"a">> => nil}).
<<"a: null\n">>
```

**Elixir**

```elixir
iex> Glazer.YAML.encode!(%{"a" => nil})
"a: null\n"
```

## API

All functions below are in `glazer_yaml`.

| Function | Description |
|---|---|
| `decode/1`, `decode/2` | Decode a YAML binary or iolist to an Erlang term |
| `try_decode/1`, `try_decode/2` | Decode YAML, returning `{ok, Term}` or `{error, Msg}` instead of raising |
| `encode/1`, `encode/2` | Encode an Erlang term to a YAML binary in block style; raises `{encode_error, {Msg, Term}}` on failure |
| `read_file/1`, `read_file/2` | Read a file and decode its contents as YAML |
| `write_file/2`, `write_file/3` | Encode a term to YAML and write it to a file |

## Benchmarking YAML

```sh
$ make bench-yaml
===> Running benchmarks (optimization: O3 - PGO)

(numbers in µs)
YAML             openrtb (1.3K)       esad (1.3K)         small (0.1K)
                decode   encode     decode   encode     decode   encode
-------------------------------------------------------------------------
glazer           57.8     19.6       39.8      8.9        2.8      1.3
yaml_rustler    170.9      n/a      102.8      n/a       12.0      n/a
fast_yaml       320.2     77.6      159.0     49.2       23.2      8.3
yamerl         2609.6      n/a     2063.7      n/a     1009.7      n/a
ymlr              n/a     79.5        n/a     85.2        n/a     12.3
===> Running benchmarks (optimization: O3 - PGO)
===> Process niceness: 0 (lower is higher priority)
```
