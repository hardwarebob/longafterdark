// adw_ui (common/ui, COVERS.md §3): the look LongAfterDark.scr's
// settings dialog and adimport's windows share. Windows 11 (Fluent) colours
// for light and dark app mode (or the system colours under high contrast),
// the user's accent colour, the Segoe UI Variable type ramp at the window's
// DPI, and the anti-aliased drawing the custom-painted controls share.
//
// Moved from the scr's ui_theme.h with the same names, signatures and
// behaviour (namespace adw::ui); the additions of COVERS.md §3.2 are marked.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace adw::ui {

struct Palette {
  bool dark = false, high_contrast = false;
  COLORREF base, card, card_stroke, divider;
  COLORREF text, text2, text3, text_disabled;
  COLORREF control, control_hover, control_pressed, control_disabled, control_stroke, control_stroke_bottom;
  COLORREF strong_stroke, rail;
  COLORREF row_hover, row_selected, row_selected_hover;
  COLORREF accent, accent_hover, accent_pressed, accent_disabled, on_accent, accent_text;
  COLORREF thumb, thumb_stroke;
  COLORREF focus_outer, focus_inner;
  COLORREF chip, chip_text, caution, caution_text, critical, critical_text;
};

// The accent ramp Windows keeps for the user's accent colour
// (HKCU\...\Explorer\Accent\AccentPalette), falling back to the default blue.
struct Accent {
  COLORREF light3, light2, light1, base, dark1, dark2, dark3;
};
Accent read_accent();

enum class ThemeMode { system, light, dark, high_contrast };
bool system_prefers_dark();      // "Choose your app mode"
bool high_contrast_on();
Palette make_palette(bool dark, const Accent& accent);
Palette high_contrast_palette();
// Tests and screenshots only: the system colours of one of Windows 11's
// contrast themes ("nightsky", "aquatic", "desert", "dusk"; any case) stand
// in for GetSysColor's in high_contrast_palette(), so a picture shows that
// theme without switching the machine to it. nullptr or "" goes back to the
// system's; false (and no change) for a name it doesn't know. The library
// never reads the environment for it: a program's own test hook calls it
// (LongAfterDark-test.scr's AD_UI_TEST_HC_SCHEME).
bool set_test_hc_scheme(const wchar_t* name);

// Fonts for one DPI (Segoe UI Variable when installed, else Segoe UI).
struct Fonts {
  int dpi = 0;
  HFONT caption = nullptr;       // 12
  HFONT body = nullptr;          // 14
  HFONT body_strong = nullptr;   // 14 semibold
  HFONT subtitle = nullptr;      // 20 semibold
  HFONT subtitle_small = nullptr;  // 16 semibold (a module name too long for the subtitle face)
  HFONT body_gray = nullptr;     // 14, always plain anti-aliasing (text that fades out)
  HFONT title = nullptr;         // 28 semibold
  HFONT icons = nullptr;         // Segoe Fluent Icons 16
  HFONT icons_small = nullptr;   // 12
  // `grayscale`: plain anti-aliasing instead of ClearType, for text drawn on
  // pictures (ClearType's colour fringes show on black).
  void create(int dpi, bool grayscale = false);
  void destroy();
  ~Fonts() { destroy(); }
};

struct Theme {
  Palette pal{};
  Fonts fonts;
  int dpi = 96;
  HBRUSH base_brush = nullptr, card_brush = nullptr;
  bool icons_font_is_fluent = false;

  // Re-reads the app mode, accent and high contrast (or applies `forced`).
  void load(ThemeMode forced = ThemeMode::system);
  void set_dpi(int dpi);
  void destroy();
  ~Theme() { destroy(); }
  int px(int dips) const { return MulDiv(dips, dpi, 96); }
  int body_line_height() const;   // the About box scrolls by whole lines of it
  float pxf(float dips) const { return dips * dpi / 96.0f; }
  // A 1-DIP line in whole pixels: 1 px up to 125%, 2 px from 150%.
  int hairline() const { return std::max(1, (int)std::lround(dpi / 96.0)); }
};

// ---- window chrome -----------------------------------------------------------------

// Dark or light title bar, rounded corners and the caption colour, to match.
void apply_window_chrome(HWND hwnd, const Palette& pal);
// Scroll bars and other native parts of a control in the app's mode.
void theme_native_control(HWND hwnd, bool dark);

// ---- drawing (GDI+ for anti-aliased shapes, GDI for ClearType text) ---------------

void gdiplus_startup();
void gdiplus_shutdown();

COLORREF blend(COLORREF a, COLORREF b, double t);   // t = 0 -> a, 1 -> b

void fill_rect(HDC dc, const RECT& r, COLORREF c);
void fill_round(HDC dc, const RECT& r, float radius, COLORREF c);
void stroke_round(HDC dc, const RECT& r, float radius, COLORREF c, float width = 1.0f, int alpha = 255);
// A control's body: fill, 1 px border, and the subtle darker bottom edge
// Fluent uses to lift buttons and dropdowns off the page.
void draw_control_body(HDC dc, const RECT& r, float radius, COLORREF fill, COLORREF stroke, COLORREF bottom);
void fill_ellipse(HDC dc, float cx, float cy, float radius, COLORREF c);
void stroke_ellipse(HDC dc, float cx, float cy, float radius, COLORREF c, float width = 1.0f);
void draw_check(HDC dc, const RECT& box, COLORREF c, float width);
// The keyboard focus ring: 2 px outer, 1 px inner, around `r`.
void draw_focus_ring(HDC dc, const RECT& r, float radius, const Palette& pal, float scale);
// Pixels outside a rounded rectangle, in `c` (rounds a rectangular picture's corners).
void mask_round_corners(HDC dc, const RECT& r, float radius, COLORREF c);
// `c` laid over `r` with its opacity going from `alpha_top` to `alpha_bottom`
// (0..255) down the rectangle: fades whatever is drawn there into `c`.
void fade_rect(HDC dc, const RECT& r, COLORREF c, int alpha_top, int alpha_bottom);
// `c` laid over `r` from nothing at the top to `max_alpha` at the bottom,
// easing out (most of the change early): text under it fades but stays legible.
void fade_out(HDC dc, const RECT& r, COLORREF c, int max_alpha);
// `c` over `r` from nothing at the left to opaque at the right.
void fade_in_right(HDC dc, const RECT& r, COLORREF c);
// A vertical gradient from `top` to `bottom` filling `r`.
void fill_gradient(HDC dc, const RECT& r, COLORREF top, COLORREF bottom);
// A radial glow: `c` at `alpha` in the middle of the ellipse `r`, nothing at its edge.
void glow_ellipse(HDC dc, const RECT& r, COLORREF c, int alpha);

// ---- the night sky (our own drawings, in the manner of the app's mark) -------------

// A four-point star centred on (cx, cy), `r` from the centre to a point.
void draw_sparkle(HDC dc, float cx, float cy, float r, COLORREF c, int alpha = 255);
// A crescent moon: a disc of radius `r` at (cx, cy) less an offset disc, in
// pale gold, with a soft glow around it when `glow` > 0 (its alpha).
void draw_crescent(HDC dc, float cx, float cy, float r, int glow = 0);
// A flying toaster in profile, facing left: chrome body `w` wide with its
// top-left at (x, y) (it is about 0.8 w tall, wings included), a slice of
// toast in the slot and a pair of feathered wings, raised by `flap` (0..1).
void draw_flying_toaster(HDC dc, float x, float y, float w, float flap, int alpha = 255);

void draw_text(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF c, UINT format);
SIZE measure_text(HDC dc, const std::wstring& text, HFONT font, UINT format = DT_SINGLELINE);

// ---- COVERS.md §3.2 additions ------------------------------------------------------

// true for the messages after which Theme::load (and a repaint) is due:
// WM_SETTINGCHANGE with lParam "ImmersiveColorSet", WM_SYSCOLORCHANGE, WM_THEMECHANGED,
// WM_DWMCOLORIZATIONCOLORCHANGED.
bool is_theme_change(UINT msg, WPARAM wp, LPARAM lp);
// Popup menus follow the app mode: uxtheme ordinal 135 (SetPreferredAppMode: AllowDark when
// `dark`, else Default) and 136 (FlushMenuThemes). A no-op where they are missing.
void allow_dark_menus(bool dark);
// A card: 8-DIP radius, pal.card, a hairline of pal.card_stroke.
void paint_card(HDC dc, const RECT& r, const Theme& t);
// The header band both apps share: the indigo glow and the stars (none under high contrast),
// the logo (the icon when given, else draw_crescent in its box), `name` in fonts.subtitle
// (the moon's navy RGB(0x1F,0x25,0x5A) in light mode, else pal.text) and `tagline` in
// fonts.body / pal.text2 on the same baseline. The scr's paint_night_band + header, moved.
// `band` is filled with pal.base first. Without an icon the logo box gets the app mark: a
// night-blue rounded tile with the crescent and three sparkles (the scr's icon, drawn).
void paint_header(HDC dc, const RECT& band, const RECT& logo, const RECT& title, HICON logo_icon,
                  const std::wstring& name, const std::wstring& tagline, const Theme& t);

// ---- additions beyond §3.2 (T) -----------------------------------------------------

// The app mark alone (what paint_header draws when it has no icon), filling `box`.
void draw_app_mark(HDC dc, const RECT& box);

// ---- additions at integration (asked for by the settings dialog's cover strip) ------

// `c` over `r` from opaque at the left to nothing at the right: the mirror image of
// fade_in_right.
void fade_in_left(HDC dc, const RECT& r, COLORREF c);
// fill_round at `alpha` (0..255): `c` laid over whatever is there (a veil).
void fill_round(HDC dc, const RECT& r, float radius, COLORREF c, int alpha);
// stroke_ellipse at `alpha` (0..255).
void stroke_ellipse(HDC dc, float cx, float cy, float radius, COLORREF c, float width, int alpha);

} // namespace adw::ui
