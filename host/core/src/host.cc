#include "adw/core/host.h"

#include <bitset>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/log.h"
#include "adw/core/pacer.h"
#include "adw/core/screen.h"
#include "adw/core/text.h"

namespace adw {

namespace {

// What gathering a frame's input decided.
enum class Gate { run, quit, stdin_closed };

// The loop after a successful init: input gating, step, present, pace. Fills
// in `r` as it goes, so an exception escaping it leaves an accurate count.
using Publish = std::function<void(uint64_t frames, uint64_t input_applied)>;

void run_frames(Lane& lane, LaneContext& ctx, InputState& input, const Env& env, HostIo& io,
                HostResult& r, const Publish& publish) {
  Screen& screen = ctx.screen;
  VirtualClock& clock = ctx.clock;

  uint32_t interval_us = env.pace_ms > 0.0 ? uint32_t(env.pace_ms * 1000.0 + 0.5)
                                           : lane.frame_interval_us();
  // A zero period (a sub-µs ADPACEMS, or a lane that returns 0) would freeze
  // headless virtual time, and a module waiting on the clock would never
  // advance. One µs keeps time moving while still meaning "as fast as you can".
  if (interval_us == 0) interval_us = 1;
  clock.set_step_us(interval_us);
  // "Headless runs never sleep": only a stream consumer benefits from pacing.
  Pacer pacer(interval_us, env.stream && !env.no_pace, io.wall_us, io.sleep_us);

  std::string out_dir;
  if (!env.out_dir.empty()) {
    if (env.stream) {
      log("ADOUT ignored in ADSTREAM mode (the frames are already on stdout)");
    } else {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(widen(env.out_dir)), ec);
      if (ec) log("ADOUT: cannot create '%s': %s", env.out_dir.c_str(), ec.message().c_str());
      else out_dir = env.out_dir;
    }
  }

  // Input lines (INTERACTION.md §3.2). Each KEY/CAPS/MOUSE gets the next
  // sequence number as it is read. A release whose down no completed step has
  // seen yet is held, with every input line after it (order is kept), and
  // applied right after the next step, so a tap batched into one GO still
  // shows "down" to pollers for exactly one step.
  uint64_t next_seq = 0;
  std::deque<Command> held;
  std::bitset<256> key_down_unseen;
  uint32_t mouse_down_unseen = 0;
  auto is_input = [](Command::Kind k) {
    return k == Command::Kind::key || k == Command::Kind::caps || k == Command::Kind::mouse;
  };
  auto must_hold = [&](const Command& c) {
    if (c.kind == Command::Kind::key)
      return c.b == 0 && c.a >= 0 && c.a < 256 && key_down_unseen.test(size_t(c.a));
    if (c.kind == Command::Kind::mouse) {
      uint32_t released = input.mouse_buttons & ~(uint32_t(c.c) & kMouseButtonMask);
      return (released & mouse_down_unseen) != 0;
    }
    return false;
  };
  auto apply_input = [&](const Command& c) {
    uint32_t buttons_before = input.mouse_buttons;
    input.apply(c);
    if (c.kind == Command::Kind::key && c.b && c.a >= 0 && c.a < 256) key_down_unseen.set(size_t(c.a));
    if (c.kind == Command::Kind::mouse) mouse_down_unseen |= input.mouse_buttons & ~buttons_before;
    lane.on_command(c);
    trace("proto", "%s %d %d %d (seq %" PRIu64 ") before frame %" PRIu64, command_name(c.kind), c.a, c.b, c.c,
          c.seq, r.frames);
  };
  // After a completed step: every down so far has been seen; apply what was
  // held, until a release that must wait for the next step again.
  auto release_held = [&]() {
    key_down_unseen.reset();
    mouse_down_unseen = 0;
    while (!held.empty() && !must_hold(held.front())) {
      apply_input(held.front());
      held.pop_front();
    }
  };

  // One stdin command, applied. `more` = keep gathering for this frame.
  enum class Handled { more, go, quit, closed };
  int unknown_logged = 0;
  auto handle = [&](const Command& c) -> Handled {
    switch (c.kind) {
      case Command::Kind::go:
        if (!pacer.lockstep()) {
          pacer.enter_lockstep();
          r.lockstep = true;
          trace("proto", "GO seen before frame %" PRIu64 ": lockstep from here on", r.frames);
        }
        return Handled::go;
      case Command::Kind::quit:
        return Handled::quit;
      case Command::Kind::eof:
        if (pacer.lockstep()) return Handled::closed;
        // No GO ever came: this front-end (or none) is not driving us by GO,
        // so stdin closing is not a request to exit.
        trace("proto", "stdin closed before any GO; free-running on");
        return Handled::more;
      case Command::Kind::unknown:
        if (unknown_logged < 20) {
          unknown_logged++;
          log("ignoring unrecognized input line '%s'", c.text.c_str());
        }
        return Handled::more;
      default: {
        if (!is_input(c.kind)) {  // SET: not an input line, never held
          apply_input(c);
          return Handled::more;
        }
        Command numbered = c;
        numbered.seq = ++next_seq;
        if (!held.empty() || must_hold(numbered)) {
          trace("proto", "%s %d %d %d (seq %" PRIu64 ") held until after frame %" PRIu64, command_name(c.kind),
                c.a, c.b, c.c, numbered.seq, r.frames);
          held.push_back(std::move(numbered));
        } else {
          apply_input(numbered);
        }
        return Handled::more;
      }
    }
  };

  // A lockstep front-end writes its first GO (often after a few SET lines)
  // the moment it spawns us. Give it a short window to land before frame 0,
  // consuming lines up to that GO, so frame 0 is already the reply to it
  // instead of an unrequested extra frame that would leave the consumer one
  // frame behind for the whole session.
  bool go_credit = false, quit_pending = false;
  if (io.commands && io.go_wait && env.go_wait_ms > 0) {
    std::function<uint64_t()> wall = io.wall_us ? io.wall_us : VirtualClock::system_wall_us;
    const uint64_t deadline = wall() + uint64_t(env.go_wait_ms) * 1000;
    for (;;) {
      uint64_t now = wall();
      if (now >= deadline || !io.commands->wait_ready(uint32_t((deadline - now + 999) / 1000))) break;
      Command c;
      if (!io.commands->poll(c)) break;
      Handled h = handle(c);
      if (h == Handled::go) go_credit = true;
      if (h == Handled::quit) quit_pending = true;
      if (h != Handled::more || c.kind == Command::Kind::eof) break;
    }
  }

  auto gather = [&]() -> Gate {
    if (quit_pending) return Gate::quit;
    if (go_credit) {  // the GO consumed while waiting before frame 0
      go_credit = false;
      return Gate::run;
    }
    for (;;) {
      Command c;
      if (!(io.commands && io.commands->poll(c))) {
        if (!pacer.lockstep()) return Gate::run;  // free-running: take what's queued, no more
        if (!io.commands || !io.commands->wait_ready(INFINITE)) return Gate::stdin_closed;
        continue;
      }
      switch (handle(c)) {
        case Handled::go: return Gate::run;
        case Handled::quit: return Gate::quit;
        case Handled::closed: return Gate::stdin_closed;
        case Handled::more: continue;
      }
    }
  };

  std::vector<uint8_t> wire;
  bool wire_valid = false;
  const bool want_hash = env.fbhash || bool(io.on_frame);
  uint64_t hash = 0;

  for (;;) {
    if (env.frames && r.frames >= env.frames) {
      r.reason = "ADFRAMES reached";
      break;
    }
    Gate g = gather();
    if (g == Gate::quit) {
      r.reason = "QUIT";
      break;
    }
    if (g == Gate::stdin_closed) {
      r.reason = "stdin closed after GO";
      break;
    }

    clock.begin_frame();
    const uint64_t applied_before_step = input.input_seq;
    StepResult sr = lane.step();
    // The audio engine renders (and the live sinks get) everything up to the
    // end of this step's virtual time.
    if (ctx.audio) ctx.audio->advance(clock.now_us());
    if (sr == StepResult::failed) {
      r.exit_code = kExitError;
      r.reason = "lane step failed";
      break;
    }
    if (sr == StepResult::finished) {
      r.reason = "module finished";
      break;
    }
    // Published before the frame is written, so a front-end that reads the
    // record when this frame arrives already sees this step's verdict.
    publish(r.frames + 1, applied_before_step);
    release_held();

    // Present. An unchanged screen re-sends the previous encoding (and hash)
    // untouched: modules that repaint nothing cost nothing to present.
    bool changed = screen.dirty() || !wire_valid;
    if (changed) {
      if (io.frames) {
        if (env.stream_p6) screen.encode_p6(wire);
        else screen.encode_p8(wire);
      }
      if (want_hash) hash = screen.fbhash();
      wire_valid = true;
      screen.clear_dirty();
    }
    if (io.frames) {
      WriteStatus ws = io.frames->write(wire.data(), wire.size());
      if (ws == WriteStatus::closed) {
        r.reason = "stdout closed by the reader";
        break;
      }
      if (ws == WriteStatus::error) {
        r.exit_code = kExitError;
        r.reason = "stdout write error";
        break;
      }
    }
    if (env.fbhash) {
      char line[64];
      int n = snprintf(line, sizeof(line), "FBHASH %" PRIu64 " %016" PRIx64 "\n", r.frames, hash);
      write_stderr(std::string_view(line, size_t(n)));
    }
    if (!out_dir.empty()) {
      char name[40];
      snprintf(name, sizeof(name), "\\frame_%05" PRIu64 ".ppm", r.frames);
      if (!screen.write_ppm(out_dir + name)) log("ADOUT: failed writing %s%s", out_dir.c_str(), name);
    }
    if (io.on_frame) io.on_frame(r.frames, hash);
    r.frames++;
    pacer.wait_for_next_frame();
  }
}

// The run's audio engine. It stays alive until the process exits: a lane is
// destroyed after run_host returns, and its destructor may still release
// voices or songs, which a shut-down engine accepts silently.
audio::Engine& start_audio(const Env& env) {
  static std::mutex mu;
  static std::vector<std::unique_ptr<audio::Engine>> engines;
  std::vector<std::string> warnings;
  audio::Config cfg = audio::Config::from_env(env, &warnings);
  for (const std::string& w : warnings) log("%s", w.c_str());
  std::unique_ptr<audio::Engine> made = audio::make_engine(cfg);
  audio::Engine& e = *made;
  {
    std::lock_guard<std::mutex> lock(mu);
    engines.push_back(std::move(made));
  }
  if (e.enabled()) {
    std::string where;
    if (!cfg.capture_wav.empty()) where += ", capture " + cfg.capture_wav + " + " + cfg.capture_mid;
    if (cfg.live) where += cfg.live_midi ? ", live PCM + MIDI" : ", live PCM";
    if (where.empty()) where = ", no output";
    log("sound on: volume %d, %u Hz%s%s", cfg.volume, cfg.rate, where.c_str(),
        cfg.mpc_base_channels ? ", MPC base channels kept" : "");
  }
  return e;
}

// The emulator and loader report faults as C++ exceptions (resource_dasm /
// phosg style). A lane should turn those into StepResult::failed itself; one
// that escapes must still end the run as a logged exit 1 with shutdown() run,
// not as std::terminate's abort with the lane's resources left behind.
std::string what_of(std::exception_ptr p) {
  try {
    std::rethrow_exception(p);
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "non-standard exception";
  }
}

}  // namespace

int configure_module(Lane& lane, const std::string& module_path, const Env& env, const ConfigureRequest& req,
                     std::string* json_line) {
  std::string json;
  ConfigureResult result = ConfigureResult::failed;
  if (!lane.can_configure()) {
    result = ConfigureResult::unsupported;
    json = configure_json(result, 0, std::string("the ") + lane.name() + " lane has no configure support", {});
  } else {
    try {
      Screen screen(env.screen_w, env.screen_h);
      InputState input;
      for (const auto& [idx, val] : env.cvset) input.controls[idx] = val;
      input.caps = env.caps_at_start;
      // Dialogs run on the real clock: a module's timers must tick while the
      // user looks at its dialog.
      VirtualClock clock(VirtualClock::Mode::realtime, 33333);
      LaneContext ctx{env, screen, clock, input};
      // Configure runs never make sound (AUDIO.md §7.1): a disabled engine.
      ctx.audio = &audio::null_engine();
      result = lane.configure(module_path, ctx, req, &json);
    } catch (...) {
      log("configure: lane %s threw: %s", lane.name(), what_of(std::current_exception()).c_str());
      result = ConfigureResult::failed;
      json.clear();
    }
    if (json.empty()) {
      const char* msg = result == ConfigureResult::shown         ? "shown"
                        : result == ConfigureResult::nothing     ? "the button showed nothing"
                        : result == ConfigureResult::unsupported ? "no configure support for this module"
                                                                 : "the button handler failed";
      json = configure_json(result, result == ConfigureResult::shown ? 1 : 0, msg, {});
    }
  }
  // One line, whatever the lane handed back.
  for (char& c : json)
    if (c == '\n' || c == '\r') c = ' ';
  if (json_line) *json_line = json;
  return configure_exit_code(result);
}

uint32_t status_lane_id(const Lane& lane) {
  const char* n = lane.name();
  if (!n) return 0;
  if (!strcmp(n, "pe32")) return kStatusLanePe32;
  if (!strcmp(n, "ne16")) return kStatusLaneNe16;
  if (!strcmp(n, "test-pattern")) return kStatusLaneTest;
  return 0;
}

HostResult run_host(Lane& lane, const std::string& module_path, const Env& env, HostIo& io) {
  HostResult r;
  Screen screen(env.screen_w, env.screen_h);
  InputState input;
  for (const auto& [idx, val] : env.cvset) input.controls[idx] = val;
  // ADCAPS: modules latch the Caps Lock toggle when they are created
  // (INTERACTION.md §1.2), so it must be in place before init.
  input.caps = env.caps_at_start;

  // Headless time is a pure function of the frame number; stream time follows
  // the wall clock so on-screen clocks and timed animations run at true speed.
  VirtualClock clock(env.stream ? VirtualClock::Mode::realtime : VirtualClock::Mode::fixed_step,
                     33333, io.wall_us);
  LaneContext ctx{env, screen, clock, input};
  // The audio engine (AUDIO.md §3): disabled unless ADSOUND=1 or ADAUDIOOUT.
  audio::Engine& engine = start_audio(env);
  ctx.audio = &engine;
  bool started = false;
  try {
    started = lane.init(module_path, ctx);
  } catch (...) {
    log("lane %s: init threw: %s", lane.name(), what_of(std::current_exception()).c_str());
  }
  if (!started) {
    engine.shutdown(clock.now_us());
    r.exit_code = kExitError;
    r.reason = "lane init failed";
    return r;
  }

  const uint32_t lane_id = status_lane_id(lane);
  Publish publish = [&](uint64_t frames, uint64_t applied) {
    if (!io.status && !io.on_status) return;
    LaneStatus st = lane.status();
    // Input the guest may still take is not settled: applied stays below it.
    if (st.unsettled && st.unsettled <= applied) applied = st.unsettled - 1;
    if (io.status) io.status->publish(st, frames, applied, lane_id);
    if (io.on_status) {
      AdwHostStatusV1 rec{};
      rec.magic = kStatusMagic;
      rec.version = kStatusVersion;
      rec.size = uint16_t(sizeof(rec));
      rec.flags = status_flags(st, true);
      rec.frames = frames;
      rec.input_applied = applied;
      rec.input_eaten = st.eaten;
      rec.source = st.source;
      rec.lane = lane_id;
      io.on_status(rec);
    }
  };

  try {
    publish(0, input.input_seq);
    run_frames(lane, ctx, input, env, io, r, publish);
  } catch (...) {
    // Almost always the lane (step / on_command); bad_alloc while encoding a
    // huge frame lands here too, so the message does not assume.
    log("frame loop (lane %s) aborted by an exception after %" PRIu64 " frames: %s", lane.name(),
        r.frames, what_of(std::current_exception()).c_str());
    r.exit_code = kExitError;
    r.reason = "exception in the frame loop";
  }
  // Silence first (live output stops, MIDI notes off, captures finalized),
  // then the lane lets go of its guest.
  engine.shutdown(clock.now_us());
  if (engine.enabled()) {
    audio::Stats st = engine.stats();
    char line[256];
    int n = snprintf(line, sizeof(line),
                     "[audio] voices=%" PRIu64 " chunks=%" PRIu64 " songs=%" PRIu64 " midi_events=%" PRIu64
                     " underruns=%" PRIu64 " drops=%" PRIu64 " frames=%" PRIu64 "\n",
                     st.voices_started, st.chunks, st.songs_started, st.midi_events, st.underruns,
                     st.dropped_frames, st.rendered_frames);
    write_stderr(std::string_view(line, size_t(n)));
  }
  try {
    lane.shutdown();
  } catch (...) {
    log("lane %s: shutdown threw: %s", lane.name(), what_of(std::current_exception()).c_str());
  }
  return r;
}

}  // namespace adw
