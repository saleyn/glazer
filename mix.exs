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
      compilers:             [:erlang, :elixir, :app],
      consolidate_protocols: consolidate_protocols(),
      elixirc_paths:         elixirc_paths(Mix.env()),
      docs:                  docs(),
      test_coverage:         test_coverage()
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

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

  # Compile benchmark tasks only in :bench environment
  defp elixirc_paths(:bench), do: ["lib"]
  defp elixirc_paths(_), do: ["lib/glazer"]

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
      {:ex_doc, "~> 0.34", only: :dev, runtime: false},
      # Benchmarking dependencies
      {:simdjsone,           "~> 0.5",    only: :bench},
      {:jason,               "~> 1.4",    only: :bench},
      {:jiffy,               "~> 2.0.2",  only: :bench},
      {:thoas,               "~> 1.2",    only: :bench},
      {:euneus,              "~> 2.0",    only: :bench},
      {:torque,              "~> 0.4.1",  only: :bench},
      {:yamerl,              "~> 0.10",   only: :bench},
      {:fast_yaml,           "~> 1.0",    only: :bench},
      {:ymlr,                "~> 5.1",    only: :bench},
      {:csv,                 "~> 3.2",    only: :bench},
      {:nimble_csv,          "~> 1.3",    only: :bench},
      {:erl_csv,             "~> 0.6.0",  only: :bench},
      {:yaml_rustler,        "~> 0.1.6",  only: :bench},
      {:rusty_csv,           "~> 0.4.6",  only: :bench},
      {:rustler,             "~> 0.38",   only: :bench, override: true, runtime: false},
      {:rustler_precompiled, "~> 0.9",    only: :bench, override: true},
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
      main:          "README.md",
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
