# Vendored headers

Third-party C++ headers used by the NIF. Each one is header-only and is
included by path from `c_src/` (`vendors/<name>.hpp`).

| Header | Version | Used by | License |
|---|---|---|---|
| `glazer_atof.hpp` | fast_float v8.3.3 | JSON, YAML and CSV decoders (float parsing) | Apache-2.0, MIT or BSL-1.0 (your choice) |
| `glazer_ftoa.hpp` | zmij 1.0.23 port | JSON, YAML and CSV encoders (float output) | MIT |
| `srell.hpp` | SRELL 2026.10 (commit `c3bcf40`) | `glazer_regex.hpp` fallback when PCRE2 is missing | BSD 2-Clause |

## `glazer_atof.hpp`

Lemire's fast_float: fast, locale-independent, round-trip-correct parsing of
doubles from a character range (`from_chars`). Lives in namespace `glz::fp`.

- Source: https://github.com/fastfloat/fast_float
- Downloaded, not hand-edited (apart from the namespace rewrite). Fetch the latest tagged release with
  `make -C c_src fast-float`, or pin one with `FAST_FLOAT_TAG=v8.3.3`. The
  download rewrites the upstream `fast_float` namespace to `glz::fp`.

## `glazer_ftoa.hpp`

Shortest round-trip float-to-text (`glz::fp::ftoa`), a scalar port of zmij
1.0.23. Writes fixed notation for decimal exponents -5..15 and otherwise
`d.ddde+X`, and always includes `.` or `e`, so the output is valid JSON, YAML
and CSV number text. Hand-written, not downloaded.

- Source: https://github.com/dtolnay/zmij (MIT, copyright David Tolnay)

## `srell.hpp`

Header-only ECMAScript-style regular expressions (Unicode properties, UTF-8
traits). Used only when `libpcre2-8` is not installed; the native matcher in
`glazer_regex.hpp` handles most schema patterns, and SRELL handles the rest
(lookaround, backreferences and similar).

- Source: https://github.com/upa-url/srell
- Fetched by `c_src/Makefile` at the pinned commit (`SRELL_COMMIT`) and checked
  against a SHA-256 digest. The file is not committed to git.
- The BSD 2-Clause license text is inside the header itself.
