defmodule Mix.Tasks.BenchYaml do
  @shortdoc "Benchmark glazer's YAML decoder against other YAML libraries"
  @moduledoc """
  Run a pivoted performance benchmark comparing glazer against other YAML
  libraries.

      mix bench-yaml

  Libraries benchmarked (when available):
    - glazer      (this library - C++ NIF)
    - fast_yaml   (libyaml NIF)
    - yamerl      (pure Erlang, decode only)
    - ymlr        (pure Elixir, encode only)
    - yaml_rustler (Rust yaml-rust2 via Rustler, decode only)

  Output: one pivoted table — libraries as rows, files×{decode,encode} as column pairs.
  yamerl has no YAML encoder and ymlr has no YAML decoder, so the
  corresponding column shows `n/a`. ymlr's encode benchmark uses glazer's
  decoder to produce the term to encode.
  """
  use Mix.Task

  @lib_w 13
  @col_w 7
  @sep   2

  @data_files [
    {"openrtb", "test/data/openrtb.yaml"},
    {"esad",    "test/data/esad.yaml"},
    {"small",   "test/data/small.yaml"}
  ]

  @impl Mix.Task
  def run(args) do
    Mix.env() != :bench && Mix.raise("mix bench-yaml must be run with MIX_ENV=bench")

    # Ensure all deps are started so NIFs get loaded.
    Mix.Task.run("app.start")

    suites = build_suites()
    files  = load_files()

    if files == [] do
      Mix.shell().error("No YAML test data files found in test/data/")
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

  defp build_suites do
    base = [
      {"glazer", &:glazer_yaml.decode/1, fn t -> :glazer_yaml.encode(t) end}
    ]

    optional_candidates = [
      {"yaml_rustler",
       fn b ->
         {:ok, doc} = YamlRustler.parse(b)
         doc
       end,
       :no_encode,
       YamlRustler},

       {"fast_yaml",
       fn b ->
         {:ok, [doc | _]} = :fast_yaml.decode(b)
         doc
       end,
       fn t ->
         {:ok, iodata} = :fast_yaml.encode(t)
         IO.iodata_to_binary(iodata)
       end,
       :fast_yaml},

      {"yamerl",
       fn b ->
         [doc | _] = :yamerl_constr.string(to_charlist(b))
         doc
       end,
       :no_encode,
       :yamerl_constr},

      {"ymlr",
       :no_decode,
       fn t -> Ymlr.document!(t) end,
       Ymlr},
    ]

    optional = Enum.flat_map(optional_candidates, fn
      {name, dec, enc, mod} ->
        if module_available?(mod), do: [{name, dec, enc}], else: []
    end)

    base ++ optional
  end

  defp module_available?(mod) do
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

  defp iterations(size) when size > 200_000, do: 200
  defp iterations(size) when size >  10_000, do: 1_000
  defp iterations(_),                        do: 5_000

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
              {dt, decoded} =
                case decode do
                  :no_decode -> {:na, :glazer_yaml.decode(bin)}
                  decode_fun -> {Mix.Tasks.Bench.Common.measure(n, fn -> decode_fun.(bin) end), decode_fun.(bin)}
                end
              et =
                case encode do
                  :no_encode -> :na
                  encode_fun -> Mix.Tasks.Bench.Common.measure(n, fn -> encode_fun.(decoded) end)
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
    pad = String.duplicate(" ", lib_w-4)
    header1 = Enum.map_join(files_data, "", fn {label, _} ->
      "  " <> centre(label, group_w)
    end)
    IO.puts("YAML" <> pad <> header1)

    # Header line 2: decode / encode sub-columns
    sub = Enum.map_join(files_data, "", fn _ ->
      "  " <>
      String.pad_leading("decode", col_w) <>
      "  " <>
      String.pad_leading("encode", col_w) <>
      String.duplicate(" ", sep)
    end)
    IO.puts("    " <>pad <> sub)

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
            String.pad_leading(format_time(dt), col_w) <>
            "  " <>
            String.pad_leading(format_time(et), col_w) <>
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

  defp format_time(:na), do: "n/a"
  defp format_time(t),   do: :io_lib.format("~.1f", [t]) |> IO.iodata_to_binary()

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
