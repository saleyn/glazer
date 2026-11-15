-module(glazer_roadmap_tasks_test).
-include_lib("eunit/include/eunit.hrl").

%% Test items from the implementation roadmap, Tasks 3-7 (types, numbers,
%% strings, arrays, objects). Each test is named after the item it covers.

ok(Schema, Data) ->
  {ok, {schema, S}} = glazer_schema:compile_schema(Schema, [{registry, false}]),
  glazer_schema:validate(S, Data).

valid(Schema, Data)   -> ?assertEqual(ok, ok(Schema, Data)).
invalid(Schema, Data) -> ?assertMatch({error, _}, ok(Schema, Data)).

%% Task 3: type

type_combinations_test() ->
  Types = [~"null", ~"boolean", ~"integer", ~"number", ~"string", ~"array", ~"object"],
  Values = [null, true, 1, 1.5, ~"s", [1], #{~"k" => 1}],
  Expected = fun(T, V) ->
    case {T, V} of
      {~"null", null} -> true;
      {~"boolean", B} when is_boolean(B) -> true;
      {~"integer", I} when is_integer(I) -> true;
      {~"number", N} when is_number(N) -> true;
      {~"string", S} when is_binary(S) -> true;
      {~"array", L} when is_list(L) -> true;
      {~"object", M} when is_map(M) -> true;
      _ -> false
    end
  end,
  [begin
     Got = ok(#{~"type" => T}, V) =:= ok,
     ?assertEqual(Expected(T, V), Got, {T, V})
   end || T <- Types, V <- Values],
  ok.

type_list_test() ->
  valid(#{~"type" => [~"string", ~"null"]}, null),
  valid(#{~"type" => [~"string", ~"null"]}, ~"x"),
  invalid(#{~"type" => [~"string", ~"null"]}, 1).

type_invalid_schema_test() ->
  ?assertMatch({error, _}, glazer_schema:compile_schema(#{~"type" => ~"nonsense"}, [{registry, false}])),
  ?assertMatch({error, _}, glazer_schema:compile_schema(#{~"type" => 42}, [{registry, false}])).

number_is_float_or_integer_test() ->
  valid(#{~"type" => ~"number"}, 1),
  valid(#{~"type" => ~"number"}, 1.5),
  invalid(#{~"type" => ~"number"}, ~"1").

integer_accepts_integral_float_test() ->
  valid(#{~"type" => ~"integer"}, 2.0),
  invalid(#{~"type" => ~"integer"}, 2.5).

%% Task 4: numeric constraints

floats_vs_integers_test() ->
  valid(#{~"minimum" => 1.5}, 2),
  invalid(#{~"minimum" => 1.5}, 1),
  valid(#{~"multipleOf" => 0.5}, 2.5),
  invalid(#{~"multipleOf" => 0.5}, 2.7),
  valid(#{~"maximum" => 10}, 10.0),
  invalid(#{~"exclusiveMaximum" => 10}, 10.0).

big_integers_within_int64_test() ->
  Max = 9223372036854775807,
  valid(#{~"maximum" => Max}, Max),
  invalid(#{~"exclusiveMaximum" => Max}, Max),
  valid(#{~"minimum" => -Max}, -Max),
  valid(#{~"multipleOf" => 3}, Max - 1).          % Max is 1 mod 3

zero_negative_edges_test() ->
  valid(#{~"minimum" => 0}, 0),
  invalid(#{~"minimum" => 0}, -1),
  valid(#{~"maximum" => 0}, -0.0),
  valid(#{~"multipleOf" => 1}, 0),
  invalid(#{~"exclusiveMinimum" => 0}, 0).

%% Task 5: strings

utf8_code_points_test() ->
  Emoji = <<"😀"/utf8>>,
  ?assertEqual(4, byte_size(Emoji)),
  valid(#{~"minLength" => 1, ~"maxLength" => 1}, Emoji),
  invalid(#{~"maxLength" => 0}, Emoji).

pattern_edge_cases_test() ->
  valid(#{~"pattern" => ~""}, ~""),
  valid(#{~"pattern" => ~"^$"}, ~""),
  valid(#{~"pattern" => ~"^a\\.b$"}, ~"a.b"),
  invalid(#{~"pattern" => ~"^a\\.b$"}, ~"axb"),
  valid(#{~"pattern" => ~"^[\\[\\]]+$"}, ~"[]"),
  valid(#{~"pattern" => ~"\\d+"}, ~"abc123"),
  invalid(#{~"pattern" => ~"^\\d+$"}, ~"abc123").

pattern_error_in_schema_test() ->
  ?assertMatch({error, _}, glazer_schema:compile_schema(#{~"pattern" => ~"(unclosed"}, [{registry, false}])).

%% Task 6: arrays

items_schema_applies_to_each_element_test() ->
  Schema = #{~"items" => #{~"type" => ~"integer"}},
  valid(Schema, [1, 2, 3]),
  invalid(Schema, [1, ~"x", 3]).

nested_arrays_test() ->
  Schema = #{~"type" => ~"array",
             ~"items" => #{~"type" => ~"array", ~"items" => #{~"type" => ~"integer"}}},
  valid(Schema, [[1, 2], [], [3]]),
  invalid(Schema, [[1, 2], [~"x"]]).

unique_items_mixed_types_test() ->
  Schema = #{~"uniqueItems" => true},
  valid(Schema, [1, ~"1", true, null, [1], #{~"a" => 1}]),
  invalid(Schema, [1, 1.0]),
  invalid(Schema, [#{~"a" => 1, ~"b" => 2}, #{~"b" => 2, ~"a" => 1}]),
  invalid(Schema, [[1, 2], [1, 2]]).

empty_arrays_test() ->
  valid(#{~"items" => #{~"type" => ~"integer"}}, []),
  valid(#{~"minItems" => 0}, []),
  invalid(#{~"minItems" => 1}, []),
  valid(#{~"uniqueItems" => true}, []),
  valid(#{~"contains" => #{~"type" => ~"integer"}, ~"minContains" => 0}, []).

%% Task 7: objects

required_test() ->
  Schema = #{~"required" => [~"a", ~"b"]},
  valid(Schema, #{~"a" => 1, ~"b" => 2}),
  ?assertEqual({error, {path, [], {missing_required, ~"b"}}},
               ok(Schema, #{~"a" => 1})).

properties_test() ->
  Schema = #{~"properties" => #{~"age" => #{~"type" => ~"integer", ~"minimum" => 0}}},
  valid(Schema, #{~"age" => 3}),
  invalid(Schema, #{~"age" => -1}),
  valid(Schema, #{~"other" => ~"x"}).

additional_properties_test() ->
  Closed = #{~"properties" => #{~"a" => #{}}, ~"additionalProperties" => false},
  valid(Closed, #{~"a" => 1}),
  ?assertEqual({error, {path, [~"z"], {extra_properties, ~"z"}}},
               ok(Closed, #{~"a" => 1, ~"z" => 2})),
  Typed = #{~"additionalProperties" => #{~"type" => ~"integer"}},
  valid(Typed, #{~"x" => 1}),
  invalid(Typed, #{~"x" => ~"s"}).

nested_objects_test() ->
  Schema = #{~"properties" =>
               #{~"user" => #{~"required" => [~"name"],
                              ~"properties" => #{~"name" => #{~"type" => ~"string"}}}}},
  valid(Schema, #{~"user" => #{~"name" => ~"Ann"}}),
  ?assertMatch({error, {path, [~"user"], {missing_required, ~"name"}}},
               ok(Schema, #{~"user" => #{}})).

%% Task 9: custom format support (the formats compile option)

custom_format_test() ->
  Schema = #{~"format" => ~"sku"},
  {ok, C} = glazer_schema:compile_schema(Schema,
              [{registry, false}, {formats, #{~"sku" => ~"^[A-Z]{3}-[0-9]{4}$"}}]),
  ok = glazer_schema:validate(C, ~"ABC-1234"),
  ?assertEqual({error, {path, [], {format_invalid, ~"abc-1"}}},
               glazer_schema:validate(C, ~"abc-1")),
  ok = glazer_schema:validate(C, 42).                    % non-strings pass

custom_format_overrides_builtin_test() ->
  % a name in the formats option replaces the built-in format of the same name
  {ok, S} = glazer_schema:compile_schema(#{~"format" => ~"email"},
              [{registry, false}, {formats, #{~"email" => ~"^x"}}]),
  ok = glazer_schema:validate(S, ~"xyz"),
  ?assertMatch({error, _}, glazer_schema:validate(S, ~"joe@example.com")).

custom_format_respects_format_assertion_off_test() ->
  {ok, S} = glazer_schema:compile_schema(#{~"format" => ~"sku"},
              [{registry, false}, {format_assertion, false},
               {formats, #{~"sku" => ~"^[A-Z]{3}$"}}]),
  ok = glazer_schema:validate(S, ~"abc").

custom_format_invalid_pattern_test() ->
  ?assertMatch({error, _},
               glazer_schema:compile_schema(#{~"format" => ~"sku"},
                 [{registry, false}, {formats, #{~"sku" => ~"(unclosed"}}])).

custom_format_invalid_options_test() ->
  ?assertEqual({error, ~"formats must be a map of name to pattern"},
               glazer_schema:compile_schema(#{}, [{registry, false}, {formats, [1]}])),
  ?assertEqual({error, ~"formats must map binary names to binary patterns"},
               glazer_schema:compile_schema(#{}, [{registry, false}, {formats, #{1 => ~"x"}}])).

custom_format_is_part_of_the_registry_key_test() ->
  S = #{~"format" => ~"sku"},
  {ok, A} = glazer_schema:compile_schema(S, [{formats, #{~"sku" => ~"^A"}}]),
  {ok, B} = glazer_schema:compile_schema(S, [{formats, #{~"sku" => ~"^B"}}]),
  ?assertNotEqual(A, B),
  ok = glazer_schema:validate(A, ~"Ax"),
  ?assertMatch({error, _}, glazer_schema:validate(B, ~"Ax")),
  _ = glazer_schema:clear_interned(),
  ok.

%% minLength / maxLength count code points (RFC 8259 characters), not graphemes

min_length_counts_code_points_test() ->
  Decomposed = <<"e", 16#301/utf8>>,        % "é" as two code points, one grapheme
  valid(#{~"minLength" => 2}, Decomposed),
  invalid(#{~"minLength" => 3}, Decomposed),
  valid(#{~"maxLength" => 2}, Decomposed),
  invalid(#{~"maxLength" => 1}, Decomposed),
  invalid(#{~"minLength" => 2}, <<"💩"/utf8>>).   % one code point, one grapheme

%% Keyword dispatch: names that share their first 8 bytes with a known keyword
%% are unknown keywords (annotations), not that keyword

unknown_keywords_sharing_a_prefix_test() ->
  valid(#{~"maxItemsX" => 1}, [1, 2]),
  valid(#{~"minLengthX" => 5}, ~"ab"),
  valid(#{~"exclusiveMinimumX" => 5}, 1),
  invalid(#{~"maxItems" => 1}, [1, 2]),
  invalid(#{~"minLength" => 5}, ~"ab"),
  invalid(#{~"exclusiveMinimum" => 5}, 1),
  ok.

%% Regex engine failures are reported, not treated as mismatches

multiple_of_overflowing_quotient_test() ->
  % 1e308 / 0.5 overflows to infinity; the value is still an exact multiple
  valid(#{~"multipleOf" => 0.5}, 1.0e308),
  % 1e308 / 1e-10 overflows too; 1e308 is not a multiple of 1e-10 as doubles
  invalid(#{~"multipleOf" => 1.0e-10}, 1.0e308),
  ok.

regex_engine_error_is_reported_test() ->
  % catastrophic backtracking on a long non-matching input exceeds PCRE2's match limit
  Long = binary:copy(~"a", 40),
  Input = <<Long/binary, "!">>,
  ?assertMatch({error, {path, [], {regex_error, _}}},
               ok(#{~"pattern" => ~"^(a+)+$"}, Input)),
  % an ordinary mismatch is still a mismatch
  ?assertMatch({error, {path, [], {pattern_mismatch, _}}},
               ok(#{~"pattern" => ~"^[0-9]+$"}, ~"abc")),
  ok.
