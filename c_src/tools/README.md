# c_src/tools

Development tools for glazer's C++ JSON code. None of these are part of the NIF
build (`c_src/Makefile` does not compile them), and none are needed to use the
library.

They fall into three groups: benchmarks that run without the BEAM, correctness
tests, and a generator.

## Benchmarks without the BEAM

The benchmarks link the real encoder or decoder from `glazer_json.hpp` against
an in-memory model of the `erl_nif` term API. Each API call costs a function
call and a few loads, so they measure the parser or encoder work itself, not
VM calls. Their numbers are useful for instruction and misprediction counts.
Wall-clock numbers from them do not match the BEAM, so use `mix bench` for
those.

Most of them can be run with `PARSE_ITERS=N`, which runs exactly N operations per
file. That mode is for cachegrind, where you subtract a 2N run from an N run to
cancel the setup cost. Without it, the tool calibrates and prints microseconds
per operation (minimum of 7 rounds).

- **`json_encode_bench.cpp`** — encodes the JSON files in `test/data` (given as
  arguments) using glazer's encoder. It builds each file into an in-memory term
  first.
- **`json_parse_bench.cpp`** — decodes the same files with glazer's decoder. Term
  construction is stubbed, so the time is spent in the parser, the key cache and
  the value bookkeeping.
- **`sonic_port_bench.cpp`** — a C++ port of the parts of sonic-rs's parser that
  differ from glazer's decoder. It is used to find which algorithms explain the
  difference. Parse only.
- **`bench_nif_vs_sonic.sh`** — builds the three C++ harnesses above, and the
  Rust crate in `sonic_parse_bench/`, then prints wall-clock and instruction
  tables side by side. Set `IR=1` to add the instruction table, and `PAYLOADS`
  to choose the files.
- **`sonic_parse_bench/`** — a Cargo crate with three binaries that run sonic-rs
  from the vendored `deps/torque` tree:
  - `sonic_parse_bench` (`src/main.rs`): parse only, with a counting visitor.
  - `sonic_decode_tree` (`src/bin/sonic_decode_tree.rs`): builds a `Value` tree,
    comparable to glazer's decode.
  - `sonic_encode` (`src/bin/sonic_encode.rs`): serialises a `Value` tree with
    `to_vec`, comparable to glazer's encode.

## Correctness tests

- **`utf8_check.cpp`** — checks `glz::utf8::valid` and `first_invalid` against a
  scalar reference. It covers every 2- and 3-byte sequence at several offsets
  inside a 16-byte block, and random byte strings. Build with `-march=native`.
- **`tail_page_test.cpp`** — checks the encoder's string tail handling
  (`emit_plain`). It maps two readable pages followed by a guard page, puts
  strings at the page end, and checks the output. A wrong read past the page
  faults on the guard page. Build with a plain `g++` and link `glazer_atoms.cpp`.

## Generator

- **`gen_idna_tables.cpp`** — generates `c_src/glazer_idna_tables.hpp` (IDNA2008
  tables, the UTS #46 mapping table and the NFC tables) from ICU's Unicode data.
  Run it only when the Unicode data needs regenerating. It needs ICU's development
  files (`pkg-config icu-uc`).

## Building

The C++ tools need the Erlang include directory:

```sh
ERTS_INCLUDE="$(erl -noshell -eval 'io:format("~ts/erts-~ts/include",[code:root_dir(),erlang:system_info(version)]),halt().')"
g++ -std=c++2b -O3 -march=native -DNDEBUG -fPIC -Ic_src -I"$ERTS_INCLUDE" \
    c_src/tools/json_encode_bench.cpp c_src/glazer_atoms.cpp -o json_encode_bench
```

Each file's header comment gives its exact build line. `bench_nif_vs_sonic.sh`
builds everything it needs.
