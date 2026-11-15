// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// UTF-8 decoding shared by the JSON, YAML, CSV and schema-regex code.
//-----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string_view>

namespace glz {

// Decode one UTF-8 sequence starting at p (p < end). Returns the code point
// and advances p past the sequence. On invalid/truncated input, returns the
// Unicode replacement character (U+FFFD) and advances p by one byte.
inline uint32_t decode_utf8(const char*& p, const char* end)
{
  auto c    = (unsigned char)*p;
  auto cont = [&](const char* q) {
    return q < end && ((unsigned char)*q & 0xC0) == 0x80;
  };

  if (c < 0x80) [[likely]] { ++p; return c; }

  if ((c & 0xE0) == 0xC0 && cont(p+1)) {
    auto cp = (uint32_t(c & 0x1F) << 6)
            | (uint32_t((unsigned char)p[1]) & 0x3F);
    p += 2;
    return cp >= 0x80 ? cp : 0xFFFD;
  }
  if ((c & 0xF0) == 0xE0 && cont(p+1) && cont(p+2)) {
    auto cp = (uint32_t(c & 0x0F) << 12)
            | (uint32_t((unsigned char)p[1] & 0x3F) << 6)
            |  uint32_t((unsigned char)p[2] & 0x3F);
    p += 3;
    return (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) ? cp : 0xFFFD;
  }
  if ((c & 0xF8) == 0xF0 && cont(p+1) && cont(p+2) && cont(p+3)) {
    auto cp = (uint32_t(c & 0x07) << 18)
            | (uint32_t((unsigned char)p[1] & 0x3F) << 12)
            | (uint32_t((unsigned char)p[2] & 0x3F) << 6)
            |  uint32_t((unsigned char)p[3] & 0x3F);
    p += 4;
    return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : 0xFFFD;
  }

  ++p;
  return 0xFFFD;
}

// True when s is well-formed UTF-8 (U+FFFD is accepted only as its own
// three-byte encoding)
inline bool valid_utf8(std::string_view s)
{
  const char* p = s.data();
  const char* e = p + s.size();
  while (p < e) {
    const char* start = p;
    uint32_t cp = decode_utf8(p, e);
    if (cp == 0xFFFD && !(p - start == 3 && (unsigned char)start[0] == 0xEF &&
                          (unsigned char)start[1] == 0xBF && (unsigned char)start[2] == 0xBD))
      return false;
  }
  return true;
}

} // namespace glz
