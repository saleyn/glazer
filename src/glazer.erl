-module(glazer).
-moduledoc """
Fast JSON encoding and decoding using the glaze C++ library.

By default JSON `null` is represented as the atom `null`. To change it
application-wide, set the `null` env key in your config:
```
{glazer, [{null, nil}]}.
```

See also [https://github.com/stephenberry/glaze]
""".
-export([decode/1, decode/2, try_decode/1, try_decode/2,
         encode/1, encode/2, minify/1, prettify/1,
         encode_bigint/1, decode_bigint/1,
         scan/1, scan/2,
         stream_decoder/0, stream_decoder/1, stream_feed/2, stream_eof/1]).

-on_load(init/0).

-define(LIBNAME, glazer).
-define(NOT_LOADED_ERROR,
  erlang:nif_error({not_loaded, [{module, ?MODULE}, {line, ?LINE}]})).

-type decode_opt() ::
    return_maps
  | object_as_tuple
  | use_nil
  | {null_term, atom()}
  | {keys, atom | existing_atom | binary}.

-doc """
Decode options:

- `return_maps`         - decode JSON objects as Erlang maps (default)
- `object_as_tuple`     - decode JSON objects as `{[{K, V}]}` proplists
- `use_nil`             - use the atom `nil` for JSON null
- `{null_term, Atom}`   - use `Atom` for JSON null
- `{keys, atom}`        - decode object keys as atoms
- `{keys, existing_atom}` - decode keys as existing atoms, fall back to binary
- `{keys, binary}`      - decode keys as binaries (default)
""".
-type decode_opts() :: [decode_opt()].

-type encode_opt() ::
    pretty
  | uescape
  | force_utf8
  | use_nil
  | {null_term, atom()}.

-doc """
Encode options:

- `pretty`            - pretty-print the JSON output
- `uescape`           - escape non-ASCII characters as \\uXXXX sequences
- `force_utf8`        - fix invalid UTF-8 sequences before encoding
- `use_nil`           - encode the atom `nil` as JSON `null`
- `{null_term, Atom}` - encode `Atom` as JSON `null`
""".
-type encode_opts() :: [encode_opt()].

-export_type([decode_opts/0, encode_opts/0]).

-type scan_state() :: tuple().

-record(stream_decoder, {
  opts   = []        :: decode_opts(),
  buffer = <<>>      :: binary(),
  state  = undefined :: scan_state() | undefined
}).

-opaque stream_decoder() :: #stream_decoder{}.
-export_type([stream_decoder/0]).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").
-endif.

init() ->
  NullVal = application:get_env(?LIBNAME, null, null),
  is_atom(NullVal) orelse erlang:error("glazer: option 'null' must be an atom"),
  SoName  =
    case code:priv_dir(?LIBNAME) of
      {error, bad_name} ->
        case code:which(?MODULE) of
          Filename when is_list(Filename) ->
            Dir = filename:dirname(filename:dirname(Filename)),
            filename:join([Dir, "priv", ?LIBNAME]);
          _ ->
            filename:join("../priv", ?LIBNAME)
        end;
      Dir ->
        filename:join(Dir, ?LIBNAME)
    end,
  erlang:load_nif(SoName, [{null, NullVal}]).

-doc """
Decode a JSON binary or iolist to an Erlang term. JSON objects are returned as
maps (default). Raises `{parse_error, Msg}` on invalid input.
""".
-spec decode(binary() | iolist()) -> term().
decode(Input) ->
  case try_decode(Input) of
    {ok, Term}    -> Term;
    {error, Reason} -> error(Reason)
  end.

-doc "Decode a JSON binary or iolist to an Erlang term with options. Raises `{parse_error, Msg}` on invalid input.".
-spec decode(binary() | iolist(), decode_opts()) -> term().
decode(Input, Opts) ->
  case try_decode(Input, Opts) of
    {ok,    Term}   -> Term;
    {error, Reason} -> error(Reason)
  end.

-doc "Decode a JSON binary or iolist, returning `{ok, Term}` or `{error, {parse_error, Msg}}` instead of raising.".
-spec try_decode(binary() | iolist()) -> {ok, term()} | {error, {parse_error, binary()}}.
try_decode(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Decode a JSON binary or iolist with options, returning `{ok, Term}` or `{error, {parse_error, Msg}}` instead of raising.".
-spec try_decode(binary() | iolist(), decode_opts()) -> {ok, term()} | {error, {parse_error, binary()}}.
try_decode(_Input, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc "Encode an Erlang term to a JSON binary.".
-spec encode(term()) -> binary().
encode(Data) ->
  encode(Data, []).

-doc "Encode an Erlang term to a JSON binary with options.".
-spec encode(term(), encode_opts()) -> binary().
encode(_Data, _Opts) ->
  ?NOT_LOADED_ERROR.

-doc "Minify a JSON binary or iolist, removing all unnecessary whitespace.".
-spec minify(binary() | iolist()) -> binary().
minify(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Pretty-print a JSON binary or iolist with two-space indentation.".
-spec prettify(binary() | iolist()) -> binary().
prettify(_Input) ->
  ?NOT_LOADED_ERROR.

-doc "Encode a big integer to its JSON string representation.".
-spec encode_bigint(integer()) -> {ok, binary()} | {error, binary()}.
encode_bigint(_BigInt) ->
  ?NOT_LOADED_ERROR.

-doc "Decode a JSON number string to a big integer.".
-spec decode_bigint(binary() | iolist()) -> {ok, integer()} | {error, binary()}.
decode_bigint(_NumberString) ->
  ?NOT_LOADED_ERROR.

-doc """
Locate the end of the next complete top-level JSON value in `Bin`, without
decoding it.

Returns:

- `{complete, EndOffset}` - a complete value spans `binary:part(Bin, 0,
  EndOffset)`; the rest of `Bin` (if any) is left over for the next call
- `{incomplete, ScanState}` - `Bin` doesn't yet contain a complete value;
  feed more data via `scan/2` once it's available, passing the *entire
  unconsumed remainder* (this `Bin`, with new bytes appended) plus
  `ScanState`

This is the low-level primitive behind [`stream_feed/2`](`stream_feed/2`);
most callers should use the `stream_*` API instead.
""".
-spec scan(binary() | iolist()) -> {complete, non_neg_integer()} | {incomplete, scan_state()}.
scan(_Bin) ->
  ?NOT_LOADED_ERROR.

-doc "Resume scanning `Bin` (the unconsumed remainder plus newly-appended bytes) from `ScanState`.".
-spec scan(binary() | iolist(), scan_state()) -> {complete, non_neg_integer()} | {incomplete, scan_state()}.
scan(_Bin, _ScanState) ->
  ?NOT_LOADED_ERROR.

%%%----------------------------------------------------------------------------
%%% Streaming / incremental decode
%%%----------------------------------------------------------------------------

-doc """
Create a new incremental decoder for feeding JSON in chunks (e.g. from a
socket or file), useful when a complete document isn't available up front
or when a stream contains a sequence of concatenated/whitespace-separated
JSON values (e.g. newline-delimited JSON).

Decoding itself is **not** incremental — each complete top-level value is
still decoded in a single pass via [`decode/2`](`decode/2`) using the
library's fast whole-buffer decoder. Only the *boundary detection* (finding
where one value ends and the next begins) is incremental, via a small
byte-scanner that tracks nesting/string state across chunks.

## Example

```erlang
1> D0 = glazer:stream_decoder(),
2> {Vals1, D1} = glazer:stream_feed(D0, <<"{\\"a\\":1} {\\"b\\":">>),
3> Vals1.
[#{<<"a">> => 1}]
4> {Vals2, _D2} = glazer:stream_feed(D1, <<"2}">>),
5> Vals2.
[#{<<"b">> => 2}]
```
""".
-spec stream_decoder() -> stream_decoder().
stream_decoder() ->
  stream_decoder([]).

-doc "Create a new incremental decoder, passing `Opts` through to every [`decode/2`](`decode/2`) call.".
-spec stream_decoder(decode_opts()) -> stream_decoder().
stream_decoder(Opts) when is_list(Opts) ->
  #stream_decoder{opts = Opts}.

-doc """
Feed a chunk of bytes into the decoder, returning any complete JSON values
found so far (in order) along with the updated decoder.

Raises the same exceptions as [`decode/2`](`decode/2`) (e.g.
`{parse_error, Reason}`) if a value that the scanner deemed complete fails
to decode.
""".
-spec stream_feed(stream_decoder(), binary() | iolist()) -> {[term()], stream_decoder()}.
stream_feed(#stream_decoder{buffer = Buf} = D, Chunk) ->
  NewBuf = iolist_to_binary([Buf, Chunk]),
  stream_drain(D#stream_decoder{buffer = NewBuf, state = undefined}, []).

stream_drain(#stream_decoder{buffer = Buf, opts = Opts, state = St} = D, Acc) ->
  ScanResult = case St of
    undefined -> scan(Buf);
    _         -> scan(Buf, St)
  end,
  case ScanResult of
    {complete, End} ->
      <<ValueBin:End/binary, Rest/binary>> = Buf,
      Term = decode(ValueBin, Opts),
      stream_drain(D#stream_decoder{buffer = Rest, state = undefined}, [Term | Acc]);
    {incomplete, NewSt} ->
      {lists:reverse(Acc), D#stream_decoder{state = NewSt}}
  end.

-doc """
Signal end-of-stream: decode any remaining buffered bytes as a final value
(useful for a trailing bare scalar, e.g. a lone number or `true`/`null`,
which the scanner can't otherwise distinguish from a value that's still
being written to mid-chunk).

Returns `{ok, [Term]}` with zero or one trailing value, or `{error,
Reason}` if the remaining bytes don't form a complete value.
""".
-spec stream_eof(stream_decoder()) -> {ok, [term()]} | {error, term()}.
stream_eof(#stream_decoder{buffer = Buf, opts = Opts}) ->
  case is_blank(Buf) of
    true  -> {ok, []};
    false ->
      try decode(Buf, Opts) of
        Term -> {ok, [Term]}
      catch
        error:Reason -> {error, Reason}
      end
  end.

%% True if `Bin` is empty or contains only JSON whitespace (space, tab, CR, LF).
is_blank(Bin) ->
  lists:all(fun(B) -> B =:= $\s orelse B =:= $\t orelse B =:= $\r orelse B =:= $\n end,
            binary_to_list(Bin)).

%%%----------------------------------------------------------------------------
%%% Tests
%%%----------------------------------------------------------------------------
-ifdef(EUNIT).

encode_test_() ->
  [
    ?_assertEqual(<<"null">>,        encode(null)),
    ?_assertEqual(<<"null">>,        encode(nil, [use_nil])),
    ?_assertEqual(<<"true">>,        encode(true)),
    ?_assertEqual(<<"false">>,       encode(false)),
    ?_assertEqual(<<"1">>,           encode(1)),
    ?_assertEqual(<<"1.5">>,         encode(1.5)),
    ?_assertEqual(<<"\"hello\"">>,   encode(<<"hello">>)),
    ?_assertEqual(<<"[1,2,3]">>,     encode([1, 2, 3])),
    ?_assertEqual(<<"{}">>,          encode(#{})),
    ?_assertMatch(<<"{", _/binary>>, encode(#{<<"a">> => 1}))
  ].

decode_test_() ->
  [
    ?_assertEqual(null,             decode(<<"null">>)),
    ?_assertEqual(nil,              decode(<<"null">>, [use_nil])),
    ?_assertEqual(true,             decode(<<"true">>)),
    ?_assertEqual(false,            decode(<<"false">>)),
    ?_assertEqual(1,                decode(<<"1">>)),
    ?_assertEqual(1.5,              decode(<<"1.5">>)),
    ?_assertEqual(<<"hello">>,      decode(<<"\"hello\"">>)),
    ?_assertEqual([1, 2, 3],        decode(<<"[1,2,3]">>)),
    ?_assertEqual(#{<<"a">> => 1},  decode(<<"{\"a\":1}">>)),
    ?_assertEqual({[{<<"a">>, 1}]}, decode(<<"{\"a\":1}">>, [object_as_tuple])),
    ?_assertEqual(null,             decode(<<"null">>, [{null_term, null}])),
    ?_assertEqual(my_null,          decode(<<"null">>, [{null_term, my_null}]))
  ].

roundtrip_test_() ->
  Vals = [null, true, false, 0, 1, -1, 1.5, <<"hello">>, [], [1, 2, 3],
          #{<<"a">> => 1, <<"b">> => [1, 2]},
          #{<<"nested">> => #{<<"x">> => true}}],
  [?_assertEqual(V, decode(encode(V))) || V <- Vals].

minify_test_() ->
  [
    ?_assertEqual(<<"[1,2,3]">>,   minify(<<"[ 1, 2, 3 ]">>)),
    ?_assertEqual(<<"{\"a\":1}">>, minify(<<" { \"a\" : 1 } ">>))
  ].

prettify_test_() ->
  [
    ?_assertMatch(<<"[\n", _/binary>>,  prettify(<<"[1,2,3]">>)),
    ?_assertMatch(<<"{\n", _/binary>>,  prettify(<<"{\"a\":1}">>))
  ].

keys_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1},  decode(<<"{\"a\":1}">>)),
    ?_assertEqual(#{<<"a">> => 1},  decode(<<"{\"a\":1}">>, [{keys, binary}])),
    ?_assertEqual(#{a => 1},        decode(<<"{\"a\":1}">>, [{keys, atom}])),
    ?_assertEqual(#{a => 1},        decode(<<"{\"a\":1}">>, [{keys, existing_atom}])),
    %% existing_atom falls back to a binary for keys with no matching atom
    ?_assertEqual(#{<<"no_such_atom_in_glazer_test_suite_xyz">> => 1},
                  decode(<<"{\"no_such_atom_in_glazer_test_suite_xyz\":1}">>,
                         [{keys, existing_atom}]))
  ].

uescape_test_() ->
  [
    %% U+00E9 (é), UTF-8: 0xC3 0xA9
    ?_assertEqual(<<"\"\\u00e9\"">>, encode(<<16#C3, 16#A9>>, [uescape])),
    %% Without uescape, UTF-8 bytes pass through unescaped
    ?_assertEqual(<<"\"", 16#C3, 16#A9, "\"">>, encode(<<16#C3, 16#A9>>)),
    %% U+1F600 (emoji, outside the BMP) encodes as a surrogate pair
    ?_assertEqual(<<"\"\\ud83d\\ude00\"">>, encode(<<16#F0,16#9F,16#98,16#80>>, [uescape])),
    %% Round-trips back to the original UTF-8 binary
    ?_assertEqual(<<16#C3, 16#A9>>, decode(encode(<<16#C3, 16#A9>>, [uescape])))
  ].

force_utf8_test_() ->
  [
    %% Invalid byte sequences are sanitized to U+FFFD (UTF-8: EF BF BD)
    ?_assertEqual(<<"\"", 16#EF, 16#BF, 16#BD, 16#EF, 16#BF, 16#BD, "a\"">>,
                  encode(<<16#FF, 16#FE, $a>>, [force_utf8])),
    %% Without force_utf8, invalid bytes pass through verbatim
    ?_assertEqual(<<"\"", 16#FF, 16#FE, "a\"">>, encode(<<16#FF, 16#FE, $a>>)),
    %% Valid UTF-8 is left untouched
    ?_assertEqual(<<"\"", 16#C3, 16#A9, "\"">>, encode(<<16#C3, 16#A9>>, [force_utf8]))
  ].

pretty_test_() ->
  [
    ?_assertEqual(<<"{\n   \"a\": 1\n}">>, encode(#{<<"a">> => 1}, [pretty])),
    ?_assertEqual(<<"[\n   1,\n   2\n]">>, encode([1, 2], [pretty])),
    ?_assertEqual(#{<<"a">> => 1}, decode(encode(#{<<"a">> => 1}, [pretty])))
  ].

null_term_encode_test_() ->
  [
    ?_assertEqual(<<"null">>, encode(null)),
    ?_assertEqual(<<"null">>, encode(nil, [{null_term, nil}])),
    ?_assertEqual(<<"null">>, encode(undefined, [{null_term, undefined}])),
    ?_assertEqual(<<"null">>, encode(null, [{null_term, undefined}]))
  ].

object_as_tuple_test_() ->
  [
    ?_assertEqual({[]}, decode(<<"{}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}, {<<"b">>, 2}]},
                  decode(<<"{\"a\":1,\"b\":2}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, {[{<<"b">>, true}]}}]},
                  decode(<<"{\"a\":{\"b\":true}}">>, [object_as_tuple])),
    ?_assertEqual({[{<<"a">>, 1}]},
                  decode(encode({[{<<"a">>, 1}]}), [object_as_tuple]))
  ].

numbers_test_() ->
  [
    ?_assertEqual(0,        decode(<<"0">>)),
    ?_assertEqual(-1,       decode(<<"-1">>)),
    ?_assertEqual(-1.5,     decode(<<"-1.5">>)),
    ?_assertEqual(1.0e10,   decode(<<"1.0e10">>)),
    ?_assertEqual(1.0e-10,  decode(<<"1.0e-10">>)),
    ?_assertEqual(<<"-1">>, encode(-1)),
    ?_assertEqual(<<"0">>,  encode(0))
  ].

iolist_input_test_() ->
  [
    ?_assertEqual(#{<<"a">> => 1}, decode([<<"{\"a\":">>, <<"1}">>])),
    ?_assertEqual(#{<<"a">> => 1}, decode([<<"{">>, [<<"\"a\":1">>], <<"}">>])),
    ?_assertEqual(<<"{\"a\":1}">>, minify([<<"{ \"a\"">>, <<": 1 }">>]))
  ].

decode_error_test_() ->
  [
    ?_assertError(_, decode(<<"">>)),
    ?_assertError(_, decode(<<"{\"a\":}">>)),
    ?_assertError(_, decode(<<"{\"a\":1">>)),
    ?_assertError(_, decode(<<"[1, 2">>)),
    ?_assertError(_, decode(<<"not json">>))
  ].

bigint_test_() ->
  Big =  123456789012345678901234567890,
  Neg = -Big,
  [
    ?_assertEqual({ok,  <<"123456789012345678901234567890">>}, encode_bigint(Big)),
    ?_assertEqual({ok, <<"-123456789012345678901234567890">>}, encode_bigint(Neg)),
    ?_assertEqual({ok, Big},                                   decode_bigint(<<"123456789012345678901234567890">>)),
    ?_assertEqual({ok, Neg},                                   decode_bigint(<<"-123456789012345678901234567890">>)),
    ?_assertEqual({ok, 123},                                   decode_bigint(<<"123">>)),
    ?_assertEqual(Big,                                         decode(<<"123456789012345678901234567890">>)),
    ?_assertEqual(<<"123456789012345678901234567890">>,        encode(Big)),
    ?_assertEqual(Big,                                         decode(encode(Big)))
  ].

scan_test_() ->
  [
    ?_assertEqual({complete, 7},  scan(<<"{\"a\":1}">>)),
    ?_assertEqual({complete, 7},  scan(<<"{\"a\":1}  {\"b\":2}">>)),
    ?_assertEqual({complete, 13}, scan(<<"[1,2,[3,4],5]rest">>)),
    ?_assertMatch({incomplete, _}, scan(<<"{\"a\":">>)),
    ?_assertMatch({incomplete, _}, scan(<<"123">>)),

    %% resume across a chunk boundary: caller passes the whole buffer + new
    %% bytes along with the previously-returned state
    ?_test(begin
      Part1 = <<"{\"a\":">>,
      Part2 = <<"1}">>,
      {incomplete, S1} = scan(Part1),
      ?assertEqual({complete, 7}, scan(<<Part1/binary, Part2/binary>>, S1))
    end),

    %% an escape sequence straddling the chunk boundary is tracked correctly
    ?_test(begin
      Chunk1 = <<"{\"k\":\"ab\\">>,
      Chunk2 = <<"\"cd\"}">>,
      {incomplete, S2} = scan(Chunk1),
      Whole = <<Chunk1/binary, Chunk2/binary>>,
      ?assertEqual({complete, byte_size(Whole)}, scan(Whole, S2))
    end)
  ].

stream_decoder_test_() ->
  [
    %% values split across feed/2 calls
    ?_test(begin
      D0 = stream_decoder(),
      {[#{<<"a">> := 1}], D1} = stream_feed(D0, <<"{\"a\":1} {\"b\":">>),
      {[#{<<"b">> := 2}], _D2} = stream_feed(D1, <<"2}">>)
    end),

    %% byte-at-a-time NDJSON feeding decodes every line
    ?_test(begin
      Doc = <<"{\"x\":1}\n{\"y\":[1,2,3]}\n{\"z\":\"hi\"}\n">>,
      {Vals, DLast} = lists:foldl(
        fun(B, {Acc, D}) ->
          {V, D2} = stream_feed(D, <<B>>),
          {Acc ++ V, D2}
        end, {[], stream_decoder()}, binary_to_list(Doc)),
      {ok, []} = stream_eof(DLast),
      ?assertEqual([#{<<"x">> => 1},
                    #{<<"y">> => [1, 2, 3]},
                    #{<<"z">> => <<"hi">>}], Vals)
    end),

    %% a trailing bare scalar is only resolved at end-of-stream
    ?_test(begin
      D0 = stream_decoder(),
      {[], D1} = stream_feed(D0, <<"   42">>),
      ?assertEqual({ok, [42]}, stream_eof(D1))
    end),

    %% trailing whitespace at EOF yields no extra value
    ?_test(begin
      D0 = stream_decoder(),
      {[#{<<"a">> := 1}], D1} = stream_feed(D0, <<"{\"a\":1}\n">>),
      ?assertEqual({ok, []}, stream_eof(D1))
    end),

    %% decode options are threaded through to every decoded value
    ?_test(begin
      D0 = stream_decoder([{keys, atom}]),
      {[#{a := 1}], _D1} = stream_feed(D0, <<"{\"a\":1}">>)
    end),

    %% malformed trailing bytes surface as an error from stream_eof/1
    ?_test(begin
      D0 = stream_decoder(),
      {[], D1} = stream_feed(D0, <<"{\"a\":">>),
      ?assertMatch({error, _}, stream_eof(D1))
    end)
  ].

-endif.
