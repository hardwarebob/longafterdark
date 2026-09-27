// StatusPublisher (INTERACTION.md §3.4) and the --configure JSON helpers.
#include "adw/core/status.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include "adw/core/env.h"
#include "adw/core/lane.h"
#include "adw/core/log.h"

namespace adw {

uint32_t status_flags(const LaneStatus& s, bool ready) {
  uint32_t f = 0;
  if (s.interactive) f |= ADWS_INTERACTIVE;
  if (s.cursor) f |= ADWS_CURSOR;
  if (s.rotate_ok) f |= ADWS_ROTATE_OK;
  if (s.key_filter) f |= ADWS_KEY_FILTER;
  if (s.wake) f |= ADWS_WAKE;
  if (ready) f |= ADWS_READY;
  return f;
}

std::string format_status_line(const AdwHostStatusV1& s) {
  char line[160];
  snprintf(line, sizeof(line), "STATUS %" PRIu64 " flags=0x%x applied=%" PRIu64 " eaten=%" PRIu64 " src=%u",
           s.frames, unsigned(s.flags), s.input_applied, s.input_eaten, unsigned(s.source));
  return line;
}

StatusPublisher::~StatusPublisher() {
  if (view_) UnmapViewOfFile(view_);
}

bool StatusPublisher::open_handle(HANDLE section) {
  if (view_) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (!section || section == INVALID_HANDLE_VALUE) return false;
  view_ = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(AdwHostStatusV1));
  return view_ != nullptr;
}

bool StatusPublisher::open(const Env& env) {
  log_ = env.flag("ADSTATUSLOG");
  const std::string* v = env.get("ADSTATUSHANDLE");
  if (!v || v->empty()) return false;
  // Decimal, or 0x hex (the env rule everywhere else).
  const char* s = v->c_str();
  while (*s == ' ' || *s == '\t') s++;
  int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
  char* end = nullptr;
  unsigned long long value = strtoull(s, &end, base);
  while (end && (*end == ' ' || *end == '\t')) end++;
  if (!end || *end || value == 0) {
    log("ADSTATUSHANDLE='%s' is not a handle value; no status record", v->c_str());
    return false;
  }
  HANDLE h = HANDLE(uintptr_t(value));
  if (!open_handle(h)) {
    log("ADSTATUSHANDLE=%s does not map (error %lu); no status record", v->c_str(), GetLastError());
    return false;
  }
  trace("proto", "status record mapped from handle %s", v->c_str());
  return true;
}

void StatusPublisher::publish(const LaneStatus& s, uint64_t frames, uint64_t input_applied, uint32_t lane,
                              bool ready) {
  if (!active()) return;
  AdwHostStatusV1 rec{};
  rec.magic = kStatusMagic;
  rec.version = kStatusVersion;
  rec.size = uint16_t(sizeof(AdwHostStatusV1));
  rec.flags = status_flags(s, ready);
  rec.frames = frames;
  rec.input_applied = input_applied;
  rec.input_eaten = s.eaten;
  rec.source = s.source;
  rec.lane = lane;
  if (view_) write_status(view_, rec);
  if (log_) {
    bool changed = !logged_once_ || rec.flags != last_.flags || rec.input_applied != last_.input_applied ||
                   rec.input_eaten != last_.input_eaten;
    if (changed) {
      std::string line = format_status_line(rec);
      line.push_back('\n');
      write_stderr(line);
    }
  }
  logged_once_ = true;
  last_ = rec;
}

// ---- --configure JSON ------------------------------------------------------

namespace {

void json_string(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
          out += buf;
        } else {
          out.push_back(char(c));  // UTF-8 passes through
        }
    }
  }
  out.push_back('"');
}

}  // namespace

std::string configure_json(ConfigureResult r, int dialogs, const std::string& message,
                           const std::vector<std::string>& written) {
  const char* result = r == ConfigureResult::shown ? "ok" : r == ConfigureResult::nothing ? "nothing" : "error";
  std::string out = "{\"result\":\"";
  out += result;
  out += "\",\"dialogs\":";
  out += std::to_string(dialogs < 0 ? 0 : dialogs);
  out += ",\"message\":";
  json_string(out, message);
  out += ",\"written\":[";
  for (size_t i = 0; i < written.size(); i++) {
    if (i) out.push_back(',');
    json_string(out, written[i]);
  }
  out += "]}";
  return out;
}

int configure_exit_code(ConfigureResult r) {
  switch (r) {
    case ConfigureResult::shown: return 0;
    case ConfigureResult::nothing: return 4;
    case ConfigureResult::unsupported: return 5;
    case ConfigureResult::failed: return 1;
  }
  return 1;
}

}  // namespace adw
