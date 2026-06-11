-module(glazer_csv_test).
-include_lib("eunit/include/eunit.hrl").

%% ----------------------------------------------------------------------------
%% csv_decode/1,2 — basic rows
%% ----------------------------------------------------------------------------

decode_basic_test_() ->
  [
    ?_assertEqual([], glazer:csv_decode(<<>>)),
    ?_assertEqual([[<<"a">>, <<"b">>, <<"c">>]],
                  glazer:csv_decode(<<"a,b,c">>)),
    ?_assertEqual([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]],
                  glazer:csv_decode(<<"a,b\n1,2\n">>)),
    %% CRLF line endings
    ?_assertEqual([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]],
                  glazer:csv_decode(<<"a,b\r\n1,2\r\n">>)),
    %% trailing newline optional
    ?_assertEqual([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]],
                  glazer:csv_decode(<<"a,b\n1,2">>)),
    %% empty fields
    ?_assertEqual([[<<"a">>, <<>>, <<"c">>]],
                  glazer:csv_decode(<<"a,,c">>)),
    %% blank lines are skipped
    ?_assertEqual([[<<"a">>], [<<"b">>]],
                  glazer:csv_decode(<<"a\n\nb\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Quoted fields
%% ----------------------------------------------------------------------------

decode_quoting_test_() ->
  [
    ?_assertEqual([[<<"hello, world">>, <<"b">>]],
                  glazer:csv_decode(<<"\"hello, world\",b">>)),
    %% embedded double quote, doubled
    ?_assertEqual([[<<"a \"quoted\" word">>]],
                  glazer:csv_decode(<<"\"a \"\"quoted\"\" word\"">>)),
    %% embedded newline inside quotes
    ?_assertEqual([[<<"line1\nline2">>, <<"b">>]],
                  glazer:csv_decode(<<"\"line1\nline2\",b">>)),
    %% embedded CRLF inside quotes
    ?_assertEqual([[<<"line1\r\nline2">>, <<"b">>]],
                  glazer:csv_decode(<<"\"line1\r\nline2\",b">>)),
    %% empty quoted field
    ?_assertEqual([[<<>>, <<"b">>]],
                  glazer:csv_decode(<<"\"\",b">>)),
    %% quoted field is the entire (only) field on the line
    ?_assertEqual([[<<"a,b">>]], glazer:csv_decode(<<"\"a,b\"">>)),
    %% quoted field containing only doubled quotes
    ?_assertEqual([[<<"\"\"">>]], glazer:csv_decode(<<"\"\"\"\"\"\"">>)),
    %% mixture of quoted and unquoted fields on one row
    ?_assertEqual([[<<"a">>, <<"b,c">>, <<"d">>]],
                  glazer:csv_decode(<<"a,\"b,c\",d">>))
  ].

%% ----------------------------------------------------------------------------
%% Custom delimiter
%% ----------------------------------------------------------------------------

decode_delimiter_test_() ->
  [
    ?_assertEqual([[<<"a">>, <<"b">>, <<"c">>]],
                  glazer:csv_decode(<<"a;b;c">>, [{delimiter, $;}])),
    ?_assertEqual([[<<"a">>, <<"b">>]],
                  glazer:csv_decode(<<"a\tb">>, [{delimiter, $\t}])),
    %% quoting still applies with a custom delimiter
    ?_assertEqual([[<<"a;b">>, <<"c">>]],
                  glazer:csv_decode(<<"\"a;b\";c">>, [{delimiter, $;}])),
    %% a row consisting solely of delimiters yields empty fields
    ?_assertEqual([[<<>>, <<>>, <<>>]], glazer:csv_decode(<<";;">>, [{delimiter, $;}]))
  ].

%% ----------------------------------------------------------------------------
%% headers option
%% ----------------------------------------------------------------------------

decode_headers_test_() ->
  [
    ?_assertEqual([#{<<"a">> => <<"1">>, <<"b">> => <<"2">>}],
                  glazer:csv_decode(<<"a,b\n1,2\n">>, [headers])),
    ?_assertEqual([#{<<"a">> => <<"1">>, <<"b">> => <<"2">>},
                   #{<<"a">> => <<"3">>, <<"b">> => <<"4">>}],
                  glazer:csv_decode(<<"a,b\n1,2\n3,4\n">>, [headers])),
    %% no data rows
    ?_assertEqual([], glazer:csv_decode(<<"a,b\n">>, [headers])),
    %% headers as atoms
    ?_assertEqual([#{a => <<"1">>, b => <<"2">>}],
                  glazer:csv_decode(<<"a,b\n1,2\n">>, [headers, {keys, atom}])),
    %% headers as existing_atom: known atoms become atom keys
    ?_assertEqual([#{a => <<"1">>, b => <<"2">>}],
                  glazer:csv_decode(<<"a,b\n1,2\n">>, [headers, {keys, existing_atom}])),
    %% existing_atom falls back to binary for atoms that don't already exist
    ?_assertEqual([#{<<"zzzz_glazer_csv_no_such_atom">> => <<"1">>, b => <<"2">>}],
                  glazer:csv_decode(<<"zzzz_glazer_csv_no_such_atom,b\n1,2\n">>,
                                    [headers, {keys, existing_atom}])),
    %% short row: missing trailing fields are simply absent from the map
    ?_assertEqual([#{<<"a">> => <<"1">>}],
                  glazer:csv_decode(<<"a,b\n1\n">>, [headers]))
  ].

%% ----------------------------------------------------------------------------
%% csv_try_decode/1,2 — {ok, Rows} | {error, Reason}
%% ----------------------------------------------------------------------------

try_decode_test_() ->
  [
    ?_assertEqual({ok, [[<<"a">>, <<"b">>]]}, glazer:csv_try_decode(<<"a,b">>)),
    ?_assertEqual({error, unterminated_quoted_field},
                  glazer:csv_try_decode(<<"\"unterminated">>)),
    %% unterminated quote with no closing quote anywhere, multi-line input
    ?_assertEqual({error, unterminated_quoted_field},
                  glazer:csv_try_decode(<<"a,b\n\"c,d\n">>))
  ].

decode_error_test_() ->
  [
    ?_assertError(unterminated_quoted_field, glazer:csv_decode(<<"\"unterminated">>))
  ].

%% ----------------------------------------------------------------------------
%% csv_encode/1,2 — basic rows
%% ----------------------------------------------------------------------------

encode_basic_test_() ->
  [
    ?_assertEqual(<<>>, glazer:csv_encode([])),
    ?_assertEqual(<<"a,b,c\r\n">>, glazer:csv_encode([[<<"a">>, <<"b">>, <<"c">>]])),
    ?_assertEqual(<<"a,b\r\n1,2\r\n">>,
                  glazer:csv_encode([[<<"a">>, <<"b">>], [1, 2]])),
    %% lf line ending
    ?_assertEqual(<<"a,b\n1,2\n">>,
                  glazer:csv_encode([[<<"a">>, <<"b">>], [1, 2]], [{line_ending, lf}])),
    %% atoms and floats
    ?_assertEqual(<<"foo,1.5\r\n">>, glazer:csv_encode([[foo, 1.5]])),
    %% empty row produces just a line terminator
    ?_assertEqual(<<"\r\n">>, glazer:csv_encode([[]])),
    %% large integers (bignum) are encoded as decimal text
    ?_assertEqual(<<"123456789012345678901234567890\r\n">>,
                  glazer:csv_encode([[123456789012345678901234567890]])),
    %% negative integers
    ?_assertEqual(<<"-1,2\r\n">>, glazer:csv_encode([[-1, 2]]))
  ].

%% ----------------------------------------------------------------------------
%% Encoding errors for unsupported field types
%% ----------------------------------------------------------------------------

encode_error_test_() ->
  [
    %% a tuple field is not encodable
    ?_assertError({encode_error, _}, glazer:csv_encode([[{a, b}]])),
    %% a nested list field is not encodable
    ?_assertError({encode_error, _}, glazer:csv_encode([[[<<"a">>]]])),
    %% a map field is not encodable
    ?_assertError({encode_error, _}, glazer:csv_encode([[#{}]])),
    %% headers option requires the rows to be maps
    ?_assertError({encode_error, _}, glazer:csv_encode([[<<"a">>, <<"b">>]], [headers])),
    %% top-level term must be a list
    ?_assertError({encode_error, _}, glazer:csv_encode(not_a_list))
  ].

%% ----------------------------------------------------------------------------
%% Quoting on encode
%% ----------------------------------------------------------------------------

encode_quoting_test_() ->
  [
    ?_assertEqual(<<"\"hello, world\",b\r\n">>,
                  glazer:csv_encode([[<<"hello, world">>, <<"b">>]])),
    ?_assertEqual(<<"\"a \"\"quoted\"\" word\"\r\n">>,
                  glazer:csv_encode([[<<"a \"quoted\" word">>]])),
    ?_assertEqual(<<"\"line1\nline2\"\r\n">>,
                  glazer:csv_encode([[<<"line1\nline2">>]]))
  ].

%% ----------------------------------------------------------------------------
%% Custom delimiter on encode
%% ----------------------------------------------------------------------------

encode_delimiter_test_() ->
  [
    ?_assertEqual(<<"a;b\r\n">>, glazer:csv_encode([[<<"a">>, <<"b">>]], [{delimiter, $;}]))
  ].

%% ----------------------------------------------------------------------------
%% headers option on encode
%% ----------------------------------------------------------------------------

encode_headers_test_() ->
  [
    ?_assertEqual(<<"a,b\r\n1,2\r\n">>,
                  glazer:csv_encode([#{<<"a">> => 1, <<"b">> => 2}], [headers])),
    %% missing key produces empty field
    ?_assertEqual(<<"a,b\r\n1,2\r\n1,\r\n">>,
                  glazer:csv_encode([#{<<"a">> => 1, <<"b">> => 2}, #{<<"a">> => 1}], [headers]))
  ].

%% ----------------------------------------------------------------------------
%% Round trip
%% ----------------------------------------------------------------------------

round_trip_test_() ->
  [
    ?_assertEqual([[<<"a">>, <<"b,c">>], [<<"1">>, <<"2">>]],
                  glazer:csv_decode(glazer:csv_encode([[<<"a">>, <<"b,c">>], [<<"1">>, <<"2">>]]))),
    %% round trip with embedded quotes and newlines
    ?_assertEqual([[<<"a \"quoted\"\nvalue">>, <<"b">>]],
                  glazer:csv_decode(glazer:csv_encode([[<<"a \"quoted\"\nvalue">>, <<"b">>]]))),
    %% round trip with headers and a custom delimiter
    ?_assertEqual([#{<<"a">> => <<"1">>, <<"b">> => <<"x;y">>}],
                  glazer:csv_decode(
                    glazer:csv_encode([#{<<"a">> => <<"1">>, <<"b">> => <<"x;y">>}],
                                       [headers, {delimiter, $;}]),
                    [headers, {delimiter, $;}])),
    %% round trip with lf line endings
    ?_assertEqual([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]],
                  glazer:csv_decode(
                    glazer:csv_encode([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]], [{line_ending, lf}])))
  ].

%% ----------------------------------------------------------------------------
%% Large input — exercises the dirty-scheduler path (DIRTY_THRESHOLD bytes)
%% ----------------------------------------------------------------------------

large_input_test_() ->
  Rows  = [[integer_to_binary(I), integer_to_binary(I * 2)] || I <- lists:seq(1, 1000)],
  Csv   = glazer:csv_encode(Rows),
  [
    %% input is large enough to cross the dirty-scheduler threshold (8192 bytes)
    ?_assert(byte_size(Csv) >= 8192),
    ?_assertEqual(Rows, glazer:csv_decode(Csv)),
    ?_assertEqual(1000, length(glazer:csv_decode(Csv)))
  ].
