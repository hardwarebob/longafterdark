#include "module_icons.h"

#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#if AD_SCR_HAVE_LOADER
#include "loader/image.hh"
#include "loader/ne.hh"
#include "loader/pe.hh"
#endif

#include "catalog.h"
#include "paths.h"
#include "thumbnails.h"

namespace adw::scr {

struct ModuleIcons::Scaled {
  int size = 0;
  bool dimmed = false;
  std::unique_ptr<Gdiplus::Bitmap> bmp;
};

namespace {

#if AD_SCR_HAVE_LOADER
// RT_GROUP_ICON: NEWHEADER + 14-byte RESDIR entries (the same in NE and PE).
struct GroupEntry {
  int w = 0, h = 0, bits = 0;
  uint16_t id = 0;
};

std::vector<GroupEntry> parse_group(std::string_view g) {
  std::vector<GroupEntry> out;
  if (g.size() < 6) return out;
  auto u16 = [&](size_t o) { return (uint16_t)((uint8_t)g[o] | ((uint8_t)g[o + 1] << 8)); };
  uint16_t count = u16(4);
  for (size_t i = 0; i < count && 6 + i * 14 + 14 <= g.size(); ++i) {
    size_t o = 6 + i * 14;
    GroupEntry e;
    e.w = (uint8_t)g[o] ? (uint8_t)g[o] : 256;
    e.h = (uint8_t)g[o + 1] ? (uint8_t)g[o + 1] : 256;
    e.bits = u16(o + 6);
    if (!e.bits) {
      int colors = (uint8_t)g[o + 2];
      e.bits = colors == 2 ? 1 : colors == 16 ? 4 : 8;
    }
    e.id = u16(o + 12);
    out.push_back(e);
  }
  return out;
}

// Closest to 32x32, then the most colours.
const GroupEntry* best_entry(const std::vector<GroupEntry>& v) {
  const GroupEntry* best = nullptr;
  for (const auto& e : v) {
    if (!best) {
      best = &e;
      continue;
    }
    int d = std::abs(e.w - 32), bd = std::abs(best->w - 32);
    if (d < bd || (d == bd && e.bits > best->bits)) best = &e;
  }
  return best;
}

template <typename Img>
std::string icon_bytes(const Img& img, int* w, int* h) {
  using adw::loader::ResId;
  namespace rt = adw::loader::rt;
  for (const auto& r : img.resources()) {
    if (r.type.is_string || r.type.num != rt::group_icon) continue;
    const GroupEntry* e = best_entry(parse_group(img.resource_data(r)));
    if (!e) continue;
    for (const auto& ir : img.resources()) {
      if (ir.type.is_string || ir.type.num != rt::icon) continue;
      if (!ir.name.matches(ResId::of(e->id))) continue;
      *w = e->w;
      *h = e->h;
      return std::string(img.resource_data(ir));
    }
  }
  return {};
}
#endif

// HICON -> straight-alpha ARGB, by drawing it on black and on white: works
// for old AND-mask icons and alpha icons alike.
std::vector<uint32_t> icon_pixels(HICON icon, int w, int h) {
  std::vector<uint32_t> out;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  void* bits[2] = {};
  HBITMAP bmp[2] = {CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits[0], nullptr, 0),
                    CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits[1], nullptr, 0)};
  if (bmp[0] && bmp[1]) {
    for (int k = 0; k < 2; ++k) {
      HGDIOBJ old = SelectObject(dc, bmp[k]);
      RECT r{0, 0, w, h};
      FillRect(dc, &r, (HBRUSH)GetStockObject(k ? WHITE_BRUSH : BLACK_BRUSH));
      DrawIconEx(dc, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);
      GdiFlush();
      SelectObject(dc, old);
    }
    const uint8_t* b = static_cast<const uint8_t*>(bits[0]);
    const uint8_t* wt = static_cast<const uint8_t*>(bits[1]);
    out.resize((size_t)w * h);
    for (size_t i = 0; i < out.size(); ++i) {
      int a = 255 - ((wt[i * 4] - b[i * 4]) + (wt[i * 4 + 1] - b[i * 4 + 1]) + (wt[i * 4 + 2] - b[i * 4 + 2])) / 3;
      a = std::clamp(a, 0, 255);
      auto un = [&](int c) { return a ? std::min(255, c * 255 / a) : 0; };
      out[i] = ((uint32_t)a << 24) | (un(b[i * 4 + 2]) << 16) | (un(b[i * 4 + 1]) << 8) | un(b[i * 4]);
    }
  }
  for (HBITMAP x : bmp) if (x) DeleteObject(x);
  DeleteDC(dc);
  return out;
}

// The night sky behind pixel-art icons and the placeholder (the moon logo's blue).
constexpr COLORREF kNight = RGB(0x1B, 0x1F, 0x3A);

// An icon's own backdrop: the colour of its four corners when they are all
// opaque and alike (black-backed pixel art), else false.
bool icon_backdrop(int w, int h, const std::vector<uint32_t>& argb, COLORREF* out) {
  if (w < 2 || h < 2 || argb.size() < (size_t)w * h) return false;
  const uint32_t c[4] = {argb[0], argb[(size_t)w - 1], argb[(size_t)(h - 1) * w], argb[(size_t)h * w - 1]};
  for (uint32_t v : c) {
    if ((v >> 24) != 0xFF) return false;
    for (int sh : {0, 8, 16}) {
      if (std::abs((int)((v >> sh) & 0xFF) - (int)((c[0] >> sh) & 0xFF)) > 12) return false;
    }
  }
  *out = RGB((c[0] >> 16) & 0xFF, (c[0] >> 8) & 0xFF, c[0] & 0xFF);
  return true;
}

void round_path(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
  float d = r * 2;
  p.AddArc(x, y, d, d, 180, 90);
  p.AddArc(x + w - d, y, d, d, 270, 90);
  p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  p.AddArc(x, y + h - d, d, d, 90, 90);
  p.CloseFigure();
}

Gdiplus::Color gp(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

std::unique_ptr<Gdiplus::Bitmap> bitmap_of(int w, int h, const std::vector<uint32_t>& argb) {
  auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
  Gdiplus::Rect all(0, 0, w, h);
  Gdiplus::BitmapData bd;
  if (bmp->LockBits(&all, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
    for (int y = 0; y < h; ++y) {
      memcpy(static_cast<uint8_t*>(bd.Scan0) + (size_t)y * bd.Stride, argb.data() + (size_t)y * w, (size_t)w * 4);
    }
    bmp->UnlockBits(&bd);
  }
  return bmp;
}

// Until a module has a picture: the night sky with a crescent moon and two
// four-point stars, as in the app's own mark (res/gen_icon.cc).
void draw_night_moon(Gdiplus::Graphics& g, float size) {
  Gdiplus::LinearGradientBrush sky(Gdiplus::PointF(0, -1), Gdiplus::PointF(0, size + 1), Gdiplus::Color(255, 0x26, 0x2D, 0x5C),
                                   Gdiplus::Color(255, 0x0D, 0x10, 0x26));
  g.FillRectangle(&sky, 0.0f, 0.0f, size, size);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  // A soft glow behind the moon.
  {
    Gdiplus::GraphicsPath halo;
    halo.AddEllipse(size * 0.10f, size * 0.08f, size * 0.84f, size * 0.84f);
    Gdiplus::PathGradientBrush b(&halo);
    b.SetCenterColor(Gdiplus::Color(40, 0x9A, 0xA8, 0xFF));
    Gdiplus::Color edge(0, 0x9A, 0xA8, 0xFF);
    int n = 1;
    b.SetSurroundColors(&edge, &n);
    b.SetCenterPoint(Gdiplus::PointF(size * 0.52f, size * 0.50f));
    g.FillPath(&b, &halo);
  }
  // Crescent: a disc less an offset disc.
  Gdiplus::GraphicsPath disc, bite;
  disc.AddEllipse(size * (0.53f - 0.25f), size * (0.50f - 0.25f), size * 0.50f, size * 0.50f);
  bite.AddEllipse(size * (0.65f - 0.215f), size * (0.40f - 0.215f), size * 0.43f, size * 0.43f);
  Gdiplus::Region moon(&disc);
  moon.Exclude(&bite);
  Gdiplus::SolidBrush gold(Gdiplus::Color(255, 0xF6, 0xE8, 0xB4));
  g.FillRegion(&gold, &moon);
  // Four-point stars.
  auto star = [&](float cx, float cy, float r, BYTE a) {
    const float k = r * 0.22f;
    Gdiplus::PointF pts[8] = {{cx, cy - r}, {cx + k, cy - k}, {cx + r, cy}, {cx + k, cy + k},
                              {cx, cy + r}, {cx - k, cy + k}, {cx - r, cy}, {cx - k, cy - k}};
    Gdiplus::SolidBrush b(Gdiplus::Color(a, 0xFF, 0xFB, 0xEE));
    g.FillPolygon(&b, pts, 8);
  };
  star(size * 0.24f, size * 0.27f, std::max(1.5f, size * 0.085f), 240);
  star(size * 0.27f, size * 0.74f, std::max(1.0f, size * 0.05f), 190);
}

// A thumbnail PNG (thumbnails.h), as straight ARGB.
bool load_thumb(const std::wstring& path, int* w, int* h, std::vector<uint32_t>* argb) {
  if (path.empty() || !file_exists(path)) return false;
  Gdiplus::Bitmap bmp(path.c_str());
  if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
  const int bw = (int)bmp.GetWidth(), bh = (int)bmp.GetHeight();
  if (bw <= 0 || bh <= 0 || bw > 512 || bh > 512) return false;
  Gdiplus::Rect all(0, 0, bw, bh);
  Gdiplus::BitmapData bd;
  if (bmp.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) return false;
  argb->resize((size_t)bw * bh);
  for (int y = 0; y < bh; ++y) {
    memcpy(argb->data() + (size_t)y * bw, static_cast<const uint8_t*>(bd.Scan0) + (size_t)y * bd.Stride, (size_t)bw * 4);
  }
  bmp.UnlockBits(&bd);
  *w = bw;
  *h = bh;
  return true;
}

} // namespace

ModuleIcons::ModuleIcons() = default;
ModuleIcons::~ModuleIcons() = default;

void ModuleIcons::clear() {
  sources_.clear();
  scaled_.clear();
}

void ModuleIcons::set_thumbs_dir(const std::wstring& dir) { thumbs_dir_ = dir; }

std::wstring ModuleIcons::thumb_path(const std::string& id) const { return thumb_file(thumbs_dir_, id); }

void ModuleIcons::forget(const std::string& id) {
  sources_.erase(id);
  scaled_.erase(id);
}

void ModuleIcons::recheck_pictureless() {
  for (auto it = sources_.begin(); it != sources_.end();) {
    if (it->second.kind == Kind::none) {
      scaled_.erase(it->first);
      it = sources_.erase(it);
    } else {
      ++it;
    }
  }
}

const ModuleIcons::Source& ModuleIcons::source(const Module& m, const std::wstring& win_dir) {
  auto it = sources_.find(m.id);
  if (it != sources_.end()) return it->second;
  Source s;
#if AD_SCR_HAVE_LOADER
  try {
    std::wstring path = resolve_module_path(win_dir, m.path);
    if (file_exists(path)) {
      std::string data = adw::loader::read_file(narrow(path));
      int w = 0, h = 0;
      std::string bytes;
      switch (adw::loader::detect_format(data)) {
        case adw::loader::Format::pe32: bytes = icon_bytes(adw::loader::pe::Image(std::move(data)), &w, &h); break;
        case adw::loader::Format::ne: bytes = icon_bytes(adw::loader::ne::Image(std::move(data)), &w, &h); break;
        default: break;
      }
      if (!bytes.empty() && w > 0 && h > 0 && w <= 256 && h <= 256) {
        HICON icon = CreateIconFromResourceEx(reinterpret_cast<PBYTE>(bytes.data()), (DWORD)bytes.size(), TRUE,
                                              0x00030000, w, h, LR_DEFAULTCOLOR);
        if (icon) {
          s.argb = icon_pixels(icon, w, h);
          s.w = w;
          s.h = h;
          s.kind = s.argb.empty() ? Kind::none : Kind::icon;
          DestroyIcon(icon);
        }
      }
    }
  } catch (...) {
    // A damaged or unexpected file just gets a thumbnail or the placeholder.
  }
#else
  (void)win_dir;
#endif
  if (s.kind == Kind::none && load_thumb(thumb_path(m.id), &s.w, &s.h, &s.argb)) s.kind = Kind::thumb;
  return sources_.emplace(m.id, std::move(s)).first->second;
}

bool ModuleIcons::has_own_icon(const Module& m, const std::wstring& win_dir) {
  return source(m, win_dir).kind == Kind::icon;
}

bool ModuleIcons::has_picture(const Module& m, const std::wstring& win_dir) {
  return source(m, win_dir).kind != Kind::none;
}

void ModuleIcons::draw(HDC dc, const Module& m, const std::wstring& win_dir, const RECT& r, bool dimmed) {
  const int size = std::min(r.right - r.left, r.bottom - r.top);
  if (size <= 0) return;
  std::unique_ptr<Scaled>& slot = scaled_[m.id];
  if (!slot || slot->size != size || slot->dimmed != dimmed) {
    slot = std::make_unique<Scaled>();
    slot->size = size;
    slot->dimmed = dimmed;
    // The tile at full strength first, then (dimmed) copied at 42%.
    Gdiplus::Bitmap tile(size, size, PixelFormat32bppPARGB);
    {
      Gdiplus::Graphics g(&tile);
      g.Clear(Gdiplus::Color(0, 0, 0, 0));
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
      const float radius = std::max(2.0f, size * 6.0f / 28.0f);
      Gdiplus::GraphicsPath path;
      round_path(path, 0, 0, (float)size, (float)size, radius);
      const Source& src = source(m, win_dir);
      // The picture on a square canvas, then that canvas as the texture of
      // the rounded tile: anti-aliased corners, no hard clip.
      Gdiplus::Bitmap canvas(size, size, PixelFormat32bppPARGB);
      {
        Gdiplus::Graphics gc(&canvas);
        gc.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        if (src.kind == Kind::icon) {
          COLORREF backdrop = kNight;
          const bool own = icon_backdrop(src.w, src.h, src.argb, &backdrop);
          gc.Clear(gp(backdrop));
          // An icon with its own backdrop fills the tile; one drawn on
          // nothing keeps a margin of night around it. The same share of
          // the tile at every DPI: nearest-neighbour up to a whole multiple
          // (square pixels), then smoothly down to the size.
          const int art = std::max(1, (int)std::lround(size * (own ? 1.0 : 0.86)));
          const int k = std::max(1, (art + src.w - 1) / std::max(1, src.w));
          std::unique_ptr<Gdiplus::Bitmap> orig = bitmap_of(src.w, src.h, src.argb);
          Gdiplus::Bitmap big(src.w * k, src.h * k, PixelFormat32bppPARGB);
          {
            Gdiplus::Graphics gb(&big);
            gb.Clear(Gdiplus::Color(0, 0, 0, 0));
            gb.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            gb.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            gb.DrawImage(orig.get(), Gdiplus::Rect(0, 0, src.w * k, src.h * k), 0, 0, src.w, src.h, Gdiplus::UnitPixel);
          }
          const int dw = std::max(1, (int)std::lround((double)art * src.w / std::max(src.w, src.h)));
          const int dh = std::max(1, (int)std::lround((double)art * src.h / std::max(src.w, src.h)));
          gc.SetInterpolationMode(dw == src.w * k ? Gdiplus::InterpolationModeNearestNeighbor
                                                  : Gdiplus::InterpolationModeHighQualityBilinear);
          Gdiplus::ImageAttributes clamp;
          clamp.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
          gc.DrawImage(&big, Gdiplus::Rect((size - dw) / 2, (size - dh) / 2, dw, dh), 0, 0, src.w * k, src.h * k,
                       Gdiplus::UnitPixel, &clamp);
        } else if (src.kind == Kind::thumb) {
          gc.Clear(Gdiplus::Color(255, 0, 0, 0));
          std::unique_ptr<Gdiplus::Bitmap> orig = bitmap_of(src.w, src.h, src.argb);
          gc.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
          Gdiplus::ImageAttributes clamp;
          clamp.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
          gc.DrawImage(orig.get(), Gdiplus::Rect(0, 0, size, size), 0, 0, src.w, src.h, Gdiplus::UnitPixel, &clamp);
        } else {
          draw_night_moon(gc, (float)size);
        }
      }
      Gdiplus::TextureBrush tex(&canvas);
      g.FillPath(&tex, &path);
      // A faint rim, so a dark picture keeps its shape on a dark card.
      Gdiplus::GraphicsPath rim;
      round_path(rim, 0.5f, 0.5f, size - 1.0f, size - 1.0f, radius - 0.5f);
      Gdiplus::Pen pen(Gdiplus::Color(28, 255, 255, 255), 1.0f);
      g.DrawPath(&pen, &rim);
    }
    slot->bmp = std::make_unique<Gdiplus::Bitmap>(size, size, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(slot->bmp.get());
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::ImageAttributes attrs;
    Gdiplus::ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 0.42f, 0}, {0, 0, 0, 0, 1}}};
    if (dimmed) attrs.SetColorMatrix(&cm);
    g.DrawImage(&tile, Gdiplus::Rect(0, 0, size, size), 0, 0, size, size, Gdiplus::UnitPixel, dimmed ? &attrs : nullptr);
  }
  Gdiplus::Graphics out(dc);
  out.DrawImage(slot->bmp.get(), (INT)r.left, (INT)r.top, (INT)size, (INT)size);
}

} // namespace adw::scr
