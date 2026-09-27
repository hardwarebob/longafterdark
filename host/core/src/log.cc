#include "adw/core/log.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace adw {
namespace {

std::mutex g_mu;
std::string g_prefix = "adw";
std::set<std::string> g_trace;
bool g_trace_all = false;
// Shims will ask tracing("gdi") on every emulated API call; with ADTRACE unset
// (the normal case) that must cost one relaxed load, not a lock and a lookup.
std::atomic<bool> g_trace_any{false};

void emit_line(std::string line) {
  if (line.empty() || line.back() != '\n') line.push_back('\n');
  std::lock_guard<std::mutex> lock(g_mu);
  fwrite(line.data(), 1, line.size(), stderr);
  fflush(stderr);
}

std::string vformat(const char* fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap2);
  va_end(ap2);
  std::string s(size_t(n > 0 ? n : 0), '\0');
  if (n > 0) vsnprintf(s.data(), size_t(n) + 1, fmt, ap);
  return s;
}

}  // namespace

void set_log_prefix(std::string_view prefix) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_prefix.assign(prefix);
}

void log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string body = vformat(fmt, ap);
  va_end(ap);
  std::string prefix;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    prefix = g_prefix;
  }
  emit_line("[" + prefix + "] " + body);
}

// Categories match case-insensitively, as Env::traced does, so ADTRACE=GDI and
// tracing("gdi") agree whichever side a caller spells in capitals.
static std::string lower_ascii(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = char(tolower(uint8_t(c)));
  return r;
}

void set_trace_categories(const std::set<std::string>& cats) {
  std::set<std::string> lowered;
  for (const std::string& c : cats) lowered.insert(lower_ascii(c));
  std::lock_guard<std::mutex> lock(g_mu);
  g_trace_all = lowered.count("all") || lowered.count("*");
  g_trace = std::move(lowered);
  g_trace_any.store(!g_trace.empty(), std::memory_order_relaxed);
}

bool tracing(std::string_view cat) {
  if (!g_trace_any.load(std::memory_order_relaxed)) return false;
  std::string key = lower_ascii(cat);
  std::lock_guard<std::mutex> lock(g_mu);
  return g_trace_all || g_trace.count(key);
}

void trace(std::string_view cat, const char* fmt, ...) {
  if (!tracing(cat)) return;
  va_list ap;
  va_start(ap, fmt);
  std::string body = vformat(fmt, ap);
  va_end(ap);
  emit_line("[" + std::string(cat) + "] " + body);
}

void write_stderr(std::string_view text) {
  std::lock_guard<std::mutex> lock(g_mu);
  fwrite(text.data(), 1, text.size(), stderr);
  fflush(stderr);
}

}  // namespace adw
