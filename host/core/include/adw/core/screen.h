// The emulated display: an 8-bit palettized framebuffer plus the 256-entry
// hardware palette, exactly what an After Dark module saw on a Win95 box in
// 256-colour mode (RC_PALETTE, 20 static colours). A frame on the wire is the
// P8 encoding of this surface.
//
// Storage is owned by default. A lane whose module draws through GDI onto a DIB
// section that aliases emulated memory attach()es the Screen to that memory
// instead, so presenting a frame reads the module's own pixels with no copy
// into an intermediate buffer (a bottom-up DIB is a negative stride).
#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adw {

// The Win95 default (static) system palette: entries 0..9 then 246..255, the
// colours GetStockObject(DEFAULT_PALETTE) reserves on a palette device.
extern const std::array<RGBQUAD, 20> kStaticColors;

// "P8\n<w> <h>\n"
std::string p8_header(int w, int h);

class Screen {
 public:
  static constexpr int kMaxDim = 16384;

  Screen(int width, int height);
  // base_ may point into owned_, so a member-wise copy would leave the copy
  // drawing into (and presenting) the original's pixels. Moves re-seat it; a
  // moved-from Screen is 0x0.
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;
  Screen(Screen&& o) noexcept;
  Screen& operator=(Screen&& o) noexcept;

  int width() const { return w_; }
  int height() const { return h_; }
  // Byte distance between row y and row y+1 (negative for bottom-up memory).
  ptrdiff_t stride() const { return stride_; }
  uint8_t* row(int y) { return base_ + ptrdiff_t(y) * stride_; }
  const uint8_t* row(int y) const { return base_ + ptrdiff_t(y) * stride_; }
  uint8_t& at(int x, int y) { return row(y)[x]; }

  // Redirect the surface to external memory. `top_row` is the first byte of the
  // TOP scanline; stride may be negative. The memory must outlive the attach.
  void attach(uint8_t* top_row, ptrdiff_t stride);
  void detach();  // back to the owned buffer (its contents are what they were)
  bool attached() const { return base_ != owned_.data(); }

  // Hardware palette (rgbReserved is ignored on the wire). Writing through the
  // reference does not set the dirty flag; set_entry/set_entries do.
  std::array<RGBQUAD, 256>& palette() { return pal_; }
  const std::array<RGBQUAD, 256>& palette() const { return pal_; }
  void set_entry(int i, uint8_t r, uint8_t g, uint8_t b);
  void set_entries(int first, int count, const RGBQUAD* entries);
  // Static colours at 0..9 / 246..255, black in between.
  void reset_system_palette();
  // The palette as the wire carries it: 256 x (R,G,B).
  void palette_rgb(uint8_t out[768]) const;

  void clear(uint8_t index);

  // Lanes set this whenever pixels or palette change; the host re-sends its
  // cached encoding of the previous frame while it is clear.
  void mark_dirty() { dirty_ = true; }
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

  // Whole P8 frame (header + 768 palette bytes + w*h top-down indices), written
  // into `out` (replacing its contents; capacity is reused frame to frame).
  size_t p8_size() const;
  void encode_p8(std::vector<uint8_t>& out) const;
  // P6 fallback frame: "P6\n<w> <h>\n255\n" + w*h*3 palette-resolved RGB.
  void encode_p6(std::vector<uint8_t>& out) const;
  // FNV-1a 64 over the P8 body (palette RGB, then indices) — FBHASH.
  uint64_t fbhash() const;
  // Binary PPM (the P6 frame) at a UTF-8 path. False on I/O failure.
  bool write_ppm(const std::string& path_utf8) const;

 private:
  int w_, h_;
  std::vector<uint8_t> owned_;
  uint8_t* base_;
  ptrdiff_t stride_;
  std::array<RGBQUAD, 256> pal_{};
  bool dirty_ = true;
};

}  // namespace adw
