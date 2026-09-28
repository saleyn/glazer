defmodule Glazer.CSV do
  @moduledoc """
  Idiomatic Elixir wrapper around `:glazer_csv` for fast CSV encoding and
  decoding.

  This module simply `defdelegate`s to the underlying Erlang `:glazer_csv`
  module — see its docs for the full behaviour, option reference, and
  examples (options are passed through unchanged, as atoms/tuples).

  ## Example

  ```elixir
  iex> Glazer.CSV.decode("a,b\\n1,2\\n3,4\\n")
  %{headers: nil, data: [["a", "b"], ["1", "2"], ["3", "4"]]}

  iex> Glazer.CSV.encode([["a", "b"], [1, 2]])
  "a,b\\r\\n1,2\\r\\n"
  ```
  """

  @doc """
  Decode a CSV binary or iolist, raising `reason` (see
  `t::glazer_csv.decode_error/0`) on invalid input. Returns a
  `t::glazer_csv.csv_result/0` map `%{headers: nil, data: rows}`.

  See `:glazer_csv.decode/1`.
  """
  defdelegate decode!(input), to: :glazer_csv, as: :decode

  @doc """
  Like `decode!/1`, but with decode options (see `t::glazer_csv.decode_opts/0`).

  See `:glazer_csv.decode/2`.
  """
  defdelegate decode!(input, opts), to: :glazer_csv, as: :decode

  @doc """
  Decode a CSV binary or iolist, returning `{:ok, result}` or `{:error,
  reason}` instead of raising.

  See `:glazer_csv.try_decode/1`.
  """
  defdelegate decode(input), to: :glazer_csv, as: :try_decode

  @doc """
  Like `decode/1`, but with decode options (see `t::glazer_csv.decode_opts/0`).

  See `:glazer_csv.try_decode/2`.
  """
  defdelegate decode(input, opts), to: :glazer_csv, as: :try_decode

  @doc """
  Encode a list of rows to a CSV binary, raising `{encode_error, reason}` if
  any row or field cannot be encoded.

  See `:glazer_csv.encode/1`.
  """
  defdelegate encode!(data), to: :glazer_csv, as: :encode

  @doc """
  Like `encode!/1`, but with encode options (see `t::glazer_csv.encode_opts/0`).

  See `:glazer_csv.encode/2`.
  """
  defdelegate encode!(data, opts), to: :glazer_csv, as: :encode

  @doc """
  Read `filename` and decode its contents as CSV.

  See `:glazer_csv.read_file/1`.
  """
  defdelegate read_file!(filename), to: :glazer_csv, as: :read_file

  @doc """
  Like `read_file!/1`, but with decode options (see `t::glazer_csv.decode_opts/0`).

  See `:glazer_csv.read_file/2`.
  """
  defdelegate read_file!(filename, opts), to: :glazer_csv, as: :read_file

  @doc """
  Encode `data` to CSV and write it to `filename`, overwriting any existing
  file.

  See `:glazer_csv.write_file/2`.
  """
  defdelegate write_file!(filename, data), to: :glazer_csv, as: :write_file

  @doc """
  Like `write_file!/2`, but with encode options (see `t::glazer_csv.encode_opts/0`).

  See `:glazer_csv.write_file/3`.
  """
  defdelegate write_file!(filename, data, opts), to: :glazer_csv, as: :write_file

  @doc """
  Start incremental (streaming) CSV decoding with a convenience form.

  Equivalent to `decode_start(input, nil, opts)`. Returns either `{:ok, row, rest}`
  when a complete CSV row is decoded, or `{:continue, state}` when more data
  is needed (resume with `decode_continue/2`).

  See `:glazer_csv.decode_start/2`.
  """
  defdelegate decode_start(input, opts), to: :glazer_csv

  @doc """
  Start incremental (streaming) CSV decoding with an explicit accumulator.

  Parses exactly one CSV row per call, returning the unparsed remainder.
  The accumulator is threaded through for context passing. Use with
  `decode_continue/2` to parse streaming CSV data.

  See `:glazer_csv.decode_start/3`.
  """
  defdelegate decode_start(input, acc, opts), to: :glazer_csv

  @doc """
  Resume incremental CSV decoding with new data or signal end of stream.

  Takes either:
  - A binary or iolist to feed more data
  - The atom `:end_of_input` to signal no more data is coming

  Returns either `{:ok, row, rest}` on success or `{:continue, state}` if
  more data is needed.

  See `:glazer_csv.decode_continue/2`.
  """
  defdelegate decode_continue(input, state), to: :glazer_csv
end
