%%% @doc Benchmark glazejson vs simdjsone (and other available decoders).
%%%
%%% Run with:
%%%   rebar3 as test do eunit --module=glazejson_bench
%%% Or directly:
%%%   erl -noshell -pa _build/test/lib/*/ebin \
%%%       -eval "glazejson_bench:run(), halt()."
-module(glazejson_bench).

-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").

%% Expose run/0 and run/1 for direct invocation outside eunit.
-export([run/0, run/1]).
-endif.

-export([run/0, run/1]).

%% ---------------------------------------------------------------------------
%% Public API
%% ---------------------------------------------------------------------------

-spec run() -> ok.
run() -> run([]).

%% Extra {Label, DecodeFun, EncodeFun} entries can be passed in.
-spec run([{string(), fun((binary()) -> term()), fun((term()) -> binary())}]) -> ok.
run(Extra) ->
  DataDir = data_dir(),
  Files = [
    {"twitter  (~.1fK)", filename:join(DataDir, "twitter.json")},
    {"twitter2 (~.1fK)", filename:join(DataDir, "twitter2.json")},
    {"openrtb  (~.1fK)", filename:join(DataDir, "openrtb.json")},
    {"esad     (~.1fK)", filename:join(DataDir, "esad.json")},
    {"small    (~.1fK)", filename:join(DataDir, "small.json")}
  ],
  Suites = build_suites(Extra),
  %% Collect {Label, [{Name, Result}]} for every file, skipping missing ones.
  FilesData = lists:filtermap(
    fun({Fmt, Path}) ->
      case file:read_file(Path) of
        {ok, Bin} ->
          Label = lists:flatten(io_lib:format(Fmt, [byte_size(Bin) / 1024])),
          {true, {Label, run_file(Bin, Suites)}};
        {error, Reason} ->
          io:format("Skipping ~s: ~p~n", [Path, Reason]),
          false
      end
    end, Files),
  print_table(Suites, FilesData).

%% ---------------------------------------------------------------------------
%% Internal
%% ---------------------------------------------------------------------------

data_dir() ->
  %% Works both from rebar3 eunit (_build/.../test) and from priv/ direct run.
  Candidates = [
    filename:join([filename:dirname(code:which(?MODULE)), "..", "..", "test", "data"]),
    filename:join([filename:dirname(code:which(?MODULE)), "data"]),
    "test/data"
  ],
  hd(lists:dropwhile(
    fun(D) -> not filelib:is_dir(D) end,
    Candidates
  ) ++ ["test/data"]).

build_suites(Extra) ->
  Base = [
    {"glazejson",
      fun(B) -> glazejson:decode(B) end,
      fun(T) -> glazejson:encode(T) end}
  ],
  Optional = lists:filtermap(
    fun({Name, Decode, Encode}) ->
      case library_available(Name) of
        true  -> {true, {Name, Decode, Encode}};
        false -> false
      end
    end,
    [
      {"simdjsone",
        fun(B) -> simdjson:decode(B) end,
        fun(T) -> simdjson:encode(T) end},
      {"jiffy",
        fun(B) -> jiffy:decode(B, [return_maps]) end,
        fun(T) -> iolist_to_binary(jiffy:encode(T)) end},
      {"json",
        fun(B) -> json:decode(B) end,
        fun(T) -> iolist_to_binary(json:encode(T)) end},
      {"thoas",
        fun(B) -> {ok, R} = thoas:decode(B), R end,
        fun(T) -> {ok, R} = thoas:encode(T), R end},
      {"euneus",
        fun(B) -> euneus:decode(B) end,
        fun(T) -> euneus:encode(T) end}
    ]
  ),
  Extra2 = [{N, D, E} || {N, D, E} <- Extra],
  Base ++ Optional ++ Extra2.

library_available("json") ->
  %% OTP 27+ built-in — ensure_loaded forces the module into the code server
  case code:ensure_loaded(json) of
    {module, _} -> erlang:function_exported(json, decode, 1);
    _           -> false
  end;
library_available("simdjsone") ->
  %% app name is simdjsone, module name is simdjson
  case code:ensure_loaded(simdjson) of
    {module, _} -> true;
    _           -> false
  end;
library_available(Name) ->
  Mod = list_to_atom(Name),
  case code:ensure_loaded(Mod) of
    {module, _} -> true;
    _           -> false
  end.

%% Run all suites against one file; return [{Name, {ok,DT,ET}|{error,_}}] in
%% suite order (not sorted — sorting happens at print time per column).
run_file(Bin, Suites) ->
  N = iterations(byte_size(Bin)),
  Parent = self(),
  Tasks = [
    begin
      Pid = spawn(fun() ->
        Result = try
          DT = tc(N, fun() -> Decode(Bin) end),
          Decoded = Decode(Bin),
          ET = tc(N, fun() -> Encode(Decoded) end),
          {ok, DT, ET}
        catch
          Class:Reason:Stack -> {error, {Class, Reason, Stack}}
        end,
        Parent ! {Name, Result}
      end),
      {Name, Pid}
    end
    || {Name, Decode, Encode} <- Suites
  ],
  Timeout = N * 200 + 10000,
  [{Name, receive {Name, R} -> R after Timeout -> {error, timeout} end}
   || {Name, _Pid} <- Tasks].

%% Print one pivoted table:
%%   rows    = libraries
%%   columns = files × {decode, encode}
print_table(Suites, FilesData) ->
  LibW  = 12,   % library name column width
  ColW  = 9,    % each numeric column width
  Sep   = 2,    % spaces between file groups

  %% Header line 1: file labels, each spanning two columns
  io:format("~n~*s", [LibW, ""]),
  lists:foreach(fun({Label, _}) ->
    GroupW = ColW * 2 + 3 + Sep,  % "dec  enc  " with separator
    Padded = string:centre(Label, GroupW),
    io:format("  ~s", [Padded])
  end, FilesData),
  io:format("~n"),

  %% Header line 2: "decode / encode" sub-columns
  io:format("~*s", [LibW, ""]),
  lists:foreach(fun(_) ->
    io:format("  ~*s  ~*s~*s", [ColW, "decode µs", ColW, "encode µs", Sep, ""])
  end, FilesData),
  io:format("~n"),

  %% Separator
  TotalW = LibW + (ColW * 2 + 3 + Sep + 2) * length(FilesData),
  io:format("~s~n", [lists:duplicate(TotalW, $-)]),

  %% One row per library
  LibNames = [Name || {Name, _, _} <- Suites],
  lists:foreach(fun(Name) ->
    io:format("~-*s", [LibW, Name]),
    lists:foreach(fun({_Label, Results}) ->
      case lists:keyfind(Name, 1, Results) of
        {_, {ok, DT, ET}} ->
          io:format("  ~*.2f  ~*.2f~*s", [ColW, DT, ColW, ET, Sep, ""]);
        {_, {error, timeout}} ->
          io:format("  ~*s  ~*s~*s", [ColW, "TIMEOUT", ColW, "TIMEOUT", Sep, ""]);
        {_, {error, {Class, Reason, _}}} ->
          io:format("  ~*s~*s", [ColW*2+3, io_lib:format("~w:~p", [Class, Reason]), Sep, ""]);
        false ->
          io:format("  ~*s  ~*s~*s", [ColW, "n/a", ColW, "n/a", Sep, ""])
      end
    end, FilesData),
    io:format("~n")
  end, LibNames),
  io:format("~n").

%% ---------------------------------------------------------------------------
%% Timing helpers (same approach as simdjsone)
%% ---------------------------------------------------------------------------

tc(N, F) ->
  time_it(fun() -> exit(call(N, N, F, erlang:system_time(microsecond))) end).

time_it(F) ->
  Pid  = spawn_opt(F, [{min_heap_size, 16384}]),
  MRef = erlang:monitor(process, Pid),
  receive
    {'DOWN', MRef, process, _, Result} -> Result
  end.

call(1, Total, F, T0) ->
  try F() catch _:_ -> ok end,
  (erlang:system_time(microsecond) - T0) / Total;
call(N, Total, F, T0) ->
  try F() catch _:_ -> ok end,
  call(N - 1, Total, F, T0).

iterations(Size) when Size > 200_000 -> 200;
iterations(Size) when Size >  10_000 -> 1000;
iterations(_)                        -> 5000.

%% ---------------------------------------------------------------------------
%% EUnit wrapper — only runs when BENCHMARK_MODE=true to avoid slowing CI
%% ---------------------------------------------------------------------------
-ifdef(TEST).

benchmark_test_() ->
  case os:getenv("BENCHMARK_MODE") of
    "true" -> ?_test(run());
    _      -> ?_assert(true)
  end.

-endif.
