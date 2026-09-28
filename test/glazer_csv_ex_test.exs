defmodule Glazer.CSVTest do
  use ExUnit.Case

  describe "decode/1,2" do
    test "returns {:ok, result} for valid CSV" do
      assert Glazer.CSV.decode("a,b\n1,2\n") ==
               {:ok, %{headers: nil, data: [["a", "b"], ["1", "2"]]}}
    end

    test "returns {:error, reason} on invalid input" do
      assert {:error, _reason} = Glazer.CSV.decode("\"unterminated")
    end

    test "passes through decode options" do
      assert Glazer.CSV.decode("name,age\nAlice,30\n", [:headers]) ==
               {:ok, %{headers: ["name", "age"], data: [["Alice", "30"]]}}
    end
  end

  describe "decode!/1,2" do
    test "decodes valid CSV" do
      assert Glazer.CSV.decode!("a,b\n1,2\n") == %{headers: nil, data: [["a", "b"], ["1", "2"]]}
    end

    test "raises on invalid input" do
      assert_raise ErlangError, fn -> Glazer.CSV.decode!("\"unterminated") end
    end

    test "passes through decode options" do
      assert Glazer.CSV.decode!("a,b\n1,2\n", [{:fields, [:binary, :integer]}]) ==
               %{headers: nil, data: [["a", "b"], ["1", 2]]}
    end
  end

  describe "encode!/1,2" do
    test "encodes rows to a CSV binary" do
      assert Glazer.CSV.encode!([["a", "b"], [1, 2]]) == "a,b\r\n1,2\r\n"
    end

    test "passes through encode options" do
      assert Glazer.CSV.encode!([["a", "b"], [1, 2]], [{:line_ending, :lf}]) == "a,b\n1,2\n"
    end

    test "raises when a row cannot be encoded" do
      assert_raise ErlangError, fn -> Glazer.CSV.encode!([[1 | 2]]) end
    end
  end

  describe "read_file!/1,2 and write_file!/2,3" do
    setup do
      path = Path.join(System.tmp_dir!(), "glazer_csv_ex_test_#{System.unique_integer([:positive])}.csv")
      on_exit(fn -> File.rm(path) end)
      %{path: path}
    end

    test "round-trips rows through a file", %{path: path} do
      assert Glazer.CSV.write_file!(path, [["a", "b"], [1, 2]]) == :ok
      assert Glazer.CSV.read_file!(path) == %{headers: nil, data: [["a", "b"], ["1", "2"]]}
    end

    test "write_file!/3 and read_file!/2 accept options", %{path: path} do
      assert Glazer.CSV.write_file!(path, [["a", "b"], [1, 2]], [{:line_ending, :lf}]) == :ok
      assert Glazer.CSV.read_file!(path, [:headers]) == %{headers: ["a", "b"], data: [["1", "2"]]}
    end
  end

  describe "streaming with decode_start/decode_continue" do
    test "decode_start/2,3 and decode_continue/2 parse one row at a time" do
      # First row: complete
      assert {["a", "b"], nil, rest} =
        Glazer.CSV.decode_start("a,b\n1,2\n", [])
      assert rest == "1,2\n"

      # Second row: complete
      assert {["1", "2"], nil, <<>>} =
        Glazer.CSV.decode_start(rest, [])
    end

    test "decode_start/3 with accumulator passes context through" do
      assert {:continue, state} =
        Glazer.CSV.decode_start("a,b", :counter, [])

      assert {["a", "b"], :counter, <<>>} =
        Glazer.CSV.decode_continue("\n", state)
    end

    test "decode_continue with end_of_input flushes incomplete rows" do
      assert {:continue, state} =
        Glazer.CSV.decode_start("1,2", [])

      assert {["1", "2"], nil, <<>>} =
        Glazer.CSV.decode_continue(:end_of_input, state)
    end

    test "decode_start and decode_continue across multiple chunks" do
      assert {:continue, state1} =
        Glazer.CSV.decode_start("a,b", [])

      assert {["a", "b"], nil, rest} =
        Glazer.CSV.decode_continue("\n1,", state1)
      assert rest == "1,"

      assert {:continue, state2} =
        Glazer.CSV.decode_start(rest, [])

      assert {["1", "2"], nil, <<>>} =
        Glazer.CSV.decode_continue("2\n", state2)
    end
  end
end
