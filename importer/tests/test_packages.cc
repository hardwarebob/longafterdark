// Multi-package imports on synthetic sources (PACKAGES.md §9, tests/
// pkg_fixture.h):
//   identification   every release as a folder, an ISO and a FAT image;
//                    unknown and ambiguous sources; --package; image md5s
//   recipes          exact file sets, fix-ups only from matching sources,
//                    the §4.2 invariants, required files and archives, the
//                    derived password (never written or logged), I5
//   atomicity        a package import leaves Deluxe and every other package
//                    byte for byte; re-imports replace only their package;
//                    failures and cancels change nothing
//   recovery         every interrupted swap and removal state
//   catalog          ids, order, the displayName rule, overrides, trimming,
//                    sameAs, the packages array, Deluxe entries unchanged
//   commands         --catalog-only without FILES, --remove, --list-packages,
//                    and adimport.exe's options and exit codes
//
//   test_import_packages <adimport.exe> <scratch>
#include <phosg/JSON.hh>

#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "catalog.h"
#include "importer.h"
#include "md5.h"
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
                         tt = test::tt_fixture(), simpsons = test::simpsons_fixture();
  test::TestRegistry reg = registry_for(
      {{"deluxe", &deluxe}, {"ad10", &ad10}, {"ad32", &ad32}, {"tt", &tt}, {"simpsons", &simpsons}});

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
    CHECK_EQ(states.size(), size_t(5));
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
    run("ad32", image(src / L"ad32.iso"), opts_for(all, reg), Status::ok);
    run("ad10", image(src / L"ad10.iso"), opts_for(all, reg), Status::ok);
    phosg::JSON cat = json_at(all / L"win" / L"catalog-win.json");
    CHECK_EQ(cat.get_string("generator"), std::string(kCatalogGenerator));
    std::vector<std::string> ids;
    for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
    CHECK(ids == concat({deluxe.ids, ad10.ids, ad32.ids, tt.ids, simpsons.ids}));
    // The top-level packages list: oldest release first (the cover strip's and the list
    // groups' order), while modules above stay in registry order.
    const auto& pk = cat.at("packages").as_list();
    CHECK_EQ(pk.size(), size_t(5));
    std::vector<std::pair<std::string, size_t>> want_pk = {{"simpsons", simpsons.ids.size()}, {"ad32", ad32.ids.size()},
                                                            {"tt", tt.ids.size()},             {"deluxe", deluxe.ids.size()},
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
    };
    for (auto& [id, name] : want_names) {
      const phosg::JSON* m = module_by_id(cat, id);
      CHECK(m != nullptr);
      if (m) CHECK_EQ(m->get_string("displayName"), name);
    }
    CHECK_EQ(module_by_id(cat, "ad32.guts2")->get_string("moduleName"), std::string("guts"));
    CHECK_EQ(module_by_id(cat, "ad10.toast2k")->get_string("moduleName"), std::string("Toasters 2k (early build)"));
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
    CHECK_EQ(cli({L"--image", (src / L"ad32.iso").wstring(), L"--package", L"nosuch"}, "--package unknown"), 1);
    CHECK_EQ(cli({L"--image", (src / L"ad32.iso").wstring(), L"--from", src.wstring()}, "--image + --from"), 1);
    CHECK_EQ(cli({L"--list-packages", L"--dest", root.wstring()}, "--list-packages", &out), 0);
    CHECK(out.find("simpsons") != std::string::npos && out.find("installed, ") != std::string::npos &&
          out.find("not installed") != std::string::npos);
    CHECK_EQ(cli({L"--list-packages", L"--image", L"x"}, "--list-packages + a source"), 1);
    CHECK_EQ(cli({L"--remove", L"tt", L"--catalog-only"}, "--remove + --catalog-only"), 1);
    CHECK_EQ(cli({L"--remove", L"nosuch", L"--dest", root.wstring()}, "--remove unknown"), 1);
    CHECK_EQ(cli({L"--remove", L"tt", L"--dest", root.wstring()}, "--remove", &out), 0);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"tt"));
    CHECK_EQ(cli({L"--remove", L"tt", L"--dest", root.wstring()}, "--remove again"), 1);
    // --catalog-only with no FILES at all.
    CHECK_EQ(cli({L"--catalog-only", L"--dest", root.wstring()}, "--catalog-only without FILES", &out), 0);
    CHECK(out.find(std::to_string(simpsons.ids.size()) + " modules") != std::string::npos);
    CHECK(!fs::exists(root / L"win" / L"FILES"));
  }
  return test::finish("import.packages");
}
