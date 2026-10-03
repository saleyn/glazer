defmodule Mix.Tasks.BenchJson do
  @shortdoc "Benchmark glazer against other JSON libraries"
  @moduledoc """
  Run a pivoted performance benchmark comparing glazer against all
  available JSON libraries.

      mix bench

  Libraries benchmarked (when available):
    - glazer      (this library - C++ NIF)
    - glazer+utf8 (glazer with `validate_utf8`, which torque always does)
    - torque      (Rust sonic-rs NIF)
    - simdjsone   (simdjson NIF)
    - jiffy       (jiffy NIF)
    - jason       (pure Elixir)
    - thoas       (pure Elixir)
    - euneus      (pure Elixir)
    - json        (OTP 27+ built-in)

  Output: one pivoted table — libraries as rows, files×{decode,encode} as column pairs.
  """
  use Mix.Task

  @compile {:no_warn_undefined, [
    Jason,
    :simdjson,
    :jiffy,
    :thoas,
    :euneus
  ]}

  @lib_w 12
  @col_w 7
  @sep   2

  @data_files [
    {"twitter",  "test/data/twitter.json"},
    {"twitter2", "test/data/twitter2.json"},
    {"openrtb",  "test/data/openrtb.json"},
    {"esad",     "test/data/esad.json"},
    {"small",    "test/data/small.json"}
  ]

  @impl Mix.Task
  def run(args) do
    Mix.env() != :bench && Mix.raise("mix bench must be run with MIX_ENV=bench")

    # Ensure all deps are started so NIFs get loaded.
    Mix.Task.run("app.start")

    suites  = top_suites(args, build_suites())
    files   = load_files()

    if files == [] do
      Mix.shell().error("No test data files found in test/data/")
    else
      workers = Mix.Tasks.Bench.Common.parallelism(args)

      files_data = Enum.map(files, fn {label, bin} ->
        results = run_file(bin, suites, workers)
        {label, results}
      end)

      print_table(suites, files_data)
    end
  end

  # ---------------------------------------------------------------------------
  # Suite builder
  # ---------------------------------------------------------------------------

  def top_suites(args, suites) do
    case top_limit(args) do
      nil -> suites
      n when is_integer(n) and n > 0 -> Enum.take(suites, n)
      _ -> suites
    end
  end

  defp build_suites do
    base = [
      {"glazer",
       &:glazer_json.decode/1,
       fn t -> :glazer_json.encode(t) end},
      {"glazer+utf8",
       fn b -> :glazer_json.decode(b, [:validate_utf8]) end,
       fn t -> :glazer_json.encode(t) end}
    ]

    optional_candidates = [
      {"torque",
       fn b -> {:ok, r} = apply(Torque, :decode, [b]); r end,
       fn t -> apply(Torque, :encode_to_iodata, [t]) end,
       Torque},

      {"simdjsone",
       &:simdjson.decode/1,
       fn t -> :simdjson.encode(t) end,
       :simdjson},

      {"jiffy",
       fn b -> :jiffy.decode(b, [:return_maps]) end,
       fn t -> IO.iodata_to_binary(:jiffy.encode(t)) end,
       :jiffy},

      {"jason",
       &Jason.decode!/1,
       &Jason.encode!/1,
       Jason},

      {"json",
       fn b -> :json.decode(b) end,
       fn t -> IO.iodata_to_binary(:json.encode(t)) end,
       :json},

      {"thoas",
       fn b -> {:ok, r} = :thoas.decode(b); r end,
       &:thoas.encode/1,
       :thoas},

      {"euneus",
       &:euneus.decode/1,
       &:euneus.encode/1,
       :euneus},
    ]

    optional = Enum.flat_map(optional_candidates, fn {name, dec, enc, mod} ->
      if module_available?(mod) do
        [{name, dec, enc}]
      else
        []
      end
    end)

    base ++ optional
  end

  defp module_available?(mod) do
    case Code.ensure_loaded(mod) do
      {:module, _} -> true
      _            -> false
    end
  end

  defp top_limit(args) do
    value = top_arg(args) || System.get_env("TOP")

    case value do
      nil -> nil
      value ->
        case Integer.parse(value) do
          {n, ""} when n > 0 -> n
          {n,  _} when n > 0 -> n
          _ -> nil
        end
    end
  end

  defp top_arg(["--top=" <> n | _]), do: n
  defp top_arg(["--top", n | _]), do: n
  defp top_arg([_ | rest]), do: top_arg(rest)
  defp top_arg([]), do: nil

  # ---------------------------------------------------------------------------
  # Data loading
  # ---------------------------------------------------------------------------

  defp load_files do
    root = File.cwd!()
    Enum.flat_map(@data_files, fn {name, rel_path} ->
      path = Path.join(root, rel_path)
      case File.read(path) do
        {:ok, bin} ->
          kb    = Float.round(byte_size(bin) / 1024, 1)
          label = "#{name} (#{kb}K)"
          [{label, bin}]
        {:error, reason} ->
          Mix.shell().info("Skipping #{path}: #{inspect(reason)}")
          []
      end
    end)
  end

  # ---------------------------------------------------------------------------
  # Benchmark runner
  # ---------------------------------------------------------------------------

  @iteration_thresholds [{200_000, 100}, {10_000, 250}]
  @default_iterations   5_000

  defp iterations(size) do
    Mix.Tasks.Bench.Common.iterations(size, @iteration_thresholds, @default_iterations)
  end

  defp run_file(bin, suites, parallelism) do
    n       = iterations(byte_size(bin))
    parent  = self()
    ref     = make_ref()
    timeout = n * 200 + 10_000

    suites
    |> Enum.chunk_every(parallelism)
    |> Enum.flat_map(fn chunk ->
      tasks = Enum.map(chunk, fn {name, decode, encode} ->
        pid = spawn(fn ->
          result =
            try do
              decoded = decode.(bin)
              Mix.Tasks.Bench.Common.repeat(div(n, 10), fn -> encode.(decode.(bin)) end)
              dt = Mix.Tasks.Bench.Common.measure(n, fn -> decode.(bin) end)
              et = Mix.Tasks.Bench.Common.measure(n, fn -> encode.(decoded) end)
              {:ok, dt, et}
            rescue
              e -> {:error, Exception.message(e)}
            catch
              kind, reason -> {:error, "#{kind}: #{inspect(reason)}"}
            end
          send(parent, {ref, name, result})
        end)
        {name, pid}
      end)

      Enum.map(tasks, fn {name, _pid} ->
        receive do
          {^ref, ^name, result} -> {name, result}
        after
          timeout -> {name, {:error, "timeout"}}
        end
      end)
    end)
  end

  # ---------------------------------------------------------------------------
  # Table printer
  # ---------------------------------------------------------------------------

  defp print_table(suites, files_data) do
    lib_w = @lib_w
    col_w = @col_w
    sep   = @sep

    # group width: "dec  enc  " + sep
    group_w = col_w * 2 + 2 + sep

    IO.puts("")
    IO.puts("(numbers in µs)")

    # Header line 1: file labels centred over each group
    pad = String.duplicate(" ", lib_w-4)
    header1 = Enum.map_join(files_data, "", fn {label, _} ->
      "  " <> centre(label, group_w)
    end)
    IO.puts("JSON" <> pad <> header1)

    # Header line 2: decode / encode sub-columns
    sub = Enum.map_join(files_data, "", fn _ ->
      "  " <>
      String.pad_leading("decode", col_w) <>
      "  " <>
      String.pad_leading("encode", col_w) <>
      String.duplicate(" ", sep)
    end)
    IO.puts("    " <> pad <> sub)

    # Separator line
    total_w = lib_w + (group_w + 2) * length(files_data)
    IO.puts(String.duplicate("-", total_w))

    # Data rows
    Enum.each(suites, fn {name, _dec, _enc} ->
      row = String.pad_trailing(name, lib_w)
      cols = Enum.map_join(files_data, "", fn {_label, results} ->
        case List.keyfind(results, name, 0) do
          {_, {:ok, dt, et}} ->
            "  " <>
            String.pad_leading(:io_lib.format("~.1f", [dt]) |> IO.iodata_to_binary(), col_w) <>
            "  " <>
            String.pad_leading(:io_lib.format("~.1f", [et]) |> IO.iodata_to_binary(), col_w) <>
            String.duplicate(" ", sep)

          {_, {:error, "timeout"}} ->
            "  " <>
            String.pad_leading("TIMEOUT", col_w) <>
            "  " <>
            String.pad_leading("TIMEOUT", col_w) <>
            String.duplicate(" ", sep)

          {_, {:error, reason}} ->
            msg = String.slice(to_string(reason), 0, col_w * 2 + 1)
            "  " <> String.pad_trailing(msg, col_w * 2 + 1) <> String.duplicate(" ", sep)

          nil ->
            "  " <>
            String.pad_leading("n/a", col_w) <>
            "  " <>
            String.pad_leading("n/a", col_w) <>
            String.duplicate(" ", sep)
        end
      end)
      IO.puts(row <> cols)
    end)

    IO.puts("")
  end

  defp centre(str, width) do
    len = String.length(str)
    if len >= width do
      str
    else
      left  = div(width - len, 2)
      right = width - len - left
      String.duplicate(" ", left) <> str <> String.duplicate(" ", right)
    end
  end
end
