// Differential test: adw::cpu::X86Emulator vs the real CPU.
//
// Generates tens of thousands of directed and random machine-code snippets (every ALU op at 8/16/32 bits in register
// and memory forms, 0x66/0x67-prefixed forms including all 16-bit addressing modes via lea, shifts and rotates with
// counts 0, 1, and past the width, mul/imul/div/idiv (plus their #DE faults), bit ops, BCD, xlat, bound (and #BR),
// cmpxchg/cmpxchg8b/xadd/bswap, string ops with rep, branches and loops, stack ops, and x87
// arithmetic/compare/conversion/transcendentals under every rounding and precision control, including unmasked
// exceptions reported as #MF), runs them natively through x86_oracle (a 32-bit helper running under WOW64) and on
// the emulator, and compares registers, flags (masking what the architecture leaves undefined), memory, the x87
// state, and faults. Memory operands through 16-bit addresses would land below 64 KiB, which Windows never maps, so
// those are covered by test_segmentation instead.
//
//   test_x86_diff <path to x86_oracle.exe> [scratch dir] [--seed N] [--count-scale N] [--verbose] [--explain]
//
// --explain re-runs failing straight-line snippets cut after each instruction, on both sides, to show the first
// instruction whose effect differs.
//
// Exits 77 (ctest SKIP) when the oracle cannot run (e.g. not a Windows x86-64 machine with WOW64).

#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "X86Emulator.hh"
#include "diff_proto.hh"

using namespace adw::cpu;
using namespace diffproto;

namespace {

constexpr uint32_t F_CF = 0x001, F_PF = 0x004, F_AF = 0x010, F_ZF = 0x040, F_SF = 0x080, F_DF = 0x400, F_OF = 0x800;
constexpr uint32_t F_ARITH = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF;
constexpr uint32_t F_ALL = F_ARITH | F_DF;

struct Rng {
  uint64_t s;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 16);
  }
  uint32_t below(uint32_t n) {
    return n ? (next() % n) : 0;
  }
  bool coin() {
    return next() & 1;
  }
  // A 32-bit value biased toward the edges the flag logic cares about.
  uint32_t value() {
    static const uint32_t specials[] = {0, 1, 2, 0x7F, 0x80, 0x81, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFF,
        0x10000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF, 0x0F, 0xF0, 0x09, 0x99, 0x9A};
    switch (below(4)) {
      case 0: {
        uint32_t v = specials[below(sizeof(specials) / sizeof(specials[0]))];
        // Replicate small specials into the other bytes/words sometimes so 8/16-bit views see them too.
        if (coin()) {
          v = (v & 0xFF) * 0x01010101;
        }
        return v;
      }
      case 1:
        return next() & 0xFF;
      default:
        return next() ^ (next() << 16);
    }
  }
};

struct TestCase {
  CaseIn in;
  std::string category;
  uint32_t flags_mask = F_ALL;
  bool compare_fpu = false;
  uint16_t fpu_sw_mask = 0xFFFF;
  uint32_t fpu_ulps = 0; // tolerance on x87 register significands
  bool store_ulps = false; // tolerance also applies to the data region (transcendentals stored to memory)
};

// Builds one snippet and the initial state it runs in.
struct Builder {
  Rng& r;
  TestCase t;
  std::vector<uint8_t> code;
  uint8_t pinned = 0; // registers whose values the snippet depends on (addressing, counters)

  Builder(Rng& r, const char* category) : r(r) {
    memset(&this->t.in, 0, sizeof(this->t.in));
    this->t.category = category;
    this->t.in.seed = r.next() | 1;
    for (uint8_t z = 0; z < 8; z++) {
      this->t.in.regs[z] = r.value();
    }
    this->t.in.regs[4] = STACK_BASE + STACK_ESP;
    // Arithmetic flags random, DF clear; IF and bit 1 as a user-mode thread has them. (Never TF/AC/NT.)
    this->t.in.eflags = (r.next() & F_ARITH) | 0x202;
    this->t.in.fcw = 0x037F;
    this->t.in.patch_len = PATCH_MAX;
    for (uint32_t z = 0; z < PATCH_MAX; z += 4) {
      uint32_t v = r.value();
      memcpy(this->t.in.patch + z, &v, 4);
    }
  }

  void b(uint8_t v) {
    this->code.push_back(v);
  }
  void w(uint16_t v) {
    b(v);
    b(v >> 8);
  }
  void d(uint32_t v) {
    w(v);
    w(v >> 16);
  }
  void bytes(std::initializer_list<uint8_t> v) {
    for (uint8_t x : v) {
      b(x);
    }
  }
  void imm(uint32_t v, uint8_t size) {
    if (size == 8) {
      b(v);
    } else if (size == 16) {
      w(v);
    } else {
      d(v);
    }
  }
  void set_reg(uint8_t reg, uint32_t v) {
    this->t.in.regs[reg] = v;
    this->pinned |= (1 << reg);
  }
  void set_flag(uint32_t mask, bool v) {
    this->t.in.eflags = (this->t.in.eflags & ~mask) | (v ? mask : 0);
  }
  void patch32(uint32_t off, uint32_t v) {
    memcpy(this->t.in.patch + off, &v, 4);
  }
  void patch64(uint32_t off, uint64_t v) {
    memcpy(this->t.in.patch + off, &v, 8);
  }
  void patch_bytes(uint32_t off, const void* p, size_t n) {
    memcpy(this->t.in.patch + off, p, n);
  }

  uint8_t pick_reg(uint8_t avoid, bool allow_esp = false, bool allow_ebp = true) {
    for (;;) {
      uint8_t reg = r.below(8);
      if ((avoid & (1 << reg)) || (reg == 4 && !allow_esp) || (reg == 5 && !allow_ebp)) {
        continue;
      }
      return reg;
    }
  }

  void modrm_reg(uint8_t reg, uint8_t rm) {
    b(0xC0 | ((reg & 7) << 3) | (rm & 7));
  }

  // A 32-bit-addressing memory operand at `target`, in a randomly chosen form. The base/index registers it uses are
  // set (and pinned); `avoid` lists registers with other roles in the instruction.
  void modrm_mem(uint8_t reg, uint32_t target, uint8_t avoid = 0) {
    avoid |= this->pinned;
    reg = (reg & 7) << 3;
    switch (r.below(7)) {
      case 0: // [disp32]
        b(0x05 | reg);
        d(target);
        return;
      case 1: { // [base]
        uint8_t base = pick_reg(avoid, false, false);
        b(reg | base);
        set_reg(base, target);
        return;
      }
      case 2: // [base + disp8]
      case 3: { // [base + disp32]
        uint8_t base = pick_reg(avoid);
        bool d8 = (r.below(4) != 3);
        int32_t disp = d8 ? static_cast<int8_t>(r.next()) : static_cast<int32_t>(r.next());
        b((d8 ? 0x40 : 0x80) | reg | base);
        if (d8) {
          b(disp);
        } else {
          d(disp);
        }
        set_reg(base, target - disp);
        return;
      }
      case 4:
      case 5: { // [base + index*scale (+ disp)]
        uint8_t base = pick_reg(avoid);
        uint8_t index = pick_reg(avoid | (1 << base));
        uint8_t ss = r.below(4);
        int32_t iv = static_cast<int32_t>(r.below(33)) - 16;
        uint8_t mod = (base == 5) ? (r.coin() ? 1 : 2) : r.below(3);
        int32_t disp = (mod == 1) ? static_cast<int8_t>(r.next()) : ((mod == 2) ? static_cast<int32_t>(r.next()) : 0);
        b((mod << 6) | reg | 4);
        b((ss << 6) | (index << 3) | base);
        if (mod == 1) {
          b(disp);
        } else if (mod == 2) {
          d(disp);
        }
        set_reg(index, iv);
        set_reg(base, target - (iv << ss) - disp);
        return;
      }
      default: { // [index*scale + disp32] (no base)
        uint8_t index = pick_reg(avoid);
        uint8_t ss = r.below(4);
        int32_t iv = static_cast<int32_t>(r.below(33)) - 16;
        b(reg | 4);
        b((ss << 6) | (index << 3) | 5);
        d(target - (iv << ss));
        set_reg(index, iv);
        return;
      }
    }
  }

  uint32_t data_target(uint32_t size) {
    return DATA_BASE + r.below(PATCH_MAX - size + 1);
  }

  // Register or memory operand for r/m, chosen at random.
  void modrm_any(uint8_t reg, uint32_t size_bytes, uint8_t avoid = 0) {
    if (r.coin()) {
      uint8_t rm = r.below(8);
      modrm_reg(reg, rm);
    } else {
      modrm_mem(reg, data_target(size_bytes), avoid);
    }
  }

  void size_prefix(uint8_t size) {
    if (size == 16) {
      b(0x66);
    }
  }

  TestCase finish() {
    if (this->code.size() > CODE_MAX) {
      fprintf(stderr, "generator bug: snippet too long (%zu bytes) in %s\n", this->code.size(), this->t.category.c_str());
      abort();
    }
    this->t.in.code_len = this->code.size();
    memcpy(this->t.in.code, this->code.data(), this->code.size());
    return this->t;
  }
};

uint8_t random_size(Rng& r) {
  static const uint8_t sizes[3] = {8, 16, 32};
  return sizes[r.below(3)];
}

// ---- Generators -----------------------------------------------------------------------------------------------------

void gen_alu(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "alu");
    uint8_t op = r.below(8);
    uint8_t size = random_size(r);
    uint8_t w = (size != 8);
    bl.size_prefix(size);
    switch (r.below(4)) {
      case 0: // op r/m, reg
        bl.b((op << 3) | w);
        bl.modrm_any(r.below(8), size / 8);
        break;
      case 1: // op reg, r/m
        bl.b((op << 3) | 2 | w);
        bl.modrm_any(r.below(8), size / 8);
        break;
      case 2: // op acc, imm
        bl.b((op << 3) | 4 | w);
        bl.imm(r.value(), size);
        break;
      default: { // 80/81/83 /op r/m, imm
        uint8_t opc = (size == 8) ? 0x80 : (r.coin() ? 0x81 : 0x83);
        bl.b(opc);
        bl.modrm_any(op, size / 8);
        bl.imm(r.value(), (opc == 0x81) ? size : 8);
        break;
      }
    }
    if (op == 1 || op == 4 || op == 6) {
      bl.t.flags_mask &= ~F_AF; // or/and/xor: AF undefined
    }
    out.push_back(bl.finish());
  }
}

void gen_test_xchg_mov(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "mov/test/xchg");
    uint8_t size = random_size(r);
    uint8_t w = (size != 8);
    bl.size_prefix(size);
    switch (r.below(8)) {
      case 0: // test r/m, reg
        bl.b(0x84 | w);
        bl.modrm_any(r.below(8), size / 8);
        bl.t.flags_mask &= ~F_AF;
        break;
      case 1: // test acc, imm
        bl.b(0xA8 | w);
        bl.imm(r.value(), size);
        bl.t.flags_mask &= ~F_AF;
        break;
      case 2: // F6/F7 /0 test r/m, imm
        bl.b(0xF6 | w);
        bl.modrm_any(0, size / 8);
        bl.imm(r.value(), size);
        bl.t.flags_mask &= ~F_AF;
        break;
      case 3: // xchg r/m, reg
        bl.b(0x86 | w);
        bl.modrm_any(r.below(8), size / 8);
        break;
      case 4: // mov r/m <-> reg (88-8B)
        bl.b(0x88 | w | (r.coin() ? 2 : 0));
        bl.modrm_any(r.below(8), size / 8);
        break;
      case 5: // mov r/m, imm (C6/C7)
        bl.b(0xC6 | w);
        bl.modrm_any(0, size / 8);
        bl.imm(r.value(), size);
        break;
      case 6: // mov reg, imm (B0-BF)
        bl.b((w ? 0xB8 : 0xB0) | r.below(8));
        bl.imm(r.value(), size);
        break;
      default: { // mov acc <-> moffs32
        bl.b(0xA0 | w | (r.coin() ? 2 : 0));
        bl.d(bl.data_target(size / 8));
        break;
      }
    }
    out.push_back(bl.finish());
  }
}

void gen_unary(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "inc/dec/neg/not");
    uint8_t size = random_size(r);
    uint8_t w = (size != 8);
    switch (r.below(3)) {
      case 0: // FE/FF /0 /1
        bl.size_prefix(size);
        bl.b(0xFE | w);
        bl.modrm_any(r.below(2), size / 8);
        break;
      case 1: // F6/F7 /2 /3
        bl.size_prefix(size);
        bl.b(0xF6 | w);
        bl.modrm_any(2 + r.below(2), size / 8);
        break;
      default: // 40-4F (16/32 only)
        if (size == 8) {
          size = 32;
        }
        bl.size_prefix(size);
        bl.b(0x40 | r.below(16));
        break;
    }
    out.push_back(bl.finish());
  }
}

void gen_shifts(Rng& r, std::vector<TestCase>& out, size_t n) {
  static const uint8_t counts[] = {0, 1, 1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 31, 32, 33, 63, 255, 0x41};
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "shift/rotate");
    uint8_t size = random_size(r);
    uint8_t w = (size != 8);
    uint8_t op = r.below(8);
    uint8_t count = (r.below(4) == 0) ? static_cast<uint8_t>(r.next()) : counts[r.below(sizeof(counts))];
    uint8_t form = r.below(3);
    bl.size_prefix(size);
    if (form == 0) { // C0/C1 imm8
      bl.b(0xC0 | w);
      bl.modrm_any(op, size / 8);
      bl.b(count);
    } else if (form == 1) { // D0/D1 by 1
      count = 1;
      bl.b(0xD0 | w);
      bl.modrm_any(op, size / 8);
    } else { // D2/D3 by CL
      bl.set_reg(1, (bl.t.in.regs[1] & ~0xFF) | count);
      bl.b(0xD2 | w);
      bl.modrm_any(op, size / 8, 1 << 1);
    }
    // Undefined flags: see the SDM's per-instruction flag notes.
    uint32_t masked = count & 0x1F;
    if (masked != 0) {
      if (op <= 3) { // rotates: only CF and OF are written; OF only defined for 1-bit rotates
        if (masked != 1) {
          bl.t.flags_mask &= ~F_OF;
        }
      } else {
        bl.t.flags_mask &= ~F_AF;
        if (masked != 1) {
          bl.t.flags_mask &= ~F_OF;
        }
        if ((op == 4 || op == 5 || op == 6) && masked >= size) {
          bl.t.flags_mask &= ~F_CF; // shl/shr: CF undefined once the count reaches the width
        }
      }
    }
    out.push_back(bl.finish());
  }
}

void gen_shld(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "shld/shrd");
    uint8_t size = r.coin() ? 16 : 32;
    bool right = r.coin();
    bool by_cl = r.coin();
    uint8_t count = r.below(size + 1);
    if (r.below(8) == 0) {
      count |= 0x20 * (1 + r.below(7)); // high bits are masked off
    }
    bl.size_prefix(size);
    bl.b(0x0F);
    if (by_cl) {
      bl.set_reg(1, (bl.t.in.regs[1] & ~0xFF) | count);
      bl.b(right ? 0xAD : 0xA5);
      bl.modrm_any(bl.pick_reg(1 << 1, true), size / 8, 1 << 1);
    } else {
      bl.b(right ? 0xAC : 0xA4);
      bl.modrm_any(r.below(8), size / 8);
      bl.b(count);
    }
    uint32_t masked = count & 0x1F;
    if (masked) {
      bl.t.flags_mask &= ~F_AF;
      if (masked != 1) {
        bl.t.flags_mask &= ~F_OF;
      }
    }
    out.push_back(bl.finish());
  }
}

void gen_muldiv(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    uint8_t size = random_size(r);
    uint8_t w = (size != 8);
    uint8_t kind = r.below(6);
    if (kind <= 1) { // mul / imul (one operand)
      Builder bl(r, "mul/imul");
      bl.size_prefix(size);
      bl.b(0xF6 | w);
      bl.modrm_any(4 + kind, size / 8, (1 << 0) | (1 << 2));
      bl.t.flags_mask = F_CF | F_OF | F_DF;
      out.push_back(bl.finish());
    } else if (kind == 2) { // imul r, r/m and imul r, r/m, imm
      Builder bl(r, "imul 2/3-op");
      if (size == 8) {
        size = 32;
      }
      bl.size_prefix(size);
      switch (r.below(3)) {
        case 0:
          bl.bytes({0x0F, 0xAF});
          bl.modrm_any(r.below(8), size / 8);
          break;
        case 1:
          bl.b(0x69);
          bl.modrm_any(r.below(8), size / 8);
          bl.imm(r.value(), size);
          break;
        default:
          bl.b(0x6B);
          bl.modrm_any(r.below(8), size / 8);
          bl.b(r.next());
          break;
      }
      bl.t.flags_mask = F_CF | F_OF | F_DF;
      out.push_back(bl.finish());
    } else { // div / idiv
      Builder bl(r, "div/idiv");
      bool is_signed = (kind & 1);
      bool want_fault = (r.below(10) == 0);
      bl.size_prefix(size);
      // Divisor in a register other than the implicit ones, so its value is known.
      uint8_t divisor_reg = bl.pick_reg((1 << 0) | (1 << 2), false);
      uint32_t mask = (size == 32) ? 0xFFFFFFFF : ((1u << size) - 1);
      uint32_t divisor = r.value() & mask;
      if (!want_fault && divisor == 0) {
        divisor = 1 + r.below(mask < 1000 ? mask : 1000);
      } else if (want_fault && r.coin()) {
        divisor = 0;
      }
      // A dividend whose quotient fits: quotient * divisor + remainder.
      uint64_t q = r.value() & (mask >> 1);
      uint64_t rem = divisor ? (r.next() % divisor) : 0;
      if (is_signed) {
        int64_t sd = (size == 8) ? static_cast<int8_t>(divisor) : ((size == 16) ? static_cast<int16_t>(divisor) : static_cast<int32_t>(divisor));
        int64_t sq = static_cast<int64_t>(q) * (r.coin() ? 1 : -1);
        if (want_fault) {
          sq = (static_cast<int64_t>(mask >> 1) + 1 + r.below(100)) * (r.coin() ? 1 : -1);
        }
        int64_t prod = sq * sd;
        // The remainder takes the dividend's sign (idiv truncates toward zero).
        int64_t srem = sd ? (static_cast<int64_t>(r.next() % (sd < 0 ? -sd : sd)) * ((prod < 0) ? -1 : 1)) : 0;
        int64_t dividend = prod + srem;
        if (size == 8) {
          bl.set_reg(0, (bl.t.in.regs[0] & ~0xFFFF) | (dividend & 0xFFFF));
        } else if (size == 16) {
          bl.set_reg(0, (bl.t.in.regs[0] & ~0xFFFF) | (dividend & 0xFFFF));
          bl.set_reg(2, (bl.t.in.regs[2] & ~0xFFFF) | ((dividend >> 16) & 0xFFFF));
        } else {
          bl.set_reg(0, static_cast<uint32_t>(dividend));
          bl.set_reg(2, static_cast<uint32_t>(dividend >> 32));
        }
      } else {
        if (want_fault) {
          q = static_cast<uint64_t>(mask) + 1 + r.below(100);
        }
        uint64_t dividend = q * divisor + rem;
        if (size == 8) {
          bl.set_reg(0, (bl.t.in.regs[0] & ~0xFFFF) | (dividend & 0xFFFF));
        } else if (size == 16) {
          bl.set_reg(0, (bl.t.in.regs[0] & ~0xFFFF) | (dividend & 0xFFFF));
          bl.set_reg(2, (bl.t.in.regs[2] & ~0xFFFF) | ((dividend >> 16) & 0xFFFF));
        } else {
          bl.set_reg(0, static_cast<uint32_t>(dividend));
          bl.set_reg(2, static_cast<uint32_t>(dividend >> 32));
        }
      }
      if (size == 8 && divisor_reg >= 4) {
        divisor_reg = 3; // ah/ch/dh/bh would alias; use bl
      }
      if (size == 8) {
        bl.set_reg(divisor_reg, (bl.t.in.regs[divisor_reg] & ~0xFF) | divisor);
      } else {
        bl.set_reg(divisor_reg, (size == 16) ? ((bl.t.in.regs[divisor_reg] & ~0xFFFF) | divisor) : divisor);
      }
      bl.b(0xF6 | w);
      bl.modrm_reg(is_signed ? 7 : 6, divisor_reg);
      bl.t.flags_mask = F_DF; // all arithmetic flags undefined
      out.push_back(bl.finish());
    }
  }
}

void gen_bitops(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "bt/bts/btr/btc");
    uint8_t size = r.coin() ? 16 : 32;
    uint8_t op = r.below(4);
    bl.size_prefix(size);
    if (r.coin()) { // 0F A3/AB/B3/BB with a register bit offset
      uint8_t src = r.below(8);
      bool mem = r.coin();
      if (mem) {
        // A signed bit offset that stays within the data region around the operand.
        int32_t bit = static_cast<int32_t>(r.below(256)) - 96;
        if (size == 16) {
          bl.set_reg(src, (bl.t.in.regs[src] & ~0xFFFF) | (bit & 0xFFFF));
        } else {
          bl.set_reg(src, bit);
        }
        bl.bytes({0x0F, static_cast<uint8_t>(0xA3 + op * 8)});
        bl.modrm_mem(src, DATA_BASE + 16 + r.below(8), 1 << src);
      } else {
        bl.bytes({0x0F, static_cast<uint8_t>(0xA3 + op * 8)});
        bl.modrm_reg(src, r.below(8));
      }
    } else { // 0F BA /4-7 imm8
      bl.bytes({0x0F, 0xBA});
      bl.modrm_any(4 + op, size / 8);
      bl.b(r.next());
    }
    bl.t.flags_mask = F_CF | F_ZF | F_DF;
    out.push_back(bl.finish());
  }
}

void gen_misc(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    uint8_t kind = r.below(19);
    uint8_t size = r.coin() ? 16 : 32;
    switch (kind) {
      case 0: { // bsf / bsr
        Builder bl(r, "bsf/bsr");
        bl.size_prefix(size);
        bl.bytes({0x0F, static_cast<uint8_t>(0xBC | r.below(2))});
        bl.modrm_any(r.below(8), size / 8);
        bl.t.flags_mask = F_ZF | F_DF;
        out.push_back(bl.finish());
        break;
      }
      case 1: { // bswap
        Builder bl(r, "bswap");
        bl.bytes({0x0F, static_cast<uint8_t>(0xC8 | r.below(8))});
        out.push_back(bl.finish());
        break;
      }
      case 2: { // cmpxchg
        Builder bl(r, "cmpxchg");
        uint8_t sz = random_size(r);
        bl.size_prefix(sz);
        bl.bytes({0x0F, static_cast<uint8_t>(0xB0 | (sz != 8))});
        uint32_t target = bl.data_target(sz / 8);
        if (r.coin()) { // make the comparison succeed
          uint32_t v;
          memcpy(&v, bl.t.in.patch + (target - DATA_BASE), 4);
          bl.set_reg(0, v);
        }
        if (r.coin()) {
          bl.modrm_reg(r.below(8), r.below(8));
        } else {
          bl.modrm_mem(r.below(8), target, 1 << 0);
        }
        out.push_back(bl.finish());
        break;
      }
      case 3: { // xadd
        Builder bl(r, "xadd");
        uint8_t sz = random_size(r);
        bl.size_prefix(sz);
        bl.bytes({0x0F, static_cast<uint8_t>(0xC0 | (sz != 8))});
        bl.modrm_any(r.below(8), sz / 8);
        out.push_back(bl.finish());
        break;
      }
      case 4: { // setcc
        Builder bl(r, "setcc");
        bl.bytes({0x0F, static_cast<uint8_t>(0x90 | r.below(16))});
        bl.modrm_any(0, 1);
        out.push_back(bl.finish());
        break;
      }
      case 5: { // cmovcc
        Builder bl(r, "cmovcc");
        bl.size_prefix(size);
        bl.bytes({0x0F, static_cast<uint8_t>(0x40 | r.below(16))});
        bl.modrm_any(r.below(8), size / 8);
        out.push_back(bl.finish());
        break;
      }
      case 6: { // movzx / movsx
        Builder bl(r, "movzx/movsx");
        bl.size_prefix(size);
        static const uint8_t ops[4] = {0xB6, 0xB7, 0xBE, 0xBF};
        uint8_t op = ops[r.below(4)];
        bl.bytes({0x0F, op});
        bl.modrm_any(r.below(8), (op & 1) ? 2 : 1);
        out.push_back(bl.finish());
        break;
      }
      case 7: { // cbw/cwde/cwd/cdq, lahf/sahf, cmc/clc/stc, salc
        Builder bl(r, "cbw/cwd/lahf/sahf/cmc/clc/stc");
        static const uint8_t ops[] = {0x98, 0x99, 0x9E, 0x9F, 0xF5, 0xF8, 0xF9, 0xD6};
        uint8_t op = ops[r.below(sizeof(ops))];
        if (op == 0x98 || op == 0x99) {
          bl.size_prefix(size);
        }
        bl.b(op);
        out.push_back(bl.finish());
        break;
      }
      case 8: { // xchg eax, reg (90-97)
        Builder bl(r, "xchg eax");
        bl.size_prefix(size);
        bl.b(0x90 | r.below(8));
        out.push_back(bl.finish());
        break;
      }
      case 9: { // lea (32-bit addressing)
        Builder bl(r, "lea");
        bl.size_prefix(size);
        bl.b(0x8D);
        bl.modrm_mem(r.below(8), r.next());
        out.push_back(bl.finish());
        break;
      }
      case 10:
      case 11: { // lea with 16-bit addressing (0x67): every mod/rm form, registers random (wraparound)
        Builder bl(r, "lea a16");
        bl.size_prefix(size);
        bl.b(0x67);
        bl.b(0x8D);
        uint8_t mod = r.below(3);
        uint8_t rm = r.below(8);
        bl.b((mod << 6) | (r.below(8) << 3) | rm);
        if (mod == 0 && rm == 6) {
          bl.w(r.next());
        } else if (mod == 1) {
          bl.b(r.next());
        } else if (mod == 2) {
          bl.w(r.next());
        }
        out.push_back(bl.finish());
        break;
      }
      case 12: { // BCD
        Builder bl(r, "bcd");
        static const uint8_t ops[] = {0x27, 0x2F, 0x37, 0x3F, 0xD4, 0xD5};
        uint8_t op = ops[r.below(sizeof(ops))];
        bl.b(op);
        if (op == 0xD4 || op == 0xD5) {
          uint8_t base = r.coin() ? 10 : static_cast<uint8_t>(r.next());
          if (op == 0xD4 && base == 0) {
            base = 7;
          }
          bl.b(base);
          bl.t.flags_mask = F_SF | F_ZF | F_PF | F_DF;
        } else if (op == 0x27 || op == 0x2F) {
          bl.t.flags_mask = F_CF | F_AF | F_SF | F_ZF | F_PF | F_DF;
        } else {
          bl.t.flags_mask = F_CF | F_AF | F_DF;
        }
        out.push_back(bl.finish());
        break;
      }
      case 13: { // aam 0 faults (#DE)
        Builder bl(r, "aam 0");
        bl.bytes({0xD4, 0x00});
        out.push_back(bl.finish());
        break;
      }
      case 14: { // push/pop sequences
        Builder bl(r, "push/pop");
        switch (r.below(9)) {
          case 0:
            bl.size_prefix(size);
            bl.b(0x50 | r.below(8));
            break;
          case 1:
            bl.size_prefix(size);
            bl.b(0x58 | r.below(8));
            break;
          case 2:
            bl.size_prefix(size);
            bl.b(0x6A);
            bl.b(r.next());
            break;
          case 3:
            bl.size_prefix(size);
            bl.b(0x68);
            bl.imm(r.next(), size);
            break;
          case 4: // push r/m
            bl.size_prefix(size);
            bl.b(0xFF);
            bl.modrm_mem(6, bl.data_target(4), 1 << 4);
            break;
          case 5: // pop r/m
            bl.size_prefix(size);
            bl.b(0x8F);
            bl.modrm_mem(0, bl.data_target(4), 1 << 4);
            break;
          case 6: // pusha / popa
            bl.size_prefix(size);
            bl.b(0x60);
            bl.size_prefix(size);
            bl.b(0x61);
            break;
          case 7: { // push imm; popfd (a controlled flags image); pushfd
            uint32_t flags = (r.next() & F_ALL) | 0x202;
            bl.b(0x68);
            bl.d(flags);
            bl.b(0x9D);
            bl.b(0x9C);
            break;
          }
          default: // enter / leave
            bl.size_prefix(size);
            bl.b(0xC8);
            bl.w(r.below(64));
            bl.b(r.below(4));
            bl.set_reg(5, STACK_BASE + 0x70 + r.below(8) * 2);
            if (r.coin()) {
              bl.size_prefix(size);
              bl.b(0xC9);
            }
            break;
        }
        out.push_back(bl.finish());
        break;
      }
      case 16: { // xlat
        Builder bl(r, "xlat");
        bl.set_reg(3, DATA_BASE); // [ebx + al] stays inside the 256-byte data region
        bl.b(0xD7);
        out.push_back(bl.finish());
        break;
      }
      case 17: { // bound (in range, and out of range -> #BR)
        Builder bl(r, "bound");
        bl.size_prefix(size);
        uint32_t target = bl.data_target(8);
        uint8_t reg = bl.pick_reg(1 << 4);
        int32_t lo = static_cast<int32_t>(r.value()), hi = static_cast<int32_t>(r.value());
        if (size == 16) {
          lo = static_cast<int16_t>(lo);
          hi = static_cast<int16_t>(hi);
        }
        if (lo > hi) {
          std::swap(lo, hi);
        }
        int64_t index = (r.below(4) == 0) ? (r.coin() ? static_cast<int64_t>(lo) - 1 : static_cast<int64_t>(hi) + 1)
                                          : lo + static_cast<int64_t>(r.next() % (static_cast<uint64_t>(hi) - lo + 1));
        if (size == 16) {
          uint16_t lo16 = lo, hi16 = hi;
          memcpy(bl.t.in.patch + (target - DATA_BASE), &lo16, 2);
          memcpy(bl.t.in.patch + (target - DATA_BASE) + 2, &hi16, 2);
        } else {
          bl.patch32(target - DATA_BASE, lo);
          bl.patch32(target - DATA_BASE + 4, hi);
        }
        bl.set_reg(reg, (size == 16) ? ((bl.t.in.regs[reg] & 0xFFFF0000) | (index & 0xFFFF)) : static_cast<uint32_t>(index));
        bl.b(0x62);
        bl.modrm_mem(reg, target, 1 << reg);
        out.push_back(bl.finish());
        break;
      }
      case 18: { // cmpxchg8b
        Builder bl(r, "cmpxchg8b");
        uint32_t target = bl.data_target(8);
        if (r.coin()) { // make the comparison succeed
          uint32_t lo, hi;
          memcpy(&lo, bl.t.in.patch + (target - DATA_BASE), 4);
          memcpy(&hi, bl.t.in.patch + (target - DATA_BASE) + 4, 4);
          bl.set_reg(0, lo);
          bl.set_reg(2, hi);
        }
        bl.bytes({0x0F, 0xC7});
        bl.modrm_mem(1, target, (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3));
        bl.t.flags_mask = F_ZF | F_DF;
        out.push_back(bl.finish());
        break;
      }
      default: { // near call / ret / ret imm16 inside the snippet
        Builder bl(r, "call/ret");
        // call +N ; (skipped bytes: mov al, 1) ; target: pop/ret imm...
        uint8_t variant = r.below(3);
        if (variant == 0) {
          bl.b(0xE8);
          bl.d(2);
          bl.bytes({0xB0, 0x01}); // skipped
          bl.b(0x58 | bl.pick_reg(1 << 4)); // pop the return address into a register
        } else if (variant == 1) {
          // call to a ret that returns to the instruction after the call
          bl.b(0xEB); // jmp over the "function"
          bl.b(3);
          bl.bytes({0xC2}); // ret imm16
          bl.w(r.below(4) * 4);
          bl.b(0xE8);
          bl.d(static_cast<uint32_t>(-8));
        } else {
          bl.b(0xEB);
          bl.b(1);
          bl.b(0xC3); // ret
          bl.b(0xE8);
          bl.d(static_cast<uint32_t>(-6));
        }
        out.push_back(bl.finish());
        break;
      }
    }
  }
}

void gen_branches(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "jcc/loop/jecxz");
    switch (r.below(4)) {
      case 0: // jcc short over a mov
        bl.b(0x70 | r.below(16));
        bl.b(2);
        bl.bytes({0xB0, 0x5A});
        break;
      case 1: // jcc near over a mov
        bl.bytes({0x0F, static_cast<uint8_t>(0x80 | r.below(16))});
        bl.d(2);
        bl.bytes({0xB1, 0x5A});
        break;
      case 2: { // a counted loop: inc ebx (or edx); loop/loope/loopne back
        uint32_t count = r.below(6);
        bool cx = r.coin(); // 0x67: the counter is CX, not ECX
        bl.set_reg(1, (r.next() & (cx ? 0xFFFF0000 : 0)) | count);
        uint8_t op = 0xE0 | r.below(3);
        if (op != 0xE2) {
          // Keep ZF meaningful for loope/loopne: cmp al, imm sets it right before the loop instruction.
          bl.b(0x40 | 2); // inc edx
          bl.bytes({0x3C, static_cast<uint8_t>(r.below(3))});
        } else {
          bl.b(0x43); // inc ebx
        }
        if (cx) {
          bl.b(0x67);
        }
        bl.b(op);
        int8_t back = -static_cast<int8_t>(bl.code.size() + 1);
        bl.b(back);
        if (count == 0 && !cx) {
          // ecx = 0 would loop 2^32 times; make it 1.
          bl.t.in.regs[1] = 1;
        }
        if (count == 0 && cx) {
          bl.t.in.regs[1] = (bl.t.in.regs[1] & 0xFFFF0000) | 1;
        }
        break;
      }
      default: { // jecxz / jcxz
        bool cx = r.coin();
        uint32_t v = r.coin() ? 0 : r.next();
        if (cx && r.coin()) {
          v &= 0xFFFF0000; // CX zero with ECX nonzero
        }
        bl.set_reg(1, v);
        if (cx) {
          bl.b(0x67);
        }
        bl.bytes({0xE3, 0x02, 0xB0, 0x5A});
        break;
      }
    }
    out.push_back(bl.finish());
  }
}

void gen_strings(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "string ops");
    static const uint8_t ops[] = {0xA4, 0xA6, 0xAA, 0xAC, 0xAE};
    uint8_t op = ops[r.below(5)] | (r.coin() ? 1 : 0);
    uint8_t size = (op & 1) ? (r.coin() ? 16 : 32) : 8;
    uint8_t esz = size / 8;
    bool df = (r.below(3) == 0);
    uint8_t rep = r.below(3); // 0 none, 1 F3, 2 F2
    uint32_t count = r.below(9);
    bl.set_flag(F_DF, df);
    // Both pointers inside the data region with room for count elements in the chosen direction.
    uint32_t span = count * esz + esz;
    uint32_t lo = df ? span : 0;
    uint32_t hi = df ? (DATA_SIZE - esz) : (DATA_SIZE - span);
    bl.set_reg(6, DATA_BASE + lo + r.below(hi - lo + 1));
    bl.set_reg(7, DATA_BASE + lo + r.below(hi - lo + 1));
    bl.set_reg(1, count);
    if (rep != 0 && (op & 0x0E) != 0x06 && (op & 0x0E) != 0x0E && r.coin()) {
      // Overlapping movs (the replicating memcpy idiom) - the fast path must not change the result.
      if ((op & 0x0E) == 0x04) {
        bl.set_reg(7, bl.t.in.regs[6] + (df ? -esz : esz));
      }
    }
    // Make repe/repne comparisons run for a while: copy the source over the destination sometimes.
    if (rep != 0 && ((op & 0x0E) == 0x06) && r.coin()) {
      bl.set_reg(7, bl.t.in.regs[6]);
    }
    if (r.coin() && ((op & 0x0E) == 0x0E)) {
      uint32_t v;
      memcpy(&v, bl.t.in.patch + (r.below(PATCH_MAX - 4) & ~3), 4);
      bl.set_reg(0, v);
    }
    bl.size_prefix(size);
    if (rep == 1) {
      bl.b(0xF3);
    } else if (rep == 2) {
      bl.b(0xF2);
    }
    bl.b(op);
    out.push_back(bl.finish());
  }
}

// ---- x87 ------------------------------------------------------------------------------------------------------------

uint64_t random_double_bits(Rng& r) {
  static const double specials[] = {0.0, -0.0, 1.0, -1.0, 2.0, 0.5, 3.0, 10.0, 1.0 / 3.0, 3.14159265358979323846,
      2.71828182845904523536, 1e10, -1e10,
      1e300, -1e300, 1e-300, 4.9e-324, 1.7976931348623157e308, 12345.678, -0.1, 0.75, 1e-5, 65536.5, -2.5, 3.5,
      9007199254740993.0, 2147483647.5, -2147483648.5, 32767.5, -32768.5, 1e20, -1e20};
  switch (r.below(8)) {
    case 0: {
      static const uint64_t bits[] = {0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL,
          0x7FF0000000000001ULL, 0xFFF8000000000001ULL, 0x000FFFFFFFFFFFFFULL, 0x8000000000000001ULL};
      return bits[r.below(sizeof(bits) / 8)];
    }
    case 1:
    case 2:
    case 3: {
      double d = specials[r.below(sizeof(specials) / sizeof(specials[0]))];
      uint64_t v;
      memcpy(&v, &d, 8);
      return v;
    }
    default: {
      // Random significand, exponent within +-2^40 so most operations stay finite.
      uint64_t mant = (static_cast<uint64_t>(r.next()) << 32 | r.next()) & 0x000FFFFFFFFFFFFFULL;
      uint64_t exp = 1023 - 40 + r.below(81);
      uint64_t sign = r.coin() ? (1ULL << 63) : 0;
      return sign | (exp << 52) | mant;
    }
  }
}

uint16_t random_fcw(Rng& r) {
  static const uint16_t pcs[3] = {0x000, 0x200, 0x300};
  return 0x007F | pcs[r.below(3)] | (r.below(4) << 10);
}

// fld qword [ebx+off]
void fld64(Builder& bl, uint8_t off) {
  bl.bytes({0xDD, 0x43, off});
}

void gen_x87(Rng& r, std::vector<TestCase>& out, size_t n) {
  for (size_t z = 0; z < n; z++) {
    Builder bl(r, "x87");
    bl.t.compare_fpu = true;
    bl.t.in.fcw = (r.below(3) == 0) ? 0x037F : random_fcw(r);
    bl.set_reg(3, DATA_BASE); // ebx -> operands
    for (uint32_t off = 0; off < 32; off += 8) {
      bl.patch64(off, random_double_bits(r));
    }
    uint8_t kind = r.below(15);
    switch (kind) {
      case 0: { // register arithmetic, all encodings
        bl.t.category = "x87 arith st";
        fld64(bl, 0);
        fld64(bl, 8);
        if (r.coin()) {
          fld64(bl, 16);
        }
        static const uint8_t pre[3] = {0xD8, 0xDC, 0xDE};
        uint8_t p = pre[r.below(3)];
        uint8_t op = r.below(8);
        if (op == 2 || op == 3) {
          op = r.coin() ? 0 : 1;
        }
        bl.b(p);
        bl.b(0xC0 | (op << 3) | (1 + r.below(2)));
        break;
      }
      case 1: { // memory arithmetic: m32real, m64real, m32int, m16int
        bl.t.category = "x87 arith mem";
        fld64(bl, 0);
        uint8_t op = r.below(8);
        static const uint8_t pre[4] = {0xD8, 0xDC, 0xDA, 0xDE};
        uint8_t p = pre[r.below(4)];
        if (p == 0xD8) {
          float f;
          uint64_t bits = random_double_bits(r);
          double d;
          memcpy(&d, &bits, 8);
          f = static_cast<float>(d);
          bl.patch_bytes(8, &f, 4);
        } else if (p == 0xDA || p == 0xDE) {
          bl.patch32(8, r.value());
        }
        bl.bytes({p, static_cast<uint8_t>(0x43 | (op << 3)), 8});
        if (op == 2 || op == 3) {
          bl.bytes({0xDF, 0xE0}); // fnstsw ax
        }
        break;
      }
      case 2: { // compares
        bl.t.category = "x87 compare";
        if (r.below(4) == 0) {
          bl.patch64(8, *reinterpret_cast<uint64_t*>(bl.t.in.patch)); // equal operands
        }
        fld64(bl, 0);
        fld64(bl, 8);
        static const uint8_t forms[][2] = {{0xD8, 0xD1}, {0xD8, 0xD9}, {0xDE, 0xD9}, {0xDD, 0xE1}, {0xDD, 0xE9},
            {0xDA, 0xE9}, {0xDB, 0xF1}, {0xDB, 0xE9}, {0xDF, 0xF1}, {0xDF, 0xE9}, {0xD9, 0xE4}, {0xD9, 0xE5},
            {0xDC, 0xD1}, {0xDC, 0xD9}};
        const uint8_t* f = forms[r.below(sizeof(forms) / 2)];
        bl.bytes({f[0], f[1]});
        bl.bytes({0xDF, 0xE0}); // fnstsw ax
        break;
      }
      case 3: { // unary / transcendental
        bl.t.category = "x87 unary";
        static const uint8_t ops[] = {0xE0, 0xE1, 0xFA, 0xFC, 0xF0, 0xFE, 0xFF, 0xFB, 0xF2, 0xF4, 0xFD, 0xF8, 0xF5,
            0xF3, 0xF1, 0xF9};
        uint8_t op = ops[r.below(sizeof(ops))];
        if (op == 0xF0 && r.coin()) {
          double d = (static_cast<int32_t>(r.next() % 2001) - 1000) / 1000.0; // f2xm1's domain
          bl.patch64(8, *reinterpret_cast<uint64_t*>(&d));
        }
        bool two = (op == 0xFD || op == 0xF8 || op == 0xF5 || op == 0xF3 || op == 0xF1 || op == 0xF9);
        if (two) {
          fld64(bl, 0);
        }
        fld64(bl, 8);
        bl.bytes({0xD9, op});
        bl.bytes({0xDF, 0xE0});
        break;
      }
      case 4: { // conversions to/from memory
        bl.t.category = "x87 load/store";
        switch (r.below(4)) {
          case 0: // fld m32/m64/m80 -> fstp m32/m64/m80
          {
            static const uint8_t loads[3][2] = {{0xD9, 0x00}, {0xDD, 0x00}, {0xDB, 0x28}};
            static const uint8_t stores[3][2] = {{0xD9, 0x18}, {0xDD, 0x18}, {0xDB, 0x38}};
            uint8_t li = r.below(3), si = r.below(3);
            if (li == 2) {
              // A plausible 80-bit value: a double's significand with its J bit set.
              uint64_t m = (static_cast<uint64_t>(r.next()) << 32) | r.next() | (r.below(8) ? 0x8000000000000000ULL : 0);
              uint16_t se = static_cast<uint16_t>(0x3FFF - 64 + r.below(128)) | (r.coin() ? 0x8000 : 0);
              if (r.below(8) == 0) {
                se = r.coin() ? 0x7FFF : 0;
              }
              bl.patch64(0, m);
              bl.patch_bytes(8, &se, 2);
            }
            bl.bytes({loads[li][0], static_cast<uint8_t>(0x43 | loads[li][1]), 0});
            bl.bytes({stores[si][0], static_cast<uint8_t>(0x43 | stores[si][1]), 32});
            if (r.coin()) {
              bl.bytes({0xDF, 0xE0});
            }
            break;
          }
          case 1: { // fild m16/m32/m64 -> fistp/fist/fisttp
            static const uint8_t loads[3][2] = {{0xDF, 0x00}, {0xDB, 0x00}, {0xDF, 0x28}};
            uint8_t li = r.below(3);
            uint64_t v = (static_cast<uint64_t>(r.value()) << 32) | r.value();
            bl.patch64(0, v);
            bl.bytes({loads[li][0], static_cast<uint8_t>(0x43 | loads[li][1]), 0});
            fld64(bl, 8);
            bl.bytes({0xDE, 0xC1}); // faddp: make it fractional sometimes
            static const uint8_t stores[][2] = {{0xDF, 0x10}, {0xDF, 0x18}, {0xDB, 0x10}, {0xDB, 0x18}, {0xDF, 0x38},
                {0xDF, 0x08}, {0xDB, 0x08}, {0xDD, 0x08}};
            const uint8_t* s = stores[r.below(sizeof(stores) / 2)];
            bl.bytes({s[0], static_cast<uint8_t>(0x43 | s[1]), 32});
            bl.bytes({0xDF, 0xE0});
            break;
          }
          case 2: { // fbld / fbstp
            uint8_t bcd[10];
            for (uint8_t z2 = 0; z2 < 9; z2++) {
              bcd[z2] = (r.below(10) << 4) | r.below(10);
            }
            bcd[9] = r.coin() ? 0x80 : 0x00;
            bl.patch_bytes(0, bcd, 10);
            bl.bytes({0xDF, 0x63, 0}); // fbld [ebx]
            if (r.coin()) {
              fld64(bl, 16);
              bl.bytes({0xDE, 0xC9}); // fmulp
            }
            bl.bytes({0xDF, 0x73, 32}); // fbstp [ebx+32]
            bl.bytes({0xDF, 0xE0});
            break;
          }
          default: { // fst m32/m64 without pop, then fstp
            fld64(bl, 0);
            bl.bytes({0xD9, 0x53, 32}); // fst dword [ebx+32]
            bl.bytes({0xDD, 0x53, 40}); // fst qword [ebx+40]
            bl.bytes({0xDF, 0xE0});
            break;
          }
        }
        break;
      }
      case 5: { // constants
        bl.t.category = "x87 constants";
        uint8_t c = 0xE8 + r.below(7);
        bl.bytes({0xD9, c});
        if (r.coin()) {
          bl.bytes({0xD9, static_cast<uint8_t>(0xE8 + r.below(7))});
          bl.bytes({0xDE, 0xC9}); // fmulp
        }
        break;
      }
      case 6: { // stack manipulation
        bl.t.category = "x87 stack";
        fld64(bl, 0);
        fld64(bl, 8);
        fld64(bl, 16);
        switch (r.below(7)) {
          case 0:
            bl.bytes({0xD9, static_cast<uint8_t>(0xC8 | r.below(3))}); // fxch
            break;
          case 1:
            bl.bytes({0xDD, static_cast<uint8_t>(0xC0 | r.below(3))}); // ffree
            break;
          case 2:
            bl.bytes({0xD9, static_cast<uint8_t>(0xF6 | r.below(2))}); // fdecstp/fincstp
            break;
          case 3:
            bl.bytes({0xD9, static_cast<uint8_t>(0xC0 | r.below(3))}); // fld st(i)
            break;
          case 4:
            bl.bytes({0xDD, static_cast<uint8_t>(0xD0 | r.below(3))}); // fst st(i)
            break;
          case 5:
            bl.bytes({0xDD, static_cast<uint8_t>(0xD8 | r.below(3))}); // fstp st(i)
            break;
          default: { // fcmovcc
            static const uint8_t pre[2] = {0xDA, 0xDB};
            bl.bytes({pre[r.below(2)], static_cast<uint8_t>(0xC0 | (r.below(4) << 3) | (1 + r.below(2)))});
            break;
          }
        }
        bl.bytes({0xDF, 0xE0});
        break;
      }
      case 7: { // stack overflow and underflow (masked)
        bl.t.category = "x87 stack faults";
        bl.t.in.fcw |= 0x3F;
        if (r.coin()) {
          for (uint8_t z2 = 0; z2 < 9; z2++) {
            bl.bytes({0xD9, 0xE8}); // fld1 x9: the 9th overflows
          }
        } else {
          fld64(bl, 0);
          bl.bytes({0xD8, 0xC0 | 3}); // fadd st, st(3): empty
          bl.bytes({0xDE, 0xC1}); // faddp with st1 empty
        }
        bl.bytes({0xDF, 0xE0});
        break;
      }
      case 8: { // fnstcw / fldcw round trip, fnstsw m16
        bl.t.category = "x87 control";
        uint16_t cw = random_fcw(r);
        bl.patch_bytes(0, &cw, 2);
        bl.bytes({0xD9, 0x7B, 32}); // fnstcw [ebx+32]
        bl.bytes({0xD9, 0x6B, 0}); // fldcw [ebx]
        fld64(bl, 8);
        fld64(bl, 16);
        bl.bytes({0xDE, 0xF9}); // fdivp
        bl.bytes({0xDD, 0x7B, 36}); // fnstsw [ebx+36]
        break;
      }
      case 9: { // fprem / fprem1 loop until complete
        bl.t.category = "x87 fprem";
        fld64(bl, 0);
        fld64(bl, 8);
        bl.bytes({0xD9, static_cast<uint8_t>(r.coin() ? 0xF8 : 0xF5)});
        bl.bytes({0xDF, 0xE0});
        break;
      }
      case 10: { // frndint under every rounding mode, fistp results
        bl.t.category = "x87 rounding";
        double d = (static_cast<int32_t>(r.next() % 20001) - 10000) / 4.0 + ((r.next() % 8) / 8.0);
        bl.patch64(8, *reinterpret_cast<uint64_t*>(&d));
        fld64(bl, 8);
        bl.bytes({0xD9, 0xFC}); // frndint
        fld64(bl, 8);
        bl.bytes({0xDB, 0x5B, 32}); // fistp dword [ebx+32]
        bl.bytes({0xDF, 0xE0});
        break;
      }
      case 11: { // mixed expression, results stored
        bl.t.category = "x87 expression";
        fld64(bl, 0);
        fld64(bl, 8);
        bl.bytes({0xD8, 0xC9}); // fmul st, st(1)
        fld64(bl, 16);
        bl.bytes({0xDE, 0xC1}); // faddp
        bl.bytes({0xD9, 0xFA}); // fsqrt
        bl.bytes({0xDD, 0x5B, 32}); // fstp qword [ebx+32]
        bl.bytes({0xDF, 0xE0});
        break;
      }
      case 12: { // fnstsw / sahf / jcc: the classic compiled floating-point compare
        bl.t.category = "x87 fcomp+sahf";
        fld64(bl, 0);
        bl.bytes({0xDC, 0x5B, 8}); // fcomp qword [ebx+8]
        bl.bytes({0xDF, 0xE0}); // fnstsw ax
        bl.b(0x9E); // sahf
        bl.bytes({0x0F, static_cast<uint8_t>(0x90 | r.below(16)), 0xC1}); // setcc cl
        break;
      }
      case 13: { // unmasked exceptions: flagged by the operation, reported (#MF) by the next waiting instruction
        // Borland's runtime starts programs with invalid, zero-divide and overflow unmasked (control word 0x1332),
        // so this path is live for the AD 4 modules.
        bl.t.category = "x87 unmasked";
        static const uint16_t unmask[] = {0x01, 0x04, 0x08, 0x10, 0x20, 0x02, 0x0D};
        bl.t.in.fcw = static_cast<uint16_t>(random_fcw(r) & ~unmask[r.below(sizeof(unmask) / 2)]);
        if (r.coin()) {
          // Values that overflow or underflow single precision on a store, or double precision after a multiply.
          static const double extremes[] = {1e39, -1e39, 1e-39, -1e-45, 1e-44, 1e300, -1e300, 1e-300, 3.4028236e38,
              1.1754942e-38};
          for (uint32_t off = 0; off < 16; off += 8) {
            double d = extremes[r.below(sizeof(extremes) / sizeof(extremes[0]))];
            bl.patch64(off, *reinterpret_cast<uint64_t*>(&d));
          }
        }
        fld64(bl, 0);
        switch (r.below(8)) {
          case 6: // compares, with a NaN (quiet or signaling) half of the time
          case 7: {
            static const uint64_t nans[] = {0x7FF8000000000000ULL, 0x7FF0000000000001ULL, 0xFFF4000000000000ULL};
            if (r.coin()) {
              bl.patch64(8, nans[r.below(3)]);
            }
            switch (r.below(4)) {
              case 0:
                bl.bytes({0xDC, 0x5B, 8}); // fcomp qword [ebx+8]
                break;
              case 1:
                fld64(bl, 8);
                bl.bytes({0xDD, 0xE9}); // fucomp st1
                break;
              case 2:
                fld64(bl, 8);
                bl.bytes({0xDF, 0xF1}); // fcomip st, st1
                bl.bytes({0x0F, 0x92, 0xC1}); // setb cl
                break;
              default:
                fld64(bl, 8);
                bl.bytes({0xDB, 0xE9}); // fucomi st, st1
                bl.bytes({0x0F, 0x94, 0xC1}); // sete cl
                break;
            }
            break;
          }
          case 5:
            bl.bytes({0xDC, 0x4B, 8}); // fmul qword [ebx+8]
            bl.bytes({0xDD, 0x5B, 32}); // fstp qword [ebx+32]
            break;
          case 0:
            fld64(bl, 8);
            bl.bytes({0xDE, 0xF9}); // fdivp
            break;
          case 1:
            bl.bytes({0xDC, 0x4B, 8}); // fmul qword [ebx+8]
            break;
          case 2:
            bl.bytes({0xD9, 0xFA}); // fsqrt
            break;
          case 3:
            bl.bytes({0xDB, 0x5B, 32}); // fistp dword [ebx+32]
            break;
          default:
            bl.bytes({0xD9, 0x5B, 32}); // fstp dword [ebx+32]
            break;
        }
        bl.bytes({0xDF, 0xE0}); // fnstsw ax (non-waiting: sees ES without faulting)
        switch (r.below(4)) {
          case 0:
            bl.b(0x9B); // fwait
            break;
          case 1:
            bl.bytes({0xD9, 0xE8}); // fld1 (waiting)
            break;
          case 2:
            // fnstenv (non-waiting) masks every exception, which clears ES/B: the fwait after it must not fault. The
            // image goes to DATA_BASE+0x200, mapped on both sides but outside the compared bytes (current CPUs store
            // FCS/FDS as zero, the emulator stores the selectors).
            bl.bytes({0xD9, 0xB3, 0x00, 0x02, 0x00, 0x00}); // fnstenv [ebx+0x200]
            bl.b(0x9B); // fwait
            break;
          default: {
            // fldcw is a waiting instruction too: a pending exception faults AT it, even when the new control word
            // masks that exception (the emulator once loaded the word and dropped the exception).
            uint16_t cw = r.coin() ? 0x037F : bl.t.in.fcw;
            bl.patch_bytes(48, &cw, 2);
            bl.bytes({0xD9, 0x6B, 48}); // fldcw [ebx+48]
            break;
          }
        }
        break;
      }
      default: { // fxtract + fscale round trip
        bl.t.category = "x87 fxtract/fscale";
        fld64(bl, 0);
        bl.bytes({0xD9, 0xF4}); // fxtract
        bl.bytes({0xD9, 0xFD}); // fscale
        bl.bytes({0xDF, 0xE0});
        break;
      }
    }
    out.push_back(bl.finish());
  }
}

// ---- Running --------------------------------------------------------------------------------------------------------

struct EmuRunner {
  std::shared_ptr<MemoryContext> mem = std::make_shared<MemoryContext>();
  X86Emulator emu{mem};
  bool faulted = false;
  X86Emulator::Fault fault;

  EmuRunner() {
    this->mem->allocate_at(CODE_BASE, 0x10000);
    this->mem->allocate_at(DATA_BASE, 0x1000);
    this->mem->allocate_at(STACK_BASE, 0x1000);
    this->emu.set_flat_mode(0);
    this->emu.set_fault_handler([this](X86Emulator& e, const X86Emulator::Fault& f) {
      this->faulted = true;
      this->fault = f;
      e.request_stop();
    });
  }

  CaseOut run(const CaseIn& in, std::string* error) {
    this->load(in);
    CaseOut out;
    memset(&out, 0, sizeof(out));
    auto reason = this->emu.run_until(CODE_BASE + in.code_len, 100000);
    if (this->faulted) {
      // Bit 8 carries Fault::divide_by_zero, which Windows turns into a different exception code.
      out.fault_code = 0xE0000000 | this->fault.vector | (this->fault.divide_by_zero ? 0x100 : 0);
      out.fault_eip = this->fault.eip;
    } else if (reason != X86Emulator::StopReason::ADDRESS) {
      *error = "emulator did not reach the end of the snippet";
    }
    this->collect(out);
    return out;
  }

  // Instruction boundaries of a straight-line snippet, by single-stepping it (for --explain).
  std::vector<uint32_t> boundaries(const CaseIn& in) {
    this->load(in);
    std::vector<uint32_t> ret;
    uint32_t last = 0;
    for (size_t z = 0; z < 64; z++) {
      this->emu.run(1);
      uint32_t off = this->emu.registers().eip - CODE_BASE;
      if (this->faulted || off <= last || off >= in.code_len) {
        break;
      }
      ret.push_back(off);
      last = off;
    }
    return ret;
  }

  void load(const CaseIn& in) {
    uint8_t buf[DATA_SIZE];
    this->mem->memset(CODE_BASE, 0xCC, 0x100);
    this->mem->memcpy(CODE_BASE, in.code, in.code_len);
    fill_region(buf, DATA_SIZE, in.seed);
    memcpy(buf, in.patch, in.patch_len);
    this->mem->memcpy(DATA_BASE, buf, DATA_SIZE);
    fill_region(buf, STACK_SIZE, in.seed ^ 0x9E3779B9);
    this->mem->memcpy(STACK_BASE, buf, STACK_SIZE);

    auto& regs = this->emu.registers();
    for (uint8_t z = 0; z < 8; z++) {
      regs.write32(z, in.regs[z]);
    }
    regs.write_eflags(in.eflags);
    regs.eip = CODE_BASE;
    this->emu.fpu().reset();
    this->emu.fpu().cw = in.fcw;
    this->faulted = false;
  }

  void collect(CaseOut& out) {
    auto& regs = this->emu.registers();
    for (uint8_t z = 0; z < 8; z++) {
      out.regs[z] = regs.read32(z);
    }
    out.eflags = regs.read_eflags();
    this->emu.fpu_save_image(out.fpu);
    this->mem->memcpy(out.data, DATA_BASE, DATA_SIZE);
    this->mem->memcpy(out.stack, STACK_BASE, STACK_SIZE);
  }
};

// Runs `inputs` through the native oracle; returns an error description, or "" with `results` filled.
std::string run_oracle(const std::string& oracle, const std::filesystem::path& scratch,
    const std::vector<CaseIn>& inputs, std::vector<CaseOut>& results) {
  auto in_path = scratch / std::format("x86diff_{}_cases.bin", GetCurrentProcessId());
  auto out_path = scratch / std::format("x86diff_{}_results.bin", GetCurrentProcessId());
  {
    FILE* f = _wfopen(in_path.c_str(), L"wb");
    if (!f) {
      return "cannot write " + in_path.string();
    }
    fwrite(inputs.data(), sizeof(CaseIn), inputs.size(), f);
    fclose(f);
  }
  // cmd.exe strips the outer quotes of a command line that starts with one; the extra pair keeps the rest intact.
  std::string cmd = std::format("\"\"{}\" \"{}\" \"{}\"\"", oracle, in_path.string(), out_path.string());
  int rc = system(cmd.c_str());
  results.assign(inputs.size(), CaseOut{});
  FILE* f = (rc == 0) ? _wfopen(out_path.c_str(), L"rb") : nullptr;
  size_t got = f ? fread(results.data(), sizeof(CaseOut), results.size(), f) : 0;
  if (f) {
    fclose(f);
  }
  std::error_code ec;
  std::filesystem::remove(in_path, ec);
  std::filesystem::remove(out_path, ec);
  if (rc != 0 || got != results.size()) {
    return std::format("exit code {}, {}/{} results", rc, got, results.size());
  }
  return "";
}

// Windows exception code -> x86 vector.
int vector_for_exception(uint32_t code) {
  switch (code) {
    case 0xC0000094: // INT_DIVIDE_BY_ZERO (#DE, zero divisor)
    case 0xC0000095: // INT_OVERFLOW (#DE, quotient too large)
      return 0;
    case 0xC000001D: // ILLEGAL_INSTRUCTION
      return 6;
    case 0xC000008C: // ARRAY_BOUNDS_EXCEEDED
      return 5;
    case 0xC000008D: // FLT_DENORMAL_OPERAND .. FLT_UNDERFLOW: #MF
    case 0xC000008E:
    case 0xC000008F:
    case 0xC0000090:
    case 0xC0000091:
    case 0xC0000092:
    case 0xC0000093:
      return 16;
    case 0xC0000096: // PRIV_INSTRUCTION
      return 13;
    case 0xC0000005: // ACCESS_VIOLATION (the flat emulator reports unmapped memory as #PF)
      return 14;
    default:
      return -1;
  }
}

bool fpu_regs_close(const uint8_t* a, const uint8_t* b, uint32_t ulps) {
  uint64_t ma, mb;
  uint16_t sa, sb;
  memcpy(&ma, a, 8);
  memcpy(&mb, b, 8);
  memcpy(&sa, a + 8, 2);
  memcpy(&sb, b + 8, 2);
  if (sa != sb) {
    return false;
  }
  uint64_t diff = (ma > mb) ? (ma - mb) : (mb - ma);
  return diff <= ulps;
}

std::string compare(const TestCase& t, const CaseOut& native, const CaseOut& emu) {
  std::string err;
  int nv = native.fault_code ? vector_for_exception(native.fault_code) : -2;
  int ev = emu.fault_code ? static_cast<int>(emu.fault_code & 0xFF) : -2;
  if (nv != ev) {
    return std::format("fault mismatch: native {:08X} (vector {}), emulator vector {}", native.fault_code, nv, ev);
  }
  if (nv == 0 && ((native.fault_code == 0xC0000094) != ((emu.fault_code & 0x100) != 0))) {
    return std::format("#DE kind mismatch: native {:08X}, emulator divide_by_zero={}", native.fault_code,
        (emu.fault_code & 0x100) != 0);
  }
  bool faulted = (nv != -2);
  if (faulted && native.fault_eip != emu.fault_eip) {
    err += std::format(" fault eip native {:08X} emu {:08X};", native.fault_eip, emu.fault_eip);
  }
  static const char* names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
  for (uint8_t z = 0; z < 8; z++) {
    if (native.regs[z] != emu.regs[z]) {
      err += std::format(" {} native {:08X} emu {:08X};", names[z], native.regs[z], emu.regs[z]);
    }
  }
  uint32_t fm = t.flags_mask;
  if ((native.eflags ^ emu.eflags) & fm) {
    err += std::format(" eflags native {:08X} emu {:08X} (mask {:03X}, diff {:03X});", native.eflags, emu.eflags, fm,
        (native.eflags ^ emu.eflags) & fm);
  }
  for (uint32_t z = 0; z < DATA_SIZE; z++) {
    if (native.data[z] != emu.data[z]) {
      if (t.store_ulps) {
        continue;
      }
      err += std::format(" data[{:02X}] native {:02X} emu {:02X};", z, native.data[z], emu.data[z]);
      break;
    }
  }
  // After a native fault Windows has pushed its exception frames below ESP; only compare the live stack.
  uint32_t stack_from = faulted ? std::min<uint32_t>(STACK_SIZE, native.regs[4] - STACK_BASE) : 0;
  for (uint32_t z = stack_from; z < STACK_SIZE; z++) {
    if (native.stack[z] != emu.stack[z]) {
      err += std::format(" stack[{:02X}] native {:02X} emu {:02X};", z, native.stack[z], emu.stack[z]);
      break;
    }
  }
  if (t.compare_fpu && !faulted) {
    uint16_t ncw, ecw, nsw, esw, ntw, etw;
    memcpy(&ncw, native.fpu, 2);
    memcpy(&ecw, emu.fpu, 2);
    memcpy(&nsw, native.fpu + 4, 2);
    memcpy(&esw, emu.fpu + 4, 2);
    memcpy(&ntw, native.fpu + 8, 2);
    memcpy(&etw, emu.fpu + 8, 2);
    if (ncw != ecw) {
      err += std::format(" fcw native {:04X} emu {:04X};", ncw, ecw);
    }
    if ((nsw ^ esw) & t.fpu_sw_mask) {
      err += std::format(" fsw native {:04X} emu {:04X};", nsw, esw);
    }
    if (ntw != etw) {
      err += std::format(" ftw native {:04X} emu {:04X};", ntw, etw);
    }
    uint8_t top = (nsw >> 11) & 7;
    for (uint8_t z = 0; z < 8; z++) {
      uint8_t phys = (top + z) & 7;
      if (((ntw >> (phys * 2)) & 3) == 3) {
        continue; // empty: the contents are stale leftovers on both sides
      }
      const uint8_t* a = native.fpu + 28 + z * 10;
      const uint8_t* b = emu.fpu + 28 + z * 10;
      if (memcmp(a, b, 10) != 0 && !(t.fpu_ulps && fpu_regs_close(a, b, t.fpu_ulps))) {
        uint64_t ma, mb;
        uint16_t sa, sb;
        memcpy(&ma, a, 8);
        memcpy(&mb, b, 8);
        memcpy(&sa, a + 8, 2);
        memcpy(&sb, b + 8, 2);
        err += std::format(" st{} native {:04X}:{:016X} emu {:04X}:{:016X};", z, sa, ma, sb, mb);
      }
    }
  }
  return err;
}

std::string hex_bytes(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t z = 0; z < n; z++) {
    s += std::format("{}{:02X}", z ? " " : "", p[z]);
  }
  return s;
}

// --explain: re-runs each failing straight-line snippet cut off after every instruction (the oracle appends its
// epilogue right after the prefix) on both sides, and prints the per-instruction states, which pinpoints the first
// instruction whose effect differs.
void explain(const std::string& oracle, const std::filesystem::path& scratch, const std::vector<TestCase>& cases,
    const std::vector<size_t>& failed_indexes) {
  EmuRunner runner;
  std::vector<CaseIn> prefixes;
  std::vector<std::pair<size_t, uint32_t>> owners; // (case index, prefix length)
  for (size_t index : failed_indexes) {
    const CaseIn& in = cases[index].in;
    for (uint32_t len : runner.boundaries(in)) {
      CaseIn p = in;
      p.code_len = len;
      memset(p.code + len, 0, CODE_MAX - len);
      prefixes.push_back(p);
      owners.emplace_back(index, len);
    }
  }
  std::vector<CaseOut> native;
  std::string err = run_oracle(oracle, scratch, prefixes, native);
  if (!err.empty()) {
    printf("explain: the oracle could not run (%s)\n", err.c_str());
    return;
  }
  size_t last_index = SIZE_MAX;
  for (size_t z = 0; z < prefixes.size(); z++) {
    auto [index, len] = owners[z];
    if (index != last_index) {
      printf("explain #%zu [%s]:\n", index, cases[index].category.c_str());
      last_index = index;
    }
    std::string e;
    CaseOut emu = runner.run(prefixes[z], &e);
    std::string d = compare(cases[index], native[z], emu);
    uint16_t nsw, esw;
    memcpy(&nsw, native[z].fpu + 4, 2);
    memcpy(&esw, emu.fpu + 4, 2);
    std::string dasm = X86Emulator::disassemble(prefixes[z].code, len, CODE_BASE, nullptr, false);
    size_t nl = dasm.rfind('\n', dasm.size() - 2);
    std::string last = (nl == std::string::npos) ? dasm : dasm.substr(nl + 1);
    if (!last.empty() && last.back() == '\n') {
      last.pop_back();
    }
    printf("  after %-44s fsw %04X/%04X fl %03X/%03X%s\n", last.c_str(), nsw, esw, native[z].eflags & 0xFFF,
        emu.eflags & 0xFFF, d.empty() ? "" : (" DIFF:" + d).c_str());
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
        "usage: test_x86_diff <x86_oracle.exe> [scratch dir] [--seed N] [--count-scale N] [--verbose] [--explain]\n");
    return 2;
  }
  std::string oracle = argv[1];
  std::filesystem::path scratch = std::filesystem::temp_directory_path();
  uint64_t seed = 0x5EED1996;
  size_t scale = 1;
  bool verbose = false;
  bool explain_failures = false;
  for (int z = 2; z < argc; z++) {
    std::string a = argv[z];
    if (a == "--seed" && z + 1 < argc) {
      seed = strtoull(argv[++z], nullptr, 0);
    } else if (a == "--count-scale" && z + 1 < argc) {
      scale = strtoull(argv[++z], nullptr, 0);
    } else if (a == "--verbose") {
      verbose = true;
    } else if (a == "--explain") {
      explain_failures = true;
    } else {
      scratch = a;
    }
  }
  if (!std::filesystem::exists(oracle)) {
    printf("SKIP: oracle %s not built\n", oracle.c_str());
    return 77;
  }

  Rng r{seed};
  std::vector<TestCase> cases;
  gen_alu(r, cases, 6000 * scale);
  gen_test_xchg_mov(r, cases, 2500 * scale);
  gen_unary(r, cases, 1500 * scale);
  gen_shifts(r, cases, 5000 * scale);
  gen_shld(r, cases, 1500 * scale);
  gen_muldiv(r, cases, 3000 * scale);
  gen_bitops(r, cases, 1500 * scale);
  gen_misc(r, cases, 5000 * scale);
  gen_branches(r, cases, 1500 * scale);
  gen_strings(r, cases, 2500 * scale);
  gen_x87(r, cases, 5000 * scale);

  std::filesystem::create_directories(scratch);
  std::vector<CaseIn> inputs;
  inputs.reserve(cases.size());
  for (const auto& t : cases) {
    inputs.push_back(t.in);
  }
  std::vector<CaseOut> native;
  std::string oracle_error = run_oracle(oracle, scratch, inputs, native);
  if (!oracle_error.empty()) {
    printf("SKIP: the native oracle could not run (%s)\n", oracle_error.c_str());
    return 77;
  }

  EmuRunner runner;
  size_t passed = 0, failed = 0;
  std::vector<size_t> failed_indexes;
  std::map<std::string, std::pair<size_t, size_t>> by_category;
  std::map<std::string, size_t> faults_by_category; // cases where the native CPU faulted (and the emulator had to match)
  for (size_t z = 0; z < cases.size(); z++) {
    const auto& t = cases[z];
    std::string error;
    CaseOut emu;
    try {
      emu = runner.run(t.in, &error);
    } catch (const std::exception& e) {
      error = std::string("emulator threw: ") + e.what();
    }
    if (error.empty()) {
      error = compare(t, native[z], emu);
    }
    auto& cat = by_category[t.category];
    if (native[z].fault_code) {
      faults_by_category[t.category]++;
    }
    if (error.empty()) {
      passed++;
      cat.first++;
    } else {
      failed++;
      cat.second++;
      failed_indexes.push_back(z);
      if (failed <= 40 || verbose) {
        std::string dasm = X86Emulator::disassemble(t.in.code, t.in.code_len, CODE_BASE, nullptr, false);
        for (auto& ch : dasm) {
          if (ch == '\n') {
            ch = ';';
          }
        }
        printf("FAIL #%zu [%s] code: %s\n    %s\n    in: eax=%08X ecx=%08X edx=%08X ebx=%08X ebp=%08X esi=%08X edi=%08X fl=%08X fcw=%04X\n   %s\n",
            z, t.category.c_str(), hex_bytes(t.in.code, t.in.code_len).c_str(), dasm.c_str(), t.in.regs[0],
            t.in.regs[1], t.in.regs[2], t.in.regs[3], t.in.regs[5], t.in.regs[6], t.in.regs[7], t.in.eflags, t.in.fcw,
            error.c_str());
      }
    }
  }
  for (const auto& [name, counts] : by_category) {
    printf("  %-32s %6zu passed %6zu failed  (%zu native faults)\n", name.c_str(), counts.first, counts.second,
        faults_by_category[name]);
  }
  if (explain_failures && !failed_indexes.empty()) {
    if (!verbose && failed_indexes.size() > 40) {
      failed_indexes.resize(40);
    }
    explain(oracle, scratch, cases, failed_indexes);
  }
  printf("x86 differential test: %zu of %zu snippets match the native CPU (%zu failed)\n", passed, cases.size(), failed);
  return failed ? 1 : 0;
}
