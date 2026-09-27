// --download of every package from its Internet Archive copies (packages.h
// `downloads`), against a loopback server serving synthetic sources shaped
// like the real ones (tests/pkg_fixture.h; no After Dark bytes):
//   images    Deluxe, 10th Anniversary, 3.2 and Totally Twisted as ISOs whose
//             md5 is the package's known image: verified "image"
//   zip       the Simpsons as a flat ZIP of its install files, read as the
//             install folder: verified "files"; the same ZIP given as an
//             image; ZIPs that nest the files or are password-protected
//   copies    a 404, a wrong size (refused before a byte is written, the
//             .part kept for the next copy to resume) and a wrong md5 each
//             fall back to the next copy; when every copy fails: 3 for a
//             wrong file, 4 for none reachable, with the advice to import
//             from the disc, and nothing left behind; a file already
//             downloaded from any copy is used without a request
//   records   import.json's download record (version 1 and 2)
//   custom    --url with and without --package, --md5 over the registry's
//   all       import_downloads over every package; a cancel stops it
//   registry  the built-in copies: https://archive.org/download/ URLs, the
//             known image md5s and sizes, file names per content, the
//             verified Simpsons ZIPs
//   adimport  --download <id> / all, their conflicts, --list-packages sizes
//
//   test_import_pkg_download <adimport.exe> <scratch>
#include "http_server.h"

#include <phosg/JSON.hh>

#include <map>
#include <set>

#include "importer.h"
#include "md5.h"
#include "pkg_fixture.h"
#include "run_process.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_log;

ImportOptions opts_for(const fs::path& root, const test::TestRegistry& reg) {
  ImportOptions o;
  o.assets_root = root;
  o.registry = reg.span();
  o.log = [](const std::string& s) {
    g_log.push_back(s);
    fprintf(stderr, "  log: %s\n", s.c_str());
  };
  return o;
}

Source download(const fs::path& dl, const std::string& package, const std::string& url = "",
                const std::string& md5 = "") {
  Source s;
  s.kind = Source::Kind::download;
  s.path = dl;
  s.package = package;
  s.url = url;
  s.expected_md5 = md5;
  return s;
}

Source image(const fs::path& p) {
  Source s;
  s.kind = Source::Kind::image;
  s.path = p;
  return s;
}

ImportResult run(const std::string& what, const Source& s, const ImportOptions& o, Status want) {
  ImportResult r = run_import(s, o);
  fprintf(stderr, "[%s] %s: %s\n", what.c_str(), status_name(r.status), r.message.c_str());
  CHECK_EQ(r.status, want);
  return r;
}

phosg::JSON json_at(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

bool logged(const std::string& needle) {
  for (auto& l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

std::string md5_of(const std::vector<uint8_t>& b) { return md5_hex(b.data(), b.size()); }

fs::path with_suffix(fs::path p, const wchar_t* suffix) {
  p += suffix;
  return p;
}

std::vector<std::string> paths(test::Server& srv) {
  std::vector<std::string> v;
  for (auto& r : srv.requests()) v.push_back(r.path);
  return v;
}

// The files installed under <win>/<root> (import.json aside) are exactly the fixture's.
void check_installed(const fs::path& win, const test::PkgFixture& f, const std::string& root) {
  test::Tree have, want;
  fs::path base = test::path_under(win, root);
  for (const std::string& rel : test::list_tree(base))
    if (rel != "import.json") have[root + "/" + rel] = test::read_bytes(test::path_under(base, rel));
  for (auto& [rel, d] : f.expect)
    if (rel.rfind(root + "/", 0) == 0) want[rel] = d;
  for (auto& [rel, d] : want) {
    auto it = have.find(rel);
    if (it == have.end() || it->second != d) {
      test::g_failures++;
      fprintf(stderr, "  %s: %s\n", it == have.end() ? "missing" : "differs", rel.c_str());
    }
  }
  for (auto& [rel, d] : have)
    if (!want.count(rel)) {
      test::g_failures++;
      fprintf(stderr, "  unexpected: %s\n", rel.c_str());
    }
}

std::vector<std::string> catalog_ids(const fs::path& win) {
  std::vector<std::string> ids;
  phosg::JSON cat = json_at(win / L"catalog-win.json");  // alive for the loop (a temporary would not be)
  for (auto& m : cat.at("modules").as_list()) ids.push_back(m->get_string("id"));
  return ids;
}

// A flat ZIP of the install files at the source's root, no password — the
// Internet Archive's Simpsons copies (which lack the owner's notes).
std::vector<uint8_t> flat_zip(const test::Tree& t, uint16_t dos_date = 0x1EF1, const std::string& prefix = "") {
  test::ZipBuilder b;
  b.password = "";
  for (const auto& [rel, d] : t) {
    if (rel == "CEREAL.TXT" || rel == "SERIAL.TXT") continue;
    b.add(prefix + rel, d, /*deflate=*/true, /*encrypt=*/false);
    b.members.back().dos_date = dos_date;
  }
  return b.build();
}

bool is_md5(const std::string& s) {
  return s.size() == 32 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_pkg_download <adimport.exe> [scratch]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  fs::path dir = test::scratch(argc - 1, argv + 1, "adw-import-pkg-download");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  const test::PkgFixture deluxe = test::deluxe_fixture(), ad10 = test::ad10_fixture(), ad32 = test::ad32_fixture(),
                         tt = test::tt_fixture(), simpsons = test::simpsons_fixture();
  test::TestRegistry reg;
  for (auto& [id, f] : std::map<std::string, const test::PkgFixture*>{
           {"deluxe", &deluxe}, {"ad10", &ad10}, {"ad32", &ad32}, {"tt", &tt}, {"simpsons", &simpsons}})
    reg.manifest(id, test::manifest_of(f->expect));

  // What the server publishes. The disc images' md5s are their packages'
  // known images, as the real downloads' are.
  const auto deluxe_iso = test::iso_of(deluxe, true, "AD_DELUXE"), ad10_iso = test::iso_of(ad10, true, "AD10TH"),
             ad32_iso = test::iso_of(ad32, false, "ADW320_C"), tt_iso = test::iso_of(tt, false, "TTW320CD");
  const auto simp_zip = flat_zip(simpsons.source), simp_zip94 = flat_zip(simpsons.source, 0x1C58);
  reg.image("deluxe", md5_of(deluxe_iso), deluxe_iso.size());
  reg.image("ad10", md5_of(ad10_iso), ad10_iso.size());
  reg.image("ad32", md5_of(ad32_iso), ad32_iso.size());
  reg.image("tt", md5_of(tt_iso), tt_iso.size());
  CHECK(md5_of(simp_zip) != md5_of(simp_zip94));

  test::Server srv(test::pattern(1000, 1));
  srv.serve("/deluxe.iso", deluxe_iso);
  srv.serve("/ad10th.iso", ad10_iso);
  srv.serve("/ad32.iso", ad32_iso);
  srv.serve("/tt.iso", tt_iso);
  srv.serve("/SIMPSONS.zip", simp_zip);
  srv.serve("/simpsons-1994.zip", simp_zip94);
  auto tt_short = tt_iso;
  tt_short.resize(tt_short.size() - 2048);  // another size: refused before the transfer
  srv.serve("/tt-short.iso", tt_short);
  auto ad32_bad = ad32_iso;
  ad32_bad[ad32_bad.size() / 2] ^= 0xFF;  // the right size, other bytes: refused by md5
  srv.serve("/ad32-bad.iso", ad32_bad);

  using Copy = test::TestRegistry::Copy;
  const Copy deluxe_copy{srv.url("/r/deluxe.iso"), L"deluxe.iso", deluxe_iso.size(), md5_of(deluxe_iso)};
  const Copy ad10_copy{srv.url("/r/ad10th.iso"), L"ad10th.iso", ad10_iso.size(), md5_of(ad10_iso)};
  const Copy ad32_gone{srv.url("/r/gone.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)};
  const Copy ad32_copy{srv.url("/r/ad32.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)};
  const Copy ad32_bad_copy{srv.url("/r/ad32-bad.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)};
  const Copy tt_copy{srv.url("/r/tt.iso"), L"TTW320CD.ISO", tt_iso.size(), md5_of(tt_iso)};
  const Copy simp_copy{srv.url("/r/SIMPSONS.zip"), L"SIMPSONS.zip", simp_zip.size(), md5_of(simp_zip), "zip"};
  const Copy simp94_copy{srv.url("/r/simpsons-1994.zip"), L"simpsons-1994.zip", simp_zip94.size(), md5_of(simp_zip94),
                         "zip"};
  // Shaped like the real registry: archive.org's hop to a storage node, and
  // a dead first copy for ad32 (its later copies are the fallbacks).
  auto good_registry = [&] {
    reg.downloads("deluxe", {deluxe_copy});
    reg.downloads("ad10", {ad10_copy});
    reg.downloads("ad32", {ad32_gone, ad32_copy});
    reg.downloads("tt", {tt_copy});
    reg.downloads("simpsons", {simp_copy, simp94_copy});
  };
  good_registry();

  // ---- every package on its own, one shared downloads folder ---------------------------------
  const fs::path dl = dir / L"downloads";
  struct One {
    const char* id;
    const test::PkgFixture* f;
    const char* root;
    const wchar_t* file;
    std::string served;  // the path the copy redirects to
    const char* verified;
    const char* format;
  };
  for (const One& o : std::vector<One>{
           {"deluxe", &deluxe, "FILES", L"deluxe.iso", "/deluxe.iso", "image", "iso9660+joliet"},
           {"ad10", &ad10, "packages/ad10", L"ad10th.iso", "/ad10th.iso", "image", "iso9660+joliet"},
           {"ad32", &ad32, "packages/ad32", L"ad32.iso", "/ad32.iso", "image", "iso9660"},
           {"tt", &tt, "packages/tt", L"TTW320CD.ISO", "/tt.iso", "image", "iso9660"},
           {"simpsons", &simpsons, "packages/simpsons", L"SIMPSONS.zip", "/SIMPSONS.zip", "files", "zip"},
       }) {
    fs::path root = dir / (L"alone-" + to_wide(o.id));
    srv.clear();
    g_log.clear();
    ImportResult r = run(std::string("download ") + o.id, download(dl, o.id), opts_for(root, reg), Status::ok);
    CHECK_EQ(r.package_id, std::string(o.id));
    CHECK_EQ(r.verified, std::string(o.verified));
    CHECK_EQ(r.format, std::string(o.format));
    CHECK(r.missing_known.empty());
    check_installed(root / L"win", *o.f, o.root);
    CHECK(catalog_ids(root / L"win") == o.f->ids);
    CHECK(fs::exists(dl / o.file));
    CHECK(!fs::exists(with_suffix(dl / o.file, L".part")));
    CHECK(!fs::exists(with_suffix(dl / o.file, L".lock")));
    CHECK_EQ(r.final_url, srv.url(o.served));
    CHECK(r.download_md5_checked);
    const std::string id = o.id;
    const std::string want_url = id == "deluxe" ? deluxe_copy.url
                                 : id == "ad10" ? ad10_copy.url
                                 : id == "ad32" ? ad32_copy.url
                                 : id == "tt"   ? tt_copy.url
                                                : simp_copy.url;
    CHECK_EQ(r.url, want_url);
    if (id == "deluxe") {
      // Deluxe's record stays version 1: kind download, the URL fetched, where it led.
      phosg::JSON j = json_at(r.import_json);
      CHECK_EQ(j.get_int("version"), int64_t(1));
      CHECK_EQ(j.at("source").get_string("kind"), std::string("download"));
      CHECK_EQ(j.at("source").get_string("url"), want_url);
      CHECK_EQ(j.at("source").get_string("finalUrl"), srv.url(o.served));
      CHECK_EQ(j.at("source").get_bool("isoMd5Known"), true);
    } else {
      phosg::JSON j = json_at(r.import_json);
      const phosg::JSON& s = j.at("source");
      CHECK_EQ(j.get_int("version"), int64_t(2));
      CHECK_EQ(s.get_string("kind"), std::string("download"));
      CHECK_EQ(s.get_string("format"), std::string(o.format));
      CHECK_EQ(s.get_string("url"), want_url);
      CHECK_EQ(s.get_string("finalUrl"), srv.url(o.served));
      CHECK_EQ(s.get_bool("md5Checked"), true);
      CHECK_EQ(s.get_string("imageMd5"), md5_file_hex(dl / o.file));
      CHECK_EQ(s.get_bool("imageMd5Known"), id != "simpsons");
      CHECK_EQ(j.get_string("verified"), std::string(o.verified));
    }
    if (id == "ad32") {
      // The first copy is gone (404): the second one is used.
      auto p = paths(srv);
      CHECK(!p.empty() && p.front() == "/r/gone.iso");
      CHECK(std::find(p.begin(), p.end(), "/ad32.iso") != p.end());
      CHECK(logged("trying another copy: " + ad32_copy.url));
    }
    if (id == "simpsons") {
      CHECK(logged("a ZIP of install files"));
      std::map<std::string, std::string> from;
      phosg::JSON j = json_at(r.import_json);
      for (auto& f : j.at("files").as_list()) from[f->get_string("path")] = f->get_string("from");
      CHECK_EQ(from["packages/simpsons/SIMPSONS/BURNS.AD"], std::string("BURNS.ZIP!BURNS.AD"));
      CHECK_EQ(from["packages/simpsons/ENGINE/ADTASK.DLL"], std::string("ENGINE.ZIP!ADTASK.DLL"));
      // The installer archives' password is still never written or logged.
      for (auto& l : g_log) CHECK(l.find(test::kTestZipPassword) == std::string::npos);
      CHECK(test::read_text(r.import_json).find(test::kTestZipPassword) == std::string::npos);
    }
  }

  // ---- reuse: nothing is requested for a file already downloaded ---------------------------
  {
    srv.clear();
    g_log.clear();
    ImportResult r = run("ad10 again", download(dl, "ad10"), opts_for(dir / L"again-ad10", reg), Status::ok);
    CHECK(srv.requests().empty());
    CHECK(r.final_url.empty());
    CHECK_EQ(r.verified, std::string("image"));
    CHECK(logged("using the already-downloaded"));
    CHECK(!json_at(r.import_json).at("source").contains("finalUrl"));
    CHECK_EQ(json_at(r.import_json).at("source").get_string("url"), ad10_copy.url);

    // ... from whichever copy it came: the second Simpsons ZIP alone on disk.
    fs::path dl2 = dir / L"downloads-second-copy";
    test::write_bytes(dl2 / L"simpsons-1994.zip", simp_zip94);
    srv.clear();
    r = run("simpsons, second copy on disk", download(dl2, "simpsons"), opts_for(dir / L"second-copy", reg), Status::ok);
    CHECK(srv.requests().empty());
    CHECK_EQ(r.url, simp94_copy.url);
    check_installed(dir / L"second-copy" / L"win", simpsons, "packages/simpsons");

    // A file of the wrong size under the first copy's name is fetched again,
    // not hashed (and not preferred over the network).
    fs::path dl3 = dir / L"downloads-stale";
    test::write_bytes(dl3 / L"SIMPSONS.zip", std::vector<uint8_t>(simp_zip.begin(), simp_zip.begin() + 1000));
    srv.clear();
    g_log.clear();
    r = run("simpsons, stale file", download(dl3, "simpsons"), opts_for(dir / L"stale", reg), Status::ok);
    CHECK(logged("is 1000 bytes, expected"));
    CHECK(test::read_bytes(dl3 / L"SIMPSONS.zip") == simp_zip);
  }

  // ---- a copy of another size: refused before a byte is written; the next
  // copy resumes the .part an earlier transfer left ------------------------------------------
  {
    reg.downloads("tt", {{srv.url("/r/tt-short.iso"), L"TTW320CD.ISO", tt_iso.size(), md5_of(tt_iso)}, tt_copy});
    fs::path dl4 = dir / L"downloads-resume";
    test::write_bytes(dl4 / L"TTW320CD.ISO.part", std::vector<uint8_t>(tt_iso.begin(), tt_iso.begin() + 5000));
    srv.clear();
    g_log.clear();
    ImportResult r = run("tt, first copy the wrong size", download(dl4, "tt"), opts_for(dir / L"resume", reg), Status::ok);
    CHECK_EQ(r.url, tt_copy.url);
    CHECK(logged("bytes, not the published " + std::to_string(tt_iso.size())));
    bool resumed = false;
    for (auto& q : srv.requests()) resumed = resumed || (q.path == "/tt.iso" && q.range && *q.range == 5000);
    CHECK(resumed);
    CHECK(test::read_bytes(dl4 / L"TTW320CD.ISO") == tt_iso);
    check_installed(dir / L"resume" / L"win", tt, "packages/tt");
  }

  // ---- a copy with other bytes: refused by md5, deleted, the next copy used ----------------
  {
    reg.downloads("ad32", {ad32_bad_copy, ad32_copy});
    fs::path dl5 = dir / L"downloads-md5";
    g_log.clear();
    ImportResult r = run("ad32, first copy damaged", download(dl5, "ad32"), opts_for(dir / L"md5", reg), Status::ok);
    CHECK_EQ(r.url, ad32_copy.url);
    CHECK(logged("downloaded file has md5 " + md5_of(ad32_bad)));
    CHECK_EQ(md5_file_hex(dl5 / L"ad32.iso"), md5_of(ad32_iso));
  }

  // ---- every copy fails: 3 for a wrong file, 4 for none reachable; nothing is left ---------
  {
    fs::path dl6 = dir / L"downloads-fail", root = dir / L"fail";
    reg.downloads("ad32", {ad32_bad_copy});
    ImportResult r = run("ad32, the only copy damaged", download(dl6, "ad32"), opts_for(root, reg), Status::verify_failed);
    CHECK(r.message.find("--image or --from") != std::string::npos);
    CHECK(!fs::exists(dl6 / L"ad32.iso") && !fs::exists(dl6 / L"ad32.iso.part"));
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32"));

    reg.downloads("ad32", {ad32_gone, {srv.url("/r/gone-too.iso"), L"ad32.iso", ad32_iso.size(), md5_of(ad32_iso)}});
    r = run("ad32, every copy gone", download(dl6, "ad32"), opts_for(root, reg), Status::network);
    CHECK(r.message.find("none of the 2 copies") != std::string::npos);
    CHECK(r.message.find("After Dark 3.2") != std::string::npos);

    reg.downloads("ad32", {ad32_bad_copy, ad32_gone});
    run("ad32, one copy wrong, one gone", download(dl6, "ad32"), opts_for(root, reg), Status::verify_failed);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"ad32"));
    CHECK(!fs::exists(dl6 / L"ad32.iso"));

    // A package with no known copy.
    reg.downloads("ad32", {});
    r = run("ad32, no copy known", download(dl6, "ad32"), opts_for(root, reg), Status::error);
    CHECK(r.message.find("no Internet Archive copy of After Dark 3.2") != std::string::npos);
    good_registry();
  }

  // ---- a custom URL ------------------------------------------------------------------------
  {
    fs::path dl7 = dir / L"downloads-custom";
    ImportResult r = run("--url --package tt", download(dl7, "tt", srv.url("/r/tt.iso"), md5_of(tt_iso)),
                         opts_for(dir / L"custom-tt", reg), Status::ok);
    CHECK_EQ(r.url, srv.url("/r/tt.iso"));
    CHECK(fs::exists(dl7 / L"tt.iso"));  // the URL's own name
    CHECK_EQ(r.verified, std::string("image"));
    r = run("--url, no package", download(dl7, "", srv.url("/SIMPSONS.zip")), opts_for(dir / L"custom-any", reg),
            Status::ok);
    CHECK_EQ(r.package_id, std::string("simpsons"));
    CHECK(!r.download_md5_checked);
    CHECK_EQ(json_at(r.import_json).at("source").get_bool("md5Checked"), false);
    r = run("--url, wrong --md5", download(dl7, "tt", srv.url("/tt-short.iso"), md5_of(tt_iso)),
            opts_for(dir / L"custom-bad", reg), Status::verify_failed);
    // --md5 without --url replaces the registry's (and its size).
    r = run("--md5 over the registry", download(dir / L"downloads-md5-override", "tt", "", md5_of(tt_iso)),
            opts_for(dir / L"custom-md5", reg), Status::ok);
    CHECK_EQ(r.url, tt_copy.url);
  }

  // ---- ZIPs given as images ----------------------------------------------------------------
  {
    ImportResult r = run("the Simpsons ZIP as --image", image(dl / L"SIMPSONS.zip"), opts_for(dir / L"zip-image", reg),
                         Status::ok);
    CHECK_EQ(r.verified, std::string("files"));
    phosg::JSON j = json_at(r.import_json);
    CHECK_EQ(j.at("source").get_string("kind"), std::string("zip"));
    CHECK_EQ(j.at("source").get_string("format"), std::string("zip"));
    CHECK(!j.at("source").contains("url"));
    check_installed(dir / L"zip-image" / L"win", simpsons, "packages/simpsons");

    test::write_bytes(dir / L"nested.zip", flat_zip(simpsons.source, 0x1EF1, "SIMPSONS/"));
    r = run("a ZIP with the files in a folder", image(dir / L"nested.zip"), opts_for(dir / L"zip-nested", reg),
            Status::source_invalid);
    CHECK(r.message.find("at its root") != std::string::npos);
    test::write_bytes(dir / L"locked.zip", test::zip_of({{"INSTALL.INS", test::install_ins("")}}));
    r = run("a password-protected ZIP", image(dir / L"locked.zip"), opts_for(dir / L"zip-locked", reg),
            Status::source_invalid);
    CHECK(r.message.find("password-protected") != std::string::npos);
    CHECK(!fs::exists(dir / L"zip-nested" / L"win" / L"packages"));
  }

  // ---- --download all -----------------------------------------------------------------------
  {
    const std::vector<std::string> ids = downloadable_packages(reg.span());
    CHECK(ids == std::vector<std::string>({"deluxe", "ad10", "ad32", "tt", "simpsons"}));
    fs::path root = dir / L"all";
    std::vector<size_t> started;
    Source base = download(dir / L"downloads-all", "");
    std::vector<ImportResult> rs = import_downloads(ids, base, opts_for(root, reg), [&](size_t i) { started.push_back(i); });
    CHECK_EQ(rs.size(), size_t(5));
    for (auto& r : rs) CHECK_EQ(r.status, Status::ok);
    CHECK(started == std::vector<size_t>({0, 1, 2, 3, 4}));
    std::vector<std::string> want;
    for (const auto* f : {&deluxe, &ad10, &ad32, &tt, &simpsons}) want.insert(want.end(), f->ids.begin(), f->ids.end());
    CHECK(catalog_ids(root / L"win") == want);
    CHECK_EQ(rs.back().installed.size(), size_t(5));

    // Cancelled during the second: the first stays imported, nothing after it starts.
    fs::path croot = dir / L"all-cancel";
    ImportOptions o = opts_for(croot, reg);
    o.progress = [](const Progress& p) { return p.package != "After Dark 10th Anniversary"; };
    rs = import_downloads(ids, download(dir / L"downloads-cancel", ""), o);
    CHECK_EQ(rs.size(), size_t(2));
    if (rs.size() == 2) {
      CHECK_EQ(rs[0].status, Status::ok);
      CHECK_EQ(rs[1].status, Status::cancelled);
    }
    CHECK(fs::exists(croot / L"win" / L"FILES"));
    CHECK(!fs::exists(croot / L"win" / L"packages" / L"ad10"));
  }

  // ---- the built-in copies -------------------------------------------------------------------
  {
    CHECK(downloadable_packages() == std::vector<std::string>({"deluxe", "ad10", "ad32", "tt", "simpsons"}));
    for (const Package& p : builtin_packages()) {
      CHECK(!p.downloads.empty());
      std::map<std::string, std::wstring> name_of_md5;
      std::map<std::wstring, std::string> md5_of_name;
      for (const Download& d : p.downloads) {
        const std::string url = d.url, md5 = d.md5, kind = d.kind;
        const std::wstring file = d.file_name;
        fprintf(stderr, "  %s: %s (%llu bytes, %s)\n", p.id, url.c_str(), (unsigned long long)d.size, kind.c_str());
        CHECK(url.rfind("https://archive.org/download/", 0) == 0);
        CHECK(url.find_first_of(" \"<>[]()") == std::string::npos);  // percent-encoded as published
        CHECK(is_md5(md5));
        CHECK(d.size > 0);
        CHECK(!file.empty() && file.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos);
        CHECK(kind == "image" || kind == "zip");
        // A disc image is the package's known image (verified: image); a ZIP
        // never is.
        bool known = false;
        for (const KnownImage& k : p.images) known = known || (md5 == k.md5 && d.size == k.size);
        CHECK_EQ(known, kind == "image");
        // Same bytes, same name (a transfer resumes across copies); other bytes, another name.
        if (name_of_md5.count(md5)) CHECK(name_of_md5[md5] == file);
        if (md5_of_name.count(file)) CHECK_EQ(md5_of_name[file], md5);
        name_of_md5[md5] = file;
        md5_of_name[file] = md5;
      }
    }
    // The Simpsons ZIPs checked on 2026-09-26 (research/win/pkg/sources).
    const Package* s = find_package("simpsons");
    CHECK(s && s->downloads.size() == 2);
    if (s && s->downloads.size() == 2) {
      CHECK_EQ(std::string(s->downloads[0].md5), std::string("90a85bf64c971fe65045f1afc6db0eba"));
      CHECK_EQ(s->downloads[0].size, uint64_t(2752575));
      CHECK_EQ(std::string(s->downloads[1].md5), std::string("1d6083344b16f09e61acf016eca25026"));
      CHECK_EQ(s->downloads[1].size, uint64_t(2752010));
    }
    CHECK_EQ(find_package("ad32")->downloads.size(), size_t(3));
    CHECK_EQ(std::string(find_package("deluxe")->downloads[0].url), std::string(kDeluxeIsoUrl));
  }

  // ---- adimport.exe ---------------------------------------------------------------------------
  {
    auto cli = [&](const std::vector<std::wstring>& args, const char* what, std::string* out = nullptr) {
      test::ProcessResult r = test::run_process(exe, args, 300000);
      fprintf(stderr, "[%s] exit %d\n%s", what, r.exit_code, r.output.c_str());
      if (out) *out = r.output;
      return r.exit_code;
    };
    std::string out;
    CHECK_EQ(cli({L"--help"}, "--help", &out), 0);
    CHECK(out.find("--download all") != std::string::npos);
    CHECK_EQ(cli({L"--download", L"all", L"--package", L"tt"}, "--download all --package"), 1);
    CHECK_EQ(cli({L"--download", L"all", L"--url", L"http://127.0.0.1:9/x.iso"}, "--download all --url"), 1);
    CHECK_EQ(cli({L"--download", L"nosuch"}, "--download <unknown id>"), 1);
    CHECK_EQ(cli({L"--download", L"ad32", L"--package", L"tt"}, "--download ad32 --package tt"), 1);
    CHECK_EQ(cli({L"--package", L"tt", L"--download", L"ad32"}, "--package tt --download ad32"), 1);
    CHECK_EQ(cli({L"--list-packages", L"--dest", (dir / L"cli-empty").wstring()}, "--list-packages", &out), 0);
    CHECK(out.find("download 2.6 MB (ZIP of the install files)") != std::string::npos);
    CHECK(out.find("download 381.7 MB (disc image)") != std::string::npos);
    CHECK(!fs::exists(dir / L"cli-empty"));

    // The built-in manifests are the real ones: the synthetic files need --no-verify.
    fs::path cdl = dir / L"cli-downloads", croot = dir / L"cli";
    CHECK_EQ(
        cli({L"--no-cover-download", L"--download", L"tt", L"--url", to_wide(srv.url("/r/tt.iso")), L"--md5",
             to_wide(md5_of(tt_iso)), L"--download-dir", cdl.wstring(), L"--dest", croot.wstring(), L"--no-verify"},
            "--download tt --url", &out),
        0);
    CHECK(out.find("Totally Twisted") != std::string::npos);
    CHECK_EQ(json_at(croot / L"win" / L"packages" / L"tt" / L"import.json").at("source").get_string("kind"),
             std::string("download"));
    CHECK_EQ(cli({L"--no-cover-download", L"--download", L"simpsons", L"--url", to_wide(srv.url("/r/SIMPSONS.zip")),
                  L"--md5", to_wide(md5_of(simp_zip)), L"--download-dir", cdl.wstring(), L"--dest", croot.wstring(),
                  L"--no-verify", L"--quiet"},
                 "--download simpsons --url"),
             0);
    CHECK_EQ(json_at(croot / L"win" / L"packages" / L"simpsons" / L"import.json").at("source").get_string("format"),
             std::string("zip"));
    CHECK_EQ(
        cli({L"--no-cover-download", L"--download", L"--package", L"tt", L"--url", to_wide(srv.url("/r/tt-short.iso")),
             L"--md5", to_wide(md5_of(tt_iso)), L"--download-dir", cdl.wstring(), L"--dest", croot.wstring()},
            "--download --package tt, wrong file"),
        3);
    CHECK_EQ(cli({L"--no-cover-download", L"--image", (cdl / L"SIMPSONS.zip").wstring(), L"--dest",
                  (dir / L"cli-zip").wstring(), L"--no-verify", L"--quiet"},
                 "--image <zip>"),
             0);
  }
  return test::finish("import.pkg_download");
}
