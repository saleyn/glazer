// Standalone JSON prettify / minify
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace glz {

// Returns a prettified copy of `in` (2-space indent, assumes valid JSON).
inline std::string prettify_json(std::string_view in)
{
  std::string out;
  out.reserve(in.size() * 2);

  int  indent   = 0;
  bool in_str   = false;
  bool escape   = false;

  auto newline = [&] {
    out += '\n';
    for (int i = 0; i < indent * 3; ++i) out += ' ';
  };

  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];

    if (escape) { out += c; escape = false; continue; }
    if (in_str) {
      out += c;
      if (c == '\\') escape = true;
      else if (c == '"') in_str = false;
      continue;
    }

    switch (c) {
      case '"':
        out += c; in_str = true;
        break;
      case '{': case '[':
        out += c;
        // peek: if immediately closed, emit on same line (e.g. `[]`, `{}`)
        {
          size_t j = i + 1;
          while (j < in.size() && (in[j] == ' ' || in[j] == '\t' || in[j] == '\r' || in[j] == '\n')) ++j;
          if (j < in.size() && (in[j] == '}' || in[j] == ']')) break;
        }
        ++indent;
        newline();
        break;
      case '}': case ']':
        // empty collection (e.g. `[]`, `{}`): closing bracket immediately
        // follows the opening one, already emitted on the same line
        if (!out.empty() && (out.back() == '{' || out.back() == '[')) {
          out += c;
          break;
        }
        --indent;
        newline();
        out += c;
        break;
      case ',':
        out += c;
        newline();
        break;
      case ':':
        out += ':'; out += ' ';
        break;
      case ' ': case '\t': case '\r': case '\n':
        // strip existing whitespace outside strings
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// Returns a minified copy of `in` (assumes valid JSON).
inline std::string minify_json(std::string in)
{
  std::string out;
  out.reserve(in.size());

  bool in_str = false;
  bool escape = false;

  for (char c : in) {
    if (escape) { out += c; escape = false; continue; }
    if (in_str) {
      out += c;
      if (c == '\\') escape = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { out += c; in_str = true; continue; }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    out += c;
  }
  return out;
}

} // namespace glz
