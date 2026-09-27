#include "adw/ui/image.h"

#include <objidl.h>
#include <wincodec.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <utility>

namespace adw::ui {

namespace {

template <typename T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() {
    if (p) p->Release();
  }
  T** out() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

bool fail(std::string* error, const std::string& what, HRESULT hr = S_OK) {
  if (error) {
    char buf[32] = "";
    if (hr != S_OK) snprintf(buf, sizeof(buf), " (hr=0x%08lX)", (unsigned long)hr);
    *error = what + buf;
  }
  return false;
}

// Pictures larger than this are refused rather than decoded (a cover is
// capped at 2048 px by the importer; a user's photo can be large, but not
// this large).
constexpr int kMaxSide = 16384;
constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;

// ---- draw_image's cache: the last few sizes each picture was drawn at -----------------

struct Scaled {
  int w = 0, h = 0;
  std::vector<uint8_t> px;   // premultiplied BGRA, top-down
};

struct ScaleCache {
  // What the copies were made from: a picture whose pixels were replaced
  // (a new buffer) starts a fresh cache.
  const uint8_t* src = nullptr;
  size_t src_size = 0;
  int sw = 0, sh = 0;
  std::list<Scaled> sizes;   // most recent first
};

constexpr size_t kCachedSizes = 4;

const std::vector<uint8_t>& scaled_pixels(const Image& img, int w, int h) {
  if (w == img.w && h == img.h) return img.pbgra;
  auto c = std::static_pointer_cast<ScaleCache>(img.cache);
  if (!c || c->src != img.pbgra.data() || c->src_size != img.pbgra.size() || c->sw != img.w || c->sh != img.h) {
    c = std::make_shared<ScaleCache>();
    c->src = img.pbgra.data();
    c->src_size = img.pbgra.size();
    c->sw = img.w;
    c->sh = img.h;
    img.cache = c;
  }
  for (auto it = c->sizes.begin(); it != c->sizes.end(); ++it) {
    if (it->w == w && it->h == h) {
      c->sizes.splice(c->sizes.begin(), c->sizes, it);
      return c->sizes.front().px;
    }
  }
  Scaled sc;
  sc.w = w;
  sc.h = h;
  sc.px.assign((size_t)w * h * 4, 0);
  {
    Gdiplus::Bitmap src(img.w, img.h, img.w * 4, PixelFormat32bppPARGB, const_cast<BYTE*>(img.pbgra.data()));
    Gdiplus::Bitmap dst(w, h, w * 4, PixelFormat32bppPARGB, sc.px.data());
    Gdiplus::Graphics g(&dst);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    // Prefiltered bicubic: a 640 x 800 tile shrinks to 48 x 60 without aliasing.
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    // Mirrored edges, so the filter doesn't pull transparency in at the border.
    Gdiplus::ImageAttributes ia;
    ia.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    g.DrawImage(&src, Gdiplus::Rect(0, 0, w, h), 0, 0, img.w, img.h, Gdiplus::UnitPixel, &ia);
    g.Flush(Gdiplus::FlushIntentionSync);
  }
  c->sizes.push_front(std::move(sc));
  while (c->sizes.size() > kCachedSizes) c->sizes.pop_back();
  return c->sizes.front().px;
}

void add_round_rect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
  r = std::max(0.0f, std::min(r, std::min(w, h) / 2));
  if (r <= 0.01f) {
    path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    return;
  }
  const float d = r * 2;
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + w - d, y, d, d, 270, 90);
  path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path.AddArc(x, y + h - d, d, d, 90, 90);
  path.CloseFigure();
}

bool face_installed(const wchar_t* face) {
  LOGFONTW lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
  bool found = false;
  HDC dc = GetDC(nullptr);
  EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
    *reinterpret_cast<bool*>(lp) = true;
    return 0;
  }, reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

// The generated covers already drawn, by size and title (a strip or a list
// redraws the same few many times).
struct GeneratedKey {
  int w, h, dpi;
  std::wstring title;
  bool operator==(const GeneratedKey& o) const { return w == o.w && h == o.h && dpi == o.dpi && title == o.title; }
};

const Image& cached_generated(int w, int h, const std::wstring& title, int dpi) {
  static std::list<std::pair<GeneratedKey, Image>> cache;
  GeneratedKey key{w, h, dpi, title};
  for (auto it = cache.begin(); it != cache.end(); ++it) {
    if (it->first == key) {
      cache.splice(cache.begin(), cache, it);
      return cache.front().second;
    }
  }
  cache.emplace_front(key, generated_cover_image(w, h, title, dpi));
  while (cache.size() > 32) cache.pop_back();
  return cache.front().second;
}

}  // namespace

bool load_image(const std::wstring& path, Image& out, std::string* error) {
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.out()));
  if (FAILED(hr)) return fail(error, "Windows Imaging Component is unavailable", hr);
  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                          decoder.out());
  if (FAILED(hr)) {
    return fail(error, GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES
                           ? "the picture file cannot be opened"
                           : "Windows can't read this picture",
                hr);
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(hr = decoder->GetFrame(0, frame.out()))) return fail(error, "the picture has no frames", hr);
  UINT w = 0, h = 0;
  if (FAILED(hr = frame->GetSize(&w, &h))) return fail(error, "GetSize", hr);
  if (w == 0 || h == 0 || w > (UINT)kMaxSide || h > (UINT)kMaxSide || (uint64_t)w * h > kMaxPixels)
    return fail(error, "the picture is too large or empty");
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(hr = factory->CreateFormatConverter(conv.out()))) return fail(error, "CreateFormatConverter", hr);
  hr = conv->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                        WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) return fail(error, "the picture cannot be converted", hr);
  std::vector<uint8_t> px((size_t)w * h * 4);
  if (FAILED(hr = conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())))
    return fail(error, "the picture cannot be decoded", hr);
  out.w = (int)w;
  out.h = (int)h;
  out.pbgra = std::move(px);
  out.cache.reset();
  return true;
}

Image image_from_bgra(int w, int h, const uint8_t* bgra, size_t stride, bool premultiplied) {
  Image img;
  if (w <= 0 || h <= 0 || !bgra) return img;
  img.w = w;
  img.h = h;
  img.pbgra.resize((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = bgra + (size_t)y * stride;
    uint8_t* d = img.pbgra.data() + (size_t)y * w * 4;
    for (int x = 0; x < w; ++x, s += 4, d += 4) {
      const unsigned a = s[3];
      if (premultiplied || a == 255) {
        d[0] = s[0], d[1] = s[1], d[2] = s[2];
      } else {
        d[0] = (uint8_t)((s[0] * a + 127) / 255);
        d[1] = (uint8_t)((s[1] * a + 127) / 255);
        d[2] = (uint8_t)((s[2] * a + 127) / 255);
      }
      d[3] = (uint8_t)a;
    }
  }
  return img;
}

void draw_image(HDC dc, const RECT& dst, const Image& img, float radius, int alpha) {
  const int w = dst.right - dst.left, h = dst.bottom - dst.top;
  if (w <= 0 || h <= 0 || img.empty() || alpha <= 0) return;
  if (img.pbgra.size() < (size_t)img.w * img.h * 4) return;
  const std::vector<uint8_t>& scaled = scaled_pixels(img, w, h);
  const std::vector<uint8_t>* px = &scaled;
  std::vector<uint8_t> faded;
  if (alpha < 255) {
    // Premultiplied: every channel scales with the opacity.
    faded.resize(scaled.size());
    for (size_t i = 0; i < scaled.size(); ++i) faded[i] = (uint8_t)((scaled[i] * alpha + 127) / 255);
    px = &faded;
  }
  Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, const_cast<BYTE*>(px->data()));
  Gdiplus::Graphics g(dc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
  // The scaled copy is exactly the destination's size: the brush maps texel
  // to pixel, and only the rounded corners are anti-aliased.
  Gdiplus::TextureBrush brush(&bmp, Gdiplus::WrapModeClamp);
  brush.TranslateTransform((float)dst.left, (float)dst.top);
  Gdiplus::GraphicsPath path;
  add_round_rect(path, (float)dst.left, (float)dst.top, (float)w, (float)h, radius);
  g.FillPath(&brush, &path);
}

void draw_generated_cover(HDC dc, const RECT& art, const std::wstring& title, int dpi) {
  const int w = art.right - art.left, h = art.bottom - art.top;
  if (w <= 0 || h <= 0) return;
  const float fw = (float)w, fh = (float)h, x0 = (float)art.left, y0 = (float)art.top;
  fill_gradient(dc, art, RGB(0x26, 0x2B, 0x4F), RGB(0x12, 0x15, 0x2A));
  // Five sparkles, clear of the moon and of the title's three lines.
  struct Sparkle {
    float x, y, r;
    int alpha;
  };
  static const Sparkle kSparkles[] = {{0.16f, 0.12f, 0.040f, 235}, {0.42f, 0.27f, 0.024f, 180},
                                      {0.88f, 0.38f, 0.030f, 205}, {0.10f, 0.34f, 0.020f, 160},
                                      {0.55f, 0.08f, 0.018f, 150}};
  for (const Sparkle& s : kSparkles)
    draw_sparkle(dc, x0 + s.x * fw, y0 + s.y * fh, std::max(0.8f, s.r * fw), RGB(0xFF, 0xFB, 0xEE), s.alpha);
  draw_crescent(dc, x0 + 0.72f * fw, y0 + 0.22f * fh, 0.11f * fw, 90);
  if (title.empty()) return;

  // The title: left at 0.1 w, bottom at 0.9 h, at most 3 lines 0.8 w wide,
  // 0.14 w tall and shrunk until it fits.
  static const bool display = face_installed(L"Segoe UI Variable Display");
  const wchar_t* face = display ? L"Segoe UI Variable Display" : L"Segoe UI";
  const int avail = std::max(1, (int)std::lround(0.8f * fw));
  const int min_px = std::max(4, std::min((int)std::lround(0.05f * fw), MulDiv(6, dpi, 96)));
  int px = std::max(min_px, (int)std::lround(0.14f * fw));
  HFONT font = nullptr;
  RECT calc{};
  int line_h = 1;
  for (;; --px) {
    if (font) DeleteObject(font);
    // Plain anti-aliasing: ClearType's colour fringes show on the night.
    font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    line_h = std::max(1, (int)tm.tmHeight);
    calc = RECT{0, 0, avail, 0};
    DrawTextW(dc, title.c_str(), (int)title.size(), &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    const int lines = (calc.bottom - calc.top + line_h - 1) / line_h;
    if ((lines <= 3 && calc.right - calc.left <= avail) || px <= min_px) break;
  }
  const int text_h = std::min((int)(calc.bottom - calc.top), 3 * line_h);
  const int left = art.left + (int)std::lround(0.1f * fw);
  const int bottom = art.top + (int)std::lround(0.9f * fh);
  RECT r{left, bottom - text_h, left + avail, bottom};
  draw_text(dc, title, r, font, RGB(0xFF, 0xFF, 0xFF), DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL);
  DeleteObject(font);
}

Image generated_cover_image(int w, int h, const std::wstring& title, int dpi) {
  Image img;
  if (w <= 0 || h <= 0) return img;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!dib || !mem) {
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    return img;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  RECT r{0, 0, w, h};
  draw_generated_cover(mem, r, title, dpi);
  GdiFlush();
  img.w = w;
  img.h = h;
  img.pbgra.assign(static_cast<const uint8_t*>(bits), static_cast<const uint8_t*>(bits) + (size_t)w * h * 4);
  // GDI leaves alpha undefined; the cover is opaque.
  for (size_t i = 3; i < img.pbgra.size(); i += 4) img.pbgra[i] = 255;
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  return img;
}

void draw_cover(HDC dc, const RECT& art, const Image* tile, const std::wstring& title, const Theme& t) {
  const int w = art.right - art.left, h = art.bottom - art.top;
  if (w <= 0 || h <= 0) return;
  const float radius = t.pxf(4);
  if (tile && !tile->empty()) {
    draw_image(dc, art, *tile, radius);
  } else {
    draw_image(dc, art, cached_generated(w, h, title, t.dpi), radius);
  }
  // pal.card_stroke is WindowText under high contrast.
  stroke_round(dc, art, radius, t.pal.card_stroke, (float)t.hairline());
}

}  // namespace adw::ui
