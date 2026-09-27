// download() against a loopback HTTP server that can redirect, honour,
// ignore or botch Range, cut a transfer short, and 404 — so redirects,
// resume, restart, retry budgeting, the per-destination lock, md5
// verification and the failure statuses are all checked offline.
#include "http_server.h"

#include "download.h"
#include "md5.h"
#include "test_util.h"

using namespace adw::import;
namespace fs = std::filesystem;

namespace {

DownloadOptions opts(const std::string& url, const fs::path& dest, const std::string& md5) {
  DownloadOptions o;
  o.url = url;
  o.dest = dest;
  o.expected_md5 = md5;
  o.max_attempts = 3;
  o.log = [](const std::string& s) { fprintf(stderr, "  log: %s\n", s.c_str()); };
  return o;
}

Status status_of(const DownloadOptions& o) {
  try {
    download(o);
    return Status::ok;
  } catch (const ImportError& e) {
    fprintf(stderr, "  -> %s: %s\n", status_name(e.status()), e.what());
    return e.status();
  }
}

// download() whose failure is a counted CHECK failure rather than an
// exception that ends the run (so one regression does not hide the rest).
DownloadResult download_or_fail(const DownloadOptions& o) {
  try {
    return download(o);
  } catch (const ImportError& e) {
    test::g_failures++;
    fprintf(stderr, "  unexpected %s: %s\n", status_name(e.status()), e.what());
    return {};
  }
}

fs::path part_of(const fs::path& p) {
  fs::path q = p;
  q += L".part";
  return q;
}

}  // namespace

int main(int argc, char** argv) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  fs::path dir = test::scratch(argc, argv, "adw-import-download");
  test::sandbox_data_root(dir / L"localappdata");  // no default may reach the real data folder
  auto blob = test::pattern(3 * 1024 * 1024 + 123, 42);
  const std::string md5 = md5_hex(blob.data(), blob.size());
  test::Server srv(blob);

  // 1. Two redirects (relative, then absolute), a dropped transfer, a resume.
  {
    fprintf(stderr, "redirect + drop + resume\n");
    fs::path dest = dir / L"one.iso";
    srv.drops = 1;
    srv.drop_after = 1000000;
    uint64_t last_done = 0, last_total = 0;
    auto o = opts(srv.url("/redirect1"), dest, md5);
    o.progress = [&](uint64_t d, uint64_t t) {
      last_done = d, last_total = t;
      return true;
    };
    DownloadResult r = download_or_fail(o);
    CHECK_EQ(r.md5, md5);
    CHECK_EQ(r.size, uint64_t(blob.size()));
    CHECK(!r.reused);
    CHECK_EQ(r.final_url, srv.url("/file.bin"));
    CHECK(test::read_bytes(dest) == blob);
    CHECK(!fs::exists(part_of(dest)));
    CHECK_EQ(last_total, uint64_t(blob.size()));
    auto reqs = srv.requests();
    // redirect1, redirect2, file (cut), redirect1, redirect2, file (Range)
    CHECK_EQ(reqs.size(), size_t(6));
    if (reqs.size() == 6) {
      CHECK(reqs[2].path == "/file.bin" && !reqs[2].range);
      CHECK(reqs[5].path == "/file.bin" && reqs[5].range && *reqs[5].range > 0 &&
            *reqs[5].range <= srv.drop_after);
      CHECK(reqs[3].range.has_value());  // Range rides every redirect hop
    }
    srv.clear();

    // 2. Already there with the right md5: no request at all, and the md5
    // pass is reported as hashing, not as transfer progress.
    fprintf(stderr, "reuse\n");
    auto ro = opts(srv.url("/redirect1"), dest, md5);
    uint64_t transfer_calls = 0, hashed = 0;
    ro.progress = [&](uint64_t, uint64_t) {
      transfer_calls++;
      return true;
    };
    ro.hash_progress = [&](uint64_t d, uint64_t t) {
      CHECK_EQ(t, uint64_t(blob.size()));
      hashed = d;
      return true;
    };
    DownloadResult again = download_or_fail(ro);
    CHECK(again.reused);
    CHECK_EQ(again.md5, md5);
    CHECK(srv.requests().empty());
    CHECK_EQ(transfer_calls, uint64_t(0));
    CHECK_EQ(hashed, uint64_t(blob.size()));

    // Cancelling during that check is a cancel, and keeps the good file.
    ro.hash_progress = [](uint64_t, uint64_t) { return false; };
    CHECK_EQ(status_of(ro), Status::cancelled);
    CHECK(test::read_bytes(dest) == blob);
  }

  // 3. A .part from an earlier run resumes on the first request; the final
  // md5 pass over the whole file goes to hash_progress.
  {
    fprintf(stderr, "resume existing .part\n");
    fs::path dest = dir / L"two.iso";
    test::write_bytes(part_of(dest), std::vector<uint8_t>(blob.begin(), blob.begin() + 100000));
    auto o = opts(srv.url("/file.bin"), dest, md5);
    uint64_t first_transfer = UINT64_MAX, hashed = 0;
    o.progress = [&](uint64_t d, uint64_t) {
      if (first_transfer == UINT64_MAX) first_transfer = d;
      return true;
    };
    o.hash_progress = [&](uint64_t d, uint64_t) {
      hashed = d;
      return true;
    };
    DownloadResult r = download_or_fail(o);
    CHECK_EQ(r.md5, md5);
    CHECK_EQ(first_transfer, uint64_t(100000));  // progress starts at the resume point
    CHECK_EQ(hashed, uint64_t(blob.size()));
    auto reqs = srv.requests();
    CHECK(reqs.size() == 1 && reqs[0].range && *reqs[0].range == 100000);
    srv.clear();
  }

  // 4. A complete .part: the server answers 416 and the file is accepted.
  {
    fprintf(stderr, "complete .part (416)\n");
    fs::path dest = dir / L"three.iso";
    test::write_bytes(part_of(dest), blob);
    DownloadResult r = download_or_fail(opts(srv.url("/file.bin"), dest, md5));
    CHECK_EQ(r.md5, md5);
    CHECK(test::read_bytes(dest) == blob);
    srv.clear();
  }

  // 5. A server that ignores Range: the stale .part is discarded, not spliced.
  {
    fprintf(stderr, "server ignores Range\n");
    fs::path dest = dir / L"four.iso";
    test::write_bytes(part_of(dest), test::pattern(100000, 5));  // garbage
    DownloadResult r = download_or_fail(opts(srv.url("/norange.bin"), dest, md5));
    CHECK_EQ(r.md5, md5);
    CHECK(test::read_bytes(dest) == blob);
    srv.clear();
  }

  // 6. An existing file with the wrong content is fetched again.
  {
    fprintf(stderr, "stale dest\n");
    fs::path dest = dir / L"five.iso";
    test::write_bytes(dest, test::pattern(5000, 6));
    DownloadResult r = download_or_fail(opts(srv.url("/file.bin"), dest, md5));
    CHECK(!r.reused);
    CHECK(test::read_bytes(dest) == blob);
    srv.clear();
  }

  // 7. md5 mismatch: verify_failed, and nothing is left to be trusted later.
  {
    fprintf(stderr, "md5 mismatch\n");
    fs::path dest = dir / L"six.iso";
    CHECK_EQ(status_of(opts(srv.url("/file.bin"), dest, std::string(32, '0'))), Status::verify_failed);
    CHECK(!fs::exists(dest));
    CHECK(!fs::exists(part_of(dest)));
    srv.clear();
  }

  // 8. 404 is permanent: one request, network status.
  {
    fprintf(stderr, "404\n");
    CHECK_EQ(status_of(opts(srv.url("/missing.iso"), dir / L"seven.iso", md5)), Status::network);
    CHECK_EQ(srv.requests().size(), size_t(1));
    srv.clear();
  }

  // 9. Nobody listening: retried, then network status.
  {
    fprintf(stderr, "connection refused\n");
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (sockaddr*)&a, sizeof(a));
    int len = sizeof(a);
    getsockname(s, (sockaddr*)&a, &len);
    closesocket(s);
    auto o = opts("http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/x.iso", dir / L"eight.iso", md5);
    o.max_attempts = 2;
    CHECK_EQ(status_of(o), Status::network);
  }

  // 11. More drops than max_attempts, but every connection moves the .part
  // forward: each one earns fresh retries, so the download still finishes.
  {
    fprintf(stderr, "drops > max_attempts, each with progress\n");
    fs::path dest = dir / L"ten.iso";
    srv.drops = 3;
    srv.drop_after = 400000;
    auto o = opts(srv.url("/file.bin"), dest, md5);
    o.max_attempts = 2;
    DownloadResult r = download_or_fail(o);
    CHECK_EQ(r.md5, md5);
    CHECK_EQ(srv.drops.load(), 0);
    auto reqs = srv.requests();
    CHECK_EQ(reqs.size(), size_t(4));
    // Each resume starts further on (not necessarily at exactly i * 400000:
    // the reset can discard bytes still in the client's receive buffer).
    uint64_t prev = 0;
    for (size_t i = 1; i < reqs.size(); i++) {
      CHECK(reqs[i].range && *reqs[i].range > prev && *reqs[i].range <= i * srv.drop_after);
      if (reqs[i].range) prev = *reqs[i].range;
    }
    srv.clear();
  }

  // 12. A 206 for a range other than the one asked for: the .part is
  // discarded and the next attempt starts from byte 0 without Range (it used
  // to resume from the same offset again and get the same wrong answer).
  {
    fprintf(stderr, "wrong 206 range\n");
    fs::path dest = dir / L"eleven.iso";
    test::write_bytes(part_of(dest), test::pattern(100000, 8));
    auto o = opts(srv.url("/badrange.bin"), dest, md5);
    o.max_attempts = 2;
    DownloadResult r = download_or_fail(o);
    CHECK_EQ(r.md5, md5);
    CHECK(test::read_bytes(dest) == blob);
    auto reqs = srv.requests();
    CHECK(reqs.size() == 2 && reqs[0].range && *reqs[0].range == 100000 && !reqs[1].range);
    srv.clear();
  }

  // 13. One download per destination: a second one while the first holds
  // "<dest>.lock" is refused (error) without touching the network.
  {
    fprintf(stderr, "concurrent download\n");
    fs::path dest = dir / L"twelve.iso";
    fs::path lock = dest;
    lock += L".lock";
    HANDLE held = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    CHECK_EQ(status_of(opts(srv.url("/file.bin"), dest, md5)), Status::error);
    CHECK(srv.requests().empty());
    CloseHandle(held);
    DownloadResult r = download_or_fail(opts(srv.url("/file.bin"), dest, md5));
    CHECK_EQ(r.md5, md5);
    CHECK(!fs::exists(lock));  // delete-on-close
    srv.clear();
  }

  // 14. Redirects never leave https (a hop to http is refused, not followed);
  // relative and scheme-relative ones stay on the scheme they came from.
  {
    fprintf(stderr, "redirect policy\n");
    using download_detail::follow_redirect;
    CHECK_EQ(follow_redirect("https://a.example/d/x.iso", "/y.iso"), std::string("https://a.example/y.iso"));
    CHECK_EQ(follow_redirect("https://a.example/d/x.iso", "y.iso"), std::string("https://a.example/d/y.iso"));
    CHECK_EQ(follow_redirect("https://a.example/d/x.iso", "//b.example/y"), std::string("https://b.example/y"));
    CHECK_EQ(follow_redirect("https://a.example/x", "https://b.example/y"), std::string("https://b.example/y"));
    CHECK_EQ(follow_redirect("http://a.example/x", "https://b.example/y"), std::string("https://b.example/y"));
    CHECK_EQ(follow_redirect("http://a.example/x", "http://b.example/y"), std::string("http://b.example/y"));
    for (const char* bad : {"http://b.example/y", "HTTP://b.example/y", "ftp://b.example/y"}) {
      Status st = Status::ok;
      try {
        follow_redirect("https://a.example/x", bad);
      } catch (const ImportError& e) {
        st = e.status();
        fprintf(stderr, "  -> %s\n", e.what());
      }
      CHECK_EQ(st, Status::network);
    }
  }

  // 15. No Content-Length (a body delimited by the connection closing): the
  // transfer is still bounded — by the published size when there is one,
  // else by max_size — and a .part that grew past it is deleted, not kept
  // for a resume.
  {
    fprintf(stderr, "unsized body\n");
    srv.serve_unsized("/stream.bin", blob);
    fs::path dest = dir / L"stream.iso";
    auto o = opts(srv.url("/stream.bin"), dest, "");
    o.max_size = 1000000;
    CHECK_EQ(status_of(o), Status::verify_failed);
    CHECK(!fs::exists(dest) && !fs::exists(part_of(dest)));
    o = opts(srv.url("/stream.bin"), dest, "");
    o.expected_size = 2000000;
    CHECK_EQ(status_of(o), Status::verify_failed);
    CHECK(!fs::exists(dest) && !fs::exists(part_of(dest)));
    // Within the limits it is the whole body.
    o = opts(srv.url("/stream.bin"), dest, md5);
    o.max_size = blob.size();
    DownloadResult r = download_or_fail(o);
    CHECK_EQ(r.md5, md5);
    CHECK_EQ(r.size, uint64_t(blob.size()));
    srv.clear();
  }

  // 16. A download with no md5 to check is reused (or resumed) only for the
  // URL it came from, which "<dest>.source" records.
  {
    fprintf(stderr, "unchecked reuse\n");
    const auto other = test::pattern(200000, 77);
    srv.serve("/a/same.iso", blob);
    srv.serve("/b/same.iso", other);
    fs::path dest = dir / L"same.iso";
    DownloadResult r = download_or_fail(opts(srv.url("/a/same.iso"), dest, ""));
    CHECK_EQ(r.md5, md5);
    fs::path note = dest;
    note += L".source";
    CHECK(test::read_text(note).find(srv.url("/a/same.iso")) == 0);
    srv.clear();
    r = download_or_fail(opts(srv.url("/a/same.iso"), dest, ""));
    CHECK(r.reused);
    CHECK(srv.requests().empty());
    r = download_or_fail(opts(srv.url("/b/same.iso"), dest, ""));  // same name, another address
    CHECK(!r.reused);
    CHECK_EQ(r.md5, md5_hex(other.data(), other.size()));
    CHECK_EQ(srv.requests().size(), size_t(1));
    CHECK(test::read_bytes(dest) == other);
    // A .part of one address is never resumed with another's bytes.
    srv.clear();
    test::write_bytes(part_of(dest), std::vector<uint8_t>(blob.begin(), blob.begin() + 50000));
    DeleteFileW(dest.c_str());
    r = download_or_fail(opts(srv.url("/b/same.iso"), dest, ""));  // .source names /b: resumes
    auto reqs = srv.requests();
    CHECK(reqs.size() == 1 && reqs[0].range && *reqs[0].range == 50000);
    srv.clear();
    test::write_bytes(part_of(dest), std::vector<uint8_t>(blob.begin(), blob.begin() + 50000));
    DeleteFileW(dest.c_str());
    r = download_or_fail(opts(srv.url("/a/same.iso"), dest, ""));  // another address: starts over
    reqs = srv.requests();
    CHECK(reqs.size() == 1 && !reqs[0].range);
    CHECK(test::read_bytes(dest) == blob);
    srv.clear();
  }

  // 17. A cancel token stops a download waiting on a silent server at once,
  // not after WinHTTP's 30-60 s timeouts; the token closes the handle the
  // call is blocked on.
  {
    fprintf(stderr, "cancel token, stalled server\n");
    srv.stall("/stalled.iso");
    CancelToken token;
    auto o = opts(srv.url("/stalled.iso"), dir / L"stalled.iso", md5);
    o.cancel = &token;
    Status st = Status::ok;
    ULONGLONG t0 = GetTickCount64(), t1 = 0;
    std::thread worker([&] {
      st = status_of(o);
      t1 = GetTickCount64();
    });
    Sleep(700);
    token.cancel();
    worker.join();
    CHECK_EQ(st, Status::cancelled);
    fprintf(stderr, "  cancelled after %llu ms\n", (unsigned long long)(t1 - t0));
    CHECK(t1 - t0 < 5000);
    // An already-cancelled token stops the next one before any request.
    srv.clear();
    CHECK_EQ(status_of(o), Status::cancelled);
    CHECK(srv.requests().empty());
  }

  // 10. Cancel: the .part stays so the next run resumes.
  {
    fprintf(stderr, "cancel\n");
    fs::path dest = dir / L"nine.iso";
    auto o = opts(srv.url("/file.bin"), dest, md5);
    o.progress = [](uint64_t d, uint64_t) { return d < 500000; };
    CHECK_EQ(status_of(o), Status::cancelled);
    CHECK(!fs::exists(dest));
    CHECK(fs::exists(part_of(dest)) && fs::file_size(part_of(dest)) >= 500000);
    DownloadResult r = download_or_fail(opts(srv.url("/file.bin"), dest, md5));
    CHECK_EQ(r.md5, md5);
    auto reqs = srv.requests();
    CHECK(!reqs.empty() && reqs.back().range && *reqs.back().range >= 500000);
  }
  return test::finish("import.download");
}
