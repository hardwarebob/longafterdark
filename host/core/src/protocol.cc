#include "adw/core/protocol.h"

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adw/core/log.h"

namespace adw {

// ---- command lines --------------------------------------------------------

namespace {

std::vector<std::string_view> split_ws(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isspace(uint8_t(s[i]))) i++;
    size_t start = i;
    while (i < s.size() && !isspace(uint8_t(s[i]))) i++;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

bool keyword_is(std::string_view tok, const char* kw) {
  size_t n = strlen(kw);
  if (tok.size() != n) return false;
  for (size_t i = 0; i < n; i++)
    if (toupper(uint8_t(tok[i])) != kw[i]) return false;
  return true;
}

bool to_i32(std::string_view tok, int32_t& out) {
  std::string s(tok);
  errno = 0;
  char* end = nullptr;
  long long v = strtoll(s.c_str(), &end, 10);
  if (errno || !end || *end || s.empty() || v < INT32_MIN || v > INT32_MAX) return false;
  out = int32_t(v);
  return true;
}

}  // namespace

const char* command_name(Command::Kind k) {
  switch (k) {
    case Command::Kind::go: return "GO";
    case Command::Kind::set: return "SET";
    case Command::Kind::key: return "KEY";
    case Command::Kind::caps: return "CAPS";
    case Command::Kind::mouse: return "MOUSE";
    case Command::Kind::quit: return "QUIT";
    case Command::Kind::eof: return "EOF";
    case Command::Kind::unknown: return "?";
  }
  return "?";
}

bool parse_command(std::string_view line, Command& out) {
  out = Command{};
  out.text.assign(line);
  auto tok = split_ws(line);
  if (tok.empty()) return false;
  auto nums = [&](size_t count) {
    if (tok.size() != count + 1) return false;
    int32_t* dst[3] = {&out.a, &out.b, &out.c};
    for (size_t i = 0; i < count; i++)
      if (!to_i32(tok[i + 1], *dst[i])) return false;
    return true;
  };
  Command::Kind k = Command::Kind::unknown;
  bool ok = false;
  if (keyword_is(tok[0], "GO")) {
    k = Command::Kind::go;
    ok = tok.size() == 1;
  } else if (keyword_is(tok[0], "QUIT")) {
    k = Command::Kind::quit;
    ok = tok.size() == 1;
  } else if (keyword_is(tok[0], "SET")) {
    k = Command::Kind::set;
    ok = nums(2) && out.a >= 0 && out.a <= 0xFFFF;
  } else if (keyword_is(tok[0], "KEY")) {
    k = Command::Kind::key;
    ok = nums(2) && out.a >= 0 && out.a <= 255;
    out.b = out.b != 0;
  } else if (keyword_is(tok[0], "CAPS")) {
    k = Command::Kind::caps;
    ok = nums(1);
    out.a = out.a != 0;
  } else if (keyword_is(tok[0], "MOUSE")) {
    // The third number is the button bitmask (1 left, 2 right, 4 middle;
    // INTERACTION.md §3.2). 0 and 1 keep their old meaning; anything outside
    // 0..7 is not a bitmask this protocol defines.
    k = Command::Kind::mouse;
    ok = nums(3) && out.c >= 0 && uint32_t(out.c) <= kMouseButtonMask;
  }
  if (!ok) {
    std::string text = std::move(out.text);
    out = Command{};
    out.text = std::move(text);
    return false;
  }
  out.kind = k;
  return true;
}

void InputState::apply(const Command& c) {
  if (c.seq && (c.kind == Command::Kind::key || c.kind == Command::Kind::caps ||
                c.kind == Command::Kind::mouse))
    input_seq = c.seq;
  switch (c.kind) {
    case Command::Kind::set:
      controls[c.a] = c.b;
      break;
    case Command::Kind::key:
      // parse_command bounds vk to 0..255, but a lane or test can build a
      // Command by hand, and bitset::set throws on an out-of-range bit.
      if (c.a < 0 || c.a >= int32_t(keys.size())) break;
      keys.set(size_t(c.a), c.b != 0);
      last_key = c.a;
      last_key_down = c.b != 0;
      break;
    case Command::Kind::caps:
      caps = c.a != 0;
      break;
    case Command::Kind::mouse:
      mouse_x = c.a;
      mouse_y = c.b;
      mouse_buttons = uint32_t(c.c) & kMouseButtonMask;
      mouse_button = (mouse_buttons & kMouseLeft) != 0;
      mouse_seen = true;
      break;
    default:
      break;
  }
}

int32_t InputState::control(int idx, int32_t fallback) const {
  auto it = controls.find(idx);
  return it == controls.end() ? fallback : it->second;
}

// ---- stdin reader ---------------------------------------------------------

struct StdinReader::Shared {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Command> queue;
  bool eof_delivered = false;
  std::atomic<bool> stopping{false};
  HANDLE handle = nullptr;
};

const char* StdinReader::kind_name(Kind k) {
  switch (k) {
    case Kind::none: return "none";
    case Kind::pipe: return "pipe";
    case Kind::console: return "console";
    case Kind::file: return "file";
    case Kind::char_device: return "char-device";
  }
  return "?";
}

StdinReader::StdinReader() : shared_(std::make_shared<Shared>()) {}

StdinReader::~StdinReader() { stop(); }

void StdinReader::start(HANDLE h) {
  if (thread_) return;
  kind_ = Kind::none;
  if (!h || h == INVALID_HANDLE_VALUE) return;
  DWORD type = GetFileType(h);
  DWORD mode = 0;
  switch (type) {
    case FILE_TYPE_PIPE: kind_ = Kind::pipe; break;
    case FILE_TYPE_DISK: kind_ = Kind::file; break;
    case FILE_TYPE_CHAR: kind_ = GetConsoleMode(h, &mode) ? Kind::console : Kind::char_device; break;
    default: return;  // FILE_TYPE_UNKNOWN: not something we can read
  }
  shared_->handle = h;
  // The thread holds its own reference: if stop() cannot unblock a read (a
  // console that ignores cancellation), the state outlives this object safely.
  auto* ref = new std::shared_ptr<Shared>(shared_);
  thread_ = CreateThread(nullptr, 0, thread_main, ref, 0, nullptr);
  if (!thread_) {
    delete ref;
    kind_ = Kind::none;
  }
}

DWORD WINAPI StdinReader::thread_main(void* param) {
  std::shared_ptr<Shared> s = *static_cast<std::shared_ptr<Shared>*>(param);
  delete static_cast<std::shared_ptr<Shared>*>(param);
  constexpr size_t kMaxLine = 64 * 1024;
  std::string pending;
  auto push = [&](Command c) {
    {
      std::lock_guard<std::mutex> lock(s->mu);
      s->queue.push_back(std::move(c));
    }
    s->cv.notify_all();
  };
  auto push_line = [&](std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    Command c;
    parse_command(line, c);  // malformed lines still go through, as `unknown`
    if (c.kind == Command::Kind::unknown && c.text.find_first_not_of(" \t") == std::string::npos)
      return;  // blank line
    push(std::move(c));
  };
  char buf[4096];
  // Inside an overlong line whose head was already discarded: everything up to
  // its newline goes too, or the tail would surface as one more junk "line".
  bool skipping = false;
  for (;;) {
    // Checked before every read too: a stop() that lands between two reads has
    // no read to cancel, and must not leave the thread to block in the next.
    if (s->stopping.load()) return 0;
    DWORD n = 0;
    BOOL ok = ReadFile(s->handle, buf, sizeof(buf), &n, nullptr);
    if (s->stopping.load()) return 0;
    if (!ok || n == 0) break;  // EOF, broken pipe, or cancelled
    pending.append(buf, n);
    size_t start = 0, nl;
    while ((nl = pending.find('\n', start)) != std::string::npos) {
      if (skipping) skipping = false;  // the overlong line ends here
      else push_line(std::string_view(pending).substr(start, nl - start));
      start = nl + 1;
    }
    pending.erase(0, start);
    if (skipping) {
      pending.clear();
    } else if (pending.size() > kMaxLine) {
      // No newline in 64 KB: this is not our protocol. Drop it rather than
      // grow without bound, and say so once through the unknown path.
      Command c;
      c.text = "(overlong line discarded)";
      push(std::move(c));
      pending.clear();
      skipping = true;
    }
  }
  // A final line without a trailing newline (a hand-written script) still counts.
  if (!pending.empty() && !skipping) push_line(pending);
  Command eof;
  eof.kind = Command::Kind::eof;
  push(std::move(eof));
  return 0;
}

void StdinReader::stop() {
  if (!thread_) return;
  {
    // Under the lock: a wait_ready() that has just evaluated its predicate
    // must not miss this transition and sleep through the notify below.
    std::lock_guard<std::mutex> lock(shared_->mu);
    shared_->stopping.store(true);
  }
  shared_->cv.notify_all();
  // CancelSynchronousIo only cancels a read that is already in progress. The
  // thread may be just about to enter ReadFile, so keep cancelling until it
  // exits, for a bounded time.
  bool exited = false;
  for (int i = 0; i < 40 && !exited; i++) {
    CancelSynchronousIo(thread_);
    exited = WaitForSingleObject(thread_, 5) == WAIT_OBJECT_0;
  }
  if (!exited) {
    // Still blocked (a device that ignores cancellation). It exits on its next
    // read or with the process; the shared state it references stays alive
    // until then.
    trace("proto", "stdin reader (%s) did not stop within 200 ms; leaving it", kind_name(kind_));
  }
  CloseHandle(thread_);
  thread_ = nullptr;
}

bool StdinReader::poll(Command& out) {
  std::lock_guard<std::mutex> lock(shared_->mu);
  if (shared_->queue.empty()) return false;
  out = std::move(shared_->queue.front());
  shared_->queue.pop_front();
  if (out.kind == Command::Kind::eof) shared_->eof_delivered = true;
  return true;
}

bool StdinReader::wait_ready(uint32_t timeout_ms) {
  if (kind_ == Kind::none) return false;
  std::unique_lock<std::mutex> lock(shared_->mu);
  auto ready = [&] {
    return !shared_->queue.empty() || shared_->eof_delivered || shared_->stopping.load();
  };
  if (timeout_ms == INFINITE) shared_->cv.wait(lock, ready);
  else shared_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
  return !shared_->queue.empty();
}

// ---- stdout ---------------------------------------------------------------

WriteStatus write_all(HANDLE h, const uint8_t* data, size_t size) {
  while (size > 0) {
    DWORD chunk = size > (1u << 30) ? (1u << 30) : DWORD(size);
    DWORD wrote = 0;
    if (!WriteFile(h, data, chunk, &wrote, nullptr)) {
      DWORD err = GetLastError();
      // ERROR_NO_DATA ("the pipe is being closed") is what a write into a pipe
      // whose reader has gone returns; ERROR_BROKEN_PIPE covers the rest.
      if (err == ERROR_NO_DATA || err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED)
        return WriteStatus::closed;
      return WriteStatus::error;
    }
    if (wrote == 0) return WriteStatus::error;
    data += wrote;
    size -= wrote;
  }
  return WriteStatus::ok;
}

StdoutSink::~StdoutSink() {
  if (owned_ && h_) CloseHandle(h_);
}

namespace {

// Two handles to one kernel object. A shell's `2>&1` (and most spawn APIs'
// "merge stderr") hands the child two DIFFERENT handle values for the same
// pipe, which a value comparison misses. CompareObjectHandles is Windows 10
// 1607+ (kernelbase, not in kernel32's import library), so it is looked up;
// without it only identical values are caught.
bool same_kernel_object(HANDLE a, HANDLE b) {
  if (a == b) return true;
  if (!a || !b || a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE) return false;
  using Fn = BOOL(WINAPI*)(HANDLE, HANDLE);
  static const Fn compare = [] {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    return kb ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(kb, "CompareObjectHandles")))
              : nullptr;
  }();
  return compare && compare(a, b);
}

}  // namespace

StdoutSink::Open StdoutSink::open(bool allow_disk_file) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!h || h == INVALID_HANDLE_VALUE) return Open::no_stdout;
  DWORD type = GetFileType(h);
  DWORD mode = 0;
  if (type == FILE_TYPE_CHAR && GetConsoleMode(h, &mode)) return Open::refused_console;
  // A disk file has no backpressure: the host would write frames as fast as it
  // renders them and fill the disk.
  if (type == FILE_TYPE_DISK && !allow_disk_file) return Open::refused_disk;
  // A parent that passed one pipe as both stdout and stderr would get every
  // log line spliced between frames. (Into NUL — `>NUL 2>&1`, a benchmark —
  // nothing can be corrupted, so a char device is only refused below when it
  // is literally one handle value.) Worse, with one handle value, parking fd 1
  // on NUL below closes that handle, leaving stderr's fd holding a handle
  // value that the next handle the process opens can recycle.
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err == h || (type != FILE_TYPE_CHAR && same_kernel_object(err, h)))
    return Open::refused_shared_stderr;

  // Binary mode on the CRT's fd 1 first (no CRLF translation on anything that
  // still reaches it), then take a private handle for frames.
  fflush(stdout);
  _setmode(_fileno(stdout), _O_BINARY);
  HANDLE dup = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE,
                       DUPLICATE_SAME_ACCESS))
    return Open::failed;
  h_ = dup;
  owned_ = true;
  // Park the CRT stdout and STD_OUTPUT_HANDLE on NUL: from here on only whole
  // frames written through this sink reach the consumer. freopen closes fd 1
  // — and with it `h` — before it opens NUL, so if NUL cannot be opened,
  // STD_OUTPUT_HANDLE must not be left holding the closed value, which a
  // later CreateFile could recycle for an unrelated object.
  HANDLE parked = nullptr;
  if (_wfreopen(L"NUL", L"wb", stdout)) {
    intptr_t os = _get_osfhandle(_fileno(stdout));
    if (os != -1) parked = HANDLE(os);
  }
  SetStdHandle(STD_OUTPUT_HANDLE, parked);
  return Open::ok;
}

WriteStatus StdoutSink::write(const uint8_t* data, size_t size) {
  if (!h_) return WriteStatus::error;
  return write_all(h_, data, size);
}

}  // namespace adw
