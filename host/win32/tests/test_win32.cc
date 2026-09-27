// Unit tests for the Win32 guest runtime: thunk dispatch and the calling
// conventions, the unimplemented-API census, imports without a signature
// (named GuestError on call, listed from bind time on), nested call_guest, SEH
// dispatch (CPU fault → handler → continue execution) and RaiseException +
// RtlUnwind through a two-frame chain, the display model's palette
// operations, and the KERNEL32/USER32 shims After Dark 10th Anniversary's
// pe32 modules brought (message queue, timers, cursors, time conversions).
//
// Guest code is hand-assembled with adw::cpu's assembler at a heap address.
// Label addresses the code needs as data (handlers, resume points, thunks) are
// written by the host into a small data table the code reads through fixed
// addresses, so the assembler never has to resolve a label as an immediate.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "adw/core/text.h"
#include "check.h"
#include "win32/display.hh"
#include "win32/modules.hh"
#include "win32/runtime.hh"
#include "win32/seh.hh"
#include "win32/shim_families.hh"

using namespace adw;
using namespace adw::win32;

namespace {

struct Fixture {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  std::unique_ptr<Runtime> rt;
  uint32_t data = 0;  // 256-byte host-written table the guest code reads
  uint32_t code = 0;

  explicit Fixture(bool all_shims = false) {
    RuntimeOptions o;
    o.heap_size = 16u << 20;
    o.call_budget = 10'000'000;
    rt = std::make_unique<Runtime>(o, clock);
    if (all_shims) register_all_shims(rt->shims());
    data = rt->heap().alloc(256, true);
    code = rt->heap().alloc(4096, true);
  }

  // Assembles `text` at `code`, replacing every "DATA" with the table address.
  cpu::X86Emulator::AssembleResult load(std::string text) {
    for (size_t p; (p = text.find("DATA")) != std::string::npos;) text.replace(p, 4, std::to_string(data));
    auto r = cpu::X86Emulator::assemble(text, nullptr, code);
    rt->mem().memcpy(code, r.code.data(), r.code.size());
    return r;
  }
  uint32_t label(const cpu::X86Emulator::AssembleResult& r, const char* name) {
    return code + r.label_offsets.at(name);
  }
  void set(uint32_t off, uint32_t v) { rt->mem().write_u32l(data + off, v); }
  uint32_t get(uint32_t off) { return rt->mem().read_u32l(data + off); }
};

}  // namespace

// A stdcall shim pops its arguments; the guest sees ESP back where it was.
TEST(thunk_stdcall_pops_args) {
  Fixture f;
  auto& e = f.rt->shims().add("TEST.DLL", "Sub", Conv::stdcall_, 8,
                              [](Call& c) { c.ret(c.arg(0) - c.arg(1)); });
  f.set(0, f.rt->shims().thunk_address(e));
  auto r = f.load(R"(
    mov esi, esp
    push 3
    push 10
    call dword [DATA]
    sub esi, esp
    mov [DATA + 8], esi
    ret
  )");
  (void)r;
  uint32_t v = f.rt->call_guest(f.code, {}, Conv::cdecl_);
  CHECK_EQ(v, 7u);
  CHECK_EQ(f.get(8), 0u);  // ESP restored by the callee
  CHECK_EQ(e.calls, 1u);
}

// A cdecl shim leaves the arguments for the caller to pop.
TEST(thunk_cdecl_leaves_args) {
  Fixture f;
  auto& e = f.rt->shims().add("TEST.DLL", "Add", Conv::cdecl_, 8,
                              [](Call& c) { c.ret(c.arg(0) + c.arg(1)); });
  f.set(0, f.rt->shims().thunk_address(e));
  f.load(R"(
    mov esi, esp
    push 3
    push 10
    call dword [DATA]
    sub esi, esp
    mov [DATA + 8], esi
    add esp, 8
    ret
  )");
  CHECK_EQ(f.rt->call_guest(f.code, {}, Conv::cdecl_), 13u);
  CHECK_EQ(f.get(8), 8u);  // still on the stack after the call
}

// An import with a signature and no handler returns 0, still pops its bytes,
// and is counted once in the census.
TEST(unimplemented_counts_and_pops) {
  Fixture f;
  auto& e = f.rt->shims().add("TEST.DLL", "Nope", Conv::stdcall_, 4);
  f.set(0, f.rt->shims().thunk_address(e));
  f.load(R"(
    mov esi, esp
    mov eax, 0x55
    push 1
    call dword [DATA]
    push 2
    call dword [DATA]
    sub esi, esp
    mov [DATA + 8], esi
    ret
  )");
  CHECK_EQ(f.rt->call_guest(f.code, {}, Conv::cdecl_), 0u);
  CHECK_EQ(f.get(8), 0u);
  CHECK_EQ(f.rt->shims().unimplemented_called().size(), size_t(1));
  CHECK_EQ(f.rt->shims().unimplemented_calls(), 2u);
}

// An import with no signature cannot be returned from (the bytes to pop are
// unknown): the call stops the guest with a GuestError that names it, instead
// of returning with the caller's stack off by the missing argument bytes. It
// is in the census before and after the call.
TEST(unknown_signature_call_is_named_guest_error) {
  Fixture f;
  auto& e = f.rt->shims().get("TEST.DLL", "Mystery");
  CHECK(!e.known_signature);
  auto unknown = f.rt->shims().unknown_signatures();
  CHECK_EQ(unknown.size(), size_t(1));
  CHECK(!unknown.empty() && unknown[0] == &e);
  f.set(0, f.rt->shims().thunk_address(e));
  f.load(R"(
    push 1
    push 2
    call dword [DATA]
    mov dword [DATA + 8], 0x77
    add esp, 8
    ret
  )");
  std::string what;
  bool fatal = false;
  try {
    f.rt->call_guest(f.code, {}, Conv::cdecl_);
  } catch (const GuestError& g) {
    what = g.what();
    fatal = g.kind() == GuestError::Kind::fatal;
  }
  CHECK(fatal);
  CHECK(what.find("TEST.DLL!Mystery") != std::string::npos);
  CHECK_EQ(f.get(8), 0u);  // the guest never ran past the call
  CHECK_EQ(e.calls, 1u);
  CHECK_EQ(f.rt->call_depth(), 0);
  auto called = f.rt->shims().unimplemented_called();
  CHECK_EQ(called.size(), size_t(1));
  // A known-signature, unimplemented import still returns, popping its bytes.
  auto& k = f.rt->shims().add("TEST.DLL", "Known", Conv::stdcall_, 8);
  f.set(4, f.rt->shims().thunk_address(k));
  f.load(R"(
    mov esi, esp
    push 1
    push 2
    call dword [DATA + 4]
    sub esi, esp
    mov [DATA + 8], esi
    ret
  )");
  CHECK_EQ(f.rt->call_guest(f.code, {}, Conv::cdecl_), 0u);
  CHECK_EQ(f.get(8), 0u);
}

namespace {

// A minimal PE32 DLL (ImageBase 0x10000000, one section at RVA 0x1000, no
// relocations, no entry point) importing KERNEL32.DLL!GetTickCount (which has
// a signature) and KERNEL32.DLL!Mystery (which does not). IAT at RVA 0x10A0.
std::string tiny_dll() {
  std::string f(0x400, '\0');
  auto u16 = [&](size_t o, uint16_t v) { memcpy(&f[o], &v, 2); };
  auto u32 = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
  auto str = [&](size_t o, const char* s) { memcpy(&f[o], s, strlen(s)); };
  str(0, "MZ");
  u32(0x3C, 0x40);
  str(0x40, "PE");
  size_t c = 0x44;
  u16(c, 0x014C);                         // i386
  u16(c + 2, 1);                          // one section
  u16(c + 16, 0xE0);                      // SizeOfOptionalHeader
  u16(c + 18, 0x2103);                    // DLL | 32BIT | EXECUTABLE | RELOCS_STRIPPED
  size_t o = 0x58;
  u16(o, 0x10B);                          // PE32
  u32(o + 28, 0x10000000);                // ImageBase
  u32(o + 32, 0x1000);                    // SectionAlignment
  u32(o + 36, 0x200);                     // FileAlignment
  u16(o + 40, 4);
  u16(o + 48, 4);
  u32(o + 56, 0x2000);                    // SizeOfImage
  u32(o + 60, 0x200);                     // SizeOfHeaders
  u16(o + 68, 2);                         // GUI
  u32(o + 92, 16);                        // NumberOfRvaAndSizes
  u32(o + 96 + 8 * 1, 0x1000);            // import directory
  u32(o + 100 + 8 * 1, 40);
  u32(o + 96 + 8 * 12, 0x10A0);           // IAT
  u32(o + 100 + 8 * 12, 12);
  size_t s = o + 0xE0;                    // section table
  str(s, ".text");
  u32(s + 8, 0x200);
  u32(s + 12, 0x1000);
  u32(s + 16, 0x200);
  u32(s + 20, 0x200);
  u32(s + 36, 0xE0000060);
  auto at = [](uint32_t rva) { return size_t(rva - 0x1000 + 0x200); };
  u32(at(0x1000), 0x1080);                // OriginalFirstThunk
  u32(at(0x100C), 0x1060);                // Name
  u32(at(0x1010), 0x10A0);                // FirstThunk
  str(at(0x1060), "KERNEL32.DLL");
  for (uint32_t t : {0x1080u, 0x10A0u}) {
    u32(at(t), 0x10C0);
    u32(at(t + 4), 0x10D0);
  }
  str(at(0x10C2), "GetTickCount");
  str(at(0x10D2), "Mystery");
  return f;
}

}  // namespace

// Binding: a module importing a name with no signature loads (it is logged,
// and listed by the census); only a call to that import stops it.
TEST(unknown_signature_bound_then_called) {
  Fixture f(true);
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = std::wstring(tmp) + L"adw_win32_tiny_" + std::to_wstring(GetCurrentProcessId()) + L".dll";
  {
    std::string img = tiny_dll();
    FILE* h = _wfopen(path.c_str(), L"wb");
    CHECK(h != nullptr);
    if (!h) return;
    fwrite(img.data(), 1, img.size(), h);
    fclose(h);
  }
  std::string host = narrow(path);
  Module* m = nullptr;
  try {
    m = f.rt->modules().load(host);
  } catch (const std::exception& e) {
    printf("load: %s\n", e.what());
  }
  DeleteFileW(path.c_str());
  CHECK(m != nullptr);
  if (!m) return;
  auto unknown = f.rt->shims().unknown_signatures();
  CHECK_EQ(unknown.size(), size_t(1));
  CHECK(!unknown.empty() && unknown[0]->key() == "KERNEL32.DLL!Mystery");
  // The known import works through its IAT slot; the unknown one stops.
  f.set(0, m->base + 0x10A0);
  f.set(4, m->base + 0x10A4);
  f.load(R"(
    mov eax, [DATA]
    call dword [eax]
    mov [DATA + 12], eax
    mov eax, [DATA + 4]
    push 5
    call dword [eax]
    ret
  )");
  std::string what;
  try {
    f.rt->call_guest(f.code, {}, Conv::cdecl_);
  } catch (const GuestError& g) {
    what = g.what();
  }
  CHECK(f.get(12) != 0u);  // GetTickCount answered
  CHECK(what.find("KERNEL32.DLL!Mystery") != std::string::npos);
  CHECK(what.find("signature") != std::string::npos);
}

// The 49 imports After Dark 10th Anniversary's pe32 modules brought that the
// table lacked (PACKAGES.md §10 B), with their stdcall argument bytes.
TEST(signature_rows_ad10) {
  Fixture f(true);
  struct Row {
    const char* dll;
    const char* name;
    uint16_t bytes;
  };
  const Row rows[] = {
      {"KERNEL32.DLL", "CreateSemaphoreA", 16}, {"KERNEL32.DLL", "GetFileTime", 16},
      {"KERNEL32.DLL", "SetFileTime", 16},      {"KERNEL32.DLL", "CopyFileA", 12},
      {"KERNEL32.DLL", "RemoveDirectoryA", 4},  {"KERNEL32.DLL", "LocalFileTimeToFileTime", 8},
      {"KERNEL32.DLL", "GetSystemInfo", 4},     {"KERNEL32.DLL", "FileTimeToSystemTime", 8},
      {"KERNEL32.DLL", "GetVersionExA", 4},     {"KERNEL32.DLL", "CreateDirectoryA", 8},
      {"KERNEL32.DLL", "MoveFileA", 8},         {"KERNEL32.DLL", "SystemTimeToFileTime", 8},
      {"KERNEL32.DLL", "HeapSize", 12},         {"WINMM.DLL", "waveOutGetDevCapsA", 12},
      {"WINMM.DLL", "waveOutReset", 4},         {"WINMM.DLL", "waveOutUnprepareHeader", 12},
      {"WINMM.DLL", "waveOutSetVolume", 8},     {"WINMM.DLL", "waveOutRestart", 4},
      {"WINMM.DLL", "waveOutPrepareHeader", 12}, {"WINMM.DLL", "waveOutWrite", 12},
      {"WINMM.DLL", "waveOutOpen", 24},         {"WINMM.DLL", "waveOutGetVolume", 8},
      {"WINMM.DLL", "waveOutGetNumDevs", 0},    {"WINMM.DLL", "waveOutPause", 4},
      {"WINMM.DLL", "waveOutClose", 4},         {"USER32.DLL", "PostQuitMessage", 4},
      {"USER32.DLL", "PostMessageA", 16},       {"USER32.DLL", "PeekMessageA", 20},
      {"USER32.DLL", "LoadCursorA", 8},         {"USER32.DLL", "KillTimer", 8},
      {"USER32.DLL", "GetScrollInfo", 12},      {"USER32.DLL", "GetClientRect", 8},
      {"USER32.DLL", "FindWindowA", 8},         {"USER32.DLL", "EnableScrollBar", 12},
      {"USER32.DLL", "DispatchMessageA", 4},    {"USER32.DLL", "BeginPaint", 8},
      {"USER32.DLL", "EndPaint", 8},            {"USER32.DLL", "TranslateMessage", 4},
      {"USER32.DLL", "SystemParametersInfoA", 16}, {"USER32.DLL", "ShowCursor", 4},
      {"USER32.DLL", "SetWindowPos", 28},       {"USER32.DLL", "SetTimer", 16},
      {"USER32.DLL", "SetScrollInfo", 16},      {"USER32.DLL", "SetCursor", 4},
      {"USER32.DLL", "SetClassLongA", 12},      {"USER32.DLL", "RegisterClassExA", 4},
      {"SHELL32.DLL", "ShellExecuteA", 24},     {"GDI32.DLL", "SetTextCharacterExtra", 8},
      {"GDI32.DLL", "GetTextMetricsA", 8},
  };
  CHECK_EQ(std::size(rows), size_t(49));
  for (const Row& r : rows) {
    ShimEntry& e = f.rt->shims().get(r.dll, r.name);
    bool ok = e.known_signature && e.conv == Conv::stdcall_ && e.arg_bytes == r.bytes;
    if (!ok) printf("  %s!%s: known=%d bytes=%u\n", r.dll, r.name, e.known_signature, e.arg_bytes);
    CHECK(ok);
  }
  CHECK(f.rt->shims().unknown_signatures().empty());
}

// Calls one shim through its thunk with dword arguments (pushed right to
// left); returns EAX.
uint32_t call_shim(Fixture& f, const char* dll, const char* name, std::initializer_list<uint32_t> args) {
  ShimEntry& e = f.rt->shims().get(dll, name);
  return f.rt->call_guest(f.rt->shims().thunk_address(e), args, e.conv);
}

// KERNEL32 additions: version, system info, time conversions, semaphores,
// HeapSize, and the refused writes.
TEST(kernel32_ad10_additions) {
  Fixture f(true);
  uint32_t buf = f.rt->heap().alloc(512, true);
  auto& mem = f.rt->mem();
  mem.write_u32l(buf, 148);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "GetVersionExA", {buf}), 1u);
  CHECK_EQ(mem.read_u32l(buf + 4), 4u);
  CHECK_EQ(mem.read_u32l(buf + 12) & 0xFFFF, 950u);
  CHECK_EQ(mem.read_u32l(buf + 16), 1u);  // VER_PLATFORM_WIN32_WINDOWS
  mem.write_u32l(buf, 20);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "GetVersionExA", {buf}), 0u);
  call_shim(f, "KERNEL32.DLL", "GetSystemInfo", {buf});
  CHECK_EQ(mem.read_u32l(buf + 4), 0x1000u);
  CHECK_EQ(mem.read_u32l(buf + 20), 1u);
  CHECK_EQ(mem.read_u32l(buf + 24), 586u);
  // 1996-09-12 12:34:56.789 round trip.
  SYSTEMTIME st{1996, 9, 4, 12, 12, 34, 56, 789};
  write_pod(mem, buf, st);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "SystemTimeToFileTime", {buf, buf + 64}), 1u);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "LocalFileTimeToFileTime", {buf + 64, buf + 80}), 1u);
  CHECK_EQ(mem.read_u64l(buf + 80), mem.read_u64l(buf + 64));  // bias 0
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "FileTimeToSystemTime", {buf + 80, buf + 96}), 1u);
  SYSTEMTIME back = read_pod<SYSTEMTIME>(mem, buf + 96);
  CHECK(back.wYear == 1996 && back.wMonth == 9 && back.wDay == 12 && back.wMinute == 34 &&
        back.wMilliseconds == 789);
  // Semaphores: a named second create reports ERROR_ALREADY_EXISTS.
  write_cstr(mem, buf + 200, "HallOfFame", 32);
  uint32_t s1 = call_shim(f, "KERNEL32.DLL", "CreateSemaphoreA", {0, 1, 1, buf + 200});
  CHECK(s1 != 0);
  CHECK_EQ(f.rt->last_error(), 0u);
  uint32_t s2 = call_shim(f, "KERNEL32.DLL", "CreateSemaphoreA", {0, 1, 1, buf + 200});
  CHECK(s2 != 0 && s2 != s1);
  CHECK_EQ(f.rt->last_error(), uint32_t(ERROR_ALREADY_EXISTS));
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "CloseHandle", {s1}), 1u);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "CreateSemaphoreA", {0, 2, 1, 0}), 0u);  // initial > maximum
  // HeapSize: the requested size; (SIZE_T)-1 for a foreign pointer.
  uint32_t heap = f.rt->heaps().process_heap();
  uint32_t p = call_shim(f, "KERNEL32.DLL", "HeapAlloc", {heap, 0, 100});
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "HeapSize", {heap, 0, p}), 100u);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "HeapSize", {heap, 0, buf}), 0xFFFFFFFFu);
  // Writes to the (read-only or absent) file system are refused.
  write_cstr(mem, buf + 300, "C:\\AFTERDRK\\cache", 64);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "CreateDirectoryA", {buf + 300, 0}), 0u);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "RemoveDirectoryA", {buf + 300}), 0u);
  CHECK_EQ(call_shim(f, "KERNEL32.DLL", "GetFileTime", {0x12345, 0, 0, buf}), 0u);
  CHECK_EQ(f.rt->last_error(), uint32_t(ERROR_INVALID_HANDLE));
  CHECK(f.rt->shims().unimplemented_called().empty());
}

// USER32 additions: the message queue (posted messages, WM_QUIT, timers by
// the virtual clock), cursors, window classes, client rects.
TEST(user32_queue_timers_cursors) {
  Fixture f(true);
  auto& mem = f.rt->mem();
  uint32_t buf = f.rt->heap().alloc(256, true);
  uint32_t saver = host_window(*f.rt);
  // An empty queue.
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_REMOVE}), 0u);
  // Posted messages come back in order; PM_NOREMOVE leaves them there.
  CHECK_EQ(call_shim(f, "USER32.DLL", "PostMessageA", {saver, 0x400, 1, 2}), 1u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PostMessageA", {saver, 0x401, 3, 4}), 1u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_NOREMOVE}), 1u);
  CHECK_EQ(mem.read_u32l(buf + 4), 0x400u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0x401, 0x401, PM_REMOVE}), 1u);
  CHECK_EQ(mem.read_u32l(buf + 4), 0x401u);
  CHECK_EQ(mem.read_u32l(buf + 8), 3u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_REMOVE}), 1u);
  CHECK_EQ(mem.read_u32l(buf + 4), 0x400u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PostMessageA", {0x0BAD0000, 0x400, 0, 0}), 0u);  // no such window
  // WM_QUIT after the posted messages.
  call_shim(f, "USER32.DLL", "PostQuitMessage", {7});
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_REMOVE}), 1u);
  CHECK_EQ(mem.read_u32l(buf + 4), uint32_t(WM_QUIT));
  CHECK_EQ(mem.read_u32l(buf + 8), 7u);
  // A timer is due by the virtual clock, and only then.
  CHECK_EQ(call_shim(f, "USER32.DLL", "SetTimer", {saver, 5, 100, 0}), 5u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_REMOVE}), 0u);
  for (int i = 0; i < 7; i++) f.clock.begin_frame();  // 6 × 16.667 ms ≥ 100 ms
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_REMOVE}), 1u);
  CHECK_EQ(mem.read_u32l(buf + 4), uint32_t(WM_TIMER));
  CHECK_EQ(mem.read_u32l(buf + 8), 5u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "PeekMessageA", {buf, 0, 0, 0, PM_REMOVE}), 0u);  // rescheduled
  CHECK_EQ(call_shim(f, "USER32.DLL", "KillTimer", {saver, 5}), 1u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "KillTimer", {saver, 5}), 0u);
  // Cursors: system ones have fixed handles; a module without the resource gets none.
  uint32_t arrow = call_shim(f, "USER32.DLL", "LoadCursorA", {0, 32512});
  CHECK(arrow != 0);
  CHECK_EQ(call_shim(f, "USER32.DLL", "LoadCursorA", {0, 32512}), arrow);
  CHECK_EQ(call_shim(f, "USER32.DLL", "SetCursor", {arrow}), 0u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "SetCursor", {0}), arrow);
  CHECK_EQ(call_shim(f, "USER32.DLL", "ShowCursor", {0}), 0xFFFFFFFFu);
  CHECK_EQ(call_shim(f, "USER32.DLL", "ShowCursor", {1}), 0u);
  // RegisterClassExA checks cbSize; GetClientRect is the window's size at 0,0.
  g32::WNDCLASSEXA wc{};
  wc.cbSize = 40;
  write_cstr(mem, buf + 200, "HofClass", 32);
  wc.lpszClassName = buf + 200;
  write_pod(mem, buf, wc);
  CHECK_EQ(call_shim(f, "USER32.DLL", "RegisterClassExA", {buf}), 0u);
  wc.cbSize = sizeof(g32::WNDCLASSEXA);
  write_pod(mem, buf, wc);
  CHECK(call_shim(f, "USER32.DLL", "RegisterClassExA", {buf}) >= 0xC000u);
  CHECK_EQ(call_shim(f, "USER32.DLL", "GetClientRect", {saver, buf}), 1u);
  RECT rc = read_pod<RECT>(mem, buf);
  CHECK(rc.left == 0 && rc.top == 0 && rc.right == 640 && rc.bottom == 480);
  CHECK(f.rt->shims().unimplemented_called().empty());
}

// Every Win32-lane import has a signature: the shared KERNEL32/USER32/GDI32
// rows the families rely on resolve with the right byte counts.
TEST(signature_table_rows) {
  Fixture f(true);
  auto* a = f.rt->shims().find("KERNEL32.DLL", "GetTickCount");
  CHECK(a && a->known_signature && a->arg_bytes == 0);
  auto* b = f.rt->shims().find("GDI32.DLL", "BitBlt");
  CHECK(b && b->known_signature && b->arg_bytes == 36);
  auto* c = f.rt->shims().find("USER32.DLL", "wsprintfA");
  CHECK(c && c->conv == Conv::varargs);
}

// Host → guest → host → guest: a shim calls back into guest code (as
// DispatchMessage calls a WndProc) and the outer guest call carries on.
TEST(call_guest_nested) {
  Fixture f;
  auto r = f.load(R"(
  outer:
    push 21
    call dword [DATA]
    inc eax
    ret
  twice:
    mov eax, [esp + 4]
    add eax, eax
    ret 4
  )");
  uint32_t twice = f.label(r, "twice");
  auto& e = f.rt->shims().add("TEST.DLL", "Callback", Conv::stdcall_, 4, [twice](Call& c) {
    c.ret(c.rt.call_guest(twice, {c.arg(0)}, Conv::stdcall_));
  });
  f.set(0, f.rt->shims().thunk_address(e));
  uint32_t esp0 = f.rt->regs().esp;
  CHECK_EQ(f.rt->call_guest(f.label(r, "outer"), {}, Conv::cdecl_), 43u);
  CHECK_EQ(f.rt->call_depth(), 0);
  CHECK_EQ(f.rt->regs().esp, esp0);
  // Direct call with stdcall popping checked by the runtime.
  CHECK_EQ(f.rt->call_guest(twice, {5}, Conv::stdcall_), 10u);
}

// A divide error in guest code reaches the FS:[0] handler as
// EXCEPTION_INT_DIVIDE_BY_ZERO; the handler edits the CONTEXT and returns
// ExceptionContinueExecution, and execution resumes where it said.
TEST(seh_fault_continue_execution) {
  Fixture f;
  const uint32_t teb = layout::kTeb;
  auto r = f.load("  mov edx, " + std::to_string(teb) + R"(
    push dword [DATA]
    push dword [edx]
    mov [edx], esp
    xor ecx, ecx
    mov eax, 100
    cdq
    div ecx
    int 3
  resume:
    mov edx, )" + std::to_string(teb) + R"(
    pop ecx
    mov [edx], ecx
    add esp, 4
    ret
  handler:
    mov eax, [esp + 4]
    mov ecx, [eax]
    mov [DATA + 8], ecx
    mov eax, [esp + 12]
    mov ecx, [DATA + 4]
    mov [eax + 0xB8], ecx
    mov dword [eax + 0xB0], 0x1234
    xor eax, eax
    ret
  )");
  f.set(0, f.label(r, "handler"));
  f.set(4, f.label(r, "resume"));
  uint32_t head = f.rt->mem().read_u32l(teb);
  CHECK_EQ(head, 0xFFFFFFFFu);  // an empty chain is -1
  CHECK_EQ(f.rt->call_guest(f.code, {}, Conv::cdecl_), 0x1234u);
  CHECK_EQ(f.get(8), 0xC0000094u);  // EXCEPTION_INT_DIVIDE_BY_ZERO
  CHECK_EQ(f.rt->mem().read_u32l(teb), head);
}

// RaiseException walks two frames: the inner handler declines
// (ExceptionContinueSearch), the outer one RtlUnwinds to itself — which calls
// the inner handler again with EXCEPTION_UNWINDING and unlinks it — then
// resumes in its own frame, as Borland's try/catch does (ABI.md §4).
TEST(seh_raise_and_unwind) {
  Fixture f(true);
  const std::string teb = std::to_string(layout::kTeb);
  auto& shims = f.rt->shims();
  f.set(16, shims.thunk_address(shims.get("KERNEL32.DLL", "RaiseException")));
  f.set(20, shims.thunk_address(shims.get("KERNEL32.DLL", "RtlUnwind")));
  auto r = f.load("  mov edx, " + teb + R"(
    push dword [DATA + 4]
    push dword [edx]
    mov [edx], esp
    push dword [DATA]
    push dword [edx]
    mov [edx], esp
    push 0
    push 0
    push 0
    push 0xE0001234
    call dword [DATA + 16]
    int 3
  resume_b:
    mov edx, )" + teb + R"(
    pop ecx
    mov [edx], ecx
    add esp, 4
    mov eax, [DATA + 12]
    ret
  handler_a:
    mov eax, [esp + 4]
    test dword [eax + 4], 6
    jz search_a
    add dword [DATA + 12], 1
    jmp done_a
  search_a:
    add dword [DATA + 12], 0x100
  done_a:
    mov eax, 1
    ret
  handler_b:
    mov eax, [esp + 4]
    test dword [eax + 4], 6
    jnz decline_b
    mov ecx, [eax]
    mov [DATA + 24], ecx
    mov ebx, [esp + 8]
    push 0
    push eax
    push 0
    push ebx
    call dword [DATA + 20]
    mov edx, )" + teb + R"(
    mov ecx, [edx]
    mov [DATA + 28], ecx
    mov [DATA + 32], ebx
    mov esp, ebx
    jmp dword [DATA + 8]
  decline_b:
    mov eax, 1
    ret
  )");
  f.set(0, f.label(r, "handler_a"));
  f.set(4, f.label(r, "handler_b"));
  f.set(8, f.label(r, "resume_b"));
  uint32_t head = f.rt->mem().read_u32l(layout::kTeb);
  // One first-pass call and one unwind call on the inner handler.
  CHECK_EQ(f.rt->call_guest(f.code, {}, Conv::cdecl_), 0x101u);
  CHECK_EQ(f.get(24), 0xE0001234u);
  CHECK_EQ(f.get(28), f.get(32));  // after RtlUnwind the head is the target frame
  CHECK_EQ(f.rt->mem().read_u32l(layout::kTeb), head);
}

// A fault with no handler is a GuestError (unhandled), not a host crash.
TEST(seh_unhandled_is_guest_error) {
  Fixture f;
  f.load(R"(
    xor ecx, ecx
    mov eax, 1
    cdq
    div ecx
    ret
  )");
  bool threw = false;
  try {
    f.rt->call_guest(f.code, {}, Conv::cdecl_);
  } catch (const GuestError& e) {
    threw = e.kind() == GuestError::Kind::unhandled;
  }
  CHECK(threw);
}

// The display model: 20 static colours, realization into the free slots,
// Screen/colour-table updates, animation of PC_RESERVED entries, and the
// GetDeviceCaps answers of an 8 bpp RC_PALETTE display (ABI.md §2.9).
TEST(display_palette) {
  Fixture f;
  Screen screen(64, 48);
  Display& d = f.rt->attach_display(screen);
  CHECK_EQ(d.width(), 64);
  CHECK_EQ(d.static_low(), 10);
  CHECK_EQ(d.device_caps(BITSPIXEL), 8);
  CHECK_EQ(d.device_caps(PLANES), 1);
  CHECK(d.device_caps(RASTERCAPS) & RC_PALETTE);
  CHECK_EQ(d.device_caps(SIZEPALETTE), 256);
  CHECK_EQ(d.device_caps(NUMRESERVED), 20);
  // Statics: index 0 black, 255 white.
  CHECK(screen.palette()[0].rgbRed == 0 && screen.palette()[0].rgbBlue == 0);
  CHECK(screen.palette()[255].rgbRed == 255 && screen.palette()[255].rgbGreen == 255);

  LogicalPalette p;
  p.entries.push_back({0, 0, 0, 0});            // collapses onto static black
  p.entries.push_back({12, 34, 56, 0});         // new colour → first free slot
  p.entries.push_back({1, 2, 3, PC_RESERVED});  // animatable
  d.realize(p);
  CHECK_EQ(int(p.map[0]), 0);
  CHECK_EQ(int(p.map[1]), 10);
  CHECK_EQ(int(p.map[2]), 11);
  CHECK(d.is_current(p));
  CHECK(screen.palette()[10].rgbRed == 12 && screen.palette()[10].rgbGreen == 34 && screen.palette()[10].rgbBlue == 56);
  CHECK(screen.palette()[11].rgbBlue == 3);

  PALETTEENTRY anim[2] = {{200, 201, 202, PC_RESERVED}, {99, 99, 99, PC_RESERVED}};
  d.animate(p, 1, 2, anim);  // entry 1 is not PC_RESERVED: unchanged
  CHECK(screen.palette()[10].rgbRed == 12);
  CHECK(screen.palette()[11].rgbRed == 99 && screen.palette()[11].rgbGreen == 99);
  CHECK_EQ(d.nearest_index(RGB(99, 99, 99), false), 11);
}

// The desktop seed (display.hh): ":win95" is solid static teal; a raw file of
// exactly w*h bytes is copied as indices; a P6 of another size is scaled and
// dithered onto the statics only; unusable specs leave the screen black.
TEST(display_seed) {
  Fixture f;
  Screen screen(64, 48);
  Display& d = f.rt->attach_display(screen);
  auto px = [&](int x, int y) { return int(d.bits()[size_t(y) * d.pitch() + size_t(x)]); };
  std::string what;
  CHECK(!d.seed(":nope", &what));
  CHECK(!d.seed("Z:\\no\\such\\seed.ppm", &what));
  CHECK_EQ(px(5, 5), 0);

  CHECK(d.seed(":win95", &what));
  CHECK_EQ(px(0, 0), 6);  // static 6 = RGB(0, 128, 128)
  CHECK_EQ(px(63, 47), 6);

  wchar_t tmp[MAX_PATH], file[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  GetTempFileNameW(tmp, L"sd", 0, file);
  std::string path = narrow(file);
  auto write = [&](const std::string& bytes) {
    FILE* fp = _wfopen(file, L"wb");
    fwrite(bytes.data(), 1, bytes.size(), fp);
    fclose(fp);
  };
  // Raw indices: used as they are.
  std::string raw(64 * 48, '\x2A');
  raw[64 * 10 + 3] = '\x07';
  write(raw);
  CHECK(d.seed(path, &what));
  CHECK_EQ(px(0, 0), 0x2A);
  CHECK_EQ(px(3, 10), 7);
  // A 2x2 P6 (red, white / blue, mid-grey), scaled up: statics only, and the
  // pure static colours land exactly.
  std::string p6 = "P6\n# seed\n2 2\n255\n";
  const unsigned char rgb[12] = {255, 0, 0, 255, 255, 255, 0, 0, 255, 100, 100, 100};
  p6.append(reinterpret_cast<const char*>(rgb), 12);
  write(p6);
  CHECK(d.seed(path, &what));
  CHECK(what.find("P6") != std::string::npos);
  bool statics_only = true;
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 64; x++) statics_only = statics_only && d.is_static(px(x, y));
  CHECK(statics_only);
  CHECK_EQ(px(5, 5), 249);    // pure red: static 249
  CHECK_EQ(px(40, 5), 255);   // white
  CHECK_EQ(px(5, 40), 252);   // pure blue
  // Deterministic: the same file seeds the same bytes.
  std::vector<uint8_t> first(d.bits(), d.bits() + d.pitch() * 48);
  CHECK(d.seed(path, &what));
  CHECK(std::equal(first.begin(), first.end(), d.bits()));
  write("P6\n2 2\n65535\n");  // not maxval 255, not raw-sized, not a BMP
  CHECK(!d.seed(path, &what));
  DeleteFileW(file);
}

int main(int argc, char** argv) { return adw_test::run_all(argc > 1 ? argv[1] : nullptr); }
