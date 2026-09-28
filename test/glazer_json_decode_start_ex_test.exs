defmodule Glazer.JSON.DecodeStartExTest do
  use ExUnit.Case

  test "decode_start/2 with complete object" do
    assert {%{"a" => 1}, nil, <<>>} =
      Glazer.JSON.decode_start(~s({"a":1}), [])
  end

  test "decode_start/2 bare scalar ambiguous - returns continue" do
    # Bare scalars are ambiguous (could be incomplete), so return continue
    assert {:continue, state} = Glazer.JSON.decode_start(~s(123), [])
    # Signal end of input to force decode
    assert {123, nil, <<>>} = Glazer.JSON.decode_continue(:end_of_input, state)
  end

  test "decode_start/2 incomplete value returns continue" do
    assert {:continue, state} = Glazer.JSON.decode_start(~s({"a":), [])
    assert {%{"a" => 1}, nil, <<>>} = Glazer.JSON.decode_continue(~s(1}), state)
  end

  test "decode_start/3 with accumulator" do
    assert {[1, 2, 3], :my_acc, <<>>} =
      Glazer.JSON.decode_start(~s([1,2,3]), :my_acc, [])
  end

  test "decode_continue with end_of_input flushes buffer" do
    assert {:continue, state} = Glazer.JSON.decode_start(~s({"key":), [])
    assert {%{"key" => 42}, nil, <<>>} = Glazer.JSON.decode_continue(~s(42}), state)
  end

  test "decode_continue with multiple values in Rest" do
    assert {[1], nil, rest} = Glazer.JSON.decode_start(~s([1][2][3]), [])
    assert rest == ~s([2][3])
    # Second array should also complete
    assert {[2], nil, rest2} = Glazer.JSON.decode_start(rest, [])
    assert rest2 == ~s([3])
    assert {[3], nil, <<>>} = Glazer.JSON.decode_start(rest2, [])
  end

  test "decode_start with null (returns atom null not nil)" do
    # Without :use_nil option, null is the atom :null
    assert {:continue, state} = Glazer.JSON.decode_start(~s(null), [])
    assert {:null, nil, <<>>} = Glazer.JSON.decode_continue(:end_of_input, state)
  end

  test "decode_continue with accumulator passthrough" do
    assert {:continue, state} = Glazer.JSON.decode_start(~s({"x":), :my_data, [])
    assert {%{"x" => 1}, :my_data, <<>>} =
      Glazer.JSON.decode_continue(~s(1}), state)
  end
end
