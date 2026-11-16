# JSON Schema validation guide

Glazer validates Erlang terms and decoded JSON against [JSON Schema 2020-12](https://json-schema.org/specification) using a native (C++) validator. A schema is compiled once into bytecode, and each validation runs that bytecode against a term, so validating many documents against one schema is cheap.

This guide covers:

1. [Quick start](#quick-start)
2. [Compiling schemas](#compiling-schemas)
3. [Validating data](#validating-data)
4. [Reading errors](#reading-errors)
5. [Validation options](#validation-options)
6. [Compile options](#compile-options)
7. [Supported keywords](#supported-keywords)
8. [References and documents](#references-and-documents)
9. [Formats](#formats)
10. [Regular expressions](#regular-expressions)
11. [Data model: what gets validated](#data-model-what-gets-validated)
12. [Limits, known issues and what is not supported](#limits-known-issues-and-what-is-not-supported)
13. [Schema registry](#schema-registry)
14. [Performance](#performance)
15. [Troubleshooting](#troubleshooting)
16. [Conformance testing](#conformance-testing)

---

## Quick start

**Erlang**

```erlang
Schema = #{
  ~"type" => ~"object",
  ~"required" => [~"name", ~"age"],
  ~"properties" => #{
    ~"name" => #{~"type" => ~"string", ~"minLength" => 1},
    ~"age"  => #{~"type" => ~"integer", ~"minimum" => 0},
    ~"email" => #{~"type" => ~"string", ~"format" => ~"email"}
  }
},

{ok, Compiled} = glazer_schema:compile(Schema),

ok = glazer_schema:validate(Compiled, #{~"name" => ~"Ann", ~"age" => 3}),

{error, {path, [~"age"], {value_too_small, 0}}} =
  glazer_schema:validate(Compiled, #{~"name" => ~"Ann", ~"age" => -1}).
```

**Elixir**

```elixir
schema = %{
  "type" => "object",
  "required" => ["name", "age"],
  "properties" => %{
    "name" => %{"type" => "string", "minLength" => 1},
    "age" => %{"type" => "integer", "minimum" => 0},
    "email" => %{"type" => "string", "format" => "email"}
  }
}

{:ok, compiled} = Glazer.Schema.compile(schema)

:ok = Glazer.Schema.validate(compiled, %{"name" => "Ann", "age" => 3})
```

For a one-off check you can skip the compile step:

**Erlang**

```erlang
ok = glazer_schema:validate_term(Schema, #{~"name" => ~"Ann", ~"age" => 3}).
```

**Elixir**

```elixir
:ok = Glazer.Schema.validate_term(schema, %{"name" => "Ann", "age" => 3})
```

---

## Compiling schemas

**Erlang**

```erlang
{ok, Compiled}   = glazer_schema:compile(Schema).
{ok, Compiled}   = glazer_schema:compile(Schema, Options).
{error, Message} = glazer_schema:compile(BadSchema).
```

**Elixir**

```elixir
{:ok, compiled} = Glazer.Schema.compile(schema)
{:ok, compiled} = Glazer.Schema.compile(schema, registry: false, format_assertion: true)
{:error, message} = Glazer.Schema.compile(bad_schema)
```

- `Schema` must be a map (a JSON object) or the atoms `true` and `false` (the boolean schemas of 2020-12). Anything else returns `{error, ~"schema must be a map"}`.
- `Message` is a binary describing the first problem found, for example `~"unknown type: bogus"` or `~"unresolved $ref (document not provided): http://x/y.json"`.
- A successful compile returns `{ok, {schema, Resource}}`. The `Resource` is a reference to the compiled schema. By default it is also kept in the schema registry, so repeated compiles of the same schema return the same handle without recompiling (see [Registry by default](#registry-by-default)). With `{registry, false}` the handle is only held by your code, and it is released once you drop it.

Schema keys can be binaries (the normal case for decoded JSON) or atoms. Both work.

### Reading a schema from a file

**Erlang**

```erlang
{ok, C} = glazer_schema:compile_file("schemas/user.json"),
{ok, D} = glazer_schema:compile_file("schemas/order.json", [{registry, false}]).
```

**Elixir**

```elixir
{:ok, c} = Glazer.Schema.compile_file("schemas/user.json")
{:ok, d} = Glazer.Schema.compile_file("schemas/order.json", registry: false)
```

The format comes from the extension: `.json` is decoded as JSON, and `.yaml` or `.yml` as YAML (the extension is matched case-insensitively). `{format, json | yaml}` overrides the extension. The file is then compiled as `compile/2` does, so the same options and registry behaviour apply. Errors are:

- `{error, {unsupported_format, Extension}}` (the extension as a binary, e.g. `<<".txt">>`): the extension is not recognised and no `format` option was given;
- `{error, {read_failed, Filename, Reason}}`: the file cannot be read;
- `{error, {invalid_json, Filename, Message}}`: the contents are not JSON;
- `{error, {invalid_yaml, Filename, Message}}`: the contents are not YAML;
- `{error, Message}`: the schema does not compile.

### Registry by default

`compile` adds each schema to the schema registry (see [Schema registry](#schema-registry)). Compiling the same schema with the same options again returns the stored handle, so repeated calls with the same schema do not recompile.

A schema with a top-level `$id` is registered under that `$id` (without any fragment), so it can be fetched by name and used by `$ref` from other schemas. A schema without `$id` is stored by its content.

**Erlang**

```erlang
{ok, C} = glazer_schema:compile(Schema),        % registered
{ok, C} = glazer_schema:compile(Schema).        % same handle, no recompile

{ok, D} = glazer_schema:compile(Schema, [{registry, false}]).  % not registered
```

**Elixir**

```elixir
{:ok, c} = Glazer.Schema.compile(schema)            # registered
{:ok, c} = Glazer.Schema.compile(schema)            # same handle, no recompile

{:ok, d} = Glazer.Schema.compile(schema, registry: false)   # not registered
```

- A schema with no `$id` can be given a name with `{name, Name}`, and is then registered like any named schema: `glazer_schema:fetch(Name)` and `validate(Name, Data)` work. If the schema has a `$id`, the name must equal it (without any fragment). A different name is an error, because two names for one schema would split its identity: `$ref` finds it by `$id`, and `validate` finds it by name.
- A schema with no `$id` and no `name` is keyed by the schema together with its compile options (`refs`, `format_assertion`). Changing either gives a separate entry.
- A schema with `$id` is one named entry. Compiling it again with the same options is unchanged; a different schema under the same `$id` creates a new version.
- If a schema refers to registered documents with `$ref`, those documents are part of the options that the entry is keyed on. Changing a referenced document makes the next compile recompile.
- Compile errors are never stored.
- Stored entries stay in the VM until `glazer_schema:clear_interned/0` is called. Each new entry triggers a global garbage collection, and entries are never removed on their own. For schemas built per request (for example with values embedded from user input), pass `{registry, false}` so the registry does not grow without bound.

### Compile once, validate many

Compilation parses the schema and checks it. Do it at startup or on first use, then reuse the handle:

**Erlang**

```erlang
-module(user_schema).
-export([compiled/0, check/1]).

compiled() ->
  persistent_term:get({?MODULE, schema}, undefined) orelse
    begin
      {ok, C} = glazer_schema:compile(schema()),
      persistent_term:put({?MODULE, schema}, C),
      C
    end.

check(Data) -> glazer_schema:validate(compiled(), Data).
```

**Elixir**

```elixir
defmodule UserSchema do
  def compiled do
    case :persistent_term.get({__MODULE__, :schema}, nil) do
      nil ->
        {:ok, c} = Glazer.Schema.compile(schema())
        :persistent_term.put({__MODULE__, :schema}, c)
        c

      c ->
        c
    end
  end

  def check(data), do: Glazer.Schema.validate(compiled(), data)
end
```

---

## Validating data

**Erlang**

```erlang
ok                     = glazer_schema:validate(Compiled, Data).
{error, Reason}        = glazer_schema:validate(Compiled, Data).
ok                     = glazer_schema:validate(Compiled, Data, Options).
ok                     = glazer_schema:validate_term(Schema, Data).
ok                     = glazer_schema:validate_term(Schema, Data, Options).
```

**Elixir**

```elixir
:ok = Glazer.Schema.validate(compiled, data)
{:error, reason} = Glazer.Schema.validate(compiled, data)
:ok = Glazer.Schema.validate(compiled, data, options)
:ok = Glazer.Schema.validate_term(schema, data)
:ok = Glazer.Schema.validate_term(schema, data, options)
```

- `validate/2,3` takes a compiled schema, either the `{schema, Resource}` tuple from `compile/1` or the bare resource.
- `validate_term/2,3` takes the uncompiled schema map and **compiles it on every call**. Use it for tests and one-off checks; use `compile` plus `validate` for anything called repeatedly.

`Data` is an Erlang term in the decoded-JSON shape:

| JSON | Erlang term |
|---|---|
| object | map (binary keys, or atom keys; see below) |
| array | list |
| string | binary (UTF-8) |
| number | integer or float |
| `true` / `false` | `true` / `false` |
| `null` | the atom `null` by default, or the value set by the `null` application env key (see `glazer`) |

Atom-keyed maps are supported: a `properties` or `required` name `~"name"` matches either the binary key `~"name"` or the existing atom `name`. Keys that do not correspond to an existing atom are never created.

---

## Reading errors

A successful validation returns `ok`. A failed one returns an `error` tuple. The shape depends on the mode (see [Validation options](#validation-options)).

### Fail-fast (default)

Validation stops at the first error:

**Erlang**

```erlang
{error, {path, Path, {Reason, Detail}}}
```

**Elixir**

```elixir
{:error, {:path, path, {reason, detail}}}
```

**Erlang**

```erlang
{error, {path, [~"age"], {value_too_small, 0}}}
```

**Elixir**

```elixir
{:error, {:path, ["age"], {:value_too_small, 0}}}
```

### Collect all errors

With `collect_all_errors` the result is a list of the same error bodies:

**Erlang**

```erlang
{error, [
  {path, [~"name"], {string_too_short, ~""}},
  {path, [~"age"],  {value_too_small, 0}}
]}
```

**Elixir**

```elixir
{:error, [
  {:path, ["name"], {:string_too_short, ""}},
  {:path, ["age"], {:value_too_small, 0}}
]}
```

### Path

`Path` locates the failing value, from the root:

- object keys are binaries (or atoms, if the data used atom keys);
- array indexes are 0-based integers.

**Erlang**

```erlang
{error, {path, [~"items", 1, ~"sku"], {missing_required, ~"sku"}}}
```

**Elixir**

```elixir
{:error, {:path, ["items", 1, "sku"], {:missing_required, "sku"}}}
```

The path of a `missing_required` error is the object that lacks the key, so the example above would have the empty path `[]` at the top level, and `[~"items", 1]` for an element of `items`.

### Reason and detail

`Reason` is an atom saying what failed. `Detail` is the offending value or a related number, depending on the reason. Common reasons:

| Reason | Raised by | Detail |
|---|---|---|
| `type_mismatch` | `type` | the value that has the wrong type |
| `value_too_small` / `value_too_large` | `minimum`, `exclusiveMinimum`, `maximum`, `exclusiveMaximum` | the bound |
| `not_multiple_of` | `multipleOf` | the divisor |
| `string_too_short` / `string_too_long` | `minLength`, `maxLength` | the string |
| `pattern_mismatch` | `pattern` | the string |
| `missing_required` | `required` | the missing key |
| `extra_properties` | `additionalProperties: false`, `unevaluatedProperties` | the key |
| `too_few_items` / `too_many_items` | `minItems`, `maxItems` | the list |
| `duplicate_item` | `uniqueItems` | the duplicated item |
| `too_few_properties` / `too_many_properties` | `minProperties`, `maxProperties` | the map |
| `no_schema_matched` | `anyOf` | the value |
| `multiple_schemas_matched` | `oneOf` | the value |
| `all_schemas_failed` | `allOf` | the value |
| `format_invalid` | `format` | the string |
| `dependent_keys_missing` | `dependentRequired` | the missing keys |
| `dependent_schema_failed` | `dependentSchemas` | the value |
| `not_const` | `const` | the expected value |
| `not_in_enum` | `enum` | the value |
| `contains_failed` | `contains`, `minContains`, `maxContains` | the match count |
| `then_validation_failed` / `else_validation_failed` | `if` / `then` / `else` | the value |
| `unresolved_ref` | `$ref` | the reference |
| `circular_reference` | `$ref` | the reference |
| `max_depth_exceeded` | any (nesting limit) | `nil` |
| `timeout` | any (`timeout_ms`) | the `timeout_ms` limit |
| `regex_error` | `pattern`, `patternProperties`, custom `formats` | the regex engine's error code (for example PCRE2's match-limit error). The regex engine failed; this is not a mismatch. |
| `timeout` | any (`timeout_ms`) | the `timeout_ms` limit |
| `expected_string`, `invalid_term_type` | term shape checks | the value |

Some reasons are internal annotations that show up only in specific situations (for example `schema_matched`). If you see a reason not in the table, match on `{path, _, {_, _}}` and log it; the set can grow.

### Formatting errors

**Erlang**

```erlang
format_error(Reason) ->
  case Reason of
    {path, Path, {value_too_small, Min}} ->
      io_lib:format("~p must be at least ~p", [Path, Min]);
    {path, Path, {missing_required, Key}} ->
      io_lib:format("~p is required in ~p", [Key, Path]);
    {path, Path, {Other, _}} ->
      io_lib:format("~p failed: ~p", [Path, Other])
  end.
```

**Elixir**

```elixir
def format_error(reason) do
  case reason do
    {:path, path, {:value_too_small, min}} ->
      "#{inspect(path)} must be at least #{min}"

    {:path, path, {:missing_required, key}} ->
      "#{key} is required in #{inspect(path)}"

    {:path, path, {other, _}} ->
      "#{inspect(path)} failed: #{other}"
  end
end
```

---

## Validation options

Options for `validate/3` and `validate_term/3` are a list of atoms and `{Key, Value}` tuples:

| Option | Default | Meaning |
|---|---|---|
| `fail_fast` | yes | Stop at the first error. Returns one error tuple. |
| `collect_all_errors` | no | Collect errors across the instance. Returns a list. Unless `max_errors` is set, no limit. |
| `{max_errors, N}` | `1` (fail-fast) | Stop after N errors. Ignored unless collecting. |
| `{max_depth, N}` | `128` | Maximum nesting depth. Deeper values fail with `max_depth_exceeded`. |
| `{timeout_ms, N}` | `0` (none) | Stop validation after N milliseconds; the result is `{error, {path, _, {timeout, N}}}`. |

**Erlang**

```erlang
glazer_schema:validate(Compiled, Data, [collect_all_errors]).
glazer_schema:validate(Compiled, Data, [collect_all_errors, {max_errors, 10}]).
glazer_schema:validate(Compiled, Data, [{max_depth, 32}]).
```

**Elixir**

```elixir
Glazer.Schema.validate(compiled, data, [:collect_all_errors])
Glazer.Schema.validate(compiled, data, [:collect_all_errors, max_errors: 10])
Glazer.Schema.validate(compiled, data, max_depth: 32)
```

Unknown options are ignored.

### Timeouts

`timeout_ms` bounds the walk. When the deadline passes, validation stops early and the result is an error with the reason `timeout` and the limit as detail:

**Erlang**

```erlang
{error, {path, [], {timeout, 50}}}
```

**Elixir**

```elixir
{:error, {:path, [], {:timeout, 50}}}
```

A validation that was cut short is never reported as `ok`, even inside `not` or `anyOf`. In collect mode the timeout error is appended to whatever genuine errors were found before the deadline. Errors that would only be known after the deadline are not recorded.

---

## Compile options

**Erlang**

```erlang
{ok, Compiled} = glazer_schema:compile(Schema, [
  {refs, #{~"https://example.com/address.json" => AddressSchema}},
  {format_assertion, true},
  {registry, true}
]).
```

**Elixir**

```elixir
{:ok, compiled} =
  Glazer.Schema.compile(schema,
    refs: %{"https://example.com/address.json" => address_schema},
    format_assertion: true,
    registry: true
  )
```

| Option | Default | Meaning |
|---|---|---|
| `{registry, Bool}` | `true` | Add the compiled schema to the schema registry, so the same schema and options compile once. `false` compiles without registering. See [Registry by default](#registry-by-default). |
| `{formats, #{Name => Pattern}}` | `#{}` | Custom formats, as binary names mapped to binary regular expressions. A string value must match its pattern; it fails with `format_invalid`. A name here replaces the built-in format of the same name. See [Custom formats](#custom-formats). |
| `{name, Name}` | none | The registry name (a binary or a string), used when the schema has no top-level `$id`. If the schema has a `$id`, `Name` must be the same; otherwise the call returns `{error, <<"name option differs from the schema's $id">>}`, with or without `{registry, false}`. With `{registry, false}` the name is checked but not registered. |
| `{refs, Map}` | `#{}` | Documents that `$ref` can resolve to, keyed by URI. See [References](#references-and-documents). |
| `{format_assertion, Bool}` | `true` | When `true`, `format` is enforced. When `false`, `format` is an annotation only. See [Formats](#formats). |

Any other option is rejected by the compiler. A non-boolean `registry` value returns `{error, ~"registry must be true or false"}`.

---

## Supported keywords

Glazer implements the 2020-12 core and validation vocabularies that matter for data validation. Keywords not listed here (such as `title`, `description`, `$comment`, `examples`, `default`) are accepted and ignored, as the specification allows for annotations.

### Types and values

- `type`: `"null"`, `"boolean"`, `"integer"`, `"number"`, `"string"`, `"array"`, `"object"`, or a list of these
- `enum`, `const`

Note that `integer` accepts a float with an integral value (`1.0` is an integer for `type`), as 2020-12 specifies.

### Numbers

- `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`
- `multipleOf`

Integers are compared exactly at any size, including bignums beyond 64 bits. A bound or a `multipleOf` factor that is an integer is kept exact; a float bound is compared as a double.

### Strings

- `minLength`, `maxLength` count Unicode code points, not bytes
- `pattern` (regular expression, see [Regular expressions](#regular-expressions))
- `format` (see [Formats](#formats))

### Arrays

- `items` (schema for every element not covered by `prefixItems`)
- `prefixItems` (tuple validation)
- `contains`, `minContains`, `maxContains`
- `minItems`, `maxItems`
- `uniqueItems`
- `unevaluatedItems`

### Objects

- `properties`, `patternProperties`, `additionalProperties`
- `propertyNames`
- `required`
- `minProperties`, `maxProperties`
- `dependentRequired`, `dependentSchemas`
- `unevaluatedProperties`

### Combining schemas

- `allOf`, `anyOf`, `oneOf`, `not`
- `if`, `then`, `else`

### Referencing

- `$ref`, `$defs`, `$anchor`, `$id`
- `$dynamicRef`, `$dynamicAnchor`

Recursive schemas work through `$ref`. Self-references are bounded by the depth limit: a schema that refers back to itself on the same value fails with `max_depth_exceeded` rather than looping.

### Unevaluated keywords

`unevaluatedProperties` and `unevaluatedItems` consider properties and items that were evaluated by sibling and applicator subschemas (`properties`, `patternProperties`, `prefixItems`, `items`, `contains`, `allOf`, `anyOf`, `oneOf`, `if`/`then`/`else`, `$ref`, and so on), as 2020-12 requires.

---

## References and documents

A `$ref` resolves in this order:

1. `$defs` and `$anchor` in the same document (for example `#/$defs/address` or `#address`);
2. documents passed in `{refs, Map}` at compile time, keyed by URI;
3. documents in the [schema registry](#schema-registry) under that URI. A registered schema's name is its `$id`, so a schema compiled earlier with `"$id": "https://example.com/address.json"` is found by any later `$ref` to that URI, with no `refs` option.

Registry documents that they refer to are found too, however deep the chain goes.

**Erlang**

```erlang
Address = #{~"type" => ~"object",
            ~"required" => [~"city"]},

Schema = #{
  ~"$id" => ~"https://example.com/person.json",
  ~"type" => ~"object",
  ~"properties" => #{
    ~"home" => #{~"$ref" => ~"https://example.com/address.json"}
  }
},

{ok, C} = glazer_schema:compile(Schema,
            [{refs, #{~"https://example.com/address.json" => Address}}]).
```

**Elixir**

```elixir
address = %{"type" => "object", "required" => ["city"]}

schema = %{
  "$id" => "https://example.com/person.json",
  "type" => "object",
  "properties" => %{"home" => %{"$ref" => "https://example.com/address.json"}}
}

{:ok, c} = Glazer.Schema.compile(schema, refs: %{"https://example.com/address.json" => address})
```

Glazer never fetches documents over the network or from disk. A reference that is not in the document, not in `refs`, and not in the registry fails at compile time with `unresolved $ref (document not provided)`. Load the documents yourself, either by compiling them (with their `$id`) before the schemas that use them, or by passing them in `refs`.

An explicit `refs` entry takes precedence over a registered document with the same URI.

Schemas that refer to each other are registered together, as a group: `glazer_schema:register_group([A, B])`. Each schema is compiled with the others as its documents, so every reference resolves, and nothing is stored unless every schema compiles. See [Schema registry](#schema-registry).

**Erlang**

```erlang
{ok, _} = glazer_schema:compile(#{~"$id" => ~"https://example.com/address.json",
                                       ~"type" => ~"object",
                                       ~"required" => [~"city"]}),
{ok, Person} = glazer_schema:compile(#{~"type" => ~"object",
                 ~"properties" => #{~"home" => #{~"$ref" => ~"https://example.com/address.json"}}}).
```

**Elixir**

```elixir
{:ok, _} = Glazer.Schema.compile(%{"$id" => "https://example.com/address.json",
                                    "type" => "object",
                                    "required" => ["city"]})

{:ok, person} =
  Glazer.Schema.compile(%{"type" => "object",
                          "properties" => %{"home" => %{"$ref" => "https://example.com/address.json"}}})
```

---

## Formats

`format` is enforced by default (`{format_assertion, true}`). Under 2020-12 the default is annotation-only; Glazer asserts it because most applications want that. Pass `{format_assertion, false}` to turn it off.

| Format | Specification | Notes |
|---|---|---|
| `date-time` | RFC 3339 | Accepts leap seconds (`23:59:60`) when they are valid for the date; RFC 9557 bracketed suffixes accepted |
| `date` | RFC 3339 | `YYYY-MM-DD` with a valid day for the month (leap years included) |
| `time` | RFC 3339 | Rejects leap seconds |
| `email` | RFC 5321 | ASCII mailbox, quoted local parts, IP literals |
| `idn-email` | RFC 6531 | Non-ASCII local part allowed; domain is an `idn-hostname` |
| `hostname` | RFC 1123 | ASCII labels, 1–63 octets each, 253 total |
| `idn-hostname` | IDNA2008 with UTS 46 mapping | See below |
| `ipv4` | RFC 2673 | Four decimal octets; leading zeros are rejected |
| `ipv6` | RFC 4291 | |
| `uri` | RFC 3986 | Absolute URI |
| `uri-reference` | RFC 3986 | Absolute or relative |
| `iri`, `iri-reference` | RFC 3987 | Unicode versions of the above |
| `uuid` | RFC 4122 / RFC 9562 | Canonical 8-4-4-4-12 form, case-insensitive |
| `duration` | ISO 8601 | Contiguous unit run, for example `P1Y2M3DT4H5M6S` or `P2W` |
| `json-pointer` | RFC 6901 | |
| `relative-json-pointer` | draft-handrews-relative-json-pointer | |
| `uri-template` | RFC 6570 | |
| `regex` | ECMA-262 | Compiled with the regex engine in use (see below) |

Unknown format names pass. Non-string values always pass the format check, as the specification requires.

### Custom formats

**Erlang**

```erlang
{ok, C} = glazer_schema:compile(#{~"format" => ~"sku"},
            [{formats, #{~"sku" => ~"^[A-Z]{3}-[0-9]{4}$"}}]),
ok = glazer_schema:validate(C, ~"ABC-1234"),
{error, {path, [], {format_invalid, ~"abc-1"}}} = glazer_schema:validate(C, ~"abc-1").
```

**Elixir**

```elixir
{:ok, c} = Glazer.Schema.compile(%{"format" => "sku"},
                                  formats: %{"sku" => "^[A-Z]{3}-[0-9]{4}$"})

:ok = Glazer.Schema.validate(c, "ABC-1234")
{:error, {:path, [], {:format_invalid, "abc-1"}}} = Glazer.Schema.validate(c, "abc-1")
```

- The pattern is compiled with the regular expression engine at compile time, so an invalid pattern fails the compile.
- The pattern is unanchored, as with `pattern`: use `^` and `$` to match the whole value.
- Custom formats are part of the registry key, so changing a pattern compiles a new schema.
- A custom format is asserted only when formats are asserted (`{format_assertion, false}` turns both off).

### idn-hostname and idn-email

Internationalized domains go through **UTS 46 mapping** first (full-width characters to ASCII, ideographic full stops to dots, ignored code points such as zero-width spaces removed, and so on), then NFC normalization, then the IDNA2008 rules (RFC 5891 and RFC 5893). The UTS 46 data is generated from ICU into the source tree, so results follow the Unicode version it was built from.

Consequences you may notice:

- `café.com` with a decomposed `é` is accepted: it is normalized to NFC.
- `ＡＢＣ.example` is accepted: it maps to `abc.example`.
- ASCII outside the LDH set (`$`, `_`, space) is rejected, even though UTS 46 would map some of them.
- A-labels (`xn--…`) must decode to a valid U-label and re-encode to the same form.
- Each label is at most 63 octets in its ASCII form, and the whole name at most 253.

### Format assertion off

**Erlang**

```erlang
{ok, C} = glazer_schema:compile(#{~"format" => ~"email"},
                                     [{format_assertion, false}]),
ok = glazer_schema:validate(C, ~"not-an-email").
```

**Elixir**

```elixir
{:ok, c} = Glazer.Schema.compile(%{"format" => "email"}, format_assertion: false)
:ok = Glazer.Schema.validate(c, "not-an-email")
```

---

## Regular expressions

`pattern` and `patternProperties` use the regular expression engine built into the NIF. The `glazer:info/0` map reports which one: `regex_engine => pcre2` or `srell`.

- Patterns are compiled at schema compile time. An invalid pattern fails the compile.
- Patterns are **not anchored**. `"pattern": "a"` matches `"cat"`. Anchor with `^` and `$` when you mean a whole-string match, as the specification requires.
- The regular expression dialect is the one 2020-12 specifies (ECMA-262 style). Syntax the chosen engine does not support may be rejected at compile time.
- Under the SRELL engine, patterns with nested quantifiers (for example `(a+)+`) are rejected at compile time to avoid catastrophic backtracking.

---

## Data model: what gets validated

- **Strings** are UTF-8 binaries. `minLength` and `maxLength` count code points.
- **Numbers**: integers are exact at any size, including bignums, against integer and float bounds alike. Floats are compared as doubles. Integer-valued floats satisfy `integer`. A float value and an integer bound are compared exactly as well.
- **Objects** can have binary or atom keys (see above). Keys that are not in `properties` are ignored unless a rule applies to them.
- **Arrays** are Erlang lists.
- **Booleans** are the atoms `true` and `false`.
- **Null** is the atom `null` unless you configured another atom (`{glazer, [{null, nil}]}`); data uses that atom.

---

## Limits, known issues and what is not supported

**Registry growth:** schemas compiled with the default `{registry, true}` stay in the VM until `glazer_schema:clear_interned/0` is called. Schemas built from per-request data should pass `{registry, false}`. See [Registry by default](#registry-by-default).

**Known gaps:**

- **Decimals beyond double precision.** Integers of any size are exact, but a decimal number in JSON text is decoded as a double, so its digits beyond double precision are lost before validation.
- **Floating-point `multipleOf`.** A finite quotient is tested with a relative tolerance of 1e-9, so a value slightly off a multiple can be accepted. When the quotient overflows, the test is exact (`fmod`), so no tolerance applies there.
- **Regex engine.** `pattern` and `patternProperties` use PCRE2 or SRELL, which are close to ECMA-262 but not identical. An engine failure is reported as `regex_error` with the engine's code, not as a mismatch. PCRE2 reports the code it returns (for example a match-limit or JIT stack-limit error). SRELL reports only exceptions, so its code is `-1`. A JIT stack-limit error is reported as is; the match is not retried without JIT.
- **Performance.** Validation costs about 50 ns per array element, mostly bytecode dispatch. Property names are cached per validation, so objects in an array share their key terms; a single object with many distinct names gets no such reuse.

**Not supported:**

- Remote `$ref` fetching. Pass documents with `refs`.
- Draft 3, 4, 6, 7 and 2019-09 keyword forms (for example `dependencies`, `additionalItems` with array-form `items`, `$recursiveRef`). Use 2020-12 forms.
- Decimal numbers with more precision than a double (JSON text such as `0.12345678912345678`) are decoded as doubles, so the exact decimal text is not compared.
- ECMAScript-only regex syntax that the engine does not support (such patterns may be rejected at compile time).
- `idn-email` and `idn-hostname` follow IDNA2008 with UTS 46 mapping, not a pure IDNA2003 reading. The bidi rule of RFC 5893 is applied across the domain: once any label contains a right-to-left character, every label must satisfy it, so `0a.א` is invalid.

---

## Schema registry

`glazer_schema` keeps compiled schemas under names, so code can fetch and validate by name without recompiling or passing schema handles around. A schema's name is its top-level `$id` (without any fragment). A schema without `$id` can still be registered under a name you choose.

**Erlang**

```erlang
%% At startup or on deploy: the name is the $id
{ok, 1} = glazer_schema:register(UserSchema),         % name: "https://example.com/user.json"
{ok, 1} = glazer_schema:register(UserSchema),         % same schema: unchanged
{ok, 2} = glazer_schema:register(~"user", NewSchema), % explicit name, new version

%% Anywhere
ok = glazer_schema:validate(~"user", Data),
{ok, Compiled} = glazer_schema:fetch(~"user"),        % {schema, Resource}
{error, {unknown_schema, ~"order"}} = glazer_schema:validate(~"order", Data).
```

**Elixir**

```elixir
# At startup or on deploy: the name is the $id
{:ok, 1} = Glazer.Schema.register(user_schema)      # name: "https://example.com/user.json"
{:ok, 1} = Glazer.Schema.register(user_schema)      # same schema: unchanged
{:ok, 2} = Glazer.Schema.register("user", new_schema)   # explicit name; version 2 after an earlier registration of "user"

# Anywhere
:ok = Glazer.Schema.validate("user", data)
{:ok, compiled} = Glazer.Schema.fetch("user")
{:error, {:unknown_schema, "order"}} = Glazer.Schema.validate("order", data)
```

`glazer_schema:compile/1,2` registers a schema with a `$id` under that name by default (see [Registry by default](#registry-by-default)), so a schema compiled once can be fetched and validated by name, and used by `$ref` from other schemas.

| Function | Returns |
|---|---|
| `register(Schema)` | `{ok, Version}`, named by the schema's `$id`; `{error, Message}` if it has none |
| `register(Name, Schema)`, `register(Name, Schema, CompileOpts)` | `{ok, Version}` or `{error, Message}` (compile error; nothing stored) |
| `name_of(Schema)` | `{ok, Name}` (the `$id` without fragment) or `error` |
| `resolve_refs(Schema, Options)` | `Options` with a `refs` entry holding the registered documents that `Schema` refers to |
| `fetch(Name)` | `{ok, {schema, Resource}}` or `{error, not_found}` |
| `'fetch!'(Name)` | `{schema, Resource}` or raises `{unknown_schema, Name}` |
| `validate(Name, Data)`, `validate(Name, Data, Opts)` | the `glazer_schema:validate` result, or `{error, {unknown_schema, Name}}` |
| `version(Name)` | `{ok, Version}` or `{error, not_found}` |
| `list()` | `[{Name, Version}]`, sorted by name |
| `register_group(Schemas)`, `register_group(Schemas, CompileOpts)` | `{ok, [{Name, Version}]}`: schemas that refer to each other, registered together (every schema needs a `$id`; all or nothing) |
| `unregister(Name)` | `ok` (unknown names are ignored) |
| `clear_interned()` | the number of schemas removed; clears the entries added by default `compile` calls. Named registrations are not affected. |

How it is stored and why:

- **Reads are one lookup.** The compiled handle lives in `persistent_term`, so `fetch/1` and `validate/2` read it from any process without copying or locking. No process is started; the registry works without a supervisor.
- **Writes are rare and serialized.** `register` and `unregister` take a `global:trans` lock and replace the stored entry. Each write triggers a global garbage collection in the VM, which is fine for schemas registered at startup or deploy, and not for registering on every request.
- **Re-registering the same schema is free.** If the name already holds the same schema and options, nothing is recompiled and the version is unchanged.
- **Versions** start at 1 and increase on each change of a name. Unregistering a name drops its history, so a later registration under that name starts again at 1.
- **Names** can be any term (atoms, binaries, tuples). A schema with a `$id` is named by it. Two different schemas under the same name are a change, not an error: the name gets a new version, and handles returned earlier still work.

The registry holds schemas only for the life of the VM. Persist the source schema yourself if you need it across restarts, and register it again at startup. Register referenced documents before the schemas that `$ref` them, or at least in the same startup step; a `$ref` is resolved when the schema is compiled.

---

## Performance

- **Compile once.** Compilation is the expensive step; keep the `{schema, Resource}` handle.
- **Use `validate`, not `validate_term`,** in hot paths. `validate_term` compiles the schema on every call. With several schemas, register them and use `glazer_schema:validate/2`.
- **Prefer fail-fast** for the common valid-or-not check. Use `collect_all_errors` only when the user needs every problem (form feedback, for example).
- **Bound the input.** `max_depth` (default 128) protects against deeply nested documents; limit request sizes before decoding.
- **Avoid unbounded `pattern`s** on large strings; a regular expression is linear only for the simple patterns, and the SRELL build rejects nested quantifiers for that reason.

---

## Troubleshooting

**`{error, ~"schema must be a map"}` from compile.** The schema is not a map. Check that you pass the decoded schema, not its JSON text. Use `glazer_json:decode/1` on a JSON string first.

**`unknown type: …` from compile.** A `type` value is not one of the seven JSON types.

**`unresolved $ref (document not provided)`.** The referenced URI is neither a `$id` in this document nor in `refs`. Add the document to `refs`, or check the URI for a trailing slash or a different scheme.

**`format_invalid` for a value you expected to pass.** Check the format name and the value. For `date-time`, leap seconds must be valid for the date. For `hostname`, labels cannot start or end with a hyphen. For `idn-hostname`, see the note on UTS 46 mapping above.

**`missing_required` for a key that is present.** The key may be an atom in the data but not in the schema, or a binary in the schema but an atom that does not exist yet. Use the same key type in both, or check that the atom exists (keys are never created by validation).

**`max_depth_exceeded`.** The data is more deeply nested than `max_depth`. Raise the limit only if the data legitimately needs it.

**Validation is slow.** Check that you compile once. If you use `collect_all_errors` on large arrays with many errors, set `{max_errors, N}`.

---

## Conformance testing

Glazer's validator is checked against the official [JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite) for 2020-12. Run it with:

```sh
make test-jsts
```

The runner is `test/json_schema_suite.es`. It reports in-scope results (2020-12 groups, with format assertions on), and separately counts the groups it excludes: other drafts, optional features, proposals, bignums, and a few upstream issues. The in-scope suite passes in full.

Unit tests for individual features live in `test/glazer_json_schema_test.erl` (EUnit).

## Standards compliance

Schema validation targets [JSON Schema 2020-12](https://json-schema.org/specification): the core, applicator, validation and unevaluated vocabularies. Compliance is checked against the official [JSON-Schema-Test-Suite](https://github.com/json-schema-org/JSON-Schema-Test-Suite), pinned at commit `38dea93`. Every in-scope test passes (709 of 709): groups whose `compatibility` covers 2020-12 and that use no legacy keywords. Run it with `make test-jsts`.

Out of scope, and so not claimed: other drafts (draft 3 to 2019-09 keyword forms), the suite's `optional/` and proposal groups, decimals with more precision than a double, and ECMAScript-only regex syntax. Integers of any size are exact (see the bignum note under Deviations). Groups excluded for an upstream issue (json-schema-org/JSON-Schema-Test-Suite#1277) are listed in `test/json_schema_suite.es`.

### Formats

`format` is asserted by default (see [deviations](#deviations) below).

| `format` | Standard | Notes |
|---|---|---|
| `date-time` | RFC 3339 §5.6 | Leap second `23:59:60` accepted only where it is a valid UTC instant. RFC 9557 bracketed suffixes accepted (an extension). |
| `date` | RFC 3339 §5.6 (`full-date`) | Valid day for the month, leap years included |
| `time` | RFC 3339 §5.6 (`full-time`) | Leap seconds rejected |
| `duration` | ISO 8601 as profiled by JSON Schema | Contiguous unit run; weeks (`P2W`) accepted |
| `email` | RFC 5321 (`Mailbox`, ASCII) | Quoted local parts and IP-literal domains |
| `idn-email` | RFC 6531 | UTF-8 local part; domain checked as `idn-hostname` |
| `hostname` | RFC 1123 | 1–63 octets per label, 253 in total |
| `idn-hostname` | RFC 5890, 5891, 5892, 5893 (IDNA2008); RFC 3492 (Punycode); UTS #46 mapping; UAX #15 (NFC) | Mapping, NFC, label rules (RFC 5892 appendix A), and the RFC 5893 bidi rule across the domain |
| `ipv4` | RFC 2673 (dotted quad) | Octets as RFC 3986 `dec-octet`: no leading zeros |
| `ipv6` | RFC 4291 §2.2 | Text forms, including an embedded IPv4 tail |
| `uri` | RFC 3986 | Absolute URI |
| `uri-reference` | RFC 3986 | Absolute or relative reference |
| `iri` | RFC 3987 | Absolute IRI |
| `iri-reference` | RFC 3987 | IRI or relative reference |
| `uuid` | RFC 9562 (canonical form, as in RFC 4122) | 8-4-4-4-12 hexadecimal, case-insensitive |
| `json-pointer` | RFC 6901 | |
| `relative-json-pointer` | draft-handrews-relative-json-pointer (not an RFC) | |
| `uri-template` | RFC 6570 | |
| `regex` | ECMA-262 syntax | Valid when it compiles with the engine in use (see below) |

Unknown format names pass, as the specification requires for annotations. Custom formats can be defined with the `formats` compile option (`#{Name => Pattern}`, matched as a regular expression). This is an extension; it is not part of any standard.

### Regular expressions

`pattern`, `patternProperties` and the `regex` format use ECMA-262 syntax, as 2020-12 requires. The matching engine is PCRE2 when `libpcre2-8` is available, otherwise [SRELL](https://github.com/upa-url/srell); `glazer:info/0` reports which one (`regex_engine`). Both are close to ECMA-262 but are not the same implementation: check unusual patterns with the engine you deploy.

### Deviations

These are the places where behaviour differs from the specification on purpose or where the specification leaves a choice:

- **`format` is asserted by default.** 2020-12 treats `format` as an annotation by default. Glazer enforces it; pass `{format_assertion, false}` to keep the specification's default.
- **`date-time` accepts RFC 9557 suffixes**, which RFC 3339 does not define.
- **`$ref` never fetches documents.** References resolve from the schema itself, from the `refs` option, or from the [schema registry](#schema-registry). Unknown references fail at compile time.
- **Decimals beyond double precision.** Integers of any size are compared exactly, but decimal numbers in JSON text are decoded as doubles, so digits beyond double precision are not compared.
- **Floating-point `multipleOf`** uses a relative tolerance of 1e-9 on the quotient, so a value slightly off a multiple can be accepted. An overflowing quotient is tested exactly instead.
- **Regex engine failures are errors, not mismatches.** They are reported as `regex_error` with the engine's code. SRELL reports only exceptions, so its code is `-1`. The regex engine is PCRE2 or SRELL, which are close to ECMA-262 but not identical.
- **`timeout_ms`** is not part of any standard. When it stops a validation early, the result is an error, never `ok`.
- **The schema registry** is not part of any standard.
