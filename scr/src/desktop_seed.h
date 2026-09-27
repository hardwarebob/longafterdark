// The desktop seed (INTERACTION.md §8): before any /s window appears, each
// monitor is captured and handed to that window's first host as ADSEEDIMG,
// so the module starts on the desktop it is saving, as the 1996 hosts did.
//
// The picture is a binary P6 (maxval 255) at the window's emulated size,
// written to %TEMP%\LongAfterDark-seed-<pid>-<window index>.ppm through a
// handle opened FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY with
// share read + delete. The saver keeps that handle for its lifetime, so the
// file disappears when the saver ends, however it ends. A host opens it with
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "geometry.h"

namespace adw::scr {

// A P6 image in memory: header + w*h*3 RGB bytes, from top-down BGRX rows.
std::vector<uint8_t> encode_p6(const uint8_t* bgrx, int w, int h, int stride);

// Creates the delete-on-close file at `path` and writes `p6` into it. Returns
// the open handle (keep it; closing it deletes the file) or
// INVALID_HANDLE_VALUE with *error set.
HANDLE write_seed_file(const std::wstring& path, const std::vector<uint8_t>& p6, std::wstring* error);

// Captures `monitor` (virtual-screen pixels) with BitBlt(SRCCOPY |
// CAPTUREBLT), shrinks or stretches it to `emu` with HALFTONE, and returns it
// as a P6 (empty on failure).
std::vector<uint8_t> capture_monitor_p6(const RECT& monitor, SizeI emu);

}  // namespace adw::scr
