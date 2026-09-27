// The emulated display: the authentic Win95 256-colour screen the modules were
// written for (ABI.md §2.9): 8 bpp, RC_PALETTE, a 256-entry hardware
// ("system") palette with 20 static colours, logical palettes realized into
// it, palette animation.
//
// This is a HOST-LEVEL model with no Win32 guest knowledge: it needs only
// core's Screen and the CPU's MemoryContext, so the Win16 shims use it too.
//
// Surface. The screen is a top-down 8-bit DIB section whose bits live in
// emulated memory (the caller allocates them inside one arena — the Win32
// runtime uses its heap arena), selected into a real memory DC. Module code
// that writes pixels directly and real GDI drawing through dc() see the same
// bytes, and core's Screen is attached to them, so presenting a frame reads
// the module's pixels in place.
//
// The key table — why 8-bit surfaces never carry the real palette. Every
// 8-bit surface the host gives real GDI (the screen, compatible bitmaps, the
// guest's 8-bit DIB sections, scratch surfaces) carries the same fixed colour
// table: entry i = RGB(i, i, i) (key_table()). Real GDI therefore sees 256
// distinct colours that never change, and:
//   * a blit between two 8-bit surfaces is an exact copy of the indices (the
//     colour tables are identical and unique, so GDI's colour translation is
//     the identity) — palette animation can never be undone by a blit
//     re-matching duplicate entries;
//   * drawing with COLORREF key_color(n) (= RGB(n,n,n), matched exactly) puts
//     index n into the pixels: map_color() turns every guest COLORREF into the
//     key colour of the hardware index a Win95 palette device would have
//     drawn, and GDI shims hand real GDI only key colours;
//   * palette changes (realize, animate, SetSystemPaletteUse) touch nothing
//     but the hardware palette, which goes to Screen for the next P8 frame.
// What an index MEANS is the host's business: on the screen and on
// device-dependent bitmaps it is a hardware palette index; in a guest DIB
// section it indexes the guest's own colour table (DIB_RGB_COLORS) or the DC's
// logical palette (DIB_PAL_COLORS). Transfers between surfaces of different
// meanings go through a 256-entry translation (Xlate) that GDI shims build
// with the helpers below — the same colour matching Win95 did.
//
// Palette model (what GDI shims build on):
//   * system_palette(): the hardware palette. Every change goes to Screen.
//   * LogicalPalette + realize(): Win95's foreground realization — entries
//     that match a static colour or an earlier entry exactly collapse onto it,
//     PC_RESERVED/PC_NOCOLLAPSE entries always get a slot of their own,
//     PC_EXPLICIT entries name a hardware index; slots are handed out from the
//     first non-static index up; when they run out, nearest match.
//   * animate(): AnimatePalette — only PC_RESERVED entries of the palette most
//     recently realized change, and the hardware palette with them.
//   * map_index(): turns a GDI COLORREF into the hardware index a palette
//     device would have drawn:
//       PALETTEINDEX(i) → the selected palette's mapping of entry i
//       PALETTERGB(rgb) → nearest entry of the selected palette, mapped
//       RGB(rgb)        → nearest STATIC colour (palette devices match plain
//                         RGB against the 20 statics; brushes would dither,
//                         we take the nearest)
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "MemoryContext.hh"
#include "adw/core/screen.h"

namespace adw::win32 {

struct LogicalPalette {
  std::vector<PALETTEENTRY> entries;
  std::vector<uint8_t> map;   // logical index → hardware index (valid once realized)
  bool realized = false;
  uint64_t realize_serial = 0;
};

// A per-index translation between two surfaces' index meanings.
struct Xlate {
  std::array<uint8_t, 256> map{};
  bool identity = true;
  void finish() {
    identity = true;
    for (int i = 0; i < 256; i++)
      if (map[size_t(i)] != i) identity = false;
  }
  static Xlate make_identity() {
    Xlate x;
    for (int i = 0; i < 256; i++) x.map[size_t(i)] = uint8_t(i);
    return x;
  }
};

class Display {
 public:
  // Bytes the caller must provide at bits_addr for a w×h screen (DWORD rows).
  static uint32_t pitch_for(int w) { return uint32_t((w + 3) & ~3); }
  static uint32_t bits_size(int w, int h) { return pitch_for(w) * uint32_t(h); }

  // bits_addr must be DWORD-aligned and the whole surface inside one arena.
  // Attaches `screen` (whose size is the display size) to the DIB bits.
  Display(Screen& screen, cpu::MemoryContext& mem, uint32_t bits_addr);
  ~Display();
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  int width() const { return w_; }
  int height() const { return h_; }
  uint32_t pitch() const { return pitch_for(w_); }
  uint32_t bits_addr() const { return bits_; }
  uint8_t* bits() { return host_bits_; }
  HDC dc() const { return dc_; }          // the screen: a memory DC with the DIB selected
  HBITMAP bitmap() const { return bmp_; }
  Screen& screen() { return screen_; }

  // ---- 8-bit surfaces (see "The key table") ----
  static const std::array<RGBQUAD, 256>& key_table();
  static COLORREF key_color(int index) { return RGB(index & 0xFF, index & 0xFF, index & 0xFF); }
  // A real 8bpp DIB section carrying the key table: over `section` at
  // `offset` (emulated memory; offset DWORD-aligned) or, with a null section,
  // in host memory. *bits receives GDI's pointer to the pixels. Null on failure.
  static HBITMAP create_surface8(int w, int h, bool top_down, HANDLE section, DWORD offset, uint8_t** bits);

  // ---- hardware palette ----
  const std::array<PALETTEENTRY, 256>& system_palette() const { return sys_; }
  COLORREF hardware_color(int index) const {
    const PALETTEENTRY& e = sys_[size_t(index & 0xFF)];
    return RGB(e.peRed, e.peGreen, e.peBlue);
  }
  void set_system_entries(int first, int count, const PALETTEENTRY* e);
  // SYSPAL_STATIC (1) / SYSPAL_NOSTATIC (2) / SYSPAL_NOSTATIC256 (3); returns the previous use.
  UINT palette_use() const { return use_; }
  UINT set_palette_use(UINT use);
  int static_low() const;   // statics are [0, static_low) and [256-static_low, 256)
  bool is_static(int index) const { return index < static_low() || index >= 256 - static_low(); }
  // DEFAULT_PALETTE: the 20 static colours as a logical palette.
  static LogicalPalette default_palette();

  // ---- logical palettes ----
  // Returns the number of logical entries mapped.
  UINT realize(LogicalPalette& p, bool background = false);
  void animate(LogicalPalette& p, int start, int count, const PALETTEENTRY* e);
  bool is_current(const LogicalPalette& p) const { return p.realized && p.realize_serial == serial_; }

  // ---- colour matching ----
  // Nearest hardware index to rgb (squared RGB distance, lowest index on ties).
  int nearest_index(COLORREF rgb, bool statics_only) const;
  static int nearest_in(const LogicalPalette& p, COLORREF rgb);
  static int nearest_in(const RGBQUAD* table, int count, COLORREF rgb);
  // COLORREF → hardware index (see the header comment). `selected` may be null.
  int map_index(COLORREF c, const LogicalPalette* selected) const;
  // … as the key colour real GDI must be given.
  COLORREF map_color(COLORREF c, const LogicalPalette* selected) const { return key_color(map_index(c, selected)); }
  // An RGB drawn onto a device surface through a DC whose palette is `selected`
  // (DIB colours on a palette device: nearest entry of the DC's palette,
  // mapped; with no palette, nearest hardware colour).
  int device_index_for_rgb(COLORREF rgb, const LogicalPalette* selected) const;

  // ---- GetDeviceCaps for the emulated 8 bpp RC_PALETTE display ----
  int device_caps(int index) const;

  // ---- desktop seed ----
  // What the screen held when the saver started (ADSEEDIMG).
  // A module's own blank decides what survives: most erase it, but Shadow
  // Agents with "Clear Screen First" off, Bad Dog!, Puzzle, Spotlight and the
  // other screen transformers draw over (or chew) the desktop; Slow Burn
  // blanks it first (PACKAGES.md §12 has the census list). A lane calls this once,
  // after the display exists and before the module's first message.
  //   ":win95"  the Windows 95 default desktop, solid teal (static index 6)
  //   <file>    raw index bytes of exactly w*h (used as they are), or a
  //             binary P6 PPM (maxval 255) or a BMP of any size: scaled to the
  //             screen (area average down, nearest up; a P6 of exactly the
  //             screen size is used 1:1) and Floyd–Steinberg dithered onto the
  //             20 static colours — the only colours a desktop keeps once a
  //             module realizes its own palette. The file is opened with share
  //             read + write + delete, so the .scr's delete-on-close capture,
  //             which it keeps open for the saver's lifetime, can be read
  //             (INTERACTION.md §8).
  // Deterministic for a given file. False (screen untouched; *what = the
  // reason) when the spec is unusable; true with *what = what was seeded.
  bool seed(const std::string& spec, std::string* what);
  // The RGB → statics half of seed(), for tests: rgb is w*h*3 bytes, top-down.
  void seed_rgb(const uint8_t* rgb, int w, int h);

  // Palette changes since construction (tests; a lane may skip re-encoding).
  uint64_t palette_changes() const { return changes_; }

 private:
  void push(int first, int count);  // hardware palette → Screen

  Screen& screen_;
  cpu::MemoryContext& mem_;
  uint32_t bits_;
  uint8_t* host_bits_ = nullptr;
  int w_, h_;
  HDC dc_ = nullptr;
  HBITMAP bmp_ = nullptr;
  HGDIOBJ old_bmp_ = nullptr;
  std::array<PALETTEENTRY, 256> sys_{};
  UINT use_ = SYSPAL_STATIC;
  uint64_t serial_ = 0;
  uint64_t changes_ = 0;
};

}  // namespace adw::win32
