// Flat-mode interpreter speed: adw::cpu::X86Emulator against upstream resource_dasm's X86Emulator (compiled from the
// clone in third_party/win/resource_dasm). DESIGN.md §2 requires that segmentation cost flat (Win32-lane) code
// nothing measurable, so this fails only if adw::cpu is more than 10% slower than upstream on the same loop.
//
// The loop is assembled once (by adw::cpu's assembler) and run by both emulators from the same address with the same
// initial state; each emulator gets several timed runs and the best one counts, which filters out scheduler noise.
// Both runs must also finish in the same architectural state, so the comparison is between equal amounts of work.
//
//   test_perf [--iterations N]

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include "X86Emulator.hh"

#ifdef AD_HAVE_UPSTREAM_X86
#include "Emulators/X86Emulator.hh"
#endif

namespace {

constexpr uint32_t CODE = 0x00401000;
constexpr uint32_t DATA = 0x00500000; // 8 KiB: a 4 KiB source table, then a 4 KiB destination
constexpr uint32_t STACK_TOP = 0x0060F000;

// A little of everything engine code does per pixel/sprite: loads and stores with base+disp and base+index*scale
// addressing, ALU ops, a rotate, movzx, lea arithmetic, a near call with a stack argument, push/pop, and taken and untaken
// conditional branches (only forms upstream implements; it lacks e.g. imul r, r/m, imm). The trailing int3 ends the
// run.
const char* const kLoop = R"(
  mov ecx, ITERATIONS
  xor eax, eax
  mov ebx, 0x9E3779B9
  xor edi, edi
top:
  mov edx, [edi + 0x00500000]
  add edx, eax
  rol edx, 5
  xor edx, ebx
  mov [edi + 0x00501000], edx
  movzx esi, dl
  add eax, esi
  lea esi, [esi + esi * 2]
  sub eax, esi
  push eax
  call helper
  add esp, 4
  add edi, 4
  and edi, 0xFFC
  test ecx, 1
  jz skip
  inc eax
skip:
  cmp eax, [esp - 8]
  adc eax, 0
  dec ecx
  jnz top
  int 3
helper:
  push ebp
  mov ebp, esp
  mov edx, [ebp + 8]
  lea eax, [edx + eax * 2 + 1]
  pop ebp
  ret
)";

// Informational only (no pass/fail): a floating-point loop of the kind the 3D modules run, which exercises the x87
// unit (each x87 instruction is executed on the host FPU; see X87.cc).
const char* const kFpuLoop = R"(
  mov ecx, ITERATIONS
  mov esi, 0x00500000
  fld1 st
  fldz st
fpu_top:
  fld st, qword [esi + 8]
  fmul st, st2
  fadd st, qword [esi + 16]
  fsqrt st
  faddp st1, st
  fld st, st0
  fmul st, qword [esi + 24]
  fstp qword [esi + 32], st
  dec ecx
  jnz fpu_top
  fstp qword [esi + 40], st
  fstp qword [esi + 48], st
  int 3
)";

// How the adw::cpu run is set up.
enum class Mode {
  FLAT, // set_flat_mode(): identity segments, the Win32 lane
  SEGMENTED, // 32-bit segments with a nonzero base: every access takes the checked, base-adding path
};

// The segmented run relocates everything by this much: segment base + the same offsets.
constexpr uint32_t SEG_BASE = 0x10000000;

struct Final {
  uint32_t regs[8];
  uint32_t eflags;
  uint32_t checksum;
  uint64_t instructions;
};

uint32_t fnv(const uint8_t* p, size_t n) {
  uint32_t h = 0x811C9DC5;
  for (size_t z = 0; z < n; z++) {
    h = (h ^ p[z]) * 0x01000193;
  }
  return h;
}

std::string table() {
  std::string t(0x1000, '\0');
  uint32_t x = 0x2545F491;
  for (auto& c : t) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c = static_cast<char>(x);
  }
  // The x87 loop reads doubles from offsets 8-31: give it tame ones.
  for (uint32_t z = 8; z < 32; z += 8) {
    double v = 0.25 * z;
    memcpy(t.data() + z, &v, 8);
  }
  return t;
}

double run_adw(const std::string& code, Final& out, Mode mode = Mode::FLAT) {
  using namespace adw::cpu;
  uint32_t base = (mode == Mode::SEGMENTED) ? SEG_BASE : 0;
  auto mem = std::make_shared<MemoryContext>();
  mem->allocate_at(base + CODE, 0x1000);
  mem->allocate_at(base + DATA, 0x2000);
  mem->allocate_at(base + STACK_TOP - 0x10000, 0x10000);
  mem->write(base + CODE, code);
  mem->write(base + DATA, table());
  X86Emulator emu(mem);
  emu.set_flat_mode(0);
  if (mode == Mode::SEGMENTED) {
    SegDesc code_desc{SEG_BASE, 0x0FFFFFFF, true, true, true, true, 3};
    SegDesc data_desc{SEG_BASE, 0x0FFFFFFF, true, false, true, true, 3};
    emu.set_segment(X86Emulator::SegReg::CS, 0x0F, code_desc);
    emu.set_segment(X86Emulator::SegReg::DS, 0x17, data_desc);
    emu.set_segment(X86Emulator::SegReg::ES, 0x17, data_desc);
    emu.set_segment(X86Emulator::SegReg::SS, 0x17, data_desc);
  }
  emu.set_interrupt_handler([](X86Emulator& e, uint8_t) { e.request_stop(); });
  auto& r = emu.registers();
  r.eip = CODE;
  r.w_esp(STACK_TOP);
  r.write_eflags(0x202);
  auto t0 = std::chrono::steady_clock::now();
  emu.run(UINT64_MAX);
  auto t1 = std::chrono::steady_clock::now();
  for (uint8_t z = 0; z < 8; z++) {
    out.regs[z] = r.read32(z);
  }
  out.eflags = r.read_eflags() & 0x8D5;
  std::string d = mem->read(base + DATA, 0x2000);
  out.checksum = fnv(reinterpret_cast<const uint8_t*>(d.data()), d.size());
  out.instructions = emu.cycles();
  return std::chrono::duration<double>(t1 - t0).count();
}

#ifdef AD_HAVE_UPSTREAM_X86
double run_upstream(const std::string& code, Final& out) {
  using namespace ResourceDASM;
  auto mem = std::make_shared<MemoryContext>();
  mem->allocate_at(CODE, 0x1000);
  mem->allocate_at(DATA, 0x2000);
  mem->allocate_at(STACK_TOP - 0x10000, 0x10000);
  mem->write(CODE, code);
  mem->write(DATA, table());
  X86Emulator emu(mem);
  emu.set_syscall_handler([](X86Emulator&, uint8_t) { throw X86Emulator::terminate_emulation(); });
  auto& r = emu.registers();
  r.eip = CODE;
  r.w_esp(STACK_TOP);
  r.write_eflags(0x202);
  auto t0 = std::chrono::steady_clock::now();
  emu.execute();
  auto t1 = std::chrono::steady_clock::now();
  for (uint8_t z = 0; z < 8; z++) {
    out.regs[z] = r.read32(z);
  }
  out.eflags = r.read_eflags() & 0x8D5;
  std::string d = mem->read(DATA, 0x2000);
  out.checksum = fnv(reinterpret_cast<const uint8_t*>(d.data()), d.size());
  out.instructions = emu.cycles();
  return std::chrono::duration<double>(t1 - t0).count();
}
#endif

std::string assemble_loop(const char* text, uint32_t iterations) {
  std::string s = text;
  s.replace(s.find("ITERATIONS"), 10, std::to_string(iterations));
  return adw::cpu::X86Emulator::assemble(s, nullptr, CODE).code;
}

// Best-of-n timing of one adw::cpu configuration.
double best_adw(const std::string& code, Final& out, Mode mode, int runs) {
  double best = 1e30;
  for (int z = 0; z < runs; z++) {
    best = std::min(best, run_adw(code, out, mode));
  }
  return best;
}

} // namespace

int main(int argc, char** argv) {
  uint32_t iterations = 1500000;
  for (int z = 1; z + 1 < argc; z++) {
    if (!strcmp(argv[z], "--iterations")) {
      iterations = strtoul(argv[++z], nullptr, 0);
    }
  }
  // Timing is only meaningful at normal priority with nothing else competing; ask for a little more.
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  std::string code, fpu_code;
  try {
    code = assemble_loop(kLoop, iterations);
    fpu_code = assemble_loop(kFpuLoop, iterations / 4);
  } catch (const std::exception& e) {
    printf("FAIL: cannot assemble the benchmark loops: %s\n", e.what());
    return 1;
  }

  constexpr int RUNS = 5;
  Final adw_final{};
  double adw_best = best_adw(code, adw_final, Mode::FLAT, RUNS);
  printf("adw::cpu, flat:                  %.3f s best of %d, %llu instructions, %.1f MIPS\n", adw_best, RUNS,
      static_cast<unsigned long long>(adw_final.instructions), adw_final.instructions / adw_best / 1e6);

  // Informational: the same loop through non-identity segments, and the x87 loop.
  Final seg_final{};
  double seg_best = best_adw(code, seg_final, Mode::SEGMENTED, 3);
  printf("adw::cpu, segmented (base != 0): %.3f s best of 3, %.1f MIPS (%.2fx the flat time)\n", seg_best,
      seg_final.instructions / seg_best / 1e6, seg_best / adw_best);
  if (memcmp(seg_final.regs, adw_final.regs, sizeof(adw_final.regs)) || seg_final.checksum != adw_final.checksum) {
    printf("FAIL: the segmented run finished in a different state from the flat one\n");
    return 1;
  }
  Final fpu_final{};
  double fpu_best = best_adw(fpu_code, fpu_final, Mode::FLAT, 3);
  printf("adw::cpu, x87 loop:              %.3f s best of 3, %llu instructions, %.1f MIPS\n", fpu_best,
      static_cast<unsigned long long>(fpu_final.instructions), fpu_final.instructions / fpu_best / 1e6);

#ifndef AD_HAVE_UPSTREAM_X86
  printf("SKIP: upstream resource_dasm clone not present (third_party/win/resource_dasm); no comparison made\n");
  return 77;
#else
  Final up_final{};
  double up_best = 1e30;
  for (int z = 0; z < RUNS; z++) {
    up_best = std::min(up_best, run_upstream(code, up_final));
  }
  printf("upstream resource_dasm, flat:    %.3f s best of %d, %llu instructions, %.1f MIPS\n", up_best, RUNS,
      static_cast<unsigned long long>(up_final.instructions), up_final.instructions / up_best / 1e6);

  // The instruction counts differ by one (adw::cpu counts the final int3; upstream leaves from inside it), so the two
  // runs are held to equal work by comparing their final architectural state instead.
  if (memcmp(adw_final.regs, up_final.regs, sizeof(adw_final.regs)) || adw_final.eflags != up_final.eflags ||
      adw_final.checksum != up_final.checksum) {
    printf("FAIL: the two emulators finished the loop in different states (eax %08X vs %08X, data %08X vs %08X)\n",
        adw_final.regs[0], up_final.regs[0], adw_final.checksum, up_final.checksum);
    return 1;
  }
  double ratio = adw_best / up_best;
  printf("adw::cpu / upstream time (flat): %.3f (%s)\n", ratio,
      (ratio <= 1.0) ? "adw::cpu is faster" : "adw::cpu is slower");
  if (ratio > 1.10) {
    printf("FAIL: adw::cpu is more than 10%% slower than upstream in flat mode\n");
    return 1;
  }
  return 0;
#endif
}
