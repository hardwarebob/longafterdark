// COMDLG32.DLL (API_SURFACE.md §1 "COMDLG32.DLL", 1 function).
//
// GetOpenFileNameA is reached only from configuration buttons (CRITIC,
// POINTS, SLOWBURN, SWIRLING), which the saver never presses (ABI.md
// §2.10.4): there the user "cancels". In configure mode (adhostwin
// --configure) user32_real.cc turns it (and GetSaveFileNameA) into the real
// common dialog with guest <-> host paths (realui.hh, INTERACTION.md §6.2).
//
// Known gap, deliberately left: OFN_ENABLETEMPLATE (a module's own
// file-dialog template) is ignored with a log line; no module of the 202 in
// the five releases uses one.
#include "win32/runtime.hh"
#include "win32/shim_families.hh"

namespace adw::win32 {

void register_comdlg32(ShimRegistry& r) {
  r.impl("COMDLG32.DLL", "GetOpenFileNameA", [](Call& c) { c.ret(0); });
}

}  // namespace adw::win32
