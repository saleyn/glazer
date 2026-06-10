-module(glazer_test).
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
  [?_assertEqual(V, glazer:decode(glazer:encode(V))) || {V, _} <- Tests]
  ++
  [?_assertEqual(V, glazer:decode(J)) || {V, J} <- Tests].

array_test_() ->
  [
    ?_assertEqual([],             glazer:decode(<<"[]">>)),
    ?_assertEqual([1, 2, 3],      glazer:decode(<<"[1,2,3]">>)),
    ?_assertEqual([[1], [2, 3]],  glazer:decode(<<"[[1],[2,3]]">>)),
    ?_assertEqual(<<"[1,2,3]">>,  glazer:encode([1, 2, 3]))
  ].

map_test_() ->
  [
    ?_assertEqual(#{},                    glazer:decode(<<"{}">>)),
    ?_assertEqual(#{<<"a">> => 1},        glazer:decode(<<"{\"a\":1}">>)),
    ?_assertEqual(#{<<"a">> => <<"b">>},  glazer:decode(<<"{\"a\":\"b\"}">>)),
    ?_assertEqual(#{<<"a">> => true},     glazer:decode(<<"{\"a\":true}">>)),
    ?_assertEqual(#{<<"a">> => null},     glazer:decode(<<"{\"a\":null}">>))
  ].

object_as_tuple_test_() ->
  [
    ?_assertEqual({[]},               glazer:decode(<<"{}">>,        [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}]},   glazer:decode(<<"{\"a\":1}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"x">>, true}, {<<"y">>, false}]},
                  glazer:decode(<<"{\"x\":true,\"y\":false}">>, [object_as_tuple]))
  ].

%% ----------------------------------------------------------------------------
%% Duplicate object keys
%%
%% By default (decoding to maps) and with `{keys, atom}`/`{keys,
%% existing_atom}`, a JSON object cannot be represented with duplicate keys
%% as an Erlang map, so duplicates are always deduped, last value wins,
%% regardless of `dedupe_keys`. With `object_as_tuple`, duplicates are
%% preserved as-is in the resulting proplist, in source order, unless
%% `dedupe_keys` is given.
%% ----------------------------------------------------------------------------

duplicate_keys_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 2},             glazer:decode(<<"{\"a\":1,\"a\":2}">>)),
    ?_assertEqual(#{<<"a">> => 3, <<"b">> => 2}, glazer:decode(<<"{\"a\":1,\"b\":2,\"a\":3}">>)),
    ?_assertEqual(#{a => 2},                   glazer:decode(<<"{\"a\":1,\"a\":2}">>, [{keys, atom}])),
    ?_assertEqual(#{a => 2},                   glazer:decode(<<"{\"a\":1,\"a\":2}">>, [{keys, existing_atom}])),

    %% duplicates inside a nested object are deduped too
    ?_assertEqual(#{<<"a">> => #{<<"x">> => 2}}, glazer:decode(<<"{\"a\":{\"x\":1,\"x\":2}}">>)),

    %% duplicates inside an object within an array are deduped too
    ?_assertEqual([#{<<"a">> => 2}],           glazer:decode(<<"[{\"a\":1,\"a\":2}]">>)),

    %% object_as_tuple preserves duplicate keys without deduping by default
    ?_assertEqual({[{<<"a">>, 1}, {<<"a">>, 2}]},
                  glazer:decode(<<"{\"a\":1,\"a\":2}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}, {<<"b">>, 2}, {<<"a">>, 3}]},
                  glazer:decode(<<"{\"a\":1,\"b\":2,\"a\":3}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"foo">>, {[{<<"bar">>, 1}, {<<"bar">>, 2}]}}]},
                  glazer:decode(<<"{\"foo\":{\"bar\":1,\"bar\":2}}">>, [object_as_tuple]))
  ].

%% ----------------------------------------------------------------------------
%% dedupe_keys (object_as_tuple only)
%%
%% Eliminates duplicate object keys from the proplist, keeping the value
%% (and position) of the *last* occurrence. Has no effect when decoding to
%% maps (default) or with `{keys, atom | existing_atom}`, which always
%% dedupe (see above).
%% ----------------------------------------------------------------------------

dedupe_keys_test_() ->
  [
    %% Simple sanity check: no duplicates, nothing changes
    ?_assertEqual({[{<<"foo">>, 1}]},
                  glazer:decode(<<"{\"foo\":1}">>, [object_as_tuple, dedupe_keys])),

    %% Basic dedup: last value wins
    ?_assertEqual({[{<<"foo">>, 2}]},
                  glazer:decode(<<"{\"foo\":1,\"foo\":2}">>, [object_as_tuple, dedupe_keys])),

    %% Multiple repeats: last value wins
    ?_assertEqual({[{<<"foo">>, 3}]},
                  glazer:decode(<<"{\"foo\":1,\"foo\":2,\"foo\":3}">>, [object_as_tuple, dedupe_keys])),

    %% Sub-objects are deduped too
    ?_assertEqual({[{<<"foo">>, {[{<<"bar">>, 2}]}}]},
                  glazer:decode(<<"{\"foo\":{\"bar\":1,\"bar\":2}}">>, [object_as_tuple, dedupe_keys])),

    %% Objects nested in arrays are deduped
    ?_assertEqual([{[{<<"foo">>, 2}]}],
                  glazer:decode(<<"[{\"foo\":1,\"foo\":2}]">>, [object_as_tuple, dedupe_keys])),

    %% Key order stays the same other than deduped keys, with the last
    %% occurrence's value and position retained
    ?_assertEqual({[{<<"bar">>, 1}, {<<"baz">>, 3}, {<<"foo">>, 4}]},
                  glazer:decode(<<"{\"bar\":1,\"foo\":2,\"baz\":3,\"foo\":4}">>,
                                 [object_as_tuple, dedupe_keys]))
  ].

null_atom_test_() ->
  [
    ?_assertEqual(nil,       glazer:decode(<<"null">>, [use_nil])),
    ?_assertEqual(my_nil,    glazer:decode(<<"null">>, [{null_term, my_nil}])),
    ?_assertEqual(<<"null">>,glazer:encode(nil,  [use_nil])),
    ?_assertEqual(<<"null">>,glazer:encode(null, []))
  ].

encode_map_keys_test_() ->
  [
    %% atom keys
    ?_assertMatch(<<"{", _/binary>>, glazer:encode(#{a => 1})),
    %% binary keys
    ?_assertMatch(<<"{", _/binary>>, glazer:encode(#{<<"a">> => 1}))
  ].

pretty_test_() ->
  Compact = <<"[1,2,3]">>,
  Pretty = glazer:prettify(Compact),
  [
    ?_assertMatch(<<"[\n", _/binary>>, Pretty),
    ?_assertEqual(Compact, glazer:minify(Pretty))
  ].

nested_test_() ->
  JSON = <<"{\"a\":{\"b\":[1,null,true]}}">>,
  Expected = #{<<"a">> => #{<<"b">> => [1, null, true]}},
  [
    ?_assertEqual(Expected,   glazer:decode(JSON)),
    ?_assertEqual(Expected,   glazer:decode(glazer:encode(Expected)))
  ].

large_integer_test_() ->
  %% Integers beyond 2^53 preserved as uint64/int64
  Big = 18446744073709551615,
  [
    ?_assertEqual(Big, glazer:decode(integer_to_binary(Big)))
  ].

%% ----------------------------------------------------------------------------
%% scan/1,2 — value-boundary scanning
%% ----------------------------------------------------------------------------

scan_complete_test_() ->
  [
    ?_assertEqual({complete, 7},  glazer:scan(<<"{\"a\":1}">>)),
    ?_assertEqual({complete, 7},  glazer:scan(<<"{\"a\":1} {\"b\":2}">>)),
    ?_assertEqual({complete, 2},  glazer:scan(<<"[]">>)),
    ?_assertEqual({complete, 13}, glazer:scan(<<"[1,2,[3,4],5]rest">>))
  ].

scan_incomplete_test_() ->
  [
    ?_assertMatch({incomplete, _}, glazer:scan(<<"{\"a\":">>)),
    ?_assertMatch({incomplete, _}, glazer:scan(<<"[1,2">>)),
    ?_assertMatch({incomplete, _}, glazer:scan(<<"123">>)),
    ?_assertMatch({incomplete, _}, glazer:scan(<<"\"unterminated">>))
  ].

%% scan/2 resumes from a previously-returned state; the caller passes the
%% *whole* buffer (previously-seen bytes plus newly-arrived ones).
scan_resume_test_() ->
  [
    ?_test(begin
      Part1 = <<"{\"a\":">>,
      Part2 = <<"1}">>,
      {incomplete, S1} = glazer:scan(Part1),
      ?assertEqual({complete, 7}, glazer:scan(<<Part1/binary, Part2/binary>>, S1))
    end),

    %% an escape sequence straddling the chunk boundary is tracked correctly
    ?_test(begin
      Chunk1 = <<"{\"k\":\"ab\\">>,
      Chunk2 = <<"\"cd\"}">>,
      {incomplete, S2} = glazer:scan(Chunk1),
      Whole = <<Chunk1/binary, Chunk2/binary>>,
      ?assertEqual({complete, byte_size(Whole)}, glazer:scan(Whole, S2))
    end),

    %% a nested-structure split mid-value resumes correctly
    ?_test(begin
      Part1 = <<"{\"a\":{\"b\":">>,
      Part2 = <<"[1,2]}}">>,
      {incomplete, S3} = glazer:scan(Part1),
      Whole = <<Part1/binary, Part2/binary>>,
      ?assertEqual({complete, byte_size(Whole)}, glazer:scan(Whole, S3))
    end)
  ].

%% ----------------------------------------------------------------------------
%% stream_decoder/0,1, stream_feed/2, stream_eof/1 — incremental decoding
%% ----------------------------------------------------------------------------

stream_feed_basic_test_() ->
  [
    %% a value split across feed/2 calls is decoded once complete
    ?_test(begin
      D0 = glazer:stream_decoder(),
      {[], D1} = glazer:stream_feed(D0, <<"{\"a\":">>),
      {Vals, _D2} = glazer:stream_feed(D1, <<"1}">>),
      ?assertEqual([#{<<"a">> => 1}], Vals)
    end),

    %% multiple whitespace-separated values delivered in one chunk
    ?_test(begin
      D0 = glazer:stream_decoder(),
      {Vals, _D1} = glazer:stream_feed(D0, <<"{\"a\":1} {\"b\":2} {\"c\":3}">>),
      ?assertEqual([#{<<"a">> => 1}, #{<<"b">> => 2}, #{<<"c">> => 3}], Vals)
    end),

    %% values straddling a chunk boundary are emitted on the chunk that
    %% completes them, not before
    ?_test(begin
      D0 = glazer:stream_decoder(),
      {V1, D1} = glazer:stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
      {V2, _D2} = glazer:stream_feed(D1, <<"2}">>),
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
        {V, D2} = glazer:stream_feed(D, <<Byte>>),
        {Acc ++ V, D2}
      end, {[], glazer:stream_decoder()}, binary_to_list(Doc)),
    ?assertEqual({ok, []}, glazer:stream_eof(DLast)),
    ?assertEqual([#{<<"x">> => 1},
                  #{<<"y">> => [1, 2, 3]},
                  #{<<"z">> => <<"hi">>}], Vals)
  end).

stream_eof_test_() ->
  [
    %% a trailing bare scalar is ambiguous mid-stream and only resolved at EOF
    ?_test(begin
      D0 = glazer:stream_decoder(),
      {[], D1} = glazer:stream_feed(D0, <<"   42">>),
      ?assertEqual({ok, [42]}, glazer:stream_eof(D1))
    end),

    %% trailing whitespace at EOF yields no extra value
    ?_test(begin
      D0 = glazer:stream_decoder(),
      {[#{<<"a">> := 1}], D1} = glazer:stream_feed(D0, <<"{\"a\":1}\n">>),
      ?assertEqual({ok, []}, glazer:stream_eof(D1))
    end),

    %% an incomplete trailing value surfaces as an error
    ?_test(begin
      D0 = glazer:stream_decoder(),
      {[], D1} = glazer:stream_feed(D0, <<"{\"a\":">>),
      ?assertMatch({error, _}, glazer:stream_eof(D1))
    end)
  ].

stream_decoder_opts_test_() ->
  ?_test(begin
    D0 = glazer:stream_decoder([{keys, atom}]),
    {[#{a := 1}], _D1} = glazer:stream_feed(D0, <<"{\"a\":1}">>)
  end).

%% Random chunk boundaries shouldn't change the decoded result.
stream_random_split_test_() ->
  {timeout, 30, ?_test(begin
    Lines = [glazer:encode(#{<<"i">> => I, <<"v">> => lists:seq(1, I rem 7)})
             || I <- lists:seq(1, 100)],
    Doc = iolist_to_binary([[L, <<"\n">>] || L <- Lines]),
    Expected = [glazer:decode(L) || L <- Lines],

    lists:foreach(fun(_) ->
      Pieces = random_chunks(Doc),
      D0 = glazer:stream_decoder(),
      {Vals, DLast} = lists:foldl(
        fun(Piece, {Acc, D}) ->
          {V, D2} = glazer:stream_feed(D, Piece),
          {Acc ++ V, D2}
        end, {[], D0}, Pieces),
      {ok, Trailing} = glazer:stream_eof(DLast),
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

%% ----------------------------------------------------------------------------
%% RFC 8259 golden tests
%%
%% These document glazer's conformance to RFC 8259 ("The JavaScript Object
%% Notation (JSON) Data Interchange Format"), including the places where the
%% parser is intentionally lenient (accepting inputs the grammar forbids).
%% Lenient cases are marked as such; everything else reflects required
%% behaviour.
%% ----------------------------------------------------------------------------

%% RFC 8259 §3 - literal names "true", "false", "null" are case-sensitive
%% and must be lowercase.
rfc8259_literals_test_() ->
  [
    ?_assertEqual(true,  glazer:decode(<<"true">>)),
    ?_assertEqual(false, glazer:decode(<<"false">>)),
    ?_assertEqual(null,  glazer:decode(<<"null">>)),

    ?_assertError({parse_error, _}, glazer:decode(<<"True">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"TRUE">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"NULL">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"nul">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"tru">>))
  ].

%% RFC 8259 §6 - numbers.
%% number = [ "-" ] int [ frac ] [ exp ]
%% int    = "0" / (digit1-9 *DIGIT)
%% frac   = "." 1*DIGIT
%% exp    = ("e" / "E") ["-" / "+"] 1*DIGIT
rfc8259_numbers_test_() ->
  [
    %% well-formed integers and floats
    ?_assertEqual(0,     glazer:decode(<<"0">>)),
    ?_assertEqual(42,    glazer:decode(<<"42">>)),
    ?_assertEqual(-7,    glazer:decode(<<"-7">>)),
    ?_assertEqual(3.14,  glazer:decode(<<"3.14">>)),
    ?_assertEqual(1.0e10, glazer:decode(<<"1e10">>)),
    ?_assertEqual(1.0e-10, glazer:decode(<<"1e-10">>)),
    ?_assertEqual(1.5e3, glazer:decode(<<"1.5E+3">>)),

    %% RFC 8259 forbids a leading '+' on numbers
    ?_assertError({parse_error, _}, glazer:decode(<<"+1">>)),

    %% RFC 8259 requires at least one digit before '.', and requires a digit
    %% after '.'; a bare leading '.' is not a valid number
    ?_assertError({parse_error, _}, glazer:decode(<<".5">>)),

    %% --- Lenient deviations from strict RFC 8259 grammar ---

    %% RFC 8259's `int` production forbids leading zeros (e.g. "01"), but
    %% glazer accepts them.
    ?_assertEqual(1,   glazer:decode(<<"01">>)),
    ?_assertEqual(-1,  glazer:decode(<<"-01">>)),

    %% "-0" / "0" are valid per the grammar and decode to 0.
    ?_assertEqual(0,   glazer:decode(<<"-0">>)),

    %% RFC 8259's `frac` production requires at least one digit after '.';
    %% glazer accepts a trailing '.' with no digits.
    ?_assertEqual(1.0, glazer:decode(<<"1.">>)),

    %% RFC 8259's `exp` production requires at least one digit after
    %% 'e'/'E' (with optional sign); glazer accepts an exponent with no
    %% digits.
    ?_assertEqual(1.0, glazer:decode(<<"1e">>))
  ].

%% RFC 8259 §7 - strings.
%% Required escapes: \" \\ \/ \b \f \n \r \t \uXXXX. Unescaped control
%% characters (U+0000-U+001F) are technically forbidden in strings.
rfc8259_strings_test_() ->
  [
    ?_assertEqual(<<"hello">>,  glazer:decode(<<"\"hello\"">>)),
    ?_assertEqual(<<"">>,       glazer:decode(<<"\"\"">>)),

    %% standard escapes
    ?_assertEqual(<<"\"">>,     glazer:decode(<<"\"\\\"\"">>)),
    ?_assertEqual(<<"\\">>,     glazer:decode(<<"\"\\\\\"">>)),
    ?_assertEqual(<<"/">>,      glazer:decode(<<"\"\\/\"">>)),
    ?_assertEqual(<<"\b">>,     glazer:decode(<<"\"\\b\"">>)),
    ?_assertEqual(<<"\f">>,     glazer:decode(<<"\"\\f\"">>)),
    ?_assertEqual(<<"\n">>,     glazer:decode(<<"\"\\n\"">>)),
    ?_assertEqual(<<"\r">>,     glazer:decode(<<"\"\\r\"">>)),
    ?_assertEqual(<<"\t">>,     glazer:decode(<<"\"\\t\"">>)),

    %% \uXXXX escapes, including a surrogate pair combined into one
    %% UTF-8-encoded code point
    ?_assertEqual(<<"A">>,      glazer:decode(<<"\"\\u0041\"">>)),
    ?_assertEqual(<<240,159,152,128>>,
                  glazer:decode(<<"\"\\ud83d\\ude00\"">>)),

    %% an unterminated string is rejected
    ?_assertError({parse_error, _}, glazer:decode(<<"\"unterminated">>)),

    %% --- Lenient deviations from strict RFC 8259 grammar ---

    %% RFC 8259 forbids unescaped control characters (e.g. raw tab) inside
    %% strings; glazer passes them through unchanged.
    ?_assertEqual(<<"a\tb">>,   glazer:decode(<<"\"a\tb\"">>)),

    %% An unrecognised escape (e.g. "\x") is not part of the RFC 8259
    %% grammar; glazer passes the backslash-prefixed text through verbatim
    %% rather than rejecting it.
    ?_assertEqual(<<"x41">>,    glazer:decode(<<"\"\\x41\"">>)),

    %% A lone (unpaired) UTF-16 surrogate in a \u escape does not represent
    %% a valid Unicode scalar value; glazer encodes it as WTF-8/CESU-8 bytes
    %% rather than rejecting it.
    ?_assertEqual(<<237,160,128>>, glazer:decode(<<"\"\\ud800\"">>)),
    ?_assertEqual(<<237,176,128>>, glazer:decode(<<"\"\\udc00\"">>))
  ].

%% RFC 8259 §2, §4, §5 - whitespace, objects, and arrays.
%% ws = *( %x20 / %x09 / %x0A / %x0D )
%% Trailing/leading commas are not permitted by the object/array grammar.
rfc8259_structure_test_() ->
  [
    %% insignificant whitespace (space, tab, LF, CR) around structural
    %% characters and values is allowed
    ?_assertEqual(#{<<"a">> => 1},
                  glazer:decode(<<" \t\r\n{ \t\r\n\"a\"\t:\t1\t}\t\r\n">>)),
    ?_assertEqual([1, 2, 3], glazer:decode(<<"[ 1 , 2 , 3 ]">>)),

    %% trailing commas are rejected
    ?_assertError({parse_error, _}, glazer:decode(<<"[1,2,]">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"{\"a\":1,}">>)),

    %% leading commas are rejected
    ?_assertError({parse_error, _}, glazer:decode(<<"[,1]">>)),

    %% JSON5-style extensions are rejected: single-quoted strings,
    %% unquoted object keys, and comments are not part of RFC 8259
    ?_assertError({parse_error, _}, glazer:decode(<<"{'a':1}">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"{a:1}">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"{\"a\":1 // comment\n}">>))
  ].

%% RFC 8259 §2 - "JSON-text = ws value ws": any of the seven JSON value
%% types may appear at the top level (RFC 8259 relaxed RFC 4627, which
%% required the top level to be an object or array), but nothing other than
%% trailing whitespace may follow the value.
rfc8259_top_level_test_() ->
  [
    ?_assertEqual(<<"hi">>, glazer:decode(<<"\"hi\"">>)),
    ?_assertEqual(42,       glazer:decode(<<"42">>)),
    ?_assertEqual(true,     glazer:decode(<<"true">>)),
    ?_assertEqual(false,    glazer:decode(<<"false">>)),
    ?_assertEqual(null,     glazer:decode(<<"null">>)),
    ?_assertEqual([],       glazer:decode(<<"[]">>)),
    ?_assertEqual(#{},      glazer:decode(<<"{}">>)),

    %% empty input is not a valid JSON text
    ?_assertError({parse_error, _}, glazer:decode(<<"">>)),

    %% trailing whitespace after the value is allowed
    ?_assertEqual(1, glazer:decode(<<"1   ">>)),
    ?_assertEqual(1, glazer:decode(<<"  1  \t\r\n">>)),

    %% trailing non-whitespace data after a complete value is rejected
    ?_assertError({parse_error, _}, glazer:decode(<<"1 2">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"123abc">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"[1] [2]">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"{} {}">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"true false">>)),

    %% try_decode/1 reports the same trailing-garbage condition as an error
    %% tuple instead of raising
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode(<<"1 2">>))
  ].

%% ----------------------------------------------------------------------------
%% RFC 8259 §5 - unterminated arrays and objects.
%%
%% An array/object that ends (at EOF, or with a trailing comma followed by
%% EOF) without a matching `]`/`}` is incomplete and must be rejected, not
%% treated as if the closing bracket were implicitly present.
%% ----------------------------------------------------------------------------
rfc8259_unterminated_test_() ->
  [
    ?_assertError({parse_error, _}, glazer:decode(<<"[">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"{">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"[1,">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"{\"a\":1,">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"[\"a\",\n4\n,1,">>)),

    %% sanity: the corresponding complete forms still decode correctly
    ?_assertEqual([],               glazer:decode(<<"[]">>)),
    ?_assertEqual(#{},              glazer:decode(<<"{}">>)),
    ?_assertEqual([1],              glazer:decode(<<"[1]">>)),
    ?_assertEqual(#{<<"a">> => 1},  glazer:decode(<<"{\"a\":1}">>))
  ].

%% ----------------------------------------------------------------------------
%% Maximum nesting depth.
%%
%% The recursive-descent parser bounds array/object nesting depth so that
%% pathologically deep input (e.g. thousands of nested arrays) is rejected
%% with a parse_error instead of overflowing the C stack.
%% ----------------------------------------------------------------------------
max_depth_test_() ->
  [
    %% moderately nested input decodes fine
    ?_test(begin
      N = 100,
      Bin = iolist_to_binary([lists:duplicate(N, "["), "1", lists:duplicate(N, "]")]),
      {ok, _} = glazer:try_decode(Bin)
    end),

    %% pathologically deep input is rejected, not crashed on
    ?_test(begin
      N = 100000,
      Bin = iolist_to_binary([lists:duplicate(N, "["), "1", lists:duplicate(N, "]")]),
      ?assertMatch({error, {parse_error, _}}, glazer:try_decode(Bin))
    end),

    %% 100,000 "[" characters with no value and no closing brackets. Exercises
    %% the depth limit on unbalanced/incomplete input, not just balanced
    %% nesting.
    ?_test(begin
      Bin = binary:copy(<<"[">>, 100000),
      ?assertMatch({error, {parse_error, _}}, glazer:try_decode(Bin))
    end)
  ].

%% ----------------------------------------------------------------------------
%% JSONTestSuite (https://github.com/nst/JSONTestSuite) "i_" cases —
%% inputs the spec leaves implementation-defined (parsers may accept or
%% reject). These document glazer's actual choices; none is a conformance
%% requirement.
%% ----------------------------------------------------------------------------
jsontestsuite_implementation_defined_test_() ->
  [
    %% Arbitrary-precision integers beyond uint64/int64 range are accepted
    %% via bigint support, in either sign.
    ?_assertEqual([100000000000000000000],
                  glazer:decode(<<"[100000000000000000000]">>)),
    ?_assertEqual([-237462374673276894279832749832423479823246327846],
                  glazer:decode(<<"[-237462374673276894279832749832423479823246327846]">>)),

    %% Numbers with an exponent so large/small that the magnitude over- or
    %% underflows a double are rejected.
    ?_assertError({parse_error, _}, glazer:decode(<<"[1e999]">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"[-1e999]">>)),
    ?_assertError({parse_error, _}, glazer:decode(<<"[1e-999]">>)),

    %% A byte-order mark (BOM) is not stripped or otherwise recognised; JSON
    %% text must be UTF-8 without a BOM.
    ?_assertError({parse_error, _}, glazer:decode(<<239,187,191, "{}">>)),

    %% glazer does not validate that string contents are well-formed UTF-8:
    %% invalid byte sequences (overlong encodings, lone continuation bytes,
    %% truncated multi-byte sequences) are passed through unchanged as raw
    %% bytes in the resulting binary.
    ?_assertEqual([<<255>>],          glazer:decode(<<"[\"", 255, "\"]">>)),
    ?_assertEqual([<<129>>],          glazer:decode(<<"[\"", 129, "\"]">>)),

    %% 500 levels of array nesting is within MAX_DEPTH and decodes fine.
    ?_test(begin
      N = 500,
      Bin = iolist_to_binary([lists:duplicate(N, "["), "[]", lists:duplicate(N, "]")]),
      ?assertMatch({ok, _}, glazer:try_decode(Bin))
    end)
  ].
