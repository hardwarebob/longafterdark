// Bounds-checked little-endian access for the loaders (internal header).
//
// Every read of image bytes goes through Bytes, so a hostile or truncated file
// can only ever produce a LoaderError, never an out-of-bounds read. Offsets are
// size_t and every check is written as `len > size - off` so it cannot wrap.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "loader/image.hh"

namespace adw::loader::detail {

[[noreturn]] inline void fail(LoaderError::Kind kind, const std::string& msg) {
  throw LoaderError(kind, msg);
}

class Bytes {
public:
  Bytes() = default;
  Bytes(std::string_view data, const char* what) : d_(data), what_(what) {}

  size_t size() const { return d_.size(); }
  std::string_view data() const { return d_; }
  bool contains(size_t off, size_t len) const {
    return off <= d_.size() && len <= d_.size() - off;
  }
  void need(size_t off, size_t len) const {
    if (!contains(off, len)) {
      fail(LoaderError::Kind::truncated,
          std::format("{}: {} bytes at 0x{:X} run past its end (0x{:X} bytes)",
              what_, len, off, d_.size()));
    }
  }
  uint8_t u8(size_t off) const {
    need(off, 1);
    return static_cast<uint8_t>(d_[off]);
  }
  uint16_t u16(size_t off) const {
    need(off, 2);
    return static_cast<uint16_t>(b(off) | (b(off + 1) << 8));
  }
  uint32_t u32(size_t off) const {
    need(off, 4);
    return static_cast<uint32_t>(b(off)) | (static_cast<uint32_t>(b(off + 1)) << 8) |
        (static_cast<uint32_t>(b(off + 2)) << 16) | (static_cast<uint32_t>(b(off + 3)) << 24);
  }
  std::string_view slice(size_t off, size_t len) const {
    need(off, len);
    return d_.substr(off, len);
  }
  // NUL-terminated string that must end inside the data (and within max bytes).
  std::string cstr(size_t off, size_t max = 0x1000) const {
    need(off, 0);
    size_t lim = std::min(d_.size() - off, max);
    size_t n = 0;
    while (n < lim && d_[off + n] != '\0') n++;
    if (n == lim) {
      fail(LoaderError::Kind::truncated,
          std::format("{}: string at 0x{:X} is not terminated", what_, off));
    }
    return std::string(d_.substr(off, n));
  }
  // Length-prefixed (Pascal) string, as NE name tables store them.
  std::string pstr(size_t off) const {
    uint8_t n = u8(off);
    return std::string(slice(off + 1, n));
  }
  const char* what() const { return what_; }

private:
  uint32_t b(size_t off) const { return static_cast<uint8_t>(d_[off]); }
  std::string_view d_;
  const char* what_ = "image";
};

// Caps the bytes of names one table may make the parser copy. Bounding entry
// counts is not enough: many entries can point at the same long string, and
// each copy is paid again (a 160 KB PE whose resource entries all name one
// 64K-character string asked for 250 MB, and the entry cap allowed ~16 GB,
// which ended in bad_alloc instead of a LoaderError). Real images use a few KB.
class NameBudget {
public:
  explicit NameBudget(const char* what, size_t limit = size_t(16) << 20)
      : what_(what), limit_(limit), left_(limit) {}
  void spend(size_t n) {
    if (n > left_) {
      fail(LoaderError::Kind::malformed,
          std::format("{}: names exceed {} MB", what_, limit_ >> 20));
    }
    left_ -= n;
  }

private:
  const char* what_;
  size_t limit_, left_;
};

// Writers into a loader-owned buffer (the image being fixed up).
inline void check_put(const std::string& buf, size_t off, size_t len, const char* what) {
  if (off > buf.size() || len > buf.size() - off) {
    fail(LoaderError::Kind::malformed,
        std::format("{}: fixup of {} bytes at 0x{:X} lies outside it (0x{:X} bytes)",
            what, len, off, buf.size()));
  }
}
inline void put8(std::string& buf, size_t off, uint8_t v, const char* what) {
  check_put(buf, off, 1, what);
  buf[off] = static_cast<char>(v);
}
inline void put16(std::string& buf, size_t off, uint16_t v, const char* what) {
  check_put(buf, off, 2, what);
  buf[off] = static_cast<char>(v);
  buf[off + 1] = static_cast<char>(v >> 8);
}
inline void put32(std::string& buf, size_t off, uint32_t v, const char* what) {
  check_put(buf, off, 4, what);
  for (int i = 0; i < 4; i++) buf[off + i] = static_cast<char>(v >> (8 * i));
}

inline bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    unsigned char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return false;
  }
  return true;
}

// 64-bit so aligning a hostile 32-bit size can never wrap to a small one.
constexpr uint64_t align_up(uint64_t v, uint64_t a) {
  return a ? ((v + a - 1) / a) * a : v;
}

} // namespace adw::loader::detail
