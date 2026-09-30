# Integration Testing Glazer

This directory contains an integration test application that verifies Glazer works correctly as an Elixir dependency.

## Quick Start

```bash
# From the integration directory
mix deps.get
mix compile
mix test
```

Expected result: **19 passed (2 doctests, 17 tests)**

## What Gets Tested

### Core Functionality
- **Encoding**: JSON encoding via `Glazer.JSON.encode!/1` and `Glazer.JSON.encode/1`
- **Decoding**: JSON decoding via `Glazer.JSON.decode!/1` and `Glazer.JSON.decode/1`
- **Data Types**: Maps, lists, nested structures, booleans, null values
- **Round-trip**: Encode → Decode to verify data integrity
- **Incremental Parsing**: Streaming JSON parsing with `decode_start/3`

### Error Handling
- Proper exception raising on invalid input
- Graceful error returns with `{:ok, value}` and `{:error, reason}` tuples

## Module Structure

- **GlazerIntegration** — Wrapper module providing convenient Elixir API around `:glazer_json`
- **GlazerIntegrationTest** — ExUnit test suite with 17 functional tests and 2 doctests

## Using Glazer from Hex

The integration app currently uses a **local path dependency** (`{:glazer, path: "..", manager: :mix}`) to test with the latest fixes for PGO handling in dependency builds.

Once Glazer 1.1.3+ is released with the PGO fixes, you can use:

```elixir
defp deps do
  [
    {:glazer, "~> 1.1", manager: :mix},
    ...
  ]
end
```

## Dependency Build Notes

When Glazer is built as a dependency (in `_build/deps/`), the PGO optimization is gracefully skipped because `bin/pgo-profile.es` is not included in Hex distributions. The build system automatically detects this and builds without PGO profiling, which is the correct behavior for dependency installations.

For local development builds, PGO optimization runs normally to produce the fastest binary.
