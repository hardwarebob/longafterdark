// Emulated-screen sizing and letterboxing. Pure arithmetic, so the unit tests
// can pin the exact numbers.
#pragma once

#include <vector>

namespace adw::scr {

struct SizeI {
  int w = 0, h = 0;
  bool operator==(const SizeI&) const = default;
};

struct RectI {
  int x = 0, y = 0, w = 0, h = 0;
  bool operator==(const RectI&) const = default;
};

// Base 640x480 (the Win95 display the modules were written for), scaled by
// `scale`, widened to `display_aspect` (never narrower than 4:3 — a portrait
// monitor letterboxes instead), both axes snapped to the nearest multiple of 8,
// width capped at twice the scaled 4:3 width so an ultrawide can't explode the
// framebuffer.
SizeI emulated_screen_size(double display_aspect, double scale, int base_w = 640, int base_h = 480);

// The largest rect with the source's aspect that fits in dst_w x dst_h,
// centred (the remainder is letterbox/pillarbox).
RectI fit_rect(int src_w, int src_h, int dst_w, int dst_h);

// ---- monitor topology changes ------------------------------------------------------
// /s puts one window on every monitor. When monitors come, go, change mode
// or move (WM_DISPLAYCHANGE: a dock, a DisplayPort monitor waking from deep
// sleep, a projector), the windows are re-planned against the new monitor
// list so every monitor is covered again and no window hangs off a monitor
// that is gone, while restarting as few modules as possible.

// One saver window's place and what it shows.
struct ScreenSlot {
  RectI rc;                 // its monitor, virtual-screen pixels
  bool runs_host = false;
  SizeI emu;                // emulated screen its host renders (only when runs_host)
  bool message = false;     // carries the not-imported / host-missing text
  bool operator==(const ScreenSlot&) const = default;
};

struct RelayoutPlan {
  // Per new slot: the current window that takes it over, or -1 for a new one.
  std::vector<int> reuse;
  // Per current window: true when no new slot wants it (destroy it and its host).
  std::vector<bool> retire;
  int kept = 0, moved = 0, created = 0, retired = 0;
  bool unchanged() const { return moved == 0 && created == 0 && retired == 0; }
};

// A window keeps its host when it keeps its role (host or not, message or
// not) and its host's emulated size — the size depends only on the monitor's
// aspect, so a mode change or a rearrangement moves the window (`moved`)
// without a restart. Exact matches (same rect) are taken first, then
// same-size ones in order. Anything else is a new window with a new host.
RelayoutPlan plan_relayout(const std::vector<ScreenSlot>& current, const std::vector<ScreenSlot>& next);

} // namespace adw::scr
