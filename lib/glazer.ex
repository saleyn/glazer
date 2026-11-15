defmodule Glazer do
  @moduledoc """
  Top-level helpers for integers beyond the JSON, YAML and CSV formats.

  Integers of any size are converted exactly between Erlang integers and their
  decimal string form. The Erlang functions are `:glazer.encode_integer/1`,
  `:glazer.decode_integer/1` and `:glazer.try_decode_integer/1`; the functions
  here wrap them with Elixir conventions.

  ## Example

  ```elixir
  iex> Glazer.encode_integer!(123456789012345678901234567890)
  "123456789012345678901234567890"

  iex> Glazer.decode_integer("123456789012345678901234567890")
  {:ok, 123456789012345678901234567890}

  iex> Glazer.decode_integer!("123456789012345678901234567890")
  123456789012345678901234567890

  iex> Glazer.decode_integer("not a number")
  {:error, :invalid_number_format}
  ```
  """

  @doc """
  Encode an integer to its decimal string form. Raises `ArgumentError` when
  `int` is not an integer.
  """
  @spec encode_integer!(integer) :: binary
  def encode_integer!(int) when is_integer(int), do: :glazer.encode_integer(int)

  def encode_integer!(other) do
    raise ArgumentError, "expected an integer, got: #{inspect(other)}"
  end

  @doc """
  Decode a decimal string to an integer, returning `{:ok, integer}`, or
  `{:error, :invalid_number_format}` for anything else.
  """
  @spec decode_integer(binary | iolist) :: {:ok, integer} | {:error, :invalid_number_format}
  def decode_integer(string), do: :glazer.try_decode_integer(string)

  @doc """
  Like `decode_integer/1`, but returns the integer directly and raises
  `Glazer.ParseError` on invalid input.
  """
  @spec decode_integer!(binary | iolist) :: integer
  def decode_integer!(string) do
    case :glazer.try_decode_integer(string) do
      {:ok, int} -> int
      {:error, why} -> raise Glazer.ParseError, message: to_string(why)
    end
  end
end
