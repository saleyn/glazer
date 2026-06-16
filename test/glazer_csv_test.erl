-module(glazer_csv_test).
-include_lib("eunit/include/eunit.hrl").

%% ----------------------------------------------------------------------------
%% csv_decode/1,2 — basic rows
%% ----------------------------------------------------------------------------

decode_basic_test_() ->
  [
    ?_assertEqual(#{headers => nil, data => []},
                  glazer_csv:decode(<<>>)),
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>, <<"c">>]]},
                  glazer_csv:decode(<<"a,b,c">>)),
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>)),
    %% CRLF line endings
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\r\n1,2\r\n">>)),
    %% trailing newline optional
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2">>)),
    %% empty fields
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<>>, <<"c">>]]},
                  glazer_csv:decode(<<"a,,c">>)),
    %% blank lines are skipped
    ?_assertEqual(#{headers => nil, data => [[<<"a">>], [<<"b">>]]},
                  glazer_csv:decode(<<"a\n\nb\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Quoted fields
%% ----------------------------------------------------------------------------

decode_quoting_test_() ->
  [
    ?_assertEqual(#{headers => nil, data => [[<<"hello, world">>, <<"b">>]]},
                  glazer_csv:decode(<<"\"hello, world\",b">>)),
    %% embedded double quote, doubled
    ?_assertEqual(#{headers => nil, data => [[<<"a \"quoted\" word">>]]},
                  glazer_csv:decode(<<"\"a \"\"quoted\"\" word\"">>)),
    %% embedded newline inside quotes
    ?_assertEqual(#{headers => nil, data => [[<<"line1\nline2">>, <<"b">>]]},
                  glazer_csv:decode(<<"\"line1\nline2\",b">>)),
    %% embedded CRLF inside quotes
    ?_assertEqual(#{headers => nil, data => [[<<"line1\r\nline2">>, <<"b">>]]},
                  glazer_csv:decode(<<"\"line1\r\nline2\",b">>)),
    %% empty quoted field
    ?_assertEqual(#{headers => nil, data => [[<<>>, <<"b">>]]},
                  glazer_csv:decode(<<"\"\",b">>)),
    %% quoted field is the entire (only) field on the line
    ?_assertEqual(#{headers => nil, data => [[<<"a,b">>]]},
                  glazer_csv:decode(<<"\"a,b\"">>)),
    %% quoted field containing only doubled quotes
    ?_assertEqual(#{headers => nil, data => [[<<"\"\"">>]]},
                  glazer_csv:decode(<<"\"\"\"\"\"\"">>)),
    %% mixture of quoted and unquoted fields on one row
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b,c">>, <<"d">>]]},
                  glazer_csv:decode(<<"a,\"b,c\",d">>))
  ].

%% ----------------------------------------------------------------------------
%% Custom delimiter
%% ----------------------------------------------------------------------------

decode_delimiter_test_() ->
  [
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>, <<"c">>]]},
                  glazer_csv:decode(<<"a;b;c">>, [{delimiter, $;}])),
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>]]},
                  glazer_csv:decode(<<"a\tb">>, [{delimiter, $\t}])),
    %% quoting still applies with a custom delimiter
    ?_assertEqual(#{headers => nil, data => [[<<"a;b">>, <<"c">>]]},
                  glazer_csv:decode(<<"\"a;b\";c">>, [{delimiter, $;}])),
    %% a row consisting solely of delimiters yields empty fields
    ?_assertEqual(#{headers => nil, data => [[<<>>, <<>>, <<>>]]},
                  glazer_csv:decode(<<";;">>, [{delimiter, $;}]))
  ].

%% ----------------------------------------------------------------------------
%% headers option — first row is extracted into `headers`, data rows stay lists
%% ----------------------------------------------------------------------------

decode_headers_test_() ->
  [
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [headers])),
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[<<"1">>, <<"2">>], [<<"3">>, <<"4">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n3,4\n">>, [headers])),
    %% no data rows
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => []},
                  glazer_csv:decode(<<"a,b\n">>, [headers])),
    %% headers as atoms
    ?_assertEqual(#{headers => [a, b], data => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, atom}])),
    %% headers as existing_atom: known atoms become atom keys
    ?_assertEqual(#{headers => [a, b], data => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, existing_atom}])),
    %% existing_atom falls back to binary for atoms that don't already exist
    ?_assertEqual(#{headers => [<<"zzzz_glazer_csv_no_such_atom">>, b],
                    data    => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"zzzz_glazer_csv_no_such_atom,b\n1,2\n">>,
                                    [{headers, existing_atom}])),
    %% short row: missing trailing fields produce a shorter list
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => [[<<"1">>]]},
                  glazer_csv:decode(<<"a,b\n1\n">>, [headers]))
  ].

%% ----------------------------------------------------------------------------
%% {headers, Type} — header format control
%% ----------------------------------------------------------------------------

decode_headers_type_test_() ->
  [
    %% {headers, binary} is the same as bare `headers`
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, binary}])),
    %% {headers, string} is an alias for binary
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, string}])),
    %% {headers, atom}: column names converted to atoms (created if needed)
    ?_assertEqual(#{headers => [a, b], data => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, atom}])),
    %% {headers, existing_atom}: known atoms → atoms, unknown → binary
    ?_assertEqual(#{headers => [a, b], data => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, existing_atom}])),
    ?_assertEqual(#{headers => [<<"zzzz_no_such_atom">>, b],
                    data    => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"zzzz_no_such_atom,b\n1,2\n">>,
                                    [{headers, existing_atom}])),
    %% {headers, charlist}: column names as lists of Unicode codepoints
    ?_assertEqual(#{headers => ["ab", "cd"], data => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"ab,cd\n1,2\n">>, [{headers, charlist}]))
  ].

%% ----------------------------------------------------------------------------
%% {headers, [List]} — explicit column names (no header row in data)
%% ----------------------------------------------------------------------------

decode_explicit_headers_test_() ->
  [
    %% explicit binary headers: all rows are data, none consumed as header
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[<<"1">>, <<"2">>], [<<"3">>, <<"4">>]]},
                  glazer_csv:decode(<<"1,2\n3,4\n">>,
                                    [{headers, [<<"a">>, <<"b">>]}])),
    %% explicit atom headers
    ?_assertEqual(#{headers => [name, age],
                    data    => [[<<"Alice">>, <<"30">>]]},
                  glazer_csv:decode(<<"Alice,30\n">>,
                                    [{headers, [name, age]}])),
    %% {headers, [List]} + {return, map}: maps keyed by the provided names
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [#{<<"a">> => <<"1">>, <<"b">> => <<"2">>},
                                #{<<"a">> => <<"3">>, <<"b">> => <<"4">>}]},
                  glazer_csv:decode(<<"1,2\n3,4\n">>,
                                    [{headers, [<<"a">>, <<"b">>]}, {return, map}])),
    %% {headers, [atom, ...]} + {return, map}: atom-keyed maps
    ?_assertEqual(#{headers => [name, age],
                    data    => [#{name => <<"Alice">>, age => <<"30">>}]},
                  glazer_csv:decode(<<"Alice,30\n">>,
                                    [{headers, [name, age]}, {return, map}])),
    %% {headers, [List]} + {fields, ...}: field conversion still applied positionally
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[1, <<"2">>]]},
                  glazer_csv:decode(<<"1,2\n">>,
                                    [{headers, [<<"a">>, <<"b">>]},
                                     {fields, [integer]}])),
    %% empty explicit header list
    ?_assertEqual(#{headers => [], data => [[<<"a">>, <<"b">>]]},
                  glazer_csv:decode(<<"a,b\n">>, [{headers, []}])),
    %% streaming: {headers, [List]} pre-populates the header; data rows are field lists
    ?_test(begin
      D0 = glazer_csv:stream_decoder([{headers, [<<"x">>, <<"y">>]}]),
      {Rows, D1} = glazer_csv:stream_feed(D0, <<"1,2\n3,4\n">>),
      ?assertEqual([[<<"1">>, <<"2">>], [<<"3">>, <<"4">>]], Rows),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(D1))
    end),
    %% streaming: {headers, [List]} + {return, map}
    ?_test(begin
      D0 = glazer_csv:stream_decoder([{headers, [<<"x">>, <<"y">>]}, {return, map}]),
      {Rows, _D1} = glazer_csv:stream_feed(D0, <<"1,2\n">>),
      ?assertEqual([#{<<"x">> => <<"1">>, <<"y">> => <<"2">>}], Rows)
    end)
  ].

%% ----------------------------------------------------------------------------
%% {skip, N} and {skip, {From, To}} — row skipping
%% ----------------------------------------------------------------------------

decode_skip_test_() ->
  %% 6-row CSV, no header
  Csv = <<"r1\nr2\nr3\nr4\nr5\nr6\n">>,
  [
    %% skip 0 = no-op
    ?_assertEqual(#{headers => nil,
                    data => [[<<"r1">>],[<<"r2">>],[<<"r3">>],
                             [<<"r4">>],[<<"r5">>],[<<"r6">>]]},
                  glazer_csv:decode(Csv, [{skip, 0}])),
    %% skip first 2 rows
    ?_assertEqual(#{headers => nil,
                    data => [[<<"r3">>],[<<"r4">>],[<<"r5">>],[<<"r6">>]]},
                  glazer_csv:decode(Csv, [{skip, 2}])),
    %% skip more rows than present → empty data
    ?_assertEqual(#{headers => nil, data => []},
                  glazer_csv:decode(Csv, [{skip, 10}])),
    %% {skip, {3, 5}} → rows 3-5 (1-based): r3, r4, r5
    ?_assertEqual(#{headers => nil,
                    data => [[<<"r3">>],[<<"r4">>],[<<"r5">>]]},
                  glazer_csv:decode(Csv, [{skip, {3, 5}}])),
    %% {skip, {1, 3}} → rows 1-3: r1, r2, r3
    ?_assertEqual(#{headers => nil,
                    data => [[<<"r1">>],[<<"r2">>],[<<"r3">>]]},
                  glazer_csv:decode(Csv, [{skip, {1, 3}}])),
    %% {skip, {N, N}} → single row
    ?_assertEqual(#{headers => nil, data => [[<<"r4">>]]},
                  glazer_csv:decode(Csv, [{skip, {4, 4}}])),
    %% skip interacts with headers: data rows after the header row are skipped
    ?_assertEqual(#{headers => [<<"h">>], data => [[<<"r3">>],[<<"r4">>]]},
                  glazer_csv:decode(<<"h\nr1\nr2\nr3\nr4\n">>,
                                    [headers, {skip, 2}]))
  ].

%% ----------------------------------------------------------------------------
%% {limit, N} — cap the number of returned rows
%% ----------------------------------------------------------------------------

decode_limit_test_() ->
  Csv = <<"r1\nr2\nr3\nr4\nr5\n">>,
  [
    ?_assertEqual(#{headers => nil, data => [[<<"r1">>],[<<"r2">>]]},
                  glazer_csv:decode(Csv, [{limit, 2}])),
    %% limit > rows → returns all
    ?_assertEqual(#{headers => nil,
                    data => [[<<"r1">>],[<<"r2">>],[<<"r3">>],
                             [<<"r4">>],[<<"r5">>]]},
                  glazer_csv:decode(Csv, [{limit, 100}])),
    %% limit 0 = no limit
    ?_assertEqual(#{headers => nil,
                    data => [[<<"r1">>],[<<"r2">>],[<<"r3">>],
                             [<<"r4">>],[<<"r5">>]]},
                  glazer_csv:decode(Csv, [{limit, 0}])),
    %% limit with headers: limit applies to data rows, not the header
    ?_assertEqual(#{headers => [<<"h">>], data => [[<<"r1">>],[<<"r2">>]]},
                  glazer_csv:decode(<<"h\nr1\nr2\nr3\nr4\n">>,
                                    [headers, {limit, 2}])),
    %% skip + limit: skip 2, then take 2 → r3, r4
    ?_assertEqual(#{headers => nil, data => [[<<"r3">>],[<<"r4">>]]},
                  glazer_csv:decode(Csv, [{skip, 2}, {limit, 2}]))
  ].

%% ----------------------------------------------------------------------------
%% {return, map} option — data rows as maps keyed by the header names
%% ----------------------------------------------------------------------------

decode_return_map_test_() ->
  [
    %% basic: rows become maps keyed by binary header names
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [#{<<"a">> => <<"1">>, <<"b">> => <<"2">>}]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [headers, {return, map}])),
    %% multiple rows
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [#{<<"a">> => <<"1">>, <<"b">> => <<"2">>},
                                #{<<"a">> => <<"3">>, <<"b">> => <<"4">>}]},
                  glazer_csv:decode(<<"a,b\n1,2\n3,4\n">>, [headers, {return, map}])),
    %% no data rows
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => []},
                  glazer_csv:decode(<<"a,b\n">>, [headers, {return, map}])),
    %% {headers, atom} + {return, map}: atom keys
    ?_assertEqual(#{headers => [a, b],
                    data    => [#{a => <<"1">>, b => <<"2">>}]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{headers, atom}, {return, map}])),
    %% {return, map} with {fields, ...}: type conversion still applies
    ?_assertEqual(#{headers => [<<"name">>, <<"age">>],
                    data    => [#{<<"name">> => <<"Alice">>, <<"age">> => 30}]},
                  glazer_csv:decode(<<"name,age\nAlice,30\n">>,
                    [headers, {return, map}, {fields, [binary, integer]}])),
    %% {return, list} is the explicit default — same as omitting the option
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [[<<"1">>, <<"2">>]]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [headers, {return, list}])),
    %% {return, map} without headers: ignored, rows stay as lists
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>]]},
                  glazer_csv:decode(<<"a,b">>, [{return, map}])),
    %% duplicate header column raises duplicate_header when {return, map} is set
    ?_assertEqual({error, duplicate_header},
                  glazer_csv:try_decode(<<"a,a\n1,2\n">>, [headers, {return, map}]))
  ].

%% ----------------------------------------------------------------------------
%% {return, tuple} option — data rows as tuples of field values
%% ----------------------------------------------------------------------------

decode_return_tuple_test_() ->
  [
    %% basic: rows become tuples, no headers
    ?_assertEqual(#{headers => nil,
                    data    => [{<<"a">>, <<"b">>}, {<<"1">>, <<"2">>}]},
                  glazer_csv:decode(<<"a,b\n1,2\n">>, [{return, tuple}])),
    %% multiple rows, with headers
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>],
                    data    => [{<<"1">>, <<"2">>}, {<<"3">>, <<"4">>}]},
                  glazer_csv:decode(<<"a,b\n1,2\n3,4\n">>, [headers, {return, tuple}])),
    %% no data rows
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => []},
                  glazer_csv:decode(<<"a,b\n">>, [headers, {return, tuple}])),
    %% {return, tuple} with {fields, ...}: type conversion still applies
    ?_assertEqual(#{headers => [<<"name">>, <<"age">>],
                    data    => [{<<"Alice">>, 30}]},
                  glazer_csv:decode(<<"name,age\nAlice,30\n">>,
                    [headers, {return, tuple}, {fields, [binary, integer]}])),
    %% streaming: rows are returned as tuples
    ?_assertEqual({[{<<"a">>, <<"b">>}, {<<"1">>, <<"2">>}], [{<<"3">>, <<"4">>}]},
                  begin
                    D0 = glazer_csv:stream_decoder([{return, tuple}]),
                    {Rows1, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2\n3,4">>),
                    {ok, Rows2} = glazer_csv:stream_eof(D1),
                    {Rows1, Rows2}
                  end)
  ].

%% ----------------------------------------------------------------------------
%% {fields, Types} option — per-column type conversion
%% ----------------------------------------------------------------------------

decode_fields_test_() ->
  [
    %% integer
    ?_assertEqual(#{headers => nil, data => [[1, <<"b">>]]},
                  glazer_csv:decode(<<"1,b">>, [{fields, [integer]}])),
    %% negative integer and bignum
    ?_assertEqual(#{headers => nil,
                    data    => [[-5], [123456789012345678901234567890]]},
                  glazer_csv:decode(<<"-5\n123456789012345678901234567890">>,
                                     [{fields, [integer]}])),
    %% {float, Precision} rounds to the given number of decimal digits
    ?_assertEqual(#{headers => nil, data => [[3.14]]},
                  glazer_csv:decode(<<"3.14159">>, [{fields, [{float, 2}]}])),
    ?_assertEqual(#{headers => nil, data => [[3.0]]},
                  glazer_csv:decode(<<"3.14159">>, [{fields, [{float, 0}]}])),
    %% boolean, case-insensitive
    ?_assertEqual(#{headers => nil, data => [[true], [false], [true]]},
                  glazer_csv:decode(<<"true\nFALSE\nTrue">>, [{fields, [boolean]}])),
    %% datetime -> Unix epoch seconds (UTC)
    ?_assertEqual(#{headers => nil, data => [[1705314600]]},
                  glazer_csv:decode(<<"2024-01-15T10:30:00Z">>,
                                     [{fields, [{datetime, <<"%Y-%m-%dT%H:%M:%SZ">>}]}])),
    %% datetime with a numeric UTC offset
    ?_assertEqual(#{headers => nil, data => [[1705296600]]},
                  glazer_csv:decode(<<"2024-01-15T10:30:00+05:00">>,
                                     [{fields, [{datetime, <<"%Y-%m-%dT%H:%M:%S%z">>}]}])),
    %% datetime without a time component
    ?_assertEqual(#{headers => nil, data => [[1705276800]]},
                  glazer_csv:decode(<<"2024-01-15">>, [{fields, [{datetime, <<"%Y-%m-%d">>}]}])),
    %% binary (default / explicit no-op)
    ?_assertEqual(#{headers => nil, data => [[<<"abc">>]]},
                  glazer_csv:decode(<<"abc">>, [{fields, [binary]}])),
    %% charlist
    ?_assertEqual(#{headers => nil, data => [["abc"]]},
                  glazer_csv:decode(<<"abc">>, [{fields, [charlist]}])),
    %% fewer types than columns: extra columns stay binaries
    ?_assertEqual(#{headers => nil, data => [[1, <<"2">>, <<"3">>]]},
                  glazer_csv:decode(<<"1,2,3">>, [{fields, [integer]}])),
    %% applies positionally with headers, independent of the header names
    ?_assertEqual(#{headers => [<<"name">>, <<"age">>, <<"active">>],
                    data    => [[<<"Alice">>, 30, true]]},
                  glazer_csv:decode(<<"name,age,active\nAlice,30,true\n">>,
                                     [headers, {fields, [binary, integer, boolean]}]))
  ].

decode_fields_error_test_() ->
  [
    %% non-numeric field with `integer` type: default on_failure is `binary`
    ?_assertEqual(#{headers => nil, data => [[<<"not_a_number">>]]},
                  glazer_csv:decode(<<"not_a_number">>, [{fields, [integer]}])),
    %% invalid boolean: left as binary
    ?_assertEqual(#{headers => nil, data => [[<<"maybe">>]]},
                  glazer_csv:decode(<<"maybe">>, [{fields, [boolean]}])),
    %% datetime that doesn't match the format: left as binary
    ?_assertEqual(#{headers => nil, data => [[<<"not-a-date">>]]},
                  glazer_csv:decode(<<"not-a-date">>, [{fields, [{datetime, <<"%Y-%m-%d">>}]}])),
    %% on_failure => raise: errors with row/col (1-based) of the failing field
    ?_assertEqual({error, {invalid_field_value, 1, 1}},
                  glazer_csv:try_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => raise}]}])),
    ?_assertEqual({error, {invalid_field_value, 1, 1}},
                  glazer_csv:try_decode(<<"maybe">>,
                    [{fields, [#{type => boolean, on_failure => raise}]}])),
    ?_assertEqual({error, {invalid_field_value, 1, 1}},
                  glazer_csv:try_decode(<<"not-a-date">>,
                    [{fields, [#{type => {datetime, <<"%Y-%m-%d">>},
                                  on_failure => raise}]}])),
    %% error column/row are 1-based and point at the failing field
    ?_assertEqual({error, {invalid_field_value, 2, 2}},
                  glazer_csv:try_decode(<<"1,2\n3,bad">>,
                    [{fields, [#{type => integer, on_failure => raise},
                                #{type => integer, on_failure => raise}]}])),
    %% raises when using csv_decode/2
    ?_assertError({invalid_field_value, 1, 1},
                  glazer_csv:decode(<<"x">>, [{fields, [#{type => integer, on_failure => raise}]}])),

    %% on_failure => default: use the spec's default value
    ?_assertEqual(#{headers => nil, data => [[-1]]},
                  glazer_csv:decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, default => -1, on_failure => default}]}])),
    %% on_failure => default with no default given falls back to binary
    ?_assertEqual(#{headers => nil, data => [[<<"not_a_number">>]]},
                  glazer_csv:decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => default}]}])),
    %% on_failure => null: use the configured null term
    ?_assertEqual(#{headers => nil, data => [[null]]},
                  glazer_csv:decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => null}]}])),
    %% on_failure => null with {null_term, Atom}: use the custom null term
    ?_assertEqual(#{headers => nil, data => [[nil]]},
                  glazer_csv:decode(<<"not_a_number">>,
                    [{null_term, nil},
                     {fields, [#{type => integer, on_failure => null}]}])),
    %% on_failure => null for boolean
    ?_assertEqual(#{headers => nil, data => [[null]]},
                  glazer_csv:decode(<<"maybe">>,
                    [{fields, [#{type => boolean, on_failure => null}]}])),
    %% on_failure => null for {float, Precision}
    ?_assertEqual(#{headers => nil, data => [[null]]},
                  glazer_csv:decode(<<"not_a_float">>,
                    [{fields, [#{type => {float, 2}, on_failure => null}]}])),
    %% on_failure => null for datetime
    ?_assertEqual(#{headers => nil, data => [[null]]},
                  glazer_csv:decode(<<"not-a-date">>,
                    [{fields, [#{type => {datetime, <<"%Y-%m-%d">>}, on_failure => null}]}])),
    %% on_failure => null leaves valid fields converted, only the failing field becomes null
    ?_assertEqual(#{headers => nil, data => [[1, null]]},
                  glazer_csv:decode(<<"1,bad">>,
                    [{fields, [integer, #{type => integer, on_failure => null}]}])),
    %% on_failure => null with headers: the failing column becomes null in the row list
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => [[1, null]]},
                  glazer_csv:decode(<<"a,b\n1,bad\n">>,
                    [headers, {fields, [integer, #{type => integer, on_failure => null}]}])),
    %% on_failure => null with {null_term, Atom} and headers
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => [[1, nil]]},
                  glazer_csv:decode(<<"a,b\n1,bad\n">>,
                    [headers, {null_term, nil},
                     {fields, [integer, #{type => integer, on_failure => null}]}])),
    %% on_failure => null never fails csv_try_decode/2 (no error)
    ?_assertEqual({ok, #{headers => nil, data => [[null]]}},
                  glazer_csv:try_decode(<<"not_a_number">>,
                    [{fields, [#{type => integer, on_failure => null}]}])),

    %% empty field uses `default` regardless of on_failure
    ?_assertEqual(#{headers => nil, data => [[0, <<"b">>]]},
                  glazer_csv:decode(<<",b">>, [{fields, [#{type => integer, default => 0}]}])),
    %% empty field with default => null and {null_term, Atom}: uses the
    %% literal atom given as `default`, not the configured null_term
    ?_assertEqual(#{headers => nil, data => [[null, <<"b">>]]},
                  glazer_csv:decode(<<",b">>,
                    [{null_term, nil},
                     {fields, [#{type => integer, default => null}]}])),
    %% empty field with no `default` given: left as an empty binary
    ?_assertEqual(#{headers => nil, data => [[<<>>, <<"b">>]]},
                  glazer_csv:decode(<<",b">>, [{fields, [integer]}])),

    %% existing_atom: converts to an existing atom
    ?_assertEqual(#{headers => nil, data => [[ok]]},
                  glazer_csv:decode(<<"ok">>, [{fields, [existing_atom]}])),
    %% existing_atom: falls back to binary for unknown atoms
    ?_assertEqual(#{headers => nil, data => [[<<"zzzz_glazer_csv_no_such_atom">>]]},
                  glazer_csv:decode(<<"zzzz_glazer_csv_no_such_atom">>, [{fields, [existing_atom]}])),

    %% {atom, ExistingAtoms}: converts only if the text matches one of ExistingAtoms
    ?_assertEqual(#{headers => nil, data => [[ok], [error]]},
                  glazer_csv:decode(<<"ok\nerror">>, [{fields, [{atom, [ok, error]}]}])),
    %% {atom, ExistingAtoms}: falls back to binary if not in the whitelist
    ?_assertEqual(#{headers => nil, data => [[<<"other">>]]},
                  glazer_csv:decode(<<"other">>, [{fields, [{atom, [ok, error]}]}]))
  ].

%% ----------------------------------------------------------------------------
%% csv_try_decode/1,2 — {ok, Result} | {error, Reason}
%% ----------------------------------------------------------------------------

try_decode_test_() ->
  [
    ?_assertEqual({ok, #{headers => nil, data => [[<<"a">>, <<"b">>]]}},
                  glazer_csv:try_decode(<<"a,b">>)),
    ?_assertEqual({error, unterminated_quoted_field},
                  glazer_csv:try_decode(<<"\"unterminated">>)),
    %% unterminated quote with no closing quote anywhere, multi-line input
    ?_assertEqual({error, unterminated_quoted_field},
                  glazer_csv:try_decode(<<"a,b\n\"c,d\n">>))
  ].

decode_error_test_() ->
  [
    ?_assertError(unterminated_quoted_field, glazer_csv:decode(<<"\"unterminated">>))
  ].

%% ----------------------------------------------------------------------------
%% csv_encode/1,2 — basic rows
%% ----------------------------------------------------------------------------

encode_basic_test_() ->
  [
    ?_assertEqual(<<>>, glazer_csv:encode([])),
    ?_assertEqual(<<"a,b,c\r\n">>, glazer_csv:encode([[<<"a">>, <<"b">>, <<"c">>]])),
    ?_assertEqual(<<"a,b\r\n1,2\r\n">>,
                  glazer_csv:encode([[<<"a">>, <<"b">>], [1, 2]])),
    %% lf line ending
    ?_assertEqual(<<"a,b\n1,2\n">>,
                  glazer_csv:encode([[<<"a">>, <<"b">>], [1, 2]], [{line_ending, lf}])),
    %% atoms and floats
    ?_assertEqual(<<"foo,1.5\r\n">>, glazer_csv:encode([[foo, 1.5]])),
    %% empty row produces just a line terminator
    ?_assertEqual(<<"\r\n">>, glazer_csv:encode([[]])),
    %% large integers (bignum) are encoded as decimal text
    ?_assertEqual(<<"123456789012345678901234567890\r\n">>,
                  glazer_csv:encode([[123456789012345678901234567890]])),
    %% negative integers
    ?_assertEqual(<<"-1,2\r\n">>, glazer_csv:encode([[-1, 2]]))
  ].

%% ----------------------------------------------------------------------------
%% Encoding errors for unsupported field types
%% ----------------------------------------------------------------------------

encode_error_test_() ->
  [
    %% a tuple field is not encodable
    ?_assertError({encode_error, _}, glazer_csv:encode([[{a, b}]])),
    %% a nested list field is not encodable
    ?_assertError({encode_error, _}, glazer_csv:encode([[[<<"a">>]]])),
    %% a map field is not encodable
    ?_assertError({encode_error, _}, glazer_csv:encode([[#{}]])),
    %% headers option requires the rows to be maps
    ?_assertError({encode_error, _}, glazer_csv:encode([[<<"a">>, <<"b">>]], [headers])),
    %% top-level term must be a list
    ?_assertError({encode_error, _}, glazer_csv:encode(not_a_list)),
    %% improper row lists must be rejected rather than silently truncated — see issue #4
    ?_assertError({encode_error, _}, glazer_csv:encode([[<<"a">>|<<"b">>]])),
    ?_assertError({encode_error, _}, glazer_csv:encode([[<<"a">>, <<"b">>|<<"c">>]]))
  ].

%% ----------------------------------------------------------------------------
%% Quoting on encode
%% ----------------------------------------------------------------------------

encode_quoting_test_() ->
  [
    ?_assertEqual(<<"\"hello, world\",b\r\n">>,
                  glazer_csv:encode([[<<"hello, world">>, <<"b">>]])),
    ?_assertEqual(<<"\"a \"\"quoted\"\" word\"\r\n">>,
                  glazer_csv:encode([[<<"a \"quoted\" word">>]])),
    ?_assertEqual(<<"\"line1\nline2\"\r\n">>,
                  glazer_csv:encode([[<<"line1\nline2">>]]))
  ].

%% ----------------------------------------------------------------------------
%% Custom delimiter on encode
%% ----------------------------------------------------------------------------

encode_delimiter_test_() ->
  [
    ?_assertEqual(<<"a;b\r\n">>, glazer_csv:encode([[<<"a">>, <<"b">>]], [{delimiter, $;}]))
  ].

%% ----------------------------------------------------------------------------
%% headers option on encode
%% ----------------------------------------------------------------------------

encode_headers_test_() ->
  [
    ?_assertEqual(<<"a,b\r\n1,2\r\n">>,
                  glazer_csv:encode([#{<<"a">> => 1, <<"b">> => 2}], [headers])),
    %% missing key produces empty field
    ?_assertEqual(<<"a,b\r\n1,2\r\n1,\r\n">>,
                  glazer_csv:encode([#{<<"a">> => 1, <<"b">> => 2}, #{<<"a">> => 1}], [headers])),
    %% explicit column order overrides the first map's key order
    ?_assertEqual(<<"b,a\r\n2,1\r\n">>,
                  glazer_csv:encode([#{<<"a">> => 1, <<"b">> => 2}],
                                     [{headers, [<<"b">>, <<"a">>]}])),
    %% explicit column order with atom keys, missing key produces empty field
    ?_assertEqual(<<"a,b,c\r\n1,2,\r\n">>,
                  glazer_csv:encode([#{a => 1, b => 2}], [{headers, [a, b, c]}]))
  ].

%% ----------------------------------------------------------------------------
%% Round trip
%% ----------------------------------------------------------------------------

round_trip_test_() ->
  Rows = [[<<"a">>, <<"b,c">>], [<<"1">>, <<"2">>]],
  [
    ?_assertEqual(#{headers => nil, data => Rows},
                  glazer_csv:decode(glazer_csv:encode(Rows))),
    %% round trip with embedded quotes and newlines
    ?_assertEqual(#{headers => nil, data => [[<<"a \"quoted\"\nvalue">>, <<"b">>]]},
                  glazer_csv:decode(glazer_csv:encode([[<<"a \"quoted\"\nvalue">>, <<"b">>]]))),
    %% round trip with headers and a custom delimiter
    ?_assertEqual(#{headers => [<<"a">>, <<"b">>], data => [[<<"1">>, <<"x;y">>]]},
                  glazer_csv:decode(
                    glazer_csv:encode([#{<<"a">> => <<"1">>, <<"b">> => <<"x;y">>}],
                                       [headers, {delimiter, $;}]),
                    [headers, {delimiter, $;}])),
    %% round trip with lf line endings
    ?_assertEqual(#{headers => nil, data => [[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]]},
                  glazer_csv:decode(
                    glazer_csv:encode([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]], [{line_ending, lf}])))
  ].

%% ----------------------------------------------------------------------------
%% Large input — exercises the dirty-scheduler path (DIRTY_THRESHOLD bytes)
%% ----------------------------------------------------------------------------

large_input_test_() ->
  Rows  = [[integer_to_binary(I), integer_to_binary(I * 2)] || I <- lists:seq(1, 1000)],
  Csv   = glazer_csv:encode(Rows),
  [
    %% input is large enough to cross the dirty-scheduler threshold (8192 bytes)
    ?_assert(byte_size(Csv) >= 8192),
    ?_assertEqual(#{headers => nil, data => Rows}, glazer_csv:decode(Csv)),
    ?_assertEqual(1000, length(maps:get(data, glazer_csv:decode(Csv))))
  ].

%% ----------------------------------------------------------------------------
%% Streaming / incremental decode
%% ----------------------------------------------------------------------------

stream_decoder_test_() ->
  [
    %% rows split across feed/2 calls
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {[[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]], D1} =
        glazer_csv:stream_feed(D0, <<"a,b\n1,2\n3,">>),
      {[[<<"3">>, <<"4">>]], _D2} = glazer_csv:stream_feed(D1, <<"4\n">>),
      ok
    end),

    %% byte-at-a-time feeding decodes every row
    ?_test(begin
      Doc = <<"a,b\n1,2\n3,4\n">>,
      {Rows, DLast} = lists:foldl(
        fun(B, {Acc, D}) ->
          {R, D2} = glazer_csv:stream_feed(D, <<B>>),
          {Acc ++ R, D2}
        end, {[], glazer_csv:stream_decoder()}, binary_to_list(Doc)),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(DLast)),
      ?assertEqual([[<<"a">>, <<"b">>],
                     [<<"1">>, <<"2">>],
                     [<<"3">>, <<"4">>]], Rows)
    end),

    %% a trailing row with no terminator is only resolved at end-of-stream
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {[[<<"a">>, <<"b">>]], D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2">>),
      ?assertEqual({ok, [[<<"1">>, <<"2">>]]}, glazer_csv:stream_eof(D1))
    end),

    %% trailing newline at EOF yields no extra row
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {[[<<"a">>, <<"b">>]], D1} = glazer_csv:stream_feed(D0, <<"a,b\n">>),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(D1))
    end),

    %% blank lines are skipped, matching decode/2
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {Rows, D1} = glazer_csv:stream_feed(D0, <<"a,b\n\n1,2\n">>),
      ?assertEqual([[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]], Rows),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(D1))
    end),

    %% a quoted field containing an embedded newline doesn't end the row
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {Rows, D1} = glazer_csv:stream_feed(D0, <<"a,\"b\nc\"\nd,e\n">>),
      ?assertEqual([[<<"a">>, <<"b\nc">>], [<<"d">>, <<"e">>]], Rows),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(D1))
    end),

    %% a quoted field containing a doubled (escaped) quote doesn't end the row
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {Rows, D1} = glazer_csv:stream_feed(D0, <<"a,\"b\"\"c\"\n">>),
      ?assertEqual([[<<"a">>, <<"b\"c">>]], Rows),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(D1))
    end),

    %% with the headers option, the first row is captured internally and data
    %% rows are returned as field lists (not maps)
    ?_test(begin
      D0 = glazer_csv:stream_decoder([headers]),
      {Rows, D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2\n3,4\n">>),
      ?assertEqual([[<<"1">>, <<"2">>], [<<"3">>, <<"4">>]], Rows),
      ?assertEqual({ok, []}, glazer_csv:stream_eof(D1))
    end),

    %% {headers, atom} affects header names but not streaming field values
    ?_test(begin
      D0 = glazer_csv:stream_decoder([{headers, atom}]),
      {[[<<"1">>, <<"2">>]], _D1} = glazer_csv:stream_feed(D0, <<"a,b\n1,2\n">>)
    end),

    %% malformed trailing bytes surface as an error from stream_eof/1
    ?_test(begin
      D0 = glazer_csv:stream_decoder(),
      {[], D1} = glazer_csv:stream_feed(D0, <<"\"unterminated">>),
      ?assertMatch({error, unterminated_quoted_field}, glazer_csv:stream_eof(D1))
    end)
  ].

%% ----------------------------------------------------------------------------
%% read_file/1,2, write_file/2,3
%% ----------------------------------------------------------------------------

file_test_() ->
  Path = ?MODULE_STRING ++ ".tmp.csv",
  Rows = [[<<"a">>, <<"b">>], [<<"1">>, <<"2">>]],
  [
    ?_test(begin
      ?assertEqual(ok, glazer_csv:write_file(Path, Rows)),
      ?assertEqual(#{headers => nil, data => Rows}, glazer_csv:read_file(Path)),
      ok = file:delete(Path)
    end),

    ?_test(begin
      MapRows = [#{<<"a">> => <<"1">>, <<"b">> => <<"2">>}],
      ?assertEqual(ok, glazer_csv:write_file(Path, MapRows, [headers])),
      ?assertEqual(#{headers => [<<"a">>, <<"b">>], data => [[<<"1">>, <<"2">>]]},
                   glazer_csv:read_file(Path, [headers])),
      ok = file:delete(Path)
    end),

    ?_assertError(<<"nonexistent.csv: no such file or directory">>,
                   glazer_csv:read_file("nonexistent.csv"))
  ].
