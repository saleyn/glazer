-module(glazer_csv_decode_start_test).
-include_lib("eunit/include/eunit.hrl").

decode_start_simple_test_() ->
  ?_assertEqual(
    {[<<"a">>, <<"b">>, <<"c">>], nil, <<>>},
    glazer_csv:decode_start(<<"a,b,c\n">>, [])
  ).

decode_start_with_acc_test_() ->
  ?_assertEqual(
    {[<<"1">>, <<"2">>], my_acc, <<>>},
    glazer_csv:decode_start(<<"1,2\n">>, my_acc, [])
  ).

decode_start_incomplete_test_() ->
  {continue, State} = glazer_csv:decode_start(<<"a,b">>, []),
  ?_assertEqual(
    {[<<"a">>, <<"b">>], nil, <<>>},
    glazer_csv:decode_continue(<<"\n">>, State)
  ).

decode_continue_multiple_rows_test_() ->
  {continue, S1} = glazer_csv:decode_start(<<"1,2">>, []),
  {[<<"1">>, <<"2">>], nil, Rest} = glazer_csv:decode_continue(<<"\n3,4\n">>, S1),
  ?_assertEqual(<<"3,4\n">>, Rest).

decode_continue_end_of_input_with_partial_data_test_() ->
  {continue, State} = glazer_csv:decode_start(<<"1,2">>, []),
  ?_assertEqual(
    {[<<"1">>, <<"2">>], nil, <<>>},
    glazer_csv:decode_continue(end_of_input, State)
  ).

decode_continue_end_of_input_empty_buffer_test_() ->
  {[<<"1">>, <<"2">>], nil, <<>>} = glazer_csv:decode_start(<<"1,2\n">>, []),
  {continue, State} = glazer_csv:decode_start(<<"">>, []),
  ?_assertEqual(
    {nil, nil, <<>>},
    glazer_csv:decode_continue(end_of_input, State)
  ).
