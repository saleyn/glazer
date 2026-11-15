-module(glazer_bignum_test).
-include_lib("eunit/include/eunit.hrl").

%% Integers beyond int64 are exact: types, bounds, multipleOf, const/enum,
%% uniqueItems. Doubles would lose the low digits of these values.

ok(Schema, Data) ->
  {ok, {schema, S}} = glazer_schema:compile_schema(Schema, [{registry, false}]),
  glazer_schema:validate(S, Data).

assert(Schema, Data) -> ?assertEqual(ok, ok(Schema, Data)).
refute(Schema, Data) -> ?assertMatch({error, _}, ok(Schema, Data)).

big() -> 1 bsl 70.

integer_and_number_types_test() ->
  assert(#{~"type" => ~"integer"}, big()),
  assert(#{~"type" => ~"integer"}, -big()),
  assert(#{~"type" => ~"number"}, big()),
  refute(#{~"type" => ~"string"}, big()),
  refute(#{~"type" => ~"integer"}, 1.5).

maximum_is_exact_test() ->
  assert(#{~"maximum" => big()}, big()),
  refute(#{~"maximum" => big()}, big() + 1),
  assert(#{~"maximum" => big()}, big() - 1),
  % 2^63 - 1 is below the double 2^63: must not be rejected by rounding
  assert(#{~"maximum" => 1.0 * (1 bsl 63)}, (1 bsl 63) - 1),
  refute(#{~"maximum" => 1.0 * (1 bsl 63)}, (1 bsl 63) + 1).

minimum_is_exact_test() ->
  assert(#{~"minimum" => big() + 1}, big() + 1),
  refute(#{~"minimum" => big() + 1}, big()),
  assert(#{~"minimum" => -(1 bsl 63)}, -(1 bsl 63)),
  refute(#{~"minimum" => -(1 bsl 63)}, -(1 bsl 63) - 1).

exclusive_bounds_are_exact_test() ->
  refute(#{~"exclusiveMaximum" => big()}, big()),
  assert(#{~"exclusiveMaximum" => big()}, big() - 1),
  refute(#{~"exclusiveMinimum" => big()}, big()),
  assert(#{~"exclusiveMinimum" => big()}, big() + 1).

large_int_against_float_bound_test() ->
  % 10^19 + 1 is a bignum; the bound 1.0e19 is an exact double
  refute(#{~"maximum" => 1.0e19}, 10000000000000000001),
  assert(#{~"maximum" => 1.0e19}, 10000000000000000000),
  % an int64 above 2^53 against a float bound
  refute(#{~"maximum" => 1.0e18}, 1000000000000000001),
  assert(#{~"maximum" => 1.0e18}, 999999999999999999).

error_detail_is_the_exact_bound_test() ->
  ?assertEqual({error, {path, [], {value_too_large, big()}}},
               ok(#{~"maximum" => big()}, big() + 1)).

multiple_of_bignum_test() ->
  refute(#{~"multipleOf" => 3}, big()),        % 2^70 mod 3 = 1
  assert(#{~"multipleOf" => 2}, big()),
  assert(#{~"multipleOf" => big()}, big() * 3),
  refute(#{~"multipleOf" => big()}, big() + 1),
  assert(#{~"multipleOf" => 3}, 300000000000000000000),
  refute(#{~"multipleOf" => 3}, 100000000000000000000),
  assert(#{~"multipleOf" => 7}, -(big() - (big() rem 7))).

const_and_enum_are_exact_test() ->
  assert(#{~"const" => big()}, big()),
  refute(#{~"const" => big()}, big() + 1),
  refute(#{~"const" => big()}, -big()),
  assert(#{~"enum" => [1, big(), ~"x"]}, big()),
  refute(#{~"enum" => [big()]}, big() + 1).

const_bignum_against_float_test() ->
  % 2^70 as a float is exact, so a float with the same value matches
  assert(#{~"const" => big()}, 1.0 * big()),
  refute(#{~"const" => big()}, 1.0 * big() * 2).

unique_items_bignum_test() ->
  assert(#{~"uniqueItems" => true}, [big(), big() + 1]),
  refute(#{~"uniqueItems" => true}, [big(), big()]),
  refute(#{~"uniqueItems" => true}, [1, big(), big()]),
  assert(#{~"uniqueItems" => true}, [big(), 1.0 * big() * 2]).
