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

    info = :glazer.info()
    opt  = info.optimization
    pgo  = info.pgo && " - PGO" || ""

    IO.puts("==> Running benchmarks with parallelism: #{nworkers} (#{opt == :none && "non optimized" || "optimization: O#{opt}"}#{pgo})")
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
    f.()
    repeat(n - 1, f)
  end

  @doc """
  Pick an iteration count for a payload of `size` bytes from a `thresholds`
  list of `{min_size, n}` pairs, sorted by descending `min_size`. Returns the
  `n` for the first threshold that `size` meets or exceeds, falling back to
  the last (smallest-size) entry's `n` otherwise.

      iterations(750_000, [{200_000, 200}, {10_000, 1_000}], 5_000)
      #=> 200
  """
  def iterations(size, thresholds, default) do
    case Enum.find(thresholds, fn {min_size, _n} -> size > min_size end) do
      {_min_size, n} -> n
      nil -> default
    end
  end
end
