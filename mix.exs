defmodule Glazer.MixProject do
  use Mix.Project

  def project do
    [
      app:                   :glazer,
      version:               version(),
      elixir:                "~> 1.15",
      deps:                  deps(),
      aliases:               aliases(),
      language:              :erlang,
      compilers:             [:make] ++ Mix.compilers(),
      consolidate_protocols: consolidate_protocols(),
      elixirc_paths:         elixirc_paths(Mix.env()),
      # Exclude bench_*.ex files from compilation outside of :bench env
      docs:                  docs(),
      test_coverage:         test_coverage()
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  # Benchmark task files live under lib/mix/tasks and must be compiled for
  # both :bench and :test so the task-level regression tests can load them.
  defp elixirc_paths(:bench), do: ["lib"]
  defp elixirc_paths(:test),  do: ["lib", "lib/glazer"]
  defp elixirc_paths(_),      do: ["lib/glazer"]

  # Disable protocol consolidation in dev/test so @derive works properly for
  # structs defined outside lib/ (e.g. in `mix run script.exs`, `mix run -e`,
  # or `iex -S mix`) — consolidation freezes the known-implementations list
  # at the end of `mix compile`, so anything @derive'd afterward would
  # otherwise raise Protocol.UndefinedError despite having a real impl.
  defp consolidate_protocols do
    Mix.env() not in [:dev, :test]
  end

  # Read the version from src/glazer.app.src (the single source of truth,
  # also used by rebar3/hex.pm for the Erlang-side release) instead of
  # duplicating it here. `.app.src` is an Erlang term file, so it's parsed
  # with `:file.consult/1` rather than as Elixir source.
  defp version do
    {:ok, [{:application, :glazer, opts}]} = :file.consult(~c"src/glazer.app.src")

    opts
    |> Keyword.fetch!(:vsn)
    |> List.to_string()
  end

  # yaml_rustler pins {:rustler, "~> 0.34.0"} and {:rustler_precompiled, "~> 0.8.2"}
  # while rusty_csv pins {:rustler, "~> 0.37.3"} and {:rustler_precompiled, "~> 0.9"} —
  # left alone, Hex can't resolve one :rustler/:rustler_precompiled version that
  # satisfies both. Both are compile-time-only deps (used solely to generate the
  # NIF stub via `use RustlerPrecompiled`; the actual NIF is a precompiled .so
  # fetched at compile time), so forcing a single shared version via `override:
  # true` is safe — confirmed both yaml_rustler and rusty_csv compile and load
  # their precompiled NIFs correctly under rustler 0.37.3 / rustler_precompiled 0.9.
  defp deps do
    [
      # mix docs
      {:ex_doc,              "~> 0.34",  only: :dev, runtime: false},
      # Benchmarking dependencies
      {:simdjsone,           "~> 0.5",   only: :bench},
      {:jason,               "~> 1.4",   only: :bench},
      {:jiffy,               "~> 2.0.2", only: :bench},
      {:thoas,               "~> 1.2",   only: :bench},
      {:euneus,              "~> 2.0",   only: :bench},
      {:torque,              "~> 0.4.5", only: :bench},
      {:yamerl,              "~> 0.10",  only: :bench},
      {:fast_yaml,           "~> 1.0",   only: :bench},
      {:ymlr,                "~> 5.1",   only: :bench},
      {:csv,                 "~> 3.2",   only: :bench},
      {:nimble_csv,          "~> 1.3",   only: :bench},
      {:erl_csv,             "~> 0.6.0", only: :bench},
      {:yaml_rustler,        "~> 0.1.6", only: :bench},
      {:rusty_csv,           "~> 0.4.6", only: :bench},
      {:rustler,             "~> 0.38",  only: :bench, override: true, runtime: false},
      {:rustler_precompiled, "~> 0.9",   only: :bench, override: true},
    ]
  end

  def aliases do
    [
      bench:        ["bench-json", "bench-yaml", "bench-csv"],
      "bench-json": "bench_json --only bench",
      "bench-yaml": "bench_yaml --only bench",
      "bench-csv":  "bench_csv  --only bench"
    ]
  end

  def test_coverage do
    [
      summary: [threshold: 90],
      # Ignore Elixir wrapper modules and protocols to get accurate coverage
      # of the core Erlang implementation. The Elixir modules are thin wrappers
      # that add minimal logic, so excluding them gives a clearer picture of
      # implementation coverage.
      ignore_modules: [
        :glazer,
        :glazer_csv,
        :glazer_yaml,
        :glazer_json,
        :"Elixir.Glazer.JSON.Encoder.Any",
        :"Elixir.Glazer.JSON.Encoder.Atom",
        :"Elixir.Glazer.JSON.Encoder.BitString",
        :"Elixir.Glazer.JSON.Encoder.Date",
        :"Elixir.Glazer.JSON.Encoder.DateTime",
        :"Elixir.Glazer.JSON.Encoder.Float",
        :"Elixir.Glazer.JSON.Encoder.Integer",
        :"Elixir.Glazer.JSON.Encoder.List",
        :"Elixir.Glazer.JSON.Encoder.Map",
        :"Elixir.Glazer.JSON.Encoder.NaiveDateTime",
        :"Elixir.Glazer.JSON.Encoder.Time",
        :"Elixir.Glazer.JSON.Encoder.DeriveHelper",
        :"Elixir.Jason.Encoder",
        :"Elixir.Jason.Encoder.Any"
      ]
    ]
  end

  def docs do
    [
      extras: [
        "README.md":  %{title: "Overview"},
        "LICENSE":    %{title: "License"},
        "RELEASE.md": %{title: "Release"}
      ],
      main:          "readme",
      source_url:    "https://github.com/saleyn/glazer",
      #assets:        %{assets: "assets/"},
      groups_for_extras: [
        %{Release: "release"}
      ],
      groups_for_modules: [
        Erlang:        ~r/glazer/,
        Elixir:        ~r/Glazer/,
        Miscellaneous: ~r/Jason/
      ],
      skip_undefined_reference_warnings_on: [
        :glazer,
        :glazer_csv,
        :glazer_json,
        :glazer_yaml,
        "README.md"
      ]
    ]
  end
end

defmodule Mix.Tasks.Compile.Make do
  use Mix.Task
  @moduledoc false

  @shortdoc "Compiles Erlang and NIF sources via Makefile"

  def run(_args) do
    app_path = Mix.Project.app_path()

    # Detect dependency build: Check if the app_path is pointing to a _build directory
    # that's NOT in the current project's _build.
    # In main project: app_path starts with _build/dev/lib/glazer from cwd=/path/to/glazer
    # In dependency: app_path starts with /path/to/integration/_build/dev/lib/glazer
    # Key difference: in dependency, app_path is absolute and outside our project directory

    #is_dependency_build = String.starts_with?(app_path, "/") and
    #                      not String.contains?(app_path, File.cwd!() <> "/_build")

    #optimize_env = System.get_env("OPTIMIZE", "0")

    # When building as a dependency, use compile target (not optimize) to skip PGO entirely
    # since pgo-profile.es isn't included in Hex/GitHub distributions
    # For the main project with OPTIMIZE=1, use the optimize target
    #target = if is_dependency_build or optimize_env != "1" do
    target = System.get_env("OPTIMIZE", "0") == "1" && "optimize" || "compile"

    # Set REBAR_BARE_COMPILER_OUTPUT_DIR so the Makefile puts priv files in the correct location
    # For dependency builds, force OPTIMIZE=0 to skip PGO
    # For the main project, inherit the OPTIMIZE environment variable
    env = [
      {"REBAR_BARE_COMPILER_OUTPUT_DIR", app_path},
      {"MIX_ENV", Mix.env() |> to_string()}
    ]

    # Build a full environment:
    # - Always remove OPTIMIZE from the inherited environment to prevent shell override
    # - Add back our version of OPTIMIZE (which is 0 for deps, or from shell for main project)
    #full_env = System.get_env() |> Map.to_list() |> Enum.reject(fn {k, _} -> k == "OPTIMIZE" end)

    # Add our controlled OPTIMIZE value
    #full_env = full_env ++ [{"OPTIMIZE", if(is_dependency_build, do: "0", else: optimize_env)}] ++
    #                        Enum.reject(env, fn {k, _} -> k == "OPTIMIZE" end)

    # Get the source directory from app_path (e.g., _build/dev/lib/glazer -> deps/glazer or integrate root)
    # We need to find the actual source directory containing the Makefile
    source_dir = if File.exists?("Makefile") do
      # We're in the main Glazer project
      "."
    else
      # We're in a dependent project - find Glazer's source
      # Mix stores dependencies in deps/<app_name> for path/git deps
      case File.ls("deps") do
        {:ok, deps} -> if "glazer" in deps, do: "deps/glazer", else: "."
        {:error, _} -> "."
      end
    end

    case System.cmd("make", [target], cd: source_dir, env: env, into: IO.stream(:stdio, :line)) do
      {_,         0} -> :ok
      {_, exit_code} -> {:error, ["Make failed with exit code #{exit_code}"]}
    end
  end
end
