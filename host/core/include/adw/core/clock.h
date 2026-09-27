// VirtualClock — the ONE time source every emulated time API reads
// (GetTickCount, timeGetTime, QueryPerformanceCounter, GetSystemTime…, and the
// Win16 equivalents). Nothing in a lane may read the real clock directly;
// that is what keeps headless runs reproducible.
//
//   fixed_step (headless): virtual time is frame * step. Two runs of the same
//     module execute the same instructions and read the same times, so their
//     FBHASH streams match bit for bit.
//   realtime (ADSTREAM): virtual time follows the wall clock (times `scale`).
//     Each gap between two observations of the clock (a time read, or a
//     begin_frame) is capped at max_gap, so a front-end that stops sending GO
//     (occluded window) does not make the module leap ahead on resume — while
//     a module that busy-waits inside one long step, reading the time as it
//     goes, still sees the wall clock run at full speed. (Capping the time
//     since the frame began instead would freeze time 250 ms into any step,
//     and a calibration loop waiting for a later tick would spin forever.)
//
// read_us() is what time APIs call. With a read step set, every read nudges
// time forward a little: a module that busy-waits on GetTickCount inside one
// frame would otherwise spin forever on a fixed-step clock. Read counts are a
// deterministic function of the emulated code, so this stays reproducible.
#pragma once

#include <cstdint>
#include <functional>

namespace adw {

class VirtualClock {
 public:
  enum class Mode { fixed_step, realtime };
  using WallSource = std::function<uint64_t()>;  // monotonic µs

  // GetTickCount() at virtual time 0 — a plausible uptime (~17 minutes), so
  // code that treats a zero tick as "never" behaves.
  static constexpr uint32_t kBootOffsetMs = 0x00100000;

  VirtualClock(Mode mode, uint32_t step_us, WallSource wall = {});

  // Called by the host once before each frame's lane step. Frame 0 runs at
  // virtual time 0.
  void begin_frame();

  uint64_t now_us() const;                  // peek (no nudge)
  uint64_t now_ms() const { return now_us() / 1000; }
  uint32_t tick_count() const { return uint32_t(kBootOffsetMs + now_ms()); }
  uint64_t read_us();                       // time-API read (applies the nudge)
  uint32_t read_tick_count() { return uint32_t(kBootOffsetMs + read_us() / 1000); }

  Mode mode() const { return mode_; }
  uint64_t frame() const { return frame_; }  // index of the current frame (after begin_frame)
  uint32_t step_us() const { return step_us_; }
  void set_step_us(uint32_t us) { step_us_ = us; }
  void set_read_step_us(uint32_t us) { read_step_us_ = us; }
  // Both take effect from now on: time already elapsed keeps the old
  // rate/cap, so virtual time never jumps (in either direction).
  void set_scale(double s);
  void set_max_gap_us(uint64_t us);

  static uint64_t system_wall_us();  // steady_clock, µs

 private:
  bool realtime_running() const { return mode_ == Mode::realtime && started_; }
  uint64_t realtime_gap_us() const;  // capped wall µs since the last observation
  void observe();                    // bank that gap (realtime)
  uint64_t realtime_us(uint64_t gap_us) const {
    return uint64_t(double(rt_wall_us_ + gap_us) * scale_);
  }

  Mode mode_;
  uint32_t step_us_;
  WallSource wall_;
  double scale_ = 1.0;
  uint64_t max_gap_us_ = 250000;
  uint32_t read_step_us_ = 0;
  bool started_ = false;
  uint64_t frame_ = 0;
  // fixed_step: frame * step. realtime: virtual µs banked by set_scale().
  uint64_t base_us_ = 0;
  // realtime: sum of the capped wall gaps observed since the last set_scale(),
  // kept UNSCALED so that folding many µs-apart reads at a fractional scale
  // loses nothing to rounding.
  uint64_t rt_wall_us_ = 0;
  uint64_t last_wall_us_ = 0;  // realtime: wall time of the last observation
  uint64_t nudge_us_ = 0;      // accumulated read nudges
};

}  // namespace adw
