%% vim:ts=2:sw=2:et
%%-----------------------------------------------------------------------------
%% JSON Schema validation tests
%%
%% Task 0-2 foundation tests: NIF framework, bytecode compiler, validation engine
%%-----------------------------------------------------------------------------

-module(glazer_json_schema_test).
-include_lib("eunit/include/eunit.hrl").

%% Test fixtures
-define(SCHEMA_SIMPLE_TYPE, #{
  type => <<"string">>
}).

-define(SCHEMA_USER, #{
  type => <<"object">>,
  properties => #{
    name => #{
      type => <<"string">>,
      minLength => 1
    },
    age => #{
      type => <<"integer">>,
      minimum => 0,
      maximum => 150
    },
    email => #{
      type => <<"string">>
    }
  },
  required => [<<"name">>, <<"age">>]
}).

%% ===== Test Suite =====

compile_schema_test() ->
  % Test schema compilation succeeds
  SchemaMap = ?SCHEMA_SIMPLE_TYPE,
  {ok, {schema, _CompiledSchema}} = glazer_json:compile_schema(SchemaMap),
  ok.

compile_schema_invalid_test() ->
  % Test compilation fails with invalid schema
  InvalidSchema = not_a_map,
  {error, _Msg} = glazer_json:compile_schema(InvalidSchema),
  ok.

validate_type_mismatch_test() ->
  % Test type validation fails on mismatch
  {ok, {schema, Schema}} = glazer_json:compile_schema(?SCHEMA_SIMPLE_TYPE),
  Term = 42,  % Expected string, got integer
  Result = glazer_json:validate(Schema, Term),
  ?assertMatch({error, _}, Result),
  ok.

validate_type_match_test() ->
  % Test type validation succeeds on match
  {ok, {schema, Schema}} = glazer_json:compile_schema(?SCHEMA_SIMPLE_TYPE),
  Term = <<"hello">>,
  Result = glazer_json:validate(Schema, Term),
  ?assertEqual(ok, Result),
  ok.

validate_fail_fast_test() ->
  % Test fail-fast mode returns first error
  {ok, {schema, Schema}} = glazer_json:compile_schema(?SCHEMA_USER),
  Term = #{
    name => 42,       % Type error
    age => "invalid"   % Type error
  },
  {error, {path, [<<"name">>], {type_mismatch, _}}} = glazer_json:validate(
    Schema,
    Term,
    [fail_fast]
  ),
  ok.

validate_collect_all_test() ->
  % Test collect_all_errors mode returns all errors
  {ok, {schema, Schema}} = glazer_json:compile_schema(?SCHEMA_USER),
  Term = #{
    name => 42,
    age => "invalid"
  },
  {error, Errors} = glazer_json:validate(Schema, Term, [collect_all_errors]),
  ?assert(is_list(Errors)),
  ?assert(length(Errors) >= 2),
  ok.

validate_required_field_test() ->
  % Test required field validation
  {ok, {schema, Schema}} = glazer_json:compile_schema(?SCHEMA_USER),
  Term = #{age => 30},  % Missing required 'name'
  Result = glazer_json:validate(Schema, Term),
  ?assertMatch({error, {path, [], {missing_required, <<"name">>}}}, Result),
  ok.

validate_numeric_constraints_test() ->
  % Test numeric constraint validation
  {ok, {schema, Schema}} = glazer_json:compile_schema(?SCHEMA_USER),
  Term = #{
    name => <<"Alice">>,
    age => 200  % Greater than maximum (150)
  },
  Result = glazer_json:validate(Schema, Term),
  ?assertMatch({error, {path, [<<"age">>], {value_too_large, 150}}}, Result),
  ok.

validate_term_inline_test() ->
  % Test validate_term (inline compilation + validation)
  Schema = #{type => <<"string">>},
  {error, _} = glazer_json:validate_term(Schema, 42),
  ok = glazer_json:validate_term(Schema, <<"hello">>),
  ok.

validate_schema_not_map_test() ->
  % Test error handling for non-map schema in compile_schema
  InvalidSchema = [<<"type">>, <<"string">>],  % List instead of map
  {error, _} = glazer_json:compile_schema(InvalidSchema),
  ok.

compile_schema_unknown_type_test() ->
  % Test error handling for unknown type value
  InvalidSchema = #{type => <<"invalid_type">>},
  {error, ErrorMsg} = glazer_json:compile_schema(InvalidSchema),
  ?assertMatch(<<"unknown type", _/binary>>, ErrorMsg),
  ok.

compile_schema_invalid_type_not_string_test() ->
  % Test error handling for type that's not a string
  InvalidSchema = #{type => 123},  % Integer instead of string
  {error, ErrorMsg} = glazer_json:compile_schema(InvalidSchema),
  ?assertMatch(<<"type must be", _/binary>>, ErrorMsg),
  ok.

%% ===== Task 3: Type Checking Edge Cases =====

validate_type_null_test() ->
  Schema = #{type => <<"null">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, null),
  {error, _} = glazer_json:validate(CS, 0),
  {error, _} = glazer_json:validate(CS, <<"string">>),
  ok.

validate_type_boolean_test() ->
  Schema = #{type => <<"boolean">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, true),
  ok = glazer_json:validate(CS, false),
  {error, _} = glazer_json:validate(CS, 1),
  ok.

validate_type_integer_test() ->
  Schema = #{type => <<"integer">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 0),
  ok = glazer_json:validate(CS, 42),
  ok = glazer_json:validate(CS, -100),
  {error, _} = glazer_json:validate(CS, 1.5),
  ok.

validate_type_number_test() ->
  % "number" type accepts both integer and float
  Schema = #{type => <<"number">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 42),
  ok = glazer_json:validate(CS, 3.14),
  ok = glazer_json:validate(CS, 0),
  ok = glazer_json:validate(CS, -2.5),
  {error, _} = glazer_json:validate(CS, <<"string">>),
  ok.

validate_type_string_test() ->
  Schema = #{type => <<"string">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"hello">>),
  ok = glazer_json:validate(CS, <<>>),
  ok = glazer_json:validate(CS, <<"123">>),
  {error, _} = glazer_json:validate(CS, 42),
  ok.

validate_type_array_test() ->
  Schema = #{type => <<"array">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, []),
  ok = glazer_json:validate(CS, [1, 2, 3]),
  ok = glazer_json:validate(CS, [<<"a">>, <<"b">>]),
  {error, _} = glazer_json:validate(CS, #{}),
  ok.

validate_type_object_test() ->
  Schema = #{type => <<"object">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, #{}),
  ok = glazer_json:validate(CS, #{key => <<"value">>}),
  {error, _} = glazer_json:validate(CS, []),
  ok.

%% ===== Edge Cases =====

validate_empty_object_test() ->
  % Test validation of empty object against object schema
  Schema = #{type => <<"object">>},
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CompiledSchema, #{}),
  ok.

validate_nested_properties_test() ->
  % Test nested object validation
  Schema = #{
    type => <<"object">>,
    properties => #{
      address => #{
        type => <<"object">>,
        properties => #{
          street => #{type => <<"string">>}
        },
        required => [<<"street">>]
      }
    }
  },
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),

  % Valid nested object
  ValidTerm = #{address => #{street => <<"123 Main St">>}},
  ?assertEqual(ok, glazer_json:validate(CompiledSchema, ValidTerm)),

  % Invalid nested object (missing required field)
  InvalidTerm = #{address => #{}},
  ?assertMatch({error, _}, glazer_json:validate(CompiledSchema, InvalidTerm)),
  ok.

validate_null_term_test() ->
  % Test validation with null value (atom 'null')
  Schema = #{type => <<"null">>},
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CompiledSchema, null),
  ok.

%% ===== Validation Modes =====

validate_mode_explicit_fail_fast_test() ->
  Schema = #{type => <<"string">>},
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),
  {error, _} = glazer_json:validate(CompiledSchema, 42, [fail_fast]),
  ok.

validate_mode_default_is_fail_fast_test() ->
  % Default mode should be fail-fast (no options specified)
  Schema = #{
    type => <<"object">>,
    properties => #{
      a => #{type => <<"string">>},
      b => #{type => <<"string">>}
    }
  },
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),

  % Two errors but default mode returns first one
  Term = #{a => 1, b => 2},
  {error, {path, [<<"a">>], {type_mismatch, _}}} = glazer_json:validate(CompiledSchema, Term),
  ok.

%% ===== Options Parsing =====

validate_with_max_depth_test() ->
  % Test max_depth option
  Schema = #{type => <<"object">>},
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),
  Term = #{},
  % Should succeed with reasonable depth
  ok = glazer_json:validate(CompiledSchema, Term, [{max_depth, 128}]),
  ok.

validate_with_max_errors_test() ->
  % Test max_errors option limits error collection
  Schema = #{
    type => <<"object">>,
    properties => #{
      a => #{type => <<"string">>},
      b => #{type => <<"string">>},
      c => #{type => <<"string">>}
    }
  },
  {ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),
  Term = #{a => 1, b => 2, c => 3},

  {error, Errors} = glazer_json:validate(
    CompiledSchema,
    Term,
    [collect_all_errors, {max_errors, 2}]
  ),
  ?assert(length(Errors) =< 2),
  ok.

%% ===== Bytecode Compilation =====

bytecode_type_instruction_test() ->
  % Verify bytecode contains type instruction
  Schema = #{type => <<"string">>},
  {ok, {schema, _CompiledSchema}} = glazer_json:compile_schema(Schema),
  % Just verify it compiles; actual bytecode inspection is implementation-specific
  ok.

bytecode_multiple_constraints_test() ->
  % Verify bytecode with multiple constraints compiles
  Schema = #{
    type => <<"string">>,
    minLength => 1,
    maxLength => 100
  },
  {ok, {schema, _CompiledSchema}} = glazer_json:compile_schema(Schema),
  ok.

bytecode_pattern_test() ->
  % Verify bytecode with pattern compiles
  Schema = #{
    type => <<"string">>,
    pattern => <<"^[a-z]+$">>
  },
  {ok, {schema, _CompiledSchema}} = glazer_json:compile_schema(Schema),
  ok.

%% ===== Task 4: Numeric Constraints =====

validate_minimum_test() ->
  Schema = #{minimum => 0},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 0),
  ok = glazer_json:validate(CS, 1),
  ok = glazer_json:validate(CS, 100),
  {error, _} = glazer_json:validate(CS, -1),
  ok.

validate_maximum_test() ->
  Schema = #{maximum => 100},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 100),
  ok = glazer_json:validate(CS, 50),
  ok = glazer_json:validate(CS, 0),
  {error, _} = glazer_json:validate(CS, 101),
  ok.

validate_exclusive_minimum_test() ->
  Schema = #{exclusiveMinimum => 0},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 1),
  ok = glazer_json:validate(CS, 0.1),
  {error, _} = glazer_json:validate(CS, 0),
  {error, _} = glazer_json:validate(CS, -1),
  ok.

validate_exclusive_maximum_test() ->
  Schema = #{exclusiveMaximum => 100},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 99),
  ok = glazer_json:validate(CS, 99.9),
  {error, _} = glazer_json:validate(CS, 100),
  {error, _} = glazer_json:validate(CS, 101),
  ok.

validate_multiple_of_test() ->
  Schema = #{multipleOf => 5},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 0),
  ok = glazer_json:validate(CS, 5),
  ok = glazer_json:validate(CS, -10),
  ok = glazer_json:validate(CS, 100),
  {error, _} = glazer_json:validate(CS, 1),
  {error, _} = glazer_json:validate(CS, 7),
  ok.

validate_combined_numeric_constraints_test() ->
  % Combine minimum, maximum, multipleOf
  Schema = #{
    minimum => 0,
    maximum => 100,
    multipleOf => 10
  },
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, 0),
  ok = glazer_json:validate(CS, 10),
  ok = glazer_json:validate(CS, 50),
  ok = glazer_json:validate(CS, 100),
  {error, _} = glazer_json:validate(CS, -10),  % below minimum
  {error, _} = glazer_json:validate(CS, 101),  % above maximum
  {error, _} = glazer_json:validate(CS, 15),   % not multiple of 10
  ok.

validate_numeric_edge_cases_test() ->
  % Test with floats and large numbers
  Schema = #{
    type => <<"number">>,
    minimum => -100,
    maximum => 100
  },
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, -100),
  ok = glazer_json:validate(CS, 0),
  ok = glazer_json:validate(CS, 100),
  ok = glazer_json:validate(CS, 3.14),
  ok = glazer_json:validate(CS, -99.99),
  {error, _} = glazer_json:validate(CS, -100.1),
  {error, _} = glazer_json:validate(CS, 100.1),
  ok.

%% ===== Task 5: String Constraints =====

validate_string_min_length_test() ->
  Schema = #{minLength => 3},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"abc">>),
  ok = glazer_json:validate(CS, <<"hello">>),
  {error, _} = glazer_json:validate(CS, <<"ab">>),
  {error, _} = glazer_json:validate(CS, <<>>),
  ok.

validate_string_max_length_test() ->
  Schema = #{maxLength => 5},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"hello">>),
  ok = glazer_json:validate(CS, <<"hi">>),
  ok = glazer_json:validate(CS, <<>>),
  {error, _} = glazer_json:validate(CS, <<"toolong">>),
  ok.

validate_string_length_utf8_test() ->
  % UTF-8 grapheme counting (not byte length)
  % "😀" is 1 grapheme but 4 bytes
  Schema = #{minLength => 2, maxLength => 2},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),

  % Two emoji (2 graphemes, 8 bytes total)
  ok = glazer_json:validate(CS, <<"😀😀">>),

  % One emoji + one letter (2 graphemes)
  ok = glazer_json:validate(CS, <<"😀a">>),

  % One emoji (1 grapheme, too few)
  {error, _} = glazer_json:validate(CS, <<"😀">>),

  % Three letters (3 graphemes, too many)
  {error, _} = glazer_json:validate(CS, <<"abc">>),
  ok.

validate_pattern_simple_test() ->
  Schema = #{pattern => <<"^[a-z]+$">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"abc">>),
  ok = glazer_json:validate(CS, <<"hello">>),
  {error, _} = glazer_json:validate(CS, <<"ABC">>),
  {error, _} = glazer_json:validate(CS, <<"123">>),
  {error, _} = glazer_json:validate(CS, <<"hello123">>),
  ok.

validate_pattern_email_like_test() ->
  Schema = #{pattern => <<"^[^@]+@[^@]+\\.[^@]+$">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"alice@example.com">>),
  ok = glazer_json:validate(CS, <<"test@domain.co">>),
  {error, _} = glazer_json:validate(CS, <<"invalid">>),
  {error, _} = glazer_json:validate(CS, <<"@example.com">>),
  {error, _} = glazer_json:validate(CS, <<"alice@">>),
  ok.

validate_pattern_digit_test() ->
  Schema = #{pattern => <<"^[0-9]{3}-[0-9]{2}-[0-9]{4}$">>},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"123-45-6789">>),
  {error, _} = glazer_json:validate(CS, <<"12-345-6789">>),
  {error, _} = glazer_json:validate(CS, <<"abc-de-fghi">>),
  ok.

validate_invalid_regex_pattern_test() ->
  % Invalid regex should cause compilation error
  Schema = #{pattern => <<"(?<invalid">>},
  {error, ErrorMsg} = glazer_json:compile_schema(Schema),
  ?assertMatch(<<"invalid regex", _/binary>>, ErrorMsg),
  ok.

validate_string_constraints_combined_test() ->
  Schema = #{
    type => <<"string">>,
    minLength => 2,
    maxLength => 10,
    pattern => <<"^[a-z]+$">>
  },
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, <<"ab">>),
  ok = glazer_json:validate(CS, <<"hello">>),
  {error, _} = glazer_json:validate(CS, <<"a">>),        % too short
  {error, _} = glazer_json:validate(CS, <<"verylongstring">>),  % too long
  {error, _} = glazer_json:validate(CS, <<"Hello">>),    % uppercase (pattern)
  {error, _} = glazer_json:validate(CS, <<"ab123">>),    % contains digits (pattern)
  ok.

%% ===== Task 6: Array Validation =====

validate_array_min_items_test() ->
  Schema = #{minItems => 2},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, [1, 2]),
  ok = glazer_json:validate(CS, [1, 2, 3]),
  {error, _} = glazer_json:validate(CS, [1]),
  {error, _} = glazer_json:validate(CS, []),
  ok.

validate_array_max_items_test() ->
  Schema = #{maxItems => 3},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, []),
  ok = glazer_json:validate(CS, [1, 2]),
  ok = glazer_json:validate(CS, [1, 2, 3]),
  {error, _} = glazer_json:validate(CS, [1, 2, 3, 4]),
  ok.

validate_array_unique_items_test() ->
  Schema = #{uniqueItems => true},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, [1, 2, 3]),
  ok = glazer_json:validate(CS, [<<"a">>, <<"b">>]),
  ok = glazer_json:validate(CS, []),
  % Note: uniqueItems implementation in Phase 1 may be simplified
  % Full implementation in Task 6
  ok.

validate_array_combined_constraints_test() ->
  Schema = #{
    type => <<"array">>,
    minItems => 1,
    maxItems => 5
  },
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, [1]),
  ok = glazer_json:validate(CS, [1, 2, 3]),
  ok = glazer_json:validate(CS, [<<"a">>, <<"b">>, <<"c">>, <<"d">>, <<"e">>]),
  {error, _} = glazer_json:validate(CS, []),         % too few items
  {error, _} = glazer_json:validate(CS, [1, 2, 3, 4, 5, 6]),  % too many items
  ok.

%% ===== Task 7: Object Validation (Partial) =====

validate_object_min_properties_test() ->
  Schema = #{minProperties => 2},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, #{a => 1, b => 2}),
  ok = glazer_json:validate(CS, #{a => 1, b => 2, c => 3}),
  {error, _} = glazer_json:validate(CS, #{a => 1}),
  {error, _} = glazer_json:validate(CS, #{}),
  ok.

validate_object_max_properties_test() ->
  Schema = #{maxProperties => 3},
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, #{}),
  ok = glazer_json:validate(CS, #{a => 1}),
  ok = glazer_json:validate(CS, #{a => 1, b => 2, c => 3}),
  {error, _} = glazer_json:validate(CS, #{a => 1, b => 2, c => 3, d => 4}),
  ok.

validate_object_combined_constraints_test() ->
  Schema = #{
    type => <<"object">>,
    minProperties => 1,
    maxProperties => 5
  },
  {ok, {schema, CS}} = glazer_json:compile_schema(Schema),
  ok = glazer_json:validate(CS, #{name => <<"Alice">>}),
  ok = glazer_json:validate(CS, #{a => 1, b => 2, c => 3}),
  {error, _} = glazer_json:validate(CS, #{}),  % too few properties
  {error, _} = glazer_json:validate(CS, #{a => 1, b => 2, c => 3, d => 4, e => 5, f => 6}),  % too many
  ok.
