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

%% ----------------------------------------------------------------------------
%% scan/1,2 — value-boundary scanning
%% ----------------------------------------------------------------------------

scan_complete_test_() ->
  [
    ?_assertEqual({complete, 7},  glazejson:scan(<<"{\"a\":1}">>)),
    ?_assertEqual({complete, 7},  glazejson:scan(<<"{\"a\":1} {\"b\":2}">>)),
    ?_assertEqual({complete, 2},  glazejson:scan(<<"[]">>)),
    ?_assertEqual({complete, 13}, glazejson:scan(<<"[1,2,[3,4],5]rest">>))
  ].

scan_incomplete_test_() ->
  [
    ?_assertMatch({incomplete, _}, glazejson:scan(<<"{\"a\":">>)),
    ?_assertMatch({incomplete, _}, glazejson:scan(<<"[1,2">>)),
    ?_assertMatch({incomplete, _}, glazejson:scan(<<"123">>)),
    ?_assertMatch({incomplete, _}, glazejson:scan(<<"\"unterminated">>))
  ].

%% scan/2 resumes from a previously-returned state; the caller passes the
%% *whole* buffer (previously-seen bytes plus newly-arrived ones).
scan_resume_test_() ->
  [
    ?_test(begin
      Part1 = <<"{\"a\":">>,
      Part2 = <<"1}">>,
      {incomplete, S1} = glazejson:scan(Part1),
      ?assertEqual({complete, 7}, glazejson:scan(<<Part1/binary, Part2/binary>>, S1))
    end),

    %% an escape sequence straddling the chunk boundary is tracked correctly
    ?_test(begin
      Chunk1 = <<"{\"k\":\"ab\\">>,
      Chunk2 = <<"\"cd\"}">>,
      {incomplete, S2} = glazejson:scan(Chunk1),
      Whole = <<Chunk1/binary, Chunk2/binary>>,
      ?assertEqual({complete, byte_size(Whole)}, glazejson:scan(Whole, S2))
    end),

    %% a nested-structure split mid-value resumes correctly
    ?_test(begin
      Part1 = <<"{\"a\":{\"b\":">>,
      Part2 = <<"[1,2]}}">>,
      {incomplete, S3} = glazejson:scan(Part1),
      Whole = <<Part1/binary, Part2/binary>>,
      ?assertEqual({complete, byte_size(Whole)}, glazejson:scan(Whole, S3))
    end)
  ].

%% ----------------------------------------------------------------------------
%% stream_decoder/0,1, stream_feed/2, stream_eof/1 — incremental decoding
%% ----------------------------------------------------------------------------

stream_feed_basic_test_() ->
  [
    %% a value split across feed/2 calls is decoded once complete
    ?_test(begin
      D0 = glazejson:stream_decoder(),
      {[], D1} = glazejson:stream_feed(D0, <<"{\"a\":">>),
      {Vals, _D2} = glazejson:stream_feed(D1, <<"1}">>),
      ?assertEqual([#{<<"a">> => 1}], Vals)
    end),

    %% multiple whitespace-separated values delivered in one chunk
    ?_test(begin
      D0 = glazejson:stream_decoder(),
      {Vals, _D1} = glazejson:stream_feed(D0, <<"{\"a\":1} {\"b\":2} {\"c\":3}">>),
      ?assertEqual([#{<<"a">> => 1}, #{<<"b">> => 2}, #{<<"c">> => 3}], Vals)
    end),

    %% values straddling a chunk boundary are emitted on the chunk that
    %% completes them, not before
    ?_test(begin
      D0 = glazejson:stream_decoder(),
      {V1, D1} = glazejson:stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
      {V2, _D2} = glazejson:stream_feed(D1, <<"2}">>),
      ?assertEqual([#{<<"a">> => 1}], V1),
      ?assertEqual([#{<<"b">> => 2}], V2)
    end)
  ].

stream_feed_ndjson_test_() ->
  ?_test(begin
    Doc = <<"{\"x\":1}\n{\"y\":[1,2,3]}\n{\"z\":\"hi\"}\n">>,
    %% feed one byte at a time to exercise the worst-case chunking
    {Vals, DLast} = lists:foldl(
      fun(Byte, {Acc, D}) ->
        {V, D2} = glazejson:stream_feed(D, <<Byte>>),
        {Acc ++ V, D2}
      end, {[], glazejson:stream_decoder()}, binary_to_list(Doc)),
    ?assertEqual({ok, []}, glazejson:stream_eof(DLast)),
    ?assertEqual([#{<<"x">> => 1},
                  #{<<"y">> => [1, 2, 3]},
                  #{<<"z">> => <<"hi">>}], Vals)
  end).

stream_eof_test_() ->
  [
    %% a trailing bare scalar is ambiguous mid-stream and only resolved at EOF
    ?_test(begin
      D0 = glazejson:stream_decoder(),
      {[], D1} = glazejson:stream_feed(D0, <<"   42">>),
      ?assertEqual({ok, [42]}, glazejson:stream_eof(D1))
    end),

    %% trailing whitespace at EOF yields no extra value
    ?_test(begin
      D0 = glazejson:stream_decoder(),
      {[#{<<"a">> := 1}], D1} = glazejson:stream_feed(D0, <<"{\"a\":1}\n">>),
      ?assertEqual({ok, []}, glazejson:stream_eof(D1))
    end),

    %% an incomplete trailing value surfaces as an error
    ?_test(begin
      D0 = glazejson:stream_decoder(),
      {[], D1} = glazejson:stream_feed(D0, <<"{\"a\":">>),
      ?assertMatch({error, _}, glazejson:stream_eof(D1))
    end)
  ].

stream_decoder_opts_test_() ->
  ?_test(begin
    D0 = glazejson:stream_decoder([{keys, atom}]),
    {[#{a := 1}], _D1} = glazejson:stream_feed(D0, <<"{\"a\":1}">>)
  end).

%% Random chunk boundaries shouldn't change the decoded result.
stream_random_split_test_() ->
  {timeout, 30, ?_test(begin
    Lines = [glazejson:encode(#{<<"i">> => I, <<"v">> => lists:seq(1, I rem 7)})
             || I <- lists:seq(1, 100)],
    Doc = iolist_to_binary([[L, <<"\n">>] || L <- Lines]),
    Expected = [glazejson:decode(L) || L <- Lines],

    lists:foreach(fun(_) ->
      Pieces = random_chunks(Doc),
      D0 = glazejson:stream_decoder(),
      {Vals, DLast} = lists:foldl(
        fun(Piece, {Acc, D}) ->
          {V, D2} = glazejson:stream_feed(D, Piece),
          {Acc ++ V, D2}
        end, {[], D0}, Pieces),
      {ok, Trailing} = glazejson:stream_eof(DLast),
      ?assertEqual(Expected, Vals ++ Trailing)
    end, lists:seq(1, 5))
  end)}.

random_chunks(Bin) ->
  Size = byte_size(Bin),
  N = rand:uniform(8),
  Points = lists:usort([rand:uniform(Size) || _ <- lists:seq(1, N)]),
  split_at(Bin, 0, Points, []).

split_at(Bin, _Off, [], Acc) ->
  lists:reverse([Bin | Acc]);
split_at(Bin, Off, [P | Rest], Acc) ->
  Len = P - Off,
  <<Piece:Len/binary, Tail/binary>> = Bin,
  split_at(Tail, P, Rest, [Piece | Acc]).
