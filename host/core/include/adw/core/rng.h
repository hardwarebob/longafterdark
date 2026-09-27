// The host's own randomness (ADSEED). Every source the host or a shim owns
// (rand() seeds, the test pattern's stars, …) derives a stream from the one
// seed, so a run is reproduced by its seed alone.
#pragma once

#include <cstdint>

namespace adw {

// splitmix64: tiny, fast, and fully specified, so a seed means the same
// sequence on every build.
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed) {}
  // An independent stream for a named purpose, from the run seed.
  static Rng derive(uint64_t seed, uint64_t stream) {
    Rng r(seed ^ (0x9E3779B97F4A7C15ULL * (stream + 1)));
    r.next();
    return r;
  }
  uint64_t next() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  // Uniform in [0, n) (n > 0); the modulo bias is irrelevant at our ranges.
  uint32_t below(uint32_t n) { return uint32_t(next() % n); }

 private:
  uint64_t s_;
};

}  // namespace adw
