// SHELL32.DLL (API_SURFACE.md §1 "SHELL32.DLL": Bad Dog's two icon functions,
// plus ShellExecuteA, which After Dark 10th Anniversary's HALLOFFA.AD imports
// for its AD Online "LaunchURL").
//
// ShellExecuteA is refused, always: a screen saver module must never start a
// browser or any other program on the user's machine. It answers
// SE_ERR_ACCESSDENIED (5; values <= 32 are errors) and logs what was asked.
//
// Known gap, deliberately left: ExtractIconA/SHGetFileInfoA answer "none".
// Bad Dog imports them for chewing on desktop icons, but no module of the 202
// in the five releases calls them (see user32.cc's known gaps on EnumWindows
// and icons).
#include "adw/core/log.h"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"

namespace adw::win32 {

void register_shell32(ShimRegistry& r) {
  r.impl("SHELL32.DLL", "ExtractIconA", [](Call& c) { c.ret(0); });
  r.impl("SHELL32.DLL", "SHGetFileInfoA", [](Call& c) { c.ret(0); });
  r.impl("SHELL32.DLL", "ShellExecuteA", [](Call& c) {
    // (hwnd, verb, file, parameters, directory, show)
    log("ShellExecute(\"%s\", \"%s\") refused: modules may not start programs", c.str(1).c_str(), c.str(2).c_str());
    c.ret(5);  // SE_ERR_ACCESSDENIED
  });
}

}  // namespace adw::win32
