// Win16 local heaps: LocalInit/LocalAlloc/… inside one segment (a module's
// DGROUP, usually). The bookkeeping is host-side; what the guest can see
// follows Windows:
//   * a fixed block's handle IS its near pointer, 4-byte aligned;
//   * a moveable block's handle is the near address of a 4-byte handle-table
//     entry inside the heap — WORD pointer, BYTE flags, BYTE lock count — so
//     handles are ≡ 2 (mod 4), never mistaken for fixed pointers, and
//     `*(NPSTR*)h` is the block's address (old code does that instead of
//     LocalLock);
//   * LocalInit writes the heap's start to the instance-data word DS:[6]
//     (pLocalHeap) of a DGROUP.
// Blocks never move by themselves. The heap may grow past the size LocalInit
// gave it, up to the segment's end: the lane reserves every DGROUP at 64 KiB,
// and modules that outgrow their NE header's initial heap (Win16 grew DGROUP
// on demand) keep working.
//
// Local* functions work on the heap of the segment in DS at the call, which
// the KERNEL shims pass in.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "MemoryContext.hh"
#include "win16/ldt.hh"

namespace adw::win16 {

class LocalHeaps16 {
 public:
  static constexpr uint16_t kMoveable = 0x0002;
  static constexpr uint16_t kZeroInit = 0x0040;
  static constexpr uint16_t kModify = 0x0080;
  static constexpr uint16_t kDiscardable = 0x0F00;
  static constexpr uint16_t kFlagDiscarded = 0x4000;

  LocalHeaps16(cpu::MemoryContext& mem, Ldt& ldt) : mem_(mem), ldt_(ldt) {}

  // A module's automatic data segment: where its static data ends, so
  // LocalInit(seg, 0, size) and a lazy heap know where the heap starts, and
  // that its first 16 bytes are the Win16 instance data (DS:[6] = pLocalHeap).
  void note_dgroup(uint16_t sel, uint16_t static_end);
  void forget(uint16_t sel);

  // LocalInit: start 0 = right after the static data (size = end bytes).
  bool init(uint16_t seg, uint16_t start, uint16_t end);
  bool has_heap(uint16_t seg) const;

  uint16_t alloc(uint16_t ds, uint16_t flags, uint16_t size);             // handle or 0
  uint16_t realloc(uint16_t ds, uint16_t h, uint16_t size, uint16_t flags);  // handle or 0
  uint16_t free(uint16_t ds, uint16_t h);                                // 0 on success, else h
  uint16_t lock(uint16_t ds, uint16_t h);                                // near pointer or 0
  bool unlock(uint16_t ds, uint16_t h);                                  // TRUE while still locked
  uint16_t size(uint16_t ds, uint16_t h);
  uint16_t handle(uint16_t ds, uint16_t ptr);
  uint16_t flags(uint16_t ds, uint16_t h);
  uint16_t compact(uint16_t ds);  // largest free block

  // Diagnostics / tests.
  size_t live_blocks(uint16_t ds) const;

 private:
  struct Moveable {
    uint16_t ptr = 0, size = 0;
    uint8_t flags = 0, locks = 0;
  };
  struct Heap {
    uint16_t start = 0;
    uint32_t end = 0;                          // exclusive; grows toward limit+1
    std::map<uint16_t, uint16_t> used;         // fixed or data block offset → size
    std::map<uint16_t, uint16_t> free_blocks;  // offset → size
    std::map<uint16_t, Moveable> handles;      // handle-table entry → block
    std::map<uint16_t, uint16_t> by_ptr;       // moveable data offset → handle
    std::vector<uint16_t> free_entries;        // unused handle-table entries
  };
  static uint16_t key(uint16_t sel) { return uint16_t(sel | 3); }
  Heap* heap(uint16_t ds);  // lazily creates the heap of a known DGROUP
  uint32_t linear(uint16_t ds, uint16_t off) const;
  uint16_t take(Heap& h, uint16_t ds, uint16_t size);  // raw block, 0 if none
  void give(Heap& h, uint16_t off);
  bool grow(Heap& h, uint16_t ds, uint32_t need);
  uint16_t new_entry(Heap& h, uint16_t ds);
  void write_entry(uint16_t ds, uint16_t e, const Moveable& m);

  cpu::MemoryContext& mem_;
  Ldt& ldt_;
  std::map<uint16_t, Heap> heaps_;
  std::map<uint16_t, uint16_t> dgroups_;  // selector key → static end
};

}  // namespace adw::win16
