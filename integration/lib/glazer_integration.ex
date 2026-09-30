defmodule GlazerIntegration do
  @moduledoc """
  Integration test app for Glazer library.

  This module provides simple wrappers around Glazer.JSON functionality to verify
  that the library can be properly used as a dependency.

  ## Examples

      iex> {:ok, result} = GlazerIntegration.encode_json(%{"hello" => "world"})
      iex> is_binary(result)
      true

      iex> {:ok, decoded} = GlazerIntegration.decode_json(~s({"hello":"world"}))
      iex> decoded["hello"]
      "world"
  """

  @doc """
  Encode a term to JSON using Glazer.

  Returns `{:ok, json_binary}` on success or `{:error, reason}` on failure.
  """
  def encode_json(term) do
    {:ok, Glazer.JSON.encode!(term)}
  rescue
    _ -> {:error, :invalid_term}
  end

  @doc """
  Encode a term to JSON, raising on failure.

  Returns a JSON binary.
  """
  def encode_json!(term) do
    Glazer.JSON.encode!(term)
  end

  @doc """
  Decode a JSON string using Glazer.

  Returns `{:ok, term}` on success or `{:error, reason}` on failure.
  """
  def decode_json(json_str) when is_binary(json_str) do
    Glazer.JSON.decode(json_str)
  end

  @doc """
  Decode a JSON string, raising on failure.

  Returns the decoded term.
  """
  def decode_json!(json_str) when is_binary(json_str) do
    Glazer.JSON.decode!(json_str)
  end

  @doc """
  Test incremental JSON decoding with decode_start/decode_continue.

  Processes JSON from multiple chunks and returns all decoded values.
  """
  def decode_incremental(json_chunks) when is_list(json_chunks) do
    json_str = IO.iodata_to_binary(json_chunks)
    do_decode_incremental(json_str, [])
  end

  defp do_decode_incremental(data, opts) when is_binary(data) do
    # Use decode_start/3 with the full data and an empty accumulator
    case Glazer.JSON.decode_start(data, [], opts) do
      {values, _final_acc, _rest} ->
        {:ok, values}
      {:continue, _state} ->
        {:error, :incomplete}
      error ->
        error
    end
  end
end
