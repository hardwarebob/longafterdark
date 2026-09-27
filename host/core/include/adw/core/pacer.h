// Pacer — how the frame loop waits between frames.
//
//   lockstep: once the front-end has sent a GO, every frame waits for its own
//     GO (the host loop blocks on the command source); the pacer never sleeps.
//   free-running: no GO seen yet. With sleeping allowed (ADSTREAM without
//     ADNOPACE) frames are released on a fixed deadline grid at the lane's
//     rate; headless and ADNOPACE never sleep (headless is as fast as the CPU;
//     ADNOPACE leaves the consumer's pipe backpressure as the only brake).
//
// The deadline grid does not accumulate debt: when a frame overruns by more
// than one period the grid restarts from now instead of bursting to catch up.
#pragma once

#include <cstdint>
#include <functional>

namespace adw {

class Pacer {
 public:
  using WallSource = std::function<uint64_t()>;       // monotonic µs
  using Sleeper = std::function<void(uint64_t us)>;

  Pacer(uint32_t interval_us, bool may_sleep, WallSource wall = {}, Sleeper sleep = {});

  bool lockstep() const { return lockstep_; }
  void enter_lockstep() { lockstep_ = true; }

  // Free-running only: block until the next frame's deadline. Returns the µs
  // actually requested from the sleeper (0 in lockstep / no-sleep modes).
  uint64_t wait_for_next_frame();

  uint32_t interval_us() const { return interval_us_; }
  bool may_sleep() const { return may_sleep_; }

  // High-resolution sleep (waitable timer; Sleep() granularity is 15.6 ms).
  static void system_sleep_us(uint64_t us);

 private:
  uint32_t interval_us_;
  bool may_sleep_;
  WallSource wall_;
  Sleeper sleep_;
  bool lockstep_ = false;
  bool have_deadline_ = false;
  uint64_t next_deadline_us_ = 0;
};

}  // namespace adw
