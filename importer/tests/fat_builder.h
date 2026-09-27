// Synthetic FAT12/FAT16 volume images for the importer tests (PACKAGES.md
// §8.2): a DOS 4-style boot sector and BPB, both FAT copies, the fixed root
// directory, subdirectories on cluster chains, files that can be fragmented
// on purpose, and raw extra directory entries (long-file-name, deleted,
// volume label) the reader must skip. After build(), set_fat() and
// entry_offset() let a test damage chains and sizes.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace test {

class FatBuilder {
 public:
  uint16_t bps = 512;
  uint8_t spc = 1;
  uint16_t reserved = 1;
  uint8_t fats = 2;
  uint16_t root_entries = 224;
  uint32_t total_sectors = 2880;
  uint8_t media = 0xF0;
  uint16_t spf = 9;
  std::string label;  // a volume-label entry in the root when non-empty

  // 1.44 MB: 18 sectors/track, 1 sector/cluster, 224 root entries.
  static FatBuilder floppy144() { return FatBuilder(); }
  // 2.88 MB (the Simpsons image's geometry): 2 sectors/cluster, 240 root entries.
  static FatBuilder floppy288() {
    FatBuilder b;
    b.spc = 2;
    b.root_entries = 240;
    b.total_sectors = 5760;
    b.spf = 9;
    return b;
  }

  struct Node {
    std::string name;  // 8.3, as stored (upper case expected)
    bool dir = false;
    std::vector<uint8_t> data;
    bool fragment = false;
    uint16_t time = 0x7A00, date = 0x1CF1;  // 15:16:00, 1994-07-17
    std::vector<std::array<uint8_t, 32>> raw_before;  // extra entries written before this one
    std::vector<std::unique_ptr<Node>> kids;
    std::vector<uint32_t> clusters;
    size_t entry_offset = 0;  // image offset of its directory entry (after build)
  };

  FatBuilder() { root_.dir = true; }

  Node& dir(const std::string& path) {
    Node& n = walk(path);
    n.dir = true;
    return n;
  }
  Node& file(const std::string& path, std::vector<uint8_t> data, bool fragment = false) {
    Node& n = walk(path);
    n.dir = false;
    n.data = std::move(data);
    n.fragment = fragment;
    return n;
  }
  // Raw entries at the start of the root directory.
  std::vector<std::array<uint8_t, 32>> root_raw;

  static std::array<uint8_t, 32> raw_entry(const std::string& name11, uint8_t attr, uint8_t first = 0) {
    std::array<uint8_t, 32> e{};
    std::string n = name11;
    n.resize(11, ' ');
    memcpy(e.data(), n.data(), 11);
    if (first) e[0] = first;
    e[11] = attr;
    return e;
  }

  uint32_t cluster_count() const { return (total_sectors - data_sector()) / spc; }
  bool fat16() const { return cluster_count() >= 4085; }

  std::vector<uint8_t> build() {
    std::fill(used_.begin(), used_.end(), false);
    used_.assign(size_t(cluster_count()) + 2, false);
    allocate(root_);
    img_.assign(size_t(total_sectors) * bps, 0);
    boot();
    for (uint8_t f = 0; f < fats; f++) {
      set_fat_raw(f, 0, 0xF00u | media | (fat16() ? 0xFF00u : 0));
      set_fat_raw(f, 1, fat16() ? 0xFFFF : 0xFFF);
    }
    chains(root_);
    write_dir(root_, nullptr);
    return img_;
  }

  // Sets FAT entry `n` in every copy of an already built image.
  void set_fat(std::vector<uint8_t>& img, uint32_t n, uint32_t v) {
    std::swap(img, img_);
    for (uint8_t f = 0; f < fats; f++) set_fat_raw(f, n, v);
    std::swap(img, img_);
  }

  Node& node(const std::string& path) { return walk(path); }

 private:
  Node root_;
  std::vector<bool> used_;
  std::vector<uint8_t> img_;

  uint32_t root_sectors() const { return (uint32_t(root_entries) * 32 + bps - 1) / bps; }
  uint32_t data_sector() const { return reserved + fats * spf + root_sectors(); }
  uint32_t cluster_bytes() const { return uint32_t(bps) * spc; }
  size_t cluster_offset(uint32_t c) const { return (size_t(data_sector()) + size_t(c - 2) * spc) * bps; }

  Node& walk(const std::string& path) {
    Node* cur = &root_;
    size_t i = 0;
    while (i < path.size()) {
      size_t j = path.find('/', i);
      if (j == std::string::npos) j = path.size();
      std::string comp = path.substr(i, j - i);
      i = j + 1;
      if (comp.empty()) continue;
      Node* next = nullptr;
      for (auto& k : cur->kids)
        if (k->name == comp) next = k.get();
      if (!next) {
        cur->kids.push_back(std::make_unique<Node>());
        next = cur->kids.back().get();
        next->name = comp;
        next->dir = true;
      }
      cur = next;
    }
    return *cur;
  }

  std::vector<uint32_t> take(uint32_t count, bool fragment) {
    std::vector<uint32_t> out;
    bool skip = false;
    for (uint32_t c = 2; c < used_.size() && out.size() < count; c++) {
      if (used_[c]) continue;
      if (fragment && skip) {
        skip = false;
        continue;
      }
      used_[c] = true;
      out.push_back(c);
      skip = fragment;
    }
    return out;
  }

  void allocate(Node& n) {
    if (&n != &root_) {
      uint32_t bytes = n.dir ? uint32_t((2 + n.kids.size() + extra_count(n)) * 32) : uint32_t(n.data.size());
      uint32_t count = n.dir ? std::max<uint32_t>(1, (bytes + cluster_bytes() - 1) / cluster_bytes())
                             : (bytes + cluster_bytes() - 1) / cluster_bytes();
      n.clusters = take(count, n.fragment);
    }
    for (auto& k : n.kids) allocate(*k);
  }

  static size_t extra_count(const Node& n) {
    size_t e = 0;
    for (auto& k : n.kids) e += k->raw_before.size();
    return e;
  }

  void set_fat_raw(uint8_t f, uint32_t n, uint32_t v) {
    size_t base = size_t(reserved + f * spf) * bps;
    if (fat16()) {
      img_[base + n * 2] = uint8_t(v);
      img_[base + n * 2 + 1] = uint8_t(v >> 8);
      return;
    }
    size_t k = base + n * 3 / 2;
    if (n & 1) {
      img_[k] = uint8_t((img_[k] & 0x0F) | ((v & 0x0F) << 4));
      img_[k + 1] = uint8_t(v >> 4);
    } else {
      img_[k] = uint8_t(v);
      img_[k + 1] = uint8_t((img_[k + 1] & 0xF0) | ((v >> 8) & 0x0F));
    }
  }

  void chains(Node& n) {
    for (size_t i = 0; i < n.clusters.size(); i++)
      for (uint8_t f = 0; f < fats; f++)
        set_fat_raw(f, n.clusters[i], i + 1 < n.clusters.size() ? n.clusters[i + 1] : (fat16() ? 0xFFFF : 0xFFF));
    for (auto& k : n.kids) chains(*k);
  }

  static void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
  static void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v)), put16(p + 2, uint16_t(v >> 16)); }

  static std::array<uint8_t, 32> entry(const Node& n, const std::string& name_override = {}) {
    std::array<uint8_t, 32> e{};
    std::string name = name_override.empty() ? n.name : name_override;
    std::string base = name, ext;
    size_t dot = name.find('.');
    if (dot != std::string::npos && name != "." && name != "..") base = name.substr(0, dot), ext = name.substr(dot + 1);
    base.resize(8, ' ');
    ext.resize(3, ' ');
    memcpy(e.data(), base.data(), 8);
    memcpy(e.data() + 8, ext.data(), 3);
    if (e[0] == 0xE5) e[0] = 0x05;
    e[11] = n.dir ? 0x10 : 0x20;
    put16(e.data() + 22, n.time);
    put16(e.data() + 24, n.date);
    put16(e.data() + 26, uint16_t(n.clusters.empty() ? 0 : n.clusters[0]));
    put32(e.data() + 28, n.dir ? 0 : uint32_t(n.data.size()));
    return e;
  }

  void write_dir(Node& d, Node* parent) {
    // Collect the entries, then place them in the root region or the chain.
    std::vector<std::array<uint8_t, 32>> ents;
    std::vector<Node*> owners;
    if (&d == &root_) {
      if (!label.empty()) ents.push_back(raw_entry(label, 0x08)), owners.push_back(nullptr);
      for (auto& r : root_raw) ents.push_back(r), owners.push_back(nullptr);
    } else {
      Node dot = Node();
      dot.dir = true;
      dot.clusters = d.clusters;
      ents.push_back(entry(dot, ".")), owners.push_back(nullptr);
      Node dotdot = Node();
      dotdot.dir = true;
      if (parent && parent != &root_) dotdot.clusters = parent->clusters;
      ents.push_back(entry(dotdot, "..")), owners.push_back(nullptr);
    }
    for (auto& k : d.kids) {
      for (auto& r : k->raw_before) ents.push_back(r), owners.push_back(nullptr);
      ents.push_back(entry(*k)), owners.push_back(k.get());
    }
    for (size_t i = 0; i < ents.size(); i++) {
      size_t off;
      if (&d == &root_) {
        off = size_t(reserved + fats * spf) * bps + i * 32;
      } else {
        size_t byte = i * 32;
        off = cluster_offset(d.clusters[byte / cluster_bytes()]) + byte % cluster_bytes();
      }
      memcpy(&img_[off], ents[i].data(), 32);
      if (owners[i]) owners[i]->entry_offset = off;
    }
    for (auto& k : d.kids) {
      if (k->dir) {
        write_dir(*k, &d);
        continue;
      }
      size_t done = 0;
      for (uint32_t c : k->clusters) {
        size_t n = std::min<size_t>(cluster_bytes(), k->data.size() - done);
        memcpy(&img_[cluster_offset(c)], k->data.data() + done, n);
        done += n;
      }
    }
  }

  void boot() {
    uint8_t* b = img_.data();
    b[0] = 0xEB, b[1] = 0x3C, b[2] = 0x90;
    memcpy(b + 3, "TESTFAT ", 8);
    put16(b + 11, bps);
    b[13] = spc;
    put16(b + 14, reserved);
    b[16] = fats;
    put16(b + 17, root_entries);
    if (total_sectors < 0x10000) put16(b + 19, uint16_t(total_sectors));
    else put32(b + 32, total_sectors);
    b[21] = media;
    put16(b + 22, spf);
    put16(b + 24, 18);
    put16(b + 26, 2);
    b[38] = 0x29;
    put32(b + 39, 0x12345678);
    std::string l = label.empty() ? "NO NAME" : label;
    l.resize(11, ' ');
    memcpy(b + 43, l.data(), 11);
    memcpy(b + 54, fat16() ? "FAT16   " : "FAT12   ", 8);
    b[510] = 0x55, b[511] = 0xAA;
  }
};

}  // namespace test
