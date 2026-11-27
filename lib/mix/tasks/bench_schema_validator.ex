defmodule Mix.Tasks.BenchSchemaValidator do
  @shortdoc "Benchmark glazer's JSON Schema validator against other Elixir validators"
  @moduledoc """
  Run a pivoted performance benchmark comparing glazer's schema validator
  against other JSON Schema libraries for the BEAM.

      mix bench-schema-validator

  Libraries benchmarked (when available):
    - glazer         (this library - C++ NIF, compiled schema)
    - ex_json_schema (pure Elixir)
    - json_xema      (pure Elixir, built on xema)
    - jsv            (pure Elixir, JSON Schema 2020-12)
    - exonerate      (pure Elixir, compiles the schema to a module)

  elixir-europe/json-schema-validator is not included: it is a JavaScript
  (npm/AJV) package, archived in favour of elixir-europe/biovalidator, and
  has no BEAM implementation to benchmark.

  Each library compiles the schema repeatedly and reports the average time
  (`compile`). Each document is then validated against the compiled schema
  (`validate`, averaged over many runs).
  Decoding the schema and the document is not included in either timing.
  Every library must accept the document before it is timed; a library that
  rejects it, or fails to compile the schema, shows its error in the table.

  The schemas use the subset of JSON Schema keywords that all libraries
  support (`type`, `properties`, `required`, `items`, `enum`, `minimum`,
  `maximum`, `minLength`, `maxLength`, `minItems`, ...). They do not use
  `$schema`, `$defs` or `format`, so every library checks the same rules.

  Output: one pivoted table, libraries as rows, files x {compile, validate}
  as column pairs.
  """
  use Mix.Task

  @compile {:no_warn_undefined, [
    Exonerate,
    ExJsonSchema.Schema,
    ExJsonSchema.Validator,
    JsonXema,
    JSV
  ]}

  @lib_w 16
  @col_w 9
  @sep   2

  # {label, data file, schema file}
  @data_files [
    {"twitter", "test/data/twitter.json", "test/data/schemas/twitter.schema.json"},
    {"openrtb", "test/data/openrtb.json", "test/data/schemas/openrtb.schema.json"},
    {"small",   "test/data/small.json",   "test/data/schemas/small.schema.json"}
  ]

  @compile_thresholds [{200_000, 10}]
  @default_compile_iterations 200
  @validate_thresholds [{200_000, 100}, {10_000, 1_000}]
  @default_validate_iterations 5_000

  @impl Mix.Task
  def run(_args) do
    Mix.env() != :bench && Mix.raise("mix benchmark-sv must be run with MIX_ENV=bench")
    Code.put_compiler_option(:ignore_module_conflict, true)

    # Ensure all deps are started so NIFs get loaded.
    Mix.Task.run("app.start")

    suites = build_suites()
    files  = load_files()

    if files == [] do
      Mix.shell().error("No schema test data files found in test/data/")
    else
      Mix.Tasks.Bench.Common.announce()

      files_data = Enum.map(files, fn {label, schema, data_bin, size} ->
        {label, run_file(schema, data_bin, size, suites)}
      end)

      print_table(suites, files_data)
    end
  end

  # ---------------------------------------------------------------------------
  # Suite builder
  #
  # Each suite is {name, compile, validate}:
  #   compile.(schema)        -> handle, raises on an invalid schema
  #   validate.(handle, data) -> :ok | {:error, reason}
  # ---------------------------------------------------------------------------

  defp build_suites do
    base = [
      {"glazer",
       fn {schema, _json} ->
         {:ok, handle} = :glazer_schema.compile(schema, [{:registry, false}])
         handle
       end,
       fn handle, data -> :glazer_schema.validate(handle, data) end}
    ]

    optional_candidates = [
      {"ex_json_schema",
       fn {schema, _json} -> ExJsonSchema.Schema.resolve(schema) end,
       fn root, data -> ExJsonSchema.Validator.validate(root, data) end,
       ExJsonSchema.Schema},

      {"json_xema",
       fn {schema, _json} -> JsonXema.new(schema, []) end,
       fn xema, data -> JsonXema.validate(xema, data) end,
       JsonXema},

      {"jsv",
       fn {schema, _json} -> JSV.build!(schema) end,
       fn root, data ->
         case JSV.validate(data, root) do
           {:ok, _}   -> :ok
           {:error, _} = error -> error
         end
       end,
       JSV},

      {"exonerate",
       fn {_schema, json} -> compile_exonerate(json) end,
       fn mod, data -> apply(mod, :validate_input, [data]) end,
       Exonerate}
    ]

    optional = Enum.flat_map(optional_candidates, fn {name, compile, validate, mod} ->
      if module_available?(mod), do: [{name, compile, validate}], else: []
    end)

    base ++ optional
  end

  # Exonerate generates a module from the schema string at compile time. It caches
  # the generated helpers by schema hash, so compiling a second module with the same
  # name fails. Each compile therefore gets a fresh module name.
  defp compile_exonerate(json) do
    mod  = Module.concat([Exonerate, Bench, "Schema#{System.unique_integer([:positive])}"])
    code = "defmodule #{inspect(mod)} do\n  require Exonerate\n  Exonerate.function_from_string(:def, :validate_input, #{inspect(json)})\nend"
    Code.compile_string(code)
    mod
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

  # Returns [{label, {schema_term, schema_json}, data_term, size}]. The data is decoded
  # with glazer_json so every library gets the same map/list term with binary keys.
  defp load_files do
    root = File.cwd!()

    Enum.flat_map(@data_files, fn {name, data_rel, schema_rel} ->
      with {:ok, data_bin}   <- File.read(Path.join(root, data_rel)),
           {:ok, schema_bin} <- File.read(Path.join(root, schema_rel)) do
        kb    = Float.round(byte_size(data_bin) / 1024, 1)
        label = "#{name} (#{kb}K)"
        schema = :glazer_json.decode(schema_bin)
        data   = :glazer_json.decode(data_bin)
        [{label, {schema, schema_bin}, data, byte_size(data_bin)}]
      else
        {:error, reason} ->
          Mix.shell().info("Skipping #{name}: #{inspect(reason)}")
          []
      end
    end)
  end

  # ---------------------------------------------------------------------------
  # Benchmark runner
  # ---------------------------------------------------------------------------

  defp compile_iterations(size) do
    Mix.Tasks.Bench.Common.iterations(size, @compile_thresholds, @default_compile_iterations)
  end

  defp validate_iterations(size) do
    Mix.Tasks.Bench.Common.iterations(size, @validate_thresholds, @default_validate_iterations)
  end

  # Exonerate's compile of a large schema takes seconds, so a per-iteration budget
  # would time out. This fixed budget per library only guards against a hang.
  @suite_timeout_ms 600_000

  defp run_file(schema, data, size, suites) do
    n_compile  = compile_iterations(size)
    n_validate = validate_iterations(size)

    Mix.Tasks.Bench.Common.run_each(suites, @suite_timeout_ms, fn {_name, compile, validate} ->
      try do
        handle = compile.(schema)

        # Check the document is accepted before timing, so an error never looks fast.
        case validate.(handle, data) do
          :ok -> :ok
          {:error, reason} -> throw({:invalid, reason})
        end

        ct = Mix.Tasks.Bench.Common.measure(n_compile, fn -> compile.(schema) end)
        vt = Mix.Tasks.Bench.Common.measure(n_validate, fn -> validate.(handle, data) end)
        {:ok, ct, vt}
      rescue
        e -> {:error, Exception.message(e)}
      catch
        {:invalid, reason} -> {:error, "invalid: #{inspect(reason)}"}
        kind, reason       -> {:error, "#{kind}: #{inspect(reason)}"}
      end
    end)
  end

  # ---------------------------------------------------------------------------
  # Table printer
  # ---------------------------------------------------------------------------

  defp print_table(suites, files_data) do
    lib_w = @lib_w
    col_w = @col_w
    sep   = @sep

    # group width: "compile  validate  " + sep
    group_w = col_w * 2 + 2 + sep

    IO.puts("")
    IO.puts("(numbers in µs)")

    pad = String.duplicate(" ", lib_w - 7)
    header1 = Enum.map_join(files_data, "", fn {label, _} ->
      "  " <> centre(label, group_w)
    end)
    IO.puts("library" <> pad <> header1)

    sub = Enum.map_join(files_data, "", fn _ ->
      "  " <>
      String.pad_leading("compile", col_w) <>
      "  " <>
      String.pad_leading("validate", col_w) <>
      String.duplicate(" ", sep)
    end)
    IO.puts(String.duplicate(" ", lib_w) <> sub)

    total_w = lib_w + (group_w + 2) * length(files_data)
    IO.puts(String.duplicate("-", total_w))

    Enum.each(suites, fn {name, _compile, _validate} ->
      row = String.pad_trailing(name, lib_w)
      cols = Enum.map_join(files_data, "", fn {_label, results} ->
        case List.keyfind(results, name, 0) do
          {_, {:ok, ct, vt}} ->
            "  " <>
            String.pad_leading(format_time(ct), col_w) <>
            "  " <>
            String.pad_leading(format_time(vt), col_w) <>
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

  defp format_time(t), do: :io_lib.format("~.1f", [t]) |> IO.iodata_to_binary()

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
