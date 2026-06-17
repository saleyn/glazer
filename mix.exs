defmodule Glazer.MixProject do
  use Mix.Project

  def project do
    [
      app:      :glazer,
      version:  "0.1.0",
      elixir:   "~> 1.15",
      deps:     deps(),
      aliases:  aliases(),
      language: :erlang,
      # yaml_rustler/rusty_csv are conditionally fetched (see bench_set_deps/0
      # below); when skipped, the bench task modules that reference them
      # directly (bench_yaml.ex, bench_csv.ex) would otherwise trigger
      # "module is undefined" compiler warnings, which warn_skipped_bench_dep/2
      # already explains on screen — so suppress the warnings themselves here.
      elixirc_options: [no_warn_undefined: [YamlRustler, RustyCSV.RFC4180]]
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

  # Emitted (once, before deps are fetched/compiled) whenever BENCH_SET
  # leaves one or both of yaml_rustler/rusty_csv unresolved, so the
  # "X.Y is undefined" compiler warnings that follow during `mix compile`
  # have an explanation already on screen instead of appearing unexplained.
  defp warn_skipped_bench_dep(name, hex_name) do
    # Plain IO.puts, not IO.warn/Mix.shell().error: this is informational
    # context for the compiler warnings that follow, not a code-quality
    # warning, so it shouldn't carry IO.warn's attached stacktrace.
    #IO.puts("""
    #==> #{name} is not being fetched (BENCH_SET=#{inspect(System.get_env("BENCH_SET"))}), so the \
    #compiler warnings below about its module being undefined are expected — it depends on a \
    #:rustler version that conflicts with the other Rust NIF dep's, so the two can't be \
    #deps.get'd together (see the BENCH_SET note above and in the Makefile). Run with \
    #BENCH_SET=#{hex_name} to fetch and benchmark against #{name} specifically.
    #""")
  end

  defp bench_set_deps do
    case System.get_env("BENCH_SET") do
      "yaml" ->
        warn_skipped_bench_dep("rusty_csv",   "csv")
        [{:yaml_rustler, "~> 0.1.6",  only: :bench}]

      "csv" ->
        warn_skipped_bench_dep("yaml_rustler", "yaml")
        [{:rusty_csv,    "~> 0.3.11", only: :bench}]

      _ ->
        warn_skipped_bench_dep("yaml_rustler", "yaml")
        warn_skipped_bench_dep("rusty_csv",     "csv")
        []
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
