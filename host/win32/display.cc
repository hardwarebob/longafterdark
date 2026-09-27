#include "win32/display.hh"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <string>

#include "adw/core/text.h"

namespace adw::win32 {

namespace {

COLORREF rgb_of(const PALETTEENTRY& e) { return RGB(e.peRed, e.peGreen, e.peBlue); }

int dist2(COLORREF a, COLORREF b) {
  int dr = int(GetRValue(a)) - int(GetRValue(b));
  int dg = int(GetGValue(a)) - int(GetGValue(b));
  int db = int(GetBValue(a)) - int(GetBValue(b));
  return dr * dr + dg * dg + db * db;
}

}  // namespace

const std::array<RGBQUAD, 256>& Display::key_table() {
  static const std::array<RGBQUAD, 256> t = [] {
    std::array<RGBQUAD, 256> k{};
    for (int i = 0; i < 256; i++) k[size_t(i)] = RGBQUAD{BYTE(i), BYTE(i), BYTE(i), 0};
    return k;
  }();
  return t;
}

HBITMAP Display::create_surface8(int w, int h, bool top_down, HANDLE section, DWORD offset, uint8_t** bits) {
  struct {
    BITMAPINFOHEADER h;
    RGBQUAD colors[256];
  } bmi = {};
  bmi.h.biSize = sizeof(BITMAPINFOHEADER);
  bmi.h.biWidth = w;
  bmi.h.biHeight = top_down ? -h : h;
  bmi.h.biPlanes = 1;
  bmi.h.biBitCount = 8;
  bmi.h.biCompression = BI_RGB;
  bmi.h.biClrUsed = 256;
  memcpy(bmi.colors, key_table().data(), sizeof(bmi.colors));
  void* p = nullptr;
  // The DC argument only matters for DIB_PAL_COLORS.
  HBITMAP b = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &p, section, offset);
  if (bits) *bits = static_cast<uint8_t*>(p);
  return b;
}

Display::Display(Screen& screen, cpu::MemoryContext& mem, uint32_t bits_addr)
    : screen_(screen), mem_(mem), bits_(bits_addr), w_(screen.width()), h_(screen.height()) {
  if (bits_ & 3) throw std::invalid_argument("screen bits must be DWORD-aligned");
  // The static colours, black in between (what a freshly booted 8-bit display shows).
  for (int i = 0; i < 256; i++) sys_[size_t(i)] = PALETTEENTRY{0, 0, 0, 0};
  for (int i = 0; i < 10; i++) {
    const RGBQUAD& lo = kStaticColors[size_t(i)];
    const RGBQUAD& hi = kStaticColors[size_t(10 + i)];
    sys_[size_t(i)] = PALETTEENTRY{lo.rgbRed, lo.rgbGreen, lo.rgbBlue, 0};
    sys_[size_t(246 + i)] = PALETTEENTRY{hi.rgbRed, hi.rgbGreen, hi.rgbBlue, 0};
  }

  auto [section, offset] = mem_.section_for(bits_);
  // The whole surface must be inside the arena the section maps.
  host_bits_ = mem_.at<uint8_t>(bits_, bits_size(w_, h_));
  dc_ = CreateCompatibleDC(nullptr);
  if (!dc_) throw std::runtime_error("CreateCompatibleDC failed for the screen");
  uint8_t* gdi_bits = nullptr;
  bmp_ = create_surface8(w_, h_, /*top_down=*/true, static_cast<HANDLE>(section), offset, &gdi_bits);
  if (!bmp_ || !gdi_bits) {
    DWORD err = GetLastError();
    DeleteDC(dc_);
    throw std::runtime_error("CreateDIBSection over emulated memory failed (error " + std::to_string(err) + ")");
  }
  old_bmp_ = SelectObject(dc_, bmp_);
  // GDI's view (gdi_bits) and ours (the arena) are two mappings of one
  // section; Screen reads through ours.
  screen_.attach(host_bits_, ptrdiff_t(pitch()));
  push(0, 256);
}

Display::~Display() {
  screen_.detach();
  if (dc_) {
    SelectObject(dc_, old_bmp_);
    DeleteDC(dc_);
  }
  if (bmp_) DeleteObject(bmp_);
}

void Display::push(int first, int count) {
  if (count <= 0) return;
  RGBQUAD q[256];
  for (int i = 0; i < count; i++) {
    const PALETTEENTRY& e = sys_[size_t(first + i)];
    q[i] = RGBQUAD{e.peBlue, e.peGreen, e.peRed, 0};
  }
  screen_.set_entries(first, count, q);
  changes_++;
}

void Display::set_system_entries(int first, int count, const PALETTEENTRY* e) {
  if (first < 0 || first >= 256) return;
  if (count > 256 - first) count = 256 - first;
  for (int i = 0; i < count; i++) sys_[size_t(first + i)] = e[i];
  push(first, count);
}

int Display::static_low() const { return use_ == SYSPAL_STATIC ? 10 : (use_ == SYSPAL_NOSTATIC ? 1 : 0); }

UINT Display::set_palette_use(UINT use) {
  UINT old = use_;
  if (use == SYSPAL_STATIC || use == SYSPAL_NOSTATIC || use == 3 /*SYSPAL_NOSTATIC256*/) {
    use_ = use;
    if (use == SYSPAL_STATIC && old != SYSPAL_STATIC) {
      // Back to static: the statics reappear.
      for (int i = 0; i < 10; i++) {
        const RGBQUAD& lo = kStaticColors[size_t(i)];
        const RGBQUAD& hi = kStaticColors[size_t(10 + i)];
        sys_[size_t(i)] = PALETTEENTRY{lo.rgbRed, lo.rgbGreen, lo.rgbBlue, 0};
        sys_[size_t(246 + i)] = PALETTEENTRY{hi.rgbRed, hi.rgbGreen, hi.rgbBlue, 0};
      }
      push(0, 10);
      push(246, 10);
    }
  }
  return old;
}

LogicalPalette Display::default_palette() {
  LogicalPalette p;
  for (const RGBQUAD& q : kStaticColors) p.entries.push_back(PALETTEENTRY{q.rgbRed, q.rgbGreen, q.rgbBlue, 0});
  // Realized by definition: entry i is static slot i (0..9) or 236+i (10..19).
  for (int i = 0; i < 20; i++) p.map.push_back(uint8_t(i < 10 ? i : 236 + i));
  p.realized = true;
  return p;
}

UINT Display::realize(LogicalPalette& p, bool background) {
  size_t n = p.entries.size();
  p.map.assign(n, 0);
  if (background) {
    for (size_t i = 0; i < n; i++) p.map[i] = uint8_t(nearest_index(rgb_of(p.entries[i]), false));
    p.realized = true;
    p.realize_serial = 0;  // never "current": animation of a background palette does nothing
    return UINT(n);
  }
  const int lo = static_low(), hi = 256 - lo;
  std::bitset<256> used;
  int next = lo, first_changed = 256, last_changed = -1;
  for (size_t i = 0; i < n; i++) {
    const PALETTEENTRY& e = p.entries[i];
    COLORREF c = rgb_of(e);
    if (e.peFlags & PC_EXPLICIT) {
      p.map[i] = uint8_t(e.peRed);  // the low word of the entry is a hardware index
      continue;
    }
    if (!(e.peFlags & (PC_RESERVED | PC_NOCOLLAPSE))) {
      int match = -1;
      for (int j = 0; j < 256 && match < 0; j++) {
        bool candidate = is_static(j) || (used[size_t(j)] && !(sys_[size_t(j)].peFlags & PC_RESERVED));
        if (candidate && rgb_of(sys_[size_t(j)]) == c) match = j;
      }
      if (match >= 0) {
        p.map[i] = uint8_t(match);
        continue;
      }
    }
    if (next < hi) {
      int slot = next++;
      used.set(size_t(slot));
      PALETTEENTRY ne{e.peRed, e.peGreen, e.peBlue, BYTE(e.peFlags & PC_RESERVED)};
      if (memcmp(&ne, &sys_[size_t(slot)], sizeof(ne)) != 0) {
        sys_[size_t(slot)] = ne;
        if (slot < first_changed) first_changed = slot;
        last_changed = slot;
      }
      p.map[i] = uint8_t(slot);
    } else {
      p.map[i] = uint8_t(nearest_index(c, false));
    }
  }
  p.realized = true;
  p.realize_serial = ++serial_;
  if (last_changed >= 0) push(first_changed, last_changed - first_changed + 1);
  return UINT(n);
}

void Display::animate(LogicalPalette& p, int start, int count, const PALETTEENTRY* e) {
  bool current = is_current(p);
  int first_changed = 256, last_changed = -1;
  for (int k = 0; k < count; k++) {
    int i = start + k;
    if (i < 0 || size_t(i) >= p.entries.size()) break;
    PALETTEENTRY& le = p.entries[size_t(i)];
    if (!(le.peFlags & PC_RESERVED)) continue;
    le.peRed = e[k].peRed;
    le.peGreen = e[k].peGreen;
    le.peBlue = e[k].peBlue;
    if (!current) continue;
    int slot = p.map[size_t(i)];
    if (!(sys_[size_t(slot)].peFlags & PC_RESERVED)) continue;  // collapsed or explicit: not ours to change
    sys_[size_t(slot)].peRed = e[k].peRed;
    sys_[size_t(slot)].peGreen = e[k].peGreen;
    sys_[size_t(slot)].peBlue = e[k].peBlue;
    if (slot < first_changed) first_changed = slot;
    if (slot > last_changed) last_changed = slot;
  }
  if (last_changed >= 0) push(first_changed, last_changed - first_changed + 1);
}

int Display::nearest_index(COLORREF rgb, bool statics_only) const {
  int best = 0, best_d = INT32_MAX;
  for (int j = 0; j < 256; j++) {
    if (statics_only && !is_static(j)) continue;
    int d = dist2(rgb & 0xFFFFFF, rgb_of(sys_[size_t(j)]));
    if (d < best_d) {
      best_d = d;
      best = j;
      if (!d) break;
    }
  }
  return best;
}

int Display::nearest_in(const LogicalPalette& p, COLORREF rgb) {
  int best = 0, best_d = INT32_MAX;
  for (size_t j = 0; j < p.entries.size(); j++) {
    int d = dist2(rgb & 0xFFFFFF, rgb_of(p.entries[j]));
    if (d < best_d) {
      best_d = d;
      best = int(j);
      if (!d) break;
    }
  }
  return best;
}

int Display::nearest_in(const RGBQUAD* table, int count, COLORREF rgb) {
  int best = 0, best_d = INT32_MAX;
  for (int j = 0; j < count; j++) {
    int d = dist2(rgb & 0xFFFFFF, RGB(table[j].rgbRed, table[j].rgbGreen, table[j].rgbBlue));
    if (d < best_d) {
      best_d = d;
      best = j;
      if (!d) break;
    }
  }
  return best;
}

int Display::map_index(COLORREF c, const LogicalPalette* pal) const {
  switch (c >> 24) {
    case 0x01: {  // PALETTEINDEX
      size_t i = c & 0xFFFF;
      if (!pal || i >= pal->entries.size()) return 0;
      if (pal->realized && i < pal->map.size()) return pal->map[i];
      return nearest_index(rgb_of(pal->entries[i]), false);
    }
    case 0x02:  // PALETTERGB
      return device_index_for_rgb(c & 0xFFFFFF, pal);
    case 0x10:  // DIBINDEX (a hardware index as far as a device surface goes)
      return int(c & 0xFF);
    default:
      return nearest_index(c & 0xFFFFFF, true);
  }
}

int Display::device_index_for_rgb(COLORREF rgb, const LogicalPalette* pal) const {
  if (!pal || pal->entries.empty()) return nearest_index(rgb, false);
  int j = nearest_in(*pal, rgb);
  if (pal->realized && size_t(j) < pal->map.size()) return pal->map[size_t(j)];
  return nearest_index(rgb_of(pal->entries[size_t(j)]), false);
}

int Display::device_caps(int index) const {
  switch (index) {
    case DRIVERVERSION: return 0x0400;
    case TECHNOLOGY: return DT_RASDISPLAY;
    case HORZSIZE: return w_ * 254 / 960;   // mm at 96 dpi
    case VERTSIZE: return h_ * 254 / 960;
    case HORZRES: return w_;
    case VERTRES: return h_;
    case BITSPIXEL: return 8;
    case PLANES: return 1;
    case NUMBRUSHES: return -1;
    case NUMPENS: return 80;
    case NUMMARKERS: return 0;
    case NUMFONTS: return 0;
    case NUMCOLORS: return 20;  // the statics; PSYCHO refuses 1..16 (ABI.md §2.5)
    case PDEVICESIZE: return 0;
    case CURVECAPS: return 0xFF;
    case LINECAPS: return 0xFE;
    case POLYGONALCAPS: return 0xFF;
    case TEXTCAPS: return 0x7E07;
    case CLIPCAPS: return CP_RECTANGLE;
    case RASTERCAPS:
      return RC_BITBLT | RC_BITMAP64 | RC_GDI20_OUTPUT | RC_DI_BITMAP | RC_PALETTE | RC_DIBTODEV | RC_BIGFONT |
             RC_STRETCHBLT | RC_FLOODFILL | RC_STRETCHDIB | RC_OP_DX_OUTPUT;
    case ASPECTX: return 36;
    case ASPECTY: return 36;
    case ASPECTXY: return 51;
    case LOGPIXELSX: return 96;
    case LOGPIXELSY: return 96;
    case SIZEPALETTE: return 256;
    case NUMRESERVED: return 20;
    case COLORRES: return 18;  // 6 bits per gun: a VGA DAC
    case VREFRESH: return 60;
    case DESKTOPVERTRES: return h_;
    case DESKTOPHORZRES: return w_;
    default: return 0;
  }
}

// ---- desktop seed ----------------------------------------------------------------------------------

namespace {

// A decoded RGB image, top-down, 3 bytes per pixel.
struct Rgb {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};

std::vector<uint8_t> read_file(const std::string& path, bool* ok) {
  std::vector<uint8_t> out;
  *ok = false;
  // Share write + delete: the .scr's seed is a delete-on-close file it keeps open
  // (INTERACTION.md §8), which only such an open may read.
  HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return out;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 30)) {
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    *ok = ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
  }
  CloseHandle(f);
  return out;
}

// Binary P6, maxval 255, comments allowed in the header.
bool decode_p6(const std::vector<uint8_t>& raw, Rgb* img) {
  if (raw.size() < 8 || raw[0] != 'P' || raw[1] != '6') return false;
  size_t i = 2;
  int v[3] = {0, 0, 0};
  for (int n = 0; n < 3; n++) {
    while (i < raw.size() && (isspace(raw[i]) || raw[i] == '#')) {
      if (raw[i] == '#') {
        while (i < raw.size() && raw[i] != '\n') i++;
      } else {
        i++;
      }
    }
    if (i >= raw.size() || !isdigit(raw[i])) return false;
    while (i < raw.size() && isdigit(raw[i]) && v[n] < 1'000'000) v[n] = v[n] * 10 + (raw[i++] - '0');
  }
  if (v[0] <= 0 || v[1] <= 0 || v[2] != 255 || i >= raw.size() || !isspace(raw[i])) return false;
  i++;  // the single whitespace byte after maxval
  size_t n = size_t(v[0]) * size_t(v[1]) * 3;
  if (raw.size() - i < n) return false;
  img->w = v[0];
  img->h = v[1];
  img->px.assign(raw.begin() + ptrdiff_t(i), raw.begin() + ptrdiff_t(i + n));
  return true;
}

// Any BMP Windows reads, through GDI, converted to RGB.
bool decode_bmp(const std::string& path, Rgb* img) {
  HBITMAP b = static_cast<HBITMAP>(
      LoadImageW(nullptr, widen(path).c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
  if (!b) return false;
  BITMAP bm{};
  bool ok = false;
  if (GetObjectW(b, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> bgra(size_t(bm.bmWidth) * size_t(bm.bmHeight) * 4);
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc && GetDIBits(dc, b, 0, UINT(bm.bmHeight), bgra.data(), &bi, DIB_RGB_COLORS) == bm.bmHeight) {
      img->w = bm.bmWidth;
      img->h = bm.bmHeight;
      img->px.resize(size_t(img->w) * size_t(img->h) * 3);
      for (size_t k = 0, n = size_t(img->w) * size_t(img->h); k < n; k++) {
        img->px[k * 3 + 0] = bgra[k * 4 + 2];
        img->px[k * 3 + 1] = bgra[k * 4 + 1];
        img->px[k * 3 + 2] = bgra[k * 4 + 0];
      }
      ok = true;
    }
    if (dc) DeleteDC(dc);
  }
  DeleteObject(b);
  return ok;
}

// Scales to w x h: each target pixel averages the source rectangle it covers
// (at least one source pixel), so a big desktop capture shrinks without
// aliasing and a small one grows by nearest neighbour. Integer-exact.
std::vector<uint8_t> resample(const Rgb& s, int w, int h) {
  if (s.w == w && s.h == h) return s.px;
  std::vector<uint8_t> out(size_t(w) * size_t(h) * 3);
  for (int y = 0; y < h; y++) {
    int y0 = int(int64_t(y) * s.h / h), y1 = std::max(y0 + 1, int(int64_t(y + 1) * s.h / h));
    for (int x = 0; x < w; x++) {
      int x0 = int(int64_t(x) * s.w / w), x1 = std::max(x0 + 1, int(int64_t(x + 1) * s.w / w));
      uint64_t acc[3] = {0, 0, 0}, n = 0;
      for (int sy = y0; sy < y1; sy++) {
        for (int sx = x0; sx < x1; sx++) {
          const uint8_t* p = &s.px[(size_t(sy) * size_t(s.w) + size_t(sx)) * 3];
          acc[0] += p[0];
          acc[1] += p[1];
          acc[2] += p[2];
          n++;
        }
      }
      uint8_t* o = &out[(size_t(y) * size_t(w) + size_t(x)) * 3];
      for (int c = 0; c < 3; c++) o[c] = uint8_t((acc[c] + n / 2) / n);
    }
  }
  return out;
}

}  // namespace

void Display::seed_rgb(const uint8_t* rgb, int w, int h) {
  // The statics, as the hardware palette holds them now.
  std::vector<int> idx;
  for (int j = 0; j < 256; j++)
    if (is_static(j)) idx.push_back(j);
  if (idx.empty()) idx.push_back(0);
  // Floyd-Steinberg; errors carried x16 with one pad cell on each side.
  std::vector<int> cur(size_t(w + 2) * 3, 0), next(size_t(w + 2) * 3, 0);
  for (int y = 0; y < h && y < h_; y++) {
    std::fill(next.begin(), next.end(), 0);
    uint8_t* row = host_bits_ + size_t(y) * pitch();
    for (int x = 0; x < w; x++) {
      const uint8_t* p = rgb + (size_t(y) * size_t(w) + size_t(x)) * 3;
      int c[3];
      for (int k = 0; k < 3; k++) c[k] = std::clamp(int(p[k]) + cur[size_t(x + 1) * 3 + size_t(k)] / 16, 0, 255);
      int best = idx[0], best_d = INT32_MAX;
      for (int j : idx) {
        const PALETTEENTRY& e = sys_[size_t(j)];
        int dr = c[0] - e.peRed, dg = c[1] - e.peGreen, db = c[2] - e.peBlue;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
          best_d = d;
          best = j;
        }
      }
      if (x < w_) row[x] = uint8_t(best);
      const PALETTEENTRY& e = sys_[size_t(best)];
      int err[3] = {c[0] - e.peRed, c[1] - e.peGreen, c[2] - e.peBlue};
      for (int k = 0; k < 3; k++) {
        cur[size_t(x + 2) * 3 + size_t(k)] += err[k] * 7;
        next[size_t(x) * 3 + size_t(k)] += err[k] * 3;
        next[size_t(x + 1) * 3 + size_t(k)] += err[k] * 5;
        next[size_t(x + 2) * 3 + size_t(k)] += err[k] * 1;
      }
    }
    std::swap(cur, next);
  }
}

bool Display::seed(const std::string& spec, std::string* what) {
  std::string dummy;
  if (!what) what = &dummy;
  const size_t n = size_t(w_) * size_t(h_);
  if (spec == ":win95") {
    int teal = nearest_index(RGB(0, 128, 128), /*statics_only=*/true);
    for (int y = 0; y < h_; y++) memset(host_bits_ + size_t(y) * pitch(), teal, size_t(w_));
    *what = "the Windows 95 desktop (teal, index " + std::to_string(teal) + ")";
    return true;
  }
  if (!spec.empty() && spec[0] == ':') {
    *what = "unknown built-in seed '" + spec + "' (known: :win95)";
    return false;
  }
  bool ok = false;
  std::vector<uint8_t> raw = read_file(spec, &ok);
  if (!ok) {
    *what = "cannot read '" + spec + "'";
    return false;
  }
  if (raw.size() == n) {
    for (int y = 0; y < h_; y++) memcpy(host_bits_ + size_t(y) * pitch(), raw.data() + size_t(y) * size_t(w_), size_t(w_));
    *what = "raw index file '" + spec + "'";
    return true;
  }
  Rgb img;
  const char* kind = nullptr;
  if (decode_p6(raw, &img)) {
    kind = "P6";
  } else if (raw.size() > 2 && raw[0] == 'B' && raw[1] == 'M' && decode_bmp(spec, &img)) {
    kind = "BMP";
  }
  if (!kind) {
    *what = "'" + spec + "' is neither " + std::to_string(n) + " raw index bytes, a P6 PPM (maxval 255) nor a BMP";
    return false;
  }
  std::vector<uint8_t> px = resample(img, w_, h_);
  seed_rgb(px.data(), w_, h_);
  *what = std::string(kind) + " '" + spec + "' " + std::to_string(img.w) + "x" + std::to_string(img.h) +
          (img.w == w_ && img.h == h_ ? "" : " scaled to " + std::to_string(w_) + "x" + std::to_string(h_)) +
          ", dithered onto the static colours";
  return true;
}

}  // namespace adw::win32
