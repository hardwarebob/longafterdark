#include "win32/ini_store.hh"

#include <algorithm>
#include <cctype>

#include "adw/core/log.h"

namespace adw::win32 {

namespace {

std::string upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(toupper(uint8_t(c)));
  return o;
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (toupper(uint8_t(a[i])) != toupper(uint8_t(b[i]))) return false;
  return true;
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
  return std::string(s.substr(a, b - a));
}

bool same_stamp(const Vfs::Stat& a, const Vfs::Stat& b) {
  return a.exists == b.exists && a.layer == b.layer && a.size == b.size && a.write_time == b.write_time &&
         a.version == b.version && a.host == b.host;
}

using Line = IniStore::Line;

// Index of the first section line named `section`, or -1.
int find_section(const std::vector<Line>& lines, std::string_view section) {
  for (size_t i = 0; i < lines.size(); i++)
    if (lines[i].kind == Line::Kind::section && ieq(lines[i].name, section)) return int(i);
  return -1;
}

// [begin, end) of the section's body.
size_t section_end(const std::vector<Line>& lines, size_t header) {
  size_t i = header + 1;
  while (i < lines.size() && lines[i].kind != Line::Kind::section) i++;
  return i;
}

}  // namespace

IniStore::IniStore(Vfs& vfs) : vfs_(vfs) {}

std::vector<Line> IniStore::parse(std::string_view text) {
  std::vector<Line> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string_view::npos) nl = text.size();
    std::string_view raw = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    Line l;
    l.text = std::string(raw);
    std::string t = trim(raw);
    if (!t.empty() && t[0] == '[') {
      size_t close = t.find(']');
      l.kind = Line::Kind::section;
      l.name = trim(std::string_view(t).substr(1, close == std::string::npos ? std::string::npos : close - 1));
    } else if (!t.empty() && t[0] != ';') {
      size_t eq = t.find('=');
      if (eq != std::string::npos) {
        l.kind = Line::Kind::key;
        l.name = trim(std::string_view(t).substr(0, eq));
        l.value = trim(std::string_view(t).substr(eq + 1));
      }
    }
    out.push_back(std::move(l));
  }
  return out;
}

std::string IniStore::serialize(const std::vector<Line>& lines) {
  std::string out;
  for (const Line& l : lines) {
    out += l.text;
    out += "\r\n";
  }
  return out;
}

std::string IniStore::key_of(std::string_view guest_path) const { return upper(vfs_.full_path(guest_path)); }

void IniStore::add_seed(std::string_view guest_path, std::string_view section, std::string_view key,
                        std::string_view value) {
  auto& v = seeds_[key_of(guest_path)];
  for (Seed& s : v) {
    if (ieq(s.section, section) && ieq(s.key, key)) {
      s.value = std::string(value);
      return;
    }
  }
  v.push_back(Seed{std::string(section), std::string(key), std::string(value)});
}

const std::vector<Line>& IniStore::load(const std::string& full) {
  Vfs::Stat st;
  bool present = vfs_.stat(full, &st) && !st.dir;
  if (!present) st = Vfs::Stat{};
  auto it = cache_.find(full);
  if (it != cache_.end() && it->second.present == present && same_stamp(it->second.stamp, st)) return it->second.lines;
  Cached c;
  c.stamp = st;
  c.present = present;
  std::string text;
  if (present && vfs_.read_file(full, &text)) c.lines = parse(text);
  return (cache_[full] = std::move(c)).lines;
}

std::optional<std::string> IniStore::get(std::string_view guest_path, std::string_view section, std::string_view key) {
  std::string full = key_of(guest_path);
  const std::vector<Line>& lines = load(full);
  int s = find_section(lines, section);
  if (s >= 0) {
    size_t end = section_end(lines, size_t(s));
    for (size_t i = size_t(s) + 1; i < end; i++)
      if (lines[i].kind == Line::Kind::key && ieq(lines[i].name, key)) return lines[i].value;
  }
  auto sd = seeds_.find(full);
  if (sd != seeds_.end()) {
    for (const Seed& e : sd->second)
      if (ieq(e.section, section) && ieq(e.key, key)) return e.value;
  }
  return std::nullopt;
}

std::vector<std::string> IniStore::sections(std::string_view guest_path) {
  std::string full = key_of(guest_path);
  std::vector<std::string> out;
  auto have = [&](std::string_view n) {
    return std::any_of(out.begin(), out.end(), [&](const std::string& x) { return ieq(x, n); });
  };
  for (const Line& l : load(full))
    if (l.kind == Line::Kind::section && !have(l.name)) out.push_back(l.name);
  auto sd = seeds_.find(full);
  if (sd != seeds_.end())
    for (const Seed& e : sd->second)
      if (!have(e.section)) out.push_back(e.section);
  return out;
}

std::vector<std::string> IniStore::keys(std::string_view guest_path, std::string_view section) {
  std::string full = key_of(guest_path);
  std::vector<std::string> out;
  auto have = [&](std::string_view n) {
    return std::any_of(out.begin(), out.end(), [&](const std::string& x) { return ieq(x, n); });
  };
  const std::vector<Line>& lines = load(full);
  int s = find_section(lines, section);
  if (s >= 0) {
    size_t end = section_end(lines, size_t(s));
    for (size_t i = size_t(s) + 1; i < end; i++)
      if (lines[i].kind == Line::Kind::key && !have(lines[i].name)) out.push_back(lines[i].name);
  }
  auto sd = seeds_.find(full);
  if (sd != seeds_.end())
    for (const Seed& e : sd->second)
      if (ieq(e.section, section) && !have(e.key)) out.push_back(e.key);
  return out;
}

bool IniStore::set(std::string_view guest_path, std::string_view section, std::optional<std::string_view> key,
                   std::optional<std::string_view> value) {
  std::string full = key_of(guest_path);
  Vfs::StateLock lock(vfs_);
  // Always from the file as it is now (another host may have written it).
  std::string text;
  Vfs::Stat st;
  bool present = vfs_.stat(full, &st) && !st.dir;
  if (present && !vfs_.read_file(full, &text)) {
    log("profile: cannot read %s to update it", full.c_str());
    return false;
  }
  std::vector<Line> lines = present ? parse(text) : std::vector<Line>{};
  int s = find_section(lines, section);
  bool changed = false;
  auto is_seed = [&](std::optional<std::string_view> k) {
    auto sd = seeds_.find(full);
    if (sd == seeds_.end()) return false;
    for (const Seed& e : sd->second)
      if (ieq(e.section, section) && (!k || ieq(e.key, *k))) return true;
    return false;
  };
  if (!key) {
    // Delete the section (every occurrence).
    if (is_seed(std::nullopt)) trace("file", "profile %s: seed entries of [%.*s] stay", full.c_str(), int(section.size()),
                                     section.data());
    while ((s = find_section(lines, section)) >= 0) {
      lines.erase(lines.begin() + s, lines.begin() + ptrdiff_t(section_end(lines, size_t(s))));
      changed = true;
    }
    if (!changed) return true;
  } else if (!value) {
    // Delete the key.
    if (s >= 0) {
      size_t end = section_end(lines, size_t(s));
      for (size_t i = size_t(s) + 1; i < end; i++) {
        if (lines[i].kind == Line::Kind::key && ieq(lines[i].name, *key)) {
          lines.erase(lines.begin() + ptrdiff_t(i));
          changed = true;
          break;
        }
      }
    }
    if (!changed) {
      if (is_seed(key))
        log("profile %s: [%.*s] %.*s is a seed; deleting it is not persisted", full.c_str(), int(section.size()),
            section.data(), int(key->size()), key->data());
      return true;
    }
  } else {
    Line kv;
    kv.kind = Line::Kind::key;
    kv.name = std::string(*key);
    kv.value = std::string(*value);
    if (s < 0) {
      Line h;
      h.kind = Line::Kind::section;
      h.name = std::string(section);
      h.text = "[" + h.name + "]";
      kv.text = kv.name + "=" + kv.value;
      lines.push_back(std::move(h));
      lines.push_back(std::move(kv));
    } else {
      size_t end = section_end(lines, size_t(s));
      size_t found = 0;
      for (size_t i = size_t(s) + 1; i < end && !found; i++)
        if (lines[i].kind == Line::Kind::key && ieq(lines[i].name, *key)) found = i;
      if (found) {
        // Keep the key's spelling.
        lines[found].value = kv.value;
        lines[found].text = lines[found].name + "=" + kv.value;
      } else {
        // After the section's last non-blank line.
        size_t at = end;
        while (at > size_t(s) + 1 && trim(lines[at - 1].text).empty()) at--;
        kv.text = kv.name + "=" + kv.value;
        lines.insert(lines.begin() + ptrdiff_t(at), std::move(kv));
      }
    }
  }
  std::string out = serialize(lines);
  uint32_t err = 0;
  if (!vfs_.write_file(full, out, &err)) {
    log("profile: cannot write %s (error %u)", full.c_str(), err);
    return false;
  }
  trace("file", "profile %s: [%.*s] %s%s", full.c_str(), int(section.size()), section.data(),
        key ? std::string(*key).c_str() : "(section)", value ? " set" : " deleted");
  // Re-parse at the next read (even a rewrite of the same size within one
  // file-time tick).
  cache_.erase(full);
  return true;
}

}  // namespace adw::win32
