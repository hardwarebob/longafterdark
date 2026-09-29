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

#include "catalog.h"

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
// §3.3): "lanes=pe32,ne16 configure=pe32,ne16 abis=afterdark,intermission
// status=1 state=1 seed=1 audio=1 numlock=1", only what that build has. The
// dialog asks once (at a new catalog again only while it hasn't answered),
// without running any module: which lanes and which module ABIs run at all
// (a module whose lane or ABI isn't listed is "Coming soon") and which lanes
// can open a module's own settings windows (its buttons, §6.3). The saver
// asks too; when its rotation holds a module of another ABI than After
// Dark's (rotation_needs_capabilities, releases.h), Random leaves out what
// the host can't run (saver.cc), and only a host that says numlock=1 hears
// Num Lock (below).
struct HostCapabilities {
  bool known = false;                 // the host answered (a host too old to know the switch doesn't)
  std::vector<std::string> lanes, configure;
  // The module ABIs it runs (catalog "abi"). A host that prints no "abis="
  // predates them and runs After Dark's alone: {"afterdark"}.
  std::vector<std::string> abis{kAfterDarkAbi};
  bool status = false, state = false, seed = false;
  // numlock=1: a Num Lock toggle beside Caps Lock (INTERACTION.md §3.2):
  // ADNUMLOCK=0|1 at start and NUMLOCK <0|1> input lines, numbered like CAPS.
  bool numlock = false;
  std::string line;                   // as printed, for the logs
  bool has_lane(const std::string& lane) const;
  bool has_abi(const std::string& abi) const;
  bool can_configure(const std::string& lane) const;
  // Whether it runs a module of `lane` and `abi` ("" = "afterdark"): both
  // listed. A host that didn't answer (!known) is taken to run everything: a
  // module whose lane it lacks exits 3 there (host.h: kExitLaneMissing),
  // which the dialog takes for that module alone (module_run's `exited_3`).
  bool runs(const std::string& lane, const std::string& abi) const;
  // Whether the saver may send it NUMLOCK lines: only once it has said
  // numlock=1. A host without the line ignores it ("unrecognized") without
  // numbering it, so every later input line would carry one number more in
  // the saver's count than in the host's, and the input rules' holds
  // (input_rules.h) would wait on numbers the host never reaches.
  bool takes_numlock_lines() const { return known && numlock; }
  // Whether a host started now gets ADNUMLOCK: unless it has answered without
  // numlock=1. Before the answer it goes too (the saver's first hosts start
  // before it unless the rotation waits for it: a module of another ABI,
  // App::caps_gate, 2 s at most): a host that doesn't know the variable
  // ignores it, while one that does must start with the real toggle, since
  // modules latch it when they start (Final Exam's exam begins on a change of
  // it): a later NUMLOCK line correcting a guessed one would read as a toggle.
  bool takes_numlock_env() const { return !known || numlock; }
};
HostCapabilities parse_capabilities(const std::string& text);
HostCapabilities probe_capabilities(const std::wstring& host_exe, DWORD timeout_ms = 5000);
// The Num Lock toggle a host is started with: {"ADNUMLOCK", "0"|"1"} when it
// takes one (takes_numlock_env), else {"ADNUMLOCK", ""}: removed, so nothing
// inherited reaches it (an empty value removes a variable, host_process.h).
std::pair<std::wstring, std::wstring> numlock_env(const HostCapabilities& caps, bool on);

// What the settings dialog can do with a module now (its preview, its
// thumbnail, its buttons, Preview):
//   waiting      the host is being asked (--capabilities): start nothing yet;
//   coming_soon  the host doesn't list its lane or its ABI, or one of its own
//                runs exited 3 (`exited_3`: the host's "valid module whose
//                lane is not built into this adhostwin", host.h, which is how
//                a host too old to answer says so): dimmed, never started,
//                its buttons read-only, "Coming soon". An exit 3 speaks for
//                that module alone, never for the others of its lane or ABI.
//                (A module ABI the host lacks fails with exit 1, as a damaged
//                module does: only its abis= answer tells the two apart);
//   runs         otherwise, a host that didn't answer included.
enum class ModuleRun { runs, waiting, coming_soon };
ModuleRun module_run(const Module& m, const HostCapabilities& caps, bool probing, bool exited_3);

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
