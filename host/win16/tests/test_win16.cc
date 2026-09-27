// adw_win16 tests: the LDT (selectors, huge-block tiling), the global and
// local heaps, Pascal far thunks (argument order, callee pops, DX:AX), far
// callbacks from a shim into guest code (nested), Catch/Throw, INT 21h/1Ah
// basics, the VGA ports, a small NE DLL built in memory (imports, prolog
// patching, LibEntry), and — with the imported assets — OLDMOD16.DLL loaded
// through the module table with its DLLENTRYPOINT, and AD_SND.DLL.
//
//   adw_win16_tests            unit tests
//   adw_win16_tests --assets   the asset tests (exit 77 when the assets are absent)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "adw/core/clock.h"
#include "adw/core/screen.h"
#include "test_paths.h"
#include "win16/dialogs16.hh"
#include "win16/dos16.hh"
#include "win16/gdi16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/display.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win16;
using SegReg = cpu::X86Emulator::SegReg;

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

struct Machine {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  Runtime16 rt{Runtime16Options{}, clock};
  Machine() {
    clock.set_read_step_us(5);
    register_all16(rt);
  }
  // A code segment holding `bytes`; returns its selector.
  uint16_t code(const std::vector<uint8_t>& bytes) {
    GlobalBlock* b = rt.global().alloc_block(uint32_t(bytes.size() + 16), true, 0, 0);
    rt.mem().memcpy(b->base, bytes.data(), bytes.size());
    return b->sel;
  }
  uint16_t data(uint32_t size) {
    GlobalBlock* b = rt.global().alloc_block(size, false, 0, 0);
    return b->sel;
  }
  // `lcall thunk` bytes (9A off seg) for a shim.
  std::vector<uint8_t> lcall(const char* module, const char* name) {
    Shim16Entry* e = rt.shims().find_name(module, name);
    uint32_t fp = rt.thunk_far(*e);
    return {0x9A, uint8_t(fp), uint8_t(fp >> 8), uint8_t(fp >> 16), uint8_t(fp >> 24)};
  }
};

void append(std::vector<uint8_t>& v, const std::vector<uint8_t>& w) { v.insert(v.end(), w.begin(), w.end()); }

// ---- LDT ----------------------------------------------------------------------------------------------

void test_ldt() {
  Ldt ldt;
  uint16_t a = ldt.alloc(1);
  uint16_t b = ldt.alloc(3);
  CHECK((a & 7) == 7 && (b & 7) == 7, "selectors are LDT/RPL3 (%04X %04X)", a, b);
  CHECK(Ldt::index_of(b) == Ldt::index_of(a) + 1, "first fit, consecutive");
  ldt.set_block(b, 0x100000, 0x28000, false);  // 160 KiB: three tiles
  CHECK(ldt.base_of(b) == 0x100000 && ldt.base_of(b + 8) == 0x110000 && ldt.base_of(b + 16) == 0x120000,
        "tiles are __AHINCR apart and 64K apart in memory");
  CHECK(ldt.limit_of(b) == 0x27FFF && ldt.limit_of(b + 16) == 0x7FFF, "tile limits run to the block end (%X %X)",
        ldt.limit_of(b), ldt.limit_of(b + 16));
  cpu::SegDesc d;
  CHECK(ldt.lookup(uint16_t(b & ~1), d) && d.base == 0x100000, "a handle (RPL 2) finds the same descriptor");
  CHECK(!ldt.lookup(0x40, d), "no BIOS selector until set");
  ldt.set_bios_area(0x10000, 0xFFFF);
  CHECK(ldt.lookup(0x40, d) && d.base == 0x10000, "selector 0x40 = BIOS data area");
  ldt.free(b, 3);
  CHECK(ldt.alloc(2) == b, "freed selectors are reused first-fit");
}

// ---- global heap ---------------------------------------------------------------------------------------

void test_global() {
  Machine m;
  GlobalHeap16& g = m.rt.global();
  uint16_t f = g.alloc(0, 100);
  uint16_t mv = g.alloc(GlobalHeap16::kMoveable | GlobalHeap16::kZeroInit, 0x3C);
  CHECK(f && (f & 1), "fixed handle == selector (%04X)", f);
  CHECK(mv && !(mv & 1), "moveable handle has bit 0 clear (%04X)", mv);
  CHECK(g.size(f) == 128 && g.size(mv) == 0x40, "sizes rounded to the 32-byte granule (%u %u)", g.size(f), g.size(mv));
  CHECK(m.rt.ldt().limit_of(f) == 127, "the selector limit covers the rounded size (%X)", m.rt.ldt().limit_of(f));
  uint32_t p = g.lock(mv);
  CHECK(p == (uint32_t(mv | 1) << 16), "GlobalLock = sel:0 (%08X)", p);
  CHECK(g.flags(mv) == 1, "lock count in GlobalFlags");
  CHECK(g.handle(uint16_t(mv | 1)) == ((uint32_t(mv | 1) << 16) | mv), "GlobalHandle(sel) = sel:handle");
  m.rt.wr32(p + 0x3C, 0x12345678);
  uint16_t h2 = g.realloc(mv, 0x30000, GlobalHeap16::kMoveable);
  CHECK(h2 == mv, "a moveable realloc keeps its handle when its selectors can grow (%04X)", h2);
  CHECK(m.rt.rd32(p + 0x3C) == 0x12345678, "realloc keeps the contents");
  CHECK(g.size(mv) == 0x30000, "grown to 192K");
  uint32_t tile2 = (uint32_t((mv | 1) + 2 * Ldt::kAhIncr) << 16) | 0xFFF0;
  m.rt.wr16(tile2, 0xBEEF);
  CHECK(m.rt.mem().read_u16l(m.rt.ldt().base_of(mv | 1) + 0x2FFF0) == 0xBEEF, "third tile addresses 128K..192K");
  CHECK(m.rt.rd16(Runtime16::huge_add(p, 0x2FFF0)) == 0xBEEF, "huge pointer arithmetic");
  CHECK(!g.unlock(mv), "unlock to 0");
  CHECK(g.free(mv) == 0 && g.free(f) == 0, "GlobalFree returns 0");
  CHECK(g.free(f) == f, "double free fails");
  CHECK(!g.find(f), "freed");
  // A zero-size moveable block is discarded: handle, no memory.
  uint16_t z = g.alloc(GlobalHeap16::kMoveable, 0);
  CHECK(z && !g.lock(z) && (g.flags(z) & GlobalHeap16::kFlagDiscarded), "discarded block");
}

// ---- local heap ----------------------------------------------------------------------------------------

void test_local() {
  Machine m;
  uint16_t ds = m.data(0x10000);
  LocalHeaps16& lh = m.rt.local();
  lh.note_dgroup(ds, 0x200);
  CHECK(lh.init(ds, 0, 0x400), "LocalInit(ds, 0, 0x400)");
  CHECK(m.rt.rd16((uint32_t(ds) << 16) | 6) == 0x200, "pLocalHeap at DS:[6]");
  uint16_t a = lh.alloc(ds, 0, 10);
  uint16_t b = lh.alloc(ds, LocalHeaps16::kMoveable | LocalHeaps16::kZeroInit, 20);
  CHECK(a >= 0x200 && (a & 3) == 0, "fixed block: 4-aligned pointer (%04X)", a);
  CHECK((b & 3) == 2, "moveable handle ≡ 2 mod 4 (%04X)", b);
  uint16_t pb = lh.lock(ds, b);
  CHECK(pb && m.rt.rd16((uint32_t(ds) << 16) | b) == pb, "*(WORD*)handle is the block (%04X)", pb);
  CHECK(lh.flags(ds, b) == 1, "lock count");
  CHECK(lh.handle(ds, pb) == b, "LocalHandle(ptr)");
  CHECK(lh.size(ds, a) == 12, "LocalSize rounds to 4 (%u)", lh.size(ds, a));
  // Growth past the initial 1K: the DGROUP is 64K.
  uint16_t big = lh.alloc(ds, 0, 0x2000);
  CHECK(big != 0, "the heap grows past LocalInit's size");
  uint16_t b2 = lh.realloc(ds, b, 400, LocalHeaps16::kMoveable);
  CHECK(b2 == b && lh.size(ds, b) >= 400, "moveable realloc keeps the handle");
  CHECK(lh.unlock(ds, b) == false, "unlocked");
  CHECK(lh.free(ds, b) == 0 && lh.free(ds, a) == 0 && lh.free(ds, big) == 0, "LocalFree");
  CHECK(lh.free(ds, a) == a, "double free fails");
}

// ---- thunks: Pascal convention -------------------------------------------------------------------------

void test_thunks() {
  Machine m;
  int called = 0;
  uint16_t got_a = 0;
  uint32_t got_b = 0;
  uint16_t got_c = 0;
  m.rt.shims().add("TESTDLL", 1, "Pascal3", Conv16::pascal_, false, 8, [&](Call16& c) {
    got_a = c.w();
    got_b = c.l();
    got_c = c.w();
    called++;
    c.ret32(0xCAFE0000u | got_a);
  });
  m.rt.shims().add("TESTDLL", 2, "Cdecl", Conv16::cdecl_, true, 4, [&](Call16& c) {
    uint16_t x = c.w(), y = c.w();
    c.ret(uint16_t(x - y));
  });
  // push 0x1111; push 0x2222; push 0x3333 (the long: high first); push 0x4444;
  // lcall Pascal3; mov bx,ax; mov si,dx; push 5; push 9 (cdecl: y first, x last); lcall Cdecl; add sp,4; retf
  uint32_t p3 = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 1));
  uint32_t cd = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 2));
  std::vector<uint8_t> code = {0x68, 0x11, 0x11, 0x68, 0x22, 0x22, 0x68, 0x33, 0x33, 0x68, 0x44, 0x44};
  append(code, {0x9A, uint8_t(p3), uint8_t(p3 >> 8), uint8_t(p3 >> 16), uint8_t(p3 >> 24)});
  append(code, {0x89, 0xC3, 0x89, 0xD6});
  append(code, {0x6A, 0x05, 0x6A, 0x09});
  append(code, {0x9A, uint8_t(cd), uint8_t(cd >> 8), uint8_t(cd >> 16), uint8_t(cd >> 24)});
  append(code, {0x83, 0xC4, 0x04});
  // Result in DX:AX = SI:BX of the first call, CX = the cdecl result.
  append(code, {0x89, 0xC1, 0x89, 0xD8, 0x89, 0xF2, 0xCB});
  uint16_t cs = m.code(code);
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {});
  CHECK(called == 1, "shim called");
  CHECK(got_a == 0x1111 && got_b == 0x22223333 && got_c == 0x4444, "Pascal args in declaration order (%04X %08X %04X)",
        got_a, got_b, got_c);
  CHECK(r == 0xCAFE1111, "DX:AX result (%08X)", r);
  CHECK(m.rt.cpu().registers().r_sp() == sp0, "call_far restores SP");
  // The callee popped exactly 8 bytes: otherwise the cdecl result would be garbage.
  CHECK(m.rt.last_sp_after() == sp0, "the stack balanced across both calls (%04X vs %04X)", m.rt.last_sp_after(), sp0);
}

// ---- callbacks: a shim calling guest code, nested -------------------------------------------------------

void test_callbacks() {
  Machine m;
  // Guest callback: int FAR PASCAL cb(int a, int b) { return a*10+b; } → retf 4.
  std::vector<uint8_t> cb = {0x55, 0x89, 0xE5, 0x8B, 0x46, 0x08, 0x6B, 0xC0, 0x0A, 0x03, 0x46, 0x06, 0x5D, 0xCA, 0x04, 0x00};
  uint16_t cbs = m.code(cb);
  uint32_t seen = 0;
  m.rt.shims().add("TESTDLL", 3, "Enum", Conv16::pascal_, true, 4, [&](Call16& c) {
    uint32_t proc = c.ptr();
    uint32_t sum = 0;
    for (uint16_t i = 1; i <= 3; i++) sum += m.rt.call_far(proc, {w16(i), w16(7)}) & 0xFFFF;
    seen = sum;
    c.ret(uint16_t(sum));
  });
  uint32_t en = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 3));
  // push cbs; push 0; lcall Enum; retf
  std::vector<uint8_t> code = {0x68, uint8_t(cbs), uint8_t(cbs >> 8), 0x6A, 0x00};
  append(code, {0x9A, uint8_t(en), uint8_t(en >> 8), uint8_t(en >> 16), uint8_t(en >> 24), 0xCB});
  uint16_t cs = m.code(code);
  // Mark SI/DI/BP/DS: callbacks must preserve the caller's registers.
  Regs16In in;
  in.si = 0x5151;
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {}, &in);
  CHECK(seen == 17 + 27 + 37, "three nested callbacks (%u)", seen);
  CHECK((r & 0xFFFF) == 81, "result through the thunk (%u)", r & 0xFFFF);
}

// ---- Catch / Throw -------------------------------------------------------------------------------------

void test_catch_throw() {
  Machine m;
  uint16_t buf = m.data(64);
  auto catch_ = m.lcall("KERNEL", "Catch");
  auto throw_ = m.lcall("KERNEL", "Throw");
  // mov ax,buf; mov es,ax  (unused) ; push buf; push 0; lcall Catch; or ax,ax; jnz caught;
  // push buf; push 0; push 42; lcall Throw; (never returns) caught: retf
  std::vector<uint8_t> code = {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00};
  append(code, catch_);
  append(code, {0x09, 0xC0, 0x75, 0x00});  // or ax,ax; jnz rel8 (patched below)
  size_t jnz = code.size() - 1;
  append(code, {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00, 0x6A, 0x2A});
  append(code, throw_);
  append(code, {0xB8, 0xFF, 0xFF, 0xCB});  // mov ax,-1; retf (not reached)
  code[jnz] = uint8_t(code.size() - (jnz + 1));
  append(code, {0xCB});  // caught: retf with AX = 42
  uint16_t cs = m.code(code);
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {});
  CHECK((r & 0xFFFF) == 42, "Throw returns through Catch with its value (%d)", int16_t(r));
  CHECK(m.rt.last_sp_after() == sp0, "stack restored by Throw");
}

// A Throw from inside a callback the host made (a window procedure called by
// a shim) to a Catch in the code that called that shim: the host's frames in
// between must unwind and the guest resume at the Catch.
void test_throw_across_host_levels() {
  Machine m;
  uint16_t buf = m.data(64);
  auto catch_ = m.lcall("KERNEL", "Catch");
  auto throw_ = m.lcall("KERNEL", "Throw");
  bool shim_returned = false;
  m.rt.shims().add("TESTDLL", 4, "CallBack", Conv16::pascal_, true, 4, [&](Call16& c) {
    uint32_t proc = c.ptr();
    m.rt.call_far(proc, {w16(1), w16(2)});
    shim_returned = true;  // must not happen: the callback Threw past us
    c.ret(0xEEEE);
  });
  // The callback: push buf; push 0; push 7; lcall Throw; retf 4 (not reached)
  std::vector<uint8_t> cb = {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00, 0x6A, 0x07};
  append(cb, throw_);
  append(cb, {0xCA, 0x04, 0x00});
  uint16_t cbs = m.code(cb);
  uint32_t shim = m.rt.thunk_far(*m.rt.shims().find("TESTDLL", 4));
  // push buf; push 0; lcall Catch; or ax,ax; jnz done; push cbs; push 0; lcall CallBack; mov ax,-1; done: retf
  std::vector<uint8_t> code = {0x68, uint8_t(buf), uint8_t(buf >> 8), 0x6A, 0x00};
  append(code, catch_);
  append(code, {0x09, 0xC0, 0x75, 0x00});
  size_t jnz = code.size() - 1;
  append(code, {0x68, uint8_t(cbs), uint8_t(cbs >> 8), 0x6A, 0x00});
  append(code, {0x9A, uint8_t(shim), uint8_t(shim >> 8), uint8_t(shim >> 16), uint8_t(shim >> 24)});
  append(code, {0xB8, 0xFF, 0xFF});
  code[jnz] = uint8_t(code.size() - (jnz + 1));
  append(code, {0xCB});
  uint16_t cs = m.code(code);
  uint16_t sp0 = m.rt.cpu().registers().r_sp();
  uint32_t r = 0;
  try {
    r = m.rt.call_far(uint32_t(cs) << 16, {});
  } catch (const std::exception& e) {
    CHECK(false, "cross-level Throw: %s", e.what());
  }
  CHECK((r & 0xFFFF) == 7, "Throw from a host-called callback lands at the outer Catch (%d)", int16_t(r));
  CHECK(!shim_returned, "the shim between Catch and Throw was unwound, not returned from");
  CHECK(m.rt.call_depth() == 0 && m.rt.last_sp_after() == sp0, "call levels and stack unwound (depth %d, sp %04X/%04X)",
        m.rt.call_depth(), m.rt.last_sp_after(), sp0);
}

// A guest fault inside a call leaves the machine as the call found it, so the
// host can call into the guest again (the lane's UNLOADADMODULE16 after a
// failed DRAWFRAME).
void test_fault_restores_state() {
  Machine m;
  uint16_t small = m.data(16);
  auto& r = m.rt.cpu().registers();
  uint16_t sp0 = r.r_sp(), ds0 = m.rt.cpu().get_segment(SegReg::DS);
  r.w_si(0x5151);
  // mov ax,small; mov ds,ax; mov si,1234h; push ax; push ax; mov ax,[100h] (past the limit); retf
  std::vector<uint8_t> code = {0xB8, uint8_t(small), uint8_t(small >> 8), 0x8E, 0xD8, 0xBE, 0x34, 0x12,
                               0x50, 0x50, 0xA1, 0x00, 0x01, 0xCB};
  bool faulted = false;
  try {
    m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  } catch (const GuestError16& e) {
    faulted = e.kind() == GuestError16::Kind::fault;
  }
  CHECK(faulted, "the out-of-limit read faults");
  CHECK(r.r_sp() == sp0 && r.r_si() == 0x5151 && m.rt.cpu().get_segment(SegReg::DS) == ds0 && m.rt.call_depth() == 0,
        "registers restored after the fault (sp %04X si %04X)", r.r_sp(), r.r_si());
  std::vector<uint8_t> ok = {0xB8, 0x05, 0x00, 0xCB};
  CHECK((m.rt.call_far(uint32_t(m.code(ok)) << 16, {}) & 0xFFFF) == 5, "the next call runs normally");
}

// WM_CREATE's CREATESTRUCT is the whole 34-byte Win16 structure (dwExStyle
// last): a window procedure reading it must not fault.
void test_create_window() {
  Machine m;
  Screen screen(64, 32);
  m.rt.attach_display(screen);
  // Window procedure: WM_CREATE → -1 unless lpcs->dwExStyle's high word is 1234h.
  // push bp; mov bp,sp; cmp word [bp+12],1; jne other; les bx,[bp+6]; mov ax,es:[bx+32];
  // cmp ax,1234h; je ok; mov ax,-1; jmp out; ok/other: xor ax,ax; out: cwd; pop bp; retf 10
  std::vector<uint8_t> wp = {0x55, 0x89, 0xE5, 0x83, 0x7E, 0x0C, 0x01, 0x75, 0x11, 0xC4, 0x5E,
                             0x06, 0x26, 0x8B, 0x47, 0x20, 0x3D, 0x34, 0x12, 0x74, 0x05, 0xB8,
                             0xFF, 0xFF, 0xEB, 0x02, 0x31, 0xC0, 0x99, 0x5D, 0xCA, 0x0A, 0x00};
  uint16_t wps = m.code(wp);
  uint16_t ds = m.data(256);
  uint32_t d = uint32_t(ds) << 16;
  m.rt.write_str(d, "TESTCLS", 16);
  // WNDCLASS: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, menu, class.
  m.rt.wr32(d + 0x20 + 2, uint32_t(wps) << 16);
  m.rt.wr32(d + 0x20 + 22, d);
  auto shim = [&](const char* mod, const char* name) { return m.rt.thunk_far(*m.rt.shims().find_name(mod, name)); };
  CHECK(m.rt.call_far(shim("USER", "RegisterClass"), {l16(d + 0x20)}) & 0xFFFF, "RegisterClass");
  uint32_t hwnd = 0;
  try {
    hwnd = m.rt.call_far(shim("USER", "CreateWindowEx"),
                         {l16(0x12345678), l16(d), l16(d), l16(0), w16(0), w16(0), w16(10), w16(10), w16(0), w16(0),
                          w16(0), l16(0)}) &
           0xFFFF;
  } catch (const std::exception& e) {
    CHECK(false, "CreateWindowEx: %s", e.what());
  }
  CHECK(hwnd != 0, "WM_CREATE saw dwExStyle in its CREATESTRUCT");
}

// ---- DOS / BIOS / ports ---------------------------------------------------------------------------------

void test_dos() {
  Machine m;
  // mov ah,30h; int 21h; mov bx,ax; mov ah,2Ch; int 21h; mov ax,bx; retf  → AX = version, CX:DX time
  std::vector<uint8_t> code = {0xB4, 0x30, 0xCD, 0x21, 0x89, 0xC3, 0xB4, 0x2C, 0xCD, 0x21, 0x89, 0xD8, 0x89, 0xCA,
                               0xCB};
  uint16_t cs = m.code(code);
  uint32_t r = m.rt.call_far(uint32_t(cs) << 16, {});
  CHECK((r & 0xFFFF) == 0x0007, "DOS 7.00 (%04X)", r & 0xFFFF);
  // The Win32 lane's headless epoch (FILETIME 125132976000000000) reads as 20:00.
  CHECK((r >> 16) == 0x1400, "headless clock: 20:00 (CX=%04X)", r >> 16);
  // INT 1Ah AH=0: ticks since midnight, 20h * 18.2/s.
  std::vector<uint8_t> c2 = {0xB4, 0x00, 0xCD, 0x1A, 0x89, 0xD0, 0x89, 0xCA, 0xCB};
  uint32_t t = m.rt.call_far(uint32_t(m.code(c2)) << 16, {});
  CHECK(t >= 1310000 && t <= 1311000, "BIOS ticks at 20:00 (%u)", t);
  // Selector 0x40: mov ax,40h; mov es,ax; mov ax,es:[6Ch]; retf
  std::vector<uint8_t> c3 = {0xB8, 0x40, 0x00, 0x8E, 0xC0, 0x26, 0xA1, 0x6C, 0x00, 0xCB};
  uint32_t lo = m.rt.call_far(uint32_t(m.code(c3)) << 16, {});
  CHECK((lo & 0xFFFF) == (t & 0xFFFF) || (lo & 0xFFFF) == ((t + 1) & 0xFFFF), "0040:006C holds the ticks");
  // File I/O through the Vfs: open this test's own directory listing is host-specific, so
  // create a file in an overlay's memory upper, write, seek, read back.
  m.rt.vfs().mount_overlay("C:\\AFTERDRK", ".", "");
  uint16_t ds = m.data(256);
  m.rt.write_str(uint32_t(ds) << 16, "C:\\AFTERDRK\\SAVE.DAT", 64);
  m.rt.write_str((uint32_t(ds) << 16) | 0x40, "hello", 16);
  // mov ax,ds_sel; mov ds,ax; mov ah,3Ch; xor cx,cx; xor dx,dx; int 21h; mov bx,ax;
  // mov ah,40h; mov cx,5; mov dx,40h; int 21h; mov ax,4200h; xor cx,cx; xor dx,dx; int 21h;
  // mov ah,3Fh; mov cx,5; mov dx,80h; int 21h; push ax; mov ah,3Eh; int 21h; pop ax; retf
  std::vector<uint8_t> c4 = {0xB8, uint8_t(ds), uint8_t(ds >> 8), 0x8E, 0xD8, 0xB4, 0x3C, 0x31, 0xC9, 0x31, 0xD2,
                             0xCD, 0x21, 0x89, 0xC3, 0xB4, 0x40, 0xB9, 0x05, 0x00, 0xBA, 0x40, 0x00, 0xCD, 0x21,
                             0xB8, 0x00, 0x42, 0x31, 0xC9, 0x31, 0xD2, 0xCD, 0x21, 0xB4, 0x3F, 0xB9, 0x05, 0x00,
                             0xBA, 0x80, 0x00, 0xCD, 0x21, 0x50, 0xB4, 0x3E, 0xCD, 0x21, 0x58, 0xCB};
  uint32_t n = m.rt.call_far(uint32_t(m.code(c4)) << 16, {});
  CHECK((n & 0xFFFF) == 5 && m.rt.read_str((uint32_t(ds) << 16) | 0x80) == "hello",
        "create/write/seek/read through INT 21h (%u, \"%s\")", n & 0xFFFF,
        m.rt.read_str((uint32_t(ds) << 16) | 0x80).c_str());
  // VGA retrace (port 0x3DA) must toggle while polled: wait for set, then clear.
  // mov dx,3DAh; l1: in al,dx; test al,8; jz l1; l2: in al,dx; test al,8; jnz l2; retf
  std::vector<uint8_t> c5 = {0xBA, 0xDA, 0x03, 0xEC, 0xA8, 0x08, 0x74, 0xFB, 0xEC, 0xA8, 0x08, 0x75, 0xFB, 0xCB};
  m.rt.call_far(uint32_t(m.code(c5)) << 16, {});
  CHECK(true, "the retrace wait loop terminates");
}

// ---- a tiny NE DLL built in memory ------------------------------------------------------------------------

// Writes an NE DLL with a code segment and a data segment. The code segment
// has LibEntry at 0 (returns AX=1 after calling KERNEL.GetVersion through an
// import) and an exported function at 0x20 with the MSVC prolog
// `push ds; pop ax; nop; …` that returns DS.
std::string build_ne() {
  std::string f(0x40, '\0');
  f[0] = 'M';
  f[1] = 'Z';
  uint32_t ne = 0x40;
  f[0x3C] = char(ne);
  std::string h(0x40, '\0');
  auto w16 = [](std::string& s, size_t at, uint16_t v) {
    if (s.size() < at + 2) s.resize(at + 2, '\0');
    s[at] = char(v);
    s[at + 1] = char(v >> 8);
  };
  h[0] = 'N';
  h[1] = 'E';
  // Tables after the header: segment table (2 x 8), resource table (none),
  // resident names, module refs, imported names, entry table.
  std::string seg, res, resident, modref, imp, entry;
  resident += char(6) + std::string("TESTNE") + std::string("\0\0", 2);
  resident += char(4) + std::string("FUNC") + std::string("\x01\x00", 2);
  resident += '\0';
  imp += '\0';
  imp += char(6) + std::string("KERNEL");
  w16(modref, 0, 1);  // KERNEL at imported-names offset 1
  // Entry table: one bundle of 1 moveable entry: ordinal 1 → seg 1:0x20.
  entry += char(1);
  entry += char(0xFF);
  entry += char(0x03);  // exported | shared data
  entry += char(0xCD);
  entry += char(0x3F);
  entry += char(1);
  entry += char(0x20);
  entry += char(0x00);
  entry += '\0';
  uint16_t off = 0x40;
  uint16_t seg_off = off;
  off += 16;
  uint16_t res_off = off;
  uint16_t resident_off = off;
  off += uint16_t(resident.size());
  uint16_t modref_off = off;
  off += uint16_t(modref.size());
  uint16_t imp_off = off;
  off += uint16_t(imp.size());
  uint16_t entry_off = off;
  off += uint16_t(entry.size());
  w16(h, 0x04, entry_off);
  w16(h, 0x06, uint16_t(entry.size()));
  w16(h, 0x0C, 0x8001);  // LIBRARY | SINGLEDATA
  w16(h, 0x0E, 2);       // autodata = segment 2
  w16(h, 0x10, 0x100);   // heap
  w16(h, 0x14, 0);       // IP
  w16(h, 0x16, 1);       // CS = segment 1
  w16(h, 0x1C, 2);       // segments
  w16(h, 0x1E, 1);       // module refs
  w16(h, 0x22, seg_off);
  w16(h, 0x24, res_off);
  w16(h, 0x26, resident_off);
  w16(h, 0x28, modref_off);
  w16(h, 0x2A, imp_off);
  w16(h, 0x32, 4);  // alignment shift 4
  h[0x36] = 2;      // Windows
  w16(h, 0x3E, 0x030A);
  std::string tables = h;
  tables.resize(0x40);
  // Code segment at file 0x200, data at 0x300 (shift 4 → sectors 0x20, 0x30).
  std::string code(0x40, '\x90');
  // LibEntry: lcall KERNEL.3 (GetVersion) [fixup at 1]; mov ax,1; retf
  const uint8_t le[] = {0x9A, 0xFF, 0xFF, 0x00, 0x00, 0xB8, 0x01, 0x00, 0xCB};
  memcpy(code.data(), le, sizeof(le));
  // FUNC at 0x20: push ds; pop ax; nop; mov ax, ax(=patched DS); retf
  const uint8_t fn[] = {0x1E, 0x58, 0x90, 0xCB};
  memcpy(code.data() + 0x20, fn, sizeof(fn));
  // Relocations: 1 record: import ordinal KERNEL.3 at offset 1, pointer32.
  std::string rel;
  w16(rel, 0, 1);
  rel += char(3);  // pointer32
  rel += char(1);  // import ordinal
  w16(rel, 4, 1);  // offset
  w16(rel, 6, 1);  // module 1
  w16(rel, 8, 3);  // ordinal 3
  std::string segt;
  w16(segt, 0, 0x20);                            // sector
  w16(segt, 2, uint16_t(code.size()));           // length
  w16(segt, 4, 0x0100 | 0x0010);                 // RELOCINFO | MOVEABLE (code)
  w16(segt, 6, uint16_t(code.size()));
  w16(segt, 8, 0x30);
  w16(segt, 10, 0x10);
  w16(segt, 12, 0x0001);  // data
  w16(segt, 14, 0x10);
  std::string all = f;
  all.resize(ne);
  all += tables;
  all += segt;
  all += resident;
  all += modref;
  all += imp;
  all += entry;
  all.resize(0x200, '\0');
  all += code;
  all += rel;
  all.resize(0x300, '\0');
  all += std::string(0x10, '\0');
  return all;
}

void test_ne_module() {
  Machine m;
  char tmp[MAX_PATH], dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  snprintf(tmp, sizeof(tmp), "%sadw_win16_test_%lu.dll", dir, GetCurrentProcessId());
  std::string img = build_ne();
  FILE* fh = fopen(tmp, "wb");
  fwrite(img.data(), 1, img.size(), fh);
  fclose(fh);
  uint16_t err = 0;
  Module16* mod = m.rt.modules().load_host(tmp, &err);
  CHECK(mod != nullptr, "the synthetic NE DLL loads (error %u)", err);
  if (mod) {
    CHECK(mod->name == "TESTNE", "module name %s", mod->name.c_str());
    CHECK(mod->initialized, "LibEntry ran");
    CHECK(m.rt.shims().find("KERNEL", 3)->calls == 1, "the import reached KERNEL.GetVersion");
    uint32_t fn = m.rt.modules().proc_address(mod, "func");
    CHECK(fn, "GetProcAddress by name, any case");
    uint32_t lin = m.rt.linear(fn, 3);
    CHECK(m.rt.mem().read_u8(lin) == 0xB8 && m.rt.mem().read_u16l(lin + 1) == mod->dgroup,
          "prolog patched to mov ax,DGROUP");
    CHECK((m.rt.call_far(fn, {}) & 0xFFFF) == mod->dgroup, "the exported function sees its DGROUP");
    CHECK(m.rt.modules().by_handle(mod->hinstance) == mod && m.rt.modules().by_name("testne") == mod, "lookups");
    CHECK(m.rt.rd16(uint32_t(mod->hmodule) << 16) == 0x454E, "module database starts with NE");
    m.rt.modules().free(mod);
    CHECK(!m.rt.modules().by_name("TESTNE"), "FreeLibrary unloads");
  }
  DeleteFileA(tmp);
}

// ---- GDI basics on an attached display ---------------------------------------------------------------------

void test_gdi() {
  Machine m;
  Screen screen(64, 32);
  m.rt.attach_display(screen);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t hdc = g.create_screen_dc(0);
  CHECK(hdc, "screen DC");
  // A palette with one reserved red entry, realized: it lands on index 10.
  std::vector<PALETTEENTRY> pe = {{255, 0, 0, PC_RESERVED}};
  uint16_t pal = g.create_palette(pe);
  Dc16* d = g.dc(hdc);
  d->s.palette = pal;
  g.display().realize(*g.get(pal, G16::palette)->pal, false);
  g.sync(hdc);
  uint16_t br = g.create_brush(0x01000000);  // PALETTEINDEX(0)
  g.select(hdc, br);
  PatBlt(g.host_dc(hdc), 0, 0, 8, 8, PATCOPY);
  GdiFlush();
  CHECK(screen.at(3, 3) == 10, "PALETTEINDEX(0) drew hardware index 10 (%u)", screen.at(3, 3));
  CHECK(screen.palette()[10].rgbRed == 255, "hardware palette entry 10 is red");
  g.release_dc(hdc);
}

// ---- the synthetic desktop, icons, and the GDI/INI additions (PACKAGES.md §7.3) -----------------------------

uint32_t api(Machine& m, const char* mod, const char* name, std::initializer_list<Arg16> args) {
  Shim16Entry* e = m.rt.shims().find_name(mod, name);
  if (!e) throw std::runtime_error(std::string("no shim ") + mod + "." + name);
  return m.rt.call_far(m.rt.thunk_far(*e), args);
}

void test_desktop() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  uint32_t progman_cls = m.rt.static_bytes("t Progman", "Progman");
  // Before anything enumerates windows there is no Program Manager (BADDOG3 looks for one).
  CHECK((api(m, "USER", "FindWindow", {l16(progman_cls), l16(0)}) & 0xFFFF) == 0, "no Program Manager yet");
  CHECK(!m.rt.vfs().exists("C:\\WINDOWS\\PROGMAN.INI"), "no PROGMAN.INI yet");
  // An EnumWindows callback in host code: records each HWND; stops when told.
  std::vector<uint16_t> seen;
  size_t stop_after = 100;
  uint32_t lp_seen = 0;
  m.rt.shims().add("TESTCB", 1, "ENUMPROC", Conv16::pascal_, true, 6, [&](Call16& c) {
    seen.push_back(c.w());
    lp_seen = c.l();
    c.ret(seen.size() < stop_after ? 1 : 0);
  });
  uint32_t cb = m.rt.thunk_far(*m.rt.shims().find_name("TESTCB", "ENUMPROC"));
  uint32_t r = api(m, "USER", "EnumWindows", {l16(cb), l16(0x12345678)});
  uint16_t pm = uint16_t(api(m, "USER", "FindWindow", {l16(progman_cls), l16(0)}));
  CHECK((r & 0xFFFF) == 1 && seen.size() == 2 && seen[0] == saver && seen[1] == pm && pm && lp_seen == 0x12345678,
        "EnumWindows: the saver, then Program Manager (%zu windows)", seen.size());
  seen.clear();
  stop_after = 1;
  api(m, "USER", "EnumWindows", {l16(cb), l16(0)});
  CHECK(seen.size() == 1, "a callback returning 0 stops the enumeration");
  // What the gatherers ask of each window.
  uint16_t icon = uint16_t(api(m, "USER", "GetClassWord", {w16(pm), w16(uint16_t(-14))}));
  CHECK(icon != 0 && (api(m, "USER", "GetClassWord", {w16(saver), w16(uint16_t(-14))}) & 0xFFFF) == 0,
        "GCW_HICON: Program Manager has a class icon, the saver window none");
  CHECK((api(m, "USER", "IsWindowVisible", {w16(pm)}) & 0xFFFF) == 1, "Program Manager is visible");
  uint16_t ds = m.data(256);
  uint32_t buf = uint32_t(ds) << 16;
  api(m, "USER", "GetWindowText", {w16(pm), l16(buf), w16(40)});
  CHECK(m.rt.read_str(buf) == "Program Manager", "its title");
  api(m, "USER", "GetClassName", {w16(pm), l16(buf), w16(40)});
  CHECK(m.rt.read_str(buf) == "Progman", "its class");
  m.rt.wr16(buf, 22);
  api(m, "USER", "GetWindowPlacement", {w16(pm), l16(buf)});
  CHECK(m.rt.rd16(buf + 4) == SW_SHOWNORMAL, "shown normally, not iconic");
  // GetWindow: Z order among the top-level windows.
  uint16_t desk = uint16_t(api(m, "USER", "GetDesktopWindow", {}));
  CHECK((api(m, "USER", "GetWindow", {w16(desk), w16(5)}) & 0xFFFF) == saver, "GW_CHILD of the desktop: the saver");
  CHECK((api(m, "USER", "GetWindow", {w16(saver), w16(2)}) & 0xFFFF) == pm, "GW_HWNDNEXT: Program Manager");
  CHECK((api(m, "USER", "GetWindow", {w16(pm), w16(2)}) & 0xFFFF) == 0, "Program Manager is last");
  CHECK((api(m, "USER", "GetWindow", {w16(pm), w16(0)}) & 0xFFFF) == saver &&
            (api(m, "USER", "GetWindow", {w16(saver), w16(1)}) & 0xFFFF) == pm,
        "GW_HWNDFIRST / GW_HWNDLAST");
  // PROGMAN.INI and its groups, as ADXPL40/ADXPL310 read them.
  uint32_t groups = m.rt.static_bytes("t Groups", "Groups"), progman_ini = m.rt.static_bytes("t ini", "Progman.ini");
  uint32_t empty = m.rt.static_bytes("t empty", "");
  uint16_t n = uint16_t(api(m, "KERNEL", "GetPrivateProfileString", {l16(groups), l16(0), l16(empty), l16(buf), w16(200), l16(progman_ini)}));
  CHECK(n > 0 && m.rt.read_str(buf) == "Group1", "[Groups] keys (%u)", n);
  uint32_t g1 = m.rt.static_bytes("t Group5", "Group5");
  api(m, "KERNEL", "GetPrivateProfileString", {l16(groups), l16(g1), l16(empty), l16(buf), w16(80), l16(progman_ini)});
  std::string grp_path = m.rt.read_str(buf);
  CHECK(grp_path == "C:\\WINDOWS\\AFTERDRK.GRP", "Group5 = %s", grp_path.c_str());
  uint16_t hf = uint16_t(api(m, "KERNEL", "_lopen", {l16(buf), w16(0)}));
  CHECK(hf != 0xFFFF, "the group file opens");
  api(m, "KERNEL", "_llseek", {w16(hf), l16(0x16), w16(0)});
  api(m, "KERNEL", "_lread", {w16(hf), l16(buf + 0x80), w16(2)});
  api(m, "KERNEL", "_llseek", {w16(hf), l16(m.rt.rd16(buf + 0x80)), w16(0)});
  api(m, "KERNEL", "_lread", {w16(hf), l16(buf + 0x80), w16(40)});
  api(m, "KERNEL", "_lclose", {w16(hf)});
  CHECK(m.rt.read_str(buf + 0x80) == "After Dark", "the group's name through pName at 0x16 (%s)",
        m.rt.read_str(buf + 0x80).c_str());
  // Guest counts and offsets never size a host allocation (dos16.cc
  // DosFiles::read/write): a 4 GB _hread of a 5-byte file reads its 5 bytes,
  // and a write 2 GB out is a full disk (0 bytes written), not a 2 GB buffer.
  {
    uint32_t path = m.rt.static_bytes("t big", "C:\\WINDOWS\\BIG.DAT"), text = m.rt.static_bytes("t hello", "hello");
    uint16_t hb = uint16_t(api(m, "KERNEL", "_lcreat", {l16(path), w16(0)}));
    CHECK(hb != 0xFFFF, "_lcreat C:\\WINDOWS\\BIG.DAT");
    CHECK((api(m, "KERNEL", "_lwrite", {w16(hb), l16(text), w16(5)}) & 0xFFFF) == 5, "_lwrite 5 bytes");
    api(m, "KERNEL", "_llseek", {w16(hb), l16(0), w16(0)});
    uint32_t got = api(m, "KERNEL", "_hread", {w16(hb), l16(buf + 0xC0), l16(0xFFFFFFF0u)});
    CHECK(got == 5 && m.rt.read_str(buf + 0xC0).compare(0, 5, "hello") == 0, "_hread of 4 GB reads the 5 bytes (%u)",
          got);
    CHECK(api(m, "KERNEL", "_llseek", {w16(hb), l16(0x7FFFFF00u), w16(0)}) == 0x7FFFFF00u, "_llseek 2 GB out");
    CHECK((api(m, "KERNEL", "_lwrite", {w16(hb), l16(text), w16(1)}) & 0xFFFF) == 0, "a write there: disk full");
    api(m, "KERNEL", "_lclose", {w16(hb)});
  }
  std::string gf = progman_group_file("Games");
  uint16_t sum = 0;
  for (size_t i = 0; i + 1 < gf.size(); i += 2) sum = uint16_t(sum + (uint8_t(gf[i]) | (uint8_t(gf[i + 1]) << 8)));
  CHECK(gf.compare(0, 4, "PMCC") == 0 && sum == 0 && gf.size() % 2 == 0, "GROUPHEADER: PMCC, checksum, even size");
  // Icons: CopyIcon makes a new handle, DestroyIcon ends it; DrawIcon paints the opaque pixels.
  uint16_t copy = uint16_t(api(m, "USER", "CopyIcon", {w16(0), w16(icon)}));
  CHECK(copy && copy != icon, "CopyIcon: a new handle");
  CHECK((api(m, "USER", "DestroyIcon", {w16(copy)}) & 0xFFFF) == 1, "DestroyIcon");
  CHECK((api(m, "USER", "DestroyCursor", {w16(0x0F00)}) & 0xFFFF) == 1, "DestroyCursor");
  api(m, "USER", "DrawIcon", {w16(hdc), w16(10), w16(8), w16(icon)});
  GdiFlush();
  const auto& pal = screen.palette();
  auto rgb_at = [&](int x, int y) {
    const RGBQUAD& q = pal[screen.at(x, y)];
    return RGB(q.rgbRed, q.rgbGreen, q.rgbBlue);
  };
  CHECK(screen.at(10, 8) == 0 && screen.at(10 + 31, 8 + 31) == 0, "transparent corners stay as they were");
  CHECK(rgb_at(10 + 10, 8 + 5) == RGB(0, 0, 128), "the title bar is navy (%06lX)", rgb_at(10 + 10, 8 + 5));
  CHECK(rgb_at(10 + 1, 8 + 3) == RGB(0, 0, 0) && rgb_at(10 + 7, 8 + 13) == RGB(255, 0, 0),
        "the frame black, a program icon red");
  // CreateIcon: a 16×16 1-bpp image, WORD-aligned rows: a white box on a clear border.
  uint16_t bits = m.data(256);
  uint32_t andp = uint32_t(bits) << 16, xorp = andp + 64;
  for (uint32_t y = 0; y < 16; y++) {
    bool inner = y >= 4 && y < 12;
    m.rt.wr16(andp + 2 * y, inner ? 0xF00F : 0xFFFF);  // bytes F0 0F: x 4..11 opaque (MSB first)
    m.rt.wr16(xorp + 2 * y, inner ? 0xF00F : 0x0000);
  }
  // Rows are bytes: set them explicitly so byte order is unambiguous.
  for (uint32_t y = 4; y < 12; y++) {
    m.rt.wr8(andp + 2 * y, 0xF0);
    m.rt.wr8(andp + 2 * y + 1, 0x0F);
    m.rt.wr8(xorp + 2 * y, 0x0F);
    m.rt.wr8(xorp + 2 * y + 1, 0xF0);
  }
  uint16_t made = uint16_t(api(m, "USER", "CreateIcon", {w16(0), w16(16), w16(16), w16(1), w16(1), l16(andp), l16(xorp)}));
  CHECK(made != 0, "CreateIcon");
  api(m, "USER", "DrawIcon", {w16(hdc), w16(40), w16(20), w16(made)});
  GdiFlush();
  CHECK(rgb_at(40 + 6, 20 + 6) == RGB(255, 255, 255) && screen.at(40 + 1, 20 + 1) == 0 && screen.at(40 + 6, 20 + 1) == 0,
        "CreateIcon + DrawIcon: the white box drawn, the clear border left");
  // LoadIcon of a system icon keeps its image-less handle; ExtractIcon of a file that is not there, 0.
  CHECK((api(m, "USER", "LoadIcon", {w16(0), l16(32512)}) & 0xFFFF) == 0x0F04, "LoadIcon(NULL, IDI_APPLICATION)");
  CHECK((api(m, "SHELL", "ExtractIcon", {w16(0), l16(m.rt.static_bytes("t x", "C:\\NOPE.EXE")), w16(0)}) & 0xFFFF) == 0,
        "ExtractIcon of a missing file");
}

void test_gdi_extras() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  CHECK(api(m, "GDI", "GetDCOrg", {w16(hdc)}) == 0, "GetDCOrg of the saver window's DC: (0, 0)");
  // A child window at (10, 20): GetDC gives a DC whose origin is its corner.
  uint32_t cls = m.rt.static_bytes("t STATIC", "STATIC");
  uint16_t child = uint16_t(api(m, "USER", "CreateWindow", {l16(cls), l16(cls), l16(WS_CHILD | WS_VISIBLE), w16(10),
                                                            w16(20), w16(16), w16(8), w16(saver), w16(0), w16(0),
                                                            l16(0)}));
  uint16_t cdc = uint16_t(api(m, "USER", "GetDC", {w16(child)}));
  CHECK(api(m, "GDI", "GetDCOrg", {w16(cdc)}) == ((20u << 16) | 10u), "GetDCOrg of a child's DC: its corner (%08X)",
        api(m, "GDI", "GetDCOrg", {w16(cdc)}));
  api(m, "USER", "ReleaseDC", {w16(child), w16(cdc)});
  // CreateBrushIndirect: solid, hollow, hatched; a solid one paints its colour.
  uint16_t ds = m.data(64);
  uint32_t lb = uint32_t(ds) << 16;
  auto brush = [&](uint16_t style, uint32_t color, uint16_t hatch) {
    m.rt.wr16(lb, style);
    m.rt.wr32(lb + 2, color);
    m.rt.wr16(lb + 6, hatch);
    return uint16_t(api(m, "GDI", "CreateBrushIndirect", {l16(lb)}));
  };
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t solid = brush(BS_SOLID, RGB(255, 0, 0), 0);
  uint16_t hollow = brush(BS_NULL, 0, 0);
  uint16_t hatched = brush(BS_HATCHED, RGB(0, 0, 255), HS_CROSS);
  CHECK(g.get(solid, G16::brush) && g.get(hollow, G16::brush) && g.get(hatched, G16::brush), "three brushes");
  CHECK(g.get(hollow, G16::brush)->style == BS_NULL && g.get(hatched, G16::brush)->hatch == HS_CROSS, "styles kept");
  g.select(hdc, solid);
  g.sync(hdc);
  PatBlt(g.host_dc(hdc), 0, 0, 4, 4, PATCOPY);
  GdiFlush();
  const RGBQUAD& q = screen.palette()[screen.at(1, 1)];
  CHECK(q.rgbRed == 255 && q.rgbGreen == 0 && q.rgbBlue == 0, "the solid brush paints red");
  uint16_t bmp = g.create_device_bitmap(8, 8, 8);
  uint16_t pat = brush(BS_PATTERN, 0, bmp);
  CHECK(g.get(pat, G16::brush) && g.get(pat, G16::brush)->pattern == bmp, "BS_PATTERN keeps its bitmap");
}

// A module that swaps palettes with its pen and brush selected (ARTIST calls
// USER.SelectPalette ~90 times a frame between two palettes while it strokes)
// re-keys them on every swap. Each key colour's real object is made once and
// reused: the process's GDI object count stays flat, where every swap used to
// make two new ones (a host at the 10,000-object quota within seconds).
void test_gdi_palette_swap() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t hdc = gdi16_screen_dc(m.rt, saver);
  Gdi16& g = m.rt.state<Gdi16>();
  uint16_t red = g.create_palette({{255, 0, 0, 0}, {0, 255, 0, 0}});
  uint16_t blue = g.create_palette({{0, 0, 255, 0}, {255, 255, 0, 0}});
  uint16_t pen = g.create_pen(PS_SOLID, 1, 0x01000000);  // PALETTEINDEX(0)
  uint16_t brush = g.create_brush(0x01000001);           // PALETTEINDEX(1)
  g.select(hdc, pen);
  g.select(hdc, brush);
  auto select_palette = [&](uint16_t pal) { api(m, "USER", "SelectPalette", {w16(hdc), w16(pal), w16(0)}); };
  select_palette(red);
  COLORREF pen_red = g.get(pen, G16::pen)->made_for;
  select_palette(blue);
  COLORREF pen_blue = g.get(pen, G16::pen)->made_for;
  CHECK(pen_red != pen_blue, "the two palettes key PALETTEINDEX(0) differently (%06lX, %06lX)", pen_red, pen_blue);
  select_palette(red);
  HGDIOBJ real_red = g.host(pen);
  const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  for (int i = 0; i < 5000; i++) {
    select_palette(blue);
    select_palette(red);
  }
  const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  CHECK(after <= before + 2, "10,000 palette swaps: GDI objects %lu -> %lu", before, after);
  CHECK(g.host(pen) == real_red && g.get(pen, G16::pen)->made_for == pen_red, "the pen is its first red object again");
  CHECK(GetCurrentObject(g.host_dc(hdc), OBJ_PEN) == real_red, "and that object is selected into the real DC");
  // The pen draws the colour of the palette selected now.
  PatBlt(g.host_dc(hdc), 0, 0, 64, 48, BLACKNESS);
  MoveToEx(g.host_dc(hdc), 0, 5, nullptr);
  LineTo(g.host_dc(hdc), 20, 5);
  select_palette(blue);
  MoveToEx(g.host_dc(hdc), 0, 9, nullptr);
  LineTo(g.host_dc(hdc), 20, 9);
  GdiFlush();
  const RGBQUAD& r = screen.palette()[screen.at(4, 5)];
  const RGBQUAD& b = screen.palette()[screen.at(4, 9)];
  CHECK(r.rgbRed > 128 && r.rgbBlue < 128, "drawn under the red palette: red (%u,%u,%u)", r.rgbRed, r.rgbGreen, r.rgbBlue);
  CHECK(b.rgbBlue > 128 && b.rgbRed < 128, "drawn under the blue palette: blue (%u,%u,%u)", b.rgbRed, b.rgbGreen, b.rgbBlue);
  // Deleting the guest objects frees every realization.
  api(m, "GDI", "SelectObject", {w16(hdc), w16(g.stock(BLACK_PEN))});
  api(m, "GDI", "SelectObject", {w16(hdc), w16(g.stock(WHITE_BRUSH))});
  const DWORD selected_out = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  api(m, "GDI", "DeleteObject", {w16(pen)});
  api(m, "GDI", "DeleteObject", {w16(brush)});
  // (Solid brushes may stay in GDI's per-process brush cache: count the pens.)
  const DWORD freed = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  CHECK(freed + 2 <= selected_out, "deleting the pen frees both its real objects (%lu -> %lu)", selected_out, freed);
}

void test_seeds() {
  Machine m;
  auto ray = [&]() {
    return profiles16(m.rt).get("C:\\WINDOWS\\MODULES.INI", "Ray", "RaySceneFile").value_or(std::string());
  };
  CHECK(ray() == "C:\\AFTERDRK\\TRACES\\ROTCUBE.TRC", "nothing mounted: ROTCUBE (%s)", ray().c_str());
  CHECK(profiles16(m.rt).get("C:\\WINDOWS\\MODULES.INI", "Logo Section", "LogoFile").value_or("") ==
            "C:\\AFTERDRK\\BITMAPS\\ADLOGO.BMP",
        "[Logo Section] LogoFile");
  // The seeds are profile seeds: the file itself exists (OF_EXIST finds it) and is empty.
  win32::Vfs::Stat st;
  CHECK(m.rt.vfs().stat("C:\\WINDOWS\\MODULES.INI", &st) && !st.dir && st.size == 0, "MODULES.INI: an empty virtual file");
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string dir = std::string(base) + "adw_win16_seed_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  CreateDirectoryA((dir + "\\TRACES").c_str(), nullptr);
  auto touch = [&](const char* f) {
    FILE* fh = fopen((dir + "\\TRACES\\" + f).c_str(), "wb");
    fclose(fh);
  };
  touch("ROTPYRA.TRC");
  touch("DIAMOND.TRC");
  m.rt.vfs().mount("C:\\AFTERDRK", dir, false);
  seed_modules_ini(m.rt);
  CHECK(ray() == "C:\\AFTERDRK\\TRACES\\DIAMOND.TRC", "AD 3.2's scenes: DIAMOND first (%s)", ray().c_str());
  touch("ROTCUBE.TRC");
  seed_modules_ini(m.rt);
  CHECK(ray() == "C:\\AFTERDRK\\TRACES\\ROTCUBE.TRC", "Deluxe's: ROTCUBE (%s)", ray().c_str());
  for (const char* f : {"ROTPYRA.TRC", "DIAMOND.TRC", "ROTCUBE.TRC"}) DeleteFileA((dir + "\\TRACES\\" + f).c_str());
  RemoveDirectoryA((dir + "\\TRACES").c_str());
  RemoveDirectoryA(dir.c_str());
}

// LoadBitmap(NULL, OBM_*) and the caption metrics: INS draws a Windows 3.1
// window's chrome with them (system-menu box, minimize/maximize, scroll
// arrows) and stopped with "Out of memory" when OBM_CLOSE was 0.
void test_system_bitmaps() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  auto metric = [&](int i) { return api(m, "USER", "GetSystemMetrics", {w16(uint16_t(i))}) & 0xFFFF; };
  CHECK(metric(SM_CXSIZE) == 18 && metric(SM_CYSIZE) == 18, "SM_CXSIZE/SM_CYSIZE 18 (%u %u)", metric(SM_CXSIZE),
        metric(SM_CYSIZE));
  CHECK(metric(SM_CYCAPTION) == metric(SM_CYSIZE) + 2, "the caption holds its bitmaps and two border lines");
  CHECK(metric(SM_CYVTHUMB) == metric(SM_CYVSCROLL) && metric(SM_CXHTHUMB) == metric(SM_CXHSCROLL), "thumbs");
  CHECK(metric(SM_CXICONSPACING) == 75 && metric(SM_CYICONSPACING) == 75, "icon spacing");
  Gdi16& g = m.rt.state<Gdi16>();
  auto obm = [&](uint16_t id) { return uint16_t(api(m, "USER", "LoadBitmap", {w16(0), l16(id)})); };
  for (uint16_t id : {uint16_t(32754), uint16_t(32749), uint16_t(32748), uint16_t(32747)}) {
    uint16_t b = obm(id);
    Obj16* o = g.get(b, G16::bitmap);
    CHECK(o && o->bmp.w == 18 && o->bmp.h == 18, "OBM %u: an 18x18 bitmap (%04X)", id, b);
  }
  for (uint16_t id : {uint16_t(32753), uint16_t(32752), uint16_t(32751), uint16_t(32750)}) {
    Obj16* o = g.get(obm(id), G16::bitmap);
    CHECK(o && o->bmp.w == 16 && o->bmp.h == 16, "scroll arrow OBM %u: 16x16", id);
  }
  CHECK(obm(32767) == 0, "an OBM this host does not draw stays 0");
  // OBM_REDUCE: a raised grey face (white top-left, black bottom-right edge) with a black glyph.
  Obj16* o = g.get(obm(32749), G16::bitmap);
  if (o && o->bmp.bits) {
    auto rgb = [&](int x, int y) { return g.display().hardware_color(o->bmp.bits[size_t(y) * o->bmp.stride + size_t(x)]); };
    CHECK(rgb(0, 0) == RGB(255, 255, 255) && rgb(17, 17) == RGB(0, 0, 0) && rgb(3, 3) == RGB(192, 192, 192),
          "OBM_REDUCE edges and face (%06lX %06lX %06lX)", rgb(0, 0), rgb(17, 17), rgb(3, 3));
    int black = 0;
    for (int y = 4; y < 14; y++)
      for (int x = 4; x < 14; x++) black += rgb(x, y) == RGB(0, 0, 0);
    CHECK(black >= 12, "OBM_REDUCE has its triangle (%d black pixels)", black);
  }
}

// Runtime16's frame-bounded time (runtime16.hh): with insns_per_us set, the
// work of a frame runs inside its period — reads within a frame advance by
// their nudges, the next frame starts at its grid time again, and only work
// past the period pushes the clock on. insns_per_us 0 keeps the accumulating
// nudges on the core clock.
void test_frame_time() {
  {
    Machine m;  // read_step_us 5, 100 MIPS, 500 per API call; frames of 16667 µs
    m.clock.begin_frame();
    uint64_t a = m.rt.clock_us(), b = m.rt.clock_us();
    CHECK(a == 5 && b == 10, "reads in frame 0 nudge 5 µs each (%llu %llu)", (unsigned long long)a,
          (unsigned long long)b);
    for (int i = 0; i < 20; i++) m.rt.clock_us();
    m.clock.begin_frame();
    uint64_t c = m.rt.clock_us();
    CHECK(c == 16667 + 5, "frame 1 starts on the grid: earlier nudges do not carry over (%llu)", (unsigned long long)c);
    // An API call's cost reaches the next read: 500 instructions = 5 µs.
    api(m, "USER", "GetTickCount", {});
    uint64_t d = m.rt.clock_us();
    CHECK(d >= c + 10, "an API call's cost moves the clock (%llu -> %llu)", (unsigned long long)c,
          (unsigned long long)d);
    // Work past the period pushes the clock beyond the grid, and the next frame continues from there.
    while (m.rt.clock_us() < 2 * 16667 + 1000) {
    }
    uint64_t e = m.rt.peek_us();
    m.clock.begin_frame();
    uint64_t f = m.rt.clock_us();
    CHECK(f > e && f < 2 * 16667 + 1100, "an overrun frame's time carries on (%llu after %llu)", (unsigned long long)f,
          (unsigned long long)e);
    CHECK(m.rt.modeled_time(), "headless with insns_per_us: modeled");
  }
  {
    VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
    Runtime16Options o;
    o.insns_per_us = 0;
    Runtime16 rt{o, clock};
    clock.set_read_step_us(5);
    clock.begin_frame();
    for (int i = 0; i < 20; i++) rt.clock_us();
    clock.begin_frame();
    uint64_t c = rt.clock_us();
    CHECK(!rt.modeled_time() && c == 16667 + 21 * 5, "insns_per_us 0: the nudges accumulate (%llu)",
          (unsigned long long)c);
  }
  // settle_time(): a frame's work without a clock read is charged inside
  // that frame, not on top of the next frame's grid line at the next read.
  {
    Machine m;
    m.clock.begin_frame();
    // mov cx, 0x4000; loop $; retf — some 16K instructions (~164 µs at 100 MIPS), no clock read.
    uint16_t cs = m.code({0xB9, 0x00, 0x40, 0xE2, 0xFE, 0xCB});
    m.rt.call_far(uint32_t(cs) << 16, {});
    m.rt.settle_time();
    m.clock.begin_frame();
    uint64_t t = m.rt.clock_us();
    CHECK(t == 16667 + 5, "settled: frame 1's first read is on its grid line (%llu)", (unsigned long long)t);
    m.rt.call_far(uint32_t(cs) << 16, {});
    m.clock.begin_frame();
    uint64_t u = m.rt.clock_us();
    CHECK(u > 2 * 16667 + 100, "unsettled: the previous frame's work lands on this frame's grid line (%llu)",
          (unsigned long long)u);
  }
  // A streamed (realtime) clock only runs from frame 0: until start_frames()
  // clock reads are modeled (init's calibration loops must see time move —
  // EINSTEIN, GLOBE and Om Appliances spun forever), then they follow the wall
  // clock plus the time init took, never going back.
  {
    uint64_t wall = 1'000'000;
    VirtualClock clock{VirtualClock::Mode::realtime, 16667, [&] { return wall; }};
    Runtime16 rt{Runtime16Options{}, clock};
    clock.set_read_step_us(0);  // what the lane sets with insns_per_us
    CHECK(rt.modeled_time(), "realtime before the first frame: modeled");
    uint64_t a = rt.clock_us(), b = rt.clock_us();
    CHECK(b > a && a > 0, "reads during init advance (%llu %llu)", (unsigned long long)a, (unsigned long long)b);
    for (int i = 0; i < 1000; i++) rt.clock_us();
    uint64_t c = rt.clock_us();
    clock.begin_frame();
    rt.start_frames();
    CHECK(!rt.modeled_time(), "realtime after start_frames: the wall clock");
    uint64_t d = rt.clock_us();
    wall += 20000;
    uint64_t e = rt.clock_us();
    CHECK(d >= c && d < c + 100, "no jump back at the first frame (%llu after %llu)", (unsigned long long)d,
          (unsigned long long)c);
    CHECK(e == d + 20000, "then it follows the wall (%llu -> %llu)", (unsigned long long)d, (unsigned long long)e);
  }
  // set_deadline(): the hook runs once, at the first API call at or after the deadline.
  {
    Machine m;
    m.clock.begin_frame();
    int fired = 0;
    m.rt.set_deadline(1000, [&] { fired++; });
    api(m, "USER", "GetTickCount", {});
    CHECK(fired == 0, "before the deadline: no hook");
    while (m.rt.clock_us() < 1000) {
    }
    api(m, "USER", "GetTickCount", {});
    api(m, "USER", "GetTickCount", {});
    CHECK(fired == 1, "at the deadline: the hook ran once (%d)", fired);
  }
}

// adw::cpu regression, found by this lane: `inc`/`dec` must leave DF alone.
// X86Exec.cc's inc/dec r16/r32 and FE/FF forms once passed ~Regs::CF as the
// flag mask, which rewrote every other EFLAGS bit — DF included — so a
// backward memmove (`std; … dec si; dec di; rep movsw`, ADXPL300/AD_RSRC) ran
// forward and faulted past its source block (fixed with
// Regs::default_int_flags & ~Regs::CF; this pins it).
int run_cpu_df() {
  Machine m;
  // std; mov si,5; dec si; inc di; pushf; pop ax; cld; retf
  std::vector<uint8_t> code = {0xFD, 0xBE, 0x05, 0x00, 0x4E, 0x47, 0x9C, 0x58, 0xFC, 0xCB};
  uint32_t r = m.rt.call_far(uint32_t(m.code(code)) << 16, {});
  CHECK((r & 0x0400) != 0, "DF survives dec si / inc di (flags %04X)", r & 0xFFFF);
  // FE/FF forms: std; mov bx,1; dec bx (FF CB); pushf; pop ax; cld; retf
  std::vector<uint8_t> code2 = {0xFD, 0xBB, 0x01, 0x00, 0xFF, 0xCB, 0x9C, 0x58, 0xFC, 0xCB};
  r = m.rt.call_far(uint32_t(m.code(code2)) << 16, {});
  CHECK((r & 0x0400) != 0, "DF survives dec r/m16 (flags %04X)", r & 0xFFFF);
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- interaction (INTERACTION.md §5.2, §6.2 Win16, §7) ------------------------------------------------------

void put16(std::string& s, uint16_t v) {
  s.push_back(char(v));
  s.push_back(char(v >> 8));
}
void put32(std::string& s, uint32_t v) {
  put16(s, uint16_t(v));
  put16(s, uint16_t(v >> 16));
}
uint16_t le16(const std::vector<uint8_t>& b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t le32(const std::vector<uint8_t>& b, size_t o) { return le16(b, o) | (uint32_t(le16(b, o + 2)) << 16); }
std::wstring wstr_at(const std::vector<uint8_t>& b, size_t* o) {
  std::wstring w;
  while (*o + 1 < b.size() && le16(b, *o)) {
    w.push_back(wchar_t(le16(b, *o)));
    *o += 2;
  }
  *o += 2;
  return w;
}

// A Win16 DLGTEMPLATE, byte by byte: DS_SETFONT, a named menu (dropped), a
// custom dialog class (dropped), a caption with a 1252 character, 8 pt Helv;
// a predefined button, a custom-class item, an SS_ICON static (its icon
// ordinal is the guest's: dropped) and an item with two extra bytes.
std::string win16_template() {
  std::string t;
  put32(t, WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFONT | DS_SYSMODAL);
  t.push_back(4);
  put16(t, 10), put16(t, 20), put16(t, 200), put16(t, 100);
  t += "MYMENU";
  t.push_back('\0');
  t += "MyDlgClass";
  t.push_back('\0');
  t += "Caf\xE9 Settings";
  t.push_back('\0');
  put16(t, 8);
  t += "Helv";
  t.push_back('\0');
  auto item = [&](int16_t x, uint16_t id, uint32_t style, const std::string& cls, bool atom, const std::string& text,
                  bool ordinal, std::string extra) {
    put16(t, uint16_t(x)), put16(t, 5), put16(t, 50), put16(t, 14), put16(t, id);
    put32(t, style);
    if (atom) {
      t.push_back(cls[0]);
    } else {
      t += cls;
      t.push_back('\0');
    }
    if (ordinal) {
      t.push_back(char(0xFF));
      put16(t, uint16_t(atoi(text.c_str())));
    } else {
      t += text;
      t.push_back('\0');
    }
    t.push_back(char(extra.size()));
    t += extra;
  };
  item(1, IDOK, WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, "\x80", true, "&OK", false, "");
  item(2, 102, WS_CHILD | WS_VISIBLE, "LunaticKey", false, "", false, "");
  item(3, 103, WS_CHILD | WS_VISIBLE | SS_ICON, "\x82", true, "100", true, "");
  item(4, 104, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, "EDIT", false, "abc", false, std::string("\x01\x02", 2));
  return t;
}

void test_template_converter() {
  std::string t16 = win16_template();
  DialogTemplate32 t = convert_dialog_template16(t16, WS_EX_TOOLWINDOW);
  CHECK(t.ok, "the template converts (%s)", t.error.c_str());
  if (!t.ok) return;
  const std::vector<uint8_t>& b = t.bytes;
  CHECK(le32(b, 0) == (WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_SETFONT), "style kept, DS_SYSMODAL dropped (%08X)",
        le32(b, 0));
  CHECK(le32(b, 4) == WS_EX_TOOLWINDOW && le16(b, 8) == 4, "extended style, 4 items");
  CHECK(int16_t(le16(b, 10)) == 10 && le16(b, 12) == 20 && le16(b, 14) == 200 && le16(b, 16) == 100, "x/y/cx/cy");
  CHECK(le16(b, 18) == 0 && le16(b, 20) == 0, "no menu, no dialog class");
  size_t o = 22;
  std::wstring cap = wstr_at(b, &o);
  CHECK(cap == L"Caf\u00E9 Settings", "the caption from code page 1252");
  uint16_t pt = le16(b, o);
  o += 2;
  CHECK(pt == 8 && wstr_at(b, &o) == L"Helv", "8 pt Helv");
  // Items: DWORD aligned; style, exstyle, x, y, cx, cy, id, class, title, creation data.
  struct Item {
    uint32_t style;
    uint16_t id, atom;
    std::wstring cls, title;
    uint16_t title_ord = 0, extra = 0;
  };
  std::vector<Item> items;
  for (int i = 0; i < 4; i++) {
    o = (o + 3) & ~size_t(3);
    Item it;
    it.style = le32(b, o);
    CHECK(le32(b, o + 4) == 0, "item %d: no extended style", i);
    it.id = le16(b, o + 16);
    o += 18;
    if (le16(b, o) == 0xFFFF) {
      it.atom = le16(b, o + 2);
      o += 4;
    } else {
      it.atom = 0;
      it.cls = wstr_at(b, &o);
    }
    if (le16(b, o) == 0xFFFF) {
      it.title_ord = le16(b, o + 2);
      o += 4;
    } else {
      it.title = wstr_at(b, &o);
    }
    it.extra = le16(b, o);
    o += 2;
    items.push_back(it);
  }
  CHECK(o == b.size(), "the whole template was walked (%zu of %zu)", o, b.size());
  CHECK(items[0].atom == 0x80 && items[0].id == IDOK && items[0].title == L"&OK", "a predefined Button");
  CHECK(items[1].atom == 0 && items[1].cls == L"LunaticKey", "a custom class by name");
  CHECK(items[2].atom == 0x82 && items[2].title.empty() && items[2].title_ord == 0, "SS_ICON loses the guest's icon");
  CHECK(items[3].atom == 0x81 && items[3].title == L"abc" && items[3].extra == 0, "\"EDIT\" by name is the Edit atom; "
        "the guest's extra bytes are dropped");
  CHECK(t.classes.size() == 1 && t.classes[0] == "LunaticKey", "custom classes reported");
  // Windows takes it: a real (invisible) dialog from the converted template.
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"LunaticKey";
  RegisterClassExW(&wc);
  HWND dlg = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(b.data()), nullptr,
                                        [](HWND, UINT, WPARAM, LPARAM) -> INT_PTR { return FALSE; }, 0);
  CHECK(dlg != nullptr, "CreateDialogIndirectParamW accepts it (error %lu)", GetLastError());
  if (dlg) {
    wchar_t buf[32] = {};
    GetDlgItemTextW(dlg, 104, buf, 32);
    CHECK(GetDlgItem(dlg, IDOK) && GetDlgItem(dlg, 102) && std::wstring(buf) == L"abc", "its items exist");
    DestroyWindow(dlg);
  }
  UnregisterClassW(L"LunaticKey", GetModuleHandleW(nullptr));
  // Truncated input fails cleanly.
  CHECK(!convert_dialog_template16(t16.substr(0, 20)).ok, "a truncated header is refused");
  CHECK(!convert_dialog_template16(t16.substr(0, t16.size() - 3)).ok, "a truncated item is refused");
}

void test_message_table() {
  // Win16's WM_USER-based control messages, by class (§6.2).
  CHECK(msg16_to_32(Ctl16::edit, WM_USER + 1) == EM_SETSEL && msg16_to_32(Ctl16::edit, WM_USER + 0x15) == EM_LIMITTEXT,
        "EM_SETSEL, EM_LIMITTEXT");
  CHECK(msg16_to_32(Ctl16::edit, WM_USER + 0x14) == EM_GETLINE && msg16_to_32(Ctl16::edit, WM_USER + 0x12) == EM_REPLACESEL,
        "EM_GETLINE, EM_REPLACESEL");
  CHECK(msg16_to_32(Ctl16::edit, WM_USER + 0x0C) == 0 && msg16_to_32(Ctl16::edit, WM_USER + 0x0D) == 0,
        "EM_SETHANDLE/EM_GETHANDLE: no Win32 form");
  CHECK(msg16_to_32(Ctl16::button, WM_USER + 1) == BM_SETCHECK && msg16_to_32(Ctl16::button, WM_USER + 0) == BM_GETCHECK,
        "BM_SETCHECK, BM_GETCHECK");
  CHECK(msg16_to_32(Ctl16::listbox, WM_USER + 1) == LB_ADDSTRING && msg16_to_32(Ctl16::listbox, WM_USER + 0x0A) == LB_GETTEXT &&
            msg16_to_32(Ctl16::listbox, WM_USER + 0x0E) == LB_DIR && msg16_to_32(Ctl16::listbox, WM_USER + 0x23) == LB_FINDSTRINGEXACT,
        "LB_ADDSTRING, LB_GETTEXT, LB_DIR, LB_FINDSTRINGEXACT");
  CHECK(msg16_to_32(Ctl16::listbox, WM_USER) == 0, "WM_USER itself is no list-box message");
  CHECK(msg16_to_32(Ctl16::combobox, WM_USER + 3) == CB_ADDSTRING && msg16_to_32(Ctl16::combobox, WM_USER + 0x0E) == CB_SETCURSEL &&
            msg16_to_32(Ctl16::combobox, WM_USER + 8) == CB_GETLBTEXT,
        "CB_ADDSTRING, CB_SETCURSEL, CB_GETLBTEXT");
  CHECK(msg16_to_32(Ctl16::statik, WM_USER) == 0 && msg16_to_32(Ctl16::scrollbar, WM_USER + 1) == 0,
        "static icons and scroll bars: none");
  CHECK(msg16_to_32(Ctl16::other, WM_USER + 7) == WM_USER + 7 && msg16_to_32(Ctl16::edit, WM_SETTEXT) == WM_SETTEXT,
        "dialogs' and custom controls' own messages, and standard ones, pass");
  for (Ctl16 k : {Ctl16::button, Ctl16::edit, Ctl16::listbox, Ctl16::combobox}) {
    for (uint16_t m = WM_USER; m < WM_USER + 0x30; m++) {
      uint32_t w = msg16_to_32(k, m);
      if (w) CHECK(msg32_to_16(k, w) == m, "round trip %04X", m);
    }
  }
  CHECK(control_kind16(L"ComboBox") == Ctl16::combobox && control_kind16(L"COMBOLBOX") == Ctl16::listbox &&
            control_kind16(L"button") == Ctl16::button && control_kind16(L"LunaticKey") == Ctl16::other,
        "classes by real name");
}

void test_keyboard_tables() {
  CHECK(vk_scan_code('A') == 0x1E && vk_scan_code('1') == 0x02 && vk_scan_code(VK_SPACE) == 0x39 &&
            vk_scan_code(VK_LEFT) == 0x4B,
        "US scan codes");
  CHECK(key_lparam(VK_LEFT, true, false) == (1u | (0x4Bu << 16) | (1u << 24)), "key down: repeat 1, scan, extended");
  CHECK(key_lparam('A', false, true) == (1u | (0x1Eu << 16) | (1u << 30) | (1u << 31)), "key up: previous + transition");
  CHECK(key_lparam('A', true, true) & (1u << 30), "auto-repeat: previous state");
  CHECK(vk_to_char('A', false, false) == 'a' && vk_to_char('A', true, false) == 'A' && vk_to_char('A', false, true) == 'A' &&
            vk_to_char('A', true, true) == 'a',
        "letters with Shift and Caps Lock");
  CHECK(vk_to_char('1', true, false) == '!' && vk_to_char(VK_OEM_2, true, false) == '?' && vk_to_char(VK_F1, false, false) == -1,
        "shifted symbols; F1 has none");
}

// Hook procedures as host thunks: each records its call, and either chains
// (DefHookProc / CallNextHookEx) or answers.
void test_hooks() {
  Machine m;
  std::vector<std::string> calls;
  uint32_t token_old = 0, token_new = 0;
  uint16_t ds = m.data(64);
  uint32_t token_slot = uint32_t(ds) << 16;
  m.rt.shims().add("TESTHOOK", 1, "OLDHOOK", Conv16::pascal_, false, 8, [&](Call16& c) {
    int16_t code = c.sw();
    uint16_t vk = c.w();
    c.l();
    calls.push_back("old(" + std::to_string(code) + "," + std::to_string(vk) + ")");
    c.ret32(vk == 'X' ? 1 : 0);  // the older hook eats X
  });
  m.rt.shims().add("TESTHOOK", 2, "NEWHOOK", Conv16::pascal_, false, 8, [&](Call16& c) {
    int16_t code = c.sw();
    uint16_t vk = c.w();
    uint32_t lp = c.l();
    calls.push_back("new(" + std::to_string(vk) + ")");
    // Win 3.0 style: DefHookProc(code, wParam, lParam, &lpfnNext).
    uint32_t r = api(m, "USER", "DefHookProc", {w16(uint16_t(code)), w16(vk), l16(lp), l16(token_slot)});
    c.ret32(r);
  });
  uint32_t old_proc = m.rt.thunk_far(*m.rt.shims().find_name("TESTHOOK", "OLDHOOK"));
  uint32_t new_proc = m.rt.thunk_far(*m.rt.shims().find_name("TESTHOOK", "NEWHOOK"));
  CHECK(!user16_has_keyboard_hook(m.rt), "no hook yet");
  CHECK(api(m, "USER", "SetWindowsHook", {w16(WH_MOUSE), l16(old_proc)}) == 0, "other hook kinds are refused");
  token_old = api(m, "USER", "SetWindowsHook", {w16(WH_KEYBOARD), l16(old_proc)});
  token_new = api(m, "USER", "SetWindowsHookEx", {w16(WH_KEYBOARD), l16(new_proc), w16(0), w16(0)});
  m.rt.wr32(token_slot, token_new);
  CHECK(token_old && token_new && token_old != token_new && user16_has_keyboard_hook(m.rt), "two keyboard hooks");
  CHECK(!user16_keyboard_hooks(m.rt, 'A', key_lparam('A', true, false), 1), "A goes through (0)");
  CHECK(calls.size() == 2 && calls[0] == "new(65)" && calls[1] == "old(0,65)",
        "most recent first, DefHookProc reaches the older one (%s %s)", calls.size() > 0 ? calls[0].c_str() : "",
        calls.size() > 1 ? calls[1].c_str() : "");
  calls.clear();
  CHECK(user16_keyboard_hooks(m.rt, 'X', key_lparam('X', true, false), 2), "X is consumed (the older hook's 1)");
  // CallNextHookEx from the older hook: nothing after it.
  CHECK(api(m, "USER", "CallNextHookEx", {l16(token_old), w16(0), w16('Q'), l16(0)}) == 0, "the end of the chain: 0");
  // Unhook the newer by token, the older by (kind, proc).
  CHECK((api(m, "USER", "UnhookWindowsHookEx", {l16(token_new)}) & 0xFFFF) == 1, "UnhookWindowsHookEx");
  calls.clear();
  user16_keyboard_hooks(m.rt, 'B', key_lparam('B', true, false), 3);
  CHECK(calls.size() == 1 && calls[0] == "old(0,66)", "only the older one is left");
  CHECK((api(m, "USER", "UnhookWindowsHook", {w16(WH_KEYBOARD), l16(old_proc)}) & 0xFFFF) == 1 &&
            !user16_has_keyboard_hook(m.rt),
        "UnhookWindowsHook");
}

// The saver window's queue: tagged input, filters, consumption, drops.
void test_saver_queue() {
  Machine m;
  Screen screen(64, 48);
  m.rt.attach_display(screen);
  InputState in;
  m.rt.set_input(&in);
  uint16_t saver = user16_saver_window(m.rt);
  uint16_t ds = m.data(256);
  uint32_t msg = uint32_t(ds) << 16;
  auto peek = [&](uint16_t hwnd, uint16_t lo, uint16_t hi, uint16_t flags) {
    return api(m, "USER", "PeekMessage", {l16(msg), w16(hwnd), w16(lo), w16(hi), w16(flags)}) & 0xFFFF;
  };
  // FindWindow("Sleep", NULL): the blanker LUNATIC looks for.
  uint32_t sleep_cls = m.rt.static_bytes("t Sleep", "Sleep");
  CHECK((api(m, "USER", "FindWindow", {l16(sleep_cls), l16(0)}) & 0xFFFF) == saver, "FindWindow(\"Sleep\") = the saver");
  StepReport16 r0 = user16_end_step(m.rt);
  user16_post_input(m.rt, WM_KEYDOWN, 'A', key_lparam('A', true, false), 7);
  user16_post_input(m.rt, WM_KEYUP, 'A', key_lparam('A', false, true), 8);
  // A module's own posted message comes first.
  api(m, "USER", "PostMessage", {w16(saver), w16(WM_USER + 5), w16(1), l16(2)});
  CHECK(peek(0, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) == 1 && m.rt.rd16(msg + 2) == WM_KEYDOWN, "a key range skips the "
        "posted message");
  CHECK(peek(0, 0, 0, PM_NOREMOVE) == 1 && m.rt.rd16(msg + 2) == WM_USER + 5, "posted before input");
  CHECK(peek(0x7777, 0, 0, PM_NOREMOVE) == 0, "another window's filter finds nothing");
  CHECK(peek(saver, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE) == 0, "a mouse range finds no key");
  // Remove the key down, translate it, keep it; dispatch the key up back.
  CHECK(peek(saver, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE) == 1 && m.rt.rd16(msg + 4) == 'A', "WM_KEYDOWN removed");
  in.keys.set(VK_SHIFT);
  CHECK((api(m, "USER", "TranslateMessage", {l16(msg)}) & 0xFFFF) == 1, "TranslateMessage");
  CHECK(peek(saver, WM_CHAR, WM_CHAR, PM_REMOVE) == 1 && m.rt.rd16(msg + 4) == 'A', "WM_CHAR 'A' (Shift) next");
  CHECK(peek(saver, WM_KEYUP, WM_KEYUP, PM_REMOVE) == 1, "WM_KEYUP removed");
  api(m, "USER", "DispatchMessage", {l16(msg)});
  StepReport16 r1 = user16_end_step(m.rt);
  CHECK(r1.consumed == 7, "the down (and its char) consumed, the dispatched up not (%llu)", (unsigned long long)r1.consumed);
  CHECK(r1.queue_reads > r0.queue_reads, "reading the saver's queue with removal for keys counts (key-filter)");
  // Untaken input is dropped after the step; the module's own message stays.
  user16_post_input(m.rt, WM_KEYDOWN, 'B', key_lparam('B', true, false), 9);
  StepReport16 r2 = user16_end_step(m.rt);
  CHECK(r2.dropped == 1 && r2.consumed == 0 && peek(0, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) == 0, "the untaken key dropped");
  CHECK(peek(0, 0, 0, PM_REMOVE) == 1 && m.rt.rd16(msg + 2) == WM_USER + 5, "the module's own message stays");
  // Kept for a suspended call that reads the queue: pending until taken.
  user16_post_input(m.rt, WM_KEYDOWN, 'C', key_lparam('C', true, false), 10);
  StepReport16 k1 = user16_end_step(m.rt, 3);
  CHECK(k1.dropped == 0 && k1.pending == 10, "kept: pending %llu", (unsigned long long)k1.pending);
  StepReport16 k2 = user16_end_step(m.rt, 3);
  CHECK(k2.dropped == 0 && k2.pending == 10, "still kept a step later");
  CHECK(peek(saver, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE) == 1 && m.rt.rd16(msg + 4) == 'C', "taken late");
  StepReport16 k3 = user16_end_step(m.rt, 3);
  CHECK(k3.consumed == 10 && k3.pending == 0, "taken late is consumed, nothing pending");
  user16_post_input(m.rt, WM_KEYDOWN, 'D', key_lparam('D', true, false), 11);
  for (int i = 0; i < 3; i++) user16_end_step(m.rt, 3);
  StepReport16 k4 = user16_end_step(m.rt, 3);
  CHECK(k4.dropped == 1 && k4.pending == 0, "dropped once kept keep_steps steps");
  // A mouse-range read with removal is no key read.
  uint64_t reads = user16_end_step(m.rt).queue_reads;
  CHECK(reads > r2.queue_reads, "the all-messages read above was one");
  peek(0, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE);
  CHECK(user16_end_step(m.rt).queue_reads == reads, "a mouse-only read is no key read");
  // WM_CLOSE posted to the saver: wake.
  CHECK(!user16_end_step(m.rt).wake, "no wake yet");
  api(m, "USER", "PostMessage", {w16(saver), w16(WM_SYSCOMMAND), w16(SC_CLOSE), l16(0)});
  CHECK(user16_end_step(m.rt).wake, "SC_CLOSE to the saver window wakes");
  // The mouse buttons from the MOUSE bitmask.
  in.mouse_buttons = 2;
  CHECK((api(m, "USER", "GetAsyncKeyState", {w16(VK_RBUTTON)}) & 0x8000) && !(api(m, "USER", "GetAsyncKeyState", {w16(VK_LBUTTON)}) & 0x8000),
        "VK_RBUTTON from the bitmask");
  in.mouse_buttons = 4;
  CHECK((api(m, "USER", "GetKeyState", {w16(VK_MBUTTON)}) & 0x8000) != 0, "VK_MBUTTON from the bitmask");
  m.rt.set_input(nullptr);
}

// The overlay (INTERACTION.md §7.3) under the DOS and profile calls: copy-up
// into a persistent upper, the lower untouched; seeds under the file; the
// upper-only file deleted and renamed; directories made.
void test_overlay16() {
  char base[MAX_PATH];
  GetTempPathA(MAX_PATH, base);
  std::string root = std::string(base) + "adw_win16_ovl_" + std::to_string(GetCurrentProcessId());
  std::string lower = root + "\\lower", upper = root + "\\upper";
  CreateDirectoryA(root.c_str(), nullptr);
  CreateDirectoryA(lower.c_str(), nullptr);
  {
    FILE* f = fopen((lower + "\\DATA.TXT").c_str(), "wb");
    fputs("lower", f);
    fclose(f);
    f = fopen((lower + "\\MINE.INI").c_str(), "wb");
    fputs("[A]\r\nx=1\r\n", f);
    fclose(f);
  }
  {
    Machine m;
    m.rt.vfs().mount_overlay("C:\\AFTERDRK", lower, upper);
    m.rt.vfs().mount_overlay("C:\\WINDOWS", "", root + "\\win");
    DosFiles& d = m.rt.state<DosFiles>();
    int h = d.open("C:\\AFTERDRK\\DATA.TXT", 2, false);
    uint16_t ds = m.data(64);
    m.rt.write_str(uint32_t(ds) << 16, "upper", 16);
    CHECK(h >= 5 && d.write(uint16_t(h), uint32_t(ds) << 16, 5) == 5 && d.close(uint16_t(h)) == 0, "write through a handle");
    std::string text;
    FILE* f = fopen((upper + "\\DATA.TXT").c_str(), "rb");
    char buf[16] = {};
    if (f) {
      fread(buf, 1, 15, f);
      fclose(f);
    }
    text = buf;
    CHECK(text == "upper", "copied up into the upper layer (%s)", text.c_str());
    f = fopen((lower + "\\DATA.TXT").c_str(), "rb");
    memset(buf, 0, sizeof(buf));
    fread(buf, 1, 15, f);
    fclose(f);
    CHECK(std::string(buf) == "lower", "the lower file is untouched");
    CHECK(d.remove("C:\\AFTERDRK\\MINE.INI") < 0, "a lower file cannot be deleted");
    CHECK(d.make_dir("C:\\AFTERDRK\\NEWDIR") == 0 && d.is_dir("C:\\AFTERDRK\\NEWDIR"), "mkdir in the upper layer");
    int h2 = d.open("C:\\AFTERDRK\\NEWDIR\\T.DAT", 2, true);
    d.close(uint16_t(h2));
    CHECK(d.rename("C:\\AFTERDRK\\NEWDIR\\T.DAT", "C:\\AFTERDRK\\NEWDIR\\U.DAT") == 0 && d.exists("C:\\AFTERDRK\\NEWDIR\\U.DAT"),
          "rename in the upper layer");
    CHECK(d.remove("C:\\AFTERDRK\\NEWDIR\\U.DAT") == 0 && !d.exists("C:\\AFTERDRK\\NEWDIR\\U.DAT"), "delete an upper-only file");
    CHECK(d.open("C:\\AFTERDRK\\DATA.TXT", 2, true, true) < 0, "create-new of an existing file fails");
    // Profiles: seeds under the file, writes to the upper file only.
    uint32_t sec = m.rt.static_bytes("t sec A", "A"), key_x = m.rt.static_bytes("t key x", "x"),
             key_y = m.rt.static_bytes("t key y", "y"), val = m.rt.static_bytes("t val", "2"),
             file = m.rt.static_bytes("t file", "C:\\AFTERDRK\\MINE.INI"), def = m.rt.static_bytes("t def", "d");
    profiles16(m.rt).add_seed("C:\\AFTERDRK\\MINE.INI", "A", "y", "seed");
    uint32_t out = uint32_t(ds) << 16;
    api(m, "KERNEL", "GetPrivateProfileString", {l16(sec), l16(key_y), l16(def), l16(out), w16(32), l16(file)});
    CHECK(m.rt.read_str(out) == "seed", "a seed shows under the file (%s)", m.rt.read_str(out).c_str());
    CHECK((api(m, "KERNEL", "GetPrivateProfileInt", {l16(sec), l16(key_x), w16(7), l16(file)}) & 0xFFFF) == 1,
          "the file's own key");
    CHECK((api(m, "KERNEL", "WritePrivateProfileString", {l16(sec), l16(key_x), l16(val), l16(file)}) & 0xFFFF) == 1,
          "WritePrivateProfileString");
    CHECK((api(m, "KERNEL", "GetPrivateProfileInt", {l16(sec), l16(key_x), w16(7), l16(file)}) & 0xFFFF) == 2, "read back");
  }
  std::string ini;
  if (FILE* f = fopen((upper + "\\MINE.INI").c_str(), "rb")) {
    char buf[256] = {};
    fread(buf, 1, 255, f);
    fclose(f);
    ini = buf;
  }
  CHECK(ini.find("x=2") != std::string::npos && ini.find("seed") == std::string::npos, "the upper INI has the write, "
        "not the seed (%s)", ini.c_str());
  for (const char* p : {"\\upper\\DATA.TXT", "\\upper\\MINE.INI", "\\lower\\DATA.TXT", "\\lower\\MINE.INI"})
    DeleteFileA((root + p).c_str());
  for (const char* p : {"\\upper\\NEWDIR", "\\upper", "\\lower", "\\win"}) RemoveDirectoryA((root + p).c_str());
  RemoveDirectoryA(root.c_str());
}

int run_unit() {
  test_template_converter();
  test_message_table();
  test_keyboard_tables();
  test_hooks();
  test_saver_queue();
  test_overlay16();
  test_ldt();
  test_global();
  test_local();
  test_thunks();
  test_callbacks();
  test_catch_throw();
  test_throw_across_host_levels();
  test_fault_restores_state();
  test_create_window();
  test_dos();
  test_ne_module();
  test_gdi();
  test_desktop();
  test_gdi_extras();
  test_gdi_palette_swap();
  test_seeds();
  test_system_bitmaps();
  test_frame_time();
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

// ---- assets: OLDMOD16 + AD_SND --------------------------------------------------------------------------

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

int run_assets() {
  std::string win = assets_win();
  if (win.empty()) {
    printf("assets not found: skipped\n");
    return 77;
  }
  std::string engine = win + "\\FILES\\ENGINE", classic = win + "\\FILES\\CLASSIC";
  Machine m;
  Screen screen(640, 480);
  m.rt.attach_display(screen);
  m.rt.vfs().mount("C:\\AFTERDRK", classic, false);
  m.rt.vfs().mount("C:\\WINDOWS\\SYSTEM", engine, false);
  m.rt.vfs().set_cwd("C:\\AFTERDRK");
  m.rt.modules().add_search_dir(engine);
  m.rt.modules().add_search_dir(classic);
  try {
    uint16_t err = 0;
    Module16* om = m.rt.modules().load_host(engine + "\\OLDMOD16.DLL", &err);
    CHECK(om != nullptr, "OLDMOD16.DLL loads (error %u)", err);
    if (om) {
      CHECK(om->dll_entry != 0, "DLLENTRYPOINT was called (reason 1)");
      // Its two blocks were allocated and locked: far pointers at DGROUP:11EC / 11F0.
      uint32_t sys = m.rt.rd32((uint32_t(om->dgroup) << 16) | 0x11EC);
      uint32_t mod = m.rt.rd32((uint32_t(om->dgroup) << 16) | 0x11F0);
      CHECK(sys && mod, "AD_SYSTEM/AD_MODULE locked (%08X %08X)", sys, mod);
      CHECK(m.rt.global().size(uint16_t(sys >> 16)) >= 0x3C && m.rt.global().size(uint16_t(mod >> 16)) >= 0x30,
            "block sizes");
      CHECK(m.rt.modules().proc_address(om, "LOADADMODULE16") && m.rt.modules().proc_address(om, "ModuleMessage16"),
            "exports resolve by name");
    }
    Module16* snd = m.rt.modules().load("ad_snd.dll", &err);
    CHECK(snd != nullptr, "AD_SND.DLL loads and its LibEntry succeeds (error %u)", err);
    if (snd) {
      CHECK(m.rt.modules().proc_address(snd, "adwSoundInit") != 0, "mixed-case GetProcAddress into AD_SND");
    }
    // LoadIcon of a module's own icon (BUGS.AD's RT_GROUP_ICON 42): one
    // shared handle however often it is loaded, as Win16 did, which
    // DestroyIcon leaves alone; CopyIcon makes one of the caller's own.
    Module16* bugs = m.rt.modules().load_host(classic + "\\BUGS.AD", &err);
    CHECK(bugs != nullptr, "BUGS.AD loads (error %u)", err);
    if (bugs) {
      uint16_t first = uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(42)}));
      CHECK(first != 0 && first != 0x0F04, "LoadIcon(BUGS, 42): its own icon (%04X)", first);
      bool same = true;
      for (int i = 0; i < 5000; i++) same &= uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(42)})) == first;
      CHECK(same, "5,000 more LoadIcons: the same handle");
      uint32_t name = m.rt.static_bytes("t #42", "#42");
      CHECK(uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(name)})) == first, "\"#42\" names it too");
      api(m, "USER", "DestroyIcon", {w16(first)});
      CHECK(uint16_t(api(m, "USER", "LoadIcon", {w16(bugs->hinstance), l16(42)})) == first,
            "DestroyIcon leaves the shared icon");
      uint16_t copy = uint16_t(api(m, "USER", "CopyIcon", {w16(bugs->hinstance), w16(first)}));
      CHECK(copy && copy != first, "CopyIcon: a new handle (%04X)", copy);
      api(m, "USER", "DestroyIcon", {w16(copy)});
      uint16_t again = uint16_t(api(m, "USER", "CopyIcon", {w16(bugs->hinstance), w16(first)}));
      CHECK(again == copy, "a destroyed copy's handle is free again (%04X)", again);
    }
    m.rt.modules().free_all();
    CHECK(m.rt.modules().by_name("OLDMOD16") == nullptr, "unloaded (DLLENTRYPOINT(0) ran)");
  } catch (const std::exception& e) {
    CHECK(false, "exception: %s", e.what());
    m.rt.log_state("assets");
  }
  m.rt.shims().print_census("win16 assets test");
  printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--assets") return run_assets();
  if (argc > 1 && std::string(argv[1]) == "--cpu-df") return run_cpu_df();
  try {
    return run_unit();
  } catch (const std::exception& e) {
    printf("FAIL: exception %s\n", e.what());
    return 1;
  }
}
