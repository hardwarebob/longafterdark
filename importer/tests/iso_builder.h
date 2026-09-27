// Synthetic ISO-9660 (+ Joliet) image writer for the importer tests.
//
// Writes a spec-shaped image (ECMA-119 volume descriptors, L and M path
// tables, sorted directory records that never straddle a sector) so the
// reader is exercised on the same structures a mastering tool produces, plus
// switches for the awkward cases: multi-extent files (optionally stored out of
// order), associated-file records, names without ";1", one-sided both-endian
// fields, and raw 2352-byte MODE1 sectors.
#pragma once

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "winutil.h"

namespace test {

class IsoBuilder {
 public:
  struct Node {
    std::string name;    // primary-volume identifier, without ";1"
    std::string joliet;  // Joliet name (UTF-8); empty = same as name
    bool dir = false;
    std::vector<uint8_t> data;
    std::vector<uint32_t> split;  // multi-extent: sizes of all but the last extent (multiples of 2048)
    bool associated = false;      // an associated-file record precedes it (primary tree)
    std::vector<std::unique_ptr<Node>> kids;
    Node* parent = nullptr;
    // A directory whose records (in both trees) point at another directory's
    // extent instead of its own: one directory listed twice, which no
    // mastering tool writes (a crafted image).
    Node* same_as = nullptr;
    uint32_t dir_lba[2] = {0, 0}, dir_size[2] = {0, 0};
    std::vector<uint32_t> ext_lba;
    uint32_t assoc_lba = 0;
  };

  enum class Endian { both, le_only, be_only };
  bool joliet = true;
  bool version_suffix = true;
  Endian endian = Endian::both;
  bool raw2352 = false;
  bool reverse_extents = false;
  std::string volume_id = "AD_TEST";
  // 1996-09-12 22:14:10 at GMT-7 (offset in 15-minute units).
  uint8_t rec_time[7] = {96, 9, 12, 22, 14, 10, uint8_t(int8_t(-28))};

  IsoBuilder() {
    root_.dir = true;
    root_.parent = &root_;
  }

  Node& dir(const std::string& path, const std::string& joliet_name = "") {
    Node& n = walk(path);
    n.dir = true;
    if (!joliet_name.empty()) n.joliet = joliet_name;
    return n;
  }

  Node& file(const std::string& path, std::vector<uint8_t> data, const std::string& joliet_name = "") {
    Node& n = walk(path);
    n.dir = false;
    n.data = std::move(data);
    if (!joliet_name.empty()) n.joliet = joliet_name;
    return n;
  }

  std::vector<uint8_t> build() {
    const int trees = joliet ? 2 : 1;
    // Directory numbering (path-table order): breadth first, siblings sorted.
    std::vector<Node*> bfs[2];
    for (int t = 0; t < trees; t++) {
      std::deque<Node*> q{&root_};
      while (!q.empty()) {
        Node* d = q.front();
        q.pop_front();
        bfs[t].push_back(d);
        for (Node* k : sorted(d, t))
          if (k->dir) q.push_back(k);
      }
    }
    uint32_t next = 16;
    const uint32_t pvd = next++;
    const uint32_t svd = joliet ? next++ : 0;
    const uint32_t term = next++;
    uint32_t pt_size[2] = {0, 0}, pt_l[2] = {0, 0}, pt_m[2] = {0, 0};
    for (int t = 0; t < trees; t++) {
      for (Node* d : bfs[t]) {
        size_t id = d == &root_ ? 1 : ident(d, t).size();
        pt_size[t] += uint32_t(8 + id + (id & 1));
      }
      pt_l[t] = next;
      next += sectors(pt_size[t]);
      pt_m[t] = next;
      next += sectors(pt_size[t]);
    }
    for (int t = 0; t < trees; t++)
      for (Node* d : bfs[t]) {
        d->dir_size[t] = dir_bytes(d, t);
        d->dir_lba[t] = next;
        next += d->dir_size[t] / 2048;
      }
    std::vector<Node*> files;
    collect_files(&root_, files);
    for (Node* f : files) {
      if (f->associated) f->assoc_lba = next++;
      auto sizes = extent_sizes(f);
      f->ext_lba.assign(sizes.size(), 0);
      std::vector<size_t> order(sizes.size());
      for (size_t i = 0; i < order.size(); i++) order[i] = reverse_extents ? order.size() - 1 - i : i;
      for (size_t i : order) {
        if (!sizes[i]) continue;  // empty files point at block 0
        f->ext_lba[i] = next;
        next += sectors(sizes[i]);
      }
    }
    const uint32_t total = next;
    img_.assign(size_t(total) * 2048, 0);

    // Volume descriptors.
    for (int t = 0; t < trees; t++) {
      uint8_t* v = &img_[size_t(t == 0 ? pvd : svd) * 2048];
      v[0] = t == 0 ? 1 : 2;
      memcpy(v + 1, "CD001", 5);
      v[6] = 1;
      text(v + 8, 32, "WIN32", t);
      text(v + 40, 32, volume_id, t);
      both32_vd(v + 80, total);
      if (t == 1) memcpy(v + 88, "%/E", 3);
      both16_vd(v + 120, 1);
      both16_vd(v + 124, 1);
      both16_vd(v + 128, 2048);
      both32_vd(v + 132, pt_size[t]);
      le32(v + 140, pt_l[t]);
      be32(v + 148, pt_m[t]);
      record(v + 156, &root_, t, root_.dir_lba[t], root_.dir_size[t], 0x02, {0});
      for (auto [off, len] : {std::pair{190, 128}, {318, 128}, {446, 128}, {574, 128}, {702, 37}, {739, 37}, {776, 37}})
        text(v + off, len, "", t);
      for (int off : {813, 830}) {
        memcpy(v + off, "1996091222141000", 16);
        v[off + 16] = rec_time[6];
      }
      for (int off : {847, 864}) memcpy(v + off, "0000000000000000", 16);
      v[881] = 1;
    }
    {
      uint8_t* v = &img_[size_t(term) * 2048];
      v[0] = 255;
      memcpy(v + 1, "CD001", 5);
      v[6] = 1;
    }

    // Path tables (L: little-endian, M: big-endian).
    for (int t = 0; t < trees; t++) {
      for (int m = 0; m < 2; m++) {
        uint8_t* p = &img_[size_t(m ? pt_m[t] : pt_l[t]) * 2048];
        for (Node* d : bfs[t]) {
          std::vector<uint8_t> id = d == &root_ ? std::vector<uint8_t>{0} : ident(d, t);
          uint16_t parent = uint16_t(std::find(bfs[t].begin(), bfs[t].end(), d->parent) - bfs[t].begin() + 1);
          p[0] = uint8_t(id.size());
          if (m) {
            be32(p + 2, d->dir_lba[t]);
            p[6] = uint8_t(parent >> 8), p[7] = uint8_t(parent);
          } else {
            le32(p + 2, d->dir_lba[t]);
            p[6] = uint8_t(parent), p[7] = uint8_t(parent >> 8);
          }
          memcpy(p + 8, id.data(), id.size());
          p += 8 + id.size() + (id.size() & 1);
        }
      }
    }

    // Directories.
    for (int t = 0; t < trees; t++)
      for (Node* d : bfs[t]) {
        size_t base = size_t(d->dir_lba[t]) * 2048, pos = 0;
        auto place = [&](const std::vector<uint8_t>& id, uint32_t lba, uint32_t size, uint8_t flags, Node* n) {
          size_t len = rec_len(id.size());
          if (pos % 2048 + len > 2048) pos = (pos / 2048 + 1) * 2048;
          record(&img_[base + pos], n, t, lba, size, flags, id);
          pos += len;
        };
        place({0}, d->dir_lba[t], d->dir_size[t], 0x02, d);
        place({1}, d->parent->dir_lba[t], d->parent->dir_size[t], 0x02, d->parent);
        for (Node* k : sorted(d, t)) {
          auto id = ident(k, t);
          if (k->dir) {
            const Node* at = k->same_as ? k->same_as : k;
            place(id, at->dir_lba[t], at->dir_size[t], 0x02, k);
            continue;
          }
          if (t == 0 && k->associated) place(id, k->assoc_lba, 100, 0x04, k);
          auto sizes = extent_sizes(k);
          for (size_t i = 0; i < sizes.size(); i++)
            place(id, k->ext_lba[i], sizes[i], i + 1 < sizes.size() ? 0x80 : 0x00, k);
        }
      }

    // File data.
    for (Node* f : files) {
      if (f->associated) memcpy(&img_[size_t(f->assoc_lba) * 2048], "RESOURCE FORK", 13);
      auto sizes = extent_sizes(f);
      size_t off = 0;
      for (size_t i = 0; i < sizes.size(); i++) {
        if (sizes[i]) memcpy(&img_[size_t(f->ext_lba[i]) * 2048], f->data.data() + off, sizes[i]);
        off += sizes[i];
      }
    }
    return raw2352 ? to_raw(img_) : img_;
  }

 private:
  Node root_;
  std::vector<uint8_t> img_;

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
        next->dir = true;  // until file() says otherwise
        next->parent = cur;
      }
      cur = next;
    }
    return *cur;
  }

  static uint32_t sectors(uint64_t bytes) { return uint32_t((bytes + 2047) / 2048); }
  static size_t rec_len(size_t id_len) { return (33 + id_len + 1) & ~size_t(1); }

  std::vector<uint32_t> extent_sizes(const Node* f) const {
    std::vector<uint32_t> s = f->split;
    uint64_t used = 0;
    for (uint32_t x : s) used += x;
    s.push_back(uint32_t(f->data.size() - used));
    return s;
  }

  std::vector<uint8_t> ident(const Node* n, int t) const {
    if (t == 0) {
      std::string s = n->name;
      if (!n->dir) {
        if (s.find('.') == std::string::npos) s += ".";
        if (version_suffix) s += ";1";
      }
      return std::vector<uint8_t>(s.begin(), s.end());
    }
    std::wstring w = adw::import::to_wide(n->joliet.empty() ? n->name : n->joliet);
    if (!n->dir && version_suffix) w += L";1";
    std::vector<uint8_t> out;
    for (wchar_t c : w) out.push_back(uint8_t(c >> 8)), out.push_back(uint8_t(c));
    return out;
  }

  std::vector<Node*> sorted(Node* d, int t) const {
    std::vector<Node*> v;
    for (auto& k : d->kids) v.push_back(k.get());
    std::sort(v.begin(), v.end(), [&](Node* a, Node* b) { return ident(a, t) < ident(b, t); });
    return v;
  }

  uint32_t dir_bytes(Node* d, int t) const {
    size_t pos = 0;
    auto place = [&](size_t len) {
      if (pos % 2048 + len > 2048) pos = (pos / 2048 + 1) * 2048;
      pos += len;
    };
    place(34);
    place(34);
    for (Node* k : sorted(d, t)) {
      size_t len = rec_len(ident(k, t).size());
      if (k->dir) {
        place(len);
        continue;
      }
      if (t == 0 && k->associated) place(len);
      for (size_t i = 0; i < k->split.size() + 1; i++) place(len);
    }
    return sectors(pos) * 2048;
  }

  void collect_files(Node* d, std::vector<Node*>& out) const {
    for (Node* k : sorted(d, 0)) {
      if (k->dir) collect_files(k, out);
      else out.push_back(k);
    }
  }

  static void le32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
  }
  static void be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
  static void both32_vd(uint8_t* p, uint32_t v) { le32(p, v), be32(p + 4, v); }
  static void both16_vd(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
  // Directory-record both-endian fields honour the one-sided test modes.
  void both32_rec(uint8_t* p, uint32_t v) const {
    if (endian != Endian::be_only) le32(p, v);
    if (endian != Endian::le_only) be32(p + 4, v);
  }

  // a-/d-characters padded with spaces; UCS-2BE for the Joliet descriptor.
  static void text(uint8_t* p, size_t len, const std::string& s, int t) {
    if (t == 0) {
      memset(p, ' ', len);
      memcpy(p, s.data(), std::min(len, s.size()));
    } else {
      for (size_t i = 0; i + 1 < len; i += 2) p[i] = 0, p[i + 1] = ' ';
      if (len & 1) p[len - 1] = 0;
      for (size_t i = 0; i < s.size() && 2 * i + 1 < len; i++) p[2 * i + 1] = uint8_t(s[i]);
    }
  }

  void record(uint8_t* r, const Node*, int, uint32_t lba, uint32_t size, uint8_t flags,
              const std::vector<uint8_t>& id) const {
    r[0] = uint8_t(rec_len(id.size()));
    r[1] = 0;
    both32_rec(r + 2, lba);
    both32_rec(r + 10, size);
    memcpy(r + 18, rec_time, 7);
    r[25] = flags;
    r[26] = r[27] = 0;
    both16_vd(r + 28, 1);
    r[32] = uint8_t(id.size());
    memcpy(r + 33, id.data(), id.size());
  }

  // Cooked 2048-byte sectors -> raw MODE1 2352-byte sectors (sync, BCD MSF
  // header, data, zeroed EDC/ECC — the reader never checks them).
  static std::vector<uint8_t> to_raw(const std::vector<uint8_t>& cooked) {
    size_t n = cooked.size() / 2048;
    std::vector<uint8_t> raw(n * 2352, 0);
    auto bcd = [](int v) { return uint8_t((v / 10) << 4 | (v % 10)); };
    for (size_t s = 0; s < n; s++) {
      uint8_t* o = &raw[s * 2352];
      o[0] = 0;
      memset(o + 1, 0xFF, 10);
      o[11] = 0;
      int f = int(s) + 150;
      o[12] = bcd(f / 4500), o[13] = bcd((f / 75) % 60), o[14] = bcd(f % 75), o[15] = 1;
      memcpy(o + 16, &cooked[s * 2048], 2048);
    }
    return raw;
  }
};

}  // namespace test
