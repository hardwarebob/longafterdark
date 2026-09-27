// The small system DLLs: MMSYSTEM (the multimedia clock here; its sound half
// — sndPlaySound, waveOut, midiOut/aux, mixer, MCI — is sound16.cc), WIN87EM
// (the FP emulator's control entry — the CPU has an x87), COMMDLG / KEYBOARD
// / SHELL / TOOLHELP (API_SURFACE.md §2), and register_all16().
//
// Sound (sound16.hh, AUDIO.md §8): without the host audio engine the lane has
// one wave-out device that plays nothing. AD_SND.DLL (ABI.md §3.6)
// initializes only when waveOutGetNumDevs() finds a device (AD_SND 1:1ea6),
// and without it adwLoadSoundResource returns 0 — which LUNATIC, BORIS,
// DOMINOES and others treat as out of memory — so, like the sound card every
// Win95 machine After Dark shipped for had, the device exists: format queries
// succeed and sndPlaySound reports the sound played. MIDI and aux devices:
// none. With the engine on (ADSOUND=1 / ADAUDIOOUT) the sounds play, and a
// MIDI device, two aux devices and the MCI sequencer exist. ADSOUNDDEV=0
// (the lane's knob) removes the wave device either way, for the no-sound-card
// path.
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "adw/core/log.h"
#include "win16/dos16.hh"
#include "win16/input16.hh"
#include "win16/modules16.hh"
#include "win16/shim_families16.hh"

namespace adw::win16 {

void register_system16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- MMSYSTEM ----
  const char* M = "MMSYSTEM";
  r.impl(M, "mmsystemGetVersion", [](Call16& c) { c.ret(0x030A); });
  r.impl(M, "timeGetTime", [](Call16& c) { c.ret32(c.rt.time_ms()); });
  // The sound half: sound16.cc (sndPlaySound, waveOut, midiOut, aux, mixer, MCI).
  register_sound16(rt);

  // ---- WIN87EM: __fpMath (BX = function) ----
  // With OSFIXUPs left as real x87 opcodes nothing calls the emulator's
  // arithmetic; the C runtimes still use this entry to set up and query it.
  r.impl("WIN87EM", "__fpMath", [](Call16& c) {
    auto& rr = c.rt.cpu().registers();
    auto& fpu = c.rt.cpu().fpu();
    switch (rr.r_bx()) {
      case 0:  // install
      case 1:  // initialize
        fpu.reset();
        rr.w_ax(0);
        break;
      case 2:  // deinstall
      case 3:  // set error handler (DX:AX)
      case 10:  // stack depth
        rr.w_ax(0);
        break;
      case 4:  // set control word
        fpu.cw = rr.r_ax();
        break;
      case 5:  // get control word
        rr.w_ax(fpu.cw);
        break;
      case 8: {  // get status word, clear exceptions
        rr.w_ax(fpu.status_word());
        fpu.sw &= uint16_t(~0x80FF);
        break;
      }
      case 11:  // installed?
        rr.w_dx(0);
        rr.w_ax(1);
        break;
      default:
        log("win16: WIN87EM.__fpMath function %u not supported", rr.r_bx());
        rr.w_ax(0);
        break;
    }
  });

  // ---- COMMDLG, KEYBOARD, SHELL ----
  r.impl("COMMDLG", "GetOpenFileName", [](Call16& c) { c.ret(0); });
  r.impl("COMMDLG", "CommDlgExtendedError", [](Call16& c) { c.ret32(0); });
  r.impl("COMMDLG", "GetFileTitle", [](Call16& c) {
    std::string path = c.rt.read_str(c.ptr());
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    size_t s = path.find_last_of("\\/:");
    std::string title = s == std::string::npos ? path : path.substr(s + 1);
    if (n <= title.size()) return c.ret(uint16_t(title.size() + 1));
    c.rt.write_str(buf, title, n);
    c.ret(0);
  });
  // The US keyboard (input16.hh), never the host's layout.
  r.impl("KEYBOARD", "MapVirtualKey", [](Call16& c) {
    uint16_t code = c.w(), type = c.w();
    switch (type) {
      case 0:  // virtual key → scan code
        return c.ret(vk_scan_code(uint8_t(code)));
      case 1:  // scan code → virtual key
        for (int vk = 1; vk < 256; vk++) {
          if (vk_scan_code(uint8_t(vk)) == code && !vk_extended(uint8_t(vk))) return c.ret(uint16_t(vk));
        }
        return c.ret(0);
      case 2: {  // virtual key → unshifted character
        int ch = vk_to_char(uint8_t(code), false, false);
        return c.ret(ch < 0 ? 0 : uint16_t(ch >= 'a' && ch <= 'z' ? ch - 32 : ch));
      }
      default:
        return c.ret(0);
    }
  });
  r.impl("KEYBOARD", "VkKeyScan", [](Call16& c) {
    uint8_t ch = uint8_t(c.w());
    for (int shift = 0; shift < 2; shift++) {
      for (int vk = 1; vk < 256; vk++) {
        if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) continue;  // the main block's key first
        if (vk_to_char(uint8_t(vk), shift != 0, false) == ch) return c.ret(uint16_t(vk | (shift << 8)));
      }
    }
    c.ret(0xFFFF);
  });
  r.impl("KEYBOARD", "OemToAnsi", [](Call16& c) {
    uint32_t src = c.ptr(), dst = c.ptr();
    std::string s = c.rt.read_str(src);
    c.rt.write_bytes(dst, s.c_str(), s.size() + 1);
    c.ret(1);
  });
  // ---- TOOLHELP ----
  // GlobalEntryModule(lpGlobal, hModule, wSeg): ADTOOL's BADTOOLSTUFF /
  // BADTOOLUNSTUFF walk segments 1..99 of a module and save/restore the
  // GT_DATA ones (the module's non-automatic data segments) by
  // dwAddress/dwBlockSize/hBlock.
  r.impl("TOOLHELP", "GlobalEntryModule", [](Call16& c) {
    uint32_t ge = c.ptr();
    uint16_t hmod = c.w(), seg = c.w();
    Module16* m = c.rt.modules().by_handle(hmod);
    if (!ge || !m || m->system || c.rt.rd32(ge) < 36 || seg == 0 || seg > m->seg_sel.size()) return c.ret(0);
    GlobalBlock* b = c.rt.global().find(m->seg_sel[seg - 1]);
    if (!b) return c.ret(0);
    bool is_data = m->image->segment(seg).is_data();
    uint16_t type = m->seg_sel[seg - 1] == m->dgroup ? 1 /*GT_DGROUP*/ : is_data ? 2 /*GT_DATA*/ : 3 /*GT_CODE*/;
    c.rt.wr32(ge + 4, b->base);
    c.rt.wr32(ge + 8, b->size);
    c.rt.wr16(ge + 12, b->handle());
    c.rt.wr16(ge + 14, b->locks);
    c.rt.wr16(ge + 16, 0);
    c.rt.wr16(ge + 18, 0);
    c.rt.wr16(ge + 20, uint16_t(type == 1 && c.rt.local().has_heap(b->sel)));
    c.rt.wr16(ge + 22, m->hmodule);
    c.rt.wr16(ge + 24, type);
    c.rt.wr16(ge + 26, seg);
    c.rt.wr32(ge + 28, 0);
    c.rt.wr32(ge + 32, 0);
    c.ret(1);
  });

  r.impl("SHELL", "RegSetValue", [](Call16& c) { c.ret32(0); });
  r.impl("SHELL", "DragQueryFile", [](Call16& c) { c.ret(0); });
  r.impl("SHELL", "DragFinish", [](Call16&) {});
}

void register_all16(Runtime16& rt) {
  register_dos(rt);
  register_kernel16(rt);
  register_user16(rt);
  register_gdi16(rt);
  register_system16(rt);
}

}  // namespace adw::win16
