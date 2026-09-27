#include "ne16/lane.hh"

#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "loader/ne.hh"
#include "win16/dos16.hh"
#include "win16/dialogs16.hh"
#include "win16/gdi16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win16/sound16.hh"
#include "win32/config_script.hh"
#include "win32/display.hh"
#include "win32/vfs.hh"

namespace adw::ne16 {

using win16::GuestError16;
using win16::Runtime16;
using win16::l16;
using win16::w16;

namespace {

std::string full_path(const std::string& p) {
  std::wstring w = widen(p);
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (!n) return p;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return narrow(out);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

uint64_t env_u64(const Env& env, const char* name, uint64_t def) {
  const std::string* v = env.get(name);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  double d = strtod(v->c_str(), &end);  // accepts 1e9 as well as plain integers
  if (end == v->c_str() || d < 0) {
    log("%s='%s' is not a number; using %" PRIu64, name, v->c_str(), def);
    return def;
  }
  return uint64_t(d);
}

bool file_exists(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_host_file(const std::string& p, std::vector<uint8_t>* out) {
  HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (16 << 20);
  if (ok) {
    out->resize(size_t(sz.QuadPart));
    DWORD got = 0;
    ok = out->empty() || (ReadFile(h, out->data(), DWORD(out->size()), &got, nullptr) && got == out->size());
  }
  CloseHandle(h);
  return ok;
}

bool same_dir(const std::string& a, const std::string& b) {
  return CompareStringOrdinal(widen(full_path(a)).c_str(), -1, widen(full_path(b)).c_str(), -1, TRUE) == CSTR_EQUAL;
}

uint16_t u16(std::string_view s, size_t off) {
  if (off + 2 > s.size()) return 0;
  return uint16_t(uint8_t(s[off]) | (uint8_t(s[off + 1]) << 8));
}

}  // namespace

// The guest's disk (INTERACTION.md §7.2), one 1996 machine per package:
//   C:\AFTERDRK   the module's folder — the install directory, the AD Data
//                 Files directory, where the helper DLLs, sound databases and
//                 data files sit beside the modules — under a copy-on-write
//                 upper layer <state>\<package>\<MODDIR> (NONSENSE.TXT,
//                 MESG_AD3.DAT land there)
//   C:\AFTERD~1   the same folder under the short name of the original
//                 install directory ("C:\After Dark"), which data files
//                 carry baked in (BITMAPS.ADC lists C:\afterd~1\bitmaps); the
//                 same upper (a memory upper is one per mount). An alias: it
//                 resolves, but a listing of C:\ leaves it out (a 1996
//                 install had one of the two; DOS Shell's DIR shows C:\)
//   C:\WINDOWS    virtual seed files under <state>\<package>\WINDOWS
//                 (MODULES.INI, AFTERDRK.INI, WIN.INI …, and LunData.dat as
//                 the installers put it there), shared with the package's AD4
//                 modules
//   C:\WINDOWS\SYSTEM  the engine dir (OLDMOD16, AD_SND), read-only
//   H:\<L>\…      the host's drives, read-only, 8.3 names (file dialogs)
// Without ADSTATE every upper layer is memory: nothing is read from or
// written to the user's state, so headless runs and FBHASH stay as they were.
void mount_disk(Runtime16& rt, const Env& env, const std::string& module_path, const Ne16Layout& layout) {
  const win16::Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  std::string pkg = package_state_dir(env, module_path);
  std::string moddir = file_of(layout.module_dir);
  if (env.state_persistent()) vfs.set_state_root(env.state_root);
  vfs.mount_overlay(o.windows_dir, "", pkg.empty() ? "" : pkg + "\\WINDOWS");
  vfs.mount_overlay(o.guest_dir, layout.module_dir, pkg.empty() ? "" : pkg + "\\" + moddir);
  vfs.mount_overlay("C:\\AFTERD~1", layout.module_dir, pkg.empty() ? "" : pkg + "\\" + moddir);
  vfs.hide_in_listing("C:\\AFTERD~1");
  vfs.mount(o.system_dir, layout.engine_dir, /*writable=*/false);
  vfs.mount_host_drives(/*short_names=*/true);
  vfs.set_cwd(o.guest_dir);
  for (const std::string& d : layout.search_dirs) rt.modules().add_search_dir(d);
  // What the installers copied from the module folder into WINDOWS (the ad10
  // install map has WINDOWS\LunData.dat, byte-identical to the disc's
  // LUNDATA.DAT beside LUNATIC.AD): Lunatic Fringe reads its keys and scores
  // from GetWindowsDirectory()\LunData.dat, and without it says
  // "Configuration File Not Accessible. Will Use Default Keys.". A lower-layer
  // seed: the module's own writes (Keys…, Clear Scores, high scores) go to the
  // upper layer as usual.
  for (const char* f : {"LunData.dat"}) {
    std::vector<uint8_t> bytes;
    if (read_host_file(layout.module_dir + "\\" + f, &bytes)) vfs.add_virtual_file(o.windows_dir + "\\" + f, std::move(bytes));
  }
  // MODULES.INI's per-install settings, now that the module dir is mounted.
  win16::seed_modules_ini(rt);
  trace("lane", "disk: C:\\WINDOWS and %s over %s", o.guest_dir.c_str(),
        pkg.empty() ? "memory (no ADSTATE)" : (pkg + " (" + moddir + ")").c_str());
}

int16_t control_default16(std::string_view rec) {
  if (rec.size() < 0x1A) return 0;
  uint16_t kind = u16(rec, 0x00);
  int16_t def = int16_t(u16(rec, 0x18));
  switch (kind) {
    case 1: {  // string slider: `count` 16-byte labels at +0x20, then the stop values
      uint16_t count = std::min<uint16_t>(u16(rec, 0x16), 101);
      if (!count) return 0;
      size_t at = 0x20 + size_t(count) * 16;
      if (at + size_t(count) * 2 > rec.size()) return 0;
      std::vector<int32_t> stops;
      // A list that does not start at 0 gets a 0 stop in front (AFTERDAR.SCR 0x404f7b).
      if (u16(rec, at) != 0) stops.push_back(0);
      for (uint16_t i = 0; i < count; i++) stops.push_back(u16(rec, at + 2 * i));
      int32_t v = stops.front();
      for (int32_t s : stops)
        if (s <= def) v = s;
      return int16_t(v);
    }
    case 2: {  // numeric slider: clamped to [min, max] at +0x30/+0x32
      if (rec.size() < 0x34) return def;
      int16_t lo = int16_t(u16(rec, 0x30)), hi = int16_t(u16(rec, 0x32));
      if (lo > hi) std::swap(lo, hi);
      return std::clamp<int16_t>(def, lo, hi);
    }
    case 3: {  // popup: an item index
      uint16_t count = u16(rec, 0x16);
      return count ? std::clamp<int16_t>(def, 0, int16_t(count - 1)) : 0;
    }
    case 5:  // checkbox
      return def ? 1 : 0;
    default:  // none, button
      return 0;
  }
}

Ne16Lane::Ne16Lane() = default;

Ne16Lane::~Ne16Lane() {
  bridge_.reset();
  rt_.reset();
  free_fibers();
}

std::string Ne16Lane::error_text() const {
  if (!rt_ || !scratch_) return "(no error text)";
  std::string s;
  try {
    s = rt_->read_str(scratch_ + scratch::kError, scratch::kErrorSize);
  } catch (const std::exception&) {
  }
  return s.empty() ? "(no error text)" : s;
}

// A failed init drops the runtime at once: it refers to ctx's screen, clock
// and input, which run_host destroys before the lane (lane.h).
bool Ne16Lane::init(const std::string& module_path, LaneContext& ctx) {
  if (init_impl(module_path, ctx)) return true;
  bridge_.reset();
  rt_.reset();
  free_fibers();
  loaded_ = false;
  ctx_ = nullptr;
  return false;
}

bool Ne16Lane::init_impl(const std::string& module_path, LaneContext& ctx) {
  ctx_ = &ctx;
  const Env& env = ctx.env;
  std::string path = full_path(module_path);
  module_name_ = file_of(path);
  // Where the engine files come from (package.hh, PACKAGES.md §7.1/§7.3).
  layout_ = resolve_layout(path, env.win_assets_dir(), file_exists);
  const std::string& dir = layout_.module_dir;
  const std::string& engine = layout_.engine_dir;
  bool bridge_auto = true;
  BridgeKind forced = BridgeKind::oldmod16;
  if (const std::string* b = env.get("ADNE16BRIDGE")) {
    if (!parse_bridge_choice(*b, &bridge_auto, &forced)) {
      log("ADNE16BRIDGE='%s' is not auto, oldmod16 or native; using auto", b->c_str());
      bridge_auto = true;
    }
  }
  bridge_kind_ = bridge_auto ? choose_bridge(layout_, file_exists) : forced;
  volume_ = uint16_t(std::min<uint64_t>(env_u64(env, "ADVOLUME", 50), 100));
  mute_ = env.flag("ADSOUND") ? 0 : 1;
  // Sound (lane.hh "Sound", AUDIO.md §8.1): with the host audio engine on,
  // the module is unmuted at After Dark's volume slider (ADVOLUME, through
  // the engine's config); AD_PREFS.INI stays absent, so AD_SND's own mute
  // default (off) holds.
  const bool sound_on = ctx.audio && ctx.audio->enabled();
  if (sound_on) {
    volume_ = uint16_t(std::clamp(ctx.audio->config().volume, 0, 100));
    mute_ = 0;
  }

  win16::Runtime16Options opts;
  opts.arena_size = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADHEAPMB", 64), 16, 512) << 20);
  opts.call_budget = env_u64(env, "ADCALLBUDGET", 1'000'000'000ull);
  opts.tick_quantum_ms = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADTICKMS", 55), 1, 1000));
  if (env.get("ADSOUNDDEV")) opts.sound_device = env.flag("ADSOUNDDEV");
  // The display's starting palette (Runtime16Options::desktop_palette): a
  // desktop's distinct colours for the AD 3 generation packages (no OLDMOD16
  // in their engine dir: the native bridge stands in for ADW30/ADTASK, and
  // ADXPL310's identity palette needs them — SIMPCLOK); the lane's original
  // black-between-the-statics for everything OLDMOD16 runs, so Deluxe's and
  // ad10's streams stay as they were. ADDESKTOPPAL=0/1 overrides.
  opts.desktop_palette = layout_.packaged && choose_bridge(layout_, file_exists) == BridgeKind::native;
  if (env.get("ADDESKTOPPAL")) opts.desktop_palette = env.flag("ADDESKTOPPAL");
  if (const std::string* w = env.get("ADSTEP16")) {
    unsigned cs = 0, lo = 0, hi = 0;
    if (sscanf(w->c_str(), "%x:%x-%x", &cs, &lo, &hi) == 3) {
      opts.step_cs = uint16_t(cs);
      opts.step_lo = uint16_t(lo);
      opts.step_hi = uint16_t(hi);
    }
  }
  if (const std::string* w = env.get("ADWATCH16")) {
    unsigned sel = 0, off = 0;
    if (sscanf(w->c_str(), "%x:%x", &sel, &off) == 2) opts.watch = (sel << 16) | (off & 0xFFFF);
  }
  // Deadline loops and CPU-speed calibrations inside one call must see time
  // move (ABI.md §4): per clock read, and per instruction executed.
  opts.read_step_us = uint32_t(env_u64(env, "ADREADSTEPUS", 5));
  opts.insns_per_us = uint32_t(env_u64(env, "ADMIPS", 100));
  opts.api_cost_insns = uint32_t(env_u64(env, "ADAPICOST", 500));
  opts.pixel_cost_insns = uint32_t(env_u64(env, "ADPIXCOST", 2));
  // Pacing (lane.hh): with ADMIPS set, headless time is Runtime16's
  // frame-bounded model, which applies the read step itself, and streamed
  // time is the wall clock as it is (a run of DRAWFRAMEs reads the clock
  // many times a frame; nudging each read would make it outrun the wall).
  // With ADMIPS=0 the core clock nudges every read, as the lane always did.
  ctx.clock.set_read_step_us(opts.insns_per_us ? 0 : opts.read_step_us);
  // The DRAWFRAME budget: ADDRAWMIPS (default 25) of the ADMIPS machine's
  // frame period — a 486-class share, what the emulator sustains in real
  // time (a 100-MIPS budget kept busy costs more than a frame of host time).
  draw_mips_ = std::min<uint64_t>(env_u64(env, "ADDRAWMIPS", 25), opts.insns_per_us);
  max_draws_ = opts.insns_per_us ? uint32_t(std::clamp<uint64_t>(env_u64(env, "ADMAXDRAWS", 64), 1, 100000)) : 1;
  // Long calls (lane.hh): on with the virtual CPU; ADNE16LONGCALLS=0 turns them off.
  long_calls_ = opts.insns_per_us && !(env.get("ADNE16LONGCALLS") && !env.flag("ADNE16LONGCALLS"));
  // Small screens (lane.hh): the guest gets a display k times the output.
  guest_scale_ = auto_guest_scale(ctx.screen.width(), ctx.screen.height());
  if (const std::string* s = env.get("ADNE16SCALE"); s && !s->empty() && *s != "auto") {
    char* end = nullptr;
    long k = strtol(s->c_str(), &end, 10);
    if (end == s->c_str() || *end || k < 1 || k > 8) {
      log("ADNE16SCALE='%s' is not auto or 1..8; using auto (%d)", s->c_str(), guest_scale_);
    } else {
      guest_scale_ = int(k);
    }
  }
  if (int64_t(ctx.screen.width()) * guest_scale_ > Screen::kMaxDim ||
      int64_t(ctx.screen.height()) * guest_scale_ > Screen::kMaxDim) {
    guest_scale_ = 1;
  }
  if (guest_scale_ > 1) {
    guest_screen_ = std::make_unique<Screen>(ctx.screen.width() * guest_scale_, ctx.screen.height() * guest_scale_);
    sync_input();
    trace("lane", "%dx%d output: the guest display is %dx%d (x%d), averaged down each frame", ctx.screen.width(),
          ctx.screen.height(), guest_screen_->width(), guest_screen_->height(), guest_scale_);
  }

  try {
    rt_ = std::make_unique<Runtime16>(opts, ctx.clock, guest_screen_ ? &guest_input_ : &ctx.input);
    Runtime16& rt = *rt_;
    win16::register_all16(rt);
    // MMSYSTEM's sound half over the host audio engine (win16/sound16.hh);
    // without an enabled one, the silent device the lane always had.
    win16::attach_audio16(rt, ctx.audio);
    mount_disk(rt, env, path, layout_);
    win32::Display& display = rt.attach_display(guest_screen_ ? *guest_screen_ : ctx.screen);
    // The desktop the saver started over (win32/display.hh "desktop seed"),
    // as in the pe32 lane. AFTERDAR.SCR's full-screen saver window never
    // erases itself (WM_ERASEBKGND 0x401466 returns 1 without painting unless
    // it is the /p preview), so the screen held the desktop when OLDMOD16 sent
    // BLANK, and each module's own BLANK decides what survives — Puzzle, Punch
    // Out, Spotlight, Down the Drain and Mowin' Man work on it; Zooommm! and
    // Mowin' Boris blank it (PACKAGES.md §12 has the census list). Off unless asked
    // for, so unseeded runs stay byte-identical.
    if (const std::string* s = env.get("ADSEEDIMG"); s && !s->empty() && !env.flag("ADNOSEED")) {
      std::string what;
      if (display.seed(*s, &what)) {
        trace("lane", "desktop seed: %s", what.c_str());
      } else {
        log("ADSEEDIMG: %s; the screen starts black", what.c_str());
      }
    }
    rt.set_scanout_hook([this] { on_scanout(); });

    hwnd_ = win16::user16_saver_window(rt);
    hdc_ = win16::gdi16_screen_dc(rt, hwnd_);
    if (!hdc_) throw std::runtime_error("no screen DC");

    std::string ad_snd = engine + "\\AD_SND.DLL", why;
    if (bridge_kind_ == BridgeKind::oldmod16) {
      // The AD_SND guard (PACKAGES.md §7.3): an AD_SND.DLL beside the module
      // (older than OLDMOD16 accepts, in every layout seen) would be found
      // before the engine dir's. Load the engine's first; OLDMOD16's
      // LoadLibrary("ad_snd.dll") then finds it by module name.
      if (!same_dir(dir, engine) && file_exists(dir + "\\AD_SND.DLL") && file_exists(ad_snd)) {
        uint16_t e = 0;
        if (rt.modules().load_host(ad_snd, &e)) log("%s: %s\\AD_SND.DLL is ignored; OLDMOD16 gets %s", module_name_.c_str(), dir.c_str(), ad_snd.c_str());
      }
      bridge_ = open_oldmod16_bridge(rt, engine + "\\OLDMOD16.DLL", &why);
    } else {
      bridge_ = open_native_bridge(rt, opts.system_dir + "\\AD_SND.DLL", &why);
    }
    if (!bridge_) {
      log("%s: %s", module_name_.c_str(), why.c_str());
      census();
      return false;
    }

    // The scratch block the far pointers point into.
    uint16_t hb = rt.global().alloc(win16::GlobalHeap16::kZeroInit, scratch::kSize);
    if (!hb) throw std::runtime_error("no guest memory for the scratch block");
    scratch_ = uint32_t(hb) << 16;

    // SetADPalette3216(hpal[i], i) for the four AD palettes (ABI.md §3.1).
    AdPalettes pals = load_palettes(layout_, bridge_kind_, file_exists);
    std::string pal_text = pals.pal.empty() ? "none: " + pals.error : pals.source;
    trace("lane", "%s: package %s, module dir %s, engine dir %s, bridge %s%s, AD_SND %s, palettes %s, %s display palette",
          module_name_.c_str(), layout_.packaged ? layout_.package_id.c_str() : "legacy", dir.c_str(), engine.c_str(),
          bridge_name(bridge_kind_), bridge_auto ? "" : " (ADNE16BRIDGE)", ad_snd.c_str(), pal_text.c_str(),
          opts.desktop_palette ? "desktop" : "boot");
    if (pals.pal.empty()) log("%s: no AD palettes (%s); palette requests will fail", module_name_.c_str(), pals.error.c_str());
    auto& gdi = rt.state<win16::Gdi16>();
    for (size_t i = 0; i < pals.pal.size(); i++) {
      uint16_t hpal = gdi.create_palette(pals.pal[i]);
      bridge_->set_palette(hpal, uint16_t(i));
    }

    // Controls: the record defaults, ADCVSET over them.
    std::shared_ptr<loader::ne::Image> img;
    try {
      img = std::make_shared<loader::ne::Image>(loader::ne::Image::from_file(path));
    } catch (const std::exception& e) {
      log("%s: not an NE module: %s", module_name_.c_str(), e.what());
      census();
      return false;
    }
    for (int i = 0; i < 4; i++) {
      int16_t def = 0;
      if (const auto* res = img->find_resource(loader::ResId::of(1000), loader::ResId::of(uint16_t(i + 1)))) {
        def = control_default16(img->resource_data(*res));
      }
      ctrl_[i] = int16_t(ctx.input.control(i, def));
      rt.wr16(scratch_ + scratch::kCtrl + 2u * uint32_t(i), uint16_t(ctrl_[i]));
      trace("lane", "control %d = %d%s", i, ctrl_[i], ctrl_[i] == def ? "" : " (ADCVSET)");
    }

    // LoadADModule3216 → LOADADMODULE16(hwnd, hdc, ctrl4, volume, mute, path, err, errLen, &errId).
    // The module as the install directory holds it (C:\AFTERD~1 is the same
    // folder under another name; the long form is the one AD's INI files held).
    std::string guest = opts.guest_dir + "\\" + win16::upper16(module_name_);
    rt.write_str(scratch_ + scratch::kPath, guest, 260);
    uint16_t r = bridge_->load(hwnd_, hdc_, scratch_ + scratch::kCtrl, volume_, mute_, scratch_ + scratch::kPath,
                               scratch_ + scratch::kError, scratch::kErrorSize, scratch_ + scratch::kErrId);
    trace("lane", "LOADADMODULE16(%s, volume %u, mute %u) -> %u", guest.c_str(), volume_, mute_, r);
    if (!r) {
      uint16_t id = rt.rd16(scratch_ + scratch::kErrId);
      // OLDMOD32 turned these ids into its own strings (ABI.md §3.7).
      std::string why_id = id == 1   ? "cannot load AD_SND.DLL (" + ad_snd + ")"
                           : id == 2 ? "AD_SND.DLL is too old"
                           : id == 3 ? "AD_SND.DLL lacks an entry point"
                                     : error_text();
      log("%s: the module did not load: %s", module_name_.c_str(), why_id.c_str());
      census();
      return false;
    }
    loaded_ = true;
    // The AD 3.x engines (ADXPL40, ADXPL310) hook the keyboard as they load.
    hooked_ = win16::user16_has_keyboard_hook(rt);
    // Long calls (lane.hh): the frame's DRAWFRAMEs run on a fiber of their own.
    if (long_calls_) {
      if (IsThreadAFiber()) {
        host_fiber_ = GetCurrentFiber();
      } else {
        host_fiber_ = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        converted_thread_ = host_fiber_ != nullptr;
      }
      // The host's own 8 MB (the top-level CMakeLists.txt): shims and nested guest
      // calls recurse on it as they do on the main thread.
      if (host_fiber_) guest_fiber_ = CreateFiberEx(0, 8u << 20, FIBER_FLAG_FLOAT_SWITCH, &Ne16Lane::fiber_main, this);
      if (!guest_fiber_) {
        log("%s: no fiber for long calls (error %lu); every DRAWFRAME ends its frame", module_name_.c_str(), GetLastError());
        long_calls_ = false;
      }
    }
    // A synchronous sndPlaySound lasts its sound's duration (lane.hh "Sound"):
    // the frames go on meanwhile, as in a long call.
    if (long_calls_) rt.set_yield_hook([this] { return suspend_frame(); });
    present();
    return true;
  } catch (const GuestError16& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  } catch (const loader::LoaderError& e) {
    log("%s: cannot load: %s", module_name_.c_str(), e.what());
  } catch (const std::exception& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  }
  census();
  return false;
}

void Ne16Lane::send_controls() {
  for (int i = 0; i < 4; i++) rt_->wr16(scratch_ + scratch::kCtrl + 2u * uint32_t(i), uint16_t(ctrl_[i]));
  // SetModuleCtrlValues3216 → SETMODULECTRLVALUES16(volume, mute, ctrl4).
  bridge_->set_controls(volume_, mute_, scratch_ + scratch::kCtrl);
  // The bridge copied them into AD_MODULE.iControlValue (+6), where the module reads them.
  if (tracing("lane")) {
    uint32_t mod = bridge_->ad_module();
    if (mod) {
      trace("lane", "SETMODULECTRLVALUES16(%u, %u, {%d, %d, %d, %d}) -> AD_MODULE controls {%d, %d, %d, %d}", volume_,
            mute_, ctrl_[0], ctrl_[1], ctrl_[2], ctrl_[3], int16_t(rt_->rd16(mod + 6)), int16_t(rt_->rd16(mod + 8)),
            int16_t(rt_->rd16(mod + 10)), int16_t(rt_->rd16(mod + 12)));
    }
  }
}

// Transient content. The host shows the screen as each DRAWFRAME leaves it,
// but a 1996 monitor showed it at every refresh during the call too: a
// module that draws, waits in a CPU delay loop and erases within one
// DRAWFRAME (ZOT's lightning bolts) was seen for a refresh or so, and would
// never be seen here. So the runtime reports every virtual 70 Hz refresh
// (Runtime16::set_scanout_hook); when a DRAWFRAME ends exactly where it
// began but a refresh during it showed something else, the frame presented
// is that refresh — and the real screen goes back before the guest runs
// again. Steps that change the screen are presented as they end, as before.
void Ne16Lane::on_scanout() {
  if (!in_step_) return;
  win32::Display* d = rt_->display();
  if (!d || start_bits_.empty()) return;
  GdiFlush();
  if (memcmp(d->bits(), start_bits_.data(), start_bits_.size()) != 0) {
    latched_bits_.assign(d->bits(), d->bits() + start_bits_.size());
    latched_ = true;
  }
}

void Ne16Lane::settle_screen() {
  if (!showing_latched_) return;
  showing_latched_ = false;
  if (win32::Display* d = rt_->display()) memcpy(d->bits(), start_bits_.data(), start_bits_.size());
}

void Ne16Lane::on_command(const Command& c) {
  if (!rt_ || !loaded_) return;
  sync_input();
  settle_screen();
  if (c.kind == Command::Kind::key || c.kind == Command::Kind::caps || c.kind == Command::Kind::mouse) {
    // An interactive module takes every input line as its own (INTERACTION.md §5.2).
    if (wants_events_ && c.seq > eaten_) eaten_ = c.seq;
    queue_input(c);
    return;
  }
  if (c.kind == Command::Kind::set && c.a >= 0 && c.a < 4) {
    ctrl_[c.a] = int16_t(c.b);
    // Mid-DRAWFRAME (a long call the last frame ended inside) the guest cannot
    // be called: the values go to the module before its next DRAWFRAME.
    if (suspended_) {
      controls_pending_ = true;
      return;
    }
    try {
      send_controls();
    } catch (const std::exception& e) {
      log("%s: SET %d %d: %s", module_name_.c_str(), c.a, c.b, e.what());
    }
  }
}

// ---- input (lane.hh "Input and status") ----

void Ne16Lane::queue_input(const Command& c) {
  PendingInput p;
  p.kind = c.kind;
  p.seq = c.seq;
  if (c.kind == Command::Kind::key) {
    p.vk = uint8_t(c.a & 0xFF);
    p.down = c.b != 0;
    p.was_down = key_down_[p.vk];
    key_down_[p.vk] = p.down;
  } else if (c.kind == Command::Kind::mouse) {
    // Guest coordinates: the output's, scaled up with the guest display (sync_input).
    const int k = guest_scale_;
    const int w = guest_screen_ ? guest_screen_->width() : ctx_->screen.width();
    const int h = guest_screen_ ? guest_screen_->height() : ctx_->screen.height();
    p.x = std::clamp<int32_t>(c.a * k + (k > 1 ? k / 2 : 0), 0, std::max(w - 1, 0));
    p.y = std::clamp<int32_t>(c.b * k + (k > 1 ? k / 2 : 0), 0, std::max(h - 1, 0));
    p.buttons = uint32_t(c.c) & kMouseButtonMask;
    p.prev_buttons = mouse_buttons_;
    p.moved = !mouse_known_ || p.x != mouse_x_ || p.y != mouse_y_;
    mouse_known_ = true;
    mouse_x_ = p.x;
    mouse_y_ = p.y;
    mouse_buttons_ = p.buttons;
  } else {
    return;  // CAPS: the toggle state only (the KEY 20 line before it is the key)
  }
  pending_input_.push_back(p);
}

// At a point the guest can be called (the start of a frame's DRAWFRAME run,
// or where a suspended long call resumes, on_deadline): each KEY
// through the WH_KEYBOARD chain, then — unless a hook consumed it — into the
// saver window's queue; each MOUSE as WM_MOUSEMOVE and button messages.
void Ne16Lane::deliver_input() {
  std::vector<PendingInput> batch;
  batch.swap(pending_input_);
  Runtime16& rt = *rt_;
  const InputState& in = rt.input();
  for (const PendingInput& p : batch) {
    if (p.kind == Command::Kind::key) {
      uint32_t lp = win16::key_lparam(p.vk, p.down, p.was_down);
      if (win16::user16_has_keyboard_hook(rt) && win16::user16_keyboard_hooks(rt, p.vk, lp, p.seq)) {
        if (p.seq > eaten_) eaten_ = p.seq;
        continue;
      }
      bool sys = p.vk == VK_MENU || p.vk == VK_F10;
      uint16_t msg = p.down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
      win16::user16_post_input(rt, msg, p.vk, lp, p.seq);
    } else {
      uint16_t mk = uint16_t(((p.buttons & kMouseLeft) ? MK_LBUTTON : 0) | ((p.buttons & kMouseRight) ? MK_RBUTTON : 0) |
                             ((p.buttons & kMouseMiddle) ? MK_MBUTTON : 0) | (in.keys.test(VK_SHIFT) ? MK_SHIFT : 0) |
                             (in.keys.test(VK_CONTROL) ? MK_CONTROL : 0));
      uint32_t lp = (uint32_t(uint16_t(p.y)) << 16) | uint16_t(p.x);
      if (p.moved) win16::user16_post_input(rt, WM_MOUSEMOVE, mk, lp, p.seq);
      static const struct {
        uint32_t bit;
        uint16_t down, up;
      } kButtons[] = {{kMouseLeft, WM_LBUTTONDOWN, WM_LBUTTONUP},
                      {kMouseRight, WM_RBUTTONDOWN, WM_RBUTTONUP},
                      {kMouseMiddle, WM_MBUTTONDOWN, WM_MBUTTONUP}};
      for (const auto& b : kButtons) {
        if ((p.buttons ^ p.prev_buttons) & b.bit) win16::user16_post_input(rt, (p.buttons & b.bit) ? b.down : b.up, mk, lp, p.seq);
      }
    }
  }
}

// After a step: what the guest consumed from the saver window's queue, whether
// it reads it, whether it asked the saver to close; input nobody took is
// dropped unless a DRAWFRAME that may still take it is suspended.
void Ne16Lane::end_step_input() {
  // Input nobody took: dropped after its step (the 1996 host pumped its queue
  // when the module's call returned, and the saver window had it), unless a
  // DRAWFRAME that may still take it is suspended: one more step, or, while
  // that call reads the saver window's queue itself, until it does (bounded).
  // Lunatic Fringe's game is one such call, polling the queue once per game
  // tick, 2 to 3 frames apart under emulation; the original host could not
  // pump before the call returned, so every key was the game's.
  const bool reader = read_queue_ && frames_ - last_read_frame_ < kReaderSteps;
  const uint32_t keep = !suspended_ ? 0 : reader ? kReaderKeepSteps : 1;
  win16::StepReport16 r = win16::user16_end_step(*rt_, keep);
  if (r.consumed > eaten_) eaten_ = r.consumed;
  if (r.queue_reads != queue_reads_) {
    queue_reads_ = r.queue_reads;
    last_read_frame_ = frames_;
    read_queue_ = true;
  }
  wake_ = wake_ || r.wake;
  queued_seq_ = r.pending;
  hooked_ = win16::user16_has_keyboard_hook(*rt_);
}

LaneStatus Ne16Lane::status() const {
  LaneStatus s;
  s.interactive = wants_events_;
  s.cursor = cursor_;
  s.source = wants_events_ ? kStatusSourceAd3 : 0;
  // Within the last 120 steps it read the saver window's queue, or a keyboard hook is in.
  s.key_filter = hooked_ || (read_queue_ && frames_ - last_read_frame_ < kReaderSteps);
  s.wake = wake_;
  s.eaten = eaten_;
  // Input still in the saver window's queue (kept while a suspended DRAWFRAME
  // may read it) or not yet delivered: the guest has not had its say.
  s.unsettled = queued_seq_;
  for (const PendingInput& p : pending_input_) {
    if (p.seq && (!s.unsettled || p.seq < s.unsettled)) s.unsettled = p.seq;
  }
  return s;
}

// The frame's run of DRAWFRAMEs (lane.hh "Pacing"): until the work budget,
// ADMAXDRAWS calls, or - with long calls on - the frame's deadline. False
// when the module stopped the run.
bool Ne16Lane::draw_run() {
  for (;;) {
    if (controls_pending_) {
      controls_pending_ = false;
      send_controls();
    }
    if (!pending_input_.empty()) deliver_input();
    // The host's message loop ran between DRAWFRAMEs: MM_MCINOTIFY and
    // MM_WOM_* due by now reach their windows (lane.hh "Sound").
    win16::audio16_pump(*rt_);
    // AFTERDAR.SCR 0x401f6f: SetWindowOrgEx(hdc, 0, 0) before every DRAWFRAME.
    if (HDC h = rt_->state<win16::Gdi16>().host_dc(hdc_)) SetWindowOrgEx(h, 0, 0, nullptr);
    mid_call_ = true;
    uint16_t r = bridge_->message(2, scratch_ + scratch::kError, scratch::kErrorSize);
    mid_call_ = false;
    draws_++;
    frame_draws_++;
    int16_t v = int16_t(r);
    if (v != 0) trace("lane", "MODULEMESSAGE16(DRAWFRAME) -> %d", v);
    if (v == 0x0E) {
      wants_events_ = !wants_events_;
    } else if (v == 0x11 || v == 0x12) {
      cursor_ = v == 0x11;
    } else if (v > 0 && v <= 0x12) {
      log("%s: frame %" PRIu64 ": the module stopped (result %d): %s", module_name_.c_str(), frames_, v,
          error_text().c_str());
      return false;
    }
    if (frame_draws_ >= max_draws_ || rt_->work_insns() - frame_w0_ >= frame_budget_) return true;
    if (long_calls_ && rt_->peek_us() >= frame_deadline_) return true;
  }
}

namespace {
// Thrown from the deadline hook into a suspended DRAWFRAME to abandon it (the
// run is closing): call_far's levels put the machine back on the way out.
struct AbandonCall {};
}  // namespace

void CALLBACK Ne16Lane::fiber_main(void* self) { static_cast<Ne16Lane*>(self)->fiber_body(); }

void Ne16Lane::fiber_body() {
  for (;;) {
    try {
      run_result_ = draw_run() ? Run::frame_done : Run::stopped;
    } catch (const AbandonCall&) {
      run_result_ = Run::abandoned;
    } catch (...) {
      fiber_error_ = std::current_exception();
      run_result_ = Run::error;
    }
    mid_call_ = false;
    SwitchToFiber(host_fiber_);
  }
}

// The deadline hook (Runtime16::set_deadline), on the guest fiber at an API
// call inside a DRAWFRAME: the frame period is over, so the frame ends here
// and the call carries on when the next frame begins.
void Ne16Lane::on_deadline() { suspend_frame(); }

// Ends the presented frame inside the DRAWFRAME in progress and returns when
// the next frame resumes it; false (nothing happens) outside a DRAWFRAME on
// the guest fiber. The deadline hook, and a synchronous sndPlaySound waiting
// out its sound (Runtime16::wait_until_us), end frames here.
bool Ne16Lane::suspend_frame() {
  if (!mid_call_ || !guest_fiber_ || GetCurrentFiber() != guest_fiber_) return false;
  suspended_ = true;
  run_result_ = Run::suspended;
  SwitchToFiber(host_fiber_);
  suspended_ = false;
  if (abandon_) throw AbandonCall{};
  // The frame resumes the call here, inside an API call the guest made (as
  // Windows ran keyboard hooks inside GetMessage/PeekMessage): input that
  // arrived meanwhile goes in now — a module whose game loop is one long
  // DRAWFRAME (LUNATIC) reads it before its next frame.
  if (!pending_input_.empty()) deliver_input();
  return true;
}

StepResult Ne16Lane::step() {
  if (!rt_ || !loaded_) return StepResult::failed;
  try {
    trace("lane", "frame %" PRIu64, frames_);
    settle_screen();
    rt_->start_frames();
    win32::Display* disp = rt_->display();
    start_bits_.assign(disp->bits(), disp->bits() + size_t(disp->pitch()) * size_t(disp->height()));
    latched_ = false;
    rt_->update_bios_ticks();
    // One presented frame = as many back-to-back DRAWFRAMEs as the virtual
    // CPU fits into a frame period (lane.hh "Pacing"): AFTERDAR.SCR sent one
    // per pass of its idle loop, so a module that counts calls (LOGO moves its
    // picture every 100th call at Medium) ran at thousands of calls a second.
    // Work counts are deterministic, so so is the number of calls.
    // (The frame period is the core's once the lane runs: ADPACEMS or ours.)
    const uint64_t period = ctx_->clock.step_us();
    frame_budget_ = draw_mips_ * period;
    frame_w0_ = rt_->work_insns();
    frame_draws_ = 0;
    const uint64_t i0 = rt_->instructions(), d0 = draws_, t0 = rt_->peek_us();
    const bool resumed = suspended_;
    in_step_ = true;
    if (long_calls_) {
      // Long calls (lane.hh): the frame ends at the deadline even inside a
      // DRAWFRAME. Headless, the deadline is the frame grid's next line;
      // streamed, 90% of a period of wall time from now (the rest is the
      // host's, to present).
      frame_deadline_ = rt_->modeled_time() ? ctx_->clock.now_us() + period : rt_->peek_us() + period * 9 / 10;
      rt_->set_deadline(frame_deadline_, [this] { on_deadline(); });
      run_result_ = Run::none;
      SwitchToFiber(guest_fiber_);
      rt_->clear_deadline();
    } else {
      try {
        run_result_ = draw_run() ? Run::frame_done : Run::stopped;
      } catch (...) {
        in_step_ = false;
        throw;
      }
    }
    in_step_ = false;
    if (run_result_ == Run::error) {
      std::exception_ptr e = fiber_error_;
      fiber_error_ = nullptr;
      std::rethrow_exception(e);
    }
    if (run_result_ != Run::frame_done && run_result_ != Run::suspended) {
      census();
      return StepResult::failed;
    }
    trace("pace", "frame %" PRIu64 ": %" PRIu64 " DRAWFRAME(s)%s%s, work %" PRIu64 " of %" PRIu64 " (%" PRIu64
          " instructions), time %" PRIu64 " us (grid %" PRIu64 ", began %" PRIu64 ")",
          frames_, draws_ - d0, resumed ? ", resumed" : "", suspended_ ? ", ended inside one" : "",
          rt_->work_insns() - frame_w0_, frame_budget_, rt_->instructions() - i0, rt_->peek_us(), ctx_->clock.now_us(), t0);
    if (suspended_) long_frames_++;
    rt_->settle_time();
    // Real GDI batches drawing per thread: finish it before the host reads the pixels.
    GdiFlush();
    if (latched_ && memcmp(disp->bits(), start_bits_.data(), start_bits_.size()) == 0) {
      // Nothing changed net, but a refresh showed something (see on_scanout).
      memcpy(disp->bits(), latched_bits_.data(), latched_bits_.size());
      showing_latched_ = true;
      transient_frames_++;
      trace("lane", "frame %" PRIu64 ": presenting content a refresh showed during DRAWFRAME", frames_);
    }
    frames_++;
    end_step_input();
    present();
    ctx_->screen.mark_dirty();
    // The engine renders up to the guest's own time (lane.hh "Sound"): it runs
    // ahead of the core clock run_host advances with (by the frame's work
    // headless, by the modeled init time streamed), and the engine takes an
    // earlier time as the latest it has seen — so without this a streamed run
    // would render only at the guest's audio calls.
    if (ctx_->audio && ctx_->audio->enabled()) ctx_->audio->advance(rt_->peek_us());
    return StepResult::ok;
  } catch (const GuestError16& e) {
    log("%s: frame %" PRIu64 ": %s", module_name_.c_str(), frames_, e.what());
    rt_->log_state("guest state");
  } catch (const std::exception& e) {
    log("%s: frame %" PRIu64 ": %s", module_name_.c_str(), frames_, e.what());
    rt_->log_state("guest state");
  }
  census();
  return StepResult::failed;
}

// Before the guest can be called on the host fiber again (UNLOAD at close): a
// DRAWFRAME the last frame ended inside is abandoned - the deadline hook
// throws, and every call level restores the machine on the way out.
void Ne16Lane::abandon_long_call() {
  if (!suspended_ || !guest_fiber_) return;
  abandon_ = true;
  SwitchToFiber(guest_fiber_);
  abandon_ = false;
  trace("lane", "%s: abandoned the DRAWFRAME in progress", module_name_.c_str());
}

void Ne16Lane::free_fibers() {
  if (guest_fiber_) {
    DeleteFiber(guest_fiber_);
    guest_fiber_ = nullptr;
  }
  if (converted_thread_) {
    ConvertFiberToThread();
    converted_thread_ = false;
  }
  host_fiber_ = nullptr;
}

void Ne16Lane::shutdown() {
  if (!rt_) return;
  settle_screen();
  if (transient_frames_) trace("lane", "%" PRIu64 " frame(s) presented transient content", transient_frames_);
  trace("lane", "%s: %" PRIu64 " DRAWFRAME calls over %" PRIu64 " frames (%" PRIu64 " ended inside one)",
        module_name_.c_str(), draws_, frames_, long_frames_);
  try {
    abandon_long_call();
    if (loaded_ && bridge_ && !suspended_) bridge_->unload();
    loaded_ = false;
    if (bridge_ && !suspended_) bridge_->close();
    if (!suspended_) rt_->modules().free_all();
    // What the guest left playing or open (win16/sound16.hh).
    win16::audio16_close(*rt_);
  } catch (const std::exception& e) {
    log("%s: while closing: %s", module_name_.c_str(), e.what());
  }
  census();
  bridge_.reset();
  rt_.reset();
  free_fibers();
}

int Ne16Lane::auto_guest_scale(int w, int h) {
  if (w <= 0 || h <= 0) return 1;
  int k = 1;
  while (k < 8 && (int64_t(w) * k < 640 || int64_t(h) * k < 480)) k++;
  return k;
}

// The guest sees the output's mouse scaled up to its own display.
void Ne16Lane::sync_input() {
  if (!guest_screen_ || !ctx_) return;
  guest_input_ = ctx_->input;
  const int k = guest_scale_;
  guest_input_.mouse_x = std::clamp<int32_t>(ctx_->input.mouse_x * k + k / 2, 0, guest_screen_->width() - 1);
  guest_input_.mouse_y = std::clamp<int32_t>(ctx_->input.mouse_y * k + k / 2, 0, guest_screen_->height() - 1);
}

// Small screens (lane.hh): the guest display averaged k×k → 1 into the output,
// each average matched to the nearest hardware palette entry (squared RGB
// distance, lowest index on ties) at 6 bits per channel, the VGA DAC's
// precision. A pure function of the guest's pixels and palette.
void Ne16Lane::present() {
  if (!guest_screen_ || !ctx_) return;
  GdiFlush();  // real GDI batches drawing per thread
  Screen& out = ctx_->screen;
  const Screen& in = *guest_screen_;
  const auto& pal = in.palette();
  if (near_cache_.empty() || memcmp(pal.data(), near_pal_.data(), sizeof(near_pal_)) != 0) {
    near_pal_ = pal;
    if (near_cache_.empty()) near_cache_.assign(size_t(1) << 18, 0);
    if (++near_gen_ >= (1u << 24)) {
      std::fill(near_cache_.begin(), near_cache_.end(), 0u);
      near_gen_ = 1;
    }
  }
  if (memcmp(out.palette().data(), pal.data(), sizeof(near_pal_)) != 0) out.set_entries(0, 256, pal.data());
  const int k = guest_scale_, n = k * k;
  const int w = out.width(), h = out.height();
  for (int y = 0; y < h; y++) {
    uint8_t* dst = out.row(y);
    for (int x = 0; x < w; x++) {
      int r = 0, g = 0, b = 0;
      const uint8_t first = in.row(y * k)[size_t(x) * size_t(k)];
      bool uniform = true;
      for (int j = 0; j < k; j++) {
        const uint8_t* src = in.row(y * k + j) + size_t(x) * size_t(k);
        for (int i = 0; i < k; i++) {
          const RGBQUAD& q = pal[src[i]];
          r += q.rgbRed;
          g += q.rgbGreen;
          b += q.rgbBlue;
          uniform &= src[i] == first;
        }
      }
      if (uniform) {  // one index: itself, exactly
        dst[x] = first;
        continue;
      }
      r = (r + n / 2) / n;
      g = (g + n / 2) / n;
      b = (b + n / 2) / n;
      uint32_t key = (uint32_t(r >> 2) << 12) | (uint32_t(g >> 2) << 6) | uint32_t(b >> 2);
      uint32_t& slot = near_cache_[key];
      if ((slot >> 8) != near_gen_) {
        // The cell's centre colour, matched against every hardware entry.
        int cr = int(key >> 12) * 4 + 2, cg = int((key >> 6) & 63) * 4 + 2, cb = int(key & 63) * 4 + 2;
        int best = 0, best_d = INT32_MAX;
        for (int e = 0; e < 256; e++) {
          int dr = pal[size_t(e)].rgbRed - cr, dg = pal[size_t(e)].rgbGreen - cg, db = pal[size_t(e)].rgbBlue - cb;
          int dd = dr * dr + dg * dg + db * db;
          if (dd < best_d) best_d = dd, best = e;
        }
        slot = (near_gen_ << 8) | uint32_t(best);
      }
      dst[x] = uint8_t(slot & 0xFF);
    }
  }
}

// Configure mode (lane.hh "Configure"): the module's button handler, as
// AFTERDAR.SCR's property page ran it through OLDMOD32's ButtonPushed3216 —
// BUTTONPUSHED16 of the real OLDMOD16, or the native bridge's same sequence —
// with the module's dialogs real (win16/dialogs16.hh) and its disk
// persistent (ADSTATE, or the --configure default).
ConfigureResult Ne16Lane::configure(const std::string& module_path, LaneContext& ctx, const ConfigureRequest& req,
                                    std::string* json_out) {
  ctx_ = &ctx;
  const Env& env = ctx.env;
  std::string path = full_path(module_path);
  module_name_ = file_of(path);
  layout_ = resolve_layout(path, env.win_assets_dir(), file_exists);
  auto finish = [&](ConfigureResult r, int shown, const std::string& message, const std::vector<std::string>& written) {
    if (json_out) *json_out = configure_json(r, shown, message, written);
    log("%s: configure button %d: %s (%d shown)%s%s", module_name_.c_str(), req.slot,
        r == ConfigureResult::shown ? "shown" : r == ConfigureResult::nothing ? "nothing shown" : "failed", shown,
        message.empty() ? "" : ": ", message.c_str());
    return r;
  };
  // The control record of the slot must be a button (kind 4, ABI.md §2.10.2).
  std::shared_ptr<loader::ne::Image> img;
  try {
    img = std::make_shared<loader::ne::Image>(loader::ne::Image::from_file(path));
  } catch (const std::exception& e) {
    return finish(ConfigureResult::failed, 0, std::string("not an NE module: ") + e.what(), {});
  }
  const auto* rec = req.slot >= 0 ? img->find_resource(loader::ResId::of(1000), loader::ResId::of(uint16_t(req.slot + 1)))
                                  : nullptr;
  if (!rec || u16(img->resource_data(*rec), 0) != 4) {
    return finish(ConfigureResult::failed, 0, "control " + std::to_string(req.slot) + " is not a button", {});
  }
  bool bridge_auto = true;
  BridgeKind forced = BridgeKind::oldmod16;
  if (const std::string* b = env.get("ADNE16BRIDGE")) {
    if (!parse_bridge_choice(*b, &bridge_auto, &forced)) bridge_auto = true;
  }
  bridge_kind_ = bridge_auto ? choose_bridge(layout_, file_exists) : forced;

  win32::ConfigScript script;
  std::string script_error;
  if (!script.load_env(env, &script_error)) return finish(ConfigureResult::failed, 0, script_error, {});
  win16::Configure16 cfg;
  cfg.script = &script;
  cfg.owner = req.owner && IsWindow(reinterpret_cast<HWND>(uintptr_t(req.owner))) ? reinterpret_cast<HWND>(uintptr_t(req.owner))
                                                                                  : nullptr;
  if (req.owner && !cfg.owner) log("%s: --owner 0x%llx is not a window; the dialogs have no owner", module_name_.c_str(),
                                   (unsigned long long)req.owner);

  win16::Runtime16Options opts;
  opts.arena_size = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADHEAPMB", 64), 16, 512) << 20);
  // No budget: the user may keep the dialog open as long as they like.
  opts.call_budget = UINT64_MAX / 2;
  opts.desktop_palette = layout_.packaged && choose_bridge(layout_, file_exists) == BridgeKind::native;
  if (env.get("ADDESKTOPPAL")) opts.desktop_palette = env.flag("ADDESKTOPPAL");
  ctx.clock.set_read_step_us(0);
  std::vector<std::string> written;
  try {
    rt_ = std::make_unique<Runtime16>(opts, ctx.clock, &ctx.input);
    Runtime16& rt = *rt_;
    win16::register_all16(rt);
    mount_disk(rt, env, path, layout_);
    rt.attach_display(ctx.screen);
    // Dialogs run on the wall clock (a module's timers tick while the user
    // looks); the core's realtime clock starts with its first frame.
    ctx.clock.begin_frame();
    rt.start_frames();
    win16::enable_real_dialogs16(rt, &cfg);
    uint16_t owner16 = win16::real_hwnd16(rt, cfg.owner);

    std::string ad_snd = layout_.engine_dir + "\\AD_SND.DLL", why;
    if (bridge_kind_ == BridgeKind::oldmod16) {
      if (!same_dir(layout_.module_dir, layout_.engine_dir) && file_exists(layout_.module_dir + "\\AD_SND.DLL") &&
          file_exists(ad_snd)) {
        uint16_t e = 0;
        rt.modules().load_host(ad_snd, &e);
      }
      bridge_ = open_oldmod16_bridge(rt, layout_.engine_dir + "\\OLDMOD16.DLL", &why);
    } else {
      bridge_ = open_native_bridge(rt, opts.system_dir + "\\AD_SND.DLL", &why);
    }
    if (!bridge_) {
      rt_.reset();
      return finish(ConfigureResult::failed, 0, why, {});
    }
    uint16_t hb = rt.global().alloc(win16::GlobalHeap16::kZeroInit, scratch::kSize);
    if (!hb) throw std::runtime_error("no guest memory for the scratch block");
    scratch_ = uint32_t(hb) << 16;
    for (int i = 0; i < 4; i++) {
      int16_t def = 0;
      if (const auto* r = img->find_resource(loader::ResId::of(1000), loader::ResId::of(uint16_t(i + 1)))) {
        def = control_default16(img->resource_data(*r));
      }
      ctrl_[i] = int16_t(ctx.input.control(i, def));
      rt.wr16(scratch_ + scratch::kCtrl + 2u * uint32_t(i), uint16_t(ctrl_[i]));
    }
    std::string guest = opts.guest_dir + "\\" + win16::upper16(module_name_);
    rt.write_str(scratch_ + scratch::kPath, guest, 260);
    trace("lane", "%s: BUTTONPUSHED16(%s, owner %04X, %d) through the %s bridge", module_name_.c_str(), guest.c_str(),
          owner16, req.slot, bridge_name(bridge_kind_));
    uint16_t r = bridge_->button(scratch_ + scratch::kPath, owner16, uint16_t(req.slot), scratch_ + scratch::kCtrl,
                                 scratch_ + scratch::kError, scratch::kErrorSize, scratch_ + scratch::kErrId);
    uint16_t err_id = rt.rd16(scratch_ + scratch::kErrId);
    std::string err_text = rt.read_str(scratch_ + scratch::kError, scratch::kErrorSize);
    trace("lane", "BUTTONPUSHED16 -> %u (errId %u, error \"%s\")", r, err_id, err_text.c_str());
    bridge_->close();
    rt.modules().free_all();
    written = rt.vfs().written();
    bridge_.reset();
    rt_.reset();
    if (err_id) {
      return finish(ConfigureResult::failed, cfg.shown,
                    err_id == 1 ? "cannot load AD_SND.DLL" : err_id == 2 ? "AD_SND.DLL is too old" : "AD_SND.DLL lacks an entry point",
                    written);
    }
    std::string message = err_text;
    for (const std::string& n : cfg.notes) message += (message.empty() ? "" : "; ") + n;
    if (cfg.failed) return finish(ConfigureResult::failed, cfg.shown, message.empty() ? "a dialog failed" : message, written);
    return finish(cfg.shown ? ConfigureResult::shown : ConfigureResult::nothing, cfg.shown, message, written);
  } catch (const GuestError16& e) {
    log("%s: configure: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
    if (rt_) written = rt_->vfs().written();
    std::string what = e.what();
    bridge_.reset();
    rt_.reset();
    return finish(ConfigureResult::failed, cfg.shown, what, written);
  } catch (const std::exception& e) {
    if (rt_) written = rt_->vfs().written();
    std::string what = e.what();
    bridge_.reset();
    rt_.reset();
    return finish(ConfigureResult::failed, cfg.shown, what, written);
  }
}

void Ne16Lane::census() {
  if (!rt_ || census_done_) return;
  census_done_ = true;
  rt_->shims().print_census(module_name_);
}

}  // namespace adw::ne16

namespace adw {
std::unique_ptr<Lane> make_ne16_lane() { return std::make_unique<ne16::Ne16Lane>(); }
}  // namespace adw
