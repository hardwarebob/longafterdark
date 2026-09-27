// MemoryContext on Windows backs every arena with a file mapping; section_for() hands that mapping to GDI. This test
// builds DIB sections over emulated memory (CreateDIBSection with hSection + offset) and checks that the DIB bits,
// GDI's GetPixel/SetPixel, and the MemoryContext all see the same bytes - the property the Win32/Win16 shims rely on
// to let module code and GDI draw into one surface without copies.

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <memory>

#include "MemoryContext.hh"

using namespace adw::cpu;

static int failures = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

int main() {
  auto mem = std::make_shared<MemoryContext>();
  constexpr uint32_t ARENA = 0x10000000;
  mem->allocate_at(ARENA, 0x20000);
  // A second, separate arena: section_for must report its own mapping and a zero-based offset.
  constexpr uint32_t ARENA2 = 0x20000000;
  mem->allocate_at(ARENA2, 0x1000);

  auto [h1, off1] = mem->section_for(ARENA + 0x1000);
  auto [h1b, off1b] = mem->section_for(ARENA + 0x1FFFC);
  auto [h2, off2] = mem->section_for(ARENA2 + 0x10);
  CHECK(h1 != nullptr, "no section handle");
  CHECK(h1 == h1b, "one arena, two handles");
  CHECK(h1 != h2, "two arenas share a handle");
  CHECK(off1 == 0x1000 && off1b == 0x1FFFC && off2 == 0x10, "offsets %X %X %X", off1, off1b, off2);
  bool threw = false;
  try {
    mem->section_for(0x30000000);
  } catch (const std::out_of_range&) {
    threw = true;
  }
  CHECK(threw, "section_for on unmapped memory did not throw");

  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  CHECK(dc != nullptr, "CreateCompatibleDC failed");

  // --- 32 bpp, top-down, 16x16 at ARENA + 0x1000 ---
  {
    constexpr uint32_t PIX = ARENA + 0x1000;
    auto [hsec, off] = mem->section_for(PIX);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = -16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, static_cast<HANDLE>(hsec), off);
    CHECK(bmp != nullptr && bits != nullptr, "CreateDIBSection over emulated memory failed (error %lu)", GetLastError());
    if (bmp) {
      HGDIOBJ old = SelectObject(dc, bmp);
      // Emulated write -> DIB bits and GetPixel.
      mem->write_u32l(PIX + (5 * 16 + 3) * 4, 0x00FF8040); // B=40 G=80 R=FF
      GdiFlush();
      uint32_t via_bits = reinterpret_cast<uint32_t*>(bits)[5 * 16 + 3];
      CHECK(via_bits == 0x00FF8040, "DIB bits read %08X", via_bits);
      COLORREF c = GetPixel(dc, 3, 5);
      CHECK(c == RGB(0xFF, 0x80, 0x40), "GetPixel read %08lX", c);
      // GDI write -> emulated memory.
      SetPixel(dc, 10, 2, RGB(0x12, 0x34, 0x56));
      RECT rc = {0, 12, 16, 16};
      HBRUSH brush = CreateSolidBrush(RGB(0x01, 0x02, 0x03));
      FillRect(dc, &rc, brush);
      DeleteObject(brush);
      GdiFlush();
      uint32_t px = mem->read_u32l(PIX + (2 * 16 + 10) * 4);
      CHECK((px & 0xFFFFFF) == 0x123456, "SetPixel landed as %08X in emulated memory", px);
      uint32_t filled = mem->read_u32l(PIX + (14 * 16 + 7) * 4);
      CHECK((filled & 0xFFFFFF) == 0x010203, "FillRect landed as %08X in emulated memory", filled);
      // DIB bits write -> emulated memory.
      reinterpret_cast<uint32_t*>(bits)[0] = 0xCAFEF00D;
      CHECK(mem->read_u32l(PIX) == 0xCAFEF00D, "bits write not visible to MemoryContext");
      SelectObject(dc, old);
      DeleteObject(bmp);
    }
  }

  // --- 8 bpp palettized (the display format the modules draw into), bottom-up, 8x4 at an odd DWORD offset ---
  {
    constexpr uint32_t PIX = ARENA + 0x8004;
    auto [hsec, off] = mem->section_for(PIX);
    struct {
      BITMAPINFOHEADER h;
      RGBQUAD colors[256];
    } bmi = {};
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = 8;
    bmi.h.biHeight = 4;
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 8;
    bmi.h.biCompression = BI_RGB;
    bmi.h.biClrUsed = 256;
    for (int z = 0; z < 256; z++) {
      bmi.colors[z] = RGBQUAD{static_cast<BYTE>(z), static_cast<BYTE>(255 - z), static_cast<BYTE>(z ^ 0x55), 0};
    }
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &bits,
        static_cast<HANDLE>(hsec), off);
    CHECK(bmp != nullptr && bits != nullptr, "8 bpp CreateDIBSection failed (error %lu)", GetLastError());
    if (bmp) {
      HGDIOBJ old = SelectObject(dc, bmp);
      // Bottom-up: row 3 (top) is the last row in memory.
      mem->write_u8(PIX + 0 * 8 + 6, 0x42); // bottom row, x=6
      GdiFlush();
      CHECK(reinterpret_cast<uint8_t*>(bits)[6] == 0x42, "8 bpp bits mismatch");
      COLORREF c = GetPixel(dc, 6, 3);
      CHECK(c == RGB(0x42 ^ 0x55, 255 - 0x42, 0x42), "8 bpp GetPixel read %08lX", c);
      SelectObject(dc, old);
      DeleteObject(bmp);
    }
  }

  DeleteDC(dc);
  ReleaseDC(nullptr, screen);

  // Freeing an arena must release its mapping (handles are per arena); a new arena gets a new one.
  {
    auto mem2 = std::make_shared<MemoryContext>();
    uint32_t a = mem2->allocate(0x3000);
    auto [h, off] = mem2->section_for(a);
    CHECK(h != nullptr && off == 0, "fresh arena handle/offset");
    mem2->free(a);
    threw = false;
    try {
      mem2->section_for(a);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    CHECK(threw, "section_for after free did not throw");
  }

  if (failures) {
    printf("memory/DIB aliasing: %d failure(s)\n", failures);
    return 1;
  }
  printf("memory/DIB aliasing: all checks passed\n");
  return 0;
}
