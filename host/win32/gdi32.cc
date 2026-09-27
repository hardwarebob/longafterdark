// GDI32.DLL — drawing on real GDI over 8-bit surfaces, with the palette and
// colour semantics of a Win95 256-colour display emulated (API_SURFACE.md §1
// "GDI32.DLL", 77 functions; display.hh and gdi_objects.hh explain the model).
//
// Rules every shim here follows:
//   * a guest COLORREF reaches real GDI only as GdiTable::key_color(hdc, c);
//   * a brush or pen is re-made for the DC's palette before drawing with it
//     (sync_brush/sync_pen), since PALETTEINDEX colours map per palette;
//   * a blit between surfaces whose indices mean different colours goes
//     through a translation (blit_translated); between surfaces of the same
//     meaning it is a plain real BitBlt/StretchBlt, which copies indices
//     exactly because all 8-bit surfaces share the key table;
//   * guest-visible colours (GetPixel, GetDIBits colour tables) are converted
//     back from key colours to what the index means.
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// reaches them: their census runs with 0 unimplemented API calls and no
// "not colour-translated" log line; API_SURFACE.md §1 GDI32):
//   * a DIB section deeper than 8 bpp (CreateDIBSection at 16/24/32 bpp)
//     blitted onto an 8-bit surface goes to real GDI untranslated (logged
//     once), and SetDIBits takes 8-bit sources only. (SetDIBitsToDevice and
//     StretchDIBits do match 16/24/32-bpp sources to the palette:
//     convert_direct, Art Critic's pictures.)
//   * GetDIBits handles 8-bit requests only; GetBitmapBits/SetBitmapBits
//     8-bit and 1-bit device bitmaps only.
//   * CreateDIBSection over a guest file mapping (hSection != 0) is refused.
//   * Brushes: only solid and hatched ones are created (no pattern brushes —
//     none are imported by the lane).
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "adw/core/log.h"
#include "win32/display.hh"
#include "win32/gdi_objects.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"

namespace adw::win32 {

namespace {

constexpr const char* G = "GDI32.DLL";

struct Gdi32State : RuntimeState {
  ~Gdi32State() override {
    if (scratch_dc) {
      if (scratch_old) SelectObject(scratch_dc, scratch_old);
      DeleteDC(scratch_dc);
    }
    if (scratch_bmp) DeleteObject(scratch_bmp);
  }
  // SaveDC/RestoreDC: the guest-side DC state real GDI does not keep for us.
  std::map<uint32_t, std::vector<GdiDc>> saved;
  std::map<uint32_t, bool> force_background;  // SelectPalette's bForceBackground per DC
  // A reusable 8-bit key-table surface for translated blits.
  HDC scratch_dc = nullptr;
  HBITMAP scratch_bmp = nullptr;
  HGDIOBJ scratch_old = nullptr;
  uint8_t* scratch_bits = nullptr;
  int scratch_w = 0, scratch_h = 0;
  bool warned_deep = false;

  // A w×h (at least) top-down scratch surface; returns its DC.
  HDC scratch(int w, int h) {
    if (!scratch_dc) scratch_dc = CreateCompatibleDC(nullptr);
    if (w > scratch_w || h > scratch_h || !scratch_bmp) {
      int nw = std::max(w, scratch_w), nh = std::max(h, scratch_h);
      uint8_t* bits = nullptr;
      HBITMAP b = Display::create_surface8(nw, nh, true, nullptr, 0, &bits);
      if (!b) return nullptr;
      if (scratch_old) SelectObject(scratch_dc, scratch_old);
      if (scratch_bmp) DeleteObject(scratch_bmp);
      scratch_bmp = b;
      scratch_bits = bits;
      scratch_w = nw;
      scratch_h = nh;
      scratch_old = SelectObject(scratch_dc, scratch_bmp);
    }
    return scratch_dc;
  }
  uint32_t scratch_stride() const { return Display::pitch_for(scratch_w); }
};

GdiTable& gt(Runtime& rt) { return rt.state<GdiTable>(); }
Gdi32State& gs(Runtime& rt) { return rt.state<Gdi32State>(); }

// The DC's selected brush/pen, made for its current palette and selected.
void sync_brush(Runtime& rt, uint32_t hdc) {
  GdiTable& g = gt(rt);
  GdiObject* dc = g.get(hdc, GdiType::dc);
  if (!dc || !dc->dc.brush) return;
  if (HGDIOBJ b = g.realize_for(dc->dc.brush, hdc)) SelectObject(static_cast<HDC>(dc->host), b);
}
void sync_pen(Runtime& rt, uint32_t hdc) {
  GdiTable& g = gt(rt);
  GdiObject* dc = g.get(hdc, GdiType::dc);
  if (!dc || !dc->dc.pen) return;
  if (HGDIOBJ p = g.realize_for(dc->dc.pen, hdc)) SelectObject(static_cast<HDC>(dc->host), p);
}
// Text and background colours depend on the palette too.
void sync_colors(Runtime& rt, uint32_t hdc) {
  GdiTable& g = gt(rt);
  GdiObject* dc = g.get(hdc, GdiType::dc);
  if (!dc) return;
  HDC h = static_cast<HDC>(dc->host);
  ::SetTextColor(h, g.key_color(hdc, dc->dc.text_color));
  ::SetBkColor(h, g.key_color(hdc, dc->dc.bk_color));
}

// ---- pixel access on surfaces ---------------------------------------------------------------------

inline int read_index(const GdiBitmap& b, const uint8_t* row, int x) {
  switch (b.bpp) {
    case 8: return row[x];
    case 4: return (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF;
    case 1: return (row[x >> 3] >> (7 - (x & 7))) & 1;
    default: return 0;
  }
}

// Copies the w×h rectangle at (sx, sy) of `src` through `x` into an 8-bit
// top-down buffer. Pixels outside the source read as index 0.
void translate_rect(const GdiBitmap& src, int sx, int sy, int w, int h, const Xlate& x, uint8_t* out,
                    uint32_t out_stride) {
  for (int j = 0; j < h; j++) {
    uint8_t* o = out + size_t(j) * out_stride;
    int y = sy + j;
    if (y < 0 || y >= std::abs(src.height)) {
      memset(o, x.map[0], size_t(w));
      continue;
    }
    const uint8_t* row = src.row(y);
    for (int i = 0; i < w; i++) {
      int px = sx + i;
      o[i] = (px < 0 || px >= src.width) ? x.map[0] : x.map[size_t(read_index(src, row, px))];
    }
  }
}

bool translatable(const GdiBitmap& b) { return b.host_bits && (b.bpp == 8 || b.bpp == 4 || b.bpp == 1); }

// BitBlt/StretchBlt from src_dc to dst_dc: plain when the indices mean the
// same colours, else through a translated copy of the source rectangle.
BOOL blit(Runtime& rt, uint32_t dst_dc, int x, int y, int w, int h, uint32_t src_dc, int sx, int sy, int sw, int sh,
          DWORD rop, bool stretch) {
  GdiTable& g = gt(rt);
  HDC d = g.host_dc(dst_dc);
  if (!d) return FALSE;
  // ROPs without a source.
  bool uses_src = ((rop >> 2) ^ rop) & 0x330000;
  if (!uses_src || !src_dc) {
    sync_brush(rt, dst_dc);
    return ::PatBlt(d, x, y, w, h, rop);
  }
  HDC s = g.host_dc(src_dc);
  if (!s) return FALSE;
  bool uses_pat = ((rop >> 4) ^ rop) & 0xF0000;
  if (uses_pat) sync_brush(rt, dst_dc);
  sync_colors(rt, dst_dc);  // mono ↔ colour conversions use them
  sync_colors(rt, src_dc);
  GdiBitmap* sb = g.dc_surface(src_dc);
  GdiBitmap* db = g.dc_surface(dst_dc);
  bool plain = !sb || !db || sb->bpp == 1 || db->bpp == 1;
  Xlate xl;
  if (!plain) {
    if (sb->bpp > 8 || db->bpp > 8) {
      Gdi32State& st = gs(rt);
      if (!st.warned_deep) {
        st.warned_deep = true;
        log("gdi: blit between a %d-bpp and a %d-bpp surface is not colour-translated yet", sb->bpp, db->bpp);
      }
      plain = true;
    } else {
      xl = g.surface_xlate(*sb, src_dc, *db, dst_dc);
      plain = xl.identity;
    }
  }
  if (plain || !translatable(*sb)) {
    return stretch ? ::StretchBlt(d, x, y, w, h, s, sx, sy, sw, sh, rop) : ::BitBlt(d, x, y, w, h, s, sx, sy, rop);
  }
  // Translated: GDI may still be writing the source (batching).
  GdiFlush();
  int tw = std::abs(stretch ? sw : w), th = std::abs(stretch ? sh : h);
  if (tw == 0 || th == 0) return TRUE;
  int tx = stretch && sw < 0 ? sx + sw : sx, ty = stretch && sh < 0 ? sy + sh : sy;
  Gdi32State& st = gs(rt);
  HDC sc = st.scratch(tw, th);
  if (!sc) return FALSE;
  translate_rect(*sb, tx, ty, tw, th, xl, st.scratch_bits, st.scratch_stride());
  if (stretch) {
    int ssx = sw < 0 ? tw : 0, ssy = sh < 0 ? th : 0;
    return ::StretchBlt(d, x, y, w, h, sc, ssx, ssy, sw, sh, rop);
  }
  return ::BitBlt(d, x, y, w, h, sc, 0, 0, rop);
}

// ---- guest DIBs (SetDIBitsToDevice, StretchDIBits, SetDIBits, CreateDIBSection) ------------------

// A BITMAPINFO in guest memory, decoded.
struct GuestDib {
  BITMAPINFOHEADER h{};
  std::vector<RGBQUAD> colors;
  std::vector<uint16_t> pal;
  uint32_t masks[3] = {0, 0, 0};
  uint32_t stride = 0;
  int width = 0, height = 0;  // height = |biHeight|
  bool top_down = false;
};

bool read_bitmapinfo(MemoryContext& mem, uint32_t bmi, uint32_t usage, GuestDib& out) {
  uint32_t size = mem.read_u32l(bmi);
  if (size == sizeof(BITMAPCOREHEADER)) {
    // OS/2 1.x header: bcSize, bcWidth(WORD), bcHeight(WORD), bcPlanes, bcBitCount; RGBTRIPLE table.
    BITMAPCOREHEADER ch = read_pod<BITMAPCOREHEADER>(mem, bmi);
    out.h.biSize = sizeof(BITMAPINFOHEADER);
    out.h.biWidth = ch.bcWidth;
    out.h.biHeight = ch.bcHeight;
    out.h.biPlanes = 1;
    out.h.biBitCount = ch.bcBitCount;
    out.h.biCompression = BI_RGB;
    uint32_t n = ch.bcBitCount <= 8 ? 1u << ch.bcBitCount : 0;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t a = bmi + size + 3 * i;
      if (usage == DIB_PAL_COLORS) out.pal.push_back(mem.read_u16l(bmi + size + 2 * i));
      else out.colors.push_back(RGBQUAD{mem.read_u8(a), mem.read_u8(a + 1), mem.read_u8(a + 2), 0});
    }
  } else if (size >= sizeof(BITMAPINFOHEADER)) {
    out.h = read_pod<BITMAPINFOHEADER>(mem, bmi);
    uint32_t table = bmi + size;
    if (out.h.biCompression == BI_BITFIELDS) {
      if (size == sizeof(BITMAPINFOHEADER)) {
        for (int i = 0; i < 3; i++) out.masks[i] = mem.read_u32l(table + 4 * uint32_t(i));
        table += 12;
      } else {
        for (int i = 0; i < 3; i++) out.masks[i] = mem.read_u32l(bmi + 40 + 4 * uint32_t(i));
      }
    }
    if (out.h.biBitCount <= 8 && out.h.biBitCount) {
      uint32_t n = out.h.biClrUsed ? std::min<uint32_t>(out.h.biClrUsed, 256) : 1u << out.h.biBitCount;
      for (uint32_t i = 0; i < n; i++) {
        if (usage == DIB_PAL_COLORS) out.pal.push_back(mem.read_u16l(table + 2 * i));
        else if (usage == DIB_RGB_COLORS) out.colors.push_back(read_pod<RGBQUAD>(mem, table + 4 * i));
      }
    }
  } else {
    return false;
  }
  out.width = out.h.biWidth;
  out.top_down = out.h.biHeight < 0;
  out.height = std::abs(out.h.biHeight);
  out.stride = uint32_t(((int64_t(out.width) * out.h.biBitCount + 31) / 32) * 4);
  return out.width > 0 && out.h.biBitCount > 0;
}

// The surface description of a guest DIB (for surface_xlate / translate_rect).
GdiBitmap dib_surface(const GuestDib& d, uint32_t usage, uint8_t* bits) {
  GdiBitmap b;
  b.kind = usage == 2 /*DIB_PAL_INDICES*/ ? BitmapKind::device : BitmapKind::dib;
  b.width = d.width;
  b.height = d.height;
  b.bpp = d.h.biBitCount;
  b.stride = d.stride;
  b.top_down = d.top_down;
  b.host_bits = bits;
  b.usage = usage;
  b.colors = d.colors;
  b.pal_indices = d.pal;
  return b;
}

struct KeyBmi {
  BITMAPINFOHEADER h;
  RGBQUAD colors[256];
};

// The guest DIB `rows` rows starting at bottom-up scan line `first` (or all
// rows) as an 8-bit key-table DIB whose indices mean what dst_dc's surface
// means. Returns false for formats not handled yet.
// One channel of a BI_BITFIELDS mask, scaled to 8 bits.
inline uint8_t field8(uint32_t v, uint32_t mask) {
  if (!mask) return 0;
  int shift = 0;
  while (!((mask >> shift) & 1)) shift++;
  uint32_t m = mask >> shift, x = (v & mask) >> shift;
  return uint8_t(m == 0xFF ? x : (x * 255 + m / 2) / m);
}

// A direct-colour guest DIB (16/24/32 bpp — Art Critic's 24-bit pictures)
// onto 8-bit: every pixel's RGB goes through the same matching an 8-bit DIB's
// colour table gets (surface_xlate, i.e. what a Win95 palette device did:
// nearest entry of the DC's palette), batched 256 distinct colours at a time
// and cached at 15-bit precision so a photograph costs a few hundred batches
// at most.
bool convert_direct(Runtime& rt, const GuestDib& d, uint32_t bits_addr, uint32_t rows, uint32_t dst_dc,
                    std::vector<uint8_t>& out) {
  const int bpp = d.h.biBitCount;
  const bool fields = d.h.biCompression == BI_BITFIELDS;
  if (!(bpp == 16 || bpp == 24 || bpp == 32) || !(d.h.biCompression == BI_RGB || (fields && bpp != 24))) return false;
  uint32_t masks[3] = {d.masks[0], d.masks[1], d.masks[2]};
  if (!fields) {
    if (bpp == 16) masks[0] = 0x7C00, masks[1] = 0x03E0, masks[2] = 0x001F;
    else masks[0] = 0xFF0000, masks[1] = 0x00FF00, masks[2] = 0x0000FF;
  }
  const uint8_t* src = rt.mem().at<uint8_t>(bits_addr, size_t(d.stride) * rows);
  auto rgb_at = [&](const uint8_t* row, int x) -> uint32_t {  // 0x00RRGGBB
    uint32_t v;
    if (bpp == 24) return uint32_t(row[3 * x + 2]) << 16 | uint32_t(row[3 * x + 1]) << 8 | row[3 * x];
    if (bpp == 16) v = uint32_t(row[2 * x]) | uint32_t(row[2 * x + 1]) << 8;
    else memcpy(&v, row + 4 * x, 4);
    return uint32_t(field8(v, masks[0])) << 16 | uint32_t(field8(v, masks[1])) << 8 | field8(v, masks[2]);
  };
  auto key15 = [](uint32_t c) { return ((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F); };

  // Pass 1: the distinct 15-bit colours, each with a representative RGB.
  std::vector<int32_t> slot(32768, -1);
  std::vector<uint32_t> reps;
  for (uint32_t r = 0; r < rows; r++) {
    const uint8_t* row = src + size_t(r) * d.stride;
    for (int i = 0; i < d.width; i++) {
      uint32_t c = rgb_at(row, i), k = key15(c);
      if (slot[k] < 0) {
        slot[k] = int32_t(reps.size());
        reps.push_back(c);
      }
    }
  }
  // Pass 2: match them in colour-table batches.
  GdiTable& g = gt(rt);
  GdiBitmap* dst = g.dc_surface(dst_dc);
  GdiBitmap device;
  device.bpp = 8;
  std::vector<uint8_t> index_of(reps.size());
  for (size_t base = 0; base < reps.size(); base += 256) {
    GuestDib t;
    t.h.biBitCount = 8;
    t.width = 1;
    t.height = 1;
    t.stride = 4;
    size_t n = std::min<size_t>(256, reps.size() - base);
    for (size_t j = 0; j < n; j++) {
      uint32_t c = reps[base + j];
      t.colors.push_back(RGBQUAD{BYTE(c), BYTE(c >> 8), BYTE(c >> 16), 0});
    }
    GdiBitmap s = dib_surface(t, DIB_RGB_COLORS, nullptr);
    Xlate x = g.surface_xlate(s, dst_dc, dst ? *dst : device, dst_dc);
    for (size_t j = 0; j < n; j++) index_of[base + j] = x.map[j];
  }
  // Pass 3: the 8-bit image, rows in the source's order.
  uint32_t ostride = Display::pitch_for(d.width);
  out.assign(size_t(ostride) * rows, 0);
  for (uint32_t r = 0; r < rows; r++) {
    const uint8_t* row = src + size_t(r) * d.stride;
    uint8_t* o = out.data() + size_t(r) * ostride;
    for (int i = 0; i < d.width; i++) o[i] = index_of[size_t(slot[key15(rgb_at(row, i))])];
  }
  return true;
}

bool convert_dib(Runtime& rt, const GuestDib& d, uint32_t usage, uint32_t bits_addr, uint32_t rows, uint32_t dst_dc,
                 KeyBmi& bmi, std::vector<uint8_t>& out, const uint8_t** direct) {
  GdiTable& g = gt(rt);
  *direct = nullptr;
  if (d.h.biBitCount > 8) {
    if (!convert_direct(rt, d, bits_addr, rows, dst_dc, out)) return false;
    bmi.h = d.h;
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biBitCount = 8;
    bmi.h.biCompression = BI_RGB;
    bmi.h.biClrUsed = 256;
    bmi.h.biClrImportant = 0;
    bmi.h.biSizeImage = 0;
    memcpy(bmi.colors, Display::key_table().data(), sizeof(bmi.colors));
    return true;
  }
  if (d.h.biCompression != BI_RGB) return false;
  uint8_t* src = rt.mem().at<uint8_t>(bits_addr, size_t(d.stride) * rows);
  GdiBitmap s = dib_surface(d, usage, src);
  s.height = int(rows);
  GdiBitmap* dst = g.dc_surface(dst_dc);
  GdiBitmap device;
  device.bpp = 8;
  Xlate x = g.surface_xlate(s, dst_dc, dst ? *dst : device, dst_dc);
  bmi.h = d.h;
  bmi.h.biSize = sizeof(BITMAPINFOHEADER);
  bmi.h.biBitCount = 8;
  bmi.h.biClrUsed = 256;
  bmi.h.biClrImportant = 0;
  bmi.h.biSizeImage = 0;
  memcpy(bmi.colors, Display::key_table().data(), sizeof(bmi.colors));
  if (d.h.biBitCount == 8 && x.identity) {
    *direct = src;
    return true;
  }
  uint32_t ostride = Display::pitch_for(d.width);
  out.assign(size_t(ostride) * rows, 0);
  // Row order is the source's: translate memory rows one to one.
  for (uint32_t r = 0; r < rows; r++) {
    const uint8_t* row = src + size_t(r) * d.stride;
    uint8_t* o = out.data() + size_t(r) * ostride;
    for (int i = 0; i < d.width; i++) o[i] = x.map[size_t(read_index(s, row, i))];
  }
  return true;
}

COLORREF guest_rgb(COLORREF c) { return c & 0xFFFFFF; }

// LOGBRUSH / LOGPEN as a 32-bit guest sees them.
struct LogBrush32 {
  uint32_t style, color, hatch;
};
struct LogPen32 {
  uint32_t style;
  int32_t width_x, width_y;
  uint32_t color;
};

uint32_t make_region(Runtime& rt, HRGN r) {
  if (!r) return 0;
  GdiObject o;
  o.type = GdiType::region;
  o.host = r;
  uint32_t h = gt(rt).add(std::move(o));
  if (!h) DeleteObject(r);
  return h;
}

HRGN host_region(Runtime& rt, uint32_t h) {
  GdiObject* o = gt(rt).get(h, GdiType::region);
  return o ? static_cast<HRGN>(o->host) : nullptr;
}

LogicalPalette* palette_of(Runtime& rt, uint32_t h) {
  GdiObject* o = gt(rt).get(h, GdiType::palette);
  return o ? o->pal.get() : nullptr;
}

std::vector<POINT> read_points(MemoryContext& mem, uint32_t p, int32_t n) {
  std::vector<POINT> pts;
  if (n <= 0 || n > 0x100000) return pts;
  pts.resize(size_t(n));
  mem.memcpy(pts.data(), p, sizeof(POINT) * size_t(n));
  return pts;
}

}  // namespace

void register_gdi32(ShimRegistry& r) {
  // ---- objects ----
  r.impl(G, "GetStockObject", [](Call& c) { c.ret(gt(c.rt).stock(c.iarg(0))); });
  r.impl(G, "DeleteObject", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* o = g.get(c.arg(0));
    if (!o || o->type == GdiType::dc) return c.ret(0);
    c.ret_bool(g.destroy(c.arg(0)));
  });
  r.impl(G, "SelectObject", [](Call& c) {
    GdiTable& g = gt(c.rt);
    uint32_t hdc = c.arg(0), h = c.arg(1);
    GdiObject* dc = g.get(hdc, GdiType::dc);
    GdiObject* o = g.get(h);
    if (!dc || !o) return c.ret(0);
    HDC hd = static_cast<HDC>(dc->host);
    switch (o->type) {
      case GdiType::bitmap: {
        if (dc->dc.is_screen) return c.ret(0);  // the screen DC's surface is fixed
        HGDIOBJ prev = SelectObject(hd, o->host);
        if (!prev) return c.ret(0);
        uint32_t old = dc->dc.bitmap ? dc->dc.bitmap : g.wrap_host(prev, GdiType::bitmap);
        g.get(hdc, GdiType::dc)->dc.bitmap = h;
        return c.ret(old);
      }
      case GdiType::brush:
      case GdiType::pen: {
        HGDIOBJ prev = SelectObject(hd, g.realize_for(h, hdc));
        dc = g.get(hdc, GdiType::dc);
        uint32_t& slot = o->type == GdiType::brush ? dc->dc.brush : dc->dc.pen;
        uint32_t old = slot ? slot : g.wrap_host(prev, GdiType::free);
        slot = h;
        return c.ret(old);
      }
      case GdiType::font: {
        HGDIOBJ prev = SelectObject(hd, o->host);
        dc = g.get(hdc, GdiType::dc);
        uint32_t old = dc->dc.font ? dc->dc.font : g.wrap_host(prev, GdiType::font);
        dc->dc.font = h;
        return c.ret(old);
      }
      case GdiType::region:
        return c.ret(uint32_t(SelectClipRgn(hd, static_cast<HRGN>(o->host))));
      default:
        return c.ret(0);
    }
  });
  r.impl(G, "GetObjectA", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* o = g.get(c.arg(0));
    uint32_t size = c.arg(1), buf = c.arg(2);
    if (!o) return c.ret(0);
    auto put = [&](const void* data, uint32_t n) {
      if (!buf) return c.ret(n);
      n = std::min(n, size);
      c.mem().memcpy(buf, data, n);
      c.ret(n);
    };
    switch (o->type) {
      case GdiType::bitmap: {
        const GdiBitmap& b = o->bmp;
        g32::DIBSECTION ds{};
        ds.dsBm.bmWidth = b.width;
        ds.dsBm.bmHeight = std::abs(b.height);
        ds.dsBm.bmPlanes = 1;
        ds.dsBm.bmBitsPixel = uint16_t(b.bpp);
        if (b.kind == BitmapKind::dib) {
          ds.dsBm.bmWidthBytes = int32_t(b.stride);
          ds.dsBm.bmBits = b.guest_bits;
          ds.dsBmih.biSize = sizeof(BITMAPINFOHEADER);
          ds.dsBmih.biWidth = b.width;
          ds.dsBmih.biHeight = b.top_down ? -std::abs(b.height) : std::abs(b.height);
          ds.dsBmih.biPlanes = 1;
          ds.dsBmih.biBitCount = WORD(b.bpp);
          ds.dsBmih.biCompression = b.compression;
          ds.dsBmih.biSizeImage = b.stride * uint32_t(std::abs(b.height));
          ds.dsBmih.biClrUsed = uint32_t(b.usage == DIB_PAL_COLORS ? b.pal_indices.size() : b.colors.size());
          memcpy(ds.dsBitfields, b.masks, sizeof(ds.dsBitfields));
          ds.dshSection = b.section;
          ds.dsOffset = b.section_offset;
          return put(&ds, size >= sizeof(g32::DIBSECTION) ? sizeof(g32::DIBSECTION) : sizeof(g32::BITMAP));
        }
        // A device bitmap: DDB rows are WORD-aligned.
        ds.dsBm.bmWidthBytes = ((b.width * b.bpp + 15) / 16) * 2;
        return put(&ds.dsBm, sizeof(g32::BITMAP));
      }
      case GdiType::palette: {
        uint16_t n = uint16_t(o->pal->entries.size());
        return put(&n, 2);
      }
      case GdiType::brush: {
        LogBrush32 lb{uint32_t(o->brush_style), guest_rgb(o->color), uint32_t(o->hatch)};
        return put(&lb, sizeof(lb));
      }
      case GdiType::pen: {
        LogPen32 lp{uint32_t(o->pen_style), o->pen_width, 0, guest_rgb(o->color)};
        return put(&lp, sizeof(lp));
      }
      case GdiType::font: {
        LOGFONTA lf{};  // pointer-free: same layout on x86 and x64
        if (!GetObjectA(o->host, sizeof(lf), &lf)) return c.ret(0);
        return put(&lf, sizeof(lf));
      }
      default:
        return c.ret(0);
    }
  });
  r.impl(G, "UnrealizeObject", [](Call& c) {
    // A palette: the next RealizePalette maps it afresh. A brush: nothing to do.
    if (LogicalPalette* p = palette_of(c.rt, c.arg(0))) p->realized = false;
    c.ret(1);
  });

  // ---- DCs ----
  r.impl(G, "CreateCompatibleDC", [](Call& c) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return c.ret(0);
    uint32_t h = gt(c.rt).add_dc(dc, true);
    if (!h) DeleteDC(dc);
    c.ret(h);
  });
  r.impl(G, "DeleteDC", [](Call& c) {
    GdiTable& g = gt(c.rt);
    if (!g.get(c.arg(0), GdiType::dc)) return c.ret(0);
    gs(c.rt).saved.erase(c.arg(0));
    c.ret_bool(g.destroy(c.arg(0)));
  });
  r.impl(G, "SaveDC", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* dc = g.get(c.arg(0), GdiType::dc);
    if (!dc) return c.ret(0);
    int n = ::SaveDC(static_cast<HDC>(dc->host));
    if (n) {
      auto& stack = gs(c.rt).saved[c.arg(0)];
      stack.resize(size_t(n - 1));
      stack.push_back(dc->dc);
    }
    c.ret(uint32_t(n));
  });
  r.impl(G, "RestoreDC", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* dc = g.get(c.arg(0), GdiType::dc);
    if (!dc) return c.ret(0);
    auto& stack = gs(c.rt).saved[c.arg(0)];
    int32_t level = c.iarg(1);
    int32_t target = level < 0 ? int32_t(stack.size()) + level + 1 : level;
    if (!::RestoreDC(static_cast<HDC>(dc->host), level)) return c.ret(0);
    if (target >= 1 && size_t(target) <= stack.size()) {
      dc->dc = stack[size_t(target - 1)];
      stack.resize(size_t(target - 1));
    }
    c.ret(1);
  });
  r.impl(G, "GetDeviceCaps", [](Call& c) {
    if (!gt(c.rt).get(c.arg(0), GdiType::dc)) return c.ret(0);
    c.ret(uint32_t(gt(c.rt).display().device_caps(c.iarg(1))));
  });
  r.impl(G, "GetDCOrgEx", [](Call& c) {
    write_pod(c.mem(), c.arg(1), POINT{0, 0});
    c.ret(1);
  });
  r.impl(G, "SetViewportOrgEx", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    POINT old{};
    if (!dc || !::SetViewportOrgEx(dc, c.iarg(1), c.iarg(2), &old)) return c.ret(0);
    if (c.arg(3)) write_pod(c.mem(), c.arg(3), old);
    c.ret(1);
  });
  r.impl(G, "GetWindowOrgEx", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    POINT p{};
    if (!dc || !::GetWindowOrgEx(dc, &p)) return c.ret(0);
    write_pod(c.mem(), c.arg(1), p);
    c.ret(1);
  });
  r.impl(G, "SetBrushOrgEx", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    POINT old{};
    if (!dc || !::SetBrushOrgEx(dc, c.iarg(1), c.iarg(2), &old)) return c.ret(0);
    if (c.arg(3)) write_pod(c.mem(), c.arg(3), old);
    c.ret(1);
  });
  r.impl(G, "SetTextColor", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* dc = g.get(c.arg(0), GdiType::dc);
    if (!dc) return c.ret(CLR_INVALID);
    COLORREF old = dc->dc.text_color;
    dc->dc.text_color = c.arg(1);
    ::SetTextColor(static_cast<HDC>(dc->host), g.key_color(c.arg(0), c.arg(1)));
    c.ret(old);
  });
  r.impl(G, "SetBkColor", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* dc = g.get(c.arg(0), GdiType::dc);
    if (!dc) return c.ret(CLR_INVALID);
    COLORREF old = dc->dc.bk_color;
    dc->dc.bk_color = c.arg(1);
    ::SetBkColor(static_cast<HDC>(dc->host), g.key_color(c.arg(0), c.arg(1)));
    c.ret(old);
  });
  r.impl(G, "GetTextColor", [](Call& c) {
    GdiObject* dc = gt(c.rt).get(c.arg(0), GdiType::dc);
    c.ret(dc ? dc->dc.text_color : CLR_INVALID);
  });
  r.impl(G, "GetBkColor", [](Call& c) {
    GdiObject* dc = gt(c.rt).get(c.arg(0), GdiType::dc);
    c.ret(dc ? dc->dc.bk_color : CLR_INVALID);
  });
  // Plain DC attributes: real GDI keeps them.
  r.impl(G, "SetBkMode", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::SetBkMode(dc, c.iarg(1))) : 0);
  });
  r.impl(G, "GetBkMode", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::GetBkMode(dc)) : 0);
  });
  r.impl(G, "SetROP2", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::SetROP2(dc, c.iarg(1))) : 0);
  });
  r.impl(G, "SetPolyFillMode", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::SetPolyFillMode(dc, c.iarg(1))) : 0);
  });
  r.impl(G, "GetPolyFillMode", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::GetPolyFillMode(dc)) : 0);
  });
  r.impl(G, "SetTextAlign", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? ::SetTextAlign(dc, c.arg(1)) : GDI_ERROR);
  });
  r.impl(G, "SetTextCharacterExtra", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::SetTextCharacterExtra(dc, c.iarg(1))) : 0x80000000);
  });
  r.impl(G, "GetTextMetricsA", [](Call& c) {
    // TEXTMETRICA has no pointers: 56 bytes on both sides. The metrics are the
    // real font's, as for GetTextExtentPoint32A.
    static_assert(sizeof(TEXTMETRICA) == 56);
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    TEXTMETRICA tm{};
    if (!dc || !::GetTextMetricsA(dc, &tm)) return c.ret(0);
    write_pod(c.mem(), c.arg(1), tm);
    c.ret(1);
  });
  r.impl(G, "SetStretchBltMode", [](Call& c) {
    GdiObject* dc = gt(c.rt).get(c.arg(0), GdiType::dc);
    if (!dc) return c.ret(0);
    int old = dc->dc.stretch_mode;
    dc->dc.stretch_mode = c.iarg(1);
    // HALFTONE averages colours, which on key-table surfaces averages indices.
    int mode = c.iarg(1) == HALFTONE ? COLORONCOLOR : c.iarg(1);
    ::SetStretchBltMode(static_cast<HDC>(dc->host), mode);
    c.ret(uint32_t(old));
  });
  r.impl(G, "GdiFlush", [](Call& c) {
    ::GdiFlush();
    c.ret(1);
  });

  // ---- palettes (display.hh) ----
  r.impl(G, "CreatePalette", [](Call& c) {
    uint32_t lp = c.arg(0);
    uint16_t version = c.mem().read_u16l(lp), n = c.mem().read_u16l(lp + 2);
    if (version != 0x300 || n == 0) return c.ret(0);
    GdiObject o;
    o.type = GdiType::palette;
    o.pal = std::make_shared<LogicalPalette>();
    o.pal->entries.resize(n);
    c.mem().memcpy(o.pal->entries.data(), lp + 4, sizeof(PALETTEENTRY) * n);
    c.ret(gt(c.rt).add(std::move(o)));
  });
  r.impl(G, "SelectPalette", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* dc = g.get(c.arg(0), GdiType::dc);
    if (!dc || !g.get(c.arg(1), GdiType::palette)) return c.ret(0);
    uint32_t old = dc->dc.palette ? dc->dc.palette : g.stock(DEFAULT_PALETTE);
    uint32_t np = c.arg(1) == g.stock(DEFAULT_PALETTE) ? 0 : c.arg(1);
    g.get(c.arg(0), GdiType::dc)->dc.palette = np;
    gs(c.rt).force_background[c.arg(0)] = c.arg(2) != 0;
    c.ret(old);
  });
  r.impl(G, "RealizePalette", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiObject* dc = g.get(c.arg(0), GdiType::dc);
    if (!dc) return c.ret(GDI_ERROR);
    if (!dc->dc.palette) return c.ret(0);  // DEFAULT_PALETTE: the statics are always there
    LogicalPalette* p = palette_of(c.rt, dc->dc.palette);
    if (!p) return c.ret(GDI_ERROR);
    bool bg = gs(c.rt).force_background[c.arg(0)];
    UINT n = g.display().realize(*p, bg);
    // Brushes/pens/text in this DC map through the new palette from now on.
    sync_colors(c.rt, c.arg(0));
    c.ret(n);
  });
  r.impl(G, "GetPaletteEntries", [](Call& c) {
    LogicalPalette* p = palette_of(c.rt, c.arg(0));
    if (!p) return c.ret(0);
    uint32_t start = c.arg(1), n = c.arg(2), out = c.arg(3);
    if (!out) return c.ret(uint32_t(p->entries.size()));
    if (start >= p->entries.size()) return c.ret(0);
    n = std::min<uint32_t>(n, uint32_t(p->entries.size()) - start);
    c.mem().memcpy(out, p->entries.data() + start, sizeof(PALETTEENTRY) * n);
    c.ret(n);
  });
  r.impl(G, "SetPaletteEntries", [](Call& c) {
    LogicalPalette* p = palette_of(c.rt, c.arg(0));
    GdiObject* o = gt(c.rt).get(c.arg(0), GdiType::palette);
    if (!p || o->stock) return c.ret(0);
    uint32_t start = c.arg(1), n = c.arg(2), in = c.arg(3);
    if (start >= p->entries.size()) return c.ret(0);
    n = std::min<uint32_t>(n, uint32_t(p->entries.size()) - start);
    c.mem().memcpy(p->entries.data() + start, in, sizeof(PALETTEENTRY) * n);
    // The hardware palette changes only when the palette is realized again.
    c.ret(n);
  });
  r.impl(G, "AnimatePalette", [](Call& c) {
    LogicalPalette* p = palette_of(c.rt, c.arg(0));
    if (!p) return c.ret(0);
    uint32_t start = c.arg(1), n = c.arg(2);
    std::vector<PALETTEENTRY> e(n);
    if (n) c.mem().memcpy(e.data(), c.arg(3), sizeof(PALETTEENTRY) * n);
    gt(c.rt).display().animate(*p, int(start), int(n), e.data());
    c.ret(1);
  });
  r.impl(G, "GetNearestPaletteIndex", [](Call& c) {
    LogicalPalette* p = palette_of(c.rt, c.arg(0));
    if (!p || p->entries.empty()) return c.ret(CLR_INVALID);
    COLORREF col = c.arg(1);
    if ((col >> 24) == 0x01) return c.ret(std::min<uint32_t>(col & 0xFFFF, uint32_t(p->entries.size()) - 1));
    c.ret(uint32_t(Display::nearest_in(*p, col & 0xFFFFFF)));
  });
  r.impl(G, "GetSystemPaletteEntries", [](Call& c) {
    const auto& sys = gt(c.rt).display().system_palette();
    uint32_t start = c.arg(1), n = c.arg(2), out = c.arg(3);
    if (!out) return c.ret(256);
    if (start >= 256) return c.ret(0);
    n = std::min<uint32_t>(n, 256 - start);
    for (uint32_t i = 0; i < n; i++) {
      PALETTEENTRY e = sys[start + i];
      e.peFlags = 0;
      write_pod(c.mem(), out + 4 * i, e);
    }
    c.ret(n);
  });
  r.impl(G, "SetSystemPaletteUse", [](Call& c) { c.ret(gt(c.rt).display().set_palette_use(c.arg(1))); });
  r.impl(G, "SetDIBColorTable", [](Call& c) {
    GdiBitmap* b = gt(c.rt).dc_surface(c.arg(0));
    if (!b || b->kind != BitmapKind::dib || b->bpp > 8 || b->usage != DIB_RGB_COLORS) return c.ret(0);
    uint32_t start = c.arg(1), n = c.arg(2);
    size_t size = size_t(1) << b->bpp;
    if (b->colors.size() < size) b->colors.resize(size, RGBQUAD{0, 0, 0, 0});
    if (start >= size) return c.ret(0);
    n = std::min<uint32_t>(n, uint32_t(size) - start);
    c.mem().memcpy(b->colors.data() + start, c.arg(3), sizeof(RGBQUAD) * n);
    c.ret(n);
  });

  // ---- bitmaps ----
  r.impl(G, "CreateCompatibleBitmap", [](Call& c) {
    GdiTable& g = gt(c.rt);
    GdiBitmap* s = g.dc_surface(c.arg(0));
    int w = c.iarg(1), h = c.iarg(2);
    if (!s || w <= 0 || h <= 0) return c.ret(gt(c.rt).create_device_bitmap(1, 1, 1));
    // Compatible with what the DC holds: monochrome stays monochrome (a fresh
    // memory DC), anything else becomes an 8-bit device bitmap.
    c.ret(gt(c.rt).create_device_bitmap(w, h, s->bpp == 1 ? 1 : 8));
  });
  r.impl(G, "CreateBitmap", [](Call& c) {
    int w = c.iarg(0), h = c.iarg(1);
    uint32_t planes = c.arg(2), bpp = c.arg(3), bits = c.arg(4);
    uint32_t depth = planes * bpp;
    if (w <= 0 || h <= 0) return c.ret(gt(c.rt).create_device_bitmap(1, 1, 1));
    if (depth != 1 && depth != 8) {
      log("gdi: CreateBitmap at %u bpp is not supported", depth);
      return c.ret(0);
    }
    uint32_t hb = gt(c.rt).create_device_bitmap(w, h, int(depth));
    GdiObject* o = gt(c.rt).get(hb, GdiType::bitmap);
    if (o && bits) {
      uint32_t src_stride = uint32_t(((w * int(depth) + 15) / 16) * 2);  // DDB rows are WORD-aligned
      if (depth == 1) {
        std::vector<uint8_t> tmp(size_t(src_stride) * uint32_t(h));
        c.mem().memcpy(tmp.data(), bits, tmp.size());
        SetBitmapBits(static_cast<HBITMAP>(o->host), DWORD(tmp.size()), tmp.data());
      } else {
        for (int y = 0; y < h; y++) c.mem().memcpy(o->bmp.row(y), bits + uint32_t(y) * src_stride, size_t(w));
      }
    }
    c.ret(hb);
  });
  r.impl(G, "CreateDIBSection", [](Call& c) {
    GdiTable& g = gt(c.rt);
    uint32_t usage = c.arg(2), out_bits = c.arg(3), section = c.arg(4);
    GuestDib d;
    if (!read_bitmapinfo(c.mem(), c.arg(1), usage, d) || d.height == 0) {
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    if (section) {
      log("gdi: CreateDIBSection over a file mapping is not supported");
      return c.ret(0);
    }
    uint32_t size = d.stride * uint32_t(d.height);
    uint32_t bits = c.rt.heap().alloc(size, /*zero=*/true, 16);
    if (!bits) {
      c.set_last_error(ERROR_NOT_ENOUGH_MEMORY);
      return c.ret(0);
    }
    auto [hsec, off] = c.mem().section_for(bits);
    HBITMAP hb = nullptr;
    uint8_t* gdi_bits = nullptr;
    int bpp = d.h.biBitCount;
    if (bpp == 8) {
      hb = Display::create_surface8(d.width, d.height, d.top_down, static_cast<HANDLE>(hsec), off, &gdi_bits);
    } else {
      // 1/4 bpp: the first 2^bpp key colours, so indices still copy exactly
      // into 8-bit surfaces; deeper formats keep their real layout (a known
      // gap, see the header).
      struct {
        BITMAPINFOHEADER h;
        RGBQUAD colors[256];
        uint32_t pad[3];
      } bmi{};
      bmi.h = d.h;
      bmi.h.biSize = sizeof(BITMAPINFOHEADER);
      int n = bpp <= 8 ? 1 << bpp : 0;
      bmi.h.biClrUsed = uint32_t(n);
      if (n) memcpy(bmi.colors, Display::key_table().data(), sizeof(RGBQUAD) * size_t(n));
      if (d.h.biCompression == BI_BITFIELDS) memcpy(bmi.colors, d.masks, sizeof(d.masks));
      void* p = nullptr;
      hb = ::CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &p,
                              static_cast<HANDLE>(hsec), off);
      gdi_bits = static_cast<uint8_t*>(p);
    }
    if (!hb) {
      c.rt.heap().free(bits);
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    GdiObject o;
    o.type = GdiType::bitmap;
    o.host = hb;
    GdiBitmap& b = o.bmp;
    b.kind = BitmapKind::dib;
    b.width = d.width;
    b.height = d.height;
    b.bpp = bpp;
    b.stride = d.stride;
    b.top_down = d.top_down;
    b.guest_bits = bits;
    b.guest_bits_alloc = bits;
    b.host_bits = c.mem().at<uint8_t>(bits, size);
    b.usage = usage;
    b.colors = d.colors;
    b.pal_indices = d.pal;
    b.compression = d.h.biCompression;
    memcpy(b.masks, d.masks, sizeof(b.masks));
    uint32_t h = g.add(std::move(o));
    if (!h) {
      DeleteObject(hb);
      c.rt.heap().free(bits);
      return c.ret(0);
    }
    if (out_bits) c.mem().write_u32l(out_bits, bits);
    trace("gdi", "CreateDIBSection %dx%d %d bpp usage %u -> 0x%X bits 0x%08X", d.width, int(d.h.biHeight), bpp, usage, h,
          bits);
    c.ret(h);
  });
  r.impl(G, "GetBitmapBits", [](Call& c) {
    GdiObject* o = gt(c.rt).get(c.arg(0), GdiType::bitmap);
    if (!o) return c.ret(0);
    const GdiBitmap& b = o->bmp;
    uint32_t n = c.arg(1), out = c.arg(2);
    ::GdiFlush();
    if (b.bpp == 1 || !b.host_bits) {
      std::vector<uint8_t> tmp(n);
      LONG got = ::GetBitmapBits(static_cast<HBITMAP>(o->host), LONG(n), tmp.data());
      if (got > 0) c.mem().memcpy(out, tmp.data(), size_t(got));
      return c.ret(uint32_t(std::max<LONG>(got, 0)));
    }
    uint32_t ws = uint32_t(((b.width * b.bpp + 15) / 16) * 2), done = 0;
    for (int y = 0; y < std::abs(b.height) && done < n; y++) {
      uint32_t k = std::min(ws, n - done);
      c.mem().memcpy(out + done, b.row(y), std::min<uint32_t>(k, b.stride));
      done += k;
    }
    c.ret(done);
  });
  r.impl(G, "SetBitmapBits", [](Call& c) {
    GdiObject* o = gt(c.rt).get(c.arg(0), GdiType::bitmap);
    if (!o) return c.ret(0);
    GdiBitmap& b = o->bmp;
    uint32_t n = c.arg(1), in = c.arg(2);
    ::GdiFlush();
    if (b.bpp == 1 || !b.host_bits) {
      std::vector<uint8_t> tmp(n);
      c.mem().memcpy(tmp.data(), in, n);
      return c.ret(uint32_t(std::max<LONG>(::SetBitmapBits(static_cast<HBITMAP>(o->host), n, tmp.data()), 0)));
    }
    uint32_t ws = uint32_t(((b.width * b.bpp + 15) / 16) * 2), done = 0;
    for (int y = 0; y < std::abs(b.height) && done < n; y++) {
      uint32_t k = std::min(ws, n - done);
      c.mem().memcpy(b.row(y), in + done, std::min<uint32_t>(k, b.stride));
      done += k;
    }
    c.ret(done);
  });
  r.impl(G, "GetDIBits", [](Call& c) {
    // (hdc, hbm, start, lines, bits, bmi, usage) — 8-bit requests only (a known gap, see the header).
    GdiTable& g = gt(c.rt);
    GdiObject* o = g.get(c.arg(1), GdiType::bitmap);
    uint32_t hdc = c.arg(0), start = c.arg(2), lines = c.arg(3), out = c.arg(4), bmi = c.arg(5), usage = c.arg(6);
    if (!o) return c.ret(0);
    const GdiBitmap& b = o->bmp;
    BITMAPINFOHEADER h = read_pod<BITMAPINFOHEADER>(c.mem(), bmi);
    if (h.biBitCount == 0) {
      // Fill in the header only.
      h.biWidth = b.width;
      h.biHeight = b.top_down ? -std::abs(b.height) : std::abs(b.height);
      h.biPlanes = 1;
      h.biBitCount = WORD(b.bpp);
      h.biCompression = BI_RGB;
      h.biSizeImage = uint32_t(((b.width * b.bpp + 31) / 32) * 4) * uint32_t(std::abs(b.height));
      h.biClrUsed = 0;
      write_pod(c.mem(), bmi, h);
      return c.ret(1);
    }
    if (h.biBitCount != 8 || !b.host_bits || b.bpp != 8) {
      log("gdi: GetDIBits to %u bpp from a %d-bpp bitmap is not supported", h.biBitCount, b.bpp);
      return c.ret(0);
    }
    ::GdiFlush();
    // The colour table: what the bitmap's indices mean.
    for (int i = 0; i < 256; i++) {
      if (usage == DIB_PAL_COLORS) {
        c.mem().write_u16l(bmi + h.biSize + 2 * uint32_t(i), uint16_t(i));
      } else {
        COLORREF rgb = g.surface_rgb(b, i, hdc);
        write_pod(c.mem(), bmi + h.biSize + 4 * uint32_t(i),
                  RGBQUAD{GetBValue(rgb), GetGValue(rgb), GetRValue(rgb), 0});
      }
    }
    if (!out) return c.ret(uint32_t(std::abs(b.height)));
    uint32_t ostride = Display::pitch_for(b.width);
    bool out_top_down = h.biHeight < 0;
    int height = std::abs(b.height);
    uint32_t done = 0;
    for (uint32_t k = 0; k < lines; k++) {
      int scan = int(start + k);  // bottom-up scan line number
      if (scan >= height) break;
      int y = out_top_down ? scan : height - 1 - scan;
      c.mem().memcpy(out + k * ostride, b.row(y), size_t(b.width));
      done++;
    }
    c.ret(done);
  });
  r.impl(G, "SetDIBits", [](Call& c) {
    // (hdc, hbm, start, lines, bits, bmi, usage) into a device bitmap.
    GdiTable& g = gt(c.rt);
    GdiObject* o = g.get(c.arg(1), GdiType::bitmap);
    uint32_t hdc = c.arg(0), start = c.arg(2), lines = c.arg(3), bits = c.arg(4), usage = c.arg(6);
    if (!o || !o->bmp.host_bits || o->bmp.bpp != 8) return c.ret(0);
    GuestDib d;
    if (!read_bitmapinfo(c.mem(), c.arg(5), usage, d) || d.h.biBitCount > 8) {
      log("gdi: SetDIBits from this format is not supported");
      return c.ret(0);
    }
    c.rt.charge_pixels(int64_t(std::min(o->bmp.width, d.width)) * lines);
    uint8_t* src = c.mem().at<uint8_t>(bits, size_t(d.stride) * lines);
    GdiBitmap s = dib_surface(d, usage, src);
    Xlate x = g.surface_xlate(s, hdc, o->bmp, hdc);
    ::GdiFlush();
    GdiBitmap& b = o->bmp;
    int height = std::abs(b.height);
    uint32_t done = 0;
    for (uint32_t k = 0; k < lines; k++) {
      int scan = int(start + k);
      if (scan >= height || scan >= d.height) break;
      int y = height - 1 - scan;
      const uint8_t* row = src + size_t(d.top_down ? d.height - 1 - scan : k) * d.stride;
      uint8_t* dst = b.row(y);
      for (int i = 0; i < std::min(b.width, d.width); i++) dst[i] = x.map[size_t(read_index(s, row, i))];
      done++;
    }
    c.ret(done);
  });

  // ---- blits ----
  r.impl(G, "BitBlt", [](Call& c) {
    c.rt.charge_pixels(int64_t(std::abs(c.iarg(3))) * std::abs(c.iarg(4)));
    c.ret_bool(blit(c.rt, c.arg(0), c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.arg(5), c.iarg(6), c.iarg(7), 0, 0,
                    c.arg(8), false));
  });
  r.impl(G, "StretchBlt", [](Call& c) {
    c.rt.charge_pixels(int64_t(std::abs(c.iarg(3))) * std::abs(c.iarg(4)));
    c.ret_bool(blit(c.rt, c.arg(0), c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.arg(5), c.iarg(6), c.iarg(7),
                    c.iarg(8), c.iarg(9), c.arg(10), true));
  });
  r.impl(G, "PatBlt", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    if (!dc) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(c.iarg(3))) * std::abs(c.iarg(4)));
    sync_brush(c.rt, c.arg(0));
    c.ret_bool(::PatBlt(dc, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.arg(5)));
  });
  r.impl(G, "SetDIBitsToDevice", [](Call& c) {
    // (hdc, xd, yd, w, h, xs, ys, start, lines, bits, bmi, usage)
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    uint32_t usage = c.arg(11), lines = c.arg(8);
    GuestDib d;
    if (!dc || !read_bitmapinfo(c.mem(), c.arg(10), usage, d)) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::max(c.iarg(3), 0)) * std::max(c.iarg(4), 0));
    KeyBmi kb;
    std::vector<uint8_t> conv;
    const uint8_t* direct;
    if (!convert_dib(c.rt, d, usage, c.arg(9), lines, c.arg(0), kb, conv, &direct)) {
      log("gdi: SetDIBitsToDevice from %u bpp (compression %u) is not supported", d.h.biBitCount,
          unsigned(d.h.biCompression));
      return c.ret(0);
    }
    ::GdiFlush();
    int n = ::SetDIBitsToDevice(dc, c.iarg(1), c.iarg(2), c.arg(3), c.arg(4), c.iarg(5), c.iarg(6), c.arg(7), lines,
                                direct ? direct : conv.data(), reinterpret_cast<BITMAPINFO*>(&kb), DIB_RGB_COLORS);
    c.ret(uint32_t(n));
  });
  r.impl(G, "StretchDIBits", [](Call& c) {
    // (hdc, xd, yd, wd, hd, xs, ys, ws, hs, bits, bmi, usage, rop)
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    uint32_t usage = c.arg(11);
    GuestDib d;
    if (!dc || !read_bitmapinfo(c.mem(), c.arg(10), usage, d)) return c.ret(0);
    c.rt.charge_pixels(int64_t(std::abs(c.iarg(3))) * std::abs(c.iarg(4)));
    KeyBmi kb;
    std::vector<uint8_t> conv;
    const uint8_t* direct;
    if (!convert_dib(c.rt, d, usage, c.arg(9), uint32_t(d.height), c.arg(0), kb, conv, &direct)) {
      log("gdi: StretchDIBits from %u bpp (compression %u) is not supported", d.h.biBitCount, unsigned(d.h.biCompression));
      return c.ret(0);
    }
    ::GdiFlush();
    sync_brush(c.rt, c.arg(0));
    int n = ::StretchDIBits(dc, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.iarg(5), c.iarg(6), c.iarg(7),
                            c.iarg(8), direct ? direct : conv.data(), reinterpret_cast<BITMAPINFO*>(&kb),
                            DIB_RGB_COLORS, c.arg(12));
    c.ret(uint32_t(n));
  });

  // ---- pixels ----
  auto set_pixel = [](Call& c) -> COLORREF {
    GdiTable& g = gt(c.rt);
    HDC dc = g.host_dc(c.arg(0));
    if (!dc) return CLR_INVALID;
    COLORREF k = g.key_color(c.arg(0), c.arg(3));
    if (!::SetPixelV(dc, c.iarg(1), c.iarg(2), k)) return CLR_INVALID;
    GdiBitmap* s = g.dc_surface(c.arg(0));
    return s ? g.surface_rgb(*s, GetRValue(k), c.arg(0)) : guest_rgb(c.arg(3));
  };
  r.impl(G, "SetPixel", [set_pixel](Call& c) { c.ret(set_pixel(c)); });
  r.impl(G, "SetPixelV", [set_pixel](Call& c) { c.ret_bool(set_pixel(c) != CLR_INVALID); });
  r.impl(G, "GetPixel", [](Call& c) {
    GdiTable& g = gt(c.rt);
    HDC dc = g.host_dc(c.arg(0));
    if (!dc) return c.ret(CLR_INVALID);
    ::GdiFlush();
    COLORREF k = ::GetPixel(dc, c.iarg(1), c.iarg(2));
    if (k == CLR_INVALID) return c.ret(CLR_INVALID);
    GdiBitmap* s = g.dc_surface(c.arg(0));
    c.ret(s && s->bpp <= 8 ? g.surface_rgb(*s, GetRValue(k), c.arg(0)) : k);
  });

  // ---- pens, brushes, fonts ----
  r.impl(G, "CreatePen", [](Call& c) {
    GdiObject o;
    o.type = GdiType::pen;
    o.pen_style = c.iarg(0);
    o.pen_width = c.iarg(1);
    o.color = c.arg(2);
    // Made for no particular DC (plain RGB → nearest static); SelectObject
    // re-makes it for the DC's palette when that maps it elsewhere.
    COLORREF k = Display::key_color(gt(c.rt).display().map_index(c.arg(2), nullptr));
    o.host = ::CreatePen(o.pen_style, o.pen_width, k);
    o.made_for = k;
    if (!o.host) return c.ret(0);
    c.ret(gt(c.rt).add(std::move(o)));
  });
  r.impl(G, "CreateSolidBrush", [](Call& c) {
    GdiObject o;
    o.type = GdiType::brush;
    o.brush_style = BS_SOLID;
    o.color = c.arg(0);
    COLORREF k = Display::key_color(gt(c.rt).display().map_index(c.arg(0), nullptr));
    o.host = ::CreateSolidBrush(k);
    o.made_for = k;
    if (!o.host) return c.ret(0);
    c.ret(gt(c.rt).add(std::move(o)));
  });
  r.impl(G, "CreateFontA", [](Call& c) {
    // Antialiasing would blend key colours, i.e. indices: always off.
    std::string face = c.str(13);
    HFONT f = ::CreateFontA(c.iarg(0), c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.arg(5), c.arg(6), c.arg(7),
                            c.arg(8), c.arg(9), c.arg(10), NONANTIALIASED_QUALITY, c.arg(12),
                            face.empty() ? nullptr : face.c_str());
    if (!f) return c.ret(0);
    GdiObject o;
    o.type = GdiType::font;
    o.host = f;
    c.ret(gt(c.rt).add(std::move(o)));
  });

  // ---- lines and shapes ----
  r.impl(G, "MoveToEx", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    POINT old{};
    if (!dc || !::MoveToEx(dc, c.iarg(1), c.iarg(2), &old)) return c.ret(0);
    if (c.arg(3)) write_pod(c.mem(), c.arg(3), old);
    c.ret(1);
  });
  r.impl(G, "LineTo", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    if (!dc) return c.ret(0);
    sync_pen(c.rt, c.arg(0));
    c.ret_bool(::LineTo(dc, c.iarg(1), c.iarg(2)));
  });
  r.impl(G, "Polyline", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    auto pts = read_points(c.mem(), c.arg(1), c.iarg(2));
    if (!dc || pts.empty()) return c.ret(0);
    sync_pen(c.rt, c.arg(0));
    c.ret_bool(::Polyline(dc, pts.data(), int(pts.size())));
  });
  r.impl(G, "Polygon", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    auto pts = read_points(c.mem(), c.arg(1), c.iarg(2));
    if (!dc || pts.empty()) return c.ret(0);
    sync_pen(c.rt, c.arg(0));
    sync_brush(c.rt, c.arg(0));
    c.ret_bool(::Polygon(dc, pts.data(), int(pts.size())));
  });
  r.impl(G, "Rectangle", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    if (!dc) return c.ret(0);
    sync_pen(c.rt, c.arg(0));
    sync_brush(c.rt, c.arg(0));
    c.ret_bool(::Rectangle(dc, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4)));
  });
  r.impl(G, "Ellipse", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    if (!dc) return c.ret(0);
    sync_pen(c.rt, c.arg(0));
    sync_brush(c.rt, c.arg(0));
    c.ret_bool(::Ellipse(dc, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4)));
  });

  // ---- text ----
  r.impl(G, "TextOutA", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    int32_t n = c.iarg(4);
    if (!dc || n < 0) return c.ret(0);
    std::string s = c.mem().read(c.arg(3), uint32_t(n));
    c.ret_bool(::TextOutA(dc, c.iarg(1), c.iarg(2), s.data(), n));
  });
  r.impl(G, "ExtTextOutA", [](Call& c) {
    // (hdc, x, y, options, rect*, str, n, dx*)
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    int32_t n = c.iarg(6);
    if (!dc || n < 0) return c.ret(0);
    std::string s = c.mem().read(c.arg(5), uint32_t(n));
    RECT rc{};
    if (c.arg(4)) rc = c.pod<RECT>(4);
    std::vector<INT> dx;
    if (c.arg(7)) {
      dx.resize(size_t(n));
      c.mem().memcpy(dx.data(), c.arg(7), sizeof(INT) * size_t(n));
    }
    c.ret_bool(::ExtTextOutA(dc, c.iarg(1), c.iarg(2), c.arg(3), c.arg(4) ? &rc : nullptr, s.data(), UINT(n),
                             dx.empty() ? nullptr : dx.data()));
  });
  r.impl(G, "GetTextExtentPoint32A", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    int32_t n = c.iarg(2);
    SIZE sz{};
    if (!dc || n < 0) return c.ret(0);
    std::string s = c.mem().read(c.arg(1), uint32_t(n));
    if (!::GetTextExtentPoint32A(dc, s.data(), n, &sz)) return c.ret(0);
    write_pod(c.mem(), c.arg(3), sz);
    c.ret(1);
  });

  // ---- regions (real) ----
  r.impl(G, "CreateRectRgn", [](Call& c) {
    c.ret(make_region(c.rt, ::CreateRectRgn(c.iarg(0), c.iarg(1), c.iarg(2), c.iarg(3))));
  });
  r.impl(G, "CreateRectRgnIndirect", [](Call& c) {
    RECT rc = c.pod<RECT>(0);
    c.ret(make_region(c.rt, ::CreateRectRgnIndirect(&rc)));
  });
  r.impl(G, "CreateEllipticRgn", [](Call& c) {
    c.ret(make_region(c.rt, ::CreateEllipticRgn(c.iarg(0), c.iarg(1), c.iarg(2), c.iarg(3))));
  });
  r.impl(G, "CreatePolygonRgn", [](Call& c) {
    auto pts = read_points(c.mem(), c.arg(0), c.iarg(1));
    if (pts.empty()) return c.ret(0);
    c.ret(make_region(c.rt, ::CreatePolygonRgn(pts.data(), int(pts.size()), c.iarg(2))));
  });
  r.impl(G, "CombineRgn", [](Call& c) {
    HRGN d = host_region(c.rt, c.arg(0)), a = host_region(c.rt, c.arg(1)), b = host_region(c.rt, c.arg(2));
    if (!d || !a) return c.ret(ERROR);
    c.ret(uint32_t(::CombineRgn(d, a, b, c.iarg(3))));
  });
  r.impl(G, "EqualRgn", [](Call& c) {
    HRGN a = host_region(c.rt, c.arg(0)), b = host_region(c.rt, c.arg(1));
    c.ret(a && b ? uint32_t(::EqualRgn(a, b)) : ERROR);
  });
  r.impl(G, "OffsetRgn", [](Call& c) {
    HRGN a = host_region(c.rt, c.arg(0));
    c.ret(a ? uint32_t(::OffsetRgn(a, c.iarg(1), c.iarg(2))) : ERROR);
  });
  r.impl(G, "PtInRegion", [](Call& c) {
    HRGN a = host_region(c.rt, c.arg(0));
    c.ret_bool(a && ::PtInRegion(a, c.iarg(1), c.iarg(2)));
  });
  r.impl(G, "RectInRegion", [](Call& c) {
    HRGN a = host_region(c.rt, c.arg(0));
    RECT rc = c.pod<RECT>(1);
    c.ret_bool(a && ::RectInRegion(a, &rc));
  });
  r.impl(G, "GetRgnBox", [](Call& c) {
    HRGN a = host_region(c.rt, c.arg(0));
    RECT rc{};
    if (!a) return c.ret(0);
    int t = ::GetRgnBox(a, &rc);
    write_pod(c.mem(), c.arg(1), rc);
    c.ret(uint32_t(t));
  });
  r.impl(G, "SelectClipRgn", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    if (!dc) return c.ret(ERROR);
    c.ret(uint32_t(::SelectClipRgn(dc, c.arg(1) ? host_region(c.rt, c.arg(1)) : nullptr)));
  });
  r.impl(G, "ExcludeClipRect", [](Call& c) {
    HDC dc = gt(c.rt).host_dc(c.arg(0));
    c.ret(dc ? uint32_t(::ExcludeClipRect(dc, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4))) : ERROR);
  });
  r.impl(G, "FillRgn", [](Call& c) {
    GdiTable& g = gt(c.rt);
    HDC dc = g.host_dc(c.arg(0));
    HRGN rg = host_region(c.rt, c.arg(1));
    HBRUSH b = static_cast<HBRUSH>(g.realize_for(c.arg(2), c.arg(0)));
    c.ret_bool(dc && rg && b && ::FillRgn(dc, rg, b));
  });
  r.impl(G, "FrameRgn", [](Call& c) {
    GdiTable& g = gt(c.rt);
    HDC dc = g.host_dc(c.arg(0));
    HRGN rg = host_region(c.rt, c.arg(1));
    HBRUSH b = static_cast<HBRUSH>(g.realize_for(c.arg(2), c.arg(0)));
    c.ret_bool(dc && rg && b && ::FrameRgn(dc, rg, b, c.iarg(3), c.iarg(4)));
  });
}

}  // namespace adw::win32
