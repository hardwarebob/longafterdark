// The four real package images end to end (opt-in: AD_E2E_PKG=1; exit 77 =
// skipped). PACKAGES.md §9.
//
//   test_import_pkg_real <adimport.exe> <scratch> [<image dir>]
//
// The images are looked for in AD_SOURCE_ISO_DIR, else the third argument
// (<repo>/source_iso), and identified by size and md5, never by file name.
//   1. Each image into a fresh root through adimport.exe: exit 0, verified
//      "image", nothing missing, every file matches the package manifest, the
//      installed files are exactly the manifest (the §4 layout and counts),
//      the invariants hold, and the catalog has 46 / 44 / 13 / 15 modules in
//      the right lanes.
//   1b. The 10th Anniversary and Totally Twisted images mounted by Windows,
//      imported --from the drive: the same files as from the image (skipped
//      when mounting is refused).
//   2. All four plus Deluxe (imported --from the installed assets, which are
//      only read) in one root: 202 modules, display names unique per lane,
//      sameAs consistent, and every Deluxe entry's existing fields equal to
//      the installed catalog's.
//   3. Re-importing each package into that root changes nothing else.
// Nothing is written outside <scratch>.
#include <phosg/JSON.hh>

#include <map>
#include <set>

#include "importer.h"
#include "md5.h"
#include "names.h"
#include "run_process.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

using Tree = std::map<std::string, std::vector<uint8_t>>;

int adimport(const std::wstring& exe, const std::vector<std::wstring>& args, const char* what) {
  fprintf(stderr, "---- %s\n", what);
  ULONGLONG t0 = GetTickCount64();
  test::ProcessResult pr = test::run_process(exe, args, 1800000);
  fprintf(stderr, "%s", pr.output.c_str());
  fprintf(stderr, "adimport exited %d after %.1f s\n", pr.exit_code, (GetTickCount64() - t0) / 1000.0);
  return pr.exit_code;
}

phosg::JSON load(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

struct Expect {
  size_t files;                            // installed files, import.json aside (§4.3)
  std::map<std::string, size_t> per_dir;   // top-level dir -> files under it
  size_t modules, pe32;
};

const std::map<std::string, Expect> kExpect = {
    {"ad10", {147, {{"AD10TH", 127}, {"ENGINE", 13}, {"AFI", 7}}, 46, 17}},
    {"ad32", {89, {{"AD32", 84}, {"ENGINE", 5}}, 44, 0}},
    {"tt", {26, {{"TWISTED", 21}, {"ENGINE", 5}}, 13, 0}},
    {"simpsons", {30, {{"SIMPSONS", 25}, {"ENGINE", 5}}, 15, 0}},
};

Tree snapshot(const fs::path& dir, bool without_record = false) {
  Tree t;
  for (const std::string& rel : test::list_tree(dir))
    if (!(without_record && rel == "import.json")) t[rel] = test::read_bytes(dir / to_wide(rel));
  return t;
}

void check_package_root(const fs::path& win, const Package& p) {
  fs::path root = win;
  for (std::string_view r = p.root; !r.empty();) {
    size_t j = r.find('/');
    root /= to_wide(r.substr(0, j));
    r = j == std::string_view::npos ? std::string_view() : r.substr(j + 1);
  }
  const Expect& e = kExpect.at(p.id);
  // The import record.
  phosg::JSON j = load(root / L"import.json");
  CHECK_EQ(j.get_int("version"), int64_t(2));
  CHECK_EQ(j.get_string("verified"), std::string("image"));
  CHECK(j.at("missingKnown").as_list().empty());
  CHECK_EQ(size_t(j.get_int("fileCount")), e.files);
  CHECK_EQ(j.at("source").get_bool("imageMd5Known"), true);
  for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
  // Installed files == the manifest, byte for byte (by md5).
  std::map<std::string, const KnownFile*> manifest;
  for (const KnownFile& k : p.manifest) manifest[k.path] = &k;
  CHECK_EQ(manifest.size(), e.files);
  std::map<std::string, size_t> per_dir;
  size_t seen = 0;
  for (const std::string& rel : test::list_tree(root)) {
    if (rel == "import.json") continue;
    seen++;
    per_dir[rel.substr(0, rel.find('/'))]++;
    std::string key = std::string(p.root) + "/" + rel;
    auto it = manifest.find(key);
    if (it == manifest.end()) {
      test::g_failures++;
      fprintf(stderr, "  not in the manifest: %s\n", key.c_str());
      continue;
    }
    CHECK_EQ(md5_file_hex(root / to_wide(rel)), std::string(it->second->md5));
  }
  CHECK_EQ(seen, e.files);
  CHECK(per_dir == e.per_dir);
  // §4.2: I1 and I3 (the importer checked all of them; these are cheap to re-check).
  for (auto& [dir, n] : e.per_dir) {
    if (dir == "ENGINE" || dir == "AFI") continue;
    for (const char* never : {"AD_SND.DLL", "OLDMOD16.DLL", "OLDMOD32.DLL", "ADTASK.DLL", "ADW30.EXE"})
      CHECK(!fs::exists(root / to_wide(dir) / to_wide(never)));
  }
  CHECK(fs::exists(root / L"ENGINE" / L"AD_SND.DLL"));
  CHECK((fs::exists(root / L"ENGINE" / L"OLDMOD16.DLL") && fs::exists(root / L"ENGINE" / L"AFTERDAR.SCR")) ||
        fs::exists(root / L"ENGINE" / L"ADTASK.DLL"));
  std::string text = test::read_text(root / L"import.json");
  CHECK(text.find("SERIAL") == std::string::npos && text.find("CEREAL") == std::string::npos);
}

void check_catalog_of(const phosg::JSON& cat, const Package& p) {
  const Expect& e = kExpect.at(p.id);
  size_t n = 0, pe = 0;
  for (auto& m : cat.at("modules").as_list()) {
    if (m->get_string("package") != p.id) continue;
    n++;
    pe += m->get_string("lane") == "pe32";
    CHECK(m->get_string("id").rfind(std::string(p.id) + ".", 0) == 0);
    CHECK(m->get_string("path").rfind(std::string(p.root) + "/", 0) == 0);
  }
  fprintf(stderr, "  catalog: %s %zu modules (%zu pe32, %zu ne16)\n", p.id, n, pe, n - pe);
  CHECK_EQ(n, e.modules);
  CHECK_EQ(pe, e.pe32);
}

}  // namespace

int main(int argc, char** argv) {
  const char* on = getenv("AD_E2E_PKG");
  if (!on || std::string(on) != "1") {
    fprintf(stderr, "import.pkg_real: skipped (set AD_E2E_PKG=1 to import the four real package images)\n");
    return 77;
  }
  if (argc < 3) {
    fprintf(stderr, "usage: test_import_pkg_real <adimport.exe> <scratch> [<image dir>]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  // The installed assets (read only: a folder source for step 2), found
  // before the sandbox hides them; every adimport run here has its own --dest.
  const fs::path installed_root = test::installed_assets_root();
  fs::path scratch = test::scratch(argc - 1, argv + 1, "adw-import-pkg-real");
  test::sandbox_data_root(scratch / L"localappdata");
  fs::path images_dir = argc > 3 ? fs::path(argv[3]) : fs::path();
  if (const wchar_t* e = _wgetenv(L"AD_SOURCE_ISO_DIR"); e && *e) images_dir = e;
  images_dir = fs::absolute(images_dir).make_preferred();  // Mount-DiskImage needs an absolute path

  // ---- find the images by size and md5 --------------------------------------------------
  std::map<std::string, fs::path> image_of;
  std::error_code ec;
  for (auto& f : fs::directory_iterator(images_dir, ec)) {
    if (!f.is_regular_file()) continue;
    uint64_t size = f.file_size();
    for (const Package& p : builtin_packages()) {
      if (p.is_deluxe()) continue;
      for (const KnownImage& k : p.images)
        if (k.size == size && md5_file_hex(f.path()) == k.md5) image_of[p.id] = f.path();
    }
  }
  for (const Package& p : builtin_packages())
    if (!p.is_deluxe())
      fprintf(stderr, "%-9s %s\n", p.id,
              image_of.count(p.id) ? to_utf8(image_of[p.id].wstring()).c_str() : "(image not found)");
  if (image_of.size() != 4) {
    fprintf(stderr, "SKIP: needs all four package images in %s\n", to_utf8(images_dir.wstring()).c_str());
    return 77;
  }

  // ---- 1. each image alone ----------------------------------------------------------------
  for (const Package& p : builtin_packages()) {
    if (p.is_deluxe()) continue;
    fs::path root = scratch / (L"alone-" + to_wide(p.id));
    int code = adimport(exe, {L"--no-cover-download", L"--image", image_of[p.id].wstring(), L"--dest", root.wstring()},
                        p.title);
    CHECK_EQ(code, 0);
    if (code) continue;
    fs::path win = root / L"win";
    CHECK(!fs::exists(win / L"FILES"));
    check_package_root(win, p);
    phosg::JSON cat = load(win / L"catalog-win.json");
    CHECK_EQ(cat.at("modules").as_list().size(), kExpect.at(p.id).modules);
    CHECK_EQ(cat.at("packages").as_list().size(), size_t(1));
    check_catalog_of(cat, p);
    // A second import of the same image into the same root: the same files.
    Tree before = snapshot(win / L"packages" / to_wide(p.id), true);
    CHECK_EQ(
        adimport(exe,
                 {L"--no-cover-download", L"--image", image_of[p.id].wstring(), L"--dest", root.wstring(), L"--quiet"},
                 "again"),
        0);
    CHECK(snapshot(win / L"packages" / to_wide(p.id), true) == before);
  }

  // ---- 1b. the CDs mounted by Windows, through --from the drive ------------------------------------
  // Windows lists the 10th Anniversary disc by its Joliet names ("Toaster
  // 2k.ad"); a CD drive root is read as the disc, so the result must be the
  // image import's, file for file. Mounting may be refused by policy; that
  // skips this step rather than failing it.
  for (const char* id : {"ad10", "tt"}) {
    const fs::path& iso = image_of[id];
    std::wstring q = L"'" + iso.wstring() + L"'";
    std::wstring ps = L"$i = Get-DiskImage -ImagePath " + q + L"; $was = $i.Attached; "
                      L"if (-not $was) { $i = Mount-DiskImage -ImagePath " + q + L" -PassThru }; $l = $null; "
                      L"for ($k = 0; $k -lt 40 -and -not $l; $k++) { $l = ($i | Get-Volume).DriveLetter; "
                      L"if (-not $l) { Start-Sleep -Milliseconds 250 } }; "
                      L"Write-Output ('DRIVE=' + $l + ' WAS=' + $was)";
    const wchar_t* powershell = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    test::ProcessResult m = test::run_process(powershell, {L"-NoProfile", L"-NonInteractive", L"-Command", ps}, 120000);
    size_t at = m.output.find("DRIVE=");
    char letter = at != std::string::npos && at + 6 < m.output.size() ? m.output[at + 6] : 0;
    if (!isalpha((unsigned char)letter)) {
      fprintf(stderr, "---- %s mounted: could not mount the image; step skipped\n%s", id, m.output.c_str());
      continue;
    }
    fs::path root = scratch / (L"drive-" + to_wide(id));
    std::wstring drive = std::wstring(1, wchar_t(letter)) + L":\\";
    int code = adimport(exe, {L"--no-cover-download", L"--from", drive, L"--dest", root.wstring()},
                        (std::string(id) + " --from the mounted disc").c_str());
    if (m.output.find("WAS=False") != std::string::npos)
      test::run_process(
          powershell, {L"-NoProfile", L"-NonInteractive", L"-Command", L"Dismount-DiskImage -ImagePath " + q}, 120000);
    CHECK_EQ(code, 0);
    if (code) continue;
    fs::path pkg_dir = root / L"win" / L"packages" / to_wide(id);
    phosg::JSON j = load(pkg_dir / L"import.json");
    CHECK_EQ(j.get_string("verified"), std::string("files"));
    CHECK_EQ(j.at("source").get_string("kind"), std::string("folder"));
    CHECK(j.at("missingKnown").as_list().empty());
    CHECK(snapshot(pkg_dir, true) == snapshot(scratch / (L"alone-" + to_wide(id)) / L"win" / L"packages" / to_wide(id), true));
    CHECK_EQ(load(root / L"win" / L"catalog-win.json").at("modules").as_list().size(), kExpect.at(id).modules);
  }

  // ---- 2. all five in one root ------------------------------------------------------------------
  fs::path installed = installed_root.empty() ? fs::path() : win_assets_dir(installed_root);
  if (installed.empty() || !fs::is_directory(installed / L"FILES" / L"AD40") ||
      !fs::exists(installed / L"catalog-win.json")) {
    fprintf(stderr, "no installed Deluxe assets at %s; the combined check is skipped\n", to_utf8(installed.wstring()).c_str());
    return test::finish("import.pkg_real");
  }
  fs::path all = scratch / L"all";
  // The installed Deluxe tree is a folder source: read, never written.
  CHECK_EQ(adimport(exe, {L"--no-cover-download", L"--from", installed.wstring(), L"--dest", all.wstring()},
                    "Deluxe from the installed assets"),
           0);
  for (const char* id : {"simpsons", "tt", "ad32", "ad10"})
    CHECK_EQ(
        adimport(exe,
                 {L"--no-cover-download", L"--image", image_of[id].wstring(), L"--dest", all.wstring(), L"--quiet"},
                 id),
        0);
  fs::path win = all / L"win";
  phosg::JSON cat = load(win / L"catalog-win.json");
  const auto& mods = cat.at("modules").as_list();
  fprintf(stderr, "combined catalog: %zu modules\n", mods.size());
  CHECK_EQ(mods.size(), size_t(202));
  CHECK_EQ(cat.at("packages").as_list().size(), size_t(5));
  for (const Package& p : builtin_packages())
    if (!p.is_deluxe()) {
      check_package_root(win, p);
      check_catalog_of(cat, p);
    }
  // Display names unique per lane; sameAs names an earlier entry with the same md5.
  std::set<std::string> names;
  std::map<std::string, std::string> md5_of;
  size_t same = 0;
  for (auto& m : mods) {
    std::string key = m->get_string("lane") + "\n" + ascii_lower(m->get_string("displayName"));
    if (!names.insert(key).second) {
      test::g_failures++;
      fprintf(stderr, "  display name repeated in its lane: %s\n", m->get_string("displayName").c_str());
    }
    if (m->contains("sameAs")) {
      same++;
      auto it = md5_of.find(m->get_string("sameAs"));
      CHECK(it != md5_of.end() && it->second == m->get_string("md5"));
    }
    md5_of[m->get_string("id")] = m->get_string("md5");
  }
  fprintf(stderr, "  %zu entries point at an identical earlier module (sameAs)\n", same);
  // Deluxe's entries: every existing field as the installed catalog has it.
  // The installed catalog may cover other releases too (any the user has
  // imported); only its Deluxe entries are the reference here (a catalog
  // from before packages has no "package" field: all Deluxe).
  phosg::JSON ref = load(installed / L"catalog-win.json");
  std::map<std::string, const phosg::JSON*> ours;
  for (auto& m : mods) ours[m->get_string("id")] = m.get();
  size_t deluxe = 0;
  for (auto& m : ref.at("modules").as_list()) {
    if (m->contains("package") && m->get_string("package") != "deluxe") continue;
    auto it = ours.find(m->get_string("id"));
    CHECK(it != ours.end());
    if (it == ours.end()) continue;
    deluxe++;
    CHECK_EQ(it->second->get_string("package"), std::string("deluxe"));
    for (auto& [k, v] : m->as_dict()) {
      if (!it->second->contains(k) || it->second->at(k).serialize() != v->serialize()) {
        test::g_failures++;
        fprintf(stderr, "  %s.%s differs from the installed catalog\n", m->get_string("id").c_str(), k.c_str());
      }
    }
  }
  CHECK_EQ(deluxe, size_t(84));

  // ---- 3. re-imports are isolated -----------------------------------------------------------
  auto everything_but = [&](const std::string& id) {
    Tree t;
    for (auto& [rel, d] : snapshot(win))
      if (rel.rfind("packages/" + id + "/", 0) != 0 && rel != "catalog-win.json") t[rel] = d;
    return t;
  };
  for (const char* id : {"ad10", "ad32", "tt", "simpsons"}) {
    Tree others = everything_but(id);
    Tree mine = snapshot(win / L"packages" / to_wide(id), true);
    CHECK_EQ(
        adimport(exe,
                 {L"--no-cover-download", L"--image", image_of[id].wstring(), L"--dest", all.wstring(), L"--quiet"},
                 (std::string("re-import ") + id).c_str()),
        0);
    CHECK(everything_but(id) == others);
    CHECK(snapshot(win / L"packages" / to_wide(id), true) == mine);
  }
  CHECK_EQ(load(win / L"catalog-win.json").at("modules").as_list().size(), size_t(202));
  return test::finish("import.pkg_real");
}
