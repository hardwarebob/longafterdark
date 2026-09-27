#include "adw/core/clock.h"

#include <algorithm>
#include <chrono>

namespace adw {

uint64_t VirtualClock::system_wall_us() {
  using namespace std::chrono;
  return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

VirtualClock::VirtualClock(Mode mode, uint32_t step_us, WallSource wall)
    : mode_(mode), step_us_(step_us), wall_(wall ? std::move(wall) : WallSource(system_wall_us)) {}

uint64_t VirtualClock::realtime_gap_us() const {
  uint64_t now = wall_();
  return std::min(now > last_wall_us_ ? now - last_wall_us_ : 0, max_gap_us_);
}

// One wall read serves both the gap and the new reference point, so no sliver
// of time between two reads is dropped. Peeks (now_us) compute the same capped
// gap without banking it, and the gap only grows until the next observation,
// so virtual time is monotonic across peeks, reads and frame boundaries.
void VirtualClock::observe() {
  uint64_t now = wall_();
  if (now > last_wall_us_) {
    rt_wall_us_ += std::min(now - last_wall_us_, max_gap_us_);
    last_wall_us_ = now;
  }
}

void VirtualClock::set_scale(double s) {
  if (realtime_running()) {
    observe();
    base_us_ += realtime_us(0);
    rt_wall_us_ = 0;
  }
  scale_ = s;
}

void VirtualClock::set_max_gap_us(uint64_t us) {
  if (realtime_running()) observe();
  max_gap_us_ = us;
}

void VirtualClock::begin_frame() {
  if (!started_) {
    // Frame 0 runs at virtual time 0 (plus any nudges from reads during the
    // lane's init); realtime starts following the wall clock from here.
    started_ = true;
    frame_ = 0;
    if (mode_ == Mode::realtime) last_wall_us_ = wall_();
    return;
  }
  frame_++;
  if (mode_ == Mode::fixed_step) base_us_ += step_us_;
  else observe();
}

uint64_t VirtualClock::now_us() const {
  uint64_t t = base_us_ + nudge_us_;
  if (realtime_running()) t += realtime_us(realtime_gap_us());
  return t;
}

uint64_t VirtualClock::read_us() {
  nudge_us_ += read_step_us_;
  if (!realtime_running()) return base_us_ + nudge_us_;
  observe();
  return base_us_ + nudge_us_ + realtime_us(0);
}

}  // namespace adw
