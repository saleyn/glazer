defmodule GlazerIntegration.MixProject do
  use Mix.Project

  def project do
    [
      app: :glazer_integration,
      version: "0.1.0",
      elixir: "~> 1.15",
      start_permanent: Mix.env() == :prod,
      deps: deps()
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp deps do
    [
      {:glazer, github: "saleyn/glazer", branch: "build", manager: :mix},
      {:jason, "~> 1.4", only: :test}
    ]
  end
end
