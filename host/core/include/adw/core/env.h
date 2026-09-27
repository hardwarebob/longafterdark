// Host environment (DESIGN.md §1), parsed once at startup. Every AD* variable
// is snapshotted into `vars` so lanes can read lane-specific knobs through
// get() without calling getenv mid-run; the typed fields below are the
// protocol-level ones every lane shares.
//
// `adhostwin.exe <module> KEY=VALUE …` overrides land in the same snapshot, so
// a command line and an environment are interchangeable.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adw {

struct Env {
  // ADSTREAM=1: frames go to stdout (P8). Otherwise headless.
  bool stream = false;
  // ADSCREENW / ADSCREENH.
  int screen_w = 640;
  int screen_h = 480;
  // ADFRAMES=<n>: stop after n frames. 0 (the default) = no limit.
  uint64_t frames = 0;
  // ADFBHASH=1: "FBHASH <frame> <hex64>" per frame on stderr.
  bool fbhash = false;
  // ADOUT=<dir>: headless frame dump as frame_NNNNN.ppm (UTF-8 path).
  std::string out_dir;
  // ADCVSET=<i>=<v>,… (an <i>:<v> spelling is accepted too),
  // in the order given.
  std::vector<std::pair<int, int32_t>> cvset;
  // ADSEED=<n> | random. Default 1: nothing we own reads entropy unless asked.
  uint64_t seed = 1;
  bool seed_random = false;  // ADSEED=random was given (seed holds what was drawn)
  // ADNOPACE=1: never sleep.
  bool no_pace = false;
  // AD_ASSETS_DIR (asset root, the directory that holds win\). Default
  // <data_root>\assets, i.e. %LOCALAPPDATA%\LongAfterDark\assets. See
  // win_assets_dir().
  std::string assets_root;
  // The per-user data folder (data_root.h): <base>\LongAfterDark, where base
  // is AD_LOCALAPPDATA when that is set and not blank (tests), else
  // LOCALAPPDATA. "" when neither is set.
  std::string data_root;
  // ADTRACE=<cat>,… (lower-cased; "all" / "*" = every category).
  std::set<std::string> trace;

  // Host-local extensions (README.md):
  // ADPACEMS=<ms>: override the lane's frame period (free-run pacing and the
  // headless virtual tick). 0 = use the lane's own.
  double pace_ms = 0.0;
  // ADSTREAMFORCE=1: stream even when stdout is a disk file.
  bool stream_force = false;
  // ADSTREAMP6=1: emit P6 (RGB) frames instead of P8, to exercise a front-end's
  // fallback path.
  bool stream_p6 = false;
  // ADGOWAITMS=<ms>: before frame 0, wait this long for the first stdin line
  // so a lockstep front-end's opening GO is seen before anything is emitted.
  uint32_t go_wait_ms = 250;

  // Interaction (INTERACTION.md §3.1):
  // ADSTATE=<dir> | :memory: — the per-user state root (§7). "" = the overlay
  // lives in memory (ADSTATE unset or ":memory:"); --configure applies
  // use_configure_state_default() so an unset ADSTATE means
  // <data_root>\state (%LOCALAPPDATA%\LongAfterDark\state) there.
  std::string state_root;
  bool state_persistent() const { return !state_root.empty(); }
  // ADCAPS=0|1: the Caps Lock toggle at start, applied to InputState.caps
  // before Lane::init.
  bool caps_at_start = false;

  // Every AD*-prefixed variable (and command-line override), verbatim.
  std::map<std::string, std::string> vars;
  // Malformed values that fell back to defaults, for the host to report.
  std::vector<std::string> warnings;

  // Pure parse over a variable map (tests use this directly).
  static Env parse(const std::map<std::string, std::string>& vars);
  // Snapshot the process environment (AD* names, which include AD_ASSETS_DIR
  // and AD_LOCALAPPDATA, plus LOCALAPPDATA for the default root), apply
  // overrides, parse. Like parse(), it never touches the disk.
  static Env from_process(const std::vector<std::pair<std::string, std::string>>& overrides = {});

  const std::string* get(std::string_view name) const;
  bool flag(std::string_view name) const;  // set and not ""/0/false/no/off
  bool traced(std::string_view cat) const;

  // <assets_root>\win when it holds FILES\, packages\ or catalog-win.json;
  // else assets_root itself when *it* holds one of them (AD_ASSETS_DIR pointed
  // one level too deep); else <root>\win. (PACKAGES.md §5.2; the importer
  // applies the same rule.)
  std::string win_assets_dir() const;

  // --configure's rule: with ADSTATE unset (not ":memory:") the state root is
  // <data_root>\state (%LOCALAPPDATA%\LongAfterDark\state).
  void use_configure_state_default();
};

// §7.1: the package a module path belongs to, for its state directory:
//   ...\FILES\<MODDIR>\<module>          -> "deluxe"
//   ...\packages\<id>\<MODDIR>\<module> -> "<id>" (lower-cased)
//   anything else                        -> "legacy-<fnv32 of the lower-cased module dir, 8 hex>"
std::string package_state_name(const Env& env, const std::string& module_path);
// <state_root>\<package_state_name>, or "" when the state is in memory.
std::string package_state_dir(const Env& env, const std::string& module_path);

// Exposed for tests: the flag truthiness rule and the ADCVSET grammar.
bool env_truthy(std::string_view value);
bool parse_cvset(std::string_view text, std::vector<std::pair<int, int32_t>>& out,
                 std::vector<std::string>* warnings = nullptr);

}  // namespace adw
