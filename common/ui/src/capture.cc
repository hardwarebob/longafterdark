#include "adw/ui/capture.h"

#include <dwmapi.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

namespace adw::ui {

namespace {

#ifndef PW_RENDERFULLCONTENT
constexpr UINT PW_RENDERFULLCONTENT = 0x00000002;
#endif

template <typename T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() { if (p) p->Release(); }
  T** out() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

// One PrintWindow(PW_RENDERFULLCONTENT) of the whole window (w x h, frame
// included) into `px`, as 32-bit pixels, top-down.
bool grab(HWND hwnd, int w, int h, std::vector<uint32_t>& px) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!dib || !mem || !bits) {
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    return false;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  BOOL printed = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
  if (!printed) {
    SendMessageW(hwnd, WM_PRINT, (WPARAM)mem, PRF_CHILDREN | PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_OWNED);
  }
  GdiFlush();
  const uint32_t* src = static_cast<const uint32_t*>(bits);
  px.assign(src, src + (size_t)w * h);
  for (uint32_t& p : px) p &= 0x00FFFFFFu;   // the alpha byte is whatever PrintWindow left
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  return true;
}

bool fail(std::string* error, const char* what, HRESULT hr = S_OK) {
  if (error) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s (hr=0x%08lX)", what, (unsigned long)hr);
    *error = buf;
  }
  return false;
}

} // namespace

void park_offscreen(HWND hwnd) {
  // Cloaked: DWM keeps composing it (so it can be rendered) but never shows
  // it. Off every monitor as well, in case cloaking is unavailable.
  BOOL cloak = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
  SetWindowPos(hwnd, HWND_BOTTOM, -32000, -32000, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

bool capture_window_png(HWND hwnd, const std::wstring& png_path, std::string* error, POINT* origin) {
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) return fail(error, "GetWindowRect");
  const int w = wr.right - wr.left, h = wr.bottom - wr.top;
  if (w <= 0 || h <= 0) return fail(error, "empty window");
  // What the user sees: without the invisible resize borders around it.
  RECT vis = wr;
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis))) ||
      vis.right <= vis.left || vis.bottom <= vis.top) {
    vis = wr;
  }
  const int cx = std::max(0L, vis.left - wr.left), cy = std::max(0L, vis.top - wr.top);
  const int ow = std::min<int>(w - cx, vis.right - vis.left), oh = std::min<int>(h - cy, vis.bottom - vis.top);
  if (origin) *origin = POINT{wr.left + cx, wr.top + cy};

  // PrintWindow's full-content copy comes from DWM, and DWM's copy of a
  // cloaked window can lag what the window drew, most of all on a busy
  // machine (a test once captured a bare background with its control
  // missing). So: grab, have the window draw itself again and DWM compose,
  // grab again, until two grabs agree (at most about a second, then the
  // latest). Nothing is pumped meanwhile, so nothing animates between grabs.
  std::vector<uint32_t> px, again;
  if (!grab(hwnd, w, h, px)) return fail(error, "CreateDIBSection");
  const ULONGLONG until = GetTickCount64() + 1000;
  for (int i = 0; i < 16 && GetTickCount64() < until; ++i) {
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
    GdiFlush();
    DwmFlush();
    if (!grab(hwnd, w, h, again)) break;
    if (again == px) break;
    px.swap(again);
  }
  // Opaque BGR rows for the encoder.
  std::vector<uint8_t> rgb((size_t)ow * oh * 3);
  for (int y = 0; y < oh; ++y) {
    for (int x = 0; x < ow; ++x) {
      const uint32_t s = px[(size_t)(y + cy) * w + (x + cx)];
      uint8_t* d = rgb.data() + ((size_t)y * ow + x) * 3;
      d[0] = (uint8_t)s;
      d[1] = (uint8_t)(s >> 8);
      d[2] = (uint8_t)(s >> 16);
    }
  }
  return save_png_bgr(png_path, ow, oh, rgb, error);
}

bool save_png_bgr(const std::wstring& png_path, int ow, int oh, const std::vector<uint8_t>& rgb, std::string* error) {
  if (ow <= 0 || oh <= 0 || rgb.size() < (size_t)ow * oh * 3) return fail(error, "empty image");
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.out()));
    if (FAILED(hr)) { fail(error, "WIC factory", hr); goto done; }
    if (FAILED(hr = factory->CreateStream(stream.out()))) { fail(error, "CreateStream", hr); goto done; }
    if (FAILED(hr = stream->InitializeFromFilename(png_path.c_str(), GENERIC_WRITE))) {
      fail(error, "cannot write the PNG file", hr);
      goto done;
    }
    if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.out()))) {
      fail(error, "CreateEncoder", hr);
      goto done;
    }
    if (FAILED(hr = encoder->Initialize(stream.p, WICBitmapEncoderNoCache))) { fail(error, "Initialize", hr); goto done; }
    if (FAILED(hr = encoder->CreateNewFrame(frame.out(), nullptr))) { fail(error, "CreateNewFrame", hr); goto done; }
    if (FAILED(hr = frame->Initialize(nullptr))) { fail(error, "frame Initialize", hr); goto done; }
    if (FAILED(hr = frame->SetSize((UINT)ow, (UINT)oh))) { fail(error, "SetSize", hr); goto done; }
    {
      WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
      if (FAILED(hr = frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) {
        fail(error, "SetPixelFormat", hr);
        goto done;
      }
    }
    if (FAILED(hr = frame->WritePixels((UINT)oh, (UINT)ow * 3, (UINT)((size_t)ow * oh * 3), const_cast<uint8_t*>(rgb.data())))) {
      fail(error, "WritePixels", hr);
      goto done;
    }
    if (FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) { fail(error, "Commit", hr); goto done; }
    ok = true;
  }
done:
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

void settle_for_capture(HWND hwnd, int settle_ms) {
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
  const ULONGLONG until = GetTickCount64() + (ULONGLONG)std::max(0, settle_ms);
  MSG m;
  do {
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) {
        PostQuitMessage((int)m.wParam);
        return;
      }
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
  } while (GetTickCount64() < until);
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  // A plain PrintWindow once: for a cloaked window that was never on screen,
  // PW_RENDERFULLCONTENT alone keeps returning black until something has made
  // the window render into its redirection surface this way (measured: every
  // time without it, never with it).
  RECT wr{};
  GetWindowRect(hwnd, &wr);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, std::max(1L, wr.right - wr.left), std::max(1L, wr.bottom - wr.top));
  ReleaseDC(nullptr, screen);
  HGDIOBJ old = SelectObject(mem, bmp);
  PrintWindow(hwnd, mem, 0);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  GdiFlush();
  // Two presents: the first may already have been under way without our pixels.
  DwmFlush();
  DwmFlush();
}

} // namespace adw::ui
