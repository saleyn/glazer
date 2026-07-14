defmodule Glazer.MixProject do
  use Mix.Project

  def project do
    [
      app:      :glazer,
      version:  "0.1.0",
      elixir:   "~> 1.15",
      deps:     deps(),
      aliases:  aliases(),
      language: :erlang
    ]
  end

  def application do
    [extra_applications: [:logger]]
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
      {:simdjsone,           "~> 0.5",    only: :bench},
      {:jason,               "~> 1.4",    only: :bench},
      {:jiffy,               "~> 2.0.2",  only: :bench},
      {:thoas,               "~> 1.2",    only: :bench},
      {:euneus,              "~> 2.0",    only: :bench},
      {:torque,              "~> 0.2.1",  only: :bench},
      {:yamerl,              "~> 0.10",   only: :bench},
      {:fast_yaml,           "~> 1.0",    only: :bench},
      {:ymlr,                "~> 5.1",    only: :bench},
      {:csv,                 "~> 3.2",    only: :bench},
      {:nimble_csv,          "~> 1.3",    only: :bench},
      {:erl_csv,             "~> 0.3.3",  only: :bench},
      {:yaml_rustler,        "~> 0.1.6",  only: :bench},
      {:rusty_csv,           "~> 0.3.11", only: :bench},
      {:rustler,             "~> 0.37.3", only: :bench, override: true, runtime: false},
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
end
