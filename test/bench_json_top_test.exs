defmodule Mix.Tasks.BenchJsonTopTest do
  use ExUnit.Case, async: true

  test "takes the first N suites from CLI arguments" do
    suites = [
      {"glazer", nil, nil},
      {"torque", nil, nil},
      {"jason", nil, nil},
      {"json", nil, nil}
    ]

    assert Mix.Tasks.BenchJson.top_suites(["--top=2"], suites) |> Enum.map(&elem(&1, 0)) == ["glazer", "torque"]
  end

  test "takes the first N suites from the TOP environment variable" do
    previous = System.get_env("TOP")
    System.put_env("TOP", "3")

    on_exit(fn ->
      if previous, do: System.put_env("TOP", previous), else: System.delete_env("TOP")
    end)

    suites = [
      {"glazer", nil, nil},
      {"torque", nil, nil},
      {"jason", nil, nil},
      {"json", nil, nil}
    ]

    assert Mix.Tasks.BenchJson.top_suites([], suites) |> Enum.map(&elem(&1, 0)) == ["glazer", "torque", "jason"]
  end

  test "ignores invalid TOP values" do
    suites = [
      {"glazer", nil, nil},
      {"torque", nil, nil}
    ]

    assert Mix.Tasks.BenchJson.top_suites(["--top=abc"], suites) == suites
    assert Mix.Tasks.BenchJson.top_suites(["--top=0"], suites) == suites
  end
end
