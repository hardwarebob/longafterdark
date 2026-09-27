// Structured exception handling per the Win32 x86 ABI: the FS:[0] chain of
// EXCEPTION_REGISTRATION records {Next, Handler}, handlers called as
//   EXCEPTION_DISPOSITION handler(EXCEPTION_RECORD*, void* EstablisherFrame,
//                                 CONTEXT*, void* DispatcherContext)   (cdecl)
// with the dispositions ContinueExecution (0), ContinueSearch (1),
// NestedException (2) and CollidedUnwind (3), and RtlUnwind for the unwind
// pass (Borland's FarThrow/longjmp and C++ catch both use it — ABI.md §4).
//
// Sources of exceptions: RaiseException (Borland C++ throw), CPU faults in
// guest code (#DE → INTEGER_DIVIDE_BY_ZERO/OVERFLOW, #PF/#GP →
// ACCESS_VIOLATION, #UD → ILLEGAL_INSTRUCTION, #MF → the x87 codes, …), `int 3`
// and stray `int n`, and bad pointers handed to shims (the CPU reports those
// as a #PF at the thunk, i.e. an access violation raised by the API).
//
// Design: the dispatcher is a host-side state machine whose state lives on
// the GUEST stack, never on the host stack. Starting a dispatch writes a frame
// below the current guest ESP:
//
//     [handler args][DispatchFrame][EXCEPTION_RECORD][CONTEXT]   ← old ESP
//
// and "calls" the first handler by setting ESP/EIP, with EBP = the frame (EBP
// is callee-saved, so it survives the handler) and a return address pointing
// at an internal continuation thunk, which reads the frame back through EBP
// and takes the next step. No host frame waits for a handler to return — so a
// handler that never returns (a C++ catch: RtlUnwind, then a jump into the
// catch block) simply abandons the guest-stack frame, exactly as on Windows.
// Around each handler call the dispatcher links its own registration record
// (inside the frame), whose handler reports nested exceptions / collided
// unwinds, as ntdll's RtlpExecuteHandlerForException/ForUnwind do.
//
// An exception that reaches the end of the chain (FS:[0] = -1) ends the lane
// with GuestError::Kind::unhandled and a description of the fault. Resuming a
// CONTEXT whose ESP lies above the innermost host callback (call_guest) would
// mean unwinding host frames too; that is reported as a fatal error.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "X86Emulator.hh"
#include "win32/runtime.hh"

namespace adw::win32 {

namespace status {
constexpr uint32_t kBreakpoint = 0x80000003;
constexpr uint32_t kSingleStep = 0x80000004;
constexpr uint32_t kAccessViolation = 0xC0000005;
constexpr uint32_t kIllegalInstruction = 0xC000001D;
constexpr uint32_t kNoncontinuable = 0xC0000025;
constexpr uint32_t kInvalidDisposition = 0xC0000026;
constexpr uint32_t kUnwind = 0xC0000027;
constexpr uint32_t kBadStack = 0xC0000028;
constexpr uint32_t kInvalidUnwindTarget = 0xC0000029;
constexpr uint32_t kArrayBoundsExceeded = 0xC000008C;
constexpr uint32_t kFltDenormalOperand = 0xC000008D;
constexpr uint32_t kFltDivideByZero = 0xC000008E;
constexpr uint32_t kFltInexactResult = 0xC000008F;
constexpr uint32_t kFltInvalidOperation = 0xC0000090;
constexpr uint32_t kFltOverflow = 0xC0000091;
constexpr uint32_t kFltStackCheck = 0xC0000092;
constexpr uint32_t kFltUnderflow = 0xC0000093;
constexpr uint32_t kIntegerDivideByZero = 0xC0000094;
constexpr uint32_t kIntegerOverflow = 0xC0000095;
constexpr uint32_t kPrivilegedInstruction = 0xC0000096;
}  // namespace status

// EXCEPTION_RECORD.ExceptionFlags
constexpr uint32_t kExceptionNoncontinuable = 0x01;
constexpr uint32_t kExceptionUnwinding = 0x02;
constexpr uint32_t kExceptionExitUnwind = 0x04;
constexpr uint32_t kExceptionStackInvalid = 0x08;
constexpr uint32_t kExceptionNestedCall = 0x10;
constexpr uint32_t kExceptionTargetUnwind = 0x20;
constexpr uint32_t kExceptionCollidedUnwind = 0x40;

// x86 CONTEXT (0x2CC bytes) field offsets.
namespace ctx {
constexpr uint32_t kSize = 0x2CC;
constexpr uint32_t kFlags = 0x00, kFloatSave = 0x1C, kGs = 0x8C, kFs = 0x90, kEs = 0x94, kDs = 0x98;
constexpr uint32_t kEdi = 0x9C, kEsi = 0xA0, kEbx = 0xA4, kEdx = 0xA8, kEcx = 0xAC, kEax = 0xB0;
constexpr uint32_t kEbp = 0xB4, kEip = 0xB8, kCs = 0xBC, kEflags = 0xC0, kEsp = 0xC4, kSs = 0xC8;
}  // namespace ctx
constexpr uint32_t kExceptionRecordSize = 0x50;

class Seh {
 public:
  explicit Seh(Runtime& rt);
  // Registers the continuation thunks (needs the shim registry).
  void install();

  // A CPU fault in guest code (EIP/ESP already rolled back to the faulting
  // instruction): starts a dispatch.
  void on_fault(const cpu::X86Emulator::Fault& f);
  // `int n` (n ≠ the thunk vector), EIP past the instruction.
  void on_software_interrupt(uint8_t vector);

  // KERNEL32 entry points (kernel32.cc binds them).
  void raise_exception(Call& c);                     // RaiseException
  void rtl_unwind(Call& c);                          // RtlUnwind
  uint32_t unhandled_exception_filter(uint32_t exception_pointers);

  // Starts dispatching an exception; `resume` is the state
  // EXCEPTION_CONTINUE_EXECUTION returns to. Sets EIP/ESP to run the first
  // handler (or throws GuestError when there is none).
  void dispatch(uint32_t code, uint32_t flags, std::span<const uint32_t> params, uint32_t address,
                const Regs32& resume, uint32_t chained_record = 0);

  uint64_t dispatched() const { return dispatched_; }
  static std::string code_name(uint32_t code);

  // Guest addresses of the internal routines (tests use them to recognize frames).
  uint32_t dispatch_return_thunk() const { return dispatch_return_; }
  uint32_t unwind_return_thunk() const { return unwind_return_; }

 private:
  uint32_t fs0() const;
  void set_fs0(uint32_t v);
  bool valid_registration(uint32_t reg) const;
  uint32_t new_frame(uint32_t magic, uint32_t top);
  void write_context(uint32_t addr, const Regs32& r);
  Regs32 read_context(uint32_t addr) const;
  void resume(uint32_t context);
  void call_handler(uint32_t frame, uint32_t reg, bool unwind);
  void walk(uint32_t frame);
  void unwind_walk(uint32_t frame);
  void on_dispatch_return(Call& c);
  void on_unwind_return(Call& c);
  [[noreturn]] void unhandled(uint32_t record, uint32_t context);
  std::string describe_exception(uint32_t record, uint32_t context) const;

  Runtime& rt_;
  uint32_t dispatch_return_ = 0, unwind_return_ = 0;
  uint32_t dispatch_nested_ = 0, unwind_nested_ = 0;
  uint64_t dispatched_ = 0;
};

}  // namespace adw::win32
