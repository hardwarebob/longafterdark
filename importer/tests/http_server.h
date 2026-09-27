// Loopback HTTP/1.1 server for the download and CLI tests: serves one blob
// at /file.bin (Range-aware, can cut the first N bodies short),
// /norange.bin (ignores Range) and /badrange.bin (answers Range with the
// wrong range), two chained redirects to /file.bin, any further files given
// to serve() (Range-aware), /r/<path> as a 302 to /<path> (archive.org's
// hop to a storage node), paths given to stall() answered with nothing at
// all (the connection is held open until the server goes away: a timeout),
// paths given to serve_unsized() sent with no Content-Length (the body ends
// when the connection closes, as a streaming server's would), and 404 for
// anything else. One connection at a time, "Connection: close".
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace test {

class Server {
 public:
  struct Request {
    std::string path;
    std::optional<uint64_t> range;
  };

  explicit Server(std::vector<uint8_t> blob) : blob_(std::move(blob)) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener_, (sockaddr*)&a, sizeof(a));
    listen(listener_, 16);
    int len = sizeof(a);
    getsockname(listener_, (sockaddr*)&a, &len);
    port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { loop(); });
  }
  ~Server() {
    closesocket(listener_);
    thread_.join();
    for (SOCKET c : held_) closesocket(c);
  }

  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
  std::vector<Request> requests() {
    std::lock_guard<std::mutex> l(m_);
    return log_;
  }
  void clear() {
    std::lock_guard<std::mutex> l(m_);
    log_.clear();
  }
  std::atomic<int> drops{0};  // cut the next N /file.bin bodies short
  size_t drop_after = 0;

  // Serves `blob` at `path` too (Range-aware, never cut short).
  void serve(const std::string& path, std::vector<uint8_t> blob) {
    std::lock_guard<std::mutex> l(m_);
    files_[path] = std::make_shared<const std::vector<uint8_t>>(std::move(blob));
  }
  // Serves `blob` at `path` with no Content-Length and no Range support: the
  // body is delimited by the connection closing.
  void serve_unsized(const std::string& path, std::vector<uint8_t> blob) {
    std::lock_guard<std::mutex> l(m_);
    unsized_[path] = std::make_shared<const std::vector<uint8_t>>(std::move(blob));
  }
  // `path` is read and never answered (a stalled server).
  void stall(const std::string& path) {
    std::lock_guard<std::mutex> l(m_);
    stalled_.push_back(path);
  }
  // Serves nothing more at `path` (404 from now on).
  void forget(const std::string& path) {
    std::lock_guard<std::mutex> l(m_);
    files_.erase(path);
  }

 private:
  void loop() {
    for (;;) {
      SOCKET c = accept(listener_, nullptr, nullptr);
      if (c == INVALID_SOCKET) return;
      if (handle(c)) {
        held_.push_back(c);  // stalled: closed with the server
        continue;
      }
      closesocket(c);
    }
  }

  static void send_all(SOCKET c, const void* p, size_t n) {
    const char* s = (const char*)p;
    while (n) {
      int k = send(c, s, int(std::min<size_t>(n, 65536)), 0);
      if (k <= 0) return;
      s += k;
      n -= size_t(k);
    }
  }

  // True when the connection is to be held open, unanswered.
  bool handle(SOCKET c) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
      int k = recv(c, buf, sizeof(buf), 0);
      if (k <= 0) return false;
      req.append(buf, size_t(k));
    }
    Request r;
    size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    r.path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string lower = req;
    for (char& ch : lower) ch = char(tolower(ch));
    size_t rg = lower.find("\r\nrange: bytes=");
    if (rg != std::string::npos) r.range = std::stoull(req.substr(rg + 15));
    {
      std::lock_guard<std::mutex> l(m_);
      log_.push_back(r);
      if (std::find(stalled_.begin(), stalled_.end(), r.path) != stalled_.end()) return true;
    }
    std::string head;
    std::shared_ptr<const std::vector<uint8_t>> file, unsized;
    {
      std::lock_guard<std::mutex> l(m_);
      auto it = files_.find(r.path);
      if (it != files_.end()) file = it->second;
      auto u = unsized_.find(r.path);
      if (u != unsized_.end()) unsized = u->second;
    }
    if (unsized) {
      head = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
      send_all(c, head.data(), head.size());
      send_all(c, unsized->data(), unsized->size());
      shutdown(c, SD_SEND);
      return false;
    }
    if (file) {
      const std::vector<uint8_t>& b = *file;
      uint64_t start = r.range ? *r.range : 0;
      if (r.range && start >= b.size()) {
        head = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(b.size()) +
               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(c, head.data(), head.size());
        return false;
      }
      head = r.range ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) + "-" +
                           std::to_string(b.size() - 1) + "/" + std::to_string(b.size()) + "\r\n"
                     : "HTTP/1.1 200 OK\r\n";
      head += "Content-Length: " + std::to_string(b.size() - start) + "\r\nConnection: close\r\n\r\n";
      send_all(c, head.data(), head.size());
      send_all(c, b.data() + start, size_t(b.size() - start));
      return false;
    }
    if (r.path.rfind("/r/", 0) == 0) {
      head = "HTTP/1.1 302 Found\r\nLocation: " + url(r.path.substr(2)) +
             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (r.path == "/redirect1") {
      head = "HTTP/1.1 302 Found\r\nLocation: /redirect2\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (r.path == "/redirect2") {
      head = "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + url("/file.bin") +
             "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (r.path == "/badrange.bin") {
      // Answers any Range with a 206 for the whole blob from byte 0 — a
      // range that is not the one asked for; plain GETs get a normal 200.
      head = std::string(r.range ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-" +
                                       std::to_string(blob_.size() - 1) + "/" + std::to_string(blob_.size()) + "\r\n"
                                 : "HTTP/1.1 200 OK\r\n") +
             "Content-Length: " + std::to_string(blob_.size()) + "\r\nConnection: close\r\n\r\n";
      send_all(c, head.data(), head.size());
      send_all(c, blob_.data(), blob_.size());
      return false;
    } else if (r.path == "/file.bin" || r.path == "/norange.bin") {
      bool ranged = r.range && r.path == "/file.bin";
      uint64_t start = ranged ? *r.range : 0;
      if (start >= blob_.size() && ranged) {
        head = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(blob_.size()) +
               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(c, head.data(), head.size());
        return false;
      }
      size_t n = blob_.size() - size_t(start);
      head = ranged ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) + "-" +
                          std::to_string(blob_.size() - 1) + "/" + std::to_string(blob_.size()) + "\r\n"
                    : "HTTP/1.1 200 OK\r\n";
      head += "Content-Length: " + std::to_string(n) + "\r\nConnection: close\r\n\r\n";
      send_all(c, head.data(), head.size());
      if (r.path == "/file.bin" && drops > 0) {
        drops--;
        send_all(c, blob_.data() + start, std::min(n, drop_after));
        // Reset rather than FIN, like a dropped link.
        linger lg{1, 0};
        setsockopt(c, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
        return false;
      }
      send_all(c, blob_.data() + start, n);
      return false;
    } else {
      head = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    send_all(c, head.data(), head.size());
    return false;
  }

  std::vector<uint8_t> blob_;
  SOCKET listener_ = INVALID_SOCKET;
  uint16_t port_ = 0;
  std::thread thread_;
  std::mutex m_;
  std::vector<Request> log_;
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> files_, unsized_;
  std::vector<std::string> stalled_;
  std::vector<SOCKET> held_;
};

}  // namespace test
