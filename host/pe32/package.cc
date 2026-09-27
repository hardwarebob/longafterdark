#include "pe32/package.hh"

#include <cctype>
#include <cstdint>

namespace adw::pe32 {

namespace {

// The part before the last separator, or "" when there is none.
std::string parent_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  if (s == std::string::npos) return {};
  std::string d = p.substr(0, s);
  // "C:" alone is a drive, not a folder a package could live in.
  if (d.empty() || (d.size() == 2 && d[1] == ':')) return {};
  return d;
}

std::string name_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

// Case-insensitive, and '/' equals '\' (paths only ever differ in those ways here).
bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i] == '/' ? '\\' : a[i], y = b[i] == '/' ? '\\' : b[i];
    if (tolower(uint8_t(x)) != tolower(uint8_t(y))) return false;
  }
  return true;
}

}  // namespace

PackagePaths locate_package(const std::string& module_path) {
  PackagePaths p;
  p.id = "legacy";
  size_t s = module_path.find_last_of("\\/");
  p.module_dir = s == std::string::npos ? "." : module_path.substr(0, s);
  p.root = parent_of(module_path.substr(0, s == std::string::npos ? 0 : s));
  if (p.root.empty()) return p;
  p.engine_dir = p.root + "\\ENGINE";
  std::string above = parent_of(p.root);
  if (!above.empty() && iequals(name_of(above), "packages")) {
    p.packaged = true;
    p.id = name_of(p.root);
  }
  return p;
}

std::vector<std::string> dll_search_dirs(const PackagePaths& p, const std::string& win_assets_dir) {
  if (p.packaged) {
    std::vector<std::string> dirs{p.module_dir};
    // A module that lives in ENGINE itself (ad10's STARRYNI.AD) searches it once.
    if (!iequals(p.engine_dir, p.module_dir)) dirs.push_back(p.engine_dir);
    return dirs;
  }
  // ADXPL510.DLL lives in FILES\AD40 (STARRYNI.AD sits in FILES\ENGINE and
  // needs no engine, but a module elsewhere may).
  return {p.module_dir, win_assets_dir + "\\FILES\\AD40"};
}

}  // namespace adw::pe32
