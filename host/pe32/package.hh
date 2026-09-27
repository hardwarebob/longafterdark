// Which package a module belongs to, and where its DLLs may come from
// (PACKAGES.md §7.1/§7.2). The layout is the contract, so this is pure path
// arithmetic — no environment variable, no descriptor file, no disk access:
//
//   module dir   = the folder of the module file
//   package root = the parent of the module dir
//   engine dir   = <package root>\ENGINE
//   packaged     = the package root's parent is named "packages" (any case)
//   package id   = the package root's name (logs only; nothing branches on it)
//
// DLL search, after the importing image's own folder:
//   packaged  module dir → engine dir. Nothing outside the package root, so a
//             package never runs another package's (or Deluxe's) engine.
//   legacy    module dir → <win>\FILES\AD40: Deluxe's FILES tree, a research
//             root laid out as FILES\<X>\, or a lone module anywhere. This is
//             the order the lane has always used, so Deluxe runs are unchanged.
#pragma once

#include <string>
#include <vector>

namespace adw::pe32 {

struct PackagePaths {
  std::string module_dir;
  std::string root;        // "" when the module dir has no parent
  std::string engine_dir;  // "" when there is no root
  bool packaged = false;
  std::string id;          // the package root's name when packaged, else "legacy"
};

// `module_path`: the module's absolute host path ('\' or '/' separators).
PackagePaths locate_package(const std::string& module_path);

// The lane's search directories (ModuleTable::add_search_dir order).
// `win_assets_dir`: Env::win_assets_dir(), used for legacy modules only.
std::vector<std::string> dll_search_dirs(const PackagePaths& p, const std::string& win_assets_dir);

}  // namespace adw::pe32
