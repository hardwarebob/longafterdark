// GDI objects as the guest sees them: small handle values (0x1000, 0x1004, …,
// below 0x10000 so code that keeps a handle in a WORD — Win16 heritage — and
// the Win16 lane both work) that name entries of this table. An entry holds the
// REAL GDI object when there is one (DCs, bitmaps, brushes, pens, fonts,
// regions: shims pass those to real GDI) and the emulated state GDI cannot
// carry for us: logical palettes (display.hh), the palette selected into a DC,
// the guest COLORREF behind a brush or pen (it maps differently through each
// DC's palette), a bitmap's pixels and what its indices mean.
//
// Owned by the Runtime as a RuntimeState: rt.state<GdiTable>(). USER32 (GetDC,
// FillRect, DrawText…) and GDI32 both use it; so will the Win16 GDI shims.
//
// Surfaces (display.hh, "The key table"): every 8-bit bitmap is a real 8bpp
// DIB section with the key table. A DEVICE bitmap (the screen,
// CreateCompatibleBitmap, CreateBitmap at 8 bpp, LoadBitmap) holds hardware
// indices; a DIB bitmap (CreateDIBSection) holds indices into its own colour
// table. surface_xlate() gives the translation between two of them.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "win32/display.hh"
#include "win32/runtime.hh"

namespace adw::win32 {

enum class GdiType : uint8_t { free = 0, dc, bitmap, brush, pen, font, region, palette };
const char* gdi_type_name(GdiType t);

struct GdiDc {
  bool is_screen = false;    // the display's DC: never deleted, drawing reaches the screen
  uint32_t palette = 0;      // guest handle of the selected logical palette (0 = DEFAULT_PALETTE)
  uint32_t bitmap = 0;       // guest handle of the selected bitmap
  uint32_t brush = 0, pen = 0, font = 0;  // guest handles of the selected objects (0 = the DC's default)
  COLORREF text_color = RGB(0, 0, 0);     // as the guest set them (GetTextColor answers these)
  COLORREF bk_color = RGB(255, 255, 255);
  int stretch_mode = BLACKONWHITE;
};

enum class BitmapKind : uint8_t {
  device,  // indices are hardware palette indices
  dib,     // indices mean the bitmap's own colour table (usage says how)
};

struct GdiBitmap {
  BitmapKind kind = BitmapKind::device;
  int width = 0, height = 0, bpp = 0;
  uint32_t stride = 0;           // bytes per row (DWORD-aligned)
  bool top_down = false;
  uint32_t guest_bits = 0;       // DIB section: the pixels' guest address (0 for device bitmaps)
  uint8_t* host_bits = nullptr;  // where the host reads/writes the pixels (null: unknown, e.g. mono)
  // DIB sections: the colour table as the guest gave it.
  uint32_t usage = DIB_RGB_COLORS;    // DIB_RGB_COLORS or DIB_PAL_COLORS
  std::vector<RGBQUAD> colors;        // DIB_RGB_COLORS
  std::vector<uint16_t> pal_indices;  // DIB_PAL_COLORS: indices into the DC's logical palette
  uint32_t compression = BI_RGB;
  uint32_t masks[3] = {0, 0, 0};      // BI_BITFIELDS
  uint32_t section = 0, section_offset = 0;  // as the guest passed them (GetObject reports them)
  uint32_t guest_bits_alloc = 0;      // heap block to free with the bitmap (0 = not ours)

  uint8_t* row(int y) const {
    if (!host_bits) return nullptr;
    int r = top_down ? y : height - 1 - y;
    return host_bits + size_t(r) * stride;
  }
};

struct GdiObject {
  GdiType type = GdiType::free;
  HGDIOBJ host = nullptr;    // the real GDI object (an HDC for DCs); null for emulated palettes
  bool stock = false;        // GetStockObject: DeleteObject is a no-op
  bool owned = true;         // release `host` when the entry is destroyed
  GdiDc dc;                  // type == dc
  GdiBitmap bmp;             // type == bitmap
  std::shared_ptr<LogicalPalette> pal;  // type == palette
  // Brushes and pens: the colour the guest asked for, and the key colour the
  // real object was made with (re-made when a DC's palette maps it elsewhere).
  COLORREF color = 0;
  COLORREF made_for = 0xFFFFFFFF;
  int pen_style = 0, pen_width = 0;   // pens
  int brush_style = BS_SOLID;         // brushes (BS_SOLID, BS_NULL, BS_HATCHED, …)
  int hatch = 0;
};

class GdiTable : public RuntimeState {
 public:
  static constexpr uint32_t kFirst = 0x1000, kStep = 4, kLimit = 0x10000;

  explicit GdiTable(Runtime& rt) : rt_(rt) {}
  ~GdiTable() override;

  // A new handle for obj (0 when the table is full).
  uint32_t add(GdiObject obj);
  GdiObject* get(uint32_t h);
  GdiObject* get(uint32_t h, GdiType t);
  // The real object behind h, or null.
  HGDIOBJ host(uint32_t h);
  HDC host_dc(uint32_t h) {
    GdiObject* o = get(h, GdiType::dc);
    return o ? static_cast<HDC>(o->host) : nullptr;
  }
  // DeleteObject/DeleteDC: releases an owned host object and frees the handle.
  // Stock objects are left alone (and still report success).
  bool destroy(uint32_t h);
  // The handle naming a real object, adding a non-owned entry when it has
  // none yet (what SelectObject hands back: the DC's default bitmap/pen/…).
  uint32_t wrap_host(HGDIOBJ obj, GdiType t);
  uint32_t handle_for_host(HGDIOBJ obj) const;
  // A DC for a real HDC (CreateCompatibleDC …); records its default bitmap.
  uint32_t add_dc(HDC dc, bool owned);

  // A device bitmap in host memory: 8 bpp (a key-table surface holding
  // hardware indices) or 1 bpp (a real monochrome bitmap). 0 on failure.
  uint32_t create_device_bitmap(int w, int h, int bpp);

  uint32_t stock(int index);        // GetStockObject
  uint32_t screen_dc();             // the display's DC (needs rt.display())
  uint32_t screen_bitmap();         // the display's surface as a (device) bitmap handle
  // The palette a DC draws with: its selected palette, or DEFAULT_PALETTE.
  LogicalPalette* dc_palette(uint32_t hdc);
  // The bitmap selected into a DC (the screen surface for the screen DC).
  GdiBitmap* dc_surface(uint32_t hdc);
  Display& display();

  // Guest COLORREF → the key colour real GDI must draw with in this DC.
  COLORREF key_color(uint32_t hdc, COLORREF c);
  // Makes the real brush/pen of `h` match `hdc`'s palette (re-created when the
  // mapping changed). Returns the real object.
  HGDIOBJ realize_for(uint32_t h, uint32_t hdc);

  // What index i of `b` looks like as an RGB (through `hdc`'s palette for
  // DIB_PAL_COLORS; hardware palette for device bitmaps).
  COLORREF surface_rgb(const GdiBitmap& b, int index, uint32_t hdc);
  // Translation of indices from surface `src` (drawn through src_dc) to
  // surface `dst` (drawn through dst_dc), as Win95 colour matching does.
  // Only for 8-bit or lower indexed surfaces; identity for device → device.
  Xlate surface_xlate(const GdiBitmap& src, uint32_t src_dc, const GdiBitmap& dst, uint32_t dst_dc);

  static GdiType type_of_host(HGDIOBJ obj);

 private:
  Runtime& rt_;
  std::vector<GdiObject> slots_;    // index (h - kFirst) / kStep
  std::vector<uint32_t> free_;
  std::unordered_map<HGDIOBJ, uint32_t> by_host_;
  std::unordered_map<int, uint32_t> stock_;
  // Real brushes/pens made for other palettes than the primary one (see
  // realize_for), kept until their guest object is deleted: any of them may
  // still be selected into some DC.
  std::unordered_map<uint32_t, std::vector<HGDIOBJ>> extra_objects_;
  uint32_t screen_dc_ = 0, screen_bitmap_ = 0;
};

}  // namespace adw::win32
