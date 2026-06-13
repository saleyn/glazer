-module(glazer).
-moduledoc """
Fast JSON/YAML/CSV encoding and decoding using the glaze C++ library.

The public API is split across [`glazer_json`](`glazer_json`),
[`glazer_yaml`](`glazer_yaml`), and [`glazer_csv`](`glazer_csv`).

By default `null`s are represented as the atom `null`. To change it
application-wide, set the `null` env key in your config:
```
{glazer, [{null, nil}]}.
```
""".
-export([encode_integer/1, decode_integer/1, try_decode_integer/1,
         compile_path/1, find/2]).

-type path_step() :: {field, binary()} | iterate | {index, integer()}.
-type path()      :: [path_step()].

-export_type([path/0]).
-export([json_try_decode/1, json_try_decode/2,
         yaml_try_decode/1, yaml_try_decode/2, yaml_encode/1, yaml_encode/2,
         csv_try_decode/1, csv_try_decode/2, csv_encode/1, csv_encode/2,
         json_encode/1, json_encode/2, json_minify/1, json_prettify/1,
         json_query/2, json_query/3, json_scan/1, json_scan/2,
         format_error/2]).

-on_load(init/0).

-define(LIBNAME, glazer).
-define(NOT_LOADED_ERROR,
  erlang:nif_error({not_loaded, [{module, ?MODULE}, {line, ?LINE}]})).

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

json_try_decode(_Input) ->
  ?NOT_LOADED_ERROR.

json_try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

yaml_try_decode(_Input) ->
  ?NOT_LOADED_ERROR.

yaml_try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

yaml_encode(_Data) ->
  ?NOT_LOADED_ERROR.

yaml_encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

csv_try_decode(_Input) ->
  ?NOT_LOADED_ERROR.

csv_try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

csv_encode(_Data) ->
  ?NOT_LOADED_ERROR.

csv_encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

json_encode(_Data) ->
  ?NOT_LOADED_ERROR.

json_encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

json_minify(_Input) ->
  ?NOT_LOADED_ERROR.

json_prettify(_Input) ->
  ?NOT_LOADED_ERROR.

json_query(_Input, _Filter) ->
  ?NOT_LOADED_ERROR.

json_query(_Input, _Filter, _DecodeOpts) ->
  ?NOT_LOADED_ERROR.

json_scan(_Bin) ->
  ?NOT_LOADED_ERROR.

json_scan(_Bin, _ScanState) ->
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
Format an error message with `io_lib:format/2` and flatten to a binary.
""".
-spec format_error(io:format(), [term()]) -> binary().
format_error(Format, Args) ->
  iolist_to_binary(io_lib:format(Format, Args)).

-doc """
Compile a [jq](https://jqlang.org/)-style path expression into a `path()`
for use with `find/2`.

Supports a small subset of jq syntax:

- `.`              - identity (returns the input term itself)
- `.foo`, `.foo.bar` - field access (map key)
- `.["foo bar"]`   - bracketed field access, for keys with special characters
- `.[]`            - iterate: every element of a list, or every value of a map
- `.[N]`, `.[-N]`  - index into a list (negative indices count from the end)

Segments can be chained freely, e.g. `.a.b[].c[0]`.

Raises `{invalid_path, Filter}` if `Filter` doesn't match this grammar.

## Example

```erlang
1> glazer:compile_path(<<".a[].b">>).
[{field,<<"a">>},iterate,{field,<<"b">>}]
```
""".
-spec compile_path(binary() | iolist()) -> path().
compile_path(Filter) ->
  case nif_compile_path(iolist_to_binary(Filter)) of
    {ok, Path}      -> Path;
    {error, Reason} -> error(Reason)
  end.

-spec nif_compile_path(binary()) -> {ok, path()} | {error, {invalid_path, binary()}}.
nif_compile_path(_Filter) ->
  ?NOT_LOADED_ERROR.

-doc """
Find value(s) in `Term` by walking `Path`.

`Term` is typically a decoded JSON/YAML document: nested maps and lists.
`Path` is either a `path()` produced by `compile_path/1`, or a raw
jq-style filter string (compiled on the fly — raises `{invalid_path,
Filter}` if it doesn't parse).

Returns the list of values found at the end of `Path`. An empty list means
no match. `.[]` steps fan out over every element of a list (or every value
of a map), so a path containing `.[]` can produce multiple results.

## Example

```erlang
1> Doc = #{<<"a">> => [#{<<"b">> => 1}, #{<<"b">> => 2}, #{<<"c">> => 3}]}.
2> glazer:find(Doc, <<".a[].b">>).
[1, 2]
3> glazer:find(Doc, <<".a[2].c">>).
[3]
```
""".
-spec find(term(), path() | binary()) -> [term()].
find(Term, Filter) when is_binary(Filter) ->
  find_path(Term, compile_path(Filter));
find(Term, Path) when is_list(Path) ->
  find_path(Term, Path).

find_path(Term, []) ->
  [Term];
find_path(Term, [iterate | Rest]) when is_list(Term) ->
  [Found || Elem <- Term, Found <- find_path(Elem, Rest)];
find_path(Term, [iterate | Rest]) when is_map(Term) ->
  [Found || Value <- maps:values(Term), Found <- find_path(Value, Rest)];
find_path(Term, [{index, Index} | Rest]) when is_list(Term) ->
  Pos = if Index < 0 -> length(Term) + Index + 1; true -> Index + 1 end,
  case Pos >= 1 andalso Pos =< length(Term) of
    true  -> find_path(lists:nth(Pos, Term), Rest);
    false -> []
  end;
find_path(Term, [{field, Key} | Rest]) when is_map(Term) ->
  case maps:find(Key, Term) of
    {ok, Value} -> find_path(Value, Rest);
    error       ->
      %% Maps decoded with {keys, atom | existing_atom} have atom keys;
      %% fall back to looking up Key as an existing atom.
      try binary_to_existing_atom(Key, utf8) of
        Atom ->
          case maps:find(Atom, Term) of
            {ok, Value} -> find_path(Value, Rest);
            error       -> []
          end
      catch
        error:badarg -> []
      end
  end;
find_path(_Term, _Path) ->
  [].