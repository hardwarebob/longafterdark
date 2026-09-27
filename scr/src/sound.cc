#include "sound.h"

#include <algorithm>

#include "paths.h"

namespace adw::scr {

const char* host_role_name(HostRole r) {
  switch (r) {
    case HostRole::saver: return "saver";
    case HostRole::control_panel: return "control-panel";
    case HostRole::live_preview: return "live-preview";
    case HostRole::thumbnail: return "thumbnail";
    case HostRole::tool: return "tool";
  }
  return "?";
}

bool sound_forced_off() {
  std::wstring v = env_w(kSoundOverrideEnv);
  while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
  while (!v.empty() && (v.front() == L' ' || v.front() == L'\t')) v.erase(v.begin());
  for (const wchar_t* off : {L"0", L"off", L"no", L"false"}) {
    if (_wcsicmp(v.c_str(), off) == 0) return true;
  }
  return false;
}

SoundChoice sound_for(const Settings& s, HostRole role, bool owner, bool forced_off) {
  SoundChoice c;
  // SoundMonitor is reserved: every value is the primary monitor for now.
  c.on = role == HostRole::saver && owner && s.sound && !forced_off;
  c.volume = c.on ? std::clamp(s.volume, 0, 100) : kDefaultVolume;
  return c;
}

void add_sound_env(EnvChanges& env, const SoundChoice& c) {
  // Replace whatever the spec already said about these.
  std::erase_if(env, [](const auto& kv) {
    return _wcsicmp(kv.first.c_str(), L"ADSOUND") == 0 || _wcsicmp(kv.first.c_str(), L"ADVOLUME") == 0 ||
           _wcsicmp(kv.first.c_str(), L"ADAUDIOOUT") == 0;
  });
  if (c.on) {
    env.emplace_back(L"ADSOUND", L"1");
    env.emplace_back(L"ADVOLUME", std::to_wstring(std::clamp(c.volume, 0, 100)));
    // ADAUDIOOUT (a capture) is left as inherited: a test or a person
    // chasing a problem can have the sound host write one.
  } else {
    env.emplace_back(L"ADSOUND", L"0");
    env.emplace_back(L"ADVOLUME", L"");
    env.emplace_back(L"ADAUDIOOUT", L"");   // it would turn sound on by itself
  }
}

EnvChanges sound_env(const SoundChoice& c) {
  EnvChanges env;
  add_sound_env(env, c);
  return env;
}

} // namespace adw::scr
