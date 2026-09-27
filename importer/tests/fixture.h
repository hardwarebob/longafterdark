// The synthetic "Deluxe-shaped" disc every importer test uses: the four
// imported folders with nested directories, a multi-extent file, an
// associated (resource-fork) record, a directory spanning several sectors,
// empty / extension-less / level-2 names, Joliet names that differ from the
// 8.3 ones — and files outside ADE\FILES\{AD40,CLASSIC,ENGINE,AFI} that an
// import must leave behind.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "iso_builder.h"
#include "test_util.h"

namespace test {

struct FixtureFile {
  std::string iso_path;  // primary-volume path on the disc
  std::string out;       // expected path under <assets>\win ("" = must not be imported)
  std::vector<uint8_t> data;
};

inline std::vector<FixtureFile> build_fixture(IsoBuilder& b) {
  std::vector<FixtureFile> v;
  uint32_t seed = 1;
  auto add = [&](const std::string& iso, bool imported, size_t size, const std::string& joliet = "") {
    FixtureFile f{iso, imported ? "FILES/" + iso.substr(strlen("ADE/FILES/")) : "", pattern(size, seed++)};
    b.file(iso, f.data, joliet);
    v.push_back(std::move(f));
  };
  b.dir("ADE/FILES/AD40/PICTURES", "Pictures Folder");
  add("ADE/FILES/AD40/ADXPL510.DLL", true, 5000);
  add("ADE/FILES/AD40/FLYINGTO.AD", true, 7000, "Flying Toasters.ad");
  b.file("ADE/FILES/AD40/FLYINGTO.AD", v.back().data, "Flying Toasters.ad").associated = true;
  add("ADE/FILES/AD40/BIG.DAT", true, 4096 + 2048 + 1500);
  b.file("ADE/FILES/AD40/BIG.DAT", v.back().data).split = {4096, 2048};
  add("ADE/FILES/AD40/PICTURES/PIC1.BMP", true, 3000);
  add("ADE/FILES/CLASSIC/ADXPL300.DLL", true, 2100);
  add("ADE/FILES/CLASSIC/BITMAPS/X.BMP", true, 10);
  for (int i = 0; i < 80; i++) {
    char n[64];
    snprintf(n, sizeof(n), "ADE/FILES/CLASSIC/F%03d.AD", i);
    add(n, true, 50 + i);
  }
  add("ADE/FILES/ENGINE/OLDMOD16.DLL", true, 4096);
  add("ADE/FILES/ENGINE/EMPTY.TXT", true, 0);
  add("ADE/FILES/ENGINE/README", true, 20);
  add("ADE/FILES/ENGINE/ADPAGE.HLP", true, 900, "adpage.hlp");
  add("ADE/FILES/AFI/AD2.AFI", true, 600);
  add("ADE/FILES/AFI/LEVEL2_LONGER_NAME.AFI", true, 700);
  add("ADE/FILES/WALLPAPR/W.BMP", false, 1234);
  add("ADE/FILES/OTHER.TXT", false, 99);
  add("ADE/SETUP.INF", false, 321);
  add("README.TXT", false, 77);
  return v;
}

inline size_t imported_count(const std::vector<FixtureFile>& v) {
  size_t n = 0;
  for (auto& f : v) n += !f.out.empty();
  return n;
}

// The same tree as plain files under `root` (as a mounted disc shows it).
// `lower` writes some names in lower case, the way a copy made on another
// system might look, to check the import upper-cases them.
inline void write_fixture_folder(const std::filesystem::path& root, const std::vector<FixtureFile>& v, bool lower) {
  for (const FixtureFile& f : v) {
    std::string p = f.iso_path;
    if (lower && p.find("CLASSIC/F0") != std::string::npos)
      for (size_t i = p.rfind('/') + 1; i < p.size(); i++) p[i] = char(tolower(p[i]));
    std::filesystem::path out = root;
    size_t i = 0;
    while (i < p.size()) {
      size_t j = p.find('/', i);
      if (j == std::string::npos) j = p.size();
      out /= adw::import::to_wide(p.substr(i, j - i));
      i = j + 1;
    }
    write_bytes(out, f.data);
  }
}

}  // namespace test
