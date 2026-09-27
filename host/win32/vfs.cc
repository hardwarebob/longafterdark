#include "win32/vfs.hh"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>

#include "adw/core/fnv.h"
#include "adw/core/log.h"
#include "adw/core/text.h"

namespace adw::win32 {

// ---- internals ------------------------------------------------------------------------------------

namespace vfs_detail {

// The bytes of a file open for writing (or of a memory-upper file).
struct FileBuffer {
  std::vector<uint8_t> data;
  bool dirty = false;       // changed since the last commit
  uint64_t write_time = 0;  // FILETIME
  uint64_t version = 0;     // bumps on every commit (memory upper)
};

struct MemNode {
  std::string name;  // the spelling it was created with
  bool dir = false;
  std::shared_ptr<FileBuffer> buf;  // files
};

struct Mount {
  enum class Kind { plain, overlay, drives } kind = Kind::plain;
  std::string guest;  // normalized; no trailing backslash except a drive root ("H:\")
  std::string host;   // plain: the host dir; overlay: the lower dir ("" = none)
  std::string upper;  // overlay: the upper host dir ("" = memory)
  bool writable = false;
  bool short_names = false;
  bool listed = true;  // shown in its parent directory's listing (Vfs::hide_in_listing)
  std::map<std::string, MemNode> mem;  // memory upper: upper-case path below `guest` → node
};

struct Resolved {
  std::string full;          // normalized guest path
  const Mount* m = nullptr;  // the covering mount, if any
  std::string rel;           // below m->guest ("" = the mount's own directory), '\'-separated
};

}  // namespace vfs_detail

using vfs_detail::FileBuffer;
using vfs_detail::MemNode;
using vfs_detail::Mount;
using vfs_detail::Resolved;

namespace {

// 1996-09-12 12:00:00 as a FILETIME: the date memory-upper files start from
// (kernel32's headless epoch).
constexpr uint64_t kMemEpoch = 125132976000000000ull;

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

// s is `prefix` or below it (component-wise): C:\AFTERDRK2 is not under C:\AFTERDRK.
bool iprefix(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    char a = s[i] == '/' ? '\\' : s[i], b = prefix[i] == '/' ? '\\' : prefix[i];
    if (toupper(uint8_t(a)) != toupper(uint8_t(b))) return false;
  }
  if (s.size() == prefix.size() || prefix.empty()) return true;
  return prefix.back() == '\\' || s[prefix.size()] == '\\' || s[prefix.size()] == '/';
}

std::string slashes(std::string s) {
  for (char& c : s)
    if (c == '/') c = '\\';
  return s;
}

std::string strip_trailing(std::string s) {
  s = slashes(std::move(s));
  while (s.size() > 3 && s.back() == '\\') s.pop_back();
  if (s.size() == 2 && s[1] == ':') s += '\\';
  return s;
}

std::string join(const std::string& a, const std::string& b) {
  if (b.empty()) return a;
  if (a.empty()) return b;
  return a.back() == '\\' ? a + b : a + "\\" + b;
}

bool is_drive_root(const std::string& full) { return full.size() == 3 && full[1] == ':' && full[2] == '\\'; }

// "C:\A\B" → "C:\A"; "C:\A" → "C:\"; "C:\" → "".
std::string parent_of(const std::string& full) {
  if (is_drive_root(full)) return {};
  size_t s = full.find_last_of('\\');
  if (s == std::string::npos) return {};
  if (s == 2) return full.substr(0, 3);
  return full.substr(0, s);
}

std::string name_of(const std::string& p) {
  size_t s = p.find_last_of('\\');
  return s == std::string::npos ? p : p.substr(s + 1);
}

uint64_t ft64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

bool host_attrs(const std::string& path, WIN32_FILE_ATTRIBUTE_DATA* out) {
  if (path.empty()) return false;
  return GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, out) != 0;
}

bool read_host_file(const std::string& path, std::string* out) {
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out->clear();
  char buf[65536];
  DWORD got = 0;
  bool ok = true;
  while ((ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != 0) && got) out->append(buf, got);
  CloseHandle(h);
  return ok;
}

// One host directory's entries (without "." and ".."), with the dots' own
// data when asked.
struct HostEntry {
  std::string name, alt;  // alt: the file system's 8.3 name ("" when none)
  uint32_t attributes = 0;
  uint64_t size = 0, write_time = 0;
};
std::vector<HostEntry> host_dir(const std::string& dir, HostEntry* dot = nullptr, HostEntry* dotdot = nullptr) {
  std::vector<HostEntry> out;
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileExW(widen(join(dir, "*")).c_str(), FindExInfoStandard, &fd, FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    HostEntry e;
    e.name = narrow(fd.cFileName);
    e.alt = upper(narrow(fd.cAlternateFileName));
    e.attributes = fd.dwFileAttributes;
    e.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    e.write_time = ft64(fd.ftLastWriteTime);
    if (e.name == ".") {
      if (dot) *dot = e;
      continue;
    }
    if (e.name == "..") {
      if (dotdot) *dotdot = e;
      continue;
    }
    out.push_back(std::move(e));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

// The 8.3 alias of every entry of a host directory, deterministic: the file
// system's own short name when it has one, the upper-cased name when that is
// already 8.3, else a synthesized BASIS~N.EXT (entries in upper-cased order).
std::vector<std::pair<std::string, std::string>> host_aliases(const std::string& dir) {
  std::vector<HostEntry> es = host_dir(dir);
  std::sort(es.begin(), es.end(), [](const HostEntry& a, const HostEntry& b) { return upper(a.name) < upper(b.name); });
  std::vector<std::string> taken;
  std::vector<std::pair<std::string, std::string>> out(es.size());
  for (size_t i = 0; i < es.size(); i++) {
    out[i].first = es[i].name;
    if (!es[i].alt.empty()) out[i].second = es[i].alt;
    else if (Vfs::is_short_name(es[i].name)) out[i].second = upper(es[i].name);
    if (!out[i].second.empty()) taken.push_back(out[i].second);
    taken.push_back(upper(es[i].name));
  }
  for (size_t i = 0; i < es.size(); i++) {
    if (!out[i].second.empty()) continue;
    out[i].second = Vfs::make_short_alias(es[i].name, taken);
    taken.push_back(out[i].second);
  }
  return out;
}

// ---- VfsFile implementations ----

// A byte buffer (memory upper, or any overlay file open for writing).
class MemFile : public VfsFile {
 public:
  MemFile(std::shared_ptr<FileBuffer> b, bool writable, std::string guest, std::function<bool(FileBuffer&)> commit)
      : b_(std::move(b)), writable_(writable), guest_(std::move(guest)), commit_(std::move(commit)) {}
  ~MemFile() override { flush(); }

  int64_t read(void* dst, uint32_t n) override {
    uint64_t avail = pos_ < b_->data.size() ? b_->data.size() - pos_ : 0;
    uint32_t take = uint32_t(std::min<uint64_t>(avail, n));
    if (take) memcpy(dst, b_->data.data() + pos_, take);
    pos_ += take;
    return take;
  }
  // What fits below Vfs::kMaxFileSize (a seek far out cannot make the host
  // allocate, or commit to the state folder, gigabytes): fewer bytes than
  // asked, 0 at the ceiling, is a full disk.
  int64_t write(const void* src, uint32_t n) override {
    if (!writable_) return -1;
    if (!n || pos_ >= Vfs::kMaxFileSize) return 0;
    n = uint32_t(std::min<uint64_t>(n, Vfs::kMaxFileSize - pos_));
    if (b_->data.size() < pos_ + n) b_->data.resize(size_t(pos_ + n), 0);
    memcpy(b_->data.data() + pos_, src, n);
    pos_ += n;
    b_->dirty = true;
    return n;
  }
  int64_t seek(int64_t offset, int whence) override {
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(pos_) : int64_t(b_->data.size());
    int64_t np = base + offset;
    if (np < 0) return -1;
    pos_ = uint64_t(np);
    return np;
  }
  uint64_t size() const override { return b_->data.size(); }
  bool truncate() override {
    if (!writable_ || pos_ > Vfs::kMaxFileSize) return false;
    b_->data.resize(size_t(pos_), 0);
    b_->dirty = true;
    return true;
  }
  bool flush() override {
    if (!writable_ || !b_->dirty || !commit_) return true;
    b_->dirty = false;
    return commit_(*b_);
  }
  uint64_t tell() const override { return pos_; }
  bool writable() const override { return writable_; }
  void times(uint64_t* c, uint64_t* a, uint64_t* w) const override {
    if (c) *c = b_->write_time;
    if (a) *a = b_->write_time;
    if (w) *w = b_->write_time;
  }
  const std::string& guest_path() const override { return guest_; }

 private:
  std::shared_ptr<FileBuffer> b_;
  uint64_t pos_ = 0;
  bool writable_;
  std::string guest_;
  std::function<bool(FileBuffer&)> commit_;
};

// A host file (read-only layers, and plain writable mounts written in place).
class HostFile : public VfsFile {
 public:
  HostFile(HANDLE h, bool writable, std::string guest) : h_(h), writable_(writable), guest_(std::move(guest)) {}
  ~HostFile() override { CloseHandle(h_); }

  int64_t read(void* dst, uint32_t n) override {
    DWORD got = 0;
    if (n && !ReadFile(h_, dst, n, &got, nullptr)) return -1;
    return got;
  }
  int64_t write(const void* src, uint32_t n) override {
    if (!writable_) return -1;
    // The same ceiling as an overlay's files (Vfs::kMaxFileSize).
    uint64_t at = tell();
    if (!n || at >= Vfs::kMaxFileSize) return 0;
    n = uint32_t(std::min<uint64_t>(n, Vfs::kMaxFileSize - at));
    DWORD put = 0;
    if (!WriteFile(h_, src, n, &put, nullptr)) return -1;
    return put;
  }
  int64_t seek(int64_t offset, int whence) override {
    LARGE_INTEGER d, out;
    d.QuadPart = offset;
    DWORD method = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(h_, d, &out, method)) return -1;
    return out.QuadPart;
  }
  uint64_t size() const override {
    LARGE_INTEGER sz{};
    return GetFileSizeEx(h_, &sz) ? uint64_t(sz.QuadPart) : 0;
  }
  bool truncate() override { return writable_ && tell() <= Vfs::kMaxFileSize && SetEndOfFile(h_); }
  bool flush() override { return !writable_ || FlushFileBuffers(h_); }
  uint64_t tell() const override {
    LARGE_INTEGER z{}, out{};
    return SetFilePointerEx(h_, z, &out, FILE_CURRENT) ? uint64_t(out.QuadPart) : 0;
  }
  bool writable() const override { return writable_; }
  void times(uint64_t* c, uint64_t* a, uint64_t* w) const override {
    FILETIME t[3]{};
    ::GetFileTime(h_, &t[0], &t[1], &t[2]);
    if (c) *c = ft64(t[0]);
    if (a) *a = ft64(t[1]);
    if (w) *w = ft64(t[2]);
  }
  const std::string& guest_path() const override { return guest_; }

 private:
  HANDLE h_;
  bool writable_;
  std::string guest_;
};

bool valid_83_char(uint8_t c) {
  if (c >= 0x80) return true;
  if (isalnum(c)) return true;
  return strchr("$%'-_@~`!(){}^#&", c) != nullptr && c != 0;
}

}  // namespace

// ---- Vfs: mounts and paths --------------------------------------------------------------------------

Vfs::Vfs() : mem_time_(kMemEpoch) {}

Vfs::~Vfs() {
  if (mutex_) CloseHandle(static_cast<HANDLE>(mutex_));
}

namespace {
void insert_mount(std::vector<std::unique_ptr<Mount>>& mounts, std::unique_ptr<Mount> m) {
  // Longest prefix first, so nested mounts win; a re-mount replaces.
  for (auto it = mounts.begin(); it != mounts.end(); ++it) {
    if (ieq((*it)->guest, m->guest)) {
      *it = std::move(m);
      return;
    }
  }
  auto it = mounts.begin();
  while (it != mounts.end() && (*it)->guest.size() >= m->guest.size()) ++it;
  mounts.insert(it, std::move(m));
}
}  // namespace

void Vfs::mount(std::string_view guest_dir, std::string_view host_dir, bool writable) {
  auto m = std::make_unique<Mount>();
  m->kind = Mount::Kind::plain;
  m->guest = strip_trailing(full_path(guest_dir));
  m->host = strip_trailing(std::string(host_dir));
  m->writable = writable;
  insert_mount(mounts_, std::move(m));
}

void Vfs::mount_overlay(std::string_view guest_dir, std::string_view lower_host, std::string_view upper_host) {
  auto m = std::make_unique<Mount>();
  m->kind = Mount::Kind::overlay;
  m->guest = strip_trailing(full_path(guest_dir));
  m->host = lower_host.empty() ? std::string() : strip_trailing(std::string(lower_host));
  m->upper = upper_host.empty() ? std::string() : strip_trailing(std::string(upper_host));
  m->writable = true;
  insert_mount(mounts_, std::move(m));
}

void Vfs::hide_in_listing(std::string_view guest_dir) {
  std::string g = strip_trailing(full_path(guest_dir));
  for (auto& m : mounts_) {
    if (ieq(m->guest, g)) m->listed = false;
  }
}

void Vfs::add_virtual_file(std::string_view guest_path, std::vector<uint8_t> bytes) {
  std::string full = full_path(guest_path);
  virtual_files_[upper(full)] = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
  virtual_names_[upper(full)] = name_of(full);
}

void Vfs::mount_host_drives(bool short_names) {
  auto m = std::make_unique<Mount>();
  m->kind = Mount::Kind::drives;
  m->guest = "H:\\";
  m->short_names = short_names;
  insert_mount(mounts_, std::move(m));
}

void Vfs::set_state_root(std::string_view host_root) { state_root_ = strip_trailing(std::string(host_root)); }

bool Vfs::persistent() const {
  for (const auto& m : mounts_)
    if (m->kind == Mount::Kind::overlay && !m->upper.empty()) return true;
  return false;
}

std::string Vfs::state_mutex_name() const {
  std::string root = state_root_;
  if (root.empty()) {
    for (const auto& m : mounts_) {
      if (m->kind == Mount::Kind::overlay && !m->upper.empty()) {
        root = m->upper;
        break;
      }
    }
  }
  if (root.empty() || !persistent()) return {};
  std::wstring w = widen(root);
  wchar_t full[MAX_PATH * 4];
  DWORD n = GetFullPathNameW(w.c_str(), DWORD(std::size(full)), full, nullptr);
  if (n && n < std::size(full)) w.assign(full, n);
  while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  if (!w.empty()) CharLowerBuffW(w.data(), DWORD(w.size()));
  std::string key = narrow(w);
  char hex[17];
  snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(fnv1a64(key.data(), key.size())));
  return std::string("Local\\LongAfterDark-state-") + hex;
}

std::string Vfs::full_path(std::string_view guest) const {
  std::string p = slashes(std::string(guest));
  std::string abs;
  if (p.size() >= 2 && p[1] == ':') {
    abs = p;
    if (abs.size() == 2 || abs[2] != '\\') abs.insert(2, "\\");  // "C:X" → "C:\X"
  } else if (!p.empty() && p[0] == '\\') {
    abs = cwd_.substr(0, 2) + p;
  } else {
    abs = cwd_;
    if (abs.back() != '\\') abs += '\\';
    abs += p;
  }
  abs[0] = char(toupper(uint8_t(abs[0])));
  // Resolve "." and ".." component by component.
  std::vector<std::string> parts;
  size_t i = 3;
  while (i <= abs.size()) {
    size_t j = abs.find('\\', i);
    if (j == std::string::npos) j = abs.size();
    std::string part = abs.substr(i, j - i);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    i = j + 1;
  }
  std::string out = abs.substr(0, 3);
  for (size_t k = 0; k < parts.size(); k++) {
    if (k) out += '\\';
    out += parts[k];
  }
  return out;
}

Resolved Vfs::resolve(std::string_view guest) const {
  Resolved r;
  r.full = full_path(guest);
  for (const auto& m : mounts_) {
    if (!iprefix(r.full, m->guest)) continue;
    r.m = m.get();
    if (r.full.size() > m->guest.size()) r.rel = r.full.substr(m->guest.size() + (m->guest.back() == '\\' ? 0 : 1));
    break;
  }
  return r;
}

// Drive roots, and every directory on the way to a mount point or a virtual file.
bool Vfs::synthetic_dir(const std::string& full) const {
  if (is_drive_root(full) && full[0] == 'C') return true;
  for (const auto& m : mounts_)
    if (m->guest.size() > full.size() && iprefix(m->guest, full)) return true;
  for (const auto& [k, v] : virtual_files_)
    if (k.size() > full.size() && iprefix(k, full)) return true;
  return false;
}

std::string Vfs::drive_host_path(const Mount& m, const std::string& rel) const {
  (void)m;
  if (rel.empty()) return {};
  size_t s = rel.find('\\');
  std::string letter = s == std::string::npos ? rel : rel.substr(0, s);
  if (letter.size() != 1 || !isalpha(uint8_t(letter[0]))) return {};
  std::string cur = std::string(1, char(toupper(uint8_t(letter[0])))) + ":\\";
  if (s == std::string::npos) return cur;
  std::string rest = rel.substr(s + 1);
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (host_attrs(join(cur, rest), &fa)) return join(cur, rest);
  // Component by component: an 8.3 alias we synthesized (the volume keeps no
  // short names) resolves through the directory's alias table.
  size_t i = 0;
  while (i <= rest.size()) {
    size_t j = rest.find('\\', i);
    if (j == std::string::npos) j = rest.size();
    std::string comp = rest.substr(i, j - i);
    i = j + 1;
    if (comp.empty()) continue;
    std::string cand = join(cur, comp);
    if (host_attrs(cand, &fa)) {
      cur = cand;
      continue;
    }
    bool found = false;
    for (const auto& [name, alias] : host_aliases(cur)) {
      if (ieq(alias, comp) || ieq(name, comp)) {
        cur = join(cur, name);
        found = true;
        break;
      }
    }
    if (!found) {
      cur = cand;
      if (i <= rest.size()) cur = join(cur, rest.substr(i));
      return cur;
    }
  }
  return cur;
}

std::string Vfs::short_host_path(const std::string& host) const {
  // "D:\Photos\Cat.jpg" → "D\PHOTOS\CAT.JPG" with every component 8.3.
  std::string out = std::string(1, char(toupper(uint8_t(host[0]))));
  std::string cur = out + ":\\";
  std::string rest = host.size() > 3 ? host.substr(3) : std::string();
  size_t i = 0;
  while (i < rest.size()) {
    size_t j = rest.find('\\', i);
    if (j == std::string::npos) j = rest.size();
    std::string comp = rest.substr(i, j - i);
    i = j + 1;
    if (comp.empty()) continue;
    std::string alias;
    if (is_short_name(comp)) {
      alias = upper(comp);
    } else {
      for (const auto& [name, a] : host_aliases(cur)) {
        if (ieq(name, comp)) {
          alias = a;
          break;
        }
      }
      if (alias.empty()) alias = make_short_alias(comp, {});
    }
    out += "\\" + alias;
    cur = join(cur, comp);
  }
  return out;
}

std::string Vfs::to_host(std::string_view guest, bool* writable) const {
  Resolved r = resolve(guest);
  if (writable) *writable = false;
  if (!r.m) return {};
  const Mount& m = *r.m;
  switch (m.kind) {
    case Mount::Kind::plain:
      if (writable) *writable = m.writable;
      return join(m.host, r.rel);
    case Mount::Kind::drives:
      return drive_host_path(m, r.rel);
    case Mount::Kind::overlay: {
      if (writable) *writable = true;
      WIN32_FILE_ATTRIBUTE_DATA fa;
      if (!m.upper.empty() && host_attrs(join(m.upper, r.rel), &fa)) return join(m.upper, r.rel);
      // A memory-upper file or a directory made there has no host path.
      if (m.upper.empty() && m.mem.count(upper(r.rel))) return {};
      if (virtual_files_.count(upper(r.full))) return {};
      if (!m.host.empty() && host_attrs(join(m.host, r.rel), &fa)) return join(m.host, r.rel);
      if (!m.upper.empty()) return join(m.upper, r.rel);
      return m.host.empty() ? std::string() : join(m.host, r.rel);
    }
  }
  return {};
}

std::string Vfs::to_guest(std::string_view host) const {
  std::string h = slashes(std::string(host));
  const Mount* best = nullptr;
  size_t best_len = 0;
  std::string best_rest;
  auto consider = [&](const Mount& m, const std::string& dir) {
    if (dir.empty() || !iprefix(h, dir) || dir.size() < best_len) return;
    best = &m;
    best_len = dir.size();
    best_rest = h.size() > dir.size() ? h.substr(dir.size() + (dir.back() == '\\' ? 0 : 1)) : std::string();
  };
  for (const auto& m : mounts_) {
    if (m->kind == Mount::Kind::drives) continue;
    consider(*m, m->host);
    if (m->kind == Mount::Kind::overlay) consider(*m, m->upper);
  }
  if (!best) return {};
  return join(best->guest, best_rest);
}

std::string Vfs::host_to_guest(std::string_view host_path) const {
  std::wstring w = widen(std::string(host_path));
  wchar_t full[MAX_PATH * 4];
  DWORD n = GetFullPathNameW(w.c_str(), DWORD(std::size(full)), full, nullptr);
  std::string h = n && n < std::size(full) ? narrow(std::wstring(full, n)) : std::string(host_path);
  h = strip_trailing(h);
  std::string g = to_guest(h);
  if (!g.empty()) return g;
  const Mount* drives = nullptr;
  for (const auto& m : mounts_)
    if (m->kind == Mount::Kind::drives) drives = m.get();
  if (!drives || h.size() < 3 || h[1] != ':' || h[2] != '\\' || !isalpha(uint8_t(h[0]))) return {};
  if (drives->short_names) return "H:\\" + short_host_path(h);
  std::string rest = h.size() > 3 ? h.substr(3) : std::string();
  return "H:\\" + std::string(1, char(toupper(uint8_t(h[0])))) + (rest.empty() ? "" : "\\" + upper(rest));
}

bool Vfs::set_cwd(std::string_view guest) {
  std::string g = full_path(guest);
  Resolved r = resolve(g);
  Stat st;
  bool ok = r.m ? !(stat_resolved(r, &st) && !st.dir) : synthetic_dir(g);
  if (ok) cwd_ = g;
  return ok;
}

// ---- Vfs: metadata ------------------------------------------------------------------------------------

bool Vfs::stat_resolved(const Resolved& r, Stat* out) const {
  *out = Stat{};
  WIN32_FILE_ATTRIBUTE_DATA fa;
  auto from_host = [&](const std::string& host, Layer layer, bool writable) {
    if (!host_attrs(host, &fa)) return false;
    out->exists = true;
    out->dir = fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    out->layer = layer;
    out->writable = writable;
    out->attributes = fa.dwFileAttributes;
    out->size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    out->write_time = ft64(fa.ftLastWriteTime);
    out->host = host;
    return true;
  };
  auto as_dir = [&](Layer layer, bool writable) {
    out->exists = out->dir = true;
    out->layer = layer;
    out->writable = writable;
    out->attributes = FILE_ATTRIBUTE_DIRECTORY;
    return true;
  };
  std::string ukey = upper(r.full);
  auto vf = virtual_files_.find(ukey);
  if (!r.m) {
    if (vf != virtual_files_.end()) {
      out->exists = true;
      out->layer = Layer::virtual_file;
      out->attributes = FILE_ATTRIBUTE_ARCHIVE;
      out->size = vf->second->size();
      out->write_time = kMemEpoch;
      return true;
    }
    return synthetic_dir(r.full) ? as_dir(Layer::synthetic_dir, false) : false;
  }
  const Mount& m = *r.m;
  switch (m.kind) {
    case Mount::Kind::plain:
      if (r.rel.empty() && !host_attrs(m.host, &fa)) return as_dir(Layer::synthetic_dir, m.writable);
      return from_host(join(m.host, r.rel), Layer::plain, m.writable);
    case Mount::Kind::drives:
      if (r.rel.empty()) return as_dir(Layer::synthetic_dir, false);
      return from_host(drive_host_path(m, r.rel), Layer::drive, false);
    case Mount::Kind::overlay: {
      std::string rkey = upper(r.rel);
      if (m.upper.empty()) {
        auto it = m.mem.find(rkey);
        if (it != m.mem.end() && !r.rel.empty()) {
          out->exists = true;
          out->writable = true;
          out->layer = Layer::upper_memory;
          out->dir = it->second.dir;
          if (it->second.dir) {
            out->attributes = FILE_ATTRIBUTE_DIRECTORY;
          } else {
            out->attributes = FILE_ATTRIBUTE_ARCHIVE;
            out->size = it->second.buf->data.size();
            out->write_time = it->second.buf->write_time;
            out->version = it->second.buf->version;
          }
          return true;
        }
      } else if (!r.rel.empty()) {
        // A handle open for writing in this process holds the newest bytes
        // (a copy-up or a new file may not be committed yet).
        std::string uh = join(m.upper, r.rel);
        auto ob = open_buffers_.find(upper(uh));
        if (ob != open_buffers_.end()) {
          if (auto b = ob->second.lock()) {
            if (!from_host(uh, Layer::upper_host, true)) {
              out->exists = true;
              out->writable = true;
              out->layer = Layer::upper_host;
              out->attributes = FILE_ATTRIBUTE_ARCHIVE;
              out->host = uh;
            }
            out->size = b->data.size();
            return true;
          }
        }
        if (from_host(uh, Layer::upper_host, true)) return true;
      }
      if (vf != virtual_files_.end()) {
        out->exists = true;
        out->writable = true;
        out->layer = Layer::virtual_file;
        out->attributes = FILE_ATTRIBUTE_ARCHIVE;
        out->size = vf->second->size();
        out->write_time = kMemEpoch;
        return true;
      }
      if (!m.host.empty() && from_host(join(m.host, r.rel), Layer::lower, true)) return true;
      if (r.rel.empty() || synthetic_dir(r.full)) return as_dir(Layer::synthetic_dir, true);
      return false;
    }
  }
  return false;
}

bool Vfs::stat(std::string_view guest, Stat* out) const {
  Stat dummy;
  if (!out) out = &dummy;
  return stat_resolved(resolve(guest), out);
}

bool Vfs::exists(std::string_view guest) const {
  Stat st;
  return stat(guest, &st);
}

bool Vfs::is_dir(std::string_view guest) const {
  Stat st;
  return stat(guest, &st) && st.dir;
}

bool Vfs::parent_exists(const Resolved& r) const {
  std::string p = parent_of(r.full);
  if (p.empty()) return true;
  Stat st;
  return stat(p, &st) && st.dir;
}

// The lower layer (a virtual file or the lower directory) has this path.
bool Vfs::lower_has(const Resolved& r) const {
  if (virtual_files_.count(upper(r.full))) return true;
  if (!r.m || r.m->kind != Mount::Kind::overlay) return true;
  if (r.rel.empty() || synthetic_dir(r.full)) return true;
  WIN32_FILE_ATTRIBUTE_DATA fa;
  return !r.m->host.empty() && host_attrs(join(r.m->host, r.rel), &fa);
}

bool Vfs::read_resolved(const Resolved& r, const Stat& st, std::string* out) const {
  out->clear();
  if (!st.exists || st.dir) return false;
  switch (st.layer) {
    case Layer::upper_memory: {
      const MemNode& n = r.m->mem.at(upper(r.rel));
      out->assign(n.buf->data.begin(), n.buf->data.end());
      return true;
    }
    case Layer::virtual_file: {
      auto it = virtual_files_.find(upper(r.full));
      if (it == virtual_files_.end()) return false;
      out->assign(it->second->begin(), it->second->end());
      return true;
    }
    case Layer::upper_host: {
      auto ob = open_buffers_.find(upper(st.host));
      if (ob != open_buffers_.end()) {
        if (auto b = ob->second.lock()) {
          out->assign(b->data.begin(), b->data.end());
          return true;
        }
      }
      return read_host_file(st.host, out);
    }
    default:
      return read_host_file(st.host, out);
  }
}

bool Vfs::read_file(std::string_view guest, std::string* out) const {
  Resolved r = resolve(guest);
  Stat st;
  if (!stat_resolved(r, &st)) {
    out->clear();
    return false;
  }
  return read_resolved(r, st, out);
}

// ---- Vfs: writes ----------------------------------------------------------------------------------------

void Vfs::note_written(const std::string& guest_full) {
  if (std::find(written_.begin(), written_.end(), guest_full) == written_.end()) written_.push_back(guest_full);
}

uint64_t Vfs::next_mem_time() {
  mem_time_ += 10'000'000;  // one second per write
  return mem_time_;
}

bool Vfs::ensure_host_dirs(const Mount& m, const std::string& rel_dir, uint32_t* err) {
  // The upper root and its parents, then rel_dir below it.
  std::string path = join(m.upper, rel_dir);
  std::string cur;
  size_t i = 0;
  if (path.size() >= 3 && path[1] == ':') {
    cur = path.substr(0, 3);
    i = 3;
  } else if (path.rfind("\\\\", 0) == 0) {
    // \\server\share\ — start after the share.
    size_t a = path.find('\\', 2), b = a == std::string::npos ? a : path.find('\\', a + 1);
    if (b == std::string::npos) return true;
    cur = path.substr(0, b + 1);
    i = b + 1;
  }
  while (i <= path.size()) {
    size_t j = path.find('\\', i);
    if (j == std::string::npos) j = path.size();
    std::string comp = path.substr(i, j - i);
    i = j + 1;
    if (comp.empty()) continue;
    cur = join(cur, comp);
    if (!CreateDirectoryW(widen(cur).c_str(), nullptr)) {
      DWORD e = GetLastError();
      if (e != ERROR_ALREADY_EXISTS) {
        if (err) *err = e;
        log("state: cannot create directory %s (error %lu)", cur.c_str(), e);
        return false;
      }
    }
  }
  return true;
}

bool Vfs::commit_host(const Mount& m, const std::string& rel, const std::string& guest_full,
                      const std::string& bytes, uint32_t* err) {
  StateLock lock(*this);
  uint32_t dummy = 0;
  if (!err) err = &dummy;
  std::string target = join(m.upper, rel);
  std::string rel_dir = parent_of("C:\\" + rel);
  rel_dir = rel_dir.size() > 3 ? rel_dir.substr(3) : std::string();
  if (!ensure_host_dirs(m, rel_dir, err)) return false;
  char suffix[64];
  snprintf(suffix, sizeof(suffix), ".~%lu.%u.tmp", GetCurrentProcessId(), ++temp_counter_);
  std::wstring tmp = widen(target + suffix), dst = widen(target);
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = GetLastError();
    log("state: cannot write %s (error %u)", target.c_str(), *err);
    return false;
  }
  size_t done = 0;
  bool ok = true;
  while (done < bytes.size()) {
    DWORD put = 0;
    DWORD chunk = DWORD(std::min<size_t>(bytes.size() - done, 1u << 20));
    if (!WriteFile(h, bytes.data() + done, chunk, &put, nullptr) || !put) {
      ok = false;
      break;
    }
    done += put;
  }
  ok = ok && FlushFileBuffers(h);
  DWORD e = ok ? 0 : GetLastError();
  CloseHandle(h);
  if (ok) {
    // Another program (a virus scanner, an indexer) may hold the target for
    // a moment: retry briefly. Our own readers share delete, so they never block this.
    for (int attempt = 0; attempt < 50; attempt++) {
      if (MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        e = 0;
        break;
      }
      e = GetLastError();
      if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) break;
      Sleep(20);
    }
    ok = e == 0;
  }
  if (!ok) {
    DeleteFileW(tmp.c_str());
    *err = e;
    log("state: cannot replace %s (error %lu)", target.c_str(), e);
    return false;
  }
  trace("file", "state: wrote %s (%zu bytes) -> %s", guest_full.c_str(), bytes.size(), target.c_str());
  note_written(guest_full);
  return true;
}

std::unique_ptr<VfsFile> Vfs::open_read(const Resolved& r, const Stat& st, uint32_t* err) const {
  switch (st.layer) {
    case Layer::upper_memory:
      return std::make_unique<MemFile>(r.m->mem.at(upper(r.rel)).buf, false, r.full, nullptr);
    case Layer::virtual_file: {
      auto b = std::make_shared<FileBuffer>();
      auto it = virtual_files_.find(upper(r.full));
      if (it != virtual_files_.end()) b->data.assign(it->second->begin(), it->second->end());
      b->write_time = kMemEpoch;
      return std::make_unique<MemFile>(b, false, r.full, nullptr);
    }
    case Layer::upper_host: {
      auto ob = open_buffers_.find(upper(st.host));
      if (ob != open_buffers_.end()) {
        if (auto b = ob->second.lock()) return std::make_unique<MemFile>(b, false, r.full, nullptr);
      }
      break;
    }
    default:
      break;
  }
  HANDLE h = CreateFileW(widen(st.host).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = GetLastError();
    return nullptr;
  }
  return std::make_unique<HostFile>(h, false, r.full);
}

std::unique_ptr<VfsFile> Vfs::open(std::string_view guest, Access access, Disposition disp, uint32_t* win32_error) {
  uint32_t dummy = 0;
  uint32_t& err = win32_error ? *win32_error : dummy;
  err = 0;
  Resolved r = resolve(guest);
  Stat st;
  bool exists = stat_resolved(r, &st);
  bool truncate = disp == Disposition::create_always || disp == Disposition::truncate_existing;
  bool want_write = access != Access::read || truncate || disp == Disposition::create_new ||
                    (disp == Disposition::open_always && !exists);
  if (exists && st.dir) {
    err = ERROR_ACCESS_DENIED;
    return nullptr;
  }
  if (!exists) {
    if (!r.m) {
      err = ERROR_PATH_NOT_FOUND;
      return nullptr;
    }
    if (!parent_exists(r)) {
      err = ERROR_PATH_NOT_FOUND;
      return nullptr;
    }
    if (disp == Disposition::open_existing || disp == Disposition::truncate_existing) {
      err = ERROR_FILE_NOT_FOUND;
      return nullptr;
    }
  } else if (disp == Disposition::create_new) {
    err = ERROR_FILE_EXISTS;
    return nullptr;
  }
  uint32_t ok_code = exists && (disp == Disposition::create_always || disp == Disposition::open_always)
                         ? uint32_t(ERROR_ALREADY_EXISTS)
                         : 0u;

  if (!want_write) {
    auto f = open_read(r, st, &err);
    if (f) err = ok_code;
    return f;
  }
  if (!r.m) {
    err = ERROR_ACCESS_DENIED;
    return nullptr;
  }
  Mount& m = const_cast<Mount&>(*r.m);
  switch (m.kind) {
    case Mount::Kind::drives:
      err = ERROR_ACCESS_DENIED;
      return nullptr;
    case Mount::Kind::plain: {
      if (!m.writable) {
        err = ERROR_ACCESS_DENIED;
        return nullptr;
      }
      DWORD cd = disp == Disposition::create_always ? CREATE_ALWAYS
                 : disp == Disposition::create_new ? CREATE_NEW
                 : disp == Disposition::open_always ? OPEN_ALWAYS
                 : disp == Disposition::truncate_existing ? TRUNCATE_EXISTING
                                                          : OPEN_EXISTING;
      HANDLE h = CreateFileW(widen(join(m.host, r.rel)).c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, cd, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return nullptr;
      }
      note_written(r.full);
      err = ok_code;
      return std::make_unique<HostFile>(h, true, r.full);
    }
    case Mount::Kind::overlay:
      break;
  }
  const std::string full = r.full;
  if (m.upper.empty()) {
    // Memory upper: the node holds the bytes; opening for writing copies up.
    std::string key = upper(r.rel);
    auto it = m.mem.find(key);
    bool dirty = false;
    if (it == m.mem.end()) {
      MemNode n;
      n.name = name_of(full);
      n.buf = std::make_shared<FileBuffer>();
      n.buf->write_time = st.write_time ? st.write_time : mem_time_;
      if (exists && !truncate) {
        std::string bytes;
        read_resolved(r, st, &bytes);
        n.buf->data.assign(bytes.begin(), bytes.end());
      }
      it = m.mem.emplace(key, std::move(n)).first;
      dirty = true;
    }
    if (truncate) {
      it->second.buf->data.clear();
      dirty = true;
    }
    if (dirty) it->second.buf->dirty = true;
    err = ok_code;
    return std::make_unique<MemFile>(it->second.buf, true, full, [this, full](FileBuffer& b) {
      b.write_time = next_mem_time();
      b.version++;
      note_written(full);
      trace("file", "state (memory): wrote %s (%zu bytes)", full.c_str(), b.data.size());
      return true;
    });
  }
  // Persistent upper: the bytes live in memory while open, committed atomically.
  std::string host = join(m.upper, r.rel);
  std::string hkey = upper(host);
  std::shared_ptr<FileBuffer> b = open_buffers_[hkey].lock();
  if (!b) {
    b = std::make_shared<FileBuffer>();
    b->write_time = st.write_time;
    if (exists && !truncate) {
      std::string bytes;
      if (!read_resolved(r, st, &bytes)) {
        err = GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED;
        return nullptr;
      }
      b->data.assign(bytes.begin(), bytes.end());
    }
    open_buffers_[hkey] = b;
  }
  if (truncate) b->data.clear();
  // A copy-up, a new file or a truncation must reach the upper layer even if
  // nothing is written.
  if (truncate || !exists || st.layer != Layer::upper_host) b->dirty = true;
  err = ok_code;
  std::string rel = r.rel;
  const Mount* mp = &m;
  return std::make_unique<MemFile>(b, true, full, [this, mp, rel, full](FileBuffer& fb) {
    uint32_t e = 0;
    return commit_host(*mp, rel, full, std::string(fb.data.begin(), fb.data.end()), &e);
  });
}

bool Vfs::write_file(std::string_view guest, std::string_view bytes, uint32_t* win32_error) {
  auto f = open(guest, Access::write, Disposition::create_always, win32_error);
  if (!f) return false;
  if (!bytes.empty() && f->write(bytes.data(), uint32_t(bytes.size())) != int64_t(bytes.size())) {
    if (win32_error) *win32_error = ERROR_WRITE_FAULT;
    return false;
  }
  bool ok = f->flush();
  if (!ok && win32_error && !*win32_error) *win32_error = ERROR_WRITE_FAULT;
  if (ok && win32_error) *win32_error = 0;
  return ok;
}

bool Vfs::remove(std::string_view guest, uint32_t* win32_error) {
  uint32_t dummy = 0;
  uint32_t& err = win32_error ? *win32_error : dummy;
  err = 0;
  Resolved r = resolve(guest);
  Stat st;
  if (!stat_resolved(r, &st)) {
    err = parent_exists(r) && r.m ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
    return false;
  }
  if (st.dir || !r.m) {
    err = ERROR_ACCESS_DENIED;
    return false;
  }
  Mount& m = const_cast<Mount&>(*r.m);
  if (m.kind == Mount::Kind::plain && m.writable) {
    if (!DeleteFileW(widen(join(m.host, r.rel)).c_str())) {
      err = GetLastError();
      return false;
    }
    return true;
  }
  if (m.kind != Mount::Kind::overlay || lower_has(r)) {
    trace("file", "delete %s refused: the file belongs to a read-only layer", r.full.c_str());
    err = ERROR_ACCESS_DENIED;
    return false;
  }
  if (m.upper.empty()) {
    auto it = m.mem.find(upper(r.rel));
    if (it != m.mem.end() && it->second.buf.use_count() > 1) {
      err = ERROR_SHARING_VIOLATION;
      return false;
    }
    m.mem.erase(upper(r.rel));
    return true;
  }
  std::string host = join(m.upper, r.rel);
  auto ob = open_buffers_.find(upper(host));
  if (ob != open_buffers_.end() && !ob->second.expired()) {
    err = ERROR_SHARING_VIOLATION;
    return false;
  }
  StateLock lock(*this);
  if (!DeleteFileW(widen(host).c_str())) {
    err = GetLastError();
    return false;
  }
  return true;
}

bool Vfs::rename(std::string_view from, std::string_view to, uint32_t* win32_error) {
  uint32_t dummy = 0;
  uint32_t& err = win32_error ? *win32_error : dummy;
  err = 0;
  Resolved rf = resolve(from), rt = resolve(to);
  Stat sf, stt;
  if (!stat_resolved(rf, &sf)) {
    err = parent_exists(rf) && rf.m ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
    return false;
  }
  if (stat_resolved(rt, &stt)) {
    err = ERROR_ALREADY_EXISTS;
    return false;
  }
  if (!parent_exists(rt) || !rt.m) {
    err = ERROR_PATH_NOT_FOUND;
    return false;
  }
  if (rf.m != rt.m) {
    err = ERROR_NOT_SAME_DEVICE;
    return false;
  }
  Mount& m = const_cast<Mount&>(*rf.m);
  if (m.kind == Mount::Kind::plain && m.writable) {
    if (!MoveFileExW(widen(join(m.host, rf.rel)).c_str(), widen(join(m.host, rt.rel)).c_str(), 0)) {
      err = GetLastError();
      return false;
    }
    note_written(rt.full);
    return true;
  }
  if (m.kind != Mount::Kind::overlay || lower_has(rf)) {
    trace("file", "rename %s refused: it belongs to a read-only layer", rf.full.c_str());
    err = ERROR_ACCESS_DENIED;
    return false;
  }
  if (m.upper.empty()) {
    std::string fk = upper(rf.rel), tk = upper(rt.rel);
    auto it = m.mem.find(fk);
    if (it == m.mem.end()) {
      err = ERROR_FILE_NOT_FOUND;
      return false;
    }
    if (!it->second.dir && it->second.buf.use_count() > 1) {
      err = ERROR_SHARING_VIOLATION;
      return false;
    }
    std::vector<std::pair<std::string, MemNode>> moved;
    for (auto jt = m.mem.begin(); jt != m.mem.end();) {
      if (jt->first == fk || jt->first.rfind(fk + "\\", 0) == 0) {
        moved.push_back({tk + jt->first.substr(fk.size()), std::move(jt->second)});
        jt = m.mem.erase(jt);
      } else {
        ++jt;
      }
    }
    for (auto& [k, n] : moved) {
      if (k == tk) n.name = name_of(rt.full);
      m.mem.emplace(k, std::move(n));
    }
    note_written(rt.full);
    return true;
  }
  std::string hf = join(m.upper, rf.rel), ht = join(m.upper, rt.rel);
  auto ob = open_buffers_.find(upper(hf));
  if (ob != open_buffers_.end() && !ob->second.expired()) {
    err = ERROR_SHARING_VIOLATION;
    return false;
  }
  StateLock lock(*this);
  std::string rel_dir = parent_of("C:\\" + rt.rel);
  rel_dir = rel_dir.size() > 3 ? rel_dir.substr(3) : std::string();
  if (!ensure_host_dirs(m, rel_dir, &err)) return false;
  if (!MoveFileExW(widen(hf).c_str(), widen(ht).c_str(), MOVEFILE_WRITE_THROUGH)) {
    err = GetLastError();
    return false;
  }
  note_written(rt.full);
  return true;
}

bool Vfs::make_dir(std::string_view guest, uint32_t* win32_error) {
  uint32_t dummy = 0;
  uint32_t& err = win32_error ? *win32_error : dummy;
  err = 0;
  Resolved r = resolve(guest);
  Stat st;
  if (stat_resolved(r, &st)) {
    err = ERROR_ALREADY_EXISTS;
    return false;
  }
  if (!r.m || !parent_exists(r)) {
    err = ERROR_PATH_NOT_FOUND;
    return false;
  }
  Mount& m = const_cast<Mount&>(*r.m);
  if (m.kind == Mount::Kind::plain && m.writable) {
    if (!CreateDirectoryW(widen(join(m.host, r.rel)).c_str(), nullptr)) {
      err = GetLastError();
      return false;
    }
    return true;
  }
  if (m.kind != Mount::Kind::overlay) {
    err = ERROR_ACCESS_DENIED;
    return false;
  }
  if (m.upper.empty()) {
    MemNode n;
    n.name = name_of(r.full);
    n.dir = true;
    m.mem.emplace(upper(r.rel), std::move(n));
    return true;
  }
  StateLock lock(*this);
  return ensure_host_dirs(m, r.rel, &err);
}

bool Vfs::remove_dir(std::string_view guest, uint32_t* win32_error) {
  uint32_t dummy = 0;
  uint32_t& err = win32_error ? *win32_error : dummy;
  err = 0;
  Resolved r = resolve(guest);
  Stat st;
  if (!stat_resolved(r, &st)) {
    err = ERROR_PATH_NOT_FOUND;
    return false;
  }
  if (!st.dir) {
    err = ERROR_DIRECTORY;
    return false;
  }
  Mount* m = const_cast<Mount*>(r.m);
  if (m && m->kind == Mount::Kind::plain && m->writable && !r.rel.empty()) {
    if (!RemoveDirectoryW(widen(join(m->host, r.rel)).c_str())) {
      err = GetLastError();
      return false;
    }
    return true;
  }
  if (!m || m->kind != Mount::Kind::overlay || lower_has(r)) {
    err = ERROR_ACCESS_DENIED;
    return false;
  }
  for (const DirEntry& e : list(r.full, "*")) {
    if (e.name != "." && e.name != "..") {
      err = ERROR_DIR_NOT_EMPTY;
      return false;
    }
  }
  if (m->upper.empty()) {
    m->mem.erase(upper(r.rel));
    return true;
  }
  StateLock lock(*this);
  if (!RemoveDirectoryW(widen(join(m->upper, r.rel)).c_str())) {
    err = GetLastError();
    return false;
  }
  return true;
}

// ---- Vfs: listings ----------------------------------------------------------------------------------------

std::vector<Vfs::DirEntry> Vfs::list(std::string_view guest_dir, std::string_view pattern) const {
  std::vector<DirEntry> out;
  Resolved r = resolve(guest_dir);
  Stat st;
  if (!stat_resolved(r, &st) || !st.dir) return out;
  std::map<std::string, DirEntry> by;  // upper-cased name → entry
  auto add = [&](DirEntry e, bool override) {
    std::string k = upper(e.name);
    if (override || !by.count(k)) by[k] = std::move(e);
  };
  HostEntry dot, dotdot;
  bool have_dot = false;
  bool virtual_added = false;
  auto add_virtual = [&]() {
    if (virtual_added) return;
    virtual_added = true;
    std::string prefix = upper(r.full);
    for (const auto& [k, v] : virtual_files_) {
      if (parent_of(k) != prefix) continue;
      auto nm = virtual_names_.find(k);
      add(DirEntry{nm == virtual_names_.end() ? name_of(k) : nm->second, "", FILE_ATTRIBUTE_ARCHIVE, v->size(),
                   kMemEpoch},
          true);
    }
  };
  auto from_host = [&](const std::string& dir, bool override, bool dots) {
    HostEntry d1, d2;
    for (HostEntry& h : host_dir(dir, &d1, &d2))
      add(DirEntry{h.name, h.alt, h.attributes, h.size, h.write_time}, override);
    if (dots && !d1.name.empty()) {
      dot = d1;
      dotdot = d2;
      have_dot = true;
    }
  };
  if (r.m) {
    const Mount& m = *r.m;
    switch (m.kind) {
      case Mount::Kind::plain:
        from_host(join(m.host, r.rel), false, true);
        break;
      case Mount::Kind::drives:
        if (r.rel.empty()) {
          DWORD drives = GetLogicalDrives();
          for (int i = 0; i < 26; i++)
            if (drives & (1u << i)) add(DirEntry{std::string(1, char('A' + i)), "", FILE_ATTRIBUTE_DIRECTORY, 0, 0}, false);
        } else {
          from_host(drive_host_path(m, r.rel), false, true);
        }
        break;
      case Mount::Kind::overlay: {
        if (!m.host.empty()) from_host(join(m.host, r.rel), false, true);
        add_virtual();
        if (m.upper.empty()) {
          std::string rk = upper(r.rel);
          for (const auto& [k, n] : m.mem) {
            size_t s = k.find_last_of('\\');
            std::string parent = s == std::string::npos ? std::string() : k.substr(0, s);
            if (parent != rk) continue;
            if (n.dir) add(DirEntry{n.name, "", FILE_ATTRIBUTE_DIRECTORY, 0, 0}, true);
            else add(DirEntry{n.name, "", FILE_ATTRIBUTE_ARCHIVE, n.buf->data.size(), n.buf->write_time}, true);
          }
        } else {
          from_host(join(m.upper, r.rel), true, false);
          // Files created here but not committed yet.
          std::string hdir = upper(join(m.upper, r.rel));
          for (const auto& [k, w] : open_buffers_) {
            auto b = w.lock();
            if (!b || parent_of(k) != hdir) continue;
            std::string n = name_of(k);
            if (!by.count(n)) add(DirEntry{n, "", FILE_ATTRIBUTE_ARCHIVE, b->data.size(), b->write_time}, false);
          }
        }
        break;
      }
    }
  }
  // Virtual files, mount points and the directories leading to virtual files.
  if (!r.m || r.m->kind != Mount::Kind::overlay) add_virtual();
  for (const auto& m : mounts_) {
    if (m->listed && ieq(parent_of(m->guest), r.full))
      add(DirEntry{name_of(m->guest), "", FILE_ATTRIBUTE_DIRECTORY, 0, 0}, true);
  }
  for (const auto& [k, v] : virtual_files_) {
    std::string p = parent_of(k);
    while (!p.empty() && p.size() > r.full.size()) {
      if (ieq(parent_of(p), r.full)) {
        add(DirEntry{name_of(p), "", FILE_ATTRIBUTE_DIRECTORY, 0, 0}, false);
        break;
      }
      p = parent_of(p);
    }
  }
  // 8.3 aliases for the names that need one, deterministically.
  std::vector<std::string> taken;
  for (auto& [k, e] : by) {
    if (e.short_name.empty() && is_short_name(e.name)) e.short_name = k;
    if (!e.short_name.empty()) taken.push_back(e.short_name);
    taken.push_back(k);
  }
  for (auto& [k, e] : by) {
    if (!e.short_name.empty()) continue;
    e.short_name = make_short_alias(e.name, taken);
    taken.push_back(e.short_name);
  }
  std::string pat(pattern.empty() ? std::string_view("*") : pattern);
  auto match = [&](const DirEntry& e) { return wild_match(pat, e.name) || wild_match(pat, e.short_name); };
  if (!is_drive_root(r.full)) {
    DirEntry d1{".", ".", FILE_ATTRIBUTE_DIRECTORY, 0, 0}, d2{"..", "..", FILE_ATTRIBUTE_DIRECTORY, 0, 0};
    if (have_dot) {
      d1 = DirEntry{".", ".", dot.attributes, 0, dot.write_time};
      d2 = DirEntry{"..", "..", dotdot.attributes ? dotdot.attributes : uint32_t(FILE_ATTRIBUTE_DIRECTORY), 0,
                    dotdot.write_time};
    }
    if (match(d1)) out.push_back(d1);
    if (match(d2)) out.push_back(d2);
  }
  for (auto& [k, e] : by)
    if (match(e)) out.push_back(e);
  return out;
}

// ---- 8.3 and wildcards ------------------------------------------------------------------------------------

bool Vfs::is_short_name(std::string_view name) {
  if (name == "." || name == "..") return true;
  if (name.empty()) return false;
  size_t dot = name.find('.');
  std::string_view base = name.substr(0, dot), ext = dot == std::string_view::npos ? std::string_view() : name.substr(dot + 1);
  if (base.empty() || base.size() > 8 || ext.size() > 3) return false;
  if (dot != std::string_view::npos && ext.empty()) return false;
  if (ext.find('.') != std::string_view::npos) return false;
  for (char c : base)
    if (!valid_83_char(uint8_t(c))) return false;
  for (char c : ext)
    if (!valid_83_char(uint8_t(c))) return false;
  return true;
}

std::string Vfs::make_short_alias(std::string_view name, const std::vector<std::string>& taken) {
  std::string n = upper(name);
  while (!n.empty() && n[0] == '.') n.erase(0, 1);
  size_t dot = n.find_last_of('.');
  std::string base = dot == std::string::npos ? n : n.substr(0, dot);
  std::string ext_raw = dot == std::string::npos ? std::string() : n.substr(dot + 1);
  auto clean = [](const std::string& s, size_t max) {
    std::string o;
    for (char c : s) {
      if (o.size() >= max) break;
      if (c == ' ' || c == '.') continue;
      o.push_back(valid_83_char(uint8_t(c)) ? c : '_');
    }
    return o;
  };
  std::string basis = clean(base, 6), ext = clean(ext_raw, 3);
  if (basis.empty()) basis = "_";
  for (int k = 1; k < 1000000; k++) {
    std::string suffix = "~" + std::to_string(k);
    std::string b = basis.substr(0, std::min(basis.size(), 8 - suffix.size()));
    std::string alias = b + suffix + (ext.empty() ? "" : "." + ext);
    if (std::find(taken.begin(), taken.end(), alias) == taken.end()) return alias;
  }
  return basis;
}

namespace {
bool glob(const char* p, const char* s) {
  // Case-insensitive '*' / '?' match, iterative with one backtrack point.
  const char *star = nullptr, *ss = nullptr;
  while (*s) {
    if (*p == '?' || (*p && *p != '*' && toupper(uint8_t(*p)) == toupper(uint8_t(*s)))) {
      p++;
      s++;
    } else if (*p == '*') {
      star = p++;
      ss = s;
    } else if (star) {
      p = star + 1;
      s = ++ss;
    } else {
      return false;
    }
  }
  while (*p == '*') p++;
  return !*p;
}
}  // namespace

bool Vfs::wild_match(std::string_view pattern, std::string_view name) {
  if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
  std::string p(pattern), n(name);
  if (glob(p.c_str(), n.c_str())) return true;
  // DOS: "NAME.*" also matches "NAME"; "NAME." matches only names with no extension.
  if (p.size() >= 2 && p.compare(p.size() - 2, 2, ".*") == 0 && n.find('.') == std::string::npos)
    return glob(p.substr(0, p.size() - 2).c_str(), n.c_str());
  if (p.size() >= 2 && p.back() == '.' && p != ".." && n.find('.') == std::string::npos)
    return glob(p.substr(0, p.size() - 1).c_str(), n.c_str());
  return false;
}

// ---- the state mutex ---------------------------------------------------------------------------------------

Vfs::StateLock::StateLock(const Vfs& vfs) {
  std::string name = vfs.state_mutex_name();
  if (name.empty()) return;
  if (!vfs.mutex_) vfs.mutex_ = CreateMutexW(nullptr, FALSE, widen(name).c_str());
  mutex_ = vfs.mutex_;
  if (!mutex_) return;
  DWORD w = WaitForSingleObject(static_cast<HANDLE>(mutex_), 30000);
  held_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
  if (!held_) log("state: the state mutex %s is still held after 30 s; going on without it", name.c_str());
}

Vfs::StateLock::~StateLock() {
  if (held_) ReleaseMutex(static_cast<HANDLE>(mutex_));
}

}  // namespace adw::win32
