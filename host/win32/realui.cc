#include "win32/realui.hh"

#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <unordered_map>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win32/config_script.hh"
#include "win32/display.hh"
#include "win32/gdi_objects.hh"
#include "win32/modules.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

// ---- internal records ------------------------------------------------------------------------------

struct RealUi::Dialog {
  HWND hwnd = nullptr;
  uint32_t proc = 0, param = 0, hinst = 0;
  bool modal = true, ended = false, destroyed = false;
  uint32_t result = 0;
};

struct RealUi::Wrap {
  HDC real = nullptr;
  uint32_t gdc = 0, gbmp = 0;
  int w = 0, h = 0;
  HWND hwnd = nullptr;
  bool release = false, paint = false;
  PAINTSTRUCT ps{};
};

// A host message being handled by a guest procedure (for DefWindowProcA).
struct RealUi::Current {
  HWND hwnd;
  UINT msg;
  WPARAM wp;
  LPARAM lp;
};

namespace {

// The settings window disables itself for a whole configure run
// (INTERACTION.md §6.3), so a modal dialog, message box or file dialog it
// owns would find its owner already disabled: the modal loop then neither
// disables nor re-enables it, and the owner is still disabled when the dialog
// goes. Lent: enabled just before the modal UI, which disables it again at
// once and re-enables it as it ends, as for any owner. (It stays enabled for
// the few moments the host has left; the settings window re-enables itself
// when the host exits anyway. adhostwin_main.cc hands it the foreground.)
struct OwnerLend {
  explicit OwnerLend(HWND o) {
    if (o && IsWindow(o) && !IsWindowEnabled(o)) EnableWindow(o, TRUE);
  }
  OwnerLend(const OwnerLend&) = delete;
  OwnerLend& operator=(const OwnerLend&) = delete;
};


RealUi* g_current = nullptr;  // the configure-mode instance (one runtime per process)

std::wstring from_ansi(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(1252, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string to_ansi(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(1252, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(1252, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring upper_w(std::wstring s) {
  for (wchar_t& c : s) c = wchar_t(towupper(c));
  return s;
}

std::wstring class_of(HWND h) {
  wchar_t buf[128] = {};
  GetClassNameW(h, buf, 128);
  return upper_w(buf);
}

// Messages that carry no pointer and no handle: passed as numbers both ways.
bool plain_message(UINT m) {
  switch (m) {
    case WM_NULL: case WM_MOVE: case WM_SIZE: case WM_ENABLE: case WM_SETREDRAW: case WM_CLOSE:
    case WM_QUIT: case WM_SHOWWINDOW: case WM_CANCELMODE: case WM_GETFONT: case WM_PAINT:
    case WM_DESTROY: case WM_NCDESTROY: case WM_SYSCOMMAND: case WM_TIMER: case WM_SYSCOLORCHANGE:
    case WM_QUERYNEWPALETTE: case WM_GETDLGCODE: case WM_CHAR: case WM_KEYDOWN: case WM_KEYUP:
    case WM_DEADCHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR: case WM_SYSDEADCHAR:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_ACTIVATEAPP: case WM_NCACTIVATE: case WM_ENTERIDLE: case WM_ENDSESSION:
    case WM_QUERYENDSESSION:
      return true;
    default:
      return false;
  }
}

// ---- dialog templates ----

// The size of a DLGTEMPLATE(EX) read through `at` (a byte accessor that
// returns -1 past the end), and the offsets of its style and of every
// string-named item class. 0 when malformed.
struct TemplateInfo {
  size_t size = 0;
  bool ex = false;
  size_t style_off = 0;
  std::vector<std::pair<size_t, std::wstring>> classes;  // (offset of the class string, name)
  std::wstring dialog_class;
};

TemplateInfo parse_template(const std::function<int(size_t)>& at, size_t limit) {
  TemplateInfo t;
  bool bad = false;
  auto u16 = [&](size_t o) -> uint32_t {
    int a = at(o), b = at(o + 1);
    if (a < 0 || b < 0 || o + 2 > limit) {
      bad = true;
      return 0;
    }
    return uint32_t(a) | uint32_t(b) << 8;
  };
  auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
  // sz_Or_Ord: 0, 0xFFFF + ordinal, or a NUL-terminated UTF-16 string.
  auto skip_sz = [&](size_t o, std::wstring* s) -> size_t {
    uint32_t w = u16(o);
    if (w == 0) return o + 2;
    if (w == 0xFFFF) return o + 4;
    size_t p = o;
    for (int n = 0; n < 4096 && !bad; n++, p += 2) {
      uint32_t c = u16(p);
      if (!c) return p + 2;
      if (s) s->push_back(wchar_t(c));
    }
    bad = true;
    return p;
  };
  t.ex = u16(0) == 1 && u16(2) == 0xFFFF;
  size_t o;
  uint32_t style, count;
  if (t.ex) {
    t.style_off = 12;
    style = u32(12);
    count = u16(16);
    o = 26;
  } else {
    t.style_off = 0;
    style = u32(0);
    count = u16(8);
    o = 18;
  }
  o = skip_sz(o, nullptr);             // menu
  size_t cls_off = o;
  o = skip_sz(o, &t.dialog_class);     // class
  (void)cls_off;
  o = skip_sz(o, nullptr);             // title
  if (style & (DS_SETFONT | DS_SHELLFONT)) {
    o += t.ex ? 6 : 2;                 // point size (+ weight, italic, charset)
    o = skip_sz(o, nullptr);           // face
  }
  for (uint32_t i = 0; i < count && !bad; i++) {
    o = (o + 3) & ~size_t(3);
    o += t.ex ? 24 : 18;
    std::wstring cls;
    size_t co = o;
    o = skip_sz(o, &cls);
    if (!cls.empty()) t.classes.push_back({co, cls});
    o = skip_sz(o, nullptr);           // text
    uint32_t extra = u16(o);
    o += 2 + extra;
  }
  if (bad || o > limit) return TemplateInfo{};
  t.size = o;
  return t;
}

bool standard_class(const std::wstring& upper_name) {
  static const wchar_t* kStd[] = {L"BUTTON", L"EDIT", L"STATIC", L"LISTBOX", L"SCROLLBAR", L"COMBOBOX",
                                  L"COMBOLBOX", L"#32770", L"MDICLIENT"};
  for (const wchar_t* s : kStd)
    if (upper_name == s) return true;
  return false;
}

}  // namespace

// ---- lifetime --------------------------------------------------------------------------------------

RealUi::RealUi(Runtime& rt) : rt_(rt) {}

RealUi::~RealUi() {
  if (g_current == this) g_current = nullptr;
  for (auto& [h, d] : dialogs_by_hwnd_)
    if (IsWindow(h)) DestroyWindow(h);
  for (auto& [g, w] : wraps_) {
    if (w->paint) EndPaint(w->hwnd, &w->ps);
    else if (w->release) ReleaseDC(w->hwnd, w->real);
  }
  for (auto& [c, b] : brushes_) DeleteObject(b);
}

RealUi& real_ui(Runtime& rt) { return rt.state<RealUi>(); }

void RealUi::enable(HWND owner, ConfigScript* script) {
  enabled_ = true;
  owner_ = owner && IsWindow(owner) ? owner : nullptr;
  if (owner && !owner_) log("configure: --owner 0x%llX is not a window; the dialogs are unowned", (unsigned long long)uintptr_t(owner));
  script_ = script;
  g_current = this;
  // DirectSound present, but its device unavailable (0x8878000A,
  // DSERR_ALLOCATED: the value ADXPL510 tests at 0x42731d, AUDIO.md §2.1),
  // as on a 1996 PC whose sound card is busy: the Music Choice buttons
  // (POINTS, SLOWBURN, SWIRLING) look for DirectSoundCreate and, when
  // dsound.dll is missing altogether, show an error instead of their dialog.
  // Configure runs never enable the audio engine, so dsound.cc's real
  // DirectSound is never registered here.
  rt_.shims().add("DSOUND.DLL", "DirectSoundCreate", Conv::stdcall_, 12, [](Call& c) {
    if (c.arg(1)) c.mem().write_u32l(c.arg(1), 0);
    c.ret(0x8878000A);  // DSERR_ALLOCATED
  });
  // Dialog-unit layouts designed for 96 DPI, scaled by the system with crisp text.
  using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  if (auto f = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
    f(reinterpret_cast<DPI_AWARENESS_CONTEXT>(intptr_t(-5)));  // DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED
}

bool RealUi::timed_out() const { return script_ && script_->timed_out(); }

// ---- guest handles -----------------------------------------------------------------------------------

uint32_t RealUi::guest(HWND h) {
  if (!h) return 0;
  auto it = by_hwnd_.find(h);
  if (it != by_hwnd_.end()) return it->second;
  if (handles_.size() >= kCount) return 0;
  uint32_t g = kFirst + kStep * uint32_t(handles_.size());
  handles_.push_back(h);
  by_hwnd_[h] = g;
  return g;
}

HWND RealUi::real(uint32_t g) const {
  if (!is_real(g)) return nullptr;
  size_t i = (g - kFirst) / kStep;
  if (i >= handles_.size()) return nullptr;
  HWND h = handles_[i];
  return IsWindow(h) ? h : nullptr;
}

HWND RealUi::owner_for(uint32_t parent_g) {
  if (HWND h = real(parent_g)) return h;
  return owner_;  // an emulated window (the saver window) or none: the settings window owns it
}

RealUi::Dialog* RealUi::dialog_of(HWND h) {
  auto it = dialogs_by_hwnd_.find(h);
  if (it != dialogs_by_hwnd_.end()) return it->second.get();
  if (creating_ && !creating_->hwnd) {
    creating_->hwnd = h;
    dialogs_by_hwnd_[h] = creating_;
    guest(h);
    return creating_.get();
  }
  return nullptr;
}

void RealUi::fail_with(std::exception_ptr e) {
  if (!error_) error_ = e;
  for (auto& [h, d] : dialogs_by_hwnd_) d->ended = true;
}

void RealUi::rethrow_pending() {
  if (!error_) return;
  std::exception_ptr e = error_;
  // Only the outermost loop clears it, so every level unwinds.
  bool outermost = std::none_of(dialogs_by_hwnd_.begin(), dialogs_by_hwnd_.end(),
                                [](const auto& kv) { return kv.second->modal && !kv.second->destroyed; });
  if (outermost) error_ = nullptr;
  std::rethrow_exception(e);
}

uint32_t RealUi::guest_alloc(uint32_t size) {
  uint32_t p = rt_.heap().alloc(size, true);
  if (!p) throw GuestError(GuestError::Kind::fatal, "no guest memory to marshal a message");
  return p;
}

void RealUi::guest_free(uint32_t addr) { rt_.heap().free(addr); }

// ---- calling the guest -------------------------------------------------------------------------------

uint32_t RealUi::call_proc(uint32_t proc, HWND h, UINT msg, uint32_t wp, uint32_t lp) {
  return rt_.call_guest(proc, {guest(h), uint32_t(msg), wp, lp}, Conv::stdcall_);
}

COLORREF RealUi::true_color(COLORREF c, uint32_t gdc) {
  Display* d = rt_.display();
  if (!d) return c & 0xFFFFFF;
  GdiTable& gdi = rt_.state<GdiTable>();
  LogicalPalette* pal = gdc ? gdi.dc_palette(gdc) : nullptr;
  return d->hardware_color(d->map_index(c, pal));
}

HBRUSH RealUi::real_brush(uint32_t gbrush, uint32_t gdc) {
  GdiTable& gdi = rt_.state<GdiTable>();
  GdiObject* o = gdi.get(gbrush, GdiType::brush);
  if (!o) return nullptr;
  if (o->stock) return static_cast<HBRUSH>(o->host);
  if (o->brush_style == BS_NULL) return static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  COLORREF rgb = true_color(o->color, gdc);
  auto it = brushes_.find(rgb);
  if (it != brushes_.end()) return it->second;
  HBRUSH b = CreateSolidBrush(rgb);
  brushes_[rgb] = b;
  return b;
}

// The guest's view of a real DC: an 8-bit key-table surface w×h whose
// (cap_x, cap_y, cap_w, cap_h) part starts as the real pixels.
uint32_t RealUi::wrap_dc(HDC real, int w, int h, int cap_x, int cap_y, int cap_w, int cap_h) {
  GdiTable& gdi = rt_.state<GdiTable>();
  w = std::max(w, 1);
  h = std::max(h, 1);
  HDC mdc = CreateCompatibleDC(nullptr);
  if (!mdc) throw GuestError(GuestError::Kind::fatal, "CreateCompatibleDC failed for a window DC");
  uint32_t gdc = gdi.add_dc(mdc, true);
  if (!gdc) {
    DeleteDC(mdc);  // the table did not take it
    throw GuestError(GuestError::Kind::fatal, "no room for a window DC");
  }
  uint32_t gb = gdi.create_device_bitmap(w, h, 8);
  GdiObject* bo = gb ? gdi.get(gb, GdiType::bitmap) : nullptr;
  if (!bo) {
    if (gb) gdi.destroy(gb);
    gdi.destroy(gdc);  // owned: deletes mdc
    throw GuestError(GuestError::Kind::fatal, "no room for a window DC's bitmap");
  }
  SelectObject(mdc, bo->host);
  gdi.get(gdc, GdiType::dc)->dc.bitmap = gb;
  if (real) {
    // What the window's DC was set up with: its font, colours and background mode.
    if (HGDIOBJ f = GetCurrentObject(real, OBJ_FONT)) SelectObject(mdc, f);
    GdiDc& dc = gdi.get(gdc, GdiType::dc)->dc;
    dc.text_color = GetTextColor(real);
    dc.bk_color = GetBkColor(real);
    ::SetTextColor(mdc, gdi.key_color(gdc, dc.text_color));
    ::SetBkColor(mdc, gdi.key_color(gdc, dc.bk_color));
    ::SetBkMode(mdc, GetBkMode(real));
  }
  Display* disp = rt_.display();
  cap_w = std::min(cap_w, w - cap_x);
  cap_h = std::min(cap_h, h - cap_y);
  if (real && disp && cap_w > 0 && cap_h > 0 && cap_x >= 0 && cap_y >= 0) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cap_w;
    bi.bmiHeader.biHeight = -cap_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC tmp = CreateCompatibleDC(nullptr);
    if (dib && tmp) {
      HGDIOBJ old = SelectObject(tmp, dib);
      BitBlt(tmp, 0, 0, cap_w, cap_h, real, cap_x, cap_y, SRCCOPY);
      GdiFlush();
      std::unordered_map<uint32_t, uint8_t> cache;
      const uint32_t* px = static_cast<const uint32_t*>(bits);
      bo = gdi.get(gb, GdiType::bitmap);
      for (int y = 0; y < cap_h; y++) {
        uint8_t* row = bo->bmp.row(cap_y + y);
        for (int x = 0; x < cap_w; x++) {
          uint32_t v = px[size_t(y) * size_t(cap_w) + size_t(x)] & 0xFFFFFF;
          auto it = cache.find(v);
          if (it == cache.end())
            it = cache.emplace(v, uint8_t(disp->nearest_index(RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF), false))).first;
          row[cap_x + x] = it->second;
        }
      }
      SelectObject(tmp, old);
    }
    if (tmp) DeleteDC(tmp);
    if (dib) DeleteObject(dib);
  }
  auto wr = std::make_unique<Wrap>();
  wr->real = real;
  wr->gdc = gdc;
  wr->gbmp = gb;
  wr->w = w;
  wr->h = h;
  wraps_[gdc] = std::move(wr);
  return gdc;
}

void RealUi::unwrap_dc(uint32_t gdc, bool blit_back, const RECT* area) {
  auto it = wraps_.find(gdc);
  if (it == wraps_.end()) return;
  std::unique_ptr<Wrap> wr = std::move(it->second);
  wraps_.erase(it);
  GdiTable& gdi = rt_.state<GdiTable>();
  Display* disp = rt_.display();
  GdiFlush();
  GdiObject* bo = gdi.get(wr->gbmp, GdiType::bitmap);
  if (blit_back && wr->real && disp && bo && bo->bmp.host_bits) {
    RECT r = area ? *area : RECT{0, 0, wr->w, wr->h};
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, wr->w);
    r.bottom = std::min<LONG>(r.bottom, wr->h);
    int cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw > 0 && ch > 0) {
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = cw;
      bi.bmiHeader.biHeight = -ch;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      void* bits = nullptr;
      HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
      HDC tmp = CreateCompatibleDC(nullptr);
      if (dib && tmp) {
        uint32_t* px = static_cast<uint32_t*>(bits);
        for (int y = 0; y < ch; y++) {
          const uint8_t* row = bo->bmp.row(r.top + y);
          for (int x = 0; x < cw; x++) {
            COLORREF c = disp->hardware_color(row[r.left + x]);
            px[size_t(y) * size_t(cw) + size_t(x)] = uint32_t(GetRValue(c)) << 16 | uint32_t(GetGValue(c)) << 8 | GetBValue(c);
          }
        }
        HGDIOBJ old = SelectObject(tmp, dib);
        BitBlt(wr->real, r.left, r.top, cw, ch, tmp, 0, 0, SRCCOPY);
        SelectObject(tmp, old);
      }
      if (tmp) DeleteDC(tmp);
      if (dib) DeleteObject(dib);
    }
  }
  gdi.destroy(wr->gdc);
  gdi.destroy(wr->gbmp);
}

// ---- host → guest ------------------------------------------------------------------------------------

LRESULT RealUi::deliver(uint32_t proc, HWND h, UINT msg, WPARAM wp, LPARAM lp, bool dialog, bool* forwarded) {
  *forwarded = true;
  auto& mem = rt_.mem();
  uint32_t gw = uint32_t(wp), gl = uint32_t(lp);
  current_.push_back(std::make_unique<Current>(Current{h, msg, wp, lp}));
  struct Pop {
    std::vector<std::unique_ptr<Current>>& v;
    ~Pop() { v.pop_back(); }
  } pop{current_};
  auto call = [&](uint32_t w, uint32_t l) {
    if (tracing("realui")) trace("realui", "message 0x%04X (0x%X, 0x%X) to guest procedure 0x%X for 0x%X", msg, w, l, proc,
                                 guest(h));
    return LRESULT(int32_t(call_proc(proc, h, msg, w, l)));
  };
  switch (msg) {
    case WM_COMMAND:
      return call(gw, guest(reinterpret_cast<HWND>(lp)));
    case WM_NOTIFY: {
      const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
      uint32_t s = guest_alloc(12);
      mem.write_u32l(s, guest(n->hwndFrom));
      mem.write_u32l(s + 4, uint32_t(n->idFrom));
      mem.write_u32l(s + 8, n->code);
      LRESULT r = call(gw, s);
      guest_free(s);
      return r;
    }
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
      RECT rc = di->rcItem, client = rc;
      if (di->CtlType != ODT_MENU) GetClientRect(di->hwndItem, &client);
      uint32_t gdc = wrap_dc(di->hDC, std::max(client.right, rc.right), std::max(client.bottom, rc.bottom), rc.left,
                             rc.top, rc.right - rc.left, rc.bottom - rc.top);
      uint32_t s = guest_alloc(48);
      mem.write_u32l(s + 0, di->CtlType);
      mem.write_u32l(s + 4, di->CtlID);
      mem.write_u32l(s + 8, di->itemID);
      mem.write_u32l(s + 12, di->itemAction);
      mem.write_u32l(s + 16, di->itemState);
      mem.write_u32l(s + 20, di->CtlType == ODT_MENU ? 0 : guest(di->hwndItem));
      mem.write_u32l(s + 24, gdc);
      write_pod(mem, s + 28, rc);
      mem.write_u32l(s + 44, uint32_t(di->itemData));
      LRESULT r = 0;
      try {
        r = call(gw, s);
      } catch (...) {
        guest_free(s);
        unwrap_dc(gdc, false, nullptr);
        throw;
      }
      guest_free(s);
      unwrap_dc(gdc, true, &rc);
      return r;
    }
    case WM_MEASUREITEM: {
      MEASUREITEMSTRUCT* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
      uint32_t s = guest_alloc(24);
      uint32_t f[6] = {mi->CtlType, mi->CtlID, mi->itemID, mi->itemWidth, mi->itemHeight, uint32_t(mi->itemData)};
      mem.memcpy(s, f, sizeof(f));
      LRESULT r = call(gw, s);
      mi->itemWidth = mem.read_u32l(s + 12);
      mi->itemHeight = mem.read_u32l(s + 16);
      guest_free(s);
      return r;
    }
    case WM_COMPAREITEM: {
      const COMPAREITEMSTRUCT* ci = reinterpret_cast<const COMPAREITEMSTRUCT*>(lp);
      uint32_t s = guest_alloc(32);
      uint32_t f[8] = {ci->CtlType, ci->CtlID, guest(ci->hwndItem), ci->itemID1, uint32_t(ci->itemData1),
                       ci->itemID2, uint32_t(ci->itemData2), ci->dwLocaleId};
      mem.memcpy(s, f, sizeof(f));
      LRESULT r = call(gw, s);
      guest_free(s);
      return r;
    }
    case WM_DELETEITEM: {
      const DELETEITEMSTRUCT* di = reinterpret_cast<const DELETEITEMSTRUCT*>(lp);
      uint32_t s = guest_alloc(20);
      uint32_t f[5] = {di->CtlType, di->CtlID, di->itemID, guest(di->hwndItem), uint32_t(di->itemData)};
      mem.memcpy(s, f, sizeof(f));
      LRESULT r = call(gw, s);
      guest_free(s);
      return r;
    }
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      GdiTable& gdi = rt_.state<GdiTable>();
      uint32_t gdc = wrap_dc(dc, 1, 1, 0, 0, 0, 0);
      GdiObject* o = gdi.get(gdc, GdiType::dc);
      COLORREF t0 = o->dc.text_color, b0 = o->dc.bk_color;
      HDC mdc = static_cast<HDC>(o->host);
      int m0 = GetBkMode(mdc);
      LRESULT r = 0;
      try {
        r = call(gdc, guest(reinterpret_cast<HWND>(lp)));
      } catch (...) {
        unwrap_dc(gdc, false, nullptr);
        throw;
      }
      o = gdi.get(gdc, GdiType::dc);
      if (o) {
        if (o->dc.text_color != t0) ::SetTextColor(dc, true_color(o->dc.text_color, gdc));
        if (o->dc.bk_color != b0) ::SetBkColor(dc, true_color(o->dc.bk_color, gdc));
        int m = GetBkMode(static_cast<HDC>(o->host));
        if (m != m0) ::SetBkMode(dc, m);
      }
      HBRUSH b = r ? real_brush(uint32_t(r), gdc) : nullptr;
      unwrap_dc(gdc, false, nullptr);
      return reinterpret_cast<LRESULT>(b);
    }
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wp);
      RECT client{};
      GetClientRect(h, &client);
      uint32_t gdc = wrap_dc(dc, client.right, client.bottom, 0, 0, client.right, client.bottom);
      LRESULT r = 0;
      try {
        r = call(gdc, 0);
      } catch (...) {
        unwrap_dc(gdc, false, nullptr);
        throw;
      }
      unwrap_dc(gdc, r != 0, &client);
      return r;
    }
    case WM_NCCREATE:
    case WM_CREATE: {
      if (dialog) break;
      const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
      std::string name = cs->lpszName ? to_ansi(cs->lpszName) : std::string();
      std::string cls = (uintptr_t(cs->lpszClass) >> 16) ? to_ansi(cs->lpszClass) : std::string();
      uint32_t s = guest_alloc(48 + uint32_t(name.size()) + uint32_t(cls.size()) + 2);
      uint32_t np = s + 48, cp = np + uint32_t(name.size()) + 1;
      write_cstr(mem, np, name, name.size() + 1);
      write_cstr(mem, cp, cls, cls.size() + 1);
      uint32_t f[12] = {creating_param_, uint32_t(rt_.modules().exe_handle()), uint32_t(uintptr_t(cs->hMenu)),
                        guest(cs->hwndParent), uint32_t(cs->cy), uint32_t(cs->cx), uint32_t(cs->y), uint32_t(cs->x),
                        uint32_t(cs->style), np, cp, cs->dwExStyle};
      mem.memcpy(s, f, sizeof(f));
      LRESULT r = call(gw, s);
      guest_free(s);
      return r;
    }
    case WM_SETFONT:
      gw = wp ? rt_.state<GdiTable>().wrap_host(reinterpret_cast<HGDIOBJ>(wp), GdiType::font) : 0;
      break;
    case WM_ACTIVATE:
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_CHARTOITEM:
    case WM_VKEYTOITEM:
      gl = guest(reinterpret_cast<HWND>(lp));
      break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_CONTEXTMENU:
    case WM_PALETTECHANGED:
    case WM_PALETTEISCHANGING:
      gw = guest(reinterpret_cast<HWND>(wp));
      break;
    case WM_PARENTNOTIFY:
      if (LOWORD(wp) == WM_CREATE || LOWORD(wp) == WM_DESTROY) gl = guest(reinterpret_cast<HWND>(lp));
      break;
    case WM_ENTERIDLE:
      gl = guest(reinterpret_cast<HWND>(lp));
      break;
    case WM_TIMER:
      gl = 0;
      break;
    case WM_GETDLGCODE:
      gl = 0;
      break;
    default:
      if (plain_message(msg)) break;
      if ((msg >= WM_USER && msg < 0x8000) || (msg >= WM_APP && msg < 0xC000)) break;  // the guest's own
      *forwarded = false;
      return 0;
  }
  return call(gw, gl);
}

INT_PTR CALLBACK RealUi::dialog_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  RealUi* self = g_current;
  if (!self) return FALSE;
  Dialog* d = self->dialog_of(h);
  if (!d) return FALSE;
  INT_PTR r = FALSE;
  if (!self->error_) {
    try {
      if (msg == WM_INITDIALOG) {
        r = INT_PTR(int32_t(self->call_proc(d->proc, h, msg, self->guest(reinterpret_cast<HWND>(wp)), d->param)));
      } else if (msg == WM_TIMER && self->timer_procs_.count({h, wp})) {
        uint32_t p = self->timer_procs_[{h, wp}];
        self->rt_.call_guest(p, {self->guest(h), uint32_t(WM_TIMER), uint32_t(wp), self->rt_.clock().tick_count()});
        r = TRUE;
      } else {
        bool fwd = false;
        LRESULT v = self->deliver(d->proc, h, msg, wp, lp, true, &fwd);
        r = fwd ? INT_PTR(v) : FALSE;
      }
    } catch (...) {
      self->fail_with(std::current_exception());
      r = FALSE;
    }
  }
  if (msg == WM_NCDESTROY) {
    d->ended = d->destroyed = true;
    auto keep = self->dialogs_by_hwnd_[h];  // the run_dialog frame holds the other reference
    self->dialogs_by_hwnd_.erase(h);
  }
  return r;
}

LRESULT CALLBACK RealUi::class_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  RealUi* self = g_current;
  if (!self || self->error_) return DefWindowProcW(h, msg, wp, lp);
  uint32_t proc = 0;
  auto wit = self->window_procs_.find(h);
  if (wit != self->window_procs_.end()) {
    proc = wit->second;
  } else {
    auto cit = self->class_procs_.find(class_of(h));
    if (cit != self->class_procs_.end()) proc = cit->second;
  }
  if (!proc) return DefWindowProcW(h, msg, wp, lp);
  if (msg == WM_NCCREATE) self->guest(h);
  try {
    if (msg == WM_TIMER && self->timer_procs_.count({h, wp})) {
      uint32_t p = self->timer_procs_[{h, wp}];
      self->rt_.call_guest(p, {self->guest(h), uint32_t(WM_TIMER), uint32_t(wp), self->rt_.clock().tick_count()});
      return 0;
    }
    bool fwd = false;
    LRESULT r = self->deliver(proc, h, msg, wp, lp, false, &fwd);
    if (!fwd) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_NCDESTROY) self->window_procs_.erase(h);
    return r;
  } catch (...) {
    self->fail_with(std::current_exception());
    return DefWindowProcW(h, msg, wp, lp);
  }
}

uint32_t RealUi::def_window_proc(uint32_t hwnd_g, uint32_t msg, uint32_t wp, uint32_t lp) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  // The host message the guest is handling: its original parameters.
  for (auto it = current_.rbegin(); it != current_.rend(); ++it) {
    if ((*it)->hwnd == h && (*it)->msg == msg) return uint32_t(DefWindowProcW(h, msg, (*it)->wp, (*it)->lp));
  }
  if (plain_message(msg) || msg >= WM_USER) return uint32_t(DefWindowProcW(h, msg, wp, lp));
  return uint32_t(DefWindowProcW(h, msg, wp, 0));
}

// ---- classes and windows the guest makes -------------------------------------------------------------

bool RealUi::ensure_class(const std::string& name) {
  std::wstring w = upper_w(from_ansi(name));
  if (class_procs_.count(w)) return true;
  GuestClassInfo info;
  if (!guest_class(rt_, name, &info) || !info.wndproc) return false;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = info.style & ~uint32_t(CS_GLOBALCLASS);
  wc.lpfnWndProc = &RealUi::class_proc;
  wc.cbClsExtra = std::max(info.cls_extra, 0);
  wc.cbWndExtra = std::max(info.wnd_extra, 0);
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = info.background && info.background <= COLOR_INFOBK + 1
                         ? reinterpret_cast<HBRUSH>(uintptr_t(info.background))
                         : (info.background ? real_brush(info.background, 0) : nullptr);
  std::wstring wname = from_ansi(name);
  wc.lpszClassName = wname.c_str();
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    log("configure: cannot register a real class for \"%s\" (error %lu)", name.c_str(), GetLastError());
    return false;
  }
  class_procs_[w] = info.wndproc;
  trace("user", "real class \"%s\" forwards to guest procedure 0x%X", name.c_str(), info.wndproc);
  return true;
}

uint32_t RealUi::create_window(uint32_t exstyle, const std::string& cls, const std::string& title, uint32_t style,
                               int x, int y, int cx, int cy, uint32_t parent, uint32_t id, uint32_t hinst,
                               uint32_t param) {
  (void)hinst;
  std::wstring ucls = upper_w(from_ansi(cls));
  if (!standard_class(ucls) && !ensure_class(cls)) return 0;
  HWND p = real(parent);
  creating_param_ = param;
  std::wstring wcls = from_ansi(cls), wtitle = from_ansi(title);
  HWND h = CreateWindowExW(exstyle, wcls.c_str(), wtitle.c_str(), style, x, y, cx, cy, p ? p : owner_,
                           reinterpret_cast<HMENU>(uintptr_t(id)), GetModuleHandleW(nullptr), nullptr);
  creating_param_ = 0;
  rethrow_pending();
  if (!h) {
    log("configure: CreateWindowEx(\"%s\") failed (error %lu)", cls.c_str(), GetLastError());
    return 0;
  }
  return guest(h);
}

// ---- dialogs -----------------------------------------------------------------------------------------

std::vector<uint8_t> RealUi::template_at(uint32_t addr) const {
  auto& mem = rt_.mem();
  auto at = [&](size_t i) -> int {
    uint32_t a = addr + uint32_t(i);
    if (!mem.exists(a, 1)) return -1;
    return mem.read_u8(a);
  };
  TemplateInfo t = parse_template(at, 1 << 20);
  if (!t.size) return {};
  std::string bytes = mem.read(addr, uint32_t(t.size));
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<uint8_t> RealUi::template_resource(uint32_t hinst, uint32_t name_arg) const {
  loader::ResId name = name_arg < 0x10000 ? loader::ResId::of(uint16_t(name_arg))
                                          : loader::ResId::of(read_cstr(rt_.mem(), name_arg));
  if (name.is_string && !name.str.empty() && name.str[0] == '#')
    name = loader::ResId::of(uint16_t(strtoul(name.str.c_str() + 1, nullptr, 10)));
  Runtime& rt = const_cast<Runtime&>(rt_);
  uint32_t hmod = hinst ? hinst : rt.modules().exe_handle();
  const loader::pe::Resource* res = find_module_resource(rt, hmod, loader::ResId::of(uint16_t(5)), name);  // RT_DIALOG
  Module* m = rt.modules().by_handle(hmod);
  if (!res || !m || !m->image) return {};
  std::string_view data = m->image->resource_data(*res);
  return std::vector<uint8_t>(data.begin(), data.end());
}

uint32_t RealUi::run_dialog(std::vector<uint8_t> tmpl, uint32_t hinst, uint32_t parent, uint32_t proc,
                            uint32_t param, bool modal) {
  const uint32_t fail = modal ? 0xFFFFFFFF : 0;
  auto at = [&](size_t i) -> int { return i < tmpl.size() ? tmpl[i] : -1; };
  TemplateInfo t = parse_template(at, tmpl.size());
  if (!t.size) {
    log("configure: the dialog template is malformed");
    return fail;
  }
  tmpl.resize((t.size + 3) & ~size_t(3), 0);
  bool hidden = script_ && script_->hidden();
  uint32_t style;
  memcpy(&style, tmpl.data() + t.style_off, 4);
  if (hidden) style &= ~uint32_t(WS_VISIBLE);
  memcpy(tmpl.data() + t.style_off, &style, 4);
  for (const auto& [off, cls] : t.classes) {
    std::wstring u = upper_w(cls);
    if (!standard_class(u)) ensure_class(to_ansi(cls));
  }
  if (!t.dialog_class.empty()) ensure_class(to_ansi(t.dialog_class));
  HWND owner = owner_for(parent);
  auto d = std::make_shared<Dialog>();
  d->proc = proc;
  d->param = param;
  d->hinst = hinst;
  d->modal = modal;
  creating_ = d;
  HWND h = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()), owner,
                                      &RealUi::dialog_proc, 0);
  DWORD err = GetLastError();
  creating_.reset();
  if (!h) {
    rethrow_pending();
    log("configure: CreateDialogIndirectParam failed (error %lu)", err);
    return fail;
  }
  dialogs_++;
  trace("user", "real dialog 0x%X (guest procedure 0x%X)%s", guest(h), proc, hidden ? " (hidden)" : "");
  if (error_) {
    DestroyWindow(h);
    rethrow_pending();
  }
  if (script_) script_->attach(h, [this](HWND x) { force_end(x); });
  if (!modal) {
    if (!hidden && (style & WS_VISIBLE)) ShowWindow(h, SW_SHOW);
    return guest(h);
  }
  // The modal loop (DialogBox's): the owner is disabled while it runs.
  OwnerLend lend(owner);
  bool owner_was_enabled = owner && IsWindowEnabled(owner);
  if (owner_was_enabled) EnableWindow(owner, FALSE);
  if (!hidden && !d->ended) {
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
  }
  MSG m;
  while (!d->ended && !error_ && IsWindow(h)) {
    BOOL got = GetMessageW(&m, nullptr, 0, 0);
    if (got <= 0) {
      if (got == 0) PostQuitMessage(int(m.wParam));
      break;
    }
    if (!IsDialogMessageW(h, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (owner_was_enabled) EnableWindow(owner, TRUE);
  uint32_t result = d->result;
  if (IsWindow(h)) {
    ShowWindow(h, SW_HIDE);
    DestroyWindow(h);
  }
  rethrow_pending();
  trace("user", "real dialog ended: %d", int32_t(result));
  return result;
}

bool RealUi::end_dialog(uint32_t hdlg, uint32_t result) {
  HWND h = real(hdlg);
  if (!h) return false;
  auto it = dialogs_by_hwnd_.find(h);
  if (it == dialogs_by_hwnd_.end()) return EndDialog(h, INT_PTR(int32_t(result))) != 0;  // e.g. a common dialog's
  it->second->ended = true;
  it->second->result = result;
  if (!it->second->modal) ShowWindow(h, SW_HIDE);
  return true;
}

void RealUi::force_end(HWND h) {
  auto it = dialogs_by_hwnd_.find(h);
  if (it != dialogs_by_hwnd_.end()) {
    it->second->ended = true;
    it->second->result = IDCANCEL;
    if (!it->second->modal) DestroyWindow(h);
    return;
  }
  EndDialog(h, IDCANCEL);
}

void RealUi::pump_modeless() {
  auto open = [&] {
    for (auto& [h, d] : dialogs_by_hwnd_)
      if (!d->modal && !d->ended && IsWindow(h)) return true;
    return false;
  };
  MSG m;
  while (open() && !error_) {
    BOOL got = GetMessageW(&m, nullptr, 0, 0);
    if (got <= 0) break;
    bool done = false;
    for (auto& [h, d] : dialogs_by_hwnd_)
      if (!d->modal && IsDialogMessageW(h, &m)) {
        done = true;
        break;
      }
    if (!done) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  for (auto it = dialogs_by_hwnd_.begin(); it != dialogs_by_hwnd_.end();) {
    HWND h = it->first;
    ++it;
    if (IsWindow(h)) DestroyWindow(h);
  }
  rethrow_pending();
}

// ---- guest → real messages ---------------------------------------------------------------------------

std::string RealUi::window_text(uint32_t hwnd_g) {
  HWND h = real(hwnd_g);
  if (!h) return {};
  int n = GetWindowTextLengthW(h);
  std::wstring w(size_t(n) + 1, L'\0');
  n = GetWindowTextW(h, w.data(), n + 1);
  w.resize(size_t(std::max(n, 0)));
  return to_ansi(w);
}

bool RealUi::set_window_text(uint32_t hwnd_g, const std::string& text) {
  HWND h = real(hwnd_g);
  return h && SetWindowTextW(h, from_ansi(text).c_str());
}

uint32_t RealUi::send(uint32_t hwnd_g, uint32_t msg, uint32_t wp, uint32_t lp) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  auto& mem = rt_.mem();
  GdiTable& gdi = rt_.state<GdiTable>();
  auto S = [&](WPARAM w, LPARAM l) { return SendMessageW(h, msg, w, l); };
  auto str = [&](uint32_t p) { return from_ansi(read_cstr(mem, p)); };
  auto ret = [](LRESULT r) { return uint32_t(r); };
  // Text into a guest buffer of `cap` bytes; returns the characters copied.
  auto put = [&](uint32_t buf, const std::wstring& w, size_t cap) -> uint32_t {
    if (!buf || !cap) return 0;
    return uint32_t(write_cstr(mem, buf, to_ansi(w), cap));
  };
  switch (msg) {
    case WM_SETTEXT: {
      std::wstring w = str(lp);
      return ret(S(0, reinterpret_cast<LPARAM>(w.c_str())));
    }
    case WM_GETTEXT:
      return put(lp, from_ansi(window_text(hwnd_g)), wp);
    case WM_GETTEXTLENGTH:
      return uint32_t(window_text(hwnd_g).size());
    case WM_SETFONT:
      return ret(S(reinterpret_cast<WPARAM>(wp ? gdi.host(wp) : nullptr), lp));
    case WM_GETFONT: {
      HGDIOBJ f = reinterpret_cast<HGDIOBJ>(S(0, 0));
      return f ? gdi.wrap_host(f, GdiType::font) : 0;
    }
    case WM_COMMAND:
      return ret(S(wp, reinterpret_cast<LPARAM>(real(lp))));
    case WM_NEXTDLGCTL:
      return ret(S(LOWORD(lp) ? reinterpret_cast<WPARAM>(real(wp)) : wp, lp));
    default:
      break;
  }
  std::wstring cls = class_of(h);
  LONG style = GetWindowLongW(h, GWL_STYLE);
  if (cls == L"EDIT") {
    switch (msg) {
      case EM_GETSEL: {
        DWORD a = 0, b = 0;
        LRESULT r = S(reinterpret_cast<WPARAM>(&a), reinterpret_cast<LPARAM>(&b));
        if (wp) mem.write_u32l(wp, a);
        if (lp) mem.write_u32l(lp, b);
        return ret(r);
      }
      case EM_GETRECT: {
        RECT rc{};
        S(0, reinterpret_cast<LPARAM>(&rc));
        if (lp) write_pod(mem, lp, rc);
        return 0;
      }
      case EM_SETRECT:
      case EM_SETRECTNP: {
        RECT rc = lp ? read_pod<RECT>(mem, lp) : RECT{};
        return ret(S(wp, lp ? reinterpret_cast<LPARAM>(&rc) : 0));
      }
      case EM_REPLACESEL: {
        std::wstring w = str(lp);
        return ret(S(wp, reinterpret_cast<LPARAM>(w.c_str())));
      }
      case EM_GETLINE: {
        uint32_t cap = lp ? mem.read_u16l(lp) : 0;
        if (!cap) return 0;
        std::wstring buf(std::max<size_t>(cap, 2) + 1, L'\0');
        buf[0] = wchar_t(cap);
        LRESULT n = S(wp, reinterpret_cast<LPARAM>(buf.data()));
        std::string a = to_ansi(std::wstring_view(buf.data(), size_t(std::max<LRESULT>(n, 0))));
        a.resize(std::min<size_t>(a.size(), cap));
        if (!a.empty()) mem.memcpy(lp, a.data(), a.size());
        return uint32_t(a.size());
      }
      case EM_SETTABSTOPS: {
        std::vector<int> t(wp);
        for (uint32_t i = 0; i < wp; i++) t[i] = int32_t(mem.read_u32l(lp + 4 * i));
        return ret(S(wp, wp ? reinterpret_cast<LPARAM>(t.data()) : 0));
      }
      case EM_SETHANDLE: case EM_GETHANDLE: case EM_SETWORDBREAKPROC: case EM_GETWORDBREAKPROC:
        return 0;
      default:
        if (msg >= EM_GETSEL && msg <= EM_CHARFROMPOS) return ret(S(wp, lp));
        break;
    }
  }
  bool is_lb = cls == L"LISTBOX" || cls == L"COMBOLBOX", is_cb = cls == L"COMBOBOX";
  bool lb_strings = !(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || (style & LBS_HASSTRINGS);
  bool cb_strings = !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (style & CBS_HASSTRINGS);
  // LB_DIR / CB_DIR: the guest's file system, not the host's.
  auto dir = [&](uint32_t attrs, uint32_t spec_p, UINT add) -> uint32_t {
    Vfs& vfs = rt_.vfs();
    std::string spec = vfs.full_path(read_cstr(mem, spec_p));
    size_t s = spec.find_last_of('\\');
    std::string d = s <= 2 ? spec.substr(0, 3) : spec.substr(0, s), pat = spec.substr(s + 1);
    LRESULT last = LB_ERR;
    bool exclusive = attrs & DDL_EXCLUSIVE;
    for (const Vfs::DirEntry& e : vfs.list(d, pat.empty() ? "*.*" : pat)) {
      bool is_dir = e.attributes & FILE_ATTRIBUTE_DIRECTORY;
      if (e.name == ".") continue;
      if (is_dir && !(attrs & DDL_DIRECTORY)) continue;
      if (!is_dir && exclusive) continue;
      std::wstring w = from_ansi(is_dir ? "[" + e.name + "]" : e.name);
      last = SendMessageW(h, add, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    if (attrs & DDL_DRIVES) {
      std::wstring w = L"[-c-]";
      last = SendMessageW(h, add, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    return uint32_t(last);
  };
  if (is_lb) {
    switch (msg) {
      case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING: case LB_FINDSTRINGEXACT:
      case LB_SELECTSTRING: {
        if (!lb_strings) return ret(S(wp, lp));
        std::wstring w = str(lp);
        return ret(S(wp, reinterpret_cast<LPARAM>(w.c_str())));
      }
      case LB_ADDFILE: {
        std::wstring w = str(lp);
        return ret(S(wp, reinterpret_cast<LPARAM>(w.c_str())));
      }
      case LB_GETTEXT: {
        if (!lb_strings) {
          LRESULT v = SendMessageW(h, LB_GETITEMDATA, wp, 0);
          if (lp) mem.write_u32l(lp, uint32_t(v));
          return 4;
        }
        LRESULT n = SendMessageW(h, LB_GETTEXTLEN, wp, 0);
        if (n == LB_ERR) return uint32_t(LB_ERR);
        std::wstring w(size_t(n) + 1, L'\0');
        n = S(wp, reinterpret_cast<LPARAM>(w.data()));
        w.resize(size_t(std::max<LRESULT>(n, 0)));
        std::string a = to_ansi(w);
        if (lp) mem.memcpy(lp, a.c_str(), a.size() + 1);
        return uint32_t(a.size());
      }
      case LB_GETTEXTLEN: {
        if (!lb_strings) return ret(S(wp, lp));
        LRESULT n = S(wp, 0);
        if (n == LB_ERR) return uint32_t(LB_ERR);
        std::wstring w(size_t(n) + 1, L'\0');
        n = SendMessageW(h, LB_GETTEXT, wp, reinterpret_cast<LPARAM>(w.data()));
        w.resize(size_t(std::max<LRESULT>(n, 0)));
        return uint32_t(to_ansi(w).size());
      }
      case LB_GETSELITEMS: {
        std::vector<int> v(wp);
        LRESULT n = S(wp, wp ? reinterpret_cast<LPARAM>(v.data()) : 0);
        for (LRESULT i = 0; i < n && i < LRESULT(wp); i++) mem.write_u32l(lp + 4 * uint32_t(i), uint32_t(v[size_t(i)]));
        return ret(n);
      }
      case LB_SETTABSTOPS: {
        std::vector<int> t(wp);
        for (uint32_t i = 0; i < wp; i++) t[i] = int32_t(mem.read_u32l(lp + 4 * i));
        return ret(S(wp, wp ? reinterpret_cast<LPARAM>(t.data()) : 0));
      }
      case LB_GETITEMRECT: {
        RECT rc{};
        LRESULT r = S(wp, reinterpret_cast<LPARAM>(&rc));
        if (lp) write_pod(mem, lp, rc);
        return ret(r);
      }
      case LB_DIR:
        return dir(wp, lp, LB_ADDSTRING);
      default:
        if (msg >= LB_ADDSTRING && msg <= LB_MSGMAX) return ret(S(wp, lp));
        break;
    }
  }
  if (is_cb) {
    switch (msg) {
      case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING: case CB_FINDSTRINGEXACT:
      case CB_SELECTSTRING: {
        if (!cb_strings) return ret(S(wp, lp));
        std::wstring w = str(lp);
        return ret(S(wp, reinterpret_cast<LPARAM>(w.c_str())));
      }
      case CB_GETLBTEXT: {
        if (!cb_strings) {
          LRESULT v = SendMessageW(h, CB_GETITEMDATA, wp, 0);
          if (lp) mem.write_u32l(lp, uint32_t(v));
          return 4;
        }
        LRESULT n = SendMessageW(h, CB_GETLBTEXTLEN, wp, 0);
        if (n == CB_ERR) return uint32_t(CB_ERR);
        std::wstring w(size_t(n) + 1, L'\0');
        n = S(wp, reinterpret_cast<LPARAM>(w.data()));
        w.resize(size_t(std::max<LRESULT>(n, 0)));
        std::string a = to_ansi(w);
        if (lp) mem.memcpy(lp, a.c_str(), a.size() + 1);
        return uint32_t(a.size());
      }
      case CB_GETLBTEXTLEN: {
        if (!cb_strings) return ret(S(wp, lp));
        LRESULT n = S(wp, 0);
        if (n == CB_ERR) return uint32_t(CB_ERR);
        std::wstring w(size_t(n) + 1, L'\0');
        n = SendMessageW(h, CB_GETLBTEXT, wp, reinterpret_cast<LPARAM>(w.data()));
        w.resize(size_t(std::max<LRESULT>(n, 0)));
        return uint32_t(to_ansi(w).size());
      }
      case CB_GETEDITSEL: {
        DWORD a = 0, b = 0;
        LRESULT r = S(reinterpret_cast<WPARAM>(&a), reinterpret_cast<LPARAM>(&b));
        if (wp) mem.write_u32l(wp, a);
        if (lp) mem.write_u32l(lp, b);
        return ret(r);
      }
      case CB_GETDROPPEDCONTROLRECT: {
        RECT rc{};
        LRESULT r = S(0, reinterpret_cast<LPARAM>(&rc));
        if (lp) write_pod(mem, lp, rc);
        return ret(r);
      }
      case CB_DIR:
        return dir(wp, lp, CB_ADDSTRING);
      default:
        if (msg >= CB_GETEDITSEL && msg <= CB_MSGMAX) return ret(S(wp, lp));
        break;
    }
  }
  if (cls == L"BUTTON" && msg >= BM_GETCHECK && msg <= BM_SETDONTCLICK) {
    if (msg == BM_SETIMAGE || msg == BM_GETIMAGE) return 0;  // guest bitmaps are not converted
    return ret(S(wp, lp));
  }
  if (cls == L"STATIC" && msg >= STM_SETICON && msg <= STM_MSGMAX) return 0;
  if (plain_message(msg) || msg >= WM_USER) return ret(S(wp, lp));
  trace("user", "SendMessage(0x%X) to a real %s window: not marshalled; lParam passed as 0", msg,
        narrow(cls).c_str());
  return ret(S(wp, 0));
}

bool RealUi::post(uint32_t hwnd_g, uint32_t msg, uint32_t wp, uint32_t lp) {
  HWND h = real(hwnd_g);
  if (!h) return false;
  LPARAM l = msg == WM_COMMAND ? reinterpret_cast<LPARAM>(real(lp)) : LPARAM(int32_t(lp));
  return PostMessageW(h, msg, wp, l) != 0;
}

// ---- window state ------------------------------------------------------------------------------------

uint32_t RealUi::get_window_long(uint32_t hwnd_g, int32_t index) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  auto dit = dialogs_by_hwnd_.find(h);
  Dialog* d = dit == dialogs_by_hwnd_.end() ? nullptr : dit->second.get();
  switch (index) {
    case -4: {  // GWL_WNDPROC
      auto w = window_procs_.find(h);
      if (w != window_procs_.end()) return w->second;
      auto c = class_procs_.find(class_of(h));
      return c != class_procs_.end() ? c->second : 0;
    }
    case -6: {  // GWL_HINSTANCE: the dialog's module (or its parent dialog's)
      for (HWND p = h; p; p = GetParent(p)) {
        auto it = dialogs_by_hwnd_.find(p);
        if (it != dialogs_by_hwnd_.end()) return it->second->hinst;
      }
      return rt_.modules().exe_handle();
    }
    case -8:  // GWL_HWNDPARENT
      return guest(reinterpret_cast<HWND>(GetWindowLongPtrW(h, GWLP_HWNDPARENT)));
    case -21:  // GWL_USERDATA
      return uint32_t(GetWindowLongPtrW(h, GWLP_USERDATA));
    case -12: case -16: case -20:
      return uint32_t(GetWindowLongW(h, index));
    default:
      break;
  }
  if (d) {
    if (index == 0) return uint32_t(GetWindowLongPtrW(h, DWLP_MSGRESULT));
    if (index == 4) return d->proc;
    if (index == 8) return uint32_t(GetWindowLongPtrW(h, DWLP_USER));
  }
  if (index >= 0) return uint32_t(GetWindowLongW(h, index));
  return 0;
}

uint32_t RealUi::set_window_long(uint32_t hwnd_g, int32_t index, uint32_t value) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  auto dit = dialogs_by_hwnd_.find(h);
  Dialog* d = dit == dialogs_by_hwnd_.end() ? nullptr : dit->second.get();
  switch (index) {
    case -4: {  // GWL_WNDPROC: only between guest procedures
      auto w = window_procs_.find(h);
      uint32_t old = 0;
      if (w != window_procs_.end()) {
        old = w->second;
      } else {
        auto c = class_procs_.find(class_of(h));
        if (c == class_procs_.end()) {
          log("configure: a module subclassed a real %s control; not supported", narrow(class_of(h)).c_str());
          return 0;
        }
        old = c->second;
      }
      window_procs_[h] = value;
      return old;
    }
    case -6:
      return 0;
    case -8:
      return guest(reinterpret_cast<HWND>(SetWindowLongPtrW(h, GWLP_HWNDPARENT, LONG_PTR(real(value)))));
    case -21:
      return uint32_t(SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(value)));
    case -12: case -16: case -20:
      return uint32_t(SetWindowLongW(h, index, LONG(value)));
    default:
      break;
  }
  if (d) {
    if (index == 0) return uint32_t(SetWindowLongPtrW(h, DWLP_MSGRESULT, LONG_PTR(int32_t(value))));
    if (index == 4) {
      uint32_t old = d->proc;
      d->proc = value;
      return old;
    }
    if (index == 8) return uint32_t(SetWindowLongPtrW(h, DWLP_USER, LONG_PTR(value)));
  }
  if (index >= 0) return uint32_t(SetWindowLongW(h, index, LONG(value)));
  return 0;
}

bool RealUi::show_window(uint32_t hwnd_g, int cmd) {
  HWND h = real(hwnd_g);
  if (!h) return false;
  bool top = !(GetWindowLongW(h, GWL_STYLE) & WS_CHILD);
  if (script_ && script_->hidden() && top && cmd != SW_HIDE) {
    // Hidden runs never show a top-level window.
    return IsWindowVisible(h) != 0;
  }
  return ShowWindow(h, cmd) != 0;
}

bool RealUi::set_window_pos(uint32_t hwnd_g, uint32_t after, int x, int y, int cx, int cy, uint32_t flags) {
  HWND h = real(hwnd_g);
  if (!h) return false;
  HWND a;
  switch (int32_t(after)) {
    case 0: case 1: case -1: case -2: a = reinterpret_cast<HWND>(intptr_t(int32_t(after))); break;
    default: a = real(after); break;
  }
  bool top = !(GetWindowLongW(h, GWL_STYLE) & WS_CHILD);
  if (script_ && script_->hidden() && top) flags = (flags | SWP_NOMOVE | SWP_NOACTIVATE) & ~uint32_t(SWP_SHOWWINDOW);
  return SetWindowPos(h, a, x, y, cx, cy, flags) != 0;
}

// ---- DCs -----------------------------------------------------------------------------------------------

uint32_t RealUi::get_dc(uint32_t hwnd_g) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  HDC dc = GetDC(h);
  if (!dc) return 0;
  RECT rc{};
  GetClientRect(h, &rc);
  uint32_t gdc = wrap_dc(dc, rc.right, rc.bottom, 0, 0, rc.right, rc.bottom);
  wraps_[gdc]->hwnd = h;
  wraps_[gdc]->release = true;
  return gdc;
}

bool RealUi::release_dc(uint32_t hwnd_g, uint32_t gdc) {
  auto it = wraps_.find(gdc);
  if (it == wraps_.end() || !it->second->release) return false;
  HWND h = it->second->hwnd;
  HDC dc = it->second->real;
  (void)hwnd_g;
  unwrap_dc(gdc, true, nullptr);
  ReleaseDC(h, dc);
  return true;
}

uint32_t RealUi::begin_paint(uint32_t hwnd_g, uint32_t ps_guest) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(h, &ps);
  if (!dc) return 0;
  RECT rc{};
  GetClientRect(h, &rc);
  uint32_t gdc = wrap_dc(dc, rc.right, rc.bottom, 0, 0, rc.right, rc.bottom);
  Wrap& w = *wraps_[gdc];
  w.hwnd = h;
  w.paint = true;
  w.ps = ps;
  g32::PAINTSTRUCT gps{};
  gps.hdc = gdc;
  gps.fErase = ps.fErase;
  gps.rcPaint = ps.rcPaint;
  write_pod(rt_.mem(), ps_guest, gps);
  return gdc;
}

bool RealUi::end_paint(uint32_t hwnd_g, uint32_t ps_guest) {
  (void)hwnd_g;
  uint32_t gdc = rt_.mem().read_u32l(ps_guest);
  auto it = wraps_.find(gdc);
  if (it == wraps_.end() || !it->second->paint) return false;
  HWND h = it->second->hwnd;
  PAINTSTRUCT ps = it->second->ps;
  unwrap_dc(gdc, true, nullptr);
  EndPaint(h, &ps);
  return true;
}

// ---- timers --------------------------------------------------------------------------------------------

uint32_t RealUi::set_timer(uint32_t hwnd_g, uint32_t id, uint32_t ms, uint32_t proc) {
  HWND h = real(hwnd_g);
  if (!h) return 0;
  UINT_PTR r = SetTimer(h, id, ms, nullptr);
  if (!r) return 0;
  if (proc) timer_procs_[{h, UINT_PTR(id)}] = proc;
  else timer_procs_.erase({h, UINT_PTR(id)});
  return uint32_t(r);
}

bool RealUi::kill_timer(uint32_t hwnd_g, uint32_t id) {
  HWND h = real(hwnd_g);
  if (!h) return false;
  timer_procs_.erase({h, UINT_PTR(id)});
  return KillTimer(h, id) != 0;
}

// ---- message boxes and file dialogs --------------------------------------------------------------------

uint32_t RealUi::message_box(uint32_t owner_g, const std::string& text, const std::string& caption, uint32_t type) {
  message_boxes_++;
  std::wstring wt = from_ansi(text), wc = from_ansi(caption);
  if (script_) {
    if (auto a = script_->message_box(wt)) return uint32_t(*a);
  }
  HWND owner = owner_for(owner_g);
  OwnerLend lend(owner);
  int r = MessageBoxW(owner, wt.c_str(), wc.c_str(), type & ~uint32_t(MB_SYSTEMMODAL | MB_SERVICE_NOTIFICATION));
  trace("user", "MessageBox \"%s\" -> %d", text.c_str(), r);
  return uint32_t(r);
}

UINT_PTR CALLBACK RealUi::file_hook_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  RealUi* self = g_current;
  if (!self || !self->file_hook_ || self->error_) return 0;
  try {
    if (msg == WM_INITDIALOG) return UINT_PTR(self->call_proc(self->file_hook_, h, msg, self->guest(reinterpret_cast<HWND>(wp)), self->file_ofn_));
    bool fwd = false;
    LRESULT r = self->deliver(self->file_hook_, h, msg, wp, lp, true, &fwd);
    return fwd ? UINT_PTR(r) : 0;
  } catch (...) {
    self->fail_with(std::current_exception());
    return 0;
  }
}

bool RealUi::file_dialog(uint32_t ofn, bool save) {
  file_dialogs_++;
  auto& mem = rt_.mem();
  Vfs& vfs = rt_.vfs();
  uint32_t size = mem.read_u32l(ofn);
  if (size < 76) {
    log("configure: OPENFILENAME of %u bytes is not supported", size);
    return false;
  }
  uint32_t f[19];
  mem.memcpy(f, ofn, sizeof(f));
  uint32_t owner_g = f[1], filter_p = f[3], index = f[6], file_p = f[7], max_file = f[8], title_p = f[9],
           max_title = f[10], idir_p = f[11], dtitle_p = f[12], flags = f[13], defext_p = f[15], hook = f[17];
  // Writes the chosen guest path back, as GetOpenFileNameA does.
  auto finish = [&](const std::string& guest_path, DWORD out_flags, DWORD out_index) -> bool {
    if (!file_p || guest_path.size() + 1 > max_file) {
      if (file_p && max_file >= 2) mem.write_u16l(file_p, uint16_t(guest_path.size() + 1));
      log("configure: the chosen path does not fit the module's buffer (%zu > %u)", guest_path.size() + 1, max_file);
      return false;
    }
    write_cstr(mem, file_p, guest_path, max_file);
    size_t slash = guest_path.find_last_of('\\');
    size_t name = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = guest_path.find_last_of('.');
    uint16_t ext = dot != std::string::npos && dot > name ? uint16_t(dot + 1) : 0;
    mem.write_u16l(ofn + 56, uint16_t(name));
    mem.write_u16l(ofn + 58, ext);
    if (title_p && max_title) write_cstr(mem, title_p, guest_path.substr(name), max_title);
    mem.write_u32l(ofn + 24, out_index);
    uint32_t kept = flags & ~uint32_t(OFN_READONLY | OFN_EXTENSIONDIFFERENT);
    mem.write_u32l(ofn + 52, kept | (out_flags & (OFN_READONLY | OFN_EXTENSIONDIFFERENT)));
    trace("user", "file dialog -> \"%s\"", guest_path.c_str());
    return true;
  };
  if (script_) {
    if (auto a = script_->file_dialog()) {
      if (a->empty()) return false;
      std::string g = vfs.host_to_guest(narrow(*a));
      if (g.empty()) {
        log("configure: the scripted file \"%s\" has no guest path", narrow(*a).c_str());
        return false;
      }
      return finish(g, 0, index);
    }
  }
  // The filter: pairs of NUL-terminated strings, ending with an empty one.
  std::wstring filter;
  if (filter_p) {
    uint32_t p = filter_p;
    for (int n = 0; n < 64; n++) {
      std::string s = read_cstr(mem, p);
      if (s.empty()) break;
      filter += from_ansi(s);
      filter.push_back(L'\0');
      p += uint32_t(s.size()) + 1;
    }
    filter.push_back(L'\0');
  }
  std::wstring file_buf(std::max<uint32_t>(max_file, MAX_PATH) + 1, L'\0');
  if (file_p) {
    std::string init = read_cstr(mem, file_p);
    std::string host = init.find_first_of("\\:") != std::string::npos ? vfs.to_host(init) : init;
    std::wstring w = from_ansi(host.empty() ? init : host);
    if (w.size() < file_buf.size()) std::copy(w.begin(), w.end(), file_buf.begin());
  }
  std::wstring idir, dtitle = from_ansi(read_cstr(mem, dtitle_p)), defext = from_ansi(read_cstr(mem, defext_p));
  if (idir_p) {
    std::string g = read_cstr(mem, idir_p);
    if (!g.empty()) idir = from_ansi(vfs.to_host(g));
  }
  std::wstring title_buf(std::max<uint32_t>(max_title, 1) + 1, L'\0');
  OPENFILENAMEW o{};
  o.lStructSize = sizeof(o);
  o.hwndOwner = owner_for(owner_g);
  o.lpstrFilter = filter_p ? filter.c_str() : nullptr;
  o.nFilterIndex = index;
  o.lpstrFile = file_buf.data();
  o.nMaxFile = DWORD(file_buf.size() - 1);
  o.lpstrFileTitle = title_p ? title_buf.data() : nullptr;
  o.nMaxFileTitle = DWORD(title_buf.size() - 1);
  o.lpstrInitialDir = idir.empty() ? nullptr : idir.c_str();
  o.lpstrTitle = dtitle_p ? dtitle.c_str() : nullptr;
  o.lpstrDefExt = defext_p ? defext.c_str() : nullptr;
  o.Flags = (flags & ~uint32_t(OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE | OFN_ALLOWMULTISELECT)) |
            OFN_NOCHANGEDIR;
  if ((flags & OFN_ENABLEHOOK) && hook) {
    o.Flags |= OFN_ENABLEHOOK;
    o.lpfnHook = &RealUi::file_hook_proc;
  }
  if (flags & OFN_ENABLETEMPLATE) log("configure: the module's file dialog template is not used");
  if (script_ && script_->hidden()) {
    log("configure: a hidden run cannot show a file dialog; cancelled");
    return false;
  }
  uint32_t saved_hook = file_hook_, saved_ofn = file_ofn_;
  file_hook_ = (flags & OFN_ENABLEHOOK) ? hook : 0;
  file_ofn_ = ofn;
  BOOL ok = FALSE;
  {
    OwnerLend lend(o.hwndOwner);
    ok = save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
  }
  file_hook_ = saved_hook;
  file_ofn_ = saved_ofn;
  rethrow_pending();
  if (!ok) {
    DWORD e = CommDlgExtendedError();
    if (e) log("configure: the file dialog failed (CommDlgExtendedError %lu)", e);
    return false;
  }
  std::string g = vfs.host_to_guest(narrow(o.lpstrFile));
  if (g.empty()) {
    OwnerLend lend(o.hwndOwner);
    MessageBoxW(o.hwndOwner, L"After Dark cannot use a file from this location (a network path). Choose a file on a drive.",
                L"After Dark", MB_OK | MB_ICONINFORMATION);
    return false;
  }
  return finish(g, o.Flags, o.nFilterIndex);
}

}  // namespace adw::win32
