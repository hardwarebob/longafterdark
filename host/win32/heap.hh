// Guest memory services: one allocator over the heap arena, and the Win32
// memory-handle models built on it.
//
// GuestHeap is the only allocator: GlobalAlloc/LocalAlloc/HeapAlloc/
// VirtualAlloc, the lane's AD_MODULE32 block and every DIB section's bits all
// come from the same pre-allocated arena (layout.hh), so a DIB can be created
// over any of it with MemoryContext::section_for(). Best fit, 16-byte aligned,
// coalescing, deterministic (same request sequence → same addresses), never
// moves a block on its own. Fresh arena memory is zero; reused memory keeps its
// old bytes unless the caller asks for zeroing (as Windows does).
//
// Handles (HandleHeap): GMEM_FIXED blocks are their own handle; GMEM_MOVEABLE
// ones get a handle that points at a guest dword holding the block address
// (as on NT, *(void**)hMem == GlobalLock(hMem)), always ≡ 4 (mod 8) so it can
// never be mistaken for a 16-aligned fixed block. Lock counts and flags are
// host-side. Heaps (HeapCreate) are block sets over the same allocator.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "MemoryContext.hh"

namespace adw::win32 {

class GuestHeap {
 public:
  // Creates the arena [base, base+size).
  GuestHeap(cpu::MemoryContext& mem, uint32_t base, uint32_t size);

  uint32_t base() const { return base_; }
  uint32_t size() const { return size_; }
  uint32_t end() const { return base_ + size_; }
  bool contains(uint32_t addr) const { return addr >= base_ && addr - base_ < size_; }

  // 0 = out of memory. align must be a power of two >= 16. size 0 allocates 16 bytes.
  uint32_t alloc(uint32_t size, bool zero = false, uint32_t align = 16);
  // Unknown addresses are ignored (returns false).
  bool free(uint32_t addr);
  // Size of the block starting at addr (rounded to 16), or 0 if addr starts none.
  uint32_t block_size(uint32_t addr) const;
  // In place when it fits; otherwise, if allow_move, a new block (contents copied,
  // old one freed). Growth is zeroed when zero_growth. 0 = failed (block untouched).
  uint32_t realloc(uint32_t addr, uint32_t size, bool allow_move, bool zero_growth = false);

  uint32_t bytes_free() const { return free_bytes_; }
  uint32_t largest_free() const;
  size_t block_count() const { return used_.size(); }

 private:
  void add_free(uint32_t addr, uint32_t size);   // coalesces
  void take_free(uint32_t addr, uint32_t size);  // removes exactly this free block
  static uint32_t round(uint32_t n) { return n ? (n + 15) & ~15u : 16; }

  cpu::MemoryContext& mem_;
  uint32_t base_, size_;
  uint32_t free_bytes_ = 0;
  std::map<uint32_t, uint32_t> free_by_addr_;
  std::set<std::pair<uint32_t, uint32_t>> free_by_size_;  // (size, addr)
  std::map<uint32_t, uint32_t> used_;                     // addr -> size
};

// GlobalAlloc/LocalAlloc semantics (Win32: the two families are the same).
class HandleHeap {
 public:
  static constexpr uint32_t kMoveable = 0x0002;   // GMEM_MOVEABLE / LMEM_MOVEABLE
  static constexpr uint32_t kZeroInit = 0x0040;   // GMEM_ZEROINIT / LMEM_ZEROINIT
  static constexpr uint32_t kModify = 0x0080;     // GMEM_MODIFY
  static constexpr uint32_t kDiscardable = 0x0100;

  HandleHeap(cpu::MemoryContext& mem, GuestHeap& heap) : mem_(mem), heap_(heap) {}

  uint32_t alloc(uint32_t flags, uint32_t size);            // handle or 0
  uint32_t free(uint32_t h);                                // 0 on success, else h
  uint32_t lock(uint32_t h);                                // pointer or 0
  bool unlock(uint32_t h);                                  // TRUE while still locked
  uint32_t realloc(uint32_t h, uint32_t size, uint32_t flags);  // new handle or 0
  uint32_t size(uint32_t h) const;                          // requested size, 0 if invalid
  uint32_t handle_of(uint32_t ptr) const;                   // GlobalHandle: 0 if unknown
  uint32_t flags(uint32_t h) const;                         // GlobalFlags (lock count in low byte)
  bool is_moveable_handle(uint32_t h) const { return entries_.count(h) != 0; }

 private:
  struct Moveable {
    uint32_t ptr = 0, size = 0, flags = 0, locks = 0;
  };
  uint32_t new_slot();

  cpu::MemoryContext& mem_;
  GuestHeap& heap_;
  std::unordered_map<uint32_t, Moveable> entries_;   // handle -> entry
  std::unordered_map<uint32_t, uint32_t> by_ptr_;    // moveable block ptr -> handle
  std::unordered_map<uint32_t, uint32_t> fixed_;     // fixed block ptr -> requested size
  std::vector<uint32_t> free_slots_;
};

// HeapCreate/HeapAlloc/… over GuestHeap. Heap handles are guest addresses of a
// small header block (heap handles are pointers on Windows).
class HeapSet {
 public:
  static constexpr uint32_t kZeroMemory = 0x00000008;       // HEAP_ZERO_MEMORY
  static constexpr uint32_t kReallocInPlaceOnly = 0x00000010;

  HeapSet(cpu::MemoryContext& mem, GuestHeap& heap) : mem_(mem), heap_(heap) {}
  uint32_t process_heap();
  uint32_t create();
  bool destroy(uint32_t h);
  uint32_t alloc(uint32_t h, uint32_t flags, uint32_t size);
  bool free(uint32_t h, uint32_t ptr);
  uint32_t realloc(uint32_t h, uint32_t flags, uint32_t ptr, uint32_t size);
  uint32_t size(uint32_t h, uint32_t ptr) const;  // 0xFFFFFFFF if unknown
  bool valid(uint32_t h) const { return heaps_.count(h) != 0; }

 private:
  struct Heap {
    std::unordered_map<uint32_t, uint32_t> blocks;  // ptr -> requested size
  };
  cpu::MemoryContext& mem_;
  GuestHeap& heap_;
  uint32_t process_heap_ = 0;
  std::map<uint32_t, Heap> heaps_;
};

}  // namespace adw::win32
