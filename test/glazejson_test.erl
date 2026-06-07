-module(glazejson_test).
-include_lib("eunit/include/eunit.hrl").

%% Basic scalar roundtrips
scalars_test_() ->
  Tests = [
    {null,    <<"null">>},
    {true,    <<"true">>},
    {false,   <<"false">>},
    {0,       <<"0">>},
    {42,      <<"42">>},
    {-7,      <<"-7">>},
    {3.14,    <<"3.14">>},
    {<<"">>,  <<"\"\"">>},
    {<<"hi">>,<<"\"hi\"">>}
  ],
  [?_assertEqual(V, glazejson:decode(glazejson:encode(V))) || {V, _} <- Tests]
  ++
  [?_assertEqual(V, glazejson:decode(J)) || {V, J} <- Tests].

array_test_() ->
  [
    ?_assertEqual([],             glazejson:decode(<<"[]">>)),
    ?_assertEqual([1, 2, 3],      glazejson:decode(<<"[1,2,3]">>)),
    ?_assertEqual([[1], [2, 3]],  glazejson:decode(<<"[[1],[2,3]]">>)),
    ?_assertEqual(<<"[1,2,3]">>,  glazejson:encode([1, 2, 3]))
  ].

map_test_() ->
  [
    ?_assertEqual(#{},                    glazejson:decode(<<"{}">>)),
    ?_assertEqual(#{<<"a">> => 1},        glazejson:decode(<<"{\"a\":1}">>)),
    ?_assertEqual(#{<<"a">> => <<"b">>},  glazejson:decode(<<"{\"a\":\"b\"}">>)),
    ?_assertEqual(#{<<"a">> => true},     glazejson:decode(<<"{\"a\":true}">>)),
    ?_assertEqual(#{<<"a">> => null},     glazejson:decode(<<"{\"a\":null}">>))
  ].

object_as_tuple_test_() ->
  [
    ?_assertEqual({[]},               glazejson:decode(<<"{}">>,        [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}]},   glazejson:decode(<<"{\"a\":1}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"x">>, true}, {<<"y">>, false}]},
                  glazejson:decode(<<"{\"x\":true,\"y\":false}">>, [object_as_tuple]))
  ].

null_atom_test_() ->
  [
    ?_assertEqual(nil,       glazejson:decode(<<"null">>, [use_nil])),
    ?_assertEqual(my_nil,    glazejson:decode(<<"null">>, [{null_term, my_nil}])),
    ?_assertEqual(<<"null">>,glazejson:encode(nil,  [use_nil])),
    ?_assertEqual(<<"null">>,glazejson:encode(null, []))
  ].

encode_map_keys_test_() ->
  [
    %% atom keys
    ?_assertMatch(<<"{", _/binary>>, glazejson:encode(#{a => 1})),
    %% binary keys
    ?_assertMatch(<<"{", _/binary>>, glazejson:encode(#{<<"a">> => 1}))
  ].

pretty_test_() ->
  Compact = <<"[1,2,3]">>,
  {ok, Pretty} = glazejson:prettify(Compact),
  [
    ?_assertMatch(<<"[\n", _/binary>>, Pretty),
    ?_assertEqual({ok, Compact}, glazejson:minify(Pretty))
  ].

nested_test_() ->
  JSON = <<"{\"a\":{\"b\":[1,null,true]}}">>,
  Expected = #{<<"a">> => #{<<"b">> => [1, null, true]}},
  [
    ?_assertEqual(Expected,   glazejson:decode(JSON)),
    ?_assertEqual(Expected,   glazejson:decode(glazejson:encode(Expected)))
  ].

large_integer_test_() ->
  %% Integers beyond 2^53 preserved as uint64/int64
  Big = 18446744073709551615,
  [
    ?_assertEqual(Big, glazejson:decode(integer_to_binary(Big)))
  ].
