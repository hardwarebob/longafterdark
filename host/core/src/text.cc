#include "adw/core/text.h"

#include <windows.h>

namespace adw {

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
  return out;
}

std::string narrow(std::wstring_view wide) {
  if (wide.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace adw
