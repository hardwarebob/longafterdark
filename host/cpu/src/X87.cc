// The x87 FPU for adw::cpu::X86Emulator (Long After Dark; not part of the upstream resource_dasm code, which only
// disassembled D8-DF).
//
// Guest arithmetic runs on the host's own x87 unit: each operation loads the guest's control word (precision and
// rounding control, every exception masked) and condition codes, then the guest's 80-bit operands, executes the same
// instruction (with m32real/m64real operands in place), and reads back the result and the status word. That makes precision control, rounding, denormals, NaN propagation,
// the C0-C3 condition codes and the transcendentals (fsin, fpatan, f2xm1, fyl2x, ...) bit-identical to what the
// original code computed on real hardware, which a software model could only approximate. The host must therefore be
// x86 (x86-64 Windows is the only target); the stack, tags, TOP, sticky flags, and unmasked-exception behavior are
// modeled here.
//
// Unmasked exceptions: the flags are raised, ES/B are set and the next waiting x87 instruction (or fwait) reports
// #MF to the fault handler. For #IA/#Z/#D the destination is left unmodified, as on hardware; so is a memory
// destination for #O/#U. A register destination with an unmasked #O/#U gets the masked (unscaled) result instead of
// the hardware's exponent-biased one, a documented deviation (it takes an 80-bit overflow to reach it).

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "X86Emulator.hh"

#if !defined(__x86_64__) && !defined(__i386__)
#error "the adw::cpu x87 unit executes guest FPU operations on the host x87 and needs an x86 host"
#endif

namespace adw::cpu {

namespace {

using FReg = X86Emulator::FPUState::Reg;

// Status word bits.
constexpr uint16_t SW_IE = 0x0001;
constexpr uint16_t SW_DE = 0x0002;
constexpr uint16_t SW_ZE = 0x0004;
constexpr uint16_t SW_OE = 0x0008;
constexpr uint16_t SW_UE = 0x0010;
constexpr uint16_t SW_PE = 0x0020;
constexpr uint16_t SW_SF = 0x0040;
constexpr uint16_t SW_ES = 0x0080;
constexpr uint16_t SW_C0 = 0x0100;
constexpr uint16_t SW_C1 = 0x0200;
constexpr uint16_t SW_C2 = 0x0400;
constexpr uint16_t SW_C3 = 0x4000;
constexpr uint16_t SW_B = 0x8000;
constexpr uint16_t SW_CC = SW_C0 | SW_C1 | SW_C2 | SW_C3;
constexpr uint16_t SW_EXC = 0x003F;

// An 80-bit value in the layout fldt/fstpt use.
struct alignas(16) Ext {
  uint64_t mant = 0;
  uint16_t se = 0;
  uint16_t pad[3] = {0, 0, 0};
};

inline Ext to_ext(const FReg& r) {
  Ext e;
  e.mant = r.mant;
  e.se = r.se;
  return e;
}
inline FReg to_reg(const Ext& e) {
  return FReg{e.mant, e.se};
}

// The "real indefinite" QNaN every masked invalid operation produces.
constexpr FReg INDEFINITE{0xC000000000000000ULL, 0xFFFF};

// ---- Host x87 primitives --------------------------------------------------------------------------------------------
// Every primitive: save the host control word, load an environment carrying the guest's control word (all exceptions
// masked) and the guest's condition codes (no exceptions raised, empty stack), run, capture the status word
// immediately after the operation (later stores would change C1), restore the host control word.
//
// Seeding the host with the guest's C0-C3 matters: many instructions leave some condition codes "undefined", which
// real hardware implements as "unchanged". Starting from the guest's codes, the host status word after the operation
// is exactly what the guest's would be, whatever the instruction defines; starting from the host's leftovers (as a
// plain fnclex + fldcw did), stale host condition codes leaked into the guest.

// The 28-byte protected-mode environment fldenv takes (the same layout in 64-bit mode without REX.W).
struct HostEnv {
  uint32_t cw, sw, tw, fip, fcs_fop, fdp, fds;
};
static_assert(sizeof(HostEnv) == 28, "fldenv image is 28 bytes");

#define X87_PRE "fnstcw %[saved]\n\tfldenv %[env]\n\t"
#define X87_POST "fldcw %[saved]\n\t"

// st0 = f(st0)
#define DEF_UNARY(NAME, INSN)                                                                                    \
  uint16_t NAME(const HostEnv& env, Ext& a) {                                                                    \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[a]\n\t" INSN "\n\tfnstsw %[sw]\n\tfstpt %[a]\n\t" X87_POST                      \
        : [a] "+m"(a), [sw] "=m"(sw), [saved] "=m"(saved)                                                        \
        : [env] "m"(env)                                                                                         \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
DEF_UNARY(h_fsqrt, "fsqrt")
DEF_UNARY(h_fsin, "fsin")
DEF_UNARY(h_fcos, "fcos")
DEF_UNARY(h_frndint, "frndint")
DEF_UNARY(h_f2xm1, "f2xm1")

// st0 = st0 op st1 (a = st0, b = st1); b is discarded.
#define DEF_BINARY(NAME, INSN)                                                                                   \
  uint16_t NAME(const HostEnv& env, Ext& a, const Ext& b) {                                                      \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[b]\n\tfldt %[a]\n\t" INSN "\n\tfnstsw %[sw]\n\tfstpt %[a]\n\tfstp %%st(0)\n\t"  \
        X87_POST                                                                                                 \
        : [a] "+m"(a), [sw] "=m"(sw), [saved] "=m"(saved)                                                        \
        : [b] "m"(b), [env] "m"(env)                                                                             \
        : "st", "st(1)", "memory");                                                                              \
    return sw;                                                                                                   \
  }
// With st(0) as the destination, AT&T and Intel operand orders agree: "fsub %st(1), %st" is st0 = st0 - st1.
DEF_BINARY(h_fadd, "fadd %%st(1), %%st")
DEF_BINARY(h_fmul, "fmul %%st(1), %%st")
DEF_BINARY(h_fsub, "fsub %%st(1), %%st")
DEF_BINARY(h_fsubr, "fsubr %%st(1), %%st")
DEF_BINARY(h_fdiv, "fdiv %%st(1), %%st")
DEF_BINARY(h_fdivr, "fdivr %%st(1), %%st")
DEF_BINARY(h_fscale, "fscale")
DEF_BINARY(h_fprem, "fprem")
DEF_BINARY(h_fprem1, "fprem1")

// st1 = f(st1, st0), pop: a = st0, b = st1, result in b.
#define DEF_BINARY_POP(NAME, INSN)                                                                               \
  uint16_t NAME(const HostEnv& env, const Ext& a, Ext& b) {                                                      \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[b]\n\tfldt %[a]\n\t" INSN "\n\tfnstsw %[sw]\n\tfstpt %[b]\n\t" X87_POST         \
        : [b] "+m"(b), [sw] "=m"(sw), [saved] "=m"(saved)                                                        \
        : [a] "m"(a), [env] "m"(env)                                                                             \
        : "st", "st(1)", "memory");                                                                              \
    return sw;                                                                                                   \
  }
DEF_BINARY_POP(h_fpatan, "fpatan")
DEF_BINARY_POP(h_fyl2x, "fyl2x")
DEF_BINARY_POP(h_fyl2xp1, "fyl2xp1")

// st0 -> (st0', st1') for fsincos (cos, sin), fptan (1.0, tan), fxtract (significand, exponent). The caller rules out
// the out-of-range case in which fsincos/fptan leave the stack alone.
#define DEF_SPLIT(NAME, INSN)                                                                                    \
  uint16_t NAME(const HostEnv& env, const Ext& a, Ext& r0, Ext& r1) {                                            \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[a]\n\t" INSN "\n\tfnstsw %[sw]\n\tfstpt %[r0]\n\tfstpt %[r1]\n\t" X87_POST      \
        : [r0] "=m"(r0), [r1] "=m"(r1), [sw] "=m"(sw), [saved] "=m"(saved)                                       \
        : [a] "m"(a), [env] "m"(env)                                                                             \
        : "st", "st(1)", "memory");                                                                              \
    return sw;                                                                                                   \
  }
DEF_SPLIT(h_fsincos, "fsincos")
DEF_SPLIT(h_fptan, "fptan")
DEF_SPLIT(h_fxtract, "fxtract")

// Comparisons: condition codes (and for fcomi/fucomi the EFLAGS image) from st0 vs st1.
#define DEF_COMPARE(NAME, INSN)                                                                                  \
  uint16_t NAME(const HostEnv& env, const Ext& a, const Ext& b) {                                                \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[b]\n\tfldt %[a]\n\t" INSN "\n\tfnstsw %[sw]\n\tfstp %%st(0)\n\tfstp %%st(0)\n\t" \
        X87_POST                                                                                                 \
        : [sw] "=m"(sw), [saved] "=m"(saved)                                                                     \
        : [a] "m"(a), [b] "m"(b), [env] "m"(env)                                                                 \
        : "st", "st(1)", "memory");                                                                              \
    return sw;                                                                                                   \
  }
DEF_COMPARE(h_fcom, "fcom %%st(1)")
DEF_COMPARE(h_fucom, "fucom %%st(1)")

#define DEF_COMPARE_I(NAME, INSN)                                                                                \
  uint16_t NAME(const HostEnv& env, const Ext& a, const Ext& b, uint64_t& rflags) {                              \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[b]\n\tfldt %[a]\n\t" INSN "\n\tpushfq\n\tpopq %[fl]\n\tfnstsw %[sw]\n\t"        \
                         "fstp %%st(0)\n\tfstp %%st(0)\n\t" X87_POST                                             \
        : [sw] "=m"(sw), [saved] "=m"(saved), [fl] "=r"(rflags)                                                  \
        : [a] "m"(a), [b] "m"(b), [env] "m"(env)                                                                 \
        : "st", "st(1)", "memory", "cc");                                                                        \
    return sw;                                                                                                   \
  }
DEF_COMPARE_I(h_fcomi, "fcomi %%st(1), %%st")
DEF_COMPARE_I(h_fucomi, "fucomi %%st(1), %%st")

#define DEF_EXAMINE(NAME, INSN)                                                                                  \
  uint16_t NAME(const HostEnv& env, const Ext& a) {                                                              \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[a]\n\t" INSN "\n\tfnstsw %[sw]\n\tfstp %%st(0)\n\t" X87_POST                    \
        : [sw] "=m"(sw), [saved] "=m"(saved)                                                                     \
        : [a] "m"(a), [env] "m"(env)                                                                             \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
DEF_EXAMINE(h_ftst, "ftst")
DEF_EXAMINE(h_fxam, "fxam")

// Loads (memory format -> 80-bit): r = convert(*src)
#define DEF_LOAD(NAME, INSN, SRC_T)                                                                              \
  uint16_t NAME(const HostEnv& env, const SRC_T& src, Ext& r) {                                                  \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE INSN " %[src]\n\tfnstsw %[sw]\n\tfstpt %[r]\n\t" X87_POST                               \
        : [r] "=m"(r), [sw] "=m"(sw), [saved] "=m"(saved)                                                        \
        : [src] "m"(src), [env] "m"(env)                                                                         \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
struct alignas(16) Bcd {
  uint8_t b[10];
};
DEF_LOAD(h_fld32, "flds", float)
DEF_LOAD(h_fld64, "fldl", double)
DEF_LOAD(h_fild16, "filds", int16_t)
DEF_LOAD(h_fild32, "fildl", int32_t)
DEF_LOAD(h_fild64, "fildll", int64_t)
DEF_LOAD(h_fbld, "fbld", Bcd)

// Stores (80-bit -> memory format): *dst = convert(a)
#define DEF_STORE(NAME, INSN, DST_T)                                                                             \
  uint16_t NAME(const HostEnv& env, const Ext& a, DST_T& dst) {                                                  \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[a]\n\t" INSN " %[dst]\n\tfnstsw %[sw]\n\t" X87_POST                             \
        : [dst] "=m"(dst), [sw] "=m"(sw), [saved] "=m"(saved)                                                    \
        : [a] "m"(a), [env] "m"(env)                                                                             \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
DEF_STORE(h_fst32, "fstps", float)
DEF_STORE(h_fst64, "fstpl", double)
DEF_STORE(h_fist16, "fistps", int16_t)
DEF_STORE(h_fist32, "fistpl", int32_t)
DEF_STORE(h_fist64, "fistpll", int64_t)
DEF_STORE(h_fisttp16, "fisttps", int16_t)
DEF_STORE(h_fisttp32, "fisttpl", int32_t)
DEF_STORE(h_fisttp64, "fisttpll", int64_t)
DEF_STORE(h_fbstp, "fbstp", Bcd)

// Constants (rounded per the guest's rounding control).
#define DEF_CONST(NAME, INSN)                                                                                    \
  uint16_t NAME(const HostEnv& env, Ext& r) {                                                                    \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE INSN "\n\tfnstsw %[sw]\n\tfstpt %[r]\n\t" X87_POST                                      \
        : [r] "=m"(r), [sw] "=m"(sw), [saved] "=m"(saved)                                                        \
        : [env] "m"(env)                                                                                         \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
DEF_CONST(h_fld1, "fld1")
DEF_CONST(h_fldl2t, "fldl2t")
DEF_CONST(h_fldl2e, "fldl2e")
DEF_CONST(h_fldpi, "fldpi")
DEF_CONST(h_fldlg2, "fldlg2")
DEF_CONST(h_fldln2, "fldln2")
DEF_CONST(h_fldz, "fldz")

// st0 = st0 op m32real/m64real, with the memory operand used in place. Converting it separately first would get the
// corner cases wrong: an SNaN operand would be quieted before the operation (x87 returns the *QNaN* operand when the
// other one is an SNaN), and a denormal one would raise #D ahead of higher-priority exceptions.
#define DEF_MEM_ARITH(NAME, INSN, SRC_T)                                                                         \
  uint16_t NAME(const HostEnv& env, Ext& a, const SRC_T& src) {                                                  \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[a]\n\t" INSN " %[src]\n\tfnstsw %[sw]\n\tfstpt %[a]\n\t" X87_POST               \
        : [a] "+m"(a), [sw] "=m"(sw), [saved] "=m"(saved)                                                        \
        : [src] "m"(src), [env] "m"(env)                                                                         \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
DEF_MEM_ARITH(h_fadd_m32, "fadds", float)
DEF_MEM_ARITH(h_fmul_m32, "fmuls", float)
DEF_MEM_ARITH(h_fsub_m32, "fsubs", float)
DEF_MEM_ARITH(h_fsubr_m32, "fsubrs", float)
DEF_MEM_ARITH(h_fdiv_m32, "fdivs", float)
DEF_MEM_ARITH(h_fdivr_m32, "fdivrs", float)
DEF_MEM_ARITH(h_fadd_m64, "faddl", double)
DEF_MEM_ARITH(h_fmul_m64, "fmull", double)
DEF_MEM_ARITH(h_fsub_m64, "fsubl", double)
DEF_MEM_ARITH(h_fsubr_m64, "fsubrl", double)
DEF_MEM_ARITH(h_fdiv_m64, "fdivl", double)
DEF_MEM_ARITH(h_fdivr_m64, "fdivrl", double)

#define DEF_MEM_COMPARE(NAME, INSN, SRC_T)                                                                       \
  uint16_t NAME(const HostEnv& env, const Ext& a, const SRC_T& src) {                                            \
    uint16_t saved, sw;                                                                                          \
    asm volatile(X87_PRE "fldt %[a]\n\t" INSN " %[src]\n\tfnstsw %[sw]\n\tfstp %%st(0)\n\t" X87_POST             \
        : [sw] "=m"(sw), [saved] "=m"(saved)                                                                     \
        : [a] "m"(a), [src] "m"(src), [env] "m"(env)                                                             \
        : "st", "memory");                                                                                       \
    return sw;                                                                                                   \
  }
DEF_MEM_COMPARE(h_fcom_m32, "fcoms", float)
DEF_MEM_COMPARE(h_fcom_m64, "fcoml", double)

#undef X87_PRE
#undef X87_POST

// Tag of a non-empty register, from its contents (00 valid, 01 zero, 10 special).
uint16_t tag_for(const FReg& r) {
  uint16_t exp = r.se & 0x7FFF;
  if (exp == 0x7FFF) {
    return 2;
  }
  if (exp == 0) {
    return (r.mant == 0) ? 1 : 2;
  }
  return (r.mant & 0x8000000000000000ULL) ? 0 : 2; // an unnormal is special
}

} // namespace

// ---- State ----------------------------------------------------------------------------------------------------------

uint16_t X86Emulator::FPUState::tag_word() const {
  uint16_t tw = 0;
  for (uint8_t z = 0; z < 8; z++) {
    uint16_t tag = (this->empty & (1 << z)) ? 3 : tag_for(this->r[z]);
    tw |= tag << (z * 2);
  }
  return tw;
}

void X86Emulator::FPUState::reset() {
  this->cw = 0x037F;
  this->sw = 0;
  this->top = 0;
  this->empty = 0xFF;
  this->fip = 0;
  this->fdp = 0;
  this->fcs = 0;
  this->fds = 0;
  this->fop = 0;
}

void X86Emulator::fpu_save_image(void* out) const {
  uint8_t* p = reinterpret_cast<uint8_t*>(out);
  const auto& f = this->fpu_state;
  auto put32 = [&](size_t off, uint32_t v) { memcpy(p + off, &v, 4); };
  put32(0, 0xFFFF0000 | f.cw);
  put32(4, 0xFFFF0000 | f.status_word());
  put32(8, 0xFFFF0000 | f.tag_word());
  put32(12, f.fip);
  put32(16, f.fcs | (static_cast<uint32_t>(f.fop & 0x7FF) << 16));
  put32(20, f.fdp);
  put32(24, 0xFFFF0000 | f.fds);
  for (uint8_t z = 0; z < 8; z++) {
    const auto& r = f.r[(f.top + z) & 7];
    memcpy(p + 28 + z * 10, &r.mant, 8);
    memcpy(p + 28 + z * 10 + 8, &r.se, 2);
  }
}

// ---- Emulator plumbing ----------------------------------------------------------------------------------------------

void X86Emulator::check_fpu_pending() {
  if (this->fpu_state.sw & SW_ES) {
    raise(VEC_MF, 0, "unmasked x87 exception pending");
  }
}

void X86Emulator::exec_9B_wait(uint8_t) {
  this->check_fpu_pending();
}

void X86Emulator::fpu_note_instruction(uint16_t fop) {
  this->fpu_state.fip = this->insn_eip;
  this->fpu_state.fcs = this->segs[SEG_CS].sel;
  this->fpu_state.fop = fop & 0x7FF;
}

uint32_t X86Emulator::fpu_mem_linear(const DecodedRM& rm, uint32_t size, bool write) {
  uint32_t off = this->resolve_mem_ea(rm);
  this->fpu_state.fdp = off;
  this->fpu_state.fds = this->segs[rm.seg].sel;
  return this->lin_data(rm.seg, off, size, write);
}

namespace {

// Everything the x87 opcode groups share, bound to one emulator for the duration of an instruction.
struct FPU {
  X86Emulator::FPUState& f;
  bool commit = true; // false once an unmasked #IA/#Z/#D suppresses the destination write

  inline uint8_t phys(uint8_t i) const {
    return (this->f.top + i) & 7;
  }
  inline bool is_empty(uint8_t i) const {
    return this->f.empty & (1 << this->phys(i));
  }
  inline FReg& st(uint8_t i) {
    return this->f.r[this->phys(i)];
  }
  // Environment for a host primitive: the guest control word with every exception masked, the guest's condition
  // codes, TOP 0, all registers empty.
  inline HostEnv host_env() const {
    return HostEnv{static_cast<uint32_t>(this->f.cw | 0x003F), static_cast<uint32_t>(this->f.sw & SW_CC), 0xFFFF, 0,
        0, 0, 0};
  }

  // Merges a host status word: exceptions are sticky, the condition codes are replaced. Returns true if the result
  // may be committed.
  //
  // The host ran the operation with every exception masked. An unmasked invalid-operation, zero-divide or
  // denormal-operand exception stops the real instruction before it computes anything, so then only those flags are
  // recorded: no result, and none of the inexact/underflow/overflow flags or the C1 rounding indication the host's
  // masked run went on to raise. C0/C2/C3 still come from the host: a compare with an unmasked #IA still reports
  // "unordered" (verified against hardware), and arithmetic leaves them as the guest had them.
  bool apply(uint16_t host_sw, uint16_t cc_mask = SW_CC) {
    uint16_t exc = host_sw & SW_EXC;
    uint16_t unmasked = exc & ~this->f.cw & SW_EXC;
    if (unmasked & (SW_IE | SW_ZE | SW_DE)) {
      uint16_t cc = cc_mask & ~SW_C1;
      this->f.sw = (this->f.sw & ~cc) | (host_sw & cc) | (exc & (SW_IE | SW_ZE | SW_DE)) | SW_ES | SW_B;
      this->commit = false;
      return false;
    }
    this->f.sw = (this->f.sw & ~cc_mask) | (host_sw & cc_mask) | exc;
    if (unmasked) {
      this->f.sw |= SW_ES | SW_B;
    }
    return this->commit;
  }

  // For a store to memory (fst/fstp m32real/m64real) of `a` into a format whose smallest normal exponent is
  // `min_exp`: an unmasked overflow or underflow suppresses both the store and the pop, and then neither the inexact
  // flag nor C1's rounding indication is reported. With underflow unmasked, a tiny value underflows even when the
  // store is exact (the masked host run reports underflow only for inexact tiny results, per IEEE 754), with tininess
  // detected before rounding. Verified against hardware by the differential test. Returns true if the value may be
  // stored.
  bool apply_store(uint16_t host_sw, const Ext& a, int32_t min_exp) {
    if (!(this->f.cw & SW_UE)) {
      uint16_t exp = a.se & 0x7FFF;
      bool nonzero_finite = (exp != 0x7FFF) && ((exp != 0) || (a.mant != 0));
      if (nonzero_finite && (static_cast<int32_t>(exp) - 16383 < min_exp)) {
        host_sw |= SW_UE;
      }
    }
    if (host_sw & ~this->f.cw & (SW_OE | SW_UE)) {
      this->apply(host_sw & ~(SW_PE | SW_C1));
      this->commit = false;
      return false;
    }
    return this->apply(host_sw);
  }

  // Stack fault: underflow (reading an empty register) or overflow (pushing onto a full one).
  bool stack_fault(bool overflow) {
    this->f.sw = (this->f.sw & ~SW_C1) | SW_IE | SW_SF | (overflow ? SW_C1 : 0);
    if (!(this->f.cw & SW_IE)) {
      this->f.sw |= SW_ES | SW_B;
      this->commit = false;
      return false;
    }
    return true; // masked: the caller stores the indefinite QNaN
  }

  void set_c1(bool v) {
    this->f.sw = (this->f.sw & ~SW_C1) | (v ? SW_C1 : 0);
  }

  void push(const FReg& v) {
    this->f.top = (this->f.top - 1) & 7;
    this->f.r[this->f.top] = v;
    this->f.empty &= ~(1 << this->f.top);
  }
  void pop() {
    this->f.empty |= (1 << this->f.top);
    this->f.top = (this->f.top + 1) & 7;
  }
  void set(uint8_t i, const FReg& v) {
    uint8_t p = this->phys(i);
    this->f.r[p] = v;
    this->f.empty &= ~(1 << p);
  }

  // Reads ST(i) for an arithmetic operand; on underflow returns false (and handles the masked/unmasked cases).
  bool read(uint8_t i, Ext& out) {
    if (this->is_empty(i)) {
      this->stack_fault(false);
      return false;
    }
    out = to_ext(this->st(i));
    return true;
  }

  // Pushes a value produced by a host load; handles overflow.
  void push_loaded(uint16_t host_sw, const Ext& v) {
    if (!this->is_empty(7)) {
      if (this->stack_fault(true)) {
        this->push(INDEFINITE);
      }
      return;
    }
    if (this->apply(host_sw)) {
      this->push(to_reg(v));
    }
  }
};

} // namespace

// Binary arithmetic helper: dest op src (or src op dest when reversed) with op = /0 add, /1 mul, /4-5 sub, /6-7 div.
static uint16_t host_arith(uint8_t op, bool reverse, const HostEnv& env, Ext& dest, const Ext& src) {
  switch (op) {
    case 0:
      return h_fadd(env, dest, src);
    case 1:
      return h_fmul(env, dest, src);
    case 4:
    case 5:
      return reverse ? h_fsubr(env, dest, src) : h_fsub(env, dest, src);
    default:
      return reverse ? h_fdivr(env, dest, src) : h_fdiv(env, dest, src);
  }
}

// ---- D8 / DC: arithmetic with m32real / m64real / ST(i) -------------------------------------------------------------

void X86Emulator::exec_D8_DC_float_basic_math(uint8_t opcode) {
  this->check_fpu_pending();
  bool is_DC = (opcode == 0xDC);
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;
  this->fpu_note_instruction(((opcode & 7) << 8) | (rm.has_mem_ref() ? (what << 3) : (0xC0 | (what << 3) | rm.ea_reg)));
  FPU fpu{this->fpu_state};

  if (rm.has_mem_ref()) {
    // m32real / m64real: the host executes the memory form itself (see DEF_MEM_ARITH).
    uint32_t addr = this->fpu_mem_linear(rm, is_DC ? 8 : 4, false);
    double d = 0.0;
    float f = 0.0f;
    if (is_DC) {
      d = this->r_mem<double>(addr);
    } else {
      f = this->r_mem<float>(addr);
    }
    Ext a;
    bool is_compare = (what == 2) || (what == 3);
    if (!fpu.read(0, a)) {
      if (fpu.commit) {
        if (is_compare) {
          this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
          if (what == 3) {
            fpu.pop();
          }
        } else {
          fpu.set(0, INDEFINITE);
        }
      }
      return;
    }
    auto env = fpu.host_env();
    if (is_compare) {
      fpu.apply(is_DC ? h_fcom_m64(env, a, d) : h_fcom_m32(env, a, f));
      if (what == 3 && fpu.commit) {
        fpu.pop();
      }
      return;
    }
    static uint16_t (*const m32_ops[8])(const HostEnv&, Ext&, const float&) = {h_fadd_m32, h_fmul_m32, nullptr,
        nullptr, h_fsub_m32, h_fsubr_m32, h_fdiv_m32, h_fdivr_m32};
    static uint16_t (*const m64_ops[8])(const HostEnv&, Ext&, const double&) = {h_fadd_m64, h_fmul_m64, nullptr,
        nullptr, h_fsub_m64, h_fsubr_m64, h_fdiv_m64, h_fdivr_m64};
    uint16_t sw = is_DC ? m64_ops[what](env, a, d) : m32_ops[what](env, a, f);
    if (fpu.apply(sw)) {
      fpu.set(0, to_reg(a));
    }
    return;
  }

  if ((what == 2) || (what == 3)) { // fcom / fcomp ST(i) (DC D0-DF are aliases)
    Ext a, b;
    if (fpu.read(0, a) && fpu.read(rm.ea_reg, b)) {
      fpu.apply(h_fcom(fpu.host_env(), a, b));
    } else if (fpu.commit) {
      this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
    }
    if (what == 3 && fpu.commit) {
      fpu.pop();
    }
    return;
  }

  // Register forms: dest = ST(0) for D8, ST(i) for DC (whose sub/div senses are reversed).
  uint8_t dest_i = is_DC ? rm.ea_reg : 0;
  bool reverse = (what >= 4) && ((what & 1) ^ (is_DC ? 1 : 0));
  Ext dest, src;
  bool ok = fpu.read(dest_i, dest) && fpu.read(is_DC ? 0 : rm.ea_reg, src);
  if (!ok) {
    if (fpu.commit) {
      fpu.set(dest_i, INDEFINITE);
    }
    return;
  }
  if (fpu.apply(host_arith(what, reverse, fpu.host_env(), dest, src))) {
    fpu.set(dest_i, to_reg(dest));
  }
}

// ---- DE: arithmetic with m16int / pop forms -------------------------------------------------------------------------
// ---- DA: arithmetic with m32int / fcmov / fucompp -------------------------------------------------------------------

void X86Emulator::exec_DE_float_misc1(uint8_t opcode) {
  this->check_fpu_pending();
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;
  this->fpu_note_instruction(((opcode & 7) << 8) | (rm.has_mem_ref() ? (what << 3) : (0xC0 | (what << 3) | rm.ea_reg)));
  FPU fpu{this->fpu_state};

  if (rm.has_mem_ref()) {
    // fiadd/fimul/ficom/ficomp/fisub/fisubr/fidiv/fidivr m16int
    uint32_t addr = this->fpu_mem_linear(rm, 2, false);
    int16_t v = this->r_mem<uint16_t>(addr);
    Ext src;
    h_fild16(fpu.host_env(), v, src);
    Ext a;
    if (!fpu.read(0, a)) {
      if (fpu.commit) {
        if (what == 2 || what == 3) {
          this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
        } else {
          fpu.set(0, INDEFINITE);
        }
      }
      if (what == 3 && fpu.commit) {
        fpu.pop();
      }
      return;
    }
    if (what == 2 || what == 3) {
      fpu.apply(h_fcom(fpu.host_env(), a, src));
      if (what == 3 && fpu.commit) {
        fpu.pop();
      }
      return;
    }
    if (fpu.apply(host_arith(what, (what & 1) && (what >= 4), fpu.host_env(), a, src))) {
      fpu.set(0, to_reg(a));
    }
    return;
  }

  if (what == 3) { // DE D9: fcompp (other DE D8-DF forms are fcomp aliases)
    Ext a, b;
    bool ok = fpu.read(0, a) && fpu.read(rm.ea_reg, b);
    if (!ok) {
      if (fpu.commit) {
        this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
      }
    } else {
      fpu.apply(h_fcom(fpu.host_env(), a, b));
    }
    if (fpu.commit) {
      fpu.pop();
      if (rm.ea_reg == 1) {
        fpu.pop();
      }
    }
    return;
  }
  if (what == 2) { // fcomp5 alias
    Ext a, b;
    bool ok = fpu.read(0, a) && fpu.read(rm.ea_reg, b);
    if (ok) {
      fpu.apply(h_fcom(fpu.host_env(), a, b));
    } else if (fpu.commit) {
      this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
    }
    if (fpu.commit) {
      fpu.pop();
    }
    return;
  }

  // faddp/fmulp/fsubrp/fsubp/fdivrp/fdivp ST(i), ST(0)
  bool reverse = (what >= 4) && !(what & 1);
  Ext dest, src;
  bool ok = fpu.read(rm.ea_reg, dest) && fpu.read(0, src);
  if (!ok) {
    if (fpu.commit) {
      fpu.set(rm.ea_reg, INDEFINITE);
      fpu.pop();
    }
    return;
  }
  if (fpu.apply(host_arith(what, reverse, fpu.host_env(), dest, src))) {
    fpu.set(rm.ea_reg, to_reg(dest));
    fpu.pop();
  }
}

void X86Emulator::exec_DA_DB_float_cmov_and_int_math(uint8_t opcode) {
  bool is_DB = (opcode & 1);
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;

  // DB E2 fnclex and DB E3 fninit are the non-waiting control forms.
  if (is_DB && !rm.has_mem_ref() && what == 4) {
    switch (rm.ea_reg) {
      case 2: // fnclex
        this->fpu_state.sw &= ~(SW_EXC | SW_SF | SW_ES | SW_B);
        return;
      case 3: // fninit
        this->fpu_state.reset();
        return;
      case 0: // fneni, fndisi, fnsetpm: no-ops since the 80387
      case 1:
      case 4:
        return;
      default:
        raise(VEC_UD, 0, "invalid DB E5-E7");
    }
  }

  this->check_fpu_pending();
  this->fpu_note_instruction(((opcode & 7) << 8) | (rm.has_mem_ref() ? (what << 3) : (0xC0 | (what << 3) | rm.ea_reg)));
  FPU fpu{this->fpu_state};

  if (!rm.has_mem_ref()) {
    if (what <= 3) { // fcmovcc
      static const uint8_t conds[4] = {0x2 /*b*/, 0x4 /*e*/, 0x6 /*be*/, 0xA /*u = p*/};
      bool cond = this->regs.check_condition(conds[what]);
      if (is_DB) {
        cond = !cond;
      }
      Ext v;
      if (fpu.is_empty(0) || fpu.is_empty(rm.ea_reg)) {
        if (fpu.stack_fault(false)) {
          fpu.set(0, INDEFINITE);
        }
        return;
      }
      fpu.set_c1(false);
      if (cond) {
        v = to_ext(fpu.st(rm.ea_reg));
        fpu.set(0, to_reg(v));
      }
      return;
    }
    if (!is_DB) {
      if (what == 5 && rm.ea_reg == 1) { // fucompp
        Ext a, b;
        bool ok = fpu.read(0, a) && fpu.read(1, b);
        if (ok) {
          fpu.apply(h_fucom(fpu.host_env(), a, b));
        } else if (fpu.commit) {
          this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
        }
        if (fpu.commit) {
          fpu.pop();
          fpu.pop();
        }
        return;
      }
      raise(VEC_UD, 0, "invalid DA register form");
    }
    if (what == 5 || what == 6) { // fucomi / fcomi
      Ext a, b;
      uint32_t zpc = Regs::ZF | Regs::PF | Regs::CF;
      if (!(fpu.read(0, a) && fpu.read(rm.ea_reg, b))) {
        if (fpu.commit) {
          this->regs.replace_flags(zpc | Regs::OF | Regs::SF | Regs::AF, zpc);
        }
        return;
      }
      uint64_t rflags;
      uint16_t sw = (what == 5) ? h_fucomi(fpu.host_env(), a, b, rflags) : h_fcomi(fpu.host_env(), a, b, rflags);
      // EFLAGS get the comparison result even when an unmasked exception stops the instruction (an unordered
      // compare still reports ZF=PF=CF=1; verified against hardware).
      fpu.apply(sw, SW_C1);
      this->regs.replace_flags(zpc | Regs::OF | Regs::SF | Regs::AF, static_cast<uint32_t>(rflags) & zpc);
      return;
    }
    raise(VEC_UD, 0, "invalid DB register form");
  }

  if (!is_DB) {
    // fiadd/fimul/ficom/ficomp/fisub/fisubr/fidiv/fidivr m32int
    uint32_t addr = this->fpu_mem_linear(rm, 4, false);
    int32_t v = this->r_mem<uint32_t>(addr);
    Ext src;
    h_fild32(fpu.host_env(), v, src);
    Ext a;
    if (!fpu.read(0, a)) {
      if (fpu.commit) {
        if (what == 2 || what == 3) {
          this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
        } else {
          fpu.set(0, INDEFINITE);
        }
      }
      if (what == 3 && fpu.commit) {
        fpu.pop();
      }
      return;
    }
    if (what == 2 || what == 3) {
      fpu.apply(h_fcom(fpu.host_env(), a, src));
      if (what == 3 && fpu.commit) {
        fpu.pop();
      }
      return;
    }
    if (fpu.apply(host_arith(what, (what & 1) && (what >= 4), fpu.host_env(), a, src))) {
      fpu.set(0, to_reg(a));
    }
    return;
  }

  switch (what) {
    case 0: { // fild m32int
      uint32_t addr = this->fpu_mem_linear(rm, 4, false);
      int32_t v = this->r_mem<uint32_t>(addr);
      Ext r;
      fpu.push_loaded(h_fild32(fpu.host_env(), v, r), r);
      return;
    }
    case 1: // fisttp m32int
    case 2: // fist m32int
    case 3: { // fistp m32int
      uint32_t addr = this->fpu_mem_linear(rm, 4, true);
      Ext a;
      int32_t out;
      if (!fpu.read(0, a)) {
        if (!fpu.commit) {
          return;
        }
        out = INT32_MIN; // integer indefinite
      } else {
        uint16_t sw = (what == 1) ? h_fisttp32(fpu.host_env(), a, out) : h_fist32(fpu.host_env(), a, out);
        if (!fpu.apply(sw)) {
          return;
        }
      }
      this->w_mem<uint32_t>(addr, out);
      if (what != 2) {
        fpu.pop();
      }
      return;
    }
    case 5: { // fld m80real (no exceptions besides overflow)
      uint32_t addr = this->fpu_mem_linear(rm, 10, false);
      FReg v{this->r_mem<uint64_t>(addr), this->r_mem<uint16_t>(addr + 8)};
      if (!fpu.is_empty(7)) {
        if (fpu.stack_fault(true)) {
          fpu.push(INDEFINITE);
        }
        return;
      }
      fpu.set_c1(false);
      fpu.push(v);
      return;
    }
    case 7: { // fstp m80real
      uint32_t addr = this->fpu_mem_linear(rm, 10, true);
      FReg v;
      if (fpu.is_empty(0)) {
        if (!fpu.stack_fault(false)) {
          return;
        }
        v = INDEFINITE;
      } else {
        v = fpu.st(0);
        fpu.set_c1(false);
      }
      this->w_mem<uint64_t>(addr, v.mant);
      this->w_mem<uint16_t>(addr + 8, v.se);
      fpu.pop();
      return;
    }
    default:
      raise(VEC_UD, 0, "invalid DB memory form");
  }
}

// ---- D9 / DD: loads, stores, control, and the transcendental group -------------------------------------------------

void X86Emulator::fpu_store_env(const DecodedRM& rm, bool op32, bool and_regs) {
  auto& f = this->fpu_state;
  uint32_t size = (op32 ? 28 : 14) + (and_regs ? 80 : 0);
  uint32_t addr = this->lin_data(rm.seg, this->resolve_mem_ea(rm), size, true);
  if (op32) {
    this->w_mem<uint32_t>(addr + 0, 0xFFFF0000 | f.cw);
    this->w_mem<uint32_t>(addr + 4, 0xFFFF0000 | f.status_word());
    this->w_mem<uint32_t>(addr + 8, 0xFFFF0000 | f.tag_word());
    this->w_mem<uint32_t>(addr + 12, f.fip);
    this->w_mem<uint32_t>(addr + 16, f.fcs | (static_cast<uint32_t>(f.fop & 0x7FF) << 16));
    this->w_mem<uint32_t>(addr + 20, f.fdp);
    this->w_mem<uint32_t>(addr + 24, 0xFFFF0000 | f.fds);
  } else {
    this->w_mem<uint16_t>(addr + 0, f.cw);
    this->w_mem<uint16_t>(addr + 2, f.status_word());
    this->w_mem<uint16_t>(addr + 4, f.tag_word());
    this->w_mem<uint16_t>(addr + 6, f.fip);
    this->w_mem<uint16_t>(addr + 8, f.fcs);
    this->w_mem<uint16_t>(addr + 10, f.fdp);
    this->w_mem<uint16_t>(addr + 12, f.fds);
  }
  if (and_regs) {
    uint32_t regs_addr = addr + (op32 ? 28 : 14);
    for (uint8_t z = 0; z < 8; z++) {
      const auto& r = f.r[(f.top + z) & 7];
      this->w_mem<uint64_t>(regs_addr + z * 10, r.mant);
      this->w_mem<uint16_t>(regs_addr + z * 10 + 8, r.se);
    }
  }
}

void X86Emulator::fpu_load_env(const DecodedRM& rm, bool op32, bool and_regs) {
  auto& f = this->fpu_state;
  uint32_t size = (op32 ? 28 : 14) + (and_regs ? 80 : 0);
  uint32_t addr = this->lin_data(rm.seg, this->resolve_mem_ea(rm), size, false);
  uint16_t tw;
  if (op32) {
    f.cw = this->r_mem<uint16_t>(addr + 0);
    f.sw = this->r_mem<uint16_t>(addr + 4);
    tw = this->r_mem<uint16_t>(addr + 8);
    f.fip = this->r_mem<uint32_t>(addr + 12);
    uint32_t cs_op = this->r_mem<uint32_t>(addr + 16);
    f.fcs = cs_op;
    f.fop = (cs_op >> 16) & 0x7FF;
    f.fdp = this->r_mem<uint32_t>(addr + 20);
    f.fds = this->r_mem<uint16_t>(addr + 24);
  } else {
    f.cw = this->r_mem<uint16_t>(addr + 0);
    f.sw = this->r_mem<uint16_t>(addr + 2);
    tw = this->r_mem<uint16_t>(addr + 4);
    f.fip = this->r_mem<uint16_t>(addr + 6);
    f.fcs = this->r_mem<uint16_t>(addr + 8);
    f.fdp = this->r_mem<uint16_t>(addr + 10);
    f.fds = this->r_mem<uint16_t>(addr + 12);
  }
  f.top = (f.sw >> 11) & 7;
  f.sw &= ~0x3800;
  // Only "empty or not" survives a reload; the other tags are recomputed from contents on the next store.
  f.empty = 0;
  for (uint8_t z = 0; z < 8; z++) {
    if (((tw >> (z * 2)) & 3) == 3) {
      f.empty |= (1 << z);
    }
  }
  // ES/B follow the unmasked-and-raised exceptions of the reloaded words.
  if (f.sw & ~f.cw & SW_EXC) {
    f.sw |= SW_ES | SW_B;
  } else {
    f.sw &= ~(SW_ES | SW_B);
  }
  if (and_regs) {
    uint32_t regs_addr = addr + (op32 ? 28 : 14);
    for (uint8_t z = 0; z < 8; z++) {
      auto& r = f.r[(f.top + z) & 7];
      r.mant = this->r_mem<uint64_t>(regs_addr + z * 10);
      r.se = this->r_mem<uint16_t>(regs_addr + z * 10 + 8);
    }
  }
}

void X86Emulator::exec_D9_DD_float_moves_and_analytical_math(uint8_t opcode) {
  bool is_DD = (opcode == 0xDD);
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;
  bool op32 = !this->overrides.operand_size;

  // The non-waiting control forms first: fnstenv/fnstcw (D9 /6 /7), fnsave/fnstsw (DD /6 /7).
  if (rm.has_mem_ref()) {
    if (what == 6) {
      if (is_DD) { // fnsave: store everything, then reinitialize
        this->fpu_store_env(rm, op32, true);
        this->fpu_state.reset();
      } else { // fnstenv: also masks all exceptions afterwards
        this->fpu_store_env(rm, op32, false);
        this->fpu_state.cw |= 0x003F;
        // With everything masked nothing is pending any more: hardware clears ES and B (the raised flags stay), so a
        // following fwait does not fault (verified on hardware). Exception handlers rely on this.
        this->fpu_state.sw &= ~(SW_ES | SW_B);
      }
      return;
    }
    if (what == 7) {
      uint32_t addr = this->lin_data(rm.seg, this->resolve_mem_ea(rm), 2, true);
      this->w_mem<uint16_t>(addr, is_DD ? this->fpu_state.status_word() : this->fpu_state.cw);
      return;
    }
    // fldenv/frstor/fldcw are waiting instructions: a pending unmasked exception is reported AT them, before the load
    // (verified on hardware), even when the new control word would mask it.
    if (what == 4) { // fldenv (D9) / frstor (DD)
      this->check_fpu_pending();
      this->fpu_load_env(rm, op32, is_DD);
      return;
    }
    if (what == 5 && !is_DD) { // fldcw
      this->check_fpu_pending();
      uint32_t addr = this->lin_data(rm.seg, this->resolve_mem_ea(rm), 2, false);
      this->fpu_state.cw = this->r_mem<uint16_t>(addr);
      if (this->fpu_state.sw & ~this->fpu_state.cw & SW_EXC) {
        this->fpu_state.sw |= SW_ES | SW_B;
      } else {
        this->fpu_state.sw &= ~(SW_ES | SW_B);
      }
      return;
    }
  }

  this->check_fpu_pending();
  this->fpu_note_instruction(((opcode & 7) << 8) | (rm.has_mem_ref() ? (what << 3) : (0xC0 | (what << 3) | rm.ea_reg)));
  FPU fpu{this->fpu_state};

  if (rm.has_mem_ref()) {
    switch (what) {
      case 0: { // fld m32real / m64real
        uint32_t addr = this->fpu_mem_linear(rm, is_DD ? 8 : 4, false);
        Ext r;
        uint16_t sw;
        if (is_DD) {
          double v = this->r_mem<double>(addr);
          sw = h_fld64(fpu.host_env(), v, r);
        } else {
          float v = this->r_mem<float>(addr);
          sw = h_fld32(fpu.host_env(), v, r);
        }
        fpu.push_loaded(sw, r);
        return;
      }
      case 1: { // fisttp m64int (DD); D9 /1 is invalid
        if (!is_DD) {
          raise(VEC_UD, 0, "D9 /1 with a memory operand");
        }
        uint32_t addr = this->fpu_mem_linear(rm, 8, true);
        Ext a;
        int64_t out;
        if (!fpu.read(0, a)) {
          if (!fpu.commit) {
            return;
          }
          out = INT64_MIN;
        } else if (!fpu.apply(h_fisttp64(fpu.host_env(), a, out))) {
          return;
        }
        this->w_mem<uint64_t>(addr, out);
        fpu.pop();
        return;
      }
      case 2: // fst m32real / m64real
      case 3: { // fstp
        uint32_t addr = this->fpu_mem_linear(rm, is_DD ? 8 : 4, true);
        Ext a;
        if (!fpu.read(0, a)) {
          if (!fpu.commit) {
            return;
          }
          if (is_DD) {
            this->w_mem<uint64_t>(addr, 0xFFF8000000000000ULL);
          } else {
            this->w_mem<uint32_t>(addr, 0xFFC00000);
          }
        } else if (is_DD) {
          double out;
          if (!fpu.apply_store(h_fst64(fpu.host_env(), a, out), a, -1022)) {
            return;
          }
          this->w_mem<double>(addr, out);
        } else {
          float out;
          if (!fpu.apply_store(h_fst32(fpu.host_env(), a, out), a, -126)) {
            return;
          }
          this->w_mem<float>(addr, out);
        }
        if (what == 3) {
          fpu.pop();
        }
        return;
      }
      default:
        raise(VEC_UD, 0, "invalid D9/DD memory form");
    }
  }

  // Register forms.
  uint8_t i = rm.ea_reg;
  if (is_DD) {
    switch (what) {
      case 0: // ffree ST(i)
        this->fpu_state.empty |= (1 << fpu.phys(i));
        return;
      case 1: // fxch4 alias
        break; // handled with D9 /1 below
      case 2: // fst ST(i)
      case 3: { // fstp ST(i)
        FReg v;
        if (fpu.is_empty(0)) {
          if (!fpu.stack_fault(false)) {
            return;
          }
          v = INDEFINITE;
        } else {
          v = fpu.st(0);
          fpu.set_c1(false);
        }
        fpu.set(i, v);
        if (what == 3) {
          fpu.pop();
        }
        return;
      }
      case 4: // fucom ST(i)
      case 5: { // fucomp ST(i)
        Ext a, b;
        if (fpu.read(0, a) && fpu.read(i, b)) {
          fpu.apply(h_fucom(fpu.host_env(), a, b));
        } else if (fpu.commit) {
          this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
        }
        if (what == 5 && fpu.commit) {
          fpu.pop();
        }
        return;
      }
      default:
        raise(VEC_UD, 0, "invalid DD register form");
    }
  }

  switch (what) {
    case 0: { // fld ST(i)
      if (fpu.is_empty(i)) {
        if (fpu.stack_fault(false)) {
          fpu.push(INDEFINITE);
        }
        return;
      }
      FReg v = fpu.st(i);
      if (!fpu.is_empty(7)) {
        if (fpu.stack_fault(true)) {
          fpu.push(INDEFINITE);
        }
        return;
      }
      fpu.set_c1(false);
      fpu.push(v);
      return;
    }
    case 1: { // fxch ST(i) (also DD C8+i, DF C8+i)
      bool e0 = fpu.is_empty(0), ei = fpu.is_empty(i);
      if (e0 || ei) {
        if (!fpu.stack_fault(false)) {
          return;
        }
      } else {
        fpu.set_c1(false);
      }
      FReg a = e0 ? INDEFINITE : fpu.st(0);
      FReg b = ei ? INDEFINITE : fpu.st(i);
      fpu.set(0, b);
      fpu.set(i, a);
      return;
    }
    case 2: // fnop (D9 D0); D9 D1-D7 are invalid
      if (i != 0) {
        raise(VEC_UD, 0, "invalid D9 D1-D7");
      }
      return;
    case 3: { // fstp1 alias (D9 D8+i)
      FReg v;
      if (fpu.is_empty(0)) {
        if (!fpu.stack_fault(false)) {
          return;
        }
        v = INDEFINITE;
      } else {
        v = fpu.st(0);
      }
      fpu.set(i, v);
      fpu.pop();
      return;
    }
    case 4: // fchs, fabs, ftst, fxam
      switch (i) {
        case 0: // fchs
        case 1: { // fabs
          Ext a;
          if (!fpu.read(0, a)) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
            }
            return;
          }
          FReg v = to_reg(a);
          v.se = (i == 0) ? (v.se ^ 0x8000) : (v.se & 0x7FFF);
          fpu.set_c1(false);
          fpu.set(0, v);
          return;
        }
        case 4: { // ftst
          Ext a;
          if (!fpu.read(0, a)) {
            if (fpu.commit) {
              this->fpu_state.sw |= SW_C0 | SW_C2 | SW_C3;
            }
            return;
          }
          fpu.apply(h_ftst(fpu.host_env(), a));
          return;
        }
        case 5: { // fxam: an empty register classifies as "empty" (C3, C0), C1 = sign
          if (fpu.is_empty(0)) {
            bool sign = fpu.st(0).se & 0x8000;
            this->fpu_state.sw = (this->fpu_state.sw & ~SW_CC) | SW_C3 | SW_C0 | (sign ? SW_C1 : 0);
            return;
          }
          Ext a = to_ext(fpu.st(0));
          uint16_t sw = h_fxam(fpu.host_env(), a);
          this->fpu_state.sw = (this->fpu_state.sw & ~SW_CC) | (sw & SW_CC);
          return;
        }
        default:
          raise(VEC_UD, 0, "invalid D9 E2/E3/E6/E7");
      }
    case 5: { // constants
      if (i == 7) {
        raise(VEC_UD, 0, "invalid D9 EF");
      }
      Ext r;
      uint16_t sw;
      switch (i) {
        case 0:
          sw = h_fld1(fpu.host_env(), r);
          break;
        case 1:
          sw = h_fldl2t(fpu.host_env(), r);
          break;
        case 2:
          sw = h_fldl2e(fpu.host_env(), r);
          break;
        case 3:
          sw = h_fldpi(fpu.host_env(), r);
          break;
        case 4:
          sw = h_fldlg2(fpu.host_env(), r);
          break;
        case 5:
          sw = h_fldln2(fpu.host_env(), r);
          break;
        default:
          sw = h_fldz(fpu.host_env(), r);
          break;
      }
      fpu.push_loaded(sw, r);
      return;
    }
    case 6: // f2xm1, fyl2x, fptan, fpatan, fxtract, fprem1, fdecstp, fincstp
      switch (i) {
        case 0: { // f2xm1
          Ext a;
          if (!fpu.read(0, a)) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
            }
            return;
          }
          if (fpu.apply(h_f2xm1(fpu.host_env(), a))) {
            fpu.set(0, to_reg(a));
          }
          return;
        }
        case 1: // fyl2x
        case 3: { // fpatan
          Ext a, b;
          if (!(fpu.read(0, a) && fpu.read(1, b))) {
            if (fpu.commit) {
              fpu.set(1, INDEFINITE);
              fpu.pop();
            }
            return;
          }
          uint16_t sw = (i == 1) ? h_fyl2x(fpu.host_env(), a, b) : h_fpatan(fpu.host_env(), a, b);
          if (fpu.apply(sw)) {
            fpu.set(1, to_reg(b));
            fpu.pop();
          }
          return;
        }
        case 2: // fptan
        case 4: { // fxtract
          Ext a;
          if (!fpu.read(0, a)) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
              if (fpu.is_empty(7)) {
                fpu.push(INDEFINITE);
              }
            }
            return;
          }
          // fptan of |x| >= 2^63 sets C2 and leaves the stack alone.
          uint16_t exp = a.se & 0x7FFF;
          if (i == 2 && exp != 0x7FFF && exp >= 0x3FFF + 63) {
            this->fpu_state.sw = (this->fpu_state.sw & ~SW_CC) | SW_C2;
            return;
          }
          if (!fpu.is_empty(7)) {
            if (fpu.stack_fault(true)) {
              fpu.set(0, INDEFINITE);
              fpu.push(INDEFINITE);
            }
            return;
          }
          Ext r0, r1;
          uint16_t sw = (i == 2) ? h_fptan(fpu.host_env(), a, r0, r1) : h_fxtract(fpu.host_env(), a, r0, r1);
          if (fpu.apply(sw)) {
            fpu.set(0, to_reg(r1));
            fpu.push(to_reg(r0));
          }
          return;
        }
        case 5: { // fprem1
          Ext a, b;
          if (!(fpu.read(0, a) && fpu.read(1, b))) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
            }
            return;
          }
          if (fpu.apply(h_fprem1(fpu.host_env(), a, b))) {
            fpu.set(0, to_reg(a));
          }
          return;
        }
        case 6: // fdecstp
          this->fpu_state.top = (this->fpu_state.top - 1) & 7;
          fpu.set_c1(false);
          return;
        default: // fincstp
          this->fpu_state.top = (this->fpu_state.top + 1) & 7;
          fpu.set_c1(false);
          return;
      }
    default: // 7: fprem, fyl2xp1, fsqrt, fsincos, frndint, fscale, fsin, fcos
      switch (i) {
        case 0: // fprem
        case 5: { // fscale
          Ext a, b;
          if (!(fpu.read(0, a) && fpu.read(1, b))) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
            }
            return;
          }
          uint16_t sw = (i == 0) ? h_fprem(fpu.host_env(), a, b) : h_fscale(fpu.host_env(), a, b);
          if (fpu.apply(sw)) {
            fpu.set(0, to_reg(a));
          }
          return;
        }
        case 1: { // fyl2xp1
          Ext a, b;
          if (!(fpu.read(0, a) && fpu.read(1, b))) {
            if (fpu.commit) {
              fpu.set(1, INDEFINITE);
              fpu.pop();
            }
            return;
          }
          if (fpu.apply(h_fyl2xp1(fpu.host_env(), a, b))) {
            fpu.set(1, to_reg(b));
            fpu.pop();
          }
          return;
        }
        case 3: { // fsincos
          Ext a;
          if (!fpu.read(0, a)) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
              if (fpu.is_empty(7)) {
                fpu.push(INDEFINITE);
              }
            }
            return;
          }
          uint16_t exp = a.se & 0x7FFF;
          if (exp != 0x7FFF && exp >= 0x3FFF + 63) {
            this->fpu_state.sw = (this->fpu_state.sw & ~SW_CC) | SW_C2;
            return;
          }
          if (!fpu.is_empty(7)) {
            if (fpu.stack_fault(true)) {
              fpu.set(0, INDEFINITE);
              fpu.push(INDEFINITE);
            }
            return;
          }
          Ext r0, r1;
          if (fpu.apply(h_fsincos(fpu.host_env(), a, r0, r1))) {
            fpu.set(0, to_reg(r1));
            fpu.push(to_reg(r0));
          }
          return;
        }
        default: { // fsqrt, frndint, fsin, fcos
          Ext a;
          if (!fpu.read(0, a)) {
            if (fpu.commit) {
              fpu.set(0, INDEFINITE);
            }
            return;
          }
          uint16_t sw;
          if (i == 2) {
            sw = h_fsqrt(fpu.host_env(), a);
          } else if (i == 4) {
            sw = h_frndint(fpu.host_env(), a);
          } else if (i == 6) {
            sw = h_fsin(fpu.host_env(), a);
          } else {
            sw = h_fcos(fpu.host_env(), a);
          }
          if (fpu.apply(sw)) {
            fpu.set(0, to_reg(a));
          }
          return;
        }
      }
  }
}

// ---- DF: m16int / m64int / BCD loads and stores, fnstsw ax, fcomip/fucomip ---------------------------------------

void X86Emulator::exec_DF_float_misc2(uint8_t opcode) {
  auto rm = this->fetch_and_decode_rm();
  uint8_t what = rm.non_ea_reg;

  if (!rm.has_mem_ref() && what == 4) {
    if (rm.ea_reg != 0) {
      raise(VEC_UD, 0, "invalid DF E1-E7");
    }
    this->regs.w_ax(this->fpu_state.status_word()); // fnstsw ax (non-waiting)
    return;
  }

  this->check_fpu_pending();
  this->fpu_note_instruction(((opcode & 7) << 8) | (rm.has_mem_ref() ? (what << 3) : (0xC0 | (what << 3) | rm.ea_reg)));
  FPU fpu{this->fpu_state};

  if (!rm.has_mem_ref()) {
    uint8_t i = rm.ea_reg;
    switch (what) {
      case 0: // ffreep ST(i)
        this->fpu_state.empty |= (1 << fpu.phys(i));
        fpu.pop();
        return;
      case 1: { // fxch7 alias
        bool e0 = fpu.is_empty(0), ei = fpu.is_empty(i);
        if ((e0 || ei) && !fpu.stack_fault(false)) {
          return;
        }
        FReg a = e0 ? INDEFINITE : fpu.st(0);
        FReg b = ei ? INDEFINITE : fpu.st(i);
        fpu.set(0, b);
        fpu.set(i, a);
        return;
      }
      case 2: // fstp8/fstp9 aliases
      case 3: {
        FReg v;
        if (fpu.is_empty(0)) {
          if (!fpu.stack_fault(false)) {
            return;
          }
          v = INDEFINITE;
        } else {
          v = fpu.st(0);
        }
        fpu.set(i, v);
        fpu.pop();
        return;
      }
      case 5: // fucomip
      case 6: { // fcomip
        Ext a, b;
        uint32_t zpc = Regs::ZF | Regs::PF | Regs::CF;
        if (!(fpu.read(0, a) && fpu.read(i, b))) {
          if (fpu.commit) {
            this->regs.replace_flags(zpc | Regs::OF | Regs::SF | Regs::AF, zpc);
            fpu.pop();
          }
          return;
        }
        uint64_t rflags;
        uint16_t sw = (what == 5) ? h_fucomi(fpu.host_env(), a, b, rflags) : h_fcomi(fpu.host_env(), a, b, rflags);
        // As for fcomi: EFLAGS are written even when an unmasked exception suppresses the pop.
        bool ok = fpu.apply(sw, SW_C1);
        this->regs.replace_flags(zpc | Regs::OF | Regs::SF | Regs::AF, static_cast<uint32_t>(rflags) & zpc);
        if (ok) {
          fpu.pop();
        }
        return;
      }
      default:
        raise(VEC_UD, 0, "invalid DF register form");
    }
  }

  switch (what) {
    case 0: { // fild m16int
      uint32_t addr = this->fpu_mem_linear(rm, 2, false);
      int16_t v = this->r_mem<uint16_t>(addr);
      Ext r;
      fpu.push_loaded(h_fild16(fpu.host_env(), v, r), r);
      return;
    }
    case 5: { // fild m64int
      uint32_t addr = this->fpu_mem_linear(rm, 8, false);
      int64_t v = this->r_mem<uint64_t>(addr);
      Ext r;
      fpu.push_loaded(h_fild64(fpu.host_env(), v, r), r);
      return;
    }
    case 4: { // fbld m80bcd
      uint32_t addr = this->fpu_mem_linear(rm, 10, false);
      Bcd v;
      for (uint8_t z = 0; z < 10; z++) {
        v.b[z] = this->r_mem<uint8_t>(addr + z);
      }
      Ext r;
      fpu.push_loaded(h_fbld(fpu.host_env(), v, r), r);
      return;
    }
    case 1: // fisttp m16int
    case 2: // fist m16int
    case 3: { // fistp m16int
      uint32_t addr = this->fpu_mem_linear(rm, 2, true);
      Ext a;
      int16_t out;
      if (!fpu.read(0, a)) {
        if (!fpu.commit) {
          return;
        }
        out = INT16_MIN;
      } else {
        uint16_t sw = (what == 1) ? h_fisttp16(fpu.host_env(), a, out) : h_fist16(fpu.host_env(), a, out);
        if (!fpu.apply(sw)) {
          return;
        }
      }
      this->w_mem<uint16_t>(addr, out);
      if (what != 2) {
        fpu.pop();
      }
      return;
    }
    case 6: { // fbstp m80bcd
      uint32_t addr = this->fpu_mem_linear(rm, 10, true);
      Ext a;
      Bcd out;
      if (!fpu.read(0, a)) {
        if (!fpu.commit) {
          return;
        }
        static const uint8_t bcd_indefinite[10] = {0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0xFF};
        memcpy(out.b, bcd_indefinite, 10);
      } else if (!fpu.apply(h_fbstp(fpu.host_env(), a, out))) {
        return;
      }
      for (uint8_t z = 0; z < 10; z++) {
        this->w_mem<uint8_t>(addr + z, out.b[z]);
      }
      fpu.pop();
      return;
    }
    default: { // 7: fistp m64int
      uint32_t addr = this->fpu_mem_linear(rm, 8, true);
      Ext a;
      int64_t out;
      if (!fpu.read(0, a)) {
        if (!fpu.commit) {
          return;
        }
        out = INT64_MIN;
      } else if (!fpu.apply(h_fist64(fpu.host_env(), a, out))) {
        return;
      }
      this->w_mem<uint64_t>(addr, out);
      fpu.pop();
      return;
    }
  }
}

} // namespace adw::cpu
