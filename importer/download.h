// Resumable HTTP(S) download through WinHTTP.
//
// Built for one job — fetching the releases' disc images (up to ~400 MB)
// and install ZIPs from the Internet Archive — so it favours robustness over
// generality: redirects are followed by hand (archive.org answers
// /download/… with a 302 to a storage node, and following them ourselves
// keeps the Range header on every hop and tells us the final URL; a hop from
// https to plain http is refused), an interrupted transfer resumes from the
// bytes already in "<dest>.part", servers that ignore Range are handled by
// restarting, no transfer grows past the published size (or max_size when
// there is none, a chunked stream included), and the finished file is
// md5-checked before it is renamed into place. A download with no md5 to
// check records its URL in "<dest>.source" and reuses or resumes only that
// URL's bytes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "cancel.h"
#include "status.h"

namespace adw::import {

struct DownloadOptions {
  std::string url;                 // http:// or https://
  std::filesystem::path dest;      // final path; "<dest>.part" while in flight
  std::string expected_md5;        // lowercase hex; empty = accept any content
  // The published size (0 = unknown). A server that announces another size is
  // refused before a byte is written (verify_failed; the .part is kept, since
  // another copy of the same file can resume it), and an existing dest of
  // another size is fetched again without hashing it first.
  uint64_t expected_size = 0;
  // Without a published size, the most a transfer may bring (0 = 2 GiB, more
  // than any known release's file): a server that streams past it is refused
  // (verify_failed) and its .part deleted.
  uint64_t max_size = 0;
  // Cancels at once, even while WinHTTP waits on the network (cancel.h).
  // Optional; the caller keeps it alive.
  const CancelToken* cancel = nullptr;
  // Failed connections in a row, each resuming, before giving up; a
  // connection that got the file further than ever before resets the count
  // (overall cap: 10x this).
  int max_attempts = 5;
  // WinHTTP's name-resolution, connect, send and between-bytes receive
  // timeouts, in ms (0 = the defaults: 30 s, and 60 s to receive). The box
  // covers use 15 s (COVERS.md §2.4), so an offline import fails fast.
  int timeout_ms = 0;
  // Bytes on disk so far and the total (0 = not yet known). Return false to
  // cancel.
  std::function<bool(uint64_t done, uint64_t total)> progress;
  // The md5 passes — over an already-present dest, and over the finished
  // transfer — which take seconds on a 400 MB image and are not transfer
  // progress. Unset = reported through `progress`.
  std::function<bool(uint64_t done, uint64_t total)> hash_progress;
  std::function<void(const std::string&)> log;  // one-line human notes
  std::string user_agent = "LongAfterDark-adimport/1.0";
};

struct DownloadResult {
  std::string md5;        // of the complete file
  uint64_t size = 0;
  std::string final_url;  // after redirects (empty when `reused`)
  bool reused = false;    // dest already existed and matched expected_md5
};

// The connection itself failed (DNS, connect, TLS, a timeout, a dropped
// transfer) and retrying did not help: a network-class failure, after which
// the cover capture skips its other downloads (COVERS.md §2.4). An HTTP error
// status (404, 5xx) is a plain ImportError(network): the server answered.
class ConnectionError : public ImportError {
 public:
  using ImportError::ImportError;
};

// Throws ImportError: network (DNS/TLS/HTTP failure after retries; the
// connection-level ones as ConnectionError; a redirect from https to http),
// verify_failed (md5 mismatch — the bad file is deleted so the next run
// starts clean — or a server announcing or sending more than expected_size /
// max_size), cancelled, or error (local file I/O, or another download to the
// same dest running — "<dest>.lock" is held for the duration).
DownloadResult download(const DownloadOptions& opts);

namespace download_detail {
// Where a redirect goes: its Location header resolved against the URL that
// answered. Throws ImportError(network) for a hop from https to anything
// that is not https (what was asked for over TLS is never fetched in the
// clear). Exposed for the tests.
std::string follow_redirect(const std::string& from, const std::string& location);
}  // namespace download_detail

}  // namespace adw::import
