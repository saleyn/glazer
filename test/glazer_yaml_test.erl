-module(glazer_yaml_test).
-include_lib("eunit/include/eunit.hrl").

%% ----------------------------------------------------------------------------
%% Basic mappings and sequences
%% ----------------------------------------------------------------------------

mapping_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 2},
                  glazer:decode_yaml(<<"a: 1\nb: 2\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"b">> => 1, <<"c">> => 2}},
                  glazer:decode_yaml(<<"a:\n  b: 1\n  c: 2\n">>)),
    ?_assertEqual(#{<<"nested">> => #{<<"a">> => #{<<"b">> => 1}}},
                  glazer:decode_yaml(<<"nested:\n  a:\n    b: 1\n">>)),
    ?_assertEqual(#{<<"a">> => null},
                  glazer:decode_yaml(<<"a:\n">>)),
    ?_assertEqual(#{<<"empty">> => null, <<"after">> => 1},
                  glazer:decode_yaml(<<"empty:\nafter: 1\n">>))
  ].

sequence_test_() ->
  [
    ?_assertEqual([1, 2, 3],
                  glazer:decode_yaml(<<"- 1\n- 2\n- 3\n">>)),
    ?_assertEqual(#{<<"list">> => [<<"a">>, <<"b">>]},
                  glazer:decode_yaml(<<"list:\n  - a\n  - b\n">>)),
    ?_assertEqual(#{<<"a">> => [1, 2]},
                  glazer:decode_yaml(<<"a:\n  - 1\n  - 2\n">>))
  ].

%% Sequences whose items are themselves mappings/sequences, inline after "- ".
nested_dash_test_() ->
  [
    ?_assertEqual([#{<<"a">> => 1, <<"b">> => 2}, #{<<"a">> => 3, <<"b">> => 4}],
                  glazer:decode_yaml(<<"- a: 1\n  b: 2\n- a: 3\n  b: 4\n">>)),
    ?_assertEqual([[1, 2], 3],
                  glazer:decode_yaml(<<"- - 1\n  - 2\n- 3\n">>)),
    ?_assertEqual([[[1, 2], 3]],
                  glazer:decode_yaml(<<"- - - 1\n    - 2\n  - 3\n">>)),
    ?_assertEqual([#{<<"a">> => #{<<"b">> => 1, <<"c">> => 2}, <<"d">> => 3}],
                  glazer:decode_yaml(<<"- a:\n    b: 1\n    c: 2\n  d: 3\n">>)),
    ?_assertEqual([#{<<"a">> => 1, <<"b">> => [<<"x">>, <<"y">>]}, #{<<"c">> => 2}],
                  glazer:decode_yaml(<<"- a: 1\n  b:\n    - x\n    - y\n- c: 2\n">>)),
    ?_assertEqual(#{<<"top">> => [#{<<"k">> => 1, <<"v">> => 2}, #{<<"k">> => 3, <<"v">> => 4}]},
                  glazer:decode_yaml(<<"top:\n  - k: 1\n    v: 2\n  - k: 3\n    v: 4\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Plain scalars
%% ----------------------------------------------------------------------------

plain_scalar_test_() ->
  [
    ?_assertEqual(<<"plain text">>, glazer:decode_yaml(<<"plain text\n">>)),
    %% multi-line plain scalars fold a single line break to a space
    ?_assertEqual(#{<<"a">> => <<"line one line two">>, <<"b">> => 2},
                  glazer:decode_yaml(<<"a: line one\n  line two\nb: 2\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Quoted scalars
%% ----------------------------------------------------------------------------

quoted_scalar_test_() ->
  [
    %% single-quoted: '' escapes to a literal '
    ?_assertEqual(#{<<"name">> => <<"it's">>},
                  glazer:decode_yaml(<<"name: 'it''s'\n">>)),
    %% double-quoted: full escape table
    ?_assertEqual(#{<<"name">> => <<"line1\nline2">>},
                  glazer:decode_yaml(<<"name: \"line1\\nline2\"\n">>)),
    %% quoted scalars are never subject to implicit typing
    ?_assertEqual(#{<<"a">> => <<"yes">>},
                  glazer:decode_yaml(<<"a: 'yes'\n">>)),
    ?_assertEqual(#{<<"a">> => <<"123">>},
                  glazer:decode_yaml(<<"a: \"123\"\n">>)),
    %% quoted keys
    ?_assertEqual(#{<<"quoted key">> => 1},
                  glazer:decode_yaml(<<"\"quoted key\": 1\n">>)),
    ?_assertEqual(#{<<"single key">> => 1},
                  glazer:decode_yaml(<<"'single key': 1\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Comments and document markers
%% ----------------------------------------------------------------------------

comments_and_markers_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1},
                  glazer:decode_yaml(<<"# comment\na: 1\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"b">> => 1}},
                  glazer:decode_yaml(<<"a: # comment\n  b: 1\n">>)),
    ?_assertEqual(#{<<"a">> => 1},
                  glazer:decode_yaml(<<"---\na: 1\n">>)),
    ?_assertEqual(null,
                  glazer:decode_yaml(<<"   \n# just a comment\n">>)),
    ?_assertEqual(null,
                  glazer:decode_yaml(<<"">>))
  ].

%% ----------------------------------------------------------------------------
%% Implicit scalar typing — YAML 1.2 core schema (default)
%% ----------------------------------------------------------------------------

null_test_() ->
  [
    ?_assertEqual(#{<<"a">> => null}, glazer:decode_yaml(<<"a: null\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer:decode_yaml(<<"a: ~\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer:decode_yaml(<<"a: Null\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer:decode_yaml(<<"a: NULL\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer:decode_yaml(<<"a:\n">>))
  ].

bool_test_() ->
  [
    ?_assertEqual(#{<<"a">> => true},  glazer:decode_yaml(<<"a: true\n">>)),
    ?_assertEqual(#{<<"a">> => false}, glazer:decode_yaml(<<"a: false\n">>)),
    ?_assertEqual(#{<<"a">> => true},  glazer:decode_yaml(<<"a: True\n">>)),
    ?_assertEqual(#{<<"a">> => false}, glazer:decode_yaml(<<"a: FALSE\n">>)),

    %% Under the YAML 1.2 core schema (default), yes/no/on/off are plain
    %% strings, not booleans.
    ?_assertEqual(#{<<"a">> => <<"yes">>}, glazer:decode_yaml(<<"a: yes\n">>)),
    ?_assertEqual(#{<<"a">> => <<"no">>},  glazer:decode_yaml(<<"a: no\n">>)),
    ?_assertEqual(#{<<"a">> => <<"on">>},  glazer:decode_yaml(<<"a: on\n">>)),
    ?_assertEqual(#{<<"a">> => <<"off">>}, glazer:decode_yaml(<<"a: off\n">>)),

    %% With yaml_1_1_bools, yes/no/on/off (and case variants) are booleans.
    ?_assertEqual(#{<<"a">> => true},
                  glazer:decode_yaml(<<"a: yes\n">>, [yaml_1_1_bools])),
    ?_assertEqual(#{<<"a">> => false},
                  glazer:decode_yaml(<<"a: no\n">>, [yaml_1_1_bools])),
    ?_assertEqual(#{<<"a">> => true},
                  glazer:decode_yaml(<<"a: On\n">>, [yaml_1_1_bools])),
    ?_assertEqual(#{<<"a">> => false},
                  glazer:decode_yaml(<<"a: OFF\n">>, [yaml_1_1_bools])),

    %% Quoted yes/no always stay strings, even with yaml_1_1_bools.
    ?_assertEqual(#{<<"a">> => <<"yes">>},
                  glazer:decode_yaml(<<"a: 'yes'\n">>, [yaml_1_1_bools]))
  ].

number_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 123},  glazer:decode_yaml(<<"a: 123\n">>)),
    ?_assertEqual(#{<<"a">> => -7},   glazer:decode_yaml(<<"a: -7\n">>)),
    ?_assertEqual(#{<<"a">> => 5},    glazer:decode_yaml(<<"a: +5\n">>)),
    ?_assertEqual(#{<<"a">> => 1.5},  glazer:decode_yaml(<<"a: 1.5\n">>)),
    ?_assertEqual(#{<<"a">> => 31},   glazer:decode_yaml(<<"a: 0x1F\n">>)),
    ?_assertEqual(#{<<"a">> => 15},   glazer:decode_yaml(<<"a: 0o17\n">>)),

    %% Integers beyond int64/uint64 range decode as bigints.
    ?_assertEqual(#{<<"a">> => 123456789012345678901234567890},
                  glazer:decode_yaml(<<"a: 123456789012345678901234567890\n">>)),

    %% Quoted numbers stay strings (no implicit typing).
    ?_assertEqual(#{<<"a">> => <<"123">>}, glazer:decode_yaml(<<"a: '123'\n">>))
  ].

%% Erlang floats can't represent infinity/NaN, so .inf/-.inf/.nan decode as
%% atoms rather than enif_make_double (which would raise badarg).
special_float_test_() ->
  [
    ?_assertEqual(#{<<"a">> => infinity},     glazer:decode_yaml(<<"a: .inf\n">>)),
    ?_assertEqual(#{<<"a">> => infinity},     glazer:decode_yaml(<<"a: +.inf\n">>)),
    ?_assertEqual(#{<<"a">> => neg_infinity}, glazer:decode_yaml(<<"a: -.inf\n">>)),
    ?_assertEqual(#{<<"a">> => nan},          glazer:decode_yaml(<<"a: .nan\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Decode options
%% ----------------------------------------------------------------------------

decode_opts_test_() ->
  [
    ?_assertEqual(#{<<"a">> => nil},
                  glazer:decode_yaml(<<"a: null\n">>, [use_nil])),
    ?_assertEqual(#{<<"a">> => my_nil},
                  glazer:decode_yaml(<<"a: null\n">>, [{null_term, my_nil}])),
    ?_assertEqual(#{a => 1},
                  glazer:decode_yaml(<<"a: 1\n">>, [{keys, atom}])),
    ?_assertEqual(#{a => 1},
                  glazer:decode_yaml(<<"a: 1\n">>, [{keys, existing_atom}])),
    ?_assertEqual(#{<<"a">> => 1},
                  glazer:decode_yaml(<<"a: 1\n">>, [{keys, binary}]))
  ].

%% ----------------------------------------------------------------------------
%% try_decode_yaml/1,2 — {ok, Term} | {error, {parse_error, _}}
%% ----------------------------------------------------------------------------

try_decode_test_() ->
  [
    ?_assertEqual({ok, #{<<"a">> => 1}}, glazer:try_decode_yaml(<<"a: 1\n">>)),
    ?_assertEqual({ok, null},            glazer:try_decode_yaml(<<"">>)),
    ?_assertEqual({ok, #{<<"a">> => nil}},
                  glazer:try_decode_yaml(<<"a: null\n">>, [use_nil]))
  ].

%% ----------------------------------------------------------------------------
%% Section 3.2/3.3 line-folding rules
%% ----------------------------------------------------------------------------

folding_test_() ->
  [
    %% A single line break between non-empty lines folds to a space.
    ?_assertEqual(#{<<"a">> => <<"foo bar">>},
                  glazer:decode_yaml(<<"a: foo\n  bar\n">>)),

    %% Two or more consecutive line breaks fold to (N-1) literal newlines.
    ?_assertEqual(#{<<"a">> => <<"foo\nbar">>},
                  glazer:decode_yaml(<<"a: foo\n\n  bar\n">>)),

    %% Same folding rules apply to double-quoted scalars.
    ?_assertEqual(#{<<"a">> => <<"foo bar">>},
                  glazer:decode_yaml(<<"a: \"foo\n  bar\"\n">>)),
    ?_assertEqual(#{<<"a">> => <<"foo\nbar">>},
                  glazer:decode_yaml(<<"a: \"foo\n\n  bar\"\n">>)),

    %% And to single-quoted scalars.
    ?_assertEqual(#{<<"a">> => <<"foo bar">>},
                  glazer:decode_yaml(<<"a: 'foo\n  bar'\n">>)),
    ?_assertEqual(#{<<"a">> => <<"foo\nbar">>},
                  glazer:decode_yaml(<<"a: 'foo\n\n  bar'\n">>)),

    %% A double-quoted trailing backslash escapes the following line break
    %% (line continuation): the break and leading whitespace are removed
    %% entirely, with no fold.
    ?_assertEqual(#{<<"a">> => <<"foobar">>},
                  glazer:decode_yaml(<<"a: \"foo\\\n  bar\"\n">>)),

    %% Leading/trailing break runs in quoted scalars fold by the same rule
    %% (1 break -> space, N breaks -> N-1 newlines).
    ?_assertEqual(#{<<"a">> => <<"x ">>},
                  glazer:decode_yaml(<<"a: \"x\n\"\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer:decode_yaml(<<"a: \"x\n\n\"\n">>)),
    ?_assertEqual(#{<<"a">> => <<" x">>},
                  glazer:decode_yaml(<<"a: \"\nx\"\n">>)),

    %% Trailing spaces inside quotes are content, not fold whitespace.
    ?_assertEqual(#{<<"a">> => <<"x  ">>},
                  glazer:decode_yaml(<<"a: 'x  '\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x  ">>},
                  glazer:decode_yaml(<<"a: \"x  \"\n">>)),

    %% Trailing blank lines are not part of a plain scalar.
    ?_assertEqual(#{<<"a">> => <<"foo">>, <<"b">> => 1},
                  glazer:decode_yaml(<<"a: foo\n\nb: 1\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Flow style ({...} mappings, [...] sequences)
%% ----------------------------------------------------------------------------

flow_test_() ->
  [
    ?_assertEqual([1, 2, 3],          glazer:decode_yaml(<<"[1, 2, 3]">>)),
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 2},
                  glazer:decode_yaml(<<"{a: 1, b: 2}">>)),
    ?_assertEqual([],                 glazer:decode_yaml(<<"[]">>)),
    ?_assertEqual(#{},                glazer:decode_yaml(<<"{}">>)),

    %% Flow nested inside block constructs.
    ?_assertEqual(#{<<"tags">> => [<<"a">>, <<"b">>, <<"c">>]},
                  glazer:decode_yaml(<<"tags: [a, b, c]\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"x">> => 1, <<"y">> => [2, 3]}},
                  glazer:decode_yaml(<<"a: {x: 1, y: [2, 3]}\n">>)),
    ?_assertEqual(#{<<"key">> => [[1, 2], #{<<"a">> => 1}]},
                  glazer:decode_yaml(<<"key:\n  - [1, 2]\n  - {a: 1}\n">>)),
    %% Flow collection on its own line as a mapping value.
    ?_assertEqual(#{<<"a">> => [1, 2]},
                  glazer:decode_yaml(<<"a:\n  [1, 2]\n">>)),

    %% Flow nested inside flow.
    ?_assertEqual([<<"a">>, [<<"b">>, <<"c">>], #{<<"d">> => 1}],
                  glazer:decode_yaml(<<"[a, [b, c], {d: 1}]">>)),

    %% Trailing commas are allowed.
    ?_assertEqual([1, 2],             glazer:decode_yaml(<<"[1, 2,]">>)),
    ?_assertEqual(#{<<"a">> => 1},    glazer:decode_yaml(<<"{a: 1,}">>)),

    %% Flow collections may span lines; comments are allowed inside.
    ?_assertEqual(#{<<"tags">> => [<<"a">>, <<"b">>]},
                  glazer:decode_yaml(<<"tags: [\n  a,\n  b,\n]\n">>)),
    ?_assertEqual([1, 2],             glazer:decode_yaml(<<"[1, # one\n 2]\n">>)),

    %% Single-pair mapping shorthand inside a flow sequence.
    ?_assertEqual([#{<<"a">> => 1}, #{<<"b">> => 2}],
                  glazer:decode_yaml(<<"[a: 1, b: 2]">>)),

    %% Keys without values get null; explicit empty value too.
    ?_assertEqual(#{<<"a">> => null, <<"b">> => null},
                  glazer:decode_yaml(<<"{a, b}">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer:decode_yaml(<<"{a: }">>)),

    %% A non-spaced colon stays inside the plain scalar (YAML 1.2).
    ?_assertEqual([<<"a:1">>],        glazer:decode_yaml(<<"[a:1]">>)),

    %% Quoted scalars in flow; flow indicators inside quotes are content.
    ?_assertEqual([<<"a, b">>, <<"c">>],
                  glazer:decode_yaml(<<"['a, b', \"c\"]">>)),

    %% Implicit typing applies to flow plain scalars.
    ?_assertEqual([1, 1.5, true, null, infinity],
                  glazer:decode_yaml(<<"[1, 1.5, true, null, .inf]">>)),

    %% Decode options apply inside flow collections.
    ?_assertEqual(#{a => 1},
                  glazer:decode_yaml(<<"{a: 1}">>, [{keys, atom}])),
    ?_assertEqual(#{<<"a">> => nil},
                  glazer:decode_yaml(<<"{a: null}">>, [use_nil]))
  ].

%% ----------------------------------------------------------------------------
%% Block scalars (| literal, > folded) with chomping
%% ----------------------------------------------------------------------------

literal_scalar_test_() ->
  [
    %% Chomping: clip (default) keeps one trailing newline, strip (-) drops
    %% all, keep (+) retains all.
    ?_assertEqual(#{<<"a">> => <<"x\ny\n">>},
                  glazer:decode_yaml(<<"a: |\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\ny">>},
                  glazer:decode_yaml(<<"a: |-\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\ny\n">>},
                  glazer:decode_yaml(<<"a: |+\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer:decode_yaml(<<"a: |\n  x\n\n\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>},
                  glazer:decode_yaml(<<"a: |-\n  x\n\n\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n\n\n">>, <<"b">> => 1},
                  glazer:decode_yaml(<<"a: |+\n  x\n\n\nb: 1">>)),

    %% No trailing newline at EOF: nothing to clip/keep.
    ?_assertEqual(#{<<"a">> => <<"x">>}, glazer:decode_yaml(<<"a: |\n  x">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>}, glazer:decode_yaml(<<"a: |-\n  x">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>}, glazer:decode_yaml(<<"a: |+\n  x">>)),

    %% Explicit indentation indicator (relative to the parent), in either
    %% order with the chomping indicator.
    ?_assertEqual(#{<<"a">> => <<"  x\n">>}, glazer:decode_yaml(<<"a: |2\n    x\n">>)),
    ?_assertEqual(#{<<"a">> => <<" x\n">>},  glazer:decode_yaml(<<"a: |1\n  x\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>},     glazer:decode_yaml(<<"a: |2-\n  x\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>},     glazer:decode_yaml(<<"a: |-2\n  x\n">>)),

    %% Verbatim details: trailing spaces kept, blank lines with whitespace
    %% beyond the indent keep the excess, leading blank lines are newlines.
    ?_assertEqual(#{<<"a">> => <<"x \ny\n">>},
                  glazer:decode_yaml(<<"a: |\n  x \n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"a\n \nb\n">>},
                  glazer:decode_yaml(<<"a: |\n  a\n   \n  b\n">>)),
    ?_assertEqual(#{<<"a">> => <<"\nx\n">>},
                  glazer:decode_yaml(<<"a: |\n\n  x\n">>)),

    %% Empty block scalars.
    ?_assertEqual(#{<<"a">> => <<>>, <<"b">> => 1},
                  glazer:decode_yaml(<<"a: |\nb: 1">>)),
    ?_assertEqual(#{<<"a">> => <<"\n">>, <<"b">> => 1},
                  glazer:decode_yaml(<<"a: |+\n\nb: 1">>)),

    %% Block scalars as sequence items (content bound is the dash column),
    %% header on its own line, header comments, top-level documents.
    ?_assertEqual([<<"x\n">>, <<"y">>],
                  glazer:decode_yaml(<<"- |\n  x\n- y\n">>)),
    ?_assertEqual([<<"x\n">>, <<"y">>],
                  glazer:decode_yaml(<<"- |\n x\n- y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer:decode_yaml(<<"a:\n  |\n  x\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer:decode_yaml(<<"a: | # comment\n  x\n">>)),
    ?_assertEqual(<<"x\n">>, glazer:decode_yaml(<<"|\n x\n">>)),
    ?_assertEqual(<<"x\n">>, glazer:decode_yaml(<<"|2\n  x\n">>)),

    %% Junk after the header is an error.
    ?_assertMatch({error, {parse_error, _}},
                  glazer:try_decode_yaml(<<"a: |junk\n  x\n">>))
  ].

folded_scalar_test_() ->
  [
    %% Chomp modes.
    ?_assertEqual(#{<<"a">> => <<"x y\n">>},
                  glazer:decode_yaml(<<"a: >\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x y">>},
                  glazer:decode_yaml(<<"a: >-\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x y\n\n">>, <<"b">> => 1},
                  glazer:decode_yaml(<<"a: >+\n  x\n  y\n\nb: 1">>)),

    %% Empty lines fold to newlines.
    ?_assertEqual(#{<<"a">> => <<"x\ny\n">>},
                  glazer:decode_yaml(<<"a: >\n  x\n\n  y\n">>)),

    %% More-indented lines are not folded: their extra indentation is
    %% content and the breaks around them stay literal.
    ?_assertEqual(#{<<"a">> => <<"x\n  b1\n  b2\ny\n">>},
                  glazer:decode_yaml(<<"a: >\n  x\n    b1\n    b2\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n\n  b1\n\ny\n">>},
                  glazer:decode_yaml(<<"a: >\n  x\n\n    b1\n\n  y\n">>)),

    %% Trailing spaces on a line are kept; the fold space is added after.
    ?_assertEqual(#{<<"a">> => <<"a  b\n">>},
                  glazer:decode_yaml(<<"a: >\n  a \n  b\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Anchors and aliases (&name / *name)
%% ----------------------------------------------------------------------------

anchor_alias_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 1},
                  glazer:decode_yaml(<<"a: &x 1\nb: *x\n">>)),
    %% Anchored collections (inline flow, nested block) are shared.
    ?_assertEqual(#{<<"a">> => #{<<"k">> => 1}, <<"b">> => #{<<"k">> => 1}},
                  glazer:decode_yaml(<<"a: &x {k: 1}\nb: *x\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"k">> => 1}, <<"b">> => #{<<"k">> => 1}},
                  glazer:decode_yaml(<<"a: &x\n  k: 1\nb: *x\n">>)),
    %% Anchors in sequences and inside flow collections.
    ?_assertEqual([<<"foo">>, <<"foo">>],
                  glazer:decode_yaml(<<"- &x foo\n- *x\n">>)),
    ?_assertEqual(#{<<"a">> => [5, 5]},
                  glazer:decode_yaml(<<"a: [&y 5, *y]\n">>)),
    %% Anchored block scalar.
    ?_assertEqual(#{<<"a">> => <<"txt\n">>, <<"b">> => <<"txt\n">>},
                  glazer:decode_yaml(<<"a: &x |\n  txt\nb: *x\n">>)),
    %% Re-binding an anchor is allowed per YAML 1.2: last binding wins.
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 2, <<"c">> => 2},
                  glazer:decode_yaml(<<"a: &x 1\nb: &x 2\nc: *x\n">>)),

    %% Undefined aliases (including cycles, whose anchor is not yet
    %% registered when the alias is reached) are errors.
    ?_assertMatch({error, {parse_error, _}},
                  glazer:try_decode_yaml(<<"a: *nope\n">>)),
    ?_assertMatch({error, {parse_error, _}},
                  glazer:try_decode_yaml(<<"a: &x [*x]\n">>)),
    %% Root-node anchors are rejected (cannot be referenced in a single
    %% document) rather than mis-parsed as scalars.
    ?_assertMatch({error, {parse_error, _}},
                  glazer:try_decode_yaml(<<"&x\na: 1\n">>)),
    ?_assertMatch({error, {parse_error, _}},
                  glazer:try_decode_yaml(<<"a: &\n">>))
  ].

%% Sequence-item plain scalars may continue on lines indented past the dash.
seq_item_continuation_test_() ->
  [
    ?_assertEqual([<<"foo bar">>],   glazer:decode_yaml(<<"- foo\n  bar\n">>)),
    ?_assertEqual([<<"foo - bar">>], glazer:decode_yaml(<<"- foo\n  - bar\n">>)),
    ?_assertEqual([#{<<"a">> => 1}], glazer:decode_yaml(<<"-\n a: 1\n">>))
  ].

flow_error_test_() ->
  [
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"[1, 2">>)),
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"{a: 1">>)),
    %% Block constructs are not allowed inside flow collections.
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"[- 1]">>)),
    %% Empty entries are invalid.
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"[,1]">>)),
    %% Trailing junk after an inline flow collection.
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"a: [1] junk\n">>)),
    %% Flow-collection (complex) keys are not supported.
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"[x]: 1\n">>)),
    ?_assertMatch({error, {parse_error, _}}, glazer:try_decode_yaml(<<"{[x]: 1}">>))
  ].
