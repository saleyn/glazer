-module(glazer_test).
-include_lib("eunit/include/eunit.hrl").

%% ----------------------------------------------------------------------------
%% encode_integer/1, decode_integer/1, try_decode_integer/1
%% ----------------------------------------------------------------------------

bigint_test_() ->
  Big =  123456789012345678901234567890,
  Neg = -Big,
  [
    ?_assertEqual(<<"123456789012345678901234567890">>,  glazer:encode_integer(Big)),
    ?_assertEqual(<<"-123456789012345678901234567890">>, glazer:encode_integer(Neg)),
    ?_assertError(badarg,                                glazer:encode_integer(<<"not an integer">>)),
    ?_assertEqual({ok, Big},                             glazer:try_decode_integer(<<"123456789012345678901234567890">>)),
    ?_assertEqual({ok, Neg},                             glazer:try_decode_integer(<<"-123456789012345678901234567890">>)),
    ?_assertEqual({ok, 123},                             glazer:try_decode_integer(<<"123">>)),
    ?_assertEqual(Big,                                   glazer:decode_integer(<<"123456789012345678901234567890">>)),
    ?_assertEqual(123,                                   glazer:decode_integer(<<"123">>)),
    ?_assertError(invalid_number_format,                 glazer:decode_integer(<<"not a number">>)),
    ?_assertEqual({error, invalid_number_format},        glazer:try_decode_integer(<<"not a number">>)),
    ?_assertEqual(Big,                                   glazer_json:decode(<<"123456789012345678901234567890">>)),
    ?_assertEqual(<<"123456789012345678901234567890">>,  glazer_json:encode(Big)),
    ?_assertEqual(Big,                                   glazer_json:decode(glazer_json:encode(Big)))
  ].

%% ----------------------------------------------------------------------------
%% compile_path/1, find/2
%% ----------------------------------------------------------------------------

compile_path_test_() ->
  [
    ?_assertEqual([],                                    glazer:compile_path(<<".">>)),
    ?_assertEqual([{field, <<"a">>}],                    glazer:compile_path(<<".a">>)),
    ?_assertEqual([{field, <<"a">>}, {field, <<"b">>}],  glazer:compile_path(<<".a.b">>)),
    ?_assertEqual([{field, <<"a">>}, iterate],           glazer:compile_path(<<".a[]">>)),
    ?_assertEqual([{field, <<"a">>}, iterate, {field, <<"b">>}],
                                                          glazer:compile_path(<<".a[].b">>)),
    ?_assertEqual([{field, <<"a">>}, {index, 0}],        glazer:compile_path(<<".a[0]">>)),
    ?_assertEqual([{field, <<"a">>}, {index, -1}],       glazer:compile_path(<<".a[-1]">>)),
    ?_assertEqual([{field, <<"foo bar">>}],              glazer:compile_path(<<".[\"foo bar\"]">>)),
    ?_assertError({invalid_path, _},                     glazer:compile_path(<<"a">>)),
    ?_assertError({invalid_path, _},                     glazer:compile_path(<<".a[">>))
  ].

find_test_() ->
  Doc = #{
    <<"a">> => #{<<"b">> => 1},
    <<"c">> => [#{<<"d">> => 1}, #{<<"d">> => 2}, #{<<"e">> => 3}],
    <<"f">> => [1, 2, 3]
  },
  [
    ?_assertEqual([Doc],     glazer:find(Doc, <<".">>)),
    ?_assertEqual([1],       glazer:find(Doc, <<".a.b">>)),
    ?_assertEqual([],        glazer:find(Doc, <<".a.missing">>)),
    ?_assertEqual([],        glazer:find(Doc, <<".missing">>)),
    ?_assertEqual([1, 2],    glazer:find(Doc, <<".c[].d">>)),
    ?_assertEqual([1, 2, 3], glazer:find(Doc, <<".f[]">>)),
    ?_assertEqual([],        glazer:find(Doc, <<".f[].x">>)),
    ?_assertEqual([1],       glazer:find(Doc, <<".a[]">>)),
    ?_assertEqual([],        glazer:find(#{<<"a">> => 1}, <<".a.b">>)),
    ?_assertEqual([1],       glazer:find(Doc, <<".c[0].d">>)),
    ?_assertEqual([3],       glazer:find(Doc, <<".c[-1].e">>)),
    ?_assertEqual([],        glazer:find(Doc, <<".c[10].d">>)),

    %% pre-compiled path
    ?_assertEqual([1, 2],    glazer:find(Doc, glazer:compile_path(<<".c[].d">>))),

    %% atom keys also work
    ?_assertEqual([1],       glazer:find(#{a => #{b => 1}}, <<".a.b">>)),

    %% .[] iterates map values too
    ?_assertEqual(lists:sort(maps:values(Doc)),
                   lists:sort(glazer:find(Doc, <<".[]">>)))
  ].
