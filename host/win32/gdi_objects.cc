#include "win32/gdi_objects.hh"

#include <algorithm>
#include <stdexcept>

namespace adw::win32 {

const char* gdi_type_name(GdiType t) {
  switch (t) {
    case GdiType::free: return "free";
    case GdiType::dc: return "DC";
    case GdiType::bitmap: return "bitmap";
    case GdiType::brush: return "brush";
    case GdiType::pen: return "pen";
    case GdiType::font: return "font";
    case GdiType::region: return "region";
    case GdiType::palette: return "palette";
  }
  return "?";
}


GdiTable::~GdiTable() {
  for (GdiObject& o : slots_) {
    if (o.type == GdiType::free || !o.owned || o.stock || !o.host) continue;
    if (o.type == GdiType::dc) DeleteDC(static_cast<HDC>(o.host));
  }
  // Objects after DCs, so none is still selected into a DC being deleted.
  for (GdiObject& o : slots_) {
    if (o.type == GdiType::free || o.type == GdiType::dc || !o.owned || o.stock || !o.host) continue;
    DeleteObject(o.host);
  }
  for (auto& [h, extra] : extra_objects_) {
    for (HGDIOBJ x : extra) DeleteObject(x);
  }
  for (GdiObject& o : slots_) {
    if (o.type == GdiType::bitmap && o.bmp.guest_bits_alloc) rt_.heap().free(o.bmp.guest_bits_alloc);
  }
}

uint32_t GdiTable::add(GdiObject obj) {
  size_t index;
  if (!free_.empty()) {
    // Lowest free handle first: deterministic, and handle values stay small.
    auto it = std::min_element(free_.begin(), free_.end());
    index = *it;
    free_.erase(it);
  } else {
    index = slots_.size();
    if (kFirst + index * kStep >= kLimit) return 0;
    slots_.emplace_back();
  }
  uint32_t h = kFirst + uint32_t(index) * kStep;
  if (obj.host) by_host_[obj.host] = h;
  slots_[index] = std::move(obj);
  return h;
}

GdiObject* GdiTable::get(uint32_t h) {
  if (h < kFirst || (h - kFirst) % kStep) return nullptr;
  size_t index = (h - kFirst) / kStep;
  if (index >= slots_.size() || slots_[index].type == GdiType::free) return nullptr;
  return &slots_[index];
}

GdiObject* GdiTable::get(uint32_t h, GdiType t) {
  GdiObject* o = get(h);
  return o && o->type == t ? o : nullptr;
}

HGDIOBJ GdiTable::host(uint32_t h) {
  GdiObject* o = get(h);
  return o ? o->host : nullptr;
}

bool GdiTable::destroy(uint32_t h) {
  GdiObject* o = get(h);
  if (!o) return false;
  if (o->stock || o->dc.is_screen || h == screen_bitmap_) return true;
  if (o->host && o->owned) {
    BOOL ok = o->type == GdiType::dc ? DeleteDC(static_cast<HDC>(o->host)) : DeleteObject(o->host);
    // A bitmap still selected into a DC cannot be deleted (Win32 fails the
    // call and keeps it); the guest may retry after deselecting it.
    if (!ok && o->type == GdiType::bitmap) return false;
  }
  if (o->host) by_host_.erase(o->host);
  auto ex = extra_objects_.find(h);
  if (ex != extra_objects_.end()) {
    for (HGDIOBJ x : ex->second) {
      by_host_.erase(x);
      DeleteObject(x);
    }
    extra_objects_.erase(ex);
  }
  if (o->type == GdiType::bitmap && o->bmp.guest_bits_alloc) rt_.heap().free(o->bmp.guest_bits_alloc);
  *o = GdiObject{};
  free_.push_back((h - kFirst) / kStep);
  return true;
}

GdiType GdiTable::type_of_host(HGDIOBJ obj) {
  switch (GetObjectType(obj)) {
    case OBJ_DC:
    case OBJ_MEMDC: return GdiType::dc;
    case OBJ_BITMAP: return GdiType::bitmap;
    case OBJ_BRUSH: return GdiType::brush;
    case OBJ_PEN:
    case OBJ_EXTPEN: return GdiType::pen;
    case OBJ_FONT: return GdiType::font;
    case OBJ_REGION: return GdiType::region;
    case OBJ_PAL: return GdiType::palette;
    default: return GdiType::free;
  }
}

uint32_t GdiTable::handle_for_host(HGDIOBJ obj) const {
  auto it = by_host_.find(obj);
  return it == by_host_.end() ? 0 : it->second;
}

uint32_t GdiTable::wrap_host(HGDIOBJ obj, GdiType t) {
  if (!obj) return 0;
  if (uint32_t h = handle_for_host(obj)) return h;
  GdiObject o;
  o.type = t == GdiType::free ? type_of_host(obj) : t;
  o.host = obj;
  o.owned = false;
  if (o.type == GdiType::bitmap) {
    BITMAP bm{};
    if (GetObjectW(obj, sizeof(bm), &bm)) {
      o.bmp.width = bm.bmWidth;
      o.bmp.height = bm.bmHeight;
      o.bmp.bpp = bm.bmBitsPixel * bm.bmPlanes;
      o.bmp.stride = uint32_t(bm.bmWidthBytes);
    }
  } else if (o.type == GdiType::brush) {
    LOGBRUSH lb{};
    if (GetObjectW(obj, sizeof(lb), &lb)) {
      o.brush_style = int(lb.lbStyle);
      o.color = lb.lbColor;
    }
  } else if (o.type == GdiType::pen) {
    LOGPEN lp{};
    if (GetObjectW(obj, sizeof(lp), &lp)) {
      o.pen_style = int(lp.lopnStyle);
      o.pen_width = lp.lopnWidth.x;
      o.color = lp.lopnColor;
    }
  }
  return add(std::move(o));
}

uint32_t GdiTable::add_dc(HDC dc, bool owned) {
  GdiObject o;
  o.type = GdiType::dc;
  o.host = dc;
  o.owned = owned;
  uint32_t h = add(std::move(o));
  if (!h) return 0;
  // The DC's initial bitmap (a 1×1 monochrome one for a memory DC): SelectObject
  // hands it back to the guest, which selects it again before DeleteDC.
  uint32_t bm = wrap_host(GetCurrentObject(dc, OBJ_BITMAP), GdiType::bitmap);
  if (GdiObject* d = get(h)) d->dc.bitmap = bm;
  return h;
}

uint32_t GdiTable::create_device_bitmap(int w, int h, int bpp) {
  GdiObject o;
  o.type = GdiType::bitmap;
  o.bmp.kind = BitmapKind::device;
  o.bmp.width = w;
  o.bmp.height = h;
  if (bpp == 1) {
    o.host = CreateBitmap(w, h, 1, 1, nullptr);
    o.bmp.bpp = 1;
    o.bmp.stride = uint32_t(((w + 15) / 16) * 2);
  } else {
    uint8_t* bits = nullptr;
    o.host = Display::create_surface8(w, h, true, nullptr, 0, &bits);
    o.bmp.bpp = 8;
    o.bmp.stride = Display::pitch_for(w);
    o.bmp.top_down = true;
    o.bmp.host_bits = bits;
  }
  if (!o.host) return 0;
  HGDIOBJ host = o.host;
  uint32_t h_ = add(std::move(o));
  if (!h_) DeleteObject(host);
  return h_;
}

uint32_t GdiTable::stock(int index) {
  auto it = stock_.find(index);
  if (it != stock_.end()) return it->second;
  uint32_t h;
  if (index == DEFAULT_PALETTE) {
    GdiObject o;
    o.stock = true;
    o.owned = false;
    o.type = GdiType::palette;
    o.pal = std::make_shared<LogicalPalette>(Display::default_palette());
    h = add(std::move(o));
  } else {
    HGDIOBJ host = GetStockObject(index);
    if (!host) return 0;
    // Already known (a DC's default object reported by SelectObject) or not:
    // wrap_host fills in the colour a stock brush/pen stands for.
    h = wrap_host(host, GdiType::free);
    if (GdiObject* o = get(h)) o->stock = true;
  }
  if (h) stock_[index] = h;
  return h;
}

Display& GdiTable::display() {
  Display* d = rt_.display();
  if (!d) throw GuestError(GuestError::Kind::fatal, "GDI used before the display was attached");
  return *d;
}

uint32_t GdiTable::screen_bitmap() {
  if (screen_bitmap_) return screen_bitmap_;
  Display& d = display();
  GdiObject o;
  o.type = GdiType::bitmap;
  o.host = d.bitmap();
  o.owned = false;
  o.stock = true;  // never deleted by the guest
  o.bmp.kind = BitmapKind::device;
  o.bmp.width = d.width();
  o.bmp.height = d.height();
  o.bmp.bpp = 8;
  o.bmp.stride = d.pitch();
  o.bmp.top_down = true;
  o.bmp.host_bits = d.bits();
  screen_bitmap_ = add(std::move(o));
  return screen_bitmap_;
}

uint32_t GdiTable::screen_dc() {
  if (screen_dc_) return screen_dc_;
  uint32_t bm = screen_bitmap();
  GdiObject o;
  o.type = GdiType::dc;
  o.host = display().dc();
  o.owned = false;
  o.dc.is_screen = true;
  o.dc.bitmap = bm;
  screen_dc_ = add(std::move(o));
  return screen_dc_;
}

LogicalPalette* GdiTable::dc_palette(uint32_t hdc) {
  GdiObject* dc = get(hdc, GdiType::dc);
  uint32_t hp = dc && dc->dc.palette ? dc->dc.palette : stock(DEFAULT_PALETTE);
  GdiObject* p = get(hp, GdiType::palette);
  return p ? p->pal.get() : nullptr;
}

GdiBitmap* GdiTable::dc_surface(uint32_t hdc) {
  GdiObject* dc = get(hdc, GdiType::dc);
  if (!dc) return nullptr;
  GdiObject* b = get(dc->dc.bitmap, GdiType::bitmap);
  return b ? &b->bmp : nullptr;
}

namespace {

COLORREF rgb_of(const PALETTEENTRY& e) { return RGB(e.peRed, e.peGreen, e.peBlue); }
COLORREF rgb_of(const RGBQUAD& q) { return RGB(q.rgbRed, q.rgbGreen, q.rgbBlue); }

}  // namespace

COLORREF GdiTable::surface_rgb(const GdiBitmap& b, int index, uint32_t hdc) {
  index &= 0xFF;
  if (b.kind == BitmapKind::device) {
    if (b.bpp == 1) return index ? RGB(255, 255, 255) : RGB(0, 0, 0);
    return display().hardware_color(index);
  }
  if (b.usage == DIB_PAL_COLORS) {
    LogicalPalette* p = dc_palette(hdc);
    if (!p || size_t(index) >= b.pal_indices.size()) return 0;
    size_t li = b.pal_indices[size_t(index)];
    return li < p->entries.size() ? rgb_of(p->entries[li]) : 0;
  }
  return size_t(index) < b.colors.size() ? rgb_of(b.colors[size_t(index)]) : 0;
}

COLORREF GdiTable::key_color(uint32_t hdc, COLORREF c) {
  Display& d = display();
  LogicalPalette* pal = dc_palette(hdc);
  GdiBitmap* s = dc_surface(hdc);
  if (!s || s->kind == BitmapKind::device) {
    if (s && s->bpp == 1) {
      // Monochrome: GDI's own rule — the background colour is white, all else black.
      return c;
    }
    return Display::key_color(d.map_index(c, pal));
  }
  // A DIB section: the colour lands on the nearest entry of its own table.
  if ((c >> 24) == 0x10) return Display::key_color(int(c & 0xFF));
  COLORREF rgb = c & 0xFFFFFF;
  if ((c >> 24) == 0x01 && pal) {
    size_t i = c & 0xFFFF;
    rgb = i < pal->entries.size() ? rgb_of(pal->entries[i]) : 0;
  }
  int n = 1 << std::min(s->bpp, 8);
  int best = 0, best_d = INT32_MAX;
  for (int i = 0; i < n; i++) {
    COLORREF e = surface_rgb(*s, i, hdc);
    int dr = int(GetRValue(e)) - int(GetRValue(rgb)), dg = int(GetGValue(e)) - int(GetGValue(rgb)),
        db = int(GetBValue(e)) - int(GetBValue(rgb));
    int dd = dr * dr + dg * dg + db * db;
    if (dd < best_d) {
      best_d = dd;
      best = i;
      if (!dd) break;
    }
  }
  return Display::key_color(best);
}

HGDIOBJ GdiTable::realize_for(uint32_t h, uint32_t hdc) {
  GdiObject* o = get(h);
  if (!o || (o->type != GdiType::brush && o->type != GdiType::pen)) return o ? o->host : nullptr;
  if (o->type == GdiType::brush && (o->brush_style == BS_NULL || o->brush_style == BS_PATTERN ||
                                    o->brush_style == BS_DIBPATTERN || o->brush_style == BS_DIBPATTERNPT)) {
    return o->host;
  }
  if (o->type == GdiType::pen && o->pen_style == PS_NULL) return o->host;
  COLORREF k = key_color(hdc, o->color);
  if (k == o->made_for) return o->host;
  // Look for a variant already made for this key colour.
  auto& extra = extra_objects_[h];
  for (HGDIOBJ x : extra) {
    if (o->type == GdiType::brush) {
      LOGBRUSH lb{};
      if (GetObjectW(x, sizeof(lb), &lb) && lb.lbColor == k) return x;
    } else {
      LOGPEN lp{};
      if (GetObjectW(x, sizeof(lp), &lp) && lp.lopnColor == k) return x;
    }
  }
  HGDIOBJ n;
  if (o->type == GdiType::brush) {
    n = o->brush_style == BS_HATCHED ? CreateHatchBrush(o->hatch, k) : CreateSolidBrush(k);
  } else {
    n = CreatePen(o->pen_style, o->pen_width, k);
  }
  if (!n) return o->host;
  extra.push_back(n);
  by_host_[n] = h;
  return n;
}

Xlate GdiTable::surface_xlate(const GdiBitmap& src, uint32_t src_dc, const GdiBitmap& dst, uint32_t dst_dc) {
  if (src.kind == BitmapKind::device && dst.kind == BitmapKind::device) return Xlate::make_identity();
  // Two DIBs with the same table: the indices mean the same colours.
  if (src.kind == BitmapKind::dib && dst.kind == BitmapKind::dib && src.usage == dst.usage &&
      src.pal_indices == dst.pal_indices && src.colors.size() == dst.colors.size() &&
      std::equal(src.colors.begin(), src.colors.end(), dst.colors.begin(), [](const RGBQUAD& a, const RGBQUAD& b) {
        return a.rgbRed == b.rgbRed && a.rgbGreen == b.rgbGreen && a.rgbBlue == b.rgbBlue;
      })) {
    return Xlate::make_identity();
  }
  Display& d = display();
  Xlate x;
  int n = 1 << std::min(std::max(src.bpp, 1), 8);
  LogicalPalette* dst_pal = dc_palette(dst_dc);
  LogicalPalette* src_pal = dc_palette(src_dc);
  int dn = 1 << std::min(std::max(dst.bpp, 1), 8);
  std::vector<COLORREF> dst_rgbs;
  if (dst.kind == BitmapKind::dib) {
    for (int j = 0; j < dn; j++) dst_rgbs.push_back(surface_rgb(dst, j, dst_dc));
  }
  for (int i = 0; i < 256; i++) {
    if (i >= n) {
      x.map[size_t(i)] = uint8_t(i);
      continue;
    }
    int out;
    if (dst.kind == BitmapKind::device) {
      if (src.kind == BitmapKind::dib && src.usage == DIB_PAL_COLORS) {
        // Palette indices go straight through the destination DC's palette.
        size_t li = size_t(i) < src.pal_indices.size() ? src.pal_indices[size_t(i)] : 0;
        const LogicalPalette* p = dst_pal;
        out = p && p->realized && li < p->map.size() ? p->map[li] : d.device_index_for_rgb(surface_rgb(src, i, src_dc), p);
      } else {
        out = d.device_index_for_rgb(surface_rgb(src, i, src_dc), dst_pal);
      }
    } else {
      COLORREF rgb = surface_rgb(src, i, src_dc);
      int best = 0, best_d = INT32_MAX;
      for (int j = 0; j < dn; j++) {
        COLORREF e = dst_rgbs[size_t(j)];
        int dr = int(GetRValue(e)) - int(GetRValue(rgb)), dg = int(GetGValue(e)) - int(GetGValue(rgb)),
            db = int(GetBValue(e)) - int(GetBValue(rgb));
        int dd = dr * dr + dg * dg + db * db;
        if (dd < best_d) {
          best_d = dd;
          best = j;
          if (!dd) break;
        }
      }
      out = best;
    }
    x.map[size_t(i)] = uint8_t(out);
  }
  (void)src_pal;
  x.finish();
  return x;
}

}  // namespace adw::win32
