#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "paths.h"

namespace adw::scr {

namespace {

void write_log_line(const char* body) {
  static std::once_flag once;
  static std::wstring path;
  std::call_once(once, [] { path = env_w(L"AD_SCR_LOG"); });
  if (path.empty()) return;

  char line[1200];
  int n = snprintf(line, sizeof(line), "[scr %lu %llu] %s\r\n", GetCurrentProcessId(),
                   (unsigned long long)GetTickCount64(), body);
  if (n <= 0) return;
  if (n >= (int)sizeof(line)) n = sizeof(line) - 1;

  // FILE_APPEND_DATA makes each WriteFile an atomic append, so the saver and
  // a Preview child (or several test processes) can share one log.
  HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD put = 0;
  WriteFile(h, line, (DWORD)n, &put, nullptr);
  CloseHandle(h);
}

// The last-exit log: the first lines of the run (how it started) and the
// latest ones (how it ended), rewritten whole on every event. Events are
// rare (spawns, exits, rotations), and a whole file on disk after every one
// survives the saver being killed.
struct LastLog {
  std::mutex mu;
  std::wstring path;
  std::vector<std::string> head;
  std::deque<std::string> tail;
  size_t dropped = 0;
  ULONGLONG started = 0;

  static constexpr size_t kHead = 40;

  void add(std::string line) {
    if (head.size() < kHead) {
      head.push_back(std::move(line));
    } else {
      tail.push_back(std::move(line));
      // One line is kept for the "... skipped ..." marker.
      while (head.size() + tail.size() + 1 > kLastLogMaxLines) {
        tail.pop_front();
        ++dropped;
      }
    }
    flush();
  }

  void flush() {
    std::string text;
    for (const auto& l : head) text += l + "\r\n";
    if (dropped) text += "... " + std::to_string(dropped) + " line(s) skipped ...\r\n";
    for (const auto& l : tail) text += l + "\r\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr);
    CloseHandle(h);
  }
};

LastLog& last() {
  static LastLog l;
  return l;
}

}  // namespace

void log_line(const char* fmt, ...) {
  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  write_log_line(body);
}

void last_log_open(const std::wstring& path) {
  LastLog& l = last();
  std::lock_guard lk(l.mu);
  l.path = path;
  l.head.clear();
  l.tail.clear();
  l.dropped = 0;
  l.started = GetTickCount64();
  if (path.empty()) return;
  ensure_dir(dir_of(path));
  l.flush();   // truncated now: a run that logs nothing leaves an empty file, not the last run's
}

void last_log(const char* fmt, ...) {
  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  write_log_line(body);
  LastLog& l = last();
  std::lock_guard lk(l.mu);
  if (l.path.empty()) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char line[1200];
  snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u +%llums %s", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long long)(GetTickCount64() - l.started), body);
  l.add(line);
}

size_t last_log_lines() {
  LastLog& l = last();
  std::lock_guard lk(l.mu);
  return l.head.size() + l.tail.size() + (l.dropped ? 1 : 0);
}

} // namespace adw::scr
