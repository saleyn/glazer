// Page-end test for JSONEncoder::emit_plain's tail handling.
//
// The encoder's 16-byte tail load must never leave the page that holds the
// string. This test maps two readable pages followed by a PROT_NONE guard page,
// places strings so that they end exactly at a page boundary (and at other
// offsets inside a page), and checks the encoded output. A wrong read past the
// end of the page faults on the guard page, and a wrong masked result changes
// the output or the return value.
//
// Build and run (any sanitizer setting works):
//   g++ -std=c++2b -O2 -I.. -I<erts>/include tail_page_test.cpp ../glazer_atoms.cpp -o tail_page_test
//   ./tail_page_test
#include "glazer_json.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

// The encoder's non-string paths reference erl_nif functions. This test only
// encodes binaries, so these are never reached; they exist to link.
ERL_NIF_TERM enif_make_atom(ErlNifEnv*, const char*) { return 0; }

namespace {

constexpr size_t PAGE = 4096;

int failures = 0;
int checks = 0;

void expect(bool ok, const char* what, size_t n, size_t off) {
  ++checks;
  if (!ok) {
    ++failures;
    if (failures <= 20)
      std::printf("FAIL %s (len %zu, offset %zu)\n", what, n, off);
  }
}

// Encode `s` through emit_plain. Returns the result and fills `out` with the bytes.
bool emit(const char* s, size_t n, std::string& out) {
  glz::JSONEncodeOpts opts{};
  glz::OutBuf buf;
  glz::JSONEncoder enc{nullptr, opts, buf};
  const bool ok = enc.emit_plain(s, n);
  out.assign(buf.m_data, buf.m_len);
  return ok;
}

// Reference: plain ASCII (no quote, backslash, control or high-bit byte) is
// emitted as-is with quotes around it. Any other byte makes emit_plain refuse.
bool is_plain(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c >= 0x80 || c == '"' || c == '\\') return false;
  }
  return true;
}

// Fill [p, p+n) with printable ASCII that needs no escaping.
void fill_plain(char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) p[i] = static_cast<char>('a' + (i % 26));
}

}  // namespace

int main() {
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (page < PAGE) { std::printf("unexpected page size %zu\n", page); return 2; }

  // P0 and P1 are readable; P2 is the guard that any stray read would hit.
  char* base = static_cast<char*>(mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (base == MAP_FAILED) { std::perror("mmap"); return 2; }
  if (mprotect(base + 2 * page, page, PROT_NONE) != 0) { std::perror("mprotect"); return 2; }

  char* const p1 = base + page;        // the page whose end we test
  char* const guard = base + 2 * page;

  // 1. Strings that end exactly at the guard boundary, lengths 1..80, plain and
  //    with one special byte at every position. The bytes between the end of the
  //    string and the guard are set to values that would be flagged if read as
  //    part of the string (quote, backslash, control), so a wrong mask shows up.
  const char specials[] = {'"', '\\', 0x01, static_cast<char>(0x80)};
  for (size_t n = 1; n <= 80; ++n) {
    char* s = guard - n;
    // Bytes after the string, inside the readable page only: none here, since
    // the string ends at the guard. The tail load past the end is therefore
    // never allowed; emit_plain must take the bounded path.
    fill_plain(s, n);
    std::string out;
    bool ok = emit(s, n, out);
    expect(ok, "plain at page end", n, 0);
    expect(ok && out == "\"" + std::string(s, n) + "\"", "plain output at page end", n, 0);

    for (char sp : specials) {
      for (size_t pos = 0; pos < n; ++pos) {
        fill_plain(s, n);
        s[pos] = sp;
        std::string o2;
        bool ok2 = emit(s, n, o2);
        expect(!ok2 || is_plain(s, n), "special byte accepted", n, pos);
        expect(ok2 || !is_plain(s, n), "plain refused at page end", n, pos);
        expect(!ok2 || o2 == "\"" + std::string(s, n) + "\"", "output at page end", n, pos);
      }
    }
  }

  // 2. Strings in the middle of the readable page with a wide tail load that
  //    stays inside the page, with bytes after the string set to '"' and '\\'.
  //    A correct mask ignores them; a wrong one refuses the string.
  for (size_t gap = 1; gap <= 40; ++gap) {
    const size_t end = page - gap;      // string ends `gap` bytes before the page end
    for (size_t n = 17; n <= 60 && n <= end; ++n) {
      char* s = p1 + end - n;
      fill_plain(s, n);
      for (size_t k = 0; k < gap; ++k)  // poison after the string, inside the page
        p1[end + k] = (k & 1) ? '"' : '\\';
      std::string out;
      bool ok = emit(s, n, out);
      expect(ok, "plain mid-page refused by poisoned tail", n, gap);
      expect(ok && out == "\"" + std::string(s, n) + "\"", "output mid-page", n, gap);
    }
  }

  // 3. A special byte in the tail is refused even when the wide load covers it.
  for (size_t n = 17; n <= 40; ++n) {
    char* s = p1 + 64;                  // mid-page, away from the boundary
    for (size_t pos = n - 15; pos < n; ++pos) {   // bytes of the last 15-byte tail
      fill_plain(s, n);
      s[pos] = '"';
      std::string out;
      expect(!emit(s, n, out), "tail quote not refused", n, pos);
    }
  }

  // 4. Short strings (1..16 bytes) inside the page, with poison after them. The
  //    single 16-byte check must mask the bytes past the string, and the copy
  //    must not depend on them.
  for (size_t gap = 17; gap <= 60; ++gap) {
    const size_t end = page - gap;
    for (size_t n = 1; n <= 16; ++n) {
      char* s = p1 + end - n;
      fill_plain(s, n);
      for (size_t k = 0; k < gap; ++k)
        p1[end + k] = (k & 1) ? '"' : (k & 2) ? '\\' : 0x01;
      std::string out;
      bool ok = emit(s, n, out);
      expect(ok, "short plain refused by poisoned bytes after it", n, gap);
      expect(ok && out == "\"" + std::string(s, n) + "\"", "short output mid-page", n, gap);
      // Same string with one special byte inside it must still be refused.
      for (size_t pos = 0; pos < n; ++pos) {
        fill_plain(s, n);
        s[pos] = '"';
        std::string o2;
        expect(!emit(s, n, o2), "short quote not refused", n, pos);
      }
    }
  }

  munmap(base, 3 * page);
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
