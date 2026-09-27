// IsoImage against synthetic images: every variant (Joliet or not, ";1" or
// not, one-sided both-endian fields, raw 2352-byte sectors, extents stored out
// of order) must list, find and read the fixture byte-for-byte, through plain
// reads and through the sector-aligned bounce buffer a raw CD volume needs.
#include "fixture.h"
#include "iso9660.h"

using namespace adw::import;
using test::IsoBuilder;

namespace {

// The whole file (the reader streams only; fixtures are small).
std::vector<uint8_t> read_all(const IsoImage& iso, const IsoEntry& e) {
  std::vector<uint8_t> out;
  iso.read(e, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
  return out;
}

struct Variant {
  const char* name;
  bool joliet, version, raw, reverse;
  IsoBuilder::Endian endian;
};

void check_variant(const std::filesystem::path& dir, const Variant& v, bool aligned) {
  fprintf(stderr, "variant %s%s\n", v.name, aligned ? " (aligned reads)" : "");
  IsoBuilder b;
  b.joliet = v.joliet;
  b.version_suffix = v.version;
  b.raw2352 = v.raw;
  b.reverse_extents = v.reverse;
  b.endian = v.endian;
  auto files = test::build_fixture(b);
  auto path = dir / (adw::import::to_wide(v.name) + L".iso");
  auto img = b.build();
  test::write_bytes(path, img);

  IsoImage iso(path, aligned);
  CHECK_EQ(iso.joliet(), v.joliet);
  CHECK_EQ(iso.raw_sectors(), v.raw);
  CHECK_EQ(iso.block_size(), 2048u);
  CHECK_EQ(iso.volume_id(), std::string("AD_TEST"));
  CHECK_EQ(iso.file_size(), uint64_t(img.size()));

  for (const test::FixtureFile& f : files) {
    auto e = iso.find(f.iso_path);
    CHECK(e.has_value());
    if (!e) {
      fprintf(stderr, "  not found: %s\n", f.iso_path.c_str());
      continue;
    }
    CHECK(!e->is_dir);
    CHECK_EQ(e->size, uint64_t(f.data.size()));
    CHECK(read_all(iso, *e) == f.data);
  }

  // Multi-extent file: three extents, whatever order they sit on disc.
  auto big = iso.find("ade/files/ad40/big.dat");  // lookups are case-insensitive
  CHECK(big && big->extents.size() == 3);
  if (big && v.reverse) CHECK(big->extents[0].lba > big->extents[2].lba);

  // The associated record for FLYINGTO.AD is not listed; the data file is.
  auto ad40 = iso.find("ADE/FILES/AD40");
  CHECK(ad40 && ad40->is_dir);
  if (ad40) {
    auto kids = iso.list(*ad40);
    int toasters = 0;
    for (auto& k : kids) toasters += k.short_name == "FLYINGTO.AD";
    CHECK_EQ(toasters, 1);
    CHECK_EQ(kids.size(), size_t(4));  // ADXPL510.DLL, BIG.DAT, FLYINGTO.AD, PICTURES
  }

  // A directory that spans sectors lists completely.
  auto classic = iso.find("ADE/FILES/CLASSIC");
  CHECK(classic && classic->size > 2048);
  if (classic) CHECK_EQ(iso.list(*classic).size(), size_t(80 + 2));

  auto readme = iso.find("ADE/FILES/ENGINE/README");
  CHECK(readme && readme->name == "README");

  auto t = iso.find("ADE/FILES/AFI/AD2.AFI");
  CHECK(t && t->mtime.valid && t->mtime.year == 1996 && t->mtime.month == 9 && t->mtime.day == 12 &&
        t->mtime.hour == 22 && t->mtime.gmt_offset_15min == -28);

  if (v.joliet) {
    // Joliet names are the listing names; the 8.3 twin rides along.
    auto toast = iso.find("ADE/FILES/AD40/Flying Toasters.ad");
    CHECK(toast.has_value());
    if (toast) {
      CHECK_EQ(toast->name, std::string("Flying Toasters.ad"));
      CHECK_EQ(toast->short_name, std::string("FLYINGTO.AD"));
    }
    auto pics = iso.find("ADE/FILES/AD40/PICTURES");  // found through the short name
    CHECK(pics.has_value());
    if (pics) {
      CHECK_EQ(pics->name, std::string("Pictures Folder"));
      CHECK_EQ(pics->short_name, std::string("PICTURES"));
      auto kids = iso.list(*pics);
      CHECK(kids.size() == 1 && kids[0].short_name == "PIC1.BMP");
    }
    auto hlp = iso.find("ADE/FILES/ENGINE/ADPAGE.HLP");
    CHECK(hlp && hlp->name == "adpage.hlp" && hlp->short_name == "ADPAGE.HLP");
    auto empty = iso.find("ADE/FILES/ENGINE/EMPTY.TXT");
    CHECK(empty && empty->short_name == "EMPTY.TXT" && empty->size == 0);
  } else {
    auto toast = iso.find("ADE/FILES/AD40/FLYINGTO.AD");
    CHECK(toast && toast->name == "FLYINGTO.AD" && toast->short_name == "FLYINGTO.AD");
    CHECK(!iso.find("ADE/FILES/AD40/Flying Toasters.ad"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  auto dir = test::scratch(argc, argv, "adw-import-iso");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  const Variant variants[] = {
      {"joliet", true, true, false, false, IsoBuilder::Endian::both},
      {"primary_only", false, true, false, false, IsoBuilder::Endian::both},
      {"no_version", true, false, false, false, IsoBuilder::Endian::both},
      {"le_only", true, true, false, false, IsoBuilder::Endian::le_only},
      {"be_only", false, true, false, true, IsoBuilder::Endian::be_only},
      {"raw2352", true, true, true, true, IsoBuilder::Endian::both},
      {"reversed_extents", true, true, false, true, IsoBuilder::Endian::both},
  };
  for (const Variant& v : variants) {
    for (bool aligned : {false, true}) {
      try {
        check_variant(dir, v, aligned);
      } catch (const std::exception& e) {
        test::g_failures++;
        fprintf(stderr, "variant %s threw: %s\n", v.name, e.what());
      }
    }
  }

  // Joliet directories whose names match neither their 8.3 twins nor each
  // other's order, two in one parent (so pairing by elimination cannot
  // decide): each must pair with the primary directory holding the same file
  // data, and its children must carry that directory's 8.3 names.
  try {
    IsoBuilder b;
    b.dir("ADE/FILES/AD40/ZSOUNDS", "A Sounds");  // Joliet sorts it first, the primary tree last
    b.dir("ADE/FILES/AD40/APICS", "B Pictures");
    b.file("ADE/FILES/AD40/ZSOUNDS/S1.WAV", test::pattern(3000, 21), "first sound.wav");
    b.file("ADE/FILES/AD40/ZSOUNDS/S2.WAV", test::pattern(2500, 22), "second sound.wav");
    b.file("ADE/FILES/AD40/APICS/P1.BMP", test::pattern(5000, 23), "first picture.bmp");
    b.file("ADE/FILES/AD40/ADXPL510.DLL", test::pattern(100, 24));
    test::write_bytes(dir / L"joliet-dirs.iso", b.build());
    IsoImage iso(dir / L"joliet-dirs.iso");
    CHECK(iso.joliet());
    auto snd = iso.find("ADE/FILES/AD40/ZSOUNDS");
    auto pic = iso.find("ADE/FILES/AD40/APICS");
    CHECK(snd && snd->name == "A Sounds" && snd->short_name == "ZSOUNDS");
    CHECK(pic && pic->name == "B Pictures" && pic->short_name == "APICS");
    if (snd) {
      auto kids = iso.list(*snd);
      CHECK(kids.size() == 2 && kids[0].short_name == "S1.WAV" && kids[1].short_name == "S2.WAV");
    }
    if (pic) {
      auto kids = iso.list(*pic);
      CHECK(kids.size() == 1 && kids[0].short_name == "P1.BMP" && kids[0].name == "first picture.bmp");
    }
  } catch (const std::exception& e) {
    test::g_failures++;
    fprintf(stderr, "joliet-dirs threw: %s\n", e.what());
  }

  // Not an image at all.
  test::write_bytes(dir / L"garbage.iso", test::pattern(100000, 9));
  bool threw = false;
  try {
    IsoImage x(dir / L"garbage.iso");
  } catch (const IsoError&) {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try {
    IsoImage x(dir / L"does-not-exist.iso");
  } catch (const IsoError&) {
    threw = true;
  }
  CHECK(threw);

  // A truncated image (an interrupted download) opens, but reading a file
  // past the cut fails loudly instead of returning short data.
  {
    IsoBuilder b;
    auto files = test::build_fixture(b);
    auto img = b.build();
    img.resize(img.size() - 4096);
    test::write_bytes(dir / L"truncated.iso", img);
    IsoImage iso(dir / L"truncated.iso");
    size_t failed = 0;
    for (auto& f : files) {
      try {
        auto e = iso.find(f.iso_path);
        if (e) read_all(iso, *e);
      } catch (const IsoError&) {
        failed++;
      }
    }
    CHECK(failed >= 1);
  }
  return test::finish("import.iso");
}
