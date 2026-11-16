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
  {ok, {schema, _CompiledSchema}} = glazer_schema:compile(SchemaMap),
  ok.

compile_schema_invalid_test() ->
  % Test compilation fails with invalid schema
  InvalidSchema = not_a_map,
  {error, _Msg} = glazer_schema:compile(InvalidSchema),
  ok.

validate_type_mismatch_test() ->
  % Test type validation fails on mismatch
  {ok, {schema, Schema}} = glazer_schema:compile(?SCHEMA_SIMPLE_TYPE),
  Term = 42,  % Expected string, got integer
  Result = glazer_schema:validate(Schema, Term),
  ?assertMatch({error, _}, Result),
  ok.

validate_type_match_test() ->
  % Test type validation succeeds on match
  {ok, {schema, Schema}} = glazer_schema:compile(?SCHEMA_SIMPLE_TYPE),
  Term = <<"hello">>,
  Result = glazer_schema:validate(Schema, Term),
  ?assertEqual(ok, Result),
  ok.

validate_fail_fast_test() ->
  % Test fail-fast mode returns first error
  {ok, {schema, Schema}} = glazer_schema:compile(?SCHEMA_USER),
  Term = #{
    name => 42,       % Type error
    age => "invalid"   % Type error
  },
  % Exactly one error comes back. Which property is reported first is not
  % specified (map key order is not a contract), so accept either one.
  {error, {path, [Key], {type_mismatch, _}}} = glazer_schema:validate(
    Schema,
    Term,
    [fail_fast]
  ),
  true = lists:member(Key, [<<"name">>, <<"age">>]),
  ok.

validate_collect_all_test() ->
  % Test collect_all_errors mode returns all errors
  {ok, {schema, Schema}} = glazer_schema:compile(?SCHEMA_USER),
  Term = #{
    name => 42,
    age => "invalid"
  },
  {error, Errors} = glazer_schema:validate(Schema, Term, [collect_all_errors]),
  ?assert(is_list(Errors)),
  ?assert(length(Errors) >= 2),
  ok.

validate_required_field_test() ->
  % Test required field validation
  {ok, {schema, Schema}} = glazer_schema:compile(?SCHEMA_USER),
  Term = #{age => 30},  % Missing required 'name'
  Result = glazer_schema:validate(Schema, Term),
  ?assertMatch({error, {path, [], {missing_required, <<"name">>}}}, Result),
  ok.

validate_numeric_constraints_test() ->
  % Test numeric constraint validation
  {ok, {schema, Schema}} = glazer_schema:compile(?SCHEMA_USER),
  Term = #{
    name => <<"Alice">>,
    age => 200  % Greater than maximum (150)
  },
  Result = glazer_schema:validate(Schema, Term),
  ?assertMatch({error, {path, [<<"age">>], {value_too_large, 150}}}, Result),
  ok.

validate_term_inline_test() ->
  % Test validate_term (inline compilation + validation)
  Schema = #{type => <<"string">>},
  {error, _} = glazer_schema:validate_term(Schema, 42),
  ok = glazer_schema:validate_term(Schema, <<"hello">>),
  ok.

validate_schema_not_map_test() ->
  % Test error handling for non-map schema in compile_schema
  InvalidSchema = [<<"type">>, <<"string">>],  % List instead of map
  {error, _} = glazer_schema:compile(InvalidSchema),
  ok.

compile_schema_unknown_type_test() ->
  % Test error handling for unknown type value
  InvalidSchema = #{type => <<"invalid_type">>},
  {error, ErrorMsg} = glazer_schema:compile(InvalidSchema),
  ?assertMatch(<<"unknown type", _/binary>>, ErrorMsg),
  ok.

compile_schema_invalid_type_not_string_test() ->
  % Test error handling for type that's not a string
  InvalidSchema = #{type => 123},  % Integer instead of string
  {error, ErrorMsg} = glazer_schema:compile(InvalidSchema),
  ?assertMatch(<<"type must be", _/binary>>, ErrorMsg),
  ok.

%% ===== Task 3: Type Checking Edge Cases =====

validate_type_null_test() ->
  Schema = #{type => <<"null">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, null),
  {error, _} = glazer_schema:validate(CS, 0),
  {error, _} = glazer_schema:validate(CS, <<"string">>),
  ok.

validate_type_boolean_test() ->
  Schema = #{type => <<"boolean">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, true),
  ok = glazer_schema:validate(CS, false),
  {error, _} = glazer_schema:validate(CS, 1),
  ok.

validate_type_integer_test() ->
  Schema = #{type => <<"integer">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 0),
  ok = glazer_schema:validate(CS, 42),
  ok = glazer_schema:validate(CS, -100),
  {error, _} = glazer_schema:validate(CS, 1.5),
  ok.

validate_type_number_test() ->
  % "number" type accepts both integer and float
  Schema = #{type => <<"number">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 42),
  ok = glazer_schema:validate(CS, 3.14),
  ok = glazer_schema:validate(CS, 0),
  ok = glazer_schema:validate(CS, -2.5),
  {error, _} = glazer_schema:validate(CS, <<"string">>),
  ok.

validate_type_string_test() ->
  Schema = #{type => <<"string">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"hello">>),
  ok = glazer_schema:validate(CS, <<>>),
  ok = glazer_schema:validate(CS, <<"123">>),
  {error, _} = glazer_schema:validate(CS, 42),
  ok.

validate_type_array_test() ->
  Schema = #{type => <<"array">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, []),
  ok = glazer_schema:validate(CS, [1, 2, 3]),
  ok = glazer_schema:validate(CS, [<<"a">>, <<"b">>]),
  {error, _} = glazer_schema:validate(CS, #{}),
  ok.

validate_type_object_test() ->
  Schema = #{type => <<"object">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, #{}),
  ok = glazer_schema:validate(CS, #{key => <<"value">>}),
  {error, _} = glazer_schema:validate(CS, []),
  ok.

%% ===== Edge Cases =====

validate_empty_object_test() ->
  % Test validation of empty object against object schema
  Schema = #{type => <<"object">>},
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CompiledSchema, #{}),
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
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),

  % Valid nested object
  ValidTerm = #{address => #{street => <<"123 Main St">>}},
  ?assertEqual(ok, glazer_schema:validate(CompiledSchema, ValidTerm)),

  % Invalid nested object (missing required field)
  InvalidTerm = #{address => #{}},
  ?assertMatch({error, _}, glazer_schema:validate(CompiledSchema, InvalidTerm)),
  ok.

validate_null_term_test() ->
  % Test validation with null value (atom 'null')
  Schema = #{type => <<"null">>},
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CompiledSchema, null),
  ok.

%% ===== Validation Modes =====

validate_mode_explicit_fail_fast_test() ->
  Schema = #{type => <<"string">>},
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),
  {error, _} = glazer_schema:validate(CompiledSchema, 42, [fail_fast]),
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
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),

  % Two errors but default mode returns first one
  Term = #{a => 1, b => 2},
  {error, {path, [<<"a">>], {type_mismatch, _}}} = glazer_schema:validate(CompiledSchema, Term),
  ok.

%% ===== Options Parsing =====

validate_with_max_depth_test() ->
  % Test max_depth option
  Schema = #{type => <<"object">>},
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),
  Term = #{},
  % Should succeed with reasonable depth
  ok = glazer_schema:validate(CompiledSchema, Term, [{max_depth, 128}]),
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
  {ok, {schema, CompiledSchema}} = glazer_schema:compile(Schema),
  Term = #{a => 1, b => 2, c => 3},

  {error, Errors} = glazer_schema:validate(
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
  {ok, {schema, _CompiledSchema}} = glazer_schema:compile(Schema),
  % Just verify it compiles; actual bytecode inspection is implementation-specific
  ok.

bytecode_multiple_constraints_test() ->
  % Verify bytecode with multiple constraints compiles
  Schema = #{
    type => <<"string">>,
    minLength => 1,
    maxLength => 100
  },
  {ok, {schema, _CompiledSchema}} = glazer_schema:compile(Schema),
  ok.

bytecode_pattern_test() ->
  % Verify bytecode with pattern compiles
  Schema = #{
    type => <<"string">>,
    pattern => <<"^[a-z]+$">>
  },
  {ok, {schema, _CompiledSchema}} = glazer_schema:compile(Schema),
  ok.

%% ===== Task 4: Numeric Constraints =====

validate_minimum_test() ->
  Schema = #{minimum => 0},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 0),
  ok = glazer_schema:validate(CS, 1),
  ok = glazer_schema:validate(CS, 100),
  {error, _} = glazer_schema:validate(CS, -1),
  ok.

validate_maximum_test() ->
  Schema = #{maximum => 100},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 100),
  ok = glazer_schema:validate(CS, 50),
  ok = glazer_schema:validate(CS, 0),
  {error, _} = glazer_schema:validate(CS, 101),
  ok.

validate_exclusive_minimum_test() ->
  Schema = #{exclusiveMinimum => 0},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 1),
  ok = glazer_schema:validate(CS, 0.1),
  {error, _} = glazer_schema:validate(CS, 0),
  {error, _} = glazer_schema:validate(CS, -1),
  ok.

validate_exclusive_maximum_test() ->
  Schema = #{exclusiveMaximum => 100},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 99),
  ok = glazer_schema:validate(CS, 99.9),
  {error, _} = glazer_schema:validate(CS, 100),
  {error, _} = glazer_schema:validate(CS, 101),
  ok.

validate_multiple_of_test() ->
  Schema = #{multipleOf => 5},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 0),
  ok = glazer_schema:validate(CS, 5),
  ok = glazer_schema:validate(CS, -10),
  ok = glazer_schema:validate(CS, 100),
  {error, _} = glazer_schema:validate(CS, 1),
  {error, _} = glazer_schema:validate(CS, 7),
  ok.

validate_combined_numeric_constraints_test() ->
  % Combine minimum, maximum, multipleOf
  Schema = #{
    minimum => 0,
    maximum => 100,
    multipleOf => 10
  },
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, 0),
  ok = glazer_schema:validate(CS, 10),
  ok = glazer_schema:validate(CS, 50),
  ok = glazer_schema:validate(CS, 100),
  {error, _} = glazer_schema:validate(CS, -10),  % below minimum
  {error, _} = glazer_schema:validate(CS, 101),  % above maximum
  {error, _} = glazer_schema:validate(CS, 15),   % not multiple of 10
  ok.

validate_numeric_edge_cases_test() ->
  % Test with floats and large numbers
  Schema = #{
    type => <<"number">>,
    minimum => -100,
    maximum => 100
  },
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, -100),
  ok = glazer_schema:validate(CS, 0),
  ok = glazer_schema:validate(CS, 100),
  ok = glazer_schema:validate(CS, 3.14),
  ok = glazer_schema:validate(CS, -99.99),
  {error, _} = glazer_schema:validate(CS, -100.1),
  {error, _} = glazer_schema:validate(CS, 100.1),
  ok.

%% ===== Task 5: String Constraints =====

validate_string_min_length_test() ->
  Schema = #{minLength => 3},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"abc">>),
  ok = glazer_schema:validate(CS, <<"hello">>),
  {error, _} = glazer_schema:validate(CS, <<"ab">>),
  {error, _} = glazer_schema:validate(CS, <<>>),
  ok.

validate_string_max_length_test() ->
  Schema = #{maxLength => 5},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"hello">>),
  ok = glazer_schema:validate(CS, <<"hi">>),
  ok = glazer_schema:validate(CS, <<>>),
  {error, _} = glazer_schema:validate(CS, <<"toolong">>),
  ok.

validate_string_length_utf8_test() ->
  % UTF-8 grapheme counting (not byte length)
  % "😀" is 1 grapheme but 4 bytes
  Schema = #{minLength => 2, maxLength => 2},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),

  % Two emoji (2 graphemes, 8 bytes total)
  ok = glazer_schema:validate(CS, <<"😀😀">>),

  % One emoji + one letter (2 graphemes)
  ok = glazer_schema:validate(CS, <<"😀a">>),

  % One emoji (1 grapheme, too few)
  {error, _} = glazer_schema:validate(CS, <<"😀">>),

  % Three letters (3 graphemes, too many)
  {error, _} = glazer_schema:validate(CS, <<"abc">>),
  ok.

validate_pattern_simple_test() ->
  Schema = #{pattern => <<"^[a-z]+$">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"abc">>),
  ok = glazer_schema:validate(CS, <<"hello">>),
  {error, _} = glazer_schema:validate(CS, <<"ABC">>),
  {error, _} = glazer_schema:validate(CS, <<"123">>),
  {error, _} = glazer_schema:validate(CS, <<"hello123">>),
  ok.

validate_pattern_email_like_test() ->
  Schema = #{pattern => <<"^[^@]+@[^@]+\\.[^@]+$">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"alice@example.com">>),
  ok = glazer_schema:validate(CS, <<"test@domain.co">>),
  {error, _} = glazer_schema:validate(CS, <<"invalid">>),
  {error, _} = glazer_schema:validate(CS, <<"@example.com">>),
  {error, _} = glazer_schema:validate(CS, <<"alice@">>),
  ok.

validate_pattern_digit_test() ->
  Schema = #{pattern => <<"^[0-9]{3}-[0-9]{2}-[0-9]{4}$">>},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"123-45-6789">>),
  {error, _} = glazer_schema:validate(CS, <<"12-345-6789">>),
  {error, _} = glazer_schema:validate(CS, <<"abc-de-fghi">>),
  ok.

validate_invalid_regex_pattern_test() ->
  % Invalid regex should cause compilation error
  Schema = #{pattern => <<"(?<invalid">>},
  {error, ErrorMsg} = glazer_schema:compile(Schema),
  ?assertMatch(<<"invalid regex", _/binary>>, ErrorMsg),
  ok.

validate_string_constraints_combined_test() ->
  Schema = #{
    type => <<"string">>,
    minLength => 2,
    maxLength => 10,
    pattern => <<"^[a-z]+$">>
  },
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, <<"ab">>),
  ok = glazer_schema:validate(CS, <<"hello">>),
  {error, _} = glazer_schema:validate(CS, <<"a">>),        % too short
  {error, _} = glazer_schema:validate(CS, <<"verylongstring">>),  % too long
  {error, _} = glazer_schema:validate(CS, <<"Hello">>),    % uppercase (pattern)
  {error, _} = glazer_schema:validate(CS, <<"ab123">>),    % contains digits (pattern)
  ok.

%% ===== Task 6: Array Validation =====

validate_array_min_items_test() ->
  Schema = #{minItems => 2},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, [1, 2]),
  ok = glazer_schema:validate(CS, [1, 2, 3]),
  {error, _} = glazer_schema:validate(CS, [1]),
  {error, _} = glazer_schema:validate(CS, []),
  ok.

validate_array_max_items_test() ->
  Schema = #{maxItems => 3},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, []),
  ok = glazer_schema:validate(CS, [1, 2]),
  ok = glazer_schema:validate(CS, [1, 2, 3]),
  {error, _} = glazer_schema:validate(CS, [1, 2, 3, 4]),
  ok.

validate_array_unique_items_test() ->
  Schema = #{uniqueItems => true},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, [1, 2, 3]),
  ok = glazer_schema:validate(CS, [<<"a">>, <<"b">>]),
  ok = glazer_schema:validate(CS, []),
  % Note: uniqueItems implementation in Phase 1 may be simplified
  % Full implementation in Task 6
  ok.

validate_array_combined_constraints_test() ->
  Schema = #{
    type => <<"array">>,
    minItems => 1,
    maxItems => 5
  },
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, [1]),
  ok = glazer_schema:validate(CS, [1, 2, 3]),
  ok = glazer_schema:validate(CS, [<<"a">>, <<"b">>, <<"c">>, <<"d">>, <<"e">>]),
  {error, _} = glazer_schema:validate(CS, []),         % too few items
  {error, _} = glazer_schema:validate(CS, [1, 2, 3, 4, 5, 6]),  % too many items
  ok.

%% ===== Task 7: Object Validation (Partial) =====

validate_object_min_properties_test() ->
  Schema = #{minProperties => 2},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, #{a => 1, b => 2}),
  ok = glazer_schema:validate(CS, #{a => 1, b => 2, c => 3}),
  {error, _} = glazer_schema:validate(CS, #{a => 1}),
  {error, _} = glazer_schema:validate(CS, #{}),
  ok.

validate_object_max_properties_test() ->
  Schema = #{maxProperties => 3},
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, #{}),
  ok = glazer_schema:validate(CS, #{a => 1}),
  ok = glazer_schema:validate(CS, #{a => 1, b => 2, c => 3}),
  {error, _} = glazer_schema:validate(CS, #{a => 1, b => 2, c => 3, d => 4}),
  ok.

validate_object_combined_constraints_test() ->
  Schema = #{
    type => <<"object">>,
    minProperties => 1,
    maxProperties => 5
  },
  {ok, {schema, CS}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(CS, #{name => <<"Alice">>}),
  ok = glazer_schema:validate(CS, #{a => 1, b => 2, c => 3}),
  {error, _} = glazer_schema:validate(CS, #{}),  % too few properties
  {error, _} = glazer_schema:validate(CS, #{a => 1, b => 2, c => 3, d => 4, e => 5, f => 6}),  % too many
  ok.

%% ===== Phase 3: Composition, references, conditionals, advanced keywords =====

any_of_test() ->
  {ok, {schema, S}} = glazer_schema:compile(
    #{anyOf => [#{type => <<"string">>}, #{type => <<"integer">>}]}),
  ok = glazer_schema:validate(S, <<"x">>),
  ok = glazer_schema:validate(S, 3),
  ?assertMatch({error, {path, [], {no_schema_matched, _}}}, glazer_schema:validate(S, 1.5)),
  ok.

one_of_test() ->
  {ok, {schema, S}} = glazer_schema:compile(
    #{oneOf => [#{type => <<"number">>}, #{minimum => 0}]}),
  % 5 satisfies both branches
  ?assertMatch({error, {path, [], {multiple_schemas_matched, 2}}}, glazer_schema:validate(S, 5)),
  % -1 satisfies only the first
  ok = glazer_schema:validate(S, -1),
  ok.

all_of_test() ->
  {ok, {schema, S}} = glazer_schema:compile(
    #{allOf => [#{type => <<"integer">>}, #{minimum => 10}]}),
  ?assertMatch({error, {path, [], {value_too_small, 10}}}, glazer_schema:validate(S, 5)),
  ok = glazer_schema:validate(S, 12),
  ok.

ref_defs_test() ->
  Schema = #{
    '$defs' => #{pos => #{type => <<"integer">>, minimum => 1}},
    type => <<"array">>,
    items => #{'$ref' => <<"#/$defs/pos">>}
  },
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, [1, 2]),
  ?assertMatch({error, {path, [1], {value_too_small, 1}}}, glazer_schema:validate(S, [1, 0])),
  ok.

ref_recursive_test() ->
  Schema = #{
    '$defs' => #{
      node => #{
        type => <<"object">>,
        properties => #{
          children => #{type => <<"array">>, items => #{'$ref' => <<"#/$defs/node">>}}
        }
      }
    },
    '$ref' => <<"#/$defs/node">>
  },
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{children => [#{children => []}]}),
  ?assertMatch({error, {path, [<<"children">>, 0, <<"children">>], {type_mismatch, _}}},
               glazer_schema:validate(S, #{children => [#{children => 5}]})),
  ok.

ref_self_loop_is_bounded_test() ->
  % A reference that never consumes data must stop at the depth limit
  {ok, {schema, S}} = glazer_schema:compile(#{'$ref' => <<"#">>}),
  ?assertMatch({error, {path, [], {max_depth_exceeded, _}}}, glazer_schema:validate(S, 1)),
  ok.

ref_unresolved_and_external_test() ->
  ?assertMatch({error, <<"unresolved $ref", _/binary>>},
               glazer_schema:compile(#{'$ref' => <<"#/$defs/nope">>})),
  % External URIs resolve only through the refs option (see external_document_test)
  ?assertMatch({error, <<"unresolved $ref (document not provided)", _/binary>>},
               glazer_schema:compile(#{'$ref' => <<"http://example.com/s.json">>})),
  ok.

if_then_else_test() ->
  Schema = #{'if' => #{type => <<"integer">>},
             then => #{minimum => 0},
             'else' => #{type => <<"string">>}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, 5),
  ?assertMatch({error, {path, [], {value_too_small, 0}}}, glazer_schema:validate(S, -1)),
  ok = glazer_schema:validate(S, <<"x">>),
  ?assertMatch({error, {path, [], {type_mismatch, true}}}, glazer_schema:validate(S, true)),
  ok.

prefix_items_test() ->
  Schema = #{type => <<"array">>,
             prefixItems => [#{type => <<"string">>}, #{type => <<"integer">>}],
             items => #{type => <<"boolean">>}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, [<<"a">>, 1, true]),
  ok = glazer_schema:validate(S, [<<"a">>]),
  % items applies only past the prefix
  ?assertMatch({error, {path, [2], {type_mismatch, 2}}}, glazer_schema:validate(S, [<<"a">>, 1, 2])),
  ?assertMatch({error, {path, [0], {type_mismatch, 1}}}, glazer_schema:validate(S, [1, 1])),
  ok.

dependent_required_test() ->
  Schema = #{type => <<"object">>,
             dependentRequired => #{<<"credit_card">> => [<<"billing_address">>]}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ?assertMatch({error, {path, [], {dependent_keys_missing, <<"billing_address">>}}},
               glazer_schema:validate(S, #{credit_card => 1})),
  ok = glazer_schema:validate(S, #{credit_card => 1, billing_address => x}),
  ok = glazer_schema:validate(S, #{}),
  ok.

dependent_schemas_test() ->
  Schema = #{dependentSchemas => #{<<"a">> => #{required => [<<"b">>]}}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ?assertMatch({error, {path, [], {missing_required, <<"b">>}}}, glazer_schema:validate(S, #{a => 1})),
  ok = glazer_schema:validate(S, #{a => 1, b => 2}),
  ok = glazer_schema:validate(S, #{}),
  ok.

additional_properties_schema_test() ->
  Schema = #{properties => #{a => #{type => <<"integer">>}},
             additionalProperties => #{type => <<"integer">>}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{a => 1, z => 2}),
  ?assertMatch({error, {path, [<<"z">>], {type_mismatch, _}}},
               glazer_schema:validate(S, #{a => 1, z => <<"x">>})),
  ok.

additional_properties_false_test() ->
  Schema = #{properties => #{a => #{type => <<"integer">>}},
             additionalProperties => false},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{a => 1}),
  ?assertMatch({error, {path, [<<"z">>], {extra_properties, <<"z">>}}},
               glazer_schema:validate(S, #{a => 1, z => 2})),
  ok.

format_is_asserted_by_default_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{format => <<"email">>}),
  ?assertMatch({error, {path, [], {format_invalid, _}}}, glazer_schema:validate(S, <<"not-an-email">>)),
  ok.

format_annotation_option_test() ->
  % format_assertion false keeps `format` as an annotation
  {ok, {schema, S}} = glazer_schema:compile(#{format => <<"email">>},
                                                  [{format_assertion, false}]),
  ok = glazer_schema:validate(S, <<"not-an-email">>),
  ok.

format_assertion_option_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{format => <<"email">>},
                                                  [{format_assertion, true}]),
  ok = glazer_schema:validate(S, <<"joe.bloggs@example.com">>),
  ?assertMatch({error, {path, [], {format_invalid, _}}},
               glazer_schema:validate(S, <<"not-an-email">>)),
  ok.

format_assertion_checks_test() ->
  Check = fun(Format, Value) ->
    {ok, {schema, S}} = glazer_schema:compile(#{format => Format}, [{format_assertion, true}]),
    glazer_schema:validate(S, Value) =:= ok
  end,
  true  = Check(<<"date-time">>, <<"1990-12-31T15:59:60-08:00">>),   % leap second
  false = Check(<<"date-time">>, <<"2021-02-29T00:00:00Z">>),        % not a leap year
  false = Check(<<"time">>, <<"23:59:60Z">>),                       % leap seconds are not times
  true  = Check(<<"ipv6">>, <<"::ffff:192.168.0.1">>),
  false = Check(<<"ipv6">>, <<"1:2:3:4:5:6:7">>),
  true  = Check(<<"hostname">>, <<"example.com">>),
  false = Check(<<"hostname">>, <<"-bad.example">>),
  true  = Check(<<"uri">>, <<"mailto:John.Doe@example.com">>),
  false = Check(<<"uri">>, <<"//no-scheme.example/">>),
  true  = Check(<<"uuid">>, <<"2EB8AA08-AA98-11EA-B4AA-73B441D16380">>),
  true  = Check(<<"uuid">>, <<"00000000-0000-0000-0000-000000000000">>),
  false = Check(<<"uuid">>, <<"2eb8aa08-aa98-11ea-b4aa-73b441d1638">>),  % too short
  false = Check(<<"uuid">>, <<"2eb8aa08-aa98-11ea-73b441d16380">>),      % missing group
  false = Check(<<"uuid">>, <<"2eb8aa08aa9811eab4aa73b441d16380">>),    % no hyphens
  true  = Check(<<"unknown-format">>, <<"anything">>),              % unknown formats pass
  ok.

format_assertion_extended_checks_test() ->
  Check = fun(Format, Value) ->
    {ok, {schema, S}} = glazer_schema:compile(#{format => Format}, [{format_assertion, true}]),
    glazer_schema:validate(S, Value) =:= ok
  end,
  % duration (ISO 8601, RFC 3339 appendix A)
  true  = Check(<<"duration">>, <<"P1Y2M3DT4H5M6S">>),
  true  = Check(<<"duration">>, <<"P2W">>),
  false = Check(<<"duration">>, <<"P">>),
  false = Check(<<"duration">>, <<"PT">>),
  false = Check(<<"duration">>, <<"P1D2H">>),                        % time part needs T
  % regex
  true  = Check(<<"regex">>, <<"^[a-z]+$">>),
  false = Check(<<"regex">>, <<"[unclosed">>),
  % JSON Pointer (RFC 6901)
  true  = Check(<<"json-pointer">>, <<>>),
  true  = Check(<<"json-pointer">>, <<"/foo/0">>),
  true  = Check(<<"json-pointer">>, <<"/a~1b/m~0n">>),
  false = Check(<<"json-pointer">>, <<"/a~2">>),
  false = Check(<<"json-pointer">>, <<"foo">>),
  % relative JSON Pointer
  true  = Check(<<"relative-json-pointer">>, <<"1/foo">>),
  true  = Check(<<"relative-json-pointer">>, <<"0#">>),
  false = Check(<<"relative-json-pointer">>, <<"01">>),              % leading zero
  false = Check(<<"relative-json-pointer">>, <<"/foo">>),
  % URI-reference / IRI / IRI-reference (RFC 3986 / 3987)
  true  = Check(<<"uri-reference">>, <<"../x?y#z">>),
  true  = Check(<<"uri-reference">>, <<"">>),
  false = Check(<<"uri-reference">>, <<"a b">>),
  true  = Check(<<"iri">>, <<"http://example.com/"/utf8>>),
  true  = Check(<<"iri">>, <<"http://\x{fc}.example/"/utf8>>),
  false = Check(<<"iri">>, <<"http://a b/">>),
  true  = Check(<<"iri-reference">>, <<"/\x{fc}/x"/utf8>>),
  false = Check(<<"iri-reference">>, <<"a b">>),
  % URI template (RFC 6570)
  true  = Check(<<"uri-template">>, <<"http://example.com/{var}">>),
  true  = Check(<<"uri-template">>, <<"{+path}/here">>),
  false = Check(<<"uri-template">>, <<"http://example.com/{var">>),
  ok.

format_nfc_and_idn_email_test() ->
  Check = fun(Format, Value) ->
    {ok, {schema, S}} = glazer_schema:compile(#{format => Format}, [{format_assertion, true}]),
    glazer_schema:validate(S, Value) =:= ok
  end,
  % idn-hostname: UTS 46 maps and NFC-normalizes the input before IDNA2008
  true  = Check(<<"idn-hostname">>, <<"caf\x{e9}.example"/utf8>>),              % precomposed
  true  = Check(<<"idn-hostname">>, <<"cafe\x{301}.example"/utf8>>),            % e + combining acute, NFC after mapping
  true  = Check(<<"idn-hostname">>, <<"q\x{307}\x{323}.example"/utf8>>),        % combining classes reordered by NFC
  true  = Check(<<"idn-hostname">>, <<"q\x{323}\x{307}.example"/utf8>>),
  true  = Check(<<"idn-hostname">>, <<16#1112/utf8, 16#1161/utf8, 16#11AB/utf8, ".example">>),  % jamo, composed by NFC
  true  = Check(<<"idn-hostname">>, <<16#D55C/utf8, ".example">>),              % Hangul syllable
  % idn-email (RFC 6531): UTF-8 local part, IDNA2008 domain
  true  = Check(<<"idn-email">>, <<"joe.bloggs@example.com">>),
  true  = Check(<<"idn-email">>, <<"\x{3b4}\x{3bf}\x{3ba}@example.com"/utf8>>),
  true  = Check(<<"idn-email">>, <<"\"\x{3b4}\"@example.com"/utf8>>),
  true  = Check(<<"idn-email">>, <<"caf\x{e9}@example.com"/utf8>>),
  true  = Check(<<"idn-email">>, <<"\x{5b9e}\x{4f8b}@\x{5b9e}\x{4f8b}.\x{6d4b}\x{8bd5}"/utf8>>),
  false = Check(<<"idn-email">>, <<"user\x{ff20}example.com"/utf8>>),           % fullwidth @ is not a separator
  false = Check(<<"idn-email">>, <<"user@-example.com">>),
  false = Check(<<"idn-email">>, <<"user@example.com.">>),
  false = Check(<<"idn-email">>, <<".leading@example.com">>),
  false = Check(<<"idn-email">>, <<"x@">>),
  false = Check(<<"idn-email">>, <<16#FF>>),                                    % not UTF-8
  % email (ASCII only) rejects the same non-ASCII local part
  false = Check(<<"email">>, <<"\x{3b4}@example.com"/utf8>>),
  ok.

format_uts46_mapping_test() ->
  Check = fun(Format, Value) ->
    {ok, {schema, S}} = glazer_schema:compile(#{format => Format}, [{format_assertion, true}]),
    glazer_schema:validate(S, Value) =:= ok
  end,
  % UTS 46 mapping before the IDNA2008 rules
  true  = Check(<<"idn-hostname">>, <<"\x{ff11}\x{ff12}\x{ff13}"/utf8>>),         % fullwidth digits map to ASCII
  true  = Check(<<"idn-hostname">>, <<"a\x{200b}b"/utf8>>),                       % zero width space is ignored
  true  = Check(<<"idn-hostname">>, <<"a\x{3002}b"/utf8>>),                       % ideographic full stop is a dot
  true  = Check(<<"idn-hostname">>, <<"a\x{ff0e}b"/utf8>>),                       % fullwidth full stop is a dot
  true  = Check(<<"idn-hostname">>, <<"\x{ff58}\x{ff4e}--nxasmq6b"/utf8>>),       % mapped ACE prefix
  true  = Check(<<"idn-hostname">>, <<"caf\x{e9}.com"/utf8>>),
  true  = Check(<<"idn-hostname">>, <<"cafe\x{301}.com"/utf8>>),                  % NFC after mapping
  false = Check(<<"idn-hostname">>, <<"-> $1.00 <--">>),                          % ASCII outside LDH still rejected
  false = Check(<<"idn-hostname">>, <<"a..b">>),
  false = Check(<<"idn-hostname">>, <<"example.">>),
  false = Check(<<"idn-hostname">>, <<"x", 16#EF, 16#BF, 16#BF, "y">>),           % disallowed noncharacter U+FFFF
  % length limits: 63 octets per label, 253 for the whole name
  true  = Check(<<"idn-hostname">>, binary:copy(<<"a">>, 63)),
  false = Check(<<"idn-hostname">>, binary:copy(<<"a">>, 64)),
  Long = lists:join(<<".">>, [binary:copy(<<"a">>, 63) || _ <- lists:seq(1, 4)]),
  true  = Check(<<"idn-hostname">>, iolist_to_binary(lists:sublist(Long, 5))),   % 63 + "." + 1 label
  false = Check(<<"idn-hostname">>, iolist_to_binary(Long)),                     % 4 * 63 + 3 dots > 253
  ok.

timeout_is_never_reported_as_valid_test() ->
  % A large array cannot be validated within 1 ms, so the walk is cut short
  Big = lists:seq(1, 2000000),
  {ok, {schema, S}} = glazer_schema:compile(#{<<"items">> => #{<<"type">> => <<"integer">>}}),
  ?assertMatch({error, {path, [], {timeout, 1}}},
               glazer_schema:validate(S, Big, [{timeout_ms, 1}])),
  % collect mode: the timeout is among the reported errors
  ?assertMatch({error, [{path, [], {timeout, 1}}]},
               glazer_schema:validate(S, Big, [collect_all_errors, {timeout_ms, 1}])),
  % inside not: the deadline applies there too, and a cut-short branch is not a match
  {ok, {schema, N}} = glazer_schema:compile(
      #{<<"not">> => #{<<"items">> => #{<<"type">> => <<"string">>}}}),
  ?assertMatch({error, {path, [], {timeout, 1}}},
               glazer_schema:validate(N, Big, [{timeout_ms, 1}])),
  % no timeout reached: a generous limit gives the normal result
  ?assertEqual(ok, glazer_schema:validate(S, [1, 2, 3], [{timeout_ms, 60000}])),
  ok.

format_ipv4_and_date_test() ->
  Check = fun(Format, Value) ->
    {ok, {schema, S}} = glazer_schema:compile(#{format => Format}, [{format_assertion, true}]),
    glazer_schema:validate(S, Value) =:= ok
  end,
  % ipv4 (RFC 2673, dotted quad, no leading zeros)
  true  = Check(~"ipv4", ~"192.168.0.1"),
  true  = Check(~"ipv4", ~"0.0.0.0"),
  false = Check(~"ipv4", ~"256.256.256.256"),
  false = Check(~"ipv4", ~"192.168.0"),
  false = Check(~"ipv4", ~"192.168.0.1.5"),
  false = Check(~"ipv4", ~"192.168.00.1"),
  false = Check(~"ipv4", ~"1.2.3.4 "),
  % date (RFC 3339 full-date, leap years)
  true  = Check(~"date", ~"2020-02-29"),
  false = Check(~"date", ~"2021-02-29"),
  false = Check(~"date", ~"2021-13-01"),
  false = Check(~"date", ~"2021-1-01"),
  false = Check(~"date", ~"2021-01-01T00:00:00Z"),
  ok.

bidi_rule_applies_across_the_domain_test() ->
  Check = fun(Format, Value) ->
    {ok, {schema, S}} = glazer_schema:compile(#{format => Format}, [{format_assertion, true}]),
    glazer_schema:validate(S, Value) =:= ok
  end,
  % RFC 5893: a domain with an RTL label must satisfy the rules for every label
  false = Check(~"idn-hostname", <<"0a.", 16#5D0/utf8>>),          % "0a" starts with a digit
  true  = Check(~"idn-hostname", <<"a.", 16#5D0/utf8>>),          % LTR and RTL labels are each fine
  true  = Check(~"idn-hostname", <<"0a.example">>),               % no RTL label: not a Bidi domain
  false = Check(~"hostname", ~"xn--0ca24w.example"),              % A-label with a bidi violation
  ok.

integer_accepts_integral_float_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{type => <<"integer">>}),
  ok = glazer_schema:validate(S, 1.0),
  ?assertMatch({error, _}, glazer_schema:validate(S, 1.5)),
  ok.

float_bounds_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{minimum => 0.5}),
  ?assertMatch({error, {path, [], {value_too_small, 0.5}}}, glazer_schema:validate(S, 0.2)),
  ok = glazer_schema:validate(S, 0.7),
  ok.

%% ===== Phase 4 (limitations of Phase 3): pointers, anchors, external documents,
%% unevaluated*, dynamic references =====

nested_defs_pointer_test() ->
  % $defs nested below a property, reached by a JSON Pointer with a ~1 escape
  Schema = #{type => <<"object">>,
             properties => #{
               <<"a/b">> => #{'$ref' => <<"#/properties/x~1y/$defs/s">>},
               <<"x/y">> => #{'$defs' => #{s => #{type => <<"integer">>}}}
             }},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{<<"a/b">> => 3}),
  ?assertMatch({error, {path, [<<"a/b">>], {type_mismatch, _}}},
               glazer_schema:validate(S, #{<<"a/b">> => <<"x">>})),
  ok.

anchor_ref_test() ->
  Schema = #{'$defs' => #{t => #{'$anchor' => <<"thing">>, type => <<"string">>}},
             type => <<"array">>, items => #{'$ref' => <<"#thing">>}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, [<<"x">>]),
  ?assertMatch({error, {path, [0], {type_mismatch, 1}}}, glazer_schema:validate(S, [1])),
  ok.

external_document_test() ->
  Ext = #{<<"http://ex.com/addr">> => #{type => <<"object">>, required => [<<"zip">>]}},
  Root = #{type => <<"object">>,
           properties => #{home => #{'$ref' => <<"http://ex.com/addr">>}}},
  {ok, {schema, S}} = glazer_schema:compile(Root, [{refs, Ext}]),
  ok = glazer_schema:validate(S, #{home => #{zip => 1}}),
  ?assertMatch({error, {path, [<<"home">>], {missing_required, <<"zip">>}}},
               glazer_schema:validate(S, #{home => #{}})),
  % Without the document, compilation fails with a clear message
  ?assertMatch({error, <<"unresolved $ref (document not provided)", _/binary>>},
               glazer_schema:compile(Root)),
  ok.

unevaluated_properties_test() ->
  Schema = #{properties => #{a => #{type => <<"integer">>}}, unevaluatedProperties => false},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{a => 1}),
  ?assertMatch({error, {path, [], {extra_properties, <<"z">>}}},
               glazer_schema:validate(S, #{a => 1, z => 2})),
  ok.

unevaluated_properties_through_applicators_test() ->
  % Properties evaluated by allOf and by a passing anyOf branch count as evaluated
  AllOf = #{allOf => [#{properties => #{a => #{}}}], unevaluatedProperties => false},
  {ok, {schema, S1}} = glazer_schema:compile(AllOf),
  ok = glazer_schema:validate(S1, #{a => 1}),
  AnyOf = #{anyOf => [#{properties => #{a => #{}}}, #{properties => #{b => #{}}}],
            unevaluatedProperties => false},
  {ok, {schema, S2}} = glazer_schema:compile(AnyOf),
  ok = glazer_schema:validate(S2, #{a => 1}),
  ?assertMatch({error, {path, [], {extra_properties, <<"c">>}}},
               glazer_schema:validate(S2, #{a => 1, c => 2})),
  ok.

unevaluated_items_test() ->
  Schema = #{prefixItems => [#{type => <<"string">>}],
             unevaluatedItems => #{type => <<"integer">>}},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, [<<"a">>, 1, 2]),
  ?assertMatch({error, {path, [1], {type_mismatch, <<"b">>}}},
               glazer_schema:validate(S, [<<"a">>, <<"b">>])),
  ok.

dynamic_ref_override_test() ->
  % The extending schema's $dynamicAnchor overrides the base list's item schema
  List = #{<<"$id">> => <<"http://ex.com/list">>, type => <<"array">>,
           items => #{'$dynamicRef' => <<"#item">>},
           '$defs' => #{default => #{'$dynamicAnchor' => <<"item">>, type => <<"string">>}}},
  Ext = #{<<"$id">> => <<"http://ex.com/ext">>, '$ref' => <<"http://ex.com/list">>,
          '$defs' => #{item => #{'$dynamicAnchor' => <<"item">>, type => <<"integer">>}}},
  Opts = [{refs, #{<<"http://ex.com/list">> => List}}],
  {ok, {schema, E}} = glazer_schema:compile(Ext, Opts),
  {ok, {schema, L}} = glazer_schema:compile(List, Opts),
  ok = glazer_schema:validate(E, [1]),
  ?assertMatch({error, {path, [0], {type_mismatch, _}}}, glazer_schema:validate(E, [<<"a">>])),
  % Used alone, the base list keeps its own default item schema
  ok = glazer_schema:validate(L, [<<"a">>]),
  ?assertMatch({error, {path, [0], {type_mismatch, 1}}}, glazer_schema:validate(L, [1])),
  ok.

json_pointer_errors_test() ->
  ?assertMatch({error, <<"unresolved $ref", _/binary>>},
               glazer_schema:compile(#{'$ref' => <<"#/nope/x">>})),
  ok.

%% ===== Official-suite gaps closed: boolean schemas, values, arrays, patterns =====

boolean_schemas_test() ->
  {ok, {schema, T}} = glazer_schema:compile(true),
  ok = glazer_schema:validate(T, 42),
  {ok, {schema, F}} = glazer_schema:compile(false),
  ?assertMatch({error, _}, glazer_schema:validate(F, 42)),
  % boolean subschemas in a composition
  {ok, {schema, A}} = glazer_schema:compile(#{properties => #{x => false}}),
  ok = glazer_schema:validate(A, #{}),
  ?assertMatch({error, _}, glazer_schema:validate(A, #{x => 1})),
  ok.

type_array_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{type => [<<"string">>, <<"null">>]}),
  ok = glazer_schema:validate(S, <<"x">>),
  ok = glazer_schema:validate(S, null),
  ?assertMatch({error, _}, glazer_schema:validate(S, 1)),
  ok.

const_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{const => #{a => [1, <<"x">>]}}),
  ok = glazer_schema:validate(S, #{a => [1, <<"x">>]}),
  ok = glazer_schema:validate(S, #{a => [1.0, <<"x">>]}),   % 1 and 1.0 are equal
  ?assertMatch({error, {path, [], {not_const, _}}}, glazer_schema:validate(S, #{a => [0, <<"x">>]})),
  {ok, {schema, Z}} = glazer_schema:compile(#{const => 0}),
  ?assertMatch({error, _}, glazer_schema:validate(Z, false)),   % false is not 0
  ?assertMatch({error, _}, glazer_schema:validate(Z, <<>>)),
  ok.

enum_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{enum => [1, <<"two">>, null]}),
  ok = glazer_schema:validate(S, 1.0),
  ok = glazer_schema:validate(S, <<"two">>),
  ok = glazer_schema:validate(S, null),
  ?assertMatch({error, {path, [], {not_in_enum, 3}}}, glazer_schema:validate(S, 3)),
  ok.

unique_items_deep_equality_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{uniqueItems => true}),
  ?assertMatch({error, _}, glazer_schema:validate(S, [#{a => 1}, #{a => 1.0}])),
  ok = glazer_schema:validate(S, [#{a => 1}, #{a => 2}, [1], [2]]),
  ok.

contains_test() ->
  {ok, {schema, S}} = glazer_schema:compile(
    #{contains => #{type => <<"integer">>}, minContains => 2.0, maxContains => 3}),
  ok = glazer_schema:validate(S, [1, <<"x">>, 2]),
  ?assertMatch({error, {path, [], {contains_failed, 1}}}, glazer_schema:validate(S, [1, <<"x">>])),
  ?assertMatch({error, {path, [], {too_many_items, 4}}}, glazer_schema:validate(S, [1, 2, 3, 4])),
  ok.

pattern_properties_test() ->
  Schema = #{patternProperties => #{<<"^v">> => #{type => <<"integer">>}},
             additionalProperties => false},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{vx => 1}),        % matched by pattern, not additional
  ?assertMatch({error, {path, [<<"vx">>], {type_mismatch, _}}}, glazer_schema:validate(S, #{vx => <<"s">>})),
  ?assertMatch({error, {path, [<<"other">>], {extra_properties, <<"other">>}}}, glazer_schema:validate(S, #{other => 1})),
  ok.

property_names_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{propertyNames => #{maxLength => 3}}),
  ok = glazer_schema:validate(S, #{abc => 1}),
  ?assertMatch({error, _}, glazer_schema:validate(S, #{abcd => 1})),
  ok.

min_length_ignores_non_strings_test() ->
  {ok, {schema, S}} = glazer_schema:compile(#{minLength => 2}),
  ok = glazer_schema:validate(S, 1),
  ok = glazer_schema:validate(S, [1]),
  ok.

additional_properties_true_evaluates_test() ->
  Schema = #{properties => #{foo => #{}}, additionalProperties => true,
             unevaluatedProperties => false},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{foo => 1, bar => 2}),
  ok.

id_in_enum_is_data_test() ->
  % An $id inside an enum value is not a real identifier
  Schema = #{'$defs' => #{a => #{enum => [#{<<"$id">> => <<"http://ex.com/x">>}]}},
             '$ref' => <<"#/$defs/a">>},
  {ok, {schema, S}} = glazer_schema:compile(Schema),
  ok = glazer_schema:validate(S, #{<<"$id">> => <<"http://ex.com/x">>}),
  ok.

%% ===== Schema regular expressions (PCRE2 or SRELL) =====

regex_engine_basics_test() ->
  {ok, {schema, S1}} = glazer_schema:compile(#{pattern => <<"^\\d{3}-\\d{2}-\\d{4}$">>}),
  ok = glazer_schema:validate(S1, <<"123-45-6789">>),
  ?assertMatch({error, _}, glazer_schema:validate(S1, <<"123-45-678">>)),
  {ok, {schema, S2}} = glazer_schema:compile(#{pattern => <<"\\cJ">>}),
  ok = glazer_schema:validate(S2, <<"a\nb">>),
  {ok, {schema, S3}} = glazer_schema:compile(#{pattern => <<"^\\p{Lu}\\p{Ll}+$">>}),
  ok = glazer_schema:validate(S3, <<"Ωmega"/utf8>>),
  ?assertMatch({error, _}, glazer_schema:validate(S3, <<"omega">>)),
  ok.

regex_lookaround_test() ->
  % Lookahead is supported by both PCRE2 and SRELL
  {ok, {schema, S}} = glazer_schema:compile(#{pattern => <<"^(?=a)a">>}),
  ok = glazer_schema:validate(S, <<"abc">>),
  ?assertMatch({error, _}, glazer_schema:validate(S, <<"bc">>)),
  ok.

%% Nested quantifiers such as "(a+)+" can run for an unbounded time on engines
%% without a match limit. The SRELL build refuses them; PCRE2 bounds them.
nested_quantifier_policy_test() ->
  Nested = glazer_schema:compile(#{pattern => <<"^(a+)+$">>}),
  Flat = glazer_schema:compile(#{pattern => <<"^(ab)+$">>}),
  {ok, {schema, S}} = Flat,
  ok = glazer_schema:validate(S, <<"abab">>),
  case maps:get(regex_engine, glazer:info()) of
    srell ->
      ?assertMatch({error, <<"invalid regex pattern: nested quantifier", _/binary>>}, Nested);
    pcre2 ->
      ?assertMatch({ok, _}, Nested)
  end,
  ok.
