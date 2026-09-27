#include "win16/local_heap.hh"

#include <algorithm>

namespace adw::win16 {

namespace {
constexpr uint16_t kEntriesPerTable = 16;
uint32_t align4(uint32_t v) { return (v + 3) & ~3u; }
}  // namespace

void LocalHeaps16::note_dgroup(uint16_t sel, uint16_t static_end) { dgroups_[key(sel)] = static_end; }

void LocalHeaps16::forget(uint16_t sel) {
  dgroups_.erase(key(sel));
  heaps_.erase(key(sel));
}

uint32_t LocalHeaps16::linear(uint16_t ds, uint16_t off) const { return ldt_.base_of(ds) + off; }

bool LocalHeaps16::init(uint16_t seg, uint16_t start, uint16_t end) {
  const SegDesc* d = ldt_.get(seg);
  if (!d || !d->present || d->code) return false;
  uint32_t seg_end = std::min<uint32_t>(d->limit + 1, 0x10000);
  uint32_t s, e;
  auto dg = dgroups_.find(key(seg));
  if (start == 0) {
    // LocalInit(seg, 0, n): an n-byte heap after the static data (the NE
    // loader sized DGROUP as static data + heap + stack).
    s = dg != dgroups_.end() ? dg->second : (seg_end > end ? seg_end - end : 0);
    e = s + end;
  } else {
    s = start;
    e = uint32_t(end) + 1;
  }
  s = align4(std::max<uint32_t>(s, dg != dgroups_.end() ? 16 : 0));
  e = std::min(e, seg_end);
  if (e < s + 16) e = std::min<uint32_t>(s + 16, seg_end);
  if (e <= s) return false;
  Heap h;
  h.start = uint16_t(s);
  h.end = e & ~3u;
  if (h.end > s) h.free_blocks[uint16_t(s)] = uint16_t(h.end - s);
  heaps_[key(seg)] = std::move(h);
  if (dg != dgroups_.end()) mem_.write_u16l(linear(seg, 6), uint16_t(s));  // pLocalHeap
  return true;
}

bool LocalHeaps16::has_heap(uint16_t seg) const { return heaps_.count(key(seg)) != 0; }

LocalHeaps16::Heap* LocalHeaps16::heap(uint16_t ds) {
  auto it = heaps_.find(key(ds));
  if (it != heaps_.end()) return &it->second;
  // A DGROUP whose module never called LocalInit (its NE header asked for no
  // heap) still gets one on demand, after its static data.
  auto dg = dgroups_.find(key(ds));
  if (dg == dgroups_.end()) return nullptr;
  if (!init(ds, dg->second, 0xFFFF)) return nullptr;
  return &heaps_[key(ds)];
}

bool LocalHeaps16::grow(Heap& h, uint16_t ds, uint32_t need) {
  const SegDesc* d = ldt_.get(ds);
  if (!d) return false;
  uint32_t seg_end = std::min<uint32_t>(d->limit + 1, 0x10000) & ~3u;
  if (seg_end <= h.end) return false;
  uint32_t add_from = h.end;
  h.end = seg_end;
  // Merge with a free block that ends at the old end.
  if (!h.free_blocks.empty()) {
    auto last = std::prev(h.free_blocks.end());
    if (uint32_t(last->first) + last->second == add_from) {
      last->second = uint16_t(std::min<uint32_t>(uint32_t(last->second) + (seg_end - add_from), 0xFFFC));
      return uint32_t(last->second) >= need;
    }
  }
  h.free_blocks[uint16_t(add_from)] = uint16_t(std::min<uint32_t>(seg_end - add_from, 0xFFFC));
  return seg_end - add_from >= need;
}

uint16_t LocalHeaps16::take(Heap& h, uint16_t ds, uint16_t size) {
  uint32_t need = align4(std::max<uint32_t>(size, 4));
  for (int pass = 0; pass < 2; pass++) {
    for (auto it = h.free_blocks.begin(); it != h.free_blocks.end(); ++it) {
      if (it->second < need) continue;
      uint16_t off = it->first;
      uint32_t rest = it->second - need;
      h.free_blocks.erase(it);
      if (rest) h.free_blocks[uint16_t(off + need)] = uint16_t(rest);
      h.used[off] = uint16_t(need);
      return off;
    }
    if (pass == 0 && !grow(h, ds, need)) break;
  }
  return 0;
}

void LocalHeaps16::give(Heap& h, uint16_t off) {
  auto u = h.used.find(off);
  if (u == h.used.end()) return;
  uint32_t start = off, size = u->second;
  h.used.erase(u);
  auto next = h.free_blocks.lower_bound(uint16_t(start));
  if (next != h.free_blocks.end() && start + size == next->first) {
    size += next->second;
    next = h.free_blocks.erase(next);
  }
  if (next != h.free_blocks.begin()) {
    auto prev = std::prev(next);
    if (uint32_t(prev->first) + prev->second == start) {
      start = prev->first;
      size += prev->second;
      h.free_blocks.erase(prev);
    }
  }
  h.free_blocks[uint16_t(start)] = uint16_t(size);
}

void LocalHeaps16::write_entry(uint16_t ds, uint16_t e, const Moveable& m) {
  uint32_t a = linear(ds, e);
  mem_.write_u16l(a, m.ptr);
  mem_.write_u8(a + 2, m.flags);
  mem_.write_u8(a + 3, m.locks);
}

uint16_t LocalHeaps16::new_entry(Heap& h, uint16_t ds) {
  if (h.free_entries.empty()) {
    // A handle table: 16 entries at table+2+4i, so every handle is ≡ 2 (mod 4).
    uint16_t t = take(h, ds, uint16_t(4 * kEntriesPerTable + 4));
    if (!t) return 0;
    mem_.memset(linear(ds, t), 0, 4 * kEntriesPerTable + 4);
    for (int i = kEntriesPerTable; i-- > 0;) h.free_entries.push_back(uint16_t(t + 2 + 4 * i));
  }
  uint16_t e = h.free_entries.back();
  h.free_entries.pop_back();
  return e;
}

uint16_t LocalHeaps16::alloc(uint16_t ds, uint16_t flags, uint16_t size) {
  Heap* h = heap(ds);
  if (!h) return 0;
  if (flags & kMoveable) {
    uint16_t e = new_entry(*h, ds);
    if (!e) return 0;
    Moveable m;
    m.flags = uint8_t((flags & kDiscardable) >> 8);
    if (size) {
      uint16_t p = take(*h, ds, size);
      if (!p) {
        h->free_entries.push_back(e);
        return 0;
      }
      if (flags & kZeroInit) mem_.memset(linear(ds, p), 0, h->used[p]);
      m.ptr = p;
      m.size = size;
      h->by_ptr[p] = e;
    }
    h->handles[e] = m;
    write_entry(ds, e, m);
    return e;
  }
  if (!size) return 0;
  uint16_t p = take(*h, ds, size);
  if (p && (flags & kZeroInit)) mem_.memset(linear(ds, p), 0, h->used[p]);
  return p;
}

uint16_t LocalHeaps16::free(uint16_t ds, uint16_t hnd) {
  Heap* h = heap(ds);
  if (!h) return hnd;
  auto m = h->handles.find(hnd);
  if (m != h->handles.end()) {
    if (m->second.ptr) {
      h->by_ptr.erase(m->second.ptr);
      give(*h, m->second.ptr);
    }
    h->handles.erase(m);
    write_entry(ds, hnd, Moveable{});
    h->free_entries.push_back(hnd);
    return 0;
  }
  if (h->used.count(hnd) && !h->by_ptr.count(hnd)) {
    give(*h, hnd);
    return 0;
  }
  return hnd;
}

uint16_t LocalHeaps16::lock(uint16_t ds, uint16_t hnd) {
  Heap* h = heap(ds);
  if (!h) return 0;
  auto m = h->handles.find(hnd);
  if (m != h->handles.end()) {
    if (!m->second.ptr) return 0;
    if (m->second.locks < 0xFF) m->second.locks++;
    write_entry(ds, hnd, m->second);
    return m->second.ptr;
  }
  return h->used.count(hnd) ? hnd : 0;
}

bool LocalHeaps16::unlock(uint16_t ds, uint16_t hnd) {
  Heap* h = heap(ds);
  if (!h) return false;
  auto m = h->handles.find(hnd);
  if (m == h->handles.end()) return false;
  if (m->second.locks) m->second.locks--;
  write_entry(ds, hnd, m->second);
  return m->second.locks != 0;
}

uint16_t LocalHeaps16::size(uint16_t ds, uint16_t hnd) {
  Heap* h = heap(ds);
  if (!h) return 0;
  auto m = h->handles.find(hnd);
  uint16_t p = m != h->handles.end() ? m->second.ptr : hnd;
  auto u = h->used.find(p);
  return (p && u != h->used.end()) ? u->second : 0;
}

uint16_t LocalHeaps16::handle(uint16_t ds, uint16_t ptr) {
  Heap* h = heap(ds);
  if (!h) return 0;
  auto b = h->by_ptr.find(ptr);
  if (b != h->by_ptr.end()) return b->second;
  return h->used.count(ptr) ? ptr : 0;
}

uint16_t LocalHeaps16::flags(uint16_t ds, uint16_t hnd) {
  Heap* h = heap(ds);
  if (!h) return 0;
  auto m = h->handles.find(hnd);
  if (m == h->handles.end()) return 0;
  uint16_t f = m->second.locks;
  f |= uint16_t(m->second.flags) << 8;
  if (!m->second.ptr) f |= kFlagDiscarded;
  return f;
}

uint16_t LocalHeaps16::realloc(uint16_t ds, uint16_t hnd, uint16_t size, uint16_t flags) {
  Heap* h = heap(ds);
  if (!h) return 0;
  auto m = h->handles.find(hnd);
  bool moveable = m != h->handles.end();
  if (flags & kModify) {
    if (moveable) {
      m->second.flags = uint8_t((flags & kDiscardable) >> 8);
      write_entry(ds, hnd, m->second);
    }
    return hnd;
  }
  uint16_t p = moveable ? m->second.ptr : hnd;
  if (!moveable && !h->used.count(p)) return 0;
  if (moveable && size == 0) {
    if (m->second.locks) return 0;
    if (p) {
      h->by_ptr.erase(p);
      give(*h, p);
    }
    m->second.ptr = 0;
    m->second.size = 0;
    write_entry(ds, hnd, m->second);
    return hnd;
  }
  uint32_t need = align4(std::max<uint32_t>(size, 4));
  uint16_t np = 0;
  if (!p) {
    np = take(*h, ds, size);
    if (!np) return 0;
    mem_.memset(linear(ds, np), 0, h->used[np]);
  } else {
    uint32_t have = h->used[p];
    if (need <= have) {
      // Shrink in place; a tail worth keeping goes back to the free list.
      if (have - need >= 8) {
        h->used[p] = uint16_t(need);
        h->used[uint16_t(p + need)] = uint16_t(have - need);
        give(*h, uint16_t(p + need));
      }
      np = p;
    } else {
      auto nx = h->free_blocks.find(uint16_t(p + have));
      if (nx != h->free_blocks.end() && have + nx->second >= need) {
        uint32_t extra = need - have;
        uint32_t rest = nx->second - extra;
        h->free_blocks.erase(nx);
        if (rest) h->free_blocks[uint16_t(p + need)] = uint16_t(rest);
        h->used[p] = uint16_t(need);
        if (flags & kZeroInit) mem_.memset(linear(ds, uint16_t(p + have)), 0, extra);
        np = p;
      } else if (moveable || (flags & kMoveable)) {
        np = take(*h, ds, size);
        if (!np) return 0;
        mem_.memcpy(linear(ds, np), linear(ds, p), have);
        if (flags & kZeroInit) mem_.memset(linear(ds, uint16_t(np + have)), 0, h->used[np] - have);
        if (moveable) h->by_ptr.erase(p);
        give(*h, p);
      } else {
        return 0;
      }
    }
  }
  if (moveable) {
    m->second.ptr = np;
    m->second.size = size;
    h->by_ptr[np] = hnd;
    write_entry(ds, hnd, m->second);
    return hnd;
  }
  return np;
}

uint16_t LocalHeaps16::compact(uint16_t ds) {
  Heap* h = heap(ds);
  if (!h) return 0;
  uint32_t best = 0;
  for (auto& [off, sz] : h->free_blocks) best = std::max<uint32_t>(best, sz);
  const SegDesc* d = ldt_.get(ds);
  if (d) {
    uint32_t seg_end = std::min<uint32_t>(d->limit + 1, 0x10000) & ~3u;
    if (seg_end > h->end) best = std::max<uint32_t>(best, seg_end - h->end);
  }
  return uint16_t(std::min<uint32_t>(best, 0xFFFC));
}

size_t LocalHeaps16::live_blocks(uint16_t ds) const {
  auto it = heaps_.find(key(ds));
  return it == heaps_.end() ? 0 : it->second.used.size();
}

}  // namespace adw::win16
