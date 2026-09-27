// x86_oracle — the native half of the adw::cpu differential test. Built as a 32-bit (i686) program, it runs under
// WOW64 on the real CPU: for each test case it loads the registers, flags, x87 control word and memory a case
// describes, jumps into the machine-code snippet, and records what the hardware left behind. The test driver runs
// the same cases on the emulator and compares.
//
//   x86_oracle <cases.bin> <results.bin>
//
// The snippet runs between a generated prologue and epilogue (below) with ESP on its own small stack; faults are
// caught by a vectored exception handler, which records the exception and resumes at a recovery stub.

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "diff_proto.hh"

using namespace diffproto;

namespace {

// Control block (fixed address, so the generated code can use absolute operands).
constexpr uint32_t CTRL = 0x33000000;
constexpr uint32_t C_IN_REGS = CTRL + 0x00; // 8 dwords (esp slot = initial ESP)
constexpr uint32_t C_IN_FLAGS = CTRL + 0x20;
constexpr uint32_t C_IN_FCW = CTRL + 0x24;
constexpr uint32_t C_OUT_REGS = CTRL + 0x40;
constexpr uint32_t C_OUT_FLAGS = CTRL + 0x60;
constexpr uint32_t C_HOST_ESP = CTRL + 0x64;
constexpr uint32_t C_HOST_FCW = CTRL + 0x68;
constexpr uint32_t C_FSAVE = CTRL + 0x80; // 108 bytes
constexpr uint32_t C_SCRATCH_TOP = CTRL + 0x800;

constexpr uint32_t STUB = CODE_BASE + 0x8000; // prologue
constexpr uint32_t RECOVER = CODE_BASE + 0x8100; // fault recovery

struct Emitter {
  uint8_t* p;
  void b(uint8_t v) {
    *p++ = v;
  }
  void d(uint32_t v) {
    memcpy(p, &v, 4);
    p += 4;
  }
  void op_abs(uint8_t op, uint8_t modrm, uint32_t addr) { // op r/m32 with [disp32]
    b(op);
    b(modrm);
    d(addr);
  }
};

void emit_prologue(uint8_t* at) {
  Emitter e{at};
  e.b(0x60); // pushad
  e.b(0x9C); // pushfd
  e.op_abs(0x89, 0x25, C_HOST_ESP); // mov [host_esp], esp
  e.b(0xD9); // fnstcw [host_fcw]
  e.b(0x3D);
  e.d(C_HOST_FCW);
  e.b(0xDB); // fninit
  e.b(0xE3);
  e.b(0xD9); // fldcw [in_fcw]
  e.b(0x2D);
  e.d(C_IN_FCW);
  // Flags are loaded while still on the host stack so nothing is pushed onto the snippet's stack.
  e.op_abs(0xFF, 0x35, C_IN_FLAGS); // push [in_flags]
  e.b(0x9D); // popfd
  e.op_abs(0x8B, 0x25, C_IN_REGS + 4 * 4); // mov esp, [in esp]
  e.b(0xA1); // mov eax, [in eax]
  e.d(C_IN_REGS + 0);
  e.op_abs(0x8B, 0x0D, C_IN_REGS + 4 * 1); // ecx
  e.op_abs(0x8B, 0x15, C_IN_REGS + 4 * 2); // edx
  e.op_abs(0x8B, 0x1D, C_IN_REGS + 4 * 3); // ebx
  e.op_abs(0x8B, 0x2D, C_IN_REGS + 4 * 5); // ebp
  e.op_abs(0x8B, 0x35, C_IN_REGS + 4 * 6); // esi
  e.op_abs(0x8B, 0x3D, C_IN_REGS + 4 * 7); // edi
  // jmp CODE_BASE
  e.b(0xE9);
  e.d(CODE_BASE - (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e.p)) + 4));
}

// Placed right after the snippet: record everything, then return to the C caller.
uint32_t emit_epilogue(uint8_t* at) {
  Emitter e{at};
  e.b(0xA3); // mov [out eax], eax
  e.d(C_OUT_REGS + 0);
  e.op_abs(0x89, 0x0D, C_OUT_REGS + 4 * 1); // ecx
  e.op_abs(0x89, 0x15, C_OUT_REGS + 4 * 2); // edx
  e.op_abs(0x89, 0x1D, C_OUT_REGS + 4 * 3); // ebx
  e.op_abs(0x89, 0x25, C_OUT_REGS + 4 * 4); // esp
  e.op_abs(0x89, 0x2D, C_OUT_REGS + 4 * 5); // ebp
  e.op_abs(0x89, 0x35, C_OUT_REGS + 4 * 6); // esi
  e.op_abs(0x89, 0x3D, C_OUT_REGS + 4 * 7); // edi
  e.b(0xBC); // mov esp, scratch (pushfd must not touch the snippet's stack)
  e.d(C_SCRATCH_TOP);
  e.b(0x9C); // pushfd
  e.op_abs(0x8F, 0x05, C_OUT_FLAGS); // pop [out flags]
  e.b(0xDD); // fnsave [fsave]
  e.b(0x35);
  e.d(C_FSAVE);
  e.b(0xFC); // cld
  e.op_abs(0x8B, 0x25, C_HOST_ESP); // mov esp, [host_esp]
  e.b(0xD9); // fldcw [host_fcw]
  e.b(0x2D);
  e.d(C_HOST_FCW);
  e.b(0x9D); // popfd
  e.b(0x61); // popad
  e.b(0xC3); // ret
  return static_cast<uint32_t>(e.p - at);
}

void emit_recover(uint8_t* at) {
  Emitter e{at};
  e.b(0xDB); // fninit
  e.b(0xE3);
  e.b(0xFC); // cld
  e.op_abs(0x8B, 0x25, C_HOST_ESP); // mov esp, [host_esp]
  e.b(0xD9); // fldcw [host_fcw]
  e.b(0x2D);
  e.d(C_HOST_FCW);
  e.b(0x9D); // popfd
  e.b(0x61); // popad
  e.b(0xC3); // ret
}

volatile uint32_t g_fault_code = 0;
volatile uint32_t g_fault_eip = 0;
uint32_t g_fault_regs[9];

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
  CONTEXT* ctx = info->ContextRecord;
  if (ctx->Eip < CODE_BASE || ctx->Eip >= CODE_BASE + 0x10000) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  g_fault_code = info->ExceptionRecord->ExceptionCode;
  g_fault_eip = ctx->Eip;
  g_fault_regs[0] = ctx->Eax;
  g_fault_regs[1] = ctx->Ecx;
  g_fault_regs[2] = ctx->Edx;
  g_fault_regs[3] = ctx->Ebx;
  g_fault_regs[4] = ctx->Esp;
  g_fault_regs[5] = ctx->Ebp;
  g_fault_regs[6] = ctx->Esi;
  g_fault_regs[7] = ctx->Edi;
  g_fault_regs[8] = ctx->EFlags;
  ctx->Eip = RECOVER;
  ctx->EFlags &= ~0x400; // DF
  return EXCEPTION_CONTINUE_EXECUTION;
}

bool alloc_at(uint32_t addr, uint32_t size, DWORD protect) {
  void* p = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), size, MEM_RESERVE | MEM_COMMIT, protect);
  return p == reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: x86_oracle <cases.bin> <results.bin>\n");
    return 2;
  }
  if (!alloc_at(CODE_BASE, 0x10000, PAGE_EXECUTE_READWRITE) || !alloc_at(DATA_BASE, 0x1000, PAGE_READWRITE) ||
      // 64 KiB below the snippet stack: a fault makes Windows push the exception frames onto the 32-bit stack.
      !alloc_at(STACK_BASE - 0x10000, 0x11000, PAGE_READWRITE) || !alloc_at(CTRL, 0x1000, PAGE_READWRITE)) {
    fprintf(stderr, "x86_oracle: cannot map the fixed test regions (error %lu)\n", GetLastError());
    return 3;
  }

  FILE* in = fopen(argv[1], "rb");
  if (!in) {
    fprintf(stderr, "x86_oracle: cannot open %s\n", argv[1]);
    return 2;
  }
  std::vector<CaseIn> cases;
  CaseIn c;
  while (fread(&c, sizeof(c), 1, in) == 1) {
    cases.push_back(c);
  }
  fclose(in);

  uint8_t* code = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(CODE_BASE));
  emit_prologue(code + (STUB - CODE_BASE));
  emit_recover(code + (RECOVER - CODE_BASE));
  AddVectoredExceptionHandler(1, on_exception);

  std::vector<CaseOut> results(cases.size());
  uint8_t* data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(DATA_BASE));
  uint8_t* stack = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(STACK_BASE));
  uint32_t* ctrl = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(CTRL));
  auto run = reinterpret_cast<void (*)()>(static_cast<uintptr_t>(STUB));

  for (size_t z = 0; z < cases.size(); z++) {
    const CaseIn& k = cases[z];
    CaseOut& out = results[z];
    memset(&out, 0, sizeof(out));

    memcpy(code, k.code, k.code_len);
    emit_epilogue(code + k.code_len);
    FlushInstructionCache(GetCurrentProcess(), code, 0x100);
    fill_region(data, DATA_SIZE, k.seed);
    fill_region(stack, STACK_SIZE, k.seed ^ 0x9E3779B9);
    memcpy(data, k.patch, k.patch_len);
    memset(ctrl, 0, 0x100);
    memcpy(ctrl, k.regs, 32);
    ctrl[0x20 / 4] = k.eflags;
    ctrl[0x24 / 4] = k.fcw;

    g_fault_code = 0;
    run();

    out.fault_code = g_fault_code;
    if (g_fault_code) {
      out.fault_eip = g_fault_eip;
      memcpy(out.regs, g_fault_regs, 32);
      out.eflags = g_fault_regs[8];
    } else {
      memcpy(out.regs, ctrl + 0x40 / 4, 32);
      out.eflags = ctrl[0x60 / 4];
      memcpy(out.fpu, reinterpret_cast<uint8_t*>(ctrl) + 0x80, 108);
    }
    memcpy(out.data, data, DATA_SIZE);
    memcpy(out.stack, stack, STACK_SIZE);
  }

  FILE* outf = fopen(argv[2], "wb");
  if (!outf) {
    fprintf(stderr, "x86_oracle: cannot create %s\n", argv[2]);
    return 2;
  }
  fwrite(results.data(), sizeof(CaseOut), results.size(), outf);
  fclose(outf);
  return 0;
}
