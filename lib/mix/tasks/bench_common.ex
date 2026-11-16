defmodule Mix.Tasks.Bench.Common do
  @moduledoc false

  @doc """
  Print the benchmark banner: the NIF optimization level and whether PGO was used.
  """
  def announce do
    info = :glazer.info()
    opt  = info.optimization
    pgo  = info.pgo && " - PGO" || ""

    IO.puts("===> Running benchmarks (#{opt == :none && "non optimized" || "optimization: #{opt}"}#{pgo})")
    IO.puts("===> Process niceness: #{niceness()} (lower is higher priority)")
  end

  # Nice values of the VM's threads. The scheduler threads inherit the niceness
  # of the process that started the VM (e.g. `sudo nice -n -20`), so this shows
  # the priority the benchmark runs at. Reads /proc on Linux, falls back to ps.
  @doc false
  def niceness do
    case File.ls("/proc/self/task") do
      {:ok, tids} ->
        tids
        |> Enum.map(&thread_nice/1)
        |> Enum.reject(&is_nil/1)
        |> Enum.uniq()
        |> Enum.sort()
        |> Enum.map_join(", ", &Integer.to_string/1)

      {:error, _} ->
        ps_niceness()
    end
  end

  defp thread_nice(tid) do
    with {:ok, stat} <- File.read("/proc/self/task/#{tid}/stat"),
         [_, after_comm] <- String.split(stat, ")", parts: 2),
         fields = String.split(String.trim(after_comm)),
         # Fields after "(comm)" start at field 3 (state); nice is field 19.
         nice = Enum.at(fields, 16),
         {n, ""} <- Integer.parse(nice) do
      n
    else
      _ -> nil
    end
  end

  defp ps_niceness do
    case System.cmd("ps", ["-o", "ni=", "-p", List.to_string(:os.getpid())],
                    stderr_to_stdout: true) do
      {out, 0} -> String.trim(out)
      _ -> "unknown"
    end
  end

  @doc """
  Run each suite in its own fresh process, one after another, and collect the
  results. `fun` receives a `{name, decode, encode}` suite and returns that
  suite's result. A suite that does not finish within `timeout` ms is reported
  as `{:error, "timeout"}`.

  Returns a list of `{name, result}` in suite order.
  """
  def run_each(suites, timeout, fun) do
    parent = self()

    Enum.map(suites, fn {name, _decode, _encode} = suite ->
      ref = make_ref()
      spawn(fn -> send(parent, {ref, fun.(suite)}) end)

      receive do
        {^ref, result} -> {name, result}
      after
        timeout -> {name, {:error, "timeout"}}
      end
    end)
  end

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
