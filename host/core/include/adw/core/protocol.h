// The host process protocol (DESIGN.md §1) — the I/O half. stdout carries
// whole frames and nothing else; stdin carries text command lines.
//
// stdin is read by a dedicated thread that turns lines into a thread-safe
// queue. The frame loop never blocks on the pipe itself: it polls the queue
// (free-running) or waits on it (lockstep, one GO per frame). stdin may be
// absent (GUI parent), NUL, a console (typing commands is a debugging aid), a
// file (a scripted run), or the front-end's pipe; all of them look the same
// to the loop.
#pragma once

#include <windows.h>

#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace adw {

struct Command {
  enum class Kind {
    go,       // GO                      advance one frame (lockstep)
    set,      // SET <idx> <val>         a = idx, b = val
    key,      // KEY <vk> <0|1>          a = Windows virtual-key code, b = down
    caps,     // CAPS <0|1>              a = on
    mouse,    // MOUSE <x> <y> <buttons> a = x, b = y, c = button bitmask (frame-local)
    quit,     // QUIT
    eof,      // (synthetic) stdin ended
    unknown,  // anything else; `text` holds the line
  };
  Kind kind = Kind::unknown;
  int32_t a = 0, b = 0, c = 0;
  std::string text;
  // Input-line number (INTERACTION.md §3.2): KEY, CAPS and MOUSE lines are
  // numbered 1, 2, 3 ... per process in the order run_host() reads them; 0 for
  // every other command (and for anything parse_command() returns — the host
  // assigns it).
  uint64_t seq = 0;
};

// MOUSE button bits (INTERACTION.md §3.2). The old 0/1 keep their meaning.
inline constexpr uint32_t kMouseLeft = 1, kMouseRight = 2, kMouseMiddle = 4, kMouseButtonMask = 7;

// One line (without its '\n'; a trailing '\r' and surrounding blanks are
// tolerated). Returns false — with kind = unknown — for anything malformed,
// including trailing junk and out-of-range numbers.
bool parse_command(std::string_view line, Command& out);
const char* command_name(Command::Kind k);

// What the host keeps between frames. Lanes read it; the host applies every
// command to it before handing the same command to Lane::on_command.
struct InputState {
  std::map<int, int32_t> controls;  // SET / ADCVSET
  std::bitset<256> keys;            // VK down state
  bool caps = false;
  int32_t mouse_x = 0, mouse_y = 0;
  bool mouse_button = false;        // left button (= mouse_buttons & kMouseLeft)
  uint32_t mouse_buttons = 0;       // bitmask: 1 left, 2 right, 4 middle
  bool mouse_seen = false;
  int32_t last_key = -1;            // most recent KEY vk
  bool last_key_down = false;
  uint64_t input_seq = 0;           // seq of the last input line applied (Command::seq)

  void apply(const Command& c);
  int32_t control(int idx, int32_t fallback) const;
};

// The loop's view of stdin.
class CommandSource {
 public:
  virtual ~CommandSource() = default;
  // Next queued command, without blocking. An `eof` command is delivered once
  // when the stream ends; after that poll() returns false forever.
  virtual bool poll(Command& out) = 0;
  // Block until a command is queued, the stream has ended (eof pending or
  // already delivered), or timeout_ms passes (INFINITE allowed). True when
  // poll() would now return something.
  virtual bool wait_ready(uint32_t timeout_ms) = 0;
};

// stdin reader thread.
class StdinReader : public CommandSource {
 public:
  enum class Kind { none, pipe, console, file, char_device };

  StdinReader();
  ~StdinReader() override;
  // Starts reading `h` (default: this process's stdin). With no usable handle
  // the reader is Kind::none: it never yields a command, not even eof — a host
  // with no stdin simply never sees one.
  void start(HANDLE h = GetStdHandle(STD_INPUT_HANDLE));
  // Cancels a blocked read and joins briefly; never hangs on a stuck console.
  void stop();

  Kind kind() const { return kind_; }
  bool poll(Command& out) override;
  bool wait_ready(uint32_t timeout_ms) override;

  static const char* kind_name(Kind k);

 private:
  struct Shared;
  static DWORD WINAPI thread_main(void* param);
  std::shared_ptr<Shared> shared_;
  HANDLE thread_ = nullptr;
  Kind kind_ = Kind::none;
};

enum class WriteStatus { ok, closed, error };

class FrameSink {
 public:
  virtual ~FrameSink() = default;
  virtual WriteStatus write(const uint8_t* data, size_t size) = 0;
};

// stdout as a frame sink. open() switches the CRT's fd 1 to binary, takes a
// private duplicate of the stdout handle for frames and points the CRT stdout
// (and STD_OUTPUT_HANDLE) at NUL, so a stray printf anywhere in the process
// can never splice bytes into the middle of a frame.
class StdoutSink : public FrameSink {
 public:
  // refused_shared_stderr: stdout and stderr are the same handle, or two
  // handles to the same pipe/file (a shell's 2>&1), so log lines would land
  // inside the frame stream.
  enum class Open { ok, refused_console, refused_disk, refused_shared_stderr, no_stdout, failed };
  ~StdoutSink() override;
  Open open(bool allow_disk_file);
  WriteStatus write(const uint8_t* data, size_t size) override;
  // Wrap an existing handle (tests); the sink does not take ownership.
  void adopt(HANDLE h) { h_ = h; owned_ = false; }

 private:
  HANDLE h_ = nullptr;
  bool owned_ = false;
};

// WriteFile loop until every byte is written. A reader that closed its end
// (ERROR_NO_DATA / ERROR_BROKEN_PIPE) is `closed`, the normal way a host is
// told to go away.
WriteStatus write_all(HANDLE h, const uint8_t* data, size_t size);

}  // namespace adw
