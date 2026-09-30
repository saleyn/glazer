# Glazer Integration Tests

This is a test application that verifies Glazer can be properly used as a dependency in an Elixir project.

## Setup

```bash
cd integration
mix deps.get
```

## Build

```bash
mix compile
```

## Test

```bash
mix test
```

## What it tests

- Basic JSON encoding with `glazer_json:encode/2`
- Basic JSON decoding with `glazer_json:decode/2`
- Incremental JSON decoding with `glazer_json:decode_start/1` and `glazer_json:decode_continue/2`
- Round-trip encoding and decoding of various data structures
- Handling of nested structures, arrays, booleans, and null values
