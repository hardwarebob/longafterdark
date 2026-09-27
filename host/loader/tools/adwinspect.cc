// adwinspect — dump a PE32 or NE image: headers, sections/segments, imports
// (Win16 ordinals named from Wine .spec files with --spec-dir), exports,
// relocations, resources (VERSIONINFO and RT_STRING decoded), OSFIXUP counts.
//
//   adwinspect [--spec-dir DIR] [-v] [--summary] [--no-siblings] FILE|DIR...
//
// A directory argument inspects every *.AD/*.DLL/*.EXE/*.SCR/*.DRV below it.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "loader/tools/inspect.hh"

namespace fs = std::filesystem;
using namespace adw::loader::inspect;

static int usage() {
  std::cerr << "usage: adwinspect [--spec-dir DIR] [-v] [--summary] [--no-siblings] FILE|DIR...\n";
  return 2;
}

int main(int argc, char** argv) {
  Options opt;
  SpecDb specs;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--spec-dir" && i + 1 < argc) {
      if (!specs.load_dir(argv[++i])) std::cerr << "adwinspect: no .spec files in " << argv[i] << "\n";
      opt.specs = &specs;
    } else if (a == "-v" || a == "--verbose") {
      opt.verbose = true;
    } else if (a == "--summary") {
      opt.summary = true;
    } else if (a == "--no-siblings") {
      opt.resolve_siblings = false;
    } else if (a == "-h" || a == "--help" || (a.size() > 1 && a[0] == '-')) {
      return usage();
    } else {
      paths.push_back(a);
    }
  }
  if (paths.empty()) return usage();

  std::vector<std::string> files;
  for (const auto& p : paths) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> found;
      for (const auto& e : fs::recursive_directory_iterator(p, ec)) {
        if (e.is_regular_file() && is_executable_name(e.path().filename().string())) {
          found.push_back(e.path().string());
        }
      }
      std::sort(found.begin(), found.end());
      files.insert(files.end(), found.begin(), found.end());
    } else {
      files.push_back(p);
    }
  }

  int failures = 0;
  for (const auto& f : files) {
    if (!inspect_file(std::cout, f, opt)) failures++;
    if (!opt.summary) std::cout << "\n";
  }
  return failures ? 1 : 0;
}
