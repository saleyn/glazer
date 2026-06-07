defmodule GlazejsonBench.MixProject do
  use Mix.Project

  def project do
    [
      app: :glazejson_bench,
      version: "0.1.0",
      elixir: "~> 1.15",
      deps: deps()
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [
      {:simdjsone, github: "saleyn/simdjsone", branch: "next"},
      {:jason,     "~> 1.4"},
      {:jiffy,     "~> 1.1"},
      {:thoas,     "~> 1.2"},
      {:euneus,    "~> 2.0"},
      {:torque,    "~> 0.1.9"}
    ]
  end
end
