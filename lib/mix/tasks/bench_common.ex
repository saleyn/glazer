defmodule Mix.Tasks.Bench.Common do
  @moduledoc false

  @doc """
  Determine the parallelism level to use for benchmark runs.

  Resolution order:
    1. `-p N` / `-p=N` / `--parallel N` / `--parallel=N` from `args`
    2. `PARALLEL` environment variable
    3. Number of online schedulers (nproc)

  The resulting value is clamped to `[1, nproc]`.
  """
  def parallelism(args) do
    nproc = System.schedulers_online()

    value = parallel_arg(args) || System.get_env("PARALLEL")

    nworkers =
      case value do
        nil ->
          nproc

        value ->
          case Integer.parse(value) do
            {n, _} -> n |> max(1) |> min(nproc)
            :error -> nproc
          end
      end

    IO.puts("==> Running benchmarks with parallelism: #{nworkers}")
    nworkers
  end

  defp parallel_arg(["-p=" <> n | _]),         do: n
  defp parallel_arg(["--parallel=" <> n | _]), do: n
  defp parallel_arg(["-p", n | _]),            do: n
  defp parallel_arg(["--parallel", n | _]),    do: n
  defp parallel_arg([_ | rest]),               do: parallel_arg(rest)
  defp parallel_arg([]),                       do: nil

  @doc "Run `f` `n` times and return the average elapsed time in microseconds."
  def measure(n, f) do
    t0 = :erlang.system_time(:microsecond)
    repeat(n, f)
    t1 = :erlang.system_time(:microsecond)
    (t1 - t0) / n
  end

  def repeat(0, _f), do: :ok
  def repeat(n, f) do
    try do
      f.()
    rescue
      _ -> :ok
    catch
      _, _ -> :ok
    end
    repeat(n - 1, f)
  end
end
