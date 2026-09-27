// adw_ui's tests (COVERS.md §3.1): ui.theme, ui.image, ui.capture and
// ui.slider (init_slider, AUDIO.md §9).
//
//   test_ui theme|image|capture|slider [scratch dir]
//
// Everything is drawn into memory bitmaps, except ui.capture's window, which
// is parked off every monitor, cloaked and never activated (as the screenshot
// hooks do), so nothing ever shows on the desktop.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <oleacc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "adw/ui/capture.h"
#include "adw/ui/image.h"
#include "adw/ui/theme.h"
#include "adw/ui/widgets.h"

using namespace adw::ui;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                          \
  } while (0)

std::string hex(COLORREF c) {
  char b[16];
  snprintf(b, sizeof(b), "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
  return b;
}

#define CHECK_COLOR(got, want, tol)                                                                        \
  do {                                                                                                     \
    COLORREF _g = (got), _w = (want);                                                                      \
    if (!near_color(_g, _w, (tol))) {                                                                      \
      g_failures++;                                                                                        \
      fprintf(stderr, "%s:%d: colour %s, want %s (+-%d): %s\n", __FILE__, __LINE__, hex(_g).c_str(),      \
              hex(_w).c_str(), (int)(tol), #got);                                                          \
    }                                                                                                      \
  } while (0)

bool near_color(COLORREF a, COLORREF b, int tol) {
  return std::abs((int)GetRValue(a) - (int)GetRValue(b)) <= tol && std::abs((int)GetGValue(a) - (int)GetGValue(b)) <= tol &&
         std::abs((int)GetBValue(a) - (int)GetBValue(b)) <= tol;
}

// A 32-bit top-down DIB selected into a memory DC: draw, then read pixels.
struct Canvas {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  uint8_t* bits = nullptr;
  int w = 0, h = 0;
  Canvas(int width, int height, COLORREF fill) : w(width), h(height) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    dc = CreateCompatibleDC(screen);
    void* b = nullptr;
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &b, nullptr, 0);
    ReleaseDC(nullptr, screen);
    bits = static_cast<uint8_t*>(b);
    old = SelectObject(dc, bmp);
    RECT r{0, 0, w, h};
    fill_rect(dc, r, fill);
  }
  ~Canvas() {
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
  }
  COLORREF at(int x, int y) {
    GdiFlush();
    const uint8_t* p = bits + ((size_t)y * w + x) * 4;
    return RGB(p[2], p[1], p[0]);
  }
};

COLORREF image_px(const Image& img, int x, int y) {
  const uint8_t* p = img.pbgra.data() + ((size_t)y * img.w + x) * 4;
  return RGB(p[2], p[1], p[0]);
}

// capture_window_png, then the PNG read back into `shot`, until `ready` finds
// what the test drew there (at most about 3 s, settling again in between):
// on a busy machine DWM's copy of a cloaked window can still be the bare
// background when the first capture is taken (QA ui-slider-flaky-under-load).
// The checks then run on the last capture either way.
bool capture_until(HWND h, const fs::path& png, Image& shot, const std::function<bool(const Image&)>& ready,
                   POINT* origin, const char* what) {
  const ULONGLONG until = GetTickCount64() + 3000;
  std::string err;
  for (int attempt = 1;; ++attempt) {
    err.clear();
    const bool got = capture_window_png(h, png.wstring(), &err, origin) && load_image(png.wstring(), shot, &err);
    if (got && ready(shot)) {
      if (attempt > 1) fprintf(stderr, "%s: complete at capture %d\n", what, attempt);
      return true;
    }
    if (GetTickCount64() > until) {
      fprintf(stderr, "%s: not complete after %d captures%s%s\n", what, attempt, err.empty() ? "" : ": ", err.c_str());
      return false;
    }
    settle_for_capture(h, 50);
  }
}

// A w x h picture: `left` on the left half, `right` on the right, opaque.
Image two_tone(int w, int h, COLORREF left, COLORREF right) {
  std::vector<uint8_t> px((size_t)w * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      COLORREF c = x < w / 2 ? left : right;
      uint8_t* p = &px[((size_t)y * w + x) * 4];
      p[0] = GetBValue(c), p[1] = GetGValue(c), p[2] = GetRValue(c), p[3] = 255;
    }
  return image_from_bgra(w, h, px.data(), (size_t)w * 4);
}

// ---- ui.theme -------------------------------------------------------------------------

void test_theme() {
  // The palettes are the scr's, moved unchanged.
  Accent blue{RGB(0x99, 0xEB, 0xFF), RGB(0x4C, 0xC2, 0xFF), RGB(0x00, 0x91, 0xF8), RGB(0x00, 0x78, 0xD4),
              RGB(0x00, 0x5F, 0xB8), RGB(0x00, 0x4C, 0x87), RGB(0x00, 0x3A, 0x68)};
  Palette light = make_palette(false, blue), dark = make_palette(true, blue);
  CHECK(light.base == RGB(0xF3, 0xF3, 0xF3) && light.card == RGB(0xFB, 0xFB, 0xFB));
  CHECK(dark.base == RGB(0x20, 0x20, 0x20) && dark.card == RGB(0x2B, 0x2B, 0x2B));
  CHECK(light.accent == blue.dark1 && dark.accent == blue.light2);
  CHECK(!light.dark && dark.dark && !light.high_contrast);
  CHECK(high_contrast_palette().high_contrast);
  CHECK(blend(RGB(0, 0, 0), RGB(200, 100, 50), 0.5) == RGB(100, 50, 25));
  // A contrast theme's colours standing in for the system's (screenshots only).
  {
    // Only set_test_hc_scheme does it: the library reads no environment.
    SetEnvironmentVariableW(L"AD_UI_TEST_HC_SCHEME", L"nightsky");
    CHECK(high_contrast_palette().base == GetSysColor(COLOR_WINDOW));
    SetEnvironmentVariableW(L"AD_UI_TEST_HC_SCHEME", nullptr);
    CHECK(set_test_hc_scheme(L"nightsky"));
    Palette night = high_contrast_palette();
    CHECK(night.high_contrast && night.dark);
    CHECK(night.base == RGB(0, 0, 0) && night.text == RGB(0xFF, 0xFF, 0xFF));
    CHECK(night.accent == RGB(0xD6, 0xB4, 0xFD) && night.on_accent == RGB(0x2B, 0x2B, 0x2B));
    CHECK(night.accent_text == RGB(0x80, 0x80, 0xFF));
    CHECK(set_test_hc_scheme(L"Desert"));
    Palette desert = high_contrast_palette();
    CHECK(!desert.dark && desert.base == RGB(0xFF, 0xFA, 0xEF) && desert.text == RGB(0x3D, 0x3D, 0x3D));
    CHECK(!set_test_hc_scheme(L"sunrise"));   // unknown: no change
    CHECK(high_contrast_palette().base == RGB(0xFF, 0xFA, 0xEF));
    CHECK(set_test_hc_scheme(nullptr));
    CHECK(high_contrast_palette().base == GetSysColor(COLOR_WINDOW));
  }

  // is_theme_change: exactly the messages after which the theme is reloaded.
  CHECK(is_theme_change(WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet"));
  CHECK(is_theme_change(WM_SETTINGCHANGE, 0, (LPARAM)L"immersivecolorset"));
  CHECK(!is_theme_change(WM_SETTINGCHANGE, 0, (LPARAM)L"Environment"));
  CHECK(!is_theme_change(WM_SETTINGCHANGE, SPI_SETWORKAREA, 0));
  CHECK(is_theme_change(WM_SYSCOLORCHANGE, 0, 0));
  CHECK(is_theme_change(WM_THEMECHANGED, 0, 0));
  CHECK(is_theme_change(WM_DWMCOLORIZATIONCOLORCHANGED, 0, 0));
  CHECK(!is_theme_change(WM_PAINT, 0, 0));
  // Best effort, never a crash, either way (and back to the default).
  allow_dark_menus(true);
  allow_dark_menus(false);

  for (int dpi : {96, 144, 192}) {
    for (ThemeMode mode : {ThemeMode::light, ThemeMode::dark, ThemeMode::high_contrast}) {
      Theme t;
      t.load(mode);
      t.set_dpi(dpi);
      CHECK(t.fonts.body && t.fonts.subtitle && t.fonts.icons && t.base_brush && t.card_brush);
      CHECK(t.px(24) == MulDiv(24, dpi, 96));
      CHECK(t.pal.high_contrast == (mode == ThemeMode::high_contrast));
      if (mode != ThemeMode::high_contrast) CHECK(t.pal.dark == (mode == ThemeMode::dark));

      // paint_card: pal.card inside, the surface untouched at the corners.
      {
        Canvas c(t.px(200), t.px(100), t.pal.base);
        RECT r{t.px(10), t.px(10), t.px(190), t.px(90)};
        paint_card(c.dc, r, t);
        CHECK_COLOR(c.at(t.px(100), t.px(50)), t.pal.card, 0);
        CHECK_COLOR(c.at(t.px(10), t.px(10)), t.pal.base, 12);   // outside the 8-DIP radius
        CHECK_COLOR(c.at(t.px(100), t.px(10)), t.pal.card_stroke, 40);  // the hairline
      }

      // paint_header: the band filled with base, the app mark in the logo box,
      // the name in navy (light) or text, and stars past the text except
      // under high contrast.
      {
        const int W = t.px(640), H = t.px(48);
        Canvas c(W, H, RGB(0xFF, 0, 0xFF));
        RECT band{0, 0, W, H}, logo{t.px(24), t.px(4), t.px(56), t.px(36)}, title{t.px(68), t.px(4), W - t.px(24), t.px(36)};
        paint_header(c.dc, band, logo, title, nullptr, L"Import After Dark", L"From your discs", t);
        CHECK_COLOR(c.at(W - 1, 0), t.pal.base, 0);
        CHECK_COLOR(c.at(0, H - 1), t.pal.base, 0);
        // The mark's night tile (dark navy) inside the logo box, near its bottom-left.
        COLORREF mark = c.at(t.px(24) + t.px(6), t.px(4) + t.px(28));
        CHECK(GetBValue(mark) > GetRValue(mark) && GetRValue(mark) < 0x40);
        // Some ink in the name's box.
        int ink = 0;
        const COLORREF name_ink = t.pal.dark || t.pal.high_contrast ? t.pal.text : RGB(0x1F, 0x25, 0x5A);
        for (int x = t.px(68); x < t.px(200); ++x)
          for (int y = t.px(8); y < t.px(32); ++y) ink += near_color(c.at(x, y), name_ink, 24);
        CHECK(ink > 20);
        // Past the text: stars in dark mode, nothing but base under high contrast.
        int off_base = 0;
        for (int x = t.px(420); x < W - t.px(24); ++x)
          for (int y = 0; y < H; ++y) off_base += c.at(x, y) != t.pal.base;
        if (t.pal.high_contrast) CHECK(off_base == 0);
        if (mode == ThemeMode::dark) CHECK(off_base > 10);
      }
      // With a tagline too long for the band it is cut short, never drawn past `title`.
      {
        const int W = t.px(360), H = t.px(48);
        Canvas c(W, H, RGB(0xFF, 0, 0xFF));
        RECT band{0, 0, W, H}, logo{t.px(24), t.px(4), t.px(56), t.px(36)}, title{t.px(68), t.px(4), W - t.px(24), t.px(36)};
        paint_header(c.dc, band, logo, title, nullptr, L"Import After Dark",
                     L"From your discs or the Internet Archive, and a great deal more besides", t);
        for (int x = W - t.px(22); x < W; ++x)
          for (int y = 0; y < H; ++y)
            if (t.pal.high_contrast) CHECK(c.at(x, y) == t.pal.base);
      }
    }
  }

  // card_height: the minimum, taller with a cover, taller still for wrapped text.
  {
    Theme t;
    t.load(ThemeMode::light);
    t.set_dpi(96);
    const int fm = focus_margin(96);
    CHECK(card_height(t, L"A disc image…\nAn ISO", 600, false) == 64 + 2 * fm);
    CHECK(card_height(t, L"After Dark 4.0 Deluxe\nCD image", 600, true) == 76 + 2 * fm);
    std::wstring long_text = L"Title\n";
    for (int i = 0; i < 40; ++i) long_text += L"many words of description ";
    CHECK(card_height(t, long_text, 400, false) > 64 + 2 * fm);
  }
}

// ---- ui.image -------------------------------------------------------------------------

void test_image(const fs::path& dir) {
  // image_from_bgra premultiplies straight alpha.
  {
    const uint8_t px[8] = {200, 100, 50, 128, 10, 20, 30, 255};
    Image img = image_from_bgra(2, 1, px, 8);
    CHECK(img.w == 2 && img.h == 1 && img.pbgra.size() == 8);
    CHECK(img.pbgra[0] == 100 && img.pbgra[1] == 50 && img.pbgra[2] == 25 && img.pbgra[3] == 128);
    CHECK(img.pbgra[4] == 10 && img.pbgra[7] == 255);
    CHECK(image_from_bgra(0, 5, px, 8).empty());
  }
  const COLORREF red = RGB(0xE0, 0x20, 0x20), blue = RGB(0x20, 0x40, 0xE0), white = RGB(255, 255, 255);

  // 1:1 is exact; a shrink keeps each half's colour away from the seam.
  {
    Image img = two_tone(64, 80, red, blue);
    Canvas c(100, 100, white);
    draw_image(c.dc, RECT{10, 10, 74, 90}, img);
    CHECK_COLOR(c.at(10, 10), red, 0);
    CHECK_COLOR(c.at(73, 89), blue, 0);
    CHECK_COLOR(c.at(9, 9), white, 0);
    CHECK(!img.cache);   // no scaled copy needed at 1:1
    Canvas d(100, 100, white);
    draw_image(d.dc, RECT{0, 0, 32, 40}, img);
    CHECK_COLOR(d.at(2, 20), red, 6);
    CHECK_COLOR(d.at(29, 20), blue, 6);
    CHECK_COLOR(d.at(40, 20), white, 0);
    // The scaled copy is cached, and reused at the same size.
    CHECK(img.cache != nullptr);
    const void* first = img.cache.get();
    draw_image(d.dc, RECT{50, 50, 82, 90}, img);
    CHECK(img.cache.get() == first);
    CHECK_COLOR(d.at(52, 70), red, 6);
    // A copy of the picture draws the same.
    Image copy = img;
    draw_image(d.dc, RECT{0, 50, 16, 70}, copy);
    CHECK_COLOR(d.at(1, 60), red, 12);
    // New pixels: a fresh cache.
    img.pbgra = two_tone(64, 80, blue, red).pbgra;
    draw_image(d.dc, RECT{0, 0, 32, 40}, img);
    CHECK_COLOR(d.at(2, 20), blue, 6);
  }

  // A 640 x 800 tile shrunk 13x to a 48 x 60 cover stays clean (prefiltered).
  {
    std::vector<uint8_t> px(640 * 800 * 4);
    for (int y = 0; y < 800; ++y)
      for (int x = 0; x < 640; ++x) {
        uint8_t* p = &px[((size_t)y * 640 + x) * 4];
        const bool on = ((x / 2) + (y / 2)) % 2 == 0;   // a fine checkerboard: grey once filtered
        p[0] = p[1] = p[2] = on ? 255 : 0;
        p[3] = 255;
      }
    Image tile = image_from_bgra(640, 800, px.data(), 640 * 4);
    Canvas c(48, 60, RGB(0, 0, 0));
    draw_image(c.dc, RECT{0, 0, 48, 60}, tile);
    for (int y = 10; y < 50; y += 7)
      for (int x = 10; x < 38; x += 7) CHECK_COLOR(c.at(x, y), RGB(128, 128, 128), 24);
  }

  // Opacity and rounded corners.
  {
    Image img = two_tone(40, 40, red, red);
    Canvas c(60, 60, white);
    draw_image(c.dc, RECT{10, 10, 50, 50}, img, 0, 128);
    CHECK_COLOR(c.at(30, 30), blend(white, red, 128 / 255.0), 3);
    Canvas d(60, 60, white);
    draw_image(d.dc, RECT{10, 10, 50, 50}, img, 10.0f);
    CHECK_COLOR(d.at(30, 30), red, 0);
    CHECK_COLOR(d.at(10, 10), white, 8);    // the corner is cut
    CHECK_COLOR(d.at(30, 10), red, 0);      // the edge is not
  }

  // load_image: PNG (written by save_png_bgr), missing files, and junk.
  {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<uint8_t> bgr(40 * 50 * 3);
    for (int y = 0; y < 50; ++y)
      for (int x = 0; x < 40; ++x) {
        uint8_t* p = &bgr[((size_t)y * 40 + x) * 3];
        p[0] = (uint8_t)(x * 6), p[1] = (uint8_t)(y * 5), p[2] = 0x80;
      }
    fs::path png = dir / "picture.png";
    std::string err;
    CHECK(save_png_bgr(png.wstring(), 40, 50, bgr, &err));
    Image img;
    CHECK(load_image(png.wstring(), img, &err));
    CHECK(img.w == 40 && img.h == 50);
    if (img.w == 40 && img.h == 50) {
      CHECK(image_px(img, 7, 9) == RGB(0x80, 45, 42));
      CHECK(img.pbgra[3] == 255);
    }
    Image none;
    CHECK(!load_image((dir / "missing.png").wstring(), none, &err));
    CHECK(!err.empty() && none.empty());
    FILE* f = _wfopen((dir / "junk.png").c_str(), L"wb");
    if (f) {
      for (int i = 0; i < 4000; ++i) fputc((i * 7919) & 0xFF, f);
      fclose(f);
    }
    err.clear();
    CHECK(!load_image((dir / "junk.png").wstring(), none, &err));
    CHECK(err.find("can't read") != std::string::npos);
    CoUninitialize();
  }

  // The generated cover: the night gradient, the moon, a white title in the
  // lower part, left at 0.1 w, shrunk to fit.
  for (int w : {48, 64, 128, 640}) {
    const int h = w * 5 / 4;
    Image g = generated_cover_image(w, h, L"The Simpsons Screen Saver", 96);
    CHECK(g.w == w && g.h == h);
    if (g.empty()) continue;
    CHECK_COLOR(image_px(g, 1, 1), RGB(0x26, 0x2B, 0x4F), 6);
    CHECK_COLOR(image_px(g, 1, h - 2), RGB(0x12, 0x15, 0x2A), 6);
    CHECK(g.pbgra[3] == 255);
    // The moon's gold at its lit edge (left of centre).
    const COLORREF moon = image_px(g, (int)(0.72 * w - 0.08 * w), (int)(0.22 * h));
    CHECK(GetRValue(moon) > 0xB0 && GetGValue(moon) > 0x90);
    if (w >= 128) {
      int bright = 0, outside = 0;
      for (int y = (int)(0.47 * h); y < (int)(0.9 * h); ++y)
        for (int x = 0; x < w; ++x) {
          const COLORREF c = image_px(g, x, y);
          if (GetRValue(c) > 200 && GetGValue(c) > 200 && GetBValue(c) > 200) {
            bright++;
            if (x < (int)(0.1 * w) - 1 || x > (int)(0.9 * w) + 1) outside++;
          }
        }
      CHECK(bright > w / 4);
      CHECK(outside == 0);
    }
  }
  // A title that can't fit at 0.14 w still stays inside its column.
  {
    const int w = 256, h = 320;
    Image g = generated_cover_image(w, h, L"Antidisestablishmentarianism Supercalifragilistic Pneumonoultramicroscopic", 96);
    int outside = 0;
    for (int y = (int)(0.47 * h); y < (int)(0.9 * h); ++y)
      for (int x = (int)(0.9 * w) + 2; x < w; ++x) {
        const COLORREF c = image_px(g, x, y);
        outside += GetRValue(c) > 200 && GetGValue(c) > 200;
      }
    CHECK(outside == 0);
  }

  // draw_cover: the tile when there is one, else the generated cover; the
  // hairline border in card_stroke (WindowText under high contrast).
  for (ThemeMode mode : {ThemeMode::light, ThemeMode::dark, ThemeMode::high_contrast}) {
    Theme t;
    t.load(mode);
    t.set_dpi(144);
    Image tile = two_tone(640, 800, red, red);
    Canvas c(t.px(80), t.px(80), t.pal.base);
    RECT art{t.px(8), t.px(4), t.px(8) + t.px(48), t.px(4) + t.px(60)};
    draw_cover(c.dc, art, &tile, L"Deluxe", t);
    CHECK_COLOR(c.at(t.px(32), t.px(34)), red, 2);
    CHECK_COLOR(c.at(art.left, (art.top + art.bottom) / 2), t.pal.card_stroke, 60);
    CHECK_COLOR(c.at(art.left, art.top), t.pal.base, 40);   // rounded corner
    Canvas d(t.px(80), t.px(80), t.pal.base);
    draw_cover(d.dc, art, nullptr, L"Deluxe", t);
    CHECK_COLOR(d.at(art.left + t.px(4), art.top + t.px(4)), RGB(0x26, 0x2B, 0x4F), 8);
    Image empty;
    Canvas e(t.px(80), t.px(80), t.pal.base);
    draw_cover(e.dc, art, &empty, L"Deluxe", t);
    CHECK_COLOR(e.at(art.left + t.px(4), art.top + t.px(4)), RGB(0x26, 0x2B, 0x4F), 8);
  }
}

// ---- ui.capture -----------------------------------------------------------------------

struct CaptureWindow {
  Theme* theme = nullptr;
  int hscrolls = 0;          // WM_HSCROLL from a slider
  int last_code = -1;        // ...and its code
};

LRESULT CALLBACK capture_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto* cw = reinterpret_cast<CaptureWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE:
      SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT cr{};
      GetClientRect(h, &cr);
      fill_rect(dc, cr, cw->theme->pal.base);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_PRINTCLIENT: {
      RECT cr{};
      GetClientRect(h, &cr);
      fill_rect((HDC)wp, cr, cw->theme->pal.base);
      return 0;
    }
    case WM_NOTIFY: {
      auto* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm->code == NM_CUSTOMDRAW) {
        wchar_t cls[32] = {};
        GetClassNameW(nm->hwndFrom, cls, 32);
        if (wcscmp(cls, WC_BUTTONW) == 0) return custom_draw_button(*cw->theme, reinterpret_cast<NMCUSTOMDRAW*>(lp));
        if (wcscmp(cls, TRACKBAR_CLASSW) == 0) return custom_draw_trackbar(*cw->theme, reinterpret_cast<NMCUSTOMDRAW*>(lp));
      }
      break;
    }
    case WM_HSCROLL:
      if (cw) {
        ++cw->hscrolls;
        cw->last_code = LOWORD(wp);
      }
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void test_capture(const fs::path& dir) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = capture_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"AdwUiTestCapture";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  for (ThemeMode mode : {ThemeMode::light, ThemeMode::dark, ThemeMode::high_contrast}) {
    Theme t;
    t.load(mode);
    CaptureWindow cw{&t};
    // Created hidden and off every monitor, then cloaked: never on the desktop.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"adw_ui capture", WS_POPUP,
                             -32000, -32000, 400, 200, nullptr, nullptr, wc.hInstance, &cw);
    CHECK(h != nullptr);
    if (!h) continue;
    park_offscreen(h);
    t.set_dpi((int)GetDpiForWindow(h));
    const int W = t.px(400), H = t.px(200);
    SetWindowPos(h, nullptr, 0, 0, W, H, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HWND card = CreateWindowExW(0, WC_BUTTONW, L"A disc image…\nAn ISO image of an After Dark CD",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, t.px(16), t.px(16), t.px(368),
                                t.px(70), h, (HMENU)101, wc.hInstance, nullptr);
    set_button_role(card, ButtonRole::card, L'\uE958');
    SendMessageW(card, WM_SETFONT, (WPARAM)t.fonts.body, TRUE);
    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, t.px(16), t.px(120), t.px(368), t.px(8),
                               h, (HMENU)102, wc.hInstance, nullptr);
    subclass_progress(bar, &t);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
    SendMessageW(bar, PBM_SETPOS, 500, 0);
    settle_for_capture(h);
    const char* name = mode == ThemeMode::light ? "light" : mode == ThemeMode::dark ? "dark" : "hc";
    fs::path png = dir / (std::string("capture-") + name + ".png");
    POINT origin{};
    Image shot;
    // Ready once the progress bar's accent shows (the card's fill is the
    // page's own colour under high contrast).
    const int bar_x = t.px(16) + t.px(368) / 4, bar_y = t.px(120) + t.px(8) / 2;
    CHECK(capture_until(h, png, shot, [&](const Image& s) {
      return s.w == W && s.h == H && image_px(s, bar_x, bar_y) != t.pal.base;
    }, &origin, "capture"));
    CHECK(shot.w == W && shot.h == H);
    if (shot.w == W && shot.h == H) {
      CHECK_COLOR(image_px(shot, 1, H - 2), t.pal.base, 0);
      // The card: its fill left of the chevron, near the bottom, clear of the text.
      const int fm = focus_margin(t.dpi);
      CHECK_COLOR(image_px(shot, t.px(16) + t.px(368) - fm - t.px(60), t.px(16) + t.px(70) - fm - t.px(4)), t.pal.card, 0);
      // The progress bar: accent up to half way, the rail after it.
      const int cy = t.px(120) + t.px(8) / 2;
      CHECK_COLOR(image_px(shot, t.px(16) + t.px(368) / 4, cy), t.pal.accent, 0);
      CHECK_COLOR(image_px(shot, t.px(16) + t.px(368) * 3 / 4, cy), t.pal.strong_stroke, 0);
      CHECK_COLOR(image_px(shot, t.px(16) + t.px(368) * 3 / 4, t.px(120)), t.pal.base, 0);
    }
    // Marquee: the style, then the sweep; still the rail and a segment somewhere.
    SetWindowLongW(bar, GWL_STYLE, GetWindowLongW(bar, GWL_STYLE) | PBS_MARQUEE);
    SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
    RedrawWindow(bar, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
    DestroyWindow(h);
  }
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  CoUninitialize();
}

// ---- ui.slider ------------------------------------------------------------------------

// What MSAA (and the UIA proxy over it) says a window is called and holds.
bool acc_name_value(HWND h, std::wstring* name, std::wstring* value) {
  IAccessible* acc = nullptr;
  if (FAILED(AccessibleObjectFromWindow(h, (DWORD)OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void**>(&acc))) || !acc)
    return false;
  VARIANT self;
  VariantInit(&self);
  self.vt = VT_I4;
  self.lVal = CHILDID_SELF;
  BSTR n = nullptr, v = nullptr;
  const bool ok = SUCCEEDED(acc->get_accName(self, &n)) && SUCCEEDED(acc->get_accValue(self, &v));
  if (n) *name = std::wstring(n, SysStringLen(n));
  if (v) *value = std::wstring(v, SysStringLen(v));
  SysFreeString(n);
  SysFreeString(v);
  acc->Release();
  return ok;
}

// init_slider (AUDIO.md §9's Volume): its range, page and name; the keys a
// Windows 11 slider answers (up is more), each reported to the parent as
// WM_HSCROLL; and its drawing in each palette, enabled and disabled: the
// accent up to the thumb, the rail after it, the thumb's accent core.
void test_slider(const fs::path& dir) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = capture_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"AdwUiTestSlider";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  for (ThemeMode mode : {ThemeMode::light, ThemeMode::dark, ThemeMode::high_contrast}) {
    Theme t;
    t.load(mode);
    CaptureWindow cw{&t};
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"adw_ui slider", WS_POPUP, -32000,
                             -32000, 400, 120, nullptr, nullptr, wc.hInstance, &cw);
    CHECK(h != nullptr);
    if (!h) continue;
    park_offscreen(h);
    t.set_dpi((int)GetDpiForWindow(h));
    const int W = t.px(400), H = t.px(120), fm = focus_margin(t.dpi);
    SetWindowPos(h, nullptr, 0, 0, W, H, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // As the dialog places it: its 32-DIP body grown by the focus margin.
    const RECT body{t.px(16), t.px(40), t.px(16) + t.px(368), t.px(40) + t.px(32)};
    HWND tb = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
                              body.left - fm, body.top - fm, body.right - body.left + 2 * fm,
                              body.bottom - body.top + 2 * fm, h, (HMENU)201, wc.hInstance, nullptr);
    CHECK(tb != nullptr);
    if (!tb) {
      DestroyWindow(h);
      continue;
    }
    init_slider(tb, &t, SliderSpec{0, 100, 10, 50, L"Volume"});
    CHECK(SendMessageW(tb, TBM_GETRANGEMIN, 0, 0) == 0 && SendMessageW(tb, TBM_GETRANGEMAX, 0, 0) == 100);
    CHECK(SendMessageW(tb, TBM_GETPAGESIZE, 0, 0) == 10 && SendMessageW(tb, TBM_GETLINESIZE, 0, 0) == 1);
    CHECK(SendMessageW(tb, TBM_GETPOS, 0, 0) == 50);
    std::wstring name, value;
    CHECK(acc_name_value(tb, &name, &value));
    CHECK(name == L"Volume");
    CHECK(value == L"50");

    // The keys: each move is one WM_HSCROLL (and the release a TB_ENDTRACK).
    struct Key { UINT vk; int want; int code; };
    const Key keys[] = {
        {VK_RIGHT, 51, TB_LINEDOWN}, {VK_UP, 52, TB_LINEDOWN},   {VK_LEFT, 51, TB_LINEUP},  {VK_DOWN, 50, TB_LINEUP},
        {VK_PRIOR, 60, TB_PAGEDOWN}, {VK_NEXT, 50, TB_PAGEUP},    {VK_END, 100, TB_BOTTOM},  {VK_PRIOR, 100, -1},
        {VK_UP, 100, -1},            {VK_HOME, 0, TB_TOP},        {VK_NEXT, 0, -1},          {VK_DOWN, 0, -1},
    };
    for (const Key& k : keys) {
      const int before = cw.hscrolls;
      SendMessageW(tb, WM_KEYDOWN, k.vk, 1);
      const int pos = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
      if (pos != k.want) {
        fprintf(stderr, "slider: key 0x%02X gave %d, want %d\n", k.vk, pos, k.want);
        ++g_failures;
      }
      if (k.code >= 0) CHECK(cw.hscrolls == before + 1 && cw.last_code == k.code);
      SendMessageW(tb, WM_KEYUP, k.vk, 0xC0000001);
      CHECK(cw.last_code == TB_ENDTRACK || k.code < 0);
    }
    // Disabled: a key sent to it moves nothing.
    SendMessageW(tb, TBM_SETPOS, TRUE, 50);
    EnableWindow(tb, FALSE);
    SendMessageW(tb, WM_KEYDOWN, VK_PRIOR, 1);
    CHECK(SendMessageW(tb, TBM_GETPOS, 0, 0) == 50);
    EnableWindow(tb, TRUE);
    CHECK(acc_name_value(tb, &name, &value) && value == L"50");

    // The drawing, enabled then disabled: along the rail's centre line.
    const char* mname = mode == ThemeMode::light ? "light" : mode == ThemeMode::dark ? "dark" : "hc";
    for (bool enabled : {true, false}) {
      EnableWindow(tb, enabled);
      settle_for_capture(h);
      fs::path png = dir / (std::string("slider-") + mname + (enabled ? "" : "-disabled") + ".png");
      // track_geom: the thumb's centre runs from 10 DIP inside the body's
      // left edge to 10 DIP inside its right; the rail is 4 DIP tall.
      const float r = t.pxf(10), x0 = body.left + r, x1 = body.right - r;
      RECT tbr{};
      GetClientRect(tb, &tbr);
      const int cy = body.top - fm + (int)((tbr.bottom - tbr.top) / 2.0f);
      auto x_at = [&](int pos) { return (int)std::lround(x0 + (x1 - x0) * pos / 100.0f); };
      POINT origin{};
      Image shot;
      CHECK(capture_until(h, png, shot, [&](const Image& s) {
        // The rail is drawn along its centre line (never the page's colour there).
        return s.w == W && s.h == H && image_px(s, x_at(25), cy) != t.pal.base && image_px(s, x_at(80), cy) != t.pal.base;
      }, &origin, "slider"));
      CHECK(shot.w == W && shot.h == H);
      if (shot.w != W || shot.h != H) continue;
      const COLORREF done = enabled ? t.pal.accent : t.pal.accent_disabled;
      const COLORREF rail = enabled ? t.pal.rail : t.pal.text_disabled;
      CHECK_COLOR(image_px(shot, x_at(25), cy), done, 0);     // the accent up to the thumb
      CHECK_COLOR(image_px(shot, x_at(80), cy), rail, 0);     // the rail after it
      CHECK_COLOR(image_px(shot, x_at(50), cy), done, 0);     // the thumb's core
      CHECK_COLOR(image_px(shot, x_at(25), cy - t.px(8)), t.pal.base, 0);   // clear above the rail
      // The thumb's ring, between core and edge.
      CHECK_COLOR(image_px(shot, x_at(50), cy - (int)std::lround(t.pxf(8))), t.pal.thumb, 16);
    }
    DestroyWindow(h);
  }
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  CoUninitialize();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_ui theme|image|capture|slider [scratch dir]\n");
    return 2;
  }
  const std::string which = argv[1];
  fs::path dir = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path() / ("adw-ui-" + which);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  gdiplus_startup();
  if (which == "theme") test_theme();
  else if (which == "image") test_image(dir);
  else if (which == "capture") test_capture(dir);
  else if (which == "slider") test_slider(dir);
  else {
    fprintf(stderr, "unknown test %s\n", which.c_str());
    return 2;
  }
  gdiplus_shutdown();
  if (g_failures) {
    fprintf(stderr, "ui.%s: %d failure(s)\n", which.c_str(), g_failures);
    return 1;
  }
  fprintf(stderr, "ui.%s: ok\n", which.c_str());
  return 0;
}
