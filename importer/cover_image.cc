#include "cover_image.h"

#include <windows.h>
#include <objbase.h>
#include <cstring>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "loader/image.hh"
#include "loader/ne.hh"
#include "loader/pe.hh"
#include "status.h"
#include "winutil.h"

namespace adw::import::cover {

namespace fs = std::filesystem;

namespace {

[[noreturn]] void invalid(const std::string& why) { throw ImportError(Status::source_invalid, why); }

uint8_t clamp8(double v) { return uint8_t(v <= 0 ? 0 : v >= 255 ? 255 : std::lround(v)); }

}  // namespace

Picture Picture::filled(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
  Picture p;
  p.w = w;
  p.h = h;
  p.bgra.resize(size_t(w) * size_t(h) * 4);
  for (size_t i = 0; i < p.bgra.size(); i += 4) {
    p.bgra[i] = b;
    p.bgra[i + 1] = g;
    p.bgra[i + 2] = r;
    p.bgra[i + 3] = a;
  }
  return p;
}

// ---- geometry ------------------------------------------------------------------------------

Picture orient(const Picture& p, int o) {
  if (o < 2 || o > 8 || p.empty()) return p;
  const bool swap = o >= 5;
  Picture out;
  out.w = swap ? p.h : p.w;
  out.h = swap ? p.w : p.h;
  out.bgra.resize(p.bgra.size());
  for (int y = 0; y < out.h; y++)
    for (int x = 0; x < out.w; x++) {
      int sx = x, sy = y;
      switch (o) {
        case 2: sx = p.w - 1 - x; break;                   // mirrored
        case 3: sx = p.w - 1 - x, sy = p.h - 1 - y; break;  // 180
        case 4: sy = p.h - 1 - y; break;                   // mirrored vertically
        case 5: sx = y, sy = x; break;                     // transposed
        case 6: sx = y, sy = p.h - 1 - x; break;           // 90 clockwise
        case 7: sx = p.w - 1 - y, sy = p.h - 1 - x; break; // transversed
        case 8: sx = p.w - 1 - y, sy = x; break;           // 90 anticlockwise
      }
      memcpy(out.px(x, y), p.px(sx, sy), 4);
    }
  return out;
}

bool crop_fits(const Picture& p, const Crop& c) {
  return c.x >= 0 && c.y >= 0 && c.w > 0 && c.h > 0 && int64_t(c.x) + c.w <= p.w && int64_t(c.y) + c.h <= p.h;
}

Picture crop(const Picture& p, const Crop& c) {
  if (!crop_fits(p, c))
    invalid("the crop " + std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.w) + "x" +
            std::to_string(c.h) + " lies outside the " + std::to_string(p.w) + "x" + std::to_string(p.h) + " picture");
  Picture out;
  out.w = c.w;
  out.h = c.h;
  out.bgra.resize(size_t(c.w) * size_t(c.h) * 4);
  for (int y = 0; y < c.h; y++) memcpy(out.px(0, y), p.px(c.x, c.y + y), size_t(c.w) * 4);
  return out;
}

int count_colours(const Picture& p, int limit) {
  std::unordered_set<uint32_t> seen;
  for (size_t i = 0; i + 3 < p.bgra.size(); i += 4) {
    uint32_t v;
    memcpy(&v, &p.bgra[i], 4);
    if (seen.insert(v).second && int(seen.size()) >= limit) break;
  }
  return int(seen.size());
}

Picture resize_nearest(const Picture& p, int w, int h) {
  Picture out;
  out.w = w;
  out.h = h;
  out.bgra.resize(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; y++) {
    int sy = std::min(p.h - 1, int((int64_t(y) * p.h) / h));
    for (int x = 0; x < w; x++) {
      int sx = std::min(p.w - 1, int((int64_t(x) * p.w) / w));
      memcpy(out.px(x, y), p.px(sx, sy), 4);
    }
  }
  return out;
}

namespace {

// Catmull-Rom (B = 0, C = 1/2).
double cubic(double x) {
  x = std::fabs(x);
  if (x < 1) return 1.5 * x * x * x - 2.5 * x * x + 1;
  if (x < 2) return -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2;
  return 0;
}

struct Tap {
  int first = 0;
  std::vector<double> w;
};

// Per output pixel, the source pixels it reads and their weights.
std::vector<Tap> taps(int src, int dst) {
  std::vector<Tap> out(static_cast<size_t>(dst));
  const double scale = double(dst) / src;
  const double widen = scale < 1 ? 1 / scale : 1;  // a wider kernel when shrinking
  const double support = 2 * widen;
  for (int i = 0; i < dst; i++) {
    double centre = (i + 0.5) / scale - 0.5;
    int lo = int(std::floor(centre - support)) + 1, hi = int(std::floor(centre + support));
    Tap& t = out[size_t(i)];
    t.first = lo;
    double sum = 0;
    for (int s = lo; s <= hi; s++) {
      double k = cubic((s - centre) / widen);
      t.w.push_back(k);
      sum += k;
    }
    if (sum != 0)
      for (double& k : t.w) k /= sum;
  }
  return out;
}

}  // namespace

Picture resize_cubic(const Picture& p, int w, int h) {
  if (p.empty() || w <= 0 || h <= 0) return {};
  if (w == p.w && h == p.h) return p;
  // Premultiplied floats, so transparent pixels lend no colour.
  std::vector<float> src(size_t(p.w) * size_t(p.h) * 4);
  for (size_t i = 0; i < src.size(); i += 4) {
    float a = p.bgra[i + 3] / 255.0f;
    src[i] = p.bgra[i] * a;
    src[i + 1] = p.bgra[i + 1] * a;
    src[i + 2] = p.bgra[i + 2] * a;
    src[i + 3] = p.bgra[i + 3];
  }
  // Horizontal pass: p.h rows of w.
  std::vector<Tap> tx = taps(p.w, w), ty = taps(p.h, h);
  std::vector<float> mid(size_t(w) * size_t(p.h) * 4);
  for (int y = 0; y < p.h; y++) {
    const float* row = &src[size_t(y) * size_t(p.w) * 4];
    float* o = &mid[size_t(y) * size_t(w) * 4];
    for (int x = 0; x < w; x++) {
      const Tap& t = tx[size_t(x)];
      double acc[4] = {0, 0, 0, 0};
      for (size_t k = 0; k < t.w.size(); k++) {
        int sx = std::clamp(t.first + int(k), 0, p.w - 1);
        const float* s = row + size_t(sx) * 4;
        for (int c = 0; c < 4; c++) acc[c] += s[c] * t.w[k];
      }
      for (int c = 0; c < 4; c++) o[size_t(x) * 4 + size_t(c)] = float(acc[c]);
    }
  }
  // Vertical pass, then back to straight alpha.
  Picture out;
  out.w = w;
  out.h = h;
  out.bgra.resize(size_t(w) * size_t(h) * 4);
  for (int y = 0; y < h; y++) {
    const Tap& t = ty[size_t(y)];
    for (int x = 0; x < w; x++) {
      double acc[4] = {0, 0, 0, 0};
      for (size_t k = 0; k < t.w.size(); k++) {
        int sy = std::clamp(t.first + int(k), 0, p.h - 1);
        const float* s = &mid[(size_t(sy) * size_t(w) + size_t(x)) * 4];
        for (int c = 0; c < 4; c++) acc[c] += s[c] * t.w[k];
      }
      uint8_t* d = out.px(x, y);
      double a = std::clamp(acc[3], 0.0, 255.0);
      d[3] = clamp8(a);
      for (int c = 0; c < 3; c++) d[c] = a > 0 ? clamp8(acc[c] * 255.0 / a) : 0;
    }
  }
  return out;
}

bool uses_nearest_first(int src_w, int src_h, int w, int h, bool few_colours) {
  double scale = std::min(double(w) / src_w, double(h) / src_h);
  return few_colours && scale >= 2;
}

Picture scale_for_tile(const Picture& p, int w, int h, bool few_colours) {
  if (p.empty()) return {};
  if (uses_nearest_first(p.w, p.h, w, h, few_colours)) {
    int k = int(std::floor(std::min(double(w) / p.w, double(h) / p.h)));
    Picture big = resize_nearest(p, p.w * k, p.h * k);
    return resize_cubic(big, w, h);
  }
  return resize_cubic(p, w, h);
}

Picture cap_long_side(const Picture& p, int max_side) {
  int longest = std::max(p.w, p.h);
  if (longest <= max_side) return p;
  double s = double(max_side) / longest;
  int w = std::max(1, int(std::lround(p.w * s))), h = std::max(1, int(std::lround(p.h * s)));
  return resize_cubic(p, std::min(w, max_side), std::min(h, max_side));
}

// ---- the tile rules (COVERS.md §2.6) ---------------------------------------------------------

bool fills_tile(int w, int h) {
  if (w <= 0 || h <= 0) return false;
  // Within ±15% of 4:5, compared exactly in integers: 0.68 <= w/h <= 0.92.
  return int64_t(w) * 100 >= int64_t(h) * 68 && int64_t(w) * 100 <= int64_t(h) * 92;
}

TileLayout plan_tile(int w, int h, std::string_view art) {
  TileLayout t;
  if (art == "disc") {
    int m = std::min(w, h);
    t.mode = TileMode::disc;
    t.source = Crop{(w - m) / 2, (h - m) / 2, m, m};
    t.x = kDiscCentreX - kDiscDiameter / 2;
    t.y = kDiscCentreY - kDiscDiameter / 2;
    t.w = t.h = kDiscDiameter;
    return t;
  }
  t.source = Crop{0, 0, w, h};
  if (fills_tile(w, h)) {
    // Scale to cover the tile, centred; the overhang is cut.
    t.mode = TileMode::fill;
    double s = std::max(double(kTileW) / w, double(kTileH) / h);
    t.w = std::max(kTileW, int(std::lround(w * s)));
    t.h = std::max(kTileH, int(std::lround(h * s)));
  } else {
    // Scale to fit, centred, on bands.
    t.mode = TileMode::contain;
    t.bands_top_bottom = int64_t(w) * kTileH > int64_t(h) * kTileW;  // wider than 4:5
    if (t.bands_top_bottom) {
      t.w = kTileW;
      t.h = std::clamp(int(std::lround(double(h) * kTileW / w)), 1, kTileH);
    } else {
      t.h = kTileH;
      t.w = std::clamp(int(std::lround(double(w) * kTileH / h)), 1, kTileW);
    }
  }
  t.x = (kTileW - t.w) / 2;
  t.y = (kTileH - t.h) / 2;
  return t;
}

namespace {

// The alpha-weighted mean of a set of pixels (the night's dark end when all
// of them are transparent).
struct MeanAcc {
  double r = 0, g = 0, b = 0, a = 0;
  void add(const uint8_t* px) {
    double w = px[3];
    b += px[0] * w;
    g += px[1] * w;
    r += px[2] * w;
    a += w;
  }
  Rgb get() const {
    if (a <= 0) return kNightBottom;
    return Rgb{clamp8(r / a), clamp8(g / a), clamp8(b / a)};
  }
};

// Fill mode shows no band; transparency is flattened onto the border's mean.
Rgb border_colour(const Picture& p) {
  MeanAcc m;
  for (int x = 0; x < p.w; x++) {
    m.add(p.px(x, 0));
    if (p.h > 1) m.add(p.px(x, p.h - 1));
  }
  for (int y = 1; y + 1 < p.h; y++) {
    m.add(p.px(0, y));
    if (p.w > 1) m.add(p.px(p.w - 1, y));
  }
  return m.get();
}

}  // namespace

Rgb band_colour(const Picture& p, bool top_bottom) {
  MeanAcc m;
  if (p.empty()) return kNightBottom;
  if (top_bottom) {
    for (int x = 0; x < p.w; x++) {
      m.add(p.px(x, 0));
      if (p.h > 1) m.add(p.px(x, p.h - 1));
    }
  } else {
    for (int y = 0; y < p.h; y++) {
      m.add(p.px(0, y));
      if (p.w > 1) m.add(p.px(p.w - 1, y));
    }
  }
  return m.get();
}

Rgb night(int y) {
  double t = std::clamp(double(y) / (kTileH - 1), 0.0, 1.0);
  auto mix = [&](uint8_t a, uint8_t b) { return clamp8(a + (double(b) - a) * t); };
  return Rgb{mix(kNightTop.r, kNightBottom.r), mix(kNightTop.g, kNightBottom.g), mix(kNightTop.b, kNightBottom.b)};
}

double disc_coverage(int x, int y) {
  double dx = x + 0.5 - kDiscCentreX, dy = y + 0.5 - kDiscCentreY;
  double d = std::sqrt(dx * dx + dy * dy);
  return std::clamp(kDiscDiameter / 2.0 + 0.5 - d, 0.0, 1.0);
}

Picture render_tile(const Picture& p, std::string_view art) {
  if (p.empty()) invalid("an empty picture has no tile");
  TileLayout t = plan_tile(p.w, p.h, art);
  Picture out = Picture::filled(kTileW, kTileH, 0, 0, 0);
  auto put = [&](int x, int y, Rgb bg, const uint8_t* src, double coverage) {
    uint8_t* d = out.px(x, y);
    double a = src ? src[3] / 255.0 * coverage : 0;
    d[0] = clamp8((src ? src[0] : 0) * a + bg.b * (1 - a));
    d[1] = clamp8((src ? src[1] : 0) * a + bg.g * (1 - a));
    d[2] = clamp8((src ? src[2] : 0) * a + bg.r * (1 - a));
    d[3] = 255;
  };
  if (t.mode == TileMode::disc) {
    Picture sq = crop(p, t.source);
    Picture scaled = scale_for_tile(sq, t.w, t.h, count_colours(sq) <= 256);
    for (int y = 0; y < kTileH; y++) {
      Rgb bg = night(y);
      for (int x = 0; x < kTileW; x++) {
        // The anti-aliased rim reaches half a pixel past the square: it
        // takes the colour of the nearest edge pixel.
        double cov = disc_coverage(x, y);
        int sx = std::clamp(x - t.x, 0, t.w - 1), sy = std::clamp(y - t.y, 0, t.h - 1);
        put(x, y, bg, cov > 0 ? scaled.px(sx, sy) : nullptr, cov);
      }
    }
    return out;
  }
  Rgb bg = t.mode == TileMode::contain ? band_colour(p, t.bands_top_bottom) : border_colour(p);
  Picture scaled = scale_for_tile(p, t.w, t.h, count_colours(p) <= 256);
  for (int y = 0; y < kTileH; y++)
    for (int x = 0; x < kTileW; x++) {
      int sx = x - t.x, sy = y - t.y;
      bool inside = sx >= 0 && sy >= 0 && sx < t.w && sy < t.h;
      put(x, y, bg, inside ? scaled.px(sx, sy) : nullptr, 1);
    }
  return out;
}

// ---- files -------------------------------------------------------------------------------------

std::vector<uint8_t> read_picture_file(const fs::path& path) {
  Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr));
  if (!h.valid()) invalid("cannot read " + to_utf8(path.wstring()) + ": " + win_error_string(GetLastError()));
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h.get(), &size)) invalid("cannot read " + to_utf8(path.wstring()));
  if (uint64_t(size.QuadPart) > kMaxPictureBytes)
    invalid(to_utf8(path.filename().wstring()) + " is larger than 64 MB. " + kCannotRead);
  std::vector<uint8_t> data(size_t(size.QuadPart));
  DWORD got = 0;
  if (!data.empty() && (!ReadFile(h.get(), data.data(), DWORD(data.size()), &got, nullptr) || got != data.size()))
    invalid("cannot read " + to_utf8(path.wstring()) + ": " + win_error_string(GetLastError()));
  return data;
}

// ---- WIC ------------------------------------------------------------------------------------------

namespace {

template <typename T>
class Com {
 public:
  Com() = default;
  ~Com() { reset(); }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  T** put() {
    reset();
    return &p_;
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

// COM on this thread for as long as it lives: a thread that already has an
// apartment (the GUI's) keeps it.
class ComScope {
 public:
  ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  HRESULT hr_;
};

std::string hr_text(HRESULT hr) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
  return b;
}

void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw ImportError(Status::error, std::string("WIC: ") + what + " failed (" + hr_text(hr) + ")");
}

Com<IWICImagingFactory> factory() {
  Com<IWICImagingFactory> f;
  check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                         (void**)f.put()),
        "CoCreateInstance(WICImagingFactory)");
  return f;
}

int exif_orientation(IWICBitmapFrameDecode* frame) {
  Com<IWICMetadataQueryReader> q;
  if (FAILED(frame->GetMetadataQueryReader(q.put())) || !q) return 1;
  for (const wchar_t* name : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
    PROPVARIANT v;
    PropVariantInit(&v);
    int o = 0;
    if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
      if (v.vt == VT_UI2) o = v.uiVal;
      else if (v.vt == VT_UI4) o = int(v.ulVal);
    }
    PropVariantClear(&v);
    if (o >= 1 && o <= 8) return o;
  }
  return 1;
}

}  // namespace

Picture decode_picture(std::span<const uint8_t> bytes, int prescale_long_side) {
  if (bytes.size() > kMaxPictureBytes) invalid(std::string("the picture is larger than 64 MB. ") + kCannotRead);
  if (bytes.empty()) invalid(std::string("the picture is empty. ") + kCannotRead);
  ComScope com;
  Com<IWICImagingFactory> f = factory();
  Com<IWICStream> stream;
  check(f->CreateStream(stream.put()), "CreateStream");
  check(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), DWORD(bytes.size())), "InitializeFromMemory");
  Com<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put())))
    invalid(kCannotRead);
  Com<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, frame.put()))) invalid(kCannotRead);
  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h))) invalid(kCannotRead);
  if (int64_t(w) < kMinSide || int64_t(h) < kMinSide)
    invalid("the picture is " + std::to_string(w) + "x" + std::to_string(h) +
            " pixels; a cover needs at least 32 on each side");
  if (int64_t(w) > kMaxSide || int64_t(h) > kMaxSide)
    invalid("the picture is " + std::to_string(w) + "x" + std::to_string(h) +
            " pixels; a cover can be at most 16384 on each side");
  const int orientation = exif_orientation(frame.get());

  // Premultiplied through the scaler (so transparent pixels lend it no
  // colour), straight otherwise.
  const bool prescale = prescale_long_side > 0 && int(std::max(w, h)) > prescale_long_side;
  Com<IWICFormatConverter> conv;
  check(f->CreateFormatConverter(conv.put()), "CreateFormatConverter");
  if (FAILED(conv->Initialize(frame.get(), prescale ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    invalid(kCannotRead);
  IWICBitmapSource* src = conv.get();
  Com<IWICBitmapScaler> scaler;
  UINT ow = w, oh = h;
  if (prescale) {
    double s = double(prescale_long_side) / std::max(w, h);
    ow = UINT(std::max<long>(1, std::lround(w * s)));
    oh = UINT(std::max<long>(1, std::lround(h * s)));
    check(f->CreateBitmapScaler(scaler.put()), "CreateBitmapScaler");
    // WICBitmapInterpolationModeHighQualityCubic (Windows 10+), which this
    // toolchain's wincodec.h does not name.
    check(scaler->Initialize(conv.get(), ow, oh, WICBitmapInterpolationMode(4)), "scaler");
    src = scaler.get();
  }
  Picture p;
  p.w = int(ow);
  p.h = int(oh);
  p.bgra.resize(size_t(ow) * size_t(oh) * 4);
  if (FAILED(src->CopyPixels(nullptr, ow * 4, UINT(p.bgra.size()), p.bgra.data()))) invalid(kCannotRead);
  if (prescale) {
    for (size_t i = 0; i < p.bgra.size(); i += 4) {
      uint8_t a = p.bgra[i + 3];
      for (int c = 0; c < 3; c++) p.bgra[i + size_t(c)] = a ? clamp8(p.bgra[i + size_t(c)] * 255.0 / a) : 0;
    }
  }
  return orient(p, orientation);
}

Picture normalize(std::span<const uint8_t> bytes, const Crop& c) {
  const bool cropping = c.w > 0;
  // Uncropped, WIC can shrink a large picture while it decodes it.
  Picture p = decode_picture(bytes, cropping ? 0 : kMaxStoredSide);
  if (cropping) p = crop(p, c);
  return cap_long_side(p, kMaxStoredSide);
}

namespace {

struct Container {
  std::string_view name;
  const GUID* guid;
};

std::vector<uint8_t> stream_bytes(IStream* s) {
  STATSTG st{};
  check(s->Stat(&st, STATFLAG_NONAME), "Stat");
  std::vector<uint8_t> out(size_t(st.cbSize.QuadPart));
  LARGE_INTEGER zero{};
  check(s->Seek(zero, STREAM_SEEK_SET, nullptr), "Seek");
  ULONG got = 0;
  if (!out.empty()) check(s->Read(out.data(), ULONG(out.size()), &got), "Read");
  out.resize(got);
  return out;
}

}  // namespace

std::vector<uint8_t> encode_picture(const Picture& p, std::string_view format, int exif_orientation) {
  if (p.empty()) throw ImportError(Status::error, "cannot encode an empty picture");
  static const Container kContainers[] = {{"png", &GUID_ContainerFormatPng},   {"jpeg", &GUID_ContainerFormatJpeg},
                                          {"bmp", &GUID_ContainerFormatBmp},   {"gif", &GUID_ContainerFormatGif},
                                          {"tiff", &GUID_ContainerFormatTiff}};
  const GUID* container = nullptr;
  for (const Container& c : kContainers)
    if (c.name == format) container = c.guid;
  if (!container) throw ImportError(Status::error, "unknown picture format " + std::string(format));
  // "png24": RGB PNG, for the opaque tile.
  ComScope com;
  Com<IWICImagingFactory> f = factory();
  Com<IStream> mem;
  check(CreateStreamOnHGlobal(nullptr, TRUE, mem.put()), "CreateStreamOnHGlobal");
  Com<IWICBitmapEncoder> enc;
  check(f->CreateEncoder(*container, nullptr, enc.put()), "CreateEncoder");
  check(enc->Initialize(mem.get(), WICBitmapEncoderNoCache), "encoder Initialize");
  Com<IWICBitmapFrameEncode> frame;
  Com<IPropertyBag2> props;
  check(enc->CreateNewFrame(frame.put(), props.put()), "CreateNewFrame");
  check(frame->Initialize(props.get()), "frame Initialize");
  check(frame->SetSize(UINT(p.w), UINT(p.h)), "SetSize");
  Com<IWICBitmap> bmp;
  check(f->CreateBitmapFromMemory(UINT(p.w), UINT(p.h), GUID_WICPixelFormat32bppBGRA, UINT(p.w) * 4,
                                  UINT(p.bgra.size()), const_cast<BYTE*>(p.bgra.data()), bmp.put()),
        "CreateBitmapFromMemory");
  WICPixelFormatGUID want = GUID_WICPixelFormat32bppBGRA;
  if (format == "jpeg") want = GUID_WICPixelFormat24bppBGR;
  if (format == "gif") want = GUID_WICPixelFormat8bppIndexed;
  WICPixelFormatGUID got = want;
  check(frame->SetPixelFormat(&got), "SetPixelFormat");
  if (exif_orientation > 0 && format == "jpeg") {
    Com<IWICMetadataQueryWriter> q;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(q.put())) && q) {
      PROPVARIANT v;
      PropVariantInit(&v);
      v.vt = VT_UI2;
      v.uiVal = USHORT(exif_orientation);
      check(q->SetMetadataByName(L"/app1/ifd/{ushort=274}", &v), "write the EXIF orientation");
    }
  }
  Com<IWICPalette> palette;
  Com<IWICFormatConverter> conv;
  check(f->CreateFormatConverter(conv.put()), "CreateFormatConverter");
  if (IsEqualGUID(got, GUID_WICPixelFormat8bppIndexed)) {
    check(f->CreatePalette(palette.put()), "CreatePalette");
    check(palette->InitializeFromBitmap(bmp.get(), 256, FALSE), "palette");
    check(conv->Initialize(bmp.get(), got, WICBitmapDitherTypeNone, palette.get(), 0, WICBitmapPaletteTypeCustom),
          "convert");
    check(frame->SetPalette(palette.get()), "SetPalette");
  } else {
    check(conv->Initialize(bmp.get(), got, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
          "convert");
  }
  check(frame->WriteSource(conv.get(), nullptr), "WriteSource");
  check(frame->Commit(), "frame Commit");
  check(enc->Commit(), "encoder Commit");
  return stream_bytes(mem.get());
}

std::vector<uint8_t> encode_png(const Picture& p, bool alpha) {
  if (alpha) return encode_picture(p, "png");
  // Opaque: an RGB PNG. WIC writes 24bpp when handed 24bpp pixels.
  if (p.empty()) throw ImportError(Status::error, "cannot encode an empty picture");
  std::vector<uint8_t> rgb(size_t(p.w) * size_t(p.h) * 3);
  for (size_t i = 0, j = 0; i < p.bgra.size(); i += 4, j += 3) memcpy(&rgb[j], &p.bgra[i], 3);
  ComScope com;
  Com<IWICImagingFactory> f = factory();
  Com<IStream> mem;
  check(CreateStreamOnHGlobal(nullptr, TRUE, mem.put()), "CreateStreamOnHGlobal");
  Com<IWICBitmapEncoder> enc;
  check(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put()), "CreateEncoder");
  check(enc->Initialize(mem.get(), WICBitmapEncoderNoCache), "encoder Initialize");
  Com<IWICBitmapFrameEncode> frame;
  Com<IPropertyBag2> props;
  check(enc->CreateNewFrame(frame.put(), props.put()), "CreateNewFrame");
  check(frame->Initialize(props.get()), "frame Initialize");
  check(frame->SetSize(UINT(p.w), UINT(p.h)), "SetSize");
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  check(frame->SetPixelFormat(&fmt), "SetPixelFormat");
  if (!IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR))
    throw ImportError(Status::error, "WIC: the PNG encoder refused 24bpp pixels");
  check(frame->WritePixels(UINT(p.h), UINT(p.w) * 3, UINT(rgb.size()), rgb.data()), "WritePixels");
  check(frame->Commit(), "frame Commit");
  check(enc->Commit(), "encoder Commit");
  return stream_bytes(mem.get());
}

// ---- bitmap resources -------------------------------------------------------------------------------

std::vector<uint8_t> dib_to_bmp(std::string_view dib) {
  auto u16 = [&](size_t o) { return o + 2 <= dib.size() ? uint16_t(uint8_t(dib[o]) | uint8_t(dib[o + 1]) << 8) : 0; };
  auto u32 = [&](size_t o) { return o + 4 <= dib.size() ? uint32_t(u16(o)) | uint32_t(u16(o + 2)) << 16 : 0u; };
  if (dib.size() < 12) invalid("the bitmap resource is too short");
  // A resource is at most a few MB, so every size here fits in 32 bits once
  // it is known to fit in the resource; they are computed in 64 bits, so a
  // damaged count cannot wrap into a plausible one.
  const uint32_t header = u32(0);
  uint64_t palette = 0;
  if (header == 12) {
    // BITMAPCOREHEADER: RGBTRIPLE entries.
    uint16_t bits = u16(10);
    palette = bits <= 8 ? (uint64_t(1) << bits) * 3 : 0;
  } else if (header >= 40 && header <= dib.size()) {
    uint16_t bits = u16(14);
    uint32_t compression = u32(16), used = u32(32);
    // biClrUsed: at most the 2^bits colours a paletted picture can index
    // (more is a damaged header, not a bigger table); above 8 bits it is an
    // optional table, whose size the resource bounds below.
    if (bits <= 8 && used > (1u << bits))
      invalid("the bitmap resource is damaged (" + std::to_string(used) + " colours for " + std::to_string(bits) +
              " bits per pixel)");
    uint64_t colours = bits <= 8 ? (used ? used : (1u << bits)) : used;
    palette = colours * 4;
    if (header == 40 && compression == 3) palette += 12;  // BI_BITFIELDS masks
    if (header == 40 && compression == 6) palette += 16;  // BI_ALPHABITFIELDS masks
  } else {
    invalid("the bitmap resource has an unknown header (" + std::to_string(header) + " bytes)");
  }
  if (uint64_t(header) + palette > dib.size()) invalid("the bitmap resource is truncated or its colour count is damaged");
  if (dib.size() > 0xFFFFFFFFull - 14) invalid("the bitmap resource is too large");
  std::vector<uint8_t> bmp(14 + dib.size());
  uint32_t file_size = uint32_t(bmp.size()), off = uint32_t(14 + header + palette);
  bmp[0] = 'B';
  bmp[1] = 'M';
  memcpy(&bmp[2], &file_size, 4);
  memcpy(&bmp[10], &off, 4);
  memcpy(&bmp[14], dib.data(), dib.size());
  return bmp;
}

std::vector<uint8_t> bitmap_resource(std::string_view module, uint16_t id) {
  std::string dib;
  const loader::ResId type = loader::ResId::of(uint16_t(loader::rt::bitmap)), name = loader::ResId::of(id);
  try {
    switch (loader::detect_format(module)) {
      case loader::Format::ne: {
        loader::ne::Image img{std::string(module)};
        if (const auto* r = img.find_resource(type, name)) dib = std::string(img.resource_data(*r));
        else invalid("it has no bitmap " + std::to_string(id));
        break;
      }
      case loader::Format::pe32: {
        loader::pe::Image img{std::string(module)};
        if (const auto* r = img.find_resource(type, name)) dib = std::string(img.resource_data(*r));
        else invalid("it has no bitmap " + std::to_string(id));
        break;
      }
      default:
        invalid("it is not an NE or PE file");
    }
  } catch (const loader::LoaderError& e) {
    invalid(std::string("it is damaged: ") + e.what());
  }
  return dib_to_bmp(dib);
}

}  // namespace adw::import::cover
