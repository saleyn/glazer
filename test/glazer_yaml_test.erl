-module(glazer_yaml_test).
-include_lib("eunit/include/eunit.hrl").

%% ----------------------------------------------------------------------------
%% Basic mappings and sequences
%% ----------------------------------------------------------------------------

mapping_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 2},
                  glazer_yaml:decode(<<"a: 1\nb: 2\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"b">> => 1, <<"c">> => 2}},
                  glazer_yaml:decode(<<"a:\n  b: 1\n  c: 2\n">>)),
    ?_assertEqual(#{<<"nested">> => #{<<"a">> => #{<<"b">> => 1}}},
                  glazer_yaml:decode(<<"nested:\n  a:\n    b: 1\n">>)),
    ?_assertEqual(#{<<"a">> => null},
                  glazer_yaml:decode(<<"a:\n">>)),
    ?_assertEqual(#{<<"empty">> => null, <<"after">> => 1},
                  glazer_yaml:decode(<<"empty:\nafter: 1\n">>))
  ].

sequence_test_() ->
  [
    ?_assertEqual([1, 2, 3],
                  glazer_yaml:decode(<<"- 1\n- 2\n- 3\n">>)),
    ?_assertEqual(#{<<"list">> => [<<"a">>, <<"b">>]},
                  glazer_yaml:decode(<<"list:\n  - a\n  - b\n">>)),
    ?_assertEqual(#{<<"a">> => [1, 2]},
                  glazer_yaml:decode(<<"a:\n  - 1\n  - 2\n">>)),
    %% A block sequence may sit at the same indentation as the mapping key
    %% that owns it (common PyYAML/libyaml output style).
    ?_assertEqual(#{<<"list">> => [<<"a">>, <<"b">>]},
                  glazer_yaml:decode(<<"list:\n- a\n- b\n">>)),
    ?_assertEqual(#{<<"a">> => [1, 2], <<"b">> => 3},
                  glazer_yaml:decode(<<"a:\n- 1\n- 2\nb: 3\n">>)),
    ?_assertEqual(#{<<"outer">> => #{<<"a">> => [1, 2], <<"b">> => 3}},
                  glazer_yaml:decode(<<"outer:\n  a:\n  - 1\n  - 2\n  b: 3\n">>)),
    ?_assertEqual(#{<<"top">> => #{<<"a">> => [#{<<"id">> => <<"x">>, <<"w">> => 1}]}},
                  glazer_yaml:decode(<<"top:\n  a:\n  - id: x\n    w: 1\n">>))
  ].

%% Sequences whose items are themselves mappings/sequences, inline after "- ".
nested_dash_test_() ->
  [
    ?_assertEqual([#{<<"a">> => 1, <<"b">> => 2}, #{<<"a">> => 3, <<"b">> => 4}],
                  glazer_yaml:decode(<<"- a: 1\n  b: 2\n- a: 3\n  b: 4\n">>)),
    ?_assertEqual([[1, 2], 3],
                  glazer_yaml:decode(<<"- - 1\n  - 2\n- 3\n">>)),
    ?_assertEqual([[[1, 2], 3]],
                  glazer_yaml:decode(<<"- - - 1\n    - 2\n  - 3\n">>)),
    ?_assertEqual([#{<<"a">> => #{<<"b">> => 1, <<"c">> => 2}, <<"d">> => 3}],
                  glazer_yaml:decode(<<"- a:\n    b: 1\n    c: 2\n  d: 3\n">>)),
    ?_assertEqual([#{<<"a">> => 1, <<"b">> => [<<"x">>, <<"y">>]}, #{<<"c">> => 2}],
                  glazer_yaml:decode(<<"- a: 1\n  b:\n    - x\n    - y\n- c: 2\n">>)),
    ?_assertEqual(#{<<"top">> => [#{<<"k">> => 1, <<"v">> => 2}, #{<<"k">> => 3, <<"v">> => 4}]},
                  glazer_yaml:decode(<<"top:\n  - k: 1\n    v: 2\n  - k: 3\n    v: 4\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Plain scalars
%% ----------------------------------------------------------------------------

plain_scalar_test_() ->
  [
    ?_assertEqual(<<"plain text">>, glazer_yaml:decode(<<"plain text\n">>)),
    %% multi-line plain scalars fold a single line break to a space
    ?_assertEqual(#{<<"a">> => <<"line one line two">>, <<"b">> => 2},
                  glazer_yaml:decode(<<"a: line one\n  line two\nb: 2\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Quoted scalars
%% ----------------------------------------------------------------------------

quoted_scalar_test_() ->
  [
    %% single-quoted: '' escapes to a literal '
    ?_assertEqual(#{<<"name">> => <<"it's">>},
                  glazer_yaml:decode(<<"name: 'it''s'\n">>)),
    %% double-quoted: full escape table
    ?_assertEqual(#{<<"name">> => <<"line1\nline2">>},
                  glazer_yaml:decode(<<"name: \"line1\\nline2\"\n">>)),
    %% quoted scalars are never subject to implicit typing
    ?_assertEqual(#{<<"a">> => <<"yes">>},
                  glazer_yaml:decode(<<"a: 'yes'\n">>)),
    ?_assertEqual(#{<<"a">> => <<"123">>},
                  glazer_yaml:decode(<<"a: \"123\"\n">>)),
    %% quoted keys
    ?_assertEqual(#{<<"quoted key">> => 1},
                  glazer_yaml:decode(<<"\"quoted key\": 1\n">>)),
    ?_assertEqual(#{<<"single key">> => 1},
                  glazer_yaml:decode(<<"'single key': 1\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Comments and document markers
%% ----------------------------------------------------------------------------

comments_and_markers_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1},
                  glazer_yaml:decode(<<"# comment\na: 1\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"b">> => 1}},
                  glazer_yaml:decode(<<"a: # comment\n  b: 1\n">>)),
    ?_assertEqual(#{<<"a">> => 1},
                  glazer_yaml:decode(<<"---\na: 1\n">>)),
    ?_assertEqual(null,
                  glazer_yaml:decode(<<"   \n# just a comment\n">>)),
    ?_assertEqual(null,
                  glazer_yaml:decode(<<"">>))
  ].

%% ----------------------------------------------------------------------------
%% Implicit scalar typing — YAML 1.2 core schema (default)
%% ----------------------------------------------------------------------------

null_test_() ->
  [
    ?_assertEqual(#{<<"a">> => null}, glazer_yaml:decode(<<"a: null\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer_yaml:decode(<<"a: ~\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer_yaml:decode(<<"a: Null\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer_yaml:decode(<<"a: NULL\n">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer_yaml:decode(<<"a:\n">>))
  ].

bool_test_() ->
  [
    ?_assertEqual(#{<<"a">> => true},  glazer_yaml:decode(<<"a: true\n">>)),
    ?_assertEqual(#{<<"a">> => false}, glazer_yaml:decode(<<"a: false\n">>)),
    ?_assertEqual(#{<<"a">> => true},  glazer_yaml:decode(<<"a: True\n">>)),
    ?_assertEqual(#{<<"a">> => false}, glazer_yaml:decode(<<"a: FALSE\n">>)),

    %% Under the YAML 1.2 core schema (default), yes/no/on/off are plain
    %% strings, not booleans.
    ?_assertEqual(#{<<"a">> => <<"yes">>}, glazer_yaml:decode(<<"a: yes\n">>)),
    ?_assertEqual(#{<<"a">> => <<"no">>},  glazer_yaml:decode(<<"a: no\n">>)),
    ?_assertEqual(#{<<"a">> => <<"on">>},  glazer_yaml:decode(<<"a: on\n">>)),
    ?_assertEqual(#{<<"a">> => <<"off">>}, glazer_yaml:decode(<<"a: off\n">>)),

    %% With yaml_1_1_bools, yes/no/on/off (and case variants) are booleans.
    ?_assertEqual(#{<<"a">> => true},
                  glazer_yaml:decode(<<"a: yes\n">>, [yaml_1_1_bools])),
    ?_assertEqual(#{<<"a">> => false},
                  glazer_yaml:decode(<<"a: no\n">>, [yaml_1_1_bools])),
    ?_assertEqual(#{<<"a">> => true},
                  glazer_yaml:decode(<<"a: On\n">>, [yaml_1_1_bools])),
    ?_assertEqual(#{<<"a">> => false},
                  glazer_yaml:decode(<<"a: OFF\n">>, [yaml_1_1_bools])),

    %% Quoted yes/no always stay strings, even with yaml_1_1_bools.
    ?_assertEqual(#{<<"a">> => <<"yes">>},
                  glazer_yaml:decode(<<"a: 'yes'\n">>, [yaml_1_1_bools]))
  ].

number_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 123},  glazer_yaml:decode(<<"a: 123\n">>)),
    ?_assertEqual(#{<<"a">> => -7},   glazer_yaml:decode(<<"a: -7\n">>)),
    ?_assertEqual(#{<<"a">> => 5},    glazer_yaml:decode(<<"a: +5\n">>)),
    ?_assertEqual(#{<<"a">> => 1.5},  glazer_yaml:decode(<<"a: 1.5\n">>)),
    ?_assertEqual(#{<<"a">> => 31},   glazer_yaml:decode(<<"a: 0x1F\n">>)),
    ?_assertEqual(#{<<"a">> => 15},   glazer_yaml:decode(<<"a: 0o17\n">>)),

    %% Integers beyond int64/uint64 range decode as bigints.
    ?_assertEqual(#{<<"a">> => 123456789012345678901234567890},
                  glazer_yaml:decode(<<"a: 123456789012345678901234567890\n">>)),

    %% Quoted numbers stay strings (no implicit typing).
    ?_assertEqual(#{<<"a">> => <<"123">>}, glazer_yaml:decode(<<"a: '123'\n">>))
  ].

%% Erlang floats can't represent infinity/NaN, so .inf/-.inf/.nan decode as
%% atoms rather than enif_make_double (which would raise badarg).
special_float_test_() ->
  [
    ?_assertEqual(#{<<"a">> => infinity},     glazer_yaml:decode(<<"a: .inf\n">>)),
    ?_assertEqual(#{<<"a">> => infinity},     glazer_yaml:decode(<<"a: +.inf\n">>)),
    ?_assertEqual(#{<<"a">> => neg_infinity}, glazer_yaml:decode(<<"a: -.inf\n">>)),
    ?_assertEqual(#{<<"a">> => nan},          glazer_yaml:decode(<<"a: .nan\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Decode options
%% ----------------------------------------------------------------------------

decode_opts_test_() ->
  [
    ?_assertEqual(#{<<"a">> => nil},
                  glazer_yaml:decode(<<"a: null\n">>, [use_nil])),
    ?_assertEqual(#{<<"a">> => my_nil},
                  glazer_yaml:decode(<<"a: null\n">>, [{null_term, my_nil}])),
    ?_assertEqual(#{a => 1},
                  glazer_yaml:decode(<<"a: 1\n">>, [{keys, atom}])),
    ?_assertEqual(#{a => 1},
                  glazer_yaml:decode(<<"a: 1\n">>, [{keys, existing_atom}])),
    ?_assertEqual(#{<<"a">> => 1},
                  glazer_yaml:decode(<<"a: 1\n">>, [{keys, binary}]))
  ].

%% ----------------------------------------------------------------------------
%% yaml_try_decode/1,2 — {ok, Term} | {error, _}
%% ----------------------------------------------------------------------------

try_decode_test_() ->
  [
    ?_assertEqual({ok, #{<<"a">> => 1}}, glazer_yaml:try_decode(<<"a: 1\n">>)),
    ?_assertEqual({ok, null},            glazer_yaml:try_decode(<<"">>)),
    ?_assertEqual({ok, #{<<"a">> => nil}},
                  glazer_yaml:try_decode(<<"a: null\n">>, [use_nil]))
  ].

%% ----------------------------------------------------------------------------
%% Section 3.2/3.3 line-folding rules
%% ----------------------------------------------------------------------------

folding_test_() ->
  [
    %% A single line break between non-empty lines folds to a space.
    ?_assertEqual(#{<<"a">> => <<"foo bar">>},
                  glazer_yaml:decode(<<"a: foo\n  bar\n">>)),

    %% Two or more consecutive line breaks fold to (N-1) literal newlines.
    ?_assertEqual(#{<<"a">> => <<"foo\nbar">>},
                  glazer_yaml:decode(<<"a: foo\n\n  bar\n">>)),

    %% Same folding rules apply to double-quoted scalars.
    ?_assertEqual(#{<<"a">> => <<"foo bar">>},
                  glazer_yaml:decode(<<"a: \"foo\n  bar\"\n">>)),
    ?_assertEqual(#{<<"a">> => <<"foo\nbar">>},
                  glazer_yaml:decode(<<"a: \"foo\n\n  bar\"\n">>)),

    %% And to single-quoted scalars.
    ?_assertEqual(#{<<"a">> => <<"foo bar">>},
                  glazer_yaml:decode(<<"a: 'foo\n  bar'\n">>)),
    ?_assertEqual(#{<<"a">> => <<"foo\nbar">>},
                  glazer_yaml:decode(<<"a: 'foo\n\n  bar'\n">>)),

    %% A double-quoted trailing backslash escapes the following line break
    %% (line continuation): the break and leading whitespace are removed
    %% entirely, with no fold.
    ?_assertEqual(#{<<"a">> => <<"foobar">>},
                  glazer_yaml:decode(<<"a: \"foo\\\n  bar\"\n">>)),

    %% Leading/trailing break runs in quoted scalars fold by the same rule
    %% (1 break -> space, N breaks -> N-1 newlines).
    ?_assertEqual(#{<<"a">> => <<"x ">>},
                  glazer_yaml:decode(<<"a: \"x\n\"\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer_yaml:decode(<<"a: \"x\n\n\"\n">>)),
    ?_assertEqual(#{<<"a">> => <<" x">>},
                  glazer_yaml:decode(<<"a: \"\nx\"\n">>)),

    %% Trailing spaces inside quotes are content, not fold whitespace.
    ?_assertEqual(#{<<"a">> => <<"x  ">>},
                  glazer_yaml:decode(<<"a: 'x  '\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x  ">>},
                  glazer_yaml:decode(<<"a: \"x  \"\n">>)),

    %% Trailing blank lines are not part of a plain scalar.
    ?_assertEqual(#{<<"a">> => <<"foo">>, <<"b">> => 1},
                  glazer_yaml:decode(<<"a: foo\n\nb: 1\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Flow style ({...} mappings, [...] sequences)
%% ----------------------------------------------------------------------------

flow_test_() ->
  [
    ?_assertEqual([1, 2, 3],          glazer_yaml:decode(<<"[1, 2, 3]">>)),
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 2},
                  glazer_yaml:decode(<<"{a: 1, b: 2}">>)),
    ?_assertEqual([],                 glazer_yaml:decode(<<"[]">>)),
    ?_assertEqual(#{},                glazer_yaml:decode(<<"{}">>)),

    %% Flow nested inside block constructs.
    ?_assertEqual(#{<<"tags">> => [<<"a">>, <<"b">>, <<"c">>]},
                  glazer_yaml:decode(<<"tags: [a, b, c]\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"x">> => 1, <<"y">> => [2, 3]}},
                  glazer_yaml:decode(<<"a: {x: 1, y: [2, 3]}\n">>)),
    ?_assertEqual(#{<<"key">> => [[1, 2], #{<<"a">> => 1}]},
                  glazer_yaml:decode(<<"key:\n  - [1, 2]\n  - {a: 1}\n">>)),
    %% Flow collection on its own line as a mapping value.
    ?_assertEqual(#{<<"a">> => [1, 2]},
                  glazer_yaml:decode(<<"a:\n  [1, 2]\n">>)),

    %% Flow nested inside flow.
    ?_assertEqual([<<"a">>, [<<"b">>, <<"c">>], #{<<"d">> => 1}],
                  glazer_yaml:decode(<<"[a, [b, c], {d: 1}]">>)),

    %% Trailing commas are allowed.
    ?_assertEqual([1, 2],             glazer_yaml:decode(<<"[1, 2,]">>)),
    ?_assertEqual(#{<<"a">> => 1},    glazer_yaml:decode(<<"{a: 1,}">>)),

    %% Flow collections may span lines; comments are allowed inside.
    ?_assertEqual(#{<<"tags">> => [<<"a">>, <<"b">>]},
                  glazer_yaml:decode(<<"tags: [\n  a,\n  b,\n]\n">>)),
    ?_assertEqual([1, 2],             glazer_yaml:decode(<<"[1, # one\n 2]\n">>)),

    %% Single-pair mapping shorthand inside a flow sequence.
    ?_assertEqual([#{<<"a">> => 1}, #{<<"b">> => 2}],
                  glazer_yaml:decode(<<"[a: 1, b: 2]">>)),

    %% Keys without values get null; explicit empty value too.
    ?_assertEqual(#{<<"a">> => null, <<"b">> => null},
                  glazer_yaml:decode(<<"{a, b}">>)),
    ?_assertEqual(#{<<"a">> => null}, glazer_yaml:decode(<<"{a: }">>)),

    %% A non-spaced colon stays inside the plain scalar (YAML 1.2).
    ?_assertEqual([<<"a:1">>],        glazer_yaml:decode(<<"[a:1]">>)),

    %% Quoted scalars in flow; flow indicators inside quotes are content.
    ?_assertEqual([<<"a, b">>, <<"c">>],
                  glazer_yaml:decode(<<"['a, b', \"c\"]">>)),

    %% Implicit typing applies to flow plain scalars.
    ?_assertEqual([1, 1.5, true, null, infinity],
                  glazer_yaml:decode(<<"[1, 1.5, true, null, .inf]">>)),

    %% Decode options apply inside flow collections.
    ?_assertEqual(#{a => 1},
                  glazer_yaml:decode(<<"{a: 1}">>, [{keys, atom}])),
    ?_assertEqual(#{<<"a">> => nil},
                  glazer_yaml:decode(<<"{a: null}">>, [use_nil]))
  ].

%% ----------------------------------------------------------------------------
%% Block scalars (| literal, > folded) with chomping
%% ----------------------------------------------------------------------------

literal_scalar_test_() ->
  [
    %% Chomping: clip (default) keeps one trailing newline, strip (-) drops
    %% all, keep (+) retains all.
    ?_assertEqual(#{<<"a">> => <<"x\ny\n">>},
                  glazer_yaml:decode(<<"a: |\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\ny">>},
                  glazer_yaml:decode(<<"a: |-\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\ny\n">>},
                  glazer_yaml:decode(<<"a: |+\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer_yaml:decode(<<"a: |\n  x\n\n\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>},
                  glazer_yaml:decode(<<"a: |-\n  x\n\n\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n\n\n">>, <<"b">> => 1},
                  glazer_yaml:decode(<<"a: |+\n  x\n\n\nb: 1">>)),

    %% No trailing newline at EOF: nothing to clip/keep.
    ?_assertEqual(#{<<"a">> => <<"x">>}, glazer_yaml:decode(<<"a: |\n  x">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>}, glazer_yaml:decode(<<"a: |-\n  x">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>}, glazer_yaml:decode(<<"a: |+\n  x">>)),

    %% Explicit indentation indicator (relative to the parent), in either
    %% order with the chomping indicator.
    ?_assertEqual(#{<<"a">> => <<"  x\n">>}, glazer_yaml:decode(<<"a: |2\n    x\n">>)),
    ?_assertEqual(#{<<"a">> => <<" x\n">>},  glazer_yaml:decode(<<"a: |1\n  x\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>},     glazer_yaml:decode(<<"a: |2-\n  x\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x">>},     glazer_yaml:decode(<<"a: |-2\n  x\n">>)),

    %% Verbatim details: trailing spaces kept, blank lines with whitespace
    %% beyond the indent keep the excess, leading blank lines are newlines.
    ?_assertEqual(#{<<"a">> => <<"x \ny\n">>},
                  glazer_yaml:decode(<<"a: |\n  x \n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"a\n \nb\n">>},
                  glazer_yaml:decode(<<"a: |\n  a\n   \n  b\n">>)),
    ?_assertEqual(#{<<"a">> => <<"\nx\n">>},
                  glazer_yaml:decode(<<"a: |\n\n  x\n">>)),

    %% Empty block scalars.
    ?_assertEqual(#{<<"a">> => <<>>, <<"b">> => 1},
                  glazer_yaml:decode(<<"a: |\nb: 1">>)),
    ?_assertEqual(#{<<"a">> => <<"\n">>, <<"b">> => 1},
                  glazer_yaml:decode(<<"a: |+\n\nb: 1">>)),

    %% Block scalars as sequence items (content bound is the dash column),
    %% header on its own line, header comments, top-level documents.
    ?_assertEqual([<<"x\n">>, <<"y">>],
                  glazer_yaml:decode(<<"- |\n  x\n- y\n">>)),
    ?_assertEqual([<<"x\n">>, <<"y">>],
                  glazer_yaml:decode(<<"- |\n x\n- y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer_yaml:decode(<<"a:\n  |\n  x\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n">>},
                  glazer_yaml:decode(<<"a: | # comment\n  x\n">>)),
    ?_assertEqual(<<"x\n">>, glazer_yaml:decode(<<"|\n x\n">>)),
    ?_assertEqual(<<"x\n">>, glazer_yaml:decode(<<"|2\n  x\n">>)),

    %% Junk after the header is an error.
    ?_assertMatch({error, _},
                  glazer_yaml:try_decode(<<"a: |junk\n  x\n">>))
  ].

folded_scalar_test_() ->
  [
    %% Chomp modes.
    ?_assertEqual(#{<<"a">> => <<"x y\n">>},
                  glazer_yaml:decode(<<"a: >\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x y">>},
                  glazer_yaml:decode(<<"a: >-\n  x\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x y\n\n">>, <<"b">> => 1},
                  glazer_yaml:decode(<<"a: >+\n  x\n  y\n\nb: 1">>)),

    %% Empty lines fold to newlines.
    ?_assertEqual(#{<<"a">> => <<"x\ny\n">>},
                  glazer_yaml:decode(<<"a: >\n  x\n\n  y\n">>)),

    %% More-indented lines are not folded: their extra indentation is
    %% content and the breaks around them stay literal.
    ?_assertEqual(#{<<"a">> => <<"x\n  b1\n  b2\ny\n">>},
                  glazer_yaml:decode(<<"a: >\n  x\n    b1\n    b2\n  y\n">>)),
    ?_assertEqual(#{<<"a">> => <<"x\n\n  b1\n\ny\n">>},
                  glazer_yaml:decode(<<"a: >\n  x\n\n    b1\n\n  y\n">>)),

    %% Trailing spaces on a line are kept; the fold space is added after.
    ?_assertEqual(#{<<"a">> => <<"a  b\n">>},
                  glazer_yaml:decode(<<"a: >\n  a \n  b\n">>))
  ].

%% ----------------------------------------------------------------------------
%% Anchors and aliases (&name / *name)
%% ----------------------------------------------------------------------------

anchor_alias_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 1},
                  glazer_yaml:decode(<<"a: &x 1\nb: *x\n">>)),
    %% Anchored collections (inline flow, nested block) are shared.
    ?_assertEqual(#{<<"a">> => #{<<"k">> => 1}, <<"b">> => #{<<"k">> => 1}},
                  glazer_yaml:decode(<<"a: &x {k: 1}\nb: *x\n">>)),
    ?_assertEqual(#{<<"a">> => #{<<"k">> => 1}, <<"b">> => #{<<"k">> => 1}},
                  glazer_yaml:decode(<<"a: &x\n  k: 1\nb: *x\n">>)),
    %% Anchors in sequences and inside flow collections.
    ?_assertEqual([<<"foo">>, <<"foo">>],
                  glazer_yaml:decode(<<"- &x foo\n- *x\n">>)),
    ?_assertEqual(#{<<"a">> => [5, 5]},
                  glazer_yaml:decode(<<"a: [&y 5, *y]\n">>)),
    %% Anchored block scalar.
    ?_assertEqual(#{<<"a">> => <<"txt\n">>, <<"b">> => <<"txt\n">>},
                  glazer_yaml:decode(<<"a: &x |\n  txt\nb: *x\n">>)),
    %% Re-binding an anchor is allowed per YAML 1.2: last binding wins.
    ?_assertEqual(#{<<"a">> => 1, <<"b">> => 2, <<"c">> => 2},
                  glazer_yaml:decode(<<"a: &x 1\nb: &x 2\nc: *x\n">>)),

    %% Undefined aliases (including cycles, whose anchor is not yet
    %% registered when the alias is reached) are errors.
    ?_assertMatch({error, _},
                  glazer_yaml:try_decode(<<"a: *nope\n">>)),
    ?_assertMatch({error, _},
                  glazer_yaml:try_decode(<<"a: &x [*x]\n">>)),
    %% Root-node anchors are rejected (cannot be referenced in a single
    %% document) rather than mis-parsed as scalars.
    ?_assertMatch({error, _},
                  glazer_yaml:try_decode(<<"&x\na: 1\n">>)),
    ?_assertMatch({error, _},
                  glazer_yaml:try_decode(<<"a: &\n">>))
  ].

%% Sequence-item plain scalars may continue on lines indented past the dash.
seq_item_continuation_test_() ->
  [
    ?_assertEqual([<<"foo bar">>],   glazer_yaml:decode(<<"- foo\n  bar\n">>)),
    ?_assertEqual([<<"foo - bar">>], glazer_yaml:decode(<<"- foo\n  - bar\n">>)),
    ?_assertEqual([#{<<"a">> => 1}], glazer_yaml:decode(<<"-\n a: 1\n">>))
  ].

flow_error_test_() ->
  [
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"[1, 2">>)),
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"{a: 1">>)),
    %% Block constructs are not allowed inside flow collections.
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"[- 1]">>)),
    %% Empty entries are invalid.
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"[,1]">>)),
    %% Trailing junk after an inline flow collection.
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"a: [1] junk\n">>)),
    %% Flow-collection (complex) keys are not supported.
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"[x]: 1\n">>)),
    ?_assertMatch({error, _}, glazer_yaml:try_decode(<<"{[x]: 1}">>))
  ].

%% ----------------------------------------------------------------------------
%% yaml_encode/1,2
%% ----------------------------------------------------------------------------

encode_scalar_test_() ->
  [
    ?_assertEqual(<<"1\n">>,        glazer_yaml:encode(1)),
    ?_assertEqual(<<"-5\n">>,       glazer_yaml:encode(-5)),
    ?_assertEqual(<<"1.5\n">>,      glazer_yaml:encode(1.5)),
    ?_assertEqual(<<"1.0\n">>,      glazer_yaml:encode(1.0)),
    ?_assertEqual(<<"true\n">>,     glazer_yaml:encode(true)),
    ?_assertEqual(<<"false\n">>,    glazer_yaml:encode(false)),
    ?_assertEqual(<<"null\n">>,     glazer_yaml:encode(null)),
    ?_assertEqual(<<"null\n">>,     glazer_yaml:encode(nil)),
    ?_assertEqual(<<".inf\n">>,     glazer_yaml:encode(infinity)),
    ?_assertEqual(<<"-.inf\n">>,    glazer_yaml:encode(neg_infinity)),
    ?_assertEqual(<<".nan\n">>,     glazer_yaml:encode(nan)),
    ?_assertEqual(<<"hello\n">>,    glazer_yaml:encode(<<"hello">>)),
    ?_assertEqual(<<"{}\n">>,       glazer_yaml:encode(#{})),
    ?_assertEqual(<<"[]\n">>,       glazer_yaml:encode([]))
  ].

encode_quoting_test_() ->
  [
    %% Strings that look like other scalar types must be quoted.
    ?_assertEqual(<<"'123'\n">>,    glazer_yaml:encode(<<"123">>)),
    ?_assertEqual(<<"'true'\n">>,   glazer_yaml:encode(<<"true">>)),
    ?_assertEqual(<<"'null'\n">>,   glazer_yaml:encode(<<"null">>)),
    ?_assertEqual(<<"'~'\n">>,      glazer_yaml:encode(<<"~">>)),
    ?_assertEqual(<<"'yes'\n">>,    glazer_yaml:encode(<<"yes">>)),
    ?_assertEqual(<<"'.inf'\n">>,   glazer_yaml:encode(<<".inf">>)),
    ?_assertEqual(<<"'.nan'\n">>,   glazer_yaml:encode(<<".nan">>)),
    ?_assertEqual(<<"'1.5e10'\n">>, glazer_yaml:encode(<<"1.5e10">>)),

    %% Plain unambiguous strings are emitted bare.
    ?_assertEqual(<<"hello world\n">>, glazer_yaml:encode(<<"hello world">>)),
    ?_assertEqual(<<"a-b_c\n">>,       glazer_yaml:encode(<<"a-b_c">>)),

    %% ': ' and trailing/leading-indicator content forces quoting.
    ?_assertEqual(<<"'a: b'\n">>,   glazer_yaml:encode(<<"a: b">>)),
    ?_assertEqual(<<"'- x'\n">>,    glazer_yaml:encode(<<"- x">>)),
    ?_assertEqual(<<"'#x'\n">>,     glazer_yaml:encode(<<"#x">>)),
    ?_assertEqual(<<"'a #b'\n">>,   glazer_yaml:encode(<<"a #b">>)),
    ?_assertEqual(<<"' x'\n">>,     glazer_yaml:encode(<<" x">>)),
    ?_assertEqual(<<"'x '\n">>,     glazer_yaml:encode(<<"x ">>)),

    %% A literal apostrophe is allowed unescaped in plain scalars.
    ?_assertEqual(<<"it's\n">>,  glazer_yaml:encode(<<"it's">>)),

    %% Embedded single quotes are doubled when single-quoting is required
    %% for some other reason (here, leading whitespace).
    ?_assertEqual(<<"' it''s'\n">>, glazer_yaml:encode(<<" it's">>)),

    %% Control characters force double-quoting with escapes.
    ?_assertEqual(<<"\"line1\\nline2\"\n">>, glazer_yaml:encode(<<"line1\nline2">>)),
    ?_assertEqual(<<"\"a\\tb\"\n">>,         glazer_yaml:encode(<<"a\tb">>)),

    %% Empty string must be quoted.
    ?_assertEqual(<<"''\n">>,       glazer_yaml:encode(<<"">>))
  ].

encode_mapping_test_() ->
  [
    ?_assertEqual(<<"a: 1\nb: 2\n">>,
                  glazer_yaml:encode(#{<<"a">> => 1, <<"b">> => 2})),
    ?_assertEqual(<<"a:\n  b: 1\n  c: 2\n">>,
                  glazer_yaml:encode(#{<<"a">> => #{<<"b">> => 1, <<"c">> => 2}})),
    ?_assertEqual(<<"a: {}\n">>,
                  glazer_yaml:encode(#{<<"a">> => #{}})),
    %% Proplist (`{[{K,V}...]}`) encodes the same as a map.
    ?_assertEqual(<<"a: 1\nb: 2\n">>,
                  glazer_yaml:encode({[{<<"a">>, 1}, {<<"b">>, 2}]})),
    %% Atom and integer keys.
    ?_assertEqual(<<"a: 1\n">>, glazer_yaml:encode(#{a => 1})),
    ?_assertEqual(<<"1: a\n">>, glazer_yaml:encode(#{1 => <<"a">>}))
  ].

encode_sequence_test_() ->
  [
    ?_assertEqual(<<"- 1\n- 2\n- 3\n">>, glazer_yaml:encode([1, 2, 3])),
    ?_assertEqual(<<"a: []\n">>,         glazer_yaml:encode(#{<<"a">> => []})),

    %% Sequences nested under a mapping key sit at the same indentation as
    %% the key (PyYAML/libyaml default style).
    ?_assertEqual(<<"a:\n- 1\n- 2\nb: 3\n">>,
                  glazer_yaml:encode(#{<<"a">> => [1, 2], <<"b">> => 3})),

    %% A sequence of mappings: each mapping starts inline after "- ".
    ?_assertEqual(<<"- id: x\n  w: 1\n- id: y\n  w: 2\n">>,
                  glazer_yaml:encode([{[{<<"id">>, <<"x">>}, {<<"w">>, 1}]},
                                       {[{<<"id">>, <<"y">>}, {<<"w">>, 2}]}])),

    %% Nested sequences.
    ?_assertEqual(<<"- - 1\n  - 2\n- - 3\n  - 4\n">>,
                  glazer_yaml:encode([[1, 2], [3, 4]]))
  ].

encode_roundtrip_test_() ->
  [
    ?_assertEqual(Term, glazer_yaml:decode(glazer_yaml:encode(Term)))
   || Term <- [
        #{<<"a">> => [1, 2, 3], <<"b">> => #{<<"c">> => <<"d">>},
          <<"e">> => 1.5, <<"f">> => true, <<"g">> => null},
        #{<<"outer">> => #{<<"a">> => [1, 2], <<"b">> => 3}},
        #{<<"top">> => #{<<"a">> => [#{<<"id">> => <<"x">>, <<"w">> => 1}]}},
        [#{<<"a">> => 1}, #{<<"b">> => 2}],
        #{<<"s">> => <<"hello world">>, <<"q">> => <<"a: b">>, <<"n">> => <<"123">>}
      ]
  ].

encode_opts_test_() ->
  [
    ?_assertEqual(<<"a: null\n">>, glazer_yaml:encode(#{<<"a">> => nil}, [use_nil])),
    ?_assertEqual(<<"a: null\n">>, glazer_yaml:encode(#{<<"a">> => my_nil}, [{null_term, my_nil}]))
  ].

%% ----------------------------------------------------------------------------
%% read_file/1,2, write_file/2,3
%% ----------------------------------------------------------------------------

file_test_() ->
  Path = ?MODULE_STRING ++ ".tmp.yaml",
  Term = #{<<"a">> => 1, <<"b">> => [true, null, 3.5]},
  [
    ?_test(begin
      ?assertEqual(ok, glazer_yaml:write_file(Path, Term)),
      ?assertEqual(Term, glazer_yaml:read_file(Path)),
      ok = file:delete(Path)
    end),

    ?_test(begin
      ?assertEqual(ok, glazer_yaml:write_file(Path, #{<<"a">> => nil}, [use_nil])),
      ?assertEqual(#{<<"a">> => nil}, glazer_yaml:read_file(Path, [use_nil])),
      ok = file:delete(Path)
    end),

    ?_assertError(<<"nonexistent.yaml: no such file or directory">>,
                   glazer_yaml:read_file("nonexistent.yaml"))
  ].
