// Execution half of X86Emulator: derived from resource_dasm's src/Emulators/X86Emulator.cc
// (https://github.com/swannman/resource_dasm, branch afterdark-perf, commit
// 02d8ea9a58eaf559a9194601b33d98723c9d4f60), itself a fork of fuzziqersoftware/resource_dasm.
// MIT, (c) Martin Michelsen; modified for Long After Dark.
// See ../LICENSE.resource_dasm for the license text.
//
// Long After Dark rewrote most of this half (the disassembler/assembler half stays in X86Emulator.cc):
//  * every memory access goes through a segment (identity segments short-circuit, so flat code pays one predictable
//    branch per access), with 16-bit addressing, 16-bit stacks, far transfers and segment loads;
//  * architectural faults are raised as CPUFault and reported to the host by the run loop;
//  * flag computations use the textbook carry/overflow formulas (verified against the host CPU by the differential
//    test in ../tests), and the forms upstream left unimplemented (imul r,r/m[,imm], enter, far ret, bound, xlat,
//    cmpxchg, lds & co., jcxz/loop, ...) are filled in.

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <bit>
#include <phosg/Encoding.hh>
#include <phosg/Strings.hh>

#include "X86Emulator.hh"

namespace adw::cpu {

static_assert(std::endian::native == std::endian::little, "the x86 core reads guest memory in host byte order");

// ---- Faults ---------------------------------------------------------------------------------------------------------

void X86Emulator::raise(uint8_t vector, uint32_t error_code, const char* what) {
  throw CPUFault{vector, error_code, 0, what};
}

void X86Emulator::raise_divide_by_zero(const char* what) {
  throw CPUFault{VEC_DE, 0, 0, what, true};
}

void X86Emulator::raise_unmapped(uint32_t addr) {
  throw CPUFault{VEC_PF, 0, addr, "access to unmapped emulated memory"};
}

void X86Emulator::host_fault(const CPUFault& f) const {
  if (this->run_depth) {
    throw f;
  }
  Fault ff{f.vector, f.error_code, this->segs[SEG_CS].sel, this->regs.eip, f.address, f.what ? f.what : "",
      f.divide_by_zero};
  throw fault_error(ff);
}

static const char* name_for_vector(uint8_t vector) {
  switch (vector) {
    case 0:
      return "#DE";
    case 1:
      return "#DB";
    case 3:
      return "#BP";
    case 4:
      return "#OF";
    case 5:
      return "#BR";
    case 6:
      return "#UD";
    case 7:
      return "#NM";
    case 10:
      return "#TS";
    case 11:
      return "#NP";
    case 12:
      return "#SS";
    case 13:
      return "#GP";
    case 14:
      return "#PF";
    case 16:
      return "#MF";
    case 17:
      return "#AC";
    default:
      return "#??";
  }
}

std::string X86Emulator::Fault::str() const {
  std::string ret = std::format("{} (vector {}) at {:04X}:{:08X}", name_for_vector(this->vector), this->vector,
      this->cs, this->eip);
  if (this->error_code) {
    ret += std::format(" error code {:04X}", this->error_code);
  }
  if (this->vector == VEC_PF) {
    ret += std::format(" address {:08X}", this->address);
  }
  if (!this->what.empty()) {
    ret += ": ";
    ret += this->what;
  }
  return ret;
}

X86Emulator::fault_error::fault_error(const Fault& f) : std::runtime_error(f.str()), fault(f) {}

// ---- Flags ----------------------------------------------------------------------------------------------------------

template <typename T>
static inline uint32_t szp_flags(T res) {
  uint32_t f = 0;
  if (res & phosg::msb_for_type<T>) {
    f |= X86Emulator::Regs::SF;
  }
  if (res == 0) {
    f |= X86Emulator::Regs::ZF;
  }
  // PF reflects only the low byte, and is set for an even number of 1 bits.
  if (!__builtin_parity(static_cast<uint8_t>(res))) {
    f |= X86Emulator::Regs::PF;
  }
  return f;
}

template <typename T>
  requires(std::is_unsigned_v<T>)
void X86Emulator::Regs::set_flags_integer_result(T res, uint32_t apply_mask) {
  this->replace_flags(apply_mask & (SF | ZF | PF), szp_flags<T>(res));
}

template <typename T>
  requires(std::is_unsigned_v<T>)
void X86Emulator::Regs::set_flags_bitwise_result(T res, uint32_t apply_mask) {
  // OF and CF are cleared; AF is architecturally undefined and left alone.
  this->replace_flags(apply_mask & (SF | ZF | PF | OF | CF), szp_flags<T>(res));
}

template <typename T>
  requires(std::is_unsigned_v<T>)
T X86Emulator::Regs::set_flags_integer_add(T a, T b, uint32_t apply_mask) {
  T res = a + b;
  uint32_t f = szp_flags<T>(res);
  f |= (res < a) ? CF : 0;
  f |= (((a ^ res) & (b ^ res)) & phosg::msb_for_type<T>) ? OF : 0;
  f |= ((a ^ b ^ res) & 0x10) ? AF : 0;
  this->replace_flags(apply_mask, f);
  return res;
}

template <typename T>
  requires(std::is_unsigned_v<T>)
T X86Emulator::Regs::set_flags_integer_add_with_carry(T a, T b, uint32_t apply_mask) {
  T c = this->read_flag(CF) ? 1 : 0;
  T res = a + b + c;
  uint32_t f = szp_flags<T>(res);
  f |= (c ? (res <= a) : (res < a)) ? CF : 0;
  f |= (((a ^ res) & (b ^ res)) & phosg::msb_for_type<T>) ? OF : 0;
  f |= ((a ^ b ^ res) & 0x10) ? AF : 0;
  this->replace_flags(apply_mask, f);
  return res;
}

template <typename T>
  requires(std::is_unsigned_v<T>)
T X86Emulator::Regs::set_flags_integer_subtract(T a, T b, uint32_t apply_mask) {
  T res = a - b;
  uint32_t f = szp_flags<T>(res);
  f |= (a < b) ? CF : 0;
  f |= (((a ^ b) & (a ^ res)) & phosg::msb_for_type<T>) ? OF : 0;
  f |= ((a ^ b ^ res) & 0x10) ? AF : 0;
  this->replace_flags(apply_mask, f);
  return res;
}

template <typename T>
  requires(std::is_unsigned_v<T>)
T X86Emulator::Regs::set_flags_integer_subtract_with_borrow(T a, T b, uint32_t apply_mask) {
  T c = this->read_flag(CF) ? 1 : 0;
  T res = a - b - c;
  uint32_t f = szp_flags<T>(res);
  f |= (c ? (a <= b) : (a < b)) ? CF : 0;
  f |= (((a ^ b) & (a ^ res)) & phosg::msb_for_type<T>) ? OF : 0;
  f |= ((a ^ b ^ res) & 0x10) ? AF : 0;
  this->replace_flags(apply_mask, f);
  return res;
}

bool X86Emulator::Regs::check_condition(uint8_t cc) const {
  uint32_t f = this->eflags;
  bool r;
  switch ((cc >> 1) & 7) {
    case 0: // o
      r = f & OF;
      break;
    case 1: // b/c
      r = f & CF;
      break;
    case 2: // z/e
      r = f & ZF;
      break;
    case 3: // be
      r = f & (CF | ZF);
      break;
    case 4: // s
      r = f & SF;
      break;
    case 5: // p
      r = f & PF;
      break;
    case 6: // l
      r = (!(f & SF)) != (!(f & OF));
      break;
    default: // le
      r = (f & ZF) || ((!(f & SF)) != (!(f & OF)));
      break;
  }
  return r != (cc & 1);
}

// ---- Construction / segmentation ------------------------------------------------------------------------------------

X86Emulator::X86Emulator(std::shared_ptr<MemoryContext> mem)
    : EmulatorBase(mem), behavior(Behavior::SPECIFICATION), tsc_offset(0), execution_labels_computed(false) {
  this->mem_gen = this->mem->layout_generation_ptr();
  // Upstream had no segmentation at all; start fully flat (every segment identity, FS included) so upstream-style
  // users keep working. set_flat_mode() gives the Win32 layout, the Win16 host loads real selectors.
  SegDesc flat_code{0, 0xFFFFFFFF, true, true, true, true, 3};
  SegDesc flat_data{0, 0xFFFFFFFF, true, false, true, true, 3};
  for (uint8_t z = 0; z < 6; z++) {
    this->set_segment(static_cast<SegReg>(z), (z == SEG_CS) ? 0x1B : 0x23, (z == SEG_CS) ? flat_code : flat_data);
  }
  this->overrides.reset(this->code16);
}

uint8_t X86Emulator::seg_index(Segment s) {
  // Segment (upstream prefix enum): NONE, CS, DS, ES, FS, GS, SS
  static const uint8_t table[7] = {SEG_DS, SEG_CS, SEG_DS, SEG_ES, SEG_FS, SEG_GS, SEG_SS};
  return table[static_cast<uint8_t>(s)];
}

void X86Emulator::refresh_identity(uint8_t seg) {
  auto& s = this->segs[seg];
  if (seg == SEG_CS) {
    this->fw_len = 0;  // the fetch window was the old CS's
  }
  // A data register holding a code or read-only descriptor keeps the checked path, so writes through it still #GP.
  bool usable = (seg == SEG_CS) || (!s.desc.code && s.desc.readable_or_writable);
  s.identity = !s.null && s.desc.present && usable && (s.desc.base == 0) && (s.desc.limit == 0xFFFFFFFF);
}

void X86Emulator::reload_segments() {
  for (uint8_t z = 0; z < 6; z++) {
    auto& s = this->segs[z];
    SegDesc d;
    if (s.null || !this->lookup_descriptor(s.sel, d)) {
      continue;
    }
    s.desc = d;
    this->refresh_identity(z);
  }
  this->update_mode();
}

void X86Emulator::update_mode() {
  this->code16 = !this->segs[SEG_CS].desc.big;
  this->stack16 = !this->segs[SEG_SS].desc.big;
}

void X86Emulator::set_segment(SegReg seg, uint16_t sel, const SegDesc& desc) {
  uint8_t z = static_cast<uint8_t>(seg);
  if (z >= 6) {
    throw std::invalid_argument("invalid segment register");
  }
  auto& s = this->segs[z];
  s.sel = sel;
  s.desc = desc;
  s.null = false;
  this->refresh_identity(z);
  this->update_mode();
}

void X86Emulator::set_segment_null(SegReg seg) {
  uint8_t z = static_cast<uint8_t>(seg);
  if (z == SEG_CS || z == SEG_SS || z >= 6) {
    throw std::invalid_argument("CS and SS cannot hold the null selector");
  }
  auto& s = this->segs[z];
  s.sel = 0;
  s.desc = SegDesc();
  s.null = true;
  s.identity = false;
}

void X86Emulator::set_flat_mode(uint32_t fs_base, uint32_t fs_limit) {
  // Windows NT's selectors: CS=0x1B, DS=ES=SS=0x23, FS=0x3B (TEB), GS null.
  SegDesc code{0, 0xFFFFFFFF, true, true, true, true, 3};
  SegDesc data{0, 0xFFFFFFFF, true, false, true, true, 3};
  SegDesc teb{fs_base, fs_limit, true, false, true, true, 3};
  this->set_segment(SegReg::CS, 0x1B, code);
  this->set_segment(SegReg::DS, 0x23, data);
  this->set_segment(SegReg::ES, 0x23, data);
  this->set_segment(SegReg::SS, 0x23, data);
  this->set_segment(SegReg::FS, 0x3B, teb);
  this->set_segment_null(SegReg::GS);
  this->flat_descriptors = {{0x1B, code}, {0x23, data}, {0x3B, teb}};
}

bool X86Emulator::lookup_descriptor(uint16_t sel, SegDesc& out) const {
  if (this->descriptor_provider && this->descriptor_provider->lookup(sel, out)) {
    return true;
  }
  for (const auto& [flat_sel, desc] : this->flat_descriptors) {
    if ((flat_sel & ~3) == (sel & ~3)) {
      out = desc;
      return true;
    }
  }
  return false;
}

// A selector whose index and TI bit are both zero is the null selector (RPL is ignored).
static inline bool is_null_selector(uint16_t sel) {
  return (sel & ~3) == 0;
}

void X86Emulator::load_data_segment(uint8_t seg, uint16_t sel) {
  if (is_null_selector(sel)) {
    if (seg == SEG_SS) {
      raise(VEC_GP, 0, "null selector loaded into SS");
    }
    auto& s = this->segs[seg];
    s.sel = sel;
    s.desc = SegDesc();
    s.null = true;
    s.identity = false;
    return;
  }
  SegDesc d;
  if (!this->lookup_descriptor(sel, d)) {
    raise(VEC_GP, sel & ~3, "segment load: no descriptor for selector");
  }
  if (seg == SEG_SS) {
    if (d.code || !d.readable_or_writable) {
      raise(VEC_GP, sel & ~3, "SS load: not a writable data segment");
    }
    if (!d.present) {
      raise(VEC_SS, sel & ~3, "SS load: segment not present");
    }
  } else {
    if (d.code && !d.readable_or_writable) {
      raise(VEC_GP, sel & ~3, "segment load: execute-only code segment");
    }
    if (!d.present) {
      raise(VEC_NP, sel & ~3, "segment load: segment not present");
    }
  }
  auto& s = this->segs[seg];
  s.sel = sel;
  s.desc = d;
  s.null = false;
  this->refresh_identity(seg);
  if (seg == SEG_SS) {
    this->update_mode();
  }
}

SegDesc X86Emulator::check_code_segment(uint16_t sel, uint32_t eip) {
  if (is_null_selector(sel)) {
    raise(VEC_GP, 0, "far transfer to the null selector");
  }
  SegDesc d;
  if (!this->lookup_descriptor(sel, d)) {
    raise(VEC_GP, sel & ~3, "far transfer: no descriptor for selector");
  }
  if (!d.code) {
    raise(VEC_GP, sel & ~3, "far transfer to a non-code segment");
  }
  if (!d.present) {
    raise(VEC_NP, sel & ~3, "far transfer: segment not present");
  }
  if (eip > d.limit) {
    raise(VEC_GP, 0, "far transfer beyond the target segment limit");
  }
  return d;
}

void X86Emulator::commit_cs(uint16_t sel, const SegDesc& desc, uint32_t eip) {
  auto& s = this->segs[SEG_CS];
  s.sel = sel;
  s.desc = desc;
  s.null = false;
  this->refresh_identity(SEG_CS);
  this->update_mode();
  this->regs.eip = eip;
}

void X86Emulator::load_segment(SegReg seg, uint16_t sel) {
  uint8_t z = static_cast<uint8_t>(seg);
  if (z >= 6) {
    throw std::invalid_argument("invalid segment register");
  }
  try {
    if (z == SEG_CS) {
      SegDesc d = this->check_code_segment(sel, 0);
      this->commit_cs(sel, d, this->regs.eip);
    } else {
      this->load_data_segment(z, sel);
    }
  } catch (const CPUFault& f) {
    this->host_fault(f);
  }
}

void X86Emulator::set_cs_eip(uint16_t cs, uint32_t eip) {
  try {
    SegDesc d = this->check_code_segment(cs, eip);
    this->commit_cs(cs, d, eip);
  } catch (const CPUFault& f) {
    this->host_fault(f);
  }
}

uint32_t X86Emulator::linear_address(SegReg seg, uint32_t offset, uint32_t size, bool write) {
  try {
    return this->lin_data(static_cast<uint8_t>(seg), offset, size, write);
  } catch (const CPUFault& f) {
    this->host_fault(f);
  }
}

uint32_t X86Emulator::lin_data_slow(uint8_t seg, uint32_t off, uint32_t size, bool write) const {
  const auto& s = this->segs[seg];
  if (s.null) {
    raise(VEC_GP, 0, "memory access through a null selector");
  }
  if (write) {
    if (s.desc.code || !s.desc.readable_or_writable) {
      raise(VEC_GP, 0, "write to a code or read-only segment");
    }
  } else if (s.desc.code && !s.desc.readable_or_writable) {
    raise(VEC_GP, 0, "read from an execute-only segment");
  }
  if (static_cast<uint64_t>(off) + size - 1 > s.desc.limit) {
    raise((seg == SEG_SS) ? VEC_SS : VEC_GP, 0, "segment limit exceeded");
  }
  return s.desc.base + off;
}

uint32_t X86Emulator::lin_stack16(uint16_t sp, uint32_t size) const {
  return this->lin_data(SEG_SS, sp, size, true);
}

uint32_t X86Emulator::lin_code_slow(uint32_t off, uint32_t size) const {
  const auto& s = this->segs[SEG_CS];
  if (static_cast<uint64_t>(off) + size - 1 > s.desc.limit) {
    raise(VEC_GP, 0, "instruction fetch beyond the CS limit");
  }
  return s.desc.base + off;
}

void X86Emulator::refill_fetch_window(uint32_t eip, uint32_t linear) {
  this->fw_len = 0;
  uint32_t lo = 0, size = 0;
  uint8_t* host = nullptr;
  if (!this->mem->arena_span(linear, &lo, &size, &host)) {
    return;
  }
  // The arena ∩ the segment [base, base + limit], in linear addresses (the checked path's base + offset), then as CS
  // offsets. A segment that wraps past 4 GB keeps its wrapped part on the checked path.
  const auto& cs = this->segs[SEG_CS];
  const uint64_t base = cs.identity ? 0 : cs.desc.base;
  const uint64_t end = base + (cs.identity ? 0xFFFFFFFFull : uint64_t(cs.desc.limit)) + 1;
  const uint64_t w_lo = std::max<uint64_t>(lo, base);
  const uint64_t w_hi = std::min<uint64_t>(uint64_t(lo) + size, end);
  if (w_hi <= w_lo || linear < w_lo || linear >= w_hi || uint64_t(eip) != linear - base) {
    return;
  }
  this->fw_off = static_cast<uint32_t>(w_lo - base);
  this->fw_len = static_cast<uint32_t>(std::min<uint64_t>(w_hi - w_lo, 0xFFFFFFFFull));
  this->fw_host = host + (w_lo - lo);
  this->fw_gen = this->mem->layout_generation();
}

uint8_t* X86Emulator::refill_data_span(uint32_t addr, uint32_t size) {
  uint32_t lo = 0, len = 0;
  uint8_t* host = nullptr;
  if (!this->mem->arena_span(addr, &lo, &len, &host)) {
    return nullptr;
  }
  DataSpan& s = this->dspan[this->dspan_next];
  this->dspan_next ^= 1;
  s.lo = lo;
  s.len = len;
  s.host = host;
  uint32_t d = addr - lo;
  return (d < len && len - d >= size) ? host + d : nullptr;
}

uint32_t X86Emulator::debug_linear(uint8_t seg, uint32_t off) const {
  return this->segs[seg].desc.base + off;
}

void X86Emulator::set_eip_near(uint32_t target) {
  if (this->overrides.operand_size) {
    target &= 0xFFFF;
  }
  if (!this->segs[SEG_CS].identity && target > this->segs[SEG_CS].desc.limit) {
    raise(VEC_GP, 0, "near branch beyond the CS limit");
  }
  this->regs.eip = target;
}

// ---- Run loop -------------------------------------------------------------------------------------------------------

void X86Emulator::deliver_fault(
    uint8_t vector, uint32_t error_code, uint32_t address, const std::string& what, bool divide_by_zero) {
  // Faults are restartable: the instruction appears not to have executed (EIP and ESP roll back; the far transfers
  // commit CS last, so CS needs no rollback).
  this->regs.eip = this->insn_eip;
  this->regs.w_esp(this->insn_esp);
  this->overrides.reset(this->code16);
  Fault f{vector, error_code, this->segs[SEG_CS].sel, this->insn_eip, address, what, divide_by_zero};
  if (vector == VEC_UD) {
    // Name the offending bytes: #UD is how unimplemented instructions surface.
    std::string bytes;
    for (uint32_t z = 0; z < 8; z++) {
      try {
        bytes += std::format("{}{:02X}", bytes.empty() ? "" : " ",
            this->mem->read<uint8_t>(this->debug_linear(SEG_CS, this->insn_eip + z)));
      } catch (const std::exception&) {
        break;
      }
    }
    f.what += " [" + bytes + "]";
  }
  if (!this->fault_handler) {
    throw fault_error(f);
  }
  // A handler that returns without resolving the fault would spin forever; give up after enough consecutive
  // identical faults with no progress in between. "Identical" includes the fault's details and the string registers:
  // a rep string op that a host services one page at a time (demand commit on #PF) faults again and again at the same
  // CS:EIP without completing, but each time at a new address with ECX/ESI/EDI advanced.
  uint32_t ecx = this->regs.r_ecx(), esi = this->regs.r_esi(), edi = this->regs.r_edi();
  if ((f.cs == this->last_fault_cs) && (f.eip == this->last_fault_eip) &&
      (this->instructions_executed == this->last_fault_icount) && (f.vector == this->last_fault_vector) &&
      (f.error_code == this->last_fault_error_code) && (f.address == this->last_fault_address) &&
      (ecx == this->last_fault_ecx) && (esi == this->last_fault_esi) && (edi == this->last_fault_edi)) {
    if (++this->repeated_fault_count > 64) {
      f.what += " (fault handler did not resolve it)";
      throw fault_error(f);
    }
  } else {
    this->repeated_fault_count = 0;
    this->last_fault_cs = f.cs;
    this->last_fault_eip = f.eip;
    this->last_fault_icount = this->instructions_executed;
    this->last_fault_vector = f.vector;
    this->last_fault_error_code = f.error_code;
    this->last_fault_address = f.address;
    this->last_fault_ecx = ecx;
    this->last_fault_esi = esi;
    this->last_fault_edi = edi;
  }
  this->fault_handler(*this, f);
}

void X86Emulator::execute_one_inner() {
  if (this->fw_gen != *this->mem_gen) [[unlikely]] {
    this->fw_len = 0;  // an arena came or went since the window was made
  }
  this->insn_eip = this->regs.eip;
  this->insn_esp = this->regs.r_esp();
  this->overrides.reset(this->code16);
  for (uint8_t prefix_count = 0;; prefix_count++) {
    uint8_t opcode = this->fetch_instruction_byte();
    (this->*fns[opcode].exec)(opcode);
    if (this->overrides.should_clear) {
      return;
    }
    // A prefix: keep going with the next byte as part of the same instruction.
    this->overrides.should_clear = true;
    if (prefix_count >= 14) {
      raise(VEC_GP, 0, "instruction longer than 15 bytes");
    }
  }
}

X86Emulator::StopReason X86Emulator::run_loop(
    bool use_stop, bool check_cs, uint16_t stop_cs, uint32_t stop_eip, uint64_t max_instructions) {
  // A nested run (a host callback into guest code from inside a handler) must not disturb the interrupted
  // instruction's rollback state or prefixes.
  uint32_t saved_insn_eip = this->insn_eip;
  uint32_t saved_insn_esp = this->insn_esp;
  Overrides saved_overrides = this->overrides;
  auto restore = [&]() {
    this->run_depth--;
    this->stop_requested = false;
    this->insn_eip = saved_insn_eip;
    this->insn_esp = saved_insn_esp;
    this->overrides = saved_overrides;
  };
  this->stop_requested = false;
  this->run_depth++;
  uint64_t executed = 0;
  StopReason reason = StopReason::LIMIT;
  try {
    for (bool done = false; !done;) {
      try {
        for (;;) {
          if (use_stop && (this->regs.eip == stop_eip) && (!check_cs || (this->segs[SEG_CS].sel == stop_cs))) {
            reason = StopReason::ADDRESS;
            done = true;
            break;
          }
          if (executed >= max_instructions) {
            reason = StopReason::LIMIT;
            done = true;
            break;
          }
          if (this->debug_hook) [[unlikely]] {
            // A fault the hook raises (say, a read_seg of a bad pointer) is reported against the instruction about to
            // run, so the rollback state must already describe it, not the previous instruction.
            this->insn_eip = this->regs.eip;
            this->insn_esp = this->regs.r_esp();
            this->debug_hook(*this);
          }
          this->execute_one_inner();
          executed++;
          this->instructions_executed++;
          if (this->stop_requested) {
            reason = StopReason::REQUESTED;
            done = true;
            break;
          }
        }
      } catch (const CPUFault& f) {
        executed++;
        this->deliver_fault(f.vector, f.error_code, f.address, f.what ? f.what : "", f.divide_by_zero);
      } catch (const std::out_of_range& e) {
        // MemoryContext found no arena at the address: the emulated equivalent of an unmapped page.
        executed++;
        this->deliver_fault(VEC_PF, 0, 0, e.what());
      }
      if (!done && this->stop_requested) {
        reason = StopReason::REQUESTED;
        done = true;
      }
    }
  } catch (const CPUFault& f) {
    // A fault raised by host code inside the fault handler itself (e.g. a load_segment that fails while "fixing" the
    // fault): there is no guest instruction left to blame, so it leaves run() as a fault_error.
    restore();
    Fault ff{f.vector, f.error_code, this->segs[SEG_CS].sel, this->regs.eip, f.address, f.what ? f.what : ""};
    throw fault_error(ff);
  } catch (...) {
    restore();
    throw;
  }
  restore();
  return reason;
}

X86Emulator::StopReason X86Emulator::run(uint64_t max_instructions) {
  return this->run_loop(false, false, 0, 0, max_instructions);
}

X86Emulator::StopReason X86Emulator::run_until(uint32_t eip, uint64_t max_instructions) {
  return this->run_loop(true, false, 0, eip, max_instructions);
}

X86Emulator::StopReason X86Emulator::run_until(uint16_t cs, uint32_t eip, uint64_t max_instructions) {
  return this->run_loop(true, true, cs, eip, max_instructions);
}

void X86Emulator::execute_one() {
  this->run_loop(false, false, 0, 0, 1);
}

void X86Emulator::execute() {
  this->execution_labels_computed = false;
  try {
    this->run_loop(false, false, 0, 0, UINT64_MAX);
  } catch (const terminate_emulation&) {
  }
  this->execution_labels.clear();
}

// ---- Operand decoding -----------------------------------------------------------------------------------------------

X86Emulator::DecodedRM X86Emulator::fetch_and_decode_rm() {
  DecodedRM ret;
  uint8_t modrm = this->fetch_instruction_byte();
  ret.non_ea_reg = (modrm >> 3) & 7;
  uint8_t mode = modrm >> 6;
  uint8_t r = modrm & 7;
  if (mode == 3) {
    ret.ea_reg = r;
    ret.ea_index_scale = -1;
    return ret;
  }

  ret.ea_index_scale = 0;
  if (!this->overrides.address_size) {
    if (r == 4) {
      uint8_t sib = this->fetch_instruction_byte();
      uint8_t base = sib & 7;
      uint8_t index = (sib >> 3) & 7;
      if (index != 4) {
        ret.ea_index_reg = index;
        ret.ea_index_scale = 1 << (sib >> 6);
      }
      if ((base == 5) && (mode == 0)) {
        ret.ea_reg = -1;
        ret.ea_disp = this->fetch_instruction_dword();
      } else {
        ret.ea_reg = base;
      }
    } else if ((r == 5) && (mode == 0)) {
      ret.ea_reg = -1;
      ret.ea_disp = this->fetch_instruction_dword();
    } else {
      ret.ea_reg = r;
    }
    if (mode == 1) {
      ret.ea_disp = static_cast<int8_t>(this->fetch_instruction_byte());
    } else if (mode == 2) {
      ret.ea_disp = this->fetch_instruction_dword();
    }

  } else {
    // 16-bit forms: [bx+si] [bx+di] [bp+si] [bp+di] [si] [di] [bp]/[disp16] [bx]
    static const int8_t bases[8] = {3, 3, 5, 5, -1, -1, 5, 3};
    static const int8_t indexes[8] = {6, 7, 6, 7, 6, 7, -1, -1};
    ret.addr16 = true;
    ret.ea_reg = bases[r];
    ret.ea_index_reg = indexes[r];
    ret.ea_index_scale = (indexes[r] >= 0) ? 1 : 0;
    if ((mode == 0) && (r == 6)) {
      ret.ea_reg = -1;
      ret.ea_disp = this->fetch_instruction_word();
    } else if (mode == 1) {
      ret.ea_disp = static_cast<int8_t>(this->fetch_instruction_byte());
    } else if (mode == 2) {
      ret.ea_disp = static_cast<int16_t>(this->fetch_instruction_word());
    }
  }

  if (this->overrides.segment != Segment::NONE) {
    ret.seg = seg_index(this->overrides.segment);
  } else {
    // EBP/ESP-based (BP-based in 16-bit forms) operands default to SS.
    ret.seg = ((ret.ea_reg == 4) || (ret.ea_reg == 5)) ? SEG_SS : SEG_DS;
  }
  return ret;
}

uint32_t X86Emulator::resolve_mem_ea(const DecodedRM& rm) const {
  if (rm.addr16) [[unlikely]] {
    uint32_t v = rm.ea_disp;
    if (rm.ea_reg >= 0) {
      v += this->regs.reg16(rm.ea_reg);
    }
    if (rm.ea_index_scale > 0) {
      v += this->regs.reg16(rm.ea_index_reg);
    }
    return v & 0xFFFF;
  }
  uint32_t v = rm.ea_disp;
  if (rm.ea_reg >= 0) {
    v += this->regs.reg32(rm.ea_reg);
  }
  if (rm.ea_index_scale > 0) {
    v += rm.ea_index_scale * this->regs.reg32(rm.ea_index_reg);
  }
  return v;
}

// ---- Integer arithmetic ---------------------------------------------------------------------------------------------

template <typename T>
T X86Emulator::exec_integer_math_logic(uint8_t what, T dest, T src) {
  switch (what) {
    case 0: // add
      return this->regs.set_flags_integer_add<T>(dest, src);
    case 1: // or
      dest |= src;
      this->regs.set_flags_bitwise_result<T>(dest);
      return dest;
    case 2: // adc
      return this->regs.set_flags_integer_add_with_carry<T>(dest, src);
    case 3: // sbb
      return this->regs.set_flags_integer_subtract_with_borrow<T>(dest, src);
    case 4: // and
      dest &= src;
      this->regs.set_flags_bitwise_result<T>(dest);
      return dest;
    case 5: // sub
      return this->regs.set_flags_integer_subtract<T>(dest, src);
    case 6: // xor
      dest ^= src;
      this->regs.set_flags_bitwise_result<T>(dest);
      return dest;
    default: // cmp
      this->regs.set_flags_integer_subtract<T>(dest, src);
      return dest;
  }
}

// dest = r/m. cmp (what == 7) only reads, so it neither needs write access nor bumps the page's write generation.
template <typename T>
void X86Emulator::alu_to_ea(uint8_t what, const DecodedRM& rm, T src) {
  if (what == 7) {
    this->exec_integer_math_logic<T>(7, this->read_ea<T>(rm), src);
  } else {
    this->rmw_ea<T>(rm, [&](T d) -> T { return this->exec_integer_math_logic<T>(what, d, src); });
  }
}

void X86Emulator::exec_0x_1x_2x_3x_x0_x1_x8_x9_mem_reg_math(uint8_t opcode) {
  uint8_t what = (opcode >> 3) & 7;
  DecodedRM rm = this->fetch_and_decode_rm();
  if (!(opcode & 1)) {
    this->alu_to_ea<uint8_t>(what, rm, this->r_non_ea8(rm));
  } else if (this->overrides.operand_size) {
    this->alu_to_ea<uint16_t>(what, rm, this->r_non_ea16(rm));
  } else {
    this->alu_to_ea<uint32_t>(what, rm, this->r_non_ea32(rm));
  }
}

void X86Emulator::exec_0x_1x_2x_3x_x2_x3_xA_xB_reg_mem_math(uint8_t opcode) {
  uint8_t what = (opcode >> 3) & 7;
  DecodedRM rm = this->fetch_and_decode_rm();
  if (!(opcode & 1)) {
    uint8_t v = this->exec_integer_math_logic<uint8_t>(what, this->r_non_ea8(rm), this->read_ea<uint8_t>(rm));
    if (what != 7) {
      this->w_non_ea8(rm, v);
    }
  } else if (this->overrides.operand_size) {
    uint16_t v = this->exec_integer_math_logic<uint16_t>(what, this->r_non_ea16(rm), this->read_ea<uint16_t>(rm));
    if (what != 7) {
      this->w_non_ea16(rm, v);
    }
  } else {
    uint32_t v = this->exec_integer_math_logic<uint32_t>(what, this->r_non_ea32(rm), this->read_ea<uint32_t>(rm));
    if (what != 7) {
      this->w_non_ea32(rm, v);
    }
  }
}

void X86Emulator::exec_0x_1x_2x_3x_x4_x5_xC_xD_eax_imm_math(uint8_t opcode) {
  uint8_t what = (opcode >> 3) & 7;
  if (!(opcode & 1)) {
    uint8_t v = this->exec_integer_math_logic<uint8_t>(what, this->regs.r_al(), this->fetch_instruction_byte());
    if (what != 7) {
      this->regs.w_al(v);
    }
  } else if (this->overrides.operand_size) {
    uint16_t v = this->exec_integer_math_logic<uint16_t>(what, this->regs.r_ax(), this->fetch_instruction_word());
    if (what != 7) {
      this->regs.w_ax(v);
    }
  } else {
    uint32_t v = this->exec_integer_math_logic<uint32_t>(what, this->regs.r_eax(), this->fetch_instruction_dword());
    if (what != 7) {
      this->regs.w_eax(v);
    }
  }
}

void X86Emulator::exec_80_to_83_imm_math(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;
  if (!(opcode & 1)) {
    // 82 is an alias of 80 in 32-bit code.
    this->alu_to_ea<uint8_t>(what, rm, this->fetch_instruction_byte());
  } else if (this->overrides.operand_size) {
    uint16_t v = (opcode & 2) ? static_cast<uint16_t>(static_cast<int8_t>(this->fetch_instruction_byte()))
                              : this->fetch_instruction_word();
    this->alu_to_ea<uint16_t>(what, rm, v);
  } else {
    uint32_t v = (opcode & 2) ? static_cast<uint32_t>(static_cast<int8_t>(this->fetch_instruction_byte()))
                              : this->fetch_instruction_dword();
    this->alu_to_ea<uint32_t>(what, rm, v);
  }
}

void X86Emulator::exec_84_85_test_rm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (!(opcode & 1)) {
    this->regs.set_flags_bitwise_result<uint8_t>(this->r_non_ea8(rm) & this->read_ea<uint8_t>(rm));
  } else if (this->overrides.operand_size) {
    this->regs.set_flags_bitwise_result<uint16_t>(this->r_non_ea16(rm) & this->read_ea<uint16_t>(rm));
  } else {
    this->regs.set_flags_bitwise_result<uint32_t>(this->r_non_ea32(rm) & this->read_ea<uint32_t>(rm));
  }
}

void X86Emulator::exec_A8_A9_test_eax_imm(uint8_t opcode) {
  if (!(opcode & 1)) {
    this->regs.set_flags_bitwise_result<uint8_t>(this->regs.r_al() & this->fetch_instruction_byte());
  } else if (this->overrides.operand_size) {
    this->regs.set_flags_bitwise_result<uint16_t>(this->regs.r_ax() & this->fetch_instruction_word());
  } else {
    this->regs.set_flags_bitwise_result<uint32_t>(this->regs.r_eax() & this->fetch_instruction_dword());
  }
}

void X86Emulator::exec_40_to_47_inc(uint8_t opcode) {
  uint8_t which = opcode & 7;
  if (this->overrides.operand_size) {
    this->regs.write16(which, this->regs.set_flags_integer_add<uint16_t>(this->regs.read16(which), 1, Regs::default_int_flags & ~Regs::CF));
  } else {
    this->regs.write32(which, this->regs.set_flags_integer_add<uint32_t>(this->regs.read32(which), 1, Regs::default_int_flags & ~Regs::CF));
  }
}

void X86Emulator::exec_48_to_4F_dec(uint8_t opcode) {
  uint8_t which = opcode & 7;
  if (this->overrides.operand_size) {
    this->regs.write16(which, this->regs.set_flags_integer_subtract<uint16_t>(this->regs.read16(which), 1, Regs::default_int_flags & ~Regs::CF));
  } else {
    this->regs.write32(which, this->regs.set_flags_integer_subtract<uint32_t>(this->regs.read32(which), 1, Regs::default_int_flags & ~Regs::CF));
  }
}

void X86Emulator::exec_27_2F_daa_das(uint8_t opcode) {
  bool is_das = (opcode & 8);
  uint8_t old_al = this->regs.r_al();
  bool old_cf = this->regs.read_flag(Regs::CF);
  uint8_t al = old_al;
  bool cf = false;
  bool af = false;
  if (((al & 0x0F) > 9) || this->regs.read_flag(Regs::AF)) {
    if (is_das) {
      cf = old_cf || (al < 6);
      al -= 6;
    } else {
      cf = old_cf || (al > 0xF9);
      al += 6;
    }
    af = true;
  }
  if ((old_al > 0x99) || old_cf) {
    al += is_das ? -0x60 : 0x60;
    cf = true;
  } else if (!is_das) {
    // DAA clears CF here; DAS keeps the borrow from the low-nibble step.
    cf = false;
  }
  this->regs.w_al(al);
  this->regs.replace_flags(Regs::CF | Regs::AF | Regs::SF | Regs::ZF | Regs::PF,
      (cf ? Regs::CF : 0) | (af ? Regs::AF : 0) | szp_flags<uint8_t>(al));
}

void X86Emulator::exec_37_3F_aaa_aas(uint8_t opcode) {
  bool is_aas = (opcode & 8);
  if (this->regs.read_flag(Regs::AF) || ((this->regs.r_al() & 0x0F) > 9)) {
    this->regs.w_ax(this->regs.r_ax() + (is_aas ? -6 : 6));
    this->regs.w_ah(this->regs.r_ah() + (is_aas ? -1 : 1));
    this->regs.replace_flags(Regs::AF | Regs::CF, Regs::AF | Regs::CF);
  } else {
    this->regs.replace_flags(Regs::AF | Regs::CF, 0);
  }
  this->regs.w_al(this->regs.r_al() & 0x0F);
}

void X86Emulator::exec_D4_amx_aam(uint8_t) {
  uint8_t base = this->fetch_instruction_byte();
  if (base == 0) {
    raise_divide_by_zero("aam with a zero base");
  }
  uint8_t al = this->regs.r_al();
  this->regs.w_ah(al / base);
  this->regs.w_al(al % base);
  this->regs.set_flags_integer_result<uint8_t>(this->regs.r_al());
}

void X86Emulator::exec_D5_adx_aad(uint8_t) {
  uint8_t base = this->fetch_instruction_byte();
  uint8_t al = this->regs.r_al() + (this->regs.r_ah() * base);
  this->regs.w_ax(al);
  this->regs.set_flags_integer_result<uint8_t>(al);
}

void X86Emulator::exec_D6_salc(uint8_t) {
  this->regs.w_al(this->regs.read_flag(Regs::CF) ? 0xFF : 0x00);
}

void X86Emulator::exec_98_cbw_cwde(uint8_t) {
  if (this->overrides.operand_size) {
    this->regs.w_ax(static_cast<int8_t>(this->regs.r_al()));
  } else {
    this->regs.w_eax(static_cast<int16_t>(this->regs.r_ax()));
  }
}

void X86Emulator::exec_99_cwd_cdq(uint8_t) {
  if (this->overrides.operand_size) {
    this->regs.w_dx((this->regs.r_ax() & 0x8000) ? 0xFFFF : 0x0000);
  } else {
    this->regs.w_edx((this->regs.r_eax() & 0x80000000) ? 0xFFFFFFFF : 0x00000000);
  }
}

template <typename T>
T X86Emulator::exec_imul_logic(T a, T b) {
  using ST = std::make_signed_t<T>;
  int64_t full = static_cast<int64_t>(static_cast<ST>(a)) * static_cast<int64_t>(static_cast<ST>(b));
  T res = static_cast<T>(full);
  bool of = (full != static_cast<int64_t>(static_cast<ST>(res)));
  // SF/ZF/PF/AF are undefined; SF/ZF/PF follow the truncated result.
  this->regs.replace_flags(Regs::CF | Regs::OF | Regs::SF | Regs::ZF | Regs::PF,
      (of ? (Regs::CF | Regs::OF) : 0) | szp_flags<T>(res));
  return res;
}

void X86Emulator::exec_69_6B_imul(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (this->overrides.operand_size) {
    uint16_t src = this->read_ea<uint16_t>(rm);
    uint16_t imm = (opcode & 2) ? static_cast<uint16_t>(static_cast<int8_t>(this->fetch_instruction_byte()))
                                : this->fetch_instruction_word();
    this->w_non_ea16(rm, this->exec_imul_logic<uint16_t>(src, imm));
  } else {
    uint32_t src = this->read_ea<uint32_t>(rm);
    uint32_t imm = (opcode & 2) ? static_cast<uint32_t>(static_cast<int8_t>(this->fetch_instruction_byte()))
                                : this->fetch_instruction_dword();
    this->w_non_ea32(rm, this->exec_imul_logic<uint32_t>(src, imm));
  }
}

void X86Emulator::exec_0F_AF_imul(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if (this->overrides.operand_size) {
    this->w_non_ea16(rm, this->exec_imul_logic<uint16_t>(this->r_non_ea16(rm), this->read_ea<uint16_t>(rm)));
  } else {
    this->w_non_ea32(rm, this->exec_imul_logic<uint32_t>(this->r_non_ea32(rm), this->read_ea<uint32_t>(rm)));
  }
}

template <typename T, typename LET>
T X86Emulator::exec_F6_F7_misc_math_logic(uint8_t what, T value) {
  constexpr uint32_t bits = phosg::bits_for_type<T>;
  switch (what) {
    case 0: // test
    case 1: { // test (documented by AMD, not Intel)
      T imm;
      if constexpr (bits == 8) {
        imm = this->fetch_instruction_byte();
      } else if constexpr (bits == 16) {
        imm = this->fetch_instruction_word();
      } else {
        imm = this->fetch_instruction_dword();
      }
      this->regs.set_flags_bitwise_result<T>(value & imm);
      break;
    }
    case 2: // not (no flags)
      value = ~value;
      break;
    case 3: // neg
      value = this->regs.set_flags_integer_subtract<T>(0, value);
      break;
    case 4: { // mul
      bool of_cf;
      if constexpr (bits == 8) {
        uint16_t res = static_cast<uint16_t>(this->regs.r_al()) * value;
        this->regs.w_ax(res);
        of_cf = (res & 0xFF00) != 0;
      } else if constexpr (bits == 16) {
        uint32_t res = static_cast<uint32_t>(this->regs.r_ax()) * value;
        this->regs.w_dx(res >> 16);
        this->regs.w_ax(res);
        of_cf = (res & 0xFFFF0000) != 0;
      } else {
        uint64_t res = static_cast<uint64_t>(this->regs.r_eax()) * value;
        this->regs.w_edx(res >> 32);
        this->regs.w_eax(res);
        of_cf = (res >> 32) != 0;
      }
      this->regs.replace_flags(Regs::OF | Regs::CF, of_cf ? (Regs::OF | Regs::CF) : 0);
      break;
    }
    case 5: { // imul
      bool of_cf;
      if constexpr (bits == 8) {
        int16_t res = static_cast<int8_t>(this->regs.r_al()) * static_cast<int8_t>(value);
        this->regs.w_ax(res);
        of_cf = (res != static_cast<int8_t>(res));
      } else if constexpr (bits == 16) {
        int32_t res = static_cast<int16_t>(this->regs.r_ax()) * static_cast<int16_t>(value);
        this->regs.w_dx(static_cast<uint32_t>(res) >> 16);
        this->regs.w_ax(res);
        of_cf = (res != static_cast<int16_t>(res));
      } else {
        int64_t res = static_cast<int64_t>(static_cast<int32_t>(this->regs.r_eax())) * static_cast<int32_t>(value);
        this->regs.w_edx(static_cast<uint64_t>(res) >> 32);
        this->regs.w_eax(res);
        of_cf = (res != static_cast<int32_t>(res));
      }
      this->regs.replace_flags(Regs::OF | Regs::CF, of_cf ? (Regs::OF | Regs::CF) : 0);
      break;
    }
    case 6: // div (flags undefined; left alone)
      if (value == 0) {
        raise_divide_by_zero("division by zero");
      }
      if constexpr (bits == 8) {
        uint16_t dividend = this->regs.r_ax();
        uint16_t quotient = dividend / value;
        if (quotient > 0xFF) {
          raise(VEC_DE, 0, "division overflow");
        }
        this->regs.w_al(quotient);
        this->regs.w_ah(dividend % value);
      } else if constexpr (bits == 16) {
        uint32_t dividend = (static_cast<uint32_t>(this->regs.r_dx()) << 16) | this->regs.r_ax();
        uint32_t quotient = dividend / value;
        if (quotient > 0xFFFF) {
          raise(VEC_DE, 0, "division overflow");
        }
        this->regs.w_ax(quotient);
        this->regs.w_dx(dividend % value);
      } else {
        uint64_t dividend = (static_cast<uint64_t>(this->regs.r_edx()) << 32) | this->regs.r_eax();
        uint64_t quotient = dividend / value;
        if (quotient > 0xFFFFFFFFULL) {
          raise(VEC_DE, 0, "division overflow");
        }
        this->regs.w_eax(quotient);
        this->regs.w_edx(dividend % value);
      }
      break;
    default: { // idiv (flags undefined; left alone)
      if (value == 0) {
        raise_divide_by_zero("division by zero");
      }
      // The division happens in a wider type so that MIN / -1 cannot trap on the host.
      if constexpr (bits == 8) {
        int32_t dividend = static_cast<int16_t>(this->regs.r_ax());
        int32_t divisor = static_cast<int8_t>(value);
        int32_t quotient = dividend / divisor;
        if (quotient < -0x80 || quotient > 0x7F) {
          raise(VEC_DE, 0, "division overflow");
        }
        this->regs.w_al(quotient);
        this->regs.w_ah(dividend % divisor);
      } else if constexpr (bits == 16) {
        int64_t dividend = static_cast<int32_t>((static_cast<uint32_t>(this->regs.r_dx()) << 16) | this->regs.r_ax());
        int64_t divisor = static_cast<int16_t>(value);
        int64_t quotient = dividend / divisor;
        if (quotient < -0x8000 || quotient > 0x7FFF) {
          raise(VEC_DE, 0, "division overflow");
        }
        this->regs.w_ax(quotient);
        this->regs.w_dx(dividend % divisor);
      } else {
        __int128 dividend = static_cast<int64_t>((static_cast<uint64_t>(this->regs.r_edx()) << 32) | this->regs.r_eax());
        __int128 divisor = static_cast<int32_t>(value);
        __int128 quotient = dividend / divisor;
        if (quotient < -0x80000000LL || quotient > 0x7FFFFFFFLL) {
          raise(VEC_DE, 0, "division overflow");
        }
        this->regs.w_eax(static_cast<uint32_t>(quotient));
        this->regs.w_edx(static_cast<uint32_t>(dividend % divisor));
      }
      break;
    }
  }
  return value;
}

void X86Emulator::exec_F6_F7_misc_math(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;
  // Only not (2) and neg (3) write the operand.
  if ((what & 6) == 2) {
    if (!(opcode & 1)) {
      this->rmw_ea<uint8_t>(rm, [&](uint8_t v) { return this->exec_F6_F7_misc_math_logic<uint8_t>(what, v); });
    } else if (this->overrides.operand_size) {
      this->rmw_ea<uint16_t>(rm, [&](uint16_t v) { return this->exec_F6_F7_misc_math_logic<uint16_t>(what, v); });
    } else {
      this->rmw_ea<uint32_t>(rm, [&](uint32_t v) { return this->exec_F6_F7_misc_math_logic<uint32_t>(what, v); });
    }
  } else if (!(opcode & 1)) {
    this->exec_F6_F7_misc_math_logic<uint8_t>(what, this->read_ea<uint8_t>(rm));
  } else if (this->overrides.operand_size) {
    this->exec_F6_F7_misc_math_logic<uint16_t>(what, this->read_ea<uint16_t>(rm));
  } else {
    this->exec_F6_F7_misc_math_logic<uint32_t>(what, this->read_ea<uint32_t>(rm));
  }
}

// ---- Shifts and rotates ---------------------------------------------------------------------------------------------

template <typename T>
T X86Emulator::exec_bit_shifts_logic(uint8_t what, T value, uint8_t distance, bool distance_is_cl) {
  (void)distance_is_cl;
  constexpr uint32_t bits = phosg::bits_for_type<T>;
  constexpr T msb = phosg::msb_for_type<T>;
  uint32_t count = distance & 0x1F;
  if (count == 0) {
    return value; // no flags are affected
  }
  switch (what) {
    case 0: // rol
    case 1: { // ror
      uint32_t n = count & (bits - 1);
      if (n) {
        value = (what == 0) ? static_cast<T>((value << n) | (value >> (bits - n)))
                            : static_cast<T>((value >> n) | (value << (bits - n)));
      }
      // CF is written whenever the masked count is nonzero (even a multiple of the width). OF is defined only for
      // 1-bit rotates; the 1-bit formula is used for every count.
      bool cf = (what == 0) ? (value & 1) : !!(value & msb);
      bool of = (what == 0) ? (!!(value & msb) != cf) : (!!(value & msb) != !!(value & (msb >> 1)));
      this->regs.replace_flags(Regs::CF | Regs::OF, (cf ? Regs::CF : 0) | (of ? Regs::OF : 0));
      return value;
    }
    case 2: // rcl
    case 3: { // rcr
      // Rotation through CF: a (bits+1)-wide rotation of CF:value.
      uint32_t n = count % (bits + 1);
      bool old_cf = this->regs.read_flag(Regs::CF);
      bool rcr_of = !!(value & msb) != old_cf; // RCR's OF is computed before the rotation
      if (n) {
        uint64_t mask = (1ULL << (bits + 1)) - 1;
        uint64_t combined = (static_cast<uint64_t>(old_cf) << bits) | value;
        if (what == 2) {
          combined = ((combined << n) | (combined >> (bits + 1 - n))) & mask;
        } else {
          combined = ((combined >> n) | (combined << (bits + 1 - n))) & mask;
        }
        value = static_cast<T>(combined);
        bool cf = (combined >> bits) & 1;
        bool of = (what == 2) ? (!!(value & msb) != cf) : rcr_of;
        this->regs.replace_flags(Regs::CF | Regs::OF, (cf ? Regs::CF : 0) | (of ? Regs::OF : 0));
      } else {
        // The masked count is a multiple of bits+1: value and CF are unchanged; OF is undefined.
        bool of = (what == 2) ? (!!(value & msb) != old_cf) : rcr_of;
        this->regs.replace_flags(Regs::OF, of ? Regs::OF : 0);
      }
      return value;
    }
    case 4: // shl
    case 6: { // sal (same as shl)
      bool cf = (count <= bits) ? ((static_cast<uint64_t>(value) >> (bits - count)) & 1) : false;
      T res = (count < bits) ? static_cast<T>(value << count) : 0;
      bool of = !!(res & msb) != cf;
      this->regs.replace_flags(Regs::CF | Regs::OF | Regs::SF | Regs::ZF | Regs::PF,
          (cf ? Regs::CF : 0) | (of ? Regs::OF : 0) | szp_flags<T>(res));
      return res;
    }
    case 5: { // shr
      bool cf = (count <= bits) ? ((value >> (count - 1)) & 1) : false;
      T res = (count < bits) ? static_cast<T>(value >> count) : 0;
      bool of = !!(value & msb);
      this->regs.replace_flags(Regs::CF | Regs::OF | Regs::SF | Regs::ZF | Regs::PF,
          (cf ? Regs::CF : 0) | (of ? Regs::OF : 0) | szp_flags<T>(res));
      return res;
    }
    default: { // sar
      using ST = std::make_signed_t<T>;
      T res;
      bool cf;
      if (count < bits) {
        res = static_cast<T>(static_cast<ST>(value) >> count);
        cf = (static_cast<ST>(value) >> (count - 1)) & 1;
      } else {
        res = (value & msb) ? static_cast<T>(~static_cast<T>(0)) : 0;
        cf = !!(value & msb);
      }
      this->regs.replace_flags(Regs::CF | Regs::OF | Regs::SF | Regs::ZF | Regs::PF,
          (cf ? Regs::CF : 0) | szp_flags<T>(res));
      return res;
    }
  }
}

void X86Emulator::exec_C0_C1_bit_shifts(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint8_t distance = this->fetch_instruction_byte();
  uint8_t what = rm.non_ea_reg;
  if (!(opcode & 1)) {
    this->rmw_ea<uint8_t>(rm, [&](uint8_t v) { return this->exec_bit_shifts_logic<uint8_t>(what, v, distance, false); });
  } else if (this->overrides.operand_size) {
    this->rmw_ea<uint16_t>(rm, [&](uint16_t v) { return this->exec_bit_shifts_logic<uint16_t>(what, v, distance, false); });
  } else {
    this->rmw_ea<uint32_t>(rm, [&](uint32_t v) { return this->exec_bit_shifts_logic<uint32_t>(what, v, distance, false); });
  }
}

void X86Emulator::exec_D0_to_D3_bit_shifts(uint8_t opcode) {
  bool distance_is_cl = (opcode & 2);
  auto rm = this->fetch_and_decode_rm();
  uint8_t distance = distance_is_cl ? this->regs.r_cl() : 1;
  uint8_t what = rm.non_ea_reg;
  if (!(opcode & 1)) {
    this->rmw_ea<uint8_t>(rm, [&](uint8_t v) { return this->exec_bit_shifts_logic<uint8_t>(what, v, distance, distance_is_cl); });
  } else if (this->overrides.operand_size) {
    this->rmw_ea<uint16_t>(rm, [&](uint16_t v) { return this->exec_bit_shifts_logic<uint16_t>(what, v, distance, distance_is_cl); });
  } else {
    this->rmw_ea<uint32_t>(rm, [&](uint32_t v) { return this->exec_bit_shifts_logic<uint32_t>(what, v, distance, distance_is_cl); });
  }
}

template <typename T>
T X86Emulator::exec_shld_shrd_logic(
    bool is_right_shift, T dest_value, T incoming_value, uint8_t distance, bool distance_is_cl) {
  (void)distance_is_cl;
  constexpr uint32_t bits = phosg::bits_for_type<T>;
  constexpr T msb = phosg::msb_for_type<T>;
  uint32_t count = distance & 0x1F;
  if (count == 0) {
    return dest_value;
  }
  // Counts above the operand width (possible only with 16-bit operands) are undefined; this shifts the 48-bit
  // concatenation dest:src:dest (shld) / dest:src:dest read from the other end (shrd), a deterministic choice.
  using U128 = unsigned __int128;
  U128 cat = (static_cast<U128>(dest_value) << (2 * bits)) | (static_cast<U128>(incoming_value) << bits) | dest_value;
  T res;
  bool cf;
  if (!is_right_shift) {
    res = static_cast<T>(cat >> (2 * bits - count));
    cf = static_cast<bool>((cat >> (3 * bits - count)) & 1);
  } else {
    res = static_cast<T>(cat >> count);
    cf = static_cast<bool>((cat >> (count - 1)) & 1);
  }
  bool of = !!((res ^ dest_value) & msb);
  this->regs.replace_flags(Regs::CF | Regs::OF | Regs::SF | Regs::ZF | Regs::PF,
      (cf ? Regs::CF : 0) | (of ? Regs::OF : 0) | szp_flags<T>(res));
  return res;
}

void X86Emulator::exec_0F_A4_A5_AC_AD_shld_shrd(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  bool distance_is_cl = (opcode & 1);
  uint8_t distance = distance_is_cl ? this->regs.r_cl() : this->fetch_instruction_byte();
  bool right = (opcode & 8);
  if (this->overrides.operand_size) {
    uint16_t src = this->r_non_ea16(rm);
    this->rmw_ea<uint16_t>(rm, [&](uint16_t v) { return this->exec_shld_shrd_logic<uint16_t>(right, v, src, distance, distance_is_cl); });
  } else {
    uint32_t src = this->r_non_ea32(rm);
    this->rmw_ea<uint32_t>(rm, [&](uint32_t v) { return this->exec_shld_shrd_logic<uint32_t>(right, v, src, distance, distance_is_cl); });
  }
}

// ---- Bit operations -------------------------------------------------------------------------------------------------

template <typename T>
T X86Emulator::exec_bit_test_ops_logic(uint8_t what, T v, uint8_t bit_number) {
  T mask = static_cast<T>(1) << bit_number;
  this->regs.replace_flag(Regs::CF, v & mask);
  switch (what) {
    case 0: // bt
      return v;
    case 1: // bts
      return v | mask;
    case 2: // btr
      return v & ~mask;
    default: // btc
      return v ^ mask;
  }
}

void X86Emulator::exec_0F_A3_AB_B3_BB_bit_tests(uint8_t opcode) {
  DecodedRM rm = this->fetch_and_decode_rm();
  uint8_t what = (opcode >> 3) & 3;
  if (this->overrides.operand_size) {
    int32_t bit = static_cast<int16_t>(this->r_non_ea16(rm));
    if (rm.ea_index_scale < 0) {
      uint16_t v = this->exec_bit_test_ops_logic<uint16_t>(what, this->regs.read16(rm.ea_reg), bit & 15);
      if (what) {
        this->regs.write16(rm.ea_reg, v);
      }
    } else {
      // A register bit offset is signed and can address anywhere around the operand.
      uint32_t off = this->resolve_mem_ea(rm) + (bit >> 4) * 2;
      if (rm.addr16) {
        off &= 0xFFFF;
      }
      uint32_t addr = this->lin_data(rm.seg, off, 2, what != 0);
      uint16_t v = this->exec_bit_test_ops_logic<uint16_t>(what, this->r_mem<uint16_t>(addr), bit & 15);
      if (what) {
        this->w_mem<uint16_t>(addr, v);
      }
    }
  } else {
    int32_t bit = static_cast<int32_t>(this->r_non_ea32(rm));
    if (rm.ea_index_scale < 0) {
      uint32_t v = this->exec_bit_test_ops_logic<uint32_t>(what, this->regs.read32(rm.ea_reg), bit & 31);
      if (what) {
        this->regs.write32(rm.ea_reg, v);
      }
    } else {
      uint32_t off = this->resolve_mem_ea(rm) + (bit >> 5) * 4;
      if (rm.addr16) {
        off &= 0xFFFF;
      }
      uint32_t addr = this->lin_data(rm.seg, off, 4, what != 0);
      uint32_t v = this->exec_bit_test_ops_logic<uint32_t>(what, this->r_mem<uint32_t>(addr), bit & 31);
      if (what) {
        this->w_mem<uint32_t>(addr, v);
      }
    }
  }
}

void X86Emulator::exec_0F_BA_bit_tests(uint8_t) {
  DecodedRM rm = this->fetch_and_decode_rm();
  if (!(rm.non_ea_reg & 4)) {
    raise(VEC_UD, 0, "0F BA /0-3");
  }
  uint8_t what = rm.non_ea_reg & 3;
  // An immediate bit offset is taken modulo the operand width (it never leaves the operand).
  uint8_t bit = this->fetch_instruction_byte();
  if (this->overrides.operand_size) {
    if (what) {
      this->rmw_ea<uint16_t>(rm, [&](uint16_t v) { return this->exec_bit_test_ops_logic<uint16_t>(what, v, bit & 15); });
    } else {
      this->exec_bit_test_ops_logic<uint16_t>(0, this->read_ea<uint16_t>(rm), bit & 15);
    }
  } else {
    if (what) {
      this->rmw_ea<uint32_t>(rm, [&](uint32_t v) { return this->exec_bit_test_ops_logic<uint32_t>(what, v, bit & 31); });
    } else {
      this->exec_bit_test_ops_logic<uint32_t>(0, this->read_ea<uint32_t>(rm), bit & 31);
    }
  }
}

void X86Emulator::exec_0F_BC_BD_bsf_bsr(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint32_t value = this->overrides.operand_size ? this->read_ea<uint16_t>(rm) : this->read_ea<uint32_t>(rm);
  if (value == 0) {
    // The destination is left unchanged (what hardware does; the manual says undefined).
    this->regs.replace_flag(Regs::ZF, true);
  } else {
    this->regs.replace_flag(Regs::ZF, false);
    uint32_t result = (opcode & 1) ? (31 - std::countl_zero(value)) : std::countr_zero(value);
    if (this->overrides.operand_size) {
      this->w_non_ea16(rm, result);
    } else {
      this->w_non_ea32(rm, result);
    }
  }
}

void X86Emulator::exec_0F_90_to_9F_setcc_rm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm(); // the reg field is ignored
  this->write_ea<uint8_t>(rm, this->regs.check_condition(opcode & 0x0F) ? 1 : 0);
}

void X86Emulator::exec_0F_40_to_4F_cmov_rm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  // The source is read (and can fault) whether or not the condition holds.
  if (this->overrides.operand_size) {
    uint16_t v = this->read_ea<uint16_t>(rm);
    if (this->regs.check_condition(opcode & 0x0F)) {
      this->w_non_ea16(rm, v);
    }
  } else {
    uint32_t v = this->read_ea<uint32_t>(rm);
    if (this->regs.check_condition(opcode & 0x0F)) {
      this->w_non_ea32(rm, v);
    }
  }
}

void X86Emulator::exec_0F_B6_B7_BE_BF_movzx_movsx(uint8_t opcode) {
  DecodedRM rm = this->fetch_and_decode_rm();
  uint32_t v;
  if (opcode & 1) {
    uint16_t x = this->read_ea<uint16_t>(rm);
    v = (opcode & 8) ? static_cast<uint32_t>(static_cast<int16_t>(x)) : x;
  } else {
    uint8_t x = this->read_ea<uint8_t>(rm);
    v = (opcode & 8) ? static_cast<uint32_t>(static_cast<int8_t>(x)) : x;
  }
  if (this->overrides.operand_size) {
    this->w_non_ea16(rm, v);
  } else {
    this->w_non_ea32(rm, v);
  }
}

void X86Emulator::exec_0F_C0_C1_xadd_rm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  // The register operand receives the old destination before the sum is written (so xadd r, r ends with the sum).
  if (!(opcode & 1)) {
    uint8_t src = this->r_non_ea8(rm);
    this->rmw_ea<uint8_t>(rm, [&](uint8_t d) {
      uint8_t sum = this->regs.set_flags_integer_add<uint8_t>(d, src);
      this->w_non_ea8(rm, d);
      return sum;
    });
  } else if (this->overrides.operand_size) {
    uint16_t src = this->r_non_ea16(rm);
    this->rmw_ea<uint16_t>(rm, [&](uint16_t d) {
      uint16_t sum = this->regs.set_flags_integer_add<uint16_t>(d, src);
      this->w_non_ea16(rm, d);
      return sum;
    });
  } else {
    uint32_t src = this->r_non_ea32(rm);
    this->rmw_ea<uint32_t>(rm, [&](uint32_t d) {
      uint32_t sum = this->regs.set_flags_integer_add<uint32_t>(d, src);
      this->w_non_ea32(rm, d);
      return sum;
    });
  }
}

void X86Emulator::exec_0F_B0_B1_cmpxchg(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  // The destination is always written (with its old value on a mismatch), as on hardware.
  if (!(opcode & 1)) {
    uint8_t src = this->r_non_ea8(rm);
    this->rmw_ea<uint8_t>(rm, [&](uint8_t d) -> uint8_t {
      uint8_t acc = this->regs.r_al();
      this->regs.set_flags_integer_subtract<uint8_t>(acc, d);
      if (acc == d) {
        return src;
      }
      this->regs.w_al(d);
      return d;
    });
  } else if (this->overrides.operand_size) {
    uint16_t src = this->r_non_ea16(rm);
    this->rmw_ea<uint16_t>(rm, [&](uint16_t d) -> uint16_t {
      uint16_t acc = this->regs.r_ax();
      this->regs.set_flags_integer_subtract<uint16_t>(acc, d);
      if (acc == d) {
        return src;
      }
      this->regs.w_ax(d);
      return d;
    });
  } else {
    uint32_t src = this->r_non_ea32(rm);
    this->rmw_ea<uint32_t>(rm, [&](uint32_t d) -> uint32_t {
      uint32_t acc = this->regs.r_eax();
      this->regs.set_flags_integer_subtract<uint32_t>(acc, d);
      if (acc == d) {
        return src;
      }
      this->regs.w_eax(d);
      return d;
    });
  }
}

void X86Emulator::exec_0F_C7_cmpxchg8b(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if (rm.non_ea_reg != 1 || !rm.has_mem_ref()) {
    raise(VEC_UD, 0, "0F C7 other than cmpxchg8b m64");
  }
  uint32_t addr = this->ea_linear(rm, 8, true);
  uint64_t v = this->r_mem<uint64_t>(addr);
  uint64_t expected = (static_cast<uint64_t>(this->regs.r_edx()) << 32) | this->regs.r_eax();
  if (v == expected) {
    this->w_mem<uint64_t>(addr, (static_cast<uint64_t>(this->regs.r_ecx()) << 32) | this->regs.r_ebx());
    this->regs.replace_flag(Regs::ZF, true);
  } else {
    this->w_mem<uint64_t>(addr, v);
    this->regs.w_eax(v);
    this->regs.w_edx(v >> 32);
    this->regs.replace_flag(Regs::ZF, false);
  }
}

void X86Emulator::exec_0F_C8_to_CF_bswap(uint8_t opcode) {
  uint8_t which = opcode & 7;
  if (this->overrides.operand_size) {
    // Undefined for 16-bit operands; hardware zeroes the register.
    this->regs.write16(which, 0);
  } else {
    this->regs.write32(which, phosg::bswap32(this->regs.read32(which)));
  }
}

// ---- Data movement --------------------------------------------------------------------------------------------------

void X86Emulator::exec_86_87_xchg_rm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (!(opcode & 1)) {
    uint8_t a = this->r_non_ea8(rm);
    this->rmw_ea<uint8_t>(rm, [&](uint8_t b) {
      this->w_non_ea8(rm, b);
      return a;
    });
  } else if (this->overrides.operand_size) {
    uint16_t a = this->r_non_ea16(rm);
    this->rmw_ea<uint16_t>(rm, [&](uint16_t b) {
      this->w_non_ea16(rm, b);
      return a;
    });
  } else {
    uint32_t a = this->r_non_ea32(rm);
    this->rmw_ea<uint32_t>(rm, [&](uint32_t b) {
      this->w_non_ea32(rm, b);
      return a;
    });
  }
}

void X86Emulator::exec_88_to_8B_mov_rm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (!(opcode & 1)) {
    if (opcode & 2) {
      this->w_non_ea8(rm, this->read_ea<uint8_t>(rm));
    } else {
      this->write_ea<uint8_t>(rm, this->r_non_ea8(rm));
    }
  } else if (this->overrides.operand_size) {
    if (opcode & 2) {
      this->w_non_ea16(rm, this->read_ea<uint16_t>(rm));
    } else {
      this->write_ea<uint16_t>(rm, this->r_non_ea16(rm));
    }
  } else {
    if (opcode & 2) {
      this->w_non_ea32(rm, this->read_ea<uint32_t>(rm));
    } else {
      this->write_ea<uint32_t>(rm, this->r_non_ea32(rm));
    }
  }
}

void X86Emulator::exec_8D_lea(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if (rm.ea_index_scale < 0) {
    raise(VEC_UD, 0, "lea with a register operand");
  }
  // The address size picks the arithmetic (16-bit forms wrap), the operand size the destination width.
  uint32_t ea = this->resolve_mem_ea(rm);
  if (this->overrides.operand_size) {
    this->w_non_ea16(rm, ea);
  } else {
    this->w_non_ea32(rm, ea);
  }
}

void X86Emulator::exec_90_to_97_xchg_eax(uint8_t opcode) {
  if (opcode == 0x90) {
    return; // nop (also pause with F3, and xchg ax, ax)
  }
  uint8_t which = opcode & 7;
  if (this->overrides.operand_size) {
    uint16_t a = this->regs.r_ax();
    this->regs.w_ax(this->regs.read16(which));
    this->regs.write16(which, a);
  } else {
    uint32_t a = this->regs.r_eax();
    this->regs.w_eax(this->regs.read32(which));
    this->regs.write32(which, a);
  }
}

void X86Emulator::exec_A0_A1_A2_A3_mov_eax_memabs(uint8_t opcode) {
  uint32_t off = this->overrides.address_size ? this->fetch_instruction_word() : this->fetch_instruction_dword();
  uint8_t seg = this->data_seg();
  if (!(opcode & 1)) {
    uint32_t addr = this->lin_data(seg, off, 1, opcode & 2);
    if (opcode & 2) {
      this->w_mem<uint8_t>(addr, this->regs.r_al());
    } else {
      this->regs.w_al(this->r_mem<uint8_t>(addr));
    }
  } else if (this->overrides.operand_size) {
    uint32_t addr = this->lin_data(seg, off, 2, opcode & 2);
    if (opcode & 2) {
      this->w_mem<uint16_t>(addr, this->regs.r_ax());
    } else {
      this->regs.w_ax(this->r_mem<uint16_t>(addr));
    }
  } else {
    uint32_t addr = this->lin_data(seg, off, 4, opcode & 2);
    if (opcode & 2) {
      this->w_mem<uint32_t>(addr, this->regs.r_eax());
    } else {
      this->regs.w_eax(this->r_mem<uint32_t>(addr));
    }
  }
}

void X86Emulator::exec_B0_to_BF_mov_imm(uint8_t opcode) {
  uint8_t which = opcode & 7;
  if (!(opcode & 8)) {
    this->regs.write8(which, this->fetch_instruction_byte());
  } else if (this->overrides.operand_size) {
    this->regs.write16(which, this->fetch_instruction_word());
  } else {
    this->regs.write32(which, this->fetch_instruction_dword());
  }
}

void X86Emulator::exec_C6_C7_mov_rm_imm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (rm.non_ea_reg != 0) {
    raise(VEC_UD, 0, "C6/C7 with reg != 0");
  }
  if (!(opcode & 1)) {
    this->write_ea<uint8_t>(rm, this->fetch_instruction_byte());
  } else if (this->overrides.operand_size) {
    this->write_ea<uint16_t>(rm, this->fetch_instruction_word());
  } else {
    this->write_ea<uint32_t>(rm, this->fetch_instruction_dword());
  }
}

void X86Emulator::exec_D7_xlat(uint8_t) {
  uint32_t off = this->overrides.address_size ? ((this->regs.r_bx() + this->regs.r_al()) & 0xFFFF)
                                              : (this->regs.r_ebx() + this->regs.r_al());
  this->regs.w_al(this->r_mem<uint8_t>(this->lin_data(this->data_seg(), off, 1, false)));
}

// ---- Stack ----------------------------------------------------------------------------------------------------------

void X86Emulator::exec_50_to_57_push(uint8_t opcode) {
  uint8_t which = opcode & 7;
  if (this->overrides.operand_size) {
    this->push_g<uint16_t>(this->regs.read16(which));
  } else {
    this->push_g<uint32_t>(this->regs.read32(which));
  }
}

void X86Emulator::exec_58_to_5F_pop(uint8_t opcode) {
  uint8_t which = opcode & 7;
  if (this->overrides.operand_size) {
    this->regs.write16(which, this->pop_g<uint16_t>());
  } else {
    this->regs.write32(which, this->pop_g<uint32_t>());
  }
}

void X86Emulator::exec_60_pusha(uint8_t) {
  uint32_t original_esp = this->regs.r_esp();
  if (this->overrides.operand_size) {
    this->push_g<uint16_t>(this->regs.r_ax());
    this->push_g<uint16_t>(this->regs.r_cx());
    this->push_g<uint16_t>(this->regs.r_dx());
    this->push_g<uint16_t>(this->regs.r_bx());
    this->push_g<uint16_t>(original_esp & 0xFFFF);
    this->push_g<uint16_t>(this->regs.r_bp());
    this->push_g<uint16_t>(this->regs.r_si());
    this->push_g<uint16_t>(this->regs.r_di());
  } else {
    this->push_g<uint32_t>(this->regs.r_eax());
    this->push_g<uint32_t>(this->regs.r_ecx());
    this->push_g<uint32_t>(this->regs.r_edx());
    this->push_g<uint32_t>(this->regs.r_ebx());
    this->push_g<uint32_t>(original_esp);
    this->push_g<uint32_t>(this->regs.r_ebp());
    this->push_g<uint32_t>(this->regs.r_esi());
    this->push_g<uint32_t>(this->regs.r_edi());
  }
}

void X86Emulator::exec_61_popa(uint8_t) {
  if (this->overrides.operand_size) {
    uint16_t di = this->pop_g<uint16_t>();
    uint16_t si = this->pop_g<uint16_t>();
    uint16_t bp = this->pop_g<uint16_t>();
    this->pop_g<uint16_t>(); // the saved SP is discarded
    uint16_t bx = this->pop_g<uint16_t>();
    uint16_t dx = this->pop_g<uint16_t>();
    uint16_t cx = this->pop_g<uint16_t>();
    uint16_t ax = this->pop_g<uint16_t>();
    this->regs.w_di(di);
    this->regs.w_si(si);
    this->regs.w_bp(bp);
    this->regs.w_bx(bx);
    this->regs.w_dx(dx);
    this->regs.w_cx(cx);
    this->regs.w_ax(ax);
  } else {
    uint32_t edi = this->pop_g<uint32_t>();
    uint32_t esi = this->pop_g<uint32_t>();
    uint32_t ebp = this->pop_g<uint32_t>();
    this->pop_g<uint32_t>();
    uint32_t ebx = this->pop_g<uint32_t>();
    uint32_t edx = this->pop_g<uint32_t>();
    uint32_t ecx = this->pop_g<uint32_t>();
    uint32_t eax = this->pop_g<uint32_t>();
    this->regs.w_edi(edi);
    this->regs.w_esi(esi);
    this->regs.w_ebp(ebp);
    this->regs.w_ebx(ebx);
    this->regs.w_edx(edx);
    this->regs.w_ecx(ecx);
    this->regs.w_eax(eax);
  }
}

void X86Emulator::exec_68_6A_push(uint8_t opcode) {
  // Unlike most opcodes, the higher code is the 8-bit (sign-extended) one.
  if (opcode & 2) {
    int8_t v = static_cast<int8_t>(this->fetch_instruction_byte());
    if (this->overrides.operand_size) {
      this->push_g<uint16_t>(static_cast<uint16_t>(v));
    } else {
      this->push_g<uint32_t>(static_cast<uint32_t>(v));
    }
  } else if (this->overrides.operand_size) {
    this->push_g<uint16_t>(this->fetch_instruction_word());
  } else {
    this->push_g<uint32_t>(this->fetch_instruction_dword());
  }
}

void X86Emulator::exec_8F_pop_rm(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if (rm.non_ea_reg) {
    raise(VEC_UD, 0, "8F with reg != 0");
  }
  // The destination address is computed after ESP is incremented (so pop [esp] stores at the new top).
  if (this->overrides.operand_size) {
    uint16_t v = this->pop_g<uint16_t>();
    this->write_ea<uint16_t>(rm, v);
  } else {
    uint32_t v = this->pop_g<uint32_t>();
    this->write_ea<uint32_t>(rm, v);
  }
}

void X86Emulator::exec_9C_pushf_pushfd(uint8_t) {
  if (this->overrides.operand_size) {
    this->push_g<uint16_t>(this->regs.read_eflags() & 0xFFFF);
  } else {
    // RF and VM read as 0
    this->push_g<uint32_t>(this->regs.read_eflags() & 0x00FCFFFF);
  }
}

void X86Emulator::exec_9D_popf_popfd(uint8_t) {
  // At CPL 3 with IOPL 0, IF and IOPL are silently preserved.
  if (this->overrides.operand_size) {
    static constexpr uint32_t mask = 0x00004DD5;
    this->regs.write_eflags((this->regs.read_eflags() & ~mask) | (this->pop_g<uint16_t>() & mask));
  } else {
    static constexpr uint32_t mask = 0x00244DD5;
    this->regs.write_eflags((this->regs.read_eflags() & ~mask) | (this->pop_g<uint32_t>() & mask));
  }
  this->regs.write_eflags((this->regs.read_eflags() & ~0x00010000) | 0x2); // RF clear; bit 1 always set
}

void X86Emulator::exec_9E_sahf(uint8_t) {
  this->regs.replace_flags(Regs::SF | Regs::ZF | Regs::AF | Regs::PF | Regs::CF, this->regs.r_ah());
}

void X86Emulator::exec_9F_lahf(uint8_t) {
  this->regs.w_ah((this->regs.read_eflags() & 0xD5) | 2);
}

void X86Emulator::exec_C8_enter(uint8_t) {
  uint16_t size = this->fetch_instruction_word();
  uint8_t level = this->fetch_instruction_byte() & 0x1F;
  bool op16 = this->overrides.operand_size;
  if (op16) {
    this->push_g<uint16_t>(this->regs.r_bp());
  } else {
    this->push_g<uint32_t>(this->regs.r_ebp());
  }
  uint32_t frame_temp = this->regs.r_esp();
  if (level > 0) {
    // Copy the enclosing frames' pointers (the display), then push the new frame pointer.
    uint32_t ebp = this->stack16 ? this->regs.r_bp() : this->regs.r_ebp();
    for (uint8_t z = 1; z < level; z++) {
      if (op16) {
        ebp = this->stack16 ? ((ebp - 2) & 0xFFFF) : (ebp - 2);
        this->push_g<uint16_t>(this->r_mem<uint16_t>(this->lin_data(SEG_SS, ebp, 2, false)));
      } else {
        ebp = this->stack16 ? ((ebp - 4) & 0xFFFF) : (ebp - 4);
        this->push_g<uint32_t>(this->r_mem<uint32_t>(this->lin_data(SEG_SS, ebp, 4, false)));
      }
    }
    if (op16) {
      this->push_g<uint16_t>(frame_temp);
    } else {
      this->push_g<uint32_t>(frame_temp);
    }
  }
  if (op16) {
    this->regs.w_bp(frame_temp);
  } else {
    this->regs.w_ebp(frame_temp);
  }
  if (this->stack16) {
    this->regs.w_sp(this->regs.r_sp() - size);
  } else {
    this->regs.w_esp(this->regs.r_esp() - size);
  }
}

void X86Emulator::exec_C9_leave(uint8_t) {
  if (this->stack16) {
    this->regs.w_sp(this->regs.r_bp());
  } else {
    this->regs.w_esp(this->regs.r_ebp());
  }
  if (this->overrides.operand_size) {
    this->regs.w_bp(this->pop_g<uint16_t>());
  } else {
    this->regs.w_ebp(this->pop_g<uint32_t>());
  }
}

void X86Emulator::exec_62_bound(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if (!rm.has_mem_ref()) {
    raise(VEC_UD, 0, "bound with a register operand");
  }
  uint32_t off = this->resolve_mem_ea(rm);
  bool out_of_range;
  if (this->overrides.operand_size) {
    int16_t index = static_cast<int16_t>(this->r_non_ea16(rm));
    uint32_t addr = this->lin_data(rm.seg, off, 4, false);
    int16_t lower = static_cast<int16_t>(this->r_mem<uint16_t>(addr));
    int16_t upper = static_cast<int16_t>(this->r_mem<uint16_t>(addr + 2));
    out_of_range = (index < lower) || (index > upper);
  } else {
    int32_t index = static_cast<int32_t>(this->r_non_ea32(rm));
    uint32_t addr = this->lin_data(rm.seg, off, 8, false);
    int32_t lower = static_cast<int32_t>(this->r_mem<uint32_t>(addr));
    int32_t upper = static_cast<int32_t>(this->r_mem<uint32_t>(addr + 4));
    out_of_range = (index < lower) || (index > upper);
  }
  if (out_of_range) {
    raise(VEC_BR, 0, "bound range exceeded");
  }
}

// ---- Control flow ---------------------------------------------------------------------------------------------------

void X86Emulator::exec_70_to_7F_jcc(uint8_t opcode) {
  // The displacement is always fetched, so a not-taken branch falls through past it.
  uint32_t offset = static_cast<int8_t>(this->fetch_instruction_byte());
  if (this->regs.check_condition(opcode & 0x0F)) {
    this->set_eip_near(this->regs.eip + offset);
  }
}

void X86Emulator::exec_0F_80_to_8F_jcc(uint8_t opcode) {
  uint32_t offset = this->overrides.operand_size ? static_cast<uint32_t>(static_cast<int16_t>(this->fetch_instruction_word()))
                                                 : this->fetch_instruction_dword();
  if (this->regs.check_condition(opcode & 0x0F)) {
    this->set_eip_near(this->regs.eip + offset);
  }
}

void X86Emulator::exec_E0_to_E3_loop_jcxz(uint8_t opcode) {
  uint32_t offset = static_cast<int8_t>(this->fetch_instruction_byte());
  // The address size picks CX or ECX as the counter.
  uint32_t count = this->overrides.address_size ? this->regs.r_cx() : this->regs.r_ecx();
  bool taken;
  if (opcode == 0xE3) { // jcxz / jecxz
    taken = (count == 0);
  } else {
    count--;
    if (this->overrides.address_size) {
      count &= 0xFFFF;
    }
    taken = (count != 0);
    if (opcode == 0xE0) { // loopne
      taken = taken && !this->regs.read_flag(Regs::ZF);
    } else if (opcode == 0xE1) { // loope
      taken = taken && this->regs.read_flag(Regs::ZF);
    }
  }
  if (taken) {
    // A target beyond the CS limit faults with the counter not yet decremented (faults are restartable).
    this->set_eip_near(this->regs.eip + offset);
  }
  if (opcode != 0xE3) {
    if (this->overrides.address_size) {
      this->regs.w_cx(count);
    } else {
      this->regs.w_ecx(count);
    }
  }
}

void X86Emulator::exec_E8_E9_call_jmp(uint8_t opcode) {
  uint32_t offset = this->overrides.operand_size ? static_cast<uint32_t>(static_cast<int16_t>(this->fetch_instruction_word()))
                                                 : this->fetch_instruction_dword();
  uint32_t target = this->regs.eip + offset;
  if (!(opcode & 1)) {
    if (this->overrides.operand_size) {
      this->push_g<uint16_t>(this->regs.eip);
    } else {
      this->push_g<uint32_t>(this->regs.eip);
    }
  }
  this->set_eip_near(target);
}

void X86Emulator::exec_EB_jmp(uint8_t) {
  uint32_t offset = static_cast<int8_t>(this->fetch_instruction_byte());
  this->set_eip_near(this->regs.eip + offset);
}

void X86Emulator::exec_C2_C3_CA_CB_ret(uint8_t opcode) {
  uint16_t release = (opcode & 1) ? 0 : this->fetch_instruction_word();
  if (opcode & 8) {
    this->far_return(!this->overrides.operand_size, release);
    return;
  }
  uint32_t new_eip = this->overrides.operand_size ? this->pop_g<uint16_t>() : this->pop_g<uint32_t>();
  this->set_eip_near(new_eip);
  if (release) {
    if (this->stack16) {
      this->regs.w_sp(this->regs.r_sp() + release);
    } else {
      this->regs.w_esp(this->regs.r_esp() + release);
    }
  }
}

void X86Emulator::far_call(uint16_t sel, uint32_t off, bool op32) {
  // Validate the target first, push the return address (which may fault), commit CS:EIP last: a fault anywhere
  // leaves CS untouched and the run loop rolls ESP back.
  SegDesc d = this->check_code_segment(sel, off);
  uint16_t old_cs = this->segs[SEG_CS].sel;
  if (op32) {
    this->push_g<uint32_t>(old_cs);
    this->push_g<uint32_t>(this->regs.eip);
  } else {
    this->push_g<uint16_t>(old_cs);
    this->push_g<uint16_t>(this->regs.eip);
  }
  this->commit_cs(sel, d, off);
}

void X86Emulator::far_jump(uint16_t sel, uint32_t off) {
  SegDesc d = this->check_code_segment(sel, off);
  this->commit_cs(sel, d, off);
}

void X86Emulator::far_return(bool op32, uint16_t release) {
  uint32_t off;
  uint16_t sel;
  if (op32) {
    off = this->pop_g<uint32_t>();
    sel = this->pop_g<uint32_t>();
  } else {
    off = this->pop_g<uint16_t>();
    sel = this->pop_g<uint16_t>();
  }
  SegDesc d = this->check_code_segment(sel, off);
  if (release) {
    if (this->stack16) {
      this->regs.w_sp(this->regs.r_sp() + release);
    } else {
      this->regs.w_esp(this->regs.r_esp() + release);
    }
  }
  this->commit_cs(sel, d, off);
}

void X86Emulator::exec_9A_call_far(uint8_t) {
  uint32_t off = this->fetch_instruction_imm();
  uint16_t sel = this->fetch_instruction_word();
  this->far_call(sel, off, !this->overrides.operand_size);
}

void X86Emulator::exec_EA_jmp_far(uint8_t) {
  uint32_t off = this->fetch_instruction_imm();
  uint16_t sel = this->fetch_instruction_word();
  this->far_jump(sel, off);
}

void X86Emulator::exec_CF_iret(uint8_t) {
  // Same-privilege return (the host runs all guest code at one privilege level).
  uint32_t off, flags;
  uint16_t sel;
  if (this->overrides.operand_size) {
    off = this->pop_g<uint16_t>();
    sel = this->pop_g<uint16_t>();
    flags = this->pop_g<uint16_t>();
  } else {
    off = this->pop_g<uint32_t>();
    sel = this->pop_g<uint32_t>();
    flags = this->pop_g<uint32_t>();
  }
  SegDesc d = this->check_code_segment(sel, off);
  uint32_t mask = this->overrides.operand_size ? 0x00004DD5 : 0x00244DD5;
  this->regs.write_eflags(((this->regs.read_eflags() & ~mask) | (flags & mask)) | 0x2);
  this->commit_cs(sel, d, off);
}

void X86Emulator::exec_FE_FF_inc_dec_misc(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;

  if (!(opcode & 1)) {
    if (what > 1) {
      raise(VEC_UD, 0, "FE /2-7");
    }
    this->rmw_ea<uint8_t>(rm, [&](uint8_t v) {
      return (what == 0) ? this->regs.set_flags_integer_add<uint8_t>(v, 1, Regs::default_int_flags & ~Regs::CF)
                         : this->regs.set_flags_integer_subtract<uint8_t>(v, 1, Regs::default_int_flags & ~Regs::CF);
    });
    return;
  }

  switch (what) {
    case 0: // inc
    case 1: // dec
      if (this->overrides.operand_size) {
        this->rmw_ea<uint16_t>(rm, [&](uint16_t v) {
          return (what == 0) ? this->regs.set_flags_integer_add<uint16_t>(v, 1, Regs::default_int_flags & ~Regs::CF)
                             : this->regs.set_flags_integer_subtract<uint16_t>(v, 1, Regs::default_int_flags & ~Regs::CF);
        });
      } else {
        this->rmw_ea<uint32_t>(rm, [&](uint32_t v) {
          return (what == 0) ? this->regs.set_flags_integer_add<uint32_t>(v, 1, Regs::default_int_flags & ~Regs::CF)
                             : this->regs.set_flags_integer_subtract<uint32_t>(v, 1, Regs::default_int_flags & ~Regs::CF);
        });
      }
      break;
    case 2: { // call near r/m (the target is read before the return address is pushed)
      uint32_t target = this->overrides.operand_size ? this->read_ea<uint16_t>(rm) : this->read_ea<uint32_t>(rm);
      if (this->overrides.operand_size) {
        this->push_g<uint16_t>(this->regs.eip);
      } else {
        this->push_g<uint32_t>(this->regs.eip);
      }
      this->set_eip_near(target);
      break;
    }
    case 4: { // jmp near r/m
      uint32_t target = this->overrides.operand_size ? this->read_ea<uint16_t>(rm) : this->read_ea<uint32_t>(rm);
      this->set_eip_near(target);
      break;
    }
    case 3: // call far m16:16 / m16:32
    case 5: { // jmp far
      if (!rm.has_mem_ref()) {
        raise(VEC_UD, 0, "far call/jmp through a register");
      }
      uint32_t off_addr = this->resolve_mem_ea(rm);
      uint32_t size = this->overrides.operand_size ? 4 : 6;
      uint32_t addr = this->lin_data(rm.seg, off_addr, size, false);
      uint32_t off = this->overrides.operand_size ? this->r_mem<uint16_t>(addr) : this->r_mem<uint32_t>(addr);
      uint16_t sel = this->r_mem<uint16_t>(addr + size - 2);
      if (what == 3) {
        this->far_call(sel, off, !this->overrides.operand_size);
      } else {
        this->far_jump(sel, off);
      }
      break;
    }
    case 6: // push r/m
      if (this->overrides.operand_size) {
        this->push_g<uint16_t>(this->read_ea<uint16_t>(rm));
      } else {
        this->push_g<uint32_t>(this->read_ea<uint32_t>(rm));
      }
      break;
    default:
      raise(VEC_UD, 0, "FF /7");
  }
}

// ---- Interrupts and traps -------------------------------------------------------------------------------------------

void X86Emulator::exec_CC_CD_int(uint8_t opcode) {
  uint8_t int_num = (opcode & 1) ? this->fetch_instruction_byte() : 3;
  if (!this->syscall_handler) {
    // Protected mode with no gate for the vector: #GP with an IDT-flavored error code.
    raise(VEC_GP, (int_num << 3) | 2, "int n with no interrupt handler installed");
  }
  this->syscall_handler(*this, int_num);
}

void X86Emulator::exec_CE_into(uint8_t) {
  if (this->regs.read_flag(Regs::OF)) {
    if (!this->syscall_handler) {
      raise(VEC_GP, (4 << 3) | 2, "into with no interrupt handler installed");
    }
    this->syscall_handler(*this, VEC_OF);
  }
}

void X86Emulator::exec_F1_icebp(uint8_t) {
  if (!this->syscall_handler) {
    raise(VEC_GP, (1 << 3) | 2, "icebp with no interrupt handler installed");
  }
  this->syscall_handler(*this, VEC_DB);
}

void X86Emulator::exec_F4_hlt(uint8_t) {
  raise(VEC_GP, 0, "hlt is privileged");
}

// ---- Segment registers ----------------------------------------------------------------------------------------------

void X86Emulator::exec_06_0E_16_1E_0FA0_0FA8_push_segment_reg(uint8_t opcode) {
  uint8_t seg;
  switch (opcode) {
    case 0x06:
      seg = SEG_ES;
      break;
    case 0x0E:
      seg = SEG_CS;
      break;
    case 0x16:
      seg = SEG_SS;
      break;
    case 0x1E:
      seg = SEG_DS;
      break;
    case 0xA0:
      seg = SEG_FS;
      break;
    default: // 0xA8
      seg = SEG_GS;
      break;
  }
  if (this->overrides.operand_size) {
    this->push_g<uint16_t>(this->segs[seg].sel);
  } else {
    // A 32-bit push of a segment register writes only the low word on current processors; we write the selector
    // zero-extended, which Windows code cannot tell apart.
    this->push_g<uint32_t>(this->segs[seg].sel);
  }
}

void X86Emulator::exec_07_17_1F_0FA1_0FA9_pop_segment_reg(uint8_t opcode) {
  uint8_t seg;
  switch (opcode) {
    case 0x07:
      seg = SEG_ES;
      break;
    case 0x17:
      seg = SEG_SS;
      break;
    case 0x1F:
      seg = SEG_DS;
      break;
    case 0xA1:
      seg = SEG_FS;
      break;
    default: // 0xA9
      seg = SEG_GS;
      break;
  }
  // A faulting load leaves ESP alone: the run loop rolls ESP back with EIP.
  uint16_t sel = this->overrides.operand_size ? this->pop_g<uint16_t>() : this->pop_g<uint32_t>();
  this->load_data_segment(seg, sel);
}

void X86Emulator::exec_8C_mov_rm_sreg(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if (rm.non_ea_reg > 5) {
    raise(VEC_UD, 0, "mov r/m, Sreg with an invalid segment register");
  }
  uint16_t sel = this->segs[rm.non_ea_reg].sel;
  if (rm.has_mem_ref() || this->overrides.operand_size) {
    // Memory destinations are always 16 bits wide.
    this->write_ea<uint16_t>(rm, sel);
  } else {
    this->regs.write32(rm.ea_reg, sel);
  }
}

void X86Emulator::exec_8E_mov_sreg_rm(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  if ((rm.non_ea_reg == SEG_CS) || (rm.non_ea_reg > 5)) {
    raise(VEC_UD, 0, "mov Sreg, r/m with CS or an invalid segment register");
  }
  this->load_data_segment(rm.non_ea_reg, this->read_ea<uint16_t>(rm));
}

void X86Emulator::exec_C4_C5_les_lds(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (!rm.has_mem_ref()) {
    raise(VEC_UD, 0, "les/lds with a register operand");
  }
  uint32_t size = this->overrides.operand_size ? 4 : 6;
  uint32_t addr = this->lin_data(rm.seg, this->resolve_mem_ea(rm), size, false);
  uint32_t off = this->overrides.operand_size ? this->r_mem<uint16_t>(addr) : this->r_mem<uint32_t>(addr);
  uint16_t sel = this->r_mem<uint16_t>(addr + size - 2);
  // The segment register is loaded first, so a faulting load leaves the general register untouched.
  this->load_data_segment((opcode & 1) ? SEG_DS : SEG_ES, sel);
  if (this->overrides.operand_size) {
    this->w_non_ea16(rm, off);
  } else {
    this->w_non_ea32(rm, off);
  }
}

void X86Emulator::exec_0F_B2_B4_B5_lss_lfs_lgs(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  if (!rm.has_mem_ref()) {
    raise(VEC_UD, 0, "lss/lfs/lgs with a register operand");
  }
  uint32_t size = this->overrides.operand_size ? 4 : 6;
  uint32_t addr = this->lin_data(rm.seg, this->resolve_mem_ea(rm), size, false);
  uint32_t off = this->overrides.operand_size ? this->r_mem<uint16_t>(addr) : this->r_mem<uint32_t>(addr);
  uint16_t sel = this->r_mem<uint16_t>(addr + size - 2);
  uint8_t seg = (opcode == 0xB2) ? SEG_SS : ((opcode == 0xB4) ? SEG_FS : SEG_GS);
  this->load_data_segment(seg, sel);
  if (this->overrides.operand_size) {
    this->w_non_ea16(rm, off);
  } else {
    this->w_non_ea32(rm, off);
  }
}

void X86Emulator::exec_63_arpl(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  uint16_t src_rpl = this->r_non_ea16(rm) & 3;
  bool adjusted = false;
  this->rmw_ea<uint16_t>(rm, [&](uint16_t dest) -> uint16_t {
    if ((dest & 3) < src_rpl) {
      adjusted = true;
      return (dest & ~3) | src_rpl;
    }
    return dest;
  });
  this->regs.replace_flag(Regs::ZF, adjusted);
}

// Access-rights and limit views of a descriptor, in the layout LAR/LSL report (the high dword of a GDT/LDT entry).
static uint32_t descriptor_high_dword(const SegDesc& d) {
  uint32_t access = 0x10 | 0x01; // S (code/data) and Accessed
  access |= d.present ? 0x80 : 0;
  access |= (d.dpl & 3) << 5;
  if (d.code) {
    access |= 0x08 | (d.readable_or_writable ? 0x02 : 0);
  } else {
    access |= d.readable_or_writable ? 0x02 : 0;
  }
  bool granular = d.limit > 0xFFFFF;
  uint32_t encoded_limit = granular ? (d.limit >> 12) : d.limit;
  uint32_t ret = (access << 8) | (encoded_limit & 0x000F0000);
  ret |= d.big ? 0x00400000 : 0;
  ret |= granular ? 0x00800000 : 0;
  return ret;
}

void X86Emulator::exec_0F_02_03_lar_lsl(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint16_t sel = this->read_ea<uint16_t>(rm);
  SegDesc d;
  // Privilege checks are not modeled: the host runs all guest code at one level.
  if (is_null_selector(sel) || !this->lookup_descriptor(sel, d)) {
    this->regs.replace_flag(Regs::ZF, false);
    return;
  }
  uint32_t value = (opcode & 1) ? d.limit : (descriptor_high_dword(d) & 0x00FFFF00);
  if (this->overrides.operand_size) {
    this->w_non_ea16(rm, (opcode & 1) ? value : (value & 0xFF00));
  } else {
    this->w_non_ea32(rm, value);
  }
  this->regs.replace_flag(Regs::ZF, true);
}

void X86Emulator::exec_0F_00_grp6(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  switch (rm.non_ea_reg) {
    case 0: // sldt: there is one (host-owned) LDT; report a plausible selector
    case 1: // str
      if (rm.has_mem_ref() || this->overrides.operand_size) {
        this->write_ea<uint16_t>(rm, (rm.non_ea_reg == 0) ? 0x0000 : 0x0028);
      } else {
        this->regs.write32(rm.ea_reg, (rm.non_ea_reg == 0) ? 0x0000 : 0x0028);
      }
      break;
    case 4: // verr
    case 5: { // verw
      uint16_t sel = this->read_ea<uint16_t>(rm);
      SegDesc d;
      bool ok = !is_null_selector(sel) && this->lookup_descriptor(sel, d);
      if (ok) {
        ok = (rm.non_ea_reg == 4) ? (!d.code || d.readable_or_writable) : (!d.code && d.readable_or_writable);
      }
      this->regs.replace_flag(Regs::ZF, ok);
      break;
    }
    default: // lldt, ltr
      raise(VEC_GP, 0, "lldt/ltr are privileged");
  }
}

void X86Emulator::exec_0F_01_grp7(uint8_t) {
  auto rm = this->fetch_and_decode_rm();
  switch (rm.non_ea_reg) {
    case 0: // sgdt
    case 1: { // sidt
      if (!rm.has_mem_ref()) {
        raise(VEC_UD, 0, "sgdt/sidt with a register operand");
      }
      // Report Windows-95-looking tables; nothing can use them from ring 3.
      uint32_t addr = this->ea_linear(rm, 6, true);
      this->w_mem<uint16_t>(addr, (rm.non_ea_reg == 0) ? 0x0FFF : 0x07FF);
      this->w_mem<uint32_t>(addr + 2, (rm.non_ea_reg == 0) ? 0x80001000 : 0x80002000);
      break;
    }
    case 4: // smsw: PE, MP, ET, NE
      if (rm.has_mem_ref() || this->overrides.operand_size) {
        this->write_ea<uint16_t>(rm, 0x0033);
      } else {
        this->regs.write32(rm.ea_reg, 0x80050033);
      }
      break;
    default: // lgdt, lidt, lmsw, invlpg
      raise(VEC_GP, 0, "privileged 0F 01 instruction");
  }
}

void X86Emulator::exec_0F_0B_ud2(uint8_t) {
  raise(VEC_UD, 0, "ud2");
}

// ---- Strings --------------------------------------------------------------------------------------------------------

template <typename T, bool Addr16>
void X86Emulator::exec_string_op_logic(uint8_t opcode) {
  // BYTES = OPCODE = SOURCE          = DESTINATION
  // A4/A5 = movs   = seg:[esi]       -> es:[edi]
  // A6/A7 = cmps   = seg:[esi] cmp es:[edi]
  // AA/AB = stos   = al/ax/eax       -> es:[edi]
  // AC/AD = lods   = seg:[esi]       -> al/ax/eax
  // AE/AF = scas   = al/ax/eax cmp es:[edi]
  // (seg is DS unless overridden; ES:[edi] cannot be overridden.)
  uint32_t esi = Addr16 ? this->regs.r_si() : this->regs.r_esi();
  uint32_t edi = Addr16 ? this->regs.r_di() : this->regs.r_edi();
  uint32_t delta = this->regs.read_flag(Regs::DF) ? static_cast<uint32_t>(-static_cast<int32_t>(sizeof(T))) : sizeof(T);
  bool advance_esi = true;
  bool advance_edi = true;

  switch (opcode & 0x0E) {
    case 0x04: { // movs
      T v = this->r_mem<T>(this->lin_data(this->data_seg(), esi, sizeof(T), false));
      this->w_mem<T>(this->lin_data(SEG_ES, edi, sizeof(T), true), v);
      break;
    }
    case 0x06: { // cmps
      T a = this->r_mem<T>(this->lin_data(this->data_seg(), esi, sizeof(T), false));
      T b = this->r_mem<T>(this->lin_data(SEG_ES, edi, sizeof(T), false));
      this->regs.set_flags_integer_subtract<T>(a, b);
      break;
    }
    case 0x0A: // stos
      this->w_mem<T>(this->lin_data(SEG_ES, edi, sizeof(T), true), static_cast<T>(this->regs.r_eax()));
      advance_esi = false;
      break;
    case 0x0C: { // lods
      T v = this->r_mem<T>(this->lin_data(this->data_seg(), esi, sizeof(T), false));
      this->regs.write<T>(0, v);
      advance_edi = false;
      break;
    }
    case 0x0E: { // scas
      T b = this->r_mem<T>(this->lin_data(SEG_ES, edi, sizeof(T), false));
      this->regs.set_flags_integer_subtract<T>(static_cast<T>(this->regs.r_eax()), b);
      advance_esi = false;
      break;
    }
    default:
      raise(VEC_UD, 0, "invalid string opcode");
  }

  if (advance_edi) {
    if (Addr16) {
      this->regs.w_di(edi + delta);
    } else {
      this->regs.w_edi(edi + delta);
    }
  }
  if (advance_esi) {
    if (Addr16) {
      this->regs.w_si(esi + delta);
    } else {
      this->regs.w_esi(esi + delta);
    }
  }
}

template <typename T, bool Addr16>
void X86Emulator::exec_rep_string_op_logic(uint8_t opcode) {
  // Each iteration updates (E)SI/(E)DI/(E)CX before the next one, so a fault mid-string is restartable exactly as
  // on hardware (EIP stays on the rep instruction).
  auto count = [&]() -> uint32_t { return Addr16 ? this->regs.r_cx() : this->regs.r_ecx(); };
  auto set_count = [&](uint32_t c) {
    if (Addr16) {
      this->regs.w_cx(c);
    } else {
      this->regs.w_ecx(c);
    }
  };

  uint8_t what = opcode & 0x0E;
  if ((what == 0x06) || (what == 0x0E)) { // cmps/scas: repe/repne
    bool expected_zf = this->overrides.repeat_z;
    for (uint32_t c = count(); c; c = count()) {
      this->exec_string_op_logic<T, Addr16>(opcode);
      set_count(c - 1);
      if (this->regs.read_flag(Regs::ZF) != expected_zf) {
        break;
      }
    }
    return;
  }

  // Fast paths for the flat, forward, non-overlapping movs/stos that blitters and memset/memcpy run: one host
  // memmove/fill instead of an interpreted iteration per element.
  if (!Addr16 && !this->regs.read_flag(Regs::DF) && this->segs[SEG_ES].identity &&
      ((what == 0x0A) || (this->segs[this->data_seg()].identity))) {
    uint32_t c = count();
    uint64_t bytes = static_cast<uint64_t>(c) * sizeof(T);
    uint32_t edi = this->regs.r_edi();
    if (c && (bytes <= 0x10000000) && (static_cast<uint64_t>(edi) + bytes <= 0x100000000ULL)) {
      if (what == 0x04) {
        uint32_t esi = this->regs.r_esi();
        // Overlap with the destination ahead of the source replicates data element by element; only take the
        // fast path when a forward memmove gives the same result.
        if ((static_cast<uint64_t>(esi) + bytes <= 0x100000000ULL) &&
            ((edi <= esi) || (edi >= esi + bytes)) && this->mem->exists(esi, bytes) && this->mem->exists(edi, bytes)) {
          uint8_t* dst = this->mem->at<uint8_t>(edi, bytes);
          const uint8_t* src = this->mem->at<uint8_t>(esi, bytes);
          ::memmove(dst, src, bytes);
          this->mem->note_write(edi, bytes);
          this->regs.w_esi(esi + bytes);
          this->regs.w_edi(edi + bytes);
          this->regs.w_ecx(0);
          return;
        }
      } else if (what == 0x0A) {
        if (this->mem->exists(edi, bytes)) {
          uint8_t* dst = this->mem->at<uint8_t>(edi, bytes);
          T v = static_cast<T>(this->regs.r_eax());
          if (sizeof(T) == 1) {
            ::memset(dst, v, bytes);
          } else {
            for (uint32_t z = 0; z < c; z++) {
              ::memcpy(dst + z * sizeof(T), &v, sizeof(T));
            }
          }
          this->mem->note_write(edi, bytes);
          this->regs.w_edi(edi + bytes);
          this->regs.w_ecx(0);
          return;
        }
      }
    }
  }

  for (uint32_t c = count(); c; c = count()) {
    this->exec_string_op_logic<T, Addr16>(opcode);
    set_count(c - 1);
  }
}

template <typename T>
void X86Emulator::exec_string_op_dispatch(uint8_t opcode) {
  bool rep = this->overrides.repeat_nz || this->overrides.repeat_z;
  if (this->overrides.address_size) {
    if (rep) {
      this->exec_rep_string_op_logic<T, true>(opcode);
    } else {
      this->exec_string_op_logic<T, true>(opcode);
    }
  } else {
    if (rep) {
      this->exec_rep_string_op_logic<T, false>(opcode);
    } else {
      this->exec_string_op_logic<T, false>(opcode);
    }
  }
}

void X86Emulator::exec_A4_to_A7_AA_to_AF_string_ops(uint8_t opcode) {
  if (!(opcode & 1)) {
    this->exec_string_op_dispatch<uint8_t>(opcode);
  } else if (this->overrides.operand_size) {
    this->exec_string_op_dispatch<uint16_t>(opcode);
  } else {
    this->exec_string_op_dispatch<uint32_t>(opcode);
  }
}

// ---- Port I/O -------------------------------------------------------------------------------------------------------

void X86Emulator::exec_E4_E5_EC_ED_in(uint8_t opcode) {
  uint16_t port = (opcode & 8) ? this->regs.r_dx() : this->fetch_instruction_byte();
  if (!this->port_handler) {
    raise(VEC_GP, 0, "port I/O (in) with no port handler");
  }
  uint8_t size = (opcode & 1) ? (this->overrides.operand_size ? 2 : 4) : 1;
  uint32_t v = this->port_handler(*this, port, size, false, 0);
  if (size == 1) {
    this->regs.w_al(v);
  } else if (size == 2) {
    this->regs.w_ax(v);
  } else {
    this->regs.w_eax(v);
  }
}

void X86Emulator::exec_E6_E7_EE_EF_out(uint8_t opcode) {
  uint16_t port = (opcode & 8) ? this->regs.r_dx() : this->fetch_instruction_byte();
  if (!this->port_handler) {
    raise(VEC_GP, 0, "port I/O (out) with no port handler");
  }
  uint8_t size = (opcode & 1) ? (this->overrides.operand_size ? 2 : 4) : 1;
  uint32_t v = (size == 1) ? this->regs.r_al() : ((size == 2) ? this->regs.r_ax() : this->regs.r_eax());
  this->port_handler(*this, port, size, true, v);
}

void X86Emulator::exec_6C_to_6F_ins_outs(uint8_t) {
  raise(VEC_GP, 0, "string port I/O is not supported");
}

// ---- Prefixes -------------------------------------------------------------------------------------------------------

void X86Emulator::exec_26_es(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.segment = Segment::ES;
}

void X86Emulator::exec_2E_cs(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.segment = Segment::CS;
}

void X86Emulator::exec_36_ss(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.segment = Segment::SS;
}

void X86Emulator::exec_3E_ds(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.segment = Segment::DS;
}

void X86Emulator::exec_64_fs(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.segment = Segment::FS;
}

void X86Emulator::exec_65_gs(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.segment = Segment::GS;
}

void X86Emulator::exec_66_operand_size(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.operand_size = !this->overrides.code16;
}

void X86Emulator::exec_67_address_size(uint8_t) {
  this->overrides.should_clear = false;
  this->overrides.address_size = !this->overrides.code16;
}

void X86Emulator::exec_F0_lock(uint8_t) {
  // Single-threaded: every access is already atomic.
  this->overrides.should_clear = false;
  this->overrides.lock = true;
}

void X86Emulator::exec_F2_F3_repz_repnz(uint8_t opcode) {
  // With several rep prefixes the last one wins.
  this->overrides.should_clear = false;
  this->overrides.repeat_z = (opcode & 1);
  this->overrides.repeat_nz = !this->overrides.repeat_z;
}

// ---- Flags ----------------------------------------------------------------------------------------------------------

void X86Emulator::exec_F5_cmc(uint8_t) {
  this->regs.replace_flag(Regs::CF, !this->regs.read_flag(Regs::CF));
}

void X86Emulator::exec_F8_clc(uint8_t) {
  this->regs.replace_flag(Regs::CF, false);
}

void X86Emulator::exec_F9_stc(uint8_t) {
  this->regs.replace_flag(Regs::CF, true);
}

void X86Emulator::exec_FA_cli(uint8_t) {
  // Win16 code may bracket stack switches with cli/sti; with no interrupts to hold off, only the flag changes.
  this->regs.replace_flag(Regs::IF, false);
}

void X86Emulator::exec_FB_sti(uint8_t) {
  this->regs.replace_flag(Regs::IF, true);
}

void X86Emulator::exec_FC_cld(uint8_t) {
  this->regs.replace_flag(Regs::DF, false);
}

void X86Emulator::exec_FD_std(uint8_t) {
  this->regs.replace_flag(Regs::DF, true);
}

// ---- Misc 0F --------------------------------------------------------------------------------------------------------

void X86Emulator::exec_0F_extensions(uint8_t) {
  uint8_t opcode = this->fetch_instruction_byte();
  auto fn = this->fns_0F[opcode].exec;
  if (fn) {
    (this->*fn)(opcode);
  } else {
    this->exec_0F_unimplemented(opcode);
  }
}

void X86Emulator::exec_0F_18_to_1F_prefetch_or_nop(uint8_t) {
  this->fetch_and_decode_rm();
}

void X86Emulator::exec_0F_31_rdtsc(uint8_t) {
  uint64_t res;
  if (this->tsc_overrides.empty()) {
    res = this->instructions_executed + this->tsc_offset;
  } else {
    res = this->tsc_overrides.front();
    this->tsc_overrides.pop_front();
  }
  this->regs.w_edx(res >> 32);
  this->regs.w_eax(res);
}

void X86Emulator::exec_0F_A2_cpuid(uint8_t) {
  // A Pentium-class part (as far as 1996 code can tell): family 5, FPU + TSC + CX8 + CMOV.
  switch (this->regs.r_eax()) {
    case 0:
      this->regs.w_eax(1);
      this->regs.w_ebx(0x756E6547); // "GenuineIntel"
      this->regs.w_edx(0x49656E69);
      this->regs.w_ecx(0x6C65746E);
      break;
    case 1:
      if (this->behavior == Behavior::WINDOWS_ARM_EMULATOR) {
        this->regs.w_eax(0x00000F4A);
        this->regs.w_ecx(0x02880203);
        this->regs.w_edx(0x17808111);
        this->regs.w_ebx(0x00040000);
      } else {
        this->regs.w_eax(0x00000543);
        this->regs.w_ebx(0);
        this->regs.w_ecx(0);
        this->regs.w_edx(0x00008111); // FPU, TSC, CX8, CMOV
      }
      break;
    default:
      this->regs.w_eax(0);
      this->regs.w_ebx(0);
      this->regs.w_ecx(0);
      this->regs.w_edx(0);
      break;
  }
}

// Upstream's SSE moves (unused by 1996 code, kept for completeness).
void X86Emulator::exec_0F_10_11_mov_xmm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();

  if (opcode & 1) { // xmm/mem <- xmm
    if (this->overrides.repeat_z) { // movss
      this->w_ea_xmm32(rm, this->r_non_ea_xmm32(rm));
    } else if (this->overrides.repeat_nz) { // movsd
      this->w_ea_xmm64(rm, this->r_non_ea_xmm64(rm));
    } else { // movups/movupd
      this->w_ea_xmm128(rm, this->r_non_ea_xmm128(rm));
    }
  } else { // xmm <- xmm/mem
    if (rm.has_mem_ref()) {
      this->w_non_ea_xmm128(rm, Regs::XMMReg());
    }
    if (this->overrides.repeat_z) { // movss
      this->w_non_ea_xmm32(rm, this->r_ea_xmm32(rm));
    } else if (this->overrides.repeat_nz) { // movsd
      this->w_non_ea_xmm64(rm, this->r_ea_xmm64(rm));
    } else { // movups/movupd
      this->w_non_ea_xmm128(rm, this->r_ea_xmm128(rm));
    }
  }
}

void X86Emulator::exec_0F_7E_7F_mov_xmm(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();

  if (this->overrides.repeat_nz) {
    raise(VEC_UD, 0, "0F 7E/7F with F2");
  }

  if (opcode & 1) { // all xmm/mem <- xmm
    if (this->overrides.repeat_z || this->overrides.operand_size) { // movdqu/movdqa
      this->w_ea_xmm128(rm, this->r_non_ea_xmm128(rm));
    } else { // movq
      raise(VEC_UD, 0, "mm registers are not supported");
    }
  } else { // all xmm/mem <- xmm EXCEPT for movq, which is the opposite
    this->regs.xmm128(rm.non_ea_reg).clear();
    if (this->overrides.repeat_z) { // movq
      this->w_non_ea_xmm64(rm, this->r_ea_xmm64(rm));
    } else { // movd
      this->w_non_ea_xmm32(rm, this->r_ea_xmm32(rm));
    }
  }
}

void X86Emulator::exec_0F_D6_movq_variants(uint8_t) {
  auto rm = this->fetch_and_decode_rm();

  if (!this->overrides.operand_size || this->overrides.repeat_z || this->overrides.repeat_nz) {
    raise(VEC_UD, 0, "mm registers are not supported");
  }

  if (!rm.has_mem_ref()) {
    this->w_ea_xmm128(rm, Regs::XMMReg());
  }
  this->w_ea_xmm64(rm, this->r_non_ea_xmm64(rm));
}

void X86Emulator::exec_unimplemented(uint8_t) {
  raise(VEC_UD, 0, "invalid or unimplemented opcode");
}

void X86Emulator::exec_0F_unimplemented(uint8_t) {
  raise(VEC_UD, 0, "invalid or unimplemented 0F opcode");
}

} // namespace adw::cpu
