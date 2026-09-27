// adw_ui (COVERS.md §3): the settings dialog's controls, shared with adimport's
// windows (moved from the scr's ui_widgets.h; additions of §3.2 are marked),
// painted in the Windows 11 style while
// staying the stock Win32 controls underneath (so keyboard handling, focus,
// BM_GETCHECK / TBM_GETPOS / CB_GETCURSEL, accessibility and the smoke tests
// that drive them by ID all keep working):
//   push buttons, checkboxes and the Single/Random segmented pair through
//   NM_CUSTOMDRAW; trackbars through NM_CUSTOMDRAW plus a subclass that
//   moves the thumb to wherever the rail is clicked; dropdowns as owner-drawn
//   CBS_DROPDOWNLIST combo boxes whose face is painted by a subclass.
// Every focusable control's window is its visual body grown by focus_margin()
// on each side, where the keyboard focus ring is drawn.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>

#include "adw/ui/theme.h"

namespace adw::ui {

enum class Surface { base, card };
enum class ButtonRole { standard, accent, subtle, segment_left, segment_right, checkbox,
                        card };   // new: a settings card (COVERS.md §3.2)

// Per-control look, kept in window properties so the shared handlers can
// paint any control in either the dialog or the settings panel.
void set_surface(HWND h, Surface s);
COLORREF surface_color(HWND h, const Palette& pal);
void set_button_role(HWND h, ButtonRole role, wchar_t glyph = 0);
// Removes those properties from `h` and every window inside it (before they
// are destroyed, as SetProp asks).
void forget_looks(HWND h);

int focus_margin(int dpi);   // px

// NM_CUSTOMDRAW from a button (push, checkbox, radio). Returns the result for
// the parent to hand back, or -1 when it isn't ours to draw.
LRESULT custom_draw_button(const Theme& t, NMCUSTOMDRAW* cd);
// NM_CUSTOMDRAW from a trackbar.
LRESULT custom_draw_trackbar(const Theme& t, NMCUSTOMDRAW* cd);
// Click-anywhere / drag handling and hover for a trackbar; `stops` > 0 draws
// that many tick marks (a slider over labelled stops).
void subclass_trackbar(HWND tb, const Theme* t, int stops);

// Owner-drawn dropdowns.
void subclass_combo(HWND combo, const Theme* t);
void measure_combo_item(const Theme& t, MEASUREITEMSTRUCT* mi);
void draw_combo_item(const Theme& t, const DRAWITEMSTRUCT* di);
// Sizes the closed face to `face_h` px and the open list to `items` rows.
void size_combo(HWND combo, const Theme& t, int face_h);
HBRUSH combo_list_brush(const Theme& t);   // WM_CTLCOLORLISTBOX

// A status pill ("After Dark 4", "Coming soon").
enum class BadgeKind { neutral, caution, critical };
void draw_badge(HDC dc, const RECT& r, const std::wstring& text, BadgeKind kind, const Theme& t, COLORREF bg);

bool keyboard_cues(HWND h);   // focus rects / mnemonics currently shown
// The window whose focus ring is drawn: GetFocus(), or the screenshot hook's
// stand-in (a window that is never activated can't take the real focus).
HWND focused_window();
void set_focus_override(HWND h);

// A thin Windows 11 scroll bar over the right edge of a list view or a
// multi-line edit, instead of the native one: the control is made wider than
// what shows by the native bar's width and a window region hides that strip
// (the native bar keeps doing the scrolling, wheel and keys included). The
// thumb is painted over the content, widens under the pointer and drags;
// `always_wide` keeps it at that width whenever there is more to see (the
// settings panel, whose rows would otherwise give no sign of it). While a
// multi-line edit's text runs on below, its last line and a half fade out
// (to 80%; not under high contrast) and a "More" link at its foot scrolls
// on by a page.
void attach_overlay_scrollbar(HWND h, const Theme* t, bool always_wide = false);
// Places the control so that w x hgt shows; the native bar hides beyond it.
void place_with_overlay_scrollbar(HWND h, int x, int y, int w, int hgt);
// Paints the thumb (list views call this at the end of their custom draw;
// edits are painted by the subclass itself).
void paint_overlay_scrollbar(HWND h, HDC dc);

// ---- COVERS.md §3.2 additions ------------------------------------------------------

// ButtonRole::card: a full-width clickable card (Windows 11 "SettingsCard"): the glyph set with
// set_button_role at the left (fonts.icons), the window text's first line in fonts.body_strong
// and its second line in fonts.caption / text2, a chevron (E76C) at the right; card fill, hover
// control_hover, pressed control_pressed, focus ring around it. Height is the caller's (64 DIP
// typical).
// A Fluent progress bar over a stock msctls_progress32 (which keeps its UIA RangeValue): a
// 1-DIP rail in pal.strong_stroke and a 3-DIP accent bar with round ends; PBS_MARQUEE draws the
// indeterminate bar (a segment sweeping every 2 s). Call once after creating the control.
void subclass_progress(HWND progress, const Theme* t);

// ---- additions beyond §3.2 (T) -----------------------------------------------------

struct Image;
// A ButtonRole::card can show a release's cover (ui::draw_cover, 4:5, 48x60 DIP at most, fitted
// to the card's height less 8 DIP above and below) at its left in place of the glyph. `cover`
// is owned by the caller and must outlive the button (or be unset first with nullptr).
struct CardCover {
  const Image* tile = nullptr;   // null or empty: the generated cover
  std::wstring title;            // for the generated cover
};
void set_card_cover(HWND h, const CardCover* cover);
// The height a ButtonRole::card needs for `text` (first line, then the description, wrapped)
// at `width` px wide: at least 64 DIP, or 76 DIP with a cover.
int card_height(const Theme& t, const std::wstring& text, int width, bool with_cover);

// ---- additions at the critique's fix round ------------------------------------------

// The name screen readers give `h` (MSAA and the UIA proxy), in place of its window text:
// IAccPropServices::SetHwndPropStr(PROPID_ACC_NAME). For controls whose visible text is the
// same on every row ("Change cover…") so each says what it is for. Initializes COM on the
// calling thread for the call when it isn't already. false when the annotation failed.
bool set_accessible_name(HWND h, const std::wstring& name);

// ---- additions for the saver's sound settings (AUDIO.md §9) -------------------------

// A value slider over a stock trackbar (which keeps its UIA RangeValue and MSAA value), set up
// whole: range, page, position, the subclass above (click anywhere, drag, hover) and the name
// screen readers give it. Keyboard as a Windows 11 slider: Right and Up raise it by one, Left
// and Down lower it, Page Up / Page Down by `page`, Home / End to the ends; every move reaches
// the parent as WM_HSCROLL, as a stock trackbar's do (TB_LINEDOWN for a raise ... TB_ENDTRACK on
// the key's release). Drawn by custom_draw_trackbar in the light, dark and high-contrast
// palettes: the accent up to the thumb, the rail after it, the thumb's accent core; disabled,
// all in the palette's disabled tones. The parent routes NM_CUSTOMDRAW to custom_draw_trackbar.
struct SliderSpec {
  int min = 0, max = 100, page = 10, pos = 0;
  std::wstring name;   // accessible name ("" keeps the one Windows derives from the label before it)
};
void init_slider(HWND tb, const Theme* t, const SliderSpec& spec);

} // namespace adw::ui
