// The emulated Win32 address space. One flat 4 GiB space (DESIGN.md §2); every
// region below is its own MemoryContext arena (a pagefile-backed file mapping),
// so nothing a guest access touches straddles two arenas except by accident.
//
//   0x00000000  null guard            unmapped: a NULL dereference is an access violation
//   0x00010000  system area (64K)     PEB, TEB, process parameters, command line, environment
//   0x00020000  thunk area (128K)     one 8-byte `int 0xFE; dw id` thunk per import/host routine
//   0x00100000  stack (1 MB)          StackLimit 0x00100000, StackBase 0x00200000
//   0x00400000  host EXE (64K)        headers of a stand-in for AFTERDAR.SCR, which sat here in
//                                     1996; every AD image prefers 0x400000, so all of them relocate
//   0x00410000  image area            relocated PE images (64K-aligned, first fit)
//   0x20000000  heap arena            Global/Local/Heap/VirtualAlloc and every DIB section's bits:
//                                     one file mapping, so CreateDIBSection can alias it
//   0x7F000000  shim DLL headers      pseudo HMODULEs of the DLLs we emulate (KERNEL32, USER32, …),
//                                     each a page with an MZ/PE header, 64K apart
//
// STARRYNI.AD prefers 0x10000000 and gets it (inside the image area).
#pragma once

#include <cstdint>

namespace adw::win32::layout {

constexpr uint32_t kNullGuardEnd = 0x00010000;

constexpr uint32_t kSystemBase = 0x00010000;
constexpr uint32_t kSystemSize = 0x00010000;
constexpr uint32_t kPeb = kSystemBase + 0x0000;             // PEB (0x1000)
constexpr uint32_t kTeb = kSystemBase + 0x1000;             // TEB (0x1000; FS limit 0xFFF)
constexpr uint32_t kProcessParams = kSystemBase + 0x2000;   // RTL_USER_PROCESS_PARAMETERS-ish
constexpr uint32_t kSystemStrings = kSystemBase + 0x3000;   // command line, env block, … (host statics)
constexpr uint32_t kSystemStringsEnd = kSystemBase + kSystemSize;

constexpr uint32_t kThunkBase = 0x00020000;
constexpr uint32_t kThunkSize = 0x00020000;
constexpr uint32_t kThunkStride = 8;
constexpr uint32_t kMaxThunks = kThunkSize / kThunkStride;  // 16384

constexpr uint32_t kStackLimit = 0x00100000;
constexpr uint32_t kStackBase = 0x00200000;

constexpr uint32_t kExeBase = 0x00400000;
constexpr uint32_t kExeSize = 0x00010000;

constexpr uint32_t kImageLow = 0x00410000;
constexpr uint32_t kImageHigh = 0x20000000;
constexpr uint32_t kImageAlign = 0x00010000;

constexpr uint32_t kHeapBase = 0x20000000;
constexpr uint32_t kDefaultHeapSize = 128u << 20;
constexpr uint32_t kMaxHeapSize = 0x40000000;  // up to 0x60000000

constexpr uint32_t kShimModuleBase = 0x7F000000;
constexpr uint32_t kShimModuleStride = 0x00010000;
constexpr uint32_t kMaxShimModules = 64;

}  // namespace adw::win32::layout
