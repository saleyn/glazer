#!/usr/bin/env bash
# Compares glazer's C++ JSON code (the NIF's decoder and encoder) with sonic-rs,
# without the BEAM in the loop. Glazer's term construction goes through an
# in-memory stand-in for the erl_nif API (json_parse_bench.cpp and
# json_encode_bench.cpp), so the numbers show the parser and encoder work
# themselves, not VM calls.
#
# Usage:
#   c_src/tools/bench_nif_vs_sonic.sh                 # wall-clock, us per op
#   IR=1 c_src/tools/bench_nif_vs_sonic.sh            # also instructions per op (cachegrind)
#   PAYLOADS="openrtb twitter" c_src/tools/bench_nif_vs_sonic.sh
#
# Columns:
#   decode glazer   glazer's decoder (JSONDecoder), terms stubbed
#   decode sonic    sonic-rs Value tree (allocates, like glazer's terms)
#   parse  sonic    sonic-rs parse with a counting visitor (no allocation)
#   encode glazer   glazer's encoder (JSONEncoder) over the same term tree
#   encode sonic    sonic-rs to_vec over the same Value tree
#
# Requirements: g++, erl (for the ERTS include directory), cargo, valgrind for IR=1.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$ROOT/c_src/tools"
DATA="$ROOT/test/data"
OUT="$ROOT/_build/nif_vs_sonic"
CRATE="$TOOLS/sonic_parse_bench"
PAYLOADS="${PAYLOADS:-openrtb esad small twitter twitter2}"
IR="${IR:-0}"

mkdir -p "$OUT"
ERTS_INC="$(erl -noshell -eval 'io:format("~ts/erts-~ts/include",[code:root_dir(),erlang:system_info(version)]),halt().')"

# NIF flags (see c_src/Makefile, release profile).
NIF_FLAGS=(-std=c++2b -O3 -march=native -mtune=native -DNDEBUG -fPIC -I"$ROOT/c_src" -I"$ERTS_INC")

echo "==> building glazer harnesses" >&2
g++ "${NIF_FLAGS[@]}" "$TOOLS/json_parse_bench.cpp" "$ROOT/c_src/glazer_atoms.cpp" -o "$OUT/glazer_decode"
g++ "${NIF_FLAGS[@]}" "$TOOLS/json_encode_bench.cpp" "$ROOT/c_src/glazer_atoms.cpp" -o "$OUT/glazer_encode"

echo "==> building sonic-rs benchmarks" >&2
# rustup installs cargo in ~/.cargo/bin, which is often not on PATH in non-login shells.
if ! command -v cargo >/dev/null 2>&1 && [ -x "$HOME/.cargo/bin/cargo" ]; then
  export PATH="$HOME/.cargo/bin:$PATH"
fi
if ! command -v cargo >/dev/null 2>&1; then
  echo "error: cargo not found. Install Rust (https://rustup.rs) or add cargo to PATH." >&2
  exit 1
fi
(cd "$CRATE" && cargo build --release --quiet --bins)
SONIC_PARSE="$CRATE/target/release/sonic_parse_bench"
SONIC_TREE="$CRATE/target/release/sonic_decode_tree"
SONIC_ENC="$CRATE/target/release/sonic_encode"

files=()
for p in $PAYLOADS; do files+=("$DATA/$p.json"); done

# Timed run of one binary: prints "<payload> <us>" lines.
timed() {
  "$@" "${files[@]}" | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^us\//) { print $1, $(i-1) } }'
}

# Instructions per operation for one binary and payload. A run pays a fixed
# setup cost (reading the input, building the term or Value tree once), so the
# count is taken as the difference between N and 2N iterations, divided by N.
ir_count() {  # <binary> <payload-file> <iterations>
  PARSE_ITERS=$3 valgrind --tool=cachegrind --cachegrind-out-file=/dev/null "$1" "$2" 2>&1 \
    | awk '/I *refs/ { gsub(",", "", $4); print $4 }'
}
ir_per_op() {  # <binary> <payload-file>
  local bin="$1" file="$2" n a b
  case "$file" in
    *twitter2*) n=10 ;;
    *twitter*)  n=20 ;;
    *)          n=2000 ;;
  esac
  a=$(ir_count "$bin" "$file" "$n")
  b=$(ir_count "$bin" "$file" "$((2 * n))")
  echo $(( (b - a) / n ))
}

echo "==> timing" >&2
timed "$OUT/glazer_decode"  > "$OUT/glazer_decode.txt"
timed "$SONIC_TREE"         > "$OUT/sonic_tree.txt"
timed "$SONIC_PARSE"        > "$OUT/sonic_parse.txt"
timed "$OUT/glazer_encode"  > "$OUT/glazer_encode.txt"
timed "$SONIC_ENC"          > "$OUT/sonic_encode.txt"

# value_of <file> <payload>
value_of() { awk -v p="$2" '$1 ~ p"\\.json$" { print $2 }' "$1"; }

echo
echo "Wall-clock, microseconds per operation (min of 7 rounds):"
printf "%-10s | %12s %12s %12s | %12s %12s\n" payload "decode glazer" "decode sonic" "parse sonic" "encode glazer" "encode sonic"
printf "%s\n" "-----------------------------------------------------------------------------------------"
for p in $PAYLOADS; do
  printf "%-10s | %12s %12s %12s | %12s %12s\n" "$p" \
    "$(value_of "$OUT/glazer_decode.txt" "$p")" "$(value_of "$OUT/sonic_tree.txt" "$p")" \
    "$(value_of "$OUT/sonic_parse.txt" "$p")" \
    "$(value_of "$OUT/glazer_encode.txt" "$p")" "$(value_of "$OUT/sonic_encode.txt" "$p")"
done

if [ "$IR" = "1" ]; then
  echo "==> instruction counts" >&2
  echo
  echo "Instructions per operation (cachegrind; machine-independent):"
  printf "%-10s | %12s %12s %12s | %12s %12s\n" payload "decode glazer" "decode sonic" "parse sonic" "encode glazer" "encode sonic"
  printf "%s\n" "-----------------------------------------------------------------------------------------"
  for p in $PAYLOADS; do
    f="$DATA/$p.json"
    printf "%-10s | %12s %12s %12s | %12s %12s\n" "$p" \
      "$(ir_per_op "$OUT/glazer_decode" "$f")" "$(ir_per_op "$SONIC_TREE" "$f")" \
      "$(ir_per_op "$SONIC_PARSE" "$f")" \
      "$(ir_per_op "$OUT/glazer_encode" "$f")" "$(ir_per_op "$SONIC_ENC" "$f")"
  done
fi

echo
echo "Notes:"
echo "  - glazer's term construction is stubbed, so its decode time excludes BEAM"
echo "    allocation and map building costs; sonic's Value tree includes its allocations."
echo "  - 'parse sonic' is the parse-only reference (no tree), the closest to glazer's"
echo "    decode with term building removed."
echo "  - Timings depend on the machine; compare the instruction table when they disagree."
echo "  - Instruction counts exclude setup: they are (count at 2N - count at N) / N."
