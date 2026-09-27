#include "win32/seh.hh"

#include <cinttypes>
#include <cstdio>
#include <vector>

#include "adw/core/log.h"
#include "win32/layout.hh"
#include "win32/modules.hh"

namespace adw::win32 {

namespace {

using SegReg = cpu::X86Emulator::SegReg;

// The dispatcher's frame on the guest stack (see seh.hh).
constexpr uint32_t kMagicDispatch = 0x44484553;  // "SEHD"
constexpr uint32_t kMagicUnwind = 0x55484553;    // "SEHU"
constexpr uint32_t kFMagic = 0x00;
constexpr uint32_t kFRecord = 0x04;
constexpr uint32_t kFContext = 0x08;
constexpr uint32_t kFReg = 0x0C;        // registration being processed
constexpr uint32_t kFDispCtx = 0x10;    // DISPATCHER_CONTEXT {RegistrationPointer}
constexpr uint32_t kFNested = 0x14;     // dispatch: NestedRegistration
constexpr uint32_t kFTarget = 0x18;     // unwind: TargetFrame
constexpr uint32_t kFLink = 0x20;       // our registration record {Next, Handler}
constexpr uint32_t kFrameSize = 0x30;
constexpr uint32_t kArgsSize = 0x14;    // return address + 4 handler arguments

constexpr uint32_t kChainEnd = 0xFFFFFFFF;

// Dispositions.
constexpr uint32_t kContinueExecution = 0, kContinueSearch = 1, kNestedException = 2, kCollidedUnwind = 3;

}  // namespace

Seh::Seh(Runtime& rt) : rt_(rt) {}

void Seh::install() {
  ShimRegistry& r = rt_.shims();
  dispatch_return_ = r.internal_thunk("seh dispatch continuation", [this](Call& c) { on_dispatch_return(c); });
  unwind_return_ = r.internal_thunk("seh unwind continuation", [this](Call& c) { on_unwind_return(c); });
  // The handlers of the registration records the dispatcher links around each
  // handler call. They are ordinary cdecl exception handlers (a guest that
  // walks the chain itself may call them), which is also how our own walk
  // reaches them: no special case in the walk.
  ShimEntry& dn = r.add("<host>", "seh nested-exception handler", Conv::cdecl_, 16, [this](Call& c) {
    uint32_t rec = c.arg(0), establisher = c.arg(1), dispctx = c.arg(3);
    uint32_t flags = rt_.mem().read_u32l(rec + 4);
    if (flags & (kExceptionUnwinding | kExceptionExitUnwind)) return c.ret(kContinueSearch);
    // An exception raised while a handler of an outer dispatch was running:
    // the outer dispatch had already searched the frames up to the one whose
    // handler it was calling; report that one so they are skipped as nested.
    uint32_t outer = establisher - kFLink;
    rt_.mem().write_u32l(dispctx, rt_.mem().read_u32l(outer + kFReg));
    c.ret(kNestedException);
  });
  dispatch_nested_ = r.thunk_address(dn);
  ShimEntry& un = r.add("<host>", "seh collided-unwind handler", Conv::cdecl_, 16, [this](Call& c) {
    uint32_t rec = c.arg(0), establisher = c.arg(1), dispctx = c.arg(3);
    uint32_t flags = rt_.mem().read_u32l(rec + 4);
    if (!(flags & (kExceptionUnwinding | kExceptionExitUnwind))) return c.ret(kContinueSearch);
    // An unwind started inside a handler that an outer unwind was calling:
    // continue from the outer unwind's current frame.
    uint32_t outer = establisher - kFLink;
    rt_.mem().write_u32l(dispctx, rt_.mem().read_u32l(outer + kFReg));
    c.ret(kCollidedUnwind);
  });
  unwind_nested_ = r.thunk_address(un);
}

// ---- helpers -----------------------------------------------------------------------------------

uint32_t Seh::fs0() const { return rt_.mem().read_u32l(rt_.teb()); }
void Seh::set_fs0(uint32_t v) { rt_.mem().write_u32l(rt_.teb(), v); }

bool Seh::valid_registration(uint32_t reg) const {
  uint32_t base = rt_.mem().read_u32l(rt_.teb() + 4), limit = rt_.mem().read_u32l(rt_.teb() + 8);
  return (reg & 3) == 0 && reg >= limit && uint64_t(reg) + 8 <= base;
}

uint32_t Seh::new_frame(uint32_t magic, uint32_t top) {
  auto& mem = rt_.mem();
  uint32_t context = align_down(top - 16 - ctx::kSize, 16);
  uint32_t record = context - kExceptionRecordSize;
  uint32_t frame = record - kFrameSize;
  mem.memset(frame, 0, kFrameSize + kExceptionRecordSize);
  mem.write_u32l(frame + kFMagic, magic);
  mem.write_u32l(frame + kFRecord, record);
  mem.write_u32l(frame + kFContext, context);
  return frame;
}

void Seh::write_context(uint32_t a, const Regs32& r) {
  auto& mem = rt_.mem();
  auto& cpu = rt_.cpu();
  mem.memset(a, 0, ctx::kSize);
  mem.write_u32l(a + ctx::kFlags, 0x0001000F);  // CONTEXT_FULL | CONTEXT_FLOATING_POINT
  uint8_t fsave[108];
  cpu.fpu_save_image(fsave);
  mem.memcpy(a + ctx::kFloatSave, fsave, sizeof(fsave));
  mem.write_u32l(a + ctx::kGs, cpu.get_segment(SegReg::GS));
  mem.write_u32l(a + ctx::kFs, cpu.get_segment(SegReg::FS));
  mem.write_u32l(a + ctx::kEs, cpu.get_segment(SegReg::ES));
  mem.write_u32l(a + ctx::kDs, cpu.get_segment(SegReg::DS));
  mem.write_u32l(a + ctx::kCs, cpu.get_segment(SegReg::CS));
  mem.write_u32l(a + ctx::kSs, cpu.get_segment(SegReg::SS));
  mem.write_u32l(a + ctx::kEdi, r.edi);
  mem.write_u32l(a + ctx::kEsi, r.esi);
  mem.write_u32l(a + ctx::kEbx, r.ebx);
  mem.write_u32l(a + ctx::kEdx, r.edx);
  mem.write_u32l(a + ctx::kEcx, r.ecx);
  mem.write_u32l(a + ctx::kEax, r.eax);
  mem.write_u32l(a + ctx::kEbp, r.ebp);
  mem.write_u32l(a + ctx::kEip, r.eip);
  mem.write_u32l(a + ctx::kEflags, r.eflags);
  mem.write_u32l(a + ctx::kEsp, r.esp);
}

Regs32 Seh::read_context(uint32_t a) const {
  auto& mem = rt_.mem();
  Regs32 r;
  r.edi = mem.read_u32l(a + ctx::kEdi);
  r.esi = mem.read_u32l(a + ctx::kEsi);
  r.ebx = mem.read_u32l(a + ctx::kEbx);
  r.edx = mem.read_u32l(a + ctx::kEdx);
  r.ecx = mem.read_u32l(a + ctx::kEcx);
  r.eax = mem.read_u32l(a + ctx::kEax);
  r.ebp = mem.read_u32l(a + ctx::kEbp);
  r.eip = mem.read_u32l(a + ctx::kEip);
  r.eflags = mem.read_u32l(a + ctx::kEflags);
  r.esp = mem.read_u32l(a + ctx::kEsp);
  return r;
}

void Seh::resume(uint32_t context) {
  Regs32 r = read_context(context);
  uint32_t boundary = rt_.callback_boundary();
  if (boundary && r.esp > boundary) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "an exception handler resumed at %s with ESP 0x%08X, above the host callback running at ESP "
             "0x%08X (unwinding across a host frame is not supported)",
             rt_.describe(r.eip).c_str(), r.esp, boundary);
    throw GuestError(GuestError::Kind::fatal, buf);
  }
  trace("seh", "resume at %s esp=0x%08X eax=0x%08X", rt_.describe(r.eip).c_str(), r.esp, r.eax);
  rt_.set_regs(r);
}

// ---- dispatch ------------------------------------------------------------------------------------

void Seh::dispatch(uint32_t code, uint32_t flags, std::span<const uint32_t> params, uint32_t address,
                   const Regs32& resume_state, uint32_t chained_record) {
  auto& mem = rt_.mem();
  uint32_t frame;
  try {
    frame = new_frame(kMagicDispatch, rt_.cpu().registers().r_esp());
    uint32_t rec = mem.read_u32l(frame + kFRecord);
    mem.write_u32l(rec + 0x00, code);
    mem.write_u32l(rec + 0x04, flags & kExceptionNoncontinuable);
    mem.write_u32l(rec + 0x08, chained_record);
    mem.write_u32l(rec + 0x0C, address);
    uint32_t n = uint32_t(params.size() > 15 ? 15 : params.size());
    mem.write_u32l(rec + 0x10, n);
    for (uint32_t i = 0; i < n; i++) mem.write_u32l(rec + 0x14 + 4 * i, params[i]);
    write_context(mem.read_u32l(frame + kFContext), resume_state);
    mem.write_u32l(frame + kFReg, fs0());
  } catch (const std::out_of_range&) {
    // The guest stack itself is unusable (overflowed into the guard, or ESP is
    // garbage): nothing on the guest side can handle this.
    char buf[160];
    snprintf(buf, sizeof(buf), "exception %s at %s with an unusable stack (ESP 0x%08X)", code_name(code).c_str(),
             rt_.describe(address).c_str(), rt_.cpu().registers().r_esp());
    throw GuestError(GuestError::Kind::unhandled, buf, code);
  }
  dispatched_++;
  trace("seh", "raise %s at %s, chain head 0x%08X", code_name(code).c_str(), rt_.describe(address).c_str(),
        fs0());
  walk(frame);
}

void Seh::walk(uint32_t frame) {
  auto& mem = rt_.mem();
  uint32_t rec = mem.read_u32l(frame + kFRecord), context = mem.read_u32l(frame + kFContext);
  uint32_t reg = mem.read_u32l(frame + kFReg);
  if (reg == kChainEnd || reg == 0) unhandled(rec, context);
  if (!valid_registration(reg)) {
    mem.write_u32l(rec + 4, mem.read_u32l(rec + 4) | kExceptionStackInvalid);
    unhandled(rec, context);
  }
  call_handler(frame, reg, /*unwind=*/false);
}

void Seh::call_handler(uint32_t frame, uint32_t reg, bool unwind) {
  auto& mem = rt_.mem();
  uint32_t handler = mem.read_u32l(reg + 4);
  uint32_t link = frame + kFLink;
  mem.write_u32l(link, fs0());
  mem.write_u32l(link + 4, unwind ? unwind_nested_ : dispatch_nested_);
  set_fs0(link);
  mem.write_u32l(frame + kFDispCtx, 0);
  uint32_t sp = frame - kArgsSize;
  mem.write_u32l(sp + 0, unwind ? unwind_return_ : dispatch_return_);
  mem.write_u32l(sp + 4, mem.read_u32l(frame + kFRecord));
  mem.write_u32l(sp + 8, reg);
  mem.write_u32l(sp + 12, mem.read_u32l(frame + kFContext));
  mem.write_u32l(sp + 16, frame + kFDispCtx);
  auto& r = rt_.cpu().registers();
  r.w_esp(sp);
  r.w_ebp(frame);
  r.eip = handler;
  trace("seh", "%s handler %s for frame 0x%08X", unwind ? "unwind" : "call", rt_.describe(handler).c_str(), reg);
}

void Seh::on_dispatch_return(Call& c) {
  c.take_over();
  auto& mem = rt_.mem();
  auto& r = rt_.cpu().registers();
  uint32_t frame = r.r_ebp();
  if (!mem.exists(frame, kFrameSize) || mem.read_u32l(frame + kFMagic) != kMagicDispatch) {
    throw GuestError(GuestError::Kind::fatal, "exception handler returned with EBP not preserved (SEH frame lost)");
  }
  uint32_t disp = r.r_eax();
  set_fs0(mem.read_u32l(frame + kFLink));  // unlink our nested-exception record
  uint32_t rec = mem.read_u32l(frame + kFRecord), context = mem.read_u32l(frame + kFContext);
  uint32_t reg = mem.read_u32l(frame + kFReg), nested = mem.read_u32l(frame + kFNested);
  uint32_t flags = mem.read_u32l(rec + 4);
  if (nested == reg) {
    flags &= ~kExceptionNestedCall;
    nested = 0;
  }
  trace("seh", "handler for frame 0x%08X returned %u", reg, disp);
  switch (disp) {
    case kContinueExecution:
      mem.write_u32l(rec + 4, flags);
      if (flags & kExceptionNoncontinuable) {
        std::vector<uint32_t> none;
        return dispatch(status::kNoncontinuable, kExceptionNoncontinuable, none, 0, read_context(context), rec);
      }
      return resume(context);
    case kContinueSearch:
      if (flags & kExceptionStackInvalid) unhandled(rec, context);
      break;
    case kNestedException: {
      flags |= kExceptionNestedCall;
      uint32_t dc = mem.read_u32l(frame + kFDispCtx);
      if (dc > nested) nested = dc;
      break;
    }
    default: {
      mem.write_u32l(rec + 4, flags);
      std::vector<uint32_t> none;
      return dispatch(status::kInvalidDisposition, kExceptionNoncontinuable, none, 0, read_context(context), rec);
    }
  }
  mem.write_u32l(rec + 4, flags);
  mem.write_u32l(frame + kFNested, nested);
  mem.write_u32l(frame + kFReg, mem.read_u32l(reg));
  walk(frame);
}

// ---- unwind --------------------------------------------------------------------------------------

void Seh::rtl_unwind(Call& c) {
  auto& mem = rt_.mem();
  uint32_t target = c.arg(0), rec_arg = c.arg(2), retval = c.arg(3);
  // TargetIp (arg 1) is not used on x86: the unwind returns to the caller.
  Regs32 r = rt_.regs();
  r.eip = c.ret_addr();
  r.esp = c.esp + 4 + 16;
  r.eax = retval;
  c.take_over();
  uint32_t frame = new_frame(kMagicUnwind, c.esp);
  write_context(mem.read_u32l(frame + kFContext), r);
  uint32_t rec = rec_arg;
  if (!rec) {
    rec = mem.read_u32l(frame + kFRecord);
    mem.write_u32l(rec + 0x00, status::kUnwind);
    mem.write_u32l(rec + 0x0C, r.eip);
  } else {
    mem.write_u32l(frame + kFRecord, rec);
  }
  uint32_t flags = mem.read_u32l(rec + 4) | kExceptionUnwinding;
  if (!target) {
    flags |= kExceptionExitUnwind;
    target = kChainEnd;
  }
  mem.write_u32l(rec + 4, flags);
  mem.write_u32l(frame + kFTarget, target);
  mem.write_u32l(frame + kFReg, fs0());
  trace("seh", "unwind to frame 0x%08X from %s", target, rt_.describe(r.eip).c_str());
  unwind_walk(frame);
}

void Seh::unwind_walk(uint32_t frame) {
  auto& mem = rt_.mem();
  uint32_t rec = mem.read_u32l(frame + kFRecord), context = mem.read_u32l(frame + kFContext);
  uint32_t reg = mem.read_u32l(frame + kFReg), target = mem.read_u32l(frame + kFTarget);
  std::vector<uint32_t> none;
  if (reg == target) return resume(context);
  if (reg == kChainEnd || reg == 0 || (target != kChainEnd && target < reg))
    return dispatch(status::kInvalidUnwindTarget, kExceptionNoncontinuable, none, 0, read_context(context), rec);
  if (!valid_registration(reg))
    return dispatch(status::kBadStack, kExceptionNoncontinuable, none, 0, read_context(context), rec);
  call_handler(frame, reg, /*unwind=*/true);
}

void Seh::on_unwind_return(Call& c) {
  c.take_over();
  auto& mem = rt_.mem();
  auto& r = rt_.cpu().registers();
  uint32_t frame = r.r_ebp();
  if (!mem.exists(frame, kFrameSize) || mem.read_u32l(frame + kFMagic) != kMagicUnwind) {
    throw GuestError(GuestError::Kind::fatal, "unwind handler returned with EBP not preserved (SEH frame lost)");
  }
  uint32_t disp = r.r_eax();
  set_fs0(mem.read_u32l(frame + kFLink));
  uint32_t reg = mem.read_u32l(frame + kFReg);
  if (disp == kCollidedUnwind) {
    reg = mem.read_u32l(frame + kFDispCtx);
  } else if (disp != kContinueSearch) {
    std::vector<uint32_t> none;
    uint32_t rec = mem.read_u32l(frame + kFRecord), context = mem.read_u32l(frame + kFContext);
    return dispatch(status::kInvalidDisposition, kExceptionNoncontinuable, none, 0, read_context(context), rec);
  }
  // Unlink the frame just unwound (RtlpUnlinkHandler) and move on.
  uint32_t next = mem.read_u32l(reg);
  set_fs0(next);
  mem.write_u32l(frame + kFReg, next);
  unwind_walk(frame);
}

// ---- entry points --------------------------------------------------------------------------------

void Seh::raise_exception(Call& c) {
  uint32_t code = c.arg(0), flags = c.arg(1), n = c.arg(2), args = c.arg(3);
  std::vector<uint32_t> params;
  if (args) {
    if (n > 15) n = 15;
    for (uint32_t i = 0; i < n; i++) params.push_back(rt_.mem().read_u32l(args + 4 * i));
  }
  // EXCEPTION_CONTINUE_EXECUTION returns from RaiseException to its caller.
  Regs32 r = rt_.regs();
  r.eip = c.ret_addr();
  r.esp = c.esp + 4 + 16;
  c.take_over();
  // As in NT's kernel32, the exception address is RaiseException itself.
  uint32_t self = rt_.shims().thunk_address(c.fn);
  dispatch(code, flags, params, self, r);
}

void Seh::on_fault(const cpu::X86Emulator::Fault& f) {
  using X = cpu::X86Emulator;
  uint32_t code = status::kAccessViolation;
  std::vector<uint32_t> params;
  switch (f.vector) {
    case X::VEC_DE:
      code = f.divide_by_zero ? status::kIntegerDivideByZero : status::kIntegerOverflow;
      break;
    case X::VEC_PF:
      // Read/write is not reported by the CPU; 0 (read) is what most handlers expect.
      params = {0, f.address};
      break;
    case X::VEC_GP:
    case X::VEC_NP:
    case X::VEC_SS:
      params = {0, 0xFFFFFFFF};
      break;
    case X::VEC_UD:
      code = status::kIllegalInstruction;
      break;
    case X::VEC_OF:
      code = status::kIntegerOverflow;
      break;
    case X::VEC_BR:
      code = status::kArrayBoundsExceeded;
      break;
    case X::VEC_DB:
      code = status::kSingleStep;
      break;
    case X::VEC_MF: {
      const auto& fpu = rt_.cpu().fpu();
      uint16_t pending = fpu.status_word() & ~fpu.cw & 0x3F;
      if (pending & 0x01) code = (fpu.status_word() & 0x40) ? status::kFltStackCheck : status::kFltInvalidOperation;
      else if (pending & 0x04) code = status::kFltDivideByZero;
      else if (pending & 0x02) code = status::kFltDenormalOperand;
      else if (pending & 0x08) code = status::kFltOverflow;
      else if (pending & 0x10) code = status::kFltUnderflow;
      else code = status::kFltInexactResult;
      break;
    }
    default:
      break;
  }
  if (tracing("seh") || f.vector == X::VEC_UD) {
    // #UD is how an instruction the CPU does not implement shows up: say so
    // even when the guest goes on to handle it.
    trace("seh", "cpu fault %s", f.str().c_str());
  }
  dispatch(code, 0, params, f.eip, rt_.regs());
}

void Seh::on_software_interrupt(uint8_t vector) {
  auto& r = rt_.cpu().registers();
  Regs32 s = rt_.regs();
  if (vector == 3) {
    // NT reports a breakpoint at the int3 itself.
    s.eip = r.eip - 1;
    std::vector<uint32_t> params = {0};
    return dispatch(status::kBreakpoint, 0, params, s.eip, s);
  }
  s.eip = r.eip - 2;
  std::vector<uint32_t> params = {0, 0xFFFFFFFF};
  dispatch(status::kAccessViolation, 0, params, s.eip, s);
}

uint32_t Seh::unhandled_exception_filter(uint32_t ptrs) {
  uint32_t rec = rt_.mem().read_u32l(ptrs), context = rt_.mem().read_u32l(ptrs + 4);
  log("UnhandledExceptionFilter: %s", describe_exception(rec, context).c_str());
  return 1;  // EXCEPTION_EXECUTE_HANDLER: the caller terminates the process
}

void Seh::unhandled(uint32_t rec, uint32_t context) {
  uint32_t code = rt_.mem().read_u32l(rec);
  throw GuestError(GuestError::Kind::unhandled, "unhandled " + describe_exception(rec, context), code);
}

std::string Seh::code_name(uint32_t code) {
  const char* n = nullptr;
  switch (code) {
    case status::kBreakpoint: n = "breakpoint"; break;
    case status::kSingleStep: n = "single step"; break;
    case status::kAccessViolation: n = "access violation"; break;
    case status::kIllegalInstruction: n = "illegal instruction"; break;
    case status::kNoncontinuable: n = "noncontinuable exception"; break;
    case status::kInvalidDisposition: n = "invalid disposition"; break;
    case status::kUnwind: n = "unwind"; break;
    case status::kBadStack: n = "bad stack"; break;
    case status::kInvalidUnwindTarget: n = "invalid unwind target"; break;
    case status::kArrayBoundsExceeded: n = "array bounds exceeded"; break;
    case status::kFltDenormalOperand: n = "float denormal operand"; break;
    case status::kFltDivideByZero: n = "float divide by zero"; break;
    case status::kFltInexactResult: n = "float inexact result"; break;
    case status::kFltInvalidOperation: n = "float invalid operation"; break;
    case status::kFltOverflow: n = "float overflow"; break;
    case status::kFltStackCheck: n = "float stack check"; break;
    case status::kFltUnderflow: n = "float underflow"; break;
    case status::kIntegerDivideByZero: n = "integer divide by zero"; break;
    case status::kIntegerOverflow: n = "integer overflow"; break;
    case status::kPrivilegedInstruction: n = "privileged instruction"; break;
    case 0x0EEDFAE6: n = "Borland C++ exception"; break;
    case 0xE06D7363: n = "MSVC C++ exception"; break;
    default: break;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "0x%08X%s%s%s", code, n ? " (" : "", n ? n : "", n ? ")" : "");
  return buf;
}

std::string Seh::describe_exception(uint32_t rec, uint32_t context) const {
  auto& mem = rt_.mem();
  std::string s;
  try {
    uint32_t code = mem.read_u32l(rec), addr = mem.read_u32l(rec + 0x0C), n = mem.read_u32l(rec + 0x10);
    s = "exception " + code_name(code) + " at " + rt_.describe(addr);
    if (code == status::kAccessViolation && n >= 2) {
      char buf[64];
      uint32_t what = mem.read_u32l(rec + 0x14), where = mem.read_u32l(rec + 0x18);
      if (where == 0xFFFFFFFF) snprintf(buf, sizeof(buf), " (general protection)");
      else snprintf(buf, sizeof(buf), " (%s 0x%08X)", what ? "writing" : "reading", where);
      s += buf;
    }
    Regs32 r = read_context(context);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "; eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X eip=%08X", r.eax, r.ebx,
             r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip);
    s += buf;
  } catch (const std::exception&) {
    s += " (exception record unreadable)";
  }
  return s;
}

}  // namespace adw::win32
