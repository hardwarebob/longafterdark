// The dump logic behind adwinspect, as a library so the corpus test can run it
// over every file too.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace adw::loader::inspect {

// Win16 ordinal → name tables from Wine .spec files (research/win/spec). Used
// only to label ordinals in dumps.
class SpecDb {
public:
  // Loads every *.spec in dir; returns how many files were read.
  size_t load_dir(const std::string& dir);
  // nullptr when unknown.
  const std::string* lookup(std::string_view module, uint16_t ordinal) const;
  bool empty() const { return modules_.empty(); }

private:
  std::map<std::string, std::map<uint16_t, std::string>> modules_; // upper-case module
};

struct Options {
  bool verbose = false;        // every relocation record / every string
  bool summary = false;        // one line per file
  const SpecDb* specs = nullptr;
  // Label imports from DLLs that sit next to the inspected file (ADXPL300,
  // AD_RSRC, ...) with the names those DLLs export.
  bool resolve_siblings = true;
};

// Dumps one file; returns false (after printing the error) if it could not be
// parsed as PE or NE.
bool inspect_file(std::ostream& out, const std::string& path, const Options& opt);

// True for the extensions the corpus uses for executables.
bool is_executable_name(std::string_view filename);

} // namespace adw::loader::inspect
