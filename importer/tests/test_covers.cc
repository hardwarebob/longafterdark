// import.covers (COVERS.md §2.14): the box-cover pipeline, offline. Every
// picture is synthesized here; the registries are synthetic (test::TestRegistry
// with its own cover sources), and the downloads come from the loopback
// server, so nothing reaches the network.
//
//   * the tile rules as pure functions: fill or contain around ±15% of 4:5,
//     band colours, the disc's circle, nearest-then-cubic, crops, orientation
//   * decoding through WIC: BMP, PNG, JPEG, GIF, EXIF orientation 6, an NE and
//     a PE RT_BITMAP, the size limits, bytes that are no picture
//   * downloads: md5 and size, a redirect, 404 -> next source, a wrong md5 ->
//     next source (file deleted), a stalled server -> timeout, the other
//     downloads skipped and the disc used, reuse without a request
//   * during an import: covers\<id> and packages[].cover, a cancel in the
//     cover phase, every source failing (exit 0, the same import.json), a
//     re-import that fetches nothing and keeps user.png, a better source
//     replacing a fallback, --remove keeping the cover, recovery, Deluxe's
//     FILES byte-identical
//   * set_cover / clear_cover / refresh_covers and adimport.exe's exit codes
#include "http_server.h"

#include <objbase.h>

#include <phosg/JSON.hh>

#include <cmath>
#include <functional>
#include <set>

#include "catalog.h"
#include "cover_image.h"
#include "covers.h"
#include "minijson.h"
#include "covers_internal.h"
#include "importer.h"
#include "md5.h"
#include "pkg_fixture.h"
#include "run_process.h"

using namespace adw::import;
using cover::Picture;
using cover::Rgb;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_log;

phosg::JSON json_at(const fs::path& p) { return phosg::JSON::parse(test::read_text(p)); }

std::string md5_of(const std::vector<uint8_t>& v) { return md5_hex(v.data(), v.size()); }
std::string md5_file(const fs::path& p) { return md5_file_hex(p); }

bool logged(const std::string& needle) {
  for (const std::string& s : g_log)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}

// ---- pictures ------------------------------------------------------------------------------

// A smooth picture with thousands of colours.
Picture gradient(int w, int h, int seed = 0) {
  Picture p = Picture::filled(w, h, 0, 0, 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t* d = p.px(x, y);
      d[0] = uint8_t((x * 255) / std::max(1, w - 1));
      d[1] = uint8_t((y * 255) / std::max(1, h - 1));
      d[2] = uint8_t((x + y + seed * 37) & 0xFF);
    }
  return p;
}

// Rows of flat colour: `top` above `split`, `bottom` from it on.
Picture halves(int w, int h, Rgb top, Rgb bottom, int split = -1) {
  if (split < 0) split = h / 2;
  Picture p = Picture::filled(w, h, 0, 0, 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      Rgb c = y < split ? top : bottom;
      uint8_t* d = p.px(x, y);
      d[0] = c.b, d[1] = c.g, d[2] = c.r, d[3] = 255;
    }
  return p;
}

Picture checker(int w, int h, int cell, Rgb a, Rgb b) {
  Picture p = Picture::filled(w, h, 0, 0, 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      Rgb c = ((x / cell) + (y / cell)) % 2 ? a : b;
      uint8_t* d = p.px(x, y);
      d[0] = c.b, d[1] = c.g, d[2] = c.r, d[3] = 255;
    }
  return p;
}

Rgb at(const Picture& p, int x, int y) {
  const uint8_t* d = p.px(x, y);
  return Rgb{d[2], d[1], d[0]};
}

bool close_to(Rgb a, Rgb b, int tol = 2) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

std::string show(Rgb c) {
  char b[32];
  snprintf(b, sizeof(b), "(%d,%d,%d)", c.r, c.g, c.b);
  return b;
}

#define CHECK_RGB(a, b, tol)                                                                              \
  do {                                                                                                    \
    Rgb _a = (a), _b = (b);                                                                               \
    if (!close_to(_a, _b, tol)) {                                                                             \
      ::test::g_failures++;                                                                               \
      fprintf(stderr, "%s:%d: colour %s is not %s (%s vs %s)\n", __FILE__, __LINE__, #a, #b, show(_a).c_str(), \
              show(_b).c_str());                                                                          \
    }                                                                                                     \
  } while (0)

constexpr Rgb kRed{220, 20, 30}, kBlue{20, 40, 210}, kGreen{30, 200, 60}, kYellow{240, 220, 10}, kWhite{255, 255, 255};

Picture decode(const std::vector<uint8_t>& b) { return cover::decode_picture(b); }

bool opaque(const Picture& p) {
  for (size_t i = 3; i < p.bgra.size(); i += 4)
    if (p.bgra[i] != 255) return false;
  return true;
}

template <typename F>
Status status_of(F&& f) {
  try {
    f();
    return Status::ok;
  } catch (const ImportError& e) {
    return e.status();
  }
}

// ---- the pure rules ------------------------------------------------------------------------------

void test_rules() {
  // Fill within ±15% of 4:5 (0.68 .. 0.92), contain outside.
  CHECK(cover::fills_tile(640, 800));
  CHECK(cover::fills_tile(68, 100));
  CHECK(!cover::fills_tile(679, 1000));
  CHECK(cover::fills_tile(92, 100));
  CHECK(!cover::fills_tile(921, 1000));
  CHECK(!cover::fills_tile(387, 183));
  CHECK(!cover::fills_tile(118, 226));
  CHECK(cover::fills_tile(600, 776));

  cover::TileLayout wide = cover::plan_tile(387, 183, "splash");
  CHECK(wide.mode == cover::TileMode::contain);
  CHECK(wide.bands_top_bottom);
  CHECK_EQ(wide.w, 640);
  CHECK_EQ(wide.h, 303);  // 183 * 640 / 387
  CHECK_EQ(wide.x, 0);
  CHECK_EQ(wide.y, (800 - 303) / 2);
  cover::TileLayout tall = cover::plan_tile(118, 226, "panel");
  CHECK(tall.mode == cover::TileMode::contain);
  CHECK(!tall.bands_top_bottom);
  CHECK_EQ(tall.h, 800);
  CHECK_EQ(tall.w, 418);  // 118 * 800 / 226
  CHECK_EQ(tall.x, (640 - 418) / 2);
  cover::TileLayout box = cover::plan_tile(600, 776, "box");
  CHECK(box.mode == cover::TileMode::fill);
  CHECK_EQ(box.w, 640);
  CHECK_EQ(box.h, 828);
  CHECK_EQ(box.y, -14);
  cover::TileLayout disc = cover::plan_tile(300, 200, "disc");
  CHECK(disc.mode == cover::TileMode::disc);
  CHECK_EQ(disc.source.x, 50);
  CHECK_EQ(disc.source.w, 200);
  CHECK_EQ(disc.source.h, 200);
  CHECK_EQ(disc.x, 320 - 276);
  CHECK_EQ(disc.y, 368 - 276);
  CHECK_EQ(disc.w, 552);

  // Band colours: the mean of the two outermost rows, or columns.
  Picture bands = halves(100, 40, kRed, kGreen, 1);  // row 0 red, the rest green
  for (int y = 0; y < bands.h; y++) {
    bands.px(0, y)[0] = kYellow.b, bands.px(0, y)[1] = kYellow.g, bands.px(0, y)[2] = kYellow.r;
    bands.px(99, y)[0] = kBlue.b, bands.px(99, y)[1] = kBlue.g, bands.px(99, y)[2] = kBlue.r;
  }
  Rgb tb = cover::band_colour(bands, true);
  // Top row: yellow, 98 red, blue; bottom row: yellow, 98 green, blue.
  auto mean = [](std::initializer_list<std::pair<Rgb, int>> parts) {
    double r = 0, g = 0, b = 0, n = 0;
    for (auto& [c, k] : parts) r += c.r * k, g += c.g * k, b += c.b * k, n += k;
    return Rgb{uint8_t(std::lround(r / n)), uint8_t(std::lround(g / n)), uint8_t(std::lround(b / n))};
  };
  CHECK_RGB(tb, mean({{kYellow, 2}, {kRed, 98}, {kGreen, 98}, {kBlue, 2}}), 1);
  CHECK_RGB(cover::band_colour(bands, false), mean({{kYellow, 40}, {kBlue, 40}}), 1);

  // Contain: the bands are that colour; the picture sits centred.
  Picture splash = halves(387, 183, kRed, kBlue);
  Picture tile = cover::render_tile(splash, "splash");
  CHECK_EQ(tile.w, 640);
  CHECK_EQ(tile.h, 800);
  CHECK(opaque(tile));
  Rgb band = cover::band_colour(splash, true);
  CHECK_RGB(at(tile, 320, 10), band, 0);
  CHECK_RGB(at(tile, 320, 790), band, 0);
  CHECK_RGB(at(tile, 320, 300), kRed, 2);
  CHECK_RGB(at(tile, 320, 500), kBlue, 2);
  // Left and right bands for a tall picture.
  Picture panel = halves(118, 226, kGreen, kYellow);
  tile = cover::render_tile(panel, "panel");
  CHECK_RGB(at(tile, 5, 400), cover::band_colour(panel, false), 0);
  CHECK_RGB(at(tile, 320, 100), kGreen, 2);
  // Alpha is flattened onto the band colour.
  Picture holed = halves(387, 183, kRed, kRed);
  for (int y = 60; y < 120; y++)
    for (int x = 150; x < 230; x++) holed.px(x, y)[3] = 0;
  tile = cover::render_tile(holed, "splash");
  CHECK_RGB(at(tile, 320, 400), cover::band_colour(holed, true), 2);
  CHECK(opaque(tile));
  // Fill: a 4:5 picture covers the whole tile.
  tile = cover::render_tile(halves(400, 500, kBlue, kYellow), "box");
  CHECK_RGB(at(tile, 5, 5), kBlue, 2);
  CHECK_RGB(at(tile, 634, 794), kYellow, 2);

  // The disc: the centred square as a circle of 552 px at (320, 368), with an
  // anti-aliased rim, over the night gradient.
  Picture lp = Picture::filled(300, 200, kRed.b, kRed.g, kRed.r);
  for (int y = 0; y < 200; y++)
    for (int x = 0; x < 50; x++) lp.px(x, y)[0] = kBlue.b, lp.px(x, y)[1] = kBlue.g, lp.px(x, y)[2] = kBlue.r;
  tile = cover::render_tile(lp, "disc");
  CHECK_RGB(at(tile, 320, 368), kRed, 1);
  CHECK_RGB(at(tile, 320 - 270, 368), kRed, 2);  // the square starts past the blue strip
  CHECK_RGB(at(tile, 0, 0), cover::kNightTop, 0);
  CHECK_RGB(at(tile, 639, 799), cover::kNightBottom, 0);
  CHECK_RGB(at(tile, 320, 20), cover::night(20), 0);
  CHECK_RGB(cover::night(0), cover::kNightTop, 0);
  CHECK_RGB(cover::night(799), cover::kNightBottom, 0);
  CHECK_EQ(cover::disc_coverage(320, 368), 1.0);
  CHECK(cover::disc_coverage(595, 368) > 0.99);  // its centre 275.5 px out
  CHECK_EQ(cover::disc_coverage(596, 368), 0.0);  // 276.5 px out
  // The rim is anti-aliased: partly covered pixels all round, drawn as blends.
  int partial = 0, bx = -1, by = -1;
  for (int y = 0; y < 800; y++)
    for (int x = 0; x < 640; x++) {
      double c = cover::disc_coverage(x, y);
      if (c > 0 && c < 1) partial++;
      if (c > 0.4 && c < 0.6 && bx < 0) bx = x, by = y;
    }
  CHECK(partial > 1000);
  CHECK(bx >= 0);
  if (bx >= 0) {
    Rgb rim = at(tile, bx, by);
    CHECK(!close_to(rim, kRed, 10) && !close_to(rim, cover::night(by), 10));
  }
  CHECK_RGB(at(tile, 598, 368), cover::night(368), 0);
  int across = 0;
  for (int x = 0; x < 640; x++) across += cover::disc_coverage(x, 368) >= 0.5;
  CHECK_EQ(across, 552);
  int down = 0;
  for (int y = 0; y < 800; y++) down += cover::disc_coverage(320, y) >= 0.5;
  CHECK_EQ(down, 552);

  // Nearest first for a picture of few colours scaled by 2 or more: a
  // checkerboard stays two colours; many colours, or less scale, go cubic.
  CHECK(cover::uses_nearest_first(40, 50, 640, 800, true));
  CHECK(!cover::uses_nearest_first(40, 50, 640, 800, false));
  CHECK(!cover::uses_nearest_first(400, 500, 640, 800, true));  // x1.6
  Picture check = checker(40, 50, 5, kWhite, kBlue);
  CHECK_EQ(cover::count_colours(cover::render_tile(check, "box")), 2);
  CHECK(cover::count_colours(cover::scale_for_tile(check, 640, 800, false)) > 2);
  // A 3.54x scale: x3 nearest, then cubic; the edges stay sharp (few colours
  // blended at the cell boundaries only).
  Picture odd = cover::scale_for_tile(checker(113, 226, 8, kWhite, kBlue), 400, 800, true);
  CHECK_EQ(odd.w, 400);
  CHECK_RGB(at(odd, 4, 4), kBlue, 1);

  // Crops, and one outside the picture (a failed source).
  Picture g = gradient(300, 200);
  CHECK(cover::crop_fits(g, Crop{0, 0, 300, 200}));
  CHECK(!cover::crop_fits(g, Crop{0, 0, 301, 200}));
  CHECK(!cover::crop_fits(g, Crop{-1, 0, 10, 10}));
  CHECK(!cover::crop_fits(g, Crop{250, 150, 60, 10}));
  Picture c = cover::crop(g, Crop{10, 20, 100, 50});
  CHECK_EQ(c.w, 100);
  CHECK_EQ(c.h, 50);
  CHECK(memcmp(c.px(0, 0), g.px(10, 20), 4) == 0);
  CHECK(memcmp(c.px(99, 49), g.px(109, 69), 4) == 0);
  CHECK_EQ(status_of([&] { cover::crop(g, Crop{0, 0, 387, 183}); }), Status::source_invalid);

  // Orientation: 6 turns clockwise; every one of the eight is a permutation.
  Picture o = gradient(3, 2);
  Picture o6 = cover::orient(o, 6);
  CHECK_EQ(o6.w, 2);
  CHECK_EQ(o6.h, 3);
  CHECK(memcmp(o6.px(0, 0), o.px(0, 1), 4) == 0);  // the bottom-left comes to the top-left
  CHECK(memcmp(o6.px(1, 0), o.px(0, 0), 4) == 0);
  Picture o8 = cover::orient(o, 8);
  CHECK(memcmp(o8.px(0, 0), o.px(2, 0), 4) == 0);
  for (int k = 1; k <= 8; k++) {
    Picture t = cover::orient(o, k);
    CHECK_EQ(t.w * t.h, 6);
    std::set<std::vector<uint8_t>> seen;
    for (int y = 0; y < t.h; y++)
      for (int x = 0; x < t.w; x++) seen.insert(std::vector<uint8_t>(t.px(x, y), t.px(x, y) + 4));
    CHECK_EQ(seen.size(), size_t(6));
  }

  // Capping: the long side at 2048, never up.
  Picture big = gradient(3000, 1500);
  Picture capped = cover::cap_long_side(big, 2048);
  CHECK_EQ(capped.w, 2048);
  CHECK_EQ(capped.h, 1024);
  CHECK_EQ(cover::cap_long_side(g, 2048).w, 300);
}

// ---- decoding ----------------------------------------------------------------------------------------

// A bottom-up 8-bit DIB (BITMAPINFOHEADER, 4 palette entries): the top half
// palette index 1, the bottom half index 2.
std::string dib8(int w, int h, Rgb one, Rgb two) {
  test::Bytes b;
  b.u32(0, 40);
  b.u32(4, uint32_t(w));
  b.u32(8, uint32_t(h));
  b.u16(12, 1);
  b.u16(14, 8);
  b.u32(32, 4);  // biClrUsed
  const Rgb pal[4] = {{0, 0, 0}, one, two, {255, 255, 255}};
  for (int i = 0; i < 4; i++) {
    b.u8(40 + 4 * size_t(i), pal[i].b);
    b.u8(41 + 4 * size_t(i), pal[i].g);
    b.u8(42 + 4 * size_t(i), pal[i].r);
    b.u8(43 + 4 * size_t(i), 0);
  }
  size_t stride = (size_t(w) + 3) & ~size_t(3), bits = 56;
  for (int row = 0; row < h; row++) {  // bottom-up: file row 0 is the bottom
    int y = h - 1 - row;
    for (int x = 0; x < w; x++) b.u8(bits + size_t(row) * stride + size_t(x), y < h / 2 ? 1 : 2);
    b.grow(bits + size_t(row + 1) * stride);
  }
  return b.d;
}

void test_decoding(const fs::path& dir) {
  Picture four = halves(64, 48, kRed, kBlue);
  for (const char* fmt : {"bmp", "png", "jpeg", "gif", "tiff"}) {
    std::vector<uint8_t> bytes = cover::encode_picture(four, fmt);
    test::write_bytes(dir / (L"decode." + to_wide(fmt)), bytes);
    Picture p = decode(test::read_bytes(dir / (L"decode." + to_wide(fmt))));
    CHECK_EQ(p.w, 64);
    CHECK_EQ(p.h, 48);
    CHECK_RGB(at(p, 10, 5), kRed, std::string(fmt) == "jpeg" ? 12 : 0);
    CHECK_RGB(at(p, 50, 40), kBlue, std::string(fmt) == "jpeg" ? 12 : 0);
  }
  // A JPEG whose EXIF says "rotate 90 clockwise": it comes out turned.
  Picture lr = Picture::filled(64, 32, 0, 0, 0);
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++) {
      Rgb c = x < 32 ? kRed : kBlue;
      lr.px(x, y)[0] = c.b, lr.px(x, y)[1] = c.g, lr.px(x, y)[2] = c.r;
    }
  Picture turned = decode(cover::encode_picture(lr, "jpeg", 6));
  CHECK_EQ(turned.w, 32);
  CHECK_EQ(turned.h, 64);
  CHECK_RGB(at(turned, 16, 8), kRed, 14);    // the left half is now on top
  CHECK_RGB(at(turned, 16, 56), kBlue, 14);
  Picture plain = decode(cover::encode_picture(lr, "jpeg"));
  CHECK_EQ(plain.w, 64);

  // An RT_BITMAP resource of an NE file and of a PE file.
  std::string dib = dib8(40, 40, kGreen, kYellow);
  std::vector<uint8_t> bmp = cover::dib_to_bmp(dib);
  CHECK_EQ(bmp.size(), dib.size() + 14);
  CHECK(bmp[0] == 'B' && bmp[1] == 'M');
  CHECK_EQ(uint32_t(bmp[10] | bmp[11] << 8), uint32_t(14 + 40 + 16));  // header + 4 palette entries
  // A damaged colour count is an error, never a wrapped offset and a garbled
  // picture: 0x40000001 entries (x4 wraps to 4 in 32 bits), more colours
  // than 8 bits can index, and a 24-bit table larger than the resource.
  for (uint32_t used : {0x40000001u, 257u}) {
    std::string bad = dib;
    for (int i = 0; i < 4; i++) bad[size_t(32 + i)] = char(uint8_t(used >> (8 * i)));
    CHECK_EQ(status_of([&] { cover::dib_to_bmp(bad); }), Status::source_invalid);
  }
  {
    std::string bad = dib;
    bad[14] = 24;  // biBitCount
    for (int i = 0; i < 4; i++) bad[size_t(32 + i)] = char(uint8_t(0x40000001u >> (8 * i)));
    CHECK_EQ(status_of([&] { cover::dib_to_bmp(bad); }), Status::source_invalid);
  }
  test::NeSpec ne;
  ne.resources = {{2, "", 7500, dib}, {2000, "", 20, std::string("Setup\0", 6)}};
  std::string ne_file = test::build_ne(ne);
  Picture from_ne = decode(cover::bitmap_resource(ne_file, 7500));
  CHECK_EQ(from_ne.w, 40);
  CHECK_EQ(from_ne.h, 40);
  CHECK_RGB(at(from_ne, 5, 5), kGreen, 0);
  CHECK_RGB(at(from_ne, 5, 35), kYellow, 0);
  test::PeSpec pe;
  pe.resources = {{2, "", 101, 0x409, dib}};
  Picture from_pe = decode(cover::bitmap_resource(test::build_pe(pe), 101));
  CHECK_RGB(at(from_pe, 5, 35), kYellow, 0);
  CHECK_EQ(status_of([&] { cover::bitmap_resource(ne_file, 7501); }), Status::source_invalid);
  CHECK_EQ(status_of([&] { cover::bitmap_resource("not a module at all", 1); }), Status::source_invalid);

  // Limits: 16x16 is too small, random bytes are no picture, a file over
  // 64 MB is refused before it is read, a large picture is capped at 2048.
  CHECK_EQ(status_of([&] { decode(cover::encode_picture(gradient(16, 16), "png")); }), Status::source_invalid);
  CHECK_EQ(status_of([&] { decode(test::pattern(5000, 3)); }), Status::source_invalid);
  CHECK_EQ(status_of([&] { decode({}); }), Status::source_invalid);
  {
    fs::path huge = dir / L"huge.png";
    HANDLE h = CreateFileW(huge.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER at{};
    at.QuadPart = (64ll << 20) + 1;
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
    std::string why;
    try {
      cover::read_picture_file(huge);
    } catch (const ImportError& e) {
      why = e.what();
    }
    CHECK(why.find("64 MB") != std::string::npos);
    fs::remove(huge);
  }
  CHECK_EQ(status_of([&] { cover::read_picture_file(dir / L"nowhere.png"); }), Status::source_invalid);
  Picture n = cover::normalize(cover::encode_picture(gradient(2600, 1300), "png"));
  CHECK_EQ(n.w, 2048);
  CHECK_EQ(n.h, 1024);
  n = cover::normalize(cover::encode_picture(gradient(2600, 1300), "png"), Crop{100, 100, 400, 500});
  CHECK_EQ(n.w, 400);
  CHECK_EQ(n.h, 500);
  // PNG round trip: RGBA kept, or RGB for the tile.
  Picture back = decode(cover::encode_png(four, false));
  CHECK_RGB(at(back, 10, 5), kRed, 0);
}

// ---- synthetic registries ------------------------------------------------------------------------

CoverSource dl_source(const std::string& url, const std::vector<uint8_t>& file, const std::wstring& name, Crop crop = {},
                      const char* art = "box", const char* label = "Box front", const char* credit = "Loopback") {
  CoverSource s{};
  s.kind = CoverSource::Kind::download;
  s.art = art;
  s.label = label;
  s.credit = credit;
  s.url = test::keep(url);
  s.md5 = test::keep(md5_of(file));
  s.size = file.size();
  s.file_name = test::keep(name);
  s.crop = crop;
  return s;
}

CoverSource disc_source(const std::string& path, const std::string& md5, Crop crop = {}, const char* art = "splash",
                        uint16_t res_type = 0, uint16_t res_id = 0) {
  CoverSource s{};
  s.kind = CoverSource::Kind::disc;
  s.art = art;
  s.label = "Installer art";
  s.credit = "your disc";
  s.path = test::keep(path);
  s.path_md5 = test::keep(md5);
  s.resource_type = res_type;
  s.resource_id = res_id;
  s.crop = crop;
  return s;
}

Source folder(const fs::path& p) {
  Source s;
  s.kind = Source::Kind::folder;
  s.path = p;
  return s;
}

ImportOptions opts_for(const fs::path& root, const test::TestRegistry& reg, const fs::path& dl, int timeout_ms = 3000) {
  ImportOptions o;
  o.assets_root = root;
  o.check_known = false;
  o.registry = reg.span();
  o.cover_download_dir = dl;
  o.cover_timeout_ms = timeout_ms;
  o.log = [](const std::string& s) {
    g_log.push_back(s);
    fprintf(stderr, "  log: %s\n", s.c_str());
  };
  return o;
}

CoverOptions cover_opts(const fs::path& root, const test::TestRegistry& reg, const fs::path& dl) {
  CoverOptions o;
  o.assets_root = root;
  o.download_dir = dl;
  o.registry = reg.span();
  o.timeout_ms = 3000;
  o.log = [](const std::string& s) {
    g_log.push_back(s);
    fprintf(stderr, "  log: %s\n", s.c_str());
  };
  return o;
}

ImportResult import(const char* what, const Source& s, const ImportOptions& o, Status want = Status::ok) {
  ImportResult r = run_import(s, o);
  fprintf(stderr, "[%s] %s: %s\n", what, status_name(r.status), r.message.c_str());
  CHECK_EQ(r.status, want);
  return r;
}

const phosg::JSON* catalog_package(const phosg::JSON& cat, const std::string& id) {
  for (auto& p : cat.at("packages").as_list())
    if (p->get_string("id") == id) return p.get();
  return nullptr;
}

phosg::JSON cover_json(const fs::path& root, const std::string& id) {
  return json_at(root / L"win" / L"covers" / to_wide(id) / L"cover.json");
}

// The catalog's cover of `id` agrees with covers\<id> on disk.
void check_catalog_cover(const fs::path& root, const std::string& id, const std::string& origin) {
  phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
  const phosg::JSON* p = catalog_package(cat, id);
  CHECK(p != nullptr);
  if (!p) return;
  const phosg::JSON& c = p->at("cover");
  CHECK_EQ(c.get_string("origin"), origin);
  if (origin == "generated") {
    CHECK_EQ(c.as_dict().size(), size_t(1));
    return;
  }
  fs::path win = root / L"win";
  CHECK_EQ(c.get_string("tile"), "covers/" + id + "/tile.png");
  CHECK_EQ(c.get_string("tileMd5"), md5_file(win / L"covers" / to_wide(id) / L"tile.png"));
  std::string image = c.get_string("image");
  CHECK(image == "covers/" + id + (origin == "user" ? "/user.png" : "/original.png"));
  Picture img = decode(test::read_bytes(test::path_under(win, image)));
  CHECK_EQ(c.get_int("width"), int64_t(img.w));
  CHECK_EQ(c.get_int("height"), int64_t(img.h));
  Picture tile = decode(test::read_bytes(win / L"covers" / to_wide(id) / L"tile.png"));
  CHECK_EQ(tile.w, 640);
  CHECK_EQ(tile.h, 800);
  CHECK(opaque(tile));
}

bool no_cover_leftovers(const fs::path& root) {
  bool ok = true;
  std::error_code ec;
  fs::path d = root / L"win" / L"covers";
  for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); ++it) {
    std::wstring n = it->path().filename().wstring();
    if (n.find(L".importing-") != std::wstring::npos || n.find(L".tmp-") != std::wstring::npos) {
      fprintf(stderr, "  leftover: %s\n", to_utf8(it->path().wstring()).c_str());
      ok = false;
    }
  }
  return ok;
}

// The paths the server was asked for since the last clear().
std::set<std::string> asked(test::Server& srv) {
  std::set<std::string> s;
  for (auto& r : srv.requests()) s.insert(r.path);
  return s;
}

std::string without_utc(std::string json) {
  size_t a = json.find("\"importedUtc\"");
  if (a == std::string::npos) return json;
  size_t b = json.find('\n', a);
  return json.erase(a, b - a);
}

// ---- downloads and imports ------------------------------------------------------------------------

struct World {
  fs::path dir, src_tt;
  test::PkgFixture tt;
  std::vector<uint8_t> box_png, box2_jpg, splash_bmp;
  test::Server* srv = nullptr;
};

void test_downloads(World& w) {
  test::Server& srv = *w.srv;
  const fs::path dl = w.dir / L"downloads";
  const std::string splash_md5 = md5_of(w.splash_bmp);
  auto disc = [&] { return disc_source("INSTALL/SETUP.BMP", splash_md5, Crop{0, 0, 200, 90}); };

  // md5 and size checked, reached through a redirect: covers\tt and the catalog.
  {
    test::TestRegistry reg;
    reg.covers("tt", {dl_source(srv.url("/r/box.png"), w.box_png, L"box.png"), disc()});
    fs::path root = w.dir / L"dl-good";
    srv.clear();
    import("good download", folder(w.src_tt), opts_for(root, reg, dl));
    std::set<std::string> a = asked(srv);
    CHECK(a.count("/r/box.png") && a.count("/box.png"));
    CHECK(fs::exists(dl / L"covers" / L"box.png"));
    phosg::JSON cj = cover_json(root, "tt");
    CHECK_EQ(cj.get_int("version"), int64_t(1));
    CHECK_EQ(cj.get_string("tool"), std::string(kToolName));
    CHECK_EQ(std::string(kToolName), std::string("adimport 1.3"));
    CHECK_EQ(cj.at("original").get_string("origin"), std::string("download"));
    CHECK_EQ(cj.at("original").get_int("source"), int64_t(0));
    CHECK_EQ(cj.at("original").get_string("fileMd5"), md5_of(w.box_png));
    CHECK_EQ(cj.at("original").get_int("fileSize"), int64_t(w.box_png.size()));
    CHECK_EQ(cj.at("original").get_string("url"), srv.url("/r/box.png"));
    CHECK(cj.at("original").at("crop").is_null());
    CHECK_EQ(cj.at("original").get_string("md5"), md5_file(root / L"win" / L"covers" / L"tt" / L"original.png"));
    CHECK_EQ(cj.at("tile").get_string("from"), std::string("original"));
    CHECK_EQ(cj.at("tile").get_int("renderer"), int64_t(cover::kRendererVersion));
    CHECK(!cj.contains("user"));
    check_catalog_cover(root, "tt", "download");
    phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
    const phosg::JSON& c = catalog_package(cat, "tt")->at("cover");
    CHECK_EQ(c.get_string("label"), std::string("Box front"));
    CHECK_EQ(c.get_string("credit"), std::string("Loopback"));
    CHECK_EQ(c.get_string("art"), std::string("box"));
    CHECK_EQ(c.get_string("original"), std::string("download"));
    CHECK_EQ(c.get_int("width"), int64_t(400));
    CoverInfo info = cover_info("tt", root, reg.span());
    CHECK(info.installed);
    CHECK(info.origin == CoverOrigin::download);
    CHECK(!info.can_download);  // nothing better than source 0
    CHECK_EQ(info.tile, root / L"win" / L"covers" / L"tt" / L"tile.png");
    CHECK(no_cover_leftovers(root));

    // Reuse: another root, the same downloads folder: no request at all.
    srv.clear();
    fs::path root2 = w.dir / L"dl-reuse";
    import("reuse the downloaded cover", folder(w.src_tt), opts_for(root2, reg, dl));
    CHECK(srv.requests().empty());
    check_catalog_cover(root2, "tt", "download");
  }
  // 404 -> the next source (with its crop).
  {
    test::TestRegistry reg;
    reg.covers("tt", {dl_source(srv.url("/missing.png"), w.box_png, L"missing.png"),
                      dl_source(srv.url("/box2.jpg"), w.box2_jpg, L"box2.jpg", Crop{0, 0, 300, 375}), disc()});
    fs::path root = w.dir / L"dl-404";
    import("404 then the next", folder(w.src_tt), opts_for(root, reg, dl));
    phosg::JSON cj = cover_json(root, "tt");
    CHECK_EQ(cj.at("original").get_int("source"), int64_t(1));
    CHECK_EQ(cj.at("original").get_int("width"), int64_t(300));
    CHECK_EQ(cj.at("original").get_int("height"), int64_t(375));
    CHECK_EQ(cj.at("original").at("crop").get_int("h"), int64_t(375));
    CHECK_EQ(cj.at("attempts").at("failed").as_list().at(0)->get_string("status"), std::string("unavailable"));
    CHECK_EQ(cj.at("attempts").at("failed").as_list().at(0)->get_int("source"), int64_t(0));
    CHECK(cover_info("tt", root, reg.span()).can_download);  // source 0 is better
    check_catalog_cover(root, "tt", "download");
  }
  // A wrong md5 -> the next source, and the bad file is deleted.
  {
    test::TestRegistry reg;
    CoverSource wrong = dl_source(srv.url("/box.png"), w.box2_jpg, L"wrong.png");
    wrong.size = w.box_png.size();  // the size the server announces; only the md5 is wrong
    reg.covers("tt", {wrong, dl_source(srv.url("/box2.jpg"), w.box2_jpg, L"box2.jpg", Crop{0, 0, 300, 375}), disc()});
    fs::path root = w.dir / L"dl-md5";
    import("wrong md5", folder(w.src_tt), opts_for(root, reg, dl));
    CHECK_EQ(cover_json(root, "tt").at("original").get_int("source"), int64_t(1));
    CHECK_EQ(cover_json(root, "tt").at("attempts").at("failed").as_list().at(0)->get_string("status"),
             std::string("mismatch"));
    CHECK(!fs::exists(dl / L"covers" / L"wrong.png"));
    CHECK(!fs::exists(dl / L"covers" / L"wrong.png.part"));
  }
  // A stalled server: a timeout, the other downloads skipped, the disc used.
  {
    srv.stall("/stall.png");
    test::TestRegistry reg;
    reg.covers("tt", {dl_source(srv.url("/stall.png"), w.box_png, L"stall.png"),
                      dl_source(srv.url("/box.png"), w.box_png, L"box-after-stall.png"), disc()});
    fs::path root = w.dir / L"dl-stall";
    srv.clear();
    ULONGLONG t0 = GetTickCount64();
    import("stalled server", folder(w.src_tt), opts_for(root, reg, dl, 300));
    ULONGLONG took = GetTickCount64() - t0;
    fprintf(stderr, "  (the stalled import took %llu ms)\n", took);
    CHECK(took < 10000);
    std::set<std::string> a = asked(srv);
    CHECK(a.count("/stall.png"));
    CHECK(!a.count("/box.png"));  // skipped after the network failure
    phosg::JSON cj = cover_json(root, "tt");
    CHECK_EQ(cj.at("original").get_string("origin"), std::string("disc"));
    CHECK_EQ(cj.at("original").get_int("source"), int64_t(2));
    CHECK_EQ(cj.at("original").get_string("path"), std::string("INSTALL/SETUP.BMP"));
    CHECK_EQ(cj.at("original").get_string("pathMd5"), splash_md5);
    CHECK_EQ(cj.at("original").get_int("width"), int64_t(200));
    CHECK_EQ(cj.at("original").get_int("height"), int64_t(90));
    const auto& failed = cj.at("attempts").at("failed").as_list();
    CHECK_EQ(failed.size(), size_t(2));
    CHECK_EQ(failed.at(0)->get_string("status"), std::string("network"));
    CHECK_EQ(failed.at(1)->get_string("status"), std::string("skipped"));
    check_catalog_cover(root, "tt", "disc");
    CoverInfo info = cover_info("tt", root, reg.span());
    CHECK(info.origin == CoverOrigin::disc);
    CHECK(info.can_download);
    CHECK_EQ(info.label, std::string("Installer art"));
    CHECK_EQ(info.width, 200);
  }
  // --no-cover-download: the downloads are not even tried.
  {
    test::TestRegistry reg;
    reg.covers("tt", {dl_source(srv.url("/box.png"), w.box_png, L"box-off.png"), disc()});
    fs::path root = w.dir / L"dl-off";
    ImportOptions o = opts_for(root, reg, dl);
    o.cover_download = false;
    srv.clear();
    import("--no-cover-download", folder(w.src_tt), o);
    CHECK(srv.requests().empty());
    CHECK_EQ(cover_json(root, "tt").at("original").get_string("origin"), std::string("disc"));
  }
  // A disc file with other bytes (another pressing) is skipped.
  {
    test::TestRegistry reg;
    reg.covers("tt", {disc_source("INSTALL/SETUP.BMP", std::string(32, '0'))});
    fs::path root = w.dir / L"dl-pressing";
    import("another pressing", folder(w.src_tt), opts_for(root, reg, dl));
    CHECK(!fs::exists(root / L"win" / L"covers" / L"tt" / L"original.png"));
    CHECK_EQ(cover_json(root, "tt").at("attempts").at("failed").as_list().at(0)->get_string("status"),
             std::string("mismatch"));
    check_catalog_cover(root, "tt", "generated");
  }
}

void test_imports(World& w) {
  test::Server& srv = *w.srv;
  const fs::path dl = w.dir / L"downloads";
  const std::string splash_md5 = md5_of(w.splash_bmp);
  auto disc = [&] { return disc_source("INSTALL/SETUP.BMP", splash_md5, Crop{0, 0, 200, 90}); };

  // A cancel in the cover phase: nothing is installed, no stage is left.
  {
    test::TestRegistry reg;
    reg.covers("tt", {dl_source(srv.url("/box.png"), w.box_png, L"box-cancel.png"), disc()});
    fs::path root = w.dir / L"cancel";
    ImportOptions o = opts_for(root, reg, dl);
    bool saw = false;
    o.progress = [&](const Progress& p) {
      if (p.phase != Progress::Phase::cover) return true;
      saw = true;
      return p.done == 0 && p.item.empty();  // the phase starts, then stop
    };
    import("cancel in the cover phase", folder(w.src_tt), o, Status::cancelled);
    CHECK(saw);
    CHECK(!fs::exists(root / L"win" / L"packages" / L"tt"));
    CHECK(!fs::exists(root / L"win" / L"covers" / L"tt"));
    CHECK(no_cover_leftovers(root));
    // The phase names its source as it goes.
    std::vector<std::string> items;
    o.progress = [&](const Progress& p) {
      if (p.phase == Progress::Phase::cover && !p.item.empty()) items.push_back(p.item);
      return true;
    };
    import("cover progress", folder(w.src_tt), o);
    CHECK(!items.empty() && items.front() == "Box front from Loopback");
  }
  // Every source failing: exit 0, and import.json as it is without covers.
  {
    test::TestRegistry none, failing;
    failing.covers("tt", {dl_source(srv.url("/missing.png"), w.box_png, L"missing2.png"),
                          disc_source("INSTALL/NOSUCH.BMP", "")});
    fs::path a = w.dir / L"fail-a", b = w.dir / L"fail-b";
    ImportResult ra = import("every cover source failing", folder(w.src_tt), opts_for(a, failing, dl));
    ImportResult rb = import("no cover sources", folder(w.src_tt), opts_for(b, none, dl));
    CHECK_EQ(ra.package_modules, rb.package_modules);
    CHECK_EQ(without_utc(test::read_text(a / L"win" / L"packages" / L"tt" / L"import.json")),
             without_utc(test::read_text(b / L"win" / L"packages" / L"tt" / L"import.json")));
    check_catalog_cover(a, "tt", "generated");
    CHECK(fs::exists(a / L"win" / L"covers" / L"tt" / L"cover.json"));  // the attempts, for --refresh-covers
    CHECK(!fs::exists(b / L"win" / L"covers"));                          // no sources: nothing written
    CHECK(logged("adimport --refresh-covers tries again"));
  }
  // A re-import fetches nothing it already has, and keeps user.png; a better
  // source replaces a fallback.
  {
    test::TestRegistry reg;
    reg.covers("tt", {dl_source(srv.url("/later.png"), w.box_png, L"later.png"), disc()});
    fs::path root = w.dir / L"reimport";
    import("fallback to the disc", folder(w.src_tt), opts_for(root, reg, dl));
    CHECK_EQ(cover_json(root, "tt").at("original").get_string("origin"), std::string("disc"));
    fs::path pic = w.dir / L"mine.png";
    test::write_bytes(pic, cover::encode_picture(halves(500, 700, kGreen, kRed), "png"));
    CoverResult s = set_cover("tt", pic, cover_opts(root, reg, dl));
    CHECK_EQ(s.status, Status::ok);
    std::string user_md5 = md5_file(root / L"win" / L"covers" / L"tt" / L"user.png");
    std::string tile_md5 = md5_file(root / L"win" / L"covers" / L"tt" / L"tile.png");
    srv.serve("/later.png", w.box_png);
    import("the better source now answers", folder(w.src_tt), opts_for(root, reg, dl));
    phosg::JSON cj = cover_json(root, "tt");
    CHECK_EQ(cj.at("original").get_string("origin"), std::string("download"));
    CHECK_EQ(cj.at("user").get_string("md5"), user_md5);
    CHECK_EQ(cj.at("user").get_string("name"), std::string("mine.png"));
    CHECK_EQ(md5_file(root / L"win" / L"covers" / L"tt" / L"user.png"), user_md5);
    CHECK_EQ(md5_file(root / L"win" / L"covers" / L"tt" / L"tile.png"), tile_md5);  // still the user's
    check_catalog_cover(root, "tt", "user");
    phosg::JSON cat = json_at(root / L"win" / L"catalog-win.json");
    CHECK_EQ(catalog_package(cat, "tt")->at("cover").get_string("original"), std::string("download"));
    srv.clear();
    import("again: nothing to fetch", folder(w.src_tt), opts_for(root, reg, dl));
    CHECK(srv.requests().empty());
    CHECK_EQ(md5_file(root / L"win" / L"covers" / L"tt" / L"user.png"), user_md5);

    // --remove keeps covers\<id>; the catalog lists no cover for it; a later
    // import finds it again.
    RemoveResult rm = remove_package("tt", root, {}, reg.span());
    CHECK_EQ(rm.status, Status::ok);
    CHECK(fs::exists(root / L"win" / L"covers" / L"tt" / L"user.png"));
    CHECK(json_at(root / L"win" / L"catalog-win.json").at("packages").as_list().empty());
    CHECK(!cover_info("tt", root, reg.span()).installed);
    srv.clear();
    import("back again", folder(w.src_tt), opts_for(root, reg, dl));
    CHECK(srv.requests().empty());
    check_catalog_cover(root, "tt", "user");
    srv.forget("/later.png");
  }
  // Recovery: stages and tmp files are deleted, and the catalog rewritten.
  {
    test::TestRegistry reg;
    reg.covers("tt", {disc()});
    fs::path root = w.dir / L"recovery";
    import("to recover", folder(w.src_tt), opts_for(root, reg, dl));
    fs::path covers = root / L"win" / L"covers";
    test::write_bytes(covers / L"tt.importing-4242" / L"original.png", {1, 2, 3});
    test::write_bytes(covers / L"tt" / L"tile.png.tmp-4242", {4, 5});
    test::write_bytes(covers / L"tt" / L"cover.json.tmp-77", {6});
    CatalogResult c = regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span());
    CHECK_EQ(c.status, Status::ok);
    CHECK(!fs::exists(covers / L"tt.importing-4242"));
    CHECK(!fs::exists(covers / L"tt" / L"tile.png.tmp-4242"));
    CHECK(!fs::exists(covers / L"tt" / L"cover.json.tmp-77"));
    CHECK(logged("removed the cover files an interrupted operation left behind"));
    check_catalog_cover(root, "tt", "disc");
  }
  // Deluxe: the cover comes from outside FILES, and FILES stays byte-identical.
  {
    test::PkgFixture dx = test::deluxe_fixture();
    std::vector<uint8_t> page = cover::encode_picture(halves(118, 226, kYellow, kBlue), "bmp");
    dx.source["ADE/PAGE1.BMP"] = page;
    fs::path src = w.dir / L"src-deluxe";
    test::write_tree(src, dx.source);
    test::TestRegistry reg;
    reg.covers("deluxe", {disc_source("ADE/PAGE1.BMP", md5_of(page), {}, "panel")});
    fs::path root = w.dir / L"deluxe";
    import("deluxe", folder(src), opts_for(root, reg, dl));
    test::Tree files;
    for (const std::string& rel : test::list_tree(root / L"win" / L"FILES"))
      files["FILES/" + rel] = test::read_bytes(test::path_under(root / L"win" / L"FILES", rel));
    CHECK(files == dx.expect);
    check_catalog_cover(root, "deluxe", "disc");
    CHECK_EQ(cover_json(root, "deluxe").at("original").get_int("width"), int64_t(118));
    // Re-importing Deluxe keeps it (and FILES is swapped as ever).
    import("deluxe again", folder(src), opts_for(root, reg, dl));
    check_catalog_cover(root, "deluxe", "disc");
    CHECK(no_cover_leftovers(root));
  }
}

// ---- the cover commands ------------------------------------------------------------------------

void test_commands(World& w, const std::wstring& exe) {
  test::Server& srv = *w.srv;
  const fs::path dl = w.dir / L"downloads-cmd";
  const std::string splash_md5 = md5_of(w.splash_bmp);
  test::TestRegistry reg;
  reg.covers("tt", {dl_source(srv.url("/cmd.png"), w.box_png, L"cmd.png"),
                    disc_source("INSTALL/SETUP.BMP", splash_md5, Crop{0, 0, 200, 90})});
  fs::path root = w.dir / L"commands";
  import("installed for the commands", folder(w.src_tt), opts_for(root, reg, dl));
  const fs::path covers = root / L"win" / L"covers" / L"tt";
  const fs::path catalog = root / L"win" / L"catalog-win.json";
  CHECK_EQ(cover_json(root, "tt").at("original").get_string("origin"), std::string("disc"));
  CoverOptions o = cover_opts(root, reg, dl);

  // set_cover: refused for an unknown or uninstalled package, an unreadable
  // picture, or while the lock is held.
  fs::path pic = w.dir / L"picture.jpg";
  test::write_bytes(pic, cover::encode_picture(gradient(820, 1000, 4), "jpeg"));
  fs::path junk = w.dir / L"junk.png";
  test::write_bytes(junk, test::pattern(3000, 9));
  CHECK_EQ(set_cover("nosuch", pic, o).status, Status::error);
  CoverResult r = set_cover("ad32", pic, o);
  CHECK_EQ(r.status, Status::error);
  CHECK(r.message.find("isn't imported") != std::string::npos);
  r = set_cover("tt", junk, o);
  CHECK_EQ(r.status, Status::source_invalid);
  CHECK(r.message.find("Windows can't read this picture") != std::string::npos);
  CHECK_EQ(set_cover("tt", w.dir / L"nowhere.png", o).status, Status::source_invalid);
  {
    HANDLE held = CreateFileW((root / L"win" / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    CHECK_EQ(set_cover("tt", pic, o).status, Status::error);
    CHECK_EQ(clear_cover("tt", o).status, Status::error);
    CHECK_EQ(refresh_covers({"tt"}, o).at(0).status, Status::error);
    CloseHandle(held);
  }
  CHECK(!fs::exists(covers / L"user.png"));
  // Set, set again (nothing changes), clear, clear again (nothing to clear).
  std::string cat0 = test::read_text(catalog);
  r = set_cover("tt", pic, o);
  CHECK_EQ(r.status, Status::ok);
  CHECK(r.changed);
  CHECK(r.info.origin == CoverOrigin::user);
  CHECK(r.info.has_user);
  CHECK(r.info.original == CoverOrigin::disc);
  CHECK_EQ(r.info.label, std::string("Your own picture"));
  CHECK_EQ(r.info.width, 820);
  CHECK(test::read_text(catalog) != cat0);
  check_catalog_cover(root, "tt", "user");
  CHECK_EQ(json_at(catalog).at("packages").as_list().at(0)->at("cover").get_string("tileMd5"), r.info.tile_md5);
  r = set_cover("tt", pic, o);
  CHECK_EQ(r.status, Status::ok);
  CHECK(!r.changed);
  r = clear_cover("tt", o);
  CHECK_EQ(r.status, Status::ok);
  CHECK(r.changed);
  CHECK(r.info.origin == CoverOrigin::disc);
  CHECK(!fs::exists(covers / L"user.png"));
  check_catalog_cover(root, "tt", "disc");
  std::string cat1 = test::read_text(catalog);
  r = clear_cover("tt", o);
  CHECK_EQ(r.status, Status::ok);
  CHECK(!r.changed);
  CHECK(r.message.find("nothing to clear") != std::string::npos);
  CHECK_EQ(test::read_text(catalog), cat1);  // no catalog write
  CHECK_EQ(clear_cover("ad32", o).status, Status::error);

  // refresh: a download that fails keeps the disc cover (4); once the server
  // answers, it replaces it; after that nothing is fetched.
  std::vector<CoverResult> rs = refresh_covers({}, o);
  CHECK_EQ(rs.size(), size_t(1));
  CHECK_EQ(rs.at(0).status, Status::network);
  CHECK(!rs.at(0).changed);
  CHECK(rs.at(0).info.origin == CoverOrigin::disc);
  srv.serve("/cmd.png", w.box_png);
  rs = refresh_covers({"tt"}, o);
  CHECK_EQ(rs.at(0).status, Status::ok);
  CHECK(rs.at(0).changed);
  CHECK(rs.at(0).info.origin == CoverOrigin::download);
  CHECK(!rs.at(0).info.can_download);
  check_catalog_cover(root, "tt", "download");
  srv.clear();
  rs = refresh_covers({"tt"}, o);
  CHECK_EQ(rs.at(0).status, Status::ok);
  CHECK(!rs.at(0).changed);
  CHECK(srv.requests().empty());
  CHECK_EQ(refresh_covers({"nosuch"}, o).at(0).status, Status::error);
  CHECK_EQ(refresh_covers({"ad32"}, o).at(0).status, Status::error);
  // --force: every download tried again; nothing works, the cover stays.
  srv.forget("/cmd.png");
  fs::remove(dl / L"covers" / L"cmd.png");
  CoverOptions forced = o;
  forced.force = true;
  rs = refresh_covers({"tt"}, forced);
  CHECK_EQ(rs.at(0).status, Status::network);
  CHECK(rs.at(0).info.origin == CoverOrigin::download);
  check_catalog_cover(root, "tt", "download");
  srv.serve("/cmd.png", w.box_png);
  srv.clear();
  rs = refresh_covers({"tt"}, forced);
  CHECK_EQ(rs.at(0).status, Status::ok);
  CHECK(asked(srv).count("/cmd.png"));
  // A damaged original.png: refresh fetches it again.
  test::write_bytes(covers / L"original.png", {'x'});
  CHECK(cover_info("tt", root, reg.span()).origin == CoverOrigin::generated);
  rs = refresh_covers({"tt"}, o);
  CHECK_EQ(rs.at(0).status, Status::ok);
  CHECK_EQ(md5_file(covers / L"original.png"), cover_json(root, "tt").at("original").get_string("md5"));
  check_catalog_cover(root, "tt", "download");
  // A missing tile, and a stale renderer: the next catalog write renders it.
  fs::remove(covers / L"tile.png");
  CHECK_EQ(regenerate_catalog(root, {}, reg.span()).status, Status::ok);
  check_catalog_cover(root, "tt", "download");
  {
    std::string cj = test::read_text(covers / L"cover.json");
    std::string from = "\"renderer\": " + std::to_string(cover::kRendererVersion);
    size_t at = cj.find(from);
    CHECK(at != std::string::npos);
    cj.replace(at, from.size(), "\"renderer\": 0");
    test::write_bytes(covers / L"cover.json", std::vector<uint8_t>(cj.begin(), cj.end()));
    g_log.clear();
    CHECK_EQ(regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span()).status, Status::ok);
    CHECK(logged("rendered the cover tile of"));
    CHECK_EQ(cover_json(root, "tt").at("tile").get_int("renderer"), int64_t(cover::kRendererVersion));
    check_catalog_cover(root, "tt", "download");
  }
  // A damaged cover.json lists as generated (logged) until refresh repairs it.
  test::write_bytes(covers / L"cover.json", {'{', 'x'});
  g_log.clear();
  CHECK_EQ(regenerate_catalog(root, [](const std::string& s) { g_log.push_back(s); }, reg.span()).status, Status::ok);
  CHECK(logged("cover.json is damaged"));
  check_catalog_cover(root, "tt", "generated");
  rs = refresh_covers({"tt"}, o);
  CHECK_EQ(rs.at(0).status, Status::ok);
  CHECK(rs.at(0).changed);
  check_catalog_cover(root, "tt", "download");
  CHECK(no_cover_leftovers(root));
  // A cover.json that is there but cannot be read (here: held open with no
  // sharing) is not "no cover": cover_info says why, and the release still
  // reads as installed.
  {
    CHECK(cover_info("tt", root, reg.span()).error.empty());
    HANDLE held = CreateFileW((covers / L"cover.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    CoverInfo info = cover_info("tt", root, reg.span());
    CloseHandle(held);
    fprintf(stderr, "  unreadable cover.json -> %s\n", info.error.c_str());
    CHECK(info.installed);
    CHECK(info.origin == CoverOrigin::generated);
    CHECK(info.error.find("cover.json") != std::string::npos);
    CHECK(cover_info("tt", root, reg.span()).error.empty());
    // A damaged one says so too.
    const std::string good = test::read_text(covers / L"cover.json");
    test::write_bytes(covers / L"cover.json", {'{', 'x'});
    info = cover_info("tt", root, reg.span());
    CHECK(info.installed);
    CHECK(info.error.find("damaged") != std::string::npos);
    test::write_bytes(covers / L"cover.json", std::vector<uint8_t>(good.begin(), good.end()));
    CHECK(cover_info("tt", root, reg.span()).error.empty());
  }
  // Numbers out of range in a hand-edited cover.json (1e300, a crop past
  // 2^31) are read as absent, never converted with undefined results.
  {
    auto j = parse_json("{\"a\": 1e300, \"b\": -1e300, \"c\": 3000000000, \"d\": 42.9, \"e\": -7, "
                        "\"f\": 9223372036854775807, \"g\": \"12\", \"h\": 9.3e18}");
    CHECK(j.has_value());
    if (j) {
      CHECK_EQ(j->integer("a", -1), int64_t(-1));
      CHECK_EQ(j->integer("b", -1), int64_t(-1));
      CHECK_EQ(j->integer("c"), int64_t(3000000000));
      CHECK_EQ(j->int_in("c", -1), -1);  // beyond int
      CHECK_EQ(j->int_in("d"), 42);
      CHECK_EQ(j->int_in("e"), -7);
      CHECK_EQ(j->integer("f", -1), int64_t(-1));  // 2^63 as a double: out of range
      CHECK_EQ(j->int_in("g", 5), 5);             // not a number
      CHECK_EQ(j->integer("h", -1), int64_t(-1));
      CHECK_EQ(j->int_in("absent", 3), 3);
    }
    const std::string good = test::read_text(covers / L"cover.json");
    std::string cj = good;
    size_t at = cj.find("\"width\": ");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) cj.replace(at, strlen("\"width\": "), "\"width\": 1e300, \"w0\": ");
    test::write_bytes(covers / L"cover.json", std::vector<uint8_t>(cj.begin(), cj.end()));
    CoverInfo info = cover_info("tt", root, reg.span());
    CHECK(info.width >= 0);  // read as absent (0) when the original shows, never a wrapped value
    rs = refresh_covers({"tt"}, o);
    CHECK(rs.at(0).status == Status::ok || rs.at(0).status == Status::network);
    test::write_bytes(covers / L"cover.json", std::vector<uint8_t>(good.begin(), good.end()));
    rs = refresh_covers({"tt"}, o);
    CHECK_EQ(rs.at(0).status, Status::ok);
  }

  // ---- through adimport.exe (the built-in registry; nothing downloads) ----
  auto cli = [&](const std::vector<std::wstring>& args, const char* what, std::string* out = nullptr) {
    test::ProcessResult p = test::run_process(exe, args, 120000);
    fprintf(stderr, "[%s] exit %d\n%s", what, p.exit_code, p.output.c_str());
    if (out) *out = p.output;
    return p.exit_code;
  };
  fs::path eroot = w.dir / L"exe";
  std::string out;
  CHECK_EQ(cli({L"--no-cover-download", L"--from", w.src_tt.wstring(), L"--package", L"tt", L"--no-verify", L"--dest",
                eroot.wstring(), L"--quiet"},
               "import tt"),
           0);
  CHECK_EQ(cli({L"--list-packages", L"--dest", eroot.wstring()}, "--list-packages", &out), 0);
  CHECK(out.find("; cover: generated") != std::string::npos);
  CHECK_EQ(cli({L"--set-cover", L"tt", pic.wstring(), L"--dest", eroot.wstring()}, "--set-cover", &out), 0);
  CHECK(out.find("your own picture") != std::string::npos);
  CHECK_EQ(cli({L"--list-packages", L"--dest", eroot.wstring()}, "--list-packages after", &out), 0);
  CHECK(out.find("; cover: user (Your own picture)") != std::string::npos);
  CHECK_EQ(cli({L"--set-cover", L"tt", junk.wstring(), L"--dest", eroot.wstring()}, "--set-cover junk"), 2);
  CHECK_EQ(cli({L"--set-cover", L"ad32", pic.wstring(), L"--dest", eroot.wstring()}, "--set-cover not imported", &out),
           1);
  CHECK(out.find("isn't imported") != std::string::npos);
  CHECK_EQ(cli({L"--set-cover", L"nosuch", pic.wstring(), L"--dest", eroot.wstring()}, "--set-cover unknown", &out), 1);
  CHECK(out.find("(see --list-packages)") != std::string::npos);
  {
    HANDLE held = CreateFileW((eroot / L"win" / L"import.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    CHECK_EQ(cli({L"--set-cover", L"tt", pic.wstring(), L"--dest", eroot.wstring()}, "--set-cover, lock held"), 1);
    CloseHandle(held);
  }
  CHECK_EQ(cli({L"--clear-cover", L"tt", L"--dest", eroot.wstring()}, "--clear-cover"), 0);
  CHECK_EQ(cli({L"--clear-cover", L"tt", L"--dest", eroot.wstring()}, "--clear-cover again", &out), 0);
  CHECK(out.find("nothing to clear") != std::string::npos);
  CHECK_EQ(cli({L"--clear-cover", L"ad32", L"--dest", eroot.wstring()}, "--clear-cover not imported"), 1);
  // Refresh with the downloads switched off in the environment: repairs only.
  SetEnvironmentVariableW(L"AD_COVER_DOWNLOAD", L"0");
  CHECK_EQ(cli({L"--refresh-covers", L"--dest", eroot.wstring(), L"--download-dir", (w.dir / L"exe-dl").wstring()},
               "--refresh-covers (downloads off)", &out),
           0);
  CHECK(out.find("downloads are off") != std::string::npos);
  CHECK_EQ(cli({L"--refresh-covers", L"ad32", L"--dest", eroot.wstring()}, "--refresh-covers not imported"), 1);
  CHECK_EQ(cli({L"--refresh-covers", L"nosuch", L"--dest", eroot.wstring()}, "--refresh-covers unknown"), 1);
  SetEnvironmentVariableW(L"AD_COVER_DOWNLOAD", nullptr);
  CHECK(!fs::exists(w.dir / L"exe-dl"));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_import_covers <adimport.exe> [scratch]\n");
    return 2;
  }
  std::wstring exe = fs::absolute(argv[1]).wstring();
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  World w;
  w.dir = test::scratch(argc - 1, argv + 1, "adw-import-covers");
  test::sandbox_data_root(w.dir / L"localappdata");  // no default may reach the real data folder
  fprintf(stderr, "-- the tile rules\n");
  test_rules();
  fprintf(stderr, "-- decoding\n");
  test_decoding(w.dir);

  // Totally Twisted with a synthetic installer splash, and three pictures to download.
  w.tt = test::tt_fixture();
  w.splash_bmp = cover::encode_picture(checker(200, 120, 10, kYellow, Rgb{200, 0, 200}), "bmp");
  w.tt.source["INSTALL/SETUP.BMP"] = w.splash_bmp;
  w.src_tt = w.dir / L"src-tt";
  test::write_tree(w.src_tt, w.tt.source);
  w.box_png = cover::encode_picture(gradient(400, 500, 1), "png");
  w.box2_jpg = cover::encode_picture(gradient(300, 600, 2), "jpeg");
  test::Server srv(std::vector<uint8_t>{});
  srv.serve("/box.png", w.box_png);
  srv.serve("/box2.jpg", w.box2_jpg);
  w.srv = &srv;
  fprintf(stderr, "-- downloads\n");
  test_downloads(w);
  fprintf(stderr, "-- imports\n");
  test_imports(w);
  fprintf(stderr, "-- the cover commands\n");
  test_commands(w, exe);
  CoUninitialize();
  return test::finish("import.covers");
}
