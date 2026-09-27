// adw_ui (COVERS.md §3.2): decoded pictures and the release covers drawn from
// them — the box-cover tiles of LongAfterDark.scr's strip and adimport's
// windows — plus the generated cover a release shows until it has a picture.
//
// Drawing uses GDI+ (gdiplus_startup() first); decoding uses WIC (COM
// initialized on the calling thread).
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adw/ui/theme.h"

namespace adw::ui {

// A decoded picture: 32-bit premultiplied BGRA, top-down.
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> pbgra;
  bool empty() const { return w <= 0 || h <= 0; }
  mutable std::shared_ptr<void> cache;   // draw_image's scaled copy (keyed by size)
};
// Any format WIC reads (first frame). COM must be initialized on the calling thread.
bool load_image(const std::wstring& path, Image& out, std::string* error = nullptr);
// `img` scaled into `dst` (high-quality bicubic; the scaled copy is cached in img.cache),
// clipped to a rounded rectangle of `radius` px, at `alpha` (0..255).
void draw_image(HDC dc, const RECT& dst, const Image& img, float radius = 0, int alpha = 255);
// A release's cover in `art` (4:5): the tile picture, or the generated cover when `tile` is
// null or empty, with 4-DIP rounded corners and a hairline border (pal.card_stroke; WindowText
// under high contrast).
void draw_cover(HDC dc, const RECT& art, const Image* tile, const std::wstring& title, const Theme& t);
// The generated cover (theme-independent, like box art): a vertical gradient #262B4F → #12152A,
// the crescent moon at (0.72 w, 0.22 h) with radius 0.11 w and a soft glow, five sparkles, and
// `title` in white Segoe UI Variable Display Semibold, left-aligned at 0.1 w, bottom-aligned at
// 0.9 h, wrapped to at most 3 lines, sized 0.14 w and shrunk until it fits.
void draw_generated_cover(HDC dc, const RECT& art, const std::wstring& title, int dpi);

// ---- additions beyond §3.2 (T) -----------------------------------------------------

// An Image from `w` x `h` top-down BGRA rows (`stride` bytes apart); `premultiplied` says
// whether the colour is already multiplied by alpha. Tests and callers that draw their own
// pictures use it.
Image image_from_bgra(int w, int h, const uint8_t* bgra, size_t stride, bool premultiplied = false);
// The generated cover rendered into an opaque Image of `w` x `h` px.
Image generated_cover_image(int w, int h, const std::wstring& title, int dpi);

}  // namespace adw::ui
