#include "win16/gdi16.hh"

#include <algorithm>

#include "adw/core/log.h"

namespace adw::win16 {

using win32::Display;
using win32::LogicalPalette;

const char* g16_name(G16 t) {
  switch (t) {
    case G16::free: return "free";
    case G16::dc: return "DC";
    case G16::bitmap: return "bitmap";
    case G16::brush: return "brush";
    case G16::pen: return "pen";
    case G16::font: return "font";
    case G16::region: return "region";
    case G16::palette: return "palette";
  }
  return "?";
}

namespace {

// The 16 VGA colours in their Windows order (black, dark red, dark green,
// dark yellow, dark blue, dark magenta, dark cyan, light grey, dark grey,
// red, green, yellow, blue, magenta, cyan, white): the table DIB.DRV matches
// colours against when a DIB has no usable colour table (1:06FC → 1:0710).
constexpr RGBQUAD kVga16[16] = {
    {0, 0, 0, 0},       {0, 0, 0x80, 0},    {0, 0x80, 0, 0},    {0, 0x80, 0x80, 0},
    {0x80, 0, 0, 0},    {0x80, 0, 0x80, 0}, {0x80, 0x80, 0, 0}, {0xC0, 0xC0, 0xC0, 0},
    {0x80, 0x80, 0x80, 0}, {0, 0, 0xFF, 0}, {0, 0xFF, 0, 0},    {0, 0xFF, 0xFF, 0},
    {0xFF, 0, 0, 0},    {0xFF, 0, 0xFF, 0}, {0xFF, 0xFF, 0, 0}, {0xFF, 0xFF, 0xFF, 0}};

void free_dib_brush(Obj16& o) {
  if (o.dib_brush) DeleteObject(o.dib_brush);
  if (o.dib_brush_bitmap) DeleteObject(o.dib_brush_bitmap);
  o.dib_brush = nullptr;
  o.dib_brush_bitmap = nullptr;
}

}  // namespace

Gdi16::Gdi16(Runtime16& rt) : rt_(rt) {}

Gdi16::~Gdi16() {
  // DCs first (they hold the other objects selected), then everything else.
  for (size_t i = 0; i < slots_.size(); i++) {
    if (slots_[i].type == G16::dc) destroy(uint16_t(kFirst + i * kStep));
  }
  for (Obj16& o : slots_) {
    if (o.type == G16::brush) free_dib_brush(o);
    if (o.type == G16::free || !o.host) continue;
    if (o.type == G16::font && o.stock) continue;  // real stock fonts
    DeleteObject(o.host);
  }
  for (auto& [h, list] : extra_) {
    for (const Realization& r : list) DeleteObject(r.obj);
  }
  if (scratch_dc_) {
    if (scratch_old_) SelectObject(scratch_dc_, scratch_old_);
    DeleteDC(scratch_dc_);
  }
  if (scratch_bmp_) DeleteObject(scratch_bmp_);
}

Display& Gdi16::display() {
  Display* d = rt_.display();
  if (!d) throw GuestError16(GuestError16::Kind::fatal, "GDI call before the display was attached");
  return *d;
}

uint16_t Gdi16::add(Obj16 o) {
  size_t i;
  if (!free_.empty()) {
    i = free_.back();
    free_.pop_back();
    o.self = uint16_t(kFirst + i * kStep);
    slots_[i] = std::move(o);
  } else {
    i = slots_.size();
    if (kFirst + i * kStep >= kLimit) {
      if (o.host && o.type != G16::dc) DeleteObject(o.host);
      log("win16 gdi: the handle table is full");
      return 0;
    }
    o.self = uint16_t(kFirst + i * kStep);
    slots_.push_back(std::move(o));
  }
  return uint16_t(kFirst + i * kStep);
}

Obj16* Gdi16::get(uint16_t h) {
  if (h < kFirst || h >= kLimit || (h - kFirst) % kStep) return nullptr;
  size_t i = (h - kFirst) / kStep;
  if (i >= slots_.size() || slots_[i].type == G16::free) return nullptr;
  return &slots_[i];
}

Obj16* Gdi16::get(uint16_t h, G16 t) {
  Obj16* o = get(h);
  return o && o->type == t ? o : nullptr;
}

HGDIOBJ Gdi16::host(uint16_t h) {
  Obj16* o = get(h);
  return o ? o->host : nullptr;
}

HDC Gdi16::host_dc(uint16_t h) {
  Obj16* o = get(h, G16::dc);
  if (!o) return nullptr;
  // The guest reads a DIB DC's bits itself: whatever real GDI does with this
  // DC now is finished before the guest runs on.
  if (o->dc.dib_device) rt_.flush_gdi_after_call();
  return static_cast<HDC>(o->host);
}

Dc16* Gdi16::dc(uint16_t h) {
  Obj16* o = get(h, G16::dc);
  return o ? &o->dc : nullptr;
}

bool Gdi16::destroy(uint16_t h) {
  Obj16* o = get(h);
  if (!o) return false;
  if (o->stock) return true;
  // A DIB pattern brush's own bitmap (never selected: the guest never saw it)
  // goes with it — after the brush's slot, so the next such pair gets the
  // same two handles back — if it is still the one made for this brush.
  uint16_t own_pattern = 0;
  if (o->type == G16::brush) {
    Obj16* b = get(o->pattern, G16::bitmap);
    if (b && b->pattern_of == h) own_pattern = o->pattern;
  }
  switch (o->type) {
    case G16::dc: {
      HDC d = static_cast<HDC>(o->host);
      if (Obj16* b = get(o->dc.s.bitmap, G16::bitmap)) b->bmp.selected_in = 0;
      if (d) {
        // Put the DC's own objects back so the guest's can be deleted later.
        SelectObject(d, GetStockObject(WHITE_BRUSH));
        SelectObject(d, GetStockObject(BLACK_PEN));
        SelectObject(d, GetStockObject(SYSTEM_FONT));
        if (o->dc.old_bitmap) SelectObject(d, o->dc.old_bitmap);
        DeleteDC(d);
      }
      if (o->dc.surface) DeleteObject(o->dc.surface);
      break;
    }
    case G16::bitmap:
      if (o->bmp.selected_in) return false;  // Windows refuses to delete a selected bitmap
      if (o->host) DeleteObject(o->host);
      break;
    case G16::brush:
    case G16::pen:
    case G16::font:
    case G16::region:
      if (o->host) DeleteObject(o->host);
      if (o->type == G16::brush) free_dib_brush(*o);
      free_realizations(h);
      break;
    default:
      break;
  }
  size_t i = (h - kFirst) / kStep;
  slots_[i] = Obj16{};
  free_.push_back(uint16_t(i));
  if (own_pattern) destroy(own_pattern);
  return true;
}

// ---- stock objects ---------------------------------------------------------------------------------

void Gdi16::init_stock() {
  if (stock_[0]) return;
  auto brush = [&](int idx, COLORREF c, int style) {
    Obj16 o;
    o.type = G16::brush;
    o.stock = true;
    o.color = c;
    o.style = style;
    stock_[idx] = add(o);
  };
  auto pen = [&](int idx, COLORREF c, int style) {
    Obj16 o;
    o.type = G16::pen;
    o.stock = true;
    o.color = c;
    o.style = style;
    o.width = 1;
    stock_[idx] = add(o);
  };
  brush(WHITE_BRUSH, RGB(255, 255, 255), BS_SOLID);
  brush(LTGRAY_BRUSH, RGB(192, 192, 192), BS_SOLID);
  brush(GRAY_BRUSH, RGB(128, 128, 128), BS_SOLID);
  brush(DKGRAY_BRUSH, RGB(64, 64, 64), BS_SOLID);
  brush(BLACK_BRUSH, RGB(0, 0, 0), BS_SOLID);
  brush(NULL_BRUSH, 0, BS_NULL);
  pen(WHITE_PEN, RGB(255, 255, 255), PS_SOLID);
  pen(BLACK_PEN, RGB(0, 0, 0), PS_SOLID);
  pen(NULL_PEN, 0, PS_NULL);
  for (int idx : {OEM_FIXED_FONT, ANSI_FIXED_FONT, ANSI_VAR_FONT, SYSTEM_FONT, DEVICE_DEFAULT_FONT,
                  SYSTEM_FIXED_FONT}) {
    Obj16 o;
    o.type = G16::font;
    o.stock = true;
    o.host = GetStockObject(idx);
    GetObjectA(o.host, sizeof(o.font), &o.font);
    stock_[idx] = add(o);
  }
  Obj16 p;
  p.type = G16::palette;
  p.stock = true;
  p.pal = std::make_shared<LogicalPalette>(Display::default_palette());
  stock_[DEFAULT_PALETTE] = add(p);
  // The 1x1 monochrome bitmap every memory DC starts with (what SelectObject
  // hands back for it): selectable into any number of DCs, never deleted.
  Obj16 b;
  b.type = G16::bitmap;
  b.stock = true;
  b.bmp.w = 1;
  b.bmp.h = 1;
  b.bmp.bpp = 1;
  b.bmp.stride = 2;
  stock_[19] = add(b);
}

uint16_t Gdi16::stock(int index) {
  init_stock();
  if (index < 0 || index >= 20) return 0;
  return stock_[index];
}

uint16_t Gdi16::create_brush(COLORREF c, int style, int hatch) {
  Obj16 o;
  o.type = G16::brush;
  o.color = c;
  o.style = style;
  o.hatch = hatch;
  return add(o);
}

uint16_t Gdi16::create_pen(int style, int width, COLORREF c) {
  Obj16 o;
  o.type = G16::pen;
  o.color = c;
  o.style = style;
  o.width = width;
  return add(o);
}

uint16_t Gdi16::create_palette(const std::vector<PALETTEENTRY>& entries) {
  Obj16 o;
  o.type = G16::palette;
  o.pal = std::make_shared<LogicalPalette>();
  o.pal->entries = entries;
  return add(o);
}

uint16_t Gdi16::wrap_region(HRGN r) {
  if (!r) return 0;
  Obj16 o;
  o.type = G16::region;
  o.host = r;
  return add(o);
}

uint16_t Gdi16::create_device_bitmap(int w, int h, int bpp) {
  w = std::max(w, 1);
  h = std::max(h, 1);
  Obj16 o;
  o.type = G16::bitmap;
  o.bmp.w = w;
  o.bmp.h = h;
  if (bpp == 1) {
    o.host = CreateBitmap(w, h, 1, 1, nullptr);
    o.bmp.bpp = 1;
    o.bmp.stride = uint32_t(((w + 15) / 16) * 2);
  } else {
    uint8_t* bits = nullptr;
    o.host = Display::create_surface8(w, h, /*top_down=*/true, nullptr, 0, &bits);
    o.bmp.bpp = 8;
    o.bmp.bits = bits;
    o.bmp.stride = Display::pitch_for(w);
  }
  if (!o.host) return 0;
  return add(o);
}

// ---- DCs ---------------------------------------------------------------------------------------------

uint16_t Gdi16::create_screen_dc(uint16_t hwnd) {
  Display& d = display();
  init_stock();
  if (!screen_bmp_.bits) {
    screen_bmp_.w = d.width();
    screen_bmp_.h = d.height();
    screen_bmp_.bpp = 8;
    screen_bmp_.bits = d.bits();
    screen_bmp_.stride = d.pitch();
  }
  HDC hdc = CreateCompatibleDC(nullptr);
  if (!hdc) return 0;
  auto [section, offset] = rt_.mem().section_for(d.bits_addr());
  uint8_t* bits = nullptr;
  HBITMAP s = Display::create_surface8(d.width(), d.height(), true, static_cast<HANDLE>(section), offset, &bits);
  if (!s) {
    DeleteDC(hdc);
    return 0;
  }
  Obj16 o;
  o.type = G16::dc;
  o.host = hdc;
  o.dc.screen = true;
  o.dc.surface = s;
  o.dc.hwnd = hwnd;
  o.dc.old_bitmap = SelectObject(hdc, s);
  o.dc.s.brush = stock(WHITE_BRUSH);
  o.dc.s.pen = stock(BLACK_PEN);
  o.dc.s.font = stock(SYSTEM_FONT);
  o.dc.s.palette = stock(DEFAULT_PALETTE);
  uint16_t h = add(o);
  if (h) sync(h);
  return h;
}

void Gdi16::release_dc(uint16_t h) {
  Obj16* o = get(h, G16::dc);
  if (o && o->dc.screen) destroy(h);
}

uint16_t Gdi16::create_memory_dc() {
  init_stock();
  HDC hdc = CreateCompatibleDC(nullptr);
  if (!hdc) return 0;
  Obj16 o;
  o.type = G16::dc;
  o.host = hdc;
  o.dc.old_bitmap = GetCurrentObject(hdc, OBJ_BITMAP);
  o.dc.s.bitmap = stock_[19];
  o.dc.s.brush = stock(WHITE_BRUSH);
  o.dc.s.pen = stock(BLACK_PEN);
  o.dc.s.font = stock(SYSTEM_FONT);
  o.dc.s.palette = stock(DEFAULT_PALETTE);
  uint16_t h = add(o);
  if (h) sync(h);
  return h;
}

uint16_t Gdi16::create_dib_dc(uint32_t packed) {
  init_stock();
  if (!packed) {
    log("win16 gdi: CreateDC(\"DIB\") without a DIB: refused");
    return 0;
  }
  // DIB.DRV's Enable (1:42E9): a BITMAPINFOHEADER, one plane, BI_RGB, 1, 4 or
  // 8 bits per pixel; the bits after a colour table of biClrUsed (else
  // 1 << biBitCount) four-byte entries, whatever the table holds.
  BITMAPINFOHEADER h = read16<BITMAPINFOHEADER>(rt_, packed);
  if (h.biSize != sizeof(BITMAPINFOHEADER) || h.biPlanes != 1 || h.biCompression != BI_RGB ||
      (h.biBitCount != 1 && h.biBitCount != 4 && h.biBitCount != 8)) {
    log("win16 gdi: CreateDC(\"DIB\"): not a DIB the DIB driver takes (size %u, %u planes, %u bpp, compression %u)",
        unsigned(h.biSize), h.biPlanes, h.biBitCount, unsigned(h.biCompression));
    return 0;
  }
  if (h.biBitCount != 8) {
    log("win16 gdi: CreateDC(\"DIB\"): %u-bit DIBs are not supported (8 bits per pixel only)", h.biBitCount);
    return 0;
  }
  int w = h.biWidth, ht = h.biHeight < 0 ? -h.biHeight : h.biHeight;
  bool top_down = h.biHeight < 0;
  if (w <= 0 || ht <= 0 || w > 16384 || ht > 16384) {
    log("win16 gdi: CreateDC(\"DIB\"): a %dx%d DIB: refused", w, int(h.biHeight));
    return 0;
  }
  uint16_t used = uint16_t(h.biClrUsed);
  uint32_t table = 4u * (used ? used : 256u);
  uint32_t stride = Display::pitch_for(w);
  uint32_t bits_fp = Runtime16::huge_add(packed, h.biSize + table);
  // The whole surface in one block (a huge block's first tile's limit covers
  // it all); the view must start on a DWORD, as CreateDIBSection wants.
  uint32_t lin = rt_.linear(bits_fp, stride * uint32_t(ht));
  auto [section, offset] = rt_.mem().section_for(lin);
  if (!section || (offset & 3)) {
    log("win16 gdi: CreateDC(\"DIB\"): the bits at %04X:%04X cannot be mapped (offset %u)", bits_fp >> 16,
        bits_fp & 0xFFFF, uint32_t(offset));
    return 0;
  }
  HDC hdc = CreateCompatibleDC(nullptr);
  if (!hdc) return 0;
  uint8_t* bits = nullptr;
  HBITMAP s = Display::create_surface8(w, ht, top_down, static_cast<HANDLE>(section), offset, &bits);
  if (!s) {
    DeleteDC(hdc);
    log("win16 gdi: CreateDC(\"DIB\"): CreateDIBSection failed (error %lu)", GetLastError());
    return 0;
  }
  Obj16 o;
  o.type = G16::dc;
  o.host = hdc;
  o.dc.dib_device = true;
  o.dc.dib_header = packed;
  o.dc.surface = s;
  o.dc.old_bitmap = SelectObject(hdc, s);
  o.dc.dib_bmp.w = w;
  o.dc.dib_bmp.h = ht;
  o.dc.dib_bmp.bpp = 8;
  o.dc.dib_bmp.bits = bits;
  o.dc.dib_bmp.stride = stride;
  o.dc.s.brush = stock(WHITE_BRUSH);
  o.dc.s.pen = stock(BLACK_PEN);
  o.dc.s.font = stock(SYSTEM_FONT);
  o.dc.s.palette = stock(DEFAULT_PALETTE);
  uint16_t dc = add(o);
  if (dc) sync(dc);
  trace("dib16", "CreateDC(\"DIB\", %04X:%04X): %dx%d %s -> %04X", packed >> 16, packed & 0xFFFF, w, ht,
        top_down ? "top-down" : "bottom-up", dc);
  return dc;
}

Gdi16::DibTable Gdi16::dib_table(uint32_t header) {
  DibTable t;
  if (!header) return t;
  try {
    BITMAPINFOHEADER h = read16<BITMAPINFOHEADER>(rt_, header);
    // 1:0749: biClrUsed's low word, else 1 << biBitCount (none at 24).
    uint32_t count = uint16_t(h.biClrUsed) ? uint16_t(h.biClrUsed) : (h.biBitCount == 24 ? 0 : 1u << std::min<int>(h.biBitCount, 8));
    if (!count || h.biSize < sizeof(BITMAPINFOHEADER) || h.biSize > 0x1000) return t;
    // 1:0724..1:0743: the first biClrImportant (else `count`) WORDs are 0, 1,
    // 2, …: an index table, which the driver matches as the 16 VGA colours.
    uint32_t check = uint16_t(h.biClrImportant) ? uint16_t(h.biClrImportant) : count;
    uint32_t table = header + h.biSize;
    std::vector<uint8_t> bytes(std::max(count * 4, check * 2));
    rt_.read_bytes(table, bytes.data(), bytes.size());
    bool identity = true;
    for (uint32_t i = 0; i < check && identity; i++) identity = (bytes[2 * i] | (bytes[2 * i + 1] << 8)) == int(i);
    if (identity) return t;
    t.vga = false;
    t.colors.resize(count);
    memcpy(t.colors.data(), bytes.data(), count * 4);
  } catch (const GuestError16&) {
    // The header is gone (the guest freed the DIB): nothing to match against.
  }
  return t;
}

int Gdi16::dib_index(const Dc16& d, COLORREF c) {
  // 1:0602: DIBINDEX(n) (0x10FFxxxx), PALETTEINDEX(n) and physical colours
  // (the flag byte's top bit) are pixel value n's low byte.
  uint8_t flags = uint8_t(c >> 24);
  if ((c >> 16) == 0x10FF || flags == 0x01 || (flags & 0x80)) return int(c & 0xFF);
  // Everything else, the flag byte ignored: the nearest table entry (1:075C,
  // squared distance, the first of equals).
  DibTable t = dib_table(d.dib_header);
  if (t.vga) return Display::nearest_in(kVga16, 16, c & 0xFFFFFF);
  return Display::nearest_in(t.colors.data(), int(t.colors.size()), c & 0xFFFFFF);
}

COLORREF Gdi16::surface_rgb(uint16_t hdc, int index) {
  Dc16* d = dc(hdc);
  if (!d || !d->dib_header) return index_rgb(index);
  // The colour ColorInfo reports for a pixel value: its table entry (the VGA
  // colour for an index table; past those 16 the driver read on into its own
  // code, and the hardware colour is as good an answer).
  DibTable t = dib_table(d->dib_header);
  index &= 0xFF;
  if (t.vga) {
    if (index < 16) return RGB(kVga16[index].rgbRed, kVga16[index].rgbGreen, kVga16[index].rgbBlue);
    return index_rgb(index);
  }
  if (size_t(index) >= t.colors.size()) return 0;
  const RGBQUAD& q = t.colors[size_t(index)];
  return RGB(q.rgbRed, q.rgbGreen, q.rgbBlue);
}

int Gdi16::dib_device_caps(const Dc16& d, int index) {
  // DIB.DRV's GDIINFO (2:0038), the resolution and depth from the DIB (Enable,
  // 2:0182..2:019D) and the display's RC_BIGFONT added (2:0128..2:0177). Not
  // a palette device: no RC_PALETTE, SIZEPALETTE 0.
  switch (index) {
    case DRIVERVERSION: return 0x0300;
    case TECHNOLOGY: return DT_RASDISPLAY;
    case HORZSIZE: return 240;
    case VERTSIZE: return 180;
    case HORZRES: return d.dib_bmp.w;
    case VERTRES: return d.dib_bmp.h;
    case BITSPIXEL: return d.dib_bmp.bpp;
    case PLANES: return 1;
    case NUMBRUSHES: return -1;
    case NUMPENS: return 0x0500;
    case NUMCOLORS: return 256;
    case PDEVICESIZE: return 0x30;
    case POLYGONALCAPS: return PC_SCANLINE;
    case TEXTCAPS: return 0x2004;
    case RASTERCAPS: return 0x2299 | (display().device_caps(RASTERCAPS) & 0x0400);  // RC_BIGFONT
    case ASPECTX: return 36;
    case ASPECTY: return 36;
    case ASPECTXY: return 51;
    case LOGPIXELSX: return 96;
    case LOGPIXELSY: return 96;
    default: return 0;
  }
}

uint16_t Gdi16::create_ic() {
  init_stock();
  Obj16 o;
  o.type = G16::dc;
  o.host = CreateCompatibleDC(nullptr);
  o.dc.info = true;
  o.dc.old_bitmap = o.host ? GetCurrentObject(static_cast<HDC>(o.host), OBJ_BITMAP) : nullptr;
  o.dc.s.brush = stock(WHITE_BRUSH);
  o.dc.s.pen = stock(BLACK_PEN);
  o.dc.s.font = stock(SYSTEM_FONT);
  o.dc.s.palette = stock(DEFAULT_PALETTE);
  return add(o);
}

LogicalPalette* Gdi16::dc_palette(uint16_t hdc) {
  Dc16* d = dc(hdc);
  Obj16* p = d ? get(d->s.palette, G16::palette) : nullptr;
  if (!p) p = get(stock(DEFAULT_PALETTE), G16::palette);
  return p ? p->pal.get() : nullptr;
}

Bitmap16* Gdi16::dc_surface(uint16_t hdc) {
  Dc16* d = dc(hdc);
  if (!d || d->info) return nullptr;
  if (d->screen) return &screen_bmp_;
  if (d->dib_device) return &d->dib_bmp;
  Obj16* b = get(d->s.bitmap, G16::bitmap);
  return b ? &b->bmp : nullptr;
}

COLORREF Gdi16::index_rgb(int index) { return display().hardware_color(index); }

COLORREF Gdi16::key(uint16_t hdc, COLORREF c) {
  Display& d = display();
  Bitmap16* s = dc_surface(hdc);
  Dc16* dcs = dc(hdc);
  // A DIB DC (or a memory DC made compatible with one) on an 8-bit surface:
  // the DIB driver's pixel value, whatever palette is selected.
  if (dcs && dcs->dib_header && !(s && s->bpp == 1)) {
    int v = dib_index(*dcs, c);
    if (tracing("dib16") && (c >> 16) != 0x10FF && (c >> 24) != 0x01 && !(c & 0x80000000u)) {
      trace("dib16", "colour %08lX on DIB DC %04X -> pixel value %d (%s table)", c, hdc, v, dib_table(dcs->dib_header).vga ? "index" : "RGB");
    }
    return Display::key_color(v);
  }
  int idx = d.map_index(c, dc_palette(hdc));
  // A monochrome surface converts real colours (by intensity), not key colours.
  if (s && s->bpp == 1) return d.hardware_color(idx);
  return Display::key_color(idx);
}

HGDIOBJ Gdi16::dib_pattern_brush(Obj16& o, const Dc16& d) {
  if (o.dib_brush) return o.dib_brush;
  // The pattern DIB (the brush's copy): its top-left 8×8 pixels, their own
  // indices. Only an 8-bit pattern has indices to keep; others paint as on
  // any other DC.
  const std::vector<uint8_t>& p = o.dib_pattern;
  BITMAPINFOHEADER h{};
  if (p.size() >= sizeof(h)) memcpy(&h, p.data(), sizeof(h));
  if (h.biSize != sizeof(BITMAPINFOHEADER) || h.biBitCount != 8 || h.biCompression != BI_RGB || h.biWidth <= 0 ||
      h.biHeight == 0) {
    return nullptr;
  }
  int w = h.biWidth, ht = h.biHeight < 0 ? -h.biHeight : h.biHeight;
  bool top_down = h.biHeight < 0;
  uint32_t colors = h.biClrUsed ? std::min<uint32_t>(h.biClrUsed, 256) : 256;
  size_t bits = h.biSize + (o.dib_usage == DIB_PAL_COLORS ? 2 : 4) * size_t(colors);
  uint32_t stride = uint32_t((w + 3) & ~3);
  if (tracing("dib16") && !dib_table(d.dib_header).vga) {
    // Two real colour tables: DIB.DRV matched one to the other (1:0653);
    // the indices are kept here (SWSE's canvases all have index tables).
    trace("dib16", "brush %04X: a DIB pattern on a DIB with an RGB colour table keeps its indices", o.self);
  }
  uint8_t* out = nullptr;
  HBITMAP bmp = Display::create_surface8(8, 8, /*top_down=*/true, nullptr, 0, &out);
  if (!bmp) return nullptr;
  uint32_t ostride = Display::pitch_for(8);
  for (int y = 0; y < 8; y++) {
    int sy = y % ht;
    int row = top_down ? sy : ht - 1 - sy;  // the pattern's row y from its top
    for (int x = 0; x < 8; x++) {
      size_t at = bits + size_t(row) * stride + size_t(x % w);
      out[size_t(y) * ostride + size_t(x)] = at < p.size() ? p[at] : 0;
    }
  }
  o.dib_brush_bitmap = bmp;
  o.dib_brush = CreatePatternBrush(bmp);
  return o.dib_brush;
}

HGDIOBJ Gdi16::realize_brush(Obj16& o, uint16_t hdc) {
  if (o.style == BS_NULL || o.style == BS_HOLLOW) return GetStockObject(NULL_BRUSH);
  if (o.style == BS_PATTERN) {
    if (!o.dib_pattern.empty()) {
      Dc16* d = dc(hdc);
      if (d && d->dib_header) {
        if (HGDIOBJ b = dib_pattern_brush(o, *d)) return b;
      }
    }
    if (!o.host) {
      Obj16* b = get(o.pattern, G16::bitmap);
      if (b && b->host) o.host = CreatePatternBrush(static_cast<HBITMAP>(b->host));
    }
    return o.host;
  }
  COLORREF k = key(hdc, o.color);
  if (o.host && o.made_for == k) return o.host;
  if (reuse_realization(o, k)) return o.host;
  HGDIOBJ nb = o.style == BS_HATCHED ? CreateHatchBrush(o.hatch, k) : CreateSolidBrush(k);
  if (!nb) return o.host;
  keep_realization(o, nb, k);
  return nb;
}

HGDIOBJ Gdi16::realize_pen(Obj16& o, uint16_t hdc) {
  if (o.style == PS_NULL) return GetStockObject(NULL_PEN);
  COLORREF k = key(hdc, o.color);
  if (o.host && o.made_for == k) return o.host;
  if (reuse_realization(o, k)) return o.host;
  HGDIOBJ np = CreatePen(o.style, o.width, k);
  if (!np) return o.host;
  keep_realization(o, np, k);
  return np;
}

bool Gdi16::reuse_realization(Obj16& o, COLORREF k) {
  auto it = extra_.find(o.self);
  if (it == extra_.end()) return false;
  std::vector<Realization>& list = it->second;
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i].key != k) continue;
    HGDIOBJ obj = list[i].obj;
    list.erase(list.begin() + ptrdiff_t(i));
    if (o.host) list.push_back({o.made_for, o.host});  // the most recently used last
    o.host = obj;
    o.made_for = k;
    return true;
  }
  return false;
}

void Gdi16::keep_realization(Obj16& o, HGDIOBJ made, COLORREF k) {
  if (o.host) {
    std::vector<Realization>& list = extra_[o.self];
    list.push_back({o.made_for, o.host});
    for (size_t i = 0; list.size() > kMaxRealizations && i < list.size();) {
      if (selected_anywhere(list[i].obj)) {
        i++;
        continue;
      }
      DeleteObject(list[i].obj);
      list.erase(list.begin() + ptrdiff_t(i));
    }
  }
  o.host = made;
  o.made_for = k;
}

bool Gdi16::selected_anywhere(HGDIOBJ obj) {
  auto in = [obj](HDC d) {
    return d && (GetCurrentObject(d, OBJ_BRUSH) == obj || GetCurrentObject(d, OBJ_PEN) == obj);
  };
  if (in(scratch_dc_)) return true;
  for (const Obj16& s : slots_) {
    if (s.type == G16::dc && in(static_cast<HDC>(s.host))) return true;
  }
  return false;
}

void Gdi16::free_realizations(uint16_t h) {
  auto it = extra_.find(h);
  if (it == extra_.end()) return;
  for (const Realization& r : it->second) DeleteObject(r.obj);
  extra_.erase(it);
}

void Gdi16::sync_brush(uint16_t hdc, uint16_t brush) {
  HDC d = host_dc(hdc);
  Obj16* b = get(brush, G16::brush);
  if (!d || !b) return;
  if (HGDIOBJ real = realize_brush(*b, hdc)) SelectObject(d, real);
}

void Gdi16::sync(uint16_t hdc) {
  Obj16* o = get(hdc, G16::dc);
  if (!o || !o->host) return;
  HDC d = static_cast<HDC>(o->host);
  SetTextColor(d, key(hdc, o->dc.s.text));
  SetBkColor(d, key(hdc, o->dc.s.bk));
  if (Obj16* b = get(o->dc.s.brush, G16::brush)) {
    if (HGDIOBJ real = realize_brush(*b, hdc)) SelectObject(d, real);
  }
  if (Obj16* p = get(o->dc.s.pen, G16::pen)) {
    if (HGDIOBJ real = realize_pen(*p, hdc)) SelectObject(d, real);
  }
}

uint16_t Gdi16::select(uint16_t hdc, uint16_t h) {
  Obj16* dco = get(hdc, G16::dc);
  Obj16* o = get(h);
  if (!dco || !o) return 0;
  Dc16& d = dco->dc;
  HDC hd = static_cast<HDC>(dco->host);
  switch (o->type) {
    case G16::bitmap: {
      if (d.screen || d.info || d.dib_device) return 0;  // a device DC, not a memory DC
      if (h == d.s.bitmap) return h;
      if (!o->stock && o->bmp.selected_in && o->bmp.selected_in != hdc) return 0;
      HGDIOBJ real = o->stock ? d.old_bitmap : o->host;
      if (!real || !SelectObject(hd, real)) return 0;
      uint16_t prev = d.s.bitmap;
      if (Obj16* pb = get(prev, G16::bitmap)) pb->bmp.selected_in = 0;
      if (!o->stock) o->bmp.selected_in = hdc;
      d.s.bitmap = h;
      // The surface kind changed (mono ↔ colour): colours re-key.
      sync(hdc);
      return prev;
    }
    case G16::brush: {
      uint16_t prev = d.s.brush;
      d.s.brush = h;
      if (HGDIOBJ real = realize_brush(*o, hdc)) SelectObject(hd, real);
      return prev;
    }
    case G16::pen: {
      uint16_t prev = d.s.pen;
      d.s.pen = h;
      if (HGDIOBJ real = realize_pen(*o, hdc)) SelectObject(hd, real);
      return prev;
    }
    case G16::font: {
      uint16_t prev = d.s.font;
      d.s.font = h;
      if (o->host) SelectObject(hd, o->host);
      return prev;
    }
    case G16::region:
      // SelectObject with a region is SelectClipRgn: the result is the region type.
      return uint16_t(SelectClipRgn(hd, static_cast<HRGN>(o->host)));
    default:
      return 0;
  }
}

HDC Gdi16::scratch(int w, int h, uint8_t** bits, uint32_t* stride) {
  if (!scratch_dc_) scratch_dc_ = CreateCompatibleDC(nullptr);
  if (w > scratch_w_ || h > scratch_h_ || !scratch_bmp_) {
    int nw = std::max(w, scratch_w_), nh = std::max(h, scratch_h_);
    uint8_t* b = nullptr;
    HBITMAP bmp = Display::create_surface8(nw, nh, true, nullptr, 0, &b);
    if (!bmp) return nullptr;
    if (scratch_old_) SelectObject(scratch_dc_, scratch_old_);
    if (scratch_bmp_) DeleteObject(scratch_bmp_);
    scratch_bmp_ = bmp;
    scratch_bits_ = b;
    scratch_w_ = nw;
    scratch_h_ = nh;
    scratch_old_ = SelectObject(scratch_dc_, scratch_bmp_);
  }
  if (bits) *bits = scratch_bits_;
  if (stride) *stride = Display::pitch_for(scratch_w_);
  return scratch_dc_;
}

uint16_t gdi16_screen_dc(Runtime16& rt, uint16_t hwnd) { return rt.state<Gdi16>().create_screen_dc(hwnd); }

}  // namespace adw::win16
