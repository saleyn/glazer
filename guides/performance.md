# Performance Optimization Details

`glazer` is faster than the majority of competitors on both encoding and
decoding in all data formats - JSON/YAML/CSV. On JSON decoding it is comparable
to `torque` (Rust `sonic-rs` NIF), though on large payload `torque` performs
around 20% better. The performance depends on the OS and hardware architecture -
Intel and ARM produce different results. Both `torque` and `glazer` sit well
ahead of the remaining contenders (`simdjsone`, `jiffy`, and the pure-Elixir
libraries `jason`, `thoas`, `euneus`, and OTP's built-in `json`). On CSV it's
close competitor is also Rust-backended `rusty_csv` project, though that project
is missing encoding implementation. Here are some observations about `glazer`'s
design:

- **No tuple-of-binaries intermediate representation.** `glazer` decodes
  straight to native Erlang terms (maps, lists, binaries, numbers) and
  encodes straight from them, in a single pass, with no generic JSON-tree
  staging step — minimizing allocation and copying on both the decode and
  encode paths.
- **Big integer support.** numbers that overflow 64 bits decode to
  Erlang bignums (and encode back to their exact decimal form) — see
  [Big integers](../README.md#big-integers).
- **No external C++ dependencies.** The NIF is fully self-contained —
  no CMake, no vendored third-party library to pull at build time, so it's
  easier to use as a dependency since it doesn't have reliance on other
  toolchains such as `sonic-rs` by other libraries that use Rust.

A few implementation techniques in `c_src/glazer_nif.cpp` account for most
of the gap over the slower contenders:

- **Single-pass, zero-copy decode/encode.** As noted above, there's no
  intermediate generic JSON tree — the decoder builds Erlang terms directly
  from the input bytes and the encoder writes JSON bytes directly from
  Erlang terms. This removes a whole staging allocate-and-copy pass that
  tree-based decoders pay for.

- **Sub-binary string/field values (zero allocation on decode).** Shared
  across the JSON, YAML, and CSV decoders: unescaped scalars are returned as
  `enif_make_sub_binary` terms — a slice of the original input binary —
  rather than newly allocated copies. No `memcpy` or heap allocation occurs
  for the common case (JSON strings with no `\` escapes, CSV fields without
  embedded quotes, single-line YAML plain scalars). Only values that need
  unescaping or reassembly (escaped JSON strings, quoted/folded YAML
  scalars, CSV fields with doubled quotes) pay the copy cost. The
  `copy_strings` decode option opts back into copying for every value when
  decoded results are long-lived and the input is large (keeping one
  sub-binary alive would otherwise pin the entire input buffer in memory).

- **Inline, growable output buffer (`OutBuf`).** Encoding writes into a
  4 KB stack-allocated buffer first; only documents that exceed that spill
  to the heap, growing geometrically via `malloc`/`realloc` (the latter
  resizes in place when possible, avoiding a copy on every growth — a
  plain `new[]`/`delete[]` doubling strategy can't do this).

- **Pre-reserved worst-case output, raw-pointer inner loop.** Before
  encoding any string, `json_escape_string` and `emit_double_quoted`
  (YAML) call `out.ensure(len * 6 + 2)` once — the absolute worst case
  of six output bytes per input byte (`\uXXXX`) plus two quote characters.
  After that single reservation the inner loop writes through a raw `char*`
  pointer with no further bounds checks or `ensure()` calls. This removes
  a branch and a potential realloc from every character in the hot path.

- **Dense escape table (`ESCAPE_TAB`).** Instead of a per-character
  `switch` statement, a 256-entry `constexpr` table maps each byte to an
  `{len, seq[7]}` struct. Emitting an escape sequence is a single indexed
  table load followed by one `memcpy(dst, e.seq, e.len)` — branch-free
  and inlined by the compiler. The same table is shared by the JSON and
  YAML encoders via `glazer_common.hpp`.

- **Key cache for repeated object keys (`KeyCache`).** Real-world JSON
  documents reuse the same small set of key strings heavily (e.g. a
  Twitter feed has ~13K key occurrences across only ~94 distinct keys).
  `KeyCache` is an open-addressed hash table (power-of-two size, linear
  probing, FNV-1a hash with a precomputed-hash fast-reject before the
  `memcmp`) that lets a repeated key reuse the same already-built
  `ERL_NIF_TERM` binary instead of paying `enif_make_new_binary` + `memcpy`
  again. It's only engaged for inputs above a size threshold
  (`KEY_CACHE_MIN_SIZE`), since small payloads (RPC-sized messages) rarely
  repeat keys enough to amortize the lookup cost.

- **Epoch-counter lazy clearing.** Both `KeyCache` and the scratch buffers
  it touches need to start "empty" on every decode call, but
  zero-initializing a multi-KB table for every single call — including
  tiny documents that never populate it — would cost more than the cache
  saves. Instead each cache entry carries a generation/`epoch` tag; a slot
  is considered live only if its `epoch` matches the cache's current
  `m_epoch` (itself seeded from a process-wide monotonically-increasing
  counter, so leftover garbage from a prior stack frame can never
  coincidentally look live). This makes cache construction effectively
  free, regardless of table size.

- **SIMD string scanning (NEON / AVX2 / SSE2).** A shared `find_escape_pos`
  function in `glazer_common.hpp` scans for `"`, `\`, and control
  characters (`c < 0x20`) using an architecture cascade: AArch64 NEON
  (16 bytes/iter), x86 AVX2 (32 bytes/iter), SSE2 (16 bytes/iter), then a
  byte-table scalar fallback. Control-character detection uses a bias trick —
  XOR with `0x80` shifts the unsigned `< 0x20` range into a region where a
  single signed `vclt`/`cmpgt` instruction covers all 32 values at once,
  avoiding 32 separate equality checks. The same scanner is used by both
  the JSON and YAML string encoders. Separate SIMD scanners handle
  format-specific stop sets: `find_break` (YAML line-break scanner),
  `find_dq_special` (YAML `"` / `\` / LF / CR), `find_field_end` (CSV
  `delimiter | LF | CR`), and `find_csv_special` (CSV quoting check) — all
  with NEON, AVX2, and SSE2 paths.

- **SWAR whitespace skipping.** `skip_ws` checks the next byte before
  paying for any wider load, then — for runs of whitespace — scans 8 bytes
  at a time using branch-free bit-twiddling ("SIMD within a register") to
  find the first non-whitespace byte. Minified JSON (the overwhelmingly
  common case) has little or no structural whitespace, so the single-byte
  fast path dominates; the 8-byte path handles pretty-printed inputs.

- **Fast integer formatting.** Integers are written to JSON using a
  four-digit lookup table (`glz::BigInt`), which is faster than routing
  every integer through `snprintf`.

## Benchmarking

Benchmarking:
- [Benchmarking JSON](json.md#benchmarking-json)
- [Benchmarking YAML](yaml.md#benchmarking-yaml)
- [Benchmarking CSV](csv.md#benchmarking-csv)
