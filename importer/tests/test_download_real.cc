// Every package from its Internet Archive copies, for real (opt-in: AD_E2E=1;
// exit 77 = skipped). packages.h `downloads`, PACKAGES.md §5.2.
//
//   test_import_download_real <adimport.exe> <scratch> [<image dir>]
//
// Everything is written under <scratch> (the build tree); anything outside it
// is only read.
//   0. Seeding (skipped with AD_E2E_NO_SEED=1): a local file with a copy's
//      published size and md5 is hard-linked (or copied) into
//      <scratch>/downloads under the copy's file name, so what can be verified
//      locally is not fetched again (the importer still checks its md5). The
//      files are looked for in AD_E2E_LOCAL_DIRS (';'-separated), else the
//      image dir (<repo>/source_iso), the installed data folder's downloads
//      (%LOCALAPPDATA%\LongAfterDark\downloads, read only) and its
//      verify\ folder.
//   1. adimport --download <id> --download-dir <scratch>/downloads --dest
//      <scratch>/<id>, for every package: exit 0; the import record says kind
//      "download", a registry URL, verified "image" (the disc images) or
//      "files" (the Simpsons ZIP), nothing missing, every file a manifest
//      match; the catalog lists the package's 84 / 46 / 44 / 13 / 15 modules.
//   2. The Simpsons' other copy (a 2.7 MB ZIP) through --url/--md5, imported
//      the same way, so the layout of every Simpsons copy is tested.
//   3. Every registry URL (fallbacks included) answers a small Range request
//      with the published size, and with the same bytes as the verified file:
//      the last 64 KiB (the ZIP directory; the end of a disc) and, for an ISO,
//      the primary volume descriptor.
// <scratch> is deleted at the end unless AD_E2E_KEEP=1.
#include <windows.h>
#include <winhttp.h>

#include <phosg/JSON.hh>

#include <map>
#include <set>

#include "importer.h"
#include "md5.h"
#include "run_process.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

phosg::JSON load(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

struct Expect {
  size_t modules;
  const char* verified;
};
const std::map<std::string, Expect> kExpect = {
    {"deluxe", {84, "image"}}, {"ad10", {46, "image"}}, {"ad32", {44, "image"}},
    {"tt", {13, "image"}},     {"simpsons", {15, "files"}},
};

std::vector<fs::path> local_dirs(const fs::path& image_dir) {
  std::vector<fs::path> dirs;
  if (const wchar_t* e = _wgetenv(L"AD_E2E_LOCAL_DIRS")) {
    std::wstring v = e;
    size_t i = 0;
    while (i <= v.size()) {
      size_t j = v.find(L';', i);
      if (j == std::wstring::npos) j = v.size();
      if (j > i) dirs.push_back(v.substr(i, j - i));
      i = j + 1;
    }
    return dirs;
  }
  if (!image_dir.empty()) dirs.push_back(image_dir);
  // The installed data folder's downloads, read only: call this before the
  // sandbox.
  if (const fs::path data = test::installed_data_root(); !data.empty()) {
    fs::path d = data / L"downloads";
    dirs.push_back(d);
    dirs.push_back(d / L"verify");
  }
  return dirs;
}

// A local file with this size and md5, or nothing. Only files of the right
// size are hashed.
std::optional<fs::path> find_local(const std::vector<fs::path>& dirs, uint64_t size, const std::string& md5) {
  std::error_code ec;
  for (const fs::path& d : dirs) {
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec) || it->file_size(ec) != size) continue;
      if (md5_file_hex(it->path()) == md5) return it->path();
    }
    ec.clear();
  }
  return std::nullopt;
}

// A hard link when the file is on the same volume and not read-only (so
// deleting the link can never touch the original), else a copy.
bool seed(const fs::path& from, const fs::path& to) {
  fs::create_directories(to.parent_path());
  DWORD attr = GetFileAttributesW(from.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_READONLY) &&
      CreateHardLinkW(to.c_str(), from.c_str(), nullptr))
    return true;
  return CopyFileW(from.c_str(), to.c_str(), TRUE) != 0;
}

std::vector<uint8_t> read_range(const fs::path& p, uint64_t from, size_t n) {
  std::vector<uint8_t> out(n);
  Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
  if (!h.valid()) return {};
  LARGE_INTEGER pos{};
  pos.QuadPart = LONGLONG(from);
  SetFilePointerEx(h.get(), pos, nullptr, FILE_BEGIN);
  DWORD got = 0;
  if (!ReadFile(h.get(), out.data(), DWORD(n), &got, nullptr)) return {};
  out.resize(got);
  return out;
}

struct Probe {
  DWORD status = 0;
  uint64_t total = 0;  // from Content-Range
  std::vector<uint8_t> body;
  std::string error;
};

// GET `url` with "Range: bytes=<from>-<from + n - 1>", redirects followed.
Probe probe(const std::string& url, uint64_t from, size_t n) {
  Probe r;
  auto fail = [&](const char* what) {
    r.error = std::string(what) + ": " + win_error_string(GetLastError());
    return r;
  };
  HINTERNET s = WinHttpOpen(L"LongAfterDark-adimport-test/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return fail("WinHttpOpen");
  std::wstring w = to_wide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = DWORD(-1);
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) {
    WinHttpCloseHandle(s);
    return fail("WinHttpCrackUrl");
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength), path(uc.lpszUrlPath, uc.dwUrlPathLength);
  HINTERNET c = WinHttpConnect(s, host.c_str(), uc.nPort, 0);
  HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  std::wstring range = L"Range: bytes=" + std::to_wstring(from) + L"-" + std::to_wstring(from + n - 1) + L"\r\n";
  bool ok = q && WinHttpSendRequest(q, range.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(q, nullptr);
  if (!ok) {
    fail("request");
  } else {
    DWORD len = sizeof(r.status);
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &r.status, &len, WINHTTP_NO_HEADER_INDEX);
    wchar_t cr[128] = {};
    len = sizeof(cr);
    if (WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, cr, &len,
                            WINHTTP_NO_HEADER_INDEX))
      if (const wchar_t* slash = wcschr(cr, L'/')) r.total = _wcstoui64(slash + 1, nullptr, 10);
    std::vector<uint8_t> buf(65536);
    for (;;) {
      DWORD got = 0;
      if (!WinHttpReadData(q, buf.data(), DWORD(buf.size()), &got) || !got) break;
      r.body.insert(r.body.end(), buf.begin(), buf.begin() + got);
      if (r.body.size() > n + 65536) break;  // a server ignoring Range: enough
    }
  }
  if (q) WinHttpCloseHandle(q);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return r;
}

struct Row {
  std::string id, how;
  double seconds = 0;
  std::string verified;
  size_t modules = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const char* on = getenv("AD_E2E");
  if (!on || std::string(on) != "1") {
    fprintf(stderr, "import.download_real: skipped (set AD_E2E=1 to fetch every package from the Internet Archive)\n");
    return 77;
  }
  if (argc < 3) {
    fprintf(stderr, "usage: test_import_download_real <adimport.exe> <scratch> [<image dir>]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  // Where verified local copies may be (read only), found before the sandbox.
  const std::vector<fs::path> dirs = local_dirs(argc > 3 ? fs::path(argv[3]) : fs::path());
  fs::path scratch = test::scratch(argc - 1, argv + 1, "adw-import-download-real");
  test::sandbox_data_root(scratch / L"localappdata");  // every run has --dest and --download-dir anyway
  fs::path dl = scratch / L"downloads";
  const bool seeding = !(getenv("AD_E2E_NO_SEED") && std::string(getenv("AD_E2E_NO_SEED")) == "1");
  std::vector<Row> rows;

  // ---- 0 + 1: each package --------------------------------------------------------------------
  for (const Package& p : builtin_packages()) {
    Row row;
    row.id = p.id;
    fprintf(stderr, "==== %s (%s)\n", p.id, p.title);
    std::set<std::wstring> seeded;
    if (seeding) {
      for (const Download& d : p.downloads) {
        fs::path to = dl / d.file_name;
        if (seeded.count(d.file_name) || fs::exists(to)) continue;
        if (auto from = find_local(dirs, d.size, d.md5)) {
          bool ok = seed(*from, to);
          fprintf(stderr, "  seeded %s from %s (%s)\n", to_utf8(to.filename().wstring()).c_str(),
                  to_utf8(from->wstring()).c_str(), ok ? "verified local copy" : "FAILED");
          if (ok) seeded.insert(d.file_name);
        }
      }
    }
    row.how = seeded.empty() ? "downloaded" : "local copy";
    fs::path root = scratch / to_wide(p.id);
    ULONGLONG t0 = GetTickCount64();
    test::ProcessResult pr = test::run_process(exe,
                                               {L"--no-cover-download", L"--download", to_wide(p.id), L"--download-dir",
                                                dl.wstring(), L"--dest", root.wstring()},
                                               3600000);
    row.seconds = (GetTickCount64() - t0) / 1000.0;
    fprintf(stderr, "%s", pr.output.c_str());
    fprintf(stderr, "  adimport exited %d after %.1f s\n", pr.exit_code, row.seconds);
    CHECK_EQ(pr.exit_code, 0);
    if (pr.exit_code != 0) {
      rows.push_back(row);
      continue;
    }
    std::set<std::string> urls;
    for (const Download& d : p.downloads) urls.insert(d.url);
    const bool deluxe = p.is_deluxe();
    fs::path record = deluxe ? root / L"win" / L"import.json" : root / L"win" / L"packages" / to_wide(p.id) / L"import.json";
    phosg::JSON j = load(record);
    const phosg::JSON& src = j.at("source");
    CHECK_EQ(src.get_string("kind"), std::string("download"));
    CHECK(urls.count(src.get_string("url")));
    if (!deluxe) CHECK_EQ(src.get_bool("md5Checked"), true);
    row.verified = j.get_string("verified");
    CHECK_EQ(row.verified, std::string(kExpect.at(p.id).verified));
    CHECK(j.at("missingKnown").as_list().empty());
    CHECK_EQ(size_t(j.get_int("fileCount")), p.manifest.size());
    for (auto& f : j.at("files").as_list()) CHECK_EQ(f->get_string("known"), std::string("match"));
    phosg::JSON cat = load(root / L"win" / L"catalog-win.json");  // alive for the loop
    for (auto& m : cat.at("modules").as_list()) row.modules += m->get_string("package") == p.id;
    CHECK_EQ(row.modules, kExpect.at(p.id).modules);
    rows.push_back(row);
  }

  // ---- 2: the Simpsons' other copies ----------------------------------------------------------
  const Package* simpsons = find_package("simpsons");
  for (size_t i = 1; i < simpsons->downloads.size(); i++) {
    const Download& d = simpsons->downloads[i];
    fprintf(stderr, "==== simpsons, copy %zu: %s\n", i + 1, d.url);
    fs::path root = scratch / (L"simpsons-copy" + std::to_wstring(i + 1));
    ULONGLONG t0 = GetTickCount64();
    test::ProcessResult pr =
        test::run_process(exe,
                          {L"--no-cover-download", L"--download", L"simpsons", L"--url", to_wide(d.url), L"--md5",
                           to_wide(d.md5), L"--download-dir", dl.wstring(), L"--dest", root.wstring()},
                          600000);
    Row row;
    row.id = "simpsons #" + std::to_string(i + 1);
    row.how = "downloaded (--url)";
    row.seconds = (GetTickCount64() - t0) / 1000.0;
    fprintf(stderr, "%s  adimport exited %d\n", pr.output.c_str(), pr.exit_code);
    CHECK_EQ(pr.exit_code, 0);
    if (pr.exit_code == 0) {
      phosg::JSON j = load(root / L"win" / L"packages" / L"simpsons" / L"import.json");
      row.verified = j.get_string("verified");
      CHECK_EQ(row.verified, std::string("files"));
      CHECK(j.at("missingKnown").as_list().empty());
      CHECK_EQ(size_t(j.get_int("fileCount")), simpsons->manifest.size());
      CHECK_EQ(j.at("source").get_string("imageMd5"), std::string(d.md5));
      phosg::JSON cat = load(root / L"win" / L"catalog-win.json");
      for (auto& m : cat.at("modules").as_list()) row.modules += m->get_string("package") == "simpsons";
      CHECK_EQ(row.modules, size_t(15));
    }
    rows.push_back(row);
  }

  // ---- 3: every registry URL serves the verified bytes -----------------------------------------
  fprintf(stderr, "==== probing every copy\n");
  size_t probed = 0;
  for (const Package& p : builtin_packages()) {
    for (const Download& d : p.downloads) {
      fs::path local = dl / d.file_name;
      if (!fs::exists(local) || fs::file_size(local) != d.size) {
        fprintf(stderr, "  %s: no verified local file to compare with\n", d.url);
        test::g_failures++;
        continue;
      }
      std::vector<std::pair<uint64_t, size_t>> ranges = {{d.size - 65536, 65536}};
      if (std::string_view(d.kind) == "image") ranges.push_back({16 * 2048, 2048});
      for (auto [from, n] : ranges) {
        Probe r = probe(d.url, from, n);
        bool same = r.body == read_range(local, from, n);
        fprintf(stderr, "  %-8s %s bytes %llu+%zu: HTTP %lu, total %llu%s%s\n", p.id, d.url, (unsigned long long)from, n,
                (unsigned long)r.status, (unsigned long long)r.total, same ? ", same bytes" : ", DIFFERENT BYTES",
                r.error.empty() ? "" : (" (" + r.error + ")").c_str());
        CHECK_EQ(r.status, DWORD(206));
        CHECK_EQ(r.total, d.size);
        CHECK(same);
        probed++;
      }
    }
  }

  fprintf(stderr, "\n%-12s %-20s %9s %-9s %s\n", "package", "source", "seconds", "verified", "modules");
  for (const Row& r : rows)
    fprintf(stderr, "%-12s %-20s %9.1f %-9s %zu\n", r.id.c_str(), r.how.c_str(), r.seconds, r.verified.c_str(),
            r.modules);
  fprintf(stderr, "%zu range probes\n", probed);

  if (!(getenv("AD_E2E_KEEP") && std::string(getenv("AD_E2E_KEEP")) == "1")) {
    // The seeded hard links go first, with DeleteFileW alone: nothing may
    // clear an attribute of the file they share with the original.
    std::error_code ec;
    for (auto it = fs::directory_iterator(dl, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
      DeleteFileW(it->path().c_str());
    remove_tree(scratch);
  }
  return test::finish("import.download_real");
}
