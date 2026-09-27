// LongAfterDark.scr — the Long After Dark screen saver for Windows.
//
// A thin front-end: it never runs module code itself. /s and /p spawn
// adhostwin.exe (one per monitor) and present the P8 frames it streams back;
// /c edits settings.ini. See docs/DESIGN.md §1 and §6a.
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "args.h"
#include "config_dialog.h"
#include "log.h"
#include "saver.h"

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
  // No "program stopped working" or missing-disk boxes over a screen saver;
  // hosts inherit this error mode too.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

  int argc = 0;
  LPWSTR* argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> argv;
  for (int i = 1; argvw && i < argc; ++i) argv.emplace_back(argvw[i]);
  if (argvw) LocalFree(argvw);

  adw::scr::Args args = adw::scr::parse_args(argv);
  switch (args.mode) {
    case adw::scr::Mode::run:
    case adw::scr::Mode::preview:
      return adw::scr::run_saver(args, hinst);
    case adw::scr::Mode::password:
      return 0;   // Win9x password change; NT handles passwords itself
    case adw::scr::Mode::settings:
    default:
      return adw::scr::run_settings_dialog(args, hinst);
  }
}
