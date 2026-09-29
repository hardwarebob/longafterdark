#include "download.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <vector>

#include "cancel.h"
#include "md5.h"
#include "winutil.h"

namespace adw::import {

namespace {

// A failure worth retrying (dropped connection, 5xx, timeout): the loop in
// download() resumes from whatever reached the .part file.
struct Transient {
  std::string what;
  bool connection = true;  // the connection failed (not an HTTP status the server sent)
};

// Every WinHTTP handle one download has open. A cancel (CancelToken) closes
// them all from the cancelling thread, which makes a WinHTTP call blocked on
// one of them fail at once (ERROR_WINHTTP_OPERATION_CANCELLED); each handle
// is closed exactly once, by whichever of the abort and its owner comes first.
class Handles {
 public:
  // False (and `h` closed) once aborted: the download is being cancelled.
  bool add(HINTERNET h) {
    std::lock_guard<std::mutex> lock(m_);
    if (aborted_) {
      WinHttpCloseHandle(h);
      return false;
    }
    live_.push_back(h);
    return true;
  }
  void close(HINTERNET h) {
    std::lock_guard<std::mutex> lock(m_);
    auto it = std::find(live_.begin(), live_.end(), h);
    if (it == live_.end()) return;  // the abort closed it
    live_.erase(it);
    WinHttpCloseHandle(h);
  }
  void abort_all() {
    std::lock_guard<std::mutex> lock(m_);
    aborted_ = true;
    // Children first (requests, then connections, then the session).
    for (auto it = live_.rbegin(); it != live_.rend(); ++it) WinHttpCloseHandle(*it);
    live_.clear();
  }

 private:
  std::mutex m_;
  std::vector<HINTERNET> live_;
  bool aborted_ = false;
};

class Inet {
 public:
  Inet(Handles& hs, HINTERNET h) : hs_(hs), h_(h) {
    if (h_ && !hs_.add(h_)) h_ = nullptr;
  }
  ~Inet() {
    if (h_) hs_.close(h_);
  }
  Inet(const Inet&) = delete;
  Inet& operator=(const Inet&) = delete;
  HINTERNET get() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

 private:
  Handles& hs_;
  HINTERNET h_;
};

[[noreturn]] void throw_cancelled() { throw ImportError(Status::cancelled, "cancelled"); }

std::string winhttp_error(DWORD err) {
  // WinHTTP's messages live in winhttp.dll, not the system table.
  wchar_t* buf = nullptr;
  DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                               FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           GetModuleHandleW(L"winhttp.dll"), err, 0, (wchar_t*)&buf, 0, nullptr);
  std::wstring msg = n ? std::wstring(buf, n) : L"WinHTTP error";
  if (buf) LocalFree(buf);
  while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L'.')) msg.pop_back();
  return to_utf8(msg) + " (" + std::to_string(err) + ")";
}

std::wstring query_header(HINTERNET req, DWORD which) {
  DWORD len = 0;
  WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len) return {};
  std::wstring s(len / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(req, which, WINHTTP_HEADER_NAME_BY_INDEX, s.data(), &len, WINHTTP_NO_HEADER_INDEX))
    return {};
  s.resize(len / sizeof(wchar_t));
  return s;
}

uint64_t parse_u64(const std::wstring& s, size_t& i) {
  uint64_t v = 0;
  while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') v = v * 10 + (s[i++] - L'0');
  return v;
}

// "bytes 100-199/1000" -> first=100, total=1000 ("*" total -> 0).
bool parse_content_range(const std::wstring& s, uint64_t& first, uint64_t& total) {
  size_t i = s.find(L' ');
  if (i == std::wstring::npos) return false;
  i++;
  if (i < s.size() && s[i] == L'*') {
    first = 0;
  } else {
    first = parse_u64(s, i);
  }
  i = s.find(L'/', i);
  if (i == std::wstring::npos) return false;
  i++;
  total = (i < s.size() && s[i] == L'*') ? 0 : parse_u64(s, i);
  return true;
}

// Resolves a Location header against the URL that produced it.
std::wstring resolve_location(const std::wstring& base, const std::wstring& loc) {
  // Absolute: any scheme ("HTTP:", "ftp:" too; RFC 3986 3.1, letters first).
  size_t colon = loc.find(L':');
  if (colon != std::wstring::npos && colon > 0 && iswalpha(loc[0]) &&
      std::all_of(loc.begin(), loc.begin() + colon, [](wchar_t c) { return iswalnum(c) || c == L'+' || c == L'-' || c == L'.'; }))
    return loc;
  size_t scheme_end = base.find(L"://");
  if (scheme_end == std::wstring::npos) return loc;
  if (loc.rfind(L"//", 0) == 0) return base.substr(0, scheme_end + 1) + loc;
  size_t host_end = base.find(L'/', scheme_end + 3);
  std::wstring origin = host_end == std::wstring::npos ? base : base.substr(0, host_end);
  if (!loc.empty() && loc[0] == L'/') return origin + loc;
  std::wstring path = host_end == std::wstring::npos ? L"/" : base.substr(host_end);
  path = path.substr(0, path.find_first_of(L"?#"));
  return origin + path.substr(0, path.rfind(L'/') + 1) + loc;
}

uint64_t file_size_or_zero(const std::filesystem::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
  return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

struct Session {
  Handles& handles;
  const CancelToken* cancel;
  Inet h;
  Session(Handles& hs, const CancelToken* c, const std::string& ua, int timeout_ms)
      : handles(hs), cancel(c), h(hs, open(ua)) {
    if (is_cancelled(cancel)) throw_cancelled();
    if (!h) throw ImportError(Status::network, "WinHttpOpen: " + winhttp_error(GetLastError()));
    if (timeout_ms > 0) WinHttpSetTimeouts(h.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    else WinHttpSetTimeouts(h.get(), 30000, 30000, 30000, 60000);
    // TLS 1.2+ on older Windows 10 builds where WinHTTP's default set is narrower.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(h.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(h.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
  }
  static HINTERNET open(const std::string& ua) {
    std::wstring w = to_wide(ua);
    // Automatic proxy (WPAD / system settings) needs Windows 8.1+; fall back
    // to the WinHTTP default proxy configuration before that.
    HINTERNET s = WinHttpOpen(w.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
      s = WinHttpOpen(w.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    return s;
  }
};

// The most bytes a download without a published size may bring: more than
// any After Dark image (the largest is 400 MB), so only a server streaming
// without end reaches it.
constexpr uint64_t kDefaultMaxSize = 2ull << 30;

uint64_t size_limit(const DownloadOptions& o) {
  if (o.expected_size) return o.expected_size;
  return o.max_size ? o.max_size : kDefaultMaxSize;
}

// A .part that has grown past what the file can be is useless: gone, so no
// later run resumes it.
[[noreturn]] void too_large(const std::filesystem::path& part, const DownloadOptions& o, const std::string& host) {
  DeleteFileW(part.c_str());
  throw ImportError(Status::verify_failed,
                    host + " sends more than " +
                        (o.expected_size ? "the published " + std::to_string(o.expected_size) + " bytes"
                                         : std::to_string(size_limit(o)) + " bytes, more than any known release's file"));
}

// One attempt: GET `url` (following redirects) starting at byte `offset` of
// the .part file and append until the server's stream ends. Updates `total`
// and `final_url`. Throws Transient for retryable failures.
void fetch_once(const Session& session, const DownloadOptions& o, const std::filesystem::path& part,
                uint64_t offset, uint64_t& total, std::string& final_url) {
  std::wstring url = to_wide(o.url);
  const uint64_t limit = size_limit(o);
  // A blocked call that fails because the token closed its handle is a cancel,
  // not a network failure.
  auto failed = [&](std::string what) -> Transient {
    if (is_cancelled(session.cancel)) throw_cancelled();
    return Transient{std::move(what)};
  };
  for (int hop = 0; hop < 10; hop++) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
      throw ImportError(Status::network, "bad URL " + to_utf8(url) + ": " + winhttp_error(GetLastError()));
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    Inet conn(session.handles, WinHttpConnect(session.h.get(), host.c_str(), uc.nPort, 0));
    if (!conn) throw failed("connect to " + to_utf8(host) + ": " + winhttp_error(GetLastError()));
    Inet req(session.handles, WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) throw failed("open request: " + winhttp_error(GetLastError()));
    DWORD no_redirect = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &no_redirect, sizeof(no_redirect));
    std::wstring headers;
    if (offset) headers = L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
    if (!WinHttpSendRequest(req.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr))
      throw failed(to_utf8(host) + ": " + winhttp_error(GetLastError()));

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      std::wstring loc = query_header(req.get(), WINHTTP_QUERY_LOCATION);
      if (loc.empty()) throw ImportError(Status::network, "redirect without a Location header");
      url = to_wide(download_detail::follow_redirect(to_utf8(url), to_utf8(loc)));
      if (o.log) o.log("redirect -> " + to_utf8(url));
      continue;
    }
    final_url = to_utf8(url);

    if (status == 416) {
      // Range not satisfiable: either the .part is already whole, or it is
      // longer than the resource (a changed file) and must start over.
      uint64_t first = 0, t = 0;
      parse_content_range(query_header(req.get(), WINHTTP_QUERY_CONTENT_RANGE), first, t);
      if (t && t == offset) {
        total = t;
        return;
      }
      std::error_code ec;
      std::filesystem::resize_file(part, 0, ec);
      throw Transient{"server rejected the resume offset; restarting", false};
    }
    if (status == 408 || status == 429 || status >= 500)
      throw Transient{"HTTP " + std::to_string(status) + " from " + to_utf8(host), false};
    if (status != 200 && status != 206)
      throw ImportError(Status::network, "HTTP " + std::to_string(status) + " from " + to_utf8(host));

    if (status == 206) {
      uint64_t first = 0, t = 0;
      if (!parse_content_range(query_header(req.get(), WINHTTP_QUERY_CONTENT_RANGE), first, t) ||
          first != offset) {
        // Resuming again from the same offset would get the same answer:
        // really restart, with no Range header on the next attempt.
        std::error_code ec;
        std::filesystem::resize_file(part, 0, ec);
        throw Transient{"server returned an unexpected byte range; restarting", false};
      }
      if (t) total = t;
    } else {
      if (offset && o.log) o.log("server ignored the Range request; restarting from byte 0");
      offset = 0;
      std::wstring cl = query_header(req.get(), WINHTTP_QUERY_CONTENT_LENGTH);
      size_t i = 0;
      uint64_t n = parse_u64(cl, i);
      if (n) total = n;
    }
    // Checked before the .part is touched: it may still be good for another
    // copy of the published file.
    if (o.expected_size && total && total != o.expected_size)
      throw ImportError(Status::verify_failed, to_utf8(host) + " serves " + std::to_string(total) +
                                                   " bytes, not the published " + std::to_string(o.expected_size));
    if (total > limit)
      throw ImportError(Status::verify_failed, to_utf8(host) + " serves " + std::to_string(total) +
                                                   " bytes, more than any known release's file");

    Handle out(CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out.valid())
      throw ImportError(Status::error, "cannot write " + to_utf8(part.wstring()) + ": " +
                                           win_error_string(GetLastError()));
    LARGE_INTEGER pos{};
    pos.QuadPart = LONGLONG(offset);
    // Appending anywhere but at the resume point would splice the file.
    if (!SetFilePointerEx(out.get(), pos, nullptr, FILE_BEGIN) || !SetEndOfFile(out.get()))
      throw ImportError(Status::error, "cannot resume " + to_utf8(part.wstring()) + " at byte " +
                                           std::to_string(offset) + ": " + win_error_string(GetLastError()));

    std::vector<uint8_t> buf(256 * 1024);
    uint64_t have = offset;
    if (o.progress && !o.progress(have, total)) throw_cancelled();
    for (;;) {
      DWORD got = 0;
      if (!WinHttpReadData(req.get(), buf.data(), DWORD(buf.size()), &got))
        throw failed("transfer interrupted at byte " + std::to_string(have) + ": " + winhttp_error(GetLastError()));
      if (!got) break;
      // Never more than the file can be, announced or not (a chunked stream
      // has no length to check up front).
      if (have + got > limit || (total && have + got > total)) {
        out.reset();
        too_large(part, o, to_utf8(host));
      }
      DWORD wrote = 0;
      if (!WriteFile(out.get(), buf.data(), got, &wrote, nullptr) || wrote != got)
        throw ImportError(Status::error, "write failed on " + to_utf8(part.wstring()) + ": " +
                                             win_error_string(GetLastError()));
      have += got;
      if (o.progress && !o.progress(have, total)) throw_cancelled();
    }
    if (is_cancelled(session.cancel)) throw_cancelled();
    if (total && have < total)
      throw Transient{"connection closed at byte " + std::to_string(have) + " of " + std::to_string(total)};
    if (!total) total = have;
    return;
  }
  throw ImportError(Status::network, "too many redirects");
}

// A download checked against no md5 records where it came from beside it,
// "<dest>.source", written before the first byte: its bytes (and those of its
// .part) are reused or resumed only for that same URL. A file from another
// address (or of unknown origin) is fetched again.
std::filesystem::path source_note(const std::filesystem::path& dest) {
  std::filesystem::path p = dest;
  p += L".source";
  return p;
}

std::string read_source_note(const std::filesystem::path& dest) {
  Handle h(CreateFileW(source_note(dest).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
  if (!h.valid()) return {};
  char buf[8192];
  DWORD got = 0;
  if (!ReadFile(h.get(), buf, sizeof(buf), &got, nullptr)) return {};
  std::string s(buf, got);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

void write_source_note(const std::filesystem::path& dest, const std::string& url) {
  const std::filesystem::path p = source_note(dest);
  SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);  // CREATE_ALWAYS refuses a hidden file otherwise
  Handle h(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
  DWORD wrote = 0;
  const std::string line = url + "\r\n";
  if (!h.valid() || !WriteFile(h.get(), line.data(), DWORD(line.size()), &wrote, nullptr) || wrote != line.size())
    throw ImportError(Status::error, "cannot write " + to_utf8(p.wstring()) + ": " + win_error_string(GetLastError()));
}

}  // namespace

std::string download_detail::follow_redirect(const std::string& from, const std::string& location) {
  const std::wstring base = to_wide(from), next = resolve_location(base, to_wide(location));
  auto is_https = [](const std::wstring& u) {
    if (u.size() < 8) return false;
    std::wstring scheme = u.substr(0, 8);
    for (wchar_t& c : scheme) c = wchar_t(towlower(c));
    return scheme == L"https://";
  };
  if (is_https(base) && !is_https(next))
    throw ImportError(Status::network, "refusing a redirect from " + from + " to " + to_utf8(next) +
                                           ", which leaves https");
  return to_utf8(next);
}

DownloadResult download(const DownloadOptions& o) {
  DownloadResult r;
  std::error_code ec;
  std::filesystem::create_directories(o.dest.parent_path(), ec);
  if (is_cancelled(o.cancel)) throw_cancelled();

  // One download per destination at a time. Two runs (the .scr's button
  // twice, or imports into two assets roots) share one downloads folder, and
  // their appends would interleave in the .part during each other's retry
  // pauses. Held across the reuse check too, which may delete a stale dest.
  // Unshared + delete-on-close, so a crashed run never leaves it held.
  std::filesystem::path lock_path = o.dest;
  lock_path += L".lock";
  Handle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
  if (!lock.valid()) {
    DWORD e = GetLastError();
    throw ImportError(Status::error, e == ERROR_SHARING_VIOLATION
                                         ? "another download of " + to_utf8(o.dest.filename().wstring()) +
                                               " is already running"
                                         : "cannot lock " + to_utf8(lock_path.wstring()) + ": " + win_error_string(e));
  }

  // The md5 passes honour the token too (a 400 MB image takes seconds).
  auto md5_of = [&](const std::filesystem::path& p) {
    const auto& report = o.hash_progress ? o.hash_progress : o.progress;
    return md5_file_hex(p, [&](uint64_t done, uint64_t total) {
      if (is_cancelled(o.cancel)) return false;
      return !report || report(done, total);
    });
  };

  std::filesystem::path part = o.dest;
  part += L".part";
  // Unchecked: only this URL's own bytes count (source_note).
  const bool unchecked = o.expected_md5.empty();
  if (unchecked && read_source_note(o.dest) != o.url) {
    const bool had = std::filesystem::exists(o.dest, ec) || std::filesystem::exists(part, ec);
    if (had && o.log)
      o.log("the existing " + to_utf8(o.dest.filename().wstring()) +
            " did not come from this address; downloading again");
    DeleteFileW(o.dest.c_str());
    DeleteFileW(part.c_str());
  }

  if (std::filesystem::exists(o.dest, ec) && o.expected_size && file_size_or_zero(o.dest) != o.expected_size) {
    if (o.log)
      o.log("existing " + to_utf8(o.dest.filename().wstring()) + " is " + std::to_string(file_size_or_zero(o.dest)) +
            " bytes, expected " + std::to_string(o.expected_size) + "; downloading again");
    DeleteFileW(o.dest.c_str());
  }
  if (std::filesystem::exists(o.dest, ec)) {
    if (unchecked) {
      r.reused = true;
      r.size = file_size_or_zero(o.dest);
      r.md5 = md5_of(o.dest);
      return r;
    }
    if (o.log) o.log("checking the existing " + to_utf8(o.dest.filename().wstring()));
    std::string have = md5_of(o.dest);
    if (have == o.expected_md5) {
      r.reused = true;
      r.size = file_size_or_zero(o.dest);
      r.md5 = have;
      return r;
    }
    if (o.log) o.log("existing file has md5 " + have + ", expected " + o.expected_md5 + "; downloading again");
    DeleteFileW(o.dest.c_str());
  }
  if (unchecked) write_source_note(o.dest, o.url);

  Handles handles;
  // From here on a cancel closes whatever WinHTTP is waiting on.
  CancelToken::Scope abort_scope(o.cancel, [&handles] { handles.abort_all(); });
  Session session(handles, o.cancel, o.user_agent, o.timeout_ms);
  uint64_t total = 0;
  // A .part longer than the file can be cannot be a prefix of it.
  if (file_size_or_zero(part) > size_limit(o)) std::filesystem::resize_file(part, 0, ec);
  // max_attempts bounds the failures in a row that bring the .part no further
  // than it has ever been. A connection that moved it forward earns a fresh
  // set, so a flaky link still finishes 400 MB one resume at a time; the
  // overall cap keeps a server that drops every connection after a few bytes
  // from retrying for ever.
  uint64_t furthest = file_size_or_zero(part);
  const int max_total = std::max(o.max_attempts, 1) * 10;
  int failures = 0;
  for (int attempt = 1;; attempt++) {
    if (is_cancelled(o.cancel)) throw_cancelled();
    uint64_t have = file_size_or_zero(part);
    if (total && have > total) {
      std::filesystem::resize_file(part, 0, ec);
      have = 0;
    }
    if (total && have == total) break;
    if (have && o.log) o.log("resuming at byte " + std::to_string(have));
    try {
      fetch_once(session, o, part, have, total, r.final_url);
      break;
    } catch (const Transient& t) {
      if (is_cancelled(o.cancel)) throw_cancelled();
      uint64_t now = file_size_or_zero(part);
      if (now > furthest) {
        furthest = now;
        failures = 0;
      }
      if (++failures >= o.max_attempts || attempt >= max_total) {
        std::string what = t.what + " (gave up after " + std::to_string(attempt) + " attempts)";
        if (t.connection) throw ConnectionError(Status::network, what);
        throw ImportError(Status::network, what);
      }
      // 1, 2, 4, 8, 10, 10 … s (the shift is capped: a large max_attempts
      // must not shift past the width of int).
      DWORD wait_ms = DWORD(std::min(1000 << std::min(failures - 1, 4), 10000));
      if (o.log) o.log(t.what + "; retrying in " + std::to_string(wait_ms / 1000) + " s");
      // Sleep in slices so a cancel request is noticed promptly.
      for (DWORD slept = 0; slept < wait_ms; slept += 250) {
        if (is_cancelled(o.cancel) || (o.progress && !o.progress(file_size_or_zero(part), total))) throw_cancelled();
        Sleep(250);
      }
    }
  }

  r.size = file_size_or_zero(part);
  r.md5 = md5_of(part);
  if (!o.expected_md5.empty() && r.md5 != o.expected_md5) {
    DeleteFileW(part.c_str());
    throw ImportError(Status::verify_failed,
                      "downloaded file has md5 " + r.md5 + ", expected " + o.expected_md5);
  }
  if (!MoveFileExW(part.c_str(), o.dest.c_str(), MOVEFILE_REPLACE_EXISTING))
    throw ImportError(Status::error, "cannot rename " + to_utf8(part.wstring()) + ": " +
                                         win_error_string(GetLastError()));
  return r;
}

}  // namespace adw::import
