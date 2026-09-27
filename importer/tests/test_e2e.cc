// End to end against the real thing (opt-in: AD_E2E=1; exit 77 = skipped).
//
//   1. adimport --download --dest <scratch>: fetches the Internet Archive
//      image into <scratch>-downloads (or AD_E2E_DOWNLOAD_DIR; reused when
//      already there and matching, and linked from the installed data
//      folder's downloads when the user has it there), verifies its md5,
//      imports.
//   2. import.json must record the known image md5 and "verified": "image";
//      the image's md5 is recomputed here independently, every recorded file
//      is re-hashed on disk, and every file must match the built-in manifest.
//   3. The same image through --iso, the imported tree through --from (a
//      folder source, verified file by file against the manifest), and the
//      image mounted as a drive by Windows (Mount-DiskImage; skipped if that
//      is refused) through --from, must all give the same files and md5s.
//   4. Every file of the previously seeded tree (the installed data folder's
//      assets\win\FILES, read only, or AD_E2E_SEED) is byte-compared with the import —
//      the seed came from HTTP range reads of the same image and this is its
//      validation — and the seed is then imported as a folder source too.
//      Files the import has and the seed lacks are listed but not failures.
#include <cctype>
#include <map>

#include "importer.h"
#include "md5.h"
#include "run_process.h"
#include "test_util.h"

#include <phosg/JSON.hh>

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

int run_adimport(const std::wstring& exe, const std::vector<std::wstring>& args, const char* what) {
  fprintf(stderr, "---- %s\n", what);
  ULONGLONG t0 = GetTickCount64();
  test::ProcessResult pr = test::run_process(exe, args);
  fprintf(stderr, "%s", pr.output.c_str());
  fprintf(stderr, "adimport exited %d after %.1f s\n", pr.exit_code, (GetTickCount64() - t0) / 1000.0);
  return pr.exit_code;
}

phosg::JSON load_json(const fs::path& root) {
  return phosg::JSON::parse(test::read_text(root / L"win" / L"import.json"));
}

// path -> md5 of an import.json's file list.
std::map<std::string, std::string> file_md5s(const phosg::JSON& j) {
  std::map<std::string, std::string> m;
  for (const auto& f : j.at("files").as_list()) m[f->get_string("path")] = f->get_string("md5");
  return m;
}

size_t count_known(const phosg::JSON& j, const char* state) {
  size_t n = 0;
  for (const auto& f : j.at("files").as_list()) n += f->get_string("known") == state;
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  const char* e2e = getenv("AD_E2E");
  if (!e2e || std::string(e2e) != "1") {
    fprintf(stderr, "import.e2e: skipped (set AD_E2E=1 to download the ~400 MB image and compare)\n");
    return 77;
  }
  if (argc < 3) {
    fprintf(stderr, "usage: test_import_e2e <adimport.exe> <scratch assets root>\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  fs::path dest = fs::absolute(argv[2]);
  fs::path dest_iso = dest.wstring() + L"-iso", dest_folder = dest.wstring() + L"-folder",
           dest_seed = dest.wstring() + L"-seed", dest_downloads = dest.wstring() + L"-downloads",
           dest_lad = dest.wstring() + L"-localappdata";
  // The user's data folder is only ever read: found here, before the sandbox
  // hides it, for an image already downloaded and the seeded tree (step 4).
  const fs::path installed_data = test::installed_data_root();
  const fs::path installed_assets = test::installed_assets_root();
  std::error_code ec;
  for (const fs::path& d : {dest, dest_iso, dest_folder, dest_seed, dest_lad}) fs::remove_all(d, ec);
  fs::create_directories(dest);
  test::sandbox_data_root(dest_lad);

  // ---- 1. download + import ------------------------------------------------
  // Into AD_E2E_DOWNLOAD_DIR, else a scratch folder next to the root (kept
  // between runs), where an image the user already downloaded is linked (or
  // copied) first so it is not fetched again; the importer checks its md5
  // either way.
  fs::path download_dir = dest_downloads;
  if (const wchar_t* d = _wgetenv(L"AD_E2E_DOWNLOAD_DIR"); d && *d) download_dir = d;
  const fs::path cached = download_dir / kDeluxeIsoFileName;
  if (!installed_data.empty() && !fs::exists(cached, ec)) {
    const fs::path mine = installed_data / L"downloads" / kDeluxeIsoFileName;
    if (fs::is_regular_file(mine, ec)) {
      fs::create_directories(download_dir, ec);
      if (!CreateHardLinkW(cached.c_str(), mine.c_str(), nullptr)) CopyFileW(mine.c_str(), cached.c_str(), TRUE);
      fprintf(stderr, "using %s (the user's download, read only)\n", to_utf8(mine.wstring()).c_str());
    }
  }
  std::vector<std::wstring> args = {L"--no-cover-download", L"--download", L"--dest", dest.wstring(), L"--download-dir",
                                    download_dir.wstring()};
  int code = run_adimport(exe, args, "adimport --download (fetches ~400 MB unless already downloaded)");
  CHECK_EQ(code, 0);
  if (code != 0) return test::finish("import.e2e");

  // ---- 2. import.json + the image md5, recomputed here ----------------------
  phosg::JSON j = load_json(dest);
  const phosg::JSON& src = j.at("source");
  CHECK_EQ(src.get_string("kind"), std::string("download"));
  CHECK_EQ(src.get_string("isoMd5"), std::string(kDeluxeIsoMd5));
  CHECK_EQ(src.get_bool("isoMd5Known"), true);
  CHECK_EQ(j.get_string("verified"), std::string("image"));
  fs::path iso = to_wide(src.get_string("path"));
  std::string iso_md5 = md5_file_hex(iso);
  fprintf(stderr, "image %s\n  size %llu, md5 %s (joliet %s, volume \"%s\")\n", src.get_string("path").c_str(),
          (unsigned long long)fs::file_size(iso), iso_md5.c_str(), src.get_bool("joliet") ? "yes" : "no",
          src.get_string("volumeId").c_str());
  CHECK_EQ(iso_md5, std::string(kDeluxeIsoMd5));

  // Each recorded file is on disk with its recorded md5.
  fs::path files = dest / L"win" / L"FILES";
  size_t recorded = 0;
  for (const auto& f : j.at("files").as_list()) {
    recorded++;
    fs::path p = dest / L"win" / to_wide(f->get_string("path"));
    CHECK_EQ(md5_file_hex(p), f->get_string("md5"));
  }
  fprintf(stderr, "import.json lists %zu files (%lld bytes)\n", recorded, (long long)j.get_int("totalBytes"));
  // ... and the built-in manifest describes exactly this image.
  CHECK_EQ(recorded, known_files().size());
  CHECK_EQ(count_known(j, "match"), recorded);
  CHECK(j.at("missingKnown").as_list().empty());
  const auto want = file_md5s(j);

  // ---- 3. the same content through --iso and --from -------------------------
  code = run_adimport(exe, {L"--no-cover-download", L"--iso", iso.wstring(), L"--dest", dest_iso.wstring()},
                      "adimport --iso");
  CHECK_EQ(code, 0);
  if (code == 0) {
    phosg::JSON ji = load_json(dest_iso);
    CHECK_EQ(ji.at("source").get_string("kind"), std::string("iso"));
    CHECK_EQ(ji.get_string("verified"), std::string("image"));
    CHECK(file_md5s(ji) == want);
  }
  code = run_adimport(exe,
                      {L"--no-cover-download", L"--from", (dest / L"win").wstring(), L"--dest", dest_folder.wstring()},
                      "adimport --from <the imported tree>");
  CHECK_EQ(code, 0);
  if (code == 0) {
    phosg::JSON jf = load_json(dest_folder);
    CHECK_EQ(jf.at("source").get_string("kind"), std::string("folder"));
    CHECK_EQ(jf.get_string("verified"), std::string("files"));  // no image: checked file by file
    CHECK_EQ(count_known(jf, "match"), want.size());
    CHECK(file_md5s(jf) == want);
  }

  // ---- 3b. the image mounted as a CD drive (Windows' own CDFS) --------------
  // The --from route as a user with the disc would take it. Mounting may be
  // refused by policy; that skips this step rather than failing it.
  {
    std::wstring q = L"'" + iso.wstring() + L"'";
    std::wstring ps = L"$i = Get-DiskImage -ImagePath " + q + L"; $was = $i.Attached; "
                      L"if (-not $was) { $i = Mount-DiskImage -ImagePath " + q + L" -PassThru }; $l = $null; "
                      L"for ($k = 0; $k -lt 40 -and -not $l; $k++) { $l = ($i | Get-Volume).DriveLetter; "
                      L"if (-not $l) { Start-Sleep -Milliseconds 250 } }; "
                      L"Write-Output ('DRIVE=' + $l + ' WAS=' + $was)";
    test::ProcessResult m =
        test::run_process(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                          {L"-NoProfile", L"-NonInteractive", L"-Command", ps}, 120000);
    size_t at = m.output.find("DRIVE=");
    char letter = at != std::string::npos && at + 6 < m.output.size() ? m.output[at + 6] : 0;
    bool we_mounted = m.output.find("WAS=False") != std::string::npos;
    if (!isalpha((unsigned char)letter)) {
      fprintf(stderr, "---- mounted drive: could not mount the image; step skipped\n%s", m.output.c_str());
    } else {
      fs::path dest_drive = dest.wstring() + L"-drive";
      fs::remove_all(dest_drive, ec);
      std::wstring root = std::wstring(1, wchar_t(letter)) + L":\\";
      code = run_adimport(exe, {L"--no-cover-download", L"--from", root, L"--dest", dest_drive.wstring()},
                          "adimport --from <mounted image>");
      CHECK_EQ(code, 0);
      if (code == 0) {
        phosg::JSON jd = load_json(dest_drive);
        CHECK_EQ(jd.get_string("verified"), std::string("files"));
        CHECK_EQ(count_known(jd, "match"), want.size());
        CHECK(file_md5s(jd) == want);
      }
      if (we_mounted)
        test::run_process(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                          {L"-NoProfile", L"-NonInteractive", L"-Command", L"Dismount-DiskImage -ImagePath " + q},
                          120000);
    }
  }

  // ---- 4. byte-compare against the seeded tree ------------------------------
  fs::path seed;
  if (const wchar_t* s = _wgetenv(L"AD_E2E_SEED")) seed = s;
  else if (!installed_assets.empty()) seed = win_assets_dir(installed_assets) / L"FILES";
  if (!fs::is_directory(seed)) {
    fprintf(stderr, "no seeded tree at %s; nothing to compare\n", to_utf8(seed.wstring()).c_str());
    return test::finish("import.e2e");
  }
  auto seeded = test::list_tree(seed);
  auto imported = test::list_tree(files);
  std::map<std::string, bool> in_import;
  for (auto& r : imported) in_import[r] = false;
  size_t same = 0;
  std::vector<std::string> differ, missing;
  for (const std::string& rel : seeded) {
    auto it = in_import.find(rel);
    if (it == in_import.end()) {
      missing.push_back(rel);
      continue;
    }
    it->second = true;
    if (test::read_bytes(seed / to_wide(rel)) == test::read_bytes(files / to_wide(rel))) {
      same++;
    } else {
      differ.push_back(rel);
    }
  }
  fprintf(stderr, "seeded tree %s: %zu files\n", to_utf8(seed.wstring()).c_str(), seeded.size());
  fprintf(stderr, "  identical: %zu\n  differ: %zu\n  missing from import: %zu\n", same, differ.size(),
          missing.size());
  for (auto& r : differ) {
    fprintf(stderr, "    DIFFERS %s (seed md5 %s, import md5 %s)\n", r.c_str(),
            md5_file_hex(seed / to_wide(r)).c_str(), md5_file_hex(files / to_wide(r)).c_str());
  }
  for (auto& r : missing) fprintf(stderr, "    MISSING %s\n", r.c_str());
  size_t extra = 0;
  for (auto& [rel, seen] : in_import)
    if (!seen) extra++;
  fprintf(stderr, "  in the import but not the seed: %zu\n", extra);
  for (auto& [rel, seen] : in_import)
    if (!seen) fprintf(stderr, "    extra %s\n", rel.c_str());
  CHECK(differ.empty());
  CHECK(missing.empty());

  // The seed as a folder source: whatever it holds must verify against the
  // manifest; what it lacks is reported in missingKnown.
  code = run_adimport(exe, {L"--no-cover-download", L"--from", seed.wstring(), L"--dest", dest_seed.wstring()},
                      "adimport --from <seed>");
  CHECK_EQ(code, 0);
  if (code == 0) {
    phosg::JSON js = load_json(dest_seed);
    CHECK_EQ(js.get_string("verified"), std::string("files"));
    CHECK_EQ(count_known(js, "match"), seeded.size());
    CHECK_EQ(js.at("missingKnown").as_list().size(), extra);
  }
  return test::finish("import.e2e");
}
