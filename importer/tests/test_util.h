// Tiny test harness shared by the importer tests: CHECK macros that count
// failures instead of aborting (one run reports every broken expectation),
// deterministic data, and file helpers.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#include "adw/core/data_root.h"
#include "winutil.h"

namespace test {

// ---- the data folder -------------------------------------------------------------
// adimport's defaults live in the data folder, %LOCALAPPDATA%\LongAfterDark
// (importer.h data_folder()). No test may write to the user's real folder:
// every suite calls sandbox_data_root() first thing, so the defaults of this
// process and of every adimport it starts resolve under a scratch base. A
// suite that reads the user's installed data finds it with
// installed_data_root() before that, read only.

// The data folder that holds the user's data: %LOCALAPPDATA%\LongAfterDark
// (AD_LOCALAPPDATA stands in for LOCALAPPDATA as usual); "" without either.
// Nothing is looked at or created.
inline std::filesystem::path installed_data_root() {
  return adw::data_root_path(adw::data_root_base());
}

// <installed data root>\assets, or AD_ASSETS_DIR when that is set (as adimport
// would take it); "" when neither is known.
inline std::filesystem::path installed_assets_root() {
  if (const wchar_t* e = _wgetenv(L"AD_ASSETS_DIR")) {
    std::wstring v = adw::data_root_detail::trim(e);
    if (!v.empty()) return v;
  }
  const std::filesystem::path d = installed_data_root();
  return d.empty() ? d : d / L"assets";
}

// An environment variable for this process (the CRT's copy, which _wgetenv
// reads, and the process's, which data_root.h and child processes read);
// "" removes it.
inline void set_env(const wchar_t* name, const std::wstring& value) {
  _wputenv_s(name, value.c_str());
  SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
}

inline std::wstring get_env(const wchar_t* name) {
  const wchar_t* v = _wgetenv(name);
  return v ? v : L"";
}

// From now on the data folder of this process and of the processes it starts
// is under `base` (AD_LOCALAPPDATA; `base` is not created). Returns `base`.
inline std::filesystem::path sandbox_data_root(const std::filesystem::path& base) {
  set_env(adw::kDataRootBaseVar, base.wstring());
  return base;
}

inline int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ::test::g_failures++;                                                      \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto _va = (a);                                                                        \
    auto _vb = (b);                                                                        \
    if (!(_va == _vb)) {                                                                   \
      ::test::g_failures++;                                                                \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  left:  %s\n  right: %s\n",      \
              __FILE__, __LINE__, #a, #b, ::test::show(_va).c_str(), ::test::show(_vb).c_str()); \
    }                                                                                      \
  } while (0)

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return s ? show(std::string(s)) : "(null)"; }
inline std::string show(const std::filesystem::path& p) { return show(adw::import::to_utf8(p.wstring())); }
template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(int(v));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(v);
  } else {
    return "(value)";
  }
}

inline int finish(const char* name) {
  if (g_failures) {
    fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
    return 1;
  }
  fprintf(stderr, "%s: ok\n", name);
  return 0;
}

// xorshift-filled buffer: reproducible, incompressible-looking test content.
inline std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    v[i] = uint8_t(x >> 24);
  }
  return v;
}

inline void write_bytes(const std::filesystem::path& p, const std::vector<uint8_t>& data) {
  std::filesystem::create_directories(p.parent_path());
  FILE* f = _wfopen(p.c_str(), L"wb");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", adw::import::to_utf8(p.wstring()).c_str());
    exit(2);
  }
  if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
  fclose(f);
}

inline std::vector<uint8_t> read_bytes(const std::filesystem::path& p) {
  std::vector<uint8_t> out;
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return out;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return out;
}

inline std::string read_text(const std::filesystem::path& p) {
  auto b = read_bytes(p);
  return std::string(b.begin(), b.end());
}

// A fresh, empty scratch directory (argv[1] of the test, or %TEMP%).
inline std::filesystem::path scratch(int argc, char** argv, const char* name) {
  std::filesystem::path p = argc > 1 ? std::filesystem::path(argv[1])
                                     : std::filesystem::temp_directory_path() / name;
  adw::import::remove_tree(p);  // not std::filesystem::remove_all: see winutil.h
  std::filesystem::create_directories(p);
  return p;
}

// Every regular file under `root`, as '/'-separated relative paths.
inline std::vector<std::string> list_tree(const std::filesystem::path& root) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (!it->is_regular_file()) continue;
    std::string rel = adw::import::to_utf8(std::filesystem::relative(it->path(), root).wstring());
    for (char& c : rel)
      if (c == '\\') c = '/';
    out.push_back(rel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace test
