#include "input_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace adw::scr {

bool exempt_key(int vk) {
  switch (vk) {
    case VK_CAPITAL:
    case VK_NUMLOCK:
    case VK_SHIFT:
    case VK_CONTROL:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LCONTROL:
    case VK_RCONTROL:
      return true;
    default:
      return false;
  }
}

Verdict decide(const InputEvent& ev, const OwnerStatus& st, const HoldSeqs& seqs, InputClock::time_point now) {
  // §4.2: the events that never need a verdict from the host.
  switch (ev.kind) {
    case InputKind::key_up:
    case InputKind::syskey_up:
    case InputKind::button_up:
      return Verdict::forward;
    case InputKind::syskey_down:  // Alt / F10: our escape, whatever the module wants
    case InputKind::deactivate:   // switching away (Win key, Alt+Tab, Ctrl+Alt+Del)
      return Verdict::exit;
    case InputKind::wheel:
      return st.interactive() ? Verdict::forward : Verdict::exit;
    case InputKind::key_down:
      if (exempt_key(ev.vk)) return Verdict::forward;
      break;
    case InputKind::move:
      if (st.interactive() || ev.dist <= kMoveThreshold) return Verdict::forward;
      break;
    case InputKind::button_down:
      break;
  }

  // §4.3 for a non-exempt key down, a button down, a move past the threshold.
  if (st.interactive()) return Verdict::forward;
  // No host (a message on the owner window, or it died): nobody to ask.
  if (!st.running || seqs.n == 0) return Verdict::exit;
  const AdwHostStatusV1& r = st.rec;
  // Stale: the host has not stepped with the input sent before this one yet
  // (nothing published at all counts as stale: it is still starting).
  const bool stale = !st.have || r.input_applied + 1 < seqs.n;
  if (!seqs.holding && !stale && !st.key_filter()) return Verdict::exit;
  // Held: settled once the host has stepped with this line, or at the limit
  // on whatever the status says then.
  const bool settled = st.have && r.input_applied >= seqs.n;
  if (settled || now - seqs.since >= kHoldLimit) {
    return st.have && r.input_eaten >= seqs.n ? Verdict::forward : Verdict::exit;
  }
  return Verdict::hold;
}

std::string exit_reason(const InputEvent& ev, long dx, long dy) {
  char buf[64];
  switch (ev.kind) {
    case InputKind::key_down:
    case InputKind::key_up:
      snprintf(buf, sizeof(buf), "key vk=0x%02X", ev.vk & 0xFF);
      return buf;
    case InputKind::syskey_down:
    case InputKind::syskey_up:
      snprintf(buf, sizeof(buf), "syskey vk=0x%02X", ev.vk & 0xFF);
      return buf;
    case InputKind::button_down:
    case InputKind::button_up:
      return "button";
    case InputKind::wheel:
      return "wheel";
    case InputKind::move:
      snprintf(buf, sizeof(buf), "move dx=%ld dy=%ld", dx, dy);
      return buf;
    case InputKind::deactivate:
      return "deactivated";
  }
  return "?";
}

POINT map_to_frame(POINT cursor, const RECT& window, const RectI& fit, SizeI emu) {
  POINT p{0, 0};
  if (fit.w <= 0 || fit.h <= 0 || emu.w <= 0 || emu.h <= 0) return p;
  const long cx = cursor.x - window.left - fit.x, cy = cursor.y - window.top - fit.y;
  // floor division, so a point just left of the frame clamps to 0 rather
  // than rounding toward it.
  auto scale = [](long v, int to, int from) {
    long long num = (long long)v * to;
    long long q = num / from;
    if (num < 0 && q * from != num) --q;
    return q;
  };
  p.x = (LONG)std::clamp<long long>(scale(cx, emu.w, fit.w), 0, emu.w - 1);
  p.y = (LONG)std::clamp<long long>(scale(cy, emu.h, fit.h), 0, emu.h - 1);
  return p;
}

RECT frame_screen_rect(const RECT& window, const RectI& fit) {
  return RECT{window.left + fit.x, window.top + fit.y, window.left + fit.x + fit.w, window.top + fit.y + fit.h};
}

std::string key_line(int vk, bool down) { return "KEY " + std::to_string(vk & 0xFF) + (down ? " 1" : " 0"); }
std::string caps_line(bool on) { return on ? "CAPS 1" : "CAPS 0"; }
std::string mouse_line(int x, int y, uint32_t buttons) {
  return "MOUSE " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(buttons & 7);
}

// ---- AD_SCR_TEST_INPUT -----------------------------------------------------------

namespace {

std::string upper(std::string s) {
  for (char& c : s) c = (char)toupper((unsigned char)c);
  return s;
}

bool to_int(const std::string& s, int& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  long v = strtol(s.c_str(), &end, 0);
  if (!end || *end) return false;
  out = (int)v;
  return true;
}

}  // namespace

bool parse_test_script(const std::string& text, std::vector<TestStep>& out, std::string* error) {
  out.clear();
  std::istringstream in(text);
  std::string raw;
  int line_no = 0;
  auto fail = [&](const std::string& why) {
    if (error) *error = "line " + std::to_string(line_no) + ": " + why;
    return false;
  };
  while (std::getline(in, raw)) {
    ++line_no;
    if (size_t hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
    std::istringstream ls(raw);
    std::string op;
    if (!(ls >> op)) continue;
    op = upper(op);
    std::vector<std::string> args;
    for (std::string a; ls >> a;) args.push_back(a);
    TestStep st;
    auto nums = [&](size_t n) {
      if (args.size() != n) return false;
      int* dst[2] = {&st.a, &st.b};
      for (size_t i = 0; i < n; ++i)
        if (!to_int(args[i], *dst[i])) return false;
      return true;
    };
    if (op == "WAIT") {
      st.op = TestStep::Op::wait;
      if (!nums(1) || st.a < 0) return fail("WAIT <ms>");
    } else if (op == "FRAMES") {
      st.op = TestStep::Op::frames;
      if (!nums(1) || st.a < 0) return fail("FRAMES <n>");
    } else if (op == "KEY" || op == "SYSKEY") {
      st.op = op == "KEY" ? TestStep::Op::key : TestStep::Op::syskey;
      if (!nums(2) || st.a < 1 || st.a > 254) return fail(op + " <vk> <0|1>");
      st.b = st.b != 0;
    } else if (op == "CAPSSTATE") {
      st.op = TestStep::Op::caps_state;
      if (!nums(1)) return fail("CAPSSTATE <0|1>");
      st.a = st.a != 0;
    } else if (op == "BUTTON") {
      st.op = TestStep::Op::button;
      if (!nums(2) || (st.a != 1 && st.a != 2 && st.a != 4)) return fail("BUTTON <1|2|4> <0|1>");
      st.b = st.b != 0;
    } else if (op == "WHEEL") {
      st.op = TestStep::Op::wheel;
      if (!args.empty()) return fail("WHEEL takes nothing");
    } else if (op == "MOVE") {
      st.op = TestStep::Op::move;
      if (!nums(2)) return fail("MOVE <dx> <dy>");
    } else if (op == "DEACTIVATE") {
      st.op = TestStep::Op::deactivate;
    } else if (op == "DISPLAYCHANGE") {
      st.op = TestStep::Op::display_change;
    } else if (op == "CLIPLOG") {
      st.op = TestStep::Op::clip_log;
    } else if (op == "STATUSLOG") {
      st.op = TestStep::Op::status_log;
    } else if (op == "LOG") {
      st.op = TestStep::Op::log;
      for (size_t i = 0; i < args.size(); ++i) st.text += (i ? " " : "") + args[i];
    } else {
      return fail("unknown command '" + op + "'");
    }
    out.push_back(st);
  }
  return true;
}

}  // namespace adw::scr
