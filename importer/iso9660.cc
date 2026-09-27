#include "iso9660.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

#include "winutil.h"

namespace adw::import {

namespace {

constexpr uint32_t kSector = 2048;        // ISO-9660 logical sector (user data per CD sector)
constexpr size_t kWindow = 64 * 1024;     // read-cache window for directory/descriptor reads
constexpr size_t kChunk = 1 << 20;        // file-content streaming chunk
constexpr uint64_t kMaxDirBytes = 16u << 20;  // no real directory comes close; guards garbage sizes

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

// ECMA-119 7.3.3 "both-byte orders" field. Compliant images repeat the value
// LE then BE; some early mastering tools filled only one half. When the halves
// disagree, keep the one `ok` accepts (LE first — it is what Windows and Linux
// read); when both are plausible a zero half is the blank one.
template <typename Ok>
uint32_t both32(const uint8_t* p, Ok ok) {
  uint32_t le = le32(p), be = be32(p + 4);
  if (le == be) return le;
  bool le_ok = ok(le), be_ok = ok(be);
  if (le_ok != be_ok) return le_ok ? le : be;
  return le ? le : be;
}

uint16_t both16(const uint8_t* p) {
  uint16_t le = le16(p), be = be16(p + 2);
  return le ? le : be;
}

std::string latin1_to_utf8(const uint8_t* p, size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    if (c < 0x80) {
      s.push_back(char(c));
    } else {
      s.push_back(char(0xC0 | (c >> 6)));
      s.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return s;
}

std::string ucs2be_to_utf8(const uint8_t* p, size_t n) {
  std::wstring w;
  for (size_t i = 0; i + 1 < n; i += 2) w.push_back(wchar_t(p[i] << 8 | p[i + 1]));
  return to_utf8(w);
}

// "TOASTERS.AD;1" -> "TOASTERS.AD", "README.;1" -> "README". Joliet names
// usually carry the version too.
std::string strip_version(std::string s) {
  size_t semi = s.rfind(';');
  if (semi != std::string::npos &&
      std::all_of(s.begin() + semi + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; }))
    s.resize(semi);
  if (s.size() > 1 && s.back() == '.') s.pop_back();
  return s;
}

std::string ascii_upper(std::string s) {
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return s;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = char(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = char(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}

bool same_data(const IsoEntry& a, const IsoEntry& b) {
  if (a.size != b.size) return false;
  if (a.size == 0) return true;  // empty files may be given any extent
  return !a.extents.empty() && !b.extents.empty() && a.extents[0].lba == b.extents[0].lba;
}

}  // namespace

struct IsoImage::Impl {
  Handle file;
  uint64_t fsize = 0;
  uint32_t phys = kSector;  // bytes per sector in the file (2048 cooked, 2352 raw)
  uint32_t data_off = 0;    // user-data offset inside a raw sector (16 MODE1, 24 MODE2/XA)
  uint32_t bs = kSector;    // logical block size from the PVD
  uint64_t logical_size = 0;
  bool joliet = false;
  std::string volid;
  IsoEntry root;

  mutable std::vector<uint8_t> win;
  mutable uint64_t win_off = 0;
  mutable size_t win_len = 0;

  // Sector-aligned reads through an aligned buffer (a raw CD volume).
  bool aligned = false;
  static constexpr size_t kBounce = 1 << 20;
  uint8_t* bounce = nullptr;  // VirtualAlloc'd, kBounce bytes
  ~Impl() {
    if (bounce) VirtualFree(bounce, 0, MEM_RELEASE);
  }

  void pread_direct(uint64_t off, uint8_t* dst, size_t n) const {
    if (!aligned) {
      pread_raw(off, dst, n);
      return;
    }
    while (n) {
      uint64_t start = off & ~uint64_t(kSector - 1);
      size_t skip = size_t(off - start);
      if (start + skip + n > fsize) throw IsoError("unexpected end of image (truncated download?)");
      // Whole sectors, except at the end of a file whose size is not a
      // multiple of them (a raw 2352-byte-sector image; a volume always is).
      size_t len = std::min<size_t>((skip + n + kSector - 1) & ~size_t(kSector - 1), kBounce);
      len = size_t(std::min<uint64_t>(len, fsize - start));
      pread_raw(start, bounce, len);
      size_t take = std::min(n, len - skip);
      memcpy(dst, bounce + skip, take);
      off += take;
      dst += take;
      n -= take;
    }
  }

  void pread_raw(uint64_t off, uint8_t* dst, size_t n) const {
    while (n) {
      OVERLAPPED ov{};
      ov.Offset = DWORD(off);
      ov.OffsetHigh = DWORD(off >> 32);
      DWORD want = n > (1u << 30) ? (1u << 30) : DWORD(n), got = 0;
      if (!ReadFile(file.get(), dst, want, &got, &ov)) {
        DWORD e = GetLastError();
        if (e != ERROR_HANDLE_EOF) throw IsoError("read error: " + win_error_string(e));
        got = 0;
      }
      if (!got) throw IsoError("unexpected end of image (truncated download?)");
      off += got;
      dst += got;
      n -= got;
    }
  }

  // Physical read through the window cache: descriptor and directory reads
  // are small and clustered, file content goes straight through.
  void pread(uint64_t off, uint8_t* dst, size_t n) const {
    if (n > kWindow / 2) {
      pread_direct(off, dst, n);
      return;
    }
    if (off < win_off || off + n > win_off + win_len) {
      uint64_t start = off & ~uint64_t(kSector - 1);
      if (start >= fsize || off + n > fsize) throw IsoError("read past end of image (truncated download?)");
      size_t len = size_t(std::min<uint64_t>(kWindow, fsize - start));
      win.resize(kWindow);
      pread_direct(start, win.data(), len);
      win_off = start;
      win_len = len;
    }
    memcpy(dst, win.data() + (off - win_off), n);
  }

  // Read `n` bytes at a logical (2048-byte-sector) address.
  void read_logical(uint64_t addr, uint8_t* dst, size_t n) const {
    if (addr + n > logical_size) throw IsoError("read past end of image (truncated download?)");
    if (phys == kSector) {
      pread(addr, dst, n);
      return;
    }
    while (n) {
      uint64_t sector = addr / kSector;
      uint32_t within = uint32_t(addr % kSector);
      size_t take = std::min<size_t>(n, kSector - within);
      pread(sector * phys + data_off + within, dst, take);
      addr += take;
      dst += take;
      n -= take;
    }
  }

  bool has_signature(uint32_t p, uint32_t off) const {
    uint64_t at = 16ull * p + off;
    if (at + kSector > fsize) return false;
    uint8_t vd[6];
    pread(at, vd, sizeof(vd));
    return memcmp(vd + 1, "CD001", 5) == 0;
  }

  struct Rec {
    uint32_t lba = 0, size = 0;
    uint8_t flags = 0, unit = 0, gap = 0, ear = 0;
    const uint8_t* name = nullptr;
    uint8_t name_len = 0;
    IsoTime time;
  };

  Rec parse_record(const uint8_t* r) const {
    Rec x;
    x.ear = r[1];
    x.lba = both32(r + 2, [&](uint32_t v) { return uint64_t(v) * bs < logical_size; });
    x.size = both32(r + 10, [&](uint32_t v) {
      return (uint64_t(x.lba) + x.ear) * bs + v <= logical_size;
    });
    const uint8_t* t = r + 18;
    if (t[1] >= 1 && t[1] <= 12 && t[2] >= 1 && t[2] <= 31) {
      x.time.valid = true;
      x.time.year = 1900 + t[0];
      x.time.month = t[1];
      x.time.day = t[2];
      x.time.hour = t[3];
      x.time.minute = t[4];
      x.time.second = t[5];
      x.time.gmt_offset_15min = int8_t(t[6]);
    }
    x.flags = r[25];
    x.unit = r[26];
    x.gap = r[27];
    x.name_len = r[32];
    x.name = r + 33;
    return x;
  }

  IsoEntry entry_from(const Rec& x, bool joliet_names) const {
    IsoEntry e;
    e.is_dir = (x.flags & 0x02) != 0;
    e.name = strip_version(joliet_names ? ucs2be_to_utf8(x.name, x.name_len)
                                        : latin1_to_utf8(x.name, x.name_len));
    e.extents.push_back({x.lba + x.ear, x.size});
    e.size = x.size;
    e.mtime = x.time;
    e.in_joliet = joliet_names;
    return e;
  }

  std::vector<IsoEntry> parse_dir(const IsoEntry& dir, bool joliet_names) const {
    if (!dir.is_dir) throw IsoError("'" + dir.name + "' is not a directory");
    std::vector<IsoEntry> out;
    for (const IsoExtent& ext : dir.extents) {
      if (ext.size > kMaxDirBytes) throw IsoError("implausible directory size in '" + dir.name + "'");
      std::vector<uint8_t> d(ext.size);
      const uint64_t base = uint64_t(ext.lba) * bs;
      read_logical(base, d.data(), d.size());

      IsoEntry pending;
      bool have_pending = false;
      size_t i = 0;
      while (i < d.size()) {
        // Records never straddle a 2048-byte sector (ECMA-119 6.8.1.1); the
        // tail of each sector is zero padding.
        size_t sector_end = size_t(((base + i) / kSector + 1) * kSector - base);
        sector_end = std::min(sector_end, d.size());
        uint8_t len = d[i];
        if (len == 0 || len < 34 || i + len > sector_end) {
          i = sector_end;
          continue;
        }
        Rec x = parse_record(d.data() + i);
        i += len;
        if (33u + x.name_len > len) continue;  // malformed: name overruns its record
        if (x.name_len == 1 && (x.name[0] == 0 || x.name[0] == 1)) continue;  // "." and ".."
        // Associated files are the Mac resource forks of an Apple-extension
        // hybrid; they share the data file's name and are never Windows files.
        if (x.flags & 0x04) continue;
        IsoEntry e = entry_from(x, joliet_names);
        if (x.unit || x.gap) throw IsoError("interleaved file '" + e.name + "' is not supported");

        // Multi-extent files: every record but the last has bit 7 set, and
        // they are consecutive records with the same identifier.
        if (have_pending) {
          if (e.name == pending.name && !e.is_dir) {
            pending.extents.push_back(e.extents[0]);
            pending.size += e.size;
            if (!(x.flags & 0x80)) {
              out.push_back(std::move(pending));
              have_pending = false;
            }
            continue;
          }
          out.push_back(std::move(pending));  // unterminated chain: keep what it had
          have_pending = false;
        }
        if ((x.flags & 0x80) && !e.is_dir) {
          pending = std::move(e);
          have_pending = true;
          continue;
        }
        out.push_back(std::move(e));
      }
      if (have_pending) out.push_back(std::move(pending));
    }
    return out;
  }

  // Give each Joliet entry its primary-volume twin: files by name or by
  // (extent, size) — both trees point at the same file data — and directories
  // by name, by elimination, or by the file extents they share.
  void pair_with_primary(std::vector<IsoEntry>& items, const std::vector<IsoEntry>& prim) const {
    std::vector<bool> used(prim.size(), false);
    std::vector<bool> done(items.size(), false);
    auto take = [&](size_t i, size_t j) {
      items[i].short_name = prim[j].name;
      if (items[i].is_dir) items[i].primary_twin = prim[j].extents[0];
      used[j] = done[i] = true;
    };
    for (size_t i = 0; i < items.size(); i++) {
      for (size_t j = 0; j < prim.size(); j++) {
        if (used[j] || prim[j].is_dir != items[i].is_dir || !iequals(prim[j].name, items[i].name)) continue;
        if (!items[i].is_dir && !same_data(items[i], prim[j])) continue;
        take(i, j);
        break;
      }
    }
    for (size_t i = 0; i < items.size(); i++) {
      if (done[i] || items[i].is_dir || items[i].size == 0) continue;
      size_t hits = 0, at = 0;
      for (size_t j = 0; j < prim.size(); j++)
        if (!used[j] && !prim[j].is_dir && same_data(items[i], prim[j])) hits++, at = j;
      if (hits == 1) take(i, at);
    }
    std::vector<size_t> jd, pd;
    for (size_t i = 0; i < items.size(); i++)
      if (!done[i] && items[i].is_dir) jd.push_back(i);
    for (size_t j = 0; j < prim.size(); j++)
      if (!used[j] && prim[j].is_dir) pd.push_back(j);
    if (jd.size() == 1 && pd.size() == 1) {
      take(jd[0], pd[0]);
      return;
    }
    auto file_keys = [&](const IsoEntry& d, bool joliet_names) {
      std::set<std::pair<uint32_t, uint64_t>> keys;
      for (const IsoEntry& c : parse_dir(d, joliet_names))
        if (!c.is_dir && c.size) keys.insert({c.extents[0].lba, c.size});
      return keys;
    };
    for (size_t i : jd) {
      auto mine = file_keys(items[i], true);
      size_t best = SIZE_MAX, best_n = 0;
      for (size_t j : pd) {
        if (used[j]) continue;
        auto theirs = file_keys(prim[j], false);
        size_t n = 0;
        for (auto& k : mine) n += theirs.count(k);
        if (n > best_n) best_n = n, best = j;
      }
      if (best != SIZE_MAX) take(i, best);
    }
  }
};

IsoImage::IsoImage(const std::filesystem::path& path, bool aligned_reads) : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  const bool device = path.native().rfind(L"\\\\.\\", 0) == 0;
  // A volume must be opened shared for writing too, or CreateFile refuses it.
  m.file = Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | (device ? FILE_SHARE_WRITE : 0), nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!m.file.valid())
    throw IsoError("cannot open " + to_utf8(path.wstring()) + ": " + win_error_string(GetLastError()));
  m.aligned = device || aligned_reads;
  if (m.aligned) {
    m.bounce = static_cast<uint8_t*>(VirtualAlloc(nullptr, Impl::kBounce, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!m.bounce) throw IsoError("out of memory");
  }
  if (device) {
    GET_LENGTH_INFORMATION li{};
    DWORD got = 0;
    if (DeviceIoControl(m.file.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &li, sizeof(li), &got, nullptr) &&
        li.Length.QuadPart > 0) {
      m.fsize = uint64_t(li.Length.QuadPart);
    } else {
      // The volume space size the primary descriptor records.
      m.fsize = uint64_t(17) * kSector;
      std::vector<uint8_t> vd(kSector);
      m.pread_direct(16ull * kSector, vd.data(), kSector);
      if (memcmp(vd.data() + 1, "CD001", 5) != 0) throw IsoError("not an ISO-9660 volume");
      m.fsize = uint64_t(both32(vd.data() + 80, [](uint32_t v) { return v > 16; })) * kSector;
    }
    m.fsize &= ~uint64_t(kSector - 1);
  } else {
    LARGE_INTEGER sz{};
    GetFileSizeEx(m.file.get(), &sz);
    m.fsize = uint64_t(sz.QuadPart);
  }

  // Cooked 2048-byte sectors first; then BIN-style raw sectors, where the
  // user data sits after the 12-byte sync + 4-byte header (MODE1) or after
  // an additional 8-byte subheader (MODE2 form 1).
  if (m.has_signature(2048, 0)) {
    m.phys = 2048, m.data_off = 0;
  } else if (m.has_signature(2352, 16)) {
    m.phys = 2352, m.data_off = 16;
  } else if (m.has_signature(2352, 24)) {
    m.phys = 2352, m.data_off = 24;
  } else {
    throw IsoError("not an ISO-9660 image (no CD001 volume descriptor at sector 16)");
  }
  m.logical_size = m.phys == kSector ? m.fsize : (m.fsize / m.phys) * kSector;

  std::vector<uint8_t> pvd, svd;
  std::vector<uint8_t> vd(kSector);
  for (uint32_t s = 16; s < 16 + 64; s++) {
    if ((uint64_t(s) + 1) * kSector > m.logical_size) break;
    m.read_logical(uint64_t(s) * kSector, vd.data(), kSector);
    if (memcmp(vd.data() + 1, "CD001", 5) != 0) break;
    uint8_t type = vd[0];
    if (type == 255) break;
    if (type == 1 && pvd.empty()) pvd = vd;
    // Joliet = a supplementary descriptor whose escape sequence names UCS-2
    // level 1/2/3 ("%/@", "%/C", "%/E").
    if (type == 2 && svd.empty() && vd[88] == '%' && vd[89] == '/' &&
        (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E'))
      svd = vd;
  }
  if (pvd.empty()) throw IsoError("ISO-9660 image has no primary volume descriptor");

  uint16_t bs = both16(pvd.data() + 128);
  m.bs = (bs == 512 || bs == 1024 || bs == 2048) ? bs : kSector;
  std::string vol = latin1_to_utf8(pvd.data() + 40, 32);
  while (!vol.empty() && (vol.back() == ' ' || vol.back() == '\0')) vol.pop_back();
  m.volid = vol;

  auto root_of = [&](const std::vector<uint8_t>& desc, bool joliet_names) {
    Impl::Rec x = m.parse_record(desc.data() + 156);
    IsoEntry e = m.entry_from(x, joliet_names);
    e.name.clear();
    e.is_dir = true;
    return e;
  };
  IsoEntry primary_root = root_of(pvd, false);
  if (!svd.empty()) {
    m.joliet = true;
    m.root = root_of(svd, true);
    m.root.primary_twin = primary_root.extents[0];
  } else {
    m.root = primary_root;
  }
}

IsoImage::~IsoImage() = default;

bool IsoImage::joliet() const { return impl_->joliet; }
bool IsoImage::raw_sectors() const { return impl_->phys != kSector; }
uint32_t IsoImage::block_size() const { return impl_->bs; }
uint64_t IsoImage::file_size() const { return impl_->fsize; }
const std::string& IsoImage::volume_id() const { return impl_->volid; }
const IsoEntry& IsoImage::root() const { return impl_->root; }

std::vector<IsoEntry> IsoImage::list(const IsoEntry& dir) const {
  std::vector<IsoEntry> items = impl_->parse_dir(dir, dir.in_joliet);
  if (!dir.in_joliet) {
    for (IsoEntry& e : items) e.short_name = e.name;
    return items;
  }
  if (dir.primary_twin.size) {
    IsoEntry twin;
    twin.is_dir = true;
    twin.name = dir.name;
    twin.extents = {dir.primary_twin};
    twin.size = dir.primary_twin.size;
    impl_->pair_with_primary(items, impl_->parse_dir(twin, false));
  }
  return items;
}

std::optional<IsoEntry> IsoImage::find(std::string_view path) const {
  IsoEntry cur = root();
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find_first_of("/\\", i);
    if (j == std::string_view::npos) j = path.size();
    std::string comp = strip_version(std::string(path.substr(i, j - i)));
    i = j + 1;
    if (comp.empty() || comp == ".") continue;
    if (!cur.is_dir) return std::nullopt;
    bool found = false;
    for (IsoEntry& e : list(cur)) {
      if (iequals(e.name, comp) || (!e.short_name.empty() && iequals(e.short_name, comp))) {
        cur = std::move(e);
        found = true;
        break;
      }
    }
    if (!found) return std::nullopt;
  }
  return cur;
}

void IsoImage::read(const IsoEntry& file, const std::function<void(const uint8_t*, size_t)>& sink) const {
  if (file.is_dir) throw IsoError("'" + file.name + "' is a directory");
  std::vector<uint8_t> buf(size_t(std::min<uint64_t>(kChunk, std::max<uint64_t>(file.size, 1))));
  for (const IsoExtent& ext : file.extents) {
    uint64_t addr = uint64_t(ext.lba) * impl_->bs;
    uint64_t left = ext.size;
    if (addr + left > impl_->logical_size)
      throw IsoError("'" + file.name + "' extends past the end of the image (truncated download?)");
    while (left) {
      size_t n = size_t(std::min<uint64_t>(left, buf.size()));
      impl_->read_logical(addr, buf.data(), n);
      sink(buf.data(), n);
      addr += n;
      left -= n;
    }
  }
}

}  // namespace adw::import
