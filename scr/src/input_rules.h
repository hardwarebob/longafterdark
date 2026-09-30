// The saver's input rules (INTERACTION.md §4): which user input wakes the
// saver and which belongs to the module. Window-free, so the whole table is
// unit-tested (scr_unit input).
//
// The input owner's host publishes a status record (adw/core/status.h) after
// every step; each input line the saver sends it has a number (HostProcess::
// send_input). For an event that would end the saver when the module is not
// interactive, decide() answers
//   forward  the event is the module's (or harmless): keep running
//   exit     the user is back: end the saver
//   hold     not known yet: the host has not stepped with the input sent
//            before this one (a Caps Lock press a moment ago may be starting
//            a game), or it may consume input without being interactive (a
//            keyboard hook, a module reading the saver window's queue). Ask
//            again on every status change; at most kHoldLimit.
#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "adw/core/status.h"
#include "geometry.h"

namespace adw::scr {

using InputClock = std::chrono::steady_clock;

inline constexpr auto kHoldLimit = std::chrono::milliseconds(300);
// Physical pixels (Euclidean) the cursor may drift from its baseline before
// a move counts as the user coming back.
inline constexpr double kMoveThreshold = 10.0;

enum class InputKind {
  key_down,     // WM_KEYDOWN
  key_up,       // WM_KEYUP
  syskey_down,  // WM_SYSKEYDOWN (Alt, Alt+x, F10)
  syskey_up,    // WM_SYSKEYUP
  button_down,  // WM_L/R/M/XBUTTONDOWN
  button_up,
  wheel,        // WM_MOUSEWHEEL / WM_MOUSEHWHEEL
  move,         // WM_MOUSEMOVE
  deactivate,   // WM_ACTIVATEAPP(FALSE)
};

struct InputEvent {
  InputKind kind = InputKind::key_down;
  int vk = 0;          // keys
  double dist = 0;     // move: distance from the baseline, physical px
};

// What the saver knows about the input owner's host.
struct OwnerStatus {
  bool running = false;          // there is a host process, and it has not exited
  bool have = false;             // ...and a consistent status record was read
  AdwHostStatusV1 rec{};
  bool interactive() const { return have && (rec.flags & ADWS_INTERACTIVE); }
  bool key_filter() const { return have && (rec.flags & ADWS_KEY_FILTER); }
  bool cursor() const { return have && (rec.flags & ADWS_CURSOR); }
  bool rotate_ok() const { return have && (rec.flags & ADWS_ROTATE_OK); }
  bool wake() const { return have && (rec.flags & ADWS_WAKE); }
};

// One event's decision state.
struct HoldSeqs {
  uint64_t n = 0;                  // number of the input line sent for this event (0 = none sent)
  bool holding = false;            // already held: wait for input_applied >= n
  InputClock::time_point since{};  // when the event happened
};

enum class Verdict { forward, exit, hold };

// Caps Lock, Num Lock, Shift and Ctrl (and their left/right forms) never
// wake the saver (AFTERDAR.SCR 0x4028db..0x4028f2).
bool exempt_key(int vk);

// §4.2 + §4.3. Pure: the same inputs give the same answer.
Verdict decide(const InputEvent& ev, const OwnerStatus& st, const HoldSeqs& seqs, InputClock::time_point now);

// The exit reason for the log, e.g. "key vk=0x41", "syskey vk=0x12",
// "move dx=12 dy=-3", "button", "wheel", "deactivated".
std::string exit_reason(const InputEvent& ev, long dx = 0, long dy = 0);

// ---- MOUSE lines -----------------------------------------------------------------
// The cursor (screen coordinates) mapped into the owner's letterboxed frame:
// `window` is the owner window's screen rect, `fit` the frame's rect in its
// client area, `emu` the host's emulated screen. Scaled and clamped to
// [0, emu - 1].
POINT map_to_frame(POINT cursor, const RECT& window, const RectI& fit, SizeI emu);
// The frame rect in screen coordinates (for ClipCursor).
RECT frame_screen_rect(const RECT& window, const RectI& fit);

std::string key_line(int vk, bool down);
std::string caps_line(bool on);
// The Num Lock toggle (INTERACTION.md §3.2), numbered like CAPS; only for a
// host whose --capabilities says numlock=1 (dialog_support.h:
// HostCapabilities::takes_numlock_lines).
std::string numlock_line(bool on);
std::string mouse_line(int x, int y, uint32_t buttons);

// ---- AD_SCR_TEST_INPUT -----------------------------------------------------------
// Synthetic input for the smoke tests, fed through the same handlers as real
// messages (real input is ignored while a script runs), with synthetic Caps
// Lock and Num Lock toggles (both off at the start) so a test never touches
// the real ones. One command per line; '#' starts a comment:
//   WAIT <ms>              pause
//   FRAMES <n>             wait until the owner window has shown n more frames
//   KEY <vk> <0|1>         WM_KEYDOWN / WM_KEYUP (KEY 20 1 flips the synthetic Caps Lock,
//                          KEY 144 1 the synthetic Num Lock, as Windows flips a toggle on the down)
//   SYSKEY <vk> <0|1>      WM_SYSKEYDOWN / WM_SYSKEYUP
//   CAPSSTATE <0|1>        set the synthetic Caps Lock without a key
//   NUMLOCKSTATE <0|1>     set the synthetic Num Lock without a key (the saver notices within 250 ms)
//   BUTTON <1|2|4> <0|1>   left / right / middle button down / up
//   WHEEL                  WM_MOUSEWHEEL
//   MOVE <dx> <dy>         move the synthetic cursor by (dx, dy) physical px
//   DEACTIVATE             WM_ACTIVATEAPP(FALSE)
//   DISPLAYCHANGE          WM_DISPLAYCHANGE to the owner window (a monitor came, went or changed mode)
//   CLIPLOG                log GetClipCursor ("test: clip=l,t,r,b" or "test: clip=none")
//   STATUSLOG              log the owner's status record
//   LOG <text>             a marker line in the saver's log
struct TestStep {
  enum class Op { wait, frames, key, syskey, caps_state, numlock_state, button, wheel, move, deactivate, display_change,
                  clip_log, status_log, log };
  Op op = Op::wait;
  int a = 0, b = 0;
  std::string text;
};
// False (with *error) on a line it does not understand.
bool parse_test_script(const std::string& text, std::vector<TestStep>& out, std::string* error);

}  // namespace adw::scr
