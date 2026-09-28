defmodule Glazer.JSON do
  @moduledoc """
  Idiomatic Elixir wrapper around `:glazer_json` for fast JSON encoding and
  decoding.

  This module simply `defdelegate`s to the underlying Erlang `:glazer_json`
  module — see its docs for the full behaviour, option reference, and
  examples (options are passed through unchanged, as atoms/tuples).

  ## Example

  ```elixir
  iex> Glazer.JSON.decode!(~s({"a":1,"b":[true,null,3.5]}))
  %{"a" => 1, "b" => [true, nil, 3.5]}

  iex> Glazer.JSON.encode!(%{"a" => 1, "b" => [true, nil, 3.5]})
  ~s({"a":1,"b":[true,null,3.5]})
  ```

  For `@derive`-based struct encoding, see `Glazer.JSON.Encoder`.
  """

  @doc """
  Decode a JSON binary or iolist, returning `{:ok, term}` or `{:error, reason}`
  instead of raising. The atom `nil` is used for JSON `null`.

  See `:glazer_json.try_decode/2`.
  """
  def decode(input), do: :glazer.json_try_decode(input, [:use_nil])

  @doc """
  Like `decode/1`, but with decode options (see `t::glazer_json.decode_opts/0`).

  See `:glazer_json.try_decode/2`.
  """
  def decode(input, opts), do: :glazer.json_try_decode(input, [:use_nil | opts])

  @doc """
  Decode a JSON binary or iolist to an Elixir term, raising `Glazer.ParseError`
  on invalid input. The atom `nil` is used for JSON `null`.

  See `:glazer_json.try_decode/2`.
  """
  def decode!(input) do
    case :glazer.json_try_decode(input, [:use_nil]) do
      {:ok,    res} -> res
      {:error, why} -> raise Glazer.ParseError, message: why
    end
  end

  @doc """
  Encode an Elixir term to a JSON binary, raising `Glazer.ParseError` if
  `data` cannot be encoded. The atom `nil` is encoded as JSON `null`.

  See `:glazer_json.try_encode/2`.
  """
  def encode!(data), do: encode!(data, [])

  @doc """
  Like `encode!/1`, but with encode options (see `t::glazer_json.encode_opts/0`).

  See `:glazer_json.try_encode/2`.
  """
  def encode!(data, opts) do
    case :glazer.json_try_encode(data, [:use_nil | opts]) do
      {:ok,    res} -> res
      {:error, why} -> raise Glazer.ParseError, message: why
    end
  end

  @doc """
  Encode a list of Elixir terms to newline-delimited JSON (NDJSON).

  See `:glazer_json.encode_ndjson/1`.
  """
  defdelegate encode_ndjson(list), to: :glazer_json

  @doc """
  Like `encode_ndjson/1`, but with encode options (see `t::glazer_json.encode_opts/0`).

  See `:glazer_json.encode_ndjson/2`.
  """
  defdelegate encode_ndjson(list, opts), to: :glazer_json

  @doc """
  Encode an Elixir term to JSON as iodata, raising `Glazer.ParseError` if
  `data` cannot be encoded. Equivalent to `encode!/1`, provided for API
  parity with Elixir's `JSON.encode_to_iodata!/1`.

  See `:glazer_json.encode_to_iodata/1`.
  """
  def encode_to_iodata!(data), do: encode!(data)

  @doc """
  Minify a JSON binary or iolist, removing all unnecessary whitespace.

  See `:glazer_json.minify/1`.
  """
  defdelegate minify!(input), to: :glazer_json, as: :minify

  @doc """
  Pretty-print a JSON binary or iolist with indentation.

  See `:glazer_json.prettify/1`.
  """
  defdelegate prettify!(input), to: :glazer_json, as: :prettify

  @doc """
  Read `filename` and decode its contents as JSON.

  See `:glazer_json.read_file/1`.
  """
  defdelegate read_file!(filename), to: :glazer_json, as: :read_file

  @doc """
  Like `read_file!/1`, but with decode options (see `t::glazer_json.decode_opts/0`).

  See `:glazer_json.read_file/2`.
  """
  defdelegate read_file!(filename, opts), to: :glazer_json, as: :read_file

  @doc """
  Encode `data` to JSON and write it to `filename`, overwriting any existing
  file.

  See `:glazer_json.write_file/2`.
  """
  defdelegate write_file!(filename, data), to: :glazer_json, as: :write_file

  @doc """
  Like `write_file!/2`, but with encode options (see `t::glazer_json.encode_opts/0`).

  See `:glazer_json.write_file/3`.
  """
  defdelegate write_file!(filename, data, opts), to: :glazer_json, as: :write_file

  @doc """
  Run a [jq](https://jqlang.org/) `filter` program against a JSON binary or
  iolist `input`, returning one Elixir term per value produced by the
  filter.

  See `:glazer_json.query/2`.
  """
  defdelegate query!(input, filter), to: :glazer_json, as: :query

  @doc """
  Like `query!/2`, but decodes each result term using `decode_opts` (see
  `t::glazer_json.decode_opts/0`).

  See `:glazer_json.query/3`.
  """
  defdelegate query!(input, filter, decode_opts), to: :glazer_json, as: :query

  @doc """
  Locate the end of the next complete top-level JSON value in `bin`, without
  decoding it.

  See `:glazer_json.scan/1`.
  """
  defdelegate scan!(bin), to: :glazer_json, as: :scan

  @doc """
  Resume scanning `bin` (the unconsumed remainder plus newly-appended bytes)
  from `scan_state`.

  See `:glazer_json.scan/2`.
  """
  defdelegate scan!(bin, scan_state), to: :glazer_json, as: :scan

  @doc """
  Start incremental (streaming) JSON decoding with a convenience form.

  Equivalent to `decode_start(input, nil, opts)`. Returns either `{value, acc, rest}`
  when a complete JSON value is decoded, or `{:continue, state}` when more data
  is needed (resume with `decode_continue/2`).

  See `:glazer_json.decode_start/2`.
  """
  defdelegate decode_start(input, opts), to: :glazer_json

  @doc """
  Start incremental (streaming) JSON decoding with an explicit accumulator.

  Parses exactly one JSON value per call, returning the unparsed remainder.
  The accumulator is threaded through for context passing. Use with
  `decode_continue/2` to parse streaming JSON data.

  See `:glazer_json.decode_start/3`.
  """
  defdelegate decode_start(input, acc, opts), to: :glazer_json

  @doc """
  Resume incremental JSON decoding with new data or signal end of stream.

  Takes either:
  - A binary or iolist to feed more data
  - The atom `:end_of_input` to signal no more data is coming

  Returns either `{value, acc, rest}` on success or `{:continue, state}` if
  more data is needed.

  See `:glazer_json.decode_continue/2`.
  """
  defdelegate decode_continue(input, state), to: :glazer_json
end
