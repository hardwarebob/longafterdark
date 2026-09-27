// audio::Config::from_env — the audio knobs of docs/AUDIO.md §4.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>

#include "adw/core/audio.h"
#include "adw/core/env.h"

namespace adw::audio {

namespace {

std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace(uint8_t(s[a]))) a++;
  while (b > a && isspace(uint8_t(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

// Whole-string decimal integer.
bool parse_int(const std::string& text, long long& out) {
  std::string s = trimmed(text);
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  long long v = strtoll(s.c_str(), &end, 10);
  if (errno || !end || *end) return false;
  out = v;
  return true;
}

// An integer knob: absent = the default; malformed = the default and a
// warning; out of range = clamped and a warning.
template <typename T>
T int_knob(const Env& env, const char* name, T def, long long lo, long long hi, std::vector<std::string>* warnings) {
  const std::string* v = env.get(name);
  if (!v || trimmed(*v).empty()) return def;
  long long n = 0;
  if (!parse_int(*v, n)) {
    if (warnings) warnings->push_back(std::string(name) + ": ignoring malformed value '" + *v + "'");
    return def;
  }
  if (n < lo || n > hi) {
    long long c = std::clamp(n, lo, hi);
    if (warnings)
      warnings->push_back(std::string(name) + "=" + trimmed(*v) + " is out of range " + std::to_string(lo) + ".." +
                          std::to_string(hi) + "; using " + std::to_string(c));
    n = c;
  }
  return T(n);
}

// A default-on switch: only an explicit false value turns it off.
bool default_on(const Env& env, const char* name) {
  const std::string* v = env.get(name);
  return !v || trimmed(*v).empty() || env_truthy(*v);
}

}  // namespace

Config Config::from_env(const Env& env, std::vector<std::string>* warnings) {
  Config c;
  const bool sound = env.flag("ADSOUND");
  if (const std::string* out = env.get("ADAUDIOOUT")) c.capture_wav = trimmed(*out);
  if (!c.capture_wav.empty()) {
    // <file.wav> -> <file>.mid: the extension of the last path component is
    // replaced (appended when it has none, or when it already is .mid).
    size_t slash = c.capture_wav.find_last_of("\\/");
    size_t dot = c.capture_wav.find_last_of('.');
    std::string stem = c.capture_wav;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) {
      std::string ext = c.capture_wav.substr(dot);
      for (char& ch : ext) ch = char(tolower(uint8_t(ch)));
      if (ext != ".mid") stem = c.capture_wav.substr(0, dot);
    }
    c.capture_mid = stem + ".mid";
  }
  c.guest_sound = sound || !c.capture_wav.empty();
  c.volume = int_knob<int>(env, "ADVOLUME", 50, 0, 100, warnings);
  c.rate = int_knob<uint32_t>(env, "ADAUDIORATE", 44100, 8000, 96000, warnings);
  c.latency_ms = int_knob<uint32_t>(env, "ADAUDIOLATENCYMS", 80, 20, 500, warnings);
  c.live = c.guest_sound && sound && env.stream && default_on(env, "ADAUDIOLIVE");
  c.live_midi = default_on(env, "ADMIDI");
  c.midi_device = int_knob<int>(env, "ADMIDIDEV", -1, -1, 0xFFFF, warnings);
  c.mpc_base_channels = env.flag("ADMIDIBASE");
  return c;
}

}  // namespace adw::audio
