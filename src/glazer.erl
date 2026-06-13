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
-export([encode_integer/1, decode_integer/1, try_decode_integer/1]).
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
-spec format_error(binary(), [term()]) -> binary().
format_error(Format, Args) ->
  lists:flatten(io_lib:format(Format, Args)).