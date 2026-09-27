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

Gdi16::Gdi16(Runtime16& rt) : rt_(rt) {}

Gdi16::~Gdi16() {
  // DCs first (they hold the other objects selected), then everything else.
  for (size_t i = 0; i < slots_.size(); i++) {
    if (slots_[i].type == G16::dc) destroy(uint16_t(kFirst + i * kStep));
  }
  for (Obj16& o : slots_) {
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
  return o ? static_cast<HDC>(o->host) : nullptr;
}

Dc16* Gdi16::dc(uint16_t h) {
  Obj16* o = get(h, G16::dc);
  return o ? &o->dc : nullptr;
}

bool Gdi16::destroy(uint16_t h) {
  Obj16* o = get(h);
  if (!o) return false;
  if (o->stock) return true;
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
      free_realizations(h);
      break;
    default:
      break;
  }
  size_t i = (h - kFirst) / kStep;
  slots_[i] = Obj16{};
  free_.push_back(uint16_t(i));
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
  Obj16* b = get(d->s.bitmap, G16::bitmap);
  return b ? &b->bmp : nullptr;
}

COLORREF Gdi16::index_rgb(int index) { return display().hardware_color(index); }

COLORREF Gdi16::key(uint16_t hdc, COLORREF c) {
  Display& d = display();
  int idx = d.map_index(c, dc_palette(hdc));
  Bitmap16* s = dc_surface(hdc);
  // A monochrome surface converts real colours (by intensity), not key colours.
  if (s && s->bpp == 1) return d.hardware_color(idx);
  return Display::key_color(idx);
}

HGDIOBJ Gdi16::realize_brush(Obj16& o, uint16_t hdc) {
  if (o.style == BS_NULL || o.style == BS_HOLLOW) return GetStockObject(NULL_BRUSH);
  if (o.style == BS_PATTERN) {
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
      if (d.screen || d.info) return 0;
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
