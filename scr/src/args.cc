#include "args.h"

#include <cwctype>

namespace adw::scr {

namespace {

// HWNDs arrive as decimal. They are 32-bit values sign-extended on x64, so
// a launcher that printed one as a signed int ("-1234") must still work;
// "0x" hex is accepted as a courtesy for hand-typed tests.
bool parse_hwnd(const std::wstring& s, uintptr_t& out) {
  size_t i = 0;
  while (i < s.size() && iswspace(s[i])) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == L'-' || s[i] == L'+')) { neg = s[i] == L'-'; ++i; }
  int base = 10;
  if (i + 1 < s.size() && s[i] == L'0' && (s[i + 1] == L'x' || s[i + 1] == L'X')) { base = 16; i += 2; }
  size_t start = i;
  unsigned long long v = 0;
  for (; i < s.size(); ++i) {
    wchar_t c = s[i];
    int d;
    if (c >= L'0' && c <= L'9') d = c - L'0';
    else if (base == 16 && c >= L'a' && c <= L'f') d = c - L'a' + 10;
    else if (base == 16 && c >= L'A' && c <= L'F') d = c - L'A' + 10;
    else break;
    v = v * base + d;
  }
  if (i == start) return false;
  while (i < s.size() && iswspace(s[i])) ++i;
  if (i != s.size()) return false;
  long long sv = neg ? -(long long)v : (long long)v;
  out = (uintptr_t)sv;
  return true;
}

} // namespace

Args parse_args(const std::vector<std::wstring>& argv) {
  Args a;
  for (size_t k = 0; k < argv.size(); ++k) {
    const std::wstring& t = argv[k];
    if (t.size() < 2 || (t[0] != L'/' && t[0] != L'-')) continue;
    wchar_t c = (wchar_t)towlower(t[1]);
    Mode m;
    if (c == L's') m = Mode::run;
    else if (c == L'p' || c == L'l') m = Mode::preview;   // "/l" is the Win3.x-era spelling
    else if (c == L'c') m = Mode::settings;
    else if (c == L'a') m = Mode::password;
    else continue;                                          // unknown switch: keep looking
    a.mode = m;

    // HWND: glued ("/p1234"), after ':'/'=' ("/c:1234"), or the next token.
    std::wstring rest = t.substr(2);
    if (!rest.empty() && (rest[0] == L':' || rest[0] == L'=')) rest.erase(0, 1);
    uintptr_t h = 0;
    if (!rest.empty()) {
      if (parse_hwnd(rest, h)) { a.hwnd = h; a.has_hwnd = true; }
    } else if (k + 1 < argv.size() && parse_hwnd(argv[k + 1], h)) {
      a.hwnd = h;
      a.has_hwnd = true;
    }
    break;
  }
  if (a.mode == Mode::preview && (!a.has_hwnd || a.hwnd == 0)) a.valid = false;
  return a;
}

} // namespace adw::scr
