// Real windows for configure mode (INTERACTION.md §6.1, §6.2 Win32).
//
// In the saver nothing is ever shown: dialogs are refused and message boxes
// answered in the log. When a lane runs a module's button (adhostwin
// --configure), it enables this layer and the module's own UI becomes real:
//
//   * DialogBox*/CreateDialog* take the guest's DLGTEMPLATE(EX) (RT_DIALOG in
//     the guest module, or the guest's in-memory template) and create a real
//     dialog with CreateDialogIndirectParamW, owned by the --owner window, run
//     modally here (the owner is disabled meanwhile, as DialogBox does). A host
//     dialog procedure forwards every message the guest can understand to the
//     guest DLGPROC through call_guest, marshalling handles and pointers
//     (below); EndDialog from the guest ends the loop with the guest's result.
//   * Classes the guest registered (RegisterClassA) and names in a template,
//     or creates children of, get a real class whose window procedure
//     forwards the same way (DefWindowProcA from the guest reaches the real
//     DefWindowProcW with the original parameters).
//   * Guest handles for real windows are 0xA000 + 4·i (at most 0xFFFC),
//     mapped both ways (guest()/real()); the owner gets one too. The USER32
//     shims act on the real window when handed one of these. INTERACTION.md
//     §6.2 proposed 0x00F00000 + 4·i; that breaks modules whose dialog
//     procedures still decode WM_COMMAND the Win16 way — FISH's and RAIN's
//     BtnDlgProc read the notification code from HIWORD(lParam), which on
//     Windows 95 was 0 for a click because its HWNDs were 16-bit values. So
//     the handles keep a zero high word, as Windows 95's did. (They share
//     numbers with GDI handles, as on Windows 95; every shim knows which kind
//     each argument is. Emulated windows start at 0x00010010.)
//   * Marshalled messages, guest → real (send) and real → guest (the host
//     procedures): WM_SETTEXT/WM_GETTEXT/WM_GETTEXTLENGTH, EM_* with text or
//     pointers (EM_GETLINE, EM_REPLACESEL, EM_GETSEL, EM_GETRECT, …),
//     LB_*/CB_* with strings (ADDSTRING, INSERTSTRING, FINDSTRING(EXACT),
//     SELECTSTRING, GETTEXT, GETLBTEXT, DIR through the Vfs, GETSELITEMS, item
//     data; owner-draw lists without HASSTRINGS pass item data), BM_*, STM_*
//     (images are not converted: 0), WM_COMMAND, WM_NOTIFY (a read-only NMHDR),
//     WM_DRAWITEM / WM_MEASUREITEM / WM_COMPAREITEM / WM_DELETEITEM (32-bit
//     struct copies), WM_CTLCOLOR* (a guest DC wrapper; the returned guest
//     brush becomes a real brush of its colour), WM_PAINT / WM_ERASEBKGND
//     (BeginPaint and GetDC on a real window give a wrapped DC), WM_TIMER,
//     scroll messages, WM_SETFONT/WM_GETFONT, focus and activation (handles
//     mapped), keyboard and mouse messages, and the WM_USER / WM_APP ranges
//     (numbers). Anything else is not forwarded to the guest (a DLGPROC
//     returns FALSE, a window procedure gets DefWindowProcW), and from the
//     guest passes numbers with a 0 pointer.
//   * A guest DC wrapper is an 8-bit key-table surface (display.hh) the guest
//     draws on with the ordinary GDI shims; it starts as the real pixels matched
//     to the hardware palette and goes back to the real DC through that palette
//     (what a 256-colour Win95 display showed), so palettes a dialog procedure
//     realizes work as they did.
//   * MessageBoxA becomes a real MessageBoxW owned by the guest's window (or
//     the --owner); GetOpenFileNameA / GetSaveFileNameA the real
//     GetOpenFileNameW / GetSaveFileNameW with guest ↔ host paths (the initial
//     directory through the Vfs, the chosen file back through
//     Vfs::host_to_guest: a mount form or H:\<L>\…); OFN hooks are called
//     through call_guest.
//   * The thread runs with DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED, so
//     dialog-unit layouts designed for 96 DPI keep their proportions with crisp
//     text.
//   * The configure-mode test hooks (config_script.hh) park, script, time out
//     and answer.
//
// A guest error inside a real window procedure (a fault, ExitProcess) cannot
// unwind through USER32's frames: it is caught, every dialog loop ends, and
// the error is rethrown from the outermost DialogBox*, so the lane sees it
// as from any other call.
#pragma once

#include <windows.h>

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "win32/runtime.hh"

namespace adw::win32 {

class ConfigScript;

class RealUi : public RuntimeState {
 public:
  static constexpr uint32_t kFirst = 0x0000A000, kStep = 4, kCount = (0x10000 - kFirst) / kStep;

  explicit RealUi(Runtime& rt);
  ~RealUi() override;

  // Configure mode on: `owner` owns every dialog (may be null); `script` (may
  // be null) is consulted for every dialog, message box and file dialog.
  void enable(HWND owner, ConfigScript* script);
  bool enabled() const { return enabled_; }
  HWND owner() const { return owner_; }
  ConfigScript* script() const { return script_; }

  // ---- guest handles of real windows ----
  static bool is_real(uint32_t h) { return h >= kFirst && h < kFirst + kStep * kCount && (h - kFirst) % kStep == 0; }
  uint32_t guest(HWND h);        // 0 for null; registers a window on first sight
  HWND real(uint32_t g) const;   // null when unknown or destroyed
  uint32_t owner_guest() { return owner_ ? guest(owner_) : 0; }

  // What the run showed (for the --configure JSON and exit code).
  int dialogs() const { return dialogs_; }
  int message_boxes() const { return message_boxes_; }
  int file_dialogs() const { return file_dialogs_; }
  int shown() const { return dialogs_ + message_boxes_ + file_dialogs_; }
  bool timed_out() const;

  // ---- dialogs ----
  // A copy of a guest DLGTEMPLATE(EX) at `addr` (its exact size), or empty.
  std::vector<uint8_t> template_at(uint32_t addr) const;
  // RT_DIALOG `name_arg` (a guest string pointer or an ordinal) of module hinst.
  std::vector<uint8_t> template_resource(uint32_t hinst, uint32_t name_arg) const;
  // DialogBox* (modal: the guest's EndDialog result, -1 on failure) and
  // CreateDialog* (modeless: the guest handle, 0 on failure).
  uint32_t run_dialog(std::vector<uint8_t> tmpl, uint32_t hinst, uint32_t parent, uint32_t proc, uint32_t param,
                      bool modal);
  // EndDialog on a real dialog.
  bool end_dialog(uint32_t hdlg, uint32_t result);
  // Runs the message loop while modeless dialogs the guest created are open
  // (after the button handler returned). Returns at once when there are none.
  void pump_modeless();

  // ---- windows the guest creates inside real ones ----
  // CreateWindowExA whose parent is real (or whose class the guest registered
  // and whose parent is real). 0 on failure.
  uint32_t create_window(uint32_t exstyle, const std::string& cls, const std::string& title, uint32_t style, int x,
                         int y, int cx, int cy, uint32_t parent, uint32_t id, uint32_t hinst, uint32_t param);
  uint32_t def_window_proc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

  // ---- messages from the guest to real windows ----
  uint32_t send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
  bool post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

  // ---- window state (GetWindowLong & co. on real windows) ----
  uint32_t get_window_long(uint32_t hwnd, int32_t index);
  uint32_t set_window_long(uint32_t hwnd, int32_t index, uint32_t value);
  std::string window_text(uint32_t hwnd);
  bool set_window_text(uint32_t hwnd, const std::string& text);
  bool show_window(uint32_t hwnd, int cmd);
  bool set_window_pos(uint32_t hwnd, uint32_t after, int x, int y, int cx, int cy, uint32_t flags);

  // ---- DCs of real windows ----
  uint32_t get_dc(uint32_t hwnd);
  bool release_dc(uint32_t hwnd, uint32_t gdc);
  uint32_t begin_paint(uint32_t hwnd, uint32_t ps_guest);
  bool end_paint(uint32_t hwnd, uint32_t ps_guest);

  // ---- timers on real windows ----
  uint32_t set_timer(uint32_t hwnd, uint32_t id, uint32_t ms, uint32_t proc);
  bool kill_timer(uint32_t hwnd, uint32_t id);

  // ---- message boxes and file dialogs (configure mode) ----
  uint32_t message_box(uint32_t owner_g, const std::string& text, const std::string& caption, uint32_t type);
  // GetOpenFileNameA / GetSaveFileNameA on a guest OPENFILENAMEA.
  bool file_dialog(uint32_t ofn, bool save);

  // The window procedure of guest-registered classes and the dialog
  // procedure of guest dialogs (host side).
  static LRESULT CALLBACK class_proc(HWND, UINT, WPARAM, LPARAM);
  static INT_PTR CALLBACK dialog_proc(HWND, UINT, WPARAM, LPARAM);
  static UINT_PTR CALLBACK file_hook_proc(HWND, UINT, WPARAM, LPARAM);

  // (internal state; public for the static procedures' helpers)
  struct Dialog;
  struct Wrap;
  struct Current;

 private:
  LRESULT deliver(uint32_t proc, HWND h, UINT msg, WPARAM wp, LPARAM lp, bool dialog, bool* forwarded);
  uint32_t call_proc(uint32_t proc, HWND h, UINT msg, uint32_t wp, uint32_t lp);
  bool ensure_class(const std::string& name);
  uint32_t wrap_dc(HDC real, int w, int h, int cap_x, int cap_y, int cap_w, int cap_h);
  void unwrap_dc(uint32_t gdc, bool blit_back, const RECT* area);
  HBRUSH real_brush(uint32_t gbrush, uint32_t gdc);
  COLORREF true_color(COLORREF c, uint32_t gdc);
  uint32_t guest_alloc(uint32_t size);
  void guest_free(uint32_t addr);
  HWND owner_for(uint32_t parent_g);
  void force_end(HWND h);
  Dialog* dialog_of(HWND h);
  void fail_with(std::exception_ptr e);
  void rethrow_pending();

  Runtime& rt_;
  bool enabled_ = false;
  HWND owner_ = nullptr;
  ConfigScript* script_ = nullptr;
  std::vector<HWND> handles_;              // index (g - kFirst) / kStep
  std::map<HWND, uint32_t> by_hwnd_;
  std::map<HWND, std::shared_ptr<Dialog>> dialogs_by_hwnd_;
  std::shared_ptr<Dialog> creating_;
  std::map<std::wstring, uint32_t> class_procs_;  // upper-case real class name → guest wndproc
  std::map<HWND, uint32_t> window_procs_;         // guest wndprocs of windows created by the guest
  uint32_t creating_param_ = 0;
  std::map<uint32_t, std::unique_ptr<Wrap>> wraps_;
  std::map<std::pair<HWND, UINT_PTR>, uint32_t> timer_procs_;
  std::map<COLORREF, HBRUSH> brushes_;
  std::vector<std::unique_ptr<Current>> current_;  // host messages being handled (innermost last)
  std::exception_ptr error_;
  int dialogs_ = 0, message_boxes_ = 0, file_dialogs_ = 0;
  uint32_t file_hook_ = 0, file_ofn_ = 0;
};

// The Runtime's RealUi (created disabled on first use).
RealUi& real_ui(Runtime& rt);

}  // namespace adw::win32
