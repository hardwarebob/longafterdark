// Synthetic sources shaped like the five known After Dark releases
// (PACKAGES.md §2–§4), for the package tests: the Deluxe and 10th
// Anniversary CDs' plain FILES trees, and the AD 3.x InstallShield installs
// (AD 3.2, Totally Twisted, the Simpsons floppies) with encrypted PKZIP
// archives under the test-only password and an INSTALL.INS-like script that
// holds it among decoys. Each fixture is a tree (path -> bytes) that can be
// written as a folder, an ISO image or a FAT floppy image, plus exactly what
// an import must install and the catalog ids it must list. Modules come from
// module_builder.h: made-up resources, no After Dark bytes.
#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "fat_builder.h"
#include "importer.h"
#include "iso_builder.h"
#include "md5.h"
#include "module_builder.h"
#include "test_util.h"
#include "zip_builder.h"

namespace test {

using Tree = std::map<std::string, std::vector<uint8_t>>;

inline std::vector<uint8_t> vec(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// An NE (Classic) module named `name` importing `refs`.
inline std::vector<uint8_t> ne_module(const std::string& name, const std::vector<std::string>& refs) {
  NeSpec ne;
  ne.module_refs = refs;
  ne.resources = {{2000, "", 20, name + '\0'},
                  {2000, "", 30, "About " + name + "\r\n"},
                  {1000, "", 1, checkbox_record("Sound", 1)}};
  return vec(build_ne(ne));
}

// A PE32 (AD4) module described as `desc`, importing `imports`.
inline std::vector<uint8_t> pe_module(const std::string& desc, const std::vector<std::string>& imports,
                                      bool msvc = false) {
  PeSpec pe;
  pe.imports = imports;
  pe.exports = {msvc ? "_Module@4" : "Module"};
  pe.resources = {{16, "", 1, 0x409, version_resource({{"FileDescription", desc}})},
                  {1000, "", 1, 0x409, checkbox_record("Clear Screen", 1)}};
  return vec(build_pe(pe));
}

inline std::vector<uint8_t> blob(const std::string& tag, size_t n = 600) {
  std::vector<uint8_t> v = vec(tag + ":");
  auto p = pattern(n, uint32_t(std::hash<std::string>{}(tag)));
  v.insert(v.end(), p.begin(), p.end());
  return v;
}

// Modules two releases share byte for byte (catalog sameAs).
inline const std::vector<uint8_t>& shared_classic() {
  static const auto v = ne_module("Same Module", {"KERNEL"});
  return v;
}
inline const std::vector<uint8_t>& shared_starry() {
  static const auto v = pe_module("Starry Night Display", {"KERNEL32.DLL"}, true);
  return v;
}
inline const std::vector<uint8_t>& shared_toilet40() {
  static const auto v = ne_module("Flying Toilets", {"KERNEL", "ADXPL40"});
  return v;
}

// An InstallShield-like script: length-prefixed strings among opcode bytes,
// the password (when given) right after the unzip failure message.
inline std::vector<uint8_t> install_ins(const std::string& password) {
  std::vector<uint8_t> s = {0xFF, 0xFF, 0x0C, 0x00};
  auto lp = [&](const std::string& t) {
    s.push_back(uint8_t(t.size()));
    s.push_back(uint8_t(t.size() >> 8));
    s.insert(s.end(), t.begin(), t.end());
    s.push_back(0x2E);  // a printable opcode right after, as the real scripts have
  };
  for (const char* d : {"SRCDIR", "TARGETDIR", "Welcome to setup", "C:\\AFTERDRK", "Decompressing engine..."}) lp(d);
  lp("Cannot initialize for unzip!");
  s.insert(s.end(), {0x03, 0x00, 0xFF, 0xFF, 0x0E, 0x00});
  if (!password.empty()) lp(password);
  for (const char* d : {"engine.zip", "Creating module folders", "folder.afi", "MUSICG.ZIP"}) lp(d);
  return s;
}

struct PkgFixture {
  Tree source;                        // as on the medium ('/'-separated)
  std::map<std::string, std::string> joliet;  // source path -> Joliet name (ISO only)
  Tree expect;                        // installed files, relative to <win> (import.json excluded)
  std::vector<std::string> ids;       // catalog ids, in catalog order
};

// ---- the plain CDs ----------------------------------------------------------------

inline PkgFixture deluxe_fixture() {
  PkgFixture f;
  auto both = [&](const std::string& rel, std::vector<uint8_t> d) {
    f.source["ADE/FILES/" + rel] = d;
    f.expect["FILES/" + rel] = std::move(d);
  };
  both("AD40/ADXPL510.DLL", blob("deluxe ADXPL510"));
  both("AD40/BADDOG.AD", pe_module("Bad Dog!", {"ADXPL510.DLL", "KERNEL32.DLL"}));
  both("AD40/TOASTERS.AD", pe_module("Flying Toasters!", {"ADXPL510.DLL", "USER32.DLL"}));
  both("AD40/TOASTERS.MID", blob("deluxe toasters mid"));
  both("CLASSIC/ADXPL300.DLL", blob("deluxe ADXPL300"));
  both("CLASSIC/TOILETS.AD", ne_module("Flying Toilets", {"KERNEL", "ADXPL300"}));
  both("CLASSIC/SAMEMOD.AD", shared_classic());
  both("ENGINE/OLDMOD16.DLL", blob("deluxe OLDMOD16"));
  both("ENGINE/AD_SND.DLL", blob("deluxe AD_SND 4.0"));
  both("ENGINE/AFTERDAR.SCR", blob("deluxe AFTERDAR"));
  both("ENGINE/STARRYNI.AD", shared_starry());
  both("AFI/AD2.AFI", blob("deluxe AD2.AFI"));
  f.source["ADE/FILES/WALLPAPR/W.BMP"] = blob("wallpaper");
  f.source["ADE/SETUP.INF"] = blob("setup inf");
  f.ids = {"ad40.baddog", "ad40.toasters", "ad40.starryni", "classic.samemod", "classic.toilets"};
  return f;
}

// `with_fixups`: the four §4.3 copies are expected (their sources matched).
inline PkgFixture ad10_fixture(bool with_fixups = true) {
  PkgFixture f;
  auto both = [&](const std::string& rel, std::vector<uint8_t> d) {
    f.source["ADE/FILES/" + rel] = d;
    f.expect["packages/ad10/" + rel] = std::move(d);
  };
  both("AD10TH/ADXPL510.DLL", blob("ad10 ADXPL510 5.2"));
  both("AD10TH/ADXPL300.DLL", blob("ad10 ADXPL300"));
  both("AD10TH/ADXPL40.DLL", blob("ad10 ADXPL40"));
  both("AD10TH/ADTOOL.DLL", blob("ad10 ADTOOL"));
  both("AD10TH/BADDOG.AD", pe_module("Bad Dog!", {"ADXPL510.DLL", "KERNEL32.DLL", "SHELL32.DLL"}));
  both("AD10TH/BADDOG3.AD", ne_module("Bad Dog!", {"KERNEL", "ADXPL300", "ADTOOL"}));
  both("AD10TH/TOAST2K.AD", pe_module("Toasters 2k", {"KERNEL32.DLL", "WINMM.DLL"}, true));
  both("AD10TH/TOASTER2.AD", pe_module("Toasters 2k", {"KERNEL32.DLL"}, true));
  both("AD10TH/TOASTERS.AD", pe_module("Flying Toasters!", {"ADXPL510.DLL"}));
  both("AD10TH/TOILET.AD", shared_toilet40());
  both("AD10TH/TOASTER1.MID", blob("toasters 2k music"));
  both("AD10TH/TOASTERS.MID", blob("flying toasters music"));
  both("AD10TH/BABY.MID", blob("baby toasters music"));
  both("AD10TH/MUSIC/TT_SND.DLL", blob("tt shared sounds"));
  both("AD10TH/MUSIC/DAWN.MID", blob("dawn"));
  both("AD10TH/PICTURES/SOMEPICT.BMP", blob("a picture"));
  f.joliet["ADE/FILES/AD10TH/PICTURES/SOMEPICT.BMP"] = "Some Picture.bmp";
  f.joliet["ADE/FILES/AD10TH/TOASTER2.AD"] = "Toaster 2k.ad";
  both("ENGINE/OLDMOD16.DLL", blob("ad10 OLDMOD16"));
  both("ENGINE/AD_SND.DLL", blob("ad10 AD_SND 4.0"));
  both("ENGINE/AFTERDAR.SCR", blob("ad10 AFTERDAR"));
  both("ENGINE/STARRYNI.AD", shared_starry());
  both("AFI/AD2.AFI", blob("ad10 AD2.AFI"));
  f.source["ADE/FILES/GAMES/BAD_DOG.EXE"] = blob("a game");
  f.source["ADE/FILES/WALLPAPR/W.BMP"] = blob("wallpaper 10");
  f.source["ADE/SETUP.INF"] = blob("setup inf 10");
  f.source["DIRECTX6/DX.EXE"] = blob("directx");
  if (with_fixups) {
    f.expect["packages/ad10/AD10TH/TT_SND.DLL"] = f.expect["packages/ad10/AD10TH/MUSIC/TT_SND.DLL"];
    f.expect["packages/ad10/AD10TH/MUSIC/Toasters2k.mid"] = f.expect["packages/ad10/AD10TH/TOASTER1.MID"];
    f.expect["packages/ad10/AD10TH/MUSIC/Flying Toasters.mid"] = f.expect["packages/ad10/AD10TH/TOASTERS.MID"];
    f.expect["packages/ad10/AD10TH/MUSIC/Baby Toasters.mid"] = f.expect["packages/ad10/AD10TH/BABY.MID"];
  }
  f.ids = {"ad10.baddog", "ad10.baddog3", "ad10.toast2k", "ad10.toaster2", "ad10.toasters", "ad10.toilet",
           "ad10.starryni"};
  return f;
}

// ---- the AD 3.x InstallShield installs --------------------------------------------------

struct Ad3Parts {
  std::string install;  // "INSTALL" or "" (floppy root)
  std::string root;     // "packages/<id>"
  std::string mdir;     // module dir
  std::string password = kTestZipPassword;
  PkgFixture f;

  std::string src(const std::string& name) const { return install.empty() ? name : install + "/" + name; }
  void zip(const std::string& name, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members,
           bool stored_small = true) {
    ZipBuilder b;
    b.password = password;
    for (const auto& [n, d] : members) b.add(n, d, /*deflate=*/!(stored_small && d.size() < 64));
    f.source[src(name)] = b.build();
  }
  void expect(const std::string& rel, const std::vector<uint8_t>& d) { f.expect[root + "/" + rel] = d; }
  void engine_zip() {
    auto snd = blob(root + " AD_SND 3.x"), task = blob(root + " ADTASK"), exe = blob(root + " ADW30.EXE"),
         ini = blob(root + " ADW30.INI", 60), eco = blob(root + " ECOLOGIC");
    zip("ENGINE.ZIP", {{"ADSETUP.DLL", blob("adsetup")},
                       {"AD_SND.DLL", snd},
                       {"ADTASK.DLL", task},
                       {"ADW30.EXE", exe},
                       {"ADW30.INI", ini},
                       {"ECOLOGIC.DLL", eco},
                       {"ADHOOK.DLL", blob("adhook")},
                       {"MULTI.AM3", pattern(16, 77)},
                       {"RANDOM.AR3", pattern(21, 78)}});
    for (auto& [n, d] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
             {"AD_SND.DLL", snd}, {"ADTASK.DLL", task}, {"ADW30.EXE", exe}, {"ADW30.INI", ini}, {"ECOLOGIC.DLL", eco}})
      expect("ENGINE/" + n, d);
  }
  void installer_files(const std::vector<std::string>& disks) {
    f.source[src("INSTALL.INS")] = install_ins(password);
    f.source[src("SETUP.PKG")] = vec("[Package]\r\nFiles=...\r\n");
    f.source[src("SETUP.EXE")] = blob("installshield launcher");
    for (const auto& d : disks) f.source[src(d)] = vec("After Dark " + d);
  }
  // A module archive: every member goes beside the modules.
  void module(const std::string& zipname, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
    zip(zipname, members);
    for (const auto& [n, d] : members) expect(mdir + "/" + n, d);
  }
};

inline PkgFixture ad32_fixture() {
  Ad3Parts p;
  p.install = "INSTALL";
  p.root = "packages/ad32";
  p.mdir = "AD32";
  p.installer_files({"DISK.1", "DISK.CD"});
  p.engine_zip();
  auto xpl = blob("ad32 ADXPL300"), tool = blob("ad32 ADTOOL"), adc = blob("bitmaps adc"), rsrc = blob("AD_RSRC");
  p.zip("MODMISC.ZIP", {{"ADXPL300.DLL", xpl}, {"ADTOOL.DLL", tool}, {"BITMAPS.ADC", adc}, {"EDITFILE.TXT", blob("edit")}});
  p.expect("AD32/ADXPL300.DLL", xpl);
  p.expect("AD32/ADTOOL.DLL", tool);
  p.expect("AD32/BITMAPS.ADC", adc);
  p.zip("WIN.ZIP", {{"AD_RSRC.DLL", rsrc}, {"UNLINK.EXE", blob("unlink")}});
  p.expect("AD32/AD_RSRC.DLL", rsrc);
  auto logo = blob("adlogo"), trc = blob("diamond trace"), ding = blob("ding wav"), mg = blob("omtw gm"),
       afi = blob("AD3.AFI");
  p.zip("BITMAPS.ZIP", {{"ADLOGO.BMP", logo}});
  p.expect("AD32/BITMAPS/ADLOGO.BMP", logo);
  p.zip("TRACES.ZIP", {{"DIAMOND.TRC", trc}});
  p.expect("AD32/TRACES/DIAMOND.TRC", trc);
  p.zip("SOUNDS.ZIP", {{"DING.WAV", ding}});
  p.expect("AD32/SOUNDS/DING.WAV", ding);
  p.zip("MUSIC.ZIP", {{"OMTW.MID", blob("omtw dual format")}});
  p.zip("MUSICG.ZIP", {{"OMTW.MID", mg}});
  p.expect("AD32/MUSIC/OMTW.MID", mg);
  p.zip("AFI.ZIP", {{"MAD.AFI", blob("MAD.AFI")}, {"AD3.AFI", afi}});
  p.expect("AD32/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"ADW30.HLP", blob("help")}});
  p.zip("MULTIS.ZIP", {{"CLOCK_AT.AM3", blob("multi")}});
  p.zip("WINSYS.ZIP", {{"PLACE.TXT", pattern(30, 5)}});
  p.zip("EXTRA.ZIP", {{"README.TXT", blob("not part of the recipe")}});
  p.module("TOILET.ZIP", {{"TOILET.AD", ne_module("Flying Toilets", {"KERNEL", "ADXPL300", "ADTOOL"})}});
  p.module("BORIS.ZIP", {{"BORIS.AD", ne_module("Boris", {"KERNEL", "AD_RSRC", "AD_SND"})}});
  p.module("BORISB.ZIP", {{"BORISB.AD", ne_module("Boris", {"KERNEL"})}});
  p.module("GUTS.ZIP", {{"GUTS.AD", ne_module("Guts", {"KERNEL"})}});
  p.module("GUTS2.ZIP", {{"GUTS2.AD", ne_module("guts", {"KERNEL", "USER"})}});
  p.module("GUTS3.ZIP", {{"GUTS3.AD", ne_module("Guts", {"KERNEL", "GDI"})}});
  p.module("SAME.ZIP", {{"SAME.AD", shared_classic()}});
  p.module("WMORPH.ZIP",
           {{"WMORPH.AD", ne_module("Draw Morph", {"KERNEL"})}, {"MORPH1.DAT", blob("m1")}, {"MORPH2.DAT", blob("m2")}});
  p.f.source["DEMOS/JACK/SETUP.EXE"] = blob("a demo");
  p.f.ids = {"ad32.boris", "ad32.borisb", "ad32.guts", "ad32.guts2", "ad32.guts3", "ad32.same", "ad32.toilet",
             "ad32.wmorph"};
  return p.f;
}

inline PkgFixture tt_fixture() {
  Ad3Parts p;
  p.install = "INSTALL";
  p.root = "packages/tt";
  p.mdir = "TWISTED";
  p.installer_files({"DISK.1", "DISK.2", "DISK.3", "DISK.CD"});
  p.engine_zip();
  auto xpl = blob("tt ADXPL40"), snd = blob("TT_SND"), mg = blob("dawn gm"), rsrc = blob("AD_RSRC"), afi = blob("PHLEM");
  p.zip("MODMISC.ZIP", {{"ADXPL40.DLL", xpl}});
  p.expect("TWISTED/ADXPL40.DLL", xpl);
  p.zip("MUSIC.ZIP", {{"DAWN.MID", blob("dawn dual")}, {"TT_SND.DLL", snd}});
  p.zip("MUSICG.ZIP", {{"DAWN.MID", mg}, {"TT_SND.DLL", snd}});
  p.expect("TWISTED/MUSIC/DAWN.MID", mg);
  p.expect("TWISTED/TT_SND.DLL", snd);
  p.zip("WIN.ZIP", {{"AD_RSRC.DLL", rsrc}, {"UNLINK.EXE", blob("unlink")}});
  p.expect("TWISTED/AD_RSRC.DLL", rsrc);
  p.zip("AFI.ZIP", {{"AD2.AFI", blob("AD2.AFI")}, {"PHLEM.AFI", afi}});
  p.expect("TWISTED/FOLDER.AFI", afi);
  p.zip("WAVEMIX.ZIP", {{"MSACM.DLL", blob("msacm")}});
  p.zip("WINSYS.ZIP", {{"PLACE.TXT", pattern(30, 6)}});
  p.module("TOILET.ZIP", {{"TOILET.AD", shared_toilet40()}});
  p.module("MESY.ZIP", {{"MESSYGES.AD", ne_module("Message Mayhem", {"KERNEL", "ADXPL40"})}});
  p.module("CHAM.ZIP", {{"CHAM.AD", ne_module("Chameleon", {"ADXPL40", "KERNEL", "USER"})}});
  p.f.ids = {"tt.cham", "tt.messyges", "tt.toilet"};
  return p.f;
}

// The Simpsons: both floppies' files in one root (as the merged image has
// them), plus the original owner's notes, which the importer must never read.
struct SimpsonsModule {
  const char* zip;
  const char* file;
  const char* name;
};
inline const std::vector<SimpsonsModule>& simpsons_modules() {
  static const std::vector<SimpsonsModule> v = {
      {"BURNS.ZIP", "BURNS.AD", "Mr. Burns"},          {"CHALKBRD.ZIP", "CHALKBRD.AD", "Chalkboard"},
      {"CLOCKS.ZIP", "SIMPCLOK.AD", "Simpsons Clocks"}, {"GRAFFITI.ZIP", "OBJETS.AD", "Objets B'art"},
      {"GRANDPA.ZIP", "GRAMPA.AD", "Grampa's Wisdom "}, {"GRASSKRT.ZIP", "GRASSKRT.AD", "Grass Skirts"},
      {"HOMEREAT.ZIP", "HOMEREAT.AD", "Homer Eats"},    {"HOW2DRAW.ZIP", "HOW2DRAW.AD", "How To Draw"},
      {"INS.ZIP", "INS.AD", "Itchy & Scratchy"},        {"KRUSTY.ZIP", "KRUSTY.AD", "Krusty"},
      {"MLISA.ZIP", "LISA.AD", "Lisa's Mood Swings"},   {"PHYSICS.ZIP", "PHYSICS.AD", "Physics"},
      {"SFILES.ZIP", "SIMPFILE.AD", "Files"},           {"SNOWBALL.ZIP", "SNOWBALL.AD", "Snowball I "},
      {"STRIVIA.ZIP", "SIMPTRIV.AD", "Simpsons Trivia"},
  };
  return v;
}

// Which of the two install floppies each root file was on (the real split).
inline int simpsons_disk(const std::string& name) {
  for (const char* d2 : {"SETUP.PKG", "DISK.2", "MODMISC.ZIP", "MUSIC.ZIP", "HELP.ZIP", "WINSYS.ZIP", "BURNS.ZIP",
                         "CHALKBRD.ZIP", "CLOCKS.ZIP", "GRANDPA.ZIP", "GRASSKRT.ZIP", "KRUSTY.ZIP", "SFILES.ZIP",
                         "SERIAL.TXT"})
    if (name == d2) return 2;
  return 1;
}

inline PkgFixture simpsons_fixture() {
  Ad3Parts p;
  p.install = "";
  p.root = "packages/simpsons";
  p.mdir = "SIMPSONS";
  p.installer_files({"DISK.1", "DISK.2"});
  p.engine_zip();
  auto xpl = blob("ADXPL310"), snd = blob("SIMP_SND", 3000), rsrc = blob("AD_RSRC"), afi = blob("SAX.AFI"),
       m1 = blob("simpsons mid"), m2 = blob("i&s show");
  p.zip("MODMISC.ZIP", {{"ADXPL310.DLL", xpl}, {"EDITFILE.TXT", blob("edit")}});
  p.expect("SIMPSONS/ADXPL310.DLL", xpl);
  p.zip("MUSIC.ZIP", {{"SIMPSONS.MID", m1}, {"I&SSHOW.MID", m2}, {"SIMP_SND.DLL", snd}});
  p.expect("SIMPSONS/MUSIC/SIMPSONS.MID", m1);
  p.expect("SIMPSONS/MUSIC/I&SSHOW.MID", m2);
  p.expect("SIMPSONS/SIMP_SND.DLL", snd);
  p.zip("WIN.ZIP", {{"SPALETTE.DLL", blob("spalette")}, {"AD_RSRC.DLL", rsrc}});
  p.expect("SIMPSONS/AD_RSRC.DLL", rsrc);
  p.zip("WINSYS.ZIP", {{"SPMME.DRV", blob("spmme")}});
  p.zip("AFI.ZIP", {{"SAX.AFI", afi}, {"AD3.AFI", blob("AD3.AFI 3.0")}});
  p.expect("SIMPSONS/FOLDER.AFI", afi);
  p.zip("HELP.ZIP", {{"SIMPSONS.HLP", blob("help")}});
  for (const SimpsonsModule& m : simpsons_modules()) p.module(m.zip, {{m.file, ne_module(m.name, {"KERNEL", "ADXPL310"})}});
  p.f.source["CEREAL.TXT"] = blob("the owner's notes");
  p.f.source["SERIAL.TXT"] = blob("a serial number", 3000);
  p.f.ids = {"simpsons.burns",    "simpsons.chalkbrd", "simpsons.grampa",   "simpsons.grasskrt", "simpsons.homereat",
             "simpsons.how2draw", "simpsons.ins",      "simpsons.krusty",   "simpsons.lisa",     "simpsons.objets",
             "simpsons.physics",  "simpsons.simpclok", "simpsons.simpfile", "simpsons.simptriv", "simpsons.snowball"};
  return p.f;
}

// ---- writing a fixture as a source -------------------------------------------------------

inline std::filesystem::path path_under(const std::filesystem::path& root, const std::string& rel) {
  std::filesystem::path p = root;
  size_t i = 0;
  while (i < rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) j = rel.size();
    p /= adw::import::to_wide(rel.substr(i, j - i));
    i = j + 1;
  }
  return p;
}

inline void write_tree(const std::filesystem::path& root, const Tree& t) {
  for (const auto& [rel, d] : t) write_bytes(path_under(root, rel), d);
}

inline std::vector<uint8_t> iso_of(const PkgFixture& f, bool joliet = true, const std::string& volume = "AD_TEST") {
  IsoBuilder b;
  b.joliet = joliet;
  b.volume_id = volume;
  for (const auto& [rel, d] : f.source) {
    auto j = f.joliet.find(rel);
    b.file(rel, d, j == f.joliet.end() ? "" : j->second);
  }
  return b.build();
}

// `disk`: 0 = every root file, 1 or 2 = that Simpsons floppy's files only.
inline std::vector<uint8_t> fat_of(const Tree& t, int disk = 0, const std::string& corrupt_chain_of = "") {
  FatBuilder b = FatBuilder::floppy288();
  for (const auto& [rel, d] : t)
    if (!disk || simpsons_disk(rel) == disk) b.file(rel, d);
  auto img = b.build();
  if (!corrupt_chain_of.empty() && (!disk || simpsons_disk(corrupt_chain_of) == disk)) {
    const auto& c = b.node(corrupt_chain_of).clusters;
    if (c.size() >= 2) b.set_fat(img, c[1], c[0]);  // a loop: reading it would fail
  }
  return img;
}

// ---- manifests and registries --------------------------------------------------------------

// Stable storage for the KnownFile strings of synthetic manifests.
inline std::deque<std::string>& string_pool() {
  static std::deque<std::string> pool;
  return pool;
}
inline const char* keep(const std::string& s) { return string_pool().emplace_back(s).c_str(); }
inline const wchar_t* keep(const std::wstring& s) {
  static std::deque<std::wstring> pool;
  return pool.emplace_back(s).c_str();
}

// A manifest describing `expect` exactly (paths sorted).
inline std::vector<adw::import::KnownFile> manifest_of(const Tree& expect) {
  std::vector<adw::import::KnownFile> m;
  for (const auto& [rel, d] : expect) m.push_back({keep(rel), d.size(), keep(adw::import::md5_hex(d.data(), d.size()))});
  return m;
}

// The built-in registry with synthetic manifests (and, optionally, known
// image md5s, download copies and cover sources) in place of the real ones —
// no Internet Archive URL and no cover download, so no test reaches the
// network by accident. Storage lives as long as the test.
struct TestRegistry {
  std::vector<adw::import::Package> packages;
  std::deque<std::vector<adw::import::KnownFile>> manifests;
  std::deque<std::vector<adw::import::KnownImage>> images;
  std::deque<std::vector<adw::import::Download>> download_lists;
  std::deque<std::vector<adw::import::CoverSource>> cover_lists;

  TestRegistry() {
    auto b = adw::import::builtin_packages();
    packages.assign(b.begin(), b.end());
    for (auto& p : packages) {
      p.manifest = {};
      p.images = {};
      p.downloads = {};
      p.covers = {};
    }
  }
  // Cover sources for `id` (COVERS.md §2.2), in the order tried.
  void covers(const std::string& id, std::vector<adw::import::CoverSource> sources) {
    cover_lists.push_back(std::move(sources));
    get(id).covers = cover_lists.back();
  }
  adw::import::Package& get(const std::string& id) {
    for (auto& p : packages)
      if (id == p.id) return p;
    abort();
  }
  void manifest(const std::string& id, std::vector<adw::import::KnownFile> m) {
    manifests.push_back(std::move(m));
    get(id).manifest = manifests.back();
  }
  void image(const std::string& id, const std::string& md5, uint64_t size) {
    images.push_back({{keep(md5), size, "synthetic", ""}});
    get(id).images = images.back();
  }
  // A download copy: `url`, saved as `file`, published with this size and md5.
  struct Copy {
    std::string url;
    std::wstring file;
    uint64_t size;
    std::string md5;
    std::string kind = "image";
  };
  void downloads(const std::string& id, const std::vector<Copy>& copies) {
    std::vector<adw::import::Download> d;
    for (const Copy& c : copies) d.push_back({keep(c.url), keep(c.file), c.size, keep(c.md5), keep(c.kind)});
    download_lists.push_back(std::move(d));
    get(id).downloads = download_lists.back();
  }
  std::span<const adw::import::Package> span() const { return packages; }
};

}  // namespace test
