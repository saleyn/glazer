# SRELL

Header-only C++ regular-expression library (ECMAScript grammar, Unicode
properties, UTF-8 traits). Used by `glazer_regex.hpp` as the fallback engine
when PCRE2 is not installed. The native matcher handles most schema patterns;
SRELL handles the rest (lookaround, backreferences, and similar).

- Source: https://github.com/upa-url/srell
- Pinned commit: `c3bcf40` (release tag `v2026.10`)
- Files fetched by `c_src/Makefile` (only when `libpcre2-8` is not found):
  - `srell-src/single-header/srell.hpp`
    (https://github.com/upa-url/srell/blob/c3bcf40/srell-src/single-header/srell.hpp)
    SHA-256 `c3bc022d72e3b984a222e0d84ee50473532dd9a9b2d93d3f77bf07d4c8e81b39`
  - `srell-src/license.txt`
    (https://github.com/upa-url/srell/blob/c3bcf40/srell-src/license.txt)
    SHA-256 `abd094e84637dd436cc33ab44ce8d59e9650acd1476fa32f32057c679b12b7e0`
- License: BSD 2-Clause (copyright Nozomu Katoo). The license text is saved
  as `LICENSE` next to the header when it is fetched.

The downloaded `srell.hpp` and `LICENSE` are not committed; this README is.
