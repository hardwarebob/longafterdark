// Name rules shared by every importer source (internal header): ASCII case
// mapping and comparison, and the check every name must pass before it
// becomes a path component under a staging directory.
//
// Every source is untrusted input. A Joliet name such as "..\..\x.dll", a
// ZIP member "C:EVIL.AD", a FAT entry "CON" — none of them may write outside
// the stage, collide with another entry, or name something Windows cannot
// represent faithfully (CreateFileW on <stage>\AD40\CON opens the console).
#pragma once

#include <string>
#include <string_view>

#include "status.h"

namespace adw::import {

inline std::string ascii_upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return s;
}

inline std::string ascii_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

inline bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = char(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = char(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}

inline bool ends_with_i(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// DOS device names ("CON", "NUL.AD", "COM1", …), with or without extension.
inline bool is_device_name(const std::string& name) {
  std::string base = ascii_upper(name.substr(0, name.find('.')));
  while (!base.empty() && base.back() == ' ') base.pop_back();
  for (const char* d : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"})
    if (base == d) return true;
  return base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && base[3] >= '0' &&
         base[3] <= '9';
}

// Throws ImportError(source_invalid) for a name that must not become a path
// component: empty, "." / "..", a trailing dot or space (Windows drops them
// silently), a control or reserved character, or a DOS device.
inline void check_component(const std::string& name, const std::string& parent) {
  bool ok = !name.empty() && name != "." && name != ".." && name.back() != '.' && name.back() != ' ' &&
            !is_device_name(name);
  for (unsigned char c : name)
    if (c < 0x20 || std::string_view("<>:\"/\\|?*").find(char(c)) != std::string_view::npos) ok = false;
  if (!ok) throw ImportError(Status::source_invalid, "unusable file name \"" + name + "\" in " + parent);
}

}  // namespace adw::import
