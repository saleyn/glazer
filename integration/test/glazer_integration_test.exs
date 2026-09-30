defmodule GlazerIntegrationTest do
  use ExUnit.Case
  doctest GlazerIntegration

  describe "encode_json/1" do
    test "encodes a simple map" do
      assert {:ok, result} = GlazerIntegration.encode_json(%{"hello" => "world"})
      assert result == "{\"hello\":\"world\"}"
    end

    test "encodes a list" do
      assert {:ok, result} = GlazerIntegration.encode_json([1, 2, 3])
      assert result == "[1,2,3]"
    end

    test "encodes nested structures" do
      assert {:ok, result} = GlazerIntegration.encode_json(%{"data" => [1, 2, %{"key" => "value"}]})
      assert is_binary(result)
    end

    test "encodes booleans and null" do
      assert {:ok, result} = GlazerIntegration.encode_json(%{"a" => true, "b" => false, "c" => nil})
      assert result == "{\"a\":true,\"b\":false,\"c\":null}"
    end
  end

  describe "encode_json!/1" do
    test "encodes a simple map" do
      result = GlazerIntegration.encode_json!(%{"hello" => "world"})
      assert result == "{\"hello\":\"world\"}"
    end

    test "raises on invalid input" do
      assert_raise Glazer.ParseError, fn ->
        # A function cannot be encoded to JSON
        GlazerIntegration.encode_json!(fn -> :ok end)
      end
    end
  end

  describe "decode_json/1" do
    test "decodes a simple JSON object" do
      assert {:ok, result} = GlazerIntegration.decode_json("{\"hello\":\"world\"}")
      assert result == %{"hello" => "world"}
    end

    test "decodes a JSON array" do
      assert {:ok, result} = GlazerIntegration.decode_json("[1,2,3]")
      assert result == [1, 2, 3]
    end

    test "decodes nested structures" do
      json = "{\"data\":[1,2,{\"key\":\"value\"}]}"
      assert {:ok, result} = GlazerIntegration.decode_json(json)
      assert is_map(result) or is_list(result)
    end

    test "decodes booleans and null" do
      json = "{\"a\":true,\"b\":false,\"c\":null}"
      assert {:ok, result} = GlazerIntegration.decode_json(json)
      assert result == %{"a" => true, "b" => false, "c" => nil}
    end
  end

  describe "decode_json!/1" do
    test "decodes a simple JSON object" do
      result = GlazerIntegration.decode_json!("{\"hello\":\"world\"}")
      assert result == %{"hello" => "world"}
    end

    test "raises on invalid JSON" do
      assert_raise Glazer.ParseError, fn ->
        GlazerIntegration.decode_json!("{invalid json}")
      end
    end
  end

  describe "decode_incremental/1" do
    test "decodes JSON from multiple chunks" do
      chunks = ["{\"", "hello\":", "\"world\"", "}"]
      assert {:ok, value} = GlazerIntegration.decode_incremental(chunks)
      # decode_start returns a single value when parsing is complete
      assert is_map(value) or is_list(value)
    end

    test "decodes array from multiple chunks" do
      chunks = ["[1,", "2,", "3]"]
      assert {:ok, value} = GlazerIntegration.decode_incremental(chunks)
      assert is_list(value)
    end
  end

  describe "round-trip encoding and decoding" do
    test "encodes and decodes simple data" do
      data = %{"name" => "test", "value" => 42, "active" => true}
      assert {:ok, encoded} = GlazerIntegration.encode_json(data)
      assert {:ok, decoded} = GlazerIntegration.decode_json(encoded)
      assert decoded == data
    end

    test "handles complex nested structures" do
      data = %{
        "users" => [
          %{"id" => 1, "name" => "Alice"},
          %{"id" => 2, "name" => "Bob"}
        ],
        "count" => 2
      }
      assert {:ok, encoded} = GlazerIntegration.encode_json(data)
      assert {:ok, decoded} = GlazerIntegration.decode_json(encoded)
      assert decoded == data
    end

    test "round-trip with arrays" do
      data = [1, "two", 3.0, true, nil, %{"nested" => "value"}]
      assert {:ok, encoded} = GlazerIntegration.encode_json(data)
      assert {:ok, decoded} = GlazerIntegration.decode_json(encoded)
      assert decoded == data
    end
  end
end
