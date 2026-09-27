// Window-free pieces of the settings dialog, kept here so the tests can pin
// them: starting its helper programs (adimport, the Preview's own "/s"),
// reading adimport's exit code, and the lifetime of the throwaway settings
// file a Preview runs from.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adw::scr {

// ---- helper programs ------------------------------------------------------------

struct ChildLaunch {
  std::wstring exe, args;       // args already quoted as one command-line tail
  std::wstring env_block;       // build_environment_block(); empty = inherit ours
  // adimport is a console program: started from this GUI process it would
  // get a console window of its own (a black box behind its progress dialog
  // on Windows before 11 24H2, whose manifest policy covers only newer
  // systems). CREATE_NO_WINDOW gives it a windowless console instead.
  bool console_program = false;
};

DWORD child_creation_flags(const ChildLaunch& c);
// Starts the child and returns its process handle (the caller owns it), or
// nullptr with `*error` set.
HANDLE start_child(const ChildLaunch& c, DWORD* error = nullptr);

// adimport's exit code is adw::import::Status (importer/status.h):
// 0 ok, 1 error, 2 source invalid, 3 verify failed, 4 network, 5 cancelled.
// Every import is atomic, so anything but 0 means nothing was changed.
enum class ImportOutcome { imported, cancelled, failed };
ImportOutcome classify_import_exit(DWORD code);
// What the dialog's assets line says after an import that did not happen
// ("" after a successful one: the refreshed module count says it all).
std::wstring import_outcome_note(DWORD code);

// ---- what the host can do ----------------------------------------------------------
// `adhostwin --capabilities` prints one line and exits 0 (INTERACTION.md
// §3.3): "lanes=pe32,ne16 configure=pe32,ne16 status=1 state=1 seed=1", only
// what that build has. The dialog asks once, without running any module:
// which lanes run at all (a Classic module on a host without ne16 is
// "Coming soon") and which can open a module's own settings windows (its
// buttons, §6.3).
struct HostCapabilities {
  bool known = false;                 // the host answered (a host too old to know the switch doesn't)
  std::vector<std::string> lanes, configure;
  bool status = false, state = false, seed = false;
  std::string line;                   // as printed, for the logs
  bool has_lane(const std::string& lane) const;
  bool can_configure(const std::string& lane) const;
};
HostCapabilities parse_capabilities(const std::string& text);
HostCapabilities probe_capabilities(const std::wstring& host_exe, DWORD timeout_ms = 5000);

// A host started for a one-shot answer (--capabilities, --configure): no
// console window (CREATE_NO_WINDOW), in a kill-on-close Job created with it,
// stdin NUL, stderr to AD_SCR_HOSTLOG (else NUL), stdout a pipe, and the
// environment every host gets (ADSTATE, host_process.h) plus `env`. Never
// sound: ADSOUND=0, ADAUDIOOUT and ADVOLUME removed (sound.h).
struct HostTool {
  HANDLE process = nullptr, job = nullptr, stdout_r = nullptr;
  DWORD pid = 0;
  HostTool() = default;
  HostTool(const HostTool&) = delete;
  HostTool& operator=(const HostTool&) = delete;
  ~HostTool();   // kills it if it still runs
  bool start(const std::wstring& exe, const std::wstring& args_tail,
             const std::vector<std::pair<std::wstring, std::wstring>>& env, std::wstring* error);
  // Collects stdout until the process exits (or the timeout kills it: false).
  bool finish(DWORD timeout_ms, std::string* out, DWORD* code);
};

// "--configure <path> --button <slot> --owner <hwnd>" (INTERACTION.md §6.1).
std::wstring configure_args(const std::wstring& module_path, int slot, uintptr_t owner);
// The note under a module button's row after a run: "" for 0, "Nothing to
// set here" for 4, "Couldn't open this option (code N)" otherwise (N in hex
// for a crash's NTSTATUS).
std::wstring configure_outcome_note(DWORD code);

// ---- the Preview's settings file ---------------------------------------------------
// Preview runs "/s" on exactly what the dialog shows, unsaved edits included,
// through AD_SETTINGS=<temp dir>\LongAfterDark-preview-<dialog pid>.ini.
// The saver deletes that file as soon as it has read it (it is told so by
// kPreviewSettingsEnv, and only ever deletes a file named like this), so it
// outlives neither the Preview nor a dialog closed while one runs; the dialog
// deletes it too when a Preview fails to start or ends, and sweeps files left
// by dialogs that died.

inline constexpr wchar_t kPreviewSettingsEnv[] = L"AD_SCR_SETTINGS_IS_TEMP";

std::wstring preview_settings_path(const std::wstring& temp_dir, DWORD dialog_pid);
// The pid in "LongAfterDark-preview-<pid>.ini" (case-insensitive); false
// for any other name.
bool parse_preview_settings_name(std::wstring_view file_name, DWORD* pid = nullptr);
// Deletes preview settings files in `temp_dir` whose dialog is no longer
// running (never this process's). Returns how many were deleted.
int sweep_stale_preview_settings(const std::wstring& temp_dir);
// The temp directory as GetTempPathW reports it, without the trailing slash.
std::wstring temp_dir();

} // namespace adw::scr
