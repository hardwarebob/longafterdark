// /c: the settings dialog (IDD_SETTINGS in res/afterdark.rc). Reads
// catalog-win.json, edits and saves settings.ini (DESIGN.md §6a).
#pragma once

#include "args.h"

namespace adw::scr {

int run_settings_dialog(const Args& args, void* hinstance);

} // namespace adw::scr
