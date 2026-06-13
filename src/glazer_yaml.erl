-module(glazer_yaml).
-moduledoc """
Fast YAML encoding and decoding using the glaze C++ library.

By default `null`s are represented as the atom `null`. To change it
application-wide, set the `null` env key in your config:
```
{glazer, [{null, nil}]}.
```

## Features

- Decoding YAML mappings/sequences/scalars to Erlang maps/lists/scalars,
  including big integers
- Encoding Erlang terms to YAML in block style
- Configurable representation of YAML `null` and mapping keys, with
  optional YAML 1.1 boolean compatibility (`yes`/`no`/`on`/`off`)
- `read_file/1,2` and `write_file/2,3` helpers for decoding/encoding
  directly to/from a file

## Example

```erlang
1> glazer_yaml:decode(<<"a: 1\nb:\n  - true\n  - null\n  - 3.5\n">>).
#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}

2> glazer_yaml:encode(#{<<"a">> => 1, <<"b">> => [true, null, 3.5]}).
<<"a: 1\nb:\n  - true\n  - null\n  - 3.5\n">>
```

See also [https://github.com/stephenberry/glaze]
""".
-export([decode/1, decode/2, try_decode/1, try_decode/2,
         encode/1, encode/2,
         read_file/1, read_file/2, write_file/2, write_file/3]).

-type decode_opt() ::
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
-type decode_opts() :: [decode_opt()].

-type encode_opt() ::
    use_nil
  | {null_term, atom()}.

-doc """
YAML encode options:

- `use_nil`           - treat the atom `nil` as YAML `null`
- `{null_term, Atom}` - treat `Atom` as YAML `null`
""".
-type encode_opts() :: [encode_opt()].

-export_type([decode_opt/0, decode_opts/0, encode_opt/0, encode_opts/0]).

-doc """
Decode a YAML binary or iolist to an Erlang term. YAML mappings are returned
as maps (default). Raises `{parse_error, Reason}` on invalid input.
""".
-spec decode(binary() | iolist()) -> term().
decode(Input) ->
  case try_decode(Input) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error(Reason)
  end.

-doc """
Decode a YAML binary or iolist to an Erlang term with options.
Raises `{parse_error, Msg}` on invalid input.

## Example

```erlang
1> glazer_yaml:decode(<<"a: ~\n">>, [use_nil]).
#{<<"a">> => nil}

2> glazer_yaml:decode(<<"a: 1\n">>, [{keys, atom}]).
#{a => 1}

3> glazer_yaml:decode(<<"a: yes\n">>, [yaml_1_1_bools]).
#{<<"a">> => true}
```
""".
-spec decode(binary() | iolist(), decode_opts()) -> term().
decode(Input, Opts) ->
  case try_decode(Input, Opts) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error({parse_error, Reason})
  end.

-doc """
Decode a YAML binary or iolist, returning `{ok, Term}` or
`{error, Msg}` instead of raising.
""".
-spec try_decode(binary() | iolist()) -> {ok, term()} | {error, binary()}.
try_decode(Input) ->
  glazer:yaml_try_decode(Input).

-doc """
Decode a YAML binary or iolist with options, returning `{ok, Term}` or
`{error, Msg}` instead of raising.
""".
-spec try_decode(binary() | iolist(), decode_opts()) -> {ok, term()} | {error, binary()}.
try_decode(Input, Opts) ->
  glazer:yaml_try_decode(Input, Opts).

-doc """
Encode an Erlang term to a YAML binary in block style (2-space indentation,
sequences at the same indentation as the mapping key that owns them).
""".
-spec encode(term()) -> binary().
encode(Data) ->
  glazer:yaml_encode(Data).

-doc """
Encode an Erlang term to a YAML binary in block style with options.

## Example

```erlang
1> glazer_yaml:encode(#{<<"a">> => nil}, [use_nil]).
<<"a: null\n">>
```
""".
-spec encode(term(), encode_opts()) -> binary().
encode(Data, Opts) ->
  glazer:yaml_encode(Data, Opts).

-doc """
Read `Filename` and decode its contents as YAML.

Raises `{parse_error, Reason}` if the file's contents aren't valid YAML, or
a binary `"Filename: Reason"` message (see `file:format_error/1`) if the
file can't be read.

## Example

```erlang
1> glazer_yaml:read_file("data.yaml").
#{<<"a">> => 1}
```
""".
-spec read_file(file:name_all()) -> term().
read_file(Filename) ->
  read_file(Filename, []).

-doc """
Read `Filename` and decode its contents as YAML, with decode options
(see `decode/2`).
""".
-spec read_file(file:name_all(), decode_opts()) -> term().
read_file(Filename, Opts) ->
  case file:read_file(Filename) of
    {ok, Bin}       -> decode(Bin, Opts);
    {error, Reason} -> error(glazer:format_error("~ts: ~ts", [Filename,file:format_error(Reason)]))
  end.

-doc """
Encode `Data` to YAML and write it to `Filename`, overwriting any existing
file.

Raises a binary `"Filename: Reason"` message (see `file:format_error/1`)
if the file can't be written.

## Example

```erlang
1> glazer_yaml:write_file("data.yaml", #{<<"a">> => 1}).
ok
```
""".
-spec write_file(file:name_all(), term()) -> ok.
write_file(Filename, Data) ->
  write_file(Filename, Data, []).

-doc """
Encode `Data` to YAML with encode options (see `encode/2`) and write it to
`Filename`, overwriting any existing file.
""".
-spec write_file(file:name_all(), term(), encode_opts()) -> ok.
write_file(Filename, Data, Opts) ->
  case file:write_file(Filename, encode(Data, Opts)) of
    ok              -> ok;
    {error, Reason} -> error(glazer:format_error("~ts: ~ts", [Filename, file:format_error(Reason)]))
  end.
