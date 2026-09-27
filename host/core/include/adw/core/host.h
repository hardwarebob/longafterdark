// run_host — the frame loop shared by every lane: input gating (lockstep or
// free-running), the clock, the lane step, presentation (stdout P8/P6,
// FBHASH, ADOUT) and pacing. adhostwin's main() is argument handling around
// this; tests drive it in-process with fake command sources and sinks.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "adw/core/env.h"
#include "adw/core/lane.h"
#include "adw/core/protocol.h"
#include "adw/core/status.h"

namespace adw {

// Process exit codes.
inline constexpr int kExitOk = 0;             // ADFRAMES reached, QUIT, EOF-after-GO, stdout closed, module finished
inline constexpr int kExitError = 1;          // lane init/step failure, stdout I/O error
inline constexpr int kExitUsage = 2;          // bad arguments, unreadable/unknown module, refused stream target
inline constexpr int kExitLaneMissing = 3;    // valid module whose lane is not built into this adhostwin

struct HostIo {
  CommandSource* commands = nullptr;  // null = no stdin at all
  FrameSink* frames = nullptr;        // null = headless (no stream)
  // Injected time for tests; empty = the real clock / a real sleep.
  std::function<uint64_t()> wall_us;
  std::function<void(uint64_t)> sleep_us;
  // Called after each presented frame with (frame index, FBHASH). Optional.
  std::function<void(uint64_t, uint64_t)> on_frame;
  // Whether a pre-frame-0 wait for the first stdin line is worthwhile (the
  // source is a pipe/file that may already hold the front-end's opening GO).
  bool go_wait = true;
  // The status record (ADSTATUSHANDLE / ADSTATUSLOG), published after init and
  // after every completed step. Optional.
  StatusPublisher* status = nullptr;
  // Called after each completed step with what was published (tests).
  std::function<void(const AdwHostStatusV1&)> on_status;
};

struct HostResult {
  int exit_code = kExitOk;
  uint64_t frames = 0;       // frames presented
  bool lockstep = false;     // a GO was seen
  const char* reason = "";   // why the loop ended (for the log)
};

HostResult run_host(Lane& lane, const std::string& module_path, const Env& env, HostIo& io);

// The status record's lane number for a lane (kStatusLane*; 0 = unknown).
uint32_t status_lane_id(const Lane& lane);

// adhostwin --configure's driver (INTERACTION.md §6.1), after argument
// parsing and lane selection: checks can_configure(), builds a LaneContext
// (ADCVSET and ADCAPS in the input state), runs Lane::configure() with its
// exceptions contained, and returns the process exit code (0 shown, 4
// nothing, 5 unsupported, 1 failed) with the one JSON line to print in
// *json_line (no newline).
int configure_module(Lane& lane, const std::string& module_path, const Env& env, const ConfigureRequest& req,
                     std::string* json_line);

}  // namespace adw
