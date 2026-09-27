#include "adw/ui/theme.h"

#include <dwmapi.h>
#include <objidl.h>
#include <uxtheme.h>

#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace adw::ui {

namespace {

constexpr COLORREF rgb(unsigned v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

Gdiplus::Color gp(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void round_path(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
  r = std::max(0.0f, std::min(r, std::min(w, h) / 2));
  if (r <= 0.01f) {
    path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    return;
  }
  float d = r * 2;
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + w - d, y, d, d, 270, 90);
  path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  path.AddArc(x, y + h - d, d, d, 90, 90);
  path.CloseFigure();
}

struct Canvas {
  Gdiplus::Graphics g;
  explicit Canvas(HDC dc) : g(dc) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  }
};

bool font_installed(const wchar_t* face) {
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

HFONT make_font(const wchar_t* face, int px, int weight, int dpi, bool grayscale = false) {
  return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                     CLIP_DEFAULT_PRECIS, grayscale ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                     face);
}

ULONG_PTR g_gdiplus = 0;

} // namespace

// ---- colours ---------------------------------------------------------------------------

Accent read_accent() {
  // The default Windows 11 blue.
  Accent a{rgb(0x99EBFF), rgb(0x4CC2FF), rgb(0x0091F8), rgb(0x0078D4), rgb(0x005FB8), rgb(0x004C87), rgb(0x003A68)};
  BYTE pal[32] = {};
  DWORD size = sizeof(pal);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                   L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 28) {
    auto at = [&](int i) { return RGB(pal[i * 4], pal[i * 4 + 1], pal[i * 4 + 2]); };
    a = {at(0), at(1), at(2), at(3), at(4), at(5), at(6)};
  }
  return a;
}

bool system_prefers_dark() {
  DWORD v = 1, size = sizeof(v);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) {
    return false;
  }
  return v == 0;
}

bool high_contrast_on() {
  HIGHCONTRASTW hc{sizeof(hc)};
  return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

COLORREF blend(COLORREF a, COLORREF b, double t) {
  auto mix = [&](int x, int y) { return (int)std::lround(x + (y - x) * t); };
  return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

Palette make_palette(bool dark, const Accent& ac) {
  // WinUI 3 theme resources, flattened onto the surfaces they sit on here
  // (base = the window, card = the panels).
  Palette p{};
  p.dark = dark;
  if (!dark) {
    p.base = rgb(0xF3F3F3);
    p.card = rgb(0xFBFBFB);
    p.card_stroke = rgb(0xE5E5E5);
    p.divider = rgb(0xE0E0E0);
    p.text = rgb(0x1B1B1B);
    p.text2 = rgb(0x5D5D5D);
    p.text3 = rgb(0x8A8A8A);
    p.text_disabled = rgb(0xA3A3A3);
    p.control = rgb(0xFEFEFE);
    p.control_hover = rgb(0xF6F6F6);
    p.control_pressed = rgb(0xF2F2F2);
    p.control_disabled = rgb(0xF6F6F6);
    p.control_stroke = rgb(0xE3E3E3);
    p.control_stroke_bottom = rgb(0xCDCDCD);
    p.strong_stroke = rgb(0x8A8A8A);
    p.rail = rgb(0x9A9A9A);
    p.row_hover = rgb(0xF0F0F0);
    p.row_selected = rgb(0xEAEAEA);
    p.row_selected_hover = rgb(0xE4E4E4);
    p.accent = ac.dark1;
    p.accent_hover = blend(ac.dark1, p.card, 0.10);
    p.accent_pressed = blend(ac.dark1, p.card, 0.20);
    p.accent_disabled = rgb(0xBFBFBF);
    p.on_accent = rgb(0xFFFFFF);
    p.accent_text = ac.dark2;
    p.thumb = rgb(0xFFFFFF);
    p.thumb_stroke = rgb(0xD5D5D5);
    p.focus_outer = rgb(0x1B1B1B);
    p.focus_inner = rgb(0xFFFFFF);
    p.chip = rgb(0xEDEDED);
    p.chip_text = rgb(0x5D5D5D);
    p.caution = rgb(0xFFF4CE);
    p.caution_text = rgb(0x8A5300);
    p.critical = rgb(0xFDE7E9);
    p.critical_text = rgb(0xC42B1C);
  } else {
    p.base = rgb(0x202020);
    p.card = rgb(0x2B2B2B);
    p.card_stroke = rgb(0x1D1D1D);
    p.divider = rgb(0x333333);
    p.text = rgb(0xFFFFFF);
    p.text2 = rgb(0xCFCFCF);
    p.text3 = rgb(0x9D9D9D);
    p.text_disabled = rgb(0x787878);
    p.control = rgb(0x373737);
    p.control_hover = rgb(0x3D3D3D);
    p.control_pressed = rgb(0x323232);
    p.control_disabled = rgb(0x323232);
    p.control_stroke = rgb(0x434343);
    p.control_stroke_bottom = rgb(0x3A3A3A);
    p.strong_stroke = rgb(0x9E9E9E);
    p.rail = rgb(0x8C8C8C);
    p.row_hover = rgb(0x323232);
    p.row_selected = rgb(0x383838);
    p.row_selected_hover = rgb(0x3E3E3E);
    p.accent = ac.light2;
    p.accent_hover = blend(ac.light2, p.card, 0.10);
    p.accent_pressed = blend(ac.light2, p.card, 0.20);
    p.accent_disabled = rgb(0x5A5A5A);
    p.on_accent = rgb(0x000000);
    p.accent_text = ac.light3;
    p.thumb = rgb(0x454545);
    p.thumb_stroke = rgb(0x505050);
    p.focus_outer = rgb(0xFFFFFF);
    p.focus_inner = rgb(0x000000);
    p.chip = rgb(0x3A3A3A);
    p.chip_text = rgb(0xCFCFCF);
    p.caution = rgb(0x433519);
    p.caution_text = rgb(0xFCE100);
    p.critical = rgb(0x442726);
    p.critical_text = rgb(0xFF99A4);
  }
  return p;
}

namespace {

// set_test_hc_scheme (tests and screenshots only): the system colours of one of Windows 11's
// contrast themes stand in for GetSysColor's, so the forced high-contrast palette shows what
// that theme looks like without switching the machine to it. The values approximate the
// themes' defaults (Settings > Accessibility > Contrast themes). Nothing here reads the
// environment: a program's test hook (LongAfterDark-test.scr's AD_UI_TEST_HC_SCHEME) calls it.
struct HcScheme {
  const wchar_t* name;
  // Background, Text, Selected text background, Selected text, Inactive text, Button
  // background, Button text, Hyperlink.
  unsigned window, window_text, highlight, highlight_text, gray_text, btn_face, btn_text, hot_light;
};
constexpr HcScheme kHcSchemes[] = {
    {L"nightsky", 0x000000, 0xFFFFFF, 0xD6B4FD, 0x2B2B2B, 0xA6A6A6, 0x000000, 0xFFEE32, 0x8080FF},
    {L"aquatic", 0x202020, 0xFFFFFF, 0x8EE3F0, 0x263B50, 0xA6A6A6, 0x202020, 0xFFFFFF, 0x75E9FC},
    {L"desert", 0xFFFAEF, 0x3D3D3D, 0x903909, 0xFFF5E3, 0x676767, 0xFFFAEF, 0x3D3D3D, 0x1C5E75},
    {L"dusk", 0x2D3236, 0xFFFFFF, 0xA6D8FF, 0x212D3B, 0xA6A6A6, 0x2D3236, 0xB6F6F0, 0x70EBDE},
};

std::atomic<const HcScheme*> g_test_hc{nullptr};

const HcScheme* test_hc_scheme() { return g_test_hc.load(std::memory_order_relaxed); }

COLORREF sys_color(int index) {
  const HcScheme* s = test_hc_scheme();
  if (!s) return GetSysColor(index);
  unsigned v = 0;
  switch (index) {
    case COLOR_WINDOW: v = s->window; break;
    case COLOR_WINDOWTEXT: v = s->window_text; break;
    case COLOR_HIGHLIGHT: v = s->highlight; break;
    case COLOR_HIGHLIGHTTEXT: v = s->highlight_text; break;
    case COLOR_GRAYTEXT: v = s->gray_text; break;
    case COLOR_BTNFACE: v = s->btn_face; break;
    case COLOR_BTNTEXT: v = s->btn_text; break;
    case COLOR_HOTLIGHT: v = s->hot_light; break;
    default: return GetSysColor(index);
  }
  return rgb(v);
}

} // namespace

bool set_test_hc_scheme(const wchar_t* name) {
  if (!name || !*name) {
    g_test_hc.store(nullptr, std::memory_order_relaxed);
    return true;
  }
  for (const HcScheme& s : kHcSchemes) {
    if (_wcsicmp(s.name, name) == 0) {
      g_test_hc.store(&s, std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

Palette high_contrast_palette() {
  // Everything from the user's high-contrast scheme; no tints of our own.
  Palette p{};
  p.high_contrast = true;
  COLORREF win = sys_color(COLOR_WINDOW), text = sys_color(COLOR_WINDOWTEXT);
  COLORREF hi = sys_color(COLOR_HIGHLIGHT), hitext = sys_color(COLOR_HIGHLIGHTTEXT);
  COLORREF gray = sys_color(COLOR_GRAYTEXT), btn = sys_color(COLOR_BTNFACE), btntext = sys_color(COLOR_BTNTEXT);
  p.dark = GetRValue(win) + GetGValue(win) + GetBValue(win) < 384;
  p.base = p.card = win;
  p.card_stroke = p.divider = text;
  p.text = p.text2 = p.text3 = text;
  p.text_disabled = gray;
  p.control = p.control_hover = p.control_pressed = btn;
  p.control_disabled = btn;
  p.control_stroke = p.control_stroke_bottom = btntext;
  p.strong_stroke = p.rail = text;
  p.row_hover = win;
  p.row_selected = p.row_selected_hover = hi;
  p.accent = p.accent_hover = p.accent_pressed = hi;
  p.accent_disabled = gray;
  p.on_accent = hitext;
  p.accent_text = sys_color(COLOR_HOTLIGHT);
  p.thumb = btn;
  p.thumb_stroke = btntext;
  p.focus_outer = text;
  p.focus_inner = win;
  p.chip = win;
  p.chip_text = text;
  p.caution = p.critical = win;
  p.caution_text = p.critical_text = text;
  return p;
}

// ---- fonts -----------------------------------------------------------------------------

void Fonts::create(int new_dpi, bool gray) {
  destroy();
  dpi = new_dpi;
  static const bool variable = font_installed(L"Segoe UI Variable Text");
  static const bool fluent = font_installed(L"Segoe Fluent Icons");
  const wchar_t* text = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
  const wchar_t* small = variable ? L"Segoe UI Variable Small" : L"Segoe UI";
  const wchar_t* display = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
  const wchar_t* icon_face = fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
  caption = make_font(small, 12, FW_NORMAL, dpi, gray);
  body = make_font(text, 14, FW_NORMAL, dpi, gray);
  body_strong = make_font(text, 14, FW_SEMIBOLD, dpi, gray);
  subtitle = make_font(display, 20, FW_SEMIBOLD, dpi, gray);
  subtitle_small = make_font(display, 16, FW_SEMIBOLD, dpi, gray);
  body_gray = make_font(text, 14, FW_NORMAL, dpi, true);
  title = make_font(display, 28, FW_SEMIBOLD, dpi, gray);
  icons = make_font(icon_face, 16, FW_NORMAL, dpi);
  icons_small = make_font(icon_face, 12, FW_NORMAL, dpi);
}

void Fonts::destroy() {
  for (HFONT* f : {&caption, &body, &body_strong, &subtitle, &subtitle_small, &body_gray, &title, &icons, &icons_small}) {
    if (*f) DeleteObject(*f);
    *f = nullptr;
  }
}

void Theme::load(ThemeMode forced) {
  bool hc = forced == ThemeMode::high_contrast || (forced == ThemeMode::system && high_contrast_on());
  bool dark = forced == ThemeMode::dark || (forced == ThemeMode::system && system_prefers_dark());
  pal = hc ? high_contrast_palette() : make_palette(dark, read_accent());
  if (base_brush) DeleteObject(base_brush);
  if (card_brush) DeleteObject(card_brush);
  base_brush = CreateSolidBrush(pal.base);
  card_brush = CreateSolidBrush(pal.card);
  icons_font_is_fluent = font_installed(L"Segoe Fluent Icons");
}

void Theme::set_dpi(int d) {
  dpi = d;
  fonts.create(d);
}

int Theme::body_line_height() const {
  HDC dc = GetDC(nullptr);
  HGDIOBJ old = SelectObject(dc, fonts.body);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  return tm.tmHeight + tm.tmExternalLeading;
}

void Theme::destroy() {
  if (base_brush) DeleteObject(base_brush);
  if (card_brush) DeleteObject(card_brush);
  base_brush = card_brush = nullptr;
  fonts.destroy();
}

// ---- chrome ----------------------------------------------------------------------------

void apply_window_chrome(HWND hwnd, const Palette& pal) {
  BOOL dark = pal.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  // One surface from the title bar down: the caption takes the window's
  // colour (Windows 11; ignored before). High contrast keeps the system's.
  COLORREF caption = pal.high_contrast ? (COLORREF)0xFFFFFFFF /* DWMWA_COLOR_DEFAULT */ : pal.base;
  DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
  // The header under the title bar names the window (with the moon); the
  // title bar keeps just its buttons. The caption text stays for the
  // taskbar, Alt+Tab and screen readers.
  WTA_OPTIONS nc{WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON, WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON};
  SetWindowThemeAttribute(hwnd, WTA_NONCLIENT, &nc, sizeof(nc));
}

void theme_native_control(HWND hwnd, bool dark) {
  SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ---- drawing ---------------------------------------------------------------------------

void gdiplus_startup() {
  if (g_gdiplus) return;
  Gdiplus::GdiplusStartupInput in;
  Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr);
}

void gdiplus_shutdown() {
  if (!g_gdiplus) return;
  Gdiplus::GdiplusShutdown(g_gdiplus);
  g_gdiplus = 0;
}

void fill_rect(HDC dc, const RECT& r, COLORREF c) {
  COLORREF old = SetBkColor(dc, c);
  ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
  SetBkColor(dc, old);
}

void fill_round(HDC dc, const RECT& r, float radius, COLORREF c) {
  Canvas cv(dc);
  Gdiplus::GraphicsPath path;
  round_path(path, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), radius);
  Gdiplus::SolidBrush b(gp(c));
  cv.g.FillPath(&b, &path);
}

void stroke_round(HDC dc, const RECT& r, float radius, COLORREF c, float width, int alpha) {
  Canvas cv(dc);
  Gdiplus::GraphicsPath path;
  float h = width / 2;
  round_path(path, r.left + h, r.top + h, r.right - r.left - width, r.bottom - r.top - width, radius - h);
  Gdiplus::Pen pen(gp(c, (BYTE)std::clamp(alpha, 0, 255)), width);
  cv.g.DrawPath(&pen, &path);
}

void draw_control_body(HDC dc, const RECT& r, float radius, COLORREF fill, COLORREF stroke, COLORREF bottom) {
  Canvas cv(dc);
  const float x = (float)r.left, y = (float)r.top, w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
  {
    Gdiplus::GraphicsPath path;
    round_path(path, x, y, w, h, radius);
    Gdiplus::SolidBrush b(gp(stroke));
    cv.g.FillPath(&b, &path);
  }
  if (bottom != stroke) {
    // The lower edge only: clip to the bottom few pixels of the outline.
    Gdiplus::GraphicsPath path;
    round_path(path, x, y, w, h, radius);
    Gdiplus::Region clip(Gdiplus::RectF(x, y + h - std::max(2.0f, radius), w, std::max(2.0f, radius)));
    cv.g.SetClip(&clip);
    Gdiplus::SolidBrush b(gp(bottom));
    cv.g.FillPath(&b, &path);
    cv.g.ResetClip();
  }
  Gdiplus::GraphicsPath inner;
  round_path(inner, x + 1, y + 1, w - 2, h - 2, std::max(0.0f, radius - 1));
  Gdiplus::SolidBrush b(gp(fill));
  cv.g.FillPath(&b, &inner);
}

void fill_ellipse(HDC dc, float cx, float cy, float radius, COLORREF c) {
  Canvas cv(dc);
  Gdiplus::SolidBrush b(gp(c));
  cv.g.FillEllipse(&b, cx - radius, cy - radius, radius * 2, radius * 2);
}

void stroke_ellipse(HDC dc, float cx, float cy, float radius, COLORREF c, float width) {
  Canvas cv(dc);
  Gdiplus::Pen pen(gp(c), width);
  float r = radius - width / 2;
  cv.g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
}

void draw_check(HDC dc, const RECT& box, COLORREF c, float width) {
  Canvas cv(dc);
  Gdiplus::Pen pen(gp(c), width);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  float x = (float)box.left, y = (float)box.top, w = (float)(box.right - box.left), h = (float)(box.bottom - box.top);
  Gdiplus::PointF pts[3] = {{x + w * 0.24f, y + h * 0.52f}, {x + w * 0.42f, y + h * 0.70f}, {x + w * 0.76f, y + h * 0.32f}};
  cv.g.DrawLines(&pen, pts, 3);
}

void draw_focus_ring(HDC dc, const RECT& r, float radius, const Palette& pal, float scale) {
  const float outer = std::max(1.0f, std::round(2 * scale)), inner = std::max(1.0f, std::round(scale));
  stroke_round(dc, r, radius, pal.focus_outer, outer);
  RECT in{r.left + (int)outer, r.top + (int)outer, r.right - (int)outer, r.bottom - (int)outer};
  stroke_round(dc, in, std::max(0.0f, radius - outer), pal.focus_inner, inner);
}

void mask_round_corners(HDC dc, const RECT& r, float radius, COLORREF c) {
  Canvas cv(dc);
  Gdiplus::GraphicsPath path;
  round_path(path, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), radius);
  Gdiplus::Region outside(Gdiplus::RectF((float)r.left - 1, (float)r.top - 1, (float)(r.right - r.left) + 2,
                                         (float)(r.bottom - r.top) + 2));
  outside.Exclude(&path);
  Gdiplus::SolidBrush b(gp(c));
  cv.g.FillRegion(&b, &outside);
}

void fade_rect(HDC dc, const RECT& r, COLORREF c, int alpha_top, int alpha_bottom) {
  if (r.bottom <= r.top || r.right <= r.left) return;
  Gdiplus::Graphics g(dc);
  // One pixel beyond each end, so the gradient's own edge pixels aren't
  // wrapped round (GDI+ tiles a linear gradient past its end points).
  Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top - 1), Gdiplus::PointF(0, (float)r.bottom + 1),
                                 gp(c, (BYTE)std::clamp(alpha_top, 0, 255)), gp(c, (BYTE)std::clamp(alpha_bottom, 0, 255)));
  g.FillRectangle(&b, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
}

void fade_out(HDC dc, const RECT& r, COLORREF c, int max_alpha) {
  if (r.bottom <= r.top || r.right <= r.left) return;
  Gdiplus::Graphics g(dc);
  Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top - 1), Gdiplus::PointF(0, (float)r.bottom + 1), gp(c, 0),
                                 gp(c, (BYTE)std::clamp(max_alpha, 0, 255)));
  // 1 - (1 - t)^2
  const Gdiplus::REAL factors[] = {0.0f, 0.4375f, 0.75f, 0.9375f, 1.0f};
  const Gdiplus::REAL positions[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  b.SetBlend(factors, positions, 5);
  g.FillRectangle(&b, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
}

void fade_in_right(HDC dc, const RECT& r, COLORREF c) {
  if (r.bottom <= r.top || r.right <= r.left) return;
  Gdiplus::Graphics g(dc);
  Gdiplus::LinearGradientBrush b(Gdiplus::PointF((float)r.left - 1, 0), Gdiplus::PointF((float)r.right + 1, 0), gp(c, 0),
                                 gp(c, 255));
  g.FillRectangle(&b, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
}

void fill_gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom) {
  if (r.bottom <= r.top || r.right <= r.left) return;
  Gdiplus::Graphics g(dc);
  Gdiplus::LinearGradientBrush b(Gdiplus::PointF(0, (float)r.top - 1), Gdiplus::PointF(0, (float)r.bottom + 1), gp(top),
                                 gp(bottom));
  g.FillRectangle(&b, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
}

void glow_ellipse(HDC dc, const RECT& r, COLORREF c, int alpha) {
  if (r.bottom <= r.top || r.right <= r.left) return;
  Canvas cv(dc);
  Gdiplus::GraphicsPath path;
  path.AddEllipse((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
  Gdiplus::PathGradientBrush b(&path);
  b.SetCenterColor(gp(c, (BYTE)std::clamp(alpha, 0, 255)));
  Gdiplus::Color edge = gp(c, 0);
  int n = 1;
  b.SetSurroundColors(&edge, &n);
  cv.g.FillPath(&b, &path);
}

// ---- the night sky -----------------------------------------------------------------------

void draw_sparkle(HDC dc, float cx, float cy, float r, COLORREF c, int alpha) {
  Canvas cv(dc);
  const float k = r * 0.24f;
  Gdiplus::PointF pts[8] = {{cx, cy - r}, {cx + k, cy - k}, {cx + r, cy}, {cx + k, cy + k},
                            {cx, cy + r}, {cx - k, cy + k}, {cx - r, cy}, {cx - k, cy - k}};
  Gdiplus::SolidBrush b(gp(c, (BYTE)std::clamp(alpha, 0, 255)));
  cv.g.FillPolygon(&b, pts, 8);
}

void draw_crescent(HDC dc, float cx, float cy, float r, int glow) {
  Canvas cv(dc);
  if (glow > 0) {
    Gdiplus::GraphicsPath halo;
    const float hr = r * 2.1f;
    halo.AddEllipse(cx - hr, cy - hr, 2 * hr, 2 * hr);
    Gdiplus::PathGradientBrush b(&halo);
    b.SetCenterColor(Gdiplus::Color((BYTE)std::clamp(glow, 0, 255), 0xB4, 0xBE, 0xFF));
    Gdiplus::Color edge(0, 0xB4, 0xBE, 0xFF);
    int n = 1;
    b.SetSurroundColors(&edge, &n);
    cv.g.FillPath(&b, &halo);
  }
  Gdiplus::GraphicsPath disc, bite;
  disc.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
  const float br = r * 0.86f, bx = cx + r * 0.48f, by = cy - r * 0.36f;
  bite.AddEllipse(bx - br, by - br, 2 * br, 2 * br);
  Gdiplus::Region moon(&disc);
  moon.Exclude(&bite);
  Gdiplus::LinearGradientBrush gold(Gdiplus::PointF(cx - r, cy - r), Gdiplus::PointF(cx + r, cy + r),
                                    Gdiplus::Color(255, 0xFB, 0xF1, 0xC8), Gdiplus::Color(255, 0xE9, 0xCF, 0x86));
  cv.g.FillRegion(&gold, &moon);
}

void draw_flying_toaster(HDC dc, float x, float y, float w, float flap, int alpha) {
  Canvas cv(dc);
  Gdiplus::Graphics& g = cv.g;
  const BYTE a = (BYTE)std::clamp(alpha, 0, 255);
  auto P = [&](float u, float v) { return Gdiplus::PointF(x + u * w, y + v * w); };
  flap = std::clamp(flap, 0.0f, 1.0f);
  // Wings: a fan of feathers from the top of the toaster, swept up and back
  // (it flies to the left). The far one first, a little darker.
  auto wing = [&](float root_u, float lift, Gdiplus::Color fill, Gdiplus::Color edge) {
    static const float kLen[] = {0.66f, 0.60f, 0.52f, 0.42f}, kWid[] = {0.17f, 0.16f, 0.15f, 0.14f};
    const Gdiplus::PointF root = P(root_u, 0.25f);
    Gdiplus::SolidBrush b(fill);
    Gdiplus::Pen pen(edge, std::max(1.0f, w * 0.012f));
    for (int k = 3; k >= 0; --k) {
      // Raised: the leading feather nearly upright; lowered: swept back.
      const float angle = -(28.0f + 62.0f * lift) + k * (14.0f + 4.0f * lift);
      Gdiplus::GraphicsState saved = g.Save();
      g.TranslateTransform(root.X, root.Y);
      g.RotateTransform(angle);
      const float L = kLen[k] * w, Wd = kWid[k] * w;
      Gdiplus::GraphicsPath f;
      f.AddEllipse(-Wd * 0.3f, -Wd / 2, L, Wd);
      g.FillPath(&b, &f);
      g.DrawPath(&pen, &f);
      g.Restore(saved);
    }
  };
  wing(0.40f, 0.55f + 0.45f * flap, Gdiplus::Color((BYTE)(a * 0.85f), 0xC9, 0xCE, 0xE0), Gdiplus::Color((BYTE)(a * 0.5f), 0x6A, 0x72, 0x90));
  // The slice of toast, peeking out of the slot (the body covers its foot).
  {
    Gdiplus::GraphicsPath slice;
    const Gdiplus::PointF o = P(0.28f, 0.07f);
    const float W = 0.34f * w, H = 0.20f * w, d = 0.12f * w;
    slice.AddArc(o.X, o.Y, d, d, 180, 90);
    slice.AddArc(o.X + W - d, o.Y, d, d, 270, 90);
    slice.AddLine(o.X + W, o.Y + d / 2, o.X + W, o.Y + H);
    slice.AddLine(o.X + W, o.Y + H, o.X, o.Y + H);
    slice.CloseFigure();
    Gdiplus::SolidBrush crust(Gdiplus::Color(a, 0xC6, 0x86, 0x3C));
    g.FillPath(&crust, &slice);
    Gdiplus::SolidBrush crumb(Gdiplus::Color(a, 0xEE, 0xC9, 0x7E));
    g.FillRectangle(&crumb, o.X + W * 0.14f, o.Y + H * 0.22f, W * 0.72f, H * 0.6f);
  }
  // The body: chrome, lit from above, with a darker base and a lever.
  {
    Gdiplus::GraphicsPath body;
    const float bx = 0.04f, by = 0.20f, bw = 0.92f, bh = 0.52f, r = 0.13f;
    const Gdiplus::PointF o = P(bx, by);
    const float W = bw * w, H = bh * w, d = 2 * r * w;
    body.AddArc(o.X, o.Y, d, d, 180, 90);
    body.AddArc(o.X + W - d, o.Y, d, d, 270, 90);
    body.AddArc(o.X + W - d, o.Y + H - d, d, d, 0, 90);
    body.AddArc(o.X, o.Y + H - d, d, d, 90, 90);
    body.CloseFigure();
    Gdiplus::LinearGradientBrush chrome(Gdiplus::PointF(0, o.Y), Gdiplus::PointF(0, o.Y + H), Gdiplus::Color(a, 0xF4, 0xF5, 0xFA),
                                        Gdiplus::Color(a, 0x8C, 0x93, 0xA8));
    g.FillPath(&chrome, &body);
    // A reflection band and the slot's shadow on top.
    Gdiplus::SolidBrush shine(Gdiplus::Color((BYTE)(a * 0.55f), 0xFF, 0xFF, 0xFF));
    g.FillRectangle(&shine, P(0.12f, 0.33f).X, P(0.12f, 0.33f).Y, 0.64f * w, 0.035f * w);
    Gdiplus::SolidBrush slot(Gdiplus::Color((BYTE)(a * 0.7f), 0x3A, 0x3F, 0x55));
    g.FillRectangle(&slot, P(0.26f, 0.205f).X, P(0.26f, 0.205f).Y, 0.38f * w, 0.03f * w);
    Gdiplus::SolidBrush base(Gdiplus::Color(a, 0x4A, 0x50, 0x66));
    g.FillRectangle(&base, P(0.12f, 0.70f).X, P(0.12f, 0.70f).Y, 0.76f * w, 0.05f * w);
    g.FillRectangle(&base, P(0.94f, 0.36f).X, P(0.94f, 0.36f).Y, 0.07f * w, 0.05f * w);
    Gdiplus::Pen edge(Gdiplus::Color((BYTE)(a * 0.6f), 0x52, 0x58, 0x70), std::max(1.0f, w * 0.012f));
    g.DrawPath(&edge, &body);
  }
  // The near wing over the body.
  wing(0.50f, 0.45f + 0.55f * flap, Gdiplus::Color(a, 0xF6, 0xF7, 0xFF), Gdiplus::Color((BYTE)(a * 0.55f), 0x70, 0x78, 0x98));
}

void draw_text(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF c, UINT format) {
  HGDIOBJ old = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  COLORREF oldc = SetTextColor(dc, c);
  DrawTextW(dc, text.c_str(), (int)text.size(), &r, format);
  SetTextColor(dc, oldc);
  SelectObject(dc, old);
}

SIZE measure_text(HDC dc, const std::wstring& text, HFONT font, UINT format) {
  HGDIOBJ old = SelectObject(dc, font);
  RECT r{0, 0, 10000, 0};
  DrawTextW(dc, text.c_str(), (int)text.size(), &r, format | DT_CALCRECT);
  SelectObject(dc, old);
  return SIZE{r.right - r.left, r.bottom - r.top};
}

// ---- COVERS.md §3.2 additions ------------------------------------------------------------

bool is_theme_change(UINT msg, WPARAM, LPARAM lp) {
  switch (msg) {
    case WM_SETTINGCHANGE:
      return lp && CompareStringOrdinal(reinterpret_cast<const wchar_t*>(lp), -1, L"ImmersiveColorSet", -1, TRUE) ==
                       CSTR_EQUAL;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      return true;
  }
  return false;
}

void allow_dark_menus(bool dark) {
  // Undocumented but stable since Windows 10 1903: uxtheme exports these by
  // ordinal only. 135 was AllowDarkModeForApp(BOOL) on 1809, whose 1/0 mean
  // the same as AllowDark/Default here, so the call is right on both.
  using SetPreferredAppModeFn = int(WINAPI*)(int);
  using FlushMenuThemesFn = void(WINAPI*)();
  static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!ux) return;
  static auto set_mode = reinterpret_cast<SetPreferredAppModeFn>(
      reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
  static auto flush = reinterpret_cast<FlushMenuThemesFn>(
      reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
  if (!set_mode) return;
  set_mode(dark ? 1 /* AllowDark */ : 0 /* Default */);
  if (flush) flush();
}

void paint_card(HDC dc, const RECT& r, const Theme& t) {
  fill_round(dc, r, t.pxf(8), t.pal.card);
  stroke_round(dc, r, t.pxf(8), t.pal.card_stroke, (float)t.hairline());
}

void draw_app_mark(HDC dc, const RECT& box) {
  const float x = (float)box.left, y = (float)box.top;
  const float s = (float)std::min(box.right - box.left, box.bottom - box.top);
  if (s <= 0) return;
  // The icon's recipe (scr/res/gen_icon.cc) in unit coordinates: a rounded
  // night tile, a crescent (disc less an offset disc) and pinched stars.
  {
    Canvas cv(dc);
    Gdiplus::GraphicsPath tile;
    const float inset = 0.04f * s;
    round_path(tile, x + inset, y + inset, s - 2 * inset, s - 2 * inset, 0.20f * s);
    Gdiplus::LinearGradientBrush night(Gdiplus::PointF(0, y - 1), Gdiplus::PointF(0, y + s + 1),
                                       Gdiplus::Color(255, 0x08, 0x0D, 0x29), Gdiplus::Color(255, 0x14, 0x26, 0x6B));
    cv.g.FillPath(&night, &tile);
  }
  // The moon's gold, as the dialogs draw it everywhere else.
  draw_crescent(dc, x + 0.54f * s, y + 0.46f * s, 0.29f * s, 0);
  struct Star {
    float x, y, r;
  };
  static const Star kStars[] = {{0.24f, 0.26f, 0.085f}, {0.20f, 0.62f, 0.055f}, {0.76f, 0.76f, 0.07f}};
  for (const Star& st : kStars) draw_sparkle(dc, x + st.x * s, y + st.y * s, st.r * s * 1.3f, RGB(0xFF, 0xFF, 0xF2), 255);
}

namespace {

// The header's touch of night: behind the moon a soft indigo glow and,
// across the band, a scatter of stars (dark mode) or a few faint indigo
// sparkles (light mode). Irregular on purpose, and the same every time.
void paint_night_band(HDC dc, const Theme& t, const RECT& band, const RECT& logo, int text_right) {
  const Palette& p = t.pal;
  if (p.high_contrast) return;
  const int cy = (logo.top + logo.bottom) / 2;
  // Inside the header band only: the controls under it paint flat backgrounds.
  const int ry = std::min(cy - (int)band.top, (int)band.bottom - cy);
  RECT glow{logo.left - t.px(120), cy - ry, text_right + t.px(160), cy + ry};
  glow_ellipse(dc, glow, RGB(0x4B, 0x5B, 0xD6), p.dark ? 45 : 22);
  // x across the band past the text (0..1), y in it (0..1), size (DIPs), sparkle?
  struct Star {
    float x, y, r;
    bool sparkle;
  };
  static const Star kStars[] = {{0.03f, 0.34f, 1.1f, false}, {0.09f, 0.71f, 0.8f, false}, {0.16f, 0.22f, 1.6f, true},
                                {0.22f, 0.58f, 0.9f, false}, {0.31f, 0.81f, 1.3f, false}, {0.37f, 0.30f, 0.8f, false},
                                {0.46f, 0.52f, 1.8f, false}, {0.52f, 0.17f, 1.0f, false}, {0.61f, 0.74f, 0.9f, false},
                                {0.66f, 0.40f, 1.2f, false}, {0.74f, 0.26f, 2.6f, true},  {0.81f, 0.66f, 1.0f, false},
                                {0.89f, 0.44f, 1.4f, false}, {0.96f, 0.20f, 0.8f, false}};
  const int x0 = text_right + t.px(40), x1 = band.right - t.px(24);
  if (x1 - x0 < t.px(120)) return;
  const int y0 = band.top + t.px(6), y1 = band.bottom - t.px(6);
  for (const Star& sd : kStars) {
    const float x = x0 + (x1 - x0) * sd.x, y = y0 + (y1 - y0) * sd.y;
    if (!p.dark) {
      // Light: only the sparkles, faint.
      if (sd.sparkle) draw_sparkle(dc, x, y, t.pxf(sd.r * 1.6f), RGB(0x4B, 0x5B, 0xD6), 70);
      continue;
    }
    if (sd.sparkle) {
      draw_sparkle(dc, x, y, t.pxf(sd.r * 1.6f), RGB(0xE8, 0xE6, 0xF4), 200);
    } else {
      const double k = 0.30 + 0.45 * (sd.r - 0.8) / 1.0;
      fill_ellipse(dc, x, y, t.pxf(sd.r * 0.55f), blend(p.base, RGB(0xE8, 0xE6, 0xF4), std::clamp(k, 0.25, 0.8)));
    }
  }
}

}  // namespace

void paint_header(HDC dc, const RECT& band, const RECT& logo, const RECT& title, HICON logo_icon,
                  const std::wstring& name, const std::wstring& tagline, const Theme& t) {
  const Palette& p = t.pal;
  fill_rect(dc, band, p.base);
  // The moon-and-stars mark, the name, and what this window is, on one
  // compact line: the name and the tagline share the baseline that centres
  // the name on the logo.
  HGDIOBJ o = SelectObject(dc, t.fonts.subtitle);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, t.fonts.body);
  TEXTMETRICW bm{};
  GetTextMetricsW(dc, &bm);
  SelectObject(dc, o);
  SIZE ns = measure_text(dc, name, t.fonts.subtitle), ws = measure_text(dc, tagline, t.fonts.body);
  const int baseline = (logo.top + logo.bottom) / 2 + (tm.tmAscent - tm.tmDescent) / 2;
  RECT nr{title.left, baseline - tm.tmAscent, std::min<LONG>(title.right, title.left + ns.cx + 2), baseline + tm.tmDescent};
  RECT sr{nr.right + t.px(10), baseline - bm.tmAscent, title.right, baseline + bm.tmDescent};
  const int text_right = tagline.empty() ? (int)nr.right : std::min<int>(sr.left + ws.cx, title.right);
  paint_night_band(dc, t, band, logo, text_right);
  if (logo_icon) {
    DrawIconEx(dc, logo.left, logo.top, logo_icon, logo.right - logo.left, logo.bottom - logo.top, 0, nullptr, DI_NORMAL);
  } else {
    draw_app_mark(dc, logo);
  }
  draw_text(dc, name, nr, t.fonts.subtitle, p.dark || p.high_contrast ? p.text : RGB(0x1F, 0x25, 0x5A),
            DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  if (!tagline.empty() && sr.right > sr.left)
    draw_text(dc, tagline, sr, t.fonts.body, p.text2, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

// ---- additions at integration ------------------------------------------------------

void fade_in_left(HDC dc, const RECT& r, COLORREF c) {
  if (r.bottom <= r.top || r.right <= r.left) return;
  Gdiplus::Graphics g(dc);
  Gdiplus::LinearGradientBrush b(Gdiplus::PointF((float)r.left - 1, 0), Gdiplus::PointF((float)r.right + 1, 0), gp(c, 255),
                                 gp(c, 0));
  g.FillRectangle(&b, (INT)r.left, (INT)r.top, (INT)(r.right - r.left), (INT)(r.bottom - r.top));
}

void fill_round(HDC dc, const RECT& r, float radius, COLORREF c, int alpha) {
  Canvas cv(dc);
  Gdiplus::GraphicsPath path;
  round_path(path, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), radius);
  Gdiplus::SolidBrush b(gp(c, (BYTE)std::clamp(alpha, 0, 255)));
  cv.g.FillPath(&b, &path);
}

void stroke_ellipse(HDC dc, float cx, float cy, float radius, COLORREF c, float width, int alpha) {
  Canvas cv(dc);
  Gdiplus::Pen pen(gp(c, (BYTE)std::clamp(alpha, 0, 255)), width);
  float r = radius - width / 2;
  cv.g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
}

} // namespace adw::ui
