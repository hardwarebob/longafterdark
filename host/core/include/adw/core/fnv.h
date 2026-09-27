// FNV-1a 64 — the FBHASH frame hash (DESIGN.md §1), with the standard offset
// basis (kFnvOffset below).
#pragma once

#include <cstddef>
#include <cstdint>

namespace adw {

inline constexpr uint64_t kFnvOffset = 14695981039346656037ULL;  // 0xcbf29ce484222325
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

// Continues a running hash `h` over `n` bytes (pass the previous result to
// hash discontiguous pieces, e.g. palette then index rows).
inline uint64_t fnv1a64(const void* data, size_t n, uint64_t h = kFnvOffset) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

}  // namespace adw
