-module(glazer_schema_test).
-include_lib("eunit/include/eunit.hrl").

%% persistent_term is global to the VM, so every test uses its own names.

person() ->
  #{<<"type">> => <<"object">>,
    <<"required">> => [<<"name">>],
    <<"properties">> => #{<<"name">> => #{<<"type">> => <<"string">>, <<"minLength">> => 1}}}.

person_v2() ->
  #{<<"type">> => <<"object">>,
    <<"required">> => [<<"name">>, <<"age">>],
    <<"properties">> => #{<<"name">> => #{<<"type">> => <<"string">>},
                          <<"age">> => #{<<"type">> => <<"integer">>}}}.

register_and_fetch_test() ->
  Name = {reg, fetch},
  ?assertEqual({ok, 1}, glazer_schema:register(Name, person())),
  ?assertMatch({ok, {schema, _}}, glazer_schema:fetch(Name)),
  ?assertMatch({schema, _}, glazer_schema:'fetch!'(Name)),
  ?assertEqual({ok, 1}, glazer_schema:version(Name)),
  ok = glazer_schema:unregister(Name).

fetched_schema_validates_test() ->
  Name = {reg, fetched_validates},
  {ok, 1} = glazer_schema:register(Name, person()),
  {ok, Compiled} = glazer_schema:fetch(Name),
  ok = glazer_schema:validate(Compiled, #{<<"name">> => <<"Ann">>}),
  ?assertMatch({error, {path, [], {missing_required, <<"name">>}}},
               glazer_schema:validate(Compiled, #{})),
  ok = glazer_schema:unregister(Name).

validate_by_name_test() ->
  Name = {reg, validate},
  {ok, 1} = glazer_schema:register(Name, person()),
  ok = glazer_schema:validate(Name, #{<<"name">> => <<"Ann">>}),
  ?assertMatch({error, {path, [<<"name">>], {string_too_short, <<>>}}},
               glazer_schema:validate(Name, #{<<"name">> => <<>>})),
  ?assertMatch({error, [_ | _]},
               glazer_schema:validate(Name, #{<<"name">> => <<>>},
                                               [collect_all_errors])),
  ok = glazer_schema:unregister(Name).

unknown_schema_test() ->
  Name = {reg, unknown},
  ?assertEqual({error, not_found}, glazer_schema:fetch(Name)),
  ?assertEqual({error, {unknown_schema, Name}},
               glazer_schema:validate(Name, 1)),
  ?assertError({unknown_schema, Name}, glazer_schema:'fetch!'(Name)),
  ?assertEqual({error, not_found}, glazer_schema:version(Name)).

same_schema_is_unchanged_test() ->
  Name = {reg, unchanged},
  {ok, 1} = glazer_schema:register(Name, person()),
  {ok, Before} = glazer_schema:fetch(Name),
  ?assertEqual({ok, 1}, glazer_schema:register(Name, person())),
  % the stored handle is reused, not recompiled
  {ok, After} = glazer_schema:fetch(Name),
  ?assertEqual(Before, After),
  ok = glazer_schema:unregister(Name).

changed_schema_bumps_version_test() ->
  Name = {reg, versions},
  {ok, 1} = glazer_schema:register(Name, person()),
  {ok, 2} = glazer_schema:register(Name, person_v2()),
  ?assertEqual({ok, 2}, glazer_schema:version(Name)),
  ?assertMatch({error, {path, [], {missing_required, <<"age">>}}},
               glazer_schema:validate(Name, #{<<"name">> => <<"Ann">>})),
  ok = glazer_schema:unregister(Name).

changed_options_bump_version_test() ->
  Name = {reg, options},
  Schema = #{<<"format">> => <<"email">>},
  {ok, 1} = glazer_schema:register(Name, Schema),
  {ok, 2} = glazer_schema:register(Name, Schema, [{format_assertion, false}]),
  ok = glazer_schema:validate(Name, <<"not-an-email">>),
  ok = glazer_schema:unregister(Name).

compile_error_stores_nothing_test() ->
  Name = {reg, bad},
  ?assertEqual({error, <<"unknown type: bogus">>},
               glazer_schema:register(Name, #{<<"type">> => <<"bogus">>})),
  ?assertEqual({error, not_found}, glazer_schema:fetch(Name)),
  ?assertEqual(false, lists:keymember(Name, 1, glazer_schema:list())).

unregister_test() ->
  Name = {reg, unregister},
  {ok, 1} = glazer_schema:register(Name, person()),
  ok = glazer_schema:unregister(Name),
  ?assertEqual({error, not_found}, glazer_schema:fetch(Name)),
  ?assertEqual(ok, glazer_schema:unregister(Name)).

list_test() ->
  A = {reg, list, a},
  B = {reg, list, b},
  {ok, 1} = glazer_schema:register(B, person()),
  {ok, 1} = glazer_schema:register(A, person()),
  {ok, 2} = glazer_schema:register(A, person_v2()),
  Listed = glazer_schema:list(),
  ?assertEqual({A, 2}, lists:keyfind(A, 1, Listed)),
  ?assertEqual({B, 1}, lists:keyfind(B, 1, Listed)),
  ok = glazer_schema:unregister(A),
  ok = glazer_schema:unregister(B),
  ?assertEqual(false, lists:keymember(A, 1, glazer_schema:list())).

%% compile_schema/1,2 go through the registry by default

compile_schema_is_interned_by_default_test() ->
  S = #{<<"type">> => <<"string">>, <<"maxLength">> => 7001},
  {ok, A} = glazer_schema:compile_schema(S),
  % same schema and options: the stored handle is returned (resource refs are
  % unique per compile, so equality shows nothing was recompiled)
  {ok, A} = glazer_schema:compile_schema(S),
  {ok, A} = glazer_schema:compile_schema(S, []),
  ok = glazer_schema:validate(A, <<"ok">>),
  _ = glazer_schema:clear_interned().

registry_false_compiles_fresh_test() ->
  S = #{<<"type">> => <<"string">>, <<"maxLength">> => 7002},
  {ok, A} = glazer_schema:compile_schema(S, [{registry, false}]),
  {ok, B} = glazer_schema:compile_schema(S, [{registry, false}]),
  ?assertNotEqual(A, B),
  % and does not touch the interned store
  {ok, C} = glazer_schema:compile_schema(S),
  ?assertNotEqual(A, C),
  _ = glazer_schema:clear_interned().

options_are_part_of_the_key_test() ->
  S = #{<<"format">> => <<"email">>},
  {ok, A} = glazer_schema:compile_schema(S),
  {ok, B} = glazer_schema:compile_schema(S, [{format_assertion, false}]),
  ?assertNotEqual(A, B),
  ?assertEqual({ok, A}, glazer_schema:compile_schema(S, [{registry, true}])),
  _ = glazer_schema:clear_interned().

invalid_registry_value_test() ->
  ?assertEqual({error, <<"registry must be true or false">>},
               glazer_schema:compile_schema(#{}, [{registry, sometimes}])).

compile_errors_are_not_interned_test() ->
  Bad = #{<<"type">> => <<"bogus">>},
  ?assertEqual({error, <<"unknown type: bogus">>}, glazer_schema:compile_schema(Bad)),
  ?assertEqual({error, <<"unknown type: bogus">>}, glazer_schema:compile_schema(Bad)).

clear_interned_test() ->
  S = #{<<"type">> => <<"number">>, <<"minimum">> => 7003},
  {ok, A} = glazer_schema:compile_schema(S),
  ?assert(glazer_schema:clear_interned() >= 1),
  {ok, B} = glazer_schema:compile_schema(S),
  ?assertNotEqual(A, B),
  _ = glazer_schema:clear_interned().

named_registrations_unaffected_by_clear_test() ->
  Name = {reg, survives_clear},
  {ok, 1} = glazer_schema:register(Name, person()),
  _ = glazer_schema:clear_interned(),
  ?assertMatch({ok, _}, glazer_schema:fetch(Name)),
  ok = glazer_schema:unregister(Name).

%% $id names, $ref resolution from the registry

schema_with_id_is_named_by_id_test() ->
  Id = ~"https://example.test/named-by-id-1.json",
  S = #{~"$id" => Id, ~"type" => ~"string"},
  {ok, C} = glazer_schema:compile_schema(S),
  ?assertEqual({ok, 1}, glazer_schema:version(Id)),
  ?assertEqual({ok, C}, glazer_schema:fetch(Id)),
  ?assertEqual(ok, glazer_schema:validate(Id, ~"x")),
  % the same schema again: unchanged, same handle
  ?assertEqual({ok, C}, glazer_schema:compile_schema(S)),
  ok = glazer_schema:unregister(Id).

fragment_in_id_is_dropped_test() ->
  S = #{~"$id" => ~"https://example.test/frag-1.json#"},
  ?assertEqual({ok, ~"https://example.test/frag-1.json"},
               glazer_schema:name_of(S)),
  ?assertEqual(error, glazer_schema:name_of(#{~"$id" => ~"#item"})),
  ?assertEqual(error, glazer_schema:name_of(#{~"type" => ~"string"})),
  ok = glazer_schema:unregister(~"https://example.test/frag-1.json").

register_1_uses_id_test() ->
  Id = ~"https://example.test/reg1-1.json",
  {ok, 1} = glazer_schema:register(#{~"$id" => Id, ~"type" => ~"integer"}),
  ?assertEqual({ok, 1}, glazer_schema:version(Id)),
  ?assertEqual({error, ~"schema has no $id to use as its name"},
               glazer_schema:register(#{~"type" => ~"integer"})),
  ok = glazer_schema:unregister(Id).

conflicting_id_creates_new_version_test() ->
  Id = ~"https://example.test/conflict-1.json",
  {ok, C1} = glazer_schema:compile_schema(#{~"$id" => Id, ~"type" => ~"string"}),
  {ok, C2} = glazer_schema:compile_schema(#{~"$id" => Id, ~"type" => ~"integer"}),
  ?assertNotEqual(C1, C2),
  ?assertEqual({ok, 2}, glazer_schema:version(Id)),
  ?assertMatch({error, _}, glazer_schema:validate(Id, ~"x")),
  % handles returned earlier still work
  ?assertEqual(ok, glazer_schema:validate(C1, ~"x")),
  ok = glazer_schema:unregister(Id).

ref_is_resolved_from_registry_test() ->
  Address = ~"https://example.test/address-1.json",
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Address,
                                         ~"type" => ~"object",
                                         ~"required" => [~"city"]}),
  Person = #{~"type" => ~"object",
             ~"properties" => #{~"home" => #{~"$ref" => Address}}},
  {ok, P} = glazer_schema:compile_schema(Person),
  ok = glazer_schema:validate(P, #{~"home" => #{~"city" => ~"Oslo"}}),
  ?assertMatch({error, _}, glazer_schema:validate(P, #{~"home" => #{}})),
  ok = glazer_schema:unregister(Address).

ref_resolution_is_transitive_test() ->
  Leaf = ~"https://example.test/leaf-1.json",
  Mid = ~"https://example.test/mid-1.json",
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Leaf, ~"type" => ~"integer"}),
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Mid,
                                         ~"properties" => #{~"n" => #{~"$ref" => Leaf}}}),
  {ok, Top} = glazer_schema:compile_schema(#{~"$ref" => Mid}),
  ok = glazer_schema:validate(Top, #{~"n" => 3}),
  ?assertMatch({error, _}, glazer_schema:validate(Top, #{~"n" => ~"x"})),
  ok = glazer_schema:unregister(Leaf),
  ok = glazer_schema:unregister(Mid).

explicit_refs_take_precedence_test() ->
  Doc = ~"https://example.test/prec-1.json",
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Doc, ~"type" => ~"string"}),
  Override = #{Doc => #{~"type" => ~"integer"}},
  {ok, C} = glazer_schema:compile_schema(#{~"$ref" => Doc}, [{refs, Override}]),
  ok = glazer_schema:validate(C, 1),
  ok = glazer_schema:unregister(Doc).

changed_referenced_document_is_recompiled_test() ->
  Doc = ~"https://example.test/changing-1.json",
  Top = #{~"$ref" => Doc},
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Doc, ~"type" => ~"string"}),
  {ok, A} = glazer_schema:compile_schema(Top, [{registry, false}]),
  ok = glazer_schema:validate(A, ~"x"),
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Doc, ~"type" => ~"integer"}),
  % a fresh compile sees the new document
  {ok, B} = glazer_schema:compile_schema(Top, [{registry, false}]),
  ok = glazer_schema:validate(B, 1),
  ?assertMatch({error, _}, glazer_schema:validate(B, ~"x")),
  ok = glazer_schema:unregister(Doc).

unknown_ref_is_still_unresolved_test() ->
  ?assertEqual({error, ~"unresolved $ref (document not provided): https://example.test/nope-1.json"},
               glazer_schema:compile_schema(#{~"$ref" => ~"https://example.test/nope-1.json"},
                                          [{registry, false}])).

%% the name option

name_option_names_schema_without_id_test() ->
  Name = ~"opt-named-1",
  {ok, C} = glazer_schema:compile_schema(#{~"type" => ~"string"}, [{name, Name}]),
  ?assertEqual({ok, C}, glazer_schema:fetch(Name)),
  ?assertEqual(ok, glazer_schema:validate(Name, ~"x")),
  ok = glazer_schema:unregister(Name).

name_option_accepts_string_test() ->
  {ok, _} = glazer_schema:compile_schema(#{~"type" => ~"integer"}, [{name, "opt-named-2"}]),
  ?assertEqual({ok, 1}, glazer_schema:version(~"opt-named-2")),
  ok = glazer_schema:unregister(~"opt-named-2").

name_option_equal_to_id_is_accepted_test() ->
  Id = ~"https://example.test/opt-same-1.json",
  {ok, C} = glazer_schema:compile_schema(#{~"$id" => Id, ~"type" => ~"string"}, [{name, Id}]),
  ?assertEqual({ok, C}, glazer_schema:fetch(Id)),
  ok = glazer_schema:unregister(Id).

name_option_differing_from_id_is_rejected_test() ->
  Id = ~"https://example.test/opt-diff-1.json",
  ?assertEqual({error, ~"name option differs from the schema's $id"},
               glazer_schema:compile_schema(#{~"$id" => Id, ~"type" => ~"string"},
                                          [{name, ~"other-name"}])),
  ?assertEqual({error, not_found}, glazer_schema:fetch(~"other-name")),
  ?assertEqual({error, not_found}, glazer_schema:fetch(Id)).

name_option_is_not_registered_without_registry_test() ->
  Name = ~"opt-noreg-1",
  {ok, _} = glazer_schema:compile_schema(#{~"type" => ~"string"},
                                       [{name, Name}, {registry, false}]),
  ?assertEqual({error, not_found}, glazer_schema:fetch(Name)).

name_option_mismatch_is_an_error_without_registry_test() ->
  Id = ~"https://example.test/opt-noreg-diff-1.json",
  ?assertEqual({error, ~"name option differs from the schema's $id"},
               glazer_schema:compile_schema(#{~"$id" => Id, ~"type" => ~"string"},
                                          [{name, ~"other"}, {registry, false}])).

invalid_name_option_test() ->
  ?assertEqual({error, ~"name must be a non-empty binary or string"},
               glazer_schema:compile_schema(#{}, [{name, 42}])),
  ?assertEqual({error, ~"name must be a non-empty binary or string"},
               glazer_schema:compile_schema(#{}, [{name, ~""}])).

no_name_and_no_id_is_stored_by_content_test() ->
  {ok, _} = glazer_schema:compile_schema(#{~"type" => ~"boolean", ~"const" => true}),
  ?assertEqual(false, lists:keymember(~"", 1, glazer_schema:list())),
  _ = glazer_schema:clear_interned(),
  ok.

%% compile_schema_file/1,2

compile_schema_file_test() ->
  Dir = case os:getenv("TMPDIR") of false -> "/tmp"; D -> D end,
  Good = filename:join(Dir, "glazer_schema_file_ok_" ++ integer_to_list(erlang:unique_integer([positive])) ++ ".json"),
  ok = file:write_file(Good, ~"""
    {"$id": "https://example.test/file-schema-1.json", "type": "object", "required": ["name"]}
    """),
  {ok, C} = glazer_schema:compile_schema_file(Good),
  ?assertEqual({ok, C}, glazer_schema:fetch(~"https://example.test/file-schema-1.json")),
  ok = glazer_schema:validate(C, #{~"name" => ~"Ann"}),
  ok = glazer_schema:unregister(~"https://example.test/file-schema-1.json"),
  {ok, _} = glazer_schema:compile_schema_file(Good, [{registry, false}]),
  ok = file:delete(Good).

compile_schema_file_errors_test() ->
  Missing = "/nonexistent/glazer-no-such-schema.json",
  ?assertMatch({error, {read_failed, Missing, enoent}}, glazer_schema:compile_schema_file(Missing)),
  Bad = filename:join(case os:getenv("TMPDIR") of false -> "/tmp"; D -> D end,
                      "glazer_schema_file_bad_" ++ integer_to_list(erlang:unique_integer([positive])) ++ ".json"),
  ok = file:write_file(Bad, ~"{not json"),
  ?assertMatch({error, {invalid_json, Bad, _}}, glazer_schema:compile_schema_file(Bad)),
  ok = file:write_file(Bad, ~"{\"type\": \"bogus\"}"),
  ?assertEqual({error, ~"unknown type: bogus"}, glazer_schema:compile_schema_file(Bad)),
  ok = file:delete(Bad).

%% info/0 return types

info_returns_binary_optimization_test() ->
  #{optimization := Opt, regex_engine := Engine, pgo := Pgo,
    version := V, app_version := AV} = glazer:info(),
  ?assert(is_binary(Opt)),
  ?assert(lists:member(Opt, [~"none", ~"O1", ~"O2", ~"O3"])),
  ?assert(lists:member(Engine, [pcre2, srell])),
  ?assert(is_boolean(Pgo)),
  ?assert(is_binary(V)),
  ?assert(is_binary(AV)).

%% compile_schema_file edge cases

compile_schema_file_boolean_schema_test() ->
  File = tmp_file("true"),
  ok = file:write_file(File, ~"true"),
  {ok, C} = glazer_schema:compile_schema_file(File, [{registry, false}]),
  ok = glazer_schema:validate(C, 1),
  ok = file:delete(File).

compile_schema_file_non_object_test() ->
  File = tmp_file("array"),
  ok = file:write_file(File, ~"[1, 2]"),
  ?assertMatch({error, _}, glazer_schema:compile_schema_file(File, [{registry, false}])),
  ok = file:delete(File).

compile_schema_file_passes_refs_test() ->
  Doc = ~"https://example.test/file-refs-doc-1.json",
  File = tmp_file("refs"),
  ok = file:write_file(File, ~"{\"$ref\": \"https://example.test/file-refs-doc-1.json\"}"),
  {ok, C} = glazer_schema:compile_schema_file(File,
              [{registry, false}, {refs, #{Doc => #{~"type" => ~"string"}}}]),
  ok = glazer_schema:validate(C, ~"x"),
  ?assertMatch({error, _}, glazer_schema:validate(C, 1)),
  ok = file:delete(File).

compile_schema_file_name_option_test() ->
  File = tmp_file("named"),
  ok = file:write_file(File, ~"{\"type\": \"integer\"}"),
  {ok, _} = glazer_schema:compile_schema_file(File, [{name, ~"file-named-1"}]),
  ?assertEqual(ok, glazer_schema:validate(~"file-named-1", 7)),
  ok = glazer_schema:unregister(~"file-named-1"),
  ok = file:delete(File).

%% $ref resolution across cycles and chains

ref_cycle_terminates_test() ->
  A = ~"https://example.test/cycle-a-1.json",
  B = ~"https://example.test/cycle-b-1.json",
  % B refers to A, which is not registered yet: give it as an explicit document
  {ok, _} = glazer_schema:compile_schema(
              #{~"$id" => B, ~"properties" => #{~"prev" => #{~"$ref" => A}}},
              [{refs, #{A => #{~"type" => ~"object"}}}]),
  % A refers to B, which is registered: A and B now refer to each other
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => A,
                                         ~"properties" => #{~"next" => #{~"$ref" => B}}}),
  % resolving A through the registry must terminate on the cycle
  {ok, Top} = glazer_schema:compile_schema(#{~"$ref" => A}, [{registry, false}]),
  ok = glazer_schema:validate(Top, #{~"next" => #{~"prev" => #{}}}),
  ok = glazer_schema:unregister(A),
  ok = glazer_schema:unregister(B).

self_reference_through_registry_test() ->
  Tree = ~"https://example.test/tree-1.json",
  {ok, _} = glazer_schema:compile_schema(#{~"$id" => Tree,
                                         ~"type" => ~"object",
                                         ~"properties" => #{~"child" => #{~"$ref" => Tree}}}),
  {ok, Top} = glazer_schema:compile_schema(#{~"$ref" => Tree}, [{registry, false}]),
  ok = glazer_schema:validate(Top, #{~"child" => #{~"child" => #{}}}),
  ?assertMatch({error, _}, glazer_schema:validate(Top, #{~"child" => 1})),
  ok = glazer_schema:unregister(Tree).

schema_with_unresolved_ref_is_not_registered_test() ->
  Id = ~"https://example.test/chain-a-1.json",
  ?assertEqual({error, ~"unresolved $ref (document not provided): https://example.test/chain-missing-1.json"},
               glazer_schema:compile_schema(#{~"$id" => Id,
                                            ~"$ref" => ~"https://example.test/chain-missing-1.json"})),
  ?assertEqual({error, not_found}, glazer_schema:fetch(Id)).

%% ipv4 and date

ipv4_and_date_boundaries_test() ->
  Check = fun(Format, V) ->
    {ok, {schema, S}} = glazer_schema:compile_schema(#{~"format" => Format}, [{registry, false}]),
    glazer_schema:validate(S, V) =:= ok
  end,
  true  = Check(~"ipv4", ~"255.255.255.255"),
  false = Check(~"ipv4", ~"255.255.255.256"),
  false = Check(~"ipv4", ~"1..2.3"),
  false = Check(~"ipv4", ~""),
  true  = Check(~"date", ~"2000-02-29"),
  false = Check(~"date", ~"1900-02-29"),
  false = Check(~"date", ~"2021-04-31"),
  false = Check(~"date", ~"2021-00-10"),
  true  = Check(~"date", ~"9999-12-31"),
  ok.

tmp_file(Tag) ->
  Dir = case os:getenv("TMPDIR") of false -> "/tmp"; D -> D end,
  filename:join(Dir, "glazer_test_" ++ Tag ++ "_" ++ integer_to_list(erlang:unique_integer([positive])) ++ ".json").

%% mutual references: groups

group_registers_mutual_references_test() ->
  A = ~"https://example.test/group-a-1.json",
  B = ~"https://example.test/group-b-1.json",
  SA = #{~"$id" => A, ~"type" => ~"object", ~"properties" => #{~"next" => #{~"$ref" => B}}},
  SB = #{~"$id" => B, ~"type" => ~"object", ~"properties" => #{~"prev" => #{~"$ref" => A}}},
  ?assertEqual({ok, [{A, 1}, {B, 1}]}, glazer_schema:register_group([SA, SB])),
  ?assertEqual({ok, 1}, glazer_schema:version(A)),
  ?assertEqual({ok, 1}, glazer_schema:version(B)),
  % both directions resolve, including through a chain back to the start
  {ok, Top} = glazer_schema:compile_schema(#{~"$ref" => A}, [{registry, false}]),
  ok = glazer_schema:validate(Top, #{~"next" => #{~"prev" => #{~"next" => #{}}}}),
  ?assertMatch({error, _}, glazer_schema:validate(Top, #{~"next" => #{~"prev" => 1}})),
  ok = glazer_schema:unregister(A),
  ok = glazer_schema:unregister(B).

group_is_all_or_nothing_test() ->
  A = ~"https://example.test/group-ok-1.json",
  Bad = #{~"$id" => ~"https://example.test/group-bad-1.json", ~"type" => ~"bogus"},
  Good = #{~"$id" => A, ~"type" => ~"string"},
  ?assertEqual({error, ~"unknown type: bogus"}, glazer_schema:register_group([Good, Bad])),
  ?assertEqual({error, not_found}, glazer_schema:fetch(A)).

group_needs_ids_test() ->
  ?assertEqual({error, ~"every schema in a group needs a $id"},
               glazer_schema:register_group([#{~"type" => ~"string"}])).

group_duplicate_ids_test() ->
  Id = ~"https://example.test/group-dup-1.json",
  S = #{~"$id" => Id, ~"type" => ~"string"},
  ?assertEqual({error, ~"duplicate $id in group"}, glazer_schema:register_group([S, S])),
  ?assertEqual({error, not_found}, glazer_schema:fetch(Id)).

group_reregistering_is_unchanged_test() ->
  A = ~"https://example.test/group-same-1.json",
  B = ~"https://example.test/group-same-2.json",
  SA = #{~"$id" => A, ~"properties" => #{~"b" => #{~"$ref" => B}}},
  SB = #{~"$id" => B, ~"type" => ~"integer"},
  {ok, _} = glazer_schema:register_group([SA, SB]),
  ?assertEqual({ok, [{A, 1}, {B, 1}]}, glazer_schema:register_group([SA, SB])),
  ok = glazer_schema:unregister(A),
  ok = glazer_schema:unregister(B).

%% compile_schema_file chooses the decoder by extension (or the format option)

file_schema_yaml_and_json_test() ->
  Dir = tmp_dir(),
  Yaml = filename:join(Dir, "user.yaml"),
  Yml = filename:join(Dir, "user.yml"),
  Json = filename:join(Dir, "USER.JSON"),
  Body = "type: object\nrequired: [name]\nproperties:\n  name: {type: string}\n",
  ok = file:write_file(Yaml, Body),
  ok = file:write_file(Yml, Body),
  ok = file:write_file(Json, ~"{\"type\": \"object\", \"required\": [\"name\"]}"),
  {ok, Y} = glazer_schema:compile_schema_file(Yaml, [{registry, false}]),
  {ok, Z} = glazer_schema:compile_schema_file(Yml, [{registry, false}]),
  {ok, J} = glazer_schema:compile_schema_file(Json, [{registry, false}]),
  ok = glazer_schema:validate(Y, #{~"name" => ~"Ann"}),
  ok = glazer_schema:validate(Z, #{~"name" => ~"Ann"}),
  ?assertMatch({error, _}, glazer_schema:validate(J, #{})),
  [ok = file:delete(F) || F <- [Yaml, Yml, Json]],
  ok.

file_schema_format_option_test() ->
  Dir = tmp_dir(),
  File = filename:join(Dir, "schema.txt"),
  ok = file:write_file(File, ~"{\"type\": \"string\"}"),
  ?assertMatch({error, {unsupported_format, ".txt"}}, glazer_schema:compile_schema_file(File)),
  {ok, C} = glazer_schema:compile_schema_file(File, [{format, json}, {registry, false}]),
  ok = glazer_schema:validate(C, ~"x"),
  ok = file:delete(File),
  ok.

file_schema_invalid_documents_test() ->
  Dir = tmp_dir(),
  Bad = filename:join(Dir, "bad.yaml"),
  BadJson = filename:join(Dir, "bad.json"),
  ok = file:write_file(Bad, ~"key: [unclosed"),
  ok = file:write_file(BadJson, ~"{not json"),
  ?assertMatch({error, {invalid_yaml, Bad, _}}, glazer_schema:compile_schema_file(Bad)),
  ?assertMatch({error, {invalid_json, BadJson, _}}, glazer_schema:compile_schema_file(BadJson)),
  ok = file:delete(Bad),
  ok = file:delete(BadJson),
  ok.

tmp_dir() ->
  case os:getenv("TMPDIR") of false -> "/tmp"; D -> D end.
