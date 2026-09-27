// adw_lane_ne16 — the Classic lane: an After Dark 2.x/3.x module (a 16-bit NE
// .AD) run on the Win16 guest runtime (host/win16), driven the way
// AFTERDAR.SCR drove it through OLDMOD32's flat thunks (ABI.md §3.1–§3.3,
// §3.7). The host side of the AD3 module protocol is a bridge (bridge.hh):
// Berkeley's own OLDMOD16.DLL as real code, or — for the AD 3.x packages that
// ship none — the host-native AD3 bridge that does what OLDMOD16 does
// (PACKAGES.md §7.4). Where the engine files come from follows the package
// rule (package.hh, PACKAGES.md §7.1/§7.3).
//
//   init:  the bridge's SETADPALETTE16(hpal, i) for the four AD palettes
//          (AFTERDAR.SCR AD_PALETTE 101..104, or ADTASK.DLL 5000/1..4);
//          LOADADMODULE16(hwnd, hdc, ctrl4, volume, mute, path, err, 260,
//          &errId) — the bridge loads AD_SND.DLL and the module (which pulls
//          in ADXPL300/310/40, AD_RSRC, … by its imports) and sends
//          MODULESELECTED → PREINITIALIZE → INITIALIZE → BLANK itself.
//   step:  SetWindowOrg(hdc, 0, 0); MODULEMESSAGE16(2 /*DRAWFRAME*/, err, 260).
//          Result (AFTERDAR.SCR 0x401f6f): 0 ok; 0x0E toggles "wants events";
//          0x11/0x12 show/hide the cursor; below 0 or above 0x12 counts as 0;
//          anything else is the module's error (text in err) and ends the run.
//   SET:   SETMODULECTRLVALUES16(volume, mute, ctrl4) — four WORDs (ABI.md
//          §3.2); the 1996 host re-sent them every 2 s, we send on change.
//   close: UNLOADADMODULE16() (CLOSE, FreeLibrary, AD_SND unloaded), then
//          every module is freed — OLDMOD16's DLLENTRYPOINT(0) included.
//
// Input and status (INTERACTION.md §5.2): AFTERDAR.SCR forwarded no key or
// mouse message to a Classic module (ABI.md §3.1); modules poll
// GetAsyncKeyState/GetCursorPos (the host's input state, VK_RBUTTON/VK_MBUTTON
// from the MOUSE bitmask, the mouse scaled to a small screen's guest display),
// install a WH_KEYBOARD hook, or read the blanker window's queue. So each
// KEY line runs the guest's WH_KEYBOARD chain (a non-zero result consumes it)
// and is otherwise posted to the saver window as WM_KEYDOWN/WM_KEYUP, MOUSE
// lines as WM_MOUSEMOVE and button messages, tagged with the line's number
// (win16/input16.hh). They go in at the next point the guest can be called:
// before the frame's first DRAWFRAME, or — the frame resuming a long call —
// inside the API call where it resumes (LUNATIC's game loop is one long
// DRAWFRAME that reads the queue every frame). After the step, what the guest
// removed and did not dispatch back is consumed; what nobody took is dropped
// (kept while a DRAWFRAME that may still take it is suspended: one more step,
// or until it does when that call reads the saver window's queue, at most
// kReaderKeepSteps; reported as unsettled meanwhile). status():
// interactive = the 0x0E toggle (source 2), cursor = 0x11 until 0x12,
// key-filter = a WH_KEYBOARD hook is in, or the guest read the saver
// window's queue with removal for keys within the last 120 steps, wake = the
// guest posted WM_CLOSE/SC_CLOSE to it, eaten = the highest line consumed (every
// line while interactive). Measured: the Caps Lock games (YBYH, SIMPTRIV, tt
// FRANKEN, tt MIMEHUNT, HOW2DRAW) hook while they play; the ADXPL40/ADXPL310
// engines hook nothing at load; without input only LUNATIC raises key-filter.
//
// Disk (INTERACTION.md §7, mount_disk below): C:\WINDOWS and C:\AFTERDRK
// (+ C:\AFTERD~1) are copy-on-write overlays whose upper layers are the
// package's state directories under ADSTATE (<state>\<package>\WINDOWS and
// \<MODDIR>) or, without ADSTATE, memory: headless runs never read or write
// user state. C:\WINDOWS\SYSTEM is the engine dir; H: the host's drives (8.3).
//
// Configure (INTERACTION.md §6.1, configure()): `adhostwin --configure`
// loads the bridge but no module and runs BUTTONPUSHED16(path, owner16, slot,
// ctrl4, err, 260, &errId) — the real OLDMOD16's, or the native bridge's
// same sequence (bridge.hh button()) — with the module's dialogs, message
// boxes and file dialogs real (win16/dialogs16.hh), owned by --owner, and the
// ADCONFIG* hooks (win32/config_script.hh). The wall clock runs (timers
// tick), no call budget. Exit: 0 when something was shown, 4 when nothing
// was, 1 on a failure (a guest fault, a dialog that could not be shown, a
// scripted dialog that timed out, AD_SND missing).
//
// Timing: 60 frames per second of virtual time (ADPACEMS overrides); the
// Win16 GetTickCount advances in 55 ms steps as on Windows 95, and every clock
// read nudges virtual time by ADREADSTEPUS (and the instructions run since
// the last read by ADMIPS) so busy-waits and the AD_RSRC /
// EINSTEIN calibration loops end (ABI.md §4). Streamed, the clock follows the
// wall from frame 0; before it (LOADADMODULE16: EINSTEIN, GLOBE and Om
// Appliances calibrate there) time is modeled as headless, then the wall clock
// carries on from there (Runtime16::start_frames). A frame's work is charged
// inside its own period when it ends (Runtime16::settle_time).
//
// Pacing: AFTERDAR.SCR sent DRAWFRAME once per pass of its idle loop, as fast
// as the machine allowed, and modules were written for that: most pace
// themselves by the clock, but some count calls (ad32 LOGO moves its picture
// every 700th/300th/100th call at Slowest/Slow/Medium, every call at Fast; 14
// distinct Classic binaries read no clock while drawing). So one presented
// frame is a run of DRAWFRAMEs: as many as fit into ADDRAWMIPS (default 25)
// million instruction-equivalents per second of the frame period —
// instructions executed, ADAPICOST per API call and ADPIXCOST per pixel a
// blit or fill writes, deterministic — at most ADMAXDRAWS. 25 is a 486-class machine, the one the AD 2/3 modules were
// written on, and what the emulator sustains in real time next to the API
// work with room to spare (~120 MIPS on 16-bit code since its fetch and data
// windows, cpu/README.md; ~70 before); a 100-MIPS budget kept busy would
// still cost about a frame of host time. That work happens inside the period (Runtime16's
// frame-bounded time, runtime16.hh): clock-paced modules keep real time, only
// a call that overruns the period pushes the clock on. ADMIPS=0 restores one
// DRAWFRAME per frame and the accumulating nudges.
//
// Long calls: some DRAWFRAMEs draw for much longer than a frame — SATORI
// about a second (it waits on the tick count, redrawing all the while),
// EINSTEIN, Fractal Forest, Om Appliances, Tunnel and Modern Art up to
// seconds, most modules' first call while it paints its scene. A 1996
// monitor showed that drawing as it happened. So the run of DRAWFRAMEs
// happens on a fiber of its own, and at the first API call (or retrace-port
// read) past the frame's deadline — the next line of the frame grid headless,
// 90% of a period of wall time streamed — it switches back: the frame is
// presented there, and the next step() resumes the call where it stopped.
// Deterministic (the deadline is virtual time); calls shorter than a frame
// are untouched. A SET that arrives mid-call reaches the module before its
// next DRAWFRAME; at close a suspended call is abandoned (the deadline hook
// throws and every call level restores the machine) before UNLOAD. Without it,
// SATORI was a 60x time-lapse headless and 1 fps streamed. ADNE16LONGCALLS=0
// (or ADMIPS=0) turns it off.
//
// Frames: the screen as the run of DRAWFRAMEs leaves it — except that a run
// which ends exactly where it began, but during which a (virtual 70 Hz) refresh
// showed something else, presents that refresh: content drawn, held by a CPU
// delay loop and erased inside one call (ZOT's lightning) is what a 1996
// monitor showed for a refresh, and would otherwise never reach a frame
// (lane.cc on_scanout).
//
// Small screens: the modules were written for a 640x480 (or larger) Win95
// display, and below it several refuse to load ("A larger screen size is
// needed…": Rat Race and You Bet Your Head below 512x384, Daredevil Dan at
// 320x240) or draw nothing or a corner of their picture (Lunatic Fringe,
// Lisa's Mood Swings, FrankenScreen). So an output smaller than 640x480 in
// either dimension (the saver's 320x240 /p preview) gets a guest display k
// times its size, the smallest whole k that reaches 640x480, and every
// presented frame is that display averaged down k×k → 1 and matched to the
// nearest hardware palette entry: a miniature of the full-screen saver. The
// mouse the guest sees is scaled up to match. Deterministic; outputs of at
// least 640x480 are untouched. ADNE16SCALE=<k> forces k (1 = off).
//
// Sound (AUDIO.md §8, win16/sound16.hh): with the host audio engine on
// (LaneContext::audio enabled: ADSOUND=1 or ADAUDIOOUT) the bridge loads the
// module unmuted at After Dark's volume slider (ADVOLUME through the engine's
// config), and MMSYSTEM plays: AD_SND's sndPlaySound images (PCM, MS-ADPCM)
// on the wave bus, the engines' MCI sequencer songs on the MIDI bus. The
// engines' music gates (§2.9) pass: one MIDI output device, TOOLHELP (a
// system module) and a stub MCISEQ.DRV; their hidden adwMidiCall window gets
// MM_MCINOTIFY at a song's end. Callbacks reach the guest at the first API
// call at or after their virtual time and at this lane's pump before every
// DRAWFRAME (the host's message loop ran between DRAWFRAMEs), which
// dispatches the notify messages the guest has not taken itself. A
// synchronous sndPlaySound (NOCTURNE) lasts its sound's duration of virtual
// time: with long calls on, the frames go on meanwhile as in a long call
// (the 1996 screen froze while the call blocked); otherwise the time is
// charged at once. Without the engine (or with it disabled) MMSYSTEM is the
// silent device of before, byte for byte, and FBHASH streams do not move.
// Every audio call carries Runtime16::peek_us() as its time, and after every
// step the lane advances the engine to it: guest time runs ahead of the core
// clock run_host advances with (a frame's work headless, the modeled init
// time for good when streamed), so live output is rendered continuously.
//
// Lane knobs (env, all optional):
//   ADNE16BRIDGE=auto|oldmod16|native  the bridge (default auto: OLDMOD16 when the engine dir has it)
//   ADNE16SCALE=auto|<k>  the guest display is k times the output (default auto; see Small screens)
//   ADNE16LONGCALLS=0  every DRAWFRAME ends its frame, however long (default on; see Long calls)
//   ADDESKTOPPAL=0|1   the display's starting palette: 1 = a desktop's 236 distinct colours between
//                      the statics (default for the AD 3 packages, which the native bridge runs:
//                      ADXPL310's identity palette needs it), 0 = black between them (the rest)
//   ADSOUND=1          sound on (with run_host's audio engine: see Sound); without an engine,
//                      unmuted on the silent device
//   ADSOUNDDEV=0       no wave device at all, sound on or off (AD_SND then refuses sounds; several
//                      modules stop); MIDI and aux devices are unaffected
//   ADVOLUME=<0..100>  the volume handed to the bridge (default 50, the registry default); with the
//                      engine on it comes from the engine's config (the same knob, clamped)
//   ADSEEDIMG=<spec>   what the screen holds before the module loads (win32/display.hh "desktop
//                      seed", as in the pe32 lane): ":win95" (teal) or a raw/P6/BMP file of any size.
//                      Unset = black. Modules with Clear Screen First off (the default for Bugs,
//                      Mowin' Man, Rebound, Mr. Burns, Objets B'art, …) and the screen transformers
//                      (Puzzle, Punch Out, Spotlight, Down the Drain) work on it. ADNOSEED=1 ignores it.
//   ADMAXDRAWS=<n>     DRAWFRAME calls per presented frame, at most (default 64; see Pacing)
//   ADDRAWMIPS=<n>     the DRAWFRAME budget, million instruction-equivalents per second (default 25,
//                      at most ADMIPS)
//   ADPIXCOST=<n>      instruction-equivalents per pixel a GDI blit/fill writes, for that budget
//                      only (default 2)
//   ADREADSTEPUS=<us>  virtual µs per clock read (default 5)
//   ADMIPS=<n>         the virtual CPU: instructions per virtual µs, for clock reads and the
//                      DRAWFRAME budget (default 100; 0 = one DRAWFRAME per frame, no instruction time)
//   ADAPICOST=<n>      instructions one API call counts as there (default 500 = 5 µs)
//   ADTICKMS=<ms>      Win16 GetTickCount granularity (default 55)
//   ADCALLBUDGET=<n>   instructions one call into the guest may run (default 1e9)
//   ADHEAPMB=<n>       the Win16 arena (default 64)
// ADTRACE=lane logs, at init, the package (or "legacy"), module dir, engine
// dir, bridge, AD_SND and palette source.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "adw/core/lane.h"
#include "ne16/bridge.hh"
#include "ne16/package.hh"

namespace adw::win16 {
class Runtime16;
struct Module16;
}  // namespace adw::win16

namespace adw::ne16 {

// The scratch block the lane passes the bridge pointers into (a fixed global block).
namespace scratch {
constexpr uint16_t kCtrl = 0x000;    // WORD ctrl4[4]
constexpr uint16_t kErrId = 0x010;   // WORD
constexpr uint16_t kPath = 0x020;    // char[260]
constexpr uint16_t kError = 0x130;   // char[260]
constexpr uint16_t kErrorSize = 260;
constexpr uint16_t kSize = 0x240;
}  // namespace scratch

class Ne16Lane : public Lane {
 public:
  Ne16Lane();
  ~Ne16Lane() override;

  const char* name() const override { return "ne16"; }
  bool init(const std::string& module_path, LaneContext& ctx) override;
  uint32_t frame_interval_us() const override { return 16667; }
  void on_command(const Command& c) override;
  StepResult step() override;
  void shutdown() override;
  LaneStatus status() const override;
  bool can_configure() const override { return true; }
  ConfigureResult configure(const std::string& module_path, LaneContext& ctx, const ConfigureRequest& req,
                            std::string* json_out) override;

  // For tests and diagnostics.
  win16::Runtime16* runtime() { return rt_.get(); }
  uint64_t frames() const { return frames_; }
  bool wants_events() const { return wants_events_; }
  const Ne16Layout& layout() const { return layout_; }
  BridgeKind bridge_kind() const { return bridge_kind_; }
  int guest_scale() const { return guest_scale_; }

  // The whole-number factor between the guest display and a w×h output (lane.hh
  // "Small screens"): the smallest k with k*w >= 640 and k*h >= 480, at most 8.
  static int auto_guest_scale(int w, int h);

 private:
  bool init_impl(const std::string& module_path, LaneContext& ctx);
  std::string error_text() const;
  void send_controls();
  void census();
  void on_scanout();
  void settle_screen();
  void sync_input();
  void present();
  bool draw_run();
  static void CALLBACK fiber_main(void* self);
  void fiber_body();
  void on_deadline();
  bool suspend_frame();
  void abandon_long_call();
  void free_fibers();
  void queue_input(const Command& c);
  void deliver_input();
  void end_step_input();

  // Input (lane.hh "Input and status"): the KEY/MOUSE lines not yet handed
  // to the guest, with what the lane knew before each (previous key state,
  // mouse position and buttons, in guest coordinates).
  struct PendingInput {
    Command::Kind kind = Command::Kind::key;
    uint8_t vk = 0;
    bool down = false, was_down = false, moved = false;
    int32_t x = 0, y = 0;
    uint32_t buttons = 0, prev_buttons = 0;
    uint64_t seq = 0;
  };
  std::vector<PendingInput> pending_input_;
  std::array<bool, 256> key_down_{};
  int32_t mouse_x_ = 0, mouse_y_ = 0;
  uint32_t mouse_buttons_ = 0;
  bool mouse_known_ = false;
  uint64_t eaten_ = 0, queue_reads_ = 0, last_read_frame_ = 0, queued_seq_ = 0;
  bool read_queue_ = false, hooked_ = false, wake_ = false;
  // A module that read the saver window's queue within this many steps is a
  // queue reader (key-filter); a suspended call of one keeps untaken input up
  // to kReaderKeepSteps steps (end_step_input).
  static constexpr uint64_t kReaderSteps = 120;
  static constexpr uint32_t kReaderKeepSteps = 600;

  // Long calls (lane.hh): the frame's DRAWFRAME run happens on guest_fiber_;
  // at the frame's deadline inside a call it switches back (suspended_), and
  // the next step() resumes it.
  enum class Run { none, frame_done, suspended, stopped, abandoned, error };
  bool long_calls_ = false;
  void* host_fiber_ = nullptr;
  void* guest_fiber_ = nullptr;
  bool converted_thread_ = false;
  bool mid_call_ = false, suspended_ = false, abandon_ = false, controls_pending_ = false;
  Run run_result_ = Run::none;
  std::exception_ptr fiber_error_;
  uint64_t frame_w0_ = 0, frame_budget_ = 0, frame_deadline_ = 0;
  uint32_t frame_draws_ = 0;
  uint64_t long_frames_ = 0;

  // Small screens: the guest's display (guest_scale_ times the output) when
  // guest_scale_ > 1, the input it sees, and the averaged-colour → hardware
  // index cache (6 bits per channel, valid for one palette).
  int guest_scale_ = 1;
  std::unique_ptr<Screen> guest_screen_;
  InputState guest_input_;
  std::array<RGBQUAD, 256> near_pal_{};
  std::vector<uint32_t> near_cache_;  // (generation << 8) | index
  uint32_t near_gen_ = 0;

  std::unique_ptr<win16::Runtime16> rt_;
  std::unique_ptr<Bridge16> bridge_;
  LaneContext* ctx_ = nullptr;
  std::string module_name_;
  Ne16Layout layout_;
  BridgeKind bridge_kind_ = BridgeKind::oldmod16;
  uint32_t scratch_ = 0;  // far pointer to the scratch block
  uint16_t hwnd_ = 0, hdc_ = 0;
  uint16_t volume_ = 50, mute_ = 1;
  int16_t ctrl_[4] = {0, 0, 0, 0};
  bool loaded_ = false, wants_events_ = false, cursor_ = false, census_done_ = false;
  uint64_t frames_ = 0;
  // Pacing: DRAWFRAMEs per presented frame until the work (instructions plus
  // ADAPICOST per API call, ADPIXCOST per pixel) reaches ADDRAWMIPS
  // (draw_mips_) × the frame period, at most max_draws_.
  uint64_t draw_mips_ = 0, draws_ = 0;
  uint32_t max_draws_ = 1;
  // Transient content (step()): the screen as the step began, the last
  // refresh during the step that showed something else, and whether the
  // presented frame is that refresh (the real screen is start_bits_ then).
  std::vector<uint8_t> start_bits_, latched_bits_;
  bool in_step_ = false, latched_ = false, showing_latched_ = false;
  uint64_t transient_frames_ = 0;
};

// The guest's disk for a module (lane.cc): C:\WINDOWS, C:\AFTERDRK/C:\AFTERD~1
// (copy-on-write overlays whose upper layers are ADSTATE's package dir, or
// memory), C:\WINDOWS\SYSTEM (the engine dir) and H: (INTERACTION.md §7.2).
void mount_disk(win16::Runtime16& rt, const Env& env, const std::string& module_path, const Ne16Layout& layout);

// The value a control record (type 1000 resource) starts at (ABI.md §2.10.2):
// a kind-1 string slider's stop value, a kind-2 numeric slider's clamped
// default, a kind-3 popup's clamped index, a kind-5 checkbox's 0/1.
int16_t control_default16(std::string_view record);

}  // namespace adw::ne16
