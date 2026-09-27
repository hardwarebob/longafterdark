#include "pe32/controls.hh"

#include <algorithm>
#include <cstring>
#include <vector>

namespace adw::pe32 {

namespace {

uint16_t u16(std::string_view s, size_t off) {
  if (off + 2 > s.size()) return 0;
  return uint16_t(uint8_t(s[off]) | (uint8_t(s[off + 1]) << 8));
}
int16_t i16(std::string_view s, size_t off) { return int16_t(u16(s, off)); }

}  // namespace

int32_t control_default(std::string_view rec) {
  if (rec.size() < 0x1A) return 0;
  uint16_t kind = u16(rec, 0x00);
  int16_t def = i16(rec, 0x18);
  switch (kind) {
    case 1: {  // string slider: count labels of 16 bytes at +0x20, then u16 values
      uint16_t count = std::min<uint16_t>(u16(rec, 0x16), 101);  // AFTERDAR.SCR caps at 101
      if (!count) return 0;
      size_t values_at = 0x20 + size_t(count) * 16;
      if (values_at + size_t(count) * 2 > rec.size()) return 0;
      std::vector<int32_t> stops;
      // AFTERDAR.SCR 0x404f7b: a list that does not start at 0 gets a 0 stop in front.
      if (u16(rec, values_at) != 0) stops.push_back(0);
      for (uint16_t i = 0; i < count; i++) stops.push_back(u16(rec, values_at + 2 * i));
      int32_t v = stops.front();
      for (int32_t s : stops)
        if (s <= def) v = s;
      return v;
    }
    case 2: {  // numeric slider: min/max at +0x30/+0x32
      if (rec.size() < 0x34) return def;
      int16_t lo = i16(rec, 0x30), hi = i16(rec, 0x32);
      if (lo > hi) std::swap(lo, hi);
      return std::clamp<int32_t>(def, lo, hi);
    }
    case 3: {  // popup: an item index
      uint16_t count = u16(rec, 0x16);
      if (!count) return 0;
      return std::clamp<int32_t>(def, 0, count - 1);
    }
    case 5:  // checkbox
      return def ? 1 : 0;
    default:  // none, button
      return 0;
  }
}

}  // namespace adw::pe32
