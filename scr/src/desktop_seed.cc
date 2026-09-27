#include "desktop_seed.h"

#include <cstring>

namespace adw::scr {

std::vector<uint8_t> encode_p6(const uint8_t* bgrx, int w, int h, int stride) {
  std::vector<uint8_t> out;
  if (!bgrx || w <= 0 || h <= 0) return out;
  std::string header = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
  out.resize(header.size() + (size_t)w * h * 3);
  memcpy(out.data(), header.data(), header.size());
  uint8_t* d = out.data() + header.size();
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = bgrx + (size_t)y * stride;
    for (int x = 0; x < w; ++x, s += 4, d += 3) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
    }
  }
  return out;
}

HANDLE write_seed_file(const std::wstring& path, const std::vector<uint8_t>& p6, std::wstring* error) {
  auto fail = [&](const wchar_t* what) {
    if (error) *error = std::wstring(what) + L" failed (error " + std::to_wstring(GetLastError()) + L")";
    return INVALID_HANDLE_VALUE;
  };
  if (p6.empty()) {
    SetLastError(ERROR_INVALID_DATA);
    return fail(L"capture");
  }
  // Not inheritable: the hosts open it by name (their own handles), so no
  // host outlives it by holding ours.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return fail(L"CreateFile");
  size_t off = 0;
  while (off < p6.size()) {
    DWORD put = 0;
    DWORD chunk = (DWORD)std::min<size_t>(p6.size() - off, 1u << 20);
    if (!WriteFile(h, p6.data() + off, chunk, &put, nullptr) || put == 0) {
      HANDLE bad = fail(L"WriteFile");
      CloseHandle(h);   // deletes it
      return bad;
    }
    off += put;
  }
  return h;
}

std::vector<uint8_t> capture_monitor_p6(const RECT& monitor, SizeI emu) {
  std::vector<uint8_t> out;
  const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
  if (mw <= 0 || mh <= 0 || emu.w <= 0 || emu.h <= 0) return out;
  HDC screen = GetDC(nullptr);
  if (!screen) return out;
  HDC src = CreateCompatibleDC(screen), dst = CreateCompatibleDC(screen);
  auto dib = [&](int w, int h, void** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(screen, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
  };
  void *src_bits = nullptr, *dst_bits = nullptr;
  HBITMAP src_bmp = src ? dib(mw, mh, &src_bits) : nullptr;
  HBITMAP dst_bmp = dst ? dib(emu.w, emu.h, &dst_bits) : nullptr;
  if (src_bmp && dst_bmp) {
    HGDIOBJ old_src = SelectObject(src, src_bmp), old_dst = SelectObject(dst, dst_bmp);
    // CAPTUREBLT: layered windows too, as the user sees the desktop.
    if (BitBlt(src, 0, 0, mw, mh, screen, monitor.left, monitor.top, SRCCOPY | CAPTUREBLT)) {
      SetStretchBltMode(dst, HALFTONE);
      SetBrushOrgEx(dst, 0, 0, nullptr);
      if (StretchBlt(dst, 0, 0, emu.w, emu.h, src, 0, 0, mw, mh, SRCCOPY)) {
        GdiFlush();
        out = encode_p6(static_cast<const uint8_t*>(dst_bits), emu.w, emu.h, emu.w * 4);
      }
    }
    SelectObject(src, old_src);
    SelectObject(dst, old_dst);
  }
  if (src_bmp) DeleteObject(src_bmp);
  if (dst_bmp) DeleteObject(dst_bmp);
  if (src) DeleteDC(src);
  if (dst) DeleteDC(dst);
  ReleaseDC(nullptr, screen);
  return out;
}

}  // namespace adw::scr
