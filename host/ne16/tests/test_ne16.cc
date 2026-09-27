// adw_lane_ne16 tests.
//
//   adw_ne16_tests                        control-record defaults (synthetic records); the
//                                         package rule, bridge choice and palette sources
//                                         (package.hh); the native AD3 bridge (bridge.hh)
//                                         driving a host-implemented AD_SND and module
//   adw_ne16_tests --assets <adhostwin>   runs Classic modules headless through
//                                         adhostwin (exit 77 when the assets are absent):
//                                         each must load through OLDMOD16 and draw frames
//                                         without the host failing, and two runs must
//                                         produce the same FBHASH stream; the bridge
//                                         oracle (native vs oldmod16, ADMIPS=0) on a few;
//                                         sound captured headless (music gates and
//                                         MM_MCINOTIFY, MS-ADPCM, synchronous sounds)
//   adw_ne16_tests --pkg <adhostwin>      the package roots of PACKAGES.md §4.4 under
//                                         AD_NE16_PKGROOTS (exit 77 when unset/absent):
//                                         one module per package, standalone
#include <windows.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "adw/core/clock.h"
#include "adw/core/screen.h"
#include "test_paths.h"
#include "ne16/bridge.hh"
#include "ne16/lane.hh"
#include "ne16/package.hh"
#include "win16/gdi16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"

using namespace adw;

namespace {

int failures = 0, checks = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    checks++;                                     \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

void put16(std::string& s, size_t at, uint16_t v) {
  if (s.size() < at + 2) s.resize(at + 2, '\0');
  s[at] = char(v);
  s[at + 1] = char(v >> 8);
}

int run_unit() {
  // Kind 1: stops 25/50/75/100 (no 0 → one is prepended); default 60 → 50.
  std::string r1(0x20, '\0');
  put16(r1, 0, 1);
  put16(r1, 0x16, 4);
  put16(r1, 0x18, 60);
  r1.resize(0x20 + 4 * 16, '\0');
  for (int i = 0; i < 4; i++) put16(r1, 0x20 + 64 + 2 * i, uint16_t(25 * (i + 1)));
  CHECK(ne16::control_default16(r1) == 50, "string slider default stop (%d)", ne16::control_default16(r1));
  put16(r1, 0x18, 10);
  CHECK(ne16::control_default16(r1) == 0, "below the first stop: the prepended 0");
  // Kind 2: numeric 1..9, default 22 → 9 (Hard Rain's "# of Drops").
  std::string r2(0x34, '\0');
  put16(r2, 0, 2);
  put16(r2, 0x18, 22);
  put16(r2, 0x30, 1);
  put16(r2, 0x32, 9);
  CHECK(ne16::control_default16(r2) == 9, "numeric default clamped");
  // Kind 3: popup of 3, default 7 → 2; kind 5 checkbox.
  std::string r3(0x20, '\0');
  put16(r3, 0, 3);
  put16(r3, 0x16, 3);
  put16(r3, 0x18, 7);
  CHECK(ne16::control_default16(r3) == 2, "popup default clamped");
  std::string r5(0x20, '\0');
  put16(r5, 0, 5);
  put16(r5, 0x18, 9);
  CHECK(ne16::control_default16(r5) == 1, "checkbox default");
  // Small screens (lane.hh): the smallest whole factor that reaches 640x480.
  CHECK(ne16::Ne16Lane::auto_guest_scale(640, 480) == 1, "640x480: no guest scaling");
  CHECK(ne16::Ne16Lane::auto_guest_scale(856, 480) == 1, "856x480: no guest scaling");
  CHECK(ne16::Ne16Lane::auto_guest_scale(320, 240) == 2, "320x240: x2");
  CHECK(ne16::Ne16Lane::auto_guest_scale(640, 400) == 2, "640x400: x2 (height)");
  CHECK(ne16::Ne16Lane::auto_guest_scale(152, 112) == 5, "152x112: x5");
  CHECK(ne16::Ne16Lane::auto_guest_scale(1, 1) == 8, "1x1: at most x8");
  return 0;
}

// ---- the package rule (PACKAGES.md §7.1/§7.3) ---------------------------------------------------------------

void test_layout() {
  using ne16::BridgeKind;
  std::map<std::string, bool> files;
  auto exists = [&](const std::string& p) {
    std::string u = p;
    for (char& ch : u) ch = char(toupper(uint8_t(ch)));
    return files.count(u) != 0;
  };
  auto add = [&](const std::string& p) {
    std::string u = p;
    for (char& ch : u) ch = char(toupper(uint8_t(ch)));
    files[u] = true;
  };
  const std::string win = "C:\\A\\win";
  // A packaged module: its package root is the parent of its folder, whose parent is "packages".
  ne16::Ne16Layout p = ne16::resolve_layout(win + "\\packages\\ad32\\AD32\\GUTS.AD", win, exists);
  CHECK(p.packaged && p.package_id == "ad32", "packaged (%d, %s)", p.packaged, p.package_id.c_str());
  CHECK(p.engine_dir == win + "\\packages\\ad32\\ENGINE", "engine dir %s", p.engine_dir.c_str());
  CHECK(p.search_dirs.size() == 2 && p.search_dirs[0] == win + "\\packages\\ad32\\AD32" &&
            p.search_dirs[1] == p.engine_dir,
        "packaged search: module dir, engine dir");
  CHECK(ne16::resolve_layout("D:\\x\\PACKAGES\\tt\\TWISTED\\CHAM.AD", win, exists).packaged,
        "\"packages\" matches in any case, anywhere");
  // No OLDMOD16 in the engine dir: the native bridge; with it, OLDMOD16.
  CHECK(ne16::choose_bridge(p, exists) == BridgeKind::native, "ad32: native bridge");
  add(win + "\\packages\\ad10\\ENGINE\\OLDMOD16.DLL");
  CHECK(ne16::choose_bridge(ne16::resolve_layout(win + "\\packages\\ad10\\AD10TH\\CHAM.AD", win, exists), exists) ==
            BridgeKind::oldmod16,
        "ad10: OLDMOD16");
  // Legacy: Deluxe's FILES tree, exactly the lane's original rule.
  add(win + "\\FILES\\ENGINE\\OLDMOD16.DLL");
  ne16::Ne16Layout l = ne16::resolve_layout(win + "\\FILES\\CLASSIC\\TOAST3.AD", win, exists);
  CHECK(!l.packaged && l.engine_dir == win + "\\FILES\\ENGINE", "legacy engine dir %s", l.engine_dir.c_str());
  CHECK(l.search_dirs.size() == 3 && l.search_dirs[1] == win + "\\FILES\\CLASSIC" && l.search_dirs[2] == l.engine_dir,
        "legacy search: module dir, CLASSIC, ENGINE");
  CHECK(ne16::choose_bridge(l, exists) == BridgeKind::oldmod16, "Deluxe: OLDMOD16");
  // A lone module with OLDMOD16 beside it, and no Deluxe engine.
  files.clear();
  add("E:\\lone\\OLDMOD16.DLL");
  ne16::Ne16Layout lone = ne16::resolve_layout("E:\\lone\\X.AD", win, exists);
  CHECK(!lone.packaged && lone.engine_dir == "E:\\lone", "lone module: its own folder is the engine dir");
  // "packages" must be the grandparent of the module dir, not any ancestor.
  CHECK(!ne16::resolve_layout(win + "\\packages\\ad32\\AD32\\SUB\\X.AD", win, exists).packaged, "a deeper folder is legacy");
  // ADNE16BRIDGE.
  bool is_auto = false;
  BridgeKind k = BridgeKind::native;
  CHECK(ne16::parse_bridge_choice("", &is_auto, &k) && is_auto, "empty = auto");
  CHECK(ne16::parse_bridge_choice("Native", &is_auto, &k) && !is_auto && k == BridgeKind::native, "native");
  CHECK(ne16::parse_bridge_choice("OLDMOD16", &is_auto, &k) && !is_auto && k == BridgeKind::oldmod16, "oldmod16");
  CHECK(!ne16::parse_bridge_choice("thunk", &is_auto, &k), "anything else is refused");
}

// A minimal NE image: no segments, the given integer-typed resources.
struct NeRes {
  uint16_t type, id;
  std::string data;
};
std::string ne_image(const std::string& module, const std::vector<NeRes>& res) {
  std::string f(0x40, '\0');
  f[0] = 'M';
  f[1] = 'Z';
  put16(f, 0x3C, 0x40);
  std::string h(0x40, '\0');
  h[0] = 'N';
  h[1] = 'E';
  // Resource table: shift 4, one type block per resource (a type may repeat), then the names.
  std::string rt;
  put16(rt, 0, 4);
  size_t data_at = 0;  // filled below
  std::vector<size_t> offs;
  for (size_t i = 0; i < res.size(); i++) {
    size_t p = rt.size();
    rt.resize(p + 8 + 12, '\0');
    put16(rt, p, uint16_t(0x8000 | res[i].type));
    put16(rt, p + 2, 1);
    offs.push_back(p + 8);
    put16(rt, p + 8 + 4, 0x30);  // MOVEABLE|PURE
    put16(rt, p + 8 + 6, uint16_t(0x8000 | res[i].id));
  }
  rt.resize(rt.size() + 2, '\0');  // end of types
  std::string resident;
  resident += char(module.size());
  resident += module;
  resident += std::string(3, '\0');  // ordinal 0, then the terminator
  std::string imp(1, '\0'), entry(2, '\0');
  uint16_t off = 0x40;
  uint16_t rt_off = off;
  off += uint16_t(rt.size());
  uint16_t resident_off = off;
  off += uint16_t(resident.size());
  uint16_t modref_off = off, imp_off = off;
  off += uint16_t(imp.size());
  uint16_t entry_off = off;
  off += uint16_t(entry.size());
  put16(h, 0x04, entry_off);
  put16(h, 0x06, uint16_t(entry.size()));
  put16(h, 0x0C, 0x8001);
  put16(h, 0x22, 0x40);  // segment table (empty)
  put16(h, 0x24, rt_off);
  put16(h, 0x26, resident_off);
  put16(h, 0x28, modref_off);
  put16(h, 0x2A, imp_off);
  put16(h, 0x32, 4);
  h[0x36] = 2;
  put16(h, 0x3E, 0x030A);
  std::string all = f + h + rt + resident + imp + entry;
  data_at = (all.size() + 15) & ~size_t(15);
  for (size_t i = 0; i < res.size(); i++) {
    all.resize(data_at, '\0');
    size_t len = (res[i].data.size() + 15) & ~size_t(15);
    put16(all, 0x80 + offs[i], uint16_t(data_at >> 4));
    put16(all, 0x80 + offs[i] + 2, uint16_t(len >> 4));
    all += res[i].data;
    data_at += len;
  }
  all.resize(data_at, '\0');
  return all;
}

// A LOGPALETTE resource of n entries whose red channel is `tag`, green the index.
std::string logpal_res(uint8_t tag, uint16_t n) {
  std::string d;
  put16(d, 0, 0x300);
  put16(d, 2, n);
  for (uint16_t i = 0; i < n; i++) d += std::string{char(tag), char(i), 0, char(PC_RESERVED)};
  return d;
}

std::string temp_dir(const char* tag) {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string d = std::string(base) + "adw_ne16_" + tag + "_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(d.c_str(), nullptr);
  return d;
}

void write_file(const std::string& path, const std::string& bytes) {
  FILE* fh = fopen(path.c_str(), "wb");
  fwrite(bytes.data(), 1, bytes.size(), fh);
  fclose(fh);
}

void test_palettes() {
  std::string dir = temp_dir("pal");
  std::string adtask = dir + "\\ADTASK.DLL";
  write_file(adtask, ne_image("ADTASK", {{5000, 1, logpal_res(1, 235)},
                                         {5000, 2, logpal_res(2, 235)},
                                         {5000, 3, logpal_res(3, 235)},
                                         {5000, 4, logpal_res(4, 235)},
                                         {5000, 5, logpal_res(5, 244)}}));
  ne16::AdPalettes p = ne16::palettes_from_adtask(adtask);
  CHECK(p.pal.size() == 4, "four palettes from ADTASK (%s)", p.error.c_str());
  if (p.pal.size() == 4) {
    // SETADPALETTE16 index i ← 5000/{3,1,4,2}[i], so request 10+k selects 5000/(k+1).
    CHECK(p.pal[0][0].peRed == 3 && p.pal[1][0].peRed == 1 && p.pal[2][0].peRed == 4 && p.pal[3][0].peRed == 2,
          "hpal[0..3] = 5000/3, 5000/1, 5000/4, 5000/2 (%u %u %u %u)", p.pal[0][0].peRed, p.pal[1][0].peRed,
          p.pal[2][0].peRed, p.pal[3][0].peRed);
    CHECK(p.pal[0].size() == 235 && p.pal[0][200].peGreen == 200 && p.pal[0][200].peFlags == PC_RESERVED,
          "235 PC_RESERVED entries, as the resource has them");
  }
  // The native bridge's source: ADTASK in the engine dir; else AFTERDAR.SCR.
  auto exists = [](const std::string& f) {
    DWORD a = GetFileAttributesA(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
  };
  ne16::Ne16Layout l;
  l.engine_dir = dir;
  ne16::AdPalettes n = ne16::load_palettes(l, ne16::BridgeKind::native, exists);
  CHECK(n.pal.size() == 4 && n.source.find("ADTASK.DLL 5000/1..4") != std::string::npos, "native: %s",
        n.source.c_str());
  ne16::AdPalettes o = ne16::load_palettes(l, ne16::BridgeKind::oldmod16, exists);
  CHECK(o.pal.empty() && o.error.find("AFTERDAR.SCR") != std::string::npos, "OLDMOD16 wants AFTERDAR.SCR (%s)",
        o.error.c_str());
  // A broken ADTASK (4 missing) gives no palettes, and says so.
  write_file(adtask, ne_image("ADTASK", {{5000, 1, logpal_res(1, 235)}, {5000, 2, logpal_res(2, 235)}}));
  ne16::AdPalettes b = ne16::palettes_from_adtask(adtask);
  CHECK(b.pal.empty() && b.error.find("5000/") != std::string::npos, "missing resource: %s", b.error.c_str());
  DeleteFileA(adtask.c_str());
  RemoveDirectoryA(dir.c_str());
}

// ---- the native bridge ---------------------------------------------------------------------------------------
//
// AD_SND and the module are host-implemented system "modules" (shims), so a
// test sees every call the bridge makes and scripts every result.
struct BridgeRig {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  win16::Runtime16 rt{win16::Runtime16Options{}, clock};
  Screen screen{64, 48};
  std::vector<std::string> calls;
  std::map<uint16_t, std::vector<uint16_t>> results;  // MODULE(msg) results, in turn
  uint16_t hsys = 0;
  uint32_t module_text = 0;  // a string the module points AD_SYSTEM+0x22 at, when set
  bool want_snd = true;

  BridgeRig() {
    clock.set_read_step_us(5);
    win16::register_all16(rt);
    rt.attach_display(screen);
    using win16::Call16;
    using win16::Conv16;
    auto& r = rt.shims();
    auto snd = [&](const char* mod, uint16_t ord, const char* name, int bytes) {
      r.add(mod, ord, name, Conv16::pascal_, true, bytes, [this, name, bytes](Call16& c) {
        std::string s = name;
        if (bytes == 2) s += "(" + std::to_string(c.w()) + ")";
        if (s == "ADWGETSYSTEMVOLUMES") c.rt.wr16(c.ptr(), 0x1234);
        calls.push_back(s);
        c.ret(0);
      });
    };
    for (const char* m : {"FAKESND", "HALFSND"}) {
      snd(m, 11, "ADWSOUNDINIT", 6);
      snd(m, 9, "ADWSOUNDCLEANUP", 0);
      snd(m, 101, "ADWGETSYSTEMVOLUMES", 4);
      snd(m, 102, "ADWSETSYSTEMVOLUMES", 2);
      snd(m, 29, "ADWSETVOLUME", 2);
      snd(m, 8, "ADWSETSOUNDMUTE", 2);
    }
    snd("FAKESND", 26, "ADWSTOPSOUND", 0);  // HALFSND lacks it
    r.add("FAKEMOD", 1, "MODULE", Conv16::pascal_, true, 6, [this](Call16& c) {
      uint16_t msg = c.w();
      c.w();
      hsys = c.w();
      calls.push_back("MODULE(" + std::to_string(msg) + ")");
      uint32_t sys = c.rt.global().lock(hsys);
      if (msg == 5 && want_snd) c.rt.wr16(c.rt.global().lock(c.rt.rd16(sys + 0x20)) + 0x1E, 1);  // bWantSnd
      if (module_text) c.rt.wr32(sys + 0x22, module_text);
      uint16_t v = 0;
      auto& q = results[msg];
      if (!q.empty()) {
        v = q.front();
        q.erase(q.begin());
      }
      c.ret(v);
    });
  }
  std::string joined() {
    std::string s;
    for (auto& x : calls) s += (s.empty() ? "" : " ") + x;
    return s;
  }
};

void test_native_bridge() {
  BridgeRig g;
  win16::Runtime16& rt = g.rt;
  uint16_t hwnd = win16::user16_saver_window(rt);
  uint16_t hdc = win16::gdi16_screen_dc(rt, hwnd);
  std::string why;
  auto b = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  CHECK(b != nullptr, "the bridge attaches (%s)", why.c_str());
  if (!b) return;
  // The lane's scratch block: ctrl4, errId, path, err.
  uint16_t hs = rt.global().alloc(win16::GlobalHeap16::kZeroInit, ne16::scratch::kSize);
  uint32_t s = uint32_t(hs) << 16;
  const int16_t ctrl[4] = {7, -1, 30, 1};
  for (uint32_t i = 0; i < 4; i++) rt.wr16(s + ne16::scratch::kCtrl + 2 * i, uint16_t(ctrl[i]));
  rt.write_str(s + ne16::scratch::kPath, "C:\\AFTERDRK\\FAKEMOD.AD", 260);
  // Four palettes: entry 0's red channel tells them apart.
  auto& gdi = rt.state<win16::Gdi16>();
  uint16_t hpal[4];
  for (uint16_t i = 0; i < 4; i++) {
    std::vector<PALETTEENTRY> pe(235, PALETTEENTRY{uint8_t(100 + i), 0, 0, PC_RESERVED});
    hpal[i] = gdi.create_palette(pe);
    b->set_palette(hpal[i], i);
  }
  // INITIALIZE asks for palette 12 (→ hpal[0]); BLANK succeeds.
  g.results[0] = {12};
  uint16_t ok = b->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                        s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok == 1, "LOADADMODULE16 -> %u", ok);
  CHECK(g.joined() ==
            "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) ADWSETSOUNDMUTE(1) ADWSETVOLUME(40) MODULE(12) MODULE(0) "
            "MODULE(1)",
        "the OLDMOD16 load sequence: %s", g.joined().c_str());
  uint32_t sys = rt.global().lock(g.hsys);
  uint32_t mod = b->ad_module();
  CHECK(rt.rd16(sys) == 2 && rt.rd16(sys + 0x14) == 300 && rt.rd16(sys + 0x06) == 64 && rt.rd16(sys + 0x08) == 48,
        "AD_SYSTEM: 2, version 300, the screen size");
  CHECK(rt.rd16(sys + 0x02) == 4 && rt.rd16(sys + 0x04) == 1 && rt.rd16(sys + 0x0A) == 8 && rt.rd16(sys + 0x2A) == 1,
        "AD_SYSTEM: a 486 with an FPU, 8 bits per pixel, a palette device");
  std::string sig;
  for (uint32_t i = 0; i < 8; i++) sig += char(rt.rd16(sys + 0x2C + 2 * i));
  CHECK(sig == "BUTTHEAD", "AD_SYSTEM signature %s", sig.c_str());
  CHECK(rt.global().lock(rt.rd16(sys + 0x20)) == mod && rt.rd32(sys + 0x22) == s + ne16::scratch::kError,
        "AD_SYSTEM: hADModule, the error buffer");
  CHECK(int16_t(rt.rd16(mod + 6)) == 7 && int16_t(rt.rd16(mod + 8)) == -1 && rt.rd16(mod + 10) == 30,
        "AD_MODULE: the control values");
  CHECK(rt.rd16(mod + 0x0E) == 1 && rt.rd16(mod + 0x14) == 4 && rt.rd16(mod + 2) == 64 && rt.rd16(mod + 4) == 48,
        "AD_MODULE: control ids 1..4, the region size");
  CHECK(rt.rd16(mod + 0x16) != 0 && rt.rd16(mod) != 0, "AD_MODULE: hModule, hDrawRgn");
  CHECK(rt.rd16(mod + 0x18) == hpal[0], "palette request 12 selected hpal[0]");
  uint32_t lp = rt.rd32(mod + 0x1A);
  CHECK(rt.rd16(lp) == 0x300 && rt.rd16(lp + 2) == 256 && rt.rd8(lp + 4) == 100 && rt.rd8(lp + 4 + 4 * 234) == 100 &&
            rt.rd8(lp + 4 + 4 * 235) == 0,
        "lpLogPalette: {0x300, 256}, 235 entries copied, the rest zero");
  // DRAWFRAME answering RESTART: INITIALIZE + BLANK again.
  g.calls.clear();
  g.results[2] = {3, 13, 0};
  uint16_t r = b->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(r == 0 && g.joined() == "MODULE(2) MODULE(0) MODULE(1)", "RESTART re-initializes: %u %s", r, g.joined().c_str());
  r = b->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(r == 0 && rt.rd16(mod + 0x18) == hpal[2] && rt.rd8(rt.rd32(mod + 0x1A) + 4) == 102,
        "palette request 13 selected hpal[2]");
  // An error text the module points AD_SYSTEM+0x22 at is copied back.
  g.module_text = rt.static_bytes("test module text", "It broke.");
  g.results[2] = {9};
  r = b->message(2, s + ne16::scratch::kError, ne16::scratch::kErrorSize);
  CHECK(r == 9 && rt.read_str(s + ne16::scratch::kError) == "It broke.", "the module's error text: %u '%s'", r,
        rt.read_str(s + ne16::scratch::kError).c_str());
  g.module_text = 0;
  // Controls: the sound calls only when volume/mute change.
  g.calls.clear();
  rt.wr16(s + ne16::scratch::kCtrl, 99);
  b->set_controls(40, 1, s + ne16::scratch::kCtrl);
  CHECK(g.calls.empty() && rt.rd16(mod + 6) == 99, "same volume: no sound calls, controls copied");
  b->set_controls(70, 0, s + ne16::scratch::kCtrl);
  CHECK(g.joined() == "ADWSETSOUNDMUTE(0) ADWSETVOLUME(70)", "new volume: %s", g.joined().c_str());
  // Unload: CLOSE, then AD_SND restored and released.
  g.calls.clear();
  b->unload();
  CHECK(g.joined() == "MODULE(3) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP", "unload: %s",
        g.joined().c_str());
  b->close();
  CHECK(rt.modules().by_name("FAKEMOD") == nullptr || rt.modules().by_name("FAKEMOD")->system, "module freed");

  // Error ids: an AD_SND that is missing (1) or lacks an entry point (3).
  auto b2 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\NOSUCH.DLL", &why);
  uint16_t ok2 = b2->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok2 == 0 && rt.rd16(s + ne16::scratch::kErrId) == 1, "no AD_SND: error id 1 (%u)",
        rt.rd16(s + ne16::scratch::kErrId));
  b2->close();
  auto b3 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\HALFSND.DLL", &why);
  uint16_t ok3 = b3->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok3 == 0 && rt.rd16(s + ne16::scratch::kErrId) == 3, "AD_SND without adwStopSound: error id 3 (%u)",
        rt.rd16(s + ne16::scratch::kErrId));
  b3->close();
  // MODULESELECTED failing: the module is unloaded again, and AD_SND with it.
  auto b4 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  g.calls.clear();
  g.results[5] = {7};
  uint16_t ok4 = b4->load(hwnd, hdc, s + ne16::scratch::kCtrl, 40, 1, s + ne16::scratch::kPath,
                          s + ne16::scratch::kError, ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(ok4 == 0 && g.joined() == "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) ADWSTOPSOUND ADWSETSYSTEMVOLUMES(4660) "
                                  "ADWSOUNDCLEANUP",
        "MODULESELECTED failing: %s", g.joined().c_str());
  b4->close();

  // BUTTONPUSHED16 (OLDMOD16 1:09e4): AD_SND, the module, AD_SYSTEM+0x26 =
  // the owner, SELECTED, the four values into AD_MODULE, MODULE(7 + slot),
  // the module's error text copied back, freed — no PREINITIALIZE, BLANK or
  // CLOSE; 1 unless SELECTED or the button answered 1 or 7.
  auto b5 = ne16::open_native_bridge(rt, "C:\\WINDOWS\\SYSTEM\\FAKESND.DLL", &why);
  g.calls.clear();
  g.results.clear();
  const int16_t bctrl[4] = {11, 12, -13, 14};
  for (uint32_t i = 0; i < 4; i++) rt.wr16(s + ne16::scratch::kCtrl + 2 * i, uint16_t(bctrl[i]));
  rt.write_str(s + ne16::scratch::kError, "stale", 16);
  uint16_t r5 = b5->button(s + ne16::scratch::kPath, 0xC004, 2, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r5 == 1 && g.joined() == "ADWSOUNDINIT ADWGETSYSTEMVOLUMES MODULE(5) MODULE(9) ADWSTOPSOUND "
                                 "ADWSETSYSTEMVOLUMES(4660) ADWSOUNDCLEANUP",
        "BUTTONPUSHED16 slot 2: %u %s", r5, g.joined().c_str());
  uint32_t sys5 = rt.global().lock(g.hsys);
  uint32_t mod5 = b5->ad_module();
  CHECK(rt.rd16(sys5 + 0x26) == 0xC004, "AD_SYSTEM+0x26 = the owner (%04X)", rt.rd16(sys5 + 0x26));
  CHECK(int16_t(rt.rd16(mod5 + 6)) == 11 && int16_t(rt.rd16(mod5 + 10)) == -13 && rt.rd16(mod5 + 12) == 14,
        "the four values reached AD_MODULE");
  CHECK(rt.read_str(s + ne16::scratch::kError).empty(), "the error buffer starts empty");
  // SELECTED answering 7: no button message, result 0; the button answering
  // with an error text: copied back.
  g.calls.clear();
  g.results[5] = {7};
  uint16_t r6 = b5->button(s + ne16::scratch::kPath, 0, 0, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r6 == 0 && g.joined().find("MODULE(7)") == std::string::npos, "SELECTED 7: no button (%u %s)", r6,
        g.joined().c_str());
  g.module_text = rt.static_bytes("test button text", "No pictures.");
  g.results[8] = {9};
  uint16_t r7 = b5->button(s + ne16::scratch::kPath, 0, 1, s + ne16::scratch::kCtrl, s + ne16::scratch::kError,
                           ne16::scratch::kErrorSize, s + ne16::scratch::kErrId);
  CHECK(r7 == 1 && rt.read_str(s + ne16::scratch::kError) == "No pictures.", "the button's error text: %u '%s'", r7,
        rt.read_str(s + ne16::scratch::kError).c_str());
  g.module_text = 0;
  b5->close();
}

int run_all_unit() {
  run_unit();
  test_layout();
  test_palettes();
  try {
    test_native_bridge();
  } catch (const std::exception& e) {
    CHECK(false, "native bridge: exception %s", e.what());
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// The installed assets' win dir: AD_ASSETS_DIR, else the data folder's
// (read-only; core/tests/test_paths.h).
std::string assets_win() {
  const char* a = getenv("AD_ASSETS_DIR");
  std::string root;
  if (a && *a) {
    root = a;
  } else {
    root = adw_test::installed_assets_root();
    if (root.empty()) return {};
  }
  if (GetFileAttributesA((root + "\\win\\FILES").c_str()) != INVALID_FILE_ATTRIBUTES) return root + "\\win";
  if (GetFileAttributesA((root + "\\FILES").c_str()) != INVALID_FILE_ATTRIBUTES) return root;
  return {};
}

// Runs adhostwin headless on a module; returns its exit code and stderr.
int run_host(const std::string& exe, const std::string& module, int frames, std::string* err,
             const std::string& extra = "") {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  CreatePipe(&rd, &wr, &sa, 0);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul;
  si.hStdOutput = nul;
  si.hStdError = wr;
  std::string cmd = "\"" + exe + "\" " + module + " ADFRAMES=" + std::to_string(frames) + " ADGOWAITMS=0 ADFBHASH=1" + (extra.empty() ? "" : " " + extra);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(rd);
    CloseHandle(wr);
    CloseHandle(nul);
    return -1;
  }
  CloseHandle(wr);
  CloseHandle(nul);
  std::string out;
  char buf[4096];
  DWORD got = 0;
  while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
  CloseHandle(rd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (err) *err = out;
  return int(code);
}

// Runs adhostwin on `module` for `frames` frames with its output discarded,
// sampling its GDI object count every 10 ms; returns the exit code and the
// highest count seen.
int run_host_gdi_peak(const std::string& exe, const std::string& module, int frames, DWORD* peak) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                           0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = si.hStdOutput = si.hStdError = nul;
  std::string cmd = "\"" + exe + "\" " + module + " ADFRAMES=" + std::to_string(frames) + " ADGOWAITMS=0";
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(nul);
  if (!ok) return -1;
  *peak = 0;
  do {
    *peak = std::max(*peak, GetGuiResources(pi.hProcess, GR_GDIOBJECTS));
  } while (WaitForSingleObject(pi.hProcess, 10) == WAIT_TIMEOUT);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(code);
}

std::string hashes(const std::string& log) {
  std::string h;
  size_t p = 0;
  while ((p = log.find("FBHASH ", p)) != std::string::npos) {
    size_t e = log.find('\n', p);
    h += log.substr(p, e - p) + "\n";
    p = e;
  }
  return h;
}

// Sound (lane.hh "Sound", AUDIO.md §8, §10.3), captured headless — no device
// is ever opened. Rat Race's engine (ADXPL300) passes the music gates, plays
// its intro song, and the song's MM_MCINOTIFY, dispatched to its adwMidiCall
// window, chains the loop song; captures are byte-identical run to run, and
// ADSOUND=1 draws what the captured run drew. Bungee Jumping (Totally Twisted)
// plays its MS-ADPCM sounds through sndPlaySound; Nocturne's synchronous
// sounds hold its DRAWFRAME for their duration.
void test_sound_assets(const std::string& exe, const std::string& win) {
  std::string dir = temp_dir("sound");
  auto slurp = [](const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "rb")) {
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
      fclose(f);
    }
    return s;
  };
  // Note-ons (velocity > 0) in a format-0 SMF event log.
  auto note_ons = [](const std::string& smf) {
    size_t p = smf.find("MTrk");
    if (p == std::string::npos) return -1;
    p += 8;
    int n = 0;
    uint8_t status = 0;
    auto byte = [&]() -> int { return p < smf.size() ? uint8_t(smf[p++]) : -1; };
    auto vlq = [&]() {
      uint32_t v = 0;
      for (int i = 0; i < 4; i++) {
        int b = byte();
        if (b < 0) break;
        v = (v << 7) | uint32_t(b & 0x7F);
        if (!(b & 0x80)) break;
      }
      return v;
    };
    while (p < smf.size()) {
      vlq();
      int b = byte();
      if (b < 0) break;
      if (b == 0xFF) {
        byte();
        p += vlq();
        continue;
      }
      if (b == 0xF0 || b == 0xF7) {
        p += vlq();
        continue;
      }
      if (b & 0x80) status = uint8_t(b);
      else p--;
      int d1 = byte(), d2 = (status & 0xE0) == 0xC0 ? 0 : byte();
      (void)d1;
      if ((status & 0xF0) == 0x90 && d2 > 0) n++;
    }
    return n;
  };
  // The largest |sample| of a 16-bit WAV's data chunk.
  auto peak = [](const std::string& wav) {
    size_t p = wav.find("data");
    int m = 0;
    for (size_t i = p == std::string::npos ? wav.size() : p + 8; i + 1 < wav.size(); i += 2) {
      int v = int16_t(uint16_t(uint8_t(wav[i]) | (uint8_t(wav[i + 1]) << 8)));
      m = std::max(m, v < 0 ? -v : v);
    }
    return m;
  };
  {
    std::string e1, e2, es;
    std::string w1 = dir + "\\rat1.wav", w2 = dir + "\\rat2.wav";
    int c1 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 900, &e1, "ADTRACE=sound \"ADAUDIOOUT=" + w1 + "\"");
    int c2 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 900, &e2, "\"ADAUDIOOUT=" + w2 + "\"");
    int cs = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 900, &es, "ADSOUND=1");
    std::string m1 = slurp(dir + "\\rat1.mid");
    CHECK(c1 == 0 && c2 == 0 && cs == 0, "RATRACE with sound: exit %d/%d/%d\n%s", c1, c2, cs, c1 ? e1.c_str() : "");
    CHECK(e1.find("open sequencer!C:\\AFTERDRK\\music\\tellintr.mid alias fred wait") != std::string::npos &&
              e1.find("notify SUCCESSFUL") != std::string::npos && e1.find("tellloop.mid") != std::string::npos,
          "RATRACE: the intro song plays and its MM_MCINOTIFY chains the loop song");
    CHECK(note_ons(m1) >= 100, "RATRACE: the MIDI log has the songs' notes (%d note-ons)", note_ons(m1));
    CHECK(!m1.empty() && slurp(w1).size() > 44 && slurp(w1) == slurp(w2) && m1 == slurp(dir + "\\rat2.mid") &&
              hashes(e1) == hashes(e2),
          "RATRACE: captures and frames identical run to run");
    CHECK(hashes(es) == hashes(e2) && std::count(e2.begin(), e2.end(), '\n') > 0,
          "RATRACE: ADSOUND=1 draws what the captured run drew");
  }
  // The bridge's sound settings (AUDIO.md §8.1): unmuted at the engine's
  // volume with sound on; muted at ADVOLUME without (the 1996 host's "sound
  // off"), through OLDMOD16 and the native bridge alike.
  for (const char* m : {"FILES/CLASSIC/DOMINOES.AD", "packages/simpsons/SIMPSONS/BURNS.AD"}) {
    if (GetFileAttributesA((win + "\\" + m).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    std::string on, off;
    int c1 = run_host(exe, m, 2, &on, "ADTRACE=lane ADSOUND=1 ADVOLUME=30");
    int c2 = run_host(exe, m, 2, &off, "ADTRACE=lane ADVOLUME=30");
    CHECK(c1 == 0 && c2 == 0 && on.find(", volume 30, mute 0) -> 1") != std::string::npos &&
              off.find(", volume 30, mute 1) -> 1") != std::string::npos,
          "%s: LOADADMODULE16 gets volume 30, unmuted only with sound on", m);
  }
  if (GetFileAttributesA((win + "\\packages\\tt\\TWISTED\\BUNGEE.AD").c_str()) != INVALID_FILE_ATTRIBUTES) {
    std::string e, w = dir + "\\bungee.wav";
    int c = run_host(exe, "packages/tt/TWISTED/BUNGEE.AD", 600, &e, "ADTRACE=sound \"ADAUDIOOUT=" + w + "\"");
    int pk = peak(slurp(w));
    CHECK(c == 0 && e.find(": voice ") != std::string::npos && e.find("tag 2,") != std::string::npos && pk > 0x1000,
          "BUNGEE: its MS-ADPCM sounds play through sndPlaySound (exit %d, peak %d)", c, pk);
  }
  {
    std::string e, w = dir + "\\nocturne.wav";
    int c = run_host(exe, "FILES/CLASSIC/NOCTURNE.AD", 600, &e, "ADTRACE=sound,pace \"ADAUDIOOUT=" + w + "\"");
    size_t sync = e.find(", synchronous until ");
    size_t held = sync == std::string::npos ? sync : e.find("0 DRAWFRAME(s), resumed, ended inside one", sync);
    CHECK(c == 0 && sync != std::string::npos && held != std::string::npos,
          "NOCTURNE: a synchronous sound holds its DRAWFRAME across frames (exit %d)", c);
  }
  for (const char* f : {"rat1.wav", "rat1.mid", "rat2.wav", "rat2.mid", "bungee.wav", "bungee.mid", "nocturne.wav",
                        "nocturne.mid"}) {
    DeleteFileA((dir + "\\" + f).c_str());
  }
  RemoveDirectoryA(dir.c_str());
}

int run_assets(const std::string& exe) {
  if (assets_win().empty()) {
    printf("assets not found: skipped\n");
    return 77;
  }
  // Every host below reads these assets and nothing under the real
  // %LOCALAPPDATA%.
  adw_test::sandbox_spawned_hosts(assets_win(), "ne16");
  // Modules that exercise the kinds of Classic module: the AD 3 engine
  // (TOAST3 on ADXPL300), AD_RSRC + ADTOOL (BUGS), a Borland-built one on
  // plain GDI (RAIN), MSVC plain GDI with palette animation (GEOBOUNC), a
  // Windows 3.0 build with a NAMETABLE and AD_SND sounds over AD_RSRC's
  // backward memmove (DOMINOES), and ADXPL300's (YBYH) — the last two need
  // the adw::cpu DF fix (win16.cpu_df_regression).
  for (const char* m : {"FILES/CLASSIC/TOAST3.AD", "FILES/CLASSIC/BUGS.AD", "FILES/CLASSIC/RAIN.AD",
                        "FILES/CLASSIC/GEOBOUNC.AD", "FILES/CLASSIC/DOMINOES.AD", "FILES/CLASSIC/YBYH.AD"}) {
    std::string e1, e2;
    int c1 = run_host(exe, m, 8, &e1);
    CHECK(c1 == 0, "%s: adhostwin exit %d\n%s", m, c1, e1.c_str());
    if (c1 != 0) continue;
    std::string h1 = hashes(e1);
    CHECK(std::count(h1.begin(), h1.end(), '\n') == 8, "%s: 8 frames hashed", m);
    int c2 = run_host(exe, m, 8, &e2);
    CHECK(c2 == 0 && hashes(e2) == h1, "%s: a second run hashes identically", m);
  }
  // Artist swaps two palettes ~90 times a frame (USER.SelectPalette in a
  // GetTickCount loop) with its stroke pen and brush selected, and each swap
  // re-keys them (win16/gdi16.hh extra_). Each key colour's real object is
  // made once: the host stays at a handful of GDI objects, where it used to
  // reach the 10,000-object quota every ~1.8 s.
  {
    DWORD peak = 0;
    int c = run_host_gdi_peak(exe, "FILES/CLASSIC/ARTIST.AD", 1800, &peak);
    CHECK(c == 0 && peak > 0 && peak < 500, "ARTIST over 1,800 frames: exit %d, peak GDI objects %lu", c, peak);
    printf("ARTIST: peak %lu GDI objects over 1,800 frames\n", peak);
  }
  // ZOT draws each lightning bolt, waits in a CPU delay loop and erases it
  // inside one DRAWFRAME: only the scanout rule (lane.cc on_scanout) lets a
  // frame show one, so its FBHASH stream must not be constant.
  {
    std::string e;
    int c = run_host(exe, "FILES/CLASSIC/ZOT.AD", 300, &e);
    std::string h = hashes(e);
    std::string first = h.substr(0, h.find('\n'));
    first = first.substr(first.rfind(' ') + 1);
    size_t lines = size_t(std::count(h.begin(), h.end(), '\n'));
    size_t same = 0;
    for (size_t p = 0; (p = h.find(first, p)) != std::string::npos; p += first.size()) same++;
    CHECK(c == 0 && lines == 300 && same < lines, "ZOT: a lightning bolt reaches some frame (%zu of %zu frames alike)",
          same, lines);
  }
  // Pacing (lane.hh): a module that reads no clock while drawing (Hard Rain)
  // gets a run of DRAWFRAMEs per presented frame, up to ADMAXDRAWS; ADMIPS=0
  // restores exactly one.
  {
    auto draws = [&](const std::string& extra) -> long {
      std::string e;
      int c = run_host(exe, "FILES/CLASSIC/RAIN.AD", 30, &e, "ADTRACE=lane " + extra);
      size_t p = e.find(" DRAWFRAME calls over 30 frames");
      if (c != 0 || p == std::string::npos) return -1;
      size_t s = e.rfind(' ', p - 1);
      return strtol(e.c_str() + s + 1, nullptr, 10);
    };
    long paced = draws(""), one = draws("ADMIPS=0"), capped = draws("ADMAXDRAWS=4");
    CHECK(paced > 30 * 8, "RAIN: a run of DRAWFRAMEs per frame (%ld over 30 frames)", paced);
    CHECK(one == 30, "RAIN with ADMIPS=0: one DRAWFRAME per frame (%ld)", one);
    CHECK(capped == 30 * 4, "RAIN with ADMAXDRAWS=4: four per frame (%ld)", capped);
  }
  // The desktop seed (ADSEEDIMG): Spotlight shows the screen it found through
  // its spots, so a seeded run differs from a black one; ADNOSEED ignores it.
  {
    std::string eb, es, en;
    int cb = run_host(exe, "FILES/CLASSIC/SPOT.AD", 20, &eb);
    int cs = run_host(exe, "FILES/CLASSIC/SPOT.AD", 20, &es, "ADSEEDIMG=:win95");
    int cn = run_host(exe, "FILES/CLASSIC/SPOT.AD", 20, &en, "ADSEEDIMG=:win95 ADNOSEED=1");
    CHECK(cb == 0 && cs == 0 && cn == 0 && hashes(es) != hashes(eb) && hashes(en) == hashes(eb),
          "SPOT: the seed reaches the frames, ADNOSEED drops it (exit %d/%d/%d)", cb, cs, cn);
  }
  // Small screens (lane.hh): Rat Race refuses a 320x240 display ("A larger
  // screen size is needed…"); the saver's /p preview asks for one, so the
  // guest gets 640x480 and the frames are averaged down — deterministically.
  {
    std::string e1, e2, e0;
    int c1 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 20, &e1, "ADSCREENW=320 ADSCREENH=240");
    int c2 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 20, &e2, "ADSCREENW=320 ADSCREENH=240");
    int c0 = run_host(exe, "FILES/CLASSIC/RATRACE.AD", 20, &e0, "ADSCREENW=320 ADSCREENH=240 ADNE16SCALE=1");
    std::string h1 = hashes(e1);
    CHECK(c1 == 0 && c2 == 0 && std::count(h1.begin(), h1.end(), '\n') == 20 && hashes(e2) == h1,
          "RATRACE at 320x240: runs on a 640x480 guest display, deterministic (exit %d/%d)\n%s", c1, c2, e1.c_str());
    CHECK(c0 != 0 && e0.find("larger screen") != std::string::npos,
          "RATRACE at 320x240 with ADNE16SCALE=1: the module refuses (exit %d)", c0);
    CHECK(e1.find("320x240") != std::string::npos, "the host still reports the 320x240 output");
  }
  // Long calls (lane.hh): SATORI's DRAWFRAME draws for about a second, waiting
  // on the tick count; frames end inside it, so its colours move every frame
  // (it was one frame per call: a 60x time-lapse headless, 1 fps streamed).
  {
    std::string e1, e2;
    int c1 = run_host(exe, "FILES/CLASSIC/SATORI.AD", 30, &e1, "ADTRACE=lane");
    int c2 = run_host(exe, "FILES/CLASSIC/SATORI.AD", 30, &e2);
    std::string h1 = hashes(e1);
    std::vector<std::string> v;
    for (size_t p = 0, q; (q = h1.find('\n', p)) != std::string::npos; p = q + 1) v.push_back(h1.substr(h1.rfind(' ', q) + 1, q - h1.rfind(' ', q) - 1));
    std::sort(v.begin(), v.end());
    size_t distinct = size_t(std::unique(v.begin(), v.end()) - v.begin());
    size_t at = e1.find(" ended inside one)");
    long inside = -1;
    if (at != std::string::npos) inside = strtol(e1.c_str() + e1.rfind('(', at) + 1, nullptr, 10);
    CHECK(c1 == 0 && c2 == 0 && distinct >= 25 && inside >= 25 && hashes(e2) == h1,
          "SATORI: frames end inside its long DRAWFRAME (exit %d/%d, %zu distinct of 30, %ld inside, %s)", c1, c2,
          distinct, inside, hashes(e2) == h1 ? "deterministic" : "NOT deterministic");
  }
  // Streamed, the realtime clock only runs from frame 0: EINSTEIN calibrates
  // its delays on the tick count while loading, which spun until the call
  // budget ran out (17 s, "lane init failed") before init time was modeled.
  // AD_RSRC's own load-time calibration (AD_RSRC 1:0110) did the same to the
  // 30 modules built on it (Bogglins, Boris, Mowin' Man, Aquatic Realm, …).
  for (const char* m : {"FILES/CLASSIC/EINSTEIN.AD", "FILES/CLASSIC/BOGGLINS.AD"}) {
    std::string e;
    DWORD t0 = GetTickCount();
    int c = run_host(exe, m, 5, &e, "ADSTREAM=1 ADSTREAMFORCE=1");
    DWORD ms = GetTickCount() - t0;
    CHECK(c == 0 && ms < 10000, "%s streamed: loads and draws (exit %d, %lu ms)\n%s", m, c, (unsigned long)ms,
          c ? e.c_str() : "");
  }
  // The bridge oracle (PACKAGES.md §9): the native AD3 bridge must drive a
  // Classic module exactly as OLDMOD16 does. With ADMIPS=0 neither bridge's
  // own instructions move virtual time, so the streams must be identical —
  // Hard Rain (RAIN) also checks AD_SYSTEM's version and "BUTTHEAD".
  for (const char* m : {"FILES/CLASSIC/TOAST3.AD", "FILES/CLASSIC/RAIN.AD", "FILES/CLASSIC/DOMINOES.AD",
                        "FILES/CLASSIC/GEOBOUNC.AD"}) {
    std::string eo, en;
    int co = run_host(exe, m, 30, &eo, "ADMIPS=0 ADNE16BRIDGE=oldmod16");
    int cn = run_host(exe, m, 30, &en, "ADMIPS=0 ADNE16BRIDGE=native");
    std::string ho = hashes(eo), hn = hashes(en);
    CHECK(co == 0 && cn == 0 && std::count(ho.begin(), ho.end(), '\n') == 30 && ho == hn,
          "%s: native bridge == OLDMOD16 (exit %d/%d)\n%s", m, co, cn, cn ? en.c_str() : "");
  }
  test_sound_assets(exe, assets_win());
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// The PACKAGES.md §4.4 roots under AD_NE16_PKGROOTS (<root>\<id>\win\packages\<id>\...):
// one module per package runs standalone, twice, identically, with no
// unimplemented call.
int run_pkg(const std::string& exe) {
  const char* r = getenv("AD_NE16_PKGROOTS");
  if (!r || !*r) {
    printf("AD_NE16_PKGROOTS not set: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts("", "ne16pkg");  // each run names its AD_ASSETS_DIR
  struct Case {
    const char* id;
    const char* module;
  };
  int ran = 0;
  for (const Case& c : {Case{"ad10", "packages/ad10/AD10TH/CHAM.AD"}, Case{"ad32", "packages/ad32/AD32/GUTS.AD"},
                        Case{"tt", "packages/tt/TWISTED/CHAM.AD"},
                        Case{"simpsons", "packages/simpsons/SIMPSONS/HOMEREAT.AD"}}) {
    std::string root = std::string(r) + "\\" + c.id;
    if (GetFileAttributesA((root + "\\win\\" + c.module).c_str()) == INVALID_FILE_ATTRIBUTES) {
      printf("%s: absent, skipped\n", c.id);
      continue;
    }
    ran++;
    std::string e1, e2, arg = "\"AD_ASSETS_DIR=" + root + "\"";
    int c1 = run_host(exe, c.module, 30, &e1, arg);
    int c2 = run_host(exe, c.module, 30, &e2, arg);
    std::string h1 = hashes(e1);
    CHECK(c1 == 0 && c2 == 0 && std::count(h1.begin(), h1.end(), '\n') == 30 && hashes(e2) == h1,
          "%s %s: exit %d/%d, deterministic\n%s", c.id, c.module, c1, c2, e1.c_str());
    CHECK(e1.find(" 0 unimplemented") != std::string::npos, "%s: no unimplemented calls", c.module);
  }
  // Bring-up regressions on the package roots (each skipped when its root is absent):
  //  * LOGO counts DRAWFRAME calls (its picture moves every 100th at Medium):
  //    with the pacing run it moves; one call per frame left it frozen.
  //  * INS draws a Windows 3.1 window with LoadBitmap(NULL, OBM_*) about a
  //    minute in; with no system bitmaps it stopped with "Out of memory".
  struct Long {
    const char* id;
    const char* module;
    int frames;
    size_t min_distinct;
  };
  for (const Long& c : {Long{"ad32", "packages/ad32/AD32/LOGO.AD", 120, 20},
                        Long{"simpsons", "packages/simpsons/SIMPSONS/INS.AD", 3600, 100}}) {
    std::string root = std::string(r) + "\\" + c.id;
    if (GetFileAttributesA((root + "\\win\\" + c.module).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    std::string e, arg = "\"AD_ASSETS_DIR=" + root + "\"";
    int code = run_host(exe, c.module, c.frames, &e, arg);
    std::string h = hashes(e);
    std::vector<std::string> v;
    for (size_t p = 0, q; (q = h.find('\n', p)) != std::string::npos; p = q + 1) v.push_back(h.substr(h.rfind(' ', q) + 1, q - h.rfind(' ', q) - 1));
    std::sort(v.begin(), v.end());
    size_t distinct = size_t(std::unique(v.begin(), v.end()) - v.begin());
    CHECK(code == 0 && std::count(h.begin(), h.end(), '\n') == c.frames && distinct >= c.min_distinct,
          "%s: exit %d, %zu distinct frames of %d\n%s", c.module, code, distinct, c.frames, code ? e.c_str() : "");
  }
  if (!ran) {
    printf("no package roots found: skipped\n");
    return 77;
  }
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- interaction (INTERACTION.md §5.2, §6, §7, §9.1) ------------------------------------------------------------

struct Proc {
  int code = -1;
  std::string err, out;
};

// Runs `cmd` with stdin from `input` (NUL when empty); stdout and stderr captured.
Proc run_cmd(const std::string& cmd, const std::string& input) {
  Proc p;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  char tmp[MAX_PATH], in_path[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  GetTempFileNameA(tmp, "adi", 0, in_path);
  if (!input.empty()) {
    FILE* f = fopen(in_path, "wb");
    fwrite(input.data(), 1, input.size(), f);
    fclose(f);
  }
  HANDLE in = CreateFileA(input.empty() ? "NUL" : in_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, nullptr);
  HANDLE err_r, err_w, out_r, out_w;
  CreatePipe(&err_r, &err_w, &sa, 0);
  CreatePipe(&out_r, &out_w, &sa, 0);
  SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out_w;
  si.hStdError = err_w;
  PROCESS_INFORMATION pi{};
  std::string c = cmd;
  if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(err_w);
    CloseHandle(out_w);
    CloseHandle(in);
    // stdout is one short JSON line (configure) or nothing: read stderr to the end, then stdout.
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(err_r, buf, sizeof(buf), &got, nullptr) && got) p.err.append(buf, got);
    while (ReadFile(out_r, buf, sizeof(buf), &got, nullptr) && got) p.out.append(buf, got);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    p.code = int(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    CloseHandle(err_w);
    CloseHandle(out_w);
    CloseHandle(in);
  }
  CloseHandle(err_r);
  CloseHandle(out_r);
  DeleteFileA(in_path);
  return p;
}

std::string gos(int n) {
  std::string s;
  for (int i = 0; i < n; i++) s += "GO\n";
  return s;
}

// STATUS <frame> flags=0x<hex> applied=<n> eaten=<n> src=<s>
struct Status {
  long frame = 0;
  unsigned flags = 0;
  unsigned long long applied = 0, eaten = 0;
};
std::vector<Status> statuses(const std::string& log) {
  std::vector<Status> v;
  size_t p = 0;
  while ((p = log.find("STATUS ", p)) != std::string::npos) {
    Status s;
    if (sscanf(log.c_str() + p, "STATUS %ld flags=0x%x applied=%llu eaten=%llu", &s.frame, &s.flags, &s.applied, &s.eaten) == 4)
      v.push_back(s);
    p += 7;
  }
  return v;
}

std::vector<std::string> hash_list(const std::string& log) {
  std::vector<std::string> v;
  size_t p = 0;
  while ((p = log.find("FBHASH ", p)) != std::string::npos) {
    size_t e = log.find('\n', p);
    std::string line = log.substr(p, e - p);
    v.push_back(line.substr(line.rfind(' ') + 1));
    p = e;
  }
  return v;
}

bool module_present(const std::string& win, const char* rel) {
  std::string p = win + "\\" + rel;
  for (char& ch : p)
    if (ch == '/') ch = '\\';
  return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void remove_tree(const std::string& dir) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::string n = fd.cFileName;
      if (n == "." || n == "..") continue;
      std::string p = dir + "\\" + n;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
      else DeleteFileA(p.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryA(dir.c_str());
}

std::string read_file(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
  }
  return s;
}

int run_interaction(const std::string& exe) {
  std::string win = assets_win();
  if (win.empty()) {
    printf("assets not found: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts(win, "ne16int");
  const std::string host = "\"" + exe + "\" ";
  // B1: Caps Lock toggles the games in and out of interactive mode (0x0E);
  // HOW2DRAW shows the cursor (0x11); Caps-only modules never go interactive.
  const std::string caps = gos(900) + "KEY 20 1\nCAPS 1\n" + gos(6) + "KEY 20 0\n" + gos(300) + "KEY 20 1\nCAPS 0\n" +
                           gos(6) + "KEY 20 0\n" + gos(60) + "QUIT\n";
  struct Game {
    const char* module;
    bool game, cursor;
  };
  for (const Game& g : {Game{"FILES/CLASSIC/YBYH.AD", true, false}, Game{"packages/simpsons/SIMPSONS/SIMPTRIV.AD", true, false},
                        Game{"packages/tt/TWISTED/FRANKEN.AD", true, false}, Game{"packages/tt/TWISTED/MIMEHUNT.AD", true, false},
                        Game{"packages/simpsons/SIMPSONS/HOW2DRAW.AD", true, true}, Game{"FILES/CLASSIC/TUNNEL.AD", false, false},
                        Game{"FILES/CLASSIC/CONFETTI.AD", false, false}, Game{"packages/tt/TWISTED/TOILET.AD", false, false},
                        Game{"packages/tt/TWISTED/CHAM.AD", false, false}}) {
    if (!module_present(win, g.module)) {
      printf("%s: absent, skipped\n", g.module);
      continue;
    }
    Proc p = run_cmd(host + g.module + " ADSTATUSLOG=1 ADGOWAITMS=5000", caps);
    std::vector<Status> st = statuses(p.err);
    bool on = false, cursor = false, filter = false;
    for (const Status& s : st) {
      on |= (s.flags & 1) != 0;
      cursor |= (s.flags & 2) != 0;
      filter |= (s.flags & 8) != 0;
    }
    bool off_at_end = !st.empty() && !(st.back().flags & 1);
    CHECK(p.code == 0 && on == g.game && off_at_end && (!g.cursor || cursor), "%s: exit %d, interactive %s, off at the end %s%s",
          g.module, p.code, on ? "seen" : "never", off_at_end ? "yes" : "no", g.cursor ? (cursor ? ", cursor" : ", NO cursor") : "");
    printf("%s: key-filter %s\n", g.module, filter ? "raised (a WH_KEYBOARD hook while it plays)" : "never");
  }
  // B2: the Trivia answers reach the WH_KEYBOARD hook, are consumed while
  // interactive, and change the frames.
  for (const char* m : {"FILES/CLASSIC/YBYH.AD", "packages/simpsons/SIMPSONS/SIMPTRIV.AD"}) {
    if (!module_present(win, m)) continue;
    std::string a = gos(900) + "KEY 20 1\nCAPS 1\n" + gos(6) + "KEY 20 0\n", n = a;
    for (int i = 0; i < 25; i++) {
      int k = 49 + i % 3;
      a += gos(118) + "KEY " + std::to_string(k) + " 1\n" + gos(2) + "KEY " + std::to_string(k) + " 0\n";
      n += gos(120);
    }
    a += gos(60) + "QUIT\n";
    n += gos(60) + "QUIT\n";
    Proc pa = run_cmd(host + m + " ADSTATUSLOG=1 ADFBHASH=1 ADGOWAITMS=5000 ADTRACE=input16", a);
    Proc pn = run_cmd(host + m + " ADFBHASH=1 ADGOWAITMS=5000", n);
    std::vector<Status> st = statuses(pa.err);
    bool hooked = pa.err.find("WH_KEYBOARD") != std::string::npos && pa.err.find("vk 31") != std::string::npos;
    bool eaten = !st.empty() && st.back().eaten >= st.back().applied - 1 && st.back().eaten > 50;
    std::vector<std::string> ha = hash_list(pa.err), hn = hash_list(pn.err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(ha.size(), hn.size()); i++) differ += ha[i] != hn[i];
    CHECK(pa.code == 0 && pn.code == 0 && hooked && eaten && differ > 0 && ha.size() == hn.size(),
          "%s answers: hook %s, eaten %llu of %llu, %zu frames differ", m, hooked ? "yes" : "no",
          st.empty() ? 0ull : st.back().eaten, st.empty() ? 0ull : st.back().applied, differ);
  }
  // B3: Lunatic Fringe finds the blanker window ("Sleep"), reads its queue
  // every frame (key-filter before the first key), eats its keys, plays; it
  // reads LunData.dat from the package's Windows directory (GetWindowsDirectory),
  // seeded from the disc's LUNDATA.DAT as the installers did, so it never says
  // "Configuration File Not Accessible".
  for (const char* m : {"FILES/CLASSIC/LUNATIC.AD", "packages/ad10/AD10TH/LUNATIC.AD"}) {
    if (!module_present(win, m)) continue;
    std::string a = gos(300) + "KEY 20 1\nCAPS 1\n" + gos(6) + "KEY 20 0\n" + gos(120), n = a;
    for (int i = 0; i < 20; i++) {
      for (int k : {37, 38, 32, 75, 74, 32, 39}) {
        a += "KEY " + std::to_string(k) + " 1\n" + gos(10) + "KEY " + std::to_string(k) + " 0\n" + gos(5);
        n += gos(15);
      }
    }
    a += gos(60) + "QUIT\n";
    n += gos(60) + "QUIT\n";
    std::string state = temp_dir("lun");
    Proc pa = run_cmd(host + m + " ADSTATUSLOG=1 ADFBHASH=1 ADGOWAITMS=5000 ADTRACE=input16 \"ADSTATE=" + state + "\"", a);
    Proc pn = run_cmd(host + m + " ADFBHASH=1 ADGOWAITMS=5000", n);
    std::vector<Status> st = statuses(pa.err);
    bool filter_first = false;
    for (const Status& s : st) {
      if (s.applied == 0 && (s.flags & 8)) filter_first = true;
    }
    std::vector<std::string> ha = hash_list(pa.err), hn = hash_list(pn.err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(ha.size(), hn.size()); i++) differ += ha[i] != hn[i];
    bool found = pa.err.find("FindWindow(\"Sleep\", NULL) -> the saver window") != std::string::npos;
    bool eaten = !st.empty() && st.back().eaten == st.back().applied;
    bool data = pa.err.find("Configuration File Not Accessible") == std::string::npos &&
                pn.err.find("Configuration File Not Accessible") == std::string::npos;
    CHECK(pa.code == 0 && found && filter_first && eaten && differ > 0 && data,
          "%s: FindWindow %s, key-filter before the first key %s, all eaten %s, %zu frames differ, LunData.dat %s", m,
          found ? "yes" : "no", filter_first ? "yes" : "no", eaten ? "yes" : "no", differ, data ? "read" : "NOT ACCESSIBLE");
    remove_tree(state);
  }
  // B7 (INTERACTION.md §9.1): DOS Shell keeps going for five virtual minutes.
  // AD 3.2 ships no OLDMOD16: the native bridge only.
  struct Shell {
    const char* module;
    const char* bridge;
  };
  for (const Shell& s : {Shell{"FILES/CLASSIC/DOSSHELL.AD", "oldmod16"}, Shell{"FILES/CLASSIC/DOSSHELL.AD", "native"},
                         Shell{"packages/ad32/AD32/DOSSHELL.AD", "native"}}) {
    if (!module_present(win, s.module)) continue;
    Proc p = run_cmd(host + s.module + " ADFRAMES=18000 ADFBHASH=1 ADGOWAITMS=0 ADNE16BRIDGE=" + s.bridge, "");
    std::vector<std::string> h = hash_list(p.err);
    std::vector<std::string> late(h.begin() + std::min<size_t>(600, h.size()), h.end());
    std::sort(late.begin(), late.end());
    size_t distinct = size_t(std::unique(late.begin(), late.end()) - late.begin());
    CHECK(p.code == 0 && h.size() == 18000 && distinct > 1, "%s (%s): exit %d, %zu frames, %zu distinct after frame 600",
          s.module, s.bridge, p.code, h.size(), distinct);
  }
  // B5/B6: the module buttons in configure mode, hidden and scripted; the
  // state lands in the package's directories; message modules show their
  // text in a later headless run.
  std::string state = temp_dir("cfg"), empty = temp_dir("empty");
  auto script_file = [&](const char* name, const std::string& text) {
    std::string p = state + "_" + name + ".txt";
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return p;
  };
  auto configure = [&](const char* m, int slot, const std::string& script, const std::string& st, const std::string& extra) {
    std::string cmd = host + "--configure " + m + " --button " + std::to_string(slot) + " ADCONFIGHIDDEN=1 ADCONFIGTIMEOUTMS=8000 \"ADSTATE=" +
                      st + "\"" + (script.empty() ? "" : " \"ADCONFIGSCRIPT=" + script + "\"") + (extra.empty() ? "" : " " + extra);
    return run_cmd(cmd, "");
  };
  std::string bmp = temp_dir("pics") + "\\A Picture With A Long Name.bmp";
  CopyFileA((win + "\\FILES\\CLASSIC\\BITMAPS\\TOASTVGA.BMP").c_str(), bmp.c_str(), FALSE);
  struct Button {
    const char* module;
    int slot;
    const char* name;
    std::string script;
    int want;             // exit code
    const char* written;  // a state file (relative to the state root), or ""
  };
  std::vector<Button> buttons = {
      {"FILES/CLASSIC/MESSAGE3.AD", 2, "message3", "TEXT 112 HELLO FROM TEST\nCLICK 113\n", 0, "deluxe\\CLASSIC\\MESG_AD3.DAT"},
      {"FILES/CLASSIC/NONSENSE.AD", 3, "nonsense", "", 0, "deluxe\\CLASSIC\\NONSENSE.TXT"},
      {"FILES/CLASSIC/FISHPRO.AD", 2, "fishpro", "CLICK 1001\nCLICK 1003\nCLICK 1\n", 0, "deluxe\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/BUGS.AD", 1, "bugs", "CLICK 1001\nCLICK 1003\nCLICK 1\n", 0, "deluxe\\WINDOWS\\MODULES.INI"},
      {"packages/ad32/AD32/BUGS.AD", 1, "bugs32", "CLICK 1001\nCLICK 1003\nCLICK 1\n", 0, "ad32\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/ARTIST.AD", 3, "artist", "FILE " + bmp + "\n", 0, "deluxe\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/SLIDE.AD", 0, "slide", "CLICK 1\n", 0, "deluxe\\CLASSIC\\BITMAPS.ADC"},
      {"FILES/CLASSIC/GLOBE.AD", 2, "globe", "TEXT 201 EARTH2.BMP\nCLICK 1\n", 0, "deluxe\\WINDOWS\\AD_PREFS.INI"},
      {"packages/ad32/AD32/LOGO.AD", 2, "logo", "FILE " + bmp + "\n", 0, "ad32\\WINDOWS\\MODULES.INI"},
      {"FILES/CLASSIC/WMORPH.AD", 2, "wmorph", "CLICK 1\n", 0, "deluxe\\CLASSIC\\morphclk.dat"},
      {"FILES/CLASSIC/WMORPH.AD", 3, "wmorph_revert", "ANSWER IDYES\n", 0, ""},
      // Custom Configuration, then OK: the keys are written (copied up over the seed).
      {"FILES/CLASSIC/LUNATIC.AD", 1, "lunatic_keys", "CLICK 101\nCLICK 1\n", 0, "deluxe\\WINDOWS\\LunData.dat"},
      {"FILES/CLASSIC/LUNATIC.AD", 0, "lunatic_clear", "ANSWER IDYES\n", 0, ""},
      {"packages/tt/TWISTED/MESSYGES.AD", 3, "messyges", "TEXT 101 HELLO FROM TEST\nCLICK 1\n", 0, "tt\\WINDOWS\\MODULES.INI"},
      // HOW2DRAW's Help is a message box (not WinHelp).
      {"packages/simpsons/SIMPSONS/HOW2DRAW.AD", 2, "how2draw", "ANSWER IDOK\n", 0, ""},
  };
  for (const Button& b : buttons) {
    if (!module_present(win, b.module)) {
      printf("%s: absent, skipped\n", b.module);
      continue;
    }
    std::string sf = b.script.empty() ? std::string() : script_file(b.name, b.script);
    Proc p = configure(b.module, b.slot, sf, state, "");
    bool file = !*b.written || GetFileAttributesA((state + "\\" + b.written).c_str()) != INVALID_FILE_ATTRIBUTES;
    CHECK(p.code == b.want && file && p.out.find("\"result\"") != std::string::npos,
          "configure %s button %d: exit %d (want %d), %s %s\n%s%s", b.module, b.slot, p.code, b.want, b.written,
          file ? "written" : "MISSING", p.out.c_str(), p.code != b.want ? p.err.c_str() : "");
    if (!sf.empty()) DeleteFileA(sf.c_str());
  }
  // The persisted H: path is 8.3.
  std::string ini = read_file(state + "\\deluxe\\WINDOWS\\MODULES.INI");
  size_t at = ini.find("Image=H:\\");
  CHECK(at != std::string::npos && ini.find("~1.BMP", at) != std::string::npos, "ARTIST keeps a short H: path\n%s", ini.c_str());
  // NONSENSE's word list, as Notepad would have left it: names of our own
  // only (its first sentences have none; one appears within 30 s).
  std::string nonsense = state + "\\deluxe\\CLASSIC\\NONSENSE.TXT";
  if (FILE* f = fopen(nonsense.c_str(), "wb")) {
    fputs("Zyxwv Quibblethorpe\r\nQuibble Zyxwv\r\nXerxes Blorp\r\n", f);
    fclose(f);
  }
  struct Later {
    const char* module;
    const char* args;
    int frames;
  };
  for (const Later& l : {Later{"FILES/CLASSIC/MESSAGE3.AD", "", 600}, Later{"packages/tt/TWISTED/MESSYGES.AD", "ADCVSET=2=0", 600},
                         Later{"FILES/CLASSIC/NONSENSE.AD", "", 1800}}) {
    if (!module_present(win, l.module)) continue;
    std::string base = host + l.module + " ADFRAMES=" + std::to_string(l.frames) + " ADFBHASH=1 ADGOWAITMS=0 " + l.args;
    Proc with = run_cmd(base + " \"ADSTATE=" + state + "\"", "");
    Proc without = run_cmd(base + " \"ADSTATE=" + empty + "\"", "");
    std::vector<std::string> hw = hash_list(with.err), ho = hash_list(without.err);
    size_t differ = 0;
    for (size_t i = 0; i < std::min(hw.size(), ho.size()); i++) differ += hw[i] != ho[i];
    CHECK(with.code == 0 && without.code == 0 && hw.size() == size_t(l.frames) && differ > 0,
          "%s: a later run shows the saved text (%zu of %d frames differ)", l.module, differ, l.frames);
  }
  // A headless run writes nothing into the state it reads.
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((empty + "\\*").c_str(), &fd);
  int entries = 0;
  if (h != INVALID_HANDLE_VALUE) {
    do entries += std::string(fd.cFileName) != "." && std::string(fd.cFileName) != "..";
    while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  CHECK(entries == 0, "the headless runs wrote nothing to their ADSTATE (%d entries)", entries);
  // B6: the native bridge's button path writes what OLDMOD16's does.
  if (module_present(win, "FILES/CLASSIC/MESSAGE3.AD")) {
    std::string so = temp_dir("oracle_o"), sn = temp_dir("oracle_n");
    std::string sf = script_file("oracle", "TEXT 112 HELLO FROM TEST\nCLICK 113\n");
    Proc po = configure("FILES/CLASSIC/MESSAGE3.AD", 2, sf, so, "ADNE16BRIDGE=oldmod16");
    Proc pn = configure("FILES/CLASSIC/MESSAGE3.AD", 2, sf, sn, "ADNE16BRIDGE=native");
    std::string fo = read_file(so + "\\deluxe\\CLASSIC\\MESG_AD3.DAT"), fn = read_file(sn + "\\deluxe\\CLASSIC\\MESG_AD3.DAT");
    CHECK(po.code == 0 && pn.code == 0 && !fo.empty() && fo == fn && fo.find("HELLO FROM TEST") != std::string::npos,
          "MESSAGE3's state: native == OLDMOD16 (%zu / %zu bytes)", fo.size(), fn.size());
    DeleteFileA(sf.c_str());
    remove_tree(so);
    remove_tree(sn);
  }
  remove_tree(state);
  remove_tree(empty);
  remove_tree(bmp.substr(0, bmp.find_last_of('\\')));
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2 && std::string(argv[1]) == "--assets") return run_assets(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--pkg") return run_pkg(argv[2]);
  if (argc > 2 && std::string(argv[1]) == "--interaction") return run_interaction(argv[2]);
  return run_all_unit();
}
