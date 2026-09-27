// GDI (and USER's palette pair, SelectPalette/RealizePalette = USER.282/283)
// on real GDI over 8-bit key surfaces (gdi16.hh; API_SURFACE.md §2 "GDI",
// 97 imports).
//
// Rules every shim follows:
//   * a guest COLORREF reaches real GDI only as Gdi16::key(hdc, c), and a DC
//     is sync()ed (brush/pen re-made for its palette, text colours keyed)
//     before real GDI draws with it;
//   * every Win16 surface holds hardware palette indices (Win16 has no DIB
//     sections), so blits between them are plain real BitBlt/StretchBlt;
//   * device-independent bits (SetDIBitsToDevice, StretchDIBits, SetDIBits,
//     CreateDIBitmap) are translated to hardware indices first — through the
//     DC's palette, as a Win95 palette device matched them — and handed to
//     real GDI as an 8bpp DIB with the key table, so real GDI still does the
//     geometry (clipping, mirroring, stretching, ROPs);
//   * colours read back (GetPixel, GetDIBits, GetNearestColor) are converted
//     from indices to what they mean.
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// needs more; API_SURFACE.md §2 GDI):
//   * EnumFonts calls nothing back (MESSAGE3 only).
//   * GetDIBits writes 8- and 24-bit rows from 8-bit bitmaps and defers mono
//     bitmaps to real GDI; 4-bit requests are refused. With DIB_PAL_COLORS it
//     writes an identity colour table and hardware indices, which Win95
//     would have expressed as the DC's logical palette indices: the same
//     whenever that palette is realized in order from index 10 (ADXPL310's
//     identity palettes), not otherwise.
//   * Mapping modes other than MM_TEXT are passed to real GDI untested
//     (WMORPH only).
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "adw/core/log.h"
#include "win16/gdi16.hh"
#include "win16/shim_families16.hh"

namespace adw::win16 {

using win32::Display;
using win32::LogicalPalette;

namespace {

constexpr const char* G = "GDI";

Gdi16& gt(Runtime16& rt) { return rt.state<Gdi16>(); }
Gdi16& gt(Call16& c) { return c.rt.state<Gdi16>(); }

DWORD pack_point(POINT p) { return (uint32_t(uint16_t(p.y)) << 16) | uint16_t(p.x); }
DWORD pack_size(SIZE s) { return (uint32_t(uint16_t(s.cy)) << 16) | uint16_t(s.cx); }

// ---- guest DIBs ------------------------------------------------------------------------------------

struct Dib16 {
  BITMAPINFOHEADER h{};
  std::vector<RGBQUAD> colors;  // DIB_RGB_COLORS
  std::vector<uint16_t> pal;    // DIB_PAL_COLORS
  uint32_t stride = 0;
  int w = 0, height = 0;  // |biHeight|
  bool top_down = false;
};

bool read_dib(Runtime16& rt, uint32_t bmi, uint16_t usage, Dib16& d) {
  if (!bmi) return false;
  uint32_t size = rt.rd32(bmi);
  uint32_t ncolors = 0;
  uint32_t table;
  bool core = size == sizeof(BITMAPCOREHEADER);
  if (core) {
    BITMAPCOREHEADER ch = read16<BITMAPCOREHEADER>(rt, bmi);
    d.h.biSize = sizeof(BITMAPINFOHEADER);
    d.h.biWidth = ch.bcWidth;
    d.h.biHeight = ch.bcHeight;
    d.h.biPlanes = 1;
    d.h.biBitCount = ch.bcBitCount;
    d.h.biCompression = BI_RGB;
    ncolors = ch.bcBitCount <= 8 ? 1u << ch.bcBitCount : 0;
    table = bmi + size;
  } else if (size >= sizeof(BITMAPINFOHEADER) && size < 0x1000) {
    d.h = read16<BITMAPINFOHEADER>(rt, bmi);
    ncolors = d.h.biBitCount <= 8 && d.h.biBitCount ? (d.h.biClrUsed ? std::min<uint32_t>(d.h.biClrUsed, 256)
                                                                     : 1u << d.h.biBitCount)
                                                    : 0;
    table = bmi + size;
  } else {
    return false;
  }
  for (uint32_t i = 0; i < ncolors; i++) {
    if (usage == DIB_PAL_COLORS) {
      d.pal.push_back(rt.rd16(table + 2 * i));
    } else if (core) {
      uint32_t a = table + 3 * i;
      d.colors.push_back(RGBQUAD{rt.rd8(a), rt.rd8(a + 1), rt.rd8(a + 2), 0});
    } else {
      uint32_t v = rt.rd32(table + 4 * i);
      d.colors.push_back(RGBQUAD{BYTE(v), BYTE(v >> 8), BYTE(v >> 16), 0});
    }
  }
  d.w = d.h.biWidth;
  d.height = d.h.biHeight < 0 ? -d.h.biHeight : d.h.biHeight;
  d.top_down = d.h.biHeight < 0;
  d.stride = uint32_t(((d.w * d.h.biBitCount + 31) / 32) * 4);
  return d.w > 0 && d.height > 0 && d.w <= 16384 && d.height <= 16384;
}

// Colour index of a DIB → hardware index when drawn through `hdc`.
std::array<uint8_t, 256> dib_xlate(Gdi16& g, const Dib16& d, uint16_t usage, uint16_t hdc) {
  std::array<uint8_t, 256> x{};
  Display& disp = g.display();
  LogicalPalette* pal = g.dc_palette(hdc);
  size_t n = usage == DIB_PAL_COLORS ? d.pal.size() : d.colors.size();
  for (size_t i = 0; i < 256; i++) {
    if (i >= n) {
      x[i] = 0;
      continue;
    }
    if (usage == DIB_PAL_COLORS) {
      x[i] = uint8_t(disp.map_index(0x01000000 | d.pal[i], pal));
    } else {
      const RGBQUAD& q = d.colors[i];
      x[i] = uint8_t(disp.device_index_for_rgb(RGB(q.rgbRed, q.rgbGreen, q.rgbBlue), pal));
    }
  }
  return x;
}

// Expands RLE8/RLE4 bits into rows (bottom-up order, as the DIB stores them).
std::vector<uint8_t> decode_rle(const uint8_t* src, size_t src_size, const Dib16& d) {
  bool rle4 = d.h.biCompression == BI_RLE4;
  uint32_t stride = uint32_t((d.w + 3) & ~3);  // decoded into 8 bits per pixel
  std::vector<uint8_t> out(size_t(stride) * d.height, 0);
  int x = 0, y = 0;
  size_t p = 0;
  auto put = [&](int v) {
    if (x < d.w && y < d.height) out[size_t(y) * stride + size_t(x)] = uint8_t(v);
    x++;
  };
  while (p + 1 < src_size && y < d.height) {
    uint8_t n = src[p], v = src[p + 1];
    p += 2;
    if (n) {
      for (int i = 0; i < n; i++) put(rle4 ? ((i & 1) ? (v & 0xF) : (v >> 4)) : v);
    } else if (v == 0) {
      x = 0;
      y++;
    } else if (v == 1) {
      break;
    } else if (v == 2) {
      if (p + 1 >= src_size) break;
      x += src[p];
      y += src[p + 1];
      p += 2;
    } else {
      for (int i = 0; i < v && p < src_size; i++) {
        uint8_t b = src[p + (rle4 ? size_t(i / 2) : size_t(i))];
        put(rle4 ? ((i & 1) ? (b & 0xF) : (b >> 4)) : b);
      }
      size_t used = rle4 ? (size_t(v) + 1) / 2 : v;
      p += (used + 1) & ~size_t(1);
    }
  }
  return out;
}

// `lines` rows of guest DIB bits → 8bpp hardware indices, same orientation,
// DWORD rows. Deep (16/24/32-bit) pixels are matched through the DC's palette.
// Only rows [row_lo, row_hi) (in memory order) are translated — a blit of a
// small rectangle out of a full-screen DIB (ADXPL300's dirty rectangles)
// needs no more; the others stay 0.
std::vector<uint8_t> dib_to_indices(Runtime16& rt, Gdi16& g, const Dib16& d, uint32_t bits, int lines,
                                    uint16_t usage, uint16_t hdc, int row_lo = 0, int row_hi = 0x7FFFFFFF) {
  uint32_t out_stride = uint32_t((d.w + 3) & ~3);
  std::vector<uint8_t> out(size_t(out_stride) * std::max(lines, 0), 0);
  if (lines <= 0 || !bits) return out;
  auto x = dib_xlate(g, d, usage, hdc);
  if (d.h.biCompression == BI_RLE8 || d.h.biCompression == BI_RLE4) {
    uint32_t n = d.h.biSizeImage ? d.h.biSizeImage : 0x10000;
    // The RLE stream: at most biSizeImage bytes, but never past its block.
    uint32_t lin = rt.linear(bits, 1);
    uint32_t avail = rt.ldt().limit_of(uint16_t(bits >> 16)) - (bits & 0xFFFF) + 1;
    const uint8_t* src = rt.mem().at<uint8_t>(lin, std::min(n, avail));
    std::vector<uint8_t> flat = decode_rle(src, std::min(n, avail), d);
    for (int y = 0; y < lines && y < d.height; y++) {
      for (int i = 0; i < d.w; i++) out[size_t(y) * out_stride + size_t(i)] = x[flat[size_t(y) * out_stride + size_t(i)]];
    }
    return out;
  }
  uint32_t lin = rt.linear(bits, d.stride * uint32_t(lines));
  const uint8_t* src = rt.mem().at<uint8_t>(lin, size_t(d.stride) * lines);
  Display& disp = g.display();
  LogicalPalette* pal = g.dc_palette(hdc);
  std::unordered_map<uint32_t, uint8_t> cache;
  auto deep = [&](uint8_t r, uint8_t gg, uint8_t b) {
    uint32_t k = RGB(r, gg, b);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    uint8_t v = uint8_t(disp.device_index_for_rgb(k, pal));
    cache[k] = v;
    return v;
  };
  for (int y = std::max(row_lo, 0); y < std::min(lines, row_hi); y++) {
    const uint8_t* s = src + size_t(y) * d.stride;
    uint8_t* o = out.data() + size_t(y) * out_stride;
    switch (d.h.biBitCount) {
      case 1:
        for (int i = 0; i < d.w; i++) o[i] = x[(s[i >> 3] >> (7 - (i & 7))) & 1];
        break;
      case 4:
        for (int i = 0; i < d.w; i++) o[i] = x[(s[i >> 1] >> ((i & 1) ? 0 : 4)) & 0xF];
        break;
      case 8:
        for (int i = 0; i < d.w; i++) o[i] = x[s[i]];
        break;
      case 16:
        for (int i = 0; i < d.w; i++) {
          uint16_t v = uint16_t(s[2 * i] | (s[2 * i + 1] << 8));
          o[i] = deep(uint8_t(((v >> 10) & 31) << 3), uint8_t(((v >> 5) & 31) << 3), uint8_t((v & 31) << 3));
        }
        break;
      case 24:
        for (int i = 0; i < d.w; i++) o[i] = deep(s[3 * i + 2], s[3 * i + 1], s[3 * i]);
        break;
      case 32:
        for (int i = 0; i < d.w; i++) o[i] = deep(s[4 * i + 2], s[4 * i + 1], s[4 * i]);
        break;
      default:
        break;
    }
  }
  return out;
}

// The BITMAPINFO real GDI gets for translated bits: 8bpp, the key table.
struct KeyBmi {
  BITMAPINFOHEADER h;
  RGBQUAD c[256];
};
KeyBmi key_bmi(const Dib16& d) {
  KeyBmi b{};
  b.h.biSize = sizeof(BITMAPINFOHEADER);
  b.h.biWidth = d.w;
  b.h.biHeight = d.top_down ? -d.height : d.height;
  b.h.biPlanes = 1;
  b.h.biBitCount = 8;
  b.h.biCompression = BI_RGB;
  b.h.biClrUsed = 256;
  memcpy(b.c, Display::key_table().data(), sizeof(b.c));
  return b;
}

// Writes translated DIB rows into an 8-bit device bitmap (SetDIBits, CreateDIBitmap).
int set_bitmap_rows(Runtime16& rt, Gdi16& g, Obj16& bmp, uint16_t hdc, uint16_t start, uint16_t lines, uint32_t bits,
                    const Dib16& d, uint16_t usage) {
  if (bmp.bmp.bpp == 1 || !bmp.bmp.bits) {
    // A monochrome target: hand real GDI the DIB with real colours.
    std::vector<uint8_t> raw(size_t(d.stride) * lines);
    rt.read_bytes(bits, raw.data(), raw.size());
    struct {
      BITMAPINFOHEADER h;
      RGBQUAD c[256];
    } bi{};
    bi.h = d.h;
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    for (size_t i = 0; i < d.colors.size() && i < 256; i++) bi.c[i] = d.colors[i];
    for (size_t i = 0; i < d.pal.size() && i < 256; i++) {
      COLORREF c = g.index_rgb(g.display().map_index(0x01000000 | d.pal[i], g.dc_palette(hdc)));
      bi.c[i] = RGBQUAD{GetBValue(c), GetGValue(c), GetRValue(c), 0};
    }
    HDC screen = GetDC(nullptr);
    int r = SetDIBits(screen, static_cast<HBITMAP>(bmp.host), start, lines, raw.data(),
                      reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return r;
  }
  GdiFlush();
  std::vector<uint8_t> idx = dib_to_indices(rt, g, d, bits, lines, usage, hdc);
  uint32_t istride = uint32_t((d.w + 3) & ~3);
  int w = std::min(d.w, bmp.bmp.w);
  for (int i = 0; i < lines; i++) {
    int scan = start + i;  // counted from the bottom for a bottom-up DIB
    int y = d.top_down ? scan : d.height - 1 - scan;
    // A DIB taller or shorter than the bitmap lines up at the bottom (bottom-up) / top (top-down).
    if (!d.top_down) y -= d.height - bmp.bmp.h;
    if (y < 0 || y >= bmp.bmp.h) continue;
    memcpy(bmp.bmp.bits + size_t(y) * bmp.bmp.stride, idx.data() + size_t(i) * istride, size_t(w));
  }
  return lines;
}

// A blit ROP that reads the source / the pattern.
bool rop_uses_src(DWORD rop) { return ((rop >> 2) ^ rop) & 0x330000; }
bool rop_uses_pat(DWORD rop) { return ((rop >> 4) ^ rop) & 0x0F0000; }

// Reads an array of POINT16s.
std::vector<POINT> read_points(Runtime16& rt, uint32_t fp, int n) {
  std::vector<POINT> pts;
  for (int i = 0; i < n && i < 8192; i++) {
    POINT16 p = read16<POINT16>(rt, fp + 4u * uint32_t(i));
    pts.push_back(POINT{p.x, p.y});
  }
  return pts;
}

LOGFONTA to_logfont(const LOGFONT16& f) {
  LOGFONTA lf{};
  lf.lfHeight = f.lfHeight;
  lf.lfWidth = f.lfWidth;
  lf.lfEscapement = f.lfEscapement;
  lf.lfOrientation = f.lfOrientation;
  lf.lfWeight = f.lfWeight;
  lf.lfItalic = f.lfItalic;
  lf.lfUnderline = f.lfUnderline;
  lf.lfStrikeOut = f.lfStrikeOut;
  lf.lfCharSet = f.lfCharSet;
  lf.lfOutPrecision = f.lfOutPrecision;
  lf.lfClipPrecision = f.lfClipPrecision;
  // Anti-aliased text would put colours between the key colours into an
  // 8-bit surface — i.e. arbitrary palette indices. 1996 GDI never smoothed.
  lf.lfQuality = NONANTIALIASED_QUALITY;
  lf.lfPitchAndFamily = f.lfPitchAndFamily;
  memcpy(lf.lfFaceName, f.lfFaceName, 31);
  lf.lfFaceName[31] = 0;
  return lf;
}

uint16_t make_font(Gdi16& g, const LOGFONTA& lf) {
  HFONT f = CreateFontIndirectA(&lf);
  if (!f) return 0;
  Obj16 o;
  o.type = G16::font;
  o.host = f;
  o.font = lf;
  return g.add(o);
}

HRGN region_of(Gdi16& g, uint16_t h) { return static_cast<HRGN>(g.get(h, G16::region) ? g.host(h) : nullptr); }

}  // namespace

uint16_t gdi16_bitmap_from_dib(Runtime16& rt, uint16_t hdc, uint32_t packed) {
  Gdi16& g = gt(rt);
  Dib16 dib;
  if (!read_dib(rt, packed, DIB_RGB_COLORS, dib)) return 0;
  uint32_t hsize = rt.rd32(packed);
  size_t ncol = dib.colors.size();
  bool core = hsize == sizeof(BITMAPCOREHEADER);
  uint32_t bits = packed + hsize + uint32_t(ncol * (core ? 3 : 4));
  if (dib.h.biCompression == BI_BITFIELDS && !core) bits += 12;
  uint16_t hb = g.create_device_bitmap(dib.w, dib.height, dib.h.biBitCount == 1 && ncol == 2 ? 1 : 8);
  Obj16* o = g.get(hb, G16::bitmap);
  if (o) set_bitmap_rows(rt, g, *o, hdc, 0, uint16_t(dib.height), bits, dib, DIB_RGB_COLORS);
  return hb;
}

// ---- registration -----------------------------------------------------------------------------------

void register_gdi16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- DCs ----
  r.impl(G, "CreateCompatibleDC", [](Call16& c) {
    c.w();
    c.ret(gt(c).create_memory_dc());
  });
  r.impl(G, "CreateDC", [](Call16& c) {
    std::string driver = c.rt.read_str(c.ptr());
    c.ptr();
    c.ptr();
    c.ptr();
    c.ret(ieq16(driver, "DISPLAY") ? gt(c).create_screen_dc(0) : 0);
  });
  r.impl(G, "CreateIC", [](Call16& c) {
    std::string driver = c.rt.read_str(c.ptr());
    c.ptr();
    c.ptr();
    c.ptr();
    c.ret(ieq16(driver, "DISPLAY") ? gt(c).create_ic() : 0);
  });
  r.impl(G, "DeleteDC", [](Call16& c) { c.ret_bool(gt(c).destroy(c.w())); });
  r.impl(G, "SaveDC", [](Call16& c) {
    uint16_t h = c.w();
    Dc16* d = gt(c).dc(h);
    if (!d) return c.ret(0);
    int n = SaveDC(gt(c).host_dc(h));
    d->saved.push_back(d->s);
    c.ret(uint16_t(n));
  });
  r.impl(G, "RestoreDC", [](Call16& c) {
    uint16_t h = c.w();
    int16_t level = c.sw();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(h);
    if (!d || d->saved.empty()) return c.ret(0);
    int n = int(d->saved.size());
    int target = level < 0 ? n + level : level - 1;  // index of the state to restore
    if (target < 0 || target >= n) return c.ret(0);
    BOOL ok = RestoreDC(g.host_dc(h), level);
    Dc16State st = d->saved[size_t(target)];
    d->saved.resize(size_t(target));
    // What real RestoreDC re-selected are the real objects of that moment;
    // re-derive them from the guest state.
    uint16_t bmp = st.bitmap;
    // The bitmap selected until now leaves the DC (unless it is the one
    // coming back), so it can be deleted or selected elsewhere again.
    if (d->s.bitmap != bmp) {
      if (Obj16* cur = g.get(d->s.bitmap, G16::bitmap)) {
        if (cur->bmp.selected_in == h) cur->bmp.selected_in = 0;
      }
    }
    d->s = st;
    if (Obj16* b = g.get(bmp, G16::bitmap)) {
      if (!b->stock) b->bmp.selected_in = h;
    }
    g.sync(h);
    c.ret_bool(ok);
  });
  r.impl(G, "GetDeviceCaps", [](Call16& c) {
    c.w();
    int16_t idx = c.sw();
    c.ret(uint16_t(gt(c).display().device_caps(idx)));
  });

  // ---- objects ----
  r.impl(G, "GetStockObject", [](Call16& c) { c.ret(gt(c).stock(c.w())); });
  r.impl(G, "SelectObject", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w();
    c.ret(gt(c).select(hdc, h));
  });
  r.impl(G, "DeleteObject", [](Call16& c) { c.ret_bool(gt(c).destroy(c.w())); });
  r.impl(G, "UnrealizeObject", [](Call16& c) {
    Obj16* o = gt(c).get(c.w());
    if (o && o->type == G16::palette && !o->stock) o->pal->realized = false;
    c.ret_bool(o != nullptr);
  });
  r.impl(G, "IsGDIObject", [](Call16& c) {
    Obj16* o = gt(c).get(c.w());
    static const uint16_t kType[] = {0, 7, 5, 2, 1, 3, 6, 4};  // Win16 OBJ_* numbering
    c.ret(o ? kType[size_t(o->type)] : 0);
  });
  r.impl(G, "GetObject", [](Call16& c) {
    uint16_t h = c.w();
    int16_t size = c.sw();
    uint32_t buf = c.ptr();
    Obj16* o = gt(c).get(h);
    if (!o) return c.ret(0);
    auto put = [&](const void* data, size_t n) {
      size_t m = size < 0 ? 0 : std::min(n, size_t(size));
      if (buf && m) c.rt.write_bytes(buf, data, m);
      c.ret(uint16_t(buf ? m : n));
    };
    switch (o->type) {
      case G16::bitmap: {
        BITMAP16 b{0, int16_t(o->bmp.w), int16_t(o->bmp.h),
                   int16_t(o->bmp.bpp == 1 ? ((o->bmp.w + 15) / 16) * 2 : (o->bmp.w + 1) & ~1), 1,
                   uint8_t(o->bmp.bpp), 0};
        return put(&b, sizeof(b));
      }
      case G16::pen: {
        LOGPEN16 p{uint16_t(o->style), POINT16{int16_t(o->width), 0}, o->color};
        return put(&p, sizeof(p));
      }
      case G16::brush: {
        LOGBRUSH16 b{uint16_t(o->style), o->color, int16_t(o->hatch)};
        return put(&b, sizeof(b));
      }
      case G16::font: {
        LOGFONT16 f{};
        f.lfHeight = int16_t(o->font.lfHeight);
        f.lfWidth = int16_t(o->font.lfWidth);
        f.lfEscapement = int16_t(o->font.lfEscapement);
        f.lfOrientation = int16_t(o->font.lfOrientation);
        f.lfWeight = int16_t(o->font.lfWeight);
        f.lfItalic = o->font.lfItalic;
        f.lfUnderline = o->font.lfUnderline;
        f.lfStrikeOut = o->font.lfStrikeOut;
        f.lfCharSet = o->font.lfCharSet;
        f.lfOutPrecision = o->font.lfOutPrecision;
        f.lfClipPrecision = o->font.lfClipPrecision;
        f.lfQuality = o->font.lfQuality;
        f.lfPitchAndFamily = o->font.lfPitchAndFamily;
        memcpy(f.lfFaceName, o->font.lfFaceName, 32);
        return put(&f, sizeof(f));
      }
      case G16::palette: {
        uint16_t n = uint16_t(o->pal->entries.size());
        return put(&n, 2);
      }
      default:
        return c.ret(0);
    }
  });
  r.impl(G, "CreateSolidBrush", [](Call16& c) { c.ret(gt(c).create_brush(c.l())); });
  // CreateBrushIndirect(LOGBRUSH FAR*): BS_SOLID, BS_NULL, BS_HATCHED,
  // BS_PATTERN (lbHatch = a bitmap) and BS_DIBPATTERN (lbHatch = a global
  // packed DIB; lbColor's low word DIB_RGB_COLORS/DIB_PAL_COLORS). GUTS makes
  // its brushes this way.
  r.impl(G, "CreateBrushIndirect", [](Call16& c) {
    LOGBRUSH16 lb = read16<LOGBRUSH16>(c.rt, c.ptr());
    Gdi16& g = gt(c);
    switch (lb.lbStyle) {
      case BS_SOLID:
        return c.ret(g.create_brush(lb.lbColor));
      case BS_NULL:
        return c.ret(g.create_brush(0, BS_NULL));
      case BS_HATCHED:
        return c.ret(g.create_brush(lb.lbColor, BS_HATCHED, lb.lbHatch));
      case BS_PATTERN:
      case BS_DIBPATTERN: {
        uint16_t bmp = uint16_t(lb.lbHatch);
        if (lb.lbStyle == BS_DIBPATTERN) {
          uint32_t dib = c.rt.global().lock(uint16_t(lb.lbHatch));
          bmp = dib ? gdi16_bitmap_from_dib(c.rt, 0, dib) : 0;
          if (dib) c.rt.global().unlock(uint16_t(lb.lbHatch));
        }
        if (!g.get(bmp, G16::bitmap)) return c.ret(0);
        uint16_t h = g.create_brush(0, BS_PATTERN);
        if (Obj16* o = g.get(h, G16::brush)) o->pattern = bmp;
        return c.ret(h);
      }
      default:
        return c.ret(g.create_brush(lb.lbColor));
    }
  });
  // GetDCOrg(hdc): DX:AX = the DC's origin on the screen — (0, 0) for the
  // full-screen saver window's DC and memory DCs; a child window's corner for
  // GetDC(child).
  r.impl(G, "GetDCOrg", [](Call16& c) {
    Dc16* d = gt(c).dc(c.w());
    c.ret32(d ? (uint32_t(uint16_t(d->org_y)) << 16) | uint16_t(d->org_x) : 0);
  });
  r.impl(G, "CreatePen", [](Call16& c) {
    int16_t style = c.sw(), width = c.sw();
    uint32_t color = c.l();
    c.ret(gt(c).create_pen(style, std::max<int>(width, 0), color));
  });
  r.impl(G, "CreateFontIndirect", [](Call16& c) {
    LOGFONT16 f = read16<LOGFONT16>(c.rt, c.ptr());
    c.ret(make_font(gt(c), to_logfont(f)));
  });
  r.impl(G, "CreateFont", [](Call16& c) {
    LOGFONT16 f{};
    f.lfHeight = c.sw();
    f.lfWidth = c.sw();
    f.lfEscapement = c.sw();
    f.lfOrientation = c.sw();
    f.lfWeight = c.sw();
    f.lfItalic = uint8_t(c.w());
    f.lfUnderline = uint8_t(c.w());
    f.lfStrikeOut = uint8_t(c.w());
    f.lfCharSet = uint8_t(c.w());
    f.lfOutPrecision = uint8_t(c.w());
    f.lfClipPrecision = uint8_t(c.w());
    f.lfQuality = uint8_t(c.w());
    f.lfPitchAndFamily = uint8_t(c.w());
    std::string face = c.rt.read_str(c.ptr(), 31);
    memcpy(f.lfFaceName, face.c_str(), face.size() + 1);
    c.ret(make_font(gt(c), to_logfont(f)));
  });
  r.impl(G, "CreateBitmap", [](Call16& c) {
    int16_t w = c.sw(), h = c.sw();
    uint16_t planes = c.w(), bpp = c.w();
    uint32_t bits = c.ptr();
    Gdi16& g = gt(c);
    int depth = planes * bpp == 1 ? 1 : 8;
    uint16_t hb = g.create_device_bitmap(w, h, depth);
    Obj16* o = g.get(hb, G16::bitmap);
    if (o && bits) {
      // Device-dependent bits: WORD-aligned rows, top-down.
      uint32_t src_stride = uint32_t(((std::max<int>(w, 1) * planes * bpp + 15) / 16) * 2);
      std::vector<uint8_t> raw(size_t(src_stride) * std::max<int>(h, 1));
      c.rt.read_bytes(bits, raw.data(), raw.size());
      if (depth == 1) {
        SetBitmapBits(static_cast<HBITMAP>(o->host), DWORD(raw.size()), raw.data());
      } else if (bpp == 8) {
        for (int y = 0; y < o->bmp.h; y++) memcpy(o->bmp.bits + size_t(y) * o->bmp.stride, raw.data() + size_t(y) * src_stride, size_t(o->bmp.w));
      }
    }
    c.ret(hb);
  });
  r.impl(G, "CreateCompatibleBitmap", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t w = c.sw(), h = c.sw();
    Gdi16& g = gt(c);
    // Compatible with a memory DC that still holds its 1x1 monochrome bitmap
    // means monochrome — the classic Windows gotcha, kept.
    Bitmap16* s = g.dc_surface(hdc);
    int bpp = s && s->bpp == 1 ? 1 : 8;
    c.ret(g.create_device_bitmap(w, h, bpp));
  });
  r.impl(G, "GetBitmapBits", [](Call16& c) {
    uint16_t hb = c.w();
    uint32_t count = c.l();
    uint32_t buf = c.ptr();
    Obj16* o = gt(c).get(hb, G16::bitmap);
    if (!o || !buf) return c.ret32(0);
    GdiFlush();
    uint32_t stride = o->bmp.bpp == 1 ? o->bmp.stride : uint32_t((o->bmp.w + 1) & ~1);
    std::vector<uint8_t> out(size_t(stride) * o->bmp.h, 0);
    if (o->bmp.bpp == 1) {
      GetBitmapBits(static_cast<HBITMAP>(o->host), LONG(out.size()), out.data());
    } else {
      for (int y = 0; y < o->bmp.h; y++) memcpy(out.data() + size_t(y) * stride, o->bmp.bits + size_t(y) * o->bmp.stride, size_t(o->bmp.w));
    }
    uint32_t n = std::min<uint32_t>(count, uint32_t(out.size()));
    for (uint32_t done = 0, dst = buf; done < n;) {
      uint32_t chunk = std::min(n - done, 0x10000 - (dst & 0xFFFF));
      c.rt.write_bytes(dst, out.data() + done, chunk);
      done += chunk;
      dst = Runtime16::huge_add(dst, chunk);
    }
    c.ret32(n);
  });
  r.impl(G, "SetBitmapBits", [](Call16& c) {
    uint16_t hb = c.w();
    uint32_t count = c.l();
    uint32_t buf = c.ptr();
    Obj16* o = gt(c).get(hb, G16::bitmap);
    if (!o || !buf) return c.ret32(0);
    GdiFlush();
    uint32_t stride = o->bmp.bpp == 1 ? o->bmp.stride : uint32_t((o->bmp.w + 1) & ~1);
    std::vector<uint8_t> in(size_t(stride) * o->bmp.h, 0);
    uint32_t n = std::min<uint32_t>(count, uint32_t(in.size()));
    for (uint32_t done = 0, src = buf; done < n;) {
      uint32_t chunk = std::min(n - done, 0x10000 - (src & 0xFFFF));
      c.rt.read_bytes(src, in.data() + done, chunk);
      done += chunk;
      src = Runtime16::huge_add(src, chunk);
    }
    if (o->bmp.bpp == 1) {
      SetBitmapBits(static_cast<HBITMAP>(o->host), DWORD(n), in.data());
    } else {
      for (int y = 0; y < o->bmp.h; y++) memcpy(o->bmp.bits + size_t(y) * o->bmp.stride, in.data() + size_t(y) * stride, size_t(o->bmp.w));
    }
    c.ret32(n);
  });

  // ---- attributes ----
  r.impl(G, "SetBkColor", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t col = c.l();
    Dc16* d = gt(c).dc(hdc);
    if (!d) return c.ret32(0x80000000);
    COLORREF old = d->s.bk;
    d->s.bk = col;
    if (HDC h = gt(c).host_dc(hdc)) SetBkColor(h, gt(c).key(hdc, col));
    c.ret32(old);
  });
  r.impl(G, "SetTextColor", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t col = c.l();
    Dc16* d = gt(c).dc(hdc);
    if (!d) return c.ret32(0x80000000);
    COLORREF old = d->s.text;
    d->s.text = col;
    if (HDC h = gt(c).host_dc(hdc)) SetTextColor(h, gt(c).key(hdc, col));
    c.ret32(old);
  });
  r.impl(G, "GetBkColor", [](Call16& c) {
    Dc16* d = gt(c).dc(c.w());
    c.ret32(d ? d->s.bk : 0);
  });
  r.impl(G, "GetTextColor", [](Call16& c) {
    Dc16* d = gt(c).dc(c.w());
    c.ret32(d ? d->s.text : 0);
  });
  auto passthrough_mode = [&](const char* name, int (*set)(HDC, int)) {
    r.impl(G, name, [set](Call16& c) {
      uint16_t hdc = c.w();
      int16_t v = c.sw();
      HDC h = gt(c).host_dc(hdc);
      c.ret(h ? uint16_t(set(h, v)) : 0);
    });
  };
  passthrough_mode("SetBkMode", [](HDC h, int v) { return ::SetBkMode(h, v); });
  passthrough_mode("SetROP2", [](HDC h, int v) { return ::SetROP2(h, v); });
  passthrough_mode("SetPolyFillMode", [](HDC h, int v) { return ::SetPolyFillMode(h, v); });
  passthrough_mode("SetStretchBltMode", [](HDC h, int v) { return ::SetStretchBltMode(h, v); });
  passthrough_mode("SetMapMode", [](HDC h, int v) { return ::SetMapMode(h, v); });
  r.impl(G, "SetTextAlign", [](Call16& c) {
    uint16_t hdc = c.w(), v = c.w();
    HDC h = gt(c).host_dc(hdc);
    c.ret(h ? uint16_t(::SetTextAlign(h, v)) : 0);
  });
  r.impl(G, "GetBkMode", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    c.ret(h ? uint16_t(::GetBkMode(h)) : 0);
  });
  r.impl(G, "GetPolyFillMode", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    c.ret(h ? uint16_t(::GetPolyFillMode(h)) : 0);
  });
  auto origin = [&](const char* name, BOOL (*fn)(HDC, int, int, POINT*)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t x = c.sw(), y = c.sw();
      HDC h = gt(c).host_dc(hdc);
      POINT old{0, 0};
      if (h) fn(h, x, y, &old);
      c.ret32(pack_point(old));
    });
  };
  origin("SetViewportOrg", [](HDC h, int x, int y, POINT* p) { return ::SetViewportOrgEx(h, x, y, p); });
  origin("SetWindowOrg", [](HDC h, int x, int y, POINT* p) { return ::SetWindowOrgEx(h, x, y, p); });
  origin("OffsetViewportOrg", [](HDC h, int x, int y, POINT* p) { return ::OffsetViewportOrgEx(h, x, y, p); });
  origin("SetBrushOrg", [](HDC h, int x, int y, POINT* p) { return ::SetBrushOrgEx(h, x, y, p); });
  auto extent = [&](const char* name, BOOL (*fn)(HDC, int, int, SIZE*)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t x = c.sw(), y = c.sw();
      HDC h = gt(c).host_dc(hdc);
      SIZE old{0, 0};
      if (h) fn(h, x, y, &old);
      c.ret32(pack_size(old));
    });
  };
  extent("SetWindowExt", [](HDC h, int x, int y, SIZE* s) { return ::SetWindowExtEx(h, x, y, s); });
  extent("SetViewportExt", [](HDC h, int x, int y, SIZE* s) { return ::SetViewportExtEx(h, x, y, s); });
  auto scale = [&](const char* name, BOOL (*fn)(HDC, int, int, int, int, SIZE*)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
      HDC h = gt(c).host_dc(hdc);
      SIZE old{0, 0};
      if (h) fn(h, a, b, d, e, &old);
      c.ret32(pack_size(old));
    });
  };
  scale("ScaleWindowExt", [](HDC h, int a, int b, int c, int d, SIZE* s) { return ::ScaleWindowExtEx(h, a, b, c, d, s); });
  scale("ScaleViewportExt",
        [](HDC h, int a, int b, int c, int d, SIZE* s) { return ::ScaleViewportExtEx(h, a, b, c, d, s); });
  r.impl(G, "GetWindowOrg", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    POINT p{0, 0};
    if (h) GetWindowOrgEx(h, &p);
    c.ret32(pack_point(p));
  });
  r.impl(G, "GetCurrentPosition", [](Call16& c) {
    HDC h = gt(c).host_dc(c.w());
    POINT p{0, 0};
    if (h) GetCurrentPositionEx(h, &p);
    c.ret32(pack_point(p));
  });

  // ---- drawing ----
  r.impl(G, "MoveTo", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    HDC h = gt(c).host_dc(hdc);
    POINT old{0, 0};
    if (h) MoveToEx(h, x, y, &old);
    c.ret32(pack_point(old));
  });
  r.impl(G, "LineTo", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    gt(c).sync(hdc);
    c.ret_bool(::LineTo(h, x, y));
  });
  auto shape4 = [&](const char* name, BOOL (*fn)(HDC, int, int, int, int)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
      HDC h = gt(c).host_dc(hdc);
      if (!h) return c.ret(0);
      gt(c).sync(hdc);
      c.ret_bool(fn(h, a, b, d, e));
    });
  };
  shape4("Rectangle", [](HDC h, int a, int b, int c, int d) { return ::Rectangle(h, a, b, c, d); });
  shape4("Ellipse", [](HDC h, int a, int b, int c, int d) { return ::Ellipse(h, a, b, c, d); });
  r.impl(G, "RoundRect", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t v[6];
    for (auto& x : v) x = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    gt(c).sync(hdc);
    c.ret_bool(::RoundRect(h, v[0], v[1], v[2], v[3], v[4], v[5]));
  });
  auto shape8 = [&](const char* name, BOOL (*fn)(HDC, int, int, int, int, int, int, int, int)) {
    r.impl(G, name, [fn](Call16& c) {
      uint16_t hdc = c.w();
      int16_t v[8];
      for (auto& x : v) x = c.sw();
      HDC h = gt(c).host_dc(hdc);
      if (!h) return c.ret(0);
      gt(c).sync(hdc);
      c.ret_bool(fn(h, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]));
    });
  };
  shape8("Arc", [](HDC h, int a, int b, int c, int d, int e, int f, int g, int i) { return ::Arc(h, a, b, c, d, e, f, g, i); });
  shape8("Pie", [](HDC h, int a, int b, int c, int d, int e, int f, int g, int i) { return ::Pie(h, a, b, c, d, e, f, g, i); });
  r.impl(G, "Polygon", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t pts = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h || n <= 0) return c.ret(0);
    auto p = read_points(c.rt, pts, n);
    gt(c).sync(hdc);
    c.ret_bool(::Polygon(h, p.data(), int(p.size())));
  });
  r.impl(G, "Polyline", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t pts = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h || n <= 0) return c.ret(0);
    auto p = read_points(c.rt, pts, n);
    gt(c).sync(hdc);
    c.ret_bool(::Polyline(h, p.data(), int(p.size())));
  });
  r.impl(G, "SetPixel", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint32_t col = c.l();
    Gdi16& g = gt(c);
    HDC h = g.host_dc(hdc);
    if (!h) return c.ret32(0xFFFFFFFF);
    COLORREF k = g.key(hdc, col);
    COLORREF got = ::SetPixel(h, x, y, k);
    if (got == CLR_INVALID) return c.ret32(0xFFFFFFFF);
    Bitmap16* s = g.dc_surface(hdc);
    c.ret32(s && s->bpp == 1 ? got : g.index_rgb(GetRValue(got)));
  });
  r.impl(G, "GetPixel", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    Gdi16& g = gt(c);
    HDC h = g.host_dc(hdc);
    if (!h) return c.ret32(0xFFFFFFFF);
    GdiFlush();
    COLORREF got = ::GetPixel(h, x, y);
    if (got == CLR_INVALID) return c.ret32(0xFFFFFFFF);
    Bitmap16* s = g.dc_surface(hdc);
    c.ret32(s && s->bpp == 1 ? got : g.index_rgb(GetRValue(got)));
  });
  r.impl(G, "PatBlt", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint32_t rop = c.l();
    HDC d = gt(c).host_dc(hdc);
    if (!d) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    gt(c).sync(hdc);
    c.ret_bool(::PatBlt(d, x, y, w, h, rop));
  });
  r.impl(G, "BitBlt", [](Call16& c) {
    uint16_t dst = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t src = c.w();
    int16_t sx = c.sw(), sy = c.sw();
    uint32_t rop = c.l();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(dst);
    if (!d) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    g.sync(dst);
    if (!rop_uses_src(rop) || !src) return c.ret_bool(::PatBlt(d, x, y, w, h, rop));
    HDC s = g.host_dc(src);
    if (!s) return c.ret(0);
    g.sync(src);
    c.ret_bool(::BitBlt(d, x, y, w, h, s, sx, sy, rop));
  });
  r.impl(G, "StretchBlt", [](Call16& c) {
    uint16_t dst = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw();
    uint16_t src = c.w();
    int16_t sx = c.sw(), sy = c.sw(), sw = c.sw(), sh = c.sw();
    uint32_t rop = c.l();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(dst);
    if (!d) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    g.sync(dst);
    if (!rop_uses_src(rop) || !src) return c.ret_bool(::PatBlt(d, x, y, w, h, rop));
    HDC s = g.host_dc(src);
    if (!s) return c.ret(0);
    g.sync(src);
    c.ret_bool(::StretchBlt(d, x, y, w, h, s, sx, sy, sw, sh, rop));
  });

  // ---- text ----
  r.impl(G, "TextOut", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint32_t s = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    std::string text = n > 0 ? c.rt.read_str(s, size_t(n)) : std::string();
    text.resize(std::max<int>(n, 0), '\0');
    gt(c).sync(hdc);
    c.ret_bool(::TextOutA(h, x, y, text.data(), int(text.size())));
  });
  r.impl(G, "ExtTextOut", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    uint16_t opts = c.w();
    uint32_t rp = c.ptr(), s = c.ptr();
    uint16_t n = c.w();
    uint32_t dx = c.ptr();
    HDC h = gt(c).host_dc(hdc);
    if (!h) return c.ret(0);
    std::string text = c.rt.read_str(s, n);
    text.resize(n, '\0');
    RECT rc{};
    if (rp) rc = to_rect(read16<RECT16>(c.rt, rp));
    std::vector<INT> widths;
    if (dx) {
      for (uint16_t i = 0; i < n; i++) widths.push_back(int16_t(c.rt.rd16(dx + 2u * i)));
    }
    gt(c).sync(hdc);
    c.ret_bool(::ExtTextOutA(h, x, y, opts, rp ? &rc : nullptr, text.data(), n, dx ? widths.data() : nullptr));
  });
  r.impl(G, "GetTextExtent", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t s = c.ptr();
    int16_t n = c.sw();
    HDC h = gt(c).host_dc(hdc);
    SIZE sz{0, 0};
    if (h && n > 0) {
      std::string text = c.rt.read_str(s, size_t(n));
      text.resize(size_t(n), '\0');
      GetTextExtentPoint32A(h, text.data(), n, &sz);
    }
    c.ret32(pack_size(sz));
  });
  r.impl(G, "GetTextMetrics", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t out = c.ptr();
    HDC h = gt(c).host_dc(hdc);
    TEXTMETRICA tm{};
    if (!h || !GetTextMetricsA(h, &tm)) return c.ret(0);
    TEXTMETRIC16 t{int16_t(tm.tmHeight), int16_t(tm.tmAscent), int16_t(tm.tmDescent),
                   int16_t(tm.tmInternalLeading), int16_t(tm.tmExternalLeading), int16_t(tm.tmAveCharWidth),
                   int16_t(tm.tmMaxCharWidth), int16_t(tm.tmWeight), tm.tmItalic, tm.tmUnderlined, tm.tmStruckOut,
                   uint8_t(tm.tmFirstChar), uint8_t(tm.tmLastChar), uint8_t(tm.tmDefaultChar),
                   uint8_t(tm.tmBreakChar), tm.tmPitchAndFamily, tm.tmCharSet, int16_t(tm.tmOverhang),
                   int16_t(tm.tmDigitizedAspectX), int16_t(tm.tmDigitizedAspectY)};
    write16(c.rt, out, t);
    c.ret(1);
  });
  // No font enumeration: the callback is never called (a known gap, above).
  r.impl(G, "EnumFonts", [](Call16& c) { c.ret(1); });

  // ---- regions ----
  r.impl(G, "CreateRectRgn", [](Call16& c) {
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    c.ret(gt(c).wrap_region(CreateRectRgn(a, b, d, e)));
  });
  r.impl(G, "CreateRectRgnIndirect", [](Call16& c) {
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    c.ret(gt(c).wrap_region(CreateRectRgnIndirect(&rc)));
  });
  r.impl(G, "CreateEllipticRgn", [](Call16& c) {
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    c.ret(gt(c).wrap_region(CreateEllipticRgn(a, b, d, e)));
  });
  r.impl(G, "CreateEllipticRgnIndirect", [](Call16& c) {
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    c.ret(gt(c).wrap_region(CreateEllipticRgnIndirect(&rc)));
  });
  r.impl(G, "CreateRoundRectRgn", [](Call16& c) {
    int16_t v[6];
    for (auto& x : v) x = c.sw();
    c.ret(gt(c).wrap_region(CreateRoundRectRgn(v[0], v[1], v[2], v[3], v[4], v[5])));
  });
  r.impl(G, "CreatePolygonRgn", [](Call16& c) {
    uint32_t pts = c.ptr();
    int16_t n = c.sw(), mode = c.sw();
    auto p = read_points(c.rt, pts, n);
    c.ret(p.empty() ? 0 : gt(c).wrap_region(CreatePolygonRgn(p.data(), int(p.size()), mode)));
  });
  r.impl(G, "CombineRgn", [](Call16& c) {
    uint16_t d = c.w(), s1 = c.w(), s2 = c.w();
    int16_t mode = c.sw();
    Gdi16& g = gt(c);
    HRGN rd = region_of(g, d), r1 = region_of(g, s1), r2 = region_of(g, s2);
    c.ret(rd && r1 ? uint16_t(::CombineRgn(rd, r1, r2 ? r2 : r1, mode)) : 0);  // ERROR
  });
  r.impl(G, "SetRectRgn", [](Call16& c) {
    uint16_t h = c.w();
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    if (HRGN rg = region_of(gt(c), h)) SetRectRgn(rg, a, b, d, e);
  });
  r.impl(G, "OffsetRgn", [](Call16& c) {
    uint16_t h = c.w();
    int16_t x = c.sw(), y = c.sw();
    HRGN rg = region_of(gt(c), h);
    c.ret(rg ? uint16_t(::OffsetRgn(rg, x, y)) : 0);
  });
  r.impl(G, "GetRgnBox", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t out = c.ptr();
    HRGN rg = region_of(gt(c), h);
    RECT rc{};
    int t = rg ? ::GetRgnBox(rg, &rc) : 0;
    write16(c.rt, out, to_rect16(rc));
    c.ret(uint16_t(t));
  });
  r.impl(G, "PtInRegion", [](Call16& c) {
    uint16_t h = c.w();
    int16_t x = c.sw(), y = c.sw();
    HRGN rg = region_of(gt(c), h);
    c.ret_bool(rg && ::PtInRegion(rg, x, y));
  });
  r.impl(G, "RectInRegionOld", [](Call16& c) {
    uint16_t h = c.w();
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    HRGN rg = region_of(gt(c), h);
    c.ret_bool(rg && ::RectInRegion(rg, &rc));
  });
  r.impl(G, "EqualRgn", [](Call16& c) {
    HRGN a = region_of(gt(c), c.w()), b = region_of(gt(c), c.w());
    c.ret_bool(a && b && ::EqualRgn(a, b));
  });
  r.impl(G, "SelectClipRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w();
    HDC d = gt(c).host_dc(hdc);
    c.ret(d ? uint16_t(::SelectClipRgn(d, region_of(gt(c), h))) : 0);
  });
  r.impl(G, "ExcludeClipRect", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t a = c.sw(), b = c.sw(), d = c.sw(), e = c.sw();
    HDC h = gt(c).host_dc(hdc);
    c.ret(h ? uint16_t(::ExcludeClipRect(h, a, b, d, e)) : 0);
  });
  r.impl(G, "GetClipBox", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t out = c.ptr();
    HDC h = gt(c).host_dc(hdc);
    RECT rc{};
    int t = h ? ::GetClipBox(h, &rc) : 0;
    write16(c.rt, out, to_rect16(rc));
    c.ret(uint16_t(t));
  });
  r.impl(G, "PtVisible", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw();
    HDC h = gt(c).host_dc(hdc);
    c.ret_bool(h && ::PtVisible(h, x, y));
  });
  r.impl(G, "RectVisibleOld", [](Call16& c) {
    uint16_t hdc = c.w();
    RECT rc = to_rect(read16<RECT16>(c.rt, c.ptr()));
    HDC h = gt(c).host_dc(hdc);
    c.ret_bool(h && ::RectVisible(h, &rc));
  });
  r.impl(G, "FillRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w(), hbr = c.w();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    HRGN rg = region_of(g, h);
    if (!d || !rg) return c.ret(0);
    uint16_t saved = g.dc(hdc)->s.brush;
    g.sync_brush(hdc, hbr);
    BOOL ok = ::PaintRgn(d, rg);
    g.sync_brush(hdc, saved);
    c.ret_bool(ok);
  });
  r.impl(G, "FrameRgn", [](Call16& c) {
    uint16_t hdc = c.w(), h = c.w(), hbr = c.w();
    int16_t w = c.sw(), hh = c.sw();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    HRGN rg = region_of(g, h);
    Obj16* b = g.get(hbr, G16::brush);
    if (!d || !rg || !b) return c.ret(0);
    // A temporary real brush in the DC's key colour.
    HBRUSH real = CreateSolidBrush(g.key(hdc, b->color));
    BOOL ok = ::FrameRgn(d, rg, real, w, hh);
    DeleteObject(real);
    c.ret_bool(ok);
  });

  // ---- palettes ----
  r.impl(G, "CreatePalette", [](Call16& c) {
    uint32_t lp = c.ptr();
    uint16_t n = c.rt.rd16(lp + 2);
    std::vector<PALETTEENTRY> e(std::min<uint16_t>(n, 1024));
    if (!e.empty()) c.rt.read_bytes(lp + 4, e.data(), e.size() * sizeof(PALETTEENTRY));
    c.ret(gt(c).create_palette(e));
  });
  r.impl(G, "GetPaletteEntries", [](Call16& c) {
    uint16_t h = c.w(), start = c.w(), n = c.w();
    uint32_t out = c.ptr();
    Obj16* o = gt(c).get(h, G16::palette);
    if (!o) return c.ret(0);
    auto& e = o->pal->entries;
    if (!out) return c.ret(uint16_t(e.size()));
    uint16_t k = 0;
    for (; k < n && size_t(start) + k < e.size(); k++) write16(c.rt, out + 4u * k, e[start + k]);
    c.ret(k);
  });
  r.impl(G, "SetPaletteEntries", [](Call16& c) {
    uint16_t h = c.w(), start = c.w(), n = c.w();
    uint32_t in = c.ptr();
    Obj16* o = gt(c).get(h, G16::palette);
    if (!o || o->stock) return c.ret(0);
    auto& e = o->pal->entries;
    uint16_t k = 0;
    for (; k < n && size_t(start) + k < e.size(); k++) e[start + k] = read16<PALETTEENTRY>(c.rt, in + 4u * k);
    c.ret(k);
  });
  r.impl(G, "AnimatePalette", [](Call16& c) {
    uint16_t h = c.w(), start = c.w(), n = c.w();
    uint32_t in = c.ptr();
    Obj16* o = gt(c).get(h, G16::palette);
    if (!o || o->stock || !n) return;
    std::vector<PALETTEENTRY> e(n);
    c.rt.read_bytes(in, e.data(), e.size() * sizeof(PALETTEENTRY));
    gt(c).display().animate(*o->pal, start, n, e.data());
  });
  r.impl(G, "GetNearestPaletteIndex", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t col = c.l();
    Obj16* o = gt(c).get(h, G16::palette);
    c.ret(o && !o->pal->entries.empty() ? uint16_t(Display::nearest_in(*o->pal, col & 0xFFFFFF)) : 0);
  });
  r.impl(G, "GetSystemPaletteEntries", [](Call16& c) {
    c.w();
    uint16_t start = c.w(), n = c.w();
    uint32_t out = c.ptr();
    auto& sys = gt(c).display().system_palette();
    if (!out) return c.ret(256);
    uint16_t k = 0;
    for (; k < n && start + k < 256; k++) write16(c.rt, out + 4u * k, sys[size_t(start + k)]);
    c.ret(k);
  });
  r.impl(G, "SetSystemPaletteUse", [](Call16& c) {
    c.w();
    c.ret(uint16_t(gt(c).display().set_palette_use(c.w())));
  });
  r.impl(G, "GetNearestColor", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t col = c.l();
    Gdi16& g = gt(c);
    c.ret32(g.index_rgb(g.display().map_index(col, g.dc_palette(hdc))));
  });
  // USER.282/283 in Win16.
  r.impl("USER", "SelectPalette", [](Call16& c) {
    uint16_t hdc = c.w(), hpal = c.w(), force = c.w();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(hdc);
    if (!d || !g.get(hpal, G16::palette)) return c.ret(0);
    uint16_t old = d->s.palette;
    d->s.palette = hpal;
    d->s.force_background = force != 0;
    g.sync(hdc);  // PALETTEINDEX colours now map through the new palette
    c.ret(old);
  });
  r.impl("USER", "RealizePalette", [](Call16& c) {
    uint16_t hdc = c.w();
    Gdi16& g = gt(c);
    Dc16* d = g.dc(hdc);
    if (!d) return c.ret(0);
    Obj16* p = g.get(d->s.palette, G16::palette);
    if (!p || p->stock) return c.ret(0);
    // A screen DC realizes into the hardware palette (foreground, as the
    // active saver window did); a memory DC maps onto it as it stands.
    bool background = !d->screen || d->s.force_background;
    UINT n = g.display().realize(*p->pal, background);
    g.sync(hdc);
    c.ret(uint16_t(n));
  });

  // ---- device-independent bitmaps ----
  r.impl(G, "SetDIBitsToDevice", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw(), cx = c.sw(), cy = c.sw(), xs = c.sw(), ys = c.sw();
    uint16_t start = c.w(), lines = c.w();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    Dib16 dib;
    if (!d || !read_dib(c.rt, bmi, usage, dib)) return c.ret(0);
    int n = std::min<int>(lines, dib.height);
    c.rt.charge_pixels(int64_t(std::max<int>(cx, 0)) * std::max<int>(cy, 0));
    std::vector<uint8_t> idx = dib_to_indices(c.rt, g, dib, bits, n, usage, hdc);
    KeyBmi kb = key_bmi(dib);
    g.sync(hdc);
    int r = ::SetDIBitsToDevice(d, x, y, DWORD(std::max<int>(cx, 0)), DWORD(std::max<int>(cy, 0)), xs, ys, start,
                                UINT(n), idx.data(), reinterpret_cast<BITMAPINFO*>(&kb), DIB_RGB_COLORS);
    c.ret(uint16_t(r));
  });
  r.impl(G, "StretchDIBits", [](Call16& c) {
    uint16_t hdc = c.w();
    int16_t x = c.sw(), y = c.sw(), w = c.sw(), h = c.sw(), xs = c.sw(), ys = c.sw(), ws = c.sw(), hs = c.sw();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    uint32_t rop = c.l();
    Gdi16& g = gt(c);
    HDC d = g.host_dc(hdc);
    Dib16 dib;
    if (!d || !read_dib(c.rt, bmi, usage, dib)) return c.ret(0);
    // StretchDIBits' source origin is the DIB's first row in memory order
    // either way (bottom-up: y from the bottom).
    int lo = std::min<int>(ys, ys + hs), hi = std::max<int>(ys, ys + hs);
    c.rt.charge_pixels(int64_t(std::abs(int(w))) * std::abs(int(h)));
    std::vector<uint8_t> idx = dib_to_indices(c.rt, g, dib, bits, dib.height, usage, hdc, lo - 1, hi + 1);
    if (tracing("dib16") && dib.h.biBitCount == 8 && dib.h.biCompression == BI_RGB) {
      // What the source rectangle holds, and what it becomes.
      std::map<int, int> src_hist, dst_hist;
      uint32_t ostride = uint32_t((dib.w + 3) & ~3);
      for (int yy = std::max(lo, 0); yy < std::min(hi, dib.height); yy++) {
        for (int xx = std::max<int>(xs, 0); xx < std::min<int>(xs + ws, dib.w); xx++) {
          src_hist[c.rt.rd8(Runtime16::huge_add(bits, uint32_t(yy) * dib.stride + uint32_t(xx)))]++;
          dst_hist[idx[size_t(yy) * ostride + size_t(xx)]]++;
        }
      }
      std::string s, t;
      for (auto& [k, v] : src_hist) s += " " + std::to_string(k) + ":" + std::to_string(v);
      for (auto& [k, v] : dst_hist) t += " " + std::to_string(k) + ":" + std::to_string(v);
      std::string p;
      for (size_t i = 0; i < dib.pal.size() && i < 24; i++) p += " " + std::to_string(dib.pal[i]);
      LogicalPalette* lp = g.dc_palette(hdc);
      trace("dib16", "StretchDIBits usage %u: source%s -> hardware%s; table%s; DC palette %04X of %zu entries", usage,
            s.c_str(), t.c_str(), p.c_str(), g.dc(hdc) ? g.dc(hdc)->s.palette : 0, lp ? lp->entries.size() : 0);
    }
    KeyBmi kb = key_bmi(dib);
    g.sync(hdc);
    int r = ::StretchDIBits(d, x, y, w, h, xs, ys, ws, hs, idx.data(), reinterpret_cast<BITMAPINFO*>(&kb),
                            DIB_RGB_COLORS, rop);
    c.ret(uint16_t(r));
  });
  r.impl(G, "SetDIBits", [](Call16& c) {
    uint16_t hdc = c.w(), hb = c.w(), start = c.w(), lines = c.w();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    Obj16* o = g.get(hb, G16::bitmap);
    Dib16 dib;
    if (!o || o->stock || !read_dib(c.rt, bmi, usage, dib)) return c.ret(0);
    c.rt.charge_pixels(int64_t(dib.w) * lines);
    c.ret(uint16_t(set_bitmap_rows(c.rt, g, *o, hdc, start, lines, bits, dib, usage)));
  });
  r.impl(G, "CreateDIBitmap", [](Call16& c) {
    uint16_t hdc = c.w();
    uint32_t hdr = c.ptr();
    uint32_t init = c.l();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    BITMAPINFOHEADER h = read16<BITMAPINFOHEADER>(c.rt, hdr);
    int w = h.biSize == sizeof(BITMAPCOREHEADER) ? int(c.rt.rd16(hdr + 4)) : int(h.biWidth);
    int ht = h.biSize == sizeof(BITMAPCOREHEADER) ? int(int16_t(c.rt.rd16(hdr + 6))) : int(h.biHeight);
    uint16_t hb = g.create_device_bitmap(w, std::abs(ht), 8);
    Obj16* o = g.get(hb, G16::bitmap);
    Dib16 dib;
    if (o && (init & 4 /*CBM_INIT*/) && bits && read_dib(c.rt, bmi, usage, dib)) {
      set_bitmap_rows(c.rt, g, *o, hdc, 0, uint16_t(dib.height), bits, dib, usage);
    }
    c.ret(hb);
  });
  r.impl(G, "GetDIBits", [](Call16& c) {
    c.w();  // hdc: the bitmap holds hardware indices, whatever the DC
    uint16_t hb = c.w(), start = c.w(), lines = c.w();
    uint32_t bits = c.ptr(), bmi = c.ptr();
    uint16_t usage = c.w();
    Gdi16& g = gt(c);
    Obj16* o = g.get(hb, G16::bitmap);
    if (!o || !bmi) return c.ret(0);
    GdiFlush();
    uint32_t size = c.rt.rd32(bmi);
    if (size < sizeof(BITMAPINFOHEADER)) return c.ret(0);
    BITMAPINFOHEADER h = read16<BITMAPINFOHEADER>(c.rt, bmi);
    if (!h.biBitCount) {
      // Just describe the bitmap.
      h.biWidth = o->bmp.w;
      h.biHeight = o->bmp.h;
      h.biPlanes = 1;
      h.biBitCount = uint16_t(o->bmp.bpp);
      h.biCompression = BI_RGB;
      h.biSizeImage = uint32_t(((o->bmp.w * o->bmp.bpp + 31) / 32) * 4 * o->bmp.h);
      write16(c.rt, bmi, h);
      return c.ret(1);
    }
    int bpp = h.biBitCount;
    if (o->bmp.bpp == 1 || (bpp != 8 && bpp != 24)) {
      if (o->bmp.bpp != 1) return c.ret(0);
      // Monochrome: real GDI's own answer.
      struct {
        BITMAPINFOHEADER h;
        RGBQUAD c[2];
      } bi{};
      bi.h = h;
      uint32_t rows = std::min<uint32_t>(lines, uint32_t(o->bmp.h));
      uint32_t stride = uint32_t(((o->bmp.w + 31) / 32) * 4);
      std::vector<uint8_t> tmp(size_t(stride) * rows);
      HDC screen = GetDC(nullptr);
      int got = ::GetDIBits(screen, static_cast<HBITMAP>(o->host), start, rows, bits ? tmp.data() : nullptr,
                            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
      ReleaseDC(nullptr, screen);
      if (bits && got > 0) c.rt.write_bytes(bits, tmp.data(), size_t(stride) * uint32_t(got));
      write16(c.rt, bmi, bi.h);
      for (int i = 0; i < 2; i++) c.rt.write_bytes(bmi + bi.h.biSize + 4u * i, &bi.c[i], 4);
      return c.ret(uint16_t(std::max(got, 0)));
    }
    int w = o->bmp.w, ht = o->bmp.h;
    bool top_down = h.biHeight < 0;
    uint32_t stride = uint32_t(((w * bpp + 31) / 32) * 4);
    // The colour table: what each hardware index means.
    if (bpp == 8) {
      for (int i = 0; i < 256; i++) {
        uint32_t a = bmi + h.biSize + (usage == DIB_PAL_COLORS ? 2u : 4u) * uint32_t(i);
        if (usage == DIB_PAL_COLORS) {
          c.rt.wr16(a, uint16_t(i));
        } else {
          COLORREF col = g.index_rgb(i);
          c.rt.wr32(a, uint32_t(GetBValue(col)) | (uint32_t(GetGValue(col)) << 8) | (uint32_t(GetRValue(col)) << 16));
        }
      }
    }
    h.biWidth = w;
    h.biHeight = top_down ? -ht : ht;
    h.biCompression = BI_RGB;
    h.biSizeImage = stride * uint32_t(ht);
    h.biClrUsed = bpp == 8 ? 256 : 0;
    write16(c.rt, bmi, h);
    if (!bits) return c.ret(uint16_t(ht));
    int n = 0;
    std::vector<uint8_t> row(stride);
    for (int i = 0; i < lines; i++) {
      int scan = start + i;
      if (scan >= ht) break;
      int y = top_down ? scan : ht - 1 - scan;
      const uint8_t* src = o->bmp.bits + size_t(y) * o->bmp.stride;
      std::fill(row.begin(), row.end(), 0);
      for (int x = 0; x < w; x++) {
        if (bpp == 8) {
          row[size_t(x)] = src[x];
        } else {
          COLORREF col = g.index_rgb(src[x]);
          row[3 * size_t(x)] = GetBValue(col);
          row[3 * size_t(x) + 1] = GetGValue(col);
          row[3 * size_t(x) + 2] = GetRValue(col);
        }
      }
      c.rt.write_bytes(Runtime16::huge_add(bits, uint32_t(i) * stride), row.data(), stride);
      n++;
    }
    c.ret(uint16_t(n));
  });

  // ---- printing and escapes: no printer ----
  r.impl(G, "Escape", [](Call16& c) { c.ret32(0); });
  r.impl(G, "StartDoc", [](Call16& c) { c.ret(0xFFFF); });
}

}  // namespace adw::win16
