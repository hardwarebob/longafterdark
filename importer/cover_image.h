// The box covers' pictures (COVERS.md §2.6): decoding any picture Windows can
// read (WIC), the normalization every stored picture gets, and the tile rules.
//
//   original.png / user.png  the picture decoded (first frame, EXIF orientation applied),
//                            cropped, scaled down to at most 2048 px on its long side, RGBA
//   tile.png                 640x800, opaque: a disc label as a round disc on the night
//                            gradient; anything else filled (within ±15% of 4:5) or contained
//                            on bands of its own edge colour
//
// Everything that decides what a tile looks like is a pure function over pixel
// buffers (plan_tile, band_colour, resize_*, render_tile), so the tests pin the
// rules without WIC; only decoding and PNG encoding go through it. The tile's
// resampler is the importer's own high-quality cubic (Catmull-Rom, widened when
// scaling down, over premultiplied alpha), so a tile comes out the same on every
// Windows. kRendererVersion names these rules: raising it makes the next
// catalog write re-render every stored tile.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packages.h"  // Crop

namespace adw::import::cover {

// A picture: 32-bit BGRA with straight (not premultiplied) alpha, top-down,
// rows of w * 4 bytes.
struct Picture {
  int w = 0, h = 0;
  std::vector<uint8_t> bgra;

  bool empty() const { return w <= 0 || h <= 0; }
  const uint8_t* px(int x, int y) const { return &bgra[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
  uint8_t* px(int x, int y) { return &bgra[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
  static Picture filled(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255);
};

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb&) const = default;
};

inline constexpr int kTileW = 640, kTileH = 800;
// The version of the rules below (cover.json tile.renderer).
inline constexpr int kRendererVersion = 1;
inline constexpr int kMaxStoredSide = 2048;             // original.png / user.png, long side
inline constexpr uint64_t kMaxPictureBytes = 64ull << 20;  // a picture file
inline constexpr int kMinSide = 32, kMaxSide = 16384;   // decoded, per side
// A disc label: a circle of this diameter centred here, on the night gradient
// of the .scr's module tiles.
inline constexpr int kDiscDiameter = 552, kDiscCentreX = 320, kDiscCentreY = 368;
inline constexpr Rgb kNightTop{0x26, 0x2B, 0x4F}, kNightBottom{0x12, 0x15, 0x2A};

// What people are told when a picture cannot be used.
inline constexpr char kCannotRead[] = "Windows can't read this picture. Save it as PNG or JPEG and try again.";

// ---- pure: geometry and pixels --------------------------------------------------------

// EXIF orientation 1..8 applied (6 = rotate 90 degrees clockwise); others as is.
Picture orient(const Picture& p, int exif_orientation);
bool crop_fits(const Picture& p, const Crop& c);
// ImportError(source_invalid) when the crop does not lie inside the picture.
Picture crop(const Picture& p, const Crop& c);
// Distinct BGRA values, counting stops at `limit`.
int count_colours(const Picture& p, int limit = 257);
Picture resize_nearest(const Picture& p, int w, int h);
// High-quality cubic (Catmull-Rom; the kernel widened by the reduction when
// scaling down), over premultiplied alpha.
Picture resize_cubic(const Picture& p, int w, int h);
// The tile's resampling: a picture of at most 256 colours scaled by 2 or more
// is first scaled up by the largest whole factor with nearest-neighbour, then
// by what remains with cubic, so small installer bitmaps stay crisp.
bool uses_nearest_first(int src_w, int src_h, int w, int h, bool few_colours);
Picture scale_for_tile(const Picture& p, int w, int h, bool few_colours);
// Scaled down (cubic) so the long side is at most `max_side`; never up.
Picture cap_long_side(const Picture& p, int max_side);

enum class TileMode { disc, fill, contain };
struct TileLayout {
  TileMode mode = TileMode::contain;
  Crop source;                  // the part of the picture drawn (disc: the centred square)
  int x = 0, y = 0, w = 0, h = 0;  // where it lands in the tile (fill: may overhang it)
  bool bands_top_bottom = true;    // contain: bands above and below, else left and right
};
// Within ±15% of 4:5 (0.68 <= w/h <= 0.92): the picture fills the tile.
bool fills_tile(int w, int h);
TileLayout plan_tile(int w, int h, std::string_view art);
// The mean colour (alpha-weighted) of the picture's two outermost rows
// (`top_bottom`) or columns: what a contained picture's bands are filled with.
Rgb band_colour(const Picture& p, bool top_bottom);
// The night gradient at tile row y (kNightTop at 0, kNightBottom at 799).
Rgb night(int y);
// The disc's anti-aliased coverage (0..1) of tile pixel (x, y).
double disc_coverage(int x, int y);
// The 640x800 opaque tile for a picture drawn as `art` ("disc", or "box",
// "splash", "panel" and user pictures).
Picture render_tile(const Picture& p, std::string_view art);

// ---- files and WIC ------------------------------------------------------------------------

// A picture file's bytes: ImportError(source_invalid) when it is missing,
// unreadable or larger than 64 MB.
std::vector<uint8_t> read_picture_file(const std::filesystem::path& path);
// Any format WIC reads (first frame), with its EXIF orientation applied, as
// BGRA. `prescale_long_side` > 0 lets WIC scale a larger picture down to it
// while decoding (the long side does not change with the orientation).
// ImportError(source_invalid) for anything else, or outside 32..16384 px.
Picture decode_picture(std::span<const uint8_t> bytes, int prescale_long_side = 0);
// What original.png and user.png hold: decoded, oriented, cropped (crop.w > 0),
// long side capped at 2048.
Picture normalize(std::span<const uint8_t> bytes, const Crop& crop = {});
// PNG bytes: RGBA (`alpha`) or RGB.
std::vector<uint8_t> encode_png(const Picture& p, bool alpha);
// Any WIC container: "png", "jpeg", "bmp", "gif", "tiff" (the tests write
// their pictures with it). `exif_orientation` > 0 is written into a JPEG's EXIF.
std::vector<uint8_t> encode_picture(const Picture& p, std::string_view format, int exif_orientation = 0);

// An NE/PE RT_BITMAP resource is a DIB with no file header: this is the BMP
// file for it (BITMAPFILEHEADER with bfOffBits = 14 + header + palette).
std::vector<uint8_t> dib_to_bmp(std::string_view dib);
// Bitmap `id` of an NE or PE file, as a BMP file. ImportError(source_invalid)
// when the file is neither or has no such bitmap.
std::vector<uint8_t> bitmap_resource(std::string_view module, uint16_t id);

}  // namespace adw::import::cover
