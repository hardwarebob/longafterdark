// Saver-window input for the Classic lane (INTERACTION.md §5.2): how the
// host's KEY/MOUSE lines reach a Win16 module that does more than poll
// GetAsyncKeyState — a WH_KEYBOARD hook (YBYH, HOW2DRAW, MIMEHUNT, the
// ADXPL40/ADXPL310 engines), or a PeekMessage loop over the blanker window's
// queue (LUNATIC, which finds it with FindWindow("Sleep", NULL)) — and
// what the lane reports back in its status: which input lines the guest
// consumed, whether it reads keys at all (key-filter), and whether it asked
// the saver to close.
//
//   the lane, per input line, at the next point the guest can be called:
//     KEY   → user16_keyboard_hooks (HC_ACTION, most recent hook first; a
//             non-zero result consumes the key) → else user16_post_input
//             WM_KEYDOWN/WM_KEYUP to the saver window, tagged with the seq
//     MOUSE → WM_MOUSEMOVE and button messages, tagged the same way
//   the lane, after each step: user16_end_step — tagged messages the guest
//     removed (PeekMessage PM_REMOVE / GetMessage) without dispatching them
//     back to the saver window are consumed; the ones nobody took are dropped
//     (the saver window "handled" them).
//
// The keyboard tables are the US layout, fixed (never the host's), so runs
// are deterministic: scan codes (set 1, what MapVirtualKey(vk, 0) returns on
// a US keyboard) and TranslateMessage's characters.
#pragma once

#include <cstdint>

namespace adw::win16 {

class Runtime16;

// ---- the US keyboard ----
// Set-1 scan code of a virtual key (0 when it has none).
uint8_t vk_scan_code(uint8_t vk);
// Extended keys (the grey arrows/Ins/Del/Home/End/PgUp/PgDn, right Ctrl/Alt,
// numpad divide, Num Lock): lParam bit 24.
bool vk_extended(uint8_t vk);
// The character TranslateMessage makes of a key down (US layout), or -1.
int vk_to_char(uint8_t vk, bool shift, bool caps);
// The Win16 lParam of a keyboard message / keyboard hook call: repeat 1,
// scan code, extended bit, bit 30 = the key was down before (always on a
// release), bit 31 = transition (1 on release).
uint32_t key_lparam(uint8_t vk, bool down, bool was_down);

// ---- WH_KEYBOARD hooks ----
bool user16_has_keyboard_hook(Runtime16& rt);
// Calls the chain with HC_ACTION; true when the result was non-zero.
bool user16_keyboard_hooks(Runtime16& rt, uint16_t vk, uint32_t lparam, uint64_t seq);

// ---- the saver window's queue ----
// Posts an input message (behind the guest's own posted messages, as the
// system queue was read after them) for the saver window, tagged with seq.
void user16_post_input(Runtime16& rt, uint16_t msg, uint16_t wparam, uint32_t lparam, uint64_t seq);
// FindWindow("Sleep", NULL): the AD 2/3 blanker window class LUNATIC looks
// for (LUNATIC 26:0089 and 26:0181 push DS:0DE8 = "Sleep"; ADW30.EXE carries
// "SLEEP" beside "Shell_TrayWnd" in its code segment 1, presumably its
// blanker's class). The saver window answers to it.
constexpr const char* kBlankerClass16 = "Sleep";

struct StepReport16 {
  uint64_t consumed = 0;    // highest seq removed and not dispatched back to the saver window (0 none)
  uint64_t queue_reads = 0; // running count of saver-queue reads with removal and a key range
  bool wake = false;        // the guest posted WM_CLOSE / SC_CLOSE to the saver window (sticky)
  uint32_t dropped = 0;     // tagged messages nobody took this step
  uint64_t pending = 0;     // lowest seq still waiting in the saver window's queue (0 none)
};
// Tagged messages nobody took are dropped once they have waited keep_steps
// steps beyond the one they arrived in (0: at the end of their step). The
// lane keeps them while a DRAWFRAME that may still read them is suspended
// (ne16/lane.cc end_step_input).
StepReport16 user16_end_step(Runtime16& rt, uint32_t keep_steps = 0);

}  // namespace adw::win16
