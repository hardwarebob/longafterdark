// Screen saver command line, per the Windows convention:
//   /s          run full screen            /p <HWND>  live preview in that window
//   /c[:HWND]   settings (owned by HWND)   (none)     settings
//   /a <HWND>   change password (Win9x; ignored on NT)
// Case-insensitive; '-' works like '/'; the HWND may follow a ':' / '=' or be
// the next argument ("/c:1234", "/p 1234", "/P1234").
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adw::scr {

enum class Mode { settings, run, preview, password };

struct Args {
  Mode mode = Mode::settings;
  uintptr_t hwnd = 0;         // parent (/p) or owner (/c); 0 when absent
  bool has_hwnd = false;
  bool valid = true;          // false: /p without a window to draw into
};

// `argv` excludes the program name.
Args parse_args(const std::vector<std::wstring>& argv);

} // namespace adw::scr
