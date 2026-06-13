#!/usr/bin/env escript
%% vim:ts=2:sw=2:et
%%-----------------------------------------------------------------------------
%% Profiling workload for PGO (`make optimize`): repeatedly decode and encode
%% the sample JSON files in test/data/ via the glazer NIF, so the
%% instrumented build (PGO=generate) sees representative branches/hot loops
%% without depending on the Elixir/Mix benchmark suite.
%%-----------------------------------------------------------------------------

main(_Args) ->
  Root  = filename:dirname(filename:dirname(escript:script_name())),
  EbinGlob = filename:join([Root, "_build", "default", "lib", "*", "ebin"]),
  [code:add_pathz(P) || P <- filelib:wildcard(EbinGlob)],

  DataDir = filename:join(Root, "test/data"),
  Files = ["small.json", "openrtb.json", "esad.json", "twitter.json", "twitter2.json"],

  Inputs = lists:filtermap(
    fun(Name) ->
      Path = filename:join(DataDir, Name),
      case file:read_file(Path) of
        {ok, Bin} -> {true, {Name, Bin}};
        {error, _} -> false
      end
    end, Files),

  Inputs == [] andalso fail("no test data files found under " ++ DataDir),

  [run(Name, Bin) || {Name, Bin} <- Inputs],
  ok.

run(Name, Bin) ->
  Iterations = iterations(byte_size(Bin)),
  io:format("==> ~s (~p bytes) x ~p~n", [Name, byte_size(Bin), Iterations]),
  repeat(Iterations, fun() ->
    Term = glazer_json:decode(Bin),
    _    = glazer_json:encode(Term)
  end).

%% Smaller files get more iterations so each input contributes a comparable
%% amount of total work to the profile.
iterations(Size) when Size >  200000 -> 100;
iterations(Size) when Size >   10000 -> 1000;
iterations(_)                        -> 5000.

repeat(0, _F) -> ok;
repeat(N, F)  -> F(), repeat(N - 1, F).

fail(Msg) ->
  io:format(standard_error, "pgo-profile: ~s~n", [Msg]),
  halt(1).
