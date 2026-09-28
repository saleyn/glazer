defmodule Glazer.CSV.DecodeStartExTest do
  use ExUnit.Case

  test "decode_start/2 with complete row" do
    assert {["a", "b", "c"], nil, <<>>} =
      Glazer.CSV.decode_start(~s(a,b,c\n), [])
  end

  test "decode_start/2 incomplete row returns continue" do
    assert {:continue, state} = Glazer.CSV.decode_start(~s(1,2), [])
    assert {["1", "2"], nil, <<>>} =
      Glazer.CSV.decode_continue(~s(\n), state)
  end

  test "decode_start/3 with accumulator" do
    assert {["x", "y"], :my_acc, <<>>} =
      Glazer.CSV.decode_start(~s(x,y\n), :my_acc, [])
  end

  test "decode_continue with end_of_input flushes buffer" do
    assert {:continue, state} = Glazer.CSV.decode_start(~s(1,2), [])
    assert {["1", "2"], nil, <<>>} =
      Glazer.CSV.decode_continue(:end_of_input, state)
  end

  test "decode_continue with multiple rows" do
    assert {:continue, state} = Glazer.CSV.decode_start(~s(a,b), [])
    assert {["a", "b"], nil, rest} =
      Glazer.CSV.decode_continue(~s(\nc,d\n), state)
    assert rest == ~s(c,d\n)
  end

  test "decode_continue with accumulator passthrough" do
    assert {:continue, state} = Glazer.CSV.decode_start(~s(1,2), :counter, [])
    assert {["1", "2"], :counter, <<>>} =
      Glazer.CSV.decode_continue(:end_of_input, state)
  end

  test "decode_continue end_of_input on empty buffer" do
    # Complete first row
    assert {["a", "b"], nil, <<>>} =
      Glazer.CSV.decode_start(~s(a,b\n), [])
    # Start with empty input
    assert {:continue, state} = Glazer.CSV.decode_start(~s(), [])
    # End of input returns nil
    assert {nil, nil, <<>>} =
      Glazer.CSV.decode_continue(:end_of_input, state)
  end
end
