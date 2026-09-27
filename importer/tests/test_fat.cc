// FAT12/FAT16 image reader (fat.h) and the SourceFs views over it
// (source.h): the 1.44 MB and 2.88 MB floppy geometries, FAT16, a fragmented
// chain, subdirectories, the entries the reader must skip (long names,
// deleted, volume label, "." and ".."), and every damaged shape PACKAGES.md
// §8.2 says to refuse. Then content sniffing (ISO first, FAT second) and the
// union of two floppy images.
#include <functional>
#include <map>

#include "fat.h"
#include "fat_builder.h"
#include "iso_builder.h"
#include "source.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> text(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> read(const FatImage& img, const FatEntry& e) {
  std::vector<uint8_t> out;
  img.read(e, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
  return out;
}

const FatEntry* find(const std::vector<FatEntry>& v, const std::string& name) {
  for (const FatEntry& e : v)
    if (e.name == name) return &e;
  return nullptr;
}

bool refused(const fs::path& p, const char* what) {
  try {
    FatImage img(p);
    fprintf(stderr, "  %s: opened, should have been refused\n", what);
    return false;
  } catch (const FatError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    return true;
  }
}

bool read_refused(const fs::path& p, const std::string& file, const char* what) {
  try {
    FatImage img(p);
    auto root = img.list(img.root());
    const FatEntry* e = find(root, file);
    if (!e) {
      fprintf(stderr, "  %s: %s not listed\n", what, file.c_str());
      return false;
    }
    read(img, *e);
    fprintf(stderr, "  %s: read, should have been refused\n", what);
    return false;
  } catch (const FatError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    return true;
  }
}

// The standard test volume: files of several sizes, one fragmented, a
// subdirectory tree, and the raw entries a reader must skip.
test::FatBuilder standard(test::FatBuilder b) {
  b.label = "TESTVOL";
  b.file("INSTALL.INS", test::pattern(5000, 1));
  b.file("SETUP.PKG", text("disk list"));
  b.file("EMPTY.TXT", {});
  b.file("BIG.ZIP", test::pattern(40000, 2), /*fragment=*/true);
  b.file("AFTER.ZIP", test::pattern(3000, 3));
  b.file("SUB/INNER.DAT", test::pattern(1500, 4));
  b.file("SUB/DEEPER/LEAF.BIN", test::pattern(700, 5));
  b.file("\xE5" "LEAD.TXT", text("leading E5"));  // stored with the 0x05 escape
  b.root_raw.push_back(test::FatBuilder::raw_entry("DELETED TXT", 0x20, 0xE5));
  b.root_raw.push_back(test::FatBuilder::raw_entry("Aa long nam", 0x0F));
  b.node("AFTER.ZIP").raw_before.push_back(test::FatBuilder::raw_entry("Bb long nam", 0x0F));
  return b;
}

void check_standard(const fs::path& p, int bits) {
  FatImage img(p);
  CHECK_EQ(img.fat_bits(), bits);
  CHECK_EQ(img.volume_label(), std::string("TESTVOL"));
  auto root = img.list(img.root());
  std::vector<std::string> names;
  for (auto& e : root) names.push_back(e.name);
  // Label, deleted and LFN entries are skipped; the rest in on-disk order.
  CHECK((names == std::vector<std::string>{"INSTALL.INS", "SETUP.PKG", "EMPTY.TXT", "BIG.ZIP", "AFTER.ZIP", "SUB",
                                          "\xCF\x83LEAD.TXT"}));
  CHECK(read(img, *find(root, "INSTALL.INS")) == test::pattern(5000, 1));
  CHECK(read(img, *find(root, "EMPTY.TXT")).empty());
  CHECK(read(img, *find(root, "BIG.ZIP")) == test::pattern(40000, 2));
  CHECK(read(img, *find(root, "AFTER.ZIP")) == test::pattern(3000, 3));
  CHECK(read(img, *find(root, "\xCF\x83LEAD.TXT")) == text("leading E5"));
  const FatEntry* sub = find(root, "SUB");
  CHECK(sub && sub->is_dir);
  if (sub) {
    auto inner = img.list(*sub);  // "." and ".." skipped
    CHECK_EQ(inner.size(), size_t(2));
    const FatEntry* f = find(inner, "INNER.DAT");
    CHECK(f && read(img, *f) == test::pattern(1500, 4));
    const FatEntry* d = find(inner, "DEEPER");
    if (d) {
      auto deeper = img.list(*d);
      CHECK(deeper.size() == 1 && deeper[0].name == "LEAF.BIN" && read(img, deeper[0]) == test::pattern(700, 5));
    } else {
      CHECK(d != nullptr);
    }
  }
  // DOS date and time come through.
  const FatEntry* ins = find(root, "INSTALL.INS");
  CHECK(ins && ins->dos_date == 0x1CF1 && ins->dos_time == 0x7A00);
}

}  // namespace

int main(int argc, char** argv) {
  fs::path dir = test::scratch(argc, argv, "adw-import-fat");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder

  // ---- geometries ----------------------------------------------------------------------
  {
    auto b = standard(test::FatBuilder::floppy144());
    test::write_bytes(dir / L"f144.img", b.build());
    CHECK_EQ(fs::file_size(dir / L"f144.img"), uint64_t(1474560));
    check_standard(dir / L"f144.img", 12);
  }
  {
    auto b = standard(test::FatBuilder::floppy288());
    test::write_bytes(dir / L"f288.img", b.build());
    CHECK_EQ(fs::file_size(dir / L"f288.img"), uint64_t(2949120));
    check_standard(dir / L"f288.img", 12);
    FatImage img(dir / L"f288.img");
    CHECK(img.sectors_per_cluster() == 2 && img.bytes_per_sector() == 512 && img.media() == 0xF0);
  }
  {
    // 16 MB with 4-sector clusters: over 4084 clusters, so FAT16.
    test::FatBuilder b;
    b.spc = 4;
    b.root_entries = 512;
    b.total_sectors = 32768;
    b.spf = 32;
    b.media = 0xF8;
    auto s = standard(std::move(b));
    CHECK(s.fat16());
    test::write_bytes(dir / L"f16.img", s.build());
    check_standard(dir / L"f16.img", 16);
  }
  {
    // Larger sectors: 1024 bytes each.
    test::FatBuilder b;
    b.bps = 1024;
    b.total_sectors = 1440;
    b.spf = 5;
    b.root_entries = 224;
    auto s = standard(std::move(b));
    test::write_bytes(dir / L"f1k.img", s.build());
    check_standard(dir / L"f1k.img", 12);
  }

  // ---- damaged images: refused ------------------------------------------------------------
  auto base = standard(test::FatBuilder::floppy144());
  const auto good = base.build();
  const uint32_t big = base.node("BIG.ZIP").clusters[0];
  const auto& big_chain = base.node("BIG.ZIP").clusters;
  auto variant = [&](const wchar_t* name, const std::function<void(std::vector<uint8_t>&)>& damage) {
    auto img = good;
    damage(img);
    test::write_bytes(dir / name, img);
    return dir / name;
  };
  CHECK(refused(variant(L"no-sig.img", [](auto& v) { v[510] = 0; }), "no 55 AA"));
  CHECK(refused(variant(L"bps0.img", [](auto& v) { v[11] = v[12] = 0; }), "bytes/sector 0"));
  CHECK(refused(variant(L"bps300.img", [](auto& v) { v[11] = 0x2C, v[12] = 0x01; }), "bytes/sector 300"));
  CHECK(refused(variant(L"spc3.img", [](auto& v) { v[13] = 3; }), "sectors/cluster 3"));
  CHECK(refused(variant(L"res0.img", [](auto& v) { v[14] = v[15] = 0; }), "no reserved sectors"));
  CHECK(refused(variant(L"fats3.img", [](auto& v) { v[16] = 3; }), "three FATs"));
  CHECK(refused(variant(L"root0.img", [](auto& v) { v[17] = v[18] = 0; }), "no root entries"));
  CHECK(refused(variant(L"spf0.img", [](auto& v) { v[22] = v[23] = 0; }), "no sectors per FAT"));
  CHECK(refused(variant(L"tot0.img", [](auto& v) { v[19] = v[20] = 0; }), "no sector count"));
  {
    auto img = good;
    img.resize(img.size() - 512);
    test::write_bytes(dir / L"short.img", img);
    CHECK(refused(dir / L"short.img", "shorter than its sectors"));
  }
  CHECK(refused(variant(L"tiny.img", [](auto& v) { v.resize(100); }), "too small"));
  {
    // FAT32-sized: 70000 clusters (a sparse 35 MB file).
    test::FatBuilder b;
    b.total_sectors = 70000;
    b.spf = 540;
    b.root_entries = 512;
    // Only the boot sector matters for this refusal: write it into a file of the right size.
    auto img = b.build();
    HANDLE h = CreateFileW((dir / L"fat32ish.img").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    WriteFile(h, img.data(), 512 * 64, &w, nullptr);
    LARGE_INTEGER sz;
    sz.QuadPart = int64_t(img.size());
    SetFilePointerEx(h, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
    CHECK(refused(dir / L"fat32ish.img", "FAT32-sized"));
  }
  // Chains: the image opens (the root lists), reading the file is refused.
  {
    test::FatBuilder& b = base;
    auto img = good;
    b.set_fat(img, big_chain[2], big_chain[1]);  // loops back
    test::write_bytes(dir / L"loop.img", img);
    CHECK(read_refused(dir / L"loop.img", "BIG.ZIP", "chain loop"));
    img = good;
    b.set_fat(img, big_chain[3], 0);  // free cluster inside the chain
    test::write_bytes(dir / L"free.img", img);
    CHECK(read_refused(dir / L"free.img", "BIG.ZIP", "free cluster in chain"));
    img = good;
    b.set_fat(img, big_chain[3], 0xFF7);  // bad cluster
    test::write_bytes(dir / L"bad.img", img);
    CHECK(read_refused(dir / L"bad.img", "BIG.ZIP", "bad cluster in chain"));
    img = good;
    b.set_fat(img, big_chain[3], 4000);  // beyond the last cluster
    test::write_bytes(dir / L"range.img", img);
    CHECK(read_refused(dir / L"range.img", "BIG.ZIP", "out-of-range cluster"));
    img = good;
    b.set_fat(img, big_chain[5], 0xFFF);  // ends early
    test::write_bytes(dir / L"shortchain.img", img);
    CHECK(read_refused(dir / L"shortchain.img", "BIG.ZIP", "chain shorter than the file"));
    img = good;
    size_t e = base.node("AFTER.ZIP").entry_offset;
    img[e + 26] = img[e + 27] = 0;  // non-empty file without clusters
    test::write_bytes(dir / L"nocluster.img", img);
    CHECK(read_refused(dir / L"nocluster.img", "AFTER.ZIP", "no clusters"));
    // The damage is local: every other file still reads.
    FatImage ok(dir / L"loop.img");
    auto root = ok.list(ok.root());
    CHECK(read(ok, *find(root, "AFTER.ZIP")) == test::pattern(3000, 3));
    (void)big;
  }

  // ---- SourceFs: sniffing, FAT view, union of two floppies -------------------------------------
  {
    auto fat = open_image(dir / L"f288.img");
    CHECK_EQ(fat->format(), std::string("fat12"));
    CHECK_EQ(fat->volume_id(), std::string("TESTVOL"));
    auto n = fat->find("sub/deeper/leaf.bin");
    CHECK(n && fat->read_all(*n) == test::pattern(700, 5));
    CHECK(n && n->mtime.has_value());
    CHECK(!fat->find("SUB/NOPE"));
    test::IsoBuilder ib;
    ib.file("INSTALL/X.TXT", text("iso"));
    test::write_bytes(dir / L"small.iso", ib.build());
    auto iso = open_image(dir / L"small.iso");
    CHECK_EQ(iso->format(), std::string("iso9660+joliet"));
    test::write_bytes(dir / L"junk.img", test::pattern(3000000, 11));
    bool threw = false;
    try {
      open_image(dir / L"junk.img");
    } catch (const ImportError& e) {
      threw = e.status() == Status::source_invalid;
      fprintf(stderr, "  junk image -> %s\n", e.what());
    }
    CHECK(threw);
  }
  {
    test::FatBuilder d1 = test::FatBuilder::floppy144(), d2 = test::FatBuilder::floppy144();
    d1.file("DISK.1", text("1"));
    d1.file("SHARED.PKG", text("same bytes"));
    d1.file("ONE.ZIP", test::pattern(2000, 21));
    d1.file("DIR/A.TXT", text("a"));
    d2.file("DISK.2", text("2"));
    d2.file("SHARED.PKG", text("same bytes"));
    d2.file("TWO.ZIP", test::pattern(2000, 22));
    d2.file("DIR/B.TXT", text("b"));
    d2.file("CLASH.TXT", text("xxxx"));
    d1.file("CLASH.TXT", text("yyyy"));  // same size, different bytes
    d2.file("SIZED.TXT", text("12345"));
    d1.file("SIZED.TXT", text("123"));
    test::write_bytes(dir / L"disk1.img", d1.build());
    test::write_bytes(dir / L"disk2.img", d2.build());
    std::vector<std::unique_ptr<SourceFs>> parts;
    parts.push_back(open_image(dir / L"disk1.img"));
    parts.push_back(open_image(dir / L"disk2.img"));
    auto u = union_of(std::move(parts));
    bool size_threw = false;
    try {
      u->list(u->root());
    } catch (const ImportError& e) {
      size_threw = true;
      fprintf(stderr, "  union, same name different size -> %s\n", e.what());
    }
    CHECK(size_threw);
    // Without the size clash: the listing merges, the byte clash shows on read.
    test::FatBuilder e1 = test::FatBuilder::floppy144(), e2 = test::FatBuilder::floppy144();
    e1.file("DISK.1", text("1"));
    e1.file("SHARED.PKG", text("same bytes"));
    e1.file("DIR/A.TXT", text("a"));
    e1.file("CLASH.TXT", text("yyyy"));
    e2.file("DISK.2", text("2"));
    e2.file("SHARED.PKG", text("same bytes"));
    e2.file("DIR/B.TXT", text("b"));
    e2.file("CLASH.TXT", text("xxxx"));
    test::write_bytes(dir / L"e1.img", e1.build());
    test::write_bytes(dir / L"e2.img", e2.build());
    std::vector<std::unique_ptr<SourceFs>> p2;
    p2.push_back(open_image(dir / L"e1.img"));
    p2.push_back(open_image(dir / L"e2.img"));
    auto v = union_of(std::move(p2));
    auto root = v->list(v->root());
    CHECK_EQ(root.size(), size_t(5));  // DISK.1, SHARED.PKG, DIR, CLASH.TXT, DISK.2
    CHECK(v->find("DISK.2") && v->find("DIR/A.TXT") && v->find("DIR/B.TXT"));
    CHECK(v->read_all(*v->find("SHARED.PKG")) == text("same bytes"));
    bool byte_threw = false;
    try {
      v->read_all(*v->find("CLASH.TXT"));
    } catch (const ImportError& e) {
      byte_threw = true;
      fprintf(stderr, "  union, same name different bytes -> %s\n", e.what());
    }
    CHECK(byte_threw);
  }
  // ---- directory identity (SourceFs::dir_key) --------------------------------------------------
  // Two entries whose first cluster is the same directory are one directory
  // listed twice (the importer refuses that); distinct ones differ, and a
  // union of images keys a directory by every image's copy.
  {
    test::FatBuilder a = test::FatBuilder::floppy144();
    a.file("ONE/X.TXT", text("x"));
    a.file("TWO/Y.TXT", text("y"));
    a.file("THREE/Z.TXT", text("z"));
    auto img = a.build();
    const uint32_t one = a.node("ONE").clusters.at(0);
    const size_t two = a.node("TWO").entry_offset;
    img[two + 26] = uint8_t(one), img[two + 27] = uint8_t(one >> 8);
    test::write_bytes(dir / L"alias.img", img);
    auto fsrc = open_image(dir / L"alias.img");
    std::map<std::string, std::string> key;
    for (const SourceNode& n : fsrc->list(fsrc->root())) key[n.name] = fsrc->dir_key(n);
    CHECK(!key["ONE"].empty());
    CHECK_EQ(key["ONE"], key["TWO"]);
    CHECK(key["ONE"] != key["THREE"]);
    CHECK(fsrc->dir_key(fsrc->root()) != key["ONE"]);
    auto x = fsrc->find("ONE/X.TXT");
    CHECK(x && fsrc->dir_key(*x).empty());  // a file has no directory key

    std::vector<std::unique_ptr<SourceFs>> parts;
    parts.push_back(open_image(dir / L"alias.img"));
    parts.push_back(open_image(dir / L"f288.img"));
    auto u = union_of(std::move(parts));
    std::map<std::string, std::string> ukey;
    for (const SourceNode& n : u->list(u->root()))
      if (n.is_dir) ukey[n.name] = u->dir_key(n);
    CHECK(!ukey["ONE"].empty());
    CHECK_EQ(ukey["ONE"], ukey["TWO"]);
    CHECK(ukey["ONE"] != ukey["THREE"]);
    CHECK(ukey["SUB"] != ukey["ONE"]);
  }
  return test::finish("import.fat");
}
