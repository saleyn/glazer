defmodule Glazer.Schema do
  @moduledoc """
  Idiomatic Elixir wrapper around `:glazer_schema` for JSON Schema 2020-12
  validation of JSON and YAML documents.

  Schemas are compiled once into a handle, and the handle is then used to
  validate any number of terms. Schema files are read by extension: `.json`
  is decoded as JSON, and `.yaml` or `.yml` as YAML. Compiled schemas can
  also be registered under a name (the schema's `$id`, or a name you give),
  so they can be validated by name and referenced from other schemas with
  `$ref`.

  This module delegates to `:glazer_schema`; options and return values are
  passed through unchanged. See `:glazer_schema` and the JSON Schema guide
  for the full behaviour.

  ## Null

  Validation compares data with the configured null atom, which is `:null`
  unless the `:glazer` application sets `null`. Decoded JSON from
  `Glazer.JSON` uses `nil` for `null`, so set `config :glazer, null: nil` in
  an Elixir project that validates such data.

  ## Example

  ```elixir
  iex> schema = %{
  ...>   "type" => "object",
  ...>   "required" => ["name"],
  ...>   "properties" => %{"name" => %{"type" => "string", "minLength" => 1}}
  ...> }
  iex> {:ok, compiled} = Glazer.Schema.compile(schema)
  iex> Glazer.Schema.validate(compiled, %{"name" => "Ann"})
  :ok
  iex> Glazer.Schema.validate(compiled, %{"name" => ""})
  {:error, {:path, ["name"], {:string_too_short, ""}}}
  ```

  Registration and validation by name:

  ```elixir
  iex> {:ok, 1} = Glazer.Schema.register("user", schema)
  iex> Glazer.Schema.validate("user", %{"name" => "Ann"})
  :ok
  iex> Glazer.Schema.validate("order", %{})
  {:error, {:unknown_schema, "order"}}
  ```
  """

  @doc """
  Compile a schema map (or `true`/`false`) to a handle, which is registered
  under its `$id` unless `registry: false` is given.

  Returns `{:ok, handle}` or `{:error, message}`.

  See `:glazer_schema.compile/2`.
  """
  defdelegate compile(schema), to: :glazer_schema, as: :compile

  @doc """
  Like `compile/1`, with compile options: `registry: false`, `name: name`,
  `refs: %{uri => schema}` and `format_assertion: boolean`.

  See `:glazer_schema.compile/2`.
  """
  defdelegate compile(schema, opts), to: :glazer_schema, as: :compile

  @doc """
  Read a schema file and compile it. The decoder is chosen by extension:
  `.json`, `.yaml` or `.yml`. Pass `format: :json` or `format: :yaml` to
  override the extension. Other options are those of `compile/2`.

  Returns `{:ok, handle}`, or `{:error, reason}` such as
  `{:unsupported_format, ext}`, `{:read_failed, path, reason}`,
  `{:invalid_json, path, message}`, or `{:invalid_yaml, path, message}`.

  See `:glazer_schema.compile_file/2`.
  """
  defdelegate compile_file(path), to: :glazer_schema, as: :compile_file

  @doc """
  Like `compile_file/1`, with options.

  See `:glazer_schema.compile_file/2`.
  """
  defdelegate compile_file(path, opts), to: :glazer_schema, as: :compile_file

  @doc """
  Validate `data` against a compiled handle, or against a registered name.

  Returns `:ok`, or `{:error, reason}`. The error reasons are those of the
  validator, such as `{:path, path, {reason, detail}}`, or
  `{:error, {:unknown_schema, name}}` for an unregistered name (the call then
  returns `{:error, {:unknown_schema, name}}`).

  See `:glazer_schema.validate/2`.
  """
  defdelegate validate(schema, data), to: :glazer_schema

  @doc """
  Like `validate/2`, with validation options: `:fail_fast` (default),
  `:collect_all_errors`, `{:max_depth, n}`, `{:max_errors, n}` and
  `{:timeout_ms, ms}`.

  See `:glazer_schema.validate/3`.
  """
  defdelegate validate(schema, data, opts), to: :glazer_schema

  @doc """
  Compile `schema` and validate `data` in one call. Compiles on every call, so
  use `compile/1` and `validate/2` for repeated validation.

  See `:glazer_schema.validate_term/2`.
  """
  defdelegate validate_term(schema, data), to: :glazer_schema

  @doc """
  Like `validate_term/2`, with validation options.

  See `:glazer_schema.validate_term/3`.
  """
  defdelegate validate_term(schema, data, opts), to: :glazer_schema

  @doc """
  Register a schema under its `$id`.

  Returns `{:ok, version}` or `{:error, message}`.

  See `:glazer_schema.register/1`.
  """
  defdelegate register(schema), to: :glazer_schema

  @doc """
  Register a schema under `name`. Registering a different schema under the same
  name creates a new version.

  See `:glazer_schema.register/2`.
  """
  defdelegate register(name, schema), to: :glazer_schema

  @doc """
  Like `register/2`, with compile options.

  See `:glazer_schema.register/3`.
  """
  defdelegate register(name, schema, opts), to: :glazer_schema

  @doc """
  Register schemas that refer to each other. Each needs a `$id`. Nothing is
  stored unless all of them compile.

  Returns `{:ok, [{name, version}]}` in the order given, or `{:error, message}`.

  See `:glazer_schema.register_group/1`.
  """
  defdelegate register_group(schemas), to: :glazer_schema

  @doc """
  Like `register_group/1`, with compile options.

  See `:glazer_schema.register_group/2`.
  """
  defdelegate register_group(schemas, opts), to: :glazer_schema

  @doc """
  Fetch the compiled handle registered under `name`: `{:ok, handle}` or
  `{:error, :not_found}`.

  See `:glazer_schema.fetch/1`.
  """
  defdelegate fetch(name), to: :glazer_schema

  @doc """
  Like `fetch/1`, but returns the handle directly, raising
  `{:unknown_schema, name}` when it is not registered.

  See `:glazer_schema.fetch!/1`.
  """
  defdelegate fetch!(name), to: :glazer_schema

  @doc """
  The current version of `name`: `{:ok, version}` or `{:error, :not_found}`.

  See `:glazer_schema.version/1`.
  """
  defdelegate version(name), to: :glazer_schema

  @doc """
  All registered names with their current versions, sorted by name.

  See `:glazer_schema.list/0`.
  """
  defdelegate list(), to: :glazer_schema

  @doc """
  Remove `name` from the registry. Unknown names are ignored.

  See `:glazer_schema.unregister/1`.
  """
  defdelegate unregister(name), to: :glazer_schema

  @doc """
  Remove every schema compiled with the default registry option (those without
  a name). Named registrations are kept. Returns the number removed.

  See `:glazer_schema.clear_interned/0`.
  """
  defdelegate clear_interned(), to: :glazer_schema
end
