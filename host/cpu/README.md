# host/cpu — the x86 interpreter

`adw_cpu` runs the modules' x86 code: flat 32-bit code for the pe32 lane and
16-bit protected-mode code (an LDT, segment limits, far transfers, 16-bit
stacks) for the ne16 lane. Its contract is `docs/DESIGN.md` §2.

## Where it comes from

The interpreter is vendored from resource_dasm's `X86Emulator`
(<https://github.com/swannman/resource_dasm>, branch `afterdark-perf`,
commit 02d8ea9; itself a fork of fuzziqersoftware/resource_dasm), MIT,
© Martin Michelsen. The license text is `LICENSE.resource_dasm`. Each source
file's header says what Long After Dark changed.

- `X86Exec.cc` (execution) is largely rewritten here: segmentation, faults,
  the x87 on the host FPU, and the forms upstream left out.
- `X86Emulator.cc` (disassembler and assembler), `MemoryContext`,
  `EmulatorBase`, `Expression` and `InterruptManager` stay close to upstream.

## Upstream TODOs

The `TODO` comments in `src/` are upstream's own, kept as they were so the
files still diff cleanly against resource_dasm. None of them is a gap in what
the modules need:

- `X86Emulator.cc` (lines ~29, 1126, 2652–2682, 3956, 4427, 4502, 6021):
  disassembler and assembler notes. Execution does not use that code.
- `MemoryContext.cc`/`.hh` (~186, 438; ~377): arena lookups that are
  linear in the number of arenas. A process has a handful of arenas.

Long After Dark's own work has no `TODO` markers. A known gap is written
down where it lives, for example the "Known gaps" blocks in `host/win32` and
`host/win16`.

## Speed

A byte-at-a-time interpreter has no decode stage to cache, because each
instruction's handler fetches its own operand bytes. Profiling a 16-bit
module (FROST, with a sampling profiler over a `-g` build) showed where the
time went: about 45% was spent translating every instruction-byte fetch
(segment check, then `MemoryContext::at`'s page lookup).

Two translation caches remove that cost. Both read live guest memory, so
self-modifying code and host writes are always seen.

- **The fetch window** (`X86Emulator::fetch_instruction_data`) is the part of
  the current code arena inside the CS limit, held as one host span. It is
  checked once per instruction against `MemoryContext::layout_generation()`,
  which changes whenever an arena comes or goes. A CS load drops it.
- **Two data windows** (`r_mem`/`w_mem`) hold the last two arenas that data
  accesses went to. They are checked against the same generation on every
  access, because host helpers use them between instructions. Writes still
  bump their page's write generation.

The results, on the same machine:

- `cpu_perf` (the flat loop): 74 to about 120 MIPS.
- Segmented code: 73 to about 117 MIPS.
- Headless host time per frame: SATORI 19.5 to 13.9 ms, FROST 15.8 to 10.6 ms,
  TOILET 15.0 to 10.2 ms.

All 202 modules draw byte-identical FBHASH streams before and after.
`cpu_segmentation`'s `test_fetch_window` covers the three invariants:
rewritten code is seen, the CS limit still faults, and a re-created arena
is never read through a stale span.

A true decode cache (pre-decoded instructions, invalidated through
`page_write_gen`) would need every handler to read from a decoded form
instead of fetching its own bytes. The remaining profile is spread over
dispatch, ModR/M decoding and flag computation.
