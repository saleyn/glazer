%%%----------------------------------------------------------------------------
%%% @doc Fast JSON encoding and decoding using the glaze C++ library.
%%%
%%% By default JSON `null' is represented as the atom `null'.
%%% To change it application-wide, set the `null' env key in your config:
%%% ```
%%% {glazejson, [{null, nil}]}.
%%% '''
%%%
%%% See also [https://github.com/stephenberry/glaze]
%%% @end
%%%----------------------------------------------------------------------------
-module(glazejson).
-export([decode/1, decode/2, encode/1, encode/2, minify/1, prettify/1,
         encode_bigint/1, decode_bigint/1]).

-on_load(init/0).

-define(LIBNAME, glazejson).
-define(NOT_LOADED_ERROR,
  erlang:nif_error({not_loaded, [{module, ?MODULE}, {line, ?LINE}]})).

-type decode_opt() ::
    return_maps
  | object_as_tuple
  | use_nil
  | {null_term, atom()}
  | {keys, atom | existing_atom | binary}.

-type decode_opts() :: [decode_opt()].
%% Decode options:
%% <ul>
%% <li>`return_maps'         - decode JSON objects as Erlang maps (default)</li>
%% <li>`object_as_tuple'     - decode JSON objects as `{[{K, V}]}' proplists</li>
%% <li>`use_nil'             - use the atom `nil' for JSON null</li>
%% <li>`{null_term, Atom}'   - use `Atom' for JSON null</li>
%% <li>`{keys, atom}'        - decode object keys as atoms</li>
%% <li>`{keys, existing_atom}' - decode keys as existing atoms, fall back to binary</li>
%% <li>`{keys, binary}'      - decode keys as binaries (default)</li>
%% </ul>

-type encode_opt() ::
    pretty
  | uescape
  | force_utf8
  | use_nil
  | {null_term, atom()}.

-type encode_opts() :: [encode_opt()].
%% Encode options:
%% <ul>
%% <li>`pretty'            - pretty-print the JSON output</li>
%% <li>`uescape'           - escape non-ASCII characters as \\uXXXX sequences</li>
%% <li>`force_utf8'        - fix invalid UTF-8 sequences before encoding</li>
%% <li>`use_nil'           - encode the atom `nil' as JSON `null'</li>
%% <li>`{null_term, Atom}' - encode `Atom' as JSON `null'</li>
%% </ul>

-export_type([decode_opts/0, encode_opts/0]).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").
-endif.

init() ->
  NullVal = application:get_env(?LIBNAME, null, null),
  is_atom(NullVal) orelse erlang:error("glazejson: option 'null' must be an atom"),
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

%% @doc Decode a JSON binary or iolist to an Erlang term.
%% JSON objects are returned as maps (default).
-spec decode(binary() | iolist()) -> term().
decode(_Input) ->
  ?NOT_LOADED_ERROR.

%% @doc Decode a JSON binary or iolist to an Erlang term with options.
-spec decode(binary() | iolist(), decode_opts()) -> term().
decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

%% @doc Encode an Erlang term to a JSON binary.
-spec encode(term()) -> binary().
encode(Data) ->
  encode(Data, []).

%% @doc Encode an Erlang term to a JSON binary with options.
-spec encode(term(), encode_opts()) -> binary().
encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

%% @doc Minify a JSON binary or iolist, removing all unnecessary whitespace.
-spec minify(binary() | iolist()) -> {ok, binary()} | {error, binary()}.
minify(_Input) ->
  ?NOT_LOADED_ERROR.

%% @doc Pretty-print a JSON binary or iolist with two-space indentation.
-spec prettify(binary() | iolist()) -> {ok, binary()} | {error, binary()}.
prettify(_Input) ->
  ?NOT_LOADED_ERROR.

%% @doc Encode a big integer to its JSON string representation.
-spec encode_bigint(integer()) -> {ok, binary()} | {error, binary()}.
encode_bigint(_BigInt) ->
  ?NOT_LOADED_ERROR.

%% @doc Decode a JSON number string to a big integer.
-spec decode_bigint(binary() | iolist()) -> {ok, integer()} | {error, binary()}.
decode_bigint(_NumberString) ->
  ?NOT_LOADED_ERROR.

%%%----------------------------------------------------------------------------
%%% Tests
%%%----------------------------------------------------------------------------
-ifdef(EUNIT).

encode_test_() ->
  [
    ?_assertEqual(<<"null">>,          encode(null)),
    ?_assertEqual(<<"null">>,          encode(nil, [use_nil])),
    ?_assertEqual(<<"true">>,          encode(true)),
    ?_assertEqual(<<"false">>,         encode(false)),
    ?_assertEqual(<<"1">>,             encode(1)),
    ?_assertEqual(<<"1.5">>,           encode(1.5)),
    ?_assertEqual(<<"\"hello\"">>,     encode(<<"hello">>)),
    ?_assertEqual(<<"[1,2,3]">>,       encode([1, 2, 3])),
    ?_assertEqual(<<"{}">>,            encode(#{})),
    ?_assertMatch(<<"{", _/binary>>,   encode(#{<<"a">> => 1}))
  ].

decode_test_() ->
  [
    ?_assertEqual(null,                         decode(<<"null">>)),
    ?_assertEqual(nil,                          decode(<<"null">>, [use_nil])),
    ?_assertEqual(true,                         decode(<<"true">>)),
    ?_assertEqual(false,                        decode(<<"false">>)),
    ?_assertEqual(1,                            decode(<<"1">>)),
    ?_assertEqual(1.5,                          decode(<<"1.5">>)),
    ?_assertEqual(<<"hello">>,                  decode(<<"\"hello\"">>)),
    ?_assertEqual([1, 2, 3],                    decode(<<"[1,2,3]">>)),
    ?_assertEqual(#{<<"a">> => 1},              decode(<<"{\"a\":1}">>)),
    ?_assertEqual({[{<<"a">>, 1}]},             decode(<<"{\"a\":1}">>, [object_as_tuple])),
    ?_assertEqual(null,                         decode(<<"null">>, [{null_term, null}])),
    ?_assertEqual(my_null,                      decode(<<"null">>, [{null_term, my_null}]))
  ].

roundtrip_test_() ->
  Vals = [null, true, false, 0, 1, -1, 1.5, <<"hello">>, [], [1, 2, 3],
          #{<<"a">> => 1, <<"b">> => [1, 2]},
          #{<<"nested">> => #{<<"x">> => true}}],
  [?_assertEqual(V, decode(encode(V))) || V <- Vals].

minify_test_() ->
  [
    ?_assertEqual({ok, <<"[1,2,3]">>},        minify(<<"[ 1, 2, 3 ]">>)),
    ?_assertEqual({ok, <<"{\"a\":1}">>},       minify(<<" { \"a\" : 1 } ">>))
  ].

prettify_test_() ->
  [
    ?_assertMatch({ok, <<"[\n", _/binary>>},  prettify(<<"[1,2,3]">>)),
    ?_assertMatch({ok, <<"{\n", _/binary>>},  prettify(<<"{\"a\":1}">>))
  ].

-endif.
