defmodule GlazerIntegerTest do
  use ExUnit.Case, async: true

  @big 123456789012345678901234567890

  test "encode_integer!/1 returns the string or raises" do
    assert Glazer.encode_integer!(@big) == "123456789012345678901234567890"
    assert_raise ArgumentError, fn -> Glazer.encode_integer!("42") end
  end

  test "decode_integer/1 returns a tuple" do
    assert {:ok, @big} = Glazer.decode_integer("123456789012345678901234567890")
    assert {:ok, -7} = Glazer.decode_integer("-7")
    assert {:error, :invalid_number_format} = Glazer.decode_integer("not a number")
    assert {:error, :invalid_number_format} = Glazer.decode_integer(" 12")
  end

  test "decode_integer!/1 returns the integer or raises ParseError" do
    assert Glazer.decode_integer!("123456789012345678901234567890") == @big
    assert_raise Glazer.ParseError, fn -> Glazer.decode_integer!("not a number") end
  end

  test "round trip of integers beyond int64" do
    for n <- [0, 9_223_372_036_854_775_807, 9_223_372_036_854_775_808, -Integer.pow(2, 90)] do
      assert {:ok, n} == Glazer.decode_integer(Glazer.encode_integer!(n))
    end
  end
end
