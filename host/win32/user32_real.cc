// USER32 / COMDLG32 on real windows (INTERACTION.md §6.2, realui.hh).
//
// Registered after the emulated families: every handler that takes a window
// handle first checks for a real-window guest handle (0xA000 + 4·i) and
// acts on the real window; everything else falls through to the emulated
// handler unchanged, so the saver's behaviour (and its FBHASH streams) is
// untouched. In configure mode (RealUi enabled) the dialog, message-box and
// file-dialog entry points become real; outside it they keep refusing.
//
// Also declared here, for GetProcAddress and future modules (none of our
// corpus imports them): DialogBoxParamW, DialogBoxIndirectParamA/W,
// CreateDialogParamA, CreateDialogIndirectParamA, GetDlgItemTextA,
// SetDlgItemInt, GetDlgItemInt, CheckRadioButton, MessageBeep and
// COMDLG32!GetSaveFileNameA.
#include <windows.h>

#include <functional>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win32/config_script.hh"
#include "win32/realui.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"

namespace adw::win32 {

namespace {

constexpr const char* U = "USER32.DLL";

// Replaces `name`'s handler with `first`, which returns true when it handled
// the call; otherwise the previous handler runs (or the call returns 0).
void wrap(ShimRegistry& r, const char* dll, const char* name, std::function<bool(Call&)> first) {
  ShimEntry* e = r.find(dll, name);
  ShimFn old = e ? e->fn : ShimFn();
  auto fn = [old, first](Call& c) {
    if (first(c)) return;
    if (old) old(c);
    else c.ret(0);
  };
  if (e) r.impl(dll, name, fn);
}

RealUi& ui(Call& c) { return real_ui(c.rt); }
bool real_arg(Call& c, int i) { return RealUi::is_real(c.arg(i)); }
HWND hw(Call& c, int i) { return ui(c).real(c.arg(i)); }

std::wstring from_ansi(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(1252, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// SetPropA's key: a string, or an atom in the low word.
std::wstring prop_key(Call& c, int i) {
  uint32_t p = c.arg(i);
  if (p < 0x10000) return L"#adw" + std::to_wstring(p);
  return from_ansi(c.str(i));
}

// A dialog entry point: modal returns the guest's result (-1 on failure),
// modeless the dialog handle (0).
void dialog_call(Call& c, bool modal, bool indirect, bool wide_name) {
  RealUi& u = ui(c);
  uint32_t hinst = c.arg(0), tmpl_arg = c.arg(1), parent = c.arg(2), proc = c.arg(3), param = c.arg(4);
  if (!u.enabled()) {
    trace("user", "%s refused (configuration dialogs are not shown)", c.fn.name.c_str());
    return c.ret(modal ? IDCANCEL : 0);
  }
  std::vector<uint8_t> tmpl;
  if (indirect) {
    tmpl = u.template_at(tmpl_arg);
  } else if (wide_name && tmpl_arg >= 0x10000) {
    // A wide resource name: look it up by its ANSI spelling.
    std::u16string w = read_wstr(c.mem(), tmpl_arg);
    std::string a(w.begin(), w.end());
    uint32_t tmp = c.rt.heap().alloc(uint32_t(a.size()) + 1, true);
    write_cstr(c.mem(), tmp, a, a.size() + 1);
    tmpl = u.template_resource(hinst, tmp);
    c.rt.heap().free(tmp);
  } else {
    tmpl = u.template_resource(hinst, tmpl_arg);
  }
  if (tmpl.empty()) {
    log("%s: the dialog template was not found", c.fn.name.c_str());
    c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
    return c.ret(modal ? 0xFFFFFFFF : 0);
  }
  c.ret(u.run_dialog(std::move(tmpl), hinst, parent, proc, param, modal));
}

}  // namespace

void register_user32_real(ShimRegistry& r) {
  // ---- dialogs ----
  wrap(r, U, "DialogBoxParamA", [](Call& c) {
    dialog_call(c, true, false, false);
    return true;
  });
  r.add(U, "DialogBoxParamW", Conv::stdcall_, 20, [](Call& c) { dialog_call(c, true, false, true); });
  r.add(U, "DialogBoxIndirectParamA", Conv::stdcall_, 20, [](Call& c) { dialog_call(c, true, true, false); });
  r.add(U, "DialogBoxIndirectParamW", Conv::stdcall_, 20, [](Call& c) { dialog_call(c, true, true, false); });
  r.add(U, "CreateDialogParamA", Conv::stdcall_, 20, [](Call& c) { dialog_call(c, false, false, false); });
  r.add(U, "CreateDialogIndirectParamA", Conv::stdcall_, 20, [](Call& c) { dialog_call(c, false, true, false); });
  wrap(r, U, "EndDialog", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).end_dialog(c.arg(0), c.arg(1)));
    return true;
  });

  // ---- dialog items ----
  wrap(r, U, "GetDlgItem", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND d = hw(c, 0);
    c.ret(d ? ui(c).guest(GetDlgItem(d, c.iarg(1))) : 0);
    return true;
  });
  wrap(r, U, "SendDlgItemMessageA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND d = hw(c, 0);
    HWND item = d ? GetDlgItem(d, c.iarg(1)) : nullptr;
    c.ret(item ? ui(c).send(ui(c).guest(item), c.arg(2), c.arg(3), c.arg(4)) : 0);
    return true;
  });
  wrap(r, U, "SetDlgItemTextA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND d = hw(c, 0);
    c.ret_bool(d && SetDlgItemTextW(d, c.iarg(1), from_ansi(c.str(2)).c_str()));
    return true;
  });
  r.add(U, "GetDlgItemTextA", Conv::stdcall_, 16, [](Call& c) {
    // (hDlg, id, buf, max)
    HWND d = hw(c, 0);
    HWND item = d ? GetDlgItem(d, c.iarg(1)) : nullptr;
    if (!item || !c.arg(2) || !c.arg(3)) {
      if (c.arg(2) && c.arg(3)) c.mem().write_u8(c.arg(2), 0);
      return c.ret(0);
    }
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(2), ui(c).window_text(ui(c).guest(item)), c.arg(3))));
  });
  r.add(U, "SetDlgItemInt", Conv::stdcall_, 16, [](Call& c) {
    HWND d = hw(c, 0);
    c.ret_bool(d && SetDlgItemInt(d, c.iarg(1), c.arg(2), c.arg(3) != 0));
  });
  r.add(U, "GetDlgItemInt", Conv::stdcall_, 16, [](Call& c) {
    // (hDlg, id, BOOL* ok, signed)
    HWND d = hw(c, 0);
    BOOL ok = FALSE;
    UINT v = d ? GetDlgItemInt(d, c.iarg(1), &ok, c.arg(3) != 0) : 0;
    if (c.arg(2)) c.mem().write_u32l(c.arg(2), ok ? 1 : 0);
    c.ret(v);
  });
  wrap(r, U, "CheckDlgButton", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND d = hw(c, 0);
    c.ret_bool(d && CheckDlgButton(d, c.iarg(1), c.arg(2)));
    return true;
  });
  wrap(r, U, "IsDlgButtonChecked", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND d = hw(c, 0);
    c.ret(d ? IsDlgButtonChecked(d, c.iarg(1)) : 0);
    return true;
  });
  r.add(U, "CheckRadioButton", Conv::stdcall_, 16, [](Call& c) {
    HWND d = hw(c, 0);
    c.ret_bool(d && CheckRadioButton(d, c.iarg(1), c.iarg(2), c.iarg(3)));
  });
  r.add(U, "MessageBeep", Conv::stdcall_, 4, [](Call& c) {
    RealUi& u = ui(c);
    if (u.enabled() && !(u.script() && u.script()->hidden())) MessageBeep(c.arg(0));
    c.ret(1);
  });

  // ---- messages ----
  wrap(r, U, "SendMessageA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).send(c.arg(0), c.arg(1), c.arg(2), c.arg(3)));
    return true;
  });
  wrap(r, U, "PostMessageA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).post(c.arg(0), c.arg(1), c.arg(2), c.arg(3)));
    return true;
  });
  wrap(r, U, "DefWindowProcA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).def_window_proc(c.arg(0), c.arg(1), c.arg(2), c.arg(3)));
    return true;
  });
  wrap(r, U, "MessageBoxA", [](Call& c) {
    RealUi& u = ui(c);
    if (!u.enabled()) return false;
    c.ret(u.message_box(c.arg(0), c.str(1), c.str(2), c.arg(3)));
    return true;
  });

  // ---- windows ----
  wrap(r, U, "CreateWindowExA", [](Call& c) {
    // (exstyle, class, title, style, x, y, cx, cy, parent, menu/id, hinst, param)
    if (!real_arg(c, 8)) return false;
    std::string cls = guest_class_arg(c.rt, c.arg(1));
    c.ret(ui(c).create_window(c.arg(0), cls, c.str(2), c.arg(3), c.iarg(4), c.iarg(5), c.iarg(6), c.iarg(7),
                              c.arg(8), c.arg(9), c.arg(10), c.arg(11)));
    return true;
  });
  wrap(r, U, "DestroyWindow", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret_bool(h && DestroyWindow(h));
    return true;
  });
  wrap(r, U, "GetWindowTextA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    if (!c.arg(1) || !c.arg(2)) return c.ret(0), true;
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), ui(c).window_text(c.arg(0)), c.arg(2))));
    return true;
  });
  wrap(r, U, "SetWindowTextA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).set_window_text(c.arg(0), c.str(1)));
    return true;
  });
  wrap(r, U, "ShowWindow", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).show_window(c.arg(0), c.iarg(1)));
    return true;
  });
  wrap(r, U, "IsWindowVisible", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret_bool(h && IsWindowVisible(h));
    return true;
  });
  wrap(r, U, "UpdateWindow", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret_bool(h && UpdateWindow(h));
    return true;
  });
  wrap(r, U, "InvalidateRect", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    RECT rc{};
    if (c.arg(1)) rc = c.pod<RECT>(1);
    c.ret_bool(h && InvalidateRect(h, c.arg(1) ? &rc : nullptr, c.arg(2) != 0));
    return true;
  });
  wrap(r, U, "EnableWindow", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret_bool(h && EnableWindow(h, c.arg(1) != 0));
    return true;
  });
  wrap(r, U, "SetFocus", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret(h ? ui(c).guest(SetFocus(h)) : 0);
    return true;
  });
  wrap(r, U, "GetParent", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret(h ? ui(c).guest(GetParent(h)) : 0);
    return true;
  });
  wrap(r, U, "GetWindowLongA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).get_window_long(c.arg(0), c.iarg(1)));
    return true;
  });
  wrap(r, U, "SetWindowLongA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).set_window_long(c.arg(0), c.iarg(1), c.arg(2)));
    return true;
  });
  wrap(r, U, "GetWindowRect", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    RECT rc{};
    if (!h || !GetWindowRect(h, &rc)) return c.ret(0), true;
    write_pod(c.mem(), c.arg(1), rc);
    c.ret(1);
    return true;
  });
  wrap(r, U, "GetClientRect", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    RECT rc{};
    if (!h || !GetClientRect(h, &rc)) return c.ret(0), true;
    write_pod(c.mem(), c.arg(1), rc);
    c.ret(1);
    return true;
  });
  wrap(r, U, "MoveWindow", [](Call& c) {
    // (hwnd, x, y, cx, cy, repaint)
    if (!real_arg(c, 0)) return false;
    uint32_t flags = SWP_NOZORDER | SWP_NOACTIVATE | (c.arg(5) ? 0 : SWP_NOREDRAW);
    c.ret_bool(ui(c).set_window_pos(c.arg(0), 0, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4), flags));
    return true;
  });
  wrap(r, U, "SetWindowPos", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).set_window_pos(c.arg(0), c.arg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.iarg(5), c.arg(6)));
    return true;
  });
  wrap(r, U, "SetPropA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret_bool(h && SetPropW(h, prop_key(c, 1).c_str(), reinterpret_cast<HANDLE>(uintptr_t(c.arg(2)))));
    return true;
  });
  wrap(r, U, "GetPropA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret(h ? uint32_t(uintptr_t(GetPropW(h, prop_key(c, 1).c_str()))) : 0);
    return true;
  });
  wrap(r, U, "RemovePropA", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    HWND h = hw(c, 0);
    c.ret(h ? uint32_t(uintptr_t(RemovePropW(h, prop_key(c, 1).c_str()))) : 0);
    return true;
  });
  wrap(r, U, "GetActiveWindow", [](Call& c) {
    RealUi& u = ui(c);
    if (!u.enabled()) return false;
    HWND a = GetActiveWindow();
    c.ret(a ? u.guest(a) : u.owner_guest());
    return true;
  });

  // ---- painting ----
  wrap(r, U, "BeginPaint", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).begin_paint(c.arg(0), c.arg(1)));
    return true;
  });
  wrap(r, U, "EndPaint", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).end_paint(c.arg(0), c.arg(1)));
    return true;
  });
  wrap(r, U, "GetDC", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).get_dc(c.arg(0)));
    return true;
  });
  wrap(r, U, "ReleaseDC", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).release_dc(c.arg(0), c.arg(1)));
    return true;
  });

  // ---- timers ----
  wrap(r, U, "SetTimer", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret(ui(c).set_timer(c.arg(0), c.arg(1), c.arg(2), c.arg(3)));
    return true;
  });
  wrap(r, U, "KillTimer", [](Call& c) {
    if (!real_arg(c, 0)) return false;
    c.ret_bool(ui(c).kill_timer(c.arg(0), c.arg(1)));
    return true;
  });

  // ---- COMDLG32 ----
  wrap(r, "COMDLG32.DLL", "GetOpenFileNameA", [](Call& c) {
    RealUi& u = ui(c);
    if (!u.enabled()) return false;
    c.ret_bool(u.file_dialog(c.arg(0), false));
    return true;
  });
  r.add("COMDLG32.DLL", "GetSaveFileNameA", Conv::stdcall_, 4, [](Call& c) {
    RealUi& u = ui(c);
    c.ret_bool(u.enabled() && u.file_dialog(c.arg(0), true));
  });
}

}  // namespace adw::win32
