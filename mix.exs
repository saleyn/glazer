defmodule GlazerBench.MixProject do
  use Mix.Project

  def project do
    [
      app:     :glazer,
      version: "0.1.0",
      elixir:  "~> 1.15",
      deps:    deps(),
      aliases: aliases()

    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [
      {:simdjsone,    "~> 0.5",   only: :bench},
      {:jason,        "~> 1.4",   only: :bench},
      {:jiffy,        "~> 2.0",   only: :bench},
      {:thoas,        "~> 1.2",   only: :bench},
      {:euneus,       "~> 2.0",   only: :bench},
      {:torque,       "~> 0.1.9", only: :bench},
      {:yamerl,       "~> 0.10",  only: :bench},
      {:fast_yaml,    "~> 1.0",   only: :bench},
      {:ymlr,         "~> 5.1",   only: :bench},
      {:yaml_rustler, "~> 0.1.6", only: :bench},
      {:csv,          "~> 3.2",   only: :bench},
      {:nimble_csv,   "~> 1.3",   only: :bench},
      {:erl_csv,      "~> 0.3.3", only: :bench}
    ]
  end

  def aliases do
    [
      bench:        ["bench-json", "bench-yaml", "bench-csv"],
      "bench-json": "bench_json --only bench",
      "bench-yaml": "bench_yaml --only bench",
      "bench-csv":  "bench_csv --only bench"
    ]
  end
end
