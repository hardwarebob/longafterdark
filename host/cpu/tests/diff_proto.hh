// Wire format between the differential test driver (x86-64, runs adw::cpu) and the native oracle (i686, runs the
// same snippets on the real CPU under WOW64). Both sides compile this header; everything is fixed-size and packed.
#pragma once

#include <stdint.h>

namespace diffproto {

// The snippet's world: identical addresses on both sides.
constexpr uint32_t CODE_BASE = 0x30000000; // snippet, then the oracle's epilogue
constexpr uint32_t DATA_BASE = 0x31000000; // 256 bytes the snippet may read and write
constexpr uint32_t STACK_BASE = 0x32000000; // 256 bytes of stack; ESP starts at STACK_BASE + STACK_ESP
constexpr uint32_t DATA_SIZE = 256;
constexpr uint32_t STACK_SIZE = 256;
constexpr uint32_t STACK_ESP = 0x80;
constexpr uint32_t CODE_MAX = 48;
constexpr uint32_t PATCH_MAX = 64;

#pragma pack(push, 1)
struct CaseIn {
  uint32_t seed; // fills the data and stack regions (see fill_region)
  uint32_t code_len;
  uint8_t code[CODE_MAX];
  uint32_t regs[8]; // eax ecx edx ebx esp ebp esi edi
  uint32_t eflags;
  uint16_t fcw; // x87 control word after fninit
  uint16_t patch_len; // bytes of `patch` copied to DATA_BASE after the fill
  uint8_t patch[PATCH_MAX];
};

struct CaseOut {
  uint32_t fault_code; // 0, or the Windows exception code (native) / 0xE0000000 | vector (emulator)
  uint32_t fault_eip;
  uint32_t regs[8];
  uint32_t eflags;
  uint8_t fpu[108]; // fnsave image (32-bit protected-mode format)
  uint8_t data[DATA_SIZE];
  uint8_t stack[STACK_SIZE];
};
#pragma pack(pop)

// xorshift32: the region contents are a pure function of the seed, so they need not travel over the wire.
inline void fill_region(uint8_t* p, uint32_t size, uint32_t seed) {
  uint32_t x = seed ? seed : 0x12345678;
  for (uint32_t z = 0; z < size; z++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p[z] = static_cast<uint8_t>(x >> 11);
  }
}

} // namespace diffproto
