defprotocol Glazer.JSON.Encoder do
  @moduledoc """
  Protocol for encoding values to JSON using Glazer's fast Erlang backend.

  When compiling via Mix/Elixir, Glazer automatically provides this protocol
  for convenient `@derive` support without requiring users to implement their own
  protocol wrapper.

  ## Deriving

  The protocol supports Elixir's `@derive` feature to automatically filter
  struct fields at compile time:

      @derive {Glazer.JSON.Encoder, only: [:id, :name, :created_at]}
      defstruct [:id, :name, :secret, :created_at]

  Accepted options:
    * `:only` - encodes only the specified fields
    * `:except` - encodes all fields except the specified ones

  By default, all fields except `:__struct__` are encoded.

  ## Example

  ```elixir
  defmodule User do
    @derive {Glazer.JSON.Encoder, only: [:id, :name, :email]}
    defstruct [:id, :name, :email, :password_hash]
  end

  user = %User{id: 1, name: "Alice", email: "alice@example.com", password_hash: "***"}
  json = Glazer.JSON.Encoder.encode(user, [])
  # Result: "{\"id\":1,\"name\":\"Alice\",\"email\":\"alice@example.com\"}"
  ```

  If a legacy module embeds a `Jason.Encoder` @derive and the `:jason` application is
  not loaded, Glazer.JSON.Encoder implements `Jason.Encoder` protocol fallback:

  ```elixir
  defmodule User do
    @derive {Jason.Encoder, only: [:id, :name, :email]}
    defstruct [:id, :name, :email, :password_hash]
  end

  user = %User{id: 1, name: "Alice", email: "alice@example.com", password_hash: "***"}
  json = Glazer.JSON.Encoder.encode(user, [])
  # Result: "{\"id\":1,\"name\":\"Alice\",\"email\":\"alice@example.com\"}"
  ```

  The `Jason.Encoder` fallback can be turned off at compile time with
  `config :glazer, jason_encoder_fallback: false`; see the comment above it in
  the source. Recompile after changing the setting.

  ## Performance

  - **Field filtering**: Happens at compile time; zero runtime cost
  - **JSON encoding**: Uses Glazer's fast C++ NIF backend (2-10x faster than pure Elixir)
  - **No allocations**: Filtered map is stack-allocated

  ## See Also

  - `Jason.Encoder`
  """

  @type t :: term
  @type opts :: keyword()

  @fallback_to_any true

  @doc """
  Encode a value to JSON.

  The `opts` are passed to the underlying Erlang encoder.

  ## Options

  - `use_nil` - encode the atom `nil` as JSON `null` (default: false, uses atom `:null`)
  - `pretty` - pretty-print the JSON output
  - `uescape` - escape non-ASCII characters as \\uXXXX sequences
  - `force_utf8` - replace invalid UTF-8 byte sequences with U+FFFD
  - `escape_fwd_slash` - escape forward slashes as \\/
  """
  @spec encode(t, opts) :: iodata
  def encode(value, opts)
end

defmodule Glazer.JSON.Encoder.DeriveHelper do
  @moduledoc false
  # Shared field-filtering logic for `@derive`, used by both
  # `Glazer.JSON.Encoder`'s and (the fallback) `Jason.Encoder`'s
  # `__deriving__/3` macros.
  #
  # This is a plain function, not a macro, specifically so it *can* be called
  # from inside another macro's body with real (already-evaluated) values —
  # macros can't invoke each other that way: calling one macro from inside
  # another via a literal `Mod.macro(...)` call passes unevaluated AST
  # instead of values, and macros aren't real functions so `apply/3` can't
  # reach them either.
  def fields_to_encode(struct, opts) do
    fields = Map.keys(struct)

    cond do
      only = Keyword.get(opts, :only) ->
        case only -- fields do
          [] ->
            only

          error_keys ->
            raise ArgumentError,
              "`:only` specified keys (#{inspect(error_keys)}) that are not defined in defstruct: " <>
                "#{inspect(fields -- [:__struct__])}"
        end

      except = Keyword.get(opts, :except) ->
        case except -- fields do
          [] ->
            fields -- [:__struct__ | except]

          error_keys ->
            raise ArgumentError,
              "`:except` specified keys (#{inspect(error_keys)}) that are not defined in defstruct: " <>
                "#{inspect(fields -- [:__struct__])}"
        end

      true ->
        fields -- [:__struct__]
    end
  end
end

defimpl Glazer.JSON.Encoder, for: Any do
  alias Glazer.JSON.Encoder.DeriveHelper

  defmacro __deriving__(module, struct, opts) do
    fields = DeriveHelper.fields_to_encode(struct, opts)

    quote do
      defimpl Glazer.JSON.Encoder, for: unquote(module) do
        def encode(value, opts) do
          filtered = Map.take(value, unquote(fields))
          :glazer_json.encode(filtered, opts)
        end
      end
    end
  end

  def encode(%_{} = struct, _opts) do
    raise Protocol.UndefinedError,
      protocol: @protocol,
      value: struct,
      description: """
      Glazer.JSON.Encoder protocol must always be explicitly implemented.

      If you own the struct, you can derive the implementation specifying \
      which fields should be encoded to JSON:

          @derive {Glazer.JSON.Encoder, only: [....]}
          defstruct ...

      It is also possible to encode all fields, although this should be \
      used carefully to avoid accidentally leaking private information \
      when new fields are added:

          @derive Glazer.JSON.Encoder
          defstruct ...

      Finally, if you don't own the struct you want to encode to JSON, \
      you may use Protocol.derive/3 placed outside of any module:

          Protocol.derive(Glazer.JSON.Encoder, NameOfTheStruct, only: [...])
          Protocol.derive(Glazer.JSON.Encoder, NameOfTheStruct)
      """
  end

  def encode(value, _opts) do
    raise Protocol.UndefinedError,
      protocol: @protocol,
      value: value,
      description: "Glazer.JSON.Encoder protocol must always be explicitly implemented"
  end
end

# Protocol implementations for basic Elixir types
# These delegate to glazer_json encoding

# Types that don't use opts
defimpl Glazer.JSON.Encoder, for: [Integer, Float] do
  def encode(value, _opts) do
    :glazer_json.encode(value, [])
  end
end

# Types that pass opts through
defimpl Glazer.JSON.Encoder, for: [Atom, List, Map] do
  def encode(value, opts) do
    :glazer_json.encode(value, opts)
  end
end

# BitString needs special handling for binary vs bitstring
defimpl Glazer.JSON.Encoder, for: BitString do
  def encode(binary, _opts) when is_binary(binary) do
    :glazer_json.encode(binary, [])
  end

  def encode(bitstring, _opts) do
    raise Protocol.UndefinedError,
      protocol: @protocol,
      value: bitstring,
      description: "cannot encode a bitstring to JSON"
  end
end

# Date/Time types - encode as ISO8601 strings like Jason does
defimpl Glazer.JSON.Encoder, for: [Date, Time, NaiveDateTime, DateTime] do
  def encode(value, _opts) do
    [?", @for.to_iso8601(value), ?"]
  end
end

# Decimal support if available
if Code.ensure_loaded?(Decimal) do
  defimpl Glazer.JSON.Encoder, for: Decimal do
    def encode(value, _opts) do
      decimal_string = Decimal.to_string(value)
      :glazer_json.encode(decimal_string, [])
    end
  end
end

# Compile-time switch for the Jason.Encoder fallback below. Set it in your config:
#
#     config :glazer, jason_encoder_fallback: false
#
# With the fallback disabled, Glazer does not define a `Jason.Encoder` protocol,
# so it never overrides a Jason that is loaded later, and `@derive {Jason.Encoder, ...}`
# needs Jason. The value is read at compile time: changing it needs a recompile
# (`mix compile --force`). Defaults to true.
if Application.compile_env(:glazer, :jason_encoder_fallback, true) and not Code.ensure_loaded?(Jason.Encoder) do
  # Disable warning about redefining the built-in Jason.Encoder protocol
  Code.compiler_options(ignore_module_conflict: true)

  # `defimpl Jason.Encoder` requires the protocol itself to exist; since Jason
  # isn't a dependency here, define a minimal shim matching Jason's own
  # protocol shape (`encode/2`, `@fallback_to_any true`) so structs can still
  # be `@derive {Jason.Encoder, ...}`'d — should the app later add Jason as a
  # real dependency, Jason's own protocol/implementation takes over instead.
  defprotocol Jason.Encoder do
    @moduledoc """
    Provide `Jason.Encoder` as a fallback if Jason is not available.
    This allows users to write `@derive {Jason.Encoder, ...}` even without `Jason`
    as a dependency. When Jason is loaded, it provides its own `Jason.Encoder`
    protocol (and this branch is skipped entirely). This way source code in
    applications that defines `@derive` for structs doen't need to be modified.

    **E.g.**:

    ```
    @derive {Jason.Encoder, only: [:id, :name, :created_at]}
    defstruct [:id, :name, :secret, :created_at]
    ```

    ### See Also

    - `Glazer.JSON.Encoder`
    """
    @fallback_to_any true
    def encode(value, opts)
  end

  defimpl Jason.Encoder, for: Any do
    alias Glazer.JSON.Encoder.DeriveHelper

    defmacro __deriving__(module, struct, opts) do
      # Reuse Glazer.JSON.Encoder's field-filtering logic (via the shared
      # DeriveHelper function, not its __deriving__ macro — macros can't be
      # invoked from inside another macro with real evaluated values), then
      # add both a Glazer.JSON.Encoder impl and a Jason.Encoder impl that
      # forwards to it.
      fields = DeriveHelper.fields_to_encode(struct, opts)

      quote do
        defimpl Glazer.JSON.Encoder, for: unquote(module) do
          def encode(value, opts) do
            filtered = Map.take(value, unquote(fields))
            :glazer_json.encode(filtered, opts)
          end
        end

        defimpl Jason.Encoder, for: unquote(module) do
          def encode(value, _opts) do
            Glazer.JSON.Encoder.encode(value, [:use_nil])
          end
        end
      end
    end

    def encode(%_{} = struct, _opts) do
      raise Protocol.UndefinedError,
        protocol: @protocol,
        value: struct,
        description: """
        Jason.Encoder protocol is provided by Glazer as a fallback when \
        Jason is not available.

        If you own the struct, you can derive the implementation specifying \
        which fields should be encoded to JSON:

            @derive {Jason.Encoder, only: [....]}
            defstruct ...

        This will use Glazer's fast C++ NIF backend for JSON encoding.
        """
    end

    def encode(value, _opts) do
      raise Protocol.UndefinedError,
        protocol: @protocol,
        value: value,
        description: "Jason.Encoder protocol must always be explicitly implemented"
    end
  end
end
