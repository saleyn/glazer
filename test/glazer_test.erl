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
