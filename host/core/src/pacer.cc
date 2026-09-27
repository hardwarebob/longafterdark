#include "adw/core/pacer.h"

#include <windows.h>

#include "adw/core/clock.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace adw {

void Pacer::system_sleep_us(uint64_t us) {
  if (us == 0) return;
  // A high-resolution waitable timer (Windows 10 1803+) sleeps to well under a
  // millisecond without raising the system-wide timer rate the way
  // timeBeginPeriod(1) does. Older systems fall back to Sleep.
  static thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(us * 10);  // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  Sleep(DWORD((us + 999) / 1000));
}

Pacer::Pacer(uint32_t interval_us, bool may_sleep, WallSource wall, Sleeper sleep)
    : interval_us_(interval_us),
      may_sleep_(may_sleep),
      wall_(wall ? std::move(wall) : WallSource(VirtualClock::system_wall_us)),
      sleep_(sleep ? std::move(sleep) : Sleeper(system_sleep_us)) {}

uint64_t Pacer::wait_for_next_frame() {
  if (lockstep_ || !may_sleep_ || interval_us_ == 0) return 0;
  uint64_t now = wall_();
  if (!have_deadline_) {
    have_deadline_ = true;
    next_deadline_us_ = now + interval_us_;
  } else {
    next_deadline_us_ += interval_us_;
    // More than a period late: restart the grid rather than burst frames out
    // back to back to "catch up" on time nobody saw.
    if (now > next_deadline_us_ + interval_us_) next_deadline_us_ = now;
  }
  if (next_deadline_us_ <= now) return 0;
  uint64_t us = next_deadline_us_ - now;
  sleep_(us);
  return us;
}

}  // namespace adw
