// The screenshot hooks' capture: render a window that is never shown on a
// visible monitor into a PNG (PrintWindow into a 32-bit DIB, encoded by WIC).
// Used by LongAfterDark-test.scr's AD_SCR_TEST_SCREENSHOT (config_dialog.cc)
// and adimport's; its PNG encoder also writes the live preview's thumbnails
// and AD_SCR_TEST_CAPTURE's pictures.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace adw::ui {

// Parks `hwnd` off every monitor, cloaked and never activated, so it can be
// rendered without appearing on the user's desktop.
void park_offscreen(HWND hwnd);

// Renders the whole window (frame included) to `png_path`. false with `*error`
// set when the window cannot be rendered or the file cannot be written.
// `origin` (optional) receives the screen point that is the PNG's top-left.
// DWM's copy of a cloaked window can lag what it drew on a busy machine, so
// the window is grabbed, redrawn and grabbed again until two grabs agree
// (at most about a second; then the latest).
bool capture_window_png(HWND hwnd, const std::wstring& png_path, std::string* error = nullptr,
                        POINT* origin = nullptr);

// Encodes `bgr` (top-down rows of w*3 bytes, B G R) as a PNG at `png_path`
// (also used for the live preview's module thumbnails).
bool save_png_bgr(const std::wstring& png_path, int w, int h, const std::vector<uint8_t>& bgr,
                  std::string* error = nullptr);

// ---- additions beyond COVERS.md §3.2 (T) ---------------------------------------------

// Makes a parked window ready for capture_window_png: repaints it and its children now,
// dispatches this thread's pending messages for up to `settle_ms`, has it render once
// through a plain PrintWindow, then waits for DWM to compose it (PrintWindow's
// full-content copy comes from DWM, which is still black for a cloaked window that has
// never rendered that way).
void settle_for_capture(HWND hwnd, int settle_ms = 150);

} // namespace adw::ui
