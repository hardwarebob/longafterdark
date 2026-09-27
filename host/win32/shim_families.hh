// The API families, one file per DLL. Each file defines register_<dll>(), which
// binds handlers to the names in its DLL (signatures come from
// signatures.cc). register_all_shims() is what a lane calls.
//
// Ownership (parallel work): a family file owns its handlers and its own
// RuntimeState struct (rt.state<T>()); shared object models live in their own
// headers — gdi_objects.hh (GDI handles, used by USER32 and GDI32),
// display.hh (the screen and palette), heap.hh (memory), modules.hh (images),
// vfs.hh (paths), seh.hh (exceptions). Host-facing helpers a lane (or another
// family) needs from a family are declared here. README.md has the recipe.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "loader/pe.hh"
#include "win32/shims.hh"

namespace adw::win32 {

class Runtime;
class IniStore;

void register_kernel32(ShimRegistry& r);
void register_user32(ShimRegistry& r);
void register_gdi32(ShimRegistry& r);
void register_winmm(ShimRegistry& r);
void register_msacm32(ShimRegistry& r);
void register_shell32(ShimRegistry& r);
void register_comdlg32(ShimRegistry& r);
// Real-window routing over the USER32/COMDLG32 handlers (realui.hh); after them.
void register_user32_real(ShimRegistry& r);

inline void register_all_shims(ShimRegistry& r) {
  register_kernel32(r);
  register_user32(r);
  register_gdi32(r);
  register_winmm(r);
  register_msacm32(r);
  register_shell32(r);
  register_comdlg32(r);
  register_user32_real(r);
}

// ---- host-facing helpers ----

// USER32: the saver window every module draws in (AD_MODULE32 +0x10), created
// on first use with the display's size as its client area.
uint32_t host_window(Runtime& rt);

// USER32: a class the guest registered (RegisterClassA/ExA), by name
// (case-insensitive). False when there is none.
struct GuestClassInfo {
  std::string name;
  uint32_t wndproc = 0, style = 0, background = 0;
  int32_t cls_extra = 0, wnd_extra = 0;
};
bool guest_class(Runtime& rt, std::string_view name, GuestClassInfo* out);
// USER32: a class-name argument (a string pointer, or an atom in the low word).
std::string guest_class_arg(Runtime& rt, uint32_t p);

// USER32: the module posted (or sent) WM_CLOSE / WM_SYSCOMMAND(SC_CLOSE) to
// the saver window (INTERACTION.md §5.1: ADWS_WAKE).
bool saver_wake_requested(Runtime& rt);

// USER32: PostMessageA from the host (MCI and waveOut notifications): the
// message joins the thread's queue with the time of the post. False for a
// window that does not exist (HWND_BROADCAST is accepted and dropped).
bool post_guest_message(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

// USER32: wsprintfA's formatter over a guest format string and a guest
// argument list (dwords at `args`) — for any shim that formats like USER does.
std::string guest_wsprintf(Runtime& rt, const std::string& fmt, uint32_t args);

// KERNEL32: the date and time the guest sees, as a FILETIME (100 ns since
// 1601; the guest's UTC is the host's local time, bias 0). Headless runs
// (fixed-step clock) start at a fixed moment so they stay reproducible;
// streamed runs start at the host's local time. Each call is a clock read.
uint64_t guest_local_filetime(Runtime& rt);

// KERNEL32: the process's profile store (ini_store.hh) over rt.vfs(), with
// WIN.INI's [Berkeley Systems] seeds; what Get/WritePrivateProfileStringA use.
IniStore& profile_store(Runtime& rt);
// KERNEL32: closes every file the guest left open (each commits to its
// overlay's upper layer), as process exit would.
void close_guest_files(Runtime& rt);

// KERNEL32: a resource of a loaded image, preferring US English, then
// neutral, then any language (ABI.md §2.10.1). Null when absent.
const loader::pe::Resource* find_module_resource(Runtime& rt, uint32_t hmod, const loader::ResId& type,
                                                 const loader::ResId& name);

}  // namespace adw::win32
