// fakeimport — a stand-in for adimport.exe behind the settings dialog's
// Import… button (AD_IMPORT_EXE). A console program, like the real one, so
// the smoke test can see whether the dialog gave it a console window.
//
//   FAKEIMPORT_LOG=<file>       append "run\tconsole=<0|1>\targs=<command line tail>"
//   FAKEIMPORT_CATALOG=<file>   copy this over %AD_ASSETS_DIR%\win\catalog-win.json
//                               (what a finished import does)
//   FAKEIMPORT_EXIT=<n>         exit code: adw::import::Status (0 ok … 5 cancelled)
//   FAKEIMPORT_WAIT_MS=<n>      take this long first (a test looks at the dialog meanwhile)
#include <windows.h>

#include <cstdlib>
#include <string>

namespace {

std::wstring env(const wchar_t* name) {
  wchar_t buf[4096];
  DWORD n = GetEnvironmentVariableW(name, buf, 4096);
  return n && n < 4096 ? std::wstring(buf, n) : std::wstring();
}

std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

// The command line after our own (quoted or bare) program name.
std::wstring args_tail() {
  const wchar_t* p = GetCommandLineW();
  if (*p == L'"') {
    ++p;
    while (*p && *p != L'"') ++p;
    if (*p) ++p;
  } else {
    while (*p && *p != L' ' && *p != L'\t') ++p;
  }
  while (*p == L' ' || *p == L'\t') ++p;
  return p;
}

} // namespace

int wmain() {
  // With CREATE_NO_WINDOW a console program has a console without a window;
  // without it, started from a GUI process, it gets a console window.
  bool console = GetConsoleWindow() != nullptr;
  std::wstring log = env(L"FAKEIMPORT_LOG");
  if (!log.empty()) {
    HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      std::string line = std::string("run\tconsole=") + (console ? "1" : "0") + "\targs=" + utf8(args_tail()) + "\r\n";
      DWORD put;
      WriteFile(h, line.data(), (DWORD)line.size(), &put, nullptr);
      CloseHandle(h);
    }
  }
  std::wstring catalog = env(L"FAKEIMPORT_CATALOG"), assets = env(L"AD_ASSETS_DIR");
  if (!catalog.empty() && !assets.empty()) {
    CopyFileW(catalog.c_str(), (assets + L"\\win\\catalog-win.json").c_str(), FALSE);
  }
  if (std::wstring wait = env(L"FAKEIMPORT_WAIT_MS"); !wait.empty()) Sleep((DWORD)_wtoi(wait.c_str()));
  std::wstring code = env(L"FAKEIMPORT_EXIT");
  return code.empty() ? 0 : _wtoi(code.c_str());
}
