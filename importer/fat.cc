#include "fat.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "winutil.h"

namespace adw::import {

namespace fs = std::filesystem;

namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

// A DOS 8.3 directory name (space padded, OEM code page 437) as UTF-8.
std::string dos_name(const uint8_t* raw) {
  uint8_t n[11];
  memcpy(n, raw, 11);
  if (n[0] == 0x05) n[0] = 0xE5;  // a real leading 0xE5, escaped
  auto part = [&](int from, int len) {
    int end = from + len;
    while (end > from && n[end - 1] == ' ') end--;
    return std::string(reinterpret_cast<const char*>(n + from), size_t(end - from));
  };
  std::string base = part(0, 8), ext = part(8, 3);
  std::string oem = ext.empty() ? base : base + "." + ext;
  bool ascii = std::all_of(oem.begin(), oem.end(), [](char c) { return uint8_t(c) < 0x80; });
  std::string out;
  if (ascii) {
    out = oem;
  } else {
    int w = MultiByteToWideChar(437, 0, oem.data(), int(oem.size()), nullptr, 0);
    std::wstring ws(size_t(w), L'\0');
    MultiByteToWideChar(437, 0, oem.data(), int(oem.size()), ws.data(), w);
    out = to_utf8(ws);
  }
  for (char& c : out)
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  return out;
}

}  // namespace

struct FatImage::Impl {
  Handle file;
  uint64_t size = 0;
  uint32_t bps = 0, spc = 0, reserved = 0, fats = 0, root_entries = 0, total_sectors = 0, spf = 0;
  uint8_t media_byte = 0;
  uint32_t root_sector = 0, root_sectors = 0, data_sector = 0, clusters = 0;
  int bits = 0;
  std::vector<uint8_t> fat;  // the first FAT
  std::string label;
  FatEntry root_entry;

  void read_at(uint64_t off, void* buf, size_t n) const {
    if (off + n > size) throw FatError("read past the end of the image");
    OVERLAPPED ov{};
    ov.Offset = DWORD(off);
    ov.OffsetHigh = DWORD(off >> 32);
    DWORD got = 0;
    if (!ReadFile(file.get(), buf, DWORD(n), &got, &ov) || got != n)
      throw FatError("read error in the image: " + win_error_string(GetLastError()));
  }

  uint32_t cluster_bytes() const { return bps * spc; }
  uint32_t max_cluster() const { return clusters + 1; }

  uint32_t fat_entry(uint32_t n) const {
    if (bits == 12) {
      size_t k = size_t(n) * 3 / 2;
      uint16_t v = le16(&fat[k]);
      return (n & 1) ? uint32_t(v >> 4) : uint32_t(v & 0x0FFF);
    }
    return le16(&fat[size_t(n) * 2]);
  }

  // The clusters of a chain, validated; at least `min_bytes` worth of them.
  std::vector<uint32_t> chain(uint32_t start, uint64_t min_bytes, const std::string& what) const {
    std::vector<uint32_t> out;
    if (start == 0) {
      if (min_bytes) throw FatError(what + ": no clusters for a non-empty file");
      return out;
    }
    const uint32_t eoc = bits == 12 ? 0xFF8 : 0xFFF8;
    const uint32_t bad = bits == 12 ? 0xFF7 : 0xFFF7;
    std::vector<bool> seen(size_t(max_cluster()) + 1, false);
    uint32_t c = start;
    for (;;) {
      if (c < 2 || c > max_cluster()) throw FatError(what + ": cluster " + std::to_string(c) + " is out of range");
      if (seen[c]) throw FatError(what + ": the cluster chain loops");
      seen[c] = true;
      out.push_back(c);
      uint32_t next = fat_entry(c);
      if (next >= eoc) break;
      if (next == 0) throw FatError(what + ": the cluster chain runs into a free cluster");
      if (next == bad) throw FatError(what + ": the cluster chain runs into a bad cluster");
      c = next;
    }
    if (uint64_t(out.size()) * cluster_bytes() < min_bytes)
      throw FatError(what + ": the cluster chain is shorter than the file");
    return out;
  }

  uint64_t cluster_offset(uint32_t c) const {
    return (uint64_t(data_sector) + uint64_t(c - 2) * spc) * bps;
  }

  // Reads `bytes` of a chain, coalescing contiguous clusters into one read.
  void read_chain(const std::vector<uint32_t>& ch, uint64_t bytes,
                  const std::function<void(const uint8_t*, size_t)>& sink) const {
    std::vector<uint8_t> buf;
    uint64_t left = bytes;
    size_t i = 0;
    const size_t max_run = std::max<size_t>(1, (64 * 1024) / cluster_bytes());
    while (left && i < ch.size()) {
      size_t j = i + 1;
      while (j < ch.size() && ch[j] == ch[j - 1] + 1 && j - i < max_run) j++;
      uint64_t run = uint64_t(j - i) * cluster_bytes();
      size_t n = size_t(std::min(run, left));
      buf.resize(n);
      read_at(cluster_offset(ch[i]), buf.data(), n);
      sink(buf.data(), n);
      left -= n;
      i = j;
    }
  }

  std::vector<FatEntry> parse_dir(const std::vector<uint8_t>& raw, bool is_root) {
    std::vector<FatEntry> out;
    for (size_t off = 0; off + 32 <= raw.size(); off += 32) {
      const uint8_t* e = &raw[off];
      if (e[0] == 0x00) break;  // end of directory
      if (e[0] == 0xE5) continue;  // deleted
      uint8_t attr = e[11];
      if ((attr & 0x0F) == 0x0F) continue;  // long-file-name entry
      if (attr & 0x08) {  // volume label
        if (is_root && label.empty()) {
          std::string l(reinterpret_cast<const char*>(e), 11);
          while (!l.empty() && l.back() == ' ') l.pop_back();
          label = l;
        }
        continue;
      }
      if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) continue;
      FatEntry f;
      f.name = dos_name(e);
      f.is_dir = (attr & 0x10) != 0;
      f.first_cluster = le16(e + 26);
      f.size = f.is_dir ? 0 : le32(e + 28);
      f.dos_time = le16(e + 22);
      f.dos_date = le16(e + 24);
      out.push_back(std::move(f));
    }
    return out;
  }
};

FatImage::FatImage(const fs::path& path) : impl_(std::make_unique<Impl>()) {
  Impl& m = *impl_;
  m.file = Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!m.file.valid()) throw FatError("cannot open the image: " + win_error_string(GetLastError()));
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(m.file.get(), &sz)) throw FatError("cannot size the image: " + win_error_string(GetLastError()));
  m.size = uint64_t(sz.QuadPart);
  if (m.size < 512) throw FatError("too small to be a FAT image");
  uint8_t bs[512];
  m.read_at(0, bs, 512);
  m.bps = le16(bs + 11);
  m.spc = bs[13];
  m.reserved = le16(bs + 14);
  m.fats = bs[16];
  m.root_entries = le16(bs + 17);
  m.total_sectors = le16(bs + 19);
  if (!m.total_sectors) m.total_sectors = le32(bs + 32);
  m.media_byte = bs[21];
  m.spf = le16(bs + 22);
  if (m.bps != 512 && m.bps != 1024 && m.bps != 2048 && m.bps != 4096)
    throw FatError("not a FAT image (bytes per sector " + std::to_string(m.bps) + ")");
  if (!m.spc || (m.spc & (m.spc - 1))) throw FatError("not a FAT image (sectors per cluster " + std::to_string(m.spc) + ")");
  if (!m.reserved) throw FatError("not a FAT image (no reserved sectors)");
  if (m.fats < 1 || m.fats > 2) throw FatError("not a FAT image (" + std::to_string(m.fats) + " FATs)");
  if (!m.root_entries) throw FatError("not a FAT12/16 image (no root directory entries)");
  if (!m.total_sectors) throw FatError("not a FAT image (no sector count)");
  if (!m.spf) throw FatError("not a FAT12/16 image (no sectors per FAT)");
  if (bs[510] != 0x55 || bs[511] != 0xAA) throw FatError("not a FAT image (no 55 AA boot signature)");
  if (uint64_t(m.total_sectors) * m.bps > m.size)
    throw FatError("the image is shorter than its " + std::to_string(m.total_sectors) + " sectors");
  m.root_sector = m.reserved + m.fats * m.spf;
  m.root_sectors = (m.root_entries * 32 + m.bps - 1) / m.bps;
  m.data_sector = m.root_sector + m.root_sectors;
  if (m.data_sector >= m.total_sectors) throw FatError("not a FAT image (no data area)");
  m.clusters = (m.total_sectors - m.data_sector) / m.spc;
  if (m.clusters < 4085) m.bits = 12;
  else if (m.clusters < 65525) m.bits = 16;
  else throw FatError("FAT32 volumes are not supported");
  size_t fat_bytes = m.bits == 12 ? (size_t(m.clusters) + 2) * 3 / 2 + 1 : (size_t(m.clusters) + 2) * 2;
  if (uint64_t(m.spf) * m.bps < fat_bytes) throw FatError("the FAT is too small for the volume");
  m.fat.resize(fat_bytes + 1, 0);
  m.read_at(uint64_t(m.reserved) * m.bps, m.fat.data(), fat_bytes);
  m.root_entry.root = true;
  m.root_entry.is_dir = true;
  // The label, when there is one, sits in the root directory.
  list(m.root_entry);
}

FatImage::~FatImage() = default;

int FatImage::fat_bits() const { return impl_->bits; }
uint64_t FatImage::file_size() const { return impl_->size; }
uint32_t FatImage::bytes_per_sector() const { return impl_->bps; }
uint32_t FatImage::sectors_per_cluster() const { return impl_->spc; }
uint32_t FatImage::cluster_count() const { return impl_->clusters; }
uint8_t FatImage::media() const { return impl_->media_byte; }
const std::string& FatImage::volume_label() const { return impl_->label; }
const FatEntry& FatImage::root() const { return impl_->root_entry; }

std::vector<FatEntry> FatImage::list(const FatEntry& dir) const {
  Impl& m = *impl_;
  if (!dir.is_dir) throw FatError(dir.name + " is not a directory");
  std::vector<uint8_t> raw;
  if (dir.root) {
    raw.resize(size_t(m.root_entries) * 32);
    m.read_at(uint64_t(m.root_sector) * m.bps, raw.data(), raw.size());
  } else {
    auto ch = m.chain(dir.first_cluster, 1, dir.name);
    // A directory of more than 64K entries is not a floppy's.
    if (uint64_t(ch.size()) * m.cluster_bytes() > 65536ull * 32)
      throw FatError(dir.name + ": the directory is implausibly large");
    m.read_chain(ch, uint64_t(ch.size()) * m.cluster_bytes(),
                 [&](const uint8_t* d, size_t n) { raw.insert(raw.end(), d, d + n); });
  }
  return m.parse_dir(raw, dir.root);
}

void FatImage::read(const FatEntry& file, const std::function<void(const uint8_t*, size_t)>& sink) const {
  if (file.is_dir) throw FatError(file.name + " is a directory");
  auto ch = impl_->chain(file.first_cluster, file.size, file.name);
  impl_->read_chain(ch, file.size, sink);
}

}  // namespace adw::import
