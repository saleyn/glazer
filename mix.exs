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

  # yaml_rustler and rusty_csv both depend (transitively) on incompatible
  # versions of :rustler (~> 0.34.0 vs ~> 0.37), so they can never be
  # resolved together. Pick which one to fetch/compile via BENCH_SET:
  #
  #   BENCH_SET=yaml mix deps.get   # pulls yaml_rustler, skips rusty_csv
  #   BENCH_SET=csv  mix deps.get   # pulls rusty_csv, skips yaml_rustler
  defp deps do
    [
      {:simdjsone,    "~> 0.5",    only: :bench},
      {:jason,        "~> 1.4",    only: :bench},
      {:jiffy,        "~> 2.0",    only: :bench},
      {:thoas,        "~> 1.2",    only: :bench},
      {:euneus,       "~> 2.0",    only: :bench},
      {:torque,       "~> 0.2.1",  only: :bench},
      {:yamerl,       "~> 0.10",   only: :bench},
      {:fast_yaml,    "~> 1.0",    only: :bench},
      {:ymlr,         "~> 5.1",    only: :bench},
      {:csv,          "~> 3.2",    only: :bench},
      {:nimble_csv,   "~> 1.3",    only: :bench},
      {:erl_csv,      "~> 0.3.3",  only: :bench},
    ] ++ bench_set_deps()
  end

  defp bench_set_deps do
    case System.get_env("BENCH_SET") do
      "yaml" -> [{:yaml_rustler, "~> 0.1.6",  only: :bench}]
      "csv"  -> [{:rusty_csv,    "~> 0.3.11", only: :bench}]
      _      -> []
    end
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
