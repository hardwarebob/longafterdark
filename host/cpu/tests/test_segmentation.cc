// Segmentation and 16-bit protected-mode tests for adw::cpu, against a test LDT: Pascal far calls between 16-bit
// segments with retf n, the 16-bit stack wrapping at SP=0, lds/les, segment overrides on string ops, an int n
// handler that rewrites CS:IP, limit violations reported to the fault handler (and retried after the handler fixes
// the descriptor), LSL/LAR/VERR/VERW/ARPL, 0x66/0x67-prefixed 32-bit operations inside 16-bit code, far transfers
// into and out of a 32-bit segment, and the flat-mode FS segment.
//
// The 16-bit code is written as bytes with the assembly alongside (the vendored assembler only speaks 32-bit).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "X86Emulator.hh"

using namespace adw::cpu;
using SegReg = X86Emulator::SegReg;

namespace {

int failures = 0;
int checks = 0;

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

struct TestLDT : DescriptorProvider {
  std::map<uint16_t, SegDesc> descs; // keyed by selector with RPL stripped
  bool lookup(uint16_t sel, SegDesc& out) override {
    auto it = this->descs.find(sel & ~3);
    if (it == this->descs.end()) {
      return false;
    }
    out = it->second;
    return true;
  }
};

// LDT selectors (TI=1, RPL=3).
constexpr uint16_t sel(uint16_t index) {
  return (index << 3) | 4 | 3;
}
constexpr uint16_t CODE_A = sel(1);
constexpr uint16_t CODE_B = sel(2);
constexpr uint16_t DATA = sel(3);
constexpr uint16_t STACK = sel(4);
constexpr uint16_t SMALL = sel(5); // 256-byte data segment
constexpr uint16_t DATA2 = sel(6);
constexpr uint16_t CODE32 = sel(7); // a 32-bit code segment
constexpr uint16_t ABSENT = sel(8); // not present
constexpr uint16_t SMALLSTACK = sel(9); // 4 KiB stack segment
constexpr uint16_t XONLY = sel(10); // execute-only code

constexpr uint32_t BASE_A = 0x100000;
constexpr uint32_t BASE_B = 0x110000;
constexpr uint32_t BASE_DATA = 0x120000;
constexpr uint32_t BASE_STACK = 0x130000;
constexpr uint32_t BASE_SMALL = 0x140000;
constexpr uint32_t BASE_DATA2 = 0x148000;
constexpr uint32_t BASE_CODE32 = 0x150000;
constexpr uint32_t BASE_SMALLSTACK = 0x158000;

struct Machine {
  std::shared_ptr<MemoryContext> mem = std::make_shared<MemoryContext>();
  X86Emulator emu{mem};
  TestLDT ldt;
  std::vector<X86Emulator::Fault> faults;
  std::vector<uint8_t> ints;
  std::function<void(X86Emulator&, uint8_t)> on_int;
  std::function<bool(X86Emulator&, const X86Emulator::Fault&)> on_fault; // return true = resolved (retry)

  Machine() {
    this->mem->allocate_at(0x100000, 0x60000);
    auto code16 = [](uint32_t base, uint32_t limit) { return SegDesc{base, limit, true, true, true, false, 3}; };
    auto data16 = [](uint32_t base, uint32_t limit) { return SegDesc{base, limit, true, false, true, false, 3}; };
    this->ldt.descs[CODE_A & ~3] = code16(BASE_A, 0xFFFF);
    this->ldt.descs[CODE_B & ~3] = code16(BASE_B, 0x0FFF);
    this->ldt.descs[DATA & ~3] = data16(BASE_DATA, 0xFFFF);
    this->ldt.descs[STACK & ~3] = data16(BASE_STACK, 0xFFFF);
    this->ldt.descs[SMALL & ~3] = data16(BASE_SMALL, 0x00FF);
    this->ldt.descs[DATA2 & ~3] = data16(BASE_DATA2, 0x0FFF);
    this->ldt.descs[CODE32 & ~3] = SegDesc{BASE_CODE32, 0xFFFF, true, true, true, true, 3};
    this->ldt.descs[ABSENT & ~3] = SegDesc{BASE_DATA, 0xFFFF, false, false, true, false, 3};
    this->ldt.descs[SMALLSTACK & ~3] = data16(BASE_SMALLSTACK, 0x0FFF);
    this->ldt.descs[XONLY & ~3] = SegDesc{BASE_A, 0xFFFF, true, true, false, false, 3};
    this->emu.set_descriptor_provider(&this->ldt);
    this->emu.load_segment(SegReg::DS, DATA);
    this->emu.load_segment(SegReg::ES, DATA);
    this->emu.load_segment(SegReg::SS, STACK);
    this->emu.set_cs_eip(CODE_A, 0);
    this->emu.registers().w_esp(0xFFF0);
    this->emu.set_interrupt_handler([this](X86Emulator& e, uint8_t v) {
      this->ints.push_back(v);
      if (v == 0x20) {
        e.request_stop(); // "hlt" for these tests
      } else if (this->on_int) {
        this->on_int(e, v);
      }
    });
    this->emu.set_fault_handler([this](X86Emulator& e, const X86Emulator::Fault& f) {
      this->faults.push_back(f);
      if (!this->on_fault || !this->on_fault(e, f)) {
        e.request_stop();
      }
    });
  }

  void code(uint32_t base, uint32_t off, std::initializer_list<uint8_t> bytes) {
    std::string s(bytes.begin(), bytes.end());
    this->mem->write(base + off, s);
  }

  X86Emulator::StopReason run() {
    return this->emu.run(10000);
  }
  X86Emulator::Regs& regs() {
    return this->emu.registers();
  }
};

void test_pascal_far_call() {
  Machine m;
  m.regs().w_esp(0x1000);
  // A:0000  push 0x1234          68 34 12
  //         push 0x0101          68 01 01
  //         call far B:0010      9A 10 00 <B>
  //         int 0x20             CD 20
  m.code(BASE_A, 0, {0x68, 0x34, 0x12, 0x68, 0x01, 0x01, 0x9A, 0x10, 0x00, CODE_B & 0xFF, CODE_B >> 8, 0xCD, 0x20});
  // B:0010  push bp              55
  //         mov bp, sp           8B EC
  //         mov ax, [bp+6]       8B 46 06   (second argument: 0x0101; first pushed is deeper)
  //         sub ax, [bp+8]       2B 46 08   (0x0101 - 0x1234)
  //         mov dx, cs           8C CA
  //         pop bp               5D
  //         retf 4               CA 04 00
  m.code(BASE_B, 0x10, {0x55, 0x8B, 0xEC, 0x8B, 0x46, 0x06, 0x2B, 0x46, 0x08, 0x8C, 0xCA, 0x5D, 0xCA, 0x04, 0x00});
  auto reason = m.run();
  CHECK(reason == X86Emulator::StopReason::REQUESTED, "pascal: did not reach int 0x20");
  CHECK(m.faults.empty(), "pascal: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.regs().r_ax() == static_cast<uint16_t>(0x0101 - 0x1234), "pascal: ax=%04X", m.regs().r_ax());
  CHECK(m.regs().r_dx() == CODE_B, "pascal: callee saw cs=%04X", m.regs().r_dx());
  CHECK(m.regs().r_sp() == 0x1000, "pascal: sp=%04X after retf 4", m.regs().r_sp());
  CHECK(m.emu.get_segment(SegReg::CS) == CODE_A && m.regs().eip == 13, "pascal: returned to %04X:%04X",
      m.emu.get_segment(SegReg::CS), m.regs().eip);
  // The return address the far call pushed: CS then IP, 16 bits each.
  CHECK(m.mem->read_u16l(BASE_STACK + 0x1000 - 8) == 11 && m.mem->read_u16l(BASE_STACK + 0x1000 - 6) == CODE_A,
      "pascal: return frame %04X:%04X", m.mem->read_u16l(BASE_STACK + 0x1000 - 6), m.mem->read_u16l(BASE_STACK + 0x1000 - 8));
}

void test_stack_wrap() {
  Machine m;
  m.regs().w_esp(0xABCD0000); // SS.B = 0: only SP counts, and the high word must survive
  // mov ax, 0xBEEF; push ax; mov bx, sp; pop cx; int 0x20
  m.code(BASE_A, 0, {0xB8, 0xEF, 0xBE, 0x50, 0x89, 0xE3, 0x59, 0xCD, 0x20});
  m.run();
  CHECK(m.faults.empty(), "wrap: unexpected fault");
  CHECK(m.regs().r_bx() == 0xFFFE, "wrap: sp after push = %04X", m.regs().r_bx());
  CHECK(m.mem->read_u16l(BASE_STACK + 0xFFFE) == 0xBEEF, "wrap: pushed word not at ss:FFFE");
  CHECK(m.regs().r_cx() == 0xBEEF, "wrap: popped %04X", m.regs().r_cx());
  CHECK(m.regs().r_esp() == 0xABCD0000, "wrap: esp=%08X", m.regs().r_esp());
}

void test_lds_les() {
  Machine m;
  // ds:0010 holds a far pointer DATA2:0020; es:[bx] (bx=0x30) holds DATA2:0040.
  m.mem->write_u16l(BASE_DATA + 0x10, 0x0020);
  m.mem->write_u16l(BASE_DATA + 0x12, DATA2);
  m.mem->write_u16l(BASE_DATA + 0x30, 0x0040);
  m.mem->write_u16l(BASE_DATA + 0x32, DATA2);
  m.mem->write_u8(BASE_DATA2 + 0x20, 0x5A);
  m.mem->write_u8(BASE_DATA2 + 0x40, 0xA5);
  // mov bx, 0x30; les di, [bx]; lds si, [0x10]; mov al, [si]; mov ah, es:[di]; int 0x20
  m.code(BASE_A, 0, {0xBB, 0x30, 0x00, 0xC4, 0x3F, 0xC5, 0x36, 0x10, 0x00, 0x8A, 0x04, 0x26, 0x8A, 0x25, 0xCD, 0x20});
  m.run();
  CHECK(m.faults.empty(), "lds/les: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.emu.get_segment(SegReg::DS) == DATA2 && m.emu.get_segment(SegReg::ES) == DATA2, "lds/les: ds=%04X es=%04X",
      m.emu.get_segment(SegReg::DS), m.emu.get_segment(SegReg::ES));
  CHECK(m.regs().r_si() == 0x20 && m.regs().r_di() == 0x40, "lds/les: si=%04X di=%04X", m.regs().r_si(), m.regs().r_di());
  CHECK(m.regs().r_ax() == 0xA55A, "lds/les: ax=%04X", m.regs().r_ax());
}

void test_string_overrides() {
  Machine m;
  m.mem->write(BASE_STACK + 0x200, std::string("\x11\x22\x33\x44", 4));
  // Copy 6 bytes from CS:0 (this code) to ES:0x100 with a CS override, then lodsw through SS.
  // xor si, si; mov di, 0x100; mov cx, 6; cs rep movsb; mov si, 0x200; ss lodsw; int 0x20
  m.code(BASE_A, 0, {0x31, 0xF6, 0xBF, 0x00, 0x01, 0xB9, 0x06, 0x00, 0x2E, 0xF3, 0xA4, 0xBE, 0x00, 0x02, 0x36, 0xAD,
                        0xCD, 0x20});
  m.run();
  CHECK(m.faults.empty(), "string: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  std::string copied = m.mem->read(BASE_DATA + 0x100, 6);
  CHECK(copied == std::string("\x31\xF6\xBF\x00\x01\xB9", 6), "string: cs: rep movsb copied the wrong bytes");
  CHECK(m.regs().r_si() == 0x202 && m.regs().r_di() == 0x106 && m.regs().r_cx() == 0, "string: si=%04X di=%04X cx=%04X",
      m.regs().r_si(), m.regs().r_di(), m.regs().r_cx());
  CHECK(m.regs().r_ax() == 0x2211, "string: ss: lodsw read %04X", m.regs().r_ax());
}

void test_int_rewrites_cs_ip() {
  Machine m;
  // A:0000 mov ax, 1; int 0x21; (not reached) int 0x20
  m.code(BASE_A, 0, {0xB8, 0x01, 0x00, 0xCD, 0x21, 0xCD, 0x20});
  // B:0100 mov bx, ax; mov ax, 0xBEEF; int 0x20
  m.code(BASE_B, 0x100, {0x89, 0xC3, 0xB8, 0xEF, 0xBE, 0xCD, 0x20});
  uint32_t seen_ip = 0;
  m.on_int = [&](X86Emulator& e, uint8_t v) {
    if (v == 0x21) {
      seen_ip = e.registers().eip; // the handler sees EIP past the int instruction
      e.set_cs_eip(CODE_B, 0x100);
    }
  };
  m.run();
  CHECK(m.faults.empty(), "int: unexpected fault");
  CHECK(seen_ip == 5, "int: handler saw ip=%04X", seen_ip);
  CHECK(m.emu.get_segment(SegReg::CS) == CODE_B, "int: cs=%04X", m.emu.get_segment(SegReg::CS));
  CHECK(m.regs().r_bx() == 1 && m.regs().r_ax() == 0xBEEF, "int: bx=%04X ax=%04X", m.regs().r_bx(), m.regs().r_ax());
}

void test_limit_fault_and_retry() {
  Machine m;
  // mov ax, SMALL; mov es, ax; mov bx, 0x7777; mov al, es:[0x100]; int 0x20
  m.code(BASE_A, 0, {0xB8, SMALL & 0xFF, SMALL >> 8, 0x8E, 0xC0, 0xBB, 0x77, 0x77, 0x26, 0xA0, 0x00, 0x01, 0xCD, 0x20});
  m.mem->write_u8(BASE_SMALL + 0x100, 0x99);
  // The handler grows the segment and retries the instruction, as a host would after (say) growing a heap.
  m.on_fault = [&](X86Emulator& e, const X86Emulator::Fault& f) {
    if (f.vector == X86Emulator::VEC_GP && m.faults.size() == 1) {
      m.ldt.descs[SMALL & ~3].limit = 0x1FF;
      e.load_segment(SegReg::ES, SMALL);
      return true;
    }
    return false;
  };
  m.run();
  CHECK(m.faults.size() == 1, "limit: %zu faults", m.faults.size());
  if (!m.faults.empty()) {
    const auto& f = m.faults[0];
    CHECK(f.vector == X86Emulator::VEC_GP && f.cs == CODE_A && f.eip == 8, "limit: fault %s", f.str().c_str());
  }
  CHECK(m.regs().r_al() == 0x99 && m.regs().r_bx() == 0x7777, "limit: retry read al=%02X", m.regs().r_al());

  // A stack push below the limit of a 4 KiB stack segment is #SS; the fault leaves SP untouched.
  Machine s;
  s.emu.load_segment(SegReg::SS, SMALLSTACK);
  s.regs().w_esp(0);
  s.code(BASE_A, 0, {0x50, 0xCD, 0x20}); // push ax
  s.run();
  CHECK(s.faults.size() == 1 && s.faults[0].vector == X86Emulator::VEC_SS && s.faults[0].eip == 0,
      "stack limit: %s", s.faults.empty() ? "no fault" : s.faults[0].str().c_str());
  CHECK(s.regs().r_sp() == 0, "stack limit: sp=%04X after the fault", s.regs().r_sp());

  // Loading a not-present segment is #NP with the selector as error code; a null ES loads fine but faults on use.
  Machine n;
  n.code(BASE_A, 0, {0xB8, ABSENT & 0xFF, ABSENT >> 8, 0x8E, 0xD8, 0xCD, 0x20}); // mov ax, ABSENT; mov ds, ax
  n.run();
  CHECK(n.faults.size() == 1 && n.faults[0].vector == X86Emulator::VEC_NP && n.faults[0].error_code == (ABSENT & ~3u),
      "not present: %s", n.faults.empty() ? "no fault" : n.faults[0].str().c_str());
  Machine z;
  // xor ax, ax; mov es, ax; mov al, es:[0]
  z.code(BASE_A, 0, {0x31, 0xC0, 0x8E, 0xC0, 0x26, 0xA0, 0x00, 0x00, 0xCD, 0x20});
  z.run();
  CHECK(z.faults.size() == 1 && z.faults[0].vector == X86Emulator::VEC_GP && z.faults[0].eip == 4,
      "null es: %s", z.faults.empty() ? "no fault" : z.faults[0].str().c_str());
  // Writing through a code segment (CS override) is #GP.
  Machine w;
  w.code(BASE_A, 0, {0x2E, 0xA2, 0x00, 0x02, 0xCD, 0x20}); // mov cs:[0x200], al
  w.run();
  CHECK(w.faults.size() == 1 && w.faults[0].vector == X86Emulator::VEC_GP, "code write: %s",
      w.faults.empty() ? "no fault" : w.faults[0].str().c_str());
  // No fault handler: faults propagate as fault_error.
  Machine t;
  t.emu.set_fault_handler(nullptr);
  t.code(BASE_A, 0, {0x0F, 0x0B}); // ud2
  bool threw = false;
  try {
    t.run();
  } catch (const X86Emulator::fault_error& e) {
    threw = (e.fault.vector == X86Emulator::VEC_UD);
  }
  CHECK(threw, "ud2 without a handler did not throw fault_error");
  // A fault the fault handler itself causes (here: loading a not-present segment while "fixing" a #UD) leaves run()
  // as fault_error too, not as an internal exception type.
  Machine u;
  u.code(BASE_A, 0, {0x0F, 0x0B});
  u.on_fault = [&](X86Emulator& e, const X86Emulator::Fault&) {
    e.load_segment(SegReg::DS, ABSENT);
    return true;
  };
  threw = false;
  try {
    u.run();
  } catch (const X86Emulator::fault_error& e) {
    threw = (e.fault.vector == X86Emulator::VEC_NP);
  }
  CHECK(threw, "a fault inside the fault handler did not leave run() as fault_error");
}

void test_reload_segments() {
  // A host moving a segment (GlobalReAlloc) edits the LDT; the loaded DS keeps its cached base until the host calls
  // reload_segments(), as on the real CPU.
  Machine m;
  m.mem->write_u8(BASE_DATA + 0x10, 0x11);
  m.mem->write_u8(BASE_DATA2 + 0x10, 0x22);
  // mov al, [0x10]; int 0x21; mov ah, [0x10]; int 0x22; mov bl, [0x10]; int 0x20
  m.code(BASE_A, 0, {0xA0, 0x10, 0x00, 0xCD, 0x21, 0x8A, 0x26, 0x10, 0x00, 0xCD, 0x22, 0x8A, 0x1E, 0x10, 0x00, 0xCD,
                        0x20});
  m.on_int = [&](X86Emulator& e, uint8_t v) {
    if (v == 0x21) {
      m.ldt.descs[DATA & ~3].base = BASE_DATA2; // not reloaded yet: still reads the old base
    } else if (v == 0x22) {
      e.reload_segments();
    }
  };
  m.run();
  CHECK(m.faults.empty(), "reload: unexpected fault");
  CHECK(m.regs().r_ax() == 0x1111 && m.regs().r_bl() == 0x22, "reload: ax=%04X bl=%02X", m.regs().r_ax(),
      m.regs().r_bl());
}

void test_lsl_lar_verr() {
  Machine m;
  // mov bx, DATA; lsl ax, bx; lar cx, bx; mov si, 0x1234; lsl dx, si (invalid: ZF=0, dx kept)
  // lahf -> save ZF of the invalid lsl into ah... do it step by step with setz into memory:
  //   lsl ax, bx      0F 03 C3
  //   setz [0x00]     0F 94 06 00 00
  //   lar cx, bx      0F 02 CB
  //   mov dx, 0x5555  BA 55 55
  //   mov si, 0x1234  BE 34 12
  //   lsl dx, si      0F 03 D6
  //   setz [0x01]     0F 94 06 01 00
  //   66 lar edi, bx  66 0F 02 FB
  //   mov bp, CODE_B  BD <B>
  //   verr bp         0F 00 E5
  //   setz [0x02]     0F 94 06 02 00
  //   verw bp         0F 00 ED
  //   setz [0x03]     0F 94 06 03 00
  //   mov bp, XONLY   BD <X>
  //   verr bp         0F 00 E5
  //   setz [0x04]     0F 94 06 04 00
  //   verw bx         0F 00 EB
  //   setz [0x05]     0F 94 06 05 00
  //   mov bx, CODE_B  BB <B>
  //   66 lsl ebx, bx  66 0F 03 DB
  //   int 0x20
  m.code(BASE_A, 0, {0xBB, DATA & 0xFF, DATA >> 8, 0x0F, 0x03, 0xC3, 0x0F, 0x94, 0x06, 0x00, 0x00, 0x0F, 0x02, 0xCB,
                        0xBA, 0x55, 0x55, 0xBE, 0x34, 0x12, 0x0F, 0x03, 0xD6, 0x0F, 0x94, 0x06, 0x01, 0x00, 0x66, 0x0F,
                        0x02, 0xFB, 0xBD, CODE_B & 0xFF, CODE_B >> 8, 0x0F, 0x00, 0xE5, 0x0F, 0x94, 0x06, 0x02, 0x00,
                        0x0F, 0x00, 0xED, 0x0F, 0x94, 0x06, 0x03, 0x00, 0xBD, XONLY & 0xFF, XONLY >> 8, 0x0F, 0x00,
                        0xE5, 0x0F, 0x94, 0x06, 0x04, 0x00, 0x0F, 0x00, 0xEB, 0x0F, 0x94, 0x06, 0x05, 0x00, 0xBB,
                        CODE_B & 0xFF, CODE_B >> 8, 0x66, 0x0F, 0x03, 0xDB, 0xCD, 0x20});
  m.run();
  CHECK(m.faults.empty(), "lsl/lar: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.regs().r_ax() == 0xFFFF, "lsl: ax=%04X", m.regs().r_ax());
  CHECK(m.mem->read_u8(BASE_DATA + 0) == 1, "lsl: ZF not set for a valid selector");
  // Present, DPL 3, code/data, writable data, accessed: 0xF3.
  CHECK(m.regs().r_cx() == 0xF300, "lar: cx=%04X", m.regs().r_cx());
  CHECK(m.regs().r_dx() == 0x5555 && m.mem->read_u8(BASE_DATA + 1) == 0, "lsl invalid: dx=%04X zf=%d",
      m.regs().r_dx(), m.mem->read_u8(BASE_DATA + 1));
  CHECK(m.regs().r_edi() == 0x0000F300, "lar r32: edi=%08X", m.regs().r_edi());
  CHECK(m.mem->read_u8(BASE_DATA + 2) == 1 && m.mem->read_u8(BASE_DATA + 3) == 0, "verr/verw on readable code");
  CHECK(m.mem->read_u8(BASE_DATA + 4) == 0, "verr on execute-only code");
  CHECK(m.mem->read_u8(BASE_DATA + 5) == 1, "verw on writable data");
  CHECK(m.regs().r_ebx() == 0x0FFF, "lsl r32: ebx=%08X", m.regs().r_ebx());
}

void test_arpl_sreg_ops() {
  Machine m;
  // mov ax, 0x0003; mov bx, 0x1230; arpl bx, ax (ZF=1, bx=0x1233); setz [0]; arpl bx, ax (ZF=0); setz [1]
  // push ds; pop es; push cs; pop ax(->si); mov cx, es; int 0x20
  m.code(BASE_A, 0, {0xB8, 0x03, 0x00, 0xBB, 0x30, 0x12, 0x63, 0xC3, 0x0F, 0x94, 0x06, 0x00, 0x00, 0x63, 0xC3, 0x0F,
                        0x94, 0x06, 0x01, 0x00, 0x1E, 0x07, 0x0E, 0x5E, 0x8C, 0xC1, 0xCD, 0x20});
  m.emu.load_segment(SegReg::ES, DATA2);
  m.run();
  CHECK(m.faults.empty(), "arpl: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.regs().r_bx() == 0x1233, "arpl: bx=%04X", m.regs().r_bx());
  CHECK(m.mem->read_u8(BASE_DATA + 0) == 1 && m.mem->read_u8(BASE_DATA + 1) == 0, "arpl: ZF sequence");
  CHECK(m.emu.get_segment(SegReg::ES) == DATA && m.regs().r_cx() == DATA, "push ds/pop es: es=%04X",
      m.emu.get_segment(SegReg::ES));
  CHECK(m.regs().r_si() == CODE_A, "push cs: %04X", m.regs().r_si());
}

void test_mixed_32bit_in_16bit() {
  Machine m;
  m.mem->write_u32l(BASE_DATA + 0x40 + 3 * 4, 0xDEADBEEF);
  // 66 B8 78 56 34 12     mov eax, 0x12345678
  // 66 05 11 11 11 11     add eax, 0x11111111
  // 66 50                 push eax           (4 bytes on the 16-bit stack)
  // 66 5B                 pop ebx
  // 66 BE 40 00 00 00     mov esi, 0x40
  // 66 B9 03 00 00 00     mov ecx, 3
  // 66 67 8B 3C 8E        mov edi, [esi+ecx*4]   (0x67: 32-bit addressing in 16-bit code)
  // 66 C1 E0 04           shl eax, 4
  // 66 0F AF C3           imul eax, ebx
  // 89 E2                 mov dx, sp
  // CD 20
  m.code(BASE_A, 0, {0x66, 0xB8, 0x78, 0x56, 0x34, 0x12, 0x66, 0x05, 0x11, 0x11, 0x11, 0x11, 0x66, 0x50, 0x66, 0x5B,
                        0x66, 0xBE, 0x40, 0x00, 0x00, 0x00, 0x66, 0xB9, 0x03, 0x00, 0x00, 0x00, 0x66, 0x67, 0x8B, 0x3C,
                        0x8E, 0x66, 0xC1, 0xE0, 0x04, 0x66, 0x0F, 0xAF, 0xC3, 0x89, 0xE2, 0xCD, 0x20});
  m.regs().w_esp(0x2000);
  m.run();
  CHECK(m.faults.empty(), "mixed: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.regs().r_ebx() == 0x23456789, "mixed: ebx=%08X", m.regs().r_ebx());
  CHECK(m.regs().r_edi() == 0xDEADBEEF, "mixed: edi=%08X", m.regs().r_edi());
  CHECK(m.regs().r_eax() == static_cast<uint32_t>(0x34567890u * 0x23456789u), "mixed: eax=%08X", m.regs().r_eax());
  CHECK(m.regs().r_dx() == 0x2000, "mixed: sp=%04X", m.regs().r_dx());
  CHECK(m.mem->read_u32l(BASE_STACK + 0x1FFC) == 0x23456789, "mixed: 32-bit push landed wrong");
}

void test_far_32bit_transfers() {
  Machine m;
  m.regs().w_esp(0x3000);
  // A:0000 (16-bit)  66 9A 00 01 00 00 <CODE32>   call far 32:CODE32:00000100 (32-bit operand: 32-bit frame)
  //                  CD 20
  m.code(BASE_A, 0, {0x66, 0x9A, 0x00, 0x01, 0x00, 0x00, CODE32 & 0xFF, CODE32 >> 8, 0xCD, 0x20});
  // CODE32:0100 (32-bit code) mov eax, [esp]; mov ebx, [esp+4]; mov ecx, 0x11223344; retf
  //   8B 04 24; 8B 5C 24 04; B9 44 33 22 11; CB
  // (SS is still the 16-bit stack: SS.B=0 means pushes use SP even from 32-bit code, but [esp] addressing is 32-bit)
  m.code(BASE_CODE32, 0x100, {0x8B, 0x04, 0x24, 0x8B, 0x5C, 0x24, 0x04, 0xB9, 0x44, 0x33, 0x22, 0x11, 0xCB});
  m.run();
  CHECK(m.faults.empty(), "far32: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.regs().r_eax() == 8, "far32: pushed eip=%08X", m.regs().r_eax());
  CHECK((m.regs().r_ebx() & 0xFFFF) == CODE_A, "far32: pushed cs=%08X", m.regs().r_ebx());
  CHECK(m.regs().r_ecx() == 0x11223344, "far32: callee did not run");
  CHECK(m.emu.get_segment(SegReg::CS) == CODE_A && m.regs().r_sp() == 0x3000 && m.emu.is_code16(),
      "far32: returned to %04X:%04X sp=%04X", m.emu.get_segment(SegReg::CS), m.regs().eip, m.regs().r_sp());

  // jmp far, iret, pushf, 16-bit pusha/popa, enter/leave, loop/jcxz on CX.
  Machine n;
  n.regs().w_esp(0x800);
  // A:0000  EA 00 02 <B>          jmp far B:0200
  n.code(BASE_A, 0, {0xEA, 0x00, 0x02, CODE_B & 0xFF, CODE_B >> 8});
  // B:0200  9C                    pushf
  //         0E                    push cs
  //         68 30 02              push 0x0230
  //         CF                    iret          -> B:0230, flags restored
  // B:0230  B9 03 00 66 C1 E1 10 ... : mov cx, 3; shl ecx, 16 (ECX high garbage, CX = 0); mov cx, 3
  //         31 C0                 xor ax, ax
  //   loop: 40                    inc ax
  //         E2 FD                 loop (CX)
  //         E3 02                 jcxz +2
  //         B0 77                 mov al, 0x77 (skipped)
  //         60 61                 pusha; popa
  //         C8 04 00 00           enter 4, 0
  //         89 EB                 mov bx, bp
  //         C9                    leave
  //         CD 20
  n.code(BASE_B, 0x200, {0x9C, 0x0E, 0x68, 0x30, 0x02, 0xCF});
  n.code(BASE_B, 0x230, {0x66, 0xB9, 0x00, 0x00, 0x05, 0x00, 0xB9, 0x03, 0x00, 0x31, 0xC0, 0x40, 0xE2, 0xFD, 0xE3, 0x02,
                            0xB0, 0x77, 0x60, 0x61, 0xC8, 0x04, 0x00, 0x00, 0x89, 0xEB, 0xC9, 0xCD, 0x20});
  n.run();
  CHECK(n.faults.empty(), "jmp/iret: unexpected fault %s", n.faults.empty() ? "" : n.faults[0].str().c_str());
  CHECK(n.emu.get_segment(SegReg::CS) == CODE_B, "jmp far: cs=%04X", n.emu.get_segment(SegReg::CS));
  CHECK(n.regs().r_ax() == 3, "loop on cx: ax=%04X", n.regs().r_ax());
  CHECK(n.regs().r_ecx() == 0x00050000, "loop on cx must leave the high word: ecx=%08X", n.regs().r_ecx());
  CHECK(n.regs().r_bx() == 0x7FE, "enter 4,0: bp=%04X", n.regs().r_bx());
  CHECK(n.regs().r_sp() == 0x800, "iret/pusha/popa/enter/leave: sp=%04X", n.regs().r_sp());
}

void test_16bit_misc() {
  Machine m;
  m.regs().w_esp(0x1000);
  // Far pointers in DS for the indirect far transfers: [0x40] = B:0300 (call target), [0x44] = A:0080 (jmp target).
  m.mem->write_u16l(BASE_DATA + 0x40, 0x0300);
  m.mem->write_u16l(BASE_DATA + 0x42, CODE_B);
  m.mem->write_u16l(BASE_DATA + 0x44, 0x0080);
  m.mem->write_u16l(BASE_DATA + 0x46, CODE_A);
  // A 16:32 far pointer for 66 les: DATA2:00012345 at [0x50] (offset dword, then selector).
  m.mem->write_u32l(BASE_DATA + 0x50, 0x00012345);
  m.mem->write_u16l(BASE_DATA + 0x54, DATA2);
  // xlat table at DS:0x100
  for (int z = 0; z < 16; z++) {
    m.mem->write_u8(BASE_DATA + 0x100 + z, 0xA0 + z);
  }
  // A:0000  BB 40 00        mov bx, 0x40
  //         FF 1F           call far [bx]            -> B:0300 (returns with retf)
  //         FF 6F 04        jmp far [bx+4]           -> A:0080
  m.code(BASE_A, 0, {0xBB, 0x40, 0x00, 0xFF, 0x1F, 0xFF, 0x6F, 0x04});
  // B:0300  B9 11 11        mov cx, 0x1111
  //         CB              retf
  m.code(BASE_B, 0x300, {0xB9, 0x11, 0x11, 0xCB});
  // A:0080  66 C4 7F 10     les edi, [bx+0x10]       (0x66: m16:32 in 16-bit code)
  //         BB 00 01        mov bx, 0x100
  //         B0 07           mov al, 7
  //         D7              xlat                     (al = ds:[bx+al])
  //         8D 70 FE        lea si, [bx+si-2]        (si = 0x100 + 0 - 2)
  //         54              push sp                  (the value before the push)
  //         5A              pop dx
  //         C8 06 00 02     enter 6, 2
  //         89 E1           mov cx, sp
  //         C9              leave
  //         CD 20           int 0x20
  m.code(BASE_A, 0x80, {0x66, 0xC4, 0x7F, 0x10, 0xBB, 0x00, 0x01, 0xB0, 0x07, 0xD7, 0x8D, 0x70, 0xFE, 0x54, 0x5A, 0xC8,
                           0x06, 0x00, 0x02, 0x89, 0xE1, 0xC9, 0xCD, 0x20});
  m.regs().w_si(0);
  m.regs().w_bp(0x1100);
  m.mem->write_u16l(BASE_STACK + 0x1100 - 2, 0xBEEF); // the enclosing frame's display entry enter level 2 copies
  m.run();
  CHECK(m.faults.empty(), "16-bit misc: unexpected fault %s", m.faults.empty() ? "" : m.faults[0].str().c_str());
  CHECK(m.emu.get_segment(SegReg::ES) == DATA2 && m.regs().r_edi() == 0x00012345, "66 les: es=%04X edi=%08X",
      m.emu.get_segment(SegReg::ES), m.regs().r_edi());
  CHECK(m.regs().r_al() == 0xA7, "xlat: al=%02X", m.regs().r_al());
  CHECK(m.regs().r_si() == 0x00FE, "lea 16: si=%04X", m.regs().r_si());
  CHECK(m.regs().r_dx() == 0x1000, "push sp pushed %04X", m.regs().r_dx());
  // enter 6, 2 at SP=0x1000: push bp (0x0FFE), copy one display entry (0x0FFC), push the frame pointer (0x0FFA), then
  // reserve 6 bytes: SP = 0x0FF4.
  CHECK(m.regs().r_cx() == 0x0FF4, "enter 6,2: sp=%04X", m.regs().r_cx());
  CHECK(m.mem->read_u16l(BASE_STACK + 0x0FFC) == 0xBEEF && m.mem->read_u16l(BASE_STACK + 0x0FFA) == 0x0FFE,
      "enter 6,2: display %04X frame %04X", m.mem->read_u16l(BASE_STACK + 0x0FFC),
      m.mem->read_u16l(BASE_STACK + 0x0FFA));
  CHECK(m.regs().r_sp() == 0x1000 && m.regs().r_bp() == 0x1100, "leave: sp=%04X bp=%04X", m.regs().r_sp(),
      m.regs().r_bp());
  CHECK(m.emu.get_segment(SegReg::CS) == CODE_A, "far jmp [mem]: cs=%04X", m.emu.get_segment(SegReg::CS));

  // A word access that straddles the end of a 64 KiB segment is #GP (DI=FFFF), and rep stosw backwards (DF=1)
  // stops there with CX and DI showing the progress made, restartable at the faulting element.
  Machine s;
  // std; mov di, 0x0003; mov cx, 5; mov ax, 0x5A5A; rep stosw; int 0x20
  s.code(BASE_A, 0, {0xFD, 0xBF, 0x03, 0x00, 0xB9, 0x05, 0x00, 0xB8, 0x5A, 0x5A, 0xF3, 0xAB, 0xCD, 0x20});
  s.run();
  // Elements at DI=3 and DI=1 are written; DI then wraps to 0xFFFF, where the word straddles the limit.
  CHECK(s.faults.size() == 1 && s.faults[0].vector == X86Emulator::VEC_GP && s.faults[0].eip == 10,
      "stosw wrap: %s", s.faults.empty() ? "no fault" : s.faults[0].str().c_str());
  CHECK(s.regs().r_cx() == 3 && s.regs().r_di() == 0xFFFF, "stosw wrap: cx=%04X di=%04X", s.regs().r_cx(),
      s.regs().r_di());
  CHECK(s.mem->read_u16l(BASE_DATA + 3) == 0x5A5A && s.mem->read_u16l(BASE_DATA + 1) == 0x5A5A, "stosw wrap: data");
}

void test_flat_mode() {
  auto mem = std::make_shared<MemoryContext>();
  mem->allocate_at(0x00400000, 0x10000);
  mem->allocate_at(0x7FFDE000, 0x1000);
  X86Emulator emu(mem);
  emu.set_flat_mode(0x7FFDE000);
  mem->write_u32l(0x7FFDE018, 0x7FFDE000); // TEB self pointer
  // mov eax, fs:[0x18]; push ds; pop es; mov bx, fs; jmp far 0x1B:0x00400020 ... ; mov ecx, 1
  // 64 A1 18 00 00 00; 1E; 07; 66 8C E3; EA 20 00 40 00 1B 00
  std::string code("\x64\xA1\x18\x00\x00\x00\x1E\x07\x66\x8C\xE3\xEA\x20\x00\x40\x00\x1B\x00", 18);
  mem->write(0x00400000, code);
  mem->write(0x00400020, std::string("\xB9\x01\x00\x00\x00\x64\x8B\x15\x00\x10\x00\x00", 12)); // mov ecx, 1; mov edx, fs:[0x1000]
  X86Emulator::Fault fault;
  bool faulted = false;
  emu.set_fault_handler([&](X86Emulator& e, const X86Emulator::Fault& f) {
    fault = f;
    faulted = true;
    e.request_stop();
  });
  emu.registers().eip = 0x00400000;
  emu.registers().w_esp(0x0040F000);
  emu.run(100);
  CHECK(emu.registers().r_eax() == 0x7FFDE000, "flat: fs:[0x18]=%08X", emu.registers().r_eax());
  CHECK(emu.get_segment(SegReg::ES) == 0x23, "flat: es=%04X after push ds/pop es", emu.get_segment(SegReg::ES));
  CHECK(emu.registers().r_bx() == 0x3B, "flat: fs selector %04X", emu.registers().r_bx());
  CHECK(emu.registers().r_ecx() == 1, "flat: far jmp to the flat CS did not land");
  // fs:[0x1000] is past the TEB segment's 4 KiB limit.
  CHECK(faulted && fault.vector == X86Emulator::VEC_GP && fault.eip == 0x00400025, "flat: TEB limit fault %s",
      faulted ? fault.str().c_str() : "missing");

  // Unmapped emulated memory is #PF with the linear address; a push onto an unmapped stack faults with ESP intact.
  mem->write(0x00400040, std::string("\xA1\x78\x56\x34\x12", 5)); // mov eax, [0x12345678]
  faulted = false;
  emu.registers().eip = 0x00400040;
  emu.run(10);
  CHECK(faulted && fault.vector == X86Emulator::VEC_PF && fault.address == 0x12345678 && fault.eip == 0x00400040,
      "flat: unmapped read %s", faulted ? fault.str().c_str() : "missing");
  mem->write(0x00400050, std::string("\x50", 1)); // push eax
  faulted = false;
  emu.registers().eip = 0x00400050;
  emu.registers().w_esp(0x00800000);
  emu.run(10);
  CHECK(faulted && fault.vector == X86Emulator::VEC_PF && fault.address == 0x007FFFFC &&
          emu.registers().r_esp() == 0x00800000,
      "flat: unmapped push %s esp=%08X", faulted ? fault.str().c_str() : "missing", emu.registers().r_esp());
}

void test_nested_run_until() {
  // A host callback into emulated code (as a window procedure call would be): the int handler runs a nested
  // run_until to a sentinel, then the outer run continues.
  auto mem = std::make_shared<MemoryContext>();
  mem->allocate_at(0x00400000, 0x10000);
  X86Emulator emu(mem);
  emu.set_flat_mode(0);
  // main: mov eax, 1; int 0x80; add eax, 100; int 0x81
  mem->write(0x00400000, std::string("\xB8\x01\x00\x00\x00\xCD\x80\x83\xC0\x64\xCD\x81", 12));
  // callback at 0x400100: add eax, 10; ret
  mem->write(0x00400100, std::string("\x83\xC0\x0A\xC3", 4));
  constexpr uint32_t SENTINEL = 0x0040F800;
  int depth_seen = 0;
  emu.set_interrupt_handler([&](X86Emulator& e, uint8_t v) {
    if (v == 0x80) {
      auto& r = e.registers();
      uint32_t saved_eip = r.eip;
      e.push<uint32_t>(SENTINEL);
      r.eip = 0x00400100;
      auto reason = e.run_until(SENTINEL, 1000);
      depth_seen = (reason == X86Emulator::StopReason::ADDRESS) ? 1 : -1;
      r.eip = saved_eip;
    } else if (v == 0x81) {
      e.request_stop();
    }
  });
  emu.registers().eip = 0x00400000;
  emu.registers().w_esp(0x0040F000);
  auto reason = emu.run(1000);
  CHECK(reason == X86Emulator::StopReason::REQUESTED, "nested: outer run did not finish");
  CHECK(depth_seen == 1, "nested: inner run_until did not stop at the sentinel");
  CHECK(emu.registers().r_eax() == 111, "nested: eax=%u", emu.registers().r_eax());
  CHECK(emu.registers().r_esp() == 0x0040F000, "nested: esp=%08X", emu.registers().r_esp());
}

void test_host_api_faults() {
  // Host-facing stack/memory helpers used outside run() report failures as fault_error (a std::runtime_error), never
  // as the CPU's internal exception type, which host code could not catch as std::exception.
  auto expect_fault = [](const char* what, uint8_t vector, auto&& fn) {
    bool ok = false;
    std::string got = "no exception";
    try {
      fn();
    } catch (const X86Emulator::fault_error& e) {
      ok = (e.fault.vector == vector);
      got = e.fault.str();
    } catch (const std::exception& e) {
      got = std::string("std::exception: ") + e.what();
    } catch (...) {
      got = "a non-std exception";
    }
    CHECK(ok, "%s: expected vector %u, got %s", what, vector, got.c_str());
  };
  {
    Machine m;
    m.emu.load_segment(SegReg::SS, SMALLSTACK);
    m.regs().w_esp(0);
    expect_fault("push below a 4 KiB stack segment", X86Emulator::VEC_SS, [&] { m.emu.push<uint16_t>(1); });
    CHECK(m.regs().r_sp() == 0, "host push fault moved sp to %04X", m.regs().r_sp());
    m.regs().w_esp(0x0FFF);
    expect_fault("pop straddling the stack limit", X86Emulator::VEC_SS, [&] { m.emu.pop<uint16_t>(); });
    expect_fault("read_seg past the DS limit", X86Emulator::VEC_GP,
        [&] { m.emu.read_seg<uint16_t>(SegReg::DS, 0xFFFF); });
    m.emu.set_segment_null(SegReg::ES);
    expect_fault("write_seg through a null ES", X86Emulator::VEC_GP,
        [&] { m.emu.write_seg<uint8_t>(SegReg::ES, 0, 1); });
  }
  {
    auto mem = std::make_shared<MemoryContext>();
    mem->allocate_at(0x00400000, 0x1000);
    X86Emulator emu(mem);
    emu.set_flat_mode(0);
    expect_fault("read_seg of unmapped flat memory", X86Emulator::VEC_PF,
        [&] { emu.read_seg<uint32_t>(SegReg::DS, 0x12345678); });
    emu.registers().w_esp(0x00800000);
    expect_fault("push onto an unmapped flat stack", X86Emulator::VEC_PF, [&] { emu.push<uint32_t>(1); });
    CHECK(emu.registers().r_esp() == 0x00800000, "host push fault moved esp to %08X", emu.registers().r_esp());
  }

  // Inside an int handler the same failure is a guest fault at the int instruction (restartable: EIP/ESP roll back).
  {
    Machine m;
    m.code(BASE_A, 0, {0xB8, 0x01, 0x00, 0xCD, 0x21, 0xCD, 0x20}); // mov ax, 1; int 0x21; int 0x20
    m.on_int = [&](X86Emulator& e, uint8_t v) {
      if (v == 0x21) {
        e.read_seg<uint16_t>(SegReg::DS, 0xFFFF); // straddles the 64 KiB limit
      }
    };
    m.run();
    CHECK(m.faults.size() == 1 && m.faults[0].vector == X86Emulator::VEC_GP && m.faults[0].eip == 3,
        "handler read_seg fault: %s", m.faults.empty() ? "no fault" : m.faults[0].str().c_str());
  }

  // A fault raised from the debug hook is reported against the instruction about to execute, and rolling back to it
  // must not re-run the previous instruction.
  {
    auto mem = std::make_shared<MemoryContext>();
    mem->allocate_at(0x00400000, 0x10000);
    X86Emulator emu(mem);
    emu.set_flat_mode(0);
    // mov eax, 1; push eax; inc eax; inc eax; int 0x20 (a rollback to the first inc would leave eax = 4)
    mem->write(0x00400000, std::string("\xB8\x01\x00\x00\x00\x50\x40\x40\xCD\x20", 10));
    bool fired = false;
    emu.set_debug_hook([&](X86Emulator& e) {
      if (!fired && e.registers().eip == 0x00400007) { // the second inc
        fired = true;
        e.read_seg<uint32_t>(SegReg::DS, 0x12345678);
      }
    });
    std::vector<X86Emulator::Fault> faults;
    emu.set_fault_handler([&](X86Emulator&, const X86Emulator::Fault& f) { faults.push_back(f); });
    emu.set_interrupt_handler([](X86Emulator& e, uint8_t) { e.request_stop(); });
    emu.registers().eip = 0x00400000;
    emu.registers().w_esp(0x0040F000);
    emu.run(100);
    CHECK(faults.size() == 1 && faults[0].eip == 0x00400007 && faults[0].vector == X86Emulator::VEC_PF &&
            faults[0].address == 0x12345678,
        "debug hook fault: %s", faults.empty() ? "none" : faults[0].str().c_str());
    CHECK(emu.registers().r_eax() == 3 && emu.registers().r_esp() == 0x0040EFFC,
        "debug hook fault re-ran an instruction: eax=%u esp=%08X", emu.registers().r_eax(), emu.registers().r_esp());
  }

  // Accesses that run past 0xFFFFFFFF are unmapped (#PF), not a wrap into the top arena's host memory.
  {
    auto mem = std::make_shared<MemoryContext>();
    mem->allocate_at(0x00400000, 0x1000);
    // (The block allocator cannot place a block on the top page - its bookkeeping wraps - but an arena can map it.)
    mem->preallocate_arena(0xFFFFF000, 0x1000);
    mem->write_u32l(0xFFFFFFFC, 0x11223344);
    bool threw = false;
    try {
      mem->read_u32l(0xFFFFFFFE);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    CHECK(threw, "MemoryContext read across 0xFFFFFFFF did not throw");
    X86Emulator emu(mem);
    emu.set_flat_mode(0);
    mem->write(0x00400000, std::string("\xA1\xFE\xFF\xFF\xFF", 5)); // mov eax, [0xFFFFFFFE]
    X86Emulator::Fault fault;
    bool faulted = false;
    emu.set_fault_handler([&](X86Emulator& e, const X86Emulator::Fault& f) {
      fault = f;
      faulted = true;
      e.request_stop();
    });
    emu.registers().eip = 0x00400000;
    emu.run(10);
    CHECK(faulted && fault.vector == X86Emulator::VEC_PF && fault.address == 0xFFFFFFFE,
        "dword at FFFFFFFE: %s", faulted ? fault.str().c_str() : "no fault");
  }
}

void test_demand_paging_rep() {
  // A host that commits memory on first touch services a long rep stosd one page at a time: every retry faults at the
  // same CS:EIP, never completing the instruction, but at a new address with EDI/ECX advanced. The unresolved-fault
  // guard must not mistake that for a handler that fixes nothing.
  auto mem = std::make_shared<MemoryContext>();
  mem->allocate_at(0x00400000, 0x1000);
  X86Emulator emu(mem);
  emu.set_flat_mode(0);
  constexpr uint32_t BUF = 0x01000000;
  constexpr uint32_t PAGES = 80;
  // mov edi, BUF; mov ecx, PAGES*1024; mov eax, 0x5A5A5A5A; cld; rep stosd; int 0x20
  std::string code("\xBF\x00\x00\x00\x01\xB9\x00\x00\x00\x00\xB8\x5A\x5A\x5A\x5A\xFC\xF3\xAB\xCD\x20", 20);
  uint32_t count = PAGES * 1024;
  memcpy(code.data() + 6, &count, 4);
  mem->write(0x00400000, code);
  uint32_t page_faults = 0;
  bool other_fault = false;
  emu.set_fault_handler([&](X86Emulator& e, const X86Emulator::Fault& f) {
    if (f.vector == X86Emulator::VEC_PF && f.address >= BUF && f.address < BUF + PAGES * 0x1000) {
      page_faults++;
      mem->preallocate_arena(f.address & ~0xFFFu, 0x1000);
    } else {
      other_fault = true;
      e.request_stop();
    }
  });
  emu.set_interrupt_handler([](X86Emulator& e, uint8_t) { e.request_stop(); });
  emu.registers().eip = 0x00400000;
  emu.registers().w_esp(0x00400F00);
  bool threw = false;
  try {
    emu.run(1000);
  } catch (const X86Emulator::fault_error& e) {
    threw = true;
    printf("  demand paging: %s\n", e.what());
  }
  CHECK(!threw && !other_fault && page_faults == PAGES, "demand paging: %u page faults, threw=%d", page_faults, threw);
  CHECK(emu.registers().r_ecx() == 0 && emu.registers().r_edi() == BUF + PAGES * 0x1000,
      "demand paging: ecx=%08X edi=%08X", emu.registers().r_ecx(), emu.registers().r_edi());
  constexpr uint32_t LAST = BUF + (PAGES - 1) * 0x1000 + 0xFFC;
  CHECK(mem->exists(LAST, 4) && mem->read_u32l(LAST) == 0x5A5A5A5A, "demand paging: last dword not stored");

  // A handler that resolves nothing still ends in fault_error rather than spinning.
  X86Emulator emu2(mem);
  emu2.set_flat_mode(0);
  mem->write(0x00400100, std::string("\xA1\x00\x00\x00\x20", 5)); // mov eax, [0x20000000]
  uint32_t calls = 0;
  emu2.set_fault_handler([&](X86Emulator&, const X86Emulator::Fault&) { calls++; });
  emu2.registers().eip = 0x00400100;
  threw = false;
  try {
    emu2.run(1000);
  } catch (const X86Emulator::fault_error& e) {
    threw = (e.fault.vector == X86Emulator::VEC_PF);
  }
  CHECK(threw && calls == 65, "unresolved fault: threw=%d after %u handler calls", threw, calls);
}

void test_16bit_disassembly() {
  // The disassembler follows CS.D too (used by traces of 16-bit code).
  static const uint8_t code[] = {0x8B, 0x46, 0x06, 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12, 0xC4, 0x3F};
  std::string dasm = X86Emulator::disassemble(code, sizeof(code), 0, nullptr, false, true);
  CHECK(dasm.find("[bp + 0x0006]") != std::string::npos && dasm.find("eax, 0x12345678") != std::string::npos &&
          dasm.find("les") != std::string::npos,
      "16-bit disassembly:\n%s", dasm.c_str());
}

} // namespace

// The instruction fetch window (X86Emulator::fetch_instruction_data): instruction bytes are read through a cached
// host view of the code arena, which must (1) see bytes written after the window was set up, (2) stop at the CS limit
// (the fetch past it is still #GP), and (3) never outlive its arena: code in a new arena at the same address runs,
// not the freed one's bytes.
void test_fetch_window() {
  {
    Machine m;
    // A:0000  mov ax, 1   B8 01 00
    //         int 0x21    CD 21        (the host rewrites the next instruction)
    //         mov ax, 3   B8 03 00  -> mov ax, 2
    //         int 0x20    CD 20
    m.code(BASE_A, 0, {0xB8, 0x01, 0x00, 0xCD, 0x21, 0xB8, 0x03, 0x00, 0xCD, 0x20});
    m.on_int = [&](X86Emulator&, uint8_t v) {
      if (v == 0x21) m.mem->write_u8(BASE_A + 6, 0x02);
    };
    m.run();
    CHECK(m.faults.empty(), "fetch window: unexpected fault");
    CHECK(m.regs().r_ax() == 2, "fetch window: rewritten code not seen (ax=%04X)", m.regs().r_ax());
  }
  {
    Machine m;
    // A:0000  jmp far B:0FF0        EA F0 0F <B>
    // B:0FF0  14 nops, then mov ax, 0x1234 at 0FFE (B8 34 12): its immediate runs past B's 0FFF limit.
    m.code(BASE_A, 0, {0xEA, 0xF0, 0x0F, CODE_B & 0xFF, CODE_B >> 8});
    for (uint32_t z = 0x0FF0; z < 0x0FFE; z++) m.code(BASE_B, z, {0x90});
    m.code(BASE_B, 0x0FFE, {0xB8, 0x34, 0x12});
    m.run();
    CHECK(m.faults.size() == 1 && m.faults[0].vector == X86Emulator::VEC_GP && m.faults[0].eip == 0x0FFE &&
            m.faults[0].cs == CODE_B,
        "fetch window: a fetch past the CS limit %s", m.faults.empty() ? "did not fault" : m.faults[0].str().c_str());
  }
  {
    auto mem = std::make_shared<MemoryContext>();
    mem->allocate_at(0x00400000, 0x1000);
    mem->allocate_at(0x00600000, 0x1000);
    X86Emulator emu(mem);
    emu.set_flat_mode(0x00600000);
    emu.set_interrupt_handler([](X86Emulator& e, uint8_t) { e.request_stop(); });
    mem->write(0x00400000, std::string("\xB8\x01\x00\x00\x00\xCD\x20", 7));  // mov eax, 1; int 0x20
    emu.registers().eip = 0x00400000;
    emu.registers().w_esp(0x00600F00);
    emu.run(100);
    CHECK(emu.registers().r_eax() == 1, "fetch window: first arena ran eax=%08X", emu.registers().r_eax());
    mem->free(0x00400000);
    mem->allocate_at(0x00400000, 0x1000);
    mem->write(0x00400000, std::string("\xB8\x02\x00\x00\x00\xCD\x20", 7));  // mov eax, 2; int 0x20
    emu.registers().eip = 0x00400000;
    emu.run(100);
    CHECK(emu.registers().r_eax() == 2, "fetch window: the new arena's code did not run (eax=%08X)",
        emu.registers().r_eax());
  }
}

int main() {
  test_fetch_window();
  test_pascal_far_call();
  test_stack_wrap();
  test_lds_les();
  test_string_overrides();
  test_int_rewrites_cs_ip();
  test_limit_fault_and_retry();
  test_reload_segments();
  test_lsl_lar_verr();
  test_arpl_sreg_ops();
  test_mixed_32bit_in_16bit();
  test_far_32bit_transfers();
  test_16bit_misc();
  test_flat_mode();
  test_nested_run_until();
  test_host_api_faults();
  test_demand_paging_rep();
  test_16bit_disassembly();
  printf("segmentation: %d of %d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}
