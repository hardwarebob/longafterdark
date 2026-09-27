#include "adw/core/screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "adw/core/fnv.h"
#include "adw/core/text.h"

namespace adw {

// RGBQUAD is {blue, green, red, reserved}.
#define Q(r, g, b) RGBQUAD{b, g, r, 0}
const std::array<RGBQUAD, 20> kStaticColors = {
    // 0..9
    Q(0, 0, 0), Q(128, 0, 0), Q(0, 128, 0), Q(128, 128, 0), Q(0, 0, 128),
    Q(128, 0, 128), Q(0, 128, 128), Q(192, 192, 192), Q(192, 220, 192), Q(166, 202, 240),
    // 246..255
    Q(255, 251, 240), Q(160, 160, 164), Q(128, 128, 128), Q(255, 0, 0), Q(0, 255, 0),
    Q(255, 255, 0), Q(0, 0, 255), Q(255, 0, 255), Q(0, 255, 255), Q(255, 255, 255),
};
#undef Q

std::string p8_header(int w, int h) {
  char buf[48];
  int n = snprintf(buf, sizeof(buf), "P8\n%d %d\n", w, h);
  return std::string(buf, size_t(n));
}

static std::string p6_header(int w, int h) {
  char buf[48];
  int n = snprintf(buf, sizeof(buf), "P6\n%d %d\n255\n", w, h);
  return std::string(buf, size_t(n));
}

Screen::Screen(int width, int height)
    : w_(std::clamp(width, 1, kMaxDim)),
      h_(std::clamp(height, 1, kMaxDim)),
      owned_(size_t(w_) * size_t(h_), 0),
      base_(owned_.data()),
      stride_(w_) {
  reset_system_palette();
}

Screen::Screen(Screen&& o) noexcept : w_(0), h_(0), base_(nullptr), stride_(0) { *this = std::move(o); }

Screen& Screen::operator=(Screen&& o) noexcept {
  if (this == &o) return *this;
  bool was_attached = o.attached();
  w_ = o.w_;
  h_ = o.h_;
  stride_ = o.stride_;
  pal_ = o.pal_;
  dirty_ = o.dirty_;
  owned_ = std::move(o.owned_);  // steals the heap buffer: its address is kept
  base_ = was_attached ? o.base_ : owned_.data();
  // The source is left an empty 0x0 surface (no rows to touch), detached.
  o.owned_.clear();
  o.w_ = o.h_ = 0;
  o.base_ = o.owned_.data();
  o.stride_ = 0;
  return *this;
}

void Screen::attach(uint8_t* top_row, ptrdiff_t stride) {
  base_ = top_row;
  stride_ = stride;
  dirty_ = true;
}

void Screen::detach() {
  base_ = owned_.data();
  stride_ = w_;
  dirty_ = true;
}

void Screen::set_entry(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i > 255) return;
  pal_[size_t(i)] = RGBQUAD{b, g, r, 0};
  dirty_ = true;
}

void Screen::set_entries(int first, int count, const RGBQUAD* entries) {
  for (int k = 0; k < count; k++) {
    int i = first + k;
    if (i < 0 || i > 255) continue;
    pal_[size_t(i)] = entries[k];
  }
  dirty_ = true;
}

void Screen::reset_system_palette() {
  pal_.fill(RGBQUAD{0, 0, 0, 0});
  for (int i = 0; i < 10; i++) pal_[size_t(i)] = kStaticColors[size_t(i)];
  for (int i = 0; i < 10; i++) pal_[size_t(246 + i)] = kStaticColors[size_t(10 + i)];
  dirty_ = true;
}

void Screen::palette_rgb(uint8_t out[768]) const {
  for (size_t i = 0; i < 256; i++) {
    out[i * 3 + 0] = pal_[i].rgbRed;
    out[i * 3 + 1] = pal_[i].rgbGreen;
    out[i * 3 + 2] = pal_[i].rgbBlue;
  }
}

void Screen::clear(uint8_t index) {
  for (int y = 0; y < h_; y++) memset(row(y), index, size_t(w_));
  dirty_ = true;
}

size_t Screen::p8_size() const {
  return p8_header(w_, h_).size() + 768 + size_t(w_) * size_t(h_);
}

void Screen::encode_p8(std::vector<uint8_t>& out) const {
  std::string hdr = p8_header(w_, h_);
  out.resize(hdr.size() + 768 + size_t(w_) * size_t(h_));
  uint8_t* p = out.data();
  memcpy(p, hdr.data(), hdr.size());
  p += hdr.size();
  palette_rgb(p);
  p += 768;
  for (int y = 0; y < h_; y++, p += w_) memcpy(p, row(y), size_t(w_));
}

void Screen::encode_p6(std::vector<uint8_t>& out) const {
  std::string hdr = p6_header(w_, h_);
  out.resize(hdr.size() + size_t(w_) * size_t(h_) * 3);
  uint8_t* p = out.data();
  memcpy(p, hdr.data(), hdr.size());
  p += hdr.size();
  uint8_t rgb[768];
  palette_rgb(rgb);
  for (int y = 0; y < h_; y++) {
    const uint8_t* src = row(y);
    for (int x = 0; x < w_; x++, p += 3) memcpy(p, rgb + size_t(src[x]) * 3, 3);
  }
}

uint64_t Screen::fbhash() const {
  uint8_t rgb[768];
  palette_rgb(rgb);
  uint64_t h = fnv1a64(rgb, sizeof(rgb));
  for (int y = 0; y < h_; y++) h = fnv1a64(row(y), size_t(w_), h);
  return h;
}

bool Screen::write_ppm(const std::string& path_utf8) const {
  std::vector<uint8_t> buf;
  encode_p6(buf);
  FILE* f = _wfopen(widen(path_utf8).c_str(), L"wb");
  if (!f) return false;
  bool ok = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
  ok = (fclose(f) == 0) && ok;
  return ok;
}

}  // namespace adw
