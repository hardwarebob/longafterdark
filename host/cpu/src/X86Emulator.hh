// Vendored from resource_dasm: https://github.com/swannman/resource_dasm
// (branch afterdark-perf, commit 02d8ea9a58eaf559a9194601b33d98723c9d4f60,
// file src/Emulators/X86Emulator.hh), itself a fork of fuzziqersoftware/resource_dasm.
// MIT, (c) Martin Michelsen; modified for Long After Dark.
// See ../LICENSE.resource_dasm for the license text.
//
// Long After Dark changes (docs/DESIGN.md §2):
//  * namespace adw::cpu;
//  * segmentation: segment registers with descriptor caches, a host-supplied DescriptorProvider (the host owns the
//    LDT), default operand/address size from CS.D, 16-bit ModRM addressing, SS.B stack width, far transfers,
//    segment loads, LAR/LSL/VERR/VERW/ARPL; flat mode skips all of it (identity segments);
//  * faults (#DE/#BR/#UD/#NP/#SS/#GP/#PF/#MF) are reported to a host callback with the faulting CS:EIP instead of
//    being thrown as bare runtime_errors;
//  * the x87 FPU (X87.cc) and the integer forms upstream left unimplemented;
//  * run()/run_until() entry points for bounded and nested (callback) execution.

#pragma once

#include <stdint.h>
#include <stdio.h>

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <phosg/Strings.hh>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>

#include "EmulatorBase.hh"
#include "Expression.hh"
#include "InterruptManager.hh"
#include "MemoryContext.hh"

namespace adw::cpu {

// A segment descriptor as the host describes it. `limit` is the highest valid offset (inclusive, byte-granular; what
// LSL returns), so a 64 KiB segment has limit 0xFFFF and a flat one 0xFFFFFFFF. `big` is the D/B bit: 32-bit default
// operand/address size for code, ESP (vs SP) for a stack segment. `readable_or_writable` is R for code, W for data.
struct SegDesc {
  uint32_t base = 0, limit = 0;
  bool present = false, code = false, readable_or_writable = false, big = false;
  uint8_t dpl = 0;
};

// Supplied by the host; the CPU calls it whenever a selector is loaded into a segment register or inspected
// (LAR/LSL/VERR/VERW). Returning false means "no such descriptor" (#GP on load, ZF=0 for the inspectors).
class DescriptorProvider {
public:
  virtual ~DescriptorProvider() = default;
  virtual bool lookup(uint16_t sel, SegDesc& out) = 0;
};

class X86Emulator : public EmulatorBase {
public:
  static constexpr bool is_little_endian = true;

  // Prefix / disassembly view of the segment registers (upstream).
  enum class Segment {
    NONE = 0,
    CS,
    DS,
    ES,
    FS,
    GS,
    SS,
  };
  static const char* name_for_segment(Segment segment);

  // Segment registers in their architectural encoding order (the reg field of mov Sreg / the sreg2 of push/pop).
  enum class SegReg : uint8_t {
    ES = 0,
    CS = 1,
    SS = 2,
    DS = 3,
    FS = 4,
    GS = 5,
  };

  // Exception vectors the CPU can report to the fault handler.
  static constexpr uint8_t VEC_DE = 0; // divide error
  static constexpr uint8_t VEC_DB = 1; // icebp (F1)
  static constexpr uint8_t VEC_OF = 4; // into with OF set
  static constexpr uint8_t VEC_BR = 5; // bound range exceeded
  static constexpr uint8_t VEC_UD = 6; // invalid (or unimplemented) opcode
  static constexpr uint8_t VEC_NP = 11; // segment not present
  static constexpr uint8_t VEC_SS = 12; // stack segment fault
  static constexpr uint8_t VEC_GP = 13; // general protection
  static constexpr uint8_t VEC_PF = 14; // emulated memory not mapped (the MemoryContext has no arena there)
  static constexpr uint8_t VEC_MF = 16; // unmasked x87 exception pending

  struct Fault {
    uint8_t vector = 0;
    uint32_t error_code = 0; // selector-style error code where the architecture defines one, else 0
    uint16_t cs = 0; // CS selector of the faulting instruction
    uint32_t eip = 0; // offset of the faulting instruction; the CPU has already rolled EIP (and ESP) back to it
    uint32_t address = 0; // linear address for #PF (0 when a host handler, not the guest, touched unmapped memory)
    std::string what;
    // #DE only: the divisor was zero (Windows reports STATUS_INTEGER_DIVIDE_BY_ZERO), as opposed to a quotient that
    // does not fit (STATUS_INTEGER_OVERFLOW). The architecture does not distinguish them; Windows does.
    bool divide_by_zero = false;

    std::string str() const;
  };

  // Thrown out of run()/execute() when a fault occurs and no fault handler is installed (or the handler keeps
  // returning without resolving it).
  class fault_error : public std::runtime_error {
  public:
    explicit fault_error(const Fault& f);
    Fault fault;
  };

  enum class StopReason {
    ADDRESS = 0, // reached the requested CS:EIP at an instruction boundary
    LIMIT, // executed the requested number of instructions
    REQUESTED, // a handler called request_stop()
  };

  struct Overrides {
    bool should_clear;
    Segment segment;
    // true = 16-bit operand/address size for the current instruction. Reset to CS.D's default before every
    // instruction; the 66/67 prefixes select the other size.
    bool operand_size;
    bool address_size;
    bool code16; // CS.D == 0 for the instruction being decoded
    bool wait;
    bool lock;
    // All opcodes for which rep/repe/repne (F2/F3) applies:
    // 6C/6D ins (rep)
    // 6E/6F outs (rep)
    // A4/A5 movs (rep)
    // AA/AB stos (rep)
    // AC/AD lods (rep)
    // A6/A7 cmps (repe/repne)
    // AE/AF scas (repe/repne)
    bool repeat_nz;
    bool repeat_z;

    Overrides() noexcept;
    std::string str() const;
    void on_opcode_complete();
    inline void reset(bool code16) {
      this->should_clear = true;
      this->segment = Segment::NONE;
      this->operand_size = code16;
      this->address_size = code16;
      this->code16 = code16;
      this->wait = false;
      this->lock = false;
      this->repeat_nz = false;
      this->repeat_z = false;
    }
    const char* overridden_segment_name() const;
  };

  class Regs {
  public:
    union IntReg {
      phosg::le_uint32_t u;
      phosg::le_int32_t s;
      phosg::le_uint16_t u16;
      phosg::le_int16_t s16;
      struct U8Fields {
        uint8_t l;
        uint8_t h;
      } __attribute__((packed));
      U8Fields u8;
      struct S8Fields {
        int8_t l;
        int8_t h;
      } __attribute__((packed));
      S8Fields s8;
    } __attribute__((packed));

    union XMMReg {
      // Because these are little-endian, the highest value is in the last entry; that is, u64[1] is the high half, and
      // u8[15] is the highest byte.
      uint8_t u8[16];
      phosg::le_uint16_t u16[8];
      phosg::le_uint32_t u32[4];
      phosg::le_uint64_t u64[2];
      int8_t s8[16];
      phosg::le_int16_t s16[8];
      phosg::le_int32_t s32[4];
      phosg::le_int64_t s64[2];
      phosg::le_float f32[4];
      phosg::le_double f64[2];

      XMMReg();
      XMMReg(uint32_t v);
      XMMReg(uint64_t v);
      XMMReg& operator=(uint32_t v);
      XMMReg& operator=(uint64_t v);

      operator uint32_t() const;
      operator uint64_t() const;

      inline void clear() {
        this->u64[0] = 0;
        this->u64[1] = 0;
      }

      template <typename T>
      T* as() {
        static_assert(sizeof(T) <= sizeof(XMMReg), "partition type is too large");
        static_assert((sizeof(XMMReg) % sizeof(T)) == 0, "partition type does not evenly divide xmm reg data");
        return reinterpret_cast<T*>(&this->u8[0]);
      }

      template <typename T>
      const T& lowest() const {
        static_assert(sizeof(T) <= sizeof(XMMReg), "partition type is too large");
        static_assert((sizeof(XMMReg) % sizeof(T)) == 0, "partition type does not evenly divide xmm reg data");
        return *reinterpret_cast<const T*>(&this->u8[0]);
      }
      template <typename T>
      const T& highest() const {
        static_assert(sizeof(T) <= sizeof(XMMReg), "partition type is too large");
        static_assert((sizeof(XMMReg) % sizeof(T)) == 0, "partition type does not evenly divide xmm reg data");
        const T* data = reinterpret_cast<const T*>(&this->u8[0]);
        return data[(sizeof(XMMReg) / sizeof(T)) - 1];
      }
    } __attribute__((packed));

    union {
      uint32_t eip;
      uint32_t pc;
    };

    Regs();

    inline IntReg& eax() { return this->regs[0]; }
    inline const IntReg& eax() const { return this->regs[0]; }
    inline IntReg& ecx() { return this->regs[1]; }
    inline const IntReg& ecx() const { return this->regs[1]; }
    inline IntReg& edx() { return this->regs[2]; }
    inline const IntReg& edx() const { return this->regs[2]; }
    inline IntReg& ebx() { return this->regs[3]; }
    inline const IntReg& ebx() const { return this->regs[3]; }
    inline IntReg& esp() { return this->regs[4]; }
    inline const IntReg& esp() const { return this->regs[4]; }
    inline IntReg& ebp() { return this->regs[5]; }
    inline const IntReg& ebp() const { return this->regs[5]; }
    inline IntReg& esi() { return this->regs[6]; }
    inline const IntReg& esi() const { return this->regs[6]; }
    inline IntReg& edi() { return this->regs[7]; }
    inline const IntReg& edi() const { return this->regs[7]; }

    // The register index is always masked to 0-7 by the decoder, so these accessors don't range-check (upstream
    // did, on every register access).
    inline uint8_t& reg8(uint8_t which) {
      return (which & 4) ? this->regs[which & 3].u8.h : this->regs[which & 3].u8.l;
    }
    inline phosg::le_uint16_t& reg16(uint8_t which) {
      return this->regs[which & 7].u16;
    }
    inline phosg::le_uint32_t& reg32(uint8_t which) {
      return this->regs[which & 7].u;
    }
    inline const uint8_t& reg8(uint8_t which) const {
      return (which & 4) ? this->regs[which & 3].u8.h : this->regs[which & 3].u8.l;
    }
    inline const phosg::le_uint16_t& reg16(uint8_t which) const {
      return this->regs[which & 7].u16;
    }
    inline const phosg::le_uint32_t& reg32(uint8_t which) const {
      return this->regs[which & 7].u;
    }

    phosg::le_uint32_t& xmm32(uint8_t which);
    phosg::le_uint64_t& xmm64(uint8_t which);
    XMMReg& xmm128(uint8_t which);
    const phosg::le_uint32_t& xmm32(uint8_t which) const;
    const phosg::le_uint64_t& xmm64(uint8_t which) const;
    const XMMReg& xmm128(uint8_t which) const;

    uint32_t read(uint8_t which, uint8_t size) const;
    XMMReg read_xmm(uint8_t which, uint8_t size) const;

    template <typename T>
    T read(uint8_t which) const {
      if constexpr (phosg::bits_for_type<T> == 8) {
        return this->reg8(which);
      } else if constexpr (phosg::bits_for_type<T> == 16) {
        return this->reg16(which).load();
      } else if constexpr (phosg::bits_for_type<T> == 32) {
        return this->reg32(which).load();
      } else {
        throw std::logic_error("invalid register size");
      }
    }
    template <typename T>
    T read_xmm(uint8_t which) const {
      if constexpr (phosg::bits_for_type<T> == 32) {
        return this->xmm32(which).load();
      } else if constexpr (phosg::bits_for_type<T> == 64) {
        return this->xmm64(which).load();
      } else if constexpr (phosg::bits_for_type<T> == 128) {
        return this->xmm128(which).lowest<T>();
      } else {
        throw std::logic_error("invalid register size");
      }
    }
    template <typename T>
    void write(uint8_t which, T value) {
      if constexpr (phosg::bits_for_type<T> == 8) {
        this->reg8(which) = value;
      } else if constexpr (phosg::bits_for_type<T> == 16) {
        this->reg16(which).store(value);
      } else if constexpr (phosg::bits_for_type<T> == 32) {
        this->reg32(which).store(value);
      } else {
        throw std::logic_error("invalid register size");
      }
    }
    template <typename T>
    void write_xmm(uint8_t which, T value) {
      if constexpr (phosg::bits_for_type<T> == 32) {
        this->xmm32(which).store(value);
      } else if constexpr (phosg::bits_for_type<T> == 64) {
        this->xmm64(which).store(value);
      } else if constexpr (phosg::bits_for_type<T> == 128) {
        this->xmm128(which) = value;
      } else {
        throw std::logic_error("invalid register size");
      }
    }

    inline uint8_t read8(uint8_t which) const { return this->read<uint8_t>(which); }
    inline uint16_t read16(uint8_t which) const { return this->read<phosg::le_uint16_t>(which); }
    inline uint32_t read32(uint8_t which) const { return this->read<phosg::le_uint32_t>(which); }
    inline uint32_t read_xmm32(uint8_t which) const { return this->read_xmm<phosg::le_uint32_t>(which); }
    inline uint64_t read_xmm64(uint8_t which) const { return this->read_xmm<phosg::le_uint64_t>(which); }
    inline XMMReg read_xmm128(uint8_t which) const { return this->read_xmm<XMMReg>(which); }
    inline void write8(uint8_t which, uint8_t v) { this->write<uint8_t>(which, v); }
    inline void write16(uint8_t which, phosg::le_uint16_t v) { this->write<phosg::le_uint16_t>(which, v); }
    inline void write32(uint8_t which, phosg::le_uint32_t v) { this->write<phosg::le_uint32_t>(which, v); }
    inline void write_xmm32(uint8_t which, uint32_t v) { this->write_xmm<phosg::le_uint32_t>(which, v); }
    inline void write_xmm64(uint8_t which, uint64_t v) { this->write_xmm<phosg::le_uint64_t>(which, v); }
    inline void write_xmm128(uint8_t which, const XMMReg& v) { this->write_xmm<XMMReg>(which, v); }

    inline uint8_t r_al() const { return this->read<uint8_t>(0); }
    inline uint8_t r_cl() const { return this->read<uint8_t>(1); }
    inline uint8_t r_dl() const { return this->read<uint8_t>(2); }
    inline uint8_t r_bl() const { return this->read<uint8_t>(3); }
    inline uint8_t r_ah() const { return this->read<uint8_t>(4); }
    inline uint8_t r_ch() const { return this->read<uint8_t>(5); }
    inline uint8_t r_dh() const { return this->read<uint8_t>(6); }
    inline uint8_t r_bh() const { return this->read<uint8_t>(7); }
    inline uint16_t r_ax() const { return this->read<phosg::le_uint16_t>(0); }
    inline uint16_t r_cx() const { return this->read<phosg::le_uint16_t>(1); }
    inline uint16_t r_dx() const { return this->read<phosg::le_uint16_t>(2); }
    inline uint16_t r_bx() const { return this->read<phosg::le_uint16_t>(3); }
    inline uint16_t r_sp() const { return this->read<phosg::le_uint16_t>(4); }
    inline uint16_t r_bp() const { return this->read<phosg::le_uint16_t>(5); }
    inline uint16_t r_si() const { return this->read<phosg::le_uint16_t>(6); }
    inline uint16_t r_di() const { return this->read<phosg::le_uint16_t>(7); }
    inline uint32_t r_eax() const { return this->read<phosg::le_uint32_t>(0); }
    inline uint32_t r_ecx() const { return this->read<phosg::le_uint32_t>(1); }
    inline uint32_t r_edx() const { return this->read<phosg::le_uint32_t>(2); }
    inline uint32_t r_ebx() const { return this->read<phosg::le_uint32_t>(3); }
    inline uint32_t r_esp() const { return this->read<phosg::le_uint32_t>(4); }
    inline uint32_t r_ebp() const { return this->read<phosg::le_uint32_t>(5); }
    inline uint32_t r_esi() const { return this->read<phosg::le_uint32_t>(6); }
    inline uint32_t r_edi() const { return this->read<phosg::le_uint32_t>(7); }

    inline void w_al(uint8_t v) { this->write<uint8_t>(0, v); }
    inline void w_cl(uint8_t v) { this->write<uint8_t>(1, v); }
    inline void w_dl(uint8_t v) { this->write<uint8_t>(2, v); }
    inline void w_bl(uint8_t v) { this->write<uint8_t>(3, v); }
    inline void w_ah(uint8_t v) { this->write<uint8_t>(4, v); }
    inline void w_ch(uint8_t v) { this->write<uint8_t>(5, v); }
    inline void w_dh(uint8_t v) { this->write<uint8_t>(6, v); }
    inline void w_bh(uint8_t v) { this->write<uint8_t>(7, v); }
    inline void w_ax(uint16_t v) { this->write<phosg::le_uint16_t>(0, v); }
    inline void w_cx(uint16_t v) { this->write<phosg::le_uint16_t>(1, v); }
    inline void w_dx(uint16_t v) { this->write<phosg::le_uint16_t>(2, v); }
    inline void w_bx(uint16_t v) { this->write<phosg::le_uint16_t>(3, v); }
    inline void w_sp(uint16_t v) { this->write<phosg::le_uint16_t>(4, v); }
    inline void w_bp(uint16_t v) { this->write<phosg::le_uint16_t>(5, v); }
    inline void w_si(uint16_t v) { this->write<phosg::le_uint16_t>(6, v); }
    inline void w_di(uint16_t v) { this->write<phosg::le_uint16_t>(7, v); }
    inline void w_eax(uint32_t v) { this->write<phosg::le_uint32_t>(0, v); }
    inline void w_ecx(uint32_t v) { this->write<phosg::le_uint32_t>(1, v); }
    inline void w_edx(uint32_t v) { this->write<phosg::le_uint32_t>(2, v); }
    inline void w_ebx(uint32_t v) { this->write<phosg::le_uint32_t>(3, v); }
    inline void w_esp(uint32_t v) { this->write<phosg::le_uint32_t>(4, v); }
    inline void w_ebp(uint32_t v) { this->write<phosg::le_uint32_t>(5, v); }
    inline void w_esi(uint32_t v) { this->write<phosg::le_uint32_t>(6, v); }
    inline void w_edi(uint32_t v) { this->write<phosg::le_uint32_t>(7, v); }

    inline uint32_t read_eflags() const {
      return this->eflags;
    }
    inline void write_eflags(uint32_t v) {
      this->eflags = v;
    }

    void set_by_name(const std::string& reg_name, uint32_t value);

    inline uint32_t get_sp() const {
      return this->r_esp();
    }
    inline void set_sp(uint32_t sp) {
      this->w_esp(sp);
    }

    static constexpr uint32_t CF = 0x0001;
    static constexpr uint32_t PF = 0x0004;
    static constexpr uint32_t AF = 0x0010;
    static constexpr uint32_t ZF = 0x0040;
    static constexpr uint32_t SF = 0x0080;
    static constexpr uint32_t TF = 0x0100;
    static constexpr uint32_t IF = 0x0200;
    static constexpr uint32_t DF = 0x0400;
    static constexpr uint32_t OF = 0x0800;
    static constexpr uint32_t default_int_flags = CF | PF | AF | ZF | SF | OF;

    inline bool read_flag(uint32_t mask) const {
      return this->eflags & mask;
    }
    inline void replace_flag(uint32_t mask, bool value) {
      this->eflags = (this->eflags & ~mask) | (value ? mask : 0);
    }
    // Replaces the flags in `mask` with the corresponding bits of `values`.
    inline void replace_flags(uint32_t mask, uint32_t values) {
      this->eflags = (this->eflags & ~mask) | (values & mask);
    }

    static std::string flags_str(uint32_t eflags);
    std::string flags_str() const;

    template <typename T>
      requires(std::is_unsigned_v<T>)
    void set_flags_integer_result(T res, uint32_t apply_mask = Regs::default_int_flags);
    template <typename T>
      requires(std::is_unsigned_v<T>)
    void set_flags_bitwise_result(T res, uint32_t apply_mask = Regs::default_int_flags);
    template <typename T>
      requires(std::is_unsigned_v<T>)
    T set_flags_integer_add(T a, T b, uint32_t apply_mask = Regs::default_int_flags);
    template <typename T>
      requires(std::is_unsigned_v<T>)
    T set_flags_integer_add_with_carry(T a, T b, uint32_t apply_mask = Regs::default_int_flags);
    template <typename T>
      requires(std::is_unsigned_v<T>)
    T set_flags_integer_subtract(T a, T b, uint32_t apply_mask = Regs::default_int_flags);
    template <typename T>
      requires(std::is_unsigned_v<T>)
    T set_flags_integer_subtract_with_borrow(T a, T b, uint32_t apply_mask = Regs::default_int_flags);

    bool check_condition(uint8_t cc) const;

    void import_state(FILE* stream);
    void export_state(FILE* stream) const;

  private:
    IntReg regs[8];
    XMMReg xmm[8];

    uint32_t eflags;
  };

  // The x87 register file. Registers are 80-bit extended values stored as (64-bit significand, sign+exponent).
  struct FPUState {
    struct Reg {
      uint64_t mant = 0;
      uint16_t se = 0;
    };
    std::array<Reg, 8> r; // physical registers R0-R7; ST(i) is r[(top + i) & 7]
    uint16_t cw = 0x037F;
    uint16_t sw = 0; // the TOP field (bits 11-13) lives in `top` and is merged in on read
    uint8_t top = 0;
    uint8_t empty = 0xFF; // bit i set = physical register i is empty (tag 11)
    // Last non-control instruction / operand pointers, reported by fnstenv/fnsave.
    uint32_t fip = 0, fdp = 0;
    uint16_t fcs = 0, fds = 0, fop = 0;

    inline uint16_t status_word() const {
      return (this->sw & ~0x3800) | ((this->top & 7) << 11);
    }
    uint16_t tag_word() const; // full 2-bit tags computed from contents, as fnstenv/fnsave store them
    void reset(); // finit
  };

  explicit X86Emulator(std::shared_ptr<MemoryContext> mem);
  virtual ~X86Emulator() = default;

  virtual void import_state(FILE* stream);
  virtual void export_state(FILE* stream) const;

  inline Regs& registers() {
    return this->regs;
  }
  inline const Regs& registers() const {
    return this->regs;
  }
  inline FPUState& fpu() {
    return this->fpu_state;
  }
  inline const FPUState& fpu() const {
    return this->fpu_state;
  }
  // Writes the 108-byte image fnsave would store in 32-bit protected mode (without resetting the FPU).
  void fpu_save_image(void* out) const;

  virtual void print_state_header(FILE* stream) const;
  virtual void print_state(FILE* stream) const;

  static std::string disassemble(
      const void* vdata,
      size_t size,
      uint32_t start_address = 0,
      const std::multimap<uint32_t, std::string>* labels = nullptr,
      bool include_hex = true,
      bool code16 = false);
  static DisassembleResult disassemble_structured(
      const void* vdata,
      size_t size,
      uint32_t start_address = 0,
      const std::multimap<uint32_t, std::string>* labels = nullptr);

  static AssembleResult assemble(
      const std::string& text, std::function<std::string(const std::string&)> get_include = nullptr,
      uint32_t start_address = 0);
  static AssembleResult assemble(
      const std::string& text, const std::vector<std::string>& include_dirs, uint32_t start_address = 0);

  static bool test_assembler(const std::string& start_opcode = "", bool stop_on_failure = false, bool verbose = false);

  // NOTE: If the storage size of this enum changes, the format versions implemented in import_state and export_state
  // must also change.
  enum class Behavior : uint8_t {
    // Default behavior is to emulate an x86 CPU implemented according to Intel's manuals. Flags the manual leaves
    // undefined get whatever the implementation computes (deterministically); tests mask them.
    SPECIFICATION = 0,
    // Behave like the CPU emulator implemented in Windows 11 for ARM64 systems (upstream; only a few of upstream's
    // special cases survive the Long After Dark rewrite of the integer core).
    WINDOWS_ARM_EMULATOR,
  };

  inline Behavior get_behavior() const {
    return this->behavior;
  }
  inline void set_behavior(Behavior b) {
    this->behavior = b;
  }
  virtual void set_behavior_by_name(const std::string& name);

  virtual void set_time_base(uint64_t time_base);
  virtual void set_time_base(const std::vector<uint64_t>& time_overrides);

  // ---- Handlers -------------------------------------------------------------------------------------------------

  // `int n` / `int3` (vector 3): called with EIP already past the instruction. The handler may change any register
  // (including CS:EIP via set_cs_eip / set_segment, and SS:ESP). Without a handler, int n is a #GP fault.
  inline void set_syscall_handler(std::function<void(X86Emulator&, uint8_t)> handler) {
    this->syscall_handler = handler;
  }
  inline void set_interrupt_handler(std::function<void(X86Emulator&, uint8_t)> handler) {
    this->syscall_handler = handler;
  }
  // Called for every fault, with EIP/ESP rolled back to the faulting instruction. The handler may fix the cause and
  // return (the instruction is retried), redirect CS:EIP and return, call request_stop(), or throw (the exception
  // propagates out of run()). Without a handler, faults throw fault_error.
  inline void set_fault_handler(std::function<void(X86Emulator&, const Fault&)> handler) {
    this->fault_handler = handler;
  }
  // in/out (not ins/outs). For reads the return value is the port value; for writes it is ignored. Without a
  // handler, port I/O is a #GP fault (as it is for ring-3 code with IOPL 0).
  inline void set_port_handler(
      std::function<uint32_t(X86Emulator&, uint16_t port, uint8_t size, bool is_write, uint32_t value)> handler) {
    this->port_handler = handler;
  }
  inline void set_debug_hook(std::function<void(X86Emulator&)> hook) {
    this->debug_hook = hook;
  }

  // ---- Segmentation -----------------------------------------------------------------------------------------------

  // Win32 lane: CS/DS/ES/SS become base-0 4 GiB big segments (selectors as on Windows NT), FS a data segment based
  // at the TEB (limit fs_limit), GS null. Segment loads of these selectors keep working without a provider.
  void set_flat_mode(uint32_t fs_base = 0, uint32_t fs_limit = 0xFFF);
  // The host-owned descriptor table (not owned by the emulator; must outlive its use). nullptr = none.
  inline void set_descriptor_provider(DescriptorProvider* provider) {
    this->descriptor_provider = provider;
  }
  inline DescriptorProvider* get_descriptor_provider() const {
    return this->descriptor_provider;
  }
  // Architectural load through the provider, with the protected-mode checks of mov Sreg / far jmp (for CS). Called
  // from outside run(), a load that would fault throws fault_error; called from a handler during run(), the fault is
  // reported against the instruction being executed (the int n that invoked the handler), as a guest fault.
  void load_segment(SegReg seg, uint16_t sel);
  // Raw load: sets the selector and descriptor cache as given, no checks.
  void set_segment(SegReg seg, uint16_t sel, const SegDesc& desc);
  // Like the CPU, the emulator caches each segment register's descriptor when the selector is loaded, so a host that
  // changes an LDT entry (a moved or resized segment) must call this to refresh the caches of the selectors currently
  // loaded (selectors the provider no longer knows keep their old cache).
  void reload_segments();
  // Sets a segment register to the null selector (DS/ES/FS/GS only).
  void set_segment_null(SegReg seg);
  inline uint16_t get_segment(SegReg seg) const {
    return this->segs[static_cast<uint8_t>(seg)].sel;
  }
  inline const SegDesc& get_segment_desc(SegReg seg) const {
    return this->segs[static_cast<uint8_t>(seg)].desc;
  }
  // Loads CS (through the provider) and EIP together, as a far jump would.
  void set_cs_eip(uint16_t cs, uint32_t eip);
  inline bool is_code16() const {
    return this->code16;
  }
  inline bool is_stack16() const {
    return this->stack16;
  }
  // Linear address of seg:offset with the access checks a guest access would get. Throws fault_error.
  uint32_t linear_address(SegReg seg, uint32_t offset, uint32_t size = 1, bool write = false);

  // Guest-visible memory access through a segment (host helpers, e.g. reading Pascal arguments at SS:SP+4). A failing
  // access (limit, null selector, unmapped memory as #PF with its address) throws fault_error outside run(); from a
  // handler it becomes a guest fault at the current instruction, like load_segment.
  template <typename T>
  T read_seg(SegReg seg, uint32_t offset) {
    try {
      return this->r_mem<T>(this->lin_data(static_cast<uint8_t>(seg), offset, sizeof(T), false));
    } catch (const CPUFault& f) {
      this->host_fault(f);
    }
  }
  template <typename T>
  void write_seg(SegReg seg, uint32_t offset, T value) {
    try {
      this->w_mem<T>(this->lin_data(static_cast<uint8_t>(seg), offset, sizeof(T), true), value);
    } catch (const CPUFault& f) {
      this->host_fault(f);
    }
  }

  // ---- Execution --------------------------------------------------------------------------------------------------

  virtual void execute_one(); // one instruction; faults go to the fault handler
  virtual void execute(); // until the syscall handler / debug hook throws terminate_emulation (upstream)
  // Run until max_instructions have executed or a handler calls request_stop().
  StopReason run(uint64_t max_instructions);
  // Run until EIP == eip (and, in the second form, CS == cs) at an instruction boundary. Re-entrant: a handler may
  // call run_until for a nested callback into emulated code; each level has its own stop address.
  StopReason run_until(uint32_t eip, uint64_t max_instructions = UINT64_MAX);
  StopReason run_until(uint16_t cs, uint32_t eip, uint64_t max_instructions = UINT64_MAX);
  // Makes the innermost run()/run_until() return REQUESTED after the current instruction.
  inline void request_stop() {
    this->stop_requested = true;
  }

  // Stack access through SS for host code (argument pushes before a callback, Pascal argument pops), honoring SS.B
  // (SP vs ESP) and the segment limit. Failures are reported as for read_seg: fault_error outside run(), a guest fault
  // from inside a handler. (The internal CPUFault must never reach host code: it is not a std::exception.)
  template <typename T>
  void push(T value) {
    try {
      this->push_g<T>(value);
    } catch (const CPUFault& f) {
      this->host_fault(f);
    }
  }
  template <typename T>
  T pop() {
    try {
      return this->pop_g<T>();
    } catch (const CPUFault& f) {
      this->host_fault(f);
    }
  }

protected:
  // The instruction-level stack operations (faults propagate as CPUFault to the run loop).
  template <typename T>
  void push_g(T value) {
    uint32_t esp = this->regs.r_esp();
    if (!this->stack16) {
      esp -= sizeof(T);
      this->w_mem<T>(this->lin_data(SEG_SS, esp, sizeof(T), true), value);
      this->regs.w_esp(esp);
    } else {
      uint16_t sp = esp - sizeof(T);
      this->w_mem<T>(this->lin_stack16(sp, sizeof(T)), value);
      this->regs.w_sp(sp);
    }
  }
  template <typename T>
  T pop_g() {
    uint32_t esp = this->regs.r_esp();
    if (!this->stack16) {
      T ret = this->r_mem<T>(this->lin_data(SEG_SS, esp, sizeof(T), false));
      this->regs.w_esp(esp + sizeof(T));
      return ret;
    } else {
      uint16_t sp = esp;
      T ret = this->r_mem<T>(this->lin_stack16(sp, sizeof(T)));
      this->regs.w_sp(sp + sizeof(T));
      return ret;
    }
  }

  static constexpr uint8_t SEG_ES = 0;
  static constexpr uint8_t SEG_CS = 1;
  static constexpr uint8_t SEG_SS = 2;
  static constexpr uint8_t SEG_DS = 3;
  static constexpr uint8_t SEG_FS = 4;
  static constexpr uint8_t SEG_GS = 5;

  struct SegState {
    uint16_t sel = 0;
    SegDesc desc;
    bool null = true; // null selector loaded (any access #GPs)
    // Base 0, limit 4 GiB, usable: accesses need no base add and no checks. This is what keeps flat mode as fast as
    // upstream (which had no segmentation at all).
    bool identity = false;
  };

  // Raised inside instruction execution; caught by the run loop, which rolls EIP/ESP back and reports it.
  struct CPUFault {
    uint8_t vector;
    uint32_t error_code;
    uint32_t address;
    const char* what;
    bool divide_by_zero = false;
  };
  [[noreturn]] static void raise(uint8_t vector, uint32_t error_code = 0, const char* what = nullptr);
  // #DE with a zero divisor (see Fault::divide_by_zero).
  [[noreturn]] static void raise_divide_by_zero(const char* what);
  // #PF for a guest access to a linear address no MemoryContext arena maps.
  [[noreturn]] static void raise_unmapped(uint32_t addr);
  // A fault caused by a host-facing call (load_segment, push, read_seg, ...): inside a run loop it is rethrown as the
  // CPUFault the loop reports against the current instruction; outside one it becomes fault_error.
  [[noreturn]] void host_fault(const CPUFault& f) const;

  Regs regs;
  Behavior behavior;
  uint64_t tsc_offset;
  std::deque<uint64_t> tsc_overrides;

  Overrides overrides;
  std::function<void(X86Emulator&, uint8_t)> syscall_handler;
  std::function<void(X86Emulator&, const Fault&)> fault_handler;
  std::function<uint32_t(X86Emulator&, uint16_t, uint8_t, bool, uint32_t)> port_handler;
  std::function<void(X86Emulator&)> debug_hook;

  SegState segs[6];
  DescriptorProvider* descriptor_provider = nullptr;
  // Descriptors set_flat_mode installed, so flat code can reload DS/ES/SS/FS without a provider.
  std::vector<std::pair<uint16_t, SegDesc>> flat_descriptors;
  bool code16 = false; // CS.D == 0
  bool stack16 = false; // SS.B == 0

  FPUState fpu_state;

  // Per-instruction state for fault rollback.
  uint32_t insn_eip = 0;
  uint32_t insn_esp = 0;
  bool stop_requested = false;
  uint32_t run_depth = 0; // nesting of run loops (host callbacks into guest code)
  // The unresolved-fault guard: the last fault and the state it left, so a retry that made no progress is recognized.
  uint32_t repeated_fault_count = 0;
  uint32_t last_fault_eip = 0;
  uint16_t last_fault_cs = 0;
  uint64_t last_fault_icount = 0;
  uint8_t last_fault_vector = 0;
  uint32_t last_fault_error_code = 0;
  uint32_t last_fault_address = 0;
  uint32_t last_fault_ecx = 0, last_fault_esi = 0, last_fault_edi = 0;

  mutable bool execution_labels_computed;
  mutable std::multimap<uint32_t, std::string> execution_labels;

  void compute_execution_labels() const;

  struct DecodedRM {
    int8_t non_ea_reg = 0;
    int8_t ea_reg = -1; // -1 = no reg
    int8_t ea_index_reg = -1; // -1 = no reg (also ea_index_scale should be -1 or 0)
    int8_t ea_index_scale = -1; // -1 (ea_reg is not to be dereferenced), 0 (no index reg), 1, 2, 4, or 8
    int32_t ea_disp = 0;
    bool has_disp8 = false;
    bool has_disp32 = false;
    bool has_disp16 = false;
    bool addr16 = false; // 16-bit addressing form ([bx+si] etc.); the offset wraps at 64 KiB
    uint8_t seg = SEG_DS; // segment register the memory operand goes through (override or DS/SS default)

    DecodedRM() = default;
    DecodedRM(int8_t ea_reg, int32_t ea_disp);

    inline bool has_mem_ref() const {
      return (this->ea_index_scale != -1);
    }

    enum StrFlags {
      EA_FIRST = 0x01,
      EA_ST = 0x02,
      NON_EA_ST = 0x04,
      EA_XMM = 0x08,
      NON_EA_XMM = 0x10,
      SUPPRESS_OPERAND_SIZE = 0x20,
      SUPPRESS_ADDRESS_TOKEN = 0x40,
    };

    std::string ea_str(uint8_t operand_size, uint8_t flags, Segment override_segment) const;
    std::string non_ea_str(uint8_t operand_size, uint8_t flags) const;
  };

  using RMF = DecodedRM::StrFlags;

  struct DisassemblyState {
    phosg::StringReader r;
    uint32_t start_address;
    bool include_hex;
    uint8_t opcode;
    std::set<std::pair<uint32_t, size_t>> imm_offsets;
    Overrides overrides;
    std::map<uint32_t, LabelRefs> branch_refs;
    const std::multimap<uint32_t, std::string>* labels;
    // If not null, the emulator pointer is used for resolving EA addresses based on the emulator's current state (for
    // use in the interactive debugging shell)
    const X86Emulator* emu;

    uint8_t standard_operand_size() const;

    std::string rm_ea_str(const DecodedRM& rm, uint8_t operand_size, uint8_t flags) const;
    std::string rm_non_ea_str(const DecodedRM& rm, uint8_t operand_size, uint8_t flags) const;
    std::string rm_str(const DecodedRM& rm, uint8_t operand_size, uint8_t flags) const;
    std::string rm_str(const DecodedRM& rm, uint8_t ea_operand_size, uint8_t non_ea_operand_size, uint8_t flags) const;

    std::string annotation_for_rm_ea(const DecodedRM& rm, int64_t operand_size, uint8_t flags = 0) const;
  };

  // ---- Segment helpers ----
  static uint8_t seg_index(Segment s);
  void update_mode(); // recomputes code16/stack16 after CS/SS change
  void refresh_identity(uint8_t seg);
  // Descriptor lookup for guest loads; returns false if no provider knows the selector.
  bool lookup_descriptor(uint16_t sel, SegDesc& out) const;
  // Guest segment-register load (mov/pop Sreg, lds & co.). Raises #GP/#NP/#SS as the architecture would.
  void load_data_segment(uint8_t seg, uint16_t sel);
  // Validates a far-transfer target; returns its descriptor (raises on failure). Does not commit.
  SegDesc check_code_segment(uint16_t sel, uint32_t eip);
  void commit_cs(uint16_t sel, const SegDesc& desc, uint32_t eip);
  uint32_t lin_data_slow(uint8_t seg, uint32_t off, uint32_t size, bool write) const;
  inline uint32_t lin_data(uint8_t seg, uint32_t off, uint32_t size, bool write) const {
    const auto& s = this->segs[seg];
    if (s.identity) [[likely]] {
      return off;
    }
    return this->lin_data_slow(seg, off, size, write);
  }
  // A 16-bit stack access at SP (the caller has already wrapped SP; a word at SP=0xFFFF straddles the wrap).
  uint32_t lin_stack16(uint16_t sp, uint32_t size) const;
  inline uint32_t lin_code(uint32_t off, uint32_t size) const {
    const auto& s = this->segs[SEG_CS];
    if (s.identity) [[likely]] {
      return off;
    }
    return this->lin_code_slow(off, size);
  }
  uint32_t lin_code_slow(uint32_t off, uint32_t size) const;
  // Debug-only address translation (no checks, never faults).
  uint32_t debug_linear(uint8_t seg, uint32_t off) const;

  // Default data segment for the current instruction (DS unless overridden).
  inline uint8_t data_seg() const {
    return (this->overrides.segment == Segment::NONE) ? SEG_DS : seg_index(this->overrides.segment);
  }

  // The instruction fetch window: CS offsets [fw_off, fw_off + fw_len) are host bytes at fw_host — the part of one
  // arena inside the CS limit (MemoryContext::arena_span) — while fw_gen equals the memory's layout generation
  // (checked once per instruction, in execute_one_inner: only host code changes the layout, and it runs between
  // instructions or inside a thunk's, never between two fetches of one). Instruction bytes are read through it (live
  // memory: self-modifying code sees its own writes), so a fetch skips the segment check and the page lookup; any
  // other fetch takes the checked path and refills it. A CS change drops it (refresh_identity). Found by profiling:
  // those two steps were ~45% of a 16-bit module's host time.
  uint32_t fw_off = 0, fw_len = 0;
  const uint8_t* fw_host = nullptr;
  uint64_t fw_gen = 0;
  const uint64_t* mem_gen = nullptr;  // the memory's layout generation (MemoryContext::layout_generation_ptr)
  void refill_fetch_window(uint32_t eip, uint32_t linear);

  // The data windows: the last two arenas a data access (r_mem/w_mem, linear addresses: the segment checks happen
  // before) went to, as host spans — a module's data and its stack, or two segments' arenas. The same idea and
  // validity rule as the fetch window; writes still record their page's write generation.
  struct DataSpan {
    uint32_t lo = 0, len = 0;
    uint8_t* host = nullptr;
  };
  DataSpan dspan[2];
  uint64_t dspan_gen = 0;
  uint8_t dspan_next = 0;
  // Host pointer for `size` bytes at linear `addr`, or null (no arena holds them all: the caller's checked path).
  inline uint8_t* data_ptr(uint32_t addr, uint32_t size) {
    if (this->dspan_gen == *this->mem_gen) [[likely]] {
      for (const DataSpan& s : this->dspan) {
        uint32_t d = addr - s.lo;
        if ((d < s.len) && (s.len - d >= size)) {
          return s.host + d;
        }
      }
    } else {
      this->dspan[0].len = this->dspan[1].len = 0;
      this->dspan_gen = *this->mem_gen;
    }
    return this->refill_data_span(addr, size);
  }
  uint8_t* refill_data_span(uint32_t addr, uint32_t size);

  template <typename T>
  T fetch_instruction_data() {
    uint32_t eip = this->regs.eip;
    uint32_t d = eip - this->fw_off;
    T ret;
    if ((d < this->fw_len) && (this->fw_len - d >= sizeof(T))) [[likely]] {
      memcpy(&ret, this->fw_host + d, sizeof(T));
    } else {
      uint32_t addr = this->lin_code(eip, sizeof(T));
      try {
        ret = this->mem->read<T>(addr);
      } catch (const std::out_of_range&) {
        raise_unmapped(addr);
      }
      this->refill_fetch_window(eip, addr);
    }
    this->regs.eip = eip + sizeof(T);
    return ret;
  }

  inline uint8_t fetch_instruction_byte() {
    return this->fetch_instruction_data<uint8_t>();
  }
  inline uint16_t fetch_instruction_word() {
    return this->fetch_instruction_data<phosg::le_uint16_t>();
  }
  inline uint32_t fetch_instruction_dword() {
    return this->fetch_instruction_data<phosg::le_uint32_t>();
  }
  // Immediate of the current operand size (16 or 32 bits).
  inline uint32_t fetch_instruction_imm() {
    return this->overrides.operand_size ? this->fetch_instruction_word() : this->fetch_instruction_dword();
  }
  // Near branch/call/return target: truncated to 16 bits under a 16-bit operand size, and checked against the CS
  // limit in segmented mode (the fault is reported at the branch, as on hardware).
  void set_eip_near(uint32_t target);

  template <typename GetU8T, typename GetU32LT>
    requires(std::is_invocable_r_v<uint8_t, GetU8T> && std::is_invocable_r_v<uint32_t, GetU32LT>)
  static DecodedRM fetch_and_decode_rm_t(GetU8T&& get_u8, GetU32LT&& get_u32l, bool addr16);

  DecodedRM fetch_and_decode_rm();
  static DecodedRM fetch_and_decode_rm(DisassemblyState& s);

  uint32_t resolve_mem_ea(const DecodedRM& rm) const;

  template <typename T>
  T read_non_ea(const DecodedRM& rm) {
    return this->regs.read<T>(rm.non_ea_reg);
  }
  template <typename T>
  T read_non_ea_xmm(const DecodedRM& rm) {
    return this->regs.read_xmm<T>(rm.non_ea_reg);
  }
  template <typename T>
  void write_non_ea(const DecodedRM& rm, T v) {
    this->regs.write<T>(rm.non_ea_reg, v);
  }
  template <typename T>
  void write_non_ea_xmm(const DecodedRM& rm, T v) {
    this->regs.write_xmm<T>(rm.non_ea_reg, v);
  }

  // Linear address of a ModRM memory operand.
  inline uint32_t ea_linear(const DecodedRM& rm, uint32_t size, bool write) {
    return this->lin_data(rm.seg, this->resolve_mem_ea(rm), size, write);
  }

  template <typename T>
  T read_ea(const DecodedRM& rm) {
    return (rm.ea_index_scale < 0) ? this->regs.read<T>(rm.ea_reg) : this->r_mem<T>(this->ea_linear(rm, sizeof(T), false));
  }
  template <typename T>
  T read_ea_xmm(const DecodedRM& rm) {
    return (rm.ea_index_scale < 0) ? this->regs.read_xmm<T>(rm.ea_reg) : this->r_mem<T>(this->ea_linear(rm, sizeof(T), false));
  }
  template <typename T>
  void write_ea(const DecodedRM& rm, T v) {
    if (rm.ea_index_scale < 0) {
      this->regs.write<T>(rm.ea_reg, v);
    } else {
      this->w_mem<T>(this->ea_linear(rm, sizeof(T), true), v);
    }
  }
  template <typename T>
  void write_ea_xmm(const DecodedRM& rm, T v) {
    if (rm.ea_index_scale < 0) {
      this->regs.write_xmm<T>(rm.ea_reg, v);
    } else {
      this->w_mem<T>(this->ea_linear(rm, sizeof(T), true), v);
    }
  }

  // Read-modify-write of a ModRM operand with a single EA computation (and a single write check).
  template <typename T, typename FnT>
  void rmw_ea(const DecodedRM& rm, FnT&& fn) {
    if (rm.ea_index_scale < 0) {
      this->regs.write<T>(rm.ea_reg, fn(this->regs.read<T>(rm.ea_reg)));
    } else {
      uint32_t addr = this->ea_linear(rm, sizeof(T), true);
      this->w_mem<T>(addr, fn(this->r_mem<T>(addr)));
    }
  }

  inline uint8_t r_non_ea8(const DecodedRM& rm) { return this->read_non_ea<uint8_t>(rm); }
  inline uint16_t r_non_ea16(const DecodedRM& rm) { return this->read_non_ea<phosg::le_uint16_t>(rm); }
  inline uint32_t r_non_ea32(const DecodedRM& rm) { return this->read_non_ea<phosg::le_uint32_t>(rm); }
  inline uint32_t r_non_ea_xmm32(const DecodedRM& rm) { return this->read_non_ea_xmm<phosg::le_uint32_t>(rm); }
  inline uint64_t r_non_ea_xmm64(const DecodedRM& rm) { return this->read_non_ea_xmm<phosg::le_uint64_t>(rm); }
  inline Regs::XMMReg r_non_ea_xmm128(const DecodedRM& rm) { return this->read_non_ea_xmm<Regs::XMMReg>(rm); }
  inline void w_non_ea8(const DecodedRM& rm, uint8_t v) { this->write_non_ea<uint8_t>(rm, v); }
  inline void w_non_ea16(const DecodedRM& rm, uint16_t v) { this->write_non_ea<phosg::le_uint16_t>(rm, v); }
  inline void w_non_ea32(const DecodedRM& rm, uint32_t v) { this->write_non_ea<phosg::le_uint32_t>(rm, v); }
  inline void w_non_ea_xmm32(const DecodedRM& rm, uint32_t v) { this->write_non_ea_xmm<phosg::le_uint32_t>(rm, v); }
  inline void w_non_ea_xmm64(const DecodedRM& rm, uint64_t v) { this->write_non_ea_xmm<phosg::le_uint64_t>(rm, v); }
  inline void w_non_ea_xmm128(const DecodedRM& rm, const Regs::XMMReg& v) { this->write_non_ea_xmm<Regs::XMMReg>(rm, v); }
  inline uint8_t r_ea8(const DecodedRM& rm) { return this->read_ea<uint8_t>(rm); }
  inline uint16_t r_ea16(const DecodedRM& rm) { return this->read_ea<phosg::le_uint16_t>(rm); }
  inline uint32_t r_ea32(const DecodedRM& rm) { return this->read_ea<phosg::le_uint32_t>(rm); }
  inline uint32_t r_ea_xmm32(const DecodedRM& rm) { return this->read_ea_xmm<phosg::le_uint32_t>(rm); }
  inline uint64_t r_ea_xmm64(const DecodedRM& rm) { return this->read_ea_xmm<phosg::le_uint64_t>(rm); }
  inline Regs::XMMReg r_ea_xmm128(const DecodedRM& rm) { return this->read_ea_xmm<Regs::XMMReg>(rm); }
  inline void w_ea8(const DecodedRM& rm, uint8_t v) { this->write_ea<uint8_t>(rm, v); }
  inline void w_ea16(const DecodedRM& rm, uint16_t v) { this->write_ea<phosg::le_uint16_t>(rm, v); }
  inline void w_ea32(const DecodedRM& rm, uint32_t v) { this->write_ea<phosg::le_uint32_t>(rm, v); }
  inline void w_ea_xmm32(const DecodedRM& rm, uint32_t v) { this->write_ea_xmm<phosg::le_uint32_t>(rm, v); }
  inline void w_ea_xmm64(const DecodedRM& rm, uint64_t v) { this->write_ea_xmm<phosg::le_uint64_t>(rm, v); }
  inline void w_ea_xmm128(const DecodedRM& rm, const Regs::XMMReg& v) { this->write_ea_xmm<Regs::XMMReg>(rm, v); }

  // Linear-address memory access (the caller has applied segmentation). An address no arena maps is the emulated
  // page fault; catching it here (zero-cost unless it happens) is what lets the fault report the address.
  template <typename T>
  T r_mem(uint32_t addr) {
    if (const uint8_t* p = this->data_ptr(addr, sizeof(T))) [[likely]] {
      T ret;
      memcpy(&ret, p, sizeof(T));
      return ret;
    }
    try {
      return this->mem->read<T>(addr);
    } catch (const std::out_of_range&) {
      raise_unmapped(addr);
    }
  }
  template <typename T>
  void w_mem(uint32_t addr, T value) {
    if (uint8_t* p = this->data_ptr(addr, sizeof(T))) [[likely]] {
      memcpy(p, &value, sizeof(T));
      this->mem->note_write(addr, sizeof(T));
      return;
    }
    try {
      this->mem->write<T>(addr, value);
    } catch (const std::out_of_range&) {
      raise_unmapped(addr);
    }
  }

  static std::string disassemble_one(DisassemblyState& s);

  // One instruction, including its prefixes; no fault handling (the callers catch CPUFault).
  void execute_one_inner();
  // The run loop shared by run/run_until/execute. Returns why it stopped.
  StopReason run_loop(bool use_stop, bool check_cs, uint16_t stop_cs, uint32_t stop_eip, uint64_t max_instructions);
  // Rolls back EIP/ESP and delivers a fault to the handler (or throws fault_error).
  void deliver_fault(
      uint8_t vector, uint32_t error_code, uint32_t address, const std::string& what, bool divide_by_zero = false);

  template <typename T>
  T exec_integer_math_logic(uint8_t what, T dest, T src);
  template <typename T>
  void alu_to_ea(uint8_t what, const DecodedRM& rm, T src);
  template <typename T, typename LET = phosg::little_endian<T>>
  T exec_F6_F7_misc_math_logic(uint8_t what, T value);
  template <typename T>
  T exec_bit_test_ops_logic(uint8_t what, T v, uint8_t bit_number);
  template <typename T>
  T exec_bit_shifts_logic(uint8_t what, T value, uint8_t distance, bool distance_is_cl);
  template <typename T>
  T exec_shld_shrd_logic(bool is_right_shift, T dest_value, T incoming_value, uint8_t distance, bool distance_is_cl);
  template <typename T, bool Addr16>
  void exec_string_op_logic(uint8_t opcode);
  template <typename T, bool Addr16>
  void exec_rep_string_op_logic(uint8_t opcode);
  template <typename T>
  void exec_string_op_dispatch(uint8_t opcode);
  template <typename T>
  T exec_imul_logic(T a, T b);
  void far_call(uint16_t sel, uint32_t off, bool op32);
  void far_jump(uint16_t sel, uint32_t off);
  void far_return(bool op32, uint16_t release);
  void check_fpu_pending();

  void exec_0F_extensions(uint8_t);
  static std::string dasm_0F_extensions(DisassemblyState& s);
  void exec_0x_1x_2x_3x_x0_x1_x8_x9_mem_reg_math(uint8_t opcode);
  static std::string dasm_0x_1x_2x_3x_x0_x1_x8_x9_mem_reg_math(DisassemblyState& s);
  void exec_0x_1x_2x_3x_x2_x3_xA_xB_reg_mem_math(uint8_t opcode);
  static std::string dasm_0x_1x_2x_3x_x2_x3_xA_xB_reg_mem_math(DisassemblyState& s);
  void exec_0x_1x_2x_3x_x4_x5_xC_xD_eax_imm_math(uint8_t opcode);
  static std::string dasm_0x_1x_2x_3x_x4_x5_xC_xD_eax_imm_math(DisassemblyState& s);
  void exec_06_0E_16_1E_0FA0_0FA8_push_segment_reg(uint8_t opcode);
  static std::string dasm_06_0E_16_1E_0FA0_0FA8_push_segment_reg(DisassemblyState& s);
  void exec_07_17_1F_0FA1_0FA9_pop_segment_reg(uint8_t opcode);
  static std::string dasm_07_17_1F_0FA1_0FA9_pop_segment_reg(DisassemblyState& s);
  void exec_26_es(uint8_t);
  static std::string dasm_26_es(DisassemblyState& s);
  void exec_27_2F_daa_das(uint8_t);
  static std::string dasm_27_2F_daa_das(DisassemblyState& s);
  void exec_2E_cs(uint8_t);
  static std::string dasm_2E_cs(DisassemblyState& s);
  void exec_36_ss(uint8_t);
  static std::string dasm_36_ss(DisassemblyState& s);
  void exec_37_3F_aaa_aas(uint8_t);
  static std::string dasm_37_3F_aaa_aas(DisassemblyState& s);
  void exec_3E_ds(uint8_t);
  static std::string dasm_3E_ds(DisassemblyState& s);
  void exec_40_to_47_inc(uint8_t opcode);
  void exec_48_to_4F_dec(uint8_t opcode);
  static std::string dasm_40_to_4F_inc_dec(DisassemblyState& s);
  void exec_50_to_57_push(uint8_t opcode);
  void exec_58_to_5F_pop(uint8_t opcode);
  static std::string dasm_50_to_5F_push_pop(DisassemblyState& s);
  void exec_60_pusha(uint8_t);
  static std::string dasm_60_pusha(DisassemblyState& s);
  void exec_61_popa(uint8_t);
  static std::string dasm_61_popa(DisassemblyState& s);
  void exec_62_bound(uint8_t);
  static std::string dasm_62_bound(DisassemblyState& s);
  void exec_63_arpl(uint8_t);
  static std::string dasm_63_arpl(DisassemblyState& s);
  void exec_64_fs(uint8_t);
  static std::string dasm_64_fs(DisassemblyState& s);
  void exec_65_gs(uint8_t);
  static std::string dasm_65_gs(DisassemblyState& s);
  void exec_66_operand_size(uint8_t);
  static std::string dasm_66_operand_size(DisassemblyState& s);
  void exec_67_address_size(uint8_t);
  static std::string dasm_67_address_size(DisassemblyState& s);
  void exec_68_6A_push(uint8_t);
  static std::string dasm_68_6A_push(DisassemblyState& s);
  void exec_69_6B_imul(uint8_t);
  static std::string dasm_69_6B_imul(DisassemblyState& s);
  void exec_6C_to_6F_ins_outs(uint8_t);
  static std::string dasm_6C_to_6F_ins_outs(DisassemblyState& s);
  void exec_70_to_7F_jcc(uint8_t opcode);
  static std::string dasm_70_to_7F_jcc(DisassemblyState& s);
  void exec_80_to_83_imm_math(uint8_t opcode);
  static std::string dasm_80_to_83_imm_math(DisassemblyState& s);
  void exec_84_85_test_rm(uint8_t opcode);
  static std::string dasm_84_85_test_rm(DisassemblyState& s);
  void exec_86_87_xchg_rm(uint8_t opcode);
  static std::string dasm_86_87_xchg_rm(DisassemblyState& s);
  void exec_88_to_8B_mov_rm(uint8_t opcode);
  static std::string dasm_88_to_8B_mov_rm(DisassemblyState& s);
  void exec_8C_mov_rm_sreg(uint8_t);
  static std::string dasm_8C_mov_rm_sreg(DisassemblyState& s);
  void exec_8D_lea(uint8_t);
  static std::string dasm_8D_lea(DisassemblyState& s);
  void exec_8E_mov_sreg_rm(uint8_t);
  static std::string dasm_8E_mov_sreg_rm(DisassemblyState& s);
  void exec_8F_pop_rm(uint8_t opcode);
  static std::string dasm_8F_pop_rm(DisassemblyState& s);
  void exec_90_to_97_xchg_eax(uint8_t opcode);
  static std::string dasm_90_to_97_xchg_eax(DisassemblyState& s);
  void exec_98_cbw_cwde(uint8_t);
  static std::string dasm_98_cbw_cwde(DisassemblyState& s);
  void exec_99_cwd_cdq(uint8_t);
  static std::string dasm_99_cwd_cdq(DisassemblyState& s);
  void exec_9A_call_far(uint8_t);
  static std::string dasm_9A_EA_far_ptr(DisassemblyState& s);
  void exec_9B_wait(uint8_t);
  static std::string dasm_9B_wait(DisassemblyState& s);
  void exec_9C_pushf_pushfd(uint8_t);
  static std::string dasm_9C_pushf_pushfd(DisassemblyState& s);
  void exec_9D_popf_popfd(uint8_t);
  static std::string dasm_9D_popf_popfd(DisassemblyState& s);
  void exec_9E_sahf(uint8_t);
  static std::string dasm_9E_sahf(DisassemblyState&);
  void exec_9F_lahf(uint8_t);
  static std::string dasm_9F_lahf(DisassemblyState&);
  void exec_A0_A1_A2_A3_mov_eax_memabs(uint8_t opcode);
  static std::string dasm_A0_A1_A2_A3_mov_eax_memabs(DisassemblyState& s);
  void exec_A4_to_A7_AA_to_AF_string_ops(uint8_t opcode);
  static std::string dasm_A4_to_A7_AA_to_AF_string_ops(DisassemblyState& s);
  void exec_A8_A9_test_eax_imm(uint8_t opcode);
  static std::string dasm_A8_A9_test_eax_imm(DisassemblyState& s);
  void exec_B0_to_BF_mov_imm(uint8_t opcode);
  static std::string dasm_B0_to_BF_mov_imm(DisassemblyState& s);
  void exec_C0_C1_bit_shifts(uint8_t opcode);
  static std::string dasm_C0_C1_bit_shifts(DisassemblyState& s);
  void exec_C2_C3_CA_CB_ret(uint8_t opcode);
  static std::string dasm_C2_C3_CA_CB_ret(DisassemblyState& s);
  void exec_C4_C5_les_lds(uint8_t opcode);
  static std::string dasm_C4_C5_les_lds(DisassemblyState& s);
  void exec_C6_C7_mov_rm_imm(uint8_t opcode);
  static std::string dasm_C6_C7_mov_rm_imm(DisassemblyState& s);
  void exec_C8_enter(uint8_t opcode);
  static std::string dasm_C8_enter(DisassemblyState& s);
  void exec_C9_leave(uint8_t);
  static std::string dasm_C9_leave(DisassemblyState& s);
  void exec_CC_CD_int(uint8_t opcode);
  static std::string dasm_CC_CD_int(DisassemblyState& s);
  void exec_CE_into(uint8_t opcode);
  static std::string dasm_CE_into(DisassemblyState& s);
  void exec_CF_iret(uint8_t opcode);
  static std::string dasm_CF_iret(DisassemblyState& s);
  void exec_D0_to_D3_bit_shifts(uint8_t opcode);
  static std::string dasm_D0_to_D3_bit_shifts(DisassemblyState& s);
  void exec_D4_amx_aam(uint8_t);
  static std::string dasm_D4_amx_aam(DisassemblyState& s);
  void exec_D5_adx_aad(uint8_t);
  static std::string dasm_D5_adx_aad(DisassemblyState& s);
  void exec_D6_salc(uint8_t);
  static std::string dasm_D6_salc(DisassemblyState& s);
  void exec_D7_xlat(uint8_t);
  static std::string dasm_D7_xlat(DisassemblyState& s);
  void exec_D8_DC_float_basic_math(uint8_t opcode);
  void exec_D9_DD_float_moves_and_analytical_math(uint8_t opcode);
  void exec_DA_DB_float_cmov_and_int_math(uint8_t opcode);
  void exec_DE_float_misc1(uint8_t opcode);
  void exec_DF_float_misc2(uint8_t opcode);
  static std::string dasm_D8_DC_float_basic_math(DisassemblyState& s);
  static std::string dasm_D9_DD_float_moves_and_analytical_math(DisassemblyState& s);
  static std::string dasm_DA_DB_float_cmov_and_int_math(DisassemblyState& s);
  static std::string dasm_DE_float_misc1(DisassemblyState& s);
  static std::string dasm_DF_float_misc2(DisassemblyState& s);
  void exec_E0_to_E3_loop_jcxz(uint8_t opcode);
  static std::string dasm_E0_to_E3_loop_jcxz(DisassemblyState& s);
  void exec_E4_E5_EC_ED_in(uint8_t opcode);
  static std::string dasm_E4_E5_EC_ED_in(DisassemblyState& s);
  void exec_E6_E7_EE_EF_out(uint8_t opcode);
  static std::string dasm_E6_E7_EE_EF_out(DisassemblyState& s);
  void exec_E8_E9_call_jmp(uint8_t opcode);
  static std::string dasm_E8_E9_call_jmp(DisassemblyState& s);
  void exec_EA_jmp_far(uint8_t opcode);
  void exec_EB_jmp(uint8_t opcode);
  static std::string dasm_EB_jmp(DisassemblyState& s);
  void exec_F0_lock(uint8_t);
  static std::string dasm_F0_lock(DisassemblyState& s);
  void exec_F1_icebp(uint8_t);
  static std::string dasm_F1_icebp(DisassemblyState& s);
  void exec_F2_F3_repz_repnz(uint8_t opcode);
  static std::string dasm_F2_F3_repz_repnz(DisassemblyState& s);
  void exec_F4_hlt(uint8_t);
  static std::string dasm_F4_hlt(DisassemblyState&);
  void exec_F5_cmc(uint8_t);
  static std::string dasm_F5_cmc(DisassemblyState&);
  void exec_F6_F7_misc_math(uint8_t opcode);
  static std::string dasm_F6_F7_misc_math(DisassemblyState& s);
  void exec_F8_clc(uint8_t);
  static std::string dasm_F8_clc(DisassemblyState&);
  void exec_F9_stc(uint8_t);
  static std::string dasm_F9_stc(DisassemblyState&);
  void exec_FA_cli(uint8_t);
  static std::string dasm_FA_cli(DisassemblyState&);
  void exec_FB_sti(uint8_t);
  static std::string dasm_FB_sti(DisassemblyState&);
  void exec_FC_cld(uint8_t);
  static std::string dasm_FC_cld(DisassemblyState&);
  void exec_FD_std(uint8_t);
  static std::string dasm_FD_std(DisassemblyState&);
  void exec_FE_FF_inc_dec_misc(uint8_t opcode);
  static std::string dasm_FE_FF_inc_dec_misc(DisassemblyState& s);

  void exec_0F_00_grp6(uint8_t opcode);
  static std::string dasm_0F_00_grp6(DisassemblyState& s);
  void exec_0F_01_grp7(uint8_t opcode);
  static std::string dasm_0F_01_grp7(DisassemblyState& s);
  void exec_0F_02_03_lar_lsl(uint8_t opcode);
  static std::string dasm_0F_02_03_lar_lsl(DisassemblyState& s);
  void exec_0F_0B_ud2(uint8_t opcode);
  static std::string dasm_0F_0B_ud2(DisassemblyState& s);
  void exec_0F_10_11_mov_xmm(uint8_t opcode);
  static std::string dasm_0F_10_11_mov_xmm(DisassemblyState& s);
  void exec_0F_18_to_1F_prefetch_or_nop(uint8_t opcode);
  static std::string dasm_0F_18_to_1F_prefetch_or_nop(DisassemblyState& s);
  void exec_0F_31_rdtsc(uint8_t opcode);
  static std::string dasm_0F_31_rdtsc(DisassemblyState& s);
  void exec_0F_40_to_4F_cmov_rm(uint8_t opcode);
  static std::string dasm_0F_40_to_4F_cmov_rm(DisassemblyState& s);
  void exec_0F_7E_7F_mov_xmm(uint8_t opcode);
  static std::string dasm_0F_7E_7F_mov_xmm(DisassemblyState& s);
  void exec_0F_80_to_8F_jcc(uint8_t opcode);
  static std::string dasm_0F_80_to_8F_jcc(DisassemblyState& s);
  void exec_0F_90_to_9F_setcc_rm(uint8_t opcode);
  static std::string dasm_0F_90_to_9F_setcc_rm(DisassemblyState& s);
  void exec_0F_A2_cpuid(uint8_t opcode);
  static std::string dasm_0F_A2_cpuid(DisassemblyState& s);
  void exec_0F_A3_AB_B3_BB_bit_tests(uint8_t opcode);
  static std::string dasm_0F_A3_AB_B3_BB_bit_tests(DisassemblyState& s);
  void exec_0F_A4_A5_AC_AD_shld_shrd(uint8_t opcode);
  static std::string dasm_0F_A4_A5_AC_AD_shld_shrd(DisassemblyState& s);
  void exec_0F_AF_imul(uint8_t opcode);
  static std::string dasm_0F_AF_imul(DisassemblyState& s);
  void exec_0F_B0_B1_cmpxchg(uint8_t opcode);
  static std::string dasm_0F_B0_B1_cmpxchg(DisassemblyState& s);
  void exec_0F_B2_B4_B5_lss_lfs_lgs(uint8_t opcode);
  static std::string dasm_0F_B2_B4_B5_lss_lfs_lgs(DisassemblyState& s);
  void exec_0F_B6_B7_BE_BF_movzx_movsx(uint8_t opcode);
  static std::string dasm_0F_B6_B7_BE_BF_movzx_movsx(DisassemblyState& s);
  void exec_0F_BA_bit_tests(uint8_t);
  static std::string dasm_0F_BA_bit_tests(DisassemblyState& s);
  void exec_0F_BC_BD_bsf_bsr(uint8_t opcode);
  static std::string dasm_0F_BC_BD_bsf_bsr(DisassemblyState& s);
  void exec_0F_C0_C1_xadd_rm(uint8_t opcode);
  static std::string dasm_0F_C0_C1_xadd_rm(DisassemblyState& s);
  void exec_0F_C7_cmpxchg8b(uint8_t opcode);
  static std::string dasm_0F_C7_cmpxchg8b(DisassemblyState& s);
  void exec_0F_C8_to_CF_bswap(uint8_t opcode);
  static std::string dasm_0F_C8_to_CF_bswap(DisassemblyState& s);
  void exec_0F_D6_movq_variants(uint8_t opcode);
  static std::string dasm_0F_D6_movq_variants(DisassemblyState& s);

  void exec_unimplemented(uint8_t opcode);
  static std::string dasm_unimplemented(DisassemblyState& s);
  void exec_0F_unimplemented(uint8_t opcode);
  static std::string dasm_0F_unimplemented(DisassemblyState& s);

  // ---- x87 (X87.cc) ----
  // Operand address of the current x87 memory operand; also records FDP/FDS.
  uint32_t fpu_mem_linear(const DecodedRM& rm, uint32_t size, bool write);
  void fpu_note_instruction(uint16_t fop);
  void fpu_store_env(const DecodedRM& rm, bool op32, bool and_regs);
  void fpu_load_env(const DecodedRM& rm, bool op32, bool and_regs);

  struct OpcodeImplementation {
    void (X86Emulator::*exec)(uint8_t);
    std::string (*dasm)(DisassemblyState& s);

    OpcodeImplementation() : exec(nullptr), dasm(nullptr) {}
    OpcodeImplementation(void (X86Emulator::*exec)(uint8_t), std::string (*dasm)(DisassemblyState& s))
        : exec(exec), dasm(dasm) {}
  };
  static const OpcodeImplementation fns[0x100];
  static const OpcodeImplementation fns_0F[0x100];

  struct Assembler {
    struct Argument {
      enum Type {
        INT_REGISTER = 0x01, // "eax", "ecx", etc. (reg_num)
        FLOAT_REGISTER = 0x02, // "st0", "st1", etc. (reg_num); plain "st" parsed as "st0"
        XMM_REGISTER = 0x04, // "xmm0", "xmm1", etc. (reg_num)

        SEGMENT_REGISTER = 0x08, // "ds", "es", etc.

        IMMEDIATE = 0x10, // "{}" or "0x{:X}", optionally preceded by a + or - (value, scale)

        // reg_num = base reg, reg_num2 = index reg (if scale != 0), value = displacement
        MEMORY_REFERENCE = 0x20, // "dword [reg]", "byte [reg + {}]", etc.

        // raw_data is set to the literal string passed as an argument to the opcode. In this case, there is always
        // only one argument, even if the string contains commas.
        RAW = 0x40,

        // Convenience masks used in check_arg_types
        MEM_OR_IREG_OR_IMM = MEMORY_REFERENCE | INT_REGISTER | IMMEDIATE,
        MEM_OR_IREG = MEMORY_REFERENCE | INT_REGISTER,
        MEM_OR_FREG = MEMORY_REFERENCE | FLOAT_REGISTER,
        MEM_OR_XMMREG = MEMORY_REFERENCE | XMM_REGISTER,
        MEM_OR_REG = MEMORY_REFERENCE | INT_REGISTER | FLOAT_REGISTER | XMM_REGISTER,
      };
      Type type;
      uint8_t operand_size = 0; // 0 = unspecified; otherwise 1, 2, 4, or 8
      uint8_t reg_num = 0;
      uint8_t index_reg_num = 0;
      uint8_t segment_reg_num = 0xFF;
      uint8_t index_scale = 0; // 0 = no scale reg; otherwise 1, 2, 4, or 8; for IMMEDIATE this is nonzero if there was a preceding + or -
      double float_value = 0;
      std::unique_ptr<const Expression::Node> int_value_expr;
      std::string raw_data;
      mutable bool has_code_delta = false;

      Argument(const std::string& text, bool raw = false);

      bool is_reg_ref() const;
      std::string str() const;
    };

    using T = Argument::Type;

    struct StreamItem {
      size_t offset = 0;
      size_t address = 0;
      size_t fixed_address = 0;
      size_t index = 0;
      size_t line_num = 0;
      std::string op_name;
      std::vector<Argument> args;
      std::string assembled_data;
      bool allow_short_jmp = true;
      std::unordered_set<std::string> label_names;

      std::string str() const;

      uint8_t resolve_operand_size(phosg::StringWriter& w, size_t max_args = 0) const;
      void check_arg_types(std::initializer_list<Argument::Type> types) const;
      [[nodiscard]] bool arg_types_match(std::initializer_list<Argument::Type> types) const;
      void check_arg_operand_sizes(std::initializer_list<uint8_t> operand_sizes) const;
      void check_arg_fixed_registers(std::initializer_list<uint8_t> reg_nums) const;
      void check_arg_is_st(size_t arg_num, uint8_t which) const;
      uint8_t require_16_or_32(phosg::StringWriter& w, size_t max_args = 0) const;
      uint8_t require_arg_16_or_32(size_t arg_index) const;
      uint8_t require_arg_32_or_64(size_t arg_index) const;
      uint8_t require_arg_16_or_32_or_64(size_t arg_index) const;
      uint8_t get_size_mnemonic_suffix(const std::string& base_name) const;
      uint8_t require_size_mnemonic_suffix(phosg::StringWriter& w, const std::string& base_name) const;
      bool any_arg_has_code_delta() const;
    };
    uint32_t start_address = 0;
    std::vector<StreamItem> stream;
    std::unordered_map<std::string, size_t> label_si_indexes;
    std::unordered_map<std::string, size_t> fixed_labels;
    std::unordered_map<std::string, std::string> includes_cache;
    std::unordered_map<std::string, std::string> metadata_keys;

    typedef void (Assembler::*AssembleFunction)(phosg::StringWriter& w, StreamItem& si) const;
    static const std::unordered_map<std::string, AssembleFunction> assemble_functions;

    AssembleResult assemble(const std::string& text, std::function<std::string(const std::string&)> get_include);

    void encode_segment_override(phosg::StringWriter& w, const Argument& mem_ref) const;
    void encode_imm(phosg::StringWriter& w, uint64_t value, uint8_t operand_size) const;
    void encode_rm(phosg::StringWriter& w, const Argument& mem_ref, const Argument& reg_ref) const;
    void encode_rm(phosg::StringWriter& w, const Argument& mem_ref, uint8_t op_type) const;
    uint32_t compute_branch_target_from_arg0(const StreamItem& si) const;
    int64_t resolve_immediate(const Argument& arg, bool is_label_def = false) const;

    void asm_aaa_aas_aad_aam(phosg::StringWriter& w, StreamItem& si) const;
    void asm_add_or_adc_sbb_and_sub_xor_cmp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_amx_adx(phosg::StringWriter& w, StreamItem& si) const;
    void asm_bsf_bsr(phosg::StringWriter& w, StreamItem& si) const;
    void asm_bswap(phosg::StringWriter& w, StreamItem& si) const;
    void asm_bt_bts_btr_btc(phosg::StringWriter& w, StreamItem& si) const;
    void asm_call_jmp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cbw_cwde(phosg::StringWriter& w, StreamItem& si) const;
    void asm_clc(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cld(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cli(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cmc(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cmov_mnemonics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_ins_outs_movs_cmps_stos_lods_scas_mnemonics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cmpxchg(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cmpxchg8b(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cpuid(phosg::StringWriter& w, StreamItem& si) const;
    void asm_crc32(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cs(phosg::StringWriter& w, StreamItem& si) const;
    void asm_cwd_cdq(phosg::StringWriter& w, StreamItem& si) const;
    void asm_daa(phosg::StringWriter& w, StreamItem& si) const;
    void asm_das(phosg::StringWriter& w, StreamItem& si) const;
    void asm_inc_dec(phosg::StringWriter& w, StreamItem& si) const;
    void asm_div_idiv(phosg::StringWriter& w, StreamItem& si) const;
    void asm_ds(phosg::StringWriter& w, StreamItem& si) const;
    void asm_enter(phosg::StringWriter& w, StreamItem& si) const;
    void asm_es(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fs(phosg::StringWriter& w, StreamItem& si) const;
    void asm_gs(phosg::StringWriter& w, StreamItem& si) const;
    void asm_hlt(phosg::StringWriter& w, StreamItem& si) const;
    void asm_imul_mul(phosg::StringWriter& w, StreamItem& si) const;
    void asm_in_out(phosg::StringWriter& w, StreamItem& si) const;
    void asm_int(phosg::StringWriter& w, StreamItem& si) const;
    void asm_into(phosg::StringWriter& w, StreamItem& si) const;
    void asm_iret(phosg::StringWriter& w, StreamItem& si) const;
    void asm_j_mnemonics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_jcxz_jecxz_loop_mnemonics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_lahf_sahf(phosg::StringWriter& w, StreamItem& si) const;
    void asm_lea(phosg::StringWriter& w, StreamItem& si) const;
    void asm_leave(phosg::StringWriter& w, StreamItem& si) const;
    void asm_lock(phosg::StringWriter& w, StreamItem& si) const;
    void asm_mov(phosg::StringWriter& w, StreamItem& si) const;
    void asm_movbe(phosg::StringWriter& w, StreamItem& si) const;
    void asm_movsx_movzx(phosg::StringWriter& w, StreamItem& si) const;
    void asm_neg_not(phosg::StringWriter& w, StreamItem& si) const;
    void asm_nop(phosg::StringWriter& w, StreamItem& si) const;
    void asm_pop_push(phosg::StringWriter& w, StreamItem& si) const;
    void asm_popa_popad(phosg::StringWriter& w, StreamItem& si) const;
    void asm_popcnt(phosg::StringWriter& w, StreamItem& si) const;
    void asm_popf_popfd(phosg::StringWriter& w, StreamItem& si) const;
    void asm_pusha_pushad(phosg::StringWriter& w, StreamItem& si) const;
    void asm_pushf_pushfd(phosg::StringWriter& w, StreamItem& si) const;
    void asm_rol_ror_rcl_rcr_shl_sal_shr_sar(phosg::StringWriter& w, StreamItem& si) const;
    void asm_rdtsc(phosg::StringWriter& w, StreamItem& si) const;
    void asm_rep_mnemomics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_ret(phosg::StringWriter& w, StreamItem& si) const;
    void asm_salc_setalc(phosg::StringWriter& w, StreamItem& si) const;
    void asm_set_mnemonics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_shld_shrd(phosg::StringWriter& w, StreamItem& si) const;
    void asm_ss(phosg::StringWriter& w, StreamItem& si) const;
    void asm_stc(phosg::StringWriter& w, StreamItem& si) const;
    void asm_std(phosg::StringWriter& w, StreamItem& si) const;
    void asm_sti(phosg::StringWriter& w, StreamItem& si) const;
    void asm_test(phosg::StringWriter& w, StreamItem& si) const;
    void asm_xadd(phosg::StringWriter& w, StreamItem& si) const;
    void asm_xchg(phosg::StringWriter& w, StreamItem& si) const;

    void asm_fxsave_fxrstor(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fsave_fnsave_frstor(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fstenv_fnstenv_fldenv(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fstcw_fnstcw_fldcw(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fstsw_fnstsw(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fwait(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fclex_fnclex(phosg::StringWriter& w, StreamItem& si) const;
    void asm_finit_fninit(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fadd(phosg::StringWriter& w, StreamItem& si) const;
    void asm_faddp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fmul(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fmulp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fcom_fcomp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fcomi_fcomip(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fcompp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fsub_fsubr(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fsubp_fsubrp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fdiv_fdivr(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fdivp_fdivrp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fld(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fld1(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fldl2t(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fldl2e(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fldpi(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fldlg2(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fldln2(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fldz(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fxch(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fst_fstp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fnop(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fchs(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fabs(phosg::StringWriter& w, StreamItem& si) const;
    void asm_ftst(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fxam(phosg::StringWriter& w, StreamItem& si) const;
    void asm_f2xm1(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fyl2x(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fptan(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fpatan(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fxtract(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fprem1(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fdecstp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fincstp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fprem(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fyl2xp1(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fsqrt(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fsincos(phosg::StringWriter& w, StreamItem& si) const;
    void asm_frndint(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fscale(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fsin(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fcos(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fcmov_mnemonics(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fiadd_fimul_ficom_ficomp_fisub_fisubr_fidiv_fidivr(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fucompp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fild(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fist_fistp_fisttp(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fneni(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fndisi(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fnsetpm(phosg::StringWriter& w, StreamItem& si) const;
    void asm_ffree_ffreep(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fucom_fucomi_fucomp_fucomip(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fbld(phosg::StringWriter& w, StreamItem& si) const;
    void asm_fbstp(phosg::StringWriter& w, StreamItem& si) const;

    void asm_dir_byte(phosg::StringWriter& w, StreamItem& si) const;
    void asm_dir_data(phosg::StringWriter& w, StreamItem& si) const;
    void asm_dir_zero(phosg::StringWriter& w, StreamItem& si) const;
    void asm_dir_binary(phosg::StringWriter& w, StreamItem& si) const;
  };
};

} // namespace adw::cpu
