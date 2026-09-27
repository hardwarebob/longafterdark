#include "paths.h"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <vector>

#include "adw/core/data_root.h"

namespace adw::scr {

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
  return out;
}

std::string narrow(std::wstring_view s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring env_w(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  n = GetEnvironmentVariableW(name, v.data(), n);
  v.resize(n);
  return v;
}

bool env_set(const wchar_t* name) {
  return GetEnvironmentVariableW(name, nullptr, 0) > 0;
}

long long env_int(const wchar_t* name, long long fallback) {
  std::wstring v = env_w(name);
  if (v.empty()) return fallback;
  wchar_t* end = nullptr;
  long long r = wcstoll(v.c_str(), &end, 10);
  return (end && end != v.c_str()) ? r : fallback;
}

std::wstring exe_path() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    if (n < buf.size()) { buf.resize(n); return buf; }
    buf.resize(buf.size() * 2);   // long-path install locations
  }
}

std::wstring dir_of(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? std::wstring(L".") : path.substr(0, p);
}

std::wstring join_path(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L"\\" + b;
}

namespace {

// The folder the data folder lives in: data_root_base() (AD_LOCALAPPDATA,
// else LOCALAPPDATA), the rule adhostwin and adimport follow too. The saver
// can run on the secure screen-saver desktop with a thin environment; the
// shell folder API still knows the profile.
std::wstring data_root_base_or_known_folder() {
  std::wstring base = adw::data_root_base();
  if (base.empty()) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
      base = p;
      CoTaskMemFree(p);
    }
  }
  return base;
}

}  // namespace

std::wstring app_data_root() {
  const std::wstring d = adw::data_root_path(data_root_base_or_known_folder());
  // No base at all (no profile): a folder of that name where we run.
  return d.empty() ? std::wstring(adw::kDataDirName) : d;
}

std::wstring assets_root() {
  std::wstring e = env_w(L"AD_ASSETS_DIR");
  return e.empty() ? join_path(app_data_root(), L"assets") : e;
}

std::wstring win_assets_dir() {
  std::wstring root = assets_root();
  std::wstring win = join_path(root, L"win");
  if (!file_exists(join_path(win, L"catalog-win.json")) &&
      file_exists(join_path(root, L"catalog-win.json"))) {
    return root;
  }
  return win;
}

std::wstring catalog_path() { return join_path(win_assets_dir(), L"catalog-win.json"); }

std::wstring settings_path() {
  std::wstring e = env_w(L"AD_SETTINGS");
  return e.empty() ? join_path(app_data_root(), L"settings.ini") : e;
}

std::wstring thumbs_dir() {
  std::wstring e = env_w(L"AD_SCR_THUMBS");
  if (!e.empty()) return e;
  // Next to the settings file: a test's or a scratch AD_SETTINGS keeps its
  // thumbnails to itself.
  return join_path(dir_of(settings_path()), L"thumbs");
}

std::wstring state_dir() {
  std::wstring e = env_w(L"AD_SCR_STATE");
  if (!e.empty()) return e;
  // Next to the settings file, like the thumbnails: a test that points
  // AD_SETTINGS at a scratch folder gets scratch module state with it.
  return join_path(dir_of(settings_path()), L"state");
}

std::wstring last_exit_log_path() {
  std::wstring e = env_w(L"AD_SCR_LASTLOG");
  if (!e.empty()) return e;
  return join_path(join_path(dir_of(settings_path()), L"logs"), L"saver-last.log");
}

std::wstring seed_file_path(unsigned long pid, int window_index) {
  wchar_t buf[MAX_PATH + 2] = {};
  DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  std::wstring dir(buf, n > 0 && n <= MAX_PATH + 1 ? n : 0);
  return join_path(dir, std::wstring(kSeedFilePrefix) + std::to_wstring(pid) + L"-" + std::to_wstring(window_index) +
                            L".ppm");
}

std::wstring host_exe_path() {
  std::wstring e = env_w(L"AD_HOST_EXE");
  return e.empty() ? join_path(dir_of(exe_path()), L"adhostwin.exe") : e;
}

std::wstring import_exe_path() {
  std::wstring e = env_w(L"AD_IMPORT_EXE");
  return e.empty() ? join_path(dir_of(exe_path()), L"adimport.exe") : e;
}

std::wstring resolve_module_path(const std::wstring& win_dir, const std::string& rel) {
  std::wstring r = widen(rel);
  for (auto& c : r) if (c == L'/') c = L'\\';
  // Already absolute (drive or UNC): a hand-written catalog may do this.
  if ((r.size() > 2 && r[1] == L':') || (r.size() > 1 && r[0] == L'\\' && r[1] == L'\\')) return r;
  while (!r.empty() && r.front() == L'\\') r.erase(r.begin());
  return join_path(win_dir, r);
}

bool file_exists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (64ll << 20)) { CloseHandle(h); return false; }
  out.resize((size_t)sz.QuadPart);
  DWORD got = 0;
  bool ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
  CloseHandle(h);
  return ok;
}

bool ensure_dir(const std::wstring& path) {
  if (path.empty() || dir_exists(path)) return true;
  std::wstring parent = dir_of(path);
  if (parent != path && parent != L"." && !parent.empty() && parent.back() != L':') ensure_dir(parent);
  return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool write_file_atomic(const std::wstring& path, const std::string& data) {
  ensure_dir(dir_of(path));
  std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size());
  ok = FlushFileBuffers(h) && ok;
  CloseHandle(h);
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

std::wstring quote_arg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') { ++i; ++backslashes; }
    if (i == arg.size()) { out.append(backslashes * 2, L'\\'); break; }
    if (arg[i] == L'"') { out.append(backslashes * 2 + 1, L'\\'); out.push_back(L'"'); }
    else { out.append(backslashes, L'\\'); out.push_back(arg[i]); }
  }
  out.push_back(L'"');
  return out;
}

} // namespace adw::scr
