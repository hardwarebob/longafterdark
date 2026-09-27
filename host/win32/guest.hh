// Guest memory helpers for shim code: strings, packed structs, errors.
//
// Every guest access goes through the MemoryContext. An address no arena maps
// throws std::out_of_range, which — when it happens inside a shim, i.e. inside
// the CPU's run loop — the CPU turns into a #PF at the calling thunk, which our
// SEH dispatch reports to the guest as an EXCEPTION_ACCESS_VIOLATION raised by
// the API. That is how Win32 behaves when an API is handed a bad pointer, so
// shims need not validate pointers themselves. (Corollary: shim code must not
// let an unrelated std::out_of_range escape — no std::map::at on guest-derived
// keys; use find().)
//
// Structs that cross the guest boundary are declared here with their 32-bit
// x86 layout (pointers and handles are uint32_t), with static_asserts on the
// size, and copied with read_pod/write_pod. Pointer-free Win32 structs whose
// layout is identical on x64 (RECT, POINT, SIZE, PALETTEENTRY, RGBQUAD,
// BITMAPINFOHEADER, SYSTEMTIME, …) may be copied as the <windows.h> type.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "MemoryContext.hh"

namespace adw::win32 {

using cpu::MemoryContext;

// ---- Errors that end the emulation --------------------------------------------------------------
//
// Thrown from shim or runtime code; they propagate out of every (nested) CPU
// run loop, so a guest condition that must stop the lane (ExitProcess, an
// unhandled exception, a hung callback) unwinds all host frames at once. They
// deliberately do not derive from std::out_of_range (see above).
class GuestError : public std::runtime_error {
 public:
  enum class Kind {
    exit,         // ExitProcess / TerminateProcess
    unhandled,    // an exception reached the end of the SEH chain
    hang,         // a guest call exceeded its instruction budget
    fatal,        // anything else the runtime cannot continue from
  };
  GuestError(Kind kind, const std::string& what, uint32_t code = 0)
      : std::runtime_error(what), kind_(kind), code_(code) {}
  Kind kind() const { return kind_; }
  uint32_t code() const { return code_; }

 private:
  Kind kind_;
  uint32_t code_;
};

// ---- Strings -----------------------------------------------------------------------------------

// A NUL-terminated 8-bit string (the ANSI code page; After Dark is ASCII/1252).
// Stops after `max` bytes. addr 0 → "".
inline std::string read_cstr(const MemoryContext& mem, uint32_t addr, size_t max = 0x10000) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; i++) {
    char c = static_cast<char>(mem.read<uint8_t>(addr + uint32_t(i)));
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

// Copies s (truncated to cap-1 bytes) plus a NUL. Returns the bytes copied, not
// counting the NUL. cap 0 writes nothing.
inline size_t write_cstr(MemoryContext& mem, uint32_t addr, std::string_view s, size_t cap) {
  if (!cap) return 0;
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  if (n) mem.memcpy(addr, s.data(), n);
  mem.write<uint8_t>(addr + uint32_t(n), 0);
  return n;
}

// UTF-16LE guest strings.
inline std::u16string read_wstr(const MemoryContext& mem, uint32_t addr, size_t max = 0x8000) {
  std::u16string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; i++) {
    char16_t c = static_cast<char16_t>(mem.read<uint16_t>(addr + uint32_t(2 * i)));
    if (!c) break;
    s.push_back(c);
  }
  return s;
}
inline size_t write_wstr(MemoryContext& mem, uint32_t addr, std::u16string_view s, size_t cap_chars) {
  if (!cap_chars) return 0;
  size_t n = s.size() < cap_chars - 1 ? s.size() : cap_chars - 1;
  for (size_t i = 0; i < n; i++) mem.write<uint16_t>(addr + uint32_t(2 * i), uint16_t(s[i]));
  mem.write<uint16_t>(addr + uint32_t(2 * n), 0);
  return n;
}

// ---- Packed structs ------------------------------------------------------------------------------

template <typename T>
T read_pod(const MemoryContext& mem, uint32_t addr) {
  static_assert(std::is_trivially_copyable_v<T>);
  T v;
  mem.memcpy(&v, addr, sizeof(T));
  return v;
}
template <typename T>
void write_pod(MemoryContext& mem, uint32_t addr, const T& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  mem.memcpy(addr, &v, sizeof(T));
}

inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1); }

// 32-bit layouts of pointer-bearing structs (the x64 <windows.h> ones differ).
namespace g32 {
#pragma pack(push, 4)
struct BITMAP {  // GetObject on an HBITMAP
  int32_t bmType, bmWidth, bmHeight, bmWidthBytes;
  uint16_t bmPlanes, bmBitsPixel;
  uint32_t bmBits;
};
static_assert(sizeof(BITMAP) == 24);
struct DIBSECTION {
  BITMAP dsBm;
  BITMAPINFOHEADER dsBmih;
  uint32_t dsBitfields[3];
  uint32_t dshSection, dsOffset;
};
static_assert(sizeof(DIBSECTION) == 84);
struct WNDCLASSA {
  uint32_t style, lpfnWndProc;
  int32_t cbClsExtra, cbWndExtra;
  uint32_t hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName;
};
static_assert(sizeof(WNDCLASSA) == 40);
struct WNDCLASSEXA {
  uint32_t cbSize, style, lpfnWndProc;
  int32_t cbClsExtra, cbWndExtra;
  uint32_t hInstance, hIcon, hCursor, hbrBackground, lpszMenuName, lpszClassName, hIconSm;
};
static_assert(sizeof(WNDCLASSEXA) == 48);
struct MSG {
  uint32_t hwnd, message, wParam, lParam, time;
  int32_t x, y;
};
static_assert(sizeof(MSG) == 28);
struct PAINTSTRUCT {
  uint32_t hdc;
  int32_t fErase;
  RECT rcPaint;
  int32_t fRestore, fIncUpdate;
  uint8_t rgbReserved[32];
};
static_assert(sizeof(PAINTSTRUCT) == 64);
struct STARTUPINFOA {
  uint32_t cb, lpReserved, lpDesktop, lpTitle;
  uint32_t dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
  uint16_t wShowWindow, cbReserved2;
  uint32_t lpReserved2, hStdInput, hStdOutput, hStdError;
};
static_assert(sizeof(STARTUPINFOA) == 68);
struct MEMORYSTATUS {
  uint32_t dwLength, dwMemoryLoad, dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile,
      dwTotalVirtual, dwAvailVirtual;
};
static_assert(sizeof(MEMORYSTATUS) == 32);
#pragma pack(pop)
}  // namespace g32

}  // namespace adw::win32
