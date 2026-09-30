#include "ne16/imreader.hh"

#include <initializer_list>
#include <stdexcept>

#include "adw/core/log.h"
#include "win16/runtime16.hh"
#include "win16/shims16.hh"

namespace adw::ne16 {

using win16::Arg16;
using win16::GuestError16;
using win16::l16;
using win16::Runtime16;
using win16::w16;

namespace {

// A Win16 API call through its thunk, exactly as the reader's imports reach it.
uint32_t api(Runtime16& rt, const char* module, const char* name, std::initializer_list<Arg16> args) {
  win16::Shim16Entry* e = rt.shims().find_name(module, name);
  if (!e) throw GuestError16(GuestError16::Kind::fatal, std::string("ne16 reader: no shim ") + module + "." + name);
  return rt.call_far(rt.thunk_far(*e), args);
}

// ---- the real IMIMXPLY.IMQ ----------------------------------------------------------------------------------

class ImqReader : public ImReader {
 public:
  ImqReader(Runtime16& rt, uint16_t h, uint32_t proc) : rt_(rt), h_(h), proc_(proc) {}
  const char* name() const override { return "imq"; }
  // PASCAL: the record's far pointer first, then the message (INTRMLIB 1:2060..1:2064).
  uint32_t saver_main(uint32_t info, uint16_t msg) override { return rt_.call_far(proc_, {l16(info), w16(msg)}); }
  uint16_t instance() const override { return h_; }
  void close() override {
    if (!h_) return;
    uint16_t h = h_;
    h_ = 0;
    api(rt_, "KERNEL", "FreeLibrary", {w16(h)});
  }

 private:
  Runtime16& rt_;
  uint16_t h_ = 0;
  uint32_t proc_ = 0;
};

// ---- the native reader --------------------------------------------------------------------------------------
//
// Each case names the IMIMXPLY code it follows (research/win/pkg/swse/dis,
// intermission_protocol.md §5). Its strings live in the system segment, its
// one local that the module sees — the WORD saverinit writes through — in a
// small block of its own (IMIMXPLY's is on its stack).
class NativeReader : public ImReader {
 public:
  NativeReader(Runtime16& rt, uint32_t w) : rt_(rt), w_(w) {}
  const char* name() const override { return "native"; }

  uint32_t saver_main(uint32_t info, uint16_t msg) override {
    const bool preview = rt_.rd8(info + 1) & (iminfo::kPreview >> 8);
    switch (msg) {
      case immsg::kDraw:  // 2:0064
        draw(info, 0);
        return 1;
      case immsg::kStart:  // 2:0090
        if (preview) draw(info, 3);
        draw(info, 1);
        return 1;
      case immsg::kStop:  // 2:00f0
        draw(info, 2);
        if (preview) draw(info, 4);
        return 1;
      case immsg::kRepaint:  // 2:014c
        draw(info, 2);
        draw(info, 1);
        return 1;
      case immsg::kPalette: {  // 2:01a4
        uint32_t pal = rt_.rd32(block(info) + imblock::kPaletteFn);
        if (pal) rt_.call_far(pal, {w16(rt_.rd8(info + iminfo::kPaletteState))});
        return 1;
      }
      case immsg::kQuery:
        return query(info);
      case immsg::kConfigure: {  // 2:02e0
        uint32_t b = block(info);
        uint32_t dlg = rt_.rd32(b + imblock::kDlgProc);
        if (!dlg) return 0;
        api(rt_, "USER", "DialogBox", {w16(rt_.rd16(b + imblock::kLib)), l16(str("DIALOGBOX")),
                                       w16(rt_.rd16(info + iminfo::kHwnd)), l16(dlg)});
        return 1;
      }
      case immsg::kPanel: {  // 2:031e: the result is the dialog's HWND, DX = 0
        uint32_t b = block(info);
        rt_.wr16(w_, 999);
        rt_.call_far(rt_.rd32(b + imblock::kInit), {l16(w_)});
        return uint16_t(api(rt_, "USER", "CreateDialog", {w16(rt_.rd16(b + imblock::kLib)), l16(str("DIALOGBOX")),
                                                          w16(rt_.rd16(info + iminfo::kHwnd)),
                                                          l16(rt_.rd32(b + imblock::kDlgProc2))}));
      }
      case immsg::kLoad:
        return load(info);
      case immsg::kFree:
        return free_module(info);
      default:  // 3, 4 and anything above 11 (2:0543)
        return 1;
    }
  }

 private:
  uint32_t block(uint32_t info) const { return rt_.rd32(info + iminfo::kBlock); }
  uint32_t str(const char* s) { return rt_.static_bytes(std::string("ne16 native reader: ") + s, s); }
  uint32_t proc(uint16_t lib, const char* name) {
    return api(rt_, "KERNEL", "GetProcAddress", {w16(lib), l16(str(name))});
  }

  // saverdraw(+4, +6, hLib, +8, code), the record read at each call.
  void draw(uint32_t info, uint16_t code) {
    uint32_t b = block(info);
    rt_.call_far(rt_.rd32(b + imblock::kDraw), {w16(rt_.rd16(info + iminfo::kHwnd)), w16(rt_.rd16(info + iminfo::kHdc)),
                                                w16(rt_.rd16(b + imblock::kLib)), w16(rt_.rd16(info + iminfo::kPalette)),
                                                w16(code)});
  }

  // 2:01d4..2:02dc.
  uint32_t query(uint32_t info) {
    if (!rt_.rd32(info + iminfo::kPath)) {  // no module: the reader's own type (2:02a6)
      rt_.wr8(info + 1, uint8_t((rt_.rd8(info + 1) | 0x0C) & ~0x10));
      api(rt_, "KERNEL", "lstrcpy", {l16(info + iminfo::kType), l16(str("IMX"))});
      api(rt_, "KERNEL", "lstrcpy", {l16(info + iminfo::kName), l16(str("IMX"))});
      return 1;
    }
    uint32_t b = block(info);
    const uint8_t f0 = rt_.rd8(info);
    rt_.wr8(info, uint8_t(rt_.rd32(b + imblock::kDlgProc2) ? (f0 | 0x04) : (f0 & ~0x04)));
    rt_.wr8(info + 1, uint8_t((rt_.rd8(info + 1) & 0xF3) | 0x10));
    uint16_t w = 0;
    if (uint32_t pal = rt_.rd32(b + imblock::kPaletteFn)) {
      w = uint16_t(rt_.call_far(pal, {w16(0)}));
      if (w == 0x100) w = 0xFE;
      rt_.wr8(info + iminfo::kPaletteState, uint8_t(w));
    }
    rt_.wr16(w_, w);
    uint32_t name = rt_.call_far(rt_.rd32(b + imblock::kInit), {l16(w_)});
    // The module's own string, cut in place (2:025a..2:0269).
    if (int16_t(api(rt_, "KERNEL", "lstrlen", {l16(name)})) > 40) rt_.wr8(name + 40, 0);
    api(rt_, "KERNEL", "lstrcpy", {l16(info + iminfo::kName), l16(name)});
    w = rt_.rd16(w_);
    if (w >= 200) w = uint16_t(w - 200);
    if (w >= 100) w = uint16_t(w - 100);
    rt_.wr8(info + iminfo::kPaletteType, uint8_t(w));
    return 1;
  }

  // 2:0372..2:04f4.
  uint32_t load(uint32_t info) {
    uint32_t path = rt_.rd32(info + iminfo::kPath);
    if (!path) return 1;
    // The file part: back from the NUL to the last '\' (never looking at the first character).
    uint16_t k = uint16_t(api(rt_, "KERNEL", "lstrlen", {l16(path)}));
    while (k != 0 && rt_.rd8(path + k) != '\\') k--;
    if (k != 0) k++;
    auto is = [&](uint16_t i, char c) {
      uint8_t v = rt_.rd8(path + uint32_t(k) + i);
      return v == uint8_t(c) || v == uint8_t(c | 0x20);
    };
    if (is(0, 'I') && is(1, 'M') && is(2, 'X') && is(3, 'X') && rt_.rd8(path + uint32_t(k) + 4) == '_') return 0;
    uint16_t hb = uint16_t(api(rt_, "KERNEL", "GlobalAlloc", {w16(0x0042), l16(imblock::kSize)}));
    uint32_t b = api(rt_, "KERNEL", "GlobalLock", {w16(hb)});
    rt_.wr32(info + iminfo::kBlock, b);
    uint16_t lib = uint16_t(api(rt_, "KERNEL", "LoadLibrary", {l16(path)}));
    rt_.wr16(b + imblock::kLib, lib);
    if (!lib) return 0;  // only 0 counts: a Win16 error code below 32 goes on (and fails below)
    if (proc(lib, "setcurrsaver")) return 0;
    uint32_t p = proc(lib, "saverinit");
    rt_.wr32(b + imblock::kInit, p);
    if (!p) return 0;
    p = proc(lib, "saverdraw");
    rt_.wr32(b + imblock::kDraw, p);
    if (!p) return 0;
    rt_.wr32(b + imblock::kDlgProc, proc(lib, "saverdlgproc"));
    rt_.wr32(b + imblock::kDlgProc2, proc(lib, "saverdlgproc2"));
    rt_.wr32(b + imblock::kPaletteFn, proc(lib, "palette"));
    return 1;
  }

  // 2:04f6..2:053e: +0x55 itself is left as it was.
  uint32_t free_module(uint32_t info) {
    uint32_t b = block(info);
    if (!b) return 1;
    if (uint16_t lib = rt_.rd16(b + imblock::kLib)) api(rt_, "KERNEL", "FreeLibrary", {w16(lib)});
    const uint16_t sel = uint16_t(b >> 16);
    api(rt_, "KERNEL", "GlobalUnlock", {w16(uint16_t(api(rt_, "KERNEL", "GlobalHandle", {w16(sel)})))});
    api(rt_, "KERNEL", "GlobalFree", {w16(uint16_t(api(rt_, "KERNEL", "GlobalHandle", {w16(sel)})))});
    return 1;
  }

  Runtime16& rt_;
  uint32_t w_ = 0;  // far pointer to the WORD saverinit reads and writes
};

}  // namespace

std::unique_ptr<ImReader> open_imq_reader(Runtime16& rt, const std::string& guest_path, std::string* why) {
  uint32_t path = rt.static_bytes("ne16 imq reader: " + guest_path, guest_path);
  uint16_t h = uint16_t(api(rt, "KERNEL", "LoadLibrary", {l16(path)}));
  if (h < 32) {
    *why = "cannot load " + guest_path + " (error " + std::to_string(h) + ")";
    return nullptr;
  }
  uint32_t proc = api(rt, "KERNEL", "GetProcAddress", {w16(h), l16(rt.static_bytes("ne16 imq reader: saverMain", "saverMain"))});
  if (!proc) {
    api(rt, "KERNEL", "FreeLibrary", {w16(h)});
    *why = guest_path + " has no SAVERMAIN";
    return nullptr;
  }
  return std::make_unique<ImqReader>(rt, h, proc);
}

std::unique_ptr<ImReader> open_native_reader(Runtime16& rt, std::string* why) {
  uint16_t h = rt.global().alloc(win16::GlobalHeap16::kZeroInit, 0x10);
  if (!h) {
    *why = "no guest memory for the native reader";
    return nullptr;
  }
  return std::make_unique<NativeReader>(rt, uint32_t(h) << 16);
}

}  // namespace adw::ne16
