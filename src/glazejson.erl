-module(glazejson).
-moduledoc """
Fast JSON encoding and decoding using the glaze C++ library.

By default JSON `null` is represented as the atom `null`. To change it
application-wide, set the `null` env key in your config:
```
{glazejson, [{null, nil}]}.
```

See also [https://github.com/stephenberry/glaze]
""".
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

-doc """
Decode options:

- `return_maps`         - decode JSON objects as Erlang maps (default)
- `object_as_tuple`     - decode JSON objects as `{[{K, V}]}` proplists
- `use_nil`             - use the atom `nil` for JSON null
- `{null_term, Atom}`   - use `Atom` for JSON null
- `{keys, atom}`        - decode object keys as atoms
- `{keys, existing_atom}` - decode keys as existing atoms, fall back to binary
- `{keys, binary}`      - decode keys as binaries (default)
""".
-type decode_opts() :: [decode_opt()].

-type encode_opt() ::
    pretty
  | uescape
  | force_utf8
  | use_nil
  | {null_term, atom()}.

-doc """
Encode options:

- `pretty`            - pretty-print the JSON output
- `uescape`           - escape non-ASCII characters as \\uXXXX sequences
- `force_utf8`        - fix invalid UTF-8 sequences before encoding
- `use_nil`           - encode the atom `nil` as JSON `null`
- `{null_term, Atom}` - encode `Atom` as JSON `null`
""".
-type encode_opts() :: [encode_opt()].

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

-doc """
Decode a JSON binary or iolist to an Erlang term. JSON objects are returned as
maps (default).
""".
-spec decode(binary() | iolist()) -> term().
decode(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Decode a JSON binary or iolist to an Erlang term with options.".
-spec decode(binary() | iolist(), decode_opts()) -> term().
decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc "Encode an Erlang term to a JSON binary.".
-spec encode(term()) -> binary().
encode(Data) ->
  encode(Data, []).

-doc "Encode an Erlang term to a JSON binary with options.".
-spec encode(term(), encode_opts()) -> binary().
encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc "Minify a JSON binary or iolist, removing all unnecessary whitespace.".
-spec minify(binary() | iolist()) -> {ok, binary()} | {error, binary()}.
minify(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Pretty-print a JSON binary or iolist with two-space indentation.".
-spec prettify(binary() | iolist()) -> {ok, binary()} | {error, binary()}.
prettify(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Encode a big integer to its JSON string representation.".
-spec encode_bigint(integer()) -> {ok, binary()} | {error, binary()}.
encode_bigint(_BigInt) ->
  ?NOT_LOADED_ERROR.

-doc "Decode a JSON number string to a big integer.".
-spec decode_bigint(binary() | iolist()) -> {ok, integer()} | {error, binary()}.
decode_bigint(_NumberString) ->
  ?NOT_LOADED_ERROR.

%%%----------------------------------------------------------------------------
%%% Tests
%%%----------------------------------------------------------------------------
-ifdef(EUNIT).

encode_test_() ->
  [
    ?_assertEqual(<<"null">>,        encode(null)),
    ?_assertEqual(<<"null">>,        encode(nil, [use_nil])),
    ?_assertEqual(<<"true">>,        encode(true)),
    ?_assertEqual(<<"false">>,       encode(false)),
    ?_assertEqual(<<"1">>,           encode(1)),
    ?_assertEqual(<<"1.5">>,         encode(1.5)),
    ?_assertEqual(<<"\"hello\"">>,   encode(<<"hello">>)),
    ?_assertEqual(<<"[1,2,3]">>,     encode([1, 2, 3])),
    ?_assertEqual(<<"{}">>,          encode(#{})),
    ?_assertMatch(<<"{", _/binary>>, encode(#{<<"a">> => 1}))
  ].

decode_test_() ->
  [
    ?_assertEqual(null,             decode(<<"null">>)),
    ?_assertEqual(nil,              decode(<<"null">>, [use_nil])),
    ?_assertEqual(true,             decode(<<"true">>)),
    ?_assertEqual(false,            decode(<<"false">>)),
    ?_assertEqual(1,                decode(<<"1">>)),
    ?_assertEqual(1.5,              decode(<<"1.5">>)),
    ?_assertEqual(<<"hello">>,      decode(<<"\"hello\"">>)),
    ?_assertEqual([1, 2, 3],        decode(<<"[1,2,3]">>)),
    ?_assertEqual(#{<<"a">> => 1},  decode(<<"{\"a\":1}">>)),
    ?_assertEqual({[{<<"a">>, 1}]}, decode(<<"{\"a\":1}">>, [object_as_tuple])),
    ?_assertEqual(null,             decode(<<"null">>, [{null_term, null}])),
    ?_assertEqual(my_null,          decode(<<"null">>, [{null_term, my_null}]))
  ].

roundtrip_test_() ->
  Vals = [null, true, false, 0, 1, -1, 1.5, <<"hello">>, [], [1, 2, 3],
          #{<<"a">> => 1, <<"b">> => [1, 2]},
          #{<<"nested">> => #{<<"x">> => true}}],
  [?_assertEqual(V, decode(encode(V))) || V <- Vals].

minify_test_() ->
  [
    ?_assertEqual({ok, <<"[1,2,3]">>},   minify(<<"[ 1, 2, 3 ]">>)),
    ?_assertEqual({ok, <<"{\"a\":1}">>}, minify(<<" { \"a\" : 1 } ">>))
  ].

prettify_test_() ->
  [
    ?_assertMatch({ok, <<"[\n", _/binary>>},  prettify(<<"[1,2,3]">>)),
    ?_assertMatch({ok, <<"{\n", _/binary>>},  prettify(<<"{\"a\":1}">>))
  ].

keys_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1},  decode(<<"{\"a\":1}">>)),
    ?_assertEqual(#{<<"a">> => 1},  decode(<<"{\"a\":1}">>, [{keys, binary}])),
    ?_assertEqual(#{a => 1},        decode(<<"{\"a\":1}">>, [{keys, atom}])),
    ?_assertEqual(#{a => 1},        decode(<<"{\"a\":1}">>, [{keys, existing_atom}])),
    %% existing_atom falls back to a binary for keys with no matching atom
    ?_assertEqual(#{<<"no_such_atom_in_glazejson_test_suite_xyz">> => 1},
                  decode(<<"{\"no_such_atom_in_glazejson_test_suite_xyz\":1}">>,
                         [{keys, existing_atom}]))
  ].

uescape_test_() ->
  [
    %% U+00E9 (é), UTF-8: 0xC3 0xA9
    ?_assertEqual(<<"\"\\u00e9\"">>, encode(<<16#C3, 16#A9>>, [uescape])),
    %% Without uescape, UTF-8 bytes pass through unescaped
    ?_assertEqual(<<"\"", 16#C3, 16#A9, "\"">>, encode(<<16#C3, 16#A9>>)),
    %% U+1F600 (emoji, outside the BMP) encodes as a surrogate pair
    ?_assertEqual(<<"\"\\ud83d\\ude00\"">>, encode(<<16#F0,16#9F,16#98,16#80>>, [uescape])),
    %% Round-trips back to the original UTF-8 binary
    ?_assertEqual(<<16#C3, 16#A9>>, decode(encode(<<16#C3, 16#A9>>, [uescape])))
  ].

force_utf8_test_() ->
  [
    %% Invalid byte sequences are sanitized to U+FFFD (UTF-8: EF BF BD)
    ?_assertEqual(<<"\"", 16#EF, 16#BF, 16#BD, 16#EF, 16#BF, 16#BD, "a\"">>,
                  encode(<<16#FF, 16#FE, $a>>, [force_utf8])),
    %% Without force_utf8, invalid bytes pass through verbatim
    ?_assertEqual(<<"\"", 16#FF, 16#FE, "a\"">>, encode(<<16#FF, 16#FE, $a>>)),
    %% Valid UTF-8 is left untouched
    ?_assertEqual(<<"\"", 16#C3, 16#A9, "\"">>, encode(<<16#C3, 16#A9>>, [force_utf8]))
  ].

pretty_test_() ->
  [
    ?_assertEqual(<<"{\n   \"a\": 1\n}">>, encode(#{<<"a">> => 1}, [pretty])),
    ?_assertEqual(<<"[\n   1,\n   2\n]">>, encode([1, 2], [pretty])),
    ?_assertEqual(#{<<"a">> => 1}, decode(encode(#{<<"a">> => 1}, [pretty])))
  ].

null_term_encode_test_() ->
  [
    ?_assertEqual(<<"null">>, encode(null)),
    ?_assertEqual(<<"null">>, encode(nil, [{null_term, nil}])),
    ?_assertEqual(<<"null">>, encode(undefined, [{null_term, undefined}])),
    ?_assertEqual(<<"null">>, encode(null, [{null_term, undefined}]))
  ].

object_as_tuple_test_() ->
  [
    ?_assertEqual({[]}, decode(<<"{}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}, {<<"b">>, 2}]},
                  decode(<<"{\"a\":1,\"b\":2}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, {[{<<"b">>, true}]}}]},
                  decode(<<"{\"a\":{\"b\":true}}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}]},
                  decode(encode({[{<<"a">>, 1}]}), [object_as_tuple]))
  ].

numbers_test_() ->
  [
    ?_assertEqual(0,        decode(<<"0">>)),
    ?_assertEqual(-1,       decode(<<"-1">>)),
    ?_assertEqual(-1.5,     decode(<<"-1.5">>)),
    ?_assertEqual(1.0e10,   decode(<<"1.0e10">>)),
    ?_assertEqual(1.0e-10,  decode(<<"1.0e-10">>)),
    ?_assertEqual(<<"-1">>, encode(-1)),
    ?_assertEqual(<<"0">>,  encode(0))
  ].

iolist_input_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1}, decode([<<"{\"a\":">>, <<"1}">>])),
    ?_assertEqual(#{<<"a">> => 1}, decode([<<"{">>, [<<"\"a\":1">>], <<"}">>])),
    ?_assertEqual({ok, <<"{\"a\":1}">>}, minify([<<"{ \"a\"">>, <<": 1 }">>]))
  ].

decode_error_test_() ->
  [
    ?_assertError(_, decode(<<"">>)),
    ?_assertError(_, decode(<<"{\"a\":}">>)),
    ?_assertError(_, decode(<<"{\"a\":1">>)),
    ?_assertError(_, decode(<<"[1, 2">>)),
    ?_assertError(_, decode(<<"not json">>))
  ].

bigint_test_() ->
  Big =  123456789012345678901234567890,
  Neg = -Big,
  [
    ?_assertEqual({ok,  <<"123456789012345678901234567890">>}, encode_bigint(Big)),
    ?_assertEqual({ok, <<"-123456789012345678901234567890">>}, encode_bigint(Neg)),
    ?_assertEqual({ok, Big},                                   decode_bigint(<<"123456789012345678901234567890">>)),
    ?_assertEqual({ok, Neg},                                   decode_bigint(<<"-123456789012345678901234567890">>)),
    ?_assertEqual({ok, 123},                                   decode_bigint(<<"123">>)),
    ?_assertEqual(Big,                                         decode(<<"123456789012345678901234567890">>)),
    ?_assertEqual(<<"123456789012345678901234567890">>,        encode(Big)),
    ?_assertEqual(Big,                                         decode(encode(Big)))
  ].

-endif.
