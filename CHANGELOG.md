# Changelog

## Unreleased

### Breaking changes

- **JSON decoding validates UTF-8 by default.** A JSON string containing
  invalid UTF-8 (overlong forms, UTF-16 surrogates, code points above
  U+10FFFF, truncated sequences) is now a parse error, where it was previously
  copied into the result unchanged. The error message is
  `invalid UTF-8 in JSON string at offset N`. Input that is not in a string
  and contains invalid bytes now reports this UTF-8 error instead of a syntax
  error. To keep the previous behavior, pass `skip_utf8_validation`.
- **JSON encoding rejects invalid UTF-8 by default.** A binary (as a value or
  an object key) that is not valid UTF-8 raises
  `{encode_error, {<<"invalid UTF-8 in string">>, Binary}}`, where it was
  previously written to the output unchanged, which produced invalid JSON.
  `force_utf8` restores replacement: each invalid byte is written as U+FFFD.

### Bug fixes

- Atom names and charlist object keys that contain non-ASCII Latin-1
  characters (code points 128..255) are now written as UTF-8. Previously the
  raw Latin-1 bytes were written, which is not valid UTF-8.

### Performance

- UTF-8 validation uses a vector check (SSSE3 on x86, NEON on AArch64), with a
  scalar fallback on other targets. Typical ASCII text skips most of the work.
  Validation is on by default for decoding, so the decoder is about 5% to 11%
  slower on instruction count than with `skip_utf8_validation`, depending on
  how much of the input is multi-byte text.
- `force_utf8` no longer takes the slow escaping path for strings that are
  already valid UTF-8. On an object-heavy document it costs about 3% extra on
  encoding, where it previously cost about 35%.
- Encoding short strings (up to 16 bytes, the common case for keys and short
  values) uses word-sized checks and copies instead of a per-byte loop.
