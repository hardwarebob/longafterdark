// Which of the front-end's hosts make sound (AUDIO.md §9). Window-free, so
// the tests can pin the rules.
//
// Exactly one host may play: the one behind the primary monitor's window of
// a "/s" run (the input owner, saver.cc App::owner()), with Sound=1 in the
// settings that run read -- the settings dialog's Preview is such a run, on
// the dialog's unsaved values. That host gets ADSOUND=1 ADVOLUME=<Volume>.
// Every other host gets ADSOUND=0 explicitly, and ADAUDIOOUT and ADVOLUME
// removed, so nothing inherited turns its sound on (adw/core/audio.h: a set
// ADAUDIOOUT alone does): the other monitors' hosts, "/p" (the Control Panel
// thumbnail), the dialog's live preview, its thumbnail generator, and
// `--configure` / `--capabilities`.
//
// AD_SCR_SOUND=0 turns sound off whatever the settings say (the scr tests
// set it: their GUI smoke tests run "/s").
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "settings.h"

namespace adw::scr {

// How long a host is given to end after QUIT before it is terminated: the
// sound host needs longer, to silence its device and send MIDI
// all-notes-off (AUDIO.md §9 "Waking": at least 200 ms).
inline constexpr unsigned long kHostStopGraceMs = 150;
inline constexpr unsigned long kSoundHostStopGraceMs = 400;

inline constexpr wchar_t kSoundOverrideEnv[] = L"AD_SCR_SOUND";

// What a host is started for.
enum class HostRole {
  saver,           // "/s": one host per monitor window
  control_panel,   // "/p": Screen Saver Settings' thumbnail
  live_preview,    // the settings dialog's live preview
  thumbnail,       // the settings dialog's background thumbnail generator
  tool,            // `--configure` (a module's own settings windows), `--capabilities`
};
const char* host_role_name(HostRole r);

// AD_SCR_SOUND=0 (also "off", "no", "false"): sound off for every host.
bool sound_forced_off();

struct SoundChoice {
  bool on = false;
  int volume = kDefaultVolume;   // 0..100; meaningful only when `on`
};
// Pure. `owner`: for HostRole::saver, the host is the primary monitor's
// window's (the input owner's).
SoundChoice sound_for(const Settings& s, HostRole role, bool owner, bool forced_off);

using EnvChanges = std::vector<std::pair<std::wstring, std::wstring>>;
// The environment changes that carry `c` to a host (an empty value removes
// the variable: host_process.h build_environment_block).
EnvChanges sound_env(const SoundChoice& c);
void add_sound_env(EnvChanges& env, const SoundChoice& c);

} // namespace adw::scr
