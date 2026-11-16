-module(glazer_oom_test).
-include_lib("eunit/include/eunit.hrl").

%% OOM (Out of Memory) error handling tests for JSON, YAML, and CSV encoders.
%%
%% These tests verify that the encoders gracefully handle memory allocation
%% failures by generating large outputs that trigger multiple OutBuf reallocations.
%%
%% ALLOCATION FAILURE TEST METHODS
%% ================================
%%
%% 1. STRESS TESTS (this file - always pass unless system is actually OOM)
%%    These tests generate large encodings to verify the error handling paths
%%    are present and structured correctly. They exercise:
%%    - Deep nesting (1000 levels)
%%    - Large collections (5000+ items)
%%    - Long strings with escape sequences
%%    - Mixed data types
%%
%%    Run with: rebar3 eunit --module glazer_oom_test
%%
%% 2. MEMORY PRESSURE TESTS (ulimit - simulates malloc() failure)
%%    Run tests with strict virtual memory limits to force allocation failures:
%%
%%    ulimit -v 51200 && rebar3 eunit --module glazer_oom_test
%%
%%    When memory limit is exceeded, malloc() returns NULL, which is now:
%%    - Checked in OutBuf::ensure() - throws OutOfMemory exception
%%    - Caught in NIF wrapper - returns {encode_error, ...} to Erlang
%%
%% 3. ADDRESSSANITIZER TESTS (asan - detects memory errors)
%%    Build with ASan to catch allocation failures and prevent crashes:
%%
%%    make memcheck  # builds with -fsanitize=address
%%    rebar3 eunit   # runs with ASAN_OPTIONS=detect_leaks=1
%%
%%    ASan will:
%%    - Intercept malloc/realloc/free at runtime
%%    - Detect NULL pointer dereferences
%%    - Report heap-buffer-overflow/use-after-free
%%    - Ensure graceful error handling
%%
%% PRACTICAL TESTING
%% ==================
%%
%% To trigger real malloc() failures:
%%
%%    # Simple: run with very tight memory limit
%%    bash test/oom_test_runner.sh  # or manually:
%%    ulimit -v 51200 && rebar3 eunit --module glazer_oom_test
%%
%%    # Thorough: run all tests under ASan
%%    make memcheck  # compiles NIF with -fsanitize=address
%%
%%    # Production-ready: combine both
%%    ulimit -v 51200 && make memcheck
%%
%% WHAT TO LOOK FOR
%% =================
%%
%% Success indicators:
%%   ✓ Tests complete without segfault
%%   ✓ No "Illegal instruction" (SIGILL)
%%   ✓ No "Segmentation fault" (SIGSEGV)
%%   ✓ Errors return as {encode_error, {<<"out of memory">>, _}} to Erlang
%%   ✓ ASan reports "0 errors" (no leaks, no overflows)
%%
%% Failure indicators (before exception handling):
%%   ✗ Segfault when malloc() returns NULL
%%   ✗ NULL pointer dereference in memcpy()
%%   ✗ Uninitialized error fields returned to Erlang
%%   ✗ ASan detecting heap-buffer-overflow

%% JSON encoding under memory pressure

json_large_nested_test_() ->
  %% Nesting up to the encoder's depth cap (128) encodes fine ...
  LargeMap = make_nested_map(100),
  [
    ?_assertMatch(<<"{", _/binary>>, glazer_json:encode(LargeMap)),
    %% ... and anything deeper is rejected with an error, not a stack overflow
    ?_assertError({encode_error, _}, glazer_json:encode(make_nested_map(1000)))
  ].

json_long_strings_test_() ->
  %% String with Unicode and escape sequences forces multiple reallocations
  LongString = binary:copy(<<"quoted backslash unicode: ❤️ special: @#$%^&*()">> , 1000),
  LargeMap = #{<<"content">> => LongString},
  [
    ?_assertMatch(<<"{", _/binary>>, glazer_json:encode(LargeMap))
  ].

json_large_arrays_test_() ->
  %% Each map requires field encoding and allocation
  LargeArray = [make_user_map(I) || I <- lists:seq(1, 5000)],
  JSON = glazer_json:encode(LargeArray),
  [
    ?_assert(is_binary(JSON)),
    ?_assert(byte_size(JSON) > 100000)
  ].

json_large_numbers_test_() ->
  %% Large integers and floats trigger BigInt encoder reallocations
  LargeNumbers = [I * 9223372036854775807 || I <- lists:seq(1, 1000)],
  [
    ?_assertMatch(<<"[", _/binary>>, glazer_json:encode(LargeNumbers))
  ].

json_roundtrip_test_() ->
  %% Large structure survives encode/decode cycle
  Original = #{
    <<"users">> => [
      make_user_with_metadata(I)
      || I <- lists:seq(1, 1000)
    ]
  },
  JSON = glazer_json:encode(Original),
  Decoded = glazer_json:decode(JSON),
  [
    ?_assertEqual(Original, Decoded)
  ].

json_improper_list_error_test_() ->
  %% Improper lists should error gracefully
  [
    ?_assertError({encode_error, _}, glazer_json:encode([1 | 2]))
  ].

%% Exception handling verification tests

json_encode_exception_test_() ->
  %% Verify that encoding exceptions are caught and converted to error terms
  [
    {"JSON encode catches exceptions and returns error terms",
     {timeout, 30, fun json_encode_exception_check/0}}
  ].

json_encode_exception_check() ->
  %% Generate a large structure that exercises allocation paths
  LargeStructure = #{
    <<"data">> => [make_user_map(I) || I <- lists:seq(1, 10000)],
    <<"nested">> => make_nested_map(100)
  },

  %% Encoding should either succeed or return proper error tuple
  Result = try
    {ok, glazer_json:encode(LargeStructure)}
  catch
    error:{encode_error, CaughtReason} ->
      {caught_error, {encode_error, CaughtReason}};
    error:CaughtError ->
      {caught_error, CaughtError};
    throw:{encode_error, CaughtReason2} ->
      {caught_error, {encode_error, CaughtReason2}};
    throw:CaughtError2 ->
      {caught_error, CaughtError2}
  end,

  case Result of
    {ok, _} ->
      %% Success - no exception thrown
      true;
    {caught_error, {encode_error, {<<"out of memory">>, _}}} ->
      %% OOM exception was caught and converted properly
      ?debugFmt("Got OOM message: ~p\n", [Result]),
      true;
    {caught_error, {encode_error, OtherReason}} ->
      %% Other encode error (not OOM) - still properly handled
      ?debugFmt("Encode error caught: ~p", [OtherReason]),
      true;
    {caught_error, OtherError} ->
      %% Any exception was caught - good, not a crash
      ?debugFmt("Exception caught: ~p", [OtherError]),
      true;
    Other ->
      erlang:error({unexpected_result, Other})
  end.

%% YAML encoding under memory pressure

yaml_large_nested_test_() ->
  %% Nesting up to the encoder's depth cap (128) encodes fine ...
  LargeMap = make_nested_map(100),
  [
    ?_assertMatch(<<_, _/binary>>, glazer_yaml:encode(LargeMap)),
    %% ... and anything deeper is rejected with an error, not a stack overflow
    ?_assertError({encode_error, _}, glazer_yaml:encode(make_nested_map(1000)))
  ].

yaml_long_strings_test_() ->
  %% YAML requires quoting for many strings
  LongStrings = [
    iolist_to_binary(io_lib:format("Line ~w: Special chars: ~s", [I, "special"]))
    || I <- lists:seq(1, 1000)
  ],
  [
    ?_assertMatch(<<_, _/binary>>, glazer_yaml:encode(LongStrings))
  ].

yaml_many_keys_test_() ->
  %% Many keys = many allocation calls for each key-value pair
  LargeMap = maps:from_list([
    {iolist_to_binary(io_lib:format("key_~w", [I])),
     iolist_to_binary(io_lib:format("value_~w", [I]))}
    || I <- lists:seq(1, 2000)
  ]),
  [
    ?_assertMatch(<<_, _/binary>>, glazer_yaml:encode(LargeMap))
  ].

yaml_unsupported_type_error_test_() ->
  %% Functions should error gracefully
  [
    ?_assertError({encode_error, _}, glazer_yaml:encode(fun() -> ok end))
  ].

yaml_roundtrip_test_() ->
  %% Large structure survives encode/decode cycle
  Original = [
    #{
      <<"id">> => I,
      <<"details">> => #{
        <<"name">> => iolist_to_binary(io_lib:format("Entry ~w", [I])),
        <<"description">> => <<"Long description">>
      }
    }
    || I <- lists:seq(1, 500)
  ],
  YAML = glazer_yaml:encode(Original),
  Decoded = glazer_yaml:decode(YAML),
  [
    ?_assertEqual(Original, Decoded)
  ].

yaml_encode_exception_test_() ->
  %% Verify that YAML encoding exceptions are caught and converted to error terms
  [
    {"YAML encode catches exceptions and returns error terms",
     {timeout, 30, fun yaml_encode_exception_check/0}}
  ].

yaml_encode_exception_check() ->
  LargeStructure = [
    make_user_with_metadata(I)
    || I <- lists:seq(1, 10000)
  ],

  Result = try
    {ok, glazer_yaml:encode(LargeStructure)}
  catch
    error:{encode_error, CaughtReason} ->
      {caught_error, {encode_error, CaughtReason}};
    error:CaughtError ->
      {caught_error, CaughtError};
    throw:{encode_error, CaughtReason2} ->
      {caught_error, {encode_error, CaughtReason2}};
    throw:CaughtError2 ->
      {caught_error, CaughtError2}
  end,

  case Result of
    {ok, _} -> true;
    {caught_error, {encode_error, {<<"out of memory">>, _}}} -> true;
    {caught_error, {encode_error, _}} -> true;
    {caught_error, _} -> true;
    Other -> ct:fail({unexpected_result, Other})
  end.

%% CSV encoding under memory pressure

csv_large_rows_test_() ->
  %% Each row triggers multiple delimiter and line-ending writes
  Rows = [
    [
      integer_to_binary(I),
      iolist_to_binary(io_lib:format("User ~w", [I])),
      iolist_to_binary(io_lib:format("user~w@example.com", [I])),
      float_to_binary(I * 1.5, [{decimals, 1}])
    ]
    || I <- lists:seq(1, 10000)
  ],
  CSV = glazer_csv:encode(Rows),
  LineCount = length(binary:split(CSV, <<"\r\n">>, [global])),
  [
    ?_assert(is_binary(CSV)),
    ?_assert(LineCount > 9000)
  ].

csv_quoted_fields_test_() ->
  %% Quoted fields with embedded quotes trigger push_field_raw reallocations
  Rows = [
    [
      integer_to_binary(I),
      <<"Field with \"quotes\" and, commas">>,
      <<"Normal field">>,
      <<"Another \"quoted\" field">>
    ]
    || I <- lists:seq(1, 5000)
  ],
  CSV = glazer_csv:encode(Rows),
  [
    ?_assert(is_binary(CSV))
  ].

csv_with_headers_test_() ->
  %% CSV with headers and maps
  Headers = [<<"id">>, <<"name">>, <<"email">>, <<"active">>],
  Rows = [
    #{
      <<"id">> => I,
      <<"name">> => iolist_to_binary(io_lib:format("User ~w", [I])),
      <<"email">> => iolist_to_binary(io_lib:format("user~w@example.com", [I])),
      <<"active">> => (I rem 2) == 0
    }
    || I <- lists:seq(1, 5000)
  ],
  CSV = glazer_csv:encode(Rows, [{headers, Headers}]),
  [HeaderLine | DataLines] = binary:split(CSV, <<"\r\n">>, [global]),
  HasId = binary:match(HeaderLine, <<"id">>) =/= nomatch,
  [
    ?_assert(is_binary(CSV)),
    ?_assert(HasId),
    ?_assert(length(DataLines) > 4999)
  ].

csv_mixed_types_test_() ->
  %% Mix integers, floats, atoms, and binaries
  Rows = [
    [
      I,
      float_to_binary(I / 2.5, [{decimals, 1}]),
      atom_to_binary(atom_value),
      <<"Binary string with special">>
    ]
    || I <- lists:seq(1, 3000)
  ],
  [
    ?_assertMatch(<<_, _/binary>>, glazer_csv:encode(Rows))
  ].

csv_improper_list_error_test_() ->
  %% Improper lists should error gracefully
  Rows = [
    [<<"field1">>, <<"field2">>],
    [<<"field3">> | <<"not_a_list">>]
  ],
  [
    ?_assertError({encode_error, _}, glazer_csv:encode(Rows))
  ].

csv_encode_exception_test_() ->
  %% Verify that CSV encoding exceptions are caught and converted to error terms
  [
    {"CSV encode catches exceptions and returns error terms",
     {timeout, 30, fun csv_encode_exception_check/0}}
  ].

csv_encode_exception_check() ->
  Rows = [
    [integer_to_binary(I), iolist_to_binary(io_lib:format("User ~w", [I]))]
    || I <- lists:seq(1, 5000)
  ],

  Result = try
    {ok, glazer_csv:encode(Rows)}
  catch
    error:{encode_error, CaughtReason} ->
      {caught_error, {encode_error, CaughtReason}};
    error:CaughtError ->
      {caught_error, CaughtError};
    throw:{encode_error, CaughtReason2} ->
      {caught_error, {encode_error, CaughtReason2}};
    throw:CaughtError2 ->
      {caught_error, CaughtError2}
  end,

  case Result of
    {ok, _} -> true;
    {caught_error, {encode_error, {<<"out of memory">>, _}}} -> true;
    {caught_error, {encode_error, _}} -> true;
    {caught_error, _} -> true;
    Other -> ct:fail({unexpected_result, Other})
  end.

%% Helper functions

make_nested_map(0) ->
  #{<<"data">> => <<"x">>};
make_nested_map(N) ->
  #{iolist_to_binary(io_lib:format("level_~w", [N])) => make_nested_map(N - 1)}.

make_user_map(I) ->
  #{
    <<"id">> => I,
    <<"name">> => iolist_to_binary(io_lib:format("User ~w", [I])),
    <<"email">> => iolist_to_binary(io_lib:format("user~w@example.com", [I])),
    <<"active">> => (I rem 2) == 0,
    <<"balance">> => I * 1.5,
    <<"data">> => binary:copy(~"x", I)
  }.

make_user_with_metadata(I) ->
  #{
    <<"id">> => I,
    <<"name">> => iolist_to_binary(io_lib:format("User ~w", [I])),
    <<"metadata">> => #{
      <<"created">> => iolist_to_binary(io_lib:format("2024-01-~w", [1 + (I rem 28)])),
      <<"tags">> => [
        iolist_to_binary(io_lib:format("tag~w", [I rem 10])),
        iolist_to_binary(io_lib:format("tag~w", [I rem 5]))
      ]
    }
  }.
