#include "adw/ui/widgets.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <oleacc.h>

#include "adw/ui/image.h"

#include <algorithm>
#include <cmath>

namespace adw::ui {

namespace {

constexpr wchar_t kSurfaceProp[] = L"adw.surface";
constexpr wchar_t kRoleProp[] = L"adw.role";
constexpr wchar_t kGlyphProp[] = L"adw.glyph";
constexpr wchar_t kCoverProp[] = L"adw.cover";

RECT deflate(RECT r, int d) {
  return RECT{r.left + d, r.top + d, r.right - d, r.bottom - d};
}

// Paint into a memory bitmap, then copy: no flicker from fill-then-draw.
struct Offscreen {
  HDC target, dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  RECT rc;
  Offscreen(HDC t, const RECT& r) : target(t), rc(r) {
    dc = CreateCompatibleDC(t);
    bmp = CreateCompatibleBitmap(t, std::max(1L, r.right - r.left), std::max(1L, r.bottom - r.top));
    old = SelectObject(dc, bmp);
    SetViewportOrgEx(dc, -r.left, -r.top, nullptr);
  }
  ~Offscreen() {
    SetViewportOrgEx(dc, 0, 0, nullptr);
    BitBlt(target, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
  }
};

std::wstring window_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(n + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}

ButtonRole role_of(HWND h) {
  auto v = (INT_PTR)GetPropW(h, kRoleProp);
  if (v) return (ButtonRole)(v - 1);
  LONG style = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
  if (style == BS_CHECKBOX || style == BS_AUTOCHECKBOX || style == BS_3STATE || style == BS_AUTO3STATE) {
    return ButtonRole::checkbox;
  }
  return ButtonRole::standard;
}

// ---- trackbar ------------------------------------------------------------------------

struct TrackState {
  const Theme* t = nullptr;
  int stops = 0;
  bool hot = false, thumb_hot = false, dragging = false;
  bool up_is_more = false;   // init_slider: Up / Page Up raise the value (a stock trackbar lowers it)
};

struct TrackGeom {
  float x0 = 0, x1 = 0, cy = 0, r = 0;
  int mn = 0, mx = 0;
  float x_of(int pos) const { return mx > mn ? x0 + (float)(pos - mn) * (x1 - x0) / (float)(mx - mn) : x0; }
  int pos_of(float x) const {
    if (mx <= mn || x1 <= x0) return mn;
    int p = mn + (int)std::lround((x - x0) / (x1 - x0) * (mx - mn));
    return std::clamp(p, mn, mx);
  }
};

TrackGeom track_geom(HWND tb, const Theme& t) {
  RECT cr{};
  GetClientRect(tb, &cr);
  TrackGeom g;
  const int fm = focus_margin(t.dpi);
  g.r = t.pxf(10);
  g.x0 = cr.left + fm + g.r;
  g.x1 = cr.right - fm - g.r;
  g.cy = (cr.top + cr.bottom) / 2.0f;
  g.mn = (int)SendMessageW(tb, TBM_GETRANGEMIN, 0, 0);
  g.mx = (int)SendMessageW(tb, TBM_GETRANGEMAX, 0, 0);
  return g;
}

void notify_scroll(HWND tb, int code, int pos) {
  SendMessageW(GetParent(tb), WM_HSCROLL, MAKEWPARAM(code, pos), (LPARAM)tb);
}

LRESULT CALLBACK track_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* st = reinterpret_cast<TrackState*>(ref);
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      if (!IsWindowEnabled(h)) break;
      SetFocus(h);
      SetCapture(h);
      st->dragging = true;
      TrackGeom g = track_geom(h, *st->t);
      int pos = g.pos_of((float)GET_X_LPARAM(lp));
      SendMessageW(h, TBM_SETPOS, TRUE, pos);
      notify_scroll(h, TB_THUMBTRACK, pos);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TrackGeom g = track_geom(h, *st->t);
      if (st->dragging) {
        int pos = g.pos_of((float)GET_X_LPARAM(lp));
        if (pos != (int)SendMessageW(h, TBM_GETPOS, 0, 0)) {
          SendMessageW(h, TBM_SETPOS, TRUE, pos);
          notify_scroll(h, TB_THUMBTRACK, pos);
          InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
      }
      float tx = g.x_of((int)SendMessageW(h, TBM_GETPOS, 0, 0));
      bool thumb = std::fabs(GET_X_LPARAM(lp) - tx) <= g.r + 2 && std::fabs(GET_Y_LPARAM(lp) - g.cy) <= g.r + 2;
      if (!st->hot || thumb != st->thumb_hot) {
        st->hot = true;
        st->thumb_hot = thumb;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (st->dragging) {
        st->dragging = false;
        ReleaseCapture();
        int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
        notify_scroll(h, TB_THUMBPOSITION, pos);
        notify_scroll(h, TB_ENDTRACK, pos);
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (st->dragging) {
        st->dragging = false;
        InvalidateRect(h, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      st->hot = st->thumb_hot = false;
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_KEYDOWN: {
      // A Windows 11 slider: up is more. A stock horizontal trackbar takes
      // Up and Page Up as towards the left (TBS_DOWNISLEFT would turn Left
      // around with them), so these four are done here, as it does the rest.
      if (!st->up_is_more) break;
      if (!IsWindowEnabled(h)) return 0;   // (only a sent key reaches a disabled one: it moves nothing)
      const int line = std::max(1, (int)SendMessageW(h, TBM_GETLINESIZE, 0, 0));
      const int page = std::max(1, (int)SendMessageW(h, TBM_GETPAGESIZE, 0, 0));
      int delta = 0, code = -1;
      switch (wp) {
        case VK_UP: delta = line, code = TB_LINEDOWN; break;
        case VK_DOWN: delta = -line, code = TB_LINEUP; break;
        case VK_PRIOR: delta = page, code = TB_PAGEDOWN; break;
        case VK_NEXT: delta = -page, code = TB_PAGEUP; break;
      }
      if (code < 0) break;
      const int mn = (int)SendMessageW(h, TBM_GETRANGEMIN, 0, 0), mx = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
      const int cur = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
      const int pos = std::clamp(cur + delta, mn, mx);
      if (pos != cur) {
        SendMessageW(h, TBM_SETPOS, TRUE, pos);
        notify_scroll(h, code, 0);
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;   // the key's release still reaches the trackbar: TB_ENDTRACK
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, track_proc, 1);
      delete st;
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// ---- combo box ------------------------------------------------------------------------

struct ComboState {
  const Theme* t = nullptr;
  bool hot = false;
};

void paint_combo_face(HWND combo, HDC target, const Theme& t) {
  RECT cr{};
  GetClientRect(combo, &cr);
  Offscreen off(target, cr);
  HDC dc = off.dc;
  const Palette& p = t.pal;
  fill_rect(dc, cr, surface_color(combo, p));
  const int fm = focus_margin(t.dpi);
  RECT body = deflate(cr, fm);
  auto* st = reinterpret_cast<ComboState*>(GetPropW(combo, L"adw.combo"));
  const bool disabled = !IsWindowEnabled(combo);
  const bool dropped = SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) != 0;
  const bool hot = st && st->hot;
  COLORREF fill = disabled ? p.control_disabled : dropped ? p.control_pressed : hot ? p.control_hover : p.control;
  draw_control_body(dc, body, t.pxf(4), fill, p.control_stroke, dropped || disabled ? p.control_stroke : p.control_stroke_bottom);
  RECT text = body;
  text.left += t.px(12);
  text.right -= t.px(36);
  draw_text(dc, window_text(combo), text, t.fonts.body, disabled ? p.text_disabled : p.text,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  RECT chev{body.right - t.px(34), body.top, body.right - t.px(10), body.bottom};
  draw_text(dc, L"", chev, t.fonts.icons_small, disabled ? p.text_disabled : p.text2,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
  if (focused_window() == combo && !dropped && keyboard_cues(combo)) draw_focus_ring(dc, cr, t.pxf(4) + fm, p, t.dpi / 96.0f);
}

LRESULT CALLBACK combo_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* st = reinterpret_cast<ComboState*>(ref);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      paint_combo_face(h, dc, *st->t);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:
      paint_combo_face(h, (HDC)wp, *st->t);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE:
      if (!st->hot) {
        st->hot = true;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      st->hot = false;
      InvalidateRect(h, nullptr, FALSE);
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(h, combo_proc, 2);
      RemovePropW(h, L"adw.combo");
      delete st;
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_CAPTURECHANGED:
    case WM_COMMAND:
    case CB_SETCURSEL:
    case CB_SHOWDROPDOWN: {
      LRESULT r = DefSubclassProc(h, msg, wp, lp);
      InvalidateRect(h, nullptr, FALSE);
      return r;
    }
  }
  return DefSubclassProc(h, msg, wp, lp);
}

// ---- overlay scroll bar ----------------------------------------------------------------

struct OverlayState {
  const Theme* t = nullptr;
  int visible_w = 0;
  bool hot = false, dragging = false, is_list = false, is_edit = false;
  bool always_wide = false;   // the thumb stays at its hover width while there is more to see
  bool more_hot = false;      // the pointer is on an edit's "More" link
  int drag_offset = 0;   // pointer y minus thumb top, while dragging
  int last_pos = -1;
};

OverlayState* overlay_of(HWND h) { return reinterpret_cast<OverlayState*>(GetPropW(h, L"adw.overlay")); }

struct ThumbGeom {
  bool show = false;
  RECT track{}, thumb{};
  int range = 0, page = 0, pos = 0, min = 0;
};

int visible_right(HWND h, const OverlayState& st) {
  RECT cr{};
  GetClientRect(h, &cr);
  return std::min<int>(cr.right, st.visible_w ? st.visible_w : cr.right);
}

ThumbGeom thumb_geom(HWND h, const OverlayState& st) {
  ThumbGeom g;
  SCROLLINFO si{sizeof(si), SIF_ALL};
  if (!(GetWindowLongW(h, GWL_STYLE) & WS_VSCROLL) || !GetScrollInfo(h, SB_VERT, &si)) return g;
  g.min = si.nMin;
  g.range = si.nMax - si.nMin + 1;
  g.page = (int)si.nPage;
  g.pos = si.nPos - si.nMin;
  if (g.page <= 0 || g.range <= g.page) return g;
  RECT cr{};
  GetClientRect(h, &cr);
  const Theme& t = *st.t;
  const int right = visible_right(h, st);
  const bool wide = st.hot || st.dragging || st.always_wide;
  const int w = t.px(wide ? 6 : 3);
  const int inset = t.px(3);
  g.track = RECT{right - inset - t.px(6), cr.top + inset, right - inset, cr.bottom - inset};
  const int th = g.track.bottom - g.track.top;
  const int thumb_h = std::min(th, std::max(t.px(28), (int)((long long)th * g.page / g.range)));
  const int travel = th - thumb_h;
  const int y = g.track.top + (int)((long long)travel * g.pos / std::max(1, g.range - g.page));
  g.thumb = RECT{g.track.right - w, y, g.track.right, y + thumb_h};
  g.show = true;
  return g;
}

void scroll_to(HWND h, OverlayState& st, int target) {
  SCROLLINFO si{sizeof(si), SIF_ALL};
  if (!GetScrollInfo(h, SB_VERT, &si)) return;
  target = std::clamp(target, si.nMin, std::max(si.nMin, si.nMax - (int)si.nPage + 1));
  int delta = target - si.nPos;
  if (!delta) return;
  if (st.is_list) {
    // Group view scrolls in pixels; a plain report view in rows.
    int unit = 1;
    if (!ListView_IsGroupViewEnabled(h)) {
      RECT r{};
      if (ListView_GetItemRect(h, 0, &r, LVIR_BOUNDS)) unit = std::max(1L, r.bottom - r.top);
    }
    ListView_Scroll(h, 0, delta * unit);
  } else if (st.is_edit) {
    SendMessageW(h, EM_LINESCROLL, 0, delta);
  } else {
    SendMessageW(h, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, target), 0);
  }
}

// A multi-line edit whose text runs on below what shows: true, with `line`
// the height of one line of its text.
bool more_below(HWND h, const OverlayState& st, int* line) {
  if (!st.is_edit) return false;
  SCROLLINFO si{sizeof(si), SIF_ALL};
  if (!(GetWindowLongW(h, GWL_STYLE) & WS_VSCROLL) || !GetScrollInfo(h, SB_VERT, &si)) return false;
  if (si.nPage == 0 || si.nMax - si.nMin + 1 <= (int)si.nPage) return false;
  if (si.nPos + (int)si.nPage > si.nMax) return false;
  if (line) {
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    *line = std::max(1, (int)(tm.tmHeight + tm.tmExternalLeading));
  }
  return true;
}

// ...its last line and a half fade into the background, so a cut-off
// sentence reads as "continues", not as the end. Empty when it all shows (or
// it's scrolled to the end), and under high contrast, which never fades text.
RECT fade_band(HWND h, const OverlayState& st) {
  RECT none{0, 0, 0, 0};
  int line = 0;
  if (st.t->pal.high_contrast || !more_below(h, st, &line)) return none;
  RECT cr{};
  GetClientRect(h, &cr);
  return RECT{cr.left, std::max(cr.top, cr.bottom - line * 3 / 2), visible_right(h, st), cr.bottom};
}

// ...and a "More" link at its foot, by the scroll bar.
RECT more_rect(HWND h, const OverlayState& st) {
  RECT none{0, 0, 0, 0};
  int line = 0;
  if (!more_below(h, st, &line)) return none;
  RECT cr{};
  GetClientRect(h, &cr);
  const Theme& t = *st.t;
  HDC dc = GetDC(h);
  const int tw = measure_text(dc, L"More", t.fonts.caption).cx + t.px(4) + t.px(12);
  ReleaseDC(h, dc);
  const int right = visible_right(h, st) - t.px(14);
  return RECT{right - tw - t.px(8), std::max(cr.top, cr.bottom - line), right, cr.bottom};
}

void paint_more(HWND h, HDC dc, const OverlayState& st) {
  RECT r = more_rect(h, st);
  if (r.right <= r.left) return;
  const Theme& t = *st.t;
  const COLORREF bg = surface_color(h, t.pal);
  // Clear of the text beside it: solid under the link, fading in to its left.
  RECT solid{r.left + t.px(6), r.top, visible_right(h, st), r.bottom};
  fill_rect(dc, solid, bg);
  if (!t.pal.high_contrast) fade_in_right(dc, RECT{r.left - t.px(18), r.top, solid.left, r.bottom}, bg);
  else fill_rect(dc, RECT{r.left - t.px(6), r.top, solid.left, r.bottom}, bg);
  const COLORREF ink = st.more_hot ? blend(t.pal.accent_text, bg, 0.2) : t.pal.accent_text;
  RECT gr{r.right - t.px(12), r.top, r.right, r.bottom};
  draw_text(dc, L"\uE70D", gr, t.fonts.icons_small, ink, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
  RECT tr{r.left + t.px(8), r.top, gr.left - t.px(4), r.bottom};
  draw_text(dc, L"More", tr, t.fonts.caption, ink, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
}

void invalidate_strip(HWND h, const OverlayState& st) {
  RECT cr{};
  GetClientRect(h, &cr);
  const int right = visible_right(h, st);
  RECT strip{right - st.t->px(16), cr.top, right, cr.bottom};
  InvalidateRect(h, &strip, FALSE);
}

LRESULT CALLBACK overlay_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* st = reinterpret_cast<OverlayState*>(ref);
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      if (st->is_edit) {
        RECT more = more_rect(h, *st);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&more, pt)) {
          SendMessageW(h, EM_SCROLL, SB_PAGEDOWN, 0);
          return 0;
        }
      }
      ThumbGeom g = thumb_geom(h, *st);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT zone = g.track;
      zone.left -= st->t->px(6);
      if (!g.show || !PtInRect(&zone, pt)) break;
      // On the thumb: drag it. Elsewhere on the track: jump there, then drag.
      st->drag_offset = (pt.y >= g.thumb.top && pt.y < g.thumb.bottom) ? pt.y - g.thumb.top
                                                                       : (g.thumb.bottom - g.thumb.top) / 2;
      st->dragging = true;
      SetCapture(h);
      [[fallthrough]];
    }
    case WM_MOUSEMOVE: {
      if (st->dragging) {
        ThumbGeom g = thumb_geom(h, *st);
        if (g.show) {
          const int th = g.track.bottom - g.track.top, thumb_h = g.thumb.bottom - g.thumb.top;
          const int travel = std::max(1, th - thumb_h);
          const int y = GET_Y_LPARAM(lp) - st->drag_offset - g.track.top;
          const int target = g.min + (int)((long long)std::clamp(y, 0, travel) * (g.range - g.page) / travel);
          scroll_to(h, *st, target);
          invalidate_strip(h, *st);
        }
        return 0;
      }
      if (st->is_edit) {
        RECT more = more_rect(h, *st);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool on = PtInRect(&more, pt) != FALSE;
        if (on != st->more_hot) {
          st->more_hot = on;
          InvalidateRect(h, &more, FALSE);
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
          TrackMouseEvent(&tme);
        }
      }
      const bool hot = GET_X_LPARAM(lp) >= visible_right(h, *st) - st->t->px(16);
      if (hot != st->hot) {
        st->hot = hot;
        invalidate_strip(h, *st);
        if (hot) {
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
          TrackMouseEvent(&tme);
        }
      }
      break;
    }
    case WM_LBUTTONUP:
      if (st->dragging) {
        st->dragging = false;
        ReleaseCapture();
        invalidate_strip(h, *st);
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      st->dragging = false;
      break;
    case WM_MOUSELEAVE:
      if (st->hot) {
        st->hot = false;
        invalidate_strip(h, *st);
      }
      if (st->more_hot) {
        st->more_hot = false;
        RECT more = more_rect(h, *st);
        InvalidateRect(h, &more, FALSE);
      }
      break;
    case WM_SETCURSOR:
      if (st->is_edit) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        RECT more = more_rect(h, *st);
        if (PtInRect(&more, pt)) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      break;
    case WM_PAINT:
      if (!st->is_list) {
        // The faded band is always repainted whole, then faded once (a
        // partial repaint would otherwise fade part of it twice).
        RECT band = fade_band(h, *st), more = more_rect(h, *st);
        if (band.bottom > band.top) InvalidateRect(h, &band, TRUE);
        if (more.bottom > more.top) InvalidateRect(h, &more, TRUE);
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (HDC dc = GetDC(h)) {
          if (band.bottom > band.top) fade_out(dc, band, surface_color(h, st->t->pal), 204);
          paint_more(h, dc, *st);
          paint_overlay_scrollbar(h, dc);
          ReleaseDC(h, dc);
        }
        return r;
      }
      break;
    case WM_NCDESTROY: {
      RemoveWindowSubclass(h, overlay_proc, 4);
      RemovePropW(h, L"adw.overlay");
      LRESULT r = DefSubclassProc(h, msg, wp, lp);
      delete st;
      return r;
    }
  }
  LRESULT r = DefSubclassProc(h, msg, wp, lp);
  // Whatever scrolled the content (wheel, keys, LVM_ENSUREVISIBLE) moved the
  // thumb's pixels along with it: repaint the strip.
  SCROLLINFO si{sizeof(si), SIF_POS};
  if (msg != WM_PAINT && GetScrollInfo(h, SB_VERT, &si) && si.nPos != st->last_pos) {
    st->last_pos = si.nPos;
    // An edit's content moved under its faded band (scrolling copies pixels):
    // repaint it all so no faded line is carried up.
    if (st->is_edit) InvalidateRect(h, nullptr, TRUE);
    else invalidate_strip(h, *st);
  }
  return r;
}

} // namespace

void attach_overlay_scrollbar(HWND h, const Theme* t, bool always_wide) {
  auto* st = new OverlayState;
  st->t = t;
  st->always_wide = always_wide;
  wchar_t cls[32] = {};
  GetClassNameW(h, cls, 32);
  st->is_list = wcscmp(cls, WC_LISTVIEWW) == 0;
  st->is_edit = wcscmp(cls, WC_EDITW) == 0;
  SetPropW(h, L"adw.overlay", st);
  SetWindowSubclass(h, overlay_proc, 4, reinterpret_cast<DWORD_PTR>(st));
}

void place_with_overlay_scrollbar(HWND h, int x, int y, int w, int hgt) {
  OverlayState* st = overlay_of(h);
  const int sb = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(h));
  if (st) st->visible_w = w;
  SetWindowPos(h, nullptr, x, y, w + (st ? sb : 0), hgt, SWP_NOZORDER | SWP_NOACTIVATE);
  if (st) SetWindowRgn(h, CreateRectRgn(0, 0, w, hgt), TRUE);
}

void paint_overlay_scrollbar(HWND h, HDC dc) {
  OverlayState* st = overlay_of(h);
  if (!st) return;
  ThumbGeom g = thumb_geom(h, *st);
  if (!g.show) return;
  const Palette& p = st->t->pal;
  if (st->hot || st->dragging) {
    COLORREF track = p.high_contrast ? p.card : blend(p.card, p.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 0.06);
    RECT tr{g.thumb.left - st->t->px(1), g.track.top, g.thumb.right + st->t->px(1), g.track.bottom};
    fill_round(dc, tr, (tr.right - tr.left) / 2.0f, track);
  }
  COLORREF thumb = p.high_contrast ? p.text : st->dragging ? p.text2 : p.text3;
  fill_round(dc, g.thumb, (g.thumb.right - g.thumb.left) / 2.0f, thumb);
}

// ---- properties ---------------------------------------------------------------------

void set_surface(HWND h, Surface s) { SetPropW(h, kSurfaceProp, (HANDLE)(INT_PTR)((int)s + 1)); }

COLORREF surface_color(HWND h, const Palette& pal) {
  auto v = (INT_PTR)GetPropW(h, kSurfaceProp);
  return v == (INT_PTR)Surface::card + 1 ? pal.card : pal.base;
}

void set_button_role(HWND h, ButtonRole role, wchar_t glyph) {
  SetPropW(h, kRoleProp, (HANDLE)(INT_PTR)((int)role + 1));
  if (glyph) SetPropW(h, kGlyphProp, (HANDLE)(INT_PTR)glyph);
}

void forget_looks(HWND h) {
  auto forget = [](HWND w) {
    RemovePropW(w, kSurfaceProp);
    RemovePropW(w, kRoleProp);
    RemovePropW(w, kGlyphProp);
    RemovePropW(w, kCoverProp);
  };
  forget(h);
  EnumChildWindows(h, [](HWND w, LPARAM) -> BOOL {
    RemovePropW(w, kSurfaceProp);
    RemovePropW(w, kRoleProp);
    RemovePropW(w, kGlyphProp);
    RemovePropW(w, kCoverProp);
    return TRUE;
  }, 0);
}

int focus_margin(int dpi) { return std::max(3, MulDiv(3, dpi, 96)); }

namespace {
HWND g_focus_override = nullptr;
}

HWND focused_window() { return g_focus_override ? g_focus_override : GetFocus(); }
void set_focus_override(HWND h) { g_focus_override = h; }

bool keyboard_cues(HWND h) {
  if (g_focus_override) return true;
  return !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}

// ---- cards (COVERS.md §3.2) -----------------------------------------------------------

namespace {

// A card's parts, in DIPs from its body's edges (the body is the window less
// focus_margin() on each side).
constexpr int kCardPadLeft = 16, kCardGlyphW = 20, kCardGap = 16;
constexpr int kCardCoverPadLeft = 12, kCardCoverMaxH = 60;
constexpr int kCardChevronW = 12, kCardPadRight = 16;
constexpr int kCardPadV = 12, kCardLineGap = 2;
constexpr int kCardMinH = 64, kCardMinHCover = 76;

// The first line, and the rest ("" when there is no newline).
void split_card_text(const std::wstring& text, std::wstring& head, std::wstring& rest) {
  size_t nl = text.find(L'\n');
  head = nl == std::wstring::npos ? text : text.substr(0, nl);
  rest = nl == std::wstring::npos ? std::wstring() : text.substr(nl + 1);
}

// Where the text column starts, relative to the body's left edge; with a
// cover, also the cover's box (4:5, at most 60 DIP tall, 8 DIP clear of the
// body's top and bottom).
int card_text_left(const Theme& t, bool with_cover, const RECT& body, RECT* cover_box) {
  if (!with_cover) return t.px(kCardPadLeft + kCardGlyphW + kCardGap);
  const int bh = body.bottom - body.top;
  const int ch = std::max(t.px(16), std::min(t.px(kCardCoverMaxH), bh - t.px(16)));
  const int cw = ch * 4 / 5;
  const int x = body.left + t.px(kCardCoverPadLeft);
  const int y = body.top + (bh - ch) / 2;
  if (cover_box) *cover_box = RECT{x, y, x + cw, y + ch};
  return t.px(kCardCoverPadLeft) + cw + t.px(kCardGap);
}

// The width of the text column for a body `bw` px wide.
int card_text_width(const Theme& t, int bw, int text_left) {
  return std::max(1, bw - t.px(kCardPadRight + kCardChevronW + kCardGap) - text_left);
}

void paint_card_button(HDC dc, HWND h, const RECT& cr, const Theme& t, const std::wstring& text, bool disabled,
                       bool hot, bool pressed, bool focus, UINT prefix) {
  const Palette& p = t.pal;
  const float s = t.dpi / 96.0f;
  const int fm = focus_margin(t.dpi);
  const float radius = 4 * s;
  RECT body = deflate(cr, fm);
  const int bh = body.bottom - body.top;
  const COLORREF fill = disabled ? p.card : pressed ? p.control_pressed : hot ? p.control_hover : p.card;
  fill_round(dc, body, radius, fill);
  if (p.high_contrast && !disabled && (hot || pressed)) {
    // High contrast has no tints: the pointer shows as a Highlight outline.
    stroke_round(dc, body, radius, p.accent, 2.0f * t.hairline());
  } else {
    stroke_round(dc, body, radius, p.card_stroke, (float)t.hairline());
  }
  const COLORREF ink = disabled ? p.text_disabled : p.text;
  const COLORREF ink2 = disabled ? p.text_disabled : p.text2;
  const auto* cover = reinterpret_cast<const CardCover*>(GetPropW(h, kCoverProp));
  RECT cover_box{};
  const int text_left = body.left + card_text_left(t, cover != nullptr, body, &cover_box);
  if (cover) {
    draw_cover(dc, cover_box, cover->tile, cover->title, t);
  } else if (wchar_t glyph = (wchar_t)(INT_PTR)GetPropW(h, kGlyphProp)) {
    RECT gr{body.left + t.px(kCardPadLeft), body.top, body.left + t.px(kCardPadLeft + kCardGlyphW), body.bottom};
    draw_text(dc, std::wstring(1, glyph), gr, t.fonts.icons, ink, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
  }
  RECT chev{body.right - t.px(kCardPadRight + kCardChevronW), body.top, body.right - t.px(kCardPadRight), body.bottom};
  draw_text(dc, L"\uE76C", chev, t.fonts.icons_small, ink2, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
  std::wstring head, rest;
  split_card_text(text, head, rest);
  const int tw = card_text_width(t, body.right - body.left, text_left - body.left);
  HGDIOBJ old = SelectObject(dc, t.fonts.body_strong);
  RECT hm{0, 0, tw, 0};
  DrawTextW(dc, head.c_str(), (int)head.size(), &hm, DT_SINGLELINE | DT_CALCRECT | prefix);
  SelectObject(dc, t.fonts.caption);
  RECT rm{0, 0, tw, 0};
  if (!rest.empty()) DrawTextW(dc, rest.c_str(), (int)rest.size(), &rm, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
  SelectObject(dc, old);
  const int head_h = hm.bottom - hm.top, rest_h = rest.empty() ? 0 : rm.bottom - rm.top;
  const int block = head_h + (rest.empty() ? 0 : t.px(kCardLineGap) + rest_h);
  const int top = body.top + std::max(0, (bh - block) / 2);
  RECT hr{text_left, top, text_left + tw, top + head_h};
  draw_text(dc, head, hr, t.fonts.body_strong, ink, DT_SINGLELINE | DT_END_ELLIPSIS | prefix);
  if (!rest.empty()) {
    RECT rr{text_left, hr.bottom + t.px(kCardLineGap), text_left + tw,
            std::min<LONG>(body.bottom, hr.bottom + t.px(kCardLineGap) + rest_h)};
    draw_text(dc, rest, rr, t.fonts.caption, ink2, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX);
  }
  if (focus) draw_focus_ring(dc, cr, radius + fm, p, s);
}

}  // namespace

void set_card_cover(HWND h, const CardCover* cover) {
  if (cover) SetPropW(h, kCoverProp, (HANDLE)cover);
  else RemovePropW(h, kCoverProp);
  InvalidateRect(h, nullptr, FALSE);
}

int card_height(const Theme& t, const std::wstring& text, int width, bool with_cover) {
  const int fm = focus_margin(t.dpi);
  const int min_body = t.px(with_cover ? kCardMinHCover : kCardMinH);
  RECT body{0, 0, std::max(1, width - 2 * fm), min_body};
  const int text_left = card_text_left(t, with_cover, body, nullptr);
  const int tw = card_text_width(t, body.right, text_left);
  std::wstring head, rest;
  split_card_text(text, head, rest);
  HDC dc = GetDC(nullptr);
  HGDIOBJ old = SelectObject(dc, t.fonts.body_strong);
  RECT hm{0, 0, tw, 0};
  DrawTextW(dc, head.c_str(), (int)head.size(), &hm, DT_SINGLELINE | DT_CALCRECT);
  SelectObject(dc, t.fonts.caption);
  RECT rm{0, 0, tw, 0};
  if (!rest.empty()) DrawTextW(dc, rest.c_str(), (int)rest.size(), &rm, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  const int block = (hm.bottom - hm.top) + (rest.empty() ? 0 : t.px(kCardLineGap) + (rm.bottom - rm.top));
  return std::max(min_body, block + 2 * t.px(kCardPadV)) + 2 * fm;
}

// ---- progress bar (COVERS.md §3.2) ---------------------------------------------------------

namespace {

constexpr UINT_PTR kProgressTimer = 0xAD01;

struct ProgressState {
  const Theme* t = nullptr;
  bool marquee = false;
  ULONGLONG marquee_start = 0;
};

void paint_progress(HWND h, HDC target, const ProgressState& st) {
  RECT cr{};
  GetClientRect(h, &cr);
  if (cr.right <= cr.left || cr.bottom <= cr.top) return;
  Offscreen off(target, cr);
  HDC dc = off.dc;
  const Theme& t = *st.t;
  const Palette& p = t.pal;
  fill_rect(dc, cr, surface_color(h, p));
  const float s = t.dpi / 96.0f;
  const float cy = (cr.top + cr.bottom) / 2.0f;
  const float bar_h = std::max(2.0f, std::round(3 * s));
  const int rail_h = t.hairline();
  const bool disabled = !IsWindowEnabled(h);
  // The rail: one DIP, centred on the bar.
  const LONG rail_top = (LONG)std::lround(cy - rail_h / 2.0f);
  RECT rail{cr.left, rail_top, cr.right, rail_top + rail_h};
  fill_rect(dc, rail, disabled ? p.text_disabled : p.strong_stroke);
  const int state = (int)SendMessageW(h, PBM_GETSTATE, 0, 0);
  const COLORREF ink = disabled              ? p.accent_disabled
                       : state == PBST_ERROR  ? p.critical_text
                       : state == PBST_PAUSED ? p.caution_text
                                              : p.accent;
  const float w = (float)(cr.right - cr.left);
  float x0 = 0, x1 = 0;
  if (GetWindowLongW(h, GWL_STYLE) & PBS_MARQUEE) {
    if (!st.marquee) return;
    // One segment, 40% of the rail, sweeping across every 2 s (eased in and out).
    const double ph = (double)((GetTickCount64() - st.marquee_start) % 2000) / 2000.0;
    const double e = ph < 0.5 ? 2 * ph * ph : 1 - 2 * (1 - ph) * (1 - ph);
    const float seg = w * 0.4f;
    x0 = (float)(cr.left - seg + e * (w + seg));
    x1 = x0 + seg;
  } else {
    PBRANGE range{};
    SendMessageW(h, PBM_GETRANGE, TRUE, (LPARAM)&range);
    const int pos = (int)SendMessageW(h, PBM_GETPOS, 0, 0);
    const double f =
        range.iHigh > range.iLow ? std::clamp((double)(pos - range.iLow) / (range.iHigh - range.iLow), 0.0, 1.0) : 0.0;
    x0 = (float)cr.left;
    x1 = (float)(cr.left + f * w);
  }
  x0 = std::max(x0, (float)cr.left);
  x1 = std::min(x1, (float)cr.right);
  if (x1 - x0 < 0.5f) return;
  // Round ends, never narrower than the bar is tall.
  if (x1 - x0 < bar_h) x1 = std::min((float)cr.right, x0 + bar_h);
  RECT bar{(LONG)std::lround(x0), (LONG)std::lround(cy - bar_h / 2), (LONG)std::lround(x1),
           (LONG)std::lround(cy + bar_h / 2)};
  fill_round(dc, bar, bar_h / 2, ink);
}

LRESULT CALLBACK progress_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* st = reinterpret_cast<ProgressState*>(ref);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      paint_progress(h, dc, *st);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:
      paint_progress(h, (HDC)wp, *st);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case PBM_SETMARQUEE: {
      LRESULT r = DefSubclassProc(h, msg, wp, lp);
      const bool on = wp != 0;
      if (on && !st->marquee) st->marquee_start = GetTickCount64();
      st->marquee = on;
      if (on) SetTimer(h, kProgressTimer, 16, nullptr);
      else KillTimer(h, kProgressTimer);
      InvalidateRect(h, nullptr, FALSE);
      return r;
    }
    case WM_TIMER:
      if (wp == kProgressTimer) {
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      break;
    case PBM_SETPOS:
    case PBM_DELTAPOS:
    case PBM_STEPIT:
    case PBM_SETRANGE:
    case PBM_SETRANGE32:
    case PBM_SETSTATE:
    case WM_ENABLE:
    case WM_STYLECHANGED:
    case WM_SIZE: {
      LRESULT r = DefSubclassProc(h, msg, wp, lp);
      InvalidateRect(h, nullptr, FALSE);
      return r;
    }
    case WM_NCDESTROY:
      KillTimer(h, kProgressTimer);
      RemoveWindowSubclass(h, progress_proc, 5);
      delete st;
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

void subclass_progress(HWND progress, const Theme* t) {
  auto* st = new ProgressState;
  st->t = t;
  SetWindowSubclass(progress, progress_proc, 5, reinterpret_cast<DWORD_PTR>(st));
  InvalidateRect(progress, nullptr, FALSE);
}

// ---- buttons --------------------------------------------------------------------------

LRESULT custom_draw_button(const Theme& t, NMCUSTOMDRAW* cd) {
  if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
  HWND h = cd->hdr.hwndFrom;
  const Palette& p = t.pal;
  const ButtonRole role = role_of(h);
  const float s = t.dpi / 96.0f;
  const int fm = focus_margin(t.dpi);
  RECT cr = cd->rc;
  Offscreen off(cd->hdc, cr);
  HDC dc = off.dc;
  const COLORREF bg = surface_color(h, p);
  fill_rect(dc, cr, bg);
  const bool disabled = (cd->uItemState & CDIS_DISABLED) || !IsWindowEnabled(h);
  const bool hot = (cd->uItemState & CDIS_HOT) != 0;
  const bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
  const bool focus = ((cd->uItemState & CDIS_FOCUS) || focused_window() == h) && keyboard_cues(h);
  const UINT prefix = keyboard_cues(h) || (cd->uItemState & CDIS_SHOWKEYBOARDCUES) ? 0 : DT_HIDEPREFIX;
  const std::wstring text = window_text(h);
  const float radius = 4 * s;

  switch (role) {
    case ButtonRole::checkbox: {
      const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
      const int box = t.px(20);
      RECT body = deflate(cr, fm);
      RECT b{body.left, (body.top + body.bottom - box) / 2, body.left + box, (body.top + body.bottom - box) / 2 + box};
      if (checked) {
        COLORREF f = disabled ? p.accent_disabled : pressed ? p.accent_pressed : hot ? p.accent_hover : p.accent;
        fill_round(dc, b, radius, f);
        draw_check(dc, b, disabled ? p.text_disabled : p.on_accent, 1.6f * s);
      } else {
        COLORREF f = pressed ? p.control_pressed : hot ? p.control_hover : p.control;
        fill_round(dc, b, radius, f);
        stroke_round(dc, b, radius, disabled ? p.text_disabled : p.strong_stroke, (float)t.hairline());
      }
      RECT tr{b.right + t.px(8), body.top, body.right, body.bottom};
      draw_text(dc, text, tr, t.fonts.body, disabled ? p.text_disabled : p.text,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | prefix);
      if (focus) draw_focus_ring(dc, cr, radius + fm, p, s);
      break;
    }
    case ButtonRole::segment_left:
    case ButtonRole::segment_right: {
      const bool left = role == ButtonRole::segment_left;
      const bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
      // The pair's container spans both windows; each paints its half.
      RECT body = cr;
      body.top += fm;
      body.bottom -= fm;
      if (left) body.left += fm;
      else body.right -= fm;
      const int bw = body.right - body.left;
      RECT whole = left ? RECT{body.left, body.top, body.left + 2 * bw, body.bottom}
                        : RECT{body.right - 2 * bw, body.top, body.right, body.bottom};
      const float cr_radius = 6 * s;
      COLORREF track = p.high_contrast ? p.base : p.dark ? blend(p.base, RGB(255, 255, 255), 0.045) : blend(p.base, RGB(0, 0, 0), 0.045);
      fill_round(dc, whole, cr_radius, p.control_stroke);
      RECT inner = deflate(whole, t.hairline());
      fill_round(dc, inner, cr_radius - 1, track);
      if (checked) {
        RECT seg = deflate(body, t.px(3));
        if (left) seg.right += t.px(3) - t.px(1);
        else seg.left -= t.px(3) - t.px(1);
        COLORREF face = p.high_contrast ? p.accent : p.dark ? p.control_hover : RGB(0xFF, 0xFF, 0xFF);
        // Nothing to choose yet (no modules): the chosen half as greyed as the other.
        if (disabled) face = p.high_contrast ? p.base : blend(face, track, 0.6);
        draw_control_body(dc, seg, radius, face, p.control_stroke, disabled ? p.control_stroke : p.control_stroke_bottom);
      } else if (hot || pressed) {
        RECT seg = deflate(body, t.px(3));
        fill_round(dc, seg, radius, pressed ? p.row_selected : p.row_hover);
      }
      COLORREF tc = disabled ? p.text_disabled : p.high_contrast && checked ? p.on_accent : checked ? p.text : p.text2;
      draw_text(dc, text, body, checked ? t.fonts.body_strong : t.fonts.body, tc,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | prefix);
      if (focus) draw_focus_ring(dc, cr, cr_radius + fm, p, s);
      break;
    }
    case ButtonRole::subtle: {
      RECT body = deflate(cr, fm);
      if (!disabled && (hot || pressed)) fill_round(dc, body, radius, pressed ? p.row_selected : p.row_hover);
      COLORREF tc = disabled ? p.text_disabled : pressed ? blend(p.accent_text, bg, 0.25) : p.accent_text;
      wchar_t glyph = (wchar_t)(INT_PTR)GetPropW(h, kGlyphProp);
      RECT tr = body;
      if (glyph) {
        const int gw = t.px(16) + t.px(8);
        const int x = body.left + t.px(8);
        RECT gr{x, body.top, x + t.px(16), body.bottom};
        // Left-aligned: the glyph's edge is the link's edge (layouts line it up).
        draw_text(dc, std::wstring(1, glyph), gr, t.fonts.icons_small, tc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        tr.left = x + gw;
        draw_text(dc, text, tr, t.fonts.body, tc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | prefix);
      } else {
        draw_text(dc, text, tr, t.fonts.body, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | prefix);
      }
      if (focus) draw_focus_ring(dc, cr, radius + fm, p, s);
      break;
    }
    case ButtonRole::card: {
      paint_card_button(dc, h, cr, t, text, disabled, hot, pressed, focus, prefix);
      break;
    }
    case ButtonRole::accent:
    case ButtonRole::standard: {
      RECT body = deflate(cr, fm);
      COLORREF tc;
      if (role == ButtonRole::accent) {
        COLORREF f = disabled ? p.accent_disabled : pressed ? p.accent_pressed : hot ? p.accent_hover : p.accent;
        COLORREF edge = p.high_contrast ? p.control_stroke : blend(f, RGB(0, 0, 0), p.dark ? 0.12 : 0.25);
        draw_control_body(dc, body, radius, f, f, disabled || pressed ? f : edge);
        tc = disabled ? (p.dark ? p.text_disabled : RGB(0xFF, 0xFF, 0xFF)) : pressed ? blend(p.on_accent, f, 0.2) : p.on_accent;
      } else {
        COLORREF f = disabled ? p.control_disabled : pressed ? p.control_pressed : hot ? p.control_hover : p.control;
        draw_control_body(dc, body, radius, f, p.control_stroke, pressed || disabled ? p.control_stroke : p.control_stroke_bottom);
        tc = disabled ? p.text_disabled : pressed ? p.text2 : p.text;
      }
      wchar_t glyph = (wchar_t)(INT_PTR)GetPropW(h, kGlyphProp);
      if (glyph) {
        SIZE ts = measure_text(dc, text, t.fonts.body, DT_SINGLELINE | prefix);
        int gw = t.px(16) + t.px(8), total = gw + ts.cx;
        int x = (body.left + body.right - total) / 2;
        RECT gr{x, body.top, x + t.px(16), body.bottom};
        draw_text(dc, std::wstring(1, glyph), gr, t.fonts.icons, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        RECT tr{x + gw, body.top, body.right, body.bottom};
        draw_text(dc, text, tr, t.fonts.body, tc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | prefix);
      } else {
        draw_text(dc, text, body, t.fonts.body, tc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | prefix);
      }
      if (focus) draw_focus_ring(dc, cr, radius + fm, p, s);
      break;
    }
  }
  return CDRF_SKIPDEFAULT;
}

// ---- trackbar ---------------------------------------------------------------------------

LRESULT custom_draw_trackbar(const Theme& t, NMCUSTOMDRAW* cd) {
  if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
  HWND h = cd->hdr.hwndFrom;
  DWORD_PTR ref = 0;
  GetWindowSubclass(h, track_proc, 1, &ref);
  auto* st = reinterpret_cast<TrackState*>(ref);
  const Palette& p = t.pal;
  const float s = t.dpi / 96.0f;
  RECT cr{};
  GetClientRect(h, &cr);
  Offscreen off(cd->hdc, cr);
  HDC dc = off.dc;
  fill_rect(dc, cr, surface_color(h, p));
  const bool disabled = !IsWindowEnabled(h);
  TrackGeom g = track_geom(h, t);
  const int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
  const float tx = g.x_of(pos);
  const float rail_h = 4 * s;
  RECT rail{(int)std::lround(g.x0 - g.r * 0.5f), (int)std::lround(g.cy - rail_h / 2),
            (int)std::lround(g.x1 + g.r * 0.5f), (int)std::lround(g.cy + rail_h / 2)};
  fill_round(dc, rail, rail_h / 2, disabled ? p.text_disabled : p.rail);
  RECT done = rail;
  done.right = (LONG)std::lround(tx);
  if (done.right > done.left) fill_round(dc, done, rail_h / 2, disabled ? p.accent_disabled : p.accent);
  // Tick marks under the rail for a slider over labelled stops.
  if (st && st->stops > 1 && st->stops <= 24) {
    for (int i = 0; i < st->stops; ++i) {
      float x = g.x0 + (g.x1 - g.x0) * i / (st->stops - 1);
      RECT tick{(int)std::lround(x - 0.5f * s), (int)std::lround(g.cy + g.r + 1 * s), (int)std::lround(x + 0.5f * s) + 1,
                (int)std::lround(g.cy + g.r + 4 * s)};
      fill_rect(dc, tick, p.text3);
    }
  }
  // Thumb: a ring and an accent core that grows under the pointer.
  fill_ellipse(dc, tx, g.cy, g.r, p.thumb_stroke);
  fill_ellipse(dc, tx, g.cy, g.r - (float)t.hairline(), p.thumb);
  const bool pressed = st && st->dragging, hot = st && (st->thumb_hot || st->dragging);
  float core = (pressed ? 4.5f : hot ? 7.0f : 6.0f) * s;
  fill_ellipse(dc, tx, g.cy, core, disabled ? p.accent_disabled : p.accent);
  if (focused_window() == h && keyboard_cues(h)) draw_focus_ring(dc, cr, 4 * s + focus_margin(t.dpi), p, s);
  return CDRF_SKIPDEFAULT;
}

void subclass_trackbar(HWND tb, const Theme* t, int stops) {
  auto* st = new TrackState;
  st->t = t;
  st->stops = stops;
  SetWindowSubclass(tb, track_proc, 1, reinterpret_cast<DWORD_PTR>(st));
}

void init_slider(HWND tb, const Theme* t, const SliderSpec& spec) {
  if (!tb) return;
  const int mn = std::min(spec.min, spec.max), mx = std::max(spec.min, spec.max);
  SendMessageW(tb, TBM_SETRANGEMIN, FALSE, mn);
  SendMessageW(tb, TBM_SETRANGEMAX, FALSE, mx);
  SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
  SendMessageW(tb, TBM_SETPAGESIZE, 0, std::max(1, spec.page));
  SendMessageW(tb, TBM_SETPOS, TRUE, std::clamp(spec.pos, mn, mx));
  DWORD_PTR ref = 0;
  if (!GetWindowSubclass(tb, track_proc, 1, &ref)) {
    subclass_trackbar(tb, t, 0);
    GetWindowSubclass(tb, track_proc, 1, &ref);
  }
  if (auto* st = reinterpret_cast<TrackState*>(ref)) st->up_is_more = true;
  if (!spec.name.empty()) set_accessible_name(tb, spec.name);
  InvalidateRect(tb, nullptr, FALSE);
}

// ---- combo box ------------------------------------------------------------------------

void subclass_combo(HWND combo, const Theme* t) {
  auto* st = new ComboState;
  st->t = t;
  SetPropW(combo, L"adw.combo", st);
  SetWindowSubclass(combo, combo_proc, 2, reinterpret_cast<DWORD_PTR>(st));
  SendMessageW(combo, CB_SETMINVISIBLE, 10, 0);
}

void measure_combo_item(const Theme& t, MEASUREITEMSTRUCT* mi) {
  mi->itemHeight = (UINT)t.px(mi->itemID == (UINT)-1 ? 26 : 32);
}

void size_combo(HWND combo, const Theme& t, int face_h) {
  SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, std::max(8, face_h - 6));
  RECT wr{};
  GetWindowRect(combo, &wr);
  int actual = wr.bottom - wr.top;
  if (actual != face_h) SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, std::max(8, face_h - 6 + (face_h - actual)));
  SendMessageW(combo, CB_SETITEMHEIGHT, 0, t.px(32));
  COMBOBOXINFO cbi{sizeof(cbi)};
  if (GetComboBoxInfo(combo, &cbi) && cbi.hwndList) {
    theme_native_control(cbi.hwndList, t.pal.dark && !t.pal.high_contrast);
    // The open list is a popup of its own: small rounded corners and a
    // border in the theme's colour, like a Windows 11 menu.
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(cbi.hwndList, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = t.pal.high_contrast ? (COLORREF)0xFFFFFFFF : t.pal.control_stroke;
    DwmSetWindowAttribute(cbi.hwndList, DWMWA_BORDER_COLOR, &border, sizeof(border));
  }
}

namespace {

// While a combo's list is dropped, hot-tracking moves the list selection with the
// mouse, so CB_GETCURSEL names whatever row is under the pointer. Worse, the row being
// left is repainted before the selection moves on, so judging "current" from the live
// selection leaves the accent bar on every row the mouse crosses. The accent bar marks
// the value that was committed when the list opened; it is stashed on the combo on the
// first paint of a drop (the list paints fully before any hover) and dropped again once
// the list is closed.
constexpr wchar_t kCommittedProp[] = L"adw.combo.committed";

int committed_index(HWND combo) {
  const int live = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
    RemovePropW(combo, kCommittedProp);
    return live;
  }
  if (HANDLE h = GetPropW(combo, kCommittedProp)) return (int)(intptr_t)h - 2;
  SetPropW(combo, kCommittedProp, (HANDLE)(intptr_t)(live + 2));  // +2: CB_ERR (-1) stays non-null
  return live;
}

}  // namespace

void draw_combo_item(const Theme& t, const DRAWITEMSTRUCT* di) {
  if (di->itemState & ODS_COMBOBOXEDIT) {
    // The face repaints when the list closes; forget the stashed value so the next drop
    // captures the (possibly new) committed selection.
    if (!SendMessageW(di->hwndItem, CB_GETDROPPEDSTATE, 0, 0)) RemovePropW(di->hwndItem, kCommittedProp);
    paint_combo_face(di->hwndItem, di->hDC, t);
    return;
  }
  const Palette& p = t.pal;
  RECT r = di->rcItem;
  HDC dc = di->hDC;
  fill_rect(dc, r, p.card);
  if ((int)di->itemID < 0) return;
  const bool hi = (di->itemState & ODS_SELECTED) != 0;
  const bool current = committed_index(di->hwndItem) == (int)di->itemID;
  RECT row{r.left + t.px(4), r.top + t.px(2), r.right - t.px(4), r.bottom - t.px(2)};
  if (hi) fill_round(dc, row, t.pxf(4), p.high_contrast ? p.accent : current ? p.row_selected_hover : p.row_hover);
  else if (current) fill_round(dc, row, t.pxf(4), p.row_selected);
  if (current && !p.high_contrast) {
    int ph = t.px(16), pw = std::max(2, t.px(3));
    int cy = (row.top + row.bottom) / 2;
    RECT pill{row.left, cy - ph / 2, row.left + pw, cy + ph / 2};
    fill_round(dc, pill, pw / 2.0f, p.accent);
  }
  int n = (int)SendMessageW(di->hwndItem, CB_GETLBTEXTLEN, di->itemID, 0);
  std::wstring text(std::max(0, n) + 1, L'\0');
  if (n > 0) SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text.data());
  text.resize(std::max(0, n));
  RECT tr{row.left + t.px(12), row.top, row.right - t.px(8), row.bottom};
  draw_text(dc, text, tr, t.fonts.body, p.high_contrast && hi ? p.on_accent : p.text,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

HBRUSH combo_list_brush(const Theme& t) { return t.card_brush; }

// ---- badge --------------------------------------------------------------------------------

void draw_badge(HDC dc, const RECT& r, const std::wstring& text, BadgeKind kind, const Theme& t, COLORREF bg) {
  const Palette& p = t.pal;
  fill_rect(dc, r, bg);
  if (text.empty()) return;
  COLORREF fill = kind == BadgeKind::caution ? p.caution : kind == BadgeKind::critical ? p.critical : p.chip;
  COLORREF ink = kind == BadgeKind::caution ? p.caution_text : kind == BadgeKind::critical ? p.critical_text : p.chip_text;
  SIZE ts = measure_text(dc, text, t.fonts.caption);
  const int h = r.bottom - r.top, pad = t.px(8);
  RECT pill{r.left, r.top, std::min<LONG>(r.right, r.left + ts.cx + 2 * pad), r.bottom};
  fill_round(dc, pill, h / 2.0f, fill);
  if (p.high_contrast) stroke_round(dc, pill, h / 2.0f, p.text);
  RECT tr{pill.left + pad, pill.top, pill.right - pad, pill.bottom};
  draw_text(dc, text, tr, t.fonts.caption, ink, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// ---- the critique's fix round ------------------------------------------------------------

bool set_accessible_name(HWND h, const std::wstring& name) {
  if (!h) return false;
  // CLSID_AccPropServices, IID_IAccPropServices, PROPID_ACC_NAME (oleacc.h; spelled out so no
  // import library has to provide them).
  static const GUID kClsid = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
  static const GUID kIid = {0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
  static const GUID kName = {0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  IAccPropServices* props = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kIid, reinterpret_cast<void**>(&props))) && props) {
    ok = SUCCEEDED(props->SetHwndPropStr(h, (DWORD)OBJID_CLIENT, (DWORD)CHILDID_SELF, kName, name.c_str()));
    props->Release();
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

} // namespace adw::ui
