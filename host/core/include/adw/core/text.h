// UTF-8 <-> UTF-16 at the Win32 boundary. Everything inside the host carries
// UTF-8 std::string (paths, env values, log text); the W APIs get widened copies.
#pragma once

#include <string>
#include <string_view>

namespace adw {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

}  // namespace adw
