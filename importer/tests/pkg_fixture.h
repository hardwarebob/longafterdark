// Synthetic sources shaped like the seven known releases (PACKAGES.md §2–§4),
// for the package tests: the Deluxe and 10th Anniversary CDs' plain FILES
// trees; the AD 3.x InstallShield installs (AD 3.2, Totally Twisted, the
// Simpsons floppies) with encrypted PKZIP archives under the test-only
// password and an INSTALL.INS-like script that holds it among decoys; a
// Presage install shaped like Star Wars Screen Entertainment's (INSTALL.DAT,
// multi-volume ARJ archives with split members, SZDD loose files, decoys
// that must never be read); and a Microsoft Setup install shaped like Star
// Trek: The Screen Saver's (SETUP.LST, KWAJ files written by kwaj_builder.h,
// two install disks, decoys). Each fixture is a tree (path -> bytes) that can
// be written as a folder, an ISO image, a FAT floppy image (or several), a
// flat ZIP or a ZIP of floppy images, plus exactly what an import must
// install and the catalog ids it must list. Modules come from
// module_builder.h: made-up resources, no bytes of any release.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "arj_builder.h"
#include "fat_builder.h"
#include "importer.h"
#include "iso_builder.h"
#include "kwaj_builder.h"
#include "md5.h"
#include "module_builder.h"
#include "szdd_builder.h"
#include "test_util.h"
#include "zip_builder.h"

namespace test {

using Tree = std::map<std::string, std::vector<uint8_t>>;

inline std::vector<uint8_t> vec(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// An NE (Classic) module named `name` importing `refs`; it exports MODULE,
// as every real one does (the catalog lists an NE only as its lane runs it).
inline std::vector<uint8_t> ne_module(const std::string& name, const std::vector<std::string>& refs) {
  NeSpec ne;
  ne.module_refs = refs;
  ne.exports = {"MODULE"};
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

// ---- Star Wars Screen Entertainment: a Presage install of Intermission modules --------------

// An Intermission IMX module: an NE exporting what the IMX reader looks for
// (SAVERDLGPROC only with `dialog`), with a dialog template blob.
inline std::vector<uint8_t> imx_module(const std::string& name, const std::vector<std::string>& refs,
                                       bool dialog = true) {
  NeSpec ne;
  ne.module_name = name;
  ne.module_refs = refs;
  ne.exports = {"WEP", "SAVERINIT", "SAVERDRAW"};
  if (dialog) ne.exports.push_back("SAVERDLGPROC");
  ne.exports.push_back("LIBMAIN");
  if (dialog) ne.exports.push_back("SAVERDLGPROC2");
  ne.resources = {{5, "", 1, "a made-up dialog template of " + name}};
  return vec(build_ne(ne));
}

// A plain NE library importing `refs` (the DLLs beside the modules, whose
// own imports the intermission invariants check).
inline std::vector<uint8_t> ne_dll(const std::string& name, const std::vector<std::string>& refs) {
  NeSpec ne;
  ne.module_name = name;
  ne.module_refs = refs;
  ne.exports = {"WEP"};
  return vec(build_ne(ne));
}

// A Presage installer script of our own (the real one is never copied):
// [disks], [data] with the product's short name, a few [install] lines.
inline std::vector<uint8_t> intermission_dat(const std::string& shortname) {
  return vec("[disks]\r\n"
             "1 = \"Made-Up Collection Disk 1\",swse1.arj\r\n"
             "2 = \"Made-Up Collection Disk 2\",swse2.arj\r\n"
             "\r\n"
             "[data]\r\n"
             "neededspace = 0\r\n"
             "  ShortName =  " +
             shortname +
             "  \r\n"
             "longname = A Made-Up Screen Saver Collection\r\n"
             "DestDir = C:\\SAVER\r\n"
             "\r\n"
             "[install]\r\n"
             "; comment lines are skipped\r\n"
             "1 = 1,stress.dl_,stress.dll,A helper library\r\n"
             "2 = 1,gm_battl.mi_,battle.mid,Some music,CHECK=24\r\n"
             "3 = 1,swse1.arj,(DST),The first archive\r\n"
             "4 = 5,swse.ini,(WIN)\\swse.ini,Settings,CHECK=100\r\n");
}

// Which install floppy each file is on (INSTALL.DAT's disk column): disk 1
// has the installer, SWSE1.ARJ and the SZDD files; disks 2-5 one volume of
// SWSE2 each; disk 5 also SWSE.INI, WinG, the VxDs and the control panel.
inline int swse_disk(const std::string& name) {
  if (name == "SWSE2.ARJ") return 2;
  if (name == "SWSE2.A01") return 3;
  if (name == "SWSE2.A02") return 4;
  for (const char* d5 : {"SWSE2.A03", "SWSE.INI", "WING.DL_", "WINGPAL.WN_", "DVA.386", "ANTHOOK.386", "IMCPL.CPL",
                         "SWSESET.EXE", "SYSINI.DAT"})
    if (name == d5) return 5;
  return 1;
}

// The files a Star Wars Screen Entertainment import must never open (I5):
// the installer, readme and notes, the other MIDI sets, WinG, the VxDs, the
// control panel, the chooser.
inline const std::vector<std::string>& swse_decoys() {
  static const std::vector<std::string> v = {"README.TXT",  "SVGA.EXE",    "INSTALL.EXE", "SYSINI.DAT",  "SB6BATTL.MI_",
                                             "FM_TITLE.MI_", "WING.DL_",   "IMCPL.CPL",   "INSTDETL.DAT", "DIB.DR_",
                                             "ANTHOOK.386", "SWSESET.EXE", "DVA.386",     "WINGPAL.WN_"};
  return v;
}

// A Presage install at the source's root (a CD copy of the five floppies):
// SWSE1.ARJ (every DLL, Intermission and its IMX reader, one module, and
// four members the recipe skips — damaged on purpose, so decoding any of them
// would fail the import), SWSE2.ARJ + .A01-.A03 (one archive: stored modules
// cut across volumes, an LZH member cut across volumes, method 1 and 4
// members), the SZDD loose files and SWSE.INI, and decoys. `swse1`, when
// set, changes SWSE1.ARJ's members before the volume is written (the
// expected files stay those of the unchanged release).
inline PkgFixture swse_fixture(const std::function<void(std::vector<ArjEntry>&)>& swse1 = {}) {
  PkgFixture f;
  const std::string root = "packages/swse";
  auto expect = [&](const std::string& rel, const std::vector<uint8_t>& d) { f.expect[root + "/" + rel] = d; };
  const std::vector<std::string> base = {"INTRMLIB", "SWSE", "KERNEL", "USER", "GDI", "STRESS"};
  auto with = [&](std::vector<std::string> extra) {
    std::vector<std::string> r = base;
    r.insert(r.end(), extra.begin(), extra.end());
    return r;
  };
  auto stored = [](const std::string& n, const std::vector<uint8_t>& d, uint8_t flags = 0x10,
                   std::optional<uint32_t> pos = std::nullopt) { return arj_stored_entry(n, d, flags, pos); };

  // SWSE1.ARJ.
  const auto intrmlib = ne_dll("INTRMLIB", {"USER", "KERNEL", "GDI", "ANTSW"});
  const auto antsw = ne_dll("ANTSW", {"KERNEL", "GDI", "USER"});
  const auto swse = ne_dll("SWSE", {"KERNEL", "USER", "GDI", "MMSYSTEM"});
  const auto readjpg = ne_dll("READJPG", {"KERNEL", "USER", "GDI", "WIN87EM"});
  const auto memmidi = arj_vector_plain("m4_text");  // method 4
  const auto intermis = blob("the Intermission control panel");
  const auto imimxply = ne_dll("IMIMXPLY", {"KERNEL", "USER"});
  const auto bluprint = imx_module("BLUPRINT", with({"READJPG"}));
  auto bad_crc = stored("IMAD_PLY.IMQ", blob("the After Dark reader"));
  bad_crc.h.crc ^= 1;
  auto garbage = arj_packed_entry("INTERMSN.HLP", 1, pattern(64, 17), blob("help"));
  auto bad_snd = stored("AD_SND.DLL", blob("Intermission's sound support"));
  bad_snd.h.crc ^= 2;
  std::vector<ArjEntry> members = {
      stored("INTRMLIB.DLL", intrmlib), stored("ANTSW.DLL", antsw), stored("SWSE.DLL", swse),
      stored("READJPG.DLL", readjpg),
      arj_packed_entry("MEMMIDI.DLL", 4, unhex(arj_vector("m4_text").packed), memmidi), stored("INTERMIS.EXE", intermis),
      stored("IMIMXPLY.IMQ", imimxply), stored("BLUPRINT.IMX", bluprint), bad_crc, bad_snd,
      stored("IWLIB.DLL", blob("an extension library")), garbage};
  if (swse1) swse1(members);
  f.source["SWSE1.ARJ"] = arj_volume("SWSE1.ARJ", false, members);
  expect("SAVER/INTRMLIB.DLL", intrmlib);
  expect("SAVER/ANTSW.DLL", antsw);
  expect("SAVER/SWSE.DLL", swse);
  expect("SAVER/READJPG.DLL", readjpg);
  expect("SAVER/MEMMIDI.DLL", memmidi);
  expect("SAVER/BLUPRINT.IMX", bluprint);
  expect("ENGINE/INTERMIS.EXE", intermis);
  expect("ENGINE/IMIMXPLY.IMQ", imimxply);

  // SWSE2.ARJ .. .A03: one archive over four disks.
  const auto battles = imx_module("BATTLES", base), cantina = imx_module("CANTINA", with({"READJPG", "WIN87EM"}));
  const auto jawas = imx_module("JAWAS", base), storybrd = imx_module("STORYBRD", with({"READJPG"}));
  const auto swtext = imx_module("SWTEXT", with({"COMMDLG"})), vader = imx_module("VADER", with({"READJPG"}));
  const auto swtext_txt = arj_vector_plain("m1_all_bytes");  // method 1
  const auto p1 = arj_vector_plain("m1_text"), p2 = arj_vector_plain("m1_two_blocks");
  std::vector<uint8_t> swsfx = p1;  // an LZH member cut across volumes: each segment its own stream
  swsfx.insert(swsfx.end(), p2.begin(), p2.end());
  auto half = [](const std::vector<uint8_t>& d, bool second) {
    const size_t cut = d.size() / 2;
    return second ? std::vector<uint8_t>(d.begin() + cut, d.end()) : std::vector<uint8_t>(d.begin(), d.begin() + cut);
  };
  f.source["SWSE2.ARJ"] = arj_volume("SWSE2.ARJ", true,
                                     {stored("BATTLES.IMX", battles), stored("CANTINA.IMX", cantina),
                                      stored("JAWAS.IMX", half(jawas, false), 0x14)});
  f.source["SWSE2.A01"] = arj_volume("SWSE2.A01", true,
                                     {stored("JAWAS.IMX", half(jawas, true), 0x18, uint32_t(jawas.size() / 2)),
                                      stored("STORYBRD.IMX", half(storybrd, false), 0x14)});
  f.source["SWSE2.A02"] = arj_volume(
      "SWSE2.A02", true,
      {stored("STORYBRD.IMX", half(storybrd, true), 0x18, uint32_t(storybrd.size() / 2)),
       arj_packed_entry("SWSFX.DLL", 1, unhex(arj_vector("m1_text").packed), p1, 0x14)});
  f.source["SWSE2.A03"] = arj_volume(
      "SWSE2.A03", false,
      {arj_packed_entry("SWSFX.DLL", 1, unhex(arj_vector("m1_two_blocks").packed), p2, 0x18, uint32_t(p1.size())),
       stored("SWTEXT.IMX", swtext),
       arj_packed_entry("SWTEXT.TXT", 1, unhex(arj_vector("m1_all_bytes").packed), swtext_txt),
       stored("VADER.IMX", vader)});
  for (auto& [n, d] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
           {"BATTLES.IMX", battles}, {"CANTINA.IMX", cantina}, {"JAWAS.IMX", jawas}, {"STORYBRD.IMX", storybrd},
           {"SWSFX.DLL", swsfx}, {"SWTEXT.IMX", swtext}, {"SWTEXT.TXT", swtext_txt}, {"VADER.IMX", vader}})
    expect("SAVER/" + n, d);

  // Loose files: SZDD under the installer's names, and the settings.
  const auto stress = ne_dll("STRESS", {"KERNEL", "USER", "TOOLHELP"});
  f.source["STRESS.DL_"] = szdd_encode(stress);
  expect("SAVER/STRESS.DLL", stress);
  for (auto [from, to] : std::vector<std::pair<const char*, const char*>>{{"GM_BATTL.MI_", "BATTLE.MID"},
                                                                          {"GM_CNTNA.MI_", "CANTINA.MID"},
                                                                          {"GM_EMPIR.MI_", "EMPIRE.MID"},
                                                                          {"GM_TITLE.MI_", "SWTHEME.MID"}}) {
    auto music = vec("MThd");
    auto body = blob(std::string("general midi ") + to, 400);
    music.insert(music.end(), body.begin(), body.end());
    f.source[from] = szdd_encode(music);
    expect(std::string("SAVER/") + to, music);
  }
  const auto ini = vec("[Made-Up Module]\r\nSetting=1\r\n");
  f.source["SWSE.INI"] = ini;
  expect("WINDOWS/SWSE.INI", ini);
  f.source["INSTALL.DAT"] = intermission_dat("SWSE");
  for (const std::string& d : swse_decoys()) f.source[d] = blob("decoy " + d, 200);
  f.ids = {"swse.battles", "swse.bluprint", "swse.cantina", "swse.jawas", "swse.storybrd", "swse.swtext", "swse.vader"};
  return f;
}

// ---- Star Trek: The Screen Saver: a Microsoft Setup install of After Dark 2.0 modules ---------

// An After Dark 2.0 module (the Classic lane: an NE exporting MODULE),
// `name` as its name resource holds it (with the leading space After Dark
// 2.0 put in most), importing `refs`, with `controls` (slot, record). Its
// About text ends with the registrant stand-in and wraps a sentence by hand,
// as the real ones do.
inline std::vector<uint8_t> ad20_module(const std::string& name, const std::vector<std::string>& refs,
                                        const std::vector<std::pair<uint16_t, std::string>>& controls = {},
                                        bool stand_in = true) {
  NeSpec ne;
  ne.module_name = "AD20MOD";
  ne.module_refs = refs;
  ne.exports = {"WEP", "MODULE"};
  std::string about = "ABOUT" + name + "\r\n\r\nA made-up module whose sentence is wrapped by \r\nhand.\r\n\r\n"
                      "Made up for the tests.";
  if (stand_in) about += "\r\nBerkeley Systems Authorized User.";
  ne.resources = {{2000, "", 20, name + '\0'}, {2000, "", 30, about + '\0'}, {2000, "", 10, "By nobody.\r\n" + std::string(1, '\0')}};
  for (const auto& [slot, rec] : controls) ne.resources.push_back({1000, "", slot, rec});
  return vec(build_ne(ne));
}

// Which install floppy each file is on (the real split: the setup files and
// the modules on disk 1; the engine, its DLLs and drivers, the art and sound
// databases on disk 2).
inline int startrek_disk(const std::string& name) {
  for (const char* d2 : {"AD.EX_", "AD.HL_", "ADINIT.EX_", "AD_AILAN.DL_", "AD_LIB.DL_", "AD_MME.DR_", "AD_MOD.DL_",
                         "AD_MPT.DR_", "AD_NET.EX_", "AD_NVLNW.DL_", "AD_RSRC.DL_", "AD_SB.DR_", "AD_SND.DL_",
                         "AD_WRAP.CO_", "AFTERDRK.NS_", "NWCONN.DL_", "NWCORE.DL_", "NWMISC.DL_", "SPALETTE.DL_",
                         "ST_MASKS.DL_", "ST_RESDB.DL_", "ST_SND.DL_", "ST_SVGA.DL_", "ST_VGA.DL_"})
    if (name == d2) return 2;
  return 1;
}

// The files a Star Trek: The Screen Saver import must never open (I5): the
// setup program and its scripts, the network, PC-speaker and Sound Blaster
// drivers, the disk's AD_PREFS.INI, the help file, the VxD, AD_MESG and the rest.
inline const std::vector<std::string>& startrek_decoys() {
  static const std::vector<std::string> v = {
      "SETUP.EXE",   "_MSTEST.EX_",  "AD_NSTLL.MS_", "AD_NSTLL.DL_", "ST_NSTLL.IN_", "AD_NSTLL.INI", "AD_MESG.AD_",
      "AD.38_",      "AD.HL_",       "AD_WRAP.CO_",  "NWCONN.DL_",   "AD_NET.EX_",   "AFTERDRK.NS_", "ADINIT.EX_",
      "SPLASH1.BM_", "AD_PREFS.IN_", "AD_MPT.DR_",   "SPALETTE.DL_", "AD_LIB.DL_",   "AD_SB.DR_"};
  return v;
}

// A Setup file list of our own (the real one is never copied): the window
// title that names the release, a command line, a file list, the trailing
// Ctrl-Z. `title` in Windows-1252.
inline std::vector<uint8_t> setup_lst(const std::string& title = "Star Trek\xAE: The Screen Saver") {
  return vec("[Params]\r\n"
             "\tWndTitle            = " + title + "\r\n"
             "\tWndMess             = Setting up a made-up screen saver...\r\n"
             "\tCmdLine             = _mstest made_up.mst  /C \"/S %s %s\"\r\n"
             "\r\n"
             "[Files]\r\n"
             "\tmade_up.in_ = made_up.inf\r\n"
             "\x1A");
}

// Star Trek: The Screen Saver's two install disks' files at the source's
// root (a copy of both, or the disks unioned): SETUP.LST, every file of the
// registry's table KWAJ-compressed under its disk name (made-up modules and
// DLLs as runs of literals; ST_VGA.DL_ is a fixed vector the research
// encoder wrote, ST_MASKS.DL_ the distance-4096 crafted stream), and decoys.
// `change`, when set, edits the sources after they are made (the expected
// files stay those of the unchanged release).
inline PkgFixture startrek_fixture(const std::function<void(Tree&)>& change = {}) {
  PkgFixture f;
  const std::string root = "packages/startrek";
  auto put = [&](const std::string& disk_name, const std::string& rel, const std::vector<uint8_t>& d) {
    f.source[disk_name] = kwaj_literals(d);
    f.expect[root + "/" + rel] = d;
  };
  const std::vector<std::string> base = {"KERNEL", "USER", "GDI", "WIN87EM", "AD_MOD", "AD_RSRC"};
  struct M {
    const char* file;
    const char* name;
  };
  // The 16 names, as the resources hold them: 15 with a leading space.
  const std::vector<M> modules = {{"BRAINCEL", " Brain Cells"},  {"COMMS", " Communications"}, {"FINAL", " Final Exam"},
                                  {"FRONTIER", " Final Frontier"}, {"HORTA", " Horta"},        {"IONSTORM", " Ion Storm"},
                                  {"MISSION", " The Mission"},   {"PANELS", " Ship Panels"},   {"PLANETS", " PlanetaryAtlas"},
                                  {"SCOTTYS", " Scotty's Files"}, {"SICKBAY", " Sickbay"},     {"SOUNDER", "Sounder"},
                                  {"SPACE", " Space"},           {"SPOCK", " Spock"},          {"THOLIAN", " Tholian Web"},
                                  {"TRIBBLE", " Tribbles"}};
  for (const M& m : modules) {
    const std::string file = m.file;
    std::vector<uint8_t> d;
    if (file == "COMMS")  // a button in slot 3; slot 0 empty
      d = ad20_module(m.name, base, {{2, popup_record("Message", {"Custom", "Other"}, 0)}, {4, button_record("Edit Custom...")}});
    else if (file == "SOUNDER")  // AD_SND only, a button in slot 2, no stand-in line
      d = ad20_module(m.name, {"KERNEL", "USER", "GDI", "AD_SND"},
                      {{1, popup_record("Sequence", {"In Order", "Shuffle"}, 0)}, {3, button_record("Sounds..")}}, false);
    else if (file == "MISSION")  // no controls
      d = ad20_module(m.name, base);
    else
      d = ad20_module(m.name, base, {{1, checkbox_record("Clear Screen First", 0)}});
    put(file + ".AD_", "AFTERDRK/" + file + ".AD", d);
  }
  put("AD_MOD.DL_", "AFTERDRK/AD_MOD.DLL", ne_dll("AD_MOD", {"KERNEL", "GDI", "USER", "WIN87EM", "AD_RSRC", "AD_SND"}));
  put("AD_RSRC.DL_", "AFTERDRK/AD_RSRC.DLL", ne_dll("AD_RSRC", {"KERNEL", "USER", "GDI"}));
  put("AD_MME.DR_", "AFTERDRK/AD_MME.DRV", ne_dll("AD_MME", {"WIN87EM", "KERNEL", "USER"}));
  put("ST_RESDB.DL_", "AFTERDRK/ST_RES/ST_RESDB.DLL", ne_dll("ARTDB", {"KERNEL"}));
  put("ST_SVGA.DL_", "AFTERDRK/ST_RES/ST_SVGA.DLL", blob("made-up art, 256 colours", 6000));
  put("ST_SND.DL_", "AFTERDRK/ST_RES/ST_SND.DLL", blob("made-up sounds", 9000));
  auto wav = vec("RIFF");
  auto body = blob("a made-up wave", 700);
  wav.insert(wav.end(), body.begin(), body.end());
  put("JIM.WA_", "AFTERDRK/SOUNDS/JIM.WAV", wav);
  put("AD_SND.DL_", "ENGINE/AD_SND.DLL", ne_dll("AD_SND", {"KERNEL", "USER"}));
  put("AD.EX_", "ENGINE/AD.EXE", blob("a made-up host", 3000));
  // Written by the research encoder, and token by token.
  f.source["ST_VGA.DL_"] = unhex(kKwajVectors[2].packed);
  f.expect[root + "/AFTERDRK/ST_RES/ST_VGA.DLL"] = kwaj_vector_plain(kKwajVectors[2].name);
  const KwajCraftedFile masks = kwaj_dist4096();
  f.source["ST_MASKS.DL_"] = masks.file;
  f.expect[root + "/AFTERDRK/ST_RES/ST_MASKS.DLL"] = masks.plain;
  f.source["SETUP.LST"] = setup_lst();
  for (const std::string& d : startrek_decoys()) f.source[d] = blob("decoy " + d, 200);
  if (change) change(f.source);
  f.ids = {"startrek.braincel", "startrek.comms",   "startrek.final",   "startrek.frontier",
           "startrek.horta",    "startrek.ionstorm", "startrek.mission", "startrek.panels",
           "startrek.planets",  "startrek.scottys",  "startrek.sickbay", "startrek.sounder",
           "startrek.space",    "startrek.spock",    "startrek.tholian", "startrek.tribble"};
  return f;
}

// A ZIP of floppy images, as the Internet Archive serves an item's: stored
// members, disk 2 first, and (`with_scan`) a label scan beside them.
inline std::vector<uint8_t> zip_of_images(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& images,
                                          bool with_scan = true) {
  ZipBuilder b;
  b.password = "";
  for (const auto& [name, d] : images) b.add(name, d, /*deflate=*/false, /*encrypt=*/false);
  if (with_scan) b.add("disk1.jpg", pattern(5000, 99), /*deflate=*/false, /*encrypt=*/false);
  return b.build();
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

// `disk`: 0 = every root file, else only that floppy's files, by `disk_of`
// (the Simpsons' two by default; swse_disk for the five of Star Wars Screen
// Entertainment, with FatBuilder::floppy144()).
inline std::vector<uint8_t> fat_of(const Tree& t, int disk = 0, const std::string& corrupt_chain_of = "",
                                   int (*disk_of)(const std::string&) = simpsons_disk,
                                   FatBuilder b = FatBuilder::floppy288()) {
  for (const auto& [rel, d] : t)
    if (!disk || disk_of(rel) == disk) b.file(rel, d);
  auto img = b.build();
  if (!corrupt_chain_of.empty() && (!disk || disk_of(corrupt_chain_of) == disk)) {
    const auto& c = b.node(corrupt_chain_of).clusters;
    if (c.size() >= 2) b.set_fat(img, c[1], c[0]);  // a loop: reading it would fail
  }
  return img;
}

// A flat ZIP of the tree (every file at the root, none encrypted): the
// Internet Archive's copies of install folders.
inline std::vector<uint8_t> zip_folder(const Tree& t) {
  ZipBuilder b;
  b.password = "";
  for (const auto& [rel, d] : t) b.add(rel, d, /*deflate=*/true, /*encrypt=*/false);
  return b.build();
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
  std::deque<std::vector<adw::import::DownloadPart>> part_lists;
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
  // Known images of install disks: md5, size, disk number (1..N).
  struct Disk {
    std::string md5;
    uint64_t size;
    int disk;
  };
  void disk_images(const std::string& id, const std::vector<Disk>& disks) {
    std::vector<adw::import::KnownImage> v;
    for (const Disk& d : disks) v.push_back({keep(d.md5), d.size, "synthetic floppy", "", d.disk});
    images.push_back(std::move(v));
    get(id).images = images.back();
  }
  // A download copy: `url`, saved as `file`, published with this size and
  // md5 (and `more`: the images of further install disks).
  struct Part {
    std::string url;
    std::wstring file;
    uint64_t size;
    std::string md5;
  };
  struct Copy {
    std::string url;
    std::wstring file;
    uint64_t size;
    std::string md5;
    std::string kind = "image";
    std::vector<Part> more = {};
  };
  void downloads(const std::string& id, const std::vector<Copy>& copies) {
    std::vector<adw::import::Download> d;
    for (const Copy& c : copies) {
      std::span<const adw::import::DownloadPart> more;
      if (!c.more.empty()) {
        std::vector<adw::import::DownloadPart> parts;
        for (const Part& q : c.more) parts.push_back({keep(q.url), keep(q.file), q.size, keep(q.md5)});
        part_lists.push_back(std::move(parts));
        more = part_lists.back();
      }
      d.push_back({keep(c.url), keep(c.file), c.size, keep(c.md5), keep(c.kind), more});
    }
    download_lists.push_back(std::move(d));
    get(id).downloads = download_lists.back();
  }
  std::span<const adw::import::Package> span() const { return packages; }
};

}  // namespace test
