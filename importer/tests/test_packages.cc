// Multi-package imports on synthetic sources (PACKAGES.md §9, tests/
// pkg_fixture.h):
//   identification   every release as a folder, an ISO and a FAT image (Star
//                    Wars Screen Entertainment also as five floppies, in
//                    either order, and a flat ZIP; Star Trek: The Screen
//                    Saver as two floppies in either order, the ZIP of
//                    them, a flat ZIP and an ISO); unknown and ambiguous
//                    sources; --package; image md5s and known sets of install
//                    disks (complete, a ZIP's image given before it read once;
//                    one disk alone, two copies of one; every disk with a
//                    second copy of one or a stranger, logged as such); ZIP
//                    member names and a volume label in code page 437 (two
//                    names that differ in a letter outside ASCII are two; the
//                    record is UTF-8); Presage's INSTALL.DAT; Microsoft
//                    Setup's SETUP.LST
//   recipes          exact file sets, fix-ups only from matching sources,
//                    the §4.2 invariants (and the intermission and ad2kwaj
//                    recipes' own), required files and archives (every
//                    install disk), the derived password (never written or
//                    logged), I5 (the owner's notes and swse's and startrek's
//                    decoys never opened; ARJ volume chains that point past
//                    the registry's archives refused, the volume they name
//                    never opened; other .ARJ skipped), damaged ARJ, SZDD and
//                    KWAJ files (a KWAJ file cut to a clean prefix refused
//                    before a byte is written), names that differ only in a
//                    non-ASCII letter's case (one file to Windows) refused
//   atomicity        a package import leaves Deluxe and every other package
//                    byte for byte; re-imports replace only their package;
//                    failures and cancels change nothing
//   recovery         every interrupted swap and removal state
//   catalog          ids, order, the displayName rule, overrides, trimming,
//                    sameAs, the packages array, Deluxe entries unchanged,
//                    After Dark 2.0's About texts and "screen" (startrek only)
//   commands         --catalog-only without FILES, --remove, --list-packages,
//                    and adimport.exe's options and exit codes
//
//   test_import_packages <adimport.exe> <scratch>
#include <phosg/JSON.hh>

#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "catalog.h"
#include "importer.h"
#include "md5.h"
#include "names.h"
#include "pkg_fixture.h"
#include "run_process.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_log;

ImportOptions opts_for(const fs::path& root, const test::TestRegistry& reg, bool check = true) {
  ImportOptions o;
  o.assets_root = root;
  o.check_known = check;
  o.registry = reg.span();
  o.log = [](const std::string& s) {
    g_log.push_back(s);
    fprintf(stderr, "  log: %s\n", s.c_str());
  };
  return o;
}

Source folder(const fs::path& p, const std::string& package = "") {
  Source s;
  s.kind = Source::Kind::folder;
  s.path = p;
  s.package = package;
  return s;
}

Source image(const fs::path& p, std::vector<fs::path> more = {}, const std::string& package = "") {
  Source s;
  s.kind = Source::Kind::image;
  s.path = p;
  s.more_images = std::move(more);
  s.package = package;
  return s;
}

ImportResult run(const char* what, const Source& s, const ImportOptions& o, Status want) {
  ImportResult r = run_import(s, o);
  fprintf(stderr, "[%s] %s: %s\n", what, status_name(r.status), r.message.c_str());
  CHECK_EQ(r.status, want);
  return r;
}

// Every file under `dir`: relative path -> bytes.
test::Tree snapshot(const fs::path& dir) {
  test::Tree t;
  for (const std::string& rel : test::list_tree(dir)) t[rel] = test::read_bytes(test::path_under(dir, rel));
  return t;
}

// The same without the package's import record (whose importedUtc a
// re-import of identical files changes).
test::Tree snapshot_files(const fs::path& dir) {
  test::Tree t = snapshot(dir);
  t.erase("import.json");
  return t;
}

// The installed files of a package (relative to <win>), import.json aside.
void check_installed(const fs::path& win, const test::PkgFixture& f, const std::string& root) {
  test::Tree have;
  for (auto& [rel, d] : snapshot(test::path_under(win, root)))
    if (rel != "import.json") have[root + "/" + rel] = d;
  test::Tree want;
  for (auto& [rel, d] : f.expect)
    if (rel.rfind(root + "/", 0) == 0) want[rel] = d;
  for (auto& [rel, d] : want) {
    auto it = have.find(rel);
    if (it == have.end()) {
      test::g_failures++;
      fprintf(stderr, "  missing: %s\n", rel.c_str());
    } else if (it->second != d) {
      test::g_failures++;
      fprintf(stderr, "  differs: %s\n", rel.c_str());
    }
  }
  for (auto& [rel, d] : have)
    if (!want.count(rel)) {
      test::g_failures++;
      fprintf(stderr, "  unexpected: %s\n", rel.c_str());
    }
}

phosg::JSON json_at(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

std::vector<std::string> catalog_ids(const fs::path& win) {
  std::vector<std::string> ids;
  phosg::JSON cat = json_at(win / L"catalog-win.json");
  for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
  return ids;
}

const phosg::JSON* module_by_id(const phosg::JSON& cat, const std::string& id) {
  for (auto& m : cat.at("modules").as_list())
    if (m->get_string("id") == id) return m.get();
  return nullptr;
}

std::vector<std::string> concat(std::initializer_list<std::vector<std::string>> parts) {
  std::vector<std::string> out;
  for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

bool no_leftovers(const fs::path& win) {
  std::error_code ec;
  bool ok = true;
  auto scan = [&](const fs::path& d) {
    for (auto& e : fs::directory_iterator(d, ec)) {
      std::wstring n = e.path().filename().wstring();
      if (n.find(L".importing-") != std::wstring::npos || n.find(L".old-") != std::wstring::npos ||
          n.find(L".removing-") != std::wstring::npos || n.find(L".tmp-") != std::wstring::npos || n == L"import.lock") {
        fprintf(stderr, "  leftover: %s\n", to_utf8(e.path().wstring()).c_str());
        ok = false;
      }
    }
  };
  scan(win);
  scan(win / L"packages");
  return ok;
}

bool logged(const std::string& needle) {
  for (auto& l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// Nothing the importer wrote or said holds the archive password.
void check_no_password(const fs::path& root) {
  for (auto& l : g_log) CHECK(l.find(test::kTestZipPassword) == std::string::npos);
  for (const std::string& rel : test::list_tree(root)) {
    auto b = test::read_bytes(test::path_under(root, rel));
    std::string s(b.begin(), b.end());
    if (s.find(test::kTestZipPassword) != std::string::npos) {
      test::g_failures++;
      fprintf(stderr, "  the password is in %s\n", rel.c_str());
    }
  }
}

std::string with_zip(test::PkgFixture& f, const std::string& path,
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members) {
  f.source[path] = test::zip_of(members);
  return path;
}

// A registry whose manifests describe the fixtures exactly (so a folder or
// ISO source verifies "files" and the ad10 fix-ups apply).
test::TestRegistry registry_for(const std::map<std::string, const test::PkgFixture*>& fx) {
  test::TestRegistry reg;
  for (auto& [id, f] : fx) reg.manifest(id, test::manifest_of(f->expect));
  return reg;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_packages <adimport.exe> [scratch]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-packages");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder

  const test::PkgFixture deluxe = test::deluxe_fixture(), ad10 = test::ad10_fixture(), ad32 = test::ad32_fixture(),
                         tt = test::tt_fixture(), simpsons = test::simpsons_fixture(), swse = test::swse_fixture(),
                         startrek = test::startrek_fixture();
  test::TestRegistry reg = registry_for({{"deluxe", &deluxe},
                                         {"ad10", &ad10},
                                         {"ad32", &ad32},
                                         {"tt", &tt},
                                         {"simpsons", &simpsons},
                                         {"swse", &swse},
                                         {"startrek", &startrek}});

  // ---- the registry's box covers (COVERS.md §2.2, §2.3) ----------------------------------------------
  {
    auto hex32 = [](const char* s) {
      return s && strlen(s) == 32 && std::string_view(s).find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    std::set<std::wstring> names;
    for (const Package& p : builtin_packages()) {
      CHECK(!p.covers.empty());  // every package has at least one cover source
      for (const CoverSource& s : p.covers) {
        const std::string art = s.art ? s.art : "";
        CHECK(art == "box" || art == "disc" || art == "splash" || art == "panel");
        CHECK(s.label && *s.label && s.credit && *s.credit);
        CHECK(s.crop.x >= 0 && s.crop.y >= 0 && s.crop.w >= 0 && s.crop.h >= 0);
        CHECK((s.crop.w == 0) == (s.crop.h == 0));
        if (s.kind == CoverSource::Kind::download) {
          CHECK(s.url && std::string_view(s.url).rfind("https://", 0) == 0);
          CHECK(hex32(s.md5));
          CHECK(s.size > 0);
          CHECK(s.file_name && *s.file_name);
          CHECK(s.file_name && names.insert(s.file_name).second);  // unique: <download dir>\covers\<file_name>
          CHECK(!s.path && !s.resource_type);
        } else {
          CHECK(s.path && *s.path && std::string_view(s.path).find('\\') == std::string_view::npos);
          CHECK(s.path_md5 && (!*s.path_md5 || hex32(s.path_md5)));
          CHECK(s.resource_type == 0 || (s.resource_type == 2 && s.resource_id != 0));
          CHECK(!s.url);
        }
      }
    }
    // The decisions COVERS.md §2.3 records: Deluxe, Totally Twisted and the
    // Simpsons start with a box front (a download), 3.2 with its disc art;
    // the installer splashes are cropped above their warning text; the
    // Simpsons' splash is SETUP.EXE's bitmap 7500.
    auto first_disc = [](const char* id) -> const CoverSource* {
      for (const CoverSource& s : find_package(id)->covers)
        if (s.kind == CoverSource::Kind::disc) return &s;
      return nullptr;
    };
    for (const char* id : {"deluxe", "tt", "simpsons"}) {
      CHECK(find_package(id)->covers[0].kind == CoverSource::Kind::download);
      CHECK_EQ(std::string(find_package(id)->covers[0].art), std::string("box"));
    }
    CHECK(find_package("ad32")->covers[0].kind == CoverSource::Kind::disc);
    CHECK_EQ(find_package("ad32")->covers[0].crop.h, 183);
    CHECK(first_disc("tt") && first_disc("tt")->crop.h == 204);
    // Deluxe's portrait setup art ranks above the disc-label scan.
    CHECK_EQ(std::string(find_package("deluxe")->covers[1].path), std::string("ADE/PAGE1.BMP"));
    const CoverSource& splash = find_package("simpsons")->covers.back();
    CHECK(splash.kind == CoverSource::Kind::disc && splash.resource_type == 2 && splash.resource_id == 7500);
    CHECK_EQ(splash.crop.h, 172);
    CHECK_EQ(std::string(find_package("simpsons")->covers[0].art), std::string("box"));
    // Star Wars Screen Entertainment: two box fronts, then two disc labels,
    // the second cropped to the disc; nothing on the disc (every picture is
    // inside the ARJ archives).
    const Package* sw = find_package("swse");
    CHECK(sw && sw->covers.size() == 4);
    if (sw && sw->covers.size() == 4) {
      for (const CoverSource& c : sw->covers) CHECK(c.kind == CoverSource::Kind::download);
      CHECK(std::string(sw->covers[0].art) == "box" && std::string(sw->covers[1].art) == "box");
      CHECK(std::string(sw->covers[3].art) == "disc");
      CHECK(sw->covers[3].crop.x == 20 && sw->covers[3].crop.y == 14 && sw->covers[3].crop.w == 1424 &&
            sw->covers[3].crop.h == 1424);
    }
    // Star Trek: The Screen Saver: the Internet Archive's box front, the same
    // at 600 dpi, then disk 1's label — drawn as a picture ("panel"), never
    // cut to a disc; nothing on the disks (every picture is KWAJ-compressed).
    const Package* st = find_package("startrek");
    CHECK(st && st->covers.size() == 3);
    if (st && st->covers.size() == 3) {
      for (const CoverSource& c : st->covers) CHECK(c.kind == CoverSource::Kind::download);
      CHECK(std::string(st->covers[0].art) == "box" && std::string(st->covers[1].art) == "box");
      CHECK_EQ(std::string(st->covers[0].md5), std::string("157eb04fcc9bdc0d4a831148ca6cc260"));
      CHECK_EQ(std::string(st->covers[2].art), std::string("panel"));
      CHECK_EQ(std::string(st->covers[2].label), std::string("Disk label"));
    }
  }

  // Sources on disk.
  fs::path src = dir / L"src";
  test::write_tree(src / L"deluxe", deluxe.source);
  test::write_tree(src / L"ad10", ad10.source);
  test::write_tree(src / L"ad32", ad32.source);
  test::write_tree(src / L"tt", tt.source);
  test::write_tree(src / L"simpsons", simpsons.source);
  test::write_bytes(src / L"ad10.iso", test::iso_of(ad10, true, "AD10TH"));
  test::write_bytes(src / L"ad32.iso", test::iso_of(ad32, false, "ADW320_C"));
  test::write_bytes(src / L"tt.iso", test::iso_of(tt, false, "TTW320CD"));
  test::write_bytes(src / L"deluxe.iso", test::iso_of(deluxe, true, "AD_DELUXE"));
  test::write_bytes(src / L"simpsons.img", test::fat_of(simpsons.source, 0, "SERIAL.TXT"));
  test::write_bytes(src / L"disk1.img", test::fat_of(simpsons.source, 1, "SERIAL.TXT"));
  test::write_bytes(src / L"disk2.img", test::fat_of(simpsons.source, 2, "SERIAL.TXT"));
  // Star Wars Screen Entertainment: the folder, the CD (level 1, no Joliet,
  // as the real one), its five 1.44 MB install floppies and a flat ZIP.
  test::write_tree(src / L"swse", swse.source);
  test::write_bytes(src / L"swse.iso", test::iso_of(swse, false, "SWSE"));
  std::vector<fs::path> swse_disks;
  for (int k = 1; k <= 5; k++) {
    swse_disks.push_back(src / (L"swse-disk" + std::to_wstring(k) + L".img"));
    test::write_bytes(swse_disks.back(), test::fat_of(swse.source, k, "", test::swse_disk, test::FatBuilder::floppy144()));
  }
  test::write_bytes(src / L"swse.zip", test::zip_folder(swse.source));
  // Star Trek: The Screen Saver: the folder, its two 1.44 MB install
  // floppies, the same disks as another copy wrote them (other image bytes,
  // the same files), the Internet Archive's ZIP of the two images (disk 2
  // first, a label scan beside them), a flat ZIP and an ISO of the files.
  test::write_tree(src / L"startrek", startrek.source);
  const fs::path st1 = src / L"st-disk1.img", st2 = src / L"st-disk2.img";
  const fs::path st1b = src / L"st-disk1-copy.img", st2b = src / L"st-disk2-copy.img";
  auto st_disk = [&](int k, const std::string& label) {
    test::FatBuilder b = test::FatBuilder::floppy144();
    b.label = label;
    return test::fat_of(startrek.source, k, "", test::startrek_disk, std::move(b));
  };
  test::write_bytes(st1, st_disk(1, ""));
  test::write_bytes(st2, st_disk(2, ""));
  test::write_bytes(st1b, st_disk(1, "WIN9XCOPY"));
  test::write_bytes(st2b, st_disk(2, "WIN9XCOPY"));
  test::write_bytes(src / L"startrek-images.zip",
                    test::zip_of_images({{"st-disk2.img", test::read_bytes(st2)}, {"st-disk1.img", test::read_bytes(st1)}}));
  test::write_bytes(src / L"startrek.zip", test::zip_folder(startrek.source));
  test::write_bytes(src / L"startrek.iso", test::iso_of(startrek, false, "STARTREK"));

  // ---- every release alone, in every source form ------------------------------------------------
  {
    fs::path root = dir / L"alone-deluxe";
    ImportResult r = run("deluxe folder", folder(src / L"deluxe"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("deluxe"));
    CHECK_EQ(r.files_dir, root / L"win" / L"FILES");
    CHECK_EQ(r.verified, std::string("files"));
    check_installed(root / L"win", deluxe, "FILES");
    phosg::JSON j = json_at(root / L"win" / L"import.json");
    CHECK_EQ(j.get_int("version"), int64_t(1));  // Deluxe's record stays version 1
    CHECK(!j.contains("package"));
    CHECK(catalog_ids(root / L"win") == deluxe.ids);
    CHECK(no_leftovers(root / L"win"));
  }
  {
    // A source that lacks one of the release's files (not a required one):
    // every file it has matches, but "files" would claim the whole release,
    // so it is "partial", and the missing one is listed.
    test::Tree partial = deluxe.source;
    partial.erase("ADE/FILES/AFI/AD2.AFI");
    test::write_tree(src / L"deluxe-partial", partial);
    fs::path root = dir / L"partial-deluxe";
    ImportResult r = run("deluxe folder, one file missing", folder(src / L"deluxe-partial"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.verified, std::string("partial"));
    CHECK(r.missing_known == std::vector<std::string>{"FILES/AFI/AD2.AFI"});
    phosg::JSON j = json_at(root / L"win" / L"import.json");
    CHECK_EQ(j.get_string("verified"), std::string("partial"));
    CHECK_EQ(j.at("missingKnown").as_list().size(), size_t(1));
    CHECK(no_leftovers(root / L"win"));
  }
  {
    // A file of another size under a path the manifest lists: refused before
    // anything is copied (it could only fail the verification later).
    test::Tree bigger = deluxe.source;
    bigger["ADE/FILES/AD40/TOASTERS.MID"].resize(bigger["ADE/FILES/AD40/TOASTERS.MID"].size() + 100000, 0x5A);
    test::write_tree(src / L"deluxe-bigger", bigger);
    fs::path root = dir / L"bigger-deluxe";
    ImportOptions o = opts_for(root, reg);
    bool copied = false;
    o.progress = [&](const Progress& p) {
      copied = copied || p.phase == Progress::Phase::copy;
      return true;
    };
    ImportResult r = run("deluxe folder, a file of another size", folder(src / L"deluxe-bigger"), o, Status::verify_failed);
    CHECK(r.message.find("FILES/AD40/TOASTERS.MID") != std::string::npos);
    CHECK(!copied);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
    CHECK(no_leftovers(root / L"win"));
    // --no-verify imports it as it is.
    o = opts_for(root, reg, false);
    r = run("the same with --no-verify", folder(src / L"deluxe-bigger"), o, Status::ok);
    CHECK_EQ(r.verified, std::string("none"));
  }
  {
    fs::path root = dir / L"alone-ad10";
    ImportResult r = run("ad10 iso", image(src / L"ad10.iso"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("ad10"));
    CHECK_EQ(r.format, std::string("iso9660+joliet"));
    CHECK_EQ(r.files_dir, root / L"win" / L"packages" / L"ad10");
    CHECK_EQ(r.verified, std::string("files"));
    CHECK_EQ(r.package_modules, size_t(7));
    check_installed(root / L"win", ad10, "packages/ad10");
    CHECK(!fs::exists(root / L"win" / L"FILES"));
    CHECK(catalog_ids(root / L"win") == ad10.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.get_int("version"), int64_t(2));
    CHECK_EQ(j.at("package").get_string("id"), std::string("ad10"));
    CHECK_EQ(j.at("package").get_string("recipe"), std::string("tree"));
    CHECK_EQ(j.at("source").get_string("kind"), std::string("iso"));
    CHECK_EQ(j.at("source").get_string("volumeId"), std::string("AD10TH"));
    CHECK_EQ(j.at("source").get_string("imageMd5"), md5_file_hex(src / L"ad10.iso"));
    CHECK_EQ(j.at("source").get_bool("imageMd5Known"), false);
    std::map<std::string, std::string> from;
    for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
    CHECK_EQ(from["packages/ad10/AD10TH/MUSIC/Toasters2k.mid"], std::string("alias:AD10TH/TOASTER1.MID"));
    CHECK_EQ(from["packages/ad10/AD10TH/TT_SND.DLL"], std::string("alias:AD10TH/MUSIC/TT_SND.DLL"));
    CHECK_EQ(from["packages/ad10/AD10TH/PICTURES/SOMEPICT.BMP"], std::string("ADE/FILES/AD10TH/PICTURES/SOMEPICT.BMP"));
    for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
    // The early Toasters 2k build has its override; the installed one keeps the name.
    phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
    CHECK_EQ(module_by_id(cat, "ad10.toast2k")->get_string("displayName"), std::string("Toasters 2k (early build)"));
    CHECK_EQ(module_by_id(cat, "ad10.toaster2")->get_string("displayName"), std::string("Toasters 2k"));
    CHECK_EQ(module_by_id(cat, "ad10.baddog")->get_string("lane"), std::string("pe32"));
    CHECK_EQ(module_by_id(cat, "ad10.baddog3")->get_string("lane"), std::string("ne16"));
    CHECK_EQ(module_by_id(cat, "ad10.baddog3")->get_string("displayName"), std::string("Bad Dog!"));  // other lane
    CHECK_EQ(module_by_id(cat, "ad10.starryni")->get_string("path"), std::string("packages/ad10/ENGINE/STARRYNI.AD"));
    CHECK(no_leftovers(root / L"win"));
  }
  for (bool as_iso : {false, true}) {
    fs::path root = dir / (as_iso ? L"alone-ad32-iso" : L"alone-ad32-folder");
    g_log.clear();
    ImportResult r = run(as_iso ? "ad32 iso" : "ad32 folder", as_iso ? image(src / L"ad32.iso") : folder(src / L"ad32"),
                         opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("ad32"));
    CHECK_EQ(r.format, std::string(as_iso ? "iso9660" : "folder"));
    check_installed(root / L"win", ad32, "packages/ad32");
    CHECK(catalog_ids(root / L"win") == ad32.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("package").get_string("recipe"), std::string("ad3zip"));
    CHECK_EQ(j.at("source").get_string("kind"), std::string(as_iso ? "iso" : "folder"));
    std::map<std::string, std::string> from;
    for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
    CHECK_EQ(from["packages/ad32/AD32/TOILET.AD"], std::string("INSTALL/TOILET.ZIP!TOILET.AD"));
    CHECK_EQ(from["packages/ad32/AD32/FOLDER.AFI"], std::string("INSTALL/AFI.ZIP!AD3.AFI"));
    CHECK_EQ(from["packages/ad32/AD32/MUSIC/OMTW.MID"], std::string("INSTALL/MUSICG.ZIP!OMTW.MID"));
    CHECK_EQ(from["packages/ad32/ENGINE/ADTASK.DLL"], std::string("INSTALL/ENGINE.ZIP!ADTASK.DLL"));
    CHECK(logged("skipped INSTALL/EXTRA.ZIP") || logged("skipped EXTRA.ZIP"));
    CHECK(logged("recovered the archive password"));
    // A ZIP member's copy keeps the member's DOS time (1995-07-17 15:16 local).
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW((r.files_dir / L"AD32" / L"GUTS.AD").c_str(), GetFileExInfoStandard, &a);
    FILETIME local;
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    CHECK(st.wYear == 1995 && st.wMonth == 7 && st.wDay == 17);
    check_no_password(root);
    CHECK(no_leftovers(root / L"win"));
  }
  {
    fs::path root = dir / L"alone-tt";
    ImportResult r = run("tt iso", image(src / L"tt.iso"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("tt"));
    check_installed(root / L"win", tt, "packages/tt");
    CHECK(catalog_ids(root / L"win") == tt.ids);
    check_no_password(root);
  }
  {
    // The merged floppy image: SERIAL.TXT's chain loops, so reading it would
    // fail — the import must never touch it (or CEREAL.TXT).
    fs::path root = dir / L"alone-simpsons";
    g_log.clear();
    ImportResult r = run("simpsons floppy", image(src / L"simpsons.img"), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("simpsons"));
    CHECK_EQ(r.format, std::string("fat12"));
    check_installed(root / L"win", simpsons, "packages/simpsons");
    CHECK(catalog_ids(root / L"win") == simpsons.ids);
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("source").get_string("kind"), std::string("floppy"));
    CHECK_EQ(j.at("source").get_string("format"), std::string("fat12"));
    std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
    for (auto& l : g_log) text += l;
    CHECK(text.find("SERIAL") == std::string::npos && text.find("CEREAL") == std::string::npos);
    check_no_password(root);
    // Trimmed names: "Grampa's Wisdom " and "Snowball I " lose the space.
    phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
    CHECK_EQ(module_by_id(cat, "simpsons.grampa")->get_string("displayName"), std::string("Grampa's Wisdom"));
    CHECK_EQ(module_by_id(cat, "simpsons.grampa")->get_string("moduleName"), std::string("Grampa's Wisdom"));
    CHECK_EQ(module_by_id(cat, "simpsons.snowball")->get_string("displayName"), std::string("Snowball I"));
  }
  {
    // The same from the two floppies (split), in either order.
    for (bool reversed : {false, true}) {
      fs::path root = dir / (reversed ? L"split-simpsons-rev" : L"split-simpsons");
      ImportResult r = run("simpsons split floppies",
                           reversed ? image(src / L"disk2.img", {src / L"disk1.img"})
                                    : image(src / L"disk1.img", {src / L"disk2.img"}),
                           opts_for(root, reg), Status::ok);
      check_installed(root / L"win", simpsons, "packages/simpsons");
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("source").at("parts").as_list().size(), size_t(2));
      CHECK(!j.at("source").contains("imageMd5"));
      CHECK_EQ(r.verified, std::string("files"));
    }
    // One disk alone is not a complete release.
    run("simpsons disk 1 alone", image(src / L"disk1.img"), opts_for(dir / L"split-1", reg), Status::source_invalid);
    run("simpsons disk 2 alone", image(src / L"disk2.img"), opts_for(dir / L"split-2", reg), Status::source_invalid);
    // A folder source with the owner's notes locked: they are never opened.
    HANDLE h1 = CreateFileW((src / L"simpsons" / L"SERIAL.TXT").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE h2 = CreateFileW((src / L"simpsons" / L"CEREAL.TXT").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE);
    run("simpsons folder, notes locked", folder(src / L"simpsons"), opts_for(dir / L"simpsons-folder", reg), Status::ok);
    check_installed(dir / L"simpsons-folder" / L"win", simpsons, "packages/simpsons");
    CloseHandle(h1);
    CloseHandle(h2);
  }

  // ---- Star Wars Screen Entertainment: Presage's installer, Intermission modules --------------------
  {
    auto froms = [](const fs::path& import_json) {
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      return from;
    };
    const auto& d = swse_disks;
    struct Form {
      const wchar_t* name;
      Source source;
      const char* format;
      const char* kind;
      size_t parts;
    };
    for (const Form& form : std::vector<Form>{
             {L"folder", folder(src / L"swse"), "folder", "folder", 0},
             {L"iso", image(src / L"swse.iso"), "iso9660", "iso", 0},
             {L"floppies", image(d[0], {d[1], d[2], d[3], d[4]}), "fat12", "floppy", 5},
             {L"floppies-reversed", image(d[4], {d[3], d[2], d[1], d[0]}), "fat12", "floppy", 5},
             {L"zip", image(src / L"swse.zip"), "zip", "zip", 0}}) {
      fs::path root = dir / (std::wstring(L"alone-swse-") + form.name);
      g_log.clear();
      ImportResult r = run(("swse " + to_utf8(form.name)).c_str(), form.source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("swse"));
      CHECK_EQ(r.format, std::string(form.format));
      CHECK_EQ(r.verified, std::string("files"));
      CHECK_EQ(r.package_modules, swse.ids.size());
      CHECK_EQ(r.files_dir, root / L"win" / L"packages" / L"swse");
      check_installed(root / L"win", swse, "packages/swse");
      CHECK(catalog_ids(root / L"win") == swse.ids);
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("package").get_string("recipe"), std::string("intermission"));
      CHECK_EQ(j.at("source").get_string("kind"), std::string(form.kind));
      CHECK_EQ(j.at("source").at("parts").as_list().size(), form.parts);
      for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
      // Where each file came from: archive members (a split one names its
      // volumes), SZDD files under their installed names, the settings.
      auto from = froms(r.import_json);
      CHECK_EQ(from["packages/swse/SAVER/JAWAS.IMX"], std::string("SWSE2.ARJ+SWSE2.A01!JAWAS.IMX"));
      CHECK_EQ(from["packages/swse/SAVER/STORYBRD.IMX"], std::string("SWSE2.A01+SWSE2.A02!STORYBRD.IMX"));
      CHECK_EQ(from["packages/swse/SAVER/SWSFX.DLL"], std::string("SWSE2.A02+SWSE2.A03!SWSFX.DLL"));
      CHECK_EQ(from["packages/swse/SAVER/ANTSW.DLL"], std::string("SWSE1.ARJ!ANTSW.DLL"));
      CHECK_EQ(from["packages/swse/SAVER/SWTEXT.TXT"], std::string("SWSE2.A03!SWTEXT.TXT"));
      CHECK_EQ(from["packages/swse/ENGINE/IMIMXPLY.IMQ"], std::string("SWSE1.ARJ!IMIMXPLY.IMQ"));
      CHECK_EQ(from["packages/swse/ENGINE/INTERMIS.EXE"], std::string("SWSE1.ARJ!INTERMIS.EXE"));
      CHECK_EQ(from["packages/swse/SAVER/BATTLE.MID"], std::string("GM_BATTL.MI_"));
      CHECK_EQ(from["packages/swse/SAVER/SWTHEME.MID"], std::string("GM_TITLE.MI_"));
      CHECK_EQ(from["packages/swse/SAVER/STRESS.DLL"], std::string("STRESS.DL_"));
      CHECK_EQ(from["packages/swse/WINDOWS/SWSE.INI"], std::string("SWSE.INI"));
      // The members the recipe skips are listed, never decoded (they are damaged).
      CHECK(logged("SWSE1.ARJ!IMAD_PLY.IMQ") && logged("SWSE1.ARJ!AD_SND.DLL") && logged("SWSE1.ARJ!INTERMSN.HLP"));
      // No decoy is named anywhere.
      std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
      for (auto& l : g_log) text += l;
      for (const std::string& decoy : test::swse_decoys()) CHECK(text.find(decoy) == std::string::npos);
      // An archive member keeps its DOS time (1994-10-12, local).
      WIN32_FILE_ATTRIBUTE_DATA a{};
      GetFileAttributesExW((r.files_dir / L"SAVER" / L"VADER.IMX").c_str(), GetFileExInfoStandard, &a);
      FILETIME local;
      FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
      SYSTEMTIME st{};
      FileTimeToSystemTime(&local, &st);
      CHECK(st.wYear == 1994 && st.wMonth == 10 && st.wDay == 12);
      // The catalog: IMX entries, names from the registry.
      phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
      const phosg::JSON* v = module_by_id(cat, "swse.vader");
      CHECK(v != nullptr);
      if (v) {
        CHECK_EQ(v->get_string("displayName"), std::string("Darth Vader"));
        CHECK_EQ(v->get_string("lane"), std::string("ne16"));
        CHECK_EQ(v->get_string("entry"), std::string("SAVERDRAW"));
        CHECK_EQ(v->get_string("abi"), std::string("intermission"));
        CHECK_EQ(v->get_string("path"), std::string("packages/swse/SAVER/VADER.IMX"));
        CHECK_EQ(v->at("controls").as_list().size(), size_t(1));
        CHECK((v->at("needs").as_list().size() == 4));  // INTRMLIB, READJPG, STRESS, SWSE
      }
      CHECK(no_leftovers(root / L"win"));
    }
    // The image md5 names it: verified "image".
    {
      test::TestRegistry by_md5;
      by_md5.image("swse", md5_file_hex(src / L"swse.iso"), fs::file_size(src / L"swse.iso"));
      ImportResult r = run("swse, known image md5", image(src / L"swse.iso"), opts_for(dir / L"swse-md5", by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(r.iso_md5_known);
      CHECK_EQ(json_at(r.import_json).at("source").get_string("volumeId"), std::string("SWSE"));
    }
    // Every install disk is needed.
    {
      ImportResult r = run("swse disk 1 alone", image(d[0]), opts_for(dir / L"swse-d1", reg), Status::source_invalid);
      CHECK(r.message.find("missing SWSE2.ARJ, SWSE2.A01, SWSE2.A02, SWSE2.A03;") != std::string::npos);
      CHECK(r.message.find("needs every install disk") != std::string::npos);
      r = run("swse disks 1-4", image(d[0], {d[1], d[2], d[3]}), opts_for(dir / L"swse-d14", reg), Status::source_invalid);
      CHECK(r.message.find("missing SWSE2.A03;") != std::string::npos);
      r = run("swse disks 2-5", image(d[1], {d[2], d[3], d[4]}), opts_for(dir / L"swse-d25", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      CHECK(!fs::exists(dir / L"swse-d1" / L"win" / L"packages" / L"swse"));
    }
    // I5: the decoys, locked in a folder source, are never opened.
    {
      std::vector<HANDLE> held;
      for (const std::string& decoy : test::swse_decoys())
        held.push_back(CreateFileW((src / L"swse" / to_wide(decoy)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
      for (HANDLE h : held) CHECK(h != INVALID_HANDLE_VALUE);
      run("swse folder, decoys locked", folder(src / L"swse"), opts_for(dir / L"swse-locked", reg), Status::ok);
      check_installed(dir / L"swse-locked" / L"win", swse, "packages/swse");
      for (HANDLE h : held) CloseHandle(h);
    }
    // I5: the volume chains stay within the registry's archives. A volume
    // that says the archive goes on past them (VOLUME_FLAG on SWSE2.A03's or
    // SWSE1.ARJ's main header) is a damaged source that names the volume it
    // points at — not a missing install disk — and a file of that name,
    // locked, is never opened; another .ARJ beside them is skipped, logged
    // and never opened either.
    {
      auto variant = [&](const wchar_t* name, const std::function<void(test::PkgFixture&)>& change) {
        test::PkgFixture f = swse;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      auto goes_on = [](const char* volume) {
        return [volume](test::PkgFixture& f) { f.source[volume] = test::arj_with_main_flags(f.source[volume], 0x14); };
      };
      const std::string past_a03 = "SWSE2.A03 says the archive continues on SWSE2.A04, which is not one of Star Wars "
                                   "Screen Entertainment's install disks (a damaged or foreign volume?)";
      ImportResult r = run("swse, SWSE2.A03 goes on", folder(variant(L"v-swse-a03on", goes_on("SWSE2.A03"))),
                           opts_for(dir / L"swse-a03on", reg), Status::source_invalid);
      CHECK_EQ(r.message, past_a03);
      CHECK(r.message.find("install disk;") == std::string::npos && r.message.find("needs every") == std::string::npos);
      fs::path a04 = variant(L"v-swse-a04", [&](test::PkgFixture& f) {
        goes_on("SWSE2.A03")(f);
        f.source["SWSE2.A04"] = test::arj_volume("SWSE2.A04", false, {test::arj_stored_entry("EXTRA.TXT", {1})});
      });
      HANDLE held = CreateFileW((a04 / L"SWSE2.A04").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(held != INVALID_HANDLE_VALUE);
      r = run("swse, SWSE2.A03 goes on, SWSE2.A04 locked", folder(a04), opts_for(dir / L"swse-a04", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, past_a03);
      CloseHandle(held);
      r = run("swse, SWSE1.ARJ goes on", folder(variant(L"v-swse-s1on", goes_on("SWSE1.ARJ"))),
              opts_for(dir / L"swse-s1on", reg), Status::source_invalid);
      CHECK(r.message.find("SWSE1.ARJ says the archive continues on SWSE1.A01, which is not one of") !=
            std::string::npos);
      CHECK(!fs::exists(dir / L"swse-a03on" / L"win" / L"packages" / L"swse"));
      fs::path other = variant(L"v-swse-otherarj", [](test::PkgFixture& f) {
        f.source["OTHER.ARJ"] = test::arj_volume("OTHER.ARJ", false, {test::arj_stored_entry("EXTRA.TXT", {1})});
      });
      held = CreateFileW((other / L"OTHER.ARJ").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(held != INVALID_HANDLE_VALUE);
      g_log.clear();
      run("swse, another .ARJ (locked)", folder(other), opts_for(dir / L"swse-otherarj", reg), Status::ok);
      CHECK(logged("skipped OTHER.ARJ (not in the recipe of Star Wars Screen Entertainment)"));
      check_installed(dir / L"swse-otherarj" / L"win", swse, "packages/swse");
      CloseHandle(held);
    }
    // Identification: the script's short name, INSTALL.DAT's size, --package, ambiguity.
    {
      auto sw = [&](const wchar_t* name, const std::function<void(test::PkgFixture&)>& change) {
        test::PkgFixture f = swse;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      auto other = sw(L"v-swse-other", [](test::PkgFixture& f) { f.source["INSTALL.DAT"] = test::intermission_dat("OTHER"); });
      ImportResult r = run("swse, another short name", folder(other), opts_for(dir / L"swse-other", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto big = sw(L"v-swse-bigdat", [](test::PkgFixture& f) {
        auto dat = test::intermission_dat("SWSE");
        dat.resize(70 * 1024, ' ');
        f.source["INSTALL.DAT"] = dat;
      });
      r = run("swse, a 70 KB INSTALL.DAT", folder(big), opts_for(dir / L"swse-bigdat", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto nofirst = sw(L"v-swse-nofirst", [](test::PkgFixture& f) { f.source.erase("SWSE1.ARJ"); });
      r = run("swse, no SWSE1.ARJ beside INSTALL.DAT", folder(nofirst), opts_for(dir / L"swse-nofirst", reg),
              Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      r = run("--package swse on ad32", folder(src / L"ad32", "swse"), opts_for(dir / L"swse-pkg", reg), Status::source_invalid);
      CHECK(r.message.find("not Star Wars Screen Entertainment") != std::string::npos);
      test::write_tree(src / L"both-swse", ad32.source);
      test::write_tree(src / L"both-swse", swse.source);
      r = run("ad32 and swse in one folder", folder(src / L"both-swse"), opts_for(dir / L"swse-both", reg),
              Status::source_invalid);
      CHECK(r.message.find("ambiguous") != std::string::npos);
      CHECK(r.message.find("Star Wars Screen Entertainment") != std::string::npos);
      r = run("both + --package swse", folder(src / L"both-swse", "swse"), opts_for(dir / L"swse-both-1", reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("swse"));
      r = run("both + --package ad32", folder(src / L"both-swse", "ad32"), opts_for(dir / L"swse-both-2", reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("ad32"));
      CHECK(identify_folder(src / L"swse") == find_package("swse"));
    }
    // Damaged sources: exit 2 (3 for an SZDD literal, which has no checksum),
    // and nothing changes, not even the catalog.
    {
      fs::path croot = dir / L"swse-corrupt";
      run("setup tt", image(src / L"tt.iso"), opts_for(croot, reg), Status::ok);
      run("setup swse", folder(src / L"swse"), opts_for(croot, reg), Status::ok);
      const test::Tree before = snapshot(croot / L"win");
      auto corrupt = [&](const char* what, const wchar_t* name, const std::function<void(test::PkgFixture&)>& change,
                         Status want, const char* in_message) {
        test::PkgFixture f = swse;
        change(f);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        ImportResult r = run(what, folder(p), opts_for(croot, reg), want);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(snapshot(croot / L"win") == before);
        CHECK(no_leftovers(croot / L"win"));
      };
      corrupt("a flipped byte in a split segment", L"v-swse-flip",
              [](test::PkgFixture& f) {
                auto& v = f.source["SWSE2.A01"];
                v[v.size() - 30] ^= 0x10;  // STORYBRD.IMX's first segment
              },
              Status::source_invalid, "SWSE2.A01!STORYBRD.IMX: CRC-32 mismatch");
      corrupt("a flipped byte in an LZH segment", L"v-swse-lzh",
              [](test::PkgFixture& f) {
                auto& v = f.source["SWSE2.A03"];
                v[120] ^= 0x04;  // inside SWSFX.DLL's second segment
              },
              Status::source_invalid, "SWSE2.A03!SWSFX.DLL");
      corrupt("a truncated .A02", L"v-swse-cut",
              [](test::PkgFixture& f) { f.source["SWSE2.A02"].resize(f.source["SWSE2.A02"].size() - 100); },
              Status::source_invalid, "SWSE2.A02: the archive is truncated");
      corrupt(".A01 of another set", L"v-swse-swap",
              [](test::PkgFixture& f) {
                f.source["SWSE2.A01"] = test::arj_volume(
                    "SWSE2.A01", true, {test::arj_stored_entry("ICLOCK.IMX", test::blob("another build"), 0x18, 100)});
              },
              Status::source_invalid, "SWSE2.ARJ!JAWAS.IMX continues, but SWSE2.A01 does not continue it");
      corrupt("an .A01 that is no ARJ volume", L"v-swse-junk",
              [](test::PkgFixture& f) { f.source["SWSE2.A01"] = test::blob("junk"); }, Status::source_invalid,
              "SWSE2.A01: not an ARJ archive");
      corrupt("a cut GM_CNTNA.MI_", L"v-swse-cutmid",
              [](test::PkgFixture& f) { f.source["GM_CNTNA.MI_"].resize(f.source["GM_CNTNA.MI_"].size() - 5); },
              Status::source_invalid, "GM_CNTNA.MI_: the compressed data ends early");
      corrupt("a KWAJ STRESS.DL_", L"v-swse-kwaj",
              [](test::PkgFixture& f) {
                auto& v = f.source["STRESS.DL_"];
                v[0] = 'K', v[1] = 'W', v[2] = 'A', v[3] = 'J';
              },
              Status::source_invalid, "KWAJ");
      corrupt("an SZDD literal changed (no checksum: the manifest tells)", L"v-swse-lit",
              [](test::PkgFixture& f) { f.source["GM_EMPIR.MI_"][15] ^= 0x01; }, Status::verify_failed,
              "differ from the release of Star Wars Screen Entertainment");
      // The planned sizes (ARJ headers, SZDD headers) are what the staging
      // budget and the manifest check see, before a byte is written.
      auto refused_early = [&](const char* what, const fs::path& source, ImportOptions o, Status want,
                               const std::string& in_message) {
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(source), o, want);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
      };
      ImportOptions few = opts_for(croot, reg);
      few.max_files = 10;
      refused_early("swse over a 10-file budget", src / L"swse", few, Status::source_invalid, "more than 10 files");
      ImportOptions small = opts_for(croot, reg);
      small.max_bytes = 4000;
      refused_early("swse over a 4000-byte budget", src / L"swse", small, Status::source_invalid,
                    "more than 4000 bytes");
      {
        test::PkgFixture f = swse;
        auto longer = test::vec("MThd");
        auto body = test::blob("a longer piece of music", 900);
        longer.insert(longer.end(), body.begin(), body.end());
        f.source["GM_BATTL.MI_"] = test::szdd_encode(longer);
        test::write_tree(src / L"v-swse-size", f.source);
        refused_early("an SZDD file of another size", src / L"v-swse-size", opts_for(croot, reg), Status::verify_failed,
                      "packages/swse/SAVER/BATTLE.MID is " + std::to_string(longer.size()) + " bytes");
      }
      {
        // Two members whose names differ only in a code page 437 letter's
        // case (u-umlaut 0x81, U-umlaut 0x9A) are one name to Windows.
        test::PkgFixture f = test::swse_fixture([](std::vector<test::ArjEntry>& e) {
          e.push_back(test::arj_stored_entry("\x81.DLL", test::blob("one")));
          e.push_back(test::arj_stored_entry("\x9A.DLL", test::blob("two")));
        });
        test::write_tree(src / L"v-swse-cp437", f.source);
        refused_early("two members, one name to Windows", src / L"v-swse-cp437", opts_for(croot, reg),
                      Status::source_invalid, "SWSE1.ARJ: two members are named \xC3\x9C.DLL");
      }
      // A cancelled re-import changes nothing either.
      ImportOptions cancel = opts_for(croot, reg);
      cancel.progress = [](const Progress& pr) { return !(pr.phase == Progress::Phase::copy && pr.done > 2000); };
      run("swse re-import, cancelled while copying", image(src / L"swse.iso"), cancel, Status::cancelled);
      CHECK(snapshot(croot / L"win") == before);
      CHECK(no_leftovers(croot / L"win"));
    }
    // The intermission invariants (registries without manifests: nothing to
    // verify against) and the required files.
    {
      const Package& builtin = *find_package("swse");
      std::vector<LooseFile> loose(builtin.loose_files.begin(), builtin.loose_files.end());
      auto variant_of = [&](const wchar_t* name, const std::function<void(std::vector<test::ArjEntry>&)>& swse1,
                            const std::map<std::string, std::vector<uint8_t>>& extra = {},
                            const std::vector<std::string>& drop = {}) {
        test::PkgFixture f = test::swse_fixture(swse1);
        for (auto& [n, b] : extra) f.source[n] = b;
        for (auto& n : drop) f.source.erase(n);
        fs::path p = src / name;
        test::write_tree(p, f.source);
        return p;
      };
      auto swse_inv = [&](const char* what, const fs::path& source, const char* expect, const test::TestRegistry& r) {
        fs::path root = dir / (L"inv-" + source.filename().wstring());
        ImportResult res = run(what, folder(source), opts_for(root, r, false), Status::source_invalid);
        CHECK(res.message.find(expect) != std::string::npos);
        CHECK(!fs::exists(root / L"win" / L"packages" / L"swse"));
        CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
      };
      auto drop_member = [](const char* n) {
        return [n](std::vector<test::ArjEntry>& e) {
          e.erase(std::remove_if(e.begin(), e.end(), [&](const test::ArjEntry& x) { return x.h.name == n; }), e.end());
        };
      };
      test::TestRegistry none;
      swse_inv("required: no SAVER\\ANTSW.DLL", variant_of(L"v-swse-noantsw", drop_member("ANTSW.DLL")),
               "missing required file(s): packages/swse/SAVER/ANTSW.DLL", none);
      test::TestRegistry lax;
      lax.get("swse").required = {};
      swse_inv("I2: INTRMLIB.DLL's ANTSW is missing", variant_of(L"v-swse-i2dll", drop_member("ANTSW.DLL")),
               "breaks I2: SAVER\\INTRMLIB.DLL needs ANTSW", lax);
      swse_inv("I2: an IMX module needs FOO",
               variant_of(L"v-swse-i2", [](std::vector<test::ArjEntry>& e) {
                 e.push_back(test::arj_stored_entry("NEEDY.IMX", test::imx_module("NEEDY", {"KERNEL", "FOO"})));
               }),
               "breaks I2: SAVER\\NEEDY.IMX needs FOO", none);
      swse_inv("I3: no ENGINE\\IMIMXPLY.IMQ", variant_of(L"v-swse-i3", drop_member("IMIMXPLY.IMQ")),
               "breaks I3: no ENGINE\\IMIMXPLY.IMQ", lax);
      swse_inv("I3: no WINDOWS\\SWSE.INI", variant_of(L"v-swse-i3ini", {}, {}, {"SWSE.INI"}),
               "breaks I3: no WINDOWS\\SWSE.INI", lax);
      {
        test::TestRegistry r;
        std::vector<LooseFile> l = loose;
        l.push_back({"IMIMXPLY.IMQ", "SAVER/IMIMXPLY.IMQ", Codec::plain});
        r.get("swse").loose_files = l;
        swse_inv("I1: an IMX reader beside the modules",
                 variant_of(L"v-swse-i1", {}, {{"IMIMXPLY.IMQ", test::ne_dll("IMIMXPLY", {"KERNEL"})}}),
                 "breaks I1: SAVER\\IMIMXPLY.IMQ belongs in ENGINE", r);
        l = loose;
        l.push_back({"SNDDRV.DLL", "ENGINE/AD_SND.DLL", Codec::plain});
        r.get("swse").loose_files = l;
        swse_inv("I3: After Dark's AD_SND in ENGINE", variant_of(L"v-swse-i3snd", {}, {{"SNDDRV.DLL", test::blob("snd")}}),
                 "breaks I3: ENGINE\\AD_SND.DLL", r);
        l = loose;
        l[1].to = "SAVER/MUSIC/BATTLE.MID";
        r.get("swse").loose_files = l;
        swse_inv("I4: a MIDI file in a subfolder", variant_of(L"v-swse-i4", {}), "breaks I4", r);
      }
    }
  }

  // ---- Star Trek: The Screen Saver: Microsoft Setup's KWAJ files, After Dark 2.0 modules ---------
  {
    auto froms = [](const fs::path& import_json) {
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      return from;
    };
    // Where the text of a startrek catalog entry says what the rules made of it.
    auto check_catalog = [&](const fs::path& win) {
      phosg::JSON cat = json_at(win / L"catalog-win.json");
      for (auto& m : cat.at("modules").as_list()) {
        if (m->get_string("package") != "startrek") {
          CHECK(!m->contains("screen"));
          continue;
        }
        CHECK_EQ(m->get_string("lane"), std::string("ne16"));
        CHECK_EQ(m->get_string("entry"), std::string("MODULE"));
        CHECK(!m->contains("abi"));
        // The fixed screen, on every entry of the package (last: below).
        CHECK_EQ(m->get_string("screen"), std::string("640x480"));
        const std::string about = m->get_string("about");
        CHECK(about.find("Authorized User") == std::string::npos);
        CHECK(about.find("wrapped by hand.") != std::string::npos);
        CHECK(about.find(" \n") == std::string::npos);
        CHECK(m->get_string("displayName").front() != ' ');
      }
      const std::string text = test::read_text(win / L"catalog-win.json");
      size_t at = text.find("\"id\": \"startrek.tribble\"");
      size_t end = text.find("\n  }", at);
      CHECK(at != std::string::npos && text.rfind("\"screen\": \"640x480\"\n", end) > at);
      const phosg::JSON* planets = module_by_id(cat, "startrek.planets");
      CHECK(planets && planets->get_string("displayName") == "Planetary Atlas" &&
            planets->get_string("moduleName") == "Planetary Atlas");
      const phosg::JSON* cells = module_by_id(cat, "startrek.braincel");
      CHECK(cells && cells->get_string("displayName") == "Brain Cells" && cells->get_string("moduleName") == "Brain Cells");
      if (cells)
        CHECK_EQ(cells->get_string("about"), std::string("ABOUT Brain Cells\n\nA made-up module whose sentence is "
                                                         "wrapped by hand.\n\nMade up for the tests."));
      const phosg::JSON* comms = module_by_id(cat, "startrek.comms");
      CHECK(comms && comms->at("controls").as_list().size() == 2);
      if (comms && comms->at("controls").as_list().size() == 2) {
        const auto& b = *comms->at("controls").as_list()[1];
        CHECK(b.get_int("index") == 3 && b.get_string("name") == "Edit Custom..." && b.get_string("type") == "button");
      }
      const phosg::JSON* sounder = module_by_id(cat, "startrek.sounder");
      CHECK(sounder && sounder->at("needs").as_list().size() == 1 && sounder->at("needs").as_list()[0]->as_string() == "AD_SND");
      if (sounder && sounder->at("controls").as_list().size() == 2)
        CHECK(sounder->at("controls").as_list()[1]->get_int("index") == 2);
      const phosg::JSON* mission = module_by_id(cat, "startrek.mission");
      CHECK(mission && mission->at("controls").as_list().empty());
    };
    struct Form {
      const wchar_t* name;
      Source source;
      const char* format;
      const char* kind;
      size_t parts;
    };
    for (const Form& form : std::vector<Form>{{L"folder", folder(src / L"startrek"), "folder", "folder", 0},
                                              {L"floppies", image(st1, {st2}), "fat12", "floppy", 2},
                                              {L"floppies-reversed", image(st2, {st1}), "fat12", "floppy", 2},
                                              {L"zip-of-images", image(src / L"startrek-images.zip"), "fat12", "floppy", 2},
                                              {L"flat-zip", image(src / L"startrek.zip"), "zip", "zip", 0},
                                              {L"iso", image(src / L"startrek.iso"), "iso9660", "iso", 0}}) {
      fs::path root = dir / (std::wstring(L"alone-startrek-") + form.name);
      g_log.clear();
      ImportResult r = run(("startrek " + to_utf8(form.name)).c_str(), form.source, opts_for(root, reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("startrek"));
      CHECK_EQ(r.format, std::string(form.format));
      CHECK_EQ(r.verified, std::string("files"));
      CHECK_EQ(r.package_modules, startrek.ids.size());
      CHECK_EQ(r.files_dir, root / L"win" / L"packages" / L"startrek");
      check_installed(root / L"win", startrek, "packages/startrek");
      CHECK(catalog_ids(root / L"win") == startrek.ids);
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.at("package").get_string("recipe"), std::string("ad2kwaj"));
      CHECK_EQ(j.at("source").get_string("kind"), std::string(form.kind));
      CHECK_EQ(j.at("source").at("parts").as_list().size(), form.parts);
      for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
      auto from = froms(r.import_json);
      CHECK_EQ(from["packages/startrek/AFTERDRK/BRAINCEL.AD"], std::string("BRAINCEL.AD_"));
      CHECK_EQ(from["packages/startrek/AFTERDRK/ST_RES/ST_VGA.DLL"], std::string("ST_VGA.DL_"));
      CHECK_EQ(from["packages/startrek/AFTERDRK/SOUNDS/JIM.WAV"], std::string("JIM.WA_"));
      CHECK_EQ(from["packages/startrek/ENGINE/AD.EXE"], std::string("AD.EX_"));
      CHECK_EQ(from.size(), size_t(27));
      // No decoy is named anywhere, and no WINDOWS folder is made.
      std::string text = test::read_text(r.import_json) + test::read_text(root / L"win" / L"catalog-win.json");
      for (auto& l : g_log) text += l;
      for (const std::string& decoy : test::startrek_decoys()) CHECK(text.find(decoy) == std::string::npos);
      CHECK(!fs::exists(r.files_dir / L"WINDOWS"));
      check_catalog(root / L"win");
      if (std::wstring_view(form.name) == L"zip-of-images") {
        CHECK(logged("reading the 2 floppy images in "));
        CHECK(logged("ignoring the rest of ") && logged(": disk1.jpg"));
        const auto& parts = j.at("source").at("parts").as_list();
        CHECK(parts.size() == 2 && ends_with_i(parts[0]->get_string("path"), "startrek-images.zip!st-disk2.img") &&
              ends_with_i(parts[1]->get_string("path"), "startrek-images.zip!st-disk1.img"));
        CHECK(parts.size() == 2 && parts[0]->get_string("md5") == md5_file_hex(st2));
      }
      if (form.parts == 2) {
        // A KWAJ file's copy keeps the compressed file's own time (the
        // floppies': 1994-07-17, local).
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW((r.files_dir / L"AFTERDRK" / L"TRIBBLE.AD").c_str(), GetFileExInfoStandard, &a);
        FILETIME local;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
        SYSTEMTIME st{};
        FileTimeToSystemTime(&local, &st);
        CHECK(st.wYear == 1994 && st.wMonth == 7 && st.wDay == 17);
      }
      CHECK(no_leftovers(root / L"win"));
    }
    // A known set of install disks: verified "image", from either copy of
    // each disk, in any order and any form; one disk alone or two copies of
    // one are not the release. The registry knows the four disk images and
    // the manifest, as the real one does.
    {
      test::TestRegistry by_md5;
      by_md5.manifest("startrek", test::manifest_of(startrek.expect));
      by_md5.disk_images("startrek", {{md5_file_hex(st1), fs::file_size(st1), 1},
                                      {md5_file_hex(st2), fs::file_size(st2), 2},
                                      {md5_file_hex(st1b), fs::file_size(st1b), 1},
                                      {md5_file_hex(st2b), fs::file_size(st2b), 2}});
      CHECK(md5_file_hex(st1) != md5_file_hex(st1b));
      test::write_bytes(src / L"st-disk2-only.zip", test::zip_of_images({{"disk2.img", test::read_bytes(st2b)}}, false));
      const std::string every_disk_and_another =
          "the images hold every install disk of Star Trek: The Screen Saver (by md5) and another image besides; "
          "checking files individually";
      int n = 0;
      for (const auto& [what, source] : std::vector<std::pair<std::string, Source>>{
               {"disks 1, 2", image(st1, {st2})},
               {"disks 2, 1", image(st2, {st1})},
               {"one disk of each copy", image(st2b, {st1})},
               {"the ZIP of the images", image(src / L"startrek-images.zip")},
               {"disk 1 and a ZIP of disk 2", image(st1, {src / L"st-disk2-only.zip"})},
               {"disk 1, then the ZIP of both", image(st1, {src / L"startrek-images.zip"})}}) {
        fs::path root = dir / (L"startrek-set-" + std::to_wstring(n++));
        g_log.clear();
        ImportResult r = run(("startrek known set: " + what).c_str(), source, opts_for(root, by_md5), Status::ok);
        CHECK_EQ(r.verified, std::string("image"));
        CHECK(r.iso_md5_known);
        CHECK(r.iso_md5.empty());
        CHECK_EQ(r.parts.size(), size_t(2));
        phosg::JSON s = json_at(r.import_json).at("source");
        CHECK_EQ(s.get_bool("imageMd5Known"), true);
        CHECK(!s.contains("imageMd5"));
        CHECK(logged("the images are the known install disks of Star Trek: The Screen Saver"));
        check_installed(root / L"win", startrek, "packages/startrek");
        if (what == "disk 1, then the ZIP of both") {
          // The ZIP's disk 1 is the image already given: read once, and
          // logged as ignored.
          const std::string zip = to_utf8(fs::absolute(src / L"startrek-images.zip").make_preferred().wstring());
          CHECK(logged("ignoring " + zip + "!st-disk1.img: the same image as " +
                       to_utf8(fs::absolute(st1).make_preferred().wstring())));
          CHECK(r.parts.size() == 2 && r.parts[1].path == zip + "!st-disk2.img");
        }
      }
      // Names in code page 437, as Explorer and 7-Zip on an English Windows
      // write a name that fits it (general-purpose bit 11 clear; 0x81
      // u-umlaut, 0x94 o-umlaut), and a volume label in it: decoded, and
      // import.json is UTF-8 JSON.
      auto utf8_source = [&](const ImportResult& r) {
        CHECK(test::strict_utf8(test::read_text(r.import_json)));
        return json_at(r.import_json).at("source");
      };
      test::write_bytes(src / L"st-cp437.zip", test::zip_of_images({{"DISK\x81\x94" "1.IMG", test::read_bytes(st1)},
                                                                   {"DISK\x81\x94" "2.IMG", test::read_bytes(st2)}}));
      // Two names that differ only in a letter outside ASCII: two names.
      test::write_bytes(src / L"st-cp437-pair.zip",
                        test::zip_of_images({{"TREK\x81.IMG", test::read_bytes(st1)}, {"TREK\x94.IMG", test::read_bytes(st2)}},
                                            false));
      for (const auto& [zip, one, two] : std::vector<std::tuple<std::wstring, std::string, std::string>>{
               {L"st-cp437.zip", "DISK\xC3\xBC\xC3\xB6" "1.IMG", "DISK\xC3\xBC\xC3\xB6" "2.IMG"},
               {L"st-cp437-pair.zip", "TREK\xC3\xBC.IMG", "TREK\xC3\xB6.IMG"}}) {
        ImportResult r = run(("startrek known set, code page 437 names: " + to_utf8(zip)).c_str(), image(src / zip),
                             opts_for(dir / (L"st-437-" + zip), by_md5), Status::ok);
        CHECK_EQ(r.verified, std::string("image"));
        check_installed(dir / (L"st-437-" + zip) / L"win", startrek, "packages/startrek");
        if (r.status != Status::ok) continue;
        const phosg::JSON s = utf8_source(r);
        const std::string where = to_utf8(fs::absolute(src / zip).make_preferred().wstring()) + "!";
        const auto& parts = s.at("parts").as_list();
        CHECK(parts.size() == 2 && parts[0]->get_string("path") == where + one && parts[1]->get_string("path") == where + two);
        CHECK_EQ(s.get_string("path"), where + one + " + " + where + two);
      }
      {
        const fs::path labelled = src / L"st-disk1-label437.img";
        test::write_bytes(labelled, st_disk(1, "STAR TR\x81K"));
        ImportResult r = run("startrek, a code page 437 volume label", image(labelled, {st2}),
                             opts_for(dir / L"st-label437", by_md5), Status::ok);
        CHECK_EQ(r.volume_id, std::string("STAR TR\xC3\xBCK"));
        if (r.status == Status::ok) CHECK_EQ(utf8_source(r).get_string("volumeId"), std::string("STAR TR\xC3\xBCK"));
      }
      ImportResult r = run("startrek disk 1 alone", image(st1), opts_for(dir / L"st-d1", by_md5), Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is missing ST_SND.DL_; importing Star Trek: The Screen Saver needs "
                                      "every install disk"));
      CHECK(logged("the source holds install disk 1 of 2 of Star Trek: The Screen Saver (by md5), not the whole set"));
      r = run("startrek disk 2 alone", image(st2b), opts_for(dir / L"st-d2", by_md5), Status::source_invalid);
      CHECK_EQ(r.message, std::string("this image is install disk 2 of 2 of Star Trek: The Screen Saver (by its md5); "
                                      "import every disk together (--image … --image …, or the ZIP they came in)"));
      g_log.clear();
      r = run("startrek two copies of disk 1", image(st1, {st1b}), opts_for(dir / L"st-d11", by_md5),
              Status::source_invalid);
      CHECK(r.message.find("missing ST_SND.DL_; importing Star Trek: The Screen Saver needs every install disk") !=
            std::string::npos);
      CHECK(logged("the source holds install disk 1 of 2 of Star Trek: The Screen Saver (by md5), not the whole set"));
      // Disk 2 with an image that holds no release at all.
      test::FatBuilder junk = test::FatBuilder::floppy144();
      junk.file("README.TXT", test::vec("not a disk of any release"));
      test::write_bytes(src / L"junk-disk.img", junk.build());
      r = run("startrek disk 2 and a stranger", image(st2, {src / L"junk-disk.img"}), opts_for(dir / L"st-d2j", by_md5),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("these images hold install disk 2 of 2 of Star Trek: The Screen Saver (by their "
                                      "md5s) but not the rest of it; import every disk together (--image … --image …, "
                                      "or the ZIP they came in)"));
      // Both copies of disk 2 are still disk 2 alone, named once.
      r = run("startrek both copies of disk 2", image(st2, {st2b}), opts_for(dir / L"st-d22", by_md5),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("these images hold install disk 2 of 2 of Star Trek: The Screen Saver (by their "
                                      "md5s) but not the rest of it; import every disk together (--image … --image …, "
                                      "or the ZIP they came in)"));
      // The whole set and an image of nothing known: the release, but not
      // "verified": "image" (the images are not only its disks); every file
      // is checked instead. The log says every disk is there, not that one
      // is missing.
      g_log.clear();
      r = run("startrek set and a stranger", image(st1, {st2, src / L"junk-disk.img"}), opts_for(dir / L"st-set-j", by_md5),
              Status::ok);
      CHECK_EQ(r.verified, std::string("files"));
      CHECK(!r.iso_md5_known);
      CHECK(logged(every_disk_and_another));
      CHECK(!logged("not the whole set"));
      check_installed(dir / L"st-set-j" / L"win", startrek, "packages/startrek");
      // Every install disk exactly once: disk 1 and both copies of disk 2 are
      // the release, verified file by file, but not its known set.
      g_log.clear();
      r = run("startrek disk 1 and both copies of disk 2", image(st1, {st2, st2b}), opts_for(dir / L"st-set-22", by_md5),
              Status::ok);
      CHECK_EQ(r.verified, std::string("files"));
      CHECK(!r.iso_md5_known);
      CHECK_EQ(r.parts.size(), size_t(3));
      CHECK_EQ(json_at(r.import_json).at("source").get_bool("imageMd5Known"), false);
      CHECK(logged(every_disk_and_another));
      CHECK(!logged("not the whole set"));
      check_installed(dir / L"st-set-22" / L"win", startrek, "packages/startrek");
      g_log.clear();
      r = run("startrek set, disk 2 again and a stranger", image(st1, {st2, st2b, src / L"junk-disk.img"}),
              opts_for(dir / L"st-set-22j", by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("files"));
      CHECK(logged("the images hold every install disk of Star Trek: The Screen Saver (by md5) and 2 other images "
                   "besides; checking files individually"));
      // A disk missing, a stranger beside it: still "not the whole set".
      g_log.clear();
      r = run("startrek disk 1 and a stranger", image(st1, {src / L"junk-disk.img"}), opts_for(dir / L"st-d1j", by_md5),
              Status::source_invalid);
      CHECK(r.message.find("missing ST_SND.DL_; importing Star Trek: The Screen Saver needs every install disk") !=
            std::string::npos);
      CHECK(logged("the source holds install disk 1 of 2 of Star Trek: The Screen Saver (by md5), not the whole set; "
                   "checking files individually"));
      CHECK(!logged("the images hold every install disk"));
      r = run("startrek set + --package swse", image(st1, {st2}, "swse"), opts_for(dir / L"st-pkg", by_md5),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("these images are Star Trek: The Screen Saver (by their md5s), not Star Wars "
                                      "Screen Entertainment"));
      // Known images of two releases together.
      test::TestRegistry two;
      two.disk_images("startrek", {{md5_file_hex(st1), fs::file_size(st1), 1}, {md5_file_hex(st2), fs::file_size(st2), 2}});
      two.image("simpsons", md5_file_hex(src / L"simpsons.img"), fs::file_size(src / L"simpsons.img"));
      r = run("startrek disk + the Simpsons image", image(st1, {src / L"simpsons.img"}), opts_for(dir / L"st-two", two),
              Status::source_invalid);
      CHECK(r.message.find("two different releases") != std::string::npos);
      // Without the known md5s, disk 2 is just an unknown image.
      r = run("startrek disk 2, md5 unknown", image(st2), opts_for(dir / L"st-d2u", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      CHECK(!fs::exists(dir / L"st-d1" / L"win" / L"packages" / L"startrek"));
    }
    // I5: the decoys, locked in a folder source, are never opened.
    {
      std::vector<HANDLE> held;
      for (const std::string& decoy : test::startrek_decoys())
        held.push_back(CreateFileW((src / L"startrek" / to_wide(decoy)).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0,
                                   nullptr));
      for (HANDLE h : held) CHECK(h != INVALID_HANDLE_VALUE);
      run("startrek folder, decoys locked", folder(src / L"startrek"), opts_for(dir / L"startrek-locked", reg), Status::ok);
      check_installed(dir / L"startrek-locked" / L"win", startrek, "packages/startrek");
      for (HANDLE h : held) CloseHandle(h);
    }
    // Identification: the setup window's title (any ASCII case), SETUP.LST's
    // size, disk 1's tag file beside it, --package.
    {
      auto st = [&](const wchar_t* name, const std::function<void(test::Tree&)>& change) {
        fs::path p = src / name;
        test::write_tree(p, test::startrek_fixture(change).source);
        return p;
      };
      auto other = st(L"v-st-title", [](test::Tree& t) { t["SETUP.LST"] = test::setup_lst("Star Trek: The Next Generation"); });
      ImportResult r = run("startrek, another title", folder(other), opts_for(dir / L"st-title", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto upper = st(L"v-st-upper", [](test::Tree& t) { t["SETUP.LST"] = test::setup_lst("STAR TREK\xAE: THE SCREEN SAVER"); });
      r = run("startrek, the title in capitals", folder(upper), opts_for(dir / L"st-upper", reg), Status::ok);
      CHECK_EQ(r.package_id, std::string("startrek"));
      auto big = st(L"v-st-biglst", [](test::Tree& t) { t["SETUP.LST"].resize(70 * 1024, ' '); });
      r = run("startrek, a 70 KB SETUP.LST", folder(big), opts_for(dir / L"st-biglst", reg), Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto notag = st(L"v-st-notag", [](test::Tree& t) { t.erase("MISSION.AD_"); });
      r = run("startrek, no MISSION.AD_ beside SETUP.LST", folder(notag), opts_for(dir / L"st-notag", reg),
              Status::source_invalid);
      CHECK(r.message.find("not a known release") != std::string::npos);
      auto nodisk2 = st(L"v-st-nodisk2", [](test::Tree& t) { t.erase("ST_SND.DL_"); });
      r = run("startrek, disk 2's tag file missing", folder(nodisk2), opts_for(dir / L"st-nodisk2", reg),
              Status::source_invalid);
      CHECK_EQ(r.message, std::string("the source is missing ST_SND.DL_; importing Star Trek: The Screen Saver needs "
                                      "every install disk"));
      r = run("--package startrek on ad32", folder(src / L"ad32", "startrek"), opts_for(dir / L"st-pkg-ad32", reg),
              Status::source_invalid);
      CHECK(r.message.find("not Star Trek: The Screen Saver") != std::string::npos);
      CHECK(identify_folder(src / L"startrek") == find_package("startrek"));
    }
    // Damaged KWAJ files: 2 for what the reader refuses, 3 for what only the
    // manifest can tell (KWAJ has no checksum and no length), and nothing
    // changes, not even the catalog.
    {
      fs::path croot = dir / L"startrek-corrupt";
      run("setup tt", image(src / L"tt.iso"), opts_for(croot, reg), Status::ok);
      run("setup startrek", folder(src / L"startrek"), opts_for(croot, reg), Status::ok);
      const test::Tree before = snapshot(croot / L"win");
      auto corrupt = [&](const char* what, const wchar_t* name, const std::function<void(test::Tree&)>& change,
                         Status want, const std::string& in_message, bool copies = true) {
        fs::path p = src / name;
        test::write_tree(p, test::startrek_fixture(change).source);
        ImportOptions o = opts_for(croot, reg);
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(p), o, want);
        CHECK(r.message.find(in_message) != std::string::npos);
        if (!copies) CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
        CHECK(no_leftovers(croot / L"win"));
      };
      corrupt("a table of another type", L"v-st-type", [](test::Tree& t) { t["COMMS.AD_"][14] = 0x40; },
              Status::source_invalid, "COMMS.AD_: MATCHLEN: code-length encoding 4 is not one of 0-3", false);
      corrupt("a header flag", L"v-st-flag", [](test::Tree& t) { t["COMMS.AD_"][12] = 0x01; }, Status::source_invalid,
              "COMMS.AD_: KWAJ header flags 0x0001 are not supported", false);
      corrupt("an SZDD file", L"v-st-szdd",
              [](test::Tree& t) { t["AD_RSRC.DL_"] = test::szdd_encode(test::vec("an SZDD file where KWAJ belongs")); },
              Status::source_invalid, "AD_RSRC.DL_: an SZDD file, not KWAJ", false);
      corrupt("tables cut short", L"v-st-cuttab", [](test::Tree& t) { t["SPOCK.AD_"].resize(16); },
              Status::source_invalid, "SPOCK.AD_: the compressed data ends inside the code tables", false);
      corrupt("a token cut short", L"v-st-cuttok", [](test::Tree& t) { t["JIM.WA_"].resize(t["JIM.WA_"].size() - 5); },
              Status::source_invalid, "JIM.WA_: the compressed data ends inside a token", false);
      corrupt("a literal changed (no checksum: the manifest tells)", L"v-st-lit",
              [](test::Tree& t) { t["TRIBBLE.AD_"][14 + 10] ^= 0x01; }, Status::verify_failed,
              "differ from the release of Star Trek: The Screen Saver");
      // Cut after its first run of 32 literals (24 bits of tables, 265 bits
      // a run: the next run begins 7 bits before the end, inside the padding):
      // a clean prefix, which the manifest's size refuses before a byte is
      // written.
      const size_t wav = startrek.expect.at("packages/startrek/AFTERDRK/SOUNDS/JIM.WAV").size();
      corrupt("cut to a clean prefix", L"v-st-prefix", [](test::Tree& t) { t["JIM.WA_"].resize(14 + (24 + 265 + 7) / 8); },
              Status::verify_failed,
              "packages/startrek/AFTERDRK/SOUNDS/JIM.WAV is 32 bytes, not " + std::to_string(wav), false);
      // The staging budget sees the expanded sizes, before a byte is written.
      auto budget = [&](const char* what, ImportOptions o, const std::string& in_message) {
        bool copied = false;
        o.progress = [&](const Progress& pr) {
          copied = copied || pr.phase == Progress::Phase::copy;
          return true;
        };
        ImportResult r = run(what, folder(src / L"startrek"), o, Status::source_invalid);
        CHECK(r.message.find(in_message) != std::string::npos);
        CHECK(!copied);
        CHECK(snapshot(croot / L"win") == before);
      };
      ImportOptions few = opts_for(croot, reg);
      few.max_files = 10;
      budget("startrek over a 10-file budget", few, "more than 10 files");
      ImportOptions small = opts_for(croot, reg);
      small.max_bytes = 4000;
      budget("startrek over a 4000-byte budget", small,
             "the source's files add up to more than 4000 bytes (at PLANETS.AD_); no known release is that large");
      // A cancelled re-import changes nothing either.
      ImportOptions cancel = opts_for(croot, reg);
      cancel.progress = [](const Progress& pr) { return !(pr.phase == Progress::Phase::copy && pr.done > 2000); };
      run("startrek re-import, cancelled while copying", image(st1, {st2}), cancel, Status::cancelled);
      CHECK(snapshot(croot / L"win") == before);
      CHECK(no_leftovers(croot / L"win"));
    }
    // The ad2kwaj invariants (registries without manifests: nothing to
    // verify against) and the required files.
    {
      const Package& builtin = *find_package("startrek");
      const std::vector<LooseFile> loose(builtin.loose_files.begin(), builtin.loose_files.end());
      auto st_inv = [&](const char* what, const wchar_t* name, const std::function<void(test::Tree&)>& change,
                        const std::function<void(std::vector<LooseFile>&)>& table, const std::string& expect,
                        bool lax = false) {
        fs::path p = src / name;
        test::write_tree(p, test::startrek_fixture(change).source);
        test::TestRegistry r;
        std::vector<LooseFile> l = loose;
        if (table) table(l);
        r.get("startrek").loose_files = l;
        if (lax) r.get("startrek").required = {};
        fs::path root = dir / (L"inv-" + std::wstring(name));
        ImportResult res = run(what, folder(p), opts_for(root, r, false), Status::source_invalid);
        CHECK(res.message.find(expect) != std::string::npos);
        CHECK(!fs::exists(root / L"win" / L"packages" / L"startrek"));
        CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
      };
      auto move_row = [](const char* from, const char* to) {
        return [from, to](std::vector<LooseFile>& l) {
          for (LooseFile& lf : l)
            if (std::string_view(lf.from) == from) lf.to = to;
        };
      };
      auto add_row = [](const char* from, const char* to, Codec codec = Codec::kwaj) {
        return [from, to, codec](std::vector<LooseFile>& l) { l.push_back({from, to, codec}); };
      };
      st_inv("I1: AD.EXE beside the modules", L"v-st-i1exe", {}, move_row("AD.EX_", "AFTERDRK/AD.EXE"),
             "breaks I1: AFTERDRK\\AD.EXE belongs in ENGINE");
      st_inv("I1: AD_SND beside the modules", L"v-st-i1snd", {}, add_row("AD_SND.DL_", "AFTERDRK/AD_SND.DLL"),
             "breaks I1: AFTERDRK\\AD_SND.DLL would shadow the engine's");
      st_inv("I2: a module needs FOO", L"v-st-i2",
             [](test::Tree& t) { t["NEEDY.AD_"] = test::kwaj_literals(test::ad20_module(" Needy", {"KERNEL", "FOO"})); },
             add_row("NEEDY.AD_", "AFTERDRK/NEEDY.AD"), "breaks I2: AFTERDRK\\NEEDY.AD needs FOO, which is not beside it");
      st_inv("I2: the sound driver needs MMEXTRA", L"v-st-i2drv",
             [](test::Tree& t) { t["AD_MME.DR_"] = test::kwaj_literals(test::ne_dll("AD_MME", {"KERNEL", "MMEXTRA"})); },
             {}, "breaks I2: AFTERDRK\\AD_MME.DRV needs MMEXTRA, which is not beside it");
      st_inv("I3: no ENGINE\\AD_SND.DLL", L"v-st-i3snd", {},
             [](std::vector<LooseFile>& l) {
               l.erase(std::remove_if(l.begin(), l.end(), [](const LooseFile& lf) { return std::string_view(lf.from) == "AD_SND.DL_"; }),
                       l.end());
             },
             "breaks I3: no ENGINE\\AD_SND.DLL", true);
      st_inv("I3: After Dark 4.0's ADTASK in ENGINE", L"v-st-i3task", {}, add_row("ST_SVGA.DL_", "ENGINE/ADTASK.DLL"),
             "breaks I3: ENGINE\\ADTASK.DLL belongs to After Dark 3.x and 4.x");
      st_inv("I3: a WINDOWS folder", L"v-st-i3win", {}, add_row("SETUP.LST", "WINDOWS/AD_PREFS.INI", Codec::plain),
             "breaks I3: WINDOWS\\AD_PREFS.INI: an After Dark 2.0 package has no WINDOWS folder");
      st_inv("I4: the sound database beside the modules", L"v-st-i4", {},
             move_row("ST_SND.DL_", "AFTERDRK/ST_SND.DLL"),
             "breaks I4: the sound database AFTERDRK\\ST_SND.DLL is not in AFTERDRK\\ST_RES", true);
      st_inv("required: no AFTERDRK\\AD_MOD.DLL", L"v-st-req", [](test::Tree& t) { t.erase("AD_MOD.DL_"); }, {},
             "missing required file(s): packages/startrek/AFTERDRK/AD_MOD.DLL");
    }
  }

  // ---- identification ---------------------------------------------------------------------------
  {
    test::write_tree(src / L"unknown", {{"README.TXT", test::vec("hello")}, {"STUFF/X.DLL", test::blob("x")}});
    ImportResult r = run("unknown folder", folder(src / L"unknown"), opts_for(dir / L"id-unknown", reg),
                         Status::source_invalid);
    CHECK(r.message.find("known:") != std::string::npos);
    for (const Package& p : builtin_packages()) CHECK(r.message.find(p.title) != std::string::npos);
    CHECK(!fs::exists(dir / L"id-unknown" / L"win"));

    // Deluxe's FILES and an AD 3.2 INSTALL in one folder: ambiguous, unless --package says which.
    test::write_tree(src / L"both", deluxe.source);
    test::write_tree(src / L"both", ad32.source);
    r = run("ambiguous", folder(src / L"both"), opts_for(dir / L"id-both", reg), Status::source_invalid);
    CHECK(r.message.find("ambiguous") != std::string::npos);
    r = run("ambiguous + --package ad32", folder(src / L"both", "ad32"), opts_for(dir / L"id-both-ad32", reg), Status::ok);
    CHECK_EQ(r.package_id, std::string("ad32"));
    r = run("ambiguous + --package deluxe", folder(src / L"both", "deluxe"), opts_for(dir / L"id-both-deluxe", reg),
            Status::ok);
    CHECK_EQ(r.package_id, std::string("deluxe"));
    // --package that does not fit.
    r = run("--package mismatch", folder(src / L"ad32", "tt"), opts_for(dir / L"id-mismatch", reg), Status::source_invalid);
    CHECK(r.message.find("Totally Twisted") != std::string::npos);
    run("--package unknown id", folder(src / L"ad32", "nosuch"), opts_for(dir / L"id-nosuch", reg), Status::error);

    // The image md5 names the package: verified "image", no manifest needed.
    test::TestRegistry by_md5;
    by_md5.image("ad32", md5_file_hex(src / L"ad32.iso"), fs::file_size(src / L"ad32.iso"));
    r = run("known image md5", image(src / L"ad32.iso"), opts_for(dir / L"id-md5", by_md5), Status::ok);
    CHECK_EQ(r.verified, std::string("image"));
    CHECK(r.iso_md5_known);
    CHECK_EQ(json_at(r.import_json).at("source").get_bool("imageMd5Known"), true);
    run("known md5 + other --package", image(src / L"ad32.iso", {}, "tt"), opts_for(dir / L"id-md5-pkg", by_md5),
        Status::source_invalid);
    // ... and its contents must then be that package's.
    test::TestRegistry wrong_md5;
    wrong_md5.image("tt", md5_file_hex(src / L"ad32.iso"), fs::file_size(src / L"ad32.iso"));
    r = run("md5 of one package, contents of another", image(src / L"ad32.iso"), opts_for(dir / L"id-md5-wrong", wrong_md5),
            Status::source_invalid);
    CHECK(r.message.find("md5 of Totally Twisted") != std::string::npos);
    // The same image given twice (or a copy of it) is one image: still the
    // known image, verified "image", one part.
    fs::copy_file(src / L"ad32.iso", src / L"ad32-copy.iso", fs::copy_options::overwrite_existing);
    for (const fs::path& again : {src / L"ad32.iso", src / L"ad32-copy.iso"}) {
      g_log.clear();
      r = run("known image given twice", image(src / L"ad32.iso", {again}),
              opts_for(dir / (again == src / L"ad32.iso" ? L"id-twice" : L"id-twice-copy"), by_md5), Status::ok);
      CHECK_EQ(r.verified, std::string("image"));
      CHECK(r.iso_md5_known);
      CHECK_EQ(r.parts.size(), size_t(1));
      CHECK(!json_at(r.import_json).at("source").contains("parts") ||
            json_at(r.import_json).at("source").at("parts").as_list().empty());
      CHECK(logged("ignoring"));
    }
    // Known images of two different releases are refused as such, not by
    // the first file the two happen to disagree on.
    test::TestRegistry two;
    two.image("ad32", md5_file_hex(src / L"ad32.iso"), fs::file_size(src / L"ad32.iso"));
    two.image("tt", md5_file_hex(src / L"tt.iso"), fs::file_size(src / L"tt.iso"));
    r = run("two releases as images", image(src / L"tt.iso", {src / L"ad32.iso"}), opts_for(dir / L"id-two", two),
            Status::source_invalid);
    CHECK(r.message.find("two different releases") != std::string::npos);
    CHECK(!fs::exists(dir / L"id-two" / L"win" / L"packages"));
    // Unknown images of different releases: the union says why it refuses.
    r = run("two unknown releases as images", image(src / L"tt.iso", {src / L"ad32.iso"}),
            opts_for(dir / L"id-two-unknown", reg), Status::source_invalid);
    CHECK(r.message.find("not the disks of one release") != std::string::npos);
    // identify_folder (the GUI's check).
    std::string why;
    CHECK(identify_folder(src / L"tt") == find_package("tt"));
    CHECK(identify_folder(src / L"unknown", &why) == nullptr && why.find("known:") != std::string::npos);
    CHECK(identify_folder(src / L"deluxe" / L"ADE" / L"FILES") == find_package("deluxe"));
  }

  // ---- recipes: fix-ups, invariants, required files, the password -----------------------------------
  {
    // A fix-up is made only from a source that matched the release.
    test::TestRegistry partial = registry_for({{"ad10", &ad10}});
    std::vector<KnownFile> m = test::manifest_of(ad10.expect);
    for (KnownFile& k : m)
      if (std::string(k.path) == "packages/ad10/AD10TH/TOASTER1.MID") k.md5 = "00000000000000000000000000000000";
    partial.manifest("ad10", m);
    g_log.clear();
    fs::path root = dir / L"fixups-partial";
    run("ad10, one fix-up source differs (--no-verify)", folder(src / L"ad10"), opts_for(root, partial, false), Status::ok);
    fs::path music = root / L"win" / L"packages" / L"ad10" / L"AD10TH" / L"MUSIC";
    CHECK(!fs::exists(music / L"Toasters2k.mid"));
    CHECK(fs::exists(music / L"Flying Toasters.mid") && fs::exists(music / L"Baby Toasters.mid"));
    CHECK(fs::exists(root / L"win" / L"packages" / L"ad10" / L"AD10TH" / L"TT_SND.DLL"));
    CHECK(logged("Toasters2k.mid skipped"));
    // Verified, the same mismatch fails the import and installs nothing.
    run("ad10, a file differs", folder(src / L"ad10"), opts_for(dir / L"fixups-verify", partial), Status::verify_failed);
    CHECK(!fs::exists(dir / L"fixups-verify" / L"win" / L"packages" / L"ad10"));
  }
  auto variant = [&](const test::PkgFixture& base, const wchar_t* name,
                     const std::function<void(test::PkgFixture&)>& change) {
    test::PkgFixture f = base;
    change(f);
    fs::path p = src / name;
    test::write_tree(p, f.source);
    return p;
  };
  auto invariant = [&](const char* what, const fs::path& source, const char* expect_in_message,
                       const test::TestRegistry& r = test::TestRegistry(), bool check = false) {
    fs::path root = dir / (L"inv-" + source.filename().wstring());
    ImportResult res = run(what, folder(source), opts_for(root, r, check), Status::source_invalid);
    CHECK(res.message.find(expect_in_message) != std::string::npos);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32") && !fs::exists(root / L"win" / L"packages" / L"tt"));
    CHECK(!fs::exists(root / L"win") || no_leftovers(root / L"win"));
  };
  {
    test::TestRegistry none;  // no manifests: nothing to verify against
    auto i1 = variant(ad32, L"v-i1", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/MODMISC.ZIP", {{"ADXPL300.DLL", test::blob("x")}, {"AD_SND.DLL", test::blob("old snd")}});
    });
    invariant("I1: AD_SND beside the modules", i1, "I1", none);
    auto i2 = variant(ad32, L"v-i2", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/NEEDY.ZIP", {{"NEEDY.AD", test::ne_module("Needy", {"KERNEL", "NOTHERE"})}});
    });
    invariant("I2: a DLL the module needs is missing", i2, "I2", none);
    test::TestRegistry lax;
    lax.get("ad32").required = {};
    auto i3 = variant(ad32, L"v-i3", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/ENGINE.ZIP", {{"AD_SND.DLL", test::blob("snd")}, {"MULTI.AM3", test::pattern(16, 1)}});
    });
    invariant("I3: no ADTASK or OLDMOD16", i3, "I3", lax);
    invariant("required: no ENGINE\\ADTASK.DLL", i3, "missing required", none);
    auto i4 = variant(tt, L"v-i4", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/CHAM.ZIP", {{"CHAM.AD", test::ne_module("Chameleon", {"ADXPL40"})}, {"EXTRA.MID", test::blob("m")}});
    });
    invariant("I4: MIDI outside MUSIC", i4, "I4", none);
    auto arch = variant(simpsons, L"v-archives", [](test::PkgFixture& f) { f.source.erase("BURNS.ZIP"); });
    invariant("a Simpsons module archive is missing", arch, "BURNS.ZIP", none);
    auto nopw = variant(ad32, L"v-nopw", [](test::PkgFixture& f) { f.source["INSTALL/INSTALL.INS"] = test::install_ins(""); });
    invariant("no password in INSTALL.INS", nopw, "password", none);
    auto corrupt = variant(ad32, L"v-corrupt", [](test::PkgFixture& f) {
      auto& z = f.source["INSTALL/GUTS.ZIP"];
      z[30 + 7 + 12 + 5] ^= 0xFF;  // inside GUTS.AD's compressed data
    });
    invariant("a damaged archive member", corrupt, "GUTS", none);
    auto notzip = variant(ad32, L"v-notzip", [](test::PkgFixture& f) { f.source["INSTALL/HELP.ZIP"] = test::blob("junk"); });
    invariant("a .ZIP that is not one", notzip, "HELP.ZIP", none);
    // Two archives' members whose names differ only in the case of a letter
    // outside ASCII (u-umlaut and U-umlaut) are one file to Windows: the
    // source is refused while it is planned, never when the second file
    // cannot be created.
    auto umlaut = variant(ad32, L"v-umlaut", [](test::PkgFixture& f) {
      with_zip(f, "INSTALL/UML1.ZIP", {{"M\xC3\xBCSIK.AD", test::ne_module("Musik", {"KERNEL"})}});
      with_zip(f, "INSTALL/UML2.ZIP", {{"M\xC3\x9CSIK.AD", test::ne_module("Musik", {"KERNEL"})}});
    });
    invariant("two archives' members, one name to Windows", umlaut, "two source files map to packages/ad32/", none);
  }

  // ---- per-package atomicity ------------------------------------------------------------------------
  fs::path aroot = dir / L"atomic";
  fs::path awin = aroot / L"win";
  {
    run("deluxe", folder(src / L"deluxe"), opts_for(aroot, reg), Status::ok);
    test::Tree files0 = snapshot(awin / L"FILES");
    std::string json0 = test::read_text(awin / L"import.json");
    ImportResult r = run("ad32 beside deluxe", folder(src / L"ad32"), opts_for(aroot, reg), Status::ok);
    CHECK(snapshot(awin / L"FILES") == files0);
    CHECK(test::read_text(awin / L"import.json") == json0);
    CHECK(catalog_ids(awin) == concat({deluxe.ids, ad32.ids}));
    CHECK_EQ(r.catalog_modules, deluxe.ids.size() + ad32.ids.size());
    CHECK_EQ(r.package_modules, ad32.ids.size());
    CHECK((r.installed == std::vector<std::string>{"After Dark 4.0 Deluxe", "After Dark 3.2"}));
    run("tt beside them", image(src / L"tt.iso"), opts_for(aroot, reg), Status::ok);
    test::Tree ad32_0 = snapshot(awin / L"packages" / L"ad32");
    test::Tree tt_0 = snapshot_files(awin / L"packages" / L"tt");
    test::Tree tt_full = snapshot(awin / L"packages" / L"tt");
    CHECK(snapshot(awin / L"FILES") == files0);
    CHECK(catalog_ids(awin) == concat({deluxe.ids, ad32.ids, tt.ids}));

    // A re-import replaces only its own package (a stray file disappears).
    test::write_bytes(awin / L"packages" / L"ad32" / L"AD32" / L"STRAY.AD", {1, 2, 3});
    run("ad32 again", image(src / L"ad32.iso"), opts_for(aroot, reg), Status::ok);
    CHECK(!fs::exists(awin / L"packages" / L"ad32" / L"AD32" / L"STRAY.AD"));
    check_installed(awin, ad32, "packages/ad32");
    CHECK(snapshot(awin / L"packages" / L"tt") == tt_full);
    CHECK(snapshot(awin / L"FILES") == files0);
    CHECK(test::read_text(awin / L"import.json") == json0);
    CHECK(no_leftovers(awin));

    // Failures and cancels change nothing, not even the catalog.
    auto everything = [&] { return snapshot(awin); };
    test::Tree before = everything();
    auto bad = variant(ad32, L"v-corrupt2", [](test::PkgFixture& f) { f.source["INSTALL/TOILET.ZIP"].resize(40); });
    run("corrupt re-import", folder(bad), opts_for(aroot, reg), Status::source_invalid);
    CHECK(everything() == before);
    ImportOptions o = opts_for(aroot, reg);
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::copy && p.done > 1000); };
    run("cancelled re-import", folder(src / L"ad32"), o, Status::cancelled);
    CHECK(everything() == before);
    o.progress = [](const Progress& p) { return !(p.phase == Progress::Phase::finalize && p.done == 0); };
    run("cancelled at the last moment", folder(src / L"tt"), o, Status::cancelled);
    CHECK(everything() == before);
    // The progress reports name the package once it is identified.
    std::set<std::string> named;
    o.progress = [&](const Progress& p) {
      named.insert(p.package);
      return !(p.phase == Progress::Phase::finalize && p.done == p.total);  // a late cancel is ignored
    };
    run("late cancel", folder(src / L"tt"), o, Status::ok);
    CHECK(named.count("Totally Twisted After Dark"));
    CHECK(snapshot_files(awin / L"packages" / L"tt") == tt_0);
    // A second operation while one holds the lock.
    {
      HANDLE held = CreateFileW((awin / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      run("concurrent import", folder(src / L"tt"), opts_for(aroot, reg), Status::error);
      CHECK_EQ(regenerate_catalog(aroot, {}, reg.span()).status, Status::error);
      CHECK_EQ(remove_package("tt", aroot, {}, reg.span()).status, Status::error);
      CloseHandle(held);
    }
    // Deluxe's own re-import leaves the packages alone.
    tt_full = snapshot(awin / L"packages" / L"tt");
    run("deluxe again", image(src / L"deluxe.iso"), opts_for(aroot, reg), Status::ok);
    CHECK(snapshot(awin / L"packages" / L"tt") == tt_full);
    check_installed(awin, ad32, "packages/ad32");
    CHECK(catalog_ids(awin) == concat({deluxe.ids, ad32.ids, tt.ids}));
    CHECK(no_leftovers(awin));
    (void)ad32_0;
  }

  // ---- recovery ----------------------------------------------------------------------------------------
  {
    auto expected_catalog = [&](const fs::path& root) {
      std::vector<CatalogTree> trees;
      fs::path w = root / L"win";
      for (const Package& p : reg.span()) {
        fs::path pr = p.is_deluxe() ? w / L"FILES" : w / L"packages" / to_wide(p.id);
        if (p.is_deluxe() ? fs::is_directory(pr) : fs::exists(pr / L"import.json")) {
          CatalogTree t;
          t.package = &p;
          t.dir = pr;
          phosg::JSON j = json_at(p.is_deluxe() ? w / L"import.json" : pr / L"import.json");
          t.verified = j.get_string("verified");
          t.imported_utc = j.get_string("importedUtc");
          trees.push_back(t);
        }
      }
      return render_catalog(build_catalog(trees));
    };
    auto move = [](const fs::path& a, const fs::path& b) {
      bool ok = false;
      for (int i = 0; i < 50 && !ok; i++)
        if (!(ok = MoveFileExW(a.c_str(), b.c_str(), 0))) Sleep(100);
      CHECK(ok);
    };
    fs::path root = dir / L"recover";
    fs::path w = root / L"win", pk = w / L"packages";
    run("setup deluxe", folder(src / L"deluxe"), opts_for(root, reg), Status::ok);
    run("setup ad32", folder(src / L"ad32"), opts_for(root, reg), Status::ok);
    run("setup tt", folder(src / L"tt"), opts_for(root, reg), Status::ok);
    test::Tree ad32_0 = snapshot(pk / L"ad32");
    const std::string full = test::read_text(w / L"catalog-win.json");

    // Died between the two renames: packages\ad32 parked, a stage and a catalog tmp left.
    move(pk / L"ad32", pk / L"ad32.old-777");
    test::write_bytes(pk / L"ad32.importing-777" / L"AD32" / L"PARTIAL.AD", {1});
    test::write_bytes(w / L"catalog-win.json.tmp-777", test::vec("{\"pending\": 1}"));
    test::write_bytes(w / L"catalog-win.json", test::vec("stale"));
    g_log.clear();
    CatalogResult cr = regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span());
    CHECK_EQ(cr.status, Status::ok);
    CHECK(snapshot(pk / L"ad32") == ad32_0);
    CHECK(logged("restored ad32"));
    CHECK(no_leftovers(w));
    CHECK_EQ(test::read_text(w / L"catalog-win.json"), expected_catalog(root));

    // Died after the swap, before the catalog rename: the old tree is deleted
    // and the catalog rebuilt — by an import that is then cancelled.
    test::write_bytes(pk / L"tt.old-778" / L"TWISTED" / L"OLD.AD", {2});
    test::write_bytes(w / L"catalog-win.json.tmp-778", test::vec("{}"));
    test::write_bytes(w / L"catalog-win.json", test::vec("stale"));
    ImportOptions o = opts_for(root, reg);
    o.progress = [](const Progress& p) { return p.phase != Progress::Phase::copy; };
    run("cancelled import after a crash", folder(src / L"ad32"), o, Status::cancelled);
    CHECK(!fs::exists(pk / L"tt.old-778"));
    CHECK(fs::exists(pk / L"tt" / L"import.json"));
    CHECK_EQ(test::read_text(w / L"catalog-win.json"), expected_catalog(root));
    CHECK(no_leftovers(w));

    // An interrupted --remove of a package, and of Deluxe.
    move(pk / L"tt", pk / L"tt.removing-779");
    CHECK_EQ(regenerate_catalog(root, {}, reg.span()).status, Status::ok);
    CHECK(!fs::exists(pk / L"tt.removing-779") && !fs::exists(pk / L"tt"));
    CHECK(catalog_ids(w) == concat({deluxe.ids, ad32.ids}));
    move(w / L"FILES", w / L"FILES.removing-780");
    CHECK_EQ(regenerate_catalog(root, {}, reg.span()).status, Status::ok);
    CHECK(!fs::exists(w / L"FILES.removing-780") && !fs::exists(w / L"FILES") && !fs::exists(w / L"import.json"));
    CHECK(catalog_ids(w) == ad32.ids);
    // A folder in packages\ that is no package is left alone (and logged).
    fs::create_directories(pk / L"zzz");
    g_log.clear();
    CHECK_EQ(regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span()).status, Status::ok);
    CHECK(fs::exists(pk / L"zzz") && logged("ignoring zzz"));
    CHECK(catalog_ids(w) == ad32.ids);
    (void)full;
  }

  // ---- --catalog-only without FILES, win_assets_dir, --remove, list --------------------------------------
  {
    fs::path root = dir / L"nofiles";
    run("ad32 alone", folder(src / L"ad32"), opts_for(root, reg), Status::ok);
    run("simpsons alone", image(src / L"simpsons.img"), opts_for(root, reg), Status::ok);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
    CHECK_EQ(win_assets_dir(root), root / L"win");
    CHECK_EQ(win_assets_dir(root / L"win"), root / L"win");  // a win dir holding only packages
    test::write_bytes(root / L"win" / L"catalog-win.json", test::vec("stale"));
    CatalogResult cr = regenerate_catalog(root, {}, reg.span());
    CHECK_EQ(cr.status, Status::ok);
    CHECK_EQ(cr.modules, ad32.ids.size() + simpsons.ids.size());
    CHECK(catalog_ids(root / L"win") == concat({ad32.ids, simpsons.ids}));
    // A root that is itself a win dir holding only packages\ still resolves.
    fs::create_directories(dir / L"bare" / L"packages");
    CHECK_EQ(win_assets_dir(dir / L"bare"), dir / L"bare");
    CHECK_EQ(win_assets_dir(dir / L"fresh"), dir / L"fresh" / L"win");

    auto states = list_packages(root, reg.span());
    CHECK_EQ(states.size(), size_t(7));
    for (auto& s : states) {
      bool want = std::string(s.package->id) == "ad32" || std::string(s.package->id) == "simpsons";
      CHECK_EQ(s.installed, want);
      if (want) CHECK(s.verified == "files" && !s.imported_utc.empty() && s.file_count > 0);
    }

    RemoveResult rr = remove_package("ad32", root, {}, reg.span());
    CHECK_EQ(rr.status, Status::ok);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32"));
    CHECK(catalog_ids(root / L"win") == simpsons.ids);
    CHECK_EQ(rr.catalog_modules, simpsons.ids.size());
    CHECK_EQ(remove_package("ad32", root, {}, reg.span()).status, Status::error);    // not installed
    CHECK_EQ(remove_package("nosuch", root, {}, reg.span()).status, Status::error);  // unknown
    CHECK(no_leftovers(root / L"win"));
    // Removing Deluxe takes FILES and its import.json.
    fs::path droot = dir / L"remove-deluxe";
    run("deluxe", folder(src / L"deluxe"), opts_for(droot, reg), Status::ok);
    run("tt", folder(src / L"tt"), opts_for(droot, reg), Status::ok);
    CHECK_EQ(remove_package("deluxe", droot, {}, reg.span()).status, Status::ok);
    CHECK(!fs::exists(droot / L"win" / L"FILES") && !fs::exists(droot / L"win" / L"import.json"));
    CHECK(catalog_ids(droot / L"win") == tt.ids);
    // Nothing installed at all: --catalog-only has nothing to do.
    CHECK_EQ(remove_package("tt", droot, {}, reg.span()).status, Status::ok);
    CHECK_EQ(regenerate_catalog(droot, {}, reg.span()).status, Status::source_invalid);
  }

  // ---- the merged catalog -------------------------------------------------------------------------------------
  {
    fs::path d = dir / L"cat-deluxe", all = dir / L"cat-all";
    run("deluxe only", folder(src / L"deluxe"), opts_for(d, reg), Status::ok);
    run("deluxe", image(src / L"deluxe.iso"), opts_for(all, reg), Status::ok);
    // Out of registry order on purpose: the catalog is in registry order anyway.
    run("simpsons", image(src / L"disk1.img", {src / L"disk2.img"}), opts_for(all, reg), Status::ok);
    run("tt", image(src / L"tt.iso"), opts_for(all, reg), Status::ok);
    run("swse", image(swse_disks[2], {swse_disks[0], swse_disks[4], swse_disks[1], swse_disks[3]}), opts_for(all, reg),
        Status::ok);
    run("startrek", image(src / L"startrek-images.zip"), opts_for(all, reg), Status::ok);
    run("ad32", image(src / L"ad32.iso"), opts_for(all, reg), Status::ok);
    run("ad10", image(src / L"ad10.iso"), opts_for(all, reg), Status::ok);
    phosg::JSON cat = json_at(all / L"win" / L"catalog-win.json");
    CHECK_EQ(cat.get_string("generator"), std::string(kCatalogGenerator));
    std::vector<std::string> ids;
    for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
    CHECK(ids == concat({deluxe.ids, ad10.ids, ad32.ids, tt.ids, simpsons.ids, swse.ids, startrek.ids}));
    // The top-level packages list: oldest release first (the cover strip's and the list
    // groups' order), while modules above stay in registry order. Star Trek:
    // The Screen Saver (1992-11) comes first; Star Wars Screen Entertainment
    // ties with the Simpsons (1994-08) and follows it, as in the registry.
    const auto& pk = cat.at("packages").as_list();
    CHECK_EQ(pk.size(), size_t(7));
    std::vector<std::pair<std::string, size_t>> want_pk = {
        {"startrek", startrek.ids.size()}, {"simpsons", simpsons.ids.size()}, {"swse", swse.ids.size()},
        {"ad32", ad32.ids.size()},         {"tt", tt.ids.size()},             {"deluxe", deluxe.ids.size()},
        {"ad10", ad10.ids.size()}};
    for (size_t i = 0; i < pk.size(); i++) {
      const Package* p = find_package(pk[i]->get_string("id"));
      CHECK(p && pk[i]->get_string("released") == std::string(p->released));
    }
    for (size_t i = 0; i < pk.size() && i < want_pk.size(); i++) {
      CHECK_EQ(pk[i]->get_string("id"), want_pk[i].first);
      CHECK_EQ(size_t(pk[i]->get_int("modules")), want_pk[i].second);
      const Package* p = find_package(want_pk[i].first);
      CHECK_EQ(pk[i]->get_string("title"), std::string(p->title));
      CHECK_EQ(pk[i]->get_string("shortTitle"), std::string(p->short_title));
      CHECK_EQ(pk[i]->get_string("root"), std::string(p->root));
      CHECK(!pk[i]->get_string("verified").empty() && !pk[i]->get_string("importedUtc").empty());
    }
    // Display names: unique per lane, case-insensitively.
    std::map<std::string, std::string> want_names = {
        {"ad40.baddog", "Bad Dog!"},
        {"classic.toilets", "Flying Toilets"},
        {"ad10.baddog", "Bad Dog! (10th Anniversary)"},
        {"ad10.baddog3", "Bad Dog!"},  // the other lane
        {"ad10.toast2k", "Toasters 2k (early build)"},
        {"ad10.toaster2", "Toasters 2k"},
        {"ad10.toasters", "Flying Toasters! (10th Anniversary)"},
        {"ad10.toilet", "Flying Toilets (10th Anniversary)"},
        {"ad10.starryni", "Starry Night Display (10th Anniversary)"},
        {"ad32.boris", "Boris"},
        {"ad32.borisb", "Boris (After Dark 3.2)"},
        {"ad32.guts", "Guts"},
        {"ad32.guts2", "guts (After Dark 3.2)"},
        {"ad32.guts3", "Guts (After Dark 3.2, GUTS3.AD)"},
        {"ad32.same", "Same Module (After Dark 3.2)"},
        {"ad32.toilet", "Flying Toilets (After Dark 3.2)"},
        {"tt.toilet", "Flying Toilets (Totally Twisted)"},
        {"tt.cham", "Chameleon"},
        {"simpsons.grampa", "Grampa's Wisdom"},
        {"swse.vader", "Darth Vader"},
        {"swse.battles", "Space Battles"},
        {"swse.swtext", "Scrolling Text"},
        {"startrek.planets", "Planetary Atlas"},
        {"startrek.braincel", "Brain Cells"},
        {"startrek.sounder", "Sounder"},
    };
    for (auto& [id, name] : want_names) {
      const phosg::JSON* m = module_by_id(cat, id);
      CHECK(m != nullptr);
      if (m) CHECK_EQ(m->get_string("displayName"), name);
    }
    CHECK_EQ(module_by_id(cat, "ad32.guts2")->get_string("moduleName"), std::string("guts"));
    CHECK_EQ(module_by_id(cat, "ad10.toast2k")->get_string("moduleName"), std::string("Toasters 2k (early build)"));
    // Only the Intermission modules carry "abi", as their last field.
    for (auto& m : cat.at("modules").as_list()) {
      const bool imx = m->get_string("package") == "swse";
      CHECK_EQ(m->contains("abi"), imx);
      if (imx) CHECK(m->get_string("abi") == "intermission" && m->at("controls").as_list().size() == 1);
    }
    {
      const std::string text = test::read_text(all / L"win" / L"catalog-win.json");
      size_t at = text.find("\"id\": \"swse.vader\"");
      size_t end = text.find("\n  }", at);
      CHECK(at != std::string::npos && text.rfind("\"abi\": \"intermission\"\n", end) > at);
    }
    // Only Star Trek: The Screen Saver's modules carry "screen" (last), and
    // only its About texts lose the stand-in line and hand-wrapped breaks.
    for (auto& m : cat.at("modules").as_list()) {
      const bool st = m->get_string("package") == "startrek";
      CHECK_EQ(m->contains("screen"), st);
      if (st) CHECK_EQ(m->get_string("screen"), std::string("640x480"));
    }
    // Every entry carries the package fields and its md5; sameAs names the
    // first entry with the same bytes.
    std::map<std::string, std::string> first;
    for (auto& m : cat.at("modules").as_list()) {
      CHECK(m->contains("package") && m->contains("packageTitle") && m->contains("moduleName") && m->contains("md5"));
      std::string md5 = m->get_string("md5");
      CHECK_EQ(md5, md5_file_hex(all / L"win" / to_wide(m->get_string("path"))));
      auto [it, fresh] = first.emplace(md5, m->get_string("id"));
      if (fresh) CHECK(!m->contains("sameAs"));
      else CHECK_EQ(m->get_string("sameAs"), it->second);
    }
    CHECK_EQ(module_by_id(cat, "ad32.same")->get_string("sameAs"), std::string("classic.samemod"));
    CHECK_EQ(module_by_id(cat, "ad10.starryni")->get_string("sameAs"), std::string("ad40.starryni"));
    CHECK_EQ(module_by_id(cat, "tt.toilet")->get_string("sameAs"), std::string("ad10.toilet"));
    CHECK_EQ(module_by_id(cat, "ad10.toilet")->get_string("package"), std::string("ad10"));
    CHECK_EQ(module_by_id(cat, "ad10.toilet")->get_string("packageTitle"), std::string("After Dark 10th Anniversary"));
    // Deluxe's entries are exactly what a Deluxe-only catalog lists.
    phosg::JSON only = json_at(d / L"win" / L"catalog-win.json");
    for (auto& m : only.at("modules").as_list()) {
      const phosg::JSON* same = module_by_id(cat, m->get_string("id"));
      CHECK(same && same->serialize() == m->serialize());
    }
    // A re-import of swse leaves every other package byte for byte.
    {
      test::Tree others;
      for (auto& [rel, bytes] : snapshot(all / L"win"))
        if (rel.rfind("packages/swse/", 0) != 0 && rel != "catalog-win.json") others[rel] = bytes;
      test::Tree mine = snapshot_files(all / L"win" / L"packages" / L"swse");
      run("swse again", image(src / L"swse.iso"), opts_for(all, reg), Status::ok);
      test::Tree after;
      for (auto& [rel, bytes] : snapshot(all / L"win"))
        if (rel.rfind("packages/swse/", 0) != 0 && rel != "catalog-win.json") after[rel] = bytes;
      CHECK(after == others);
      CHECK(snapshot_files(all / L"win" / L"packages" / L"swse") == mine);
      CHECK(catalog_ids(all / L"win") == ids);
    }
    // Names depend on what is installed; ids never do: without Deluxe, ad10's
    // Bad Dog! keeps its name.
    CHECK_EQ(remove_package("deluxe", all, {}, reg.span()).status, Status::ok);
    phosg::JSON less = json_at(all / L"win" / L"catalog-win.json");
    CHECK_EQ(module_by_id(less, "ad10.baddog")->get_string("displayName"), std::string("Bad Dog!"));
    CHECK_EQ(module_by_id(less, "ad32.same")->get_string("displayName"), std::string("Same Module"));
    CHECK(!module_by_id(less, "ad32.same")->contains("sameAs"));
  }

  // ---- adimport.exe (built-in registry: synthetic files differ from the real manifests) ------------------
  {
    auto cli = [&](const std::vector<std::wstring>& args, const char* what, std::string* out = nullptr) {
      test::ProcessResult r = test::run_process(exe, args, 300000);
      fprintf(stderr, "[%s] exit %d\n%s", what, r.exit_code, r.output.c_str());
      if (out) *out = r.output;
      return r.exit_code;
    };
    fs::path root = dir / L"cli";
    std::string out;
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"simpsons.img").wstring(), L"--dest", root.wstring()},
                 "real manifest"),
             3);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"simpsons"));
    CHECK_EQ(cli({L"--no-cover-download", L"--iso", (src / L"simpsons.img").wstring(), L"--dest", root.wstring(),
                  L"--no-verify"},
                 "--iso with a floppy image", &out),
             0);
    CHECK(out.find("The Simpsons Screen Saver") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"disk1.img").wstring(), L"--image",
                  (src / L"disk2.img").wstring(), L"--dest", root.wstring(), L"--no-verify", L"--quiet"},
                 "two --image"),
             0);
    CHECK_EQ(cli({L"--no-cover-download", L"--from", (src / L"tt").wstring(), L"--dest", root.wstring(), L"--no-verify",
                  L"--package", L"tt"},
                 "--from --package"),
             0);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"ad32.iso").wstring(), L"--dest", root.wstring(),
                  L"--no-verify", L"--package", L"tt"},
                 "--package mismatch"),
             2);
    // Star Wars Screen Entertainment through adimport.exe: the synthetic files
    // are not the release (3), --no-verify imports them, five --image.
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"swse.iso").wstring(), L"--dest", root.wstring()},
                 "swse, real manifest", &out),
             3);
    CHECK(out.find("Star Wars Screen Entertainment") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", swse_disks[0].wstring(), L"--image", swse_disks[1].wstring(),
                  L"--image", swse_disks[2].wstring(), L"--image", swse_disks[3].wstring(), L"--image",
                  swse_disks[4].wstring(), L"--dest", root.wstring(), L"--no-verify", L"--quiet"},
                 "swse, five --image"),
             0);
    CHECK(fs::exists(root / L"win" / L"packages" / L"swse" / L"SAVER" / L"VADER.IMX"));
    // Star Trek: The Screen Saver through adimport.exe: the synthetic files
    // are not the release (3); --no-verify imports them, from the two disks
    // or the ZIP of them.
    CHECK_EQ(cli({L"--no-cover-download", L"--image", st1.wstring(), L"--image", st2.wstring(), L"--dest",
                  root.wstring()},
                 "startrek, real manifest", &out),
             3);
    CHECK(out.find("Star Trek: The Screen Saver") != std::string::npos);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (src / L"startrek-images.zip").wstring(), L"--dest",
                  root.wstring(), L"--no-verify"},
                 "startrek, the ZIP of its images", &out),
             0);
    CHECK(out.find("imported 27 files") != std::string::npos);
    CHECK(fs::exists(root / L"win" / L"packages" / L"startrek" / L"AFTERDRK" / L"ST_RES" / L"ST_SND.DLL"));
    CHECK_EQ(cli({L"--no-cover-download", L"--image", st2.wstring(), L"--dest", root.wstring(), L"--no-verify"},
                 "startrek, disk 2 alone"),
             2);
    CHECK_EQ(cli({L"--image", (src / L"ad32.iso").wstring(), L"--package", L"nosuch"}, "--package unknown"), 1);
    CHECK_EQ(cli({L"--image", (src / L"ad32.iso").wstring(), L"--from", src.wstring()}, "--image + --from"), 1);
    CHECK_EQ(cli({L"--list-packages", L"--dest", root.wstring()}, "--list-packages", &out), 0);
    CHECK(out.find("simpsons") != std::string::npos && out.find("installed, ") != std::string::npos &&
          out.find("not installed") != std::string::npos);
    // The title column fits the longest title: the state starts in one column on every line.
    {
      std::vector<size_t> cols;
      size_t pos = 0;
      while ((pos = out.find("\n  ", pos)) != std::string::npos) {
        size_t eol = out.find('\n', pos + 1);
        std::string line = out.substr(pos + 1, eol == std::string::npos ? std::string::npos : eol - pos - 1);
        size_t col = line.find("installed");
        if (col != std::string::npos) cols.push_back(line.rfind("not ", col) == col - 4 ? col - 4 : col);
        pos += 3;
      }
      CHECK_EQ(cols.size(), size_t(7));
      for (size_t c : cols) CHECK_EQ(c, cols.front());
      CHECK(out.find("  swse      Star Wars Screen Entertainment installed, ") != std::string::npos);
      CHECK(out.find("  startrek  Star Trek: The Screen Saver    installed, ") != std::string::npos);
      CHECK(out.find("; download 2.8 MB (2 floppy images)") != std::string::npos);
    }
    CHECK_EQ(cli({L"--list-packages", L"--image", L"x"}, "--list-packages + a source"), 1);
    CHECK_EQ(cli({L"--remove", L"tt", L"--catalog-only"}, "--remove + --catalog-only"), 1);
    CHECK_EQ(cli({L"--remove", L"nosuch", L"--dest", root.wstring()}, "--remove unknown"), 1);
    CHECK_EQ(cli({L"--remove", L"tt", L"--dest", root.wstring()}, "--remove", &out), 0);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"tt"));
    CHECK_EQ(cli({L"--remove", L"tt", L"--dest", root.wstring()}, "--remove again"), 1);
    // --catalog-only with no FILES at all.
    CHECK_EQ(cli({L"--catalog-only", L"--dest", root.wstring()}, "--catalog-only without FILES", &out), 0);
    CHECK(out.find(std::to_string(simpsons.ids.size() + swse.ids.size() + startrek.ids.size()) + " modules") !=
          std::string::npos);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
  }
  return test::finish("import.packages");
}
