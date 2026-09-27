// Outcome codes shared by every importer entry point. The numeric values are
// adimport.exe's exit codes (the .scr's settings dialog reads them), so they
// are part of the contract and must not be renumbered.
#pragma once

#include <stdexcept>
#include <string>

namespace adw::import {

enum class Status : int {
  ok = 0,
  error = 1,           // usage, local I/O, anything not covered below
  source_invalid = 2,  // not an ISO / no ADE\FILES / required engine files absent
  verify_failed = 3,   // md5 of the image or of a known file does not match
  network = 4,         // download failed (DNS, TLS, HTTP status, dropped transfer)
  cancelled = 5,       // the user pressed Cancel (GUI) or a progress callback said stop
};

const char* status_name(Status s);

// Thrown inside the importer and converted to a Status at the API boundary.
class ImportError : public std::runtime_error {
 public:
  ImportError(Status status, const std::string& what) : std::runtime_error(what), status_(status) {}
  Status status() const { return status_; }

 private:
  Status status_;
};

}  // namespace adw::import
