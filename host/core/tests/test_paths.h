// Where the host test suites find the installed assets, and how they keep the
// hosts they spawn away from the user's real data folder.
//
// A spawned adhostwin that is left to its defaults resolves the assets under
// %LOCALAPPDATA%\LongAfterDark, and --configure keeps its state there
// (data_root.h). A test must keep its hosts out of the real folder, so a
// suite that spawns hosts finds the installed assets read-only first and then
// calls sandbox_spawned_hosts(): every host it starts after that gets
// explicit assets and a scratch AD_LOCALAPPDATA. Header-only (data_root.h is
// too), so suites that do not link adw_core can use it.
#pragma once

#include <windows.h>

#include <cstdlib>
#include <string>

#include "adw/core/data_root.h"

namespace adw_test {

inline std::wstring from_utf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

inline std::string to_utf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// <data folder>\assets, where the data folder is %LOCALAPPDATA%\LongAfterDark
// (adw::data_root_path: nothing is looked at or created). AD_LOCALAPPDATA
// stands in for LOCALAPPDATA as usual. "" without either variable.
// AD_ASSETS_DIR is not consulted: callers that honour it check it first.
inline std::string installed_assets_root() {
  std::wstring d = adw::data_root_path(adw::data_root_base());
  if (d.empty()) return {};
  return to_utf8(d) + "\\assets";
}

// From now on every process this one starts reads its assets from
// `assets_root` (AD_ASSETS_DIR; skipped when "") and resolves any other
// default location under %TEMP%\adw_<tag>_lad_<pid> (AD_LOCALAPPDATA, not
// created here). Both go through the CRT, so getenv() agrees. Returns the
// scratch base.
inline std::string sandbox_spawned_hosts(const std::string& assets_root, const char* tag) {
  if (!assets_root.empty()) _wputenv_s(L"AD_ASSETS_DIR", from_utf8(assets_root).c_str());
  wchar_t tmp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, tmp);
  std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(tmp, n) : std::wstring(L".\\");
  base += L"adw_" + from_utf8(tag) + L"_lad_" + std::to_wstring(GetCurrentProcessId());
  _wputenv_s(adw::kDataRootBaseVar, base.c_str());
  return to_utf8(base);
}

}  // namespace adw_test
