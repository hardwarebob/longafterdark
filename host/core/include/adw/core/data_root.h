// The per-user data folder of Long After Dark:
//
//   %LOCALAPPDATA%\LongAfterDark     assets\, downloads\, state\, settings.ini, ...
//
// Every program that derives a location from the data folder (adhostwin's
// Env, the .scr's paths, adimport's defaults) takes it from here, so all
// three agree on the one folder. Nothing here looks at the disk or creates
// anything.
//
// This header is self-contained (inline, kernel32 only), so the .scr and
// adimport, which do not link adw_core, call this same code. Paths are
// UTF-16. See host/core/README.md, "The data folder".
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace adw {

inline constexpr wchar_t kDataDirName[] = L"LongAfterDark";
// Stands in for %LOCALAPPDATA% when it is set and not blank, so tests (and
// anyone trying a build) can use a scratch base without touching the real one.
inline constexpr wchar_t kDataRootBaseVar[] = L"AD_LOCALAPPDATA";

namespace data_root_detail {

inline std::wstring trim(std::wstring_view s) {
  while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n'))
    s.remove_suffix(1);
  return std::wstring(s);
}

inline std::wstring env(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  n = GetEnvironmentVariableW(name, v.data(), n);
  v.resize(n);
  return v;
}

}  // namespace data_root_detail

// The base folder for the data folder: AD_LOCALAPPDATA when it is set and not
// blank, else LOCALAPPDATA, each trimmed; "" when neither is set. A caller
// that runs with a thin environment (the .scr on the secure desktop) falls
// back to SHGetKnownFolderPath(FOLDERID_LocalAppData) itself when this is "".
inline std::wstring data_root_base() {
  std::wstring v = data_root_detail::trim(data_root_detail::env(kDataRootBaseVar));
  return v.empty() ? data_root_detail::trim(data_root_detail::env(L"LOCALAPPDATA")) : v;
}

// <base>\LongAfterDark, with base trimmed and a trailing separator
// tolerated; "" when base is blank.
inline std::wstring data_root_path(const std::wstring& base) {
  std::wstring b = data_root_detail::trim(base);
  if (b.empty()) return {};
  return (b.back() == L'\\' || b.back() == L'/') ? b + kDataDirName : b + L"\\" + kDataDirName;
}

}  // namespace adw
