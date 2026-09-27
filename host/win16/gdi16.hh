// Win16 GDI objects on real GDI (the adapter host/win32's GdiTable would be,
// were it not tied to a Win32 Runtime — see the handoff note in README.md).
//
// The model is the Win32 lane's (win32/display.hh "The key table"): every
// 8-bit surface real GDI sees — the screen, device bitmaps, scratch surfaces —
// is an 8bpp DIB section carrying the fixed key colour table (entry i =
// RGB(i,i,i)), so a blit between them copies indices exactly, a guest
// COLORREF reaches real GDI only as the key colour of the hardware index a
// Win95 palette device would have drawn (Display::map_color through the DC's
// selected palette), and palette changes touch nothing but the hardware
// palette. Device-independent bitmaps are translated to hardware indices on
// the way in (and back to colours on the way out).
//
// Handles are 16-bit values 0x2000–0x7FFC (step 4) naming entries of this
// table; the real object behind one never reaches the guest. Screen DCs each
// own a DIB section aliasing the display's bits in emulated memory (so any
// number of GetDC DCs can draw at once), memory DCs are real memory DCs.
#pragma once

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

#include "win16/runtime16.hh"
#include "win32/display.hh"

namespace adw::win16 {

enum class G16 : uint8_t { free = 0, dc, bitmap, brush, pen, font, region, palette };
const char* g16_name(G16 t);

// The guest-visible state of a DC (what SaveDC saves beside real GDI's own).
struct Dc16State {
  uint16_t bitmap = 0, brush = 0, pen = 0, font = 0, palette = 0;
  COLORREF text = RGB(0, 0, 0), bk = RGB(255, 255, 255);
  bool force_background = false;
};

struct Dc16 {
  HDC host = nullptr;
  bool screen = false;          // draws on the display surface
  bool info = false;            // an information context (CreateIC): never drawn on
  HBITMAP surface = nullptr;    // screen DCs: their DIB section over the display's bits
  HGDIOBJ old_bitmap = nullptr;
  uint16_t hwnd = 0;            // GetDC's window (ReleaseDC checks nothing)
  int16_t org_x = 0, org_y = 0; // GetDCOrg: the window corner on the screen (GetDC sets it)
  Dc16State s;
  std::vector<Dc16State> saved;
};

struct Bitmap16 {
  int w = 0, h = 0, bpp = 8;
  uint8_t* bits = nullptr;  // host view of the pixels (8bpp key surfaces); null for mono bitmaps
  uint32_t stride = 0;
  uint16_t selected_in = 0;  // the DC it is selected into (a bitmap is in at most one)
};

struct Obj16 {
  G16 type = G16::free;
  uint16_t self = 0;       // its own handle
  HGDIOBJ host = nullptr;  // DC: the HDC; others: the real object (null for palettes)
  bool stock = false;
  Dc16 dc;
  Bitmap16 bmp;
  std::shared_ptr<win32::LogicalPalette> pal;
  // Brushes and pens: the guest colour, and the key colour the real object was made with.
  COLORREF color = 0;
  COLORREF made_for = 0xFFFFFFFF;
  int style = 0, width = 0, hatch = 0;
  uint16_t pattern = 0;  // pattern brushes: the bitmap
  LOGFONTA font{};       // fonts: as created (GetObject)
};

class Gdi16 : public RuntimeState16 {
 public:
  static constexpr uint16_t kFirst = 0x2000, kStep = 4, kLimit = 0x8000;

  explicit Gdi16(Runtime16& rt);
  ~Gdi16() override;

  win32::Display& display();

  uint16_t add(Obj16 o);
  Obj16* get(uint16_t h);
  Obj16* get(uint16_t h, G16 t);
  bool destroy(uint16_t h);  // DeleteObject/DeleteDC; stock objects stay
  HGDIOBJ host(uint16_t h);
  HDC host_dc(uint16_t h);
  Dc16* dc(uint16_t h);

  // ---- DCs ----
  uint16_t create_screen_dc(uint16_t hwnd);  // GetDC / the lane's saver-window DC
  void release_dc(uint16_t h);               // ReleaseDC (pooled for the next GetDC)
  uint16_t create_memory_dc();
  uint16_t create_ic();
  // The palette a DC draws with: its selected palette, or DEFAULT_PALETTE.
  win32::LogicalPalette* dc_palette(uint16_t hdc);
  // The surface a DC draws on (the screen's pseudo bitmap for screen DCs); null for mono/IC.
  Bitmap16* dc_surface(uint16_t hdc);
  // A guest COLORREF as the key colour real GDI must use in this DC.
  COLORREF key(uint16_t hdc, COLORREF c);
  // What the guest sees for a pixel index read back from a DC's surface.
  COLORREF index_rgb(int index);
  // Before drawing: the DC's brush/pen re-made for its palette, text colours keyed.
  void sync(uint16_t hdc);
  void sync_brush(uint16_t hdc, uint16_t brush);  // select a specific brush (FillRect)

  // ---- objects ----
  uint16_t stock(int index);
  uint16_t create_brush(COLORREF c, int style = BS_SOLID, int hatch = 0);
  uint16_t create_pen(int style, int width, COLORREF c);
  uint16_t create_palette(const std::vector<PALETTEENTRY>& entries);
  // A device bitmap: 8bpp key surface (hardware indices) or a real 1bpp bitmap.
  uint16_t create_device_bitmap(int w, int h, int bpp);
  uint16_t wrap_region(HRGN r);
  // SelectObject; returns the previous object of that kind (0 on failure).
  uint16_t select(uint16_t hdc, uint16_t obj);

  // ---- surfaces ----
  // A w×h top-down 8-bit key scratch surface (reused), selected into a scratch DC.
  HDC scratch(int w, int h, uint8_t** bits, uint32_t* stride);
  Bitmap16& screen_bitmap() { return screen_bmp_; }

 private:
  void init_stock();
  HGDIOBJ realize_brush(Obj16& o, uint16_t hdc);
  HGDIOBJ realize_pen(Obj16& o, uint16_t hdc);
  // The realization cache (see extra_): true when o now holds an earlier
  // real object made for key colour k (the one it held is kept in its place).
  bool reuse_realization(Obj16& o, COLORREF k);
  // o is about to hold `made` (made for key k): keeps the one it held, then
  // trims o's cache to kMaxRealizations, oldest first, skipping any object
  // still selected into a real DC.
  void keep_realization(Obj16& o, HGDIOBJ made, COLORREF k);
  bool selected_anywhere(HGDIOBJ obj);
  void free_realizations(uint16_t h);

  Runtime16& rt_;
  std::deque<Obj16> slots_;  // stable addresses: an Obj16* survives add()
  std::vector<uint16_t> free_;
  uint16_t stock_[20] = {};
  std::vector<uint16_t> dc_pool_;  // released screen DCs
  Bitmap16 screen_bmp_;
  // Real brushes/pens a guest brush/pen was realized as for other key colours
  // than the one it holds now (a palette change re-keys PALETTEINDEX and
  // palette-relative colours), by guest handle, oldest first. Each may still
  // be selected into some DC, and a module that swaps palettes back and forth
  // (ARTIST calls SelectPalette ~90 times a frame between two palettes, with
  // its pen and brush selected) needs the same few again and again: they are
  // reused by key colour rather than made anew, at most kMaxRealizations per
  // object, and freed with the guest object.
  static constexpr size_t kMaxRealizations = 8;
  struct Realization {
    COLORREF key;
    HGDIOBJ obj;
  };
  std::unordered_map<uint16_t, std::vector<Realization>> extra_;
  HDC scratch_dc_ = nullptr;
  HBITMAP scratch_bmp_ = nullptr;
  HGDIOBJ scratch_old_ = nullptr;
  uint8_t* scratch_bits_ = nullptr;
  int scratch_w_ = 0, scratch_h_ = 0;
};

// 16-bit structures (packed as Win16 lays them out).
#pragma pack(push, 1)
struct RECT16 {
  int16_t left, top, right, bottom;
};
struct POINT16 {
  int16_t x, y;
};
struct BITMAP16 {
  int16_t bmType, bmWidth, bmHeight, bmWidthBytes;
  uint8_t bmPlanes, bmBitsPixel;
  uint32_t bmBits;
};
struct LOGPEN16 {
  uint16_t lopnStyle;
  POINT16 lopnWidth;
  uint32_t lopnColor;
};
struct LOGBRUSH16 {
  uint16_t lbStyle;
  uint32_t lbColor;
  int16_t lbHatch;
};
struct LOGFONT16 {
  int16_t lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
  uint8_t lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality,
      lfPitchAndFamily;
  char lfFaceName[32];
};
struct TEXTMETRIC16 {
  int16_t tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth,
      tmWeight;
  uint8_t tmItalic, tmUnderlined, tmStruckOut, tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar,
      tmPitchAndFamily, tmCharSet;
  int16_t tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
};
#pragma pack(pop)
static_assert(sizeof(RECT16) == 8);
static_assert(sizeof(BITMAP16) == 14);
static_assert(sizeof(LOGPEN16) == 10);
static_assert(sizeof(LOGBRUSH16) == 8);
static_assert(sizeof(LOGFONT16) == 50);
static_assert(sizeof(TEXTMETRIC16) == 31);

// Guest struct access through far pointers.
template <typename T>
T read16(Runtime16& rt, uint32_t fp) {
  T v;
  rt.read_bytes(fp, &v, sizeof(T));
  return v;
}
template <typename T>
void write16(Runtime16& rt, uint32_t fp, const T& v) {
  rt.write_bytes(fp, &v, sizeof(T));
}
inline RECT to_rect(const RECT16& r) { return RECT{r.left, r.top, r.right, r.bottom}; }
inline RECT16 to_rect16(const RECT& r) {
  return RECT16{int16_t(r.left), int16_t(r.top), int16_t(r.right), int16_t(r.bottom)};
}

// The screen DC OLDMOD16 is handed (the saver window's), for the lane.
uint16_t gdi16_screen_dc(Runtime16& rt, uint16_t hwnd);
// A device bitmap from a packed DIB (BITMAPINFO + bits) in guest memory,
// colours matched through `hdc`'s palette (0 = DEFAULT_PALETTE, what
// LoadBitmap used). 0 on failure.
uint16_t gdi16_bitmap_from_dib(Runtime16& rt, uint16_t hdc, uint32_t packed_dib);

}  // namespace adw::win16
