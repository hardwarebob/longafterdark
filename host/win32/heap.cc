#include "win32/heap.hh"

#include <algorithm>
#include <stdexcept>

namespace adw::win32 {

// ---- GuestHeap ---------------------------------------------------------------------------------

GuestHeap::GuestHeap(cpu::MemoryContext& mem, uint32_t base, uint32_t size)
    : mem_(mem), base_(base), size_(size & ~15u) {
  // One arena for the whole heap. preallocate_arena (not allocate_at): the
  // MemoryContext's own block bookkeeping is not used here, and an arena with
  // no allocated block is never deleted behind our back.
  mem_.preallocate_arena(base_, size_);
  add_free(base_, size_);
}

void GuestHeap::add_free(uint32_t addr, uint32_t size) {
  free_bytes_ += size;
  // Merge with the neighbours.
  auto next = free_by_addr_.lower_bound(addr);
  if (next != free_by_addr_.end() && addr + size == next->first) {
    size += next->second;
    free_by_size_.erase({next->second, next->first});
    next = free_by_addr_.erase(next);
  }
  if (next != free_by_addr_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == addr) {
      addr = prev->first;
      size += prev->second;
      free_by_size_.erase({prev->second, prev->first});
      free_by_addr_.erase(prev);
    }
  }
  free_by_addr_[addr] = size;
  free_by_size_.insert({size, addr});
}

void GuestHeap::take_free(uint32_t addr, uint32_t size) {
  free_by_addr_.erase(addr);
  free_by_size_.erase({size, addr});
  free_bytes_ -= size;
}

uint32_t GuestHeap::alloc(uint32_t size, bool zero, uint32_t align) {
  if (size > size_) return 0;
  uint32_t need = round(size);
  if (align < 16) align = 16;
  // Best fit: the smallest free block that holds an aligned block of `need`.
  for (auto it = free_by_size_.lower_bound({need, 0}); it != free_by_size_.end(); ++it) {
    uint32_t fsize = it->first, faddr = it->second;
    uint32_t a = (faddr + align - 1) & ~(align - 1);
    if (a < faddr || a - faddr + uint64_t(need) > fsize) continue;
    take_free(faddr, fsize);
    if (a > faddr) add_free(faddr, a - faddr);
    uint32_t tail = faddr + fsize - (a + need);
    if (tail) add_free(a + need, tail);
    used_[a] = need;
    if (zero) mem_.memset(a, 0, need);
    return a;
  }
  return 0;
}

bool GuestHeap::free(uint32_t addr) {
  auto it = used_.find(addr);
  if (it == used_.end()) return false;
  uint32_t size = it->second;
  used_.erase(it);
  add_free(addr, size);
  return true;
}

uint32_t GuestHeap::block_size(uint32_t addr) const {
  auto it = used_.find(addr);
  return it == used_.end() ? 0 : it->second;
}

uint32_t GuestHeap::realloc(uint32_t addr, uint32_t size, bool allow_move, bool zero_growth) {
  auto it = used_.find(addr);
  if (it == used_.end()) return 0;
  uint32_t old = it->second, need = round(size);
  if (need <= old) {
    if (need < old) {
      it->second = need;
      add_free(addr + need, old - need);
    }
    return addr;
  }
  // Grow in place into a directly following free block.
  auto nf = free_by_addr_.find(addr + old);
  if (nf != free_by_addr_.end() && old + uint64_t(nf->second) >= need) {
    uint32_t faddr = nf->first, fsize = nf->second;
    take_free(faddr, fsize);
    uint32_t used_extra = need - old;
    if (fsize > used_extra) add_free(faddr + used_extra, fsize - used_extra);
    it->second = need;
    if (zero_growth) mem_.memset(addr + old, 0, used_extra);
    return addr;
  }
  if (!allow_move) return 0;
  uint32_t n = alloc(size, false);
  if (!n) return 0;
  mem_.memcpy(n, addr, old);
  if (zero_growth) mem_.memset(n + old, 0, need - old);
  free(addr);
  return n;
}

uint32_t GuestHeap::largest_free() const {
  return free_by_size_.empty() ? 0 : free_by_size_.rbegin()->first;
}

// ---- HandleHeap --------------------------------------------------------------------------------

uint32_t HandleHeap::new_slot() {
  if (free_slots_.empty()) {
    // A page of 8-byte slots; the handle is slot+4 and *(handle) is the pointer,
    // so the slot's first dword is padding and the pointer lives at +4.
    uint32_t page = heap_.alloc(0x1000, true);
    if (!page) return 0;
    for (uint32_t off = 0x1000; off >= 8; off -= 8) free_slots_.push_back(page + off - 8 + 4);
  }
  uint32_t h = free_slots_.back();
  free_slots_.pop_back();
  return h;
}

uint32_t HandleHeap::alloc(uint32_t flags, uint32_t size) {
  bool zero = flags & kZeroInit;
  if (!(flags & kMoveable)) {
    uint32_t p = heap_.alloc(size, zero);
    if (p) fixed_[p] = size;
    return p;
  }
  uint32_t h = new_slot();
  if (!h) return 0;
  Moveable m;
  m.size = size;
  m.flags = flags;
  if (size) {
    m.ptr = heap_.alloc(size, zero);
    if (!m.ptr) {
      free_slots_.push_back(h);
      return 0;
    }
    by_ptr_[m.ptr] = h;
  }
  mem_.write_u32l(h, m.ptr);
  entries_[h] = m;
  return h;
}

uint32_t HandleHeap::free(uint32_t h) {
  auto it = entries_.find(h);
  if (it != entries_.end()) {
    if (it->second.ptr) {
      by_ptr_.erase(it->second.ptr);
      heap_.free(it->second.ptr);
    }
    entries_.erase(it);
    mem_.write_u32l(h, 0);
    free_slots_.push_back(h);
    return 0;
  }
  auto f = fixed_.find(h);
  if (f != fixed_.end()) {
    fixed_.erase(f);
    heap_.free(h);
    return 0;
  }
  return h;
}

uint32_t HandleHeap::lock(uint32_t h) {
  auto it = entries_.find(h);
  if (it != entries_.end()) {
    if (it->second.ptr && it->second.locks < 0xFF) it->second.locks++;
    return it->second.ptr;
  }
  return fixed_.count(h) ? h : 0;
}

bool HandleHeap::unlock(uint32_t h) {
  auto it = entries_.find(h);
  if (it == entries_.end()) return false;
  if (it->second.locks) it->second.locks--;
  return it->second.locks != 0;
}

uint32_t HandleHeap::realloc(uint32_t h, uint32_t size, uint32_t flags) {
  bool zero = flags & kZeroInit;
  auto it = entries_.find(h);
  if (it != entries_.end()) {
    Moveable& m = it->second;
    if (flags & kModify) {
      m.flags = (m.flags & ~kDiscardable) | (flags & kDiscardable);
      return h;
    }
    if (!m.ptr) {
      if (size) {
        m.ptr = heap_.alloc(size, zero);
        if (!m.ptr) return 0;
        by_ptr_[m.ptr] = h;
      }
    } else if (!size) {
      // Discard: the handle stays valid with no memory behind it.
      by_ptr_.erase(m.ptr);
      heap_.free(m.ptr);
      m.ptr = 0;
    } else {
      uint32_t old_size = m.size;
      uint32_t n = heap_.realloc(m.ptr, size, /*allow_move=*/true);
      if (!n) return 0;
      if (zero && size > old_size) mem_.memset(n + old_size, 0, size - old_size);
      if (n != m.ptr) {
        by_ptr_.erase(m.ptr);
        by_ptr_[n] = h;
        m.ptr = n;
      }
    }
    m.size = size;
    mem_.write_u32l(h, m.ptr);
    return h;
  }
  auto f = fixed_.find(h);
  if (f == fixed_.end()) return 0;
  if (flags & kModify) return h;
  uint32_t old_size = f->second;
  // A fixed block may move only when GMEM_MOVEABLE is passed.
  uint32_t n = heap_.realloc(h, size ? size : 1, (flags & kMoveable) != 0);
  if (!n) return 0;
  if (zero && size > old_size) mem_.memset(n + old_size, 0, size - old_size);
  fixed_.erase(f);
  fixed_[n] = size;
  return n;
}

uint32_t HandleHeap::size(uint32_t h) const {
  auto it = entries_.find(h);
  if (it != entries_.end()) return it->second.size;
  auto f = fixed_.find(h);
  return f == fixed_.end() ? 0 : f->second;
}

uint32_t HandleHeap::handle_of(uint32_t ptr) const {
  auto it = by_ptr_.find(ptr);
  if (it != by_ptr_.end()) return it->second;
  return fixed_.count(ptr) ? ptr : 0;
}

uint32_t HandleHeap::flags(uint32_t h) const {
  auto it = entries_.find(h);
  if (it != entries_.end()) {
    uint32_t f = it->second.locks & 0xFF;
    if (it->second.flags & kDiscardable) f |= 0x0100;  // GMEM_DISCARDABLE
    if (!it->second.ptr) f |= 0x4000;                  // GMEM_DISCARDED
    return f;
  }
  return fixed_.count(h) ? 0 : 0x8000;                 // GMEM_INVALID_HANDLE
}

// ---- HeapSet -----------------------------------------------------------------------------------

uint32_t HeapSet::process_heap() {
  if (!process_heap_) process_heap_ = create();
  return process_heap_;
}

uint32_t HeapSet::create() {
  uint32_t h = heap_.alloc(64, true);
  if (h) heaps_[h];
  return h;
}

bool HeapSet::destroy(uint32_t h) {
  auto it = heaps_.find(h);
  if (it == heaps_.end() || h == process_heap_) return false;
  // Free in address order so the allocator state is independent of hash order.
  std::vector<uint32_t> blocks;
  for (auto& [p, s] : it->second.blocks) blocks.push_back(p);
  std::sort(blocks.begin(), blocks.end());
  for (uint32_t p : blocks) heap_.free(p);
  heaps_.erase(it);
  heap_.free(h);
  return true;
}

uint32_t HeapSet::alloc(uint32_t h, uint32_t flags, uint32_t size) {
  auto it = heaps_.find(h);
  if (it == heaps_.end()) return 0;
  uint32_t p = heap_.alloc(size, (flags & kZeroMemory) != 0);
  if (p) it->second.blocks[p] = size;
  return p;
}

bool HeapSet::free(uint32_t h, uint32_t ptr) {
  auto it = heaps_.find(h);
  if (it == heaps_.end()) return false;
  if (!ptr) return true;
  auto b = it->second.blocks.find(ptr);
  if (b == it->second.blocks.end()) return false;
  it->second.blocks.erase(b);
  heap_.free(ptr);
  return true;
}

uint32_t HeapSet::realloc(uint32_t h, uint32_t flags, uint32_t ptr, uint32_t size) {
  auto it = heaps_.find(h);
  if (it == heaps_.end()) return 0;
  auto b = it->second.blocks.find(ptr);
  if (b == it->second.blocks.end()) return 0;
  uint32_t old_size = b->second;
  uint32_t n = heap_.realloc(ptr, size, !(flags & kReallocInPlaceOnly));
  if (!n) return 0;
  if ((flags & kZeroMemory) && size > old_size) mem_.memset(n + old_size, 0, size - old_size);
  it->second.blocks.erase(ptr);
  it->second.blocks[n] = size;
  return n;
}

uint32_t HeapSet::size(uint32_t h, uint32_t ptr) const {
  auto it = heaps_.find(h);
  if (it == heaps_.end()) return 0xFFFFFFFF;
  auto b = it->second.blocks.find(ptr);
  return b == it->second.blocks.end() ? 0xFFFFFFFF : b->second;
}

}  // namespace adw::win32
