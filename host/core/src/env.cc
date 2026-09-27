#include "adw/core/env.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "adw/core/data_root.h"
#include "adw/core/rng.h"
#include "adw/core/screen.h"
#include "adw/core/text.h"

namespace adw {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && isspace(uint8_t(s.front()))) s.remove_prefix(1);
  while (!s.empty() && isspace(uint8_t(s.back()))) s.remove_suffix(1);
  return s;
}

std::string upper(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = char(toupper(uint8_t(c)));
  return r;
}

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = char(tolower(uint8_t(c)));
  return r;
}

// The numeric base of a (trimmed) value: 16 for an explicit 0x prefix after an
// optional sign, else 10. Never base 0: strtoll's octal rule would read a
// front-end's zero-padded "010" as 8, where it means 10.
int numeric_base(const std::string& s) {
  size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
  return (s.size() > i + 1 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) ? 16 : 10;
}

// Whole-string integer (decimal, or 0x-hex), no trailing junk.
bool parse_i64(std::string_view text, int64_t& out) {
  std::string s(trim(text));
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  long long v = strtoll(s.c_str(), &end, numeric_base(s));
  if (errno || !end || *end) return false;
  out = v;
  return true;
}

bool parse_u64(std::string_view text, uint64_t& out) {
  std::string s(trim(text));
  if (s.empty() || s[0] == '-') return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long v = strtoull(s.c_str(), &end, numeric_base(s));
  if (errno || !end || *end) return false;
  out = v;
  return true;
}

bool parse_double(std::string_view text, double& out) {
  std::string s(trim(text));
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double v = strtod(s.c_str(), &end);
  if (errno || !end || *end) return false;
  out = v;
  return true;
}

bool dir_exists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool file_exists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string join(const std::string& a, const char* b) {
  if (a.empty()) return b;
  char last = a.back();
  return (last == '\\' || last == '/') ? a + b : a + "\\" + b;
}

// data_root_base()'s rule (data_root.h) over the snapshot: AD_LOCALAPPDATA
// when set and not blank, else LOCALAPPDATA, trimmed.
std::string data_root_base_of(const Env& e) {
  for (const char* name : {"AD_LOCALAPPDATA", "LOCALAPPDATA"}) {
    if (const std::string* v = e.get(name); v && !trim(*v).empty()) return std::string(trim(*v));
  }
  return {};
}

uint64_t entropy_seed() {
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  Rng r(uint64_t(qpc.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32) ^ GetTickCount64());
  return r.next();
}

}  // namespace

bool env_truthy(std::string_view value) {
  std::string v = lower(trim(value));
  return !(v.empty() || v == "0" || v == "false" || v == "no" || v == "off");
}

bool parse_cvset(std::string_view text, std::vector<std::pair<int, int32_t>>& out,
                 std::vector<std::string>* warnings) {
  bool ok = true;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string_view::npos) comma = text.size();
    std::string_view item = trim(text.substr(pos, comma - pos));
    pos = comma + 1;
    if (item.empty()) {
      if (comma >= text.size()) break;
      continue;
    }
    size_t sep = item.find_first_of("=:");
    int64_t idx = 0, val = 0;
    if (sep == std::string_view::npos || !parse_i64(item.substr(0, sep), idx) ||
        !parse_i64(item.substr(sep + 1), val) || idx < 0 || idx > 0xFFFF ||
        val < INT32_MIN || val > INT32_MAX) {
      ok = false;
      if (warnings) warnings->push_back("ADCVSET: ignoring malformed item '" + std::string(item) + "'");
      continue;
    }
    out.emplace_back(int(idx), int32_t(val));
  }
  return ok;
}

const std::string* Env::get(std::string_view name) const {
  auto it = vars.find(upper(name));
  return it == vars.end() ? nullptr : &it->second;
}

bool Env::flag(std::string_view name) const {
  const std::string* v = get(name);
  return v && env_truthy(*v);
}

bool Env::traced(std::string_view cat) const {
  return trace.count("all") || trace.count("*") || trace.count(lower(cat));
}

std::string Env::win_assets_dir() const {
  // A win dir is recognised by what the importer puts there (PACKAGES.md
  // §5.2): Deluxe's FILES\, the packages\ of every other release, or the
  // catalog. An install holding only non-Deluxe packages has no FILES\.
  auto holds_assets = [](const std::string& d) {
    return dir_exists(join(d, "FILES")) || dir_exists(join(d, "packages")) ||
           file_exists(join(d, "catalog-win.json"));
  };
  std::string win = join(assets_root, "win");
  if (holds_assets(win)) return win;
  if (!assets_root.empty() && holds_assets(assets_root)) return assets_root;
  return win;
}

Env Env::parse(const std::map<std::string, std::string>& in) {
  Env e;
  for (const auto& [k, v] : in) e.vars[upper(k)] = v;

  auto num_in = [&](const char* name, int64_t lo, int64_t hi, int64_t& target) {
    const std::string* v = e.get(name);
    if (!v) return;
    int64_t n = 0;
    if (parse_i64(*v, n) && n >= lo && n <= hi) {
      target = n;
    } else {
      e.warnings.push_back(std::string(name) + "='" + *v + "' is not an integer in [" +
                           std::to_string(lo) + ", " + std::to_string(hi) + "]; using " +
                           std::to_string(target));
    }
  };

  e.stream = e.flag("ADSTREAM");
  int64_t w = e.screen_w, h = e.screen_h;
  num_in("ADSCREENW", 1, Screen::kMaxDim, w);
  num_in("ADSCREENH", 1, Screen::kMaxDim, h);
  e.screen_w = int(w);
  e.screen_h = int(h);

  if (const std::string* v = e.get("ADFRAMES")) {
    uint64_t n = 0;
    if (parse_u64(*v, n)) e.frames = n;
    else e.warnings.push_back("ADFRAMES='" + *v + "' is not a frame count; running unbounded");
  }
  e.fbhash = e.flag("ADFBHASH");
  if (const std::string* v = e.get("ADOUT")) e.out_dir = std::string(trim(*v));
  if (const std::string* v = e.get("ADCVSET")) parse_cvset(*v, e.cvset, &e.warnings);

  if (const std::string* v = e.get("ADSEED")) {
    uint64_t s = 0;
    if (lower(trim(*v)) == "random") {
      e.seed = entropy_seed();
      e.seed_random = true;
    } else if (parse_u64(*v, s)) {
      e.seed = s;
    } else {
      e.warnings.push_back("ADSEED='" + *v + "' is not a number or 'random'; using 1");
    }
  }
  e.no_pace = e.flag("ADNOPACE");

  e.data_root = narrow(data_root_path(widen(data_root_base_of(e))));
  if (const std::string* v = e.get("AD_ASSETS_DIR"); v && !trim(*v).empty()) {
    e.assets_root = std::string(trim(*v));
  } else if (!e.data_root.empty()) {
    e.assets_root = join(e.data_root, "assets");
  }

  if (const std::string* v = e.get("ADTRACE")) {
    std::string cur;
    for (char c : *v + ",") {
      if (c == ',' || c == ';' || isspace(uint8_t(c))) {
        if (!cur.empty()) e.trace.insert(lower(cur));
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
  }

  if (const std::string* v = e.get("ADPACEMS")) {
    double ms = 0;
    if (parse_double(*v, ms) && ms >= 0.0 && ms <= 10000.0) e.pace_ms = ms;
    else e.warnings.push_back("ADPACEMS='" + *v + "' is not in [0, 10000]; using the lane's rate");
  }
  e.stream_force = e.flag("ADSTREAMFORCE");
  e.stream_p6 = e.flag("ADSTREAMP6");
  int64_t gw = e.go_wait_ms;
  num_in("ADGOWAITMS", 0, 60000, gw);
  e.go_wait_ms = uint32_t(gw);

  if (const std::string* v = e.get("ADSTATE")) {
    std::string root(trim(*v));
    if (root != ":memory:") {
      // No trailing separator, so <root>\<package> joins cleanly.
      while (root.size() > 3 && (root.back() == '\\' || root.back() == '/')) root.pop_back();
      e.state_root = root;
    }
  }
  e.caps_at_start = e.flag("ADCAPS");
  return e;
}

void Env::use_configure_state_default() {
  if (get("ADSTATE")) return;  // set: a directory, or ":memory:" on purpose
  if (data_root.empty()) return;
  state_root = join(data_root, "state");
}

namespace {

// Path components of a module path, separators normalized and "." and ".."
// resolved lexically (a relative path stays relative: a ".." with nothing
// before it to take back is kept, and never names a package).
std::vector<std::string> path_parts(const std::string& path) {
  std::vector<std::string> parts;
  std::string cur;
  auto flush = [&] {
    if (cur == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else {
        parts.push_back(cur);
      }
    } else if (!cur.empty() && cur != ".") {
      parts.push_back(cur);
    }
    cur.clear();
  };
  for (char c : path) {
    if (c == '\\' || c == '/') {
      flush();
    } else {
      cur.push_back(c);
    }
  }
  flush();
  return parts;
}

// A package folder name that can be a state folder name: the registry's ids
// are [a-z0-9_-]+ (after lower-casing); "." and ".." never pass.
bool is_package_id(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

uint32_t fnv1a32(std::string_view s) {
  uint32_t h = 2166136261u;
  for (char c : s) {
    h ^= uint8_t(c);
    h *= 16777619u;
  }
  return h;
}

}  // namespace

std::string package_state_name(const Env& env, const std::string& module_path) {
  (void)env;
  // The two packaged layouts are recognised from the last parts alone, so a
  // relative catalog path ("FILES/AD40/X.AD") names the same package as the
  // absolute path adhostwin resolves it to.
  // Only a registry-shaped id names a package: a hand-written catalog or
  // command-line path whose package component is anything else ("..", a
  // name with spaces or dots) gets a legacy name below, so its state cannot
  // land beside the state root.
  std::vector<std::string> parts = path_parts(module_path);
  if (parts.size() >= 3) {
    if (lower(parts[parts.size() - 3]) == "files") return "deluxe";
    if (parts.size() >= 4 && lower(parts[parts.size() - 4]) == "packages") {
      std::string id = lower(parts[parts.size() - 3]);
      if (is_package_id(id)) return id;
    }
  }
  // Anything else: a stable name from the module's (absolute, lower-cased)
  // directory.
  std::string full = module_path;
  std::wstring w = widen(full);
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetFullPathNameW(w.c_str(), DWORD(std::size(buf)), buf, nullptr);
  if (n > 0 && n < std::size(buf)) full = narrow(std::wstring_view(buf, n));
  parts = path_parts(full);
  std::string dir;
  for (size_t i = 0; i + 1 < parts.size(); i++) {
    if (!dir.empty()) dir += '\\';
    dir += lower(parts[i]);
  }
  char name[32];
  snprintf(name, sizeof(name), "legacy-%08x", unsigned(fnv1a32(dir)));
  return name;
}

std::string package_state_dir(const Env& env, const std::string& module_path) {
  if (!env.state_persistent()) return "";
  return join(env.state_root, package_state_name(env, module_path).c_str());
}

Env Env::from_process(const std::vector<std::pair<std::string, std::string>>& overrides) {
  std::map<std::string, std::string> vars;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
      std::wstring_view entry(p);
      // "=C:=C:\dir" entries are the shell's per-drive cwd bookkeeping.
      size_t eq = entry.find(L'=', 1);
      if (entry.empty() || entry[0] == L'=' || eq == std::wstring_view::npos) continue;
      std::string name = upper(narrow(entry.substr(0, eq)));
      if (name.compare(0, 2, "AD") != 0 && name != "LOCALAPPDATA") continue;
      vars[name] = narrow(entry.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [k, v] : overrides) vars[upper(k)] = v;
  return parse(vars);
}

}  // namespace adw
