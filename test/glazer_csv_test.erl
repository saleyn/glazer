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
%% {fields, Types} option — per-column type conversion
%% ----------------------------------------------------------------------------

decode_fields_test_() ->
  [
    %% integer
    ?_assertEqual([[1, <<"b">>]],
                  glazer:csv_decode(<<"1,b">>, [{fields, [integer]}])),
    %% negative integer and bignum
    ?_assertEqual([[-5], [123456789012345678901234567890]],
                  glazer:csv_decode(<<"-5\n123456789012345678901234567890">>,
                                     [{fields, [integer]}])),
    %% {float, Precision} rounds to the given number of decimal digits
    ?_assertEqual([[3.14]],
                  glazer:csv_decode(<<"3.14159">>, [{fields, [{float, 2}]}])),
    ?_assertEqual([[3.0]],
                  glazer:csv_decode(<<"3.14159">>, [{fields, [{float, 0}]}])),
    %% boolean, case-insensitive
    ?_assertEqual([[true], [false], [true]],
                  glazer:csv_decode(<<"true\nFALSE\nTrue">>, [{fields, [boolean]}])),
    %% datetime -> Unix epoch seconds (UTC)
    ?_assertEqual([[1705314600]],
                  glazer:csv_decode(<<"2024-01-15T10:30:00Z">>,
                                     [{fields, [{datetime, <<"%Y-%m-%dT%H:%M:%SZ">>}]}])),
    %% datetime with a numeric UTC offset
    ?_assertEqual([[1705296600]],
                  glazer:csv_decode(<<"2024-01-15T10:30:00+05:00">>,
                                     [{fields, [{datetime, <<"%Y-%m-%dT%H:%M:%S%z">>}]}])),
    %% datetime without a time component
    ?_assertEqual([[1705276800]],
                  glazer:csv_decode(<<"2024-01-15">>, [{fields, [{datetime, <<"%Y-%m-%d">>}]}])),
    %% binary (default / explicit no-op)
    ?_assertEqual([[<<"abc">>]], glazer:csv_decode(<<"abc">>, [{fields, [binary]}])),
    %% charlist
    ?_assertEqual([["abc"]], glazer:csv_decode(<<"abc">>, [{fields, [charlist]}])),
    %% fewer types than columns: extra columns stay binaries
    ?_assertEqual([[1, <<"2">>, <<"3">>]],
                  glazer:csv_decode(<<"1,2,3">>, [{fields, [integer]}])),
    %% applies positionally with headers, independent of the header names
    ?_assertEqual([#{<<"name">> => <<"Alice">>, <<"age">> => 30, <<"active">> => true}],
                  glazer:csv_decode(<<"name,age,active\nAlice,30,true\n">>,
                                     [headers, {fields, [binary, integer, boolean]}]))
  ].

decode_fields_error_test_() ->
  [
    %% non-numeric field with `integer` type: default on_failure is `binary`
    ?_assertEqual([[<<"not_a_number">>]],
                  glazer:csv_decode(<<"not_a_number">>, [{fields, [integer]}])),
    %% invalid boolean: left as binary
    ?_assertEqual([[<<"maybe">>]],
                  glazer:csv_decode(<<"maybe">>, [{fields, [boolean]}])),
    %% datetime that doesn't match the format: left as binary
    ?_assertEqual([[<<"not-a-date">>]],
                  glazer:csv_decode(<<"not-a-date">>, [{fields, [{datetime, <<"%Y-%m-%d">>}]}])),
    %% on_failure => raise: errors with row/col (1-based) of the failing field
    ?_assertEqual({error, {invalid_field_value, 1, 1}},
                  glazer:csv_try_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => raise}]}])),
    ?_assertEqual({error, {invalid_field_value, 1, 1}},
                  glazer:csv_try_decode(<<"maybe">>,
                    [{fields, [#{type => boolean, on_failure => raise}]}])),
    ?_assertEqual({error, {invalid_field_value, 1, 1}},
                  glazer:csv_try_decode(<<"not-a-date">>,
                    [{fields, [#{type => {datetime, <<"%Y-%m-%d">>},
                                  on_failure => raise}]}])),
    %% error column/row are 1-based and point at the failing field
    ?_assertEqual({error, {invalid_field_value, 2, 2}},
                  glazer:csv_try_decode(<<"1,2\n3,bad">>,
                    [{fields, [#{type => integer, on_failure => raise},
                                #{type => integer, on_failure => raise}]}])),
    %% raises when using csv_decode/2
    ?_assertError({invalid_field_value, 1, 1},
                  glazer:csv_decode(<<"x">>, [{fields, [#{type => integer, on_failure => raise}]}])),

    %% on_failure => default: use the spec's default value
    ?_assertEqual([[-1]],
                  glazer:csv_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, default => -1, on_failure => default}]}])),
    %% on_failure => default with no default given falls back to binary
    ?_assertEqual([[<<"not_a_number">>]],
                  glazer:csv_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => default}]}])),
    %% on_failure => null: use the configured null term
    ?_assertEqual([[null]],
                  glazer:csv_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => null}]}])),
    %% on_failure => null with {null_term, Atom}: use the custom null term
    ?_assertEqual([[nil]],
                  glazer:csv_decode(<<"not_a_number">>,
                    [{null_term, nil},
                     {fields, [#{type => integer, on_failure => null}]}])),
    %% on_failure => null for boolean
    ?_assertEqual([[null]],
                  glazer:csv_decode(<<"maybe">>,
                    [{fields, [#{type => boolean, on_failure => null}]}])),
    %% on_failure => null for {float, Precision}
    ?_assertEqual([[null]],
                  glazer:csv_decode(<<"not_a_float">>,
                    [{fields, [#{type => {float, 2}, on_failure => null}]}])),
    %% on_failure => null for datetime
    ?_assertEqual([[null]],
                  glazer:csv_decode(<<"not-a-date">>,
                    [{fields, [#{type => {datetime, <<"%Y-%m-%d">>}, on_failure => null}]}])),
    %% on_failure => null leaves valid fields converted, only the failing
    %% field becomes null
    ?_assertEqual([[1, null]],
                  glazer:csv_decode(<<"1,bad">>,
                    [{fields, [integer, #{type => integer, on_failure => null}]}])),
    %% on_failure => null with headers: the failing column becomes null
    ?_assertEqual([#{<<"a">> => 1, <<"b">> => null}],
                  glazer:csv_decode(<<"a,b\n1,bad\n">>,
                    [headers, {fields, [integer, #{type => integer, on_failure => null}]}])),
    %% on_failure => null with {null_term, Atom} and headers
    ?_assertEqual([#{<<"a">> => 1, <<"b">> => nil}],
                  glazer:csv_decode(<<"a,b\n1,bad\n">>,
                    [headers, {null_term, nil},
                     {fields, [integer, #{type => integer, on_failure => null}]}])),
    %% on_failure => null never fails csv_try_decode/2 (no error)
    ?_assertEqual({ok, [[null]]},
                  glazer:csv_try_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => null}]}])),

    %% empty field uses `default` regardless of on_failure
    ?_assertEqual([[0, <<"b">>]],
                  glazer:csv_decode(<<",b">>, [{fields, [#{type => integer, default => 0}]}])),
    %% empty field with default => null and {null_term, Atom}: uses the
    %% literal atom given as `default`, not the configured null_term
    ?_assertEqual([[null, <<"b">>]],
                  glazer:csv_decode(<<",b">>,
                    [{null_term, nil},
                     {fields, [#{type => integer, default => null}]}])),
    %% empty field with no `default` given: left as an empty binary
    ?_assertEqual([[<<>>, <<"b">>]],
                  glazer:csv_decode(<<",b">>, [{fields, [integer]}])),

    %% existing_atom: converts to an existing atom
    ?_assertEqual([[ok]], glazer:csv_decode(<<"ok">>, [{fields, [existing_atom]}])),
    %% existing_atom: falls back to binary for unknown atoms
    ?_assertEqual([[<<"zzzz_glazer_csv_no_such_atom">>]],
                  glazer:csv_decode(<<"zzzz_glazer_csv_no_such_atom">>, [{fields, [existing_atom]}])),

    %% {atom, ExistingAtoms}: converts only if the text matches one of ExistingAtoms
    ?_assertEqual([[ok], [error]],
                  glazer:csv_decode(<<"ok\nerror">>, [{fields, [{atom, [ok, error]}]}])),
    %% {atom, ExistingAtoms}: falls back to binary if not in the whitelist
    ?_assertEqual([[<<"other">>]],
                  glazer:csv_decode(<<"other">>, [{fields, [{atom, [ok, error]}]}]))
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
