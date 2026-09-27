// The emulated Win16 machine's linear address space. Win16 code never sees
// linear addresses — it sees selectors from the host-owned LDT (ldt.hh) — so
// this layout only has to keep the host's own segments apart and give every
// guest allocation room in ONE arena (so a DIB section can alias any of it,
// MemoryContext::section_for).
//
//   0x00000000  null guard            unmapped
//   0x00010000  BIOS data area (64K)  selector 0x40 (base here, limit 0xFFFF): ticks, video mode, …
//   0x00020000  system segment (64K)  environment block, PSP, catch/throw scratch, host strings
//   0x00030000  thunk segment (64K)   one 4-byte `int 0xFE; dw id` far thunk per import (shims16.hh)
//   0x00040000  task stack (64K)      the 16-bit stack every call runs on (SS), as the Win16 stack
//                                     the Win95 thunk layer switched OLDMOD32's thread to
//   0x00100000  global arena          every GlobalAlloc block and every NE segment (global_heap.hh),
//                                     module databases, the screen surface's bits
#pragma once

#include <cstdint>

namespace adw::win16::layout {

constexpr uint32_t kBdaBase = 0x00010000;
constexpr uint32_t kBdaSize = 0x00010000;

constexpr uint32_t kSysBase = 0x00020000;
constexpr uint32_t kSysSize = 0x00010000;
// Offsets inside the system segment.
constexpr uint16_t kSysEnvironment = 0x0000;  // DOS environment block (GetDOSEnvironment)
constexpr uint16_t kSysPsp = 0x1000;          // a PSP (INT 21h AH=62h, 51h)
constexpr uint16_t kSysDta = 0x1100;          // default DTA
constexpr uint16_t kSysStrings = 0x2000;      // host strings handed to the guest

constexpr uint32_t kThunkBase = 0x00030000;
constexpr uint32_t kThunkSize = 0x00010000;

constexpr uint32_t kStackBase = 0x00040000;
constexpr uint32_t kStackSize = 0x00010000;
// Instance-data words of the stack segment (the Win16 "task header" at SS:0,
// read by compiler stack probes): pStackTop (lowest valid SP), pStackMin,
// pStackBottom (highest).
constexpr uint16_t kStackTop = 0x0200;
constexpr uint16_t kStackBottom = 0xFFFE;
constexpr uint16_t kInitialSp = 0xFFF0;

constexpr uint32_t kArenaBase = 0x00100000;
constexpr uint32_t kDefaultArenaSize = 64u << 20;
constexpr uint32_t kMaxArenaSize = 512u << 20;

}  // namespace adw::win16::layout
