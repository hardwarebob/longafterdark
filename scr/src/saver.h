// The running screen saver: /s (one borderless topmost window per monitor)
// and /p (a child of the Display control panel's preview window).
#pragma once

#include "args.h"

namespace adw::scr {

// Exit codes. 0 is the normal "user came back" exit; the others only matter
// to the test hooks, which assert which path the saver took.
enum : int {
  kExitOk = 0,
  kExitBadArgs = 1,
  kExitNotImported = 10,     // catalog/assets missing: the message was shown
  kExitHostMissing = 11,     // adhostwin.exe missing: the message was shown
  kExitStartFailed = 12,     // test hook only: a module "could not be started"
};

int run_saver(const Args& args, void* hinstance);

} // namespace adw::scr
