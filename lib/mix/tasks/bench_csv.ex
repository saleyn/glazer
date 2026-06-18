defmodule Mix.Tasks.BenchCsv do
  @shortdoc "Benchmark glazer's CSV codec against other CSV libraries"
  @moduledoc """
  Run a pivoted performance benchmark comparing glazer against other CSV
  libraries.

      mix bench-csv

  Libraries benchmarked (when available):
    - glazer      (this library - C++ NIF)
    - nimble_csv  (pure Elixir, RFC 4180)
    - csv         (pure Elixir)
    - erl_csv     (pure Erlang)

  Output: one pivoted table — libraries as rows, files×{decode,encode} as column pairs.
  """
  use Mix.Task

  @lib_w 11
  @col_w 9
  @sep   2

  @data_files [
    {"small",  "test/data/small.csv"},
    {"medium", "test/data/medium.csv"},
    {"large",  "test/data/large.csv"}
  ]

  @impl Mix.Task
  def run(args) do
    Mix.env() != :bench && Mix.raise("mix bench-csv must be run with MIX_ENV=bench")

    # Ensure all deps are started so NIFs get loaded.
    Mix.Task.run("app.start")

    suites = build_suites()
    files  = load_files()

    if files == [] do
      Mix.shell().error("No CSV test data files found in test/data/")
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

  if Code.ensure_loaded?(NimbleCSV) do
    NimbleCSV.define(GlazerBench.NimbleCSVParser, separator: ",", escape: "\"")
  end

  defp build_suites do
    base = [
      {"glazer",
       &:glazer_csv.decode/1,
       fn %{data: rows} -> :glazer_csv.encode(rows) end}
    ]

    optional_candidates = [
      {"rusty_csv",
       fn b -> RustyCSV.RFC4180.parse_string(b) end,
       nil,
       RustyCSV.RFC4180},

      {"nimble_csv",
       fn b -> GlazerBench.NimbleCSVParser.parse_string(b, skip_headers: false) end,
       fn rows -> IO.iodata_to_binary(GlazerBench.NimbleCSVParser.dump_to_iodata(rows)) end,
       NimbleCSV},

      {"csv",
       fn b ->
         b
         |> String.split("\r\n")
         |> Enum.reject(&(&1 == ""))
         |> Enum.map(&(&1 <> "\n"))
         |> CSV.decode!(headers: false)
         |> Enum.to_list()
       end,
       fn rows ->
         rows
         |> CSV.encode()
         |> Enum.to_list()
         |> IO.iodata_to_binary()
       end,
       CSV},

      {"erl_csv",
       fn b ->
         case :erl_csv.decode(b) do
           {:ok, rows}              -> rows
           {:has_trailer, rows, _t} -> rows
         end
       end,
       fn rows -> IO.iodata_to_binary(:erl_csv.encode(rows)) end,
       :erl_csv},
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

  defp module_available?(mod) when is_atom(mod) do
    case Code.ensure_loaded(mod) do
      {:module, _} -> true
      _            -> false
    end
  end

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

  @iteration_thresholds [{1_000_000, 10}, {10_000, 100}]
  @default_iterations   2_000

  defp iterations(size) do
    Mix.Tasks.Bench.Common.iterations(size, @iteration_thresholds, @default_iterations)
  end

  defp run_file(bin, suites, parallelism) do
    n       = iterations(byte_size(bin))
    parent  = self()
    ref     = make_ref()
    timeout = n * 500 + 10_000

    suites
    |> Enum.chunk_every(parallelism)
    |> Enum.flat_map(fn chunk ->
      tasks = Enum.map(chunk, fn {name, decode, encode} ->
        pid = spawn(fn ->
          result =
            try do
              dt = Mix.Tasks.Bench.Common.measure(n, fn -> decode.(bin) end)
              et =
                if is_function(encode) do
                  decoded = decode.(bin)
                  Mix.Tasks.Bench.Common.measure(n, fn -> encode.(decoded) end)
                else
                  :na
                end
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
    pad = String.duplicate(" ", lib_w-3)
    header1 = Enum.map_join(files_data, "", fn {label, _} ->
      "  " <> centre(label, group_w)
    end)
    IO.puts("CSV" <> pad <> header1)

    # Header line 2: decode / encode sub-columns
    sub = Enum.map_join(files_data, "", fn _ ->
      "  " <>
      String.pad_leading("decode", col_w) <>
      "  " <>
      String.pad_leading("encode", col_w) <>
      String.duplicate(" ", sep)
    end)
    IO.puts("   " <> pad <> sub)

    # Separator line
    total_w = lib_w + (group_w + 2) * length(files_data)
    IO.puts(String.duplicate("-", total_w))

    # Data rows
    Enum.each(suites, fn {name, _dec, _enc} ->
      row = String.pad_trailing(name, lib_w)
      cols = Enum.map_join(files_data, "", fn {_label, results} ->
        case List.keyfind(results, name, 0) do
          {_, {:ok, dt, et}} ->
            et_str =
              case et do
                :na -> "n/a"
                _   -> :io_lib.format("~.1f", [et]) |> IO.iodata_to_binary()
              end
            "  " <>
            String.pad_leading(:io_lib.format("~.1f", [dt]) |> IO.iodata_to_binary(), col_w) <>
            "  " <>
            String.pad_leading(et_str, col_w) <>
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
