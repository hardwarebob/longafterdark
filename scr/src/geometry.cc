#include "geometry.h"

#include <algorithm>

namespace adw::scr {

namespace {
// Truncate, then round to the nearest multiple of 8.
int snap8(double v) { return (((int)v + 4) / 8) * 8; }
} // namespace

SizeI emulated_screen_size(double display_aspect, double scale, int base_w, int base_h) {
  if (!(scale > 0.0)) scale = 1.0;
  int h = snap8(base_h * scale);
  double a = std::max(display_aspect > 0.0 ? display_aspect : 0.0, (double)base_w / base_h);
  int w = std::min(snap8(h * a), snap8(base_w * scale) * 2);
  return {w, h};
}

RectI fit_rect(int src_w, int src_h, int dst_w, int dst_h) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return {0, 0, std::max(dst_w, 0), std::max(dst_h, 0)};
  // Compare cross-products in 64-bit to pick the limiting axis exactly.
  long long lhs = (long long)dst_w * src_h, rhs = (long long)dst_h * src_w;
  int w, h;
  if (lhs > rhs) {            // destination is wider: full height, pillarbox
    h = dst_h;
    w = (int)(((long long)dst_h * src_w + src_h / 2) / src_h);
  } else {                    // destination is taller (or equal): full width, letterbox
    w = dst_w;
    h = (int)(((long long)dst_w * src_h + src_w / 2) / src_w);
  }
  w = std::min(w, dst_w);
  h = std::min(h, dst_h);
  return {(dst_w - w) / 2, (dst_h - h) / 2, w, h};
}

namespace {
// What a window shows, apart from where: interchangeable windows have equal roles.
bool same_role(const ScreenSlot& a, const ScreenSlot& b) {
  return a.runs_host == b.runs_host && a.message == b.message && (!a.runs_host || a.emu == b.emu);
}
} // namespace

RelayoutPlan plan_relayout(const std::vector<ScreenSlot>& current, const std::vector<ScreenSlot>& next) {
  RelayoutPlan p;
  p.reuse.assign(next.size(), -1);
  std::vector<bool> taken(current.size(), false);
  // Pass 1: the same monitor as before (and nothing else about it changed).
  for (size_t n = 0; n < next.size(); ++n) {
    for (size_t c = 0; c < current.size(); ++c) {
      if (!taken[c] && current[c].rc == next[n].rc && same_role(current[c], next[n])) {
        p.reuse[n] = (int)c;
        taken[c] = true;
        ++p.kept;
        break;
      }
    }
  }
  // Pass 2: a window whose host can carry on where it is sent.
  for (size_t n = 0; n < next.size(); ++n) {
    if (p.reuse[n] >= 0) continue;
    for (size_t c = 0; c < current.size(); ++c) {
      if (!taken[c] && same_role(current[c], next[n])) {
        p.reuse[n] = (int)c;
        taken[c] = true;
        ++p.moved;
        break;
      }
    }
    if (p.reuse[n] < 0) ++p.created;
  }
  p.retire.resize(current.size());
  for (size_t c = 0; c < current.size(); ++c) {
    p.retire[c] = !taken[c];
    if (!taken[c]) ++p.retired;
  }
  return p;
}

} // namespace adw::scr
