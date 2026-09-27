// KERNEL — memory (global + local heaps), modules and resources, files,
// profile strings, strings, time-independent process services, Catch/Throw,
// SwitchStackTo/Back, DOS3Call (API_SURFACE.md §2 "KERNEL", 80 imports).
//
// Behaviour notes:
//   * GetVersion: Windows "3.95" on DOS 7.00 — what Win16 code saw on Windows
//     95; GetWinFlags: protected + enhanced mode, 486, x87.
//   * Profiles go through the shared store (win32/ini_store.hh, INTERACTION.md
//     §7.3): the seeds register_dos made (MODULES.INI's per-install settings,
//     WIN.INI's [Berkeley Systems], which points the AD data/INI directories
//     at the guest install directory, as the installer wrote it) under the
//     file; writes land in the upper layer of the file's overlay — the
//     per-user state with ADSTATE, else memory, so a headless run starts from
//     the same settings every time.
//   * MakeProcInstance returns the procedure itself: every callback a Classic
//     DLL hands out is an exported entry whose prolog loads DGROUP (prolog
//     patching, modules16.hh), which is also what Windows did for DLLs.
//   * Files go through DosFiles (dos16.hh) over the Vfs overlays: reads from
//     the upper or lower layer, writes copied up.
//   * GetCurrentTask: DX is the first task of the task list, this one
//     (NONSENSE walks the list for the Notepad it started).
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// needs more; API_SURFACE.md §2 KERNEL):
//   * GetTempFileName creates nothing on disk (C:\WINDOWS\TEMP is an in-memory
//     overlay; the file appears on first open for writing).
//   * WinExec refuses (error 2), except "notepad <file>" in configure mode
//     (dialogs16.cc).
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <vector>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win16/dos16.hh"
#include "win16/modules16.hh"
#include "win16/shim_families16.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

namespace adw::win16 {

namespace {

constexpr const char* K = "KERNEL";
using SegReg = cpu::X86Emulator::SegReg;

constexpr uint16_t kWinFlags = 0x0001 | 0x0020 | 0x0008 | 0x0400;  // PMODE | ENHANCED | CPU486 | 80x87
constexpr uint16_t kHfileError = 0xFFFF;

// ---- profiles ---------------------------------------------------------------------------------------

std::string trim(std::string s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  size_t e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

struct KernelState : RuntimeState16 {
  explicit KernelState(Runtime16& rt) : rt(rt) {}
  Runtime16& rt;
  uint16_t error_mode = 0;
  uint16_t task = 0;
  // Resources: HRSRC = index + 1 into this table. Each row keeps its image
  // alive: the module may be freed (and another loaded at the same address)
  // while the guest still holds an HRSRC or a loaded copy.
  struct Res {
    std::shared_ptr<loader::ne::Image> image;
    std::string module;  // for traces
    const loader::ne::Resource* res;
    uint16_t hglobal = 0;
  };
  std::vector<Res> resources;
  // SwitchStackTo/Back.
  struct StackSwitch {
    uint16_t ss = 0, sp = 0, bp = 0, frame = 0;
  };
  std::vector<StackSwitch> stack_switches;

};

KernelState& ks(Runtime16& rt) { return rt.state<KernelState>(); }

std::string profile_path(Runtime16& rt, const std::string& name) {
  if (name.find_first_of("\\/:") == std::string::npos) return upper16(rt.options().windows_dir + "\\" + name);
  return upper16(rt.vfs().full_path(name));
}

// Writes a NUL-separated, double-NUL-terminated list; returns the characters
// copied excluding the final NUL (size-2 when truncated).
uint16_t write_list(Runtime16& rt, uint32_t buf, uint16_t size, const std::vector<std::string>& items) {
  if (!buf || size < 2) {
    if (buf && size) rt.wr8(buf, 0);
    return 0;
  }
  std::string out;
  for (const std::string& s : items) {
    out += s;
    out.push_back('\0');
  }
  if (out.size() + 1 > size) {
    out.resize(size - 2);
    out.push_back('\0');
    rt.write_bytes(buf, out.data(), out.size());
    rt.wr8(buf + uint32_t(out.size()), 0);
    return uint16_t(size - 2);
  }
  if (out.empty()) out.push_back('\0');
  rt.write_bytes(buf, out.data(), out.size());
  rt.wr8(buf + uint32_t(out.size()), 0);
  return uint16_t(out.size() == 1 && items.empty() ? 0 : out.size() - 1);
}

// Profiles go through the shared store (win32/ini_store.hh): the seeds
// register_dos made (MODULES.INI's per-install settings, WIN.INI's [Berkeley
// Systems]) under the file, the file wins per key, writes land in the upper
// layer of the file's overlay (the per-user state, or memory).
uint16_t get_profile_string(Runtime16& rt, const std::string& file, uint32_t app, uint32_t key, uint32_t def,
                            uint32_t buf, uint16_t size) {
  win32::IniStore& ini = profiles16(rt);
  std::string path = profile_path(rt, file);
  if (!app) return write_list(rt, buf, size, ini.sections(path));
  std::string section = rt.read_str(app);
  if (!key) return write_list(rt, buf, size, ini.keys(path, section));
  std::string k = rt.read_str(key);
  std::optional<std::string> v = ini.get(path, section, k);
  bool found = v.has_value();
  std::string value = found ? *v : trim(rt.read_str(def));
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
    value = value.substr(1, value.size() - 2);
  }
  trace("file16", "GetProfileString %s [%s] %s -> \"%s\"%s", file.c_str(), section.c_str(), k.c_str(), value.c_str(),
        found ? "" : " (default)");
  if (!buf || !size) return 0;
  return uint16_t(rt.write_str(buf, value, size));
}

int32_t get_profile_int(Runtime16& rt, const std::string& file, uint32_t app, uint32_t key, int16_t def) {
  std::string section = rt.read_str(app), k = rt.read_str(key);
  trace("file16", "GetProfileInt %s [%s] %s (default %d)", file.c_str(), section.c_str(), k.c_str(), def);
  std::optional<std::string> v = profiles16(rt).get(profile_path(rt, file), section, k);
  if (!v) return def;
  // Leading digits only, as Windows parses it; a non-number reads as 0.
  int32_t n = 0;
  size_t i = 0;
  bool neg = false;
  if (i < v->size() && ((*v)[i] == '-' || (*v)[i] == '+')) neg = (*v)[i++] == '-';
  while (i < v->size() && isdigit(uint8_t((*v)[i]))) n = n * 10 + ((*v)[i++] - '0');
  return neg ? -n : n;
}

bool write_profile_string(Runtime16& rt, const std::string& file, uint32_t app, uint32_t key, uint32_t value) {
  if (!app) return true;  // Win16: a NULL section flushes the cache (nothing to do)
  std::string path = profile_path(rt, file);
  std::string section = rt.read_str(app);
  std::optional<std::string> k, v;
  if (key) k = rt.read_str(key);
  if (key && value) v = rt.read_str(value);
  bool ok = profiles16(rt).set(path, section, k ? std::optional<std::string_view>(*k) : std::nullopt,
                               v ? std::optional<std::string_view>(*v) : std::nullopt);
  trace("file16", "WriteProfileString %s [%s] %s%s", path.c_str(), section.c_str(), k ? k->c_str() : "(section)",
        ok ? "" : ": not written");
  return ok;
}

// ---- files ---------------------------------------------------------------------------------------------

// OpenFile's search for a bare name: current, Windows and System directories,
// then where the modules live.
std::string search_file(Runtime16& rt, const std::string& name) {
  DosFiles& d = rt.state<DosFiles>();
  if (name.find_first_of("\\/:") != std::string::npos) return rt.vfs().full_path(name);
  const Runtime16Options& o = rt.options();
  for (const std::string& dir : {rt.vfs().cwd(), o.windows_dir, o.system_dir, o.guest_dir}) {
    std::string p = rt.vfs().full_path(dir + "\\" + name);
    if (d.exists(p)) return p;
  }
  return rt.vfs().full_path(name);
}

uint16_t open_file(Call16& c) {
  Runtime16& rt = c.rt;
  uint32_t name_p = c.ptr();
  uint32_t of = c.ptr();
  uint16_t style = c.w();
  DosFiles& d = rt.state<DosFiles>();
  std::string name = (style & 0x8000) && of ? rt.read_str(of + 8) : rt.read_str(name_p);
  std::string path = (style & 0x1000) ? rt.vfs().full_path(name) : search_file(rt, name);
  auto set_of = [&](uint16_t err) {
    if (!of) return;
    rt.wr8(of, 136);
    rt.wr8(of + 1, 1);
    rt.wr16(of + 2, err);
    rt.wr32(of + 4, 0);
    rt.write_str(of + 8, upper16(path), 128);
  };
  if (style & 0x0100) {  // OF_PARSE
    set_of(0);
    return 0;
  }
  if (style & 0x0200) {  // OF_DELETE
    bool ok = d.remove(path) == 0;
    set_of(ok ? 0 : doserr::kAccessDenied);
    return ok ? 1 : kHfileError;
  }
  int h;
  if (style & 0x1000) h = d.open(path, 2, true);  // OF_CREATE
  else h = d.open(path, style & 3, false);
  if (h < 0) {
    set_of(uint16_t(-h));
    return kHfileError;
  }
  set_of(0);
  if (style & 0x4000) {  // OF_EXIST: open and close again
    d.close(uint16_t(h));
    return 1;
  }
  return uint16_t(h);
}

// Huge copies (hmemcpy): tile by tile.
void huge_copy(Runtime16& rt, uint32_t dst, uint32_t src, uint32_t n) {
  std::vector<uint8_t> buf;
  while (n) {
    uint32_t chunk = std::min({n, 0x10000 - (dst & 0xFFFF), 0x10000 - (src & 0xFFFF)});
    buf.resize(chunk);
    rt.read_bytes(src, buf.data(), chunk);
    rt.write_bytes(dst, buf.data(), chunk);
    dst = Runtime16::huge_add(dst, chunk);
    src = Runtime16::huge_add(src, chunk);
    n -= chunk;
  }
}

Module16* caller_module(Call16& c) { return c.rt.modules().containing(c.ret_cs()); }

}  // namespace

// ---- shared helpers ------------------------------------------------------------------------------------

uint16_t caller_ds(Call16& c) { return c.rt.cpu().get_segment(SegReg::DS); }

std::string upper16(std::string_view s) {
  std::string u(s);
  for (char& ch : u) ch = char(toupper(uint8_t(ch)));
  return u;
}

bool ieq16(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (toupper(uint8_t(a[i])) != toupper(uint8_t(b[i]))) return false;
  }
  return true;
}

loader::ResId res_id(Runtime16& rt, uint32_t fp) {
  if ((fp >> 16) == 0) return loader::ResId::of(uint16_t(fp));
  std::string s = rt.read_str(fp);
  if (!s.empty() && s[0] == '#') return loader::ResId::of(uint16_t(atoi(s.c_str() + 1)));
  return loader::ResId::of(s);
}

// ---- registration ------------------------------------------------------------------------------------

void register_kernel16(Runtime16& rt) {
  Shim16Registry& r = rt.shims();

  // ---- process ----
  r.add(K, 1, "FatalExit", Conv16::pascal_, true, 2, [](Call16& c) {
    uint16_t code = c.w();
    throw GuestError16(GuestError16::Kind::exit, "FatalExit(" + std::to_string(code) + ") from " +
                                                     c.rt.describe(c.ret_cs(), c.ret_ip()));
  });
  r.impl(K, "FatalAppExit", [](Call16& c) {
    c.w();
    std::string msg = c.rt.read_str(c.ptr());
    throw GuestError16(GuestError16::Kind::exit, "FatalAppExit: " + msg);
  });
  // Windows 3.95 on DOS 7.00: AX = 0x5F03, DX = 0x0700.
  r.impl(K, "GetVersion", [](Call16& c) { c.ret32(0x07005F03); });
  r.impl(K, "GetWinFlags", [](Call16& c) { c.ret32(kWinFlags); });
  r.impl(K, "InitTask", [](Call16& c) { c.rt.cpu().registers().w_ax(1); });
  r.impl(K, "GetCurrentTask", [](Call16& c) {
    KernelState& s = ks(c.rt);
    if (!s.task) {
      // A task database block: the handle only has to be a stable, valid selector.
      if (GlobalBlock* b = c.rt.global().alloc_block(0x200, false, 0, 0)) {
        b->owner = 0xFFFF;
        s.task = b->sel;
        c.rt.ldt().set_tag(b->sel, "task database");
        c.rt.mem().write_u16l(b->base + 0xFA, 0x4454);  // 'TD' signature at TDB+0xFA
      }
    }
    // DX: the first task of the task list (TDB+0 links the next, +1Ch is the
    // task's hInstance): NONSENSE walks it to find the Notepad it started.
    // This task is the only one; nothing follows it.
    c.ret32((uint32_t(s.task) << 16) | s.task);
  });
  r.impl(K, "IsTask", [](Call16& c) { c.ret_bool(c.w() == ks(c.rt).task && ks(c.rt).task); });
  r.impl(K, "SetErrorMode", [](Call16& c) {
    uint16_t m = c.w();
    uint16_t old = ks(c.rt).error_mode;
    ks(c.rt).error_mode = m;
    c.ret(old);
  });
  r.impl(K, "OutputDebugString", [](Call16& c) {
    std::string s = c.rt.read_str(c.ptr());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    trace("debug16", "OutputDebugString: %s", s.c_str());
  });
  r.impl(K, "DebugBreak", [](Call16&) {});
  r.impl(K, "WinExec", [](Call16& c) {
    std::string cmd = c.rt.read_str(c.ptr());
    c.w();
    log("win16: WinExec(\"%s\") refused", cmd.c_str());
    c.ret(2);
  });
  r.impl(K, "GetDOSEnvironment", [](Call16& c) { c.ret32(uint32_t(c.rt.sys_sel()) << 16 | layout::kSysEnvironment); });
  r.impl(K, "DOS3Call", [](Call16& c) { dos_int21(c.rt); });
  // OLDMOD16's DLLENTRYPOINT returns whatever this does; the flat-thunk
  // plumbing it connects is replaced by the lane (ABI.md §3.2).
  r.impl(K, "ThunkConnect16", [](Call16& c) { c.ret32(1); });

  // ---- global memory ----
  r.impl(K, "GlobalAlloc", [](Call16& c) {
    uint16_t flags = c.w();
    uint32_t size = c.l();
    c.ret(c.rt.global().alloc(flags, size));
  });
  r.impl(K, "GlobalReAlloc", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t size = c.l();
    uint16_t flags = c.w();
    c.ret(c.rt.global().realloc(h, size, flags));
  });
  r.impl(K, "GlobalFree", [](Call16& c) { c.ret(c.rt.global().free(c.w())); });
  r.impl(K, "GlobalLock", [](Call16& c) { c.ret32(c.rt.global().lock(c.w())); });
  r.impl(K, "GlobalUnlock", [](Call16& c) { c.ret_bool(c.rt.global().unlock(c.w())); });
  r.impl(K, "GlobalSize", [](Call16& c) { c.ret32(c.rt.global().size(c.w())); });
  r.impl(K, "GlobalHandle", [](Call16& c) { c.ret32(c.rt.global().handle(c.w())); });
  r.impl(K, "GlobalFlags", [](Call16& c) { c.ret(c.rt.global().flags(c.w())); });
  r.impl(K, "GlobalCompact", [](Call16& c) {
    c.l();
    c.ret32(std::min<uint32_t>(c.rt.arena().largest_free(), 0x00FF0000));
  });
  r.impl(K, "GetFreeSpace", [](Call16& c) {
    c.w();
    c.ret32(c.rt.arena().bytes_free());
  });
  r.impl(K, "LockSegment", [](Call16& c) {
    uint16_t s = c.w();
    c.ret(s == 0xFFFF ? caller_ds(c) : s);
  });
  r.impl(K, "UnlockSegment", [](Call16& c) {
    uint16_t s = c.w();
    c.ret(s == 0xFFFF ? caller_ds(c) : s);
  });
  r.impl(K, "GlobalPageLock", [](Call16& c) {
    c.w();
    c.ret(1);
  });
  r.impl(K, "GlobalPageUnlock", [](Call16& c) {
    c.w();
    c.ret(0);
  });
  r.impl(K, "GlobalLRUOldest", [](Call16& c) { c.ret(c.w()); });
  r.impl(K, "hmemcpy", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr(), n = c.l();
    huge_copy(c.rt, dst, src, n);
  });

  // ---- local memory (the caller's DS) ----
  r.impl(K, "LocalInit", [](Call16& c) {
    uint16_t seg = c.w(), start = c.w(), end = c.w();
    c.ret_bool(c.rt.local().init(seg ? seg : caller_ds(c), start, end));
  });
  r.impl(K, "LocalAlloc", [](Call16& c) {
    uint16_t flags = c.w(), size = c.w();
    uint16_t h = c.rt.local().alloc(caller_ds(c), flags, size);
    c.rt.cpu().registers().w_cx(h);
    c.ret(h);
  });
  r.impl(K, "LocalReAlloc", [](Call16& c) {
    uint16_t h = c.w(), size = c.w(), flags = c.w();
    c.ret(c.rt.local().realloc(caller_ds(c), h, size, flags));
  });
  r.impl(K, "LocalFree", [](Call16& c) { c.ret(c.rt.local().free(caller_ds(c), c.w())); });
  r.impl(K, "LocalLock", [](Call16& c) {
    uint16_t p = c.rt.local().lock(caller_ds(c), c.w());
    c.ret32(p ? (uint32_t(caller_ds(c)) << 16) | p : 0);
  });
  r.impl(K, "LocalUnlock", [](Call16& c) { c.ret_bool(c.rt.local().unlock(caller_ds(c), c.w())); });
  r.impl(K, "LocalSize", [](Call16& c) { c.ret(c.rt.local().size(caller_ds(c), c.w())); });
  r.impl(K, "LocalHandle", [](Call16& c) { c.ret(c.rt.local().handle(caller_ds(c), c.w())); });
  r.impl(K, "LocalFlags", [](Call16& c) { c.ret(c.rt.local().flags(caller_ds(c), c.w())); });
  r.impl(K, "LocalCompact", [](Call16& c) {
    c.w();
    c.ret(c.rt.local().compact(caller_ds(c)));
  });

  // ---- modules ----
  r.impl(K, "LoadLibrary", [](Call16& c) {
    std::string name = c.rt.read_str(c.ptr());
    uint16_t err = 0;
    Module16* m = c.rt.modules().load(name, &err);
    trace("mod16", "LoadLibrary(\"%s\") -> %s", name.c_str(), m ? m->name.c_str() : "failed");
    c.ret(m ? m->hinstance : (err ? err : 2));
  });
  r.impl(K, "FreeLibrary", [](Call16& c) {
    Module16* m = c.rt.modules().by_handle(c.w());
    if (m) c.rt.modules().free(m);
  });
  r.impl(K, "GetModuleHandle", [](Call16& c) {
    uint32_t p = c.ptr();
    Module16* m = (p >> 16) ? c.rt.modules().by_name(c.rt.read_str(p)) : c.rt.modules().by_handle(uint16_t(p));
    // HIWORD = the instance handle as well, as Windows returned it.
    c.ret32(m ? (uint32_t(m->hinstance) << 16) | m->hmodule : 0);
  });
  r.impl(K, "GetModuleUsage", [](Call16& c) {
    Module16* m = c.rt.modules().by_handle(c.w());
    c.ret(m ? uint16_t(std::max(m->refs, 1)) : 0);
  });
  r.impl(K, "GetModuleFileName", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    int16_t n = c.sw();
    Module16* m = h ? c.rt.modules().by_handle(h) : caller_module(c);
    std::string path = m ? upper16(m->guest_path) : upper16(c.rt.options().system_dir + "\\KRNL386.EXE");
    c.ret(n > 0 ? uint16_t(c.rt.write_str(buf, path, size_t(n))) : 0);
  });
  r.impl(K, "GetProcAddress", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr();
    Module16* m = h ? c.rt.modules().by_handle(h) : caller_module(c);
    uint32_t fp = 0;
    if (m) fp = (name >> 16) ? c.rt.modules().proc_address(m, c.rt.read_str(name))
                             : c.rt.modules().proc_address(m, uint16_t(name));
    if (tracing("mod16")) {
      std::string n = (name >> 16) ? c.rt.read_str(name) : "#" + std::to_string(name & 0xFFFF);
      trace("mod16", "GetProcAddress(%s, %s) -> %04X:%04X", m ? m->name.c_str() : "?", n.c_str(), fp >> 16,
            fp & 0xFFFF);
    }
    c.ret32(fp);
  });
  r.impl(K, "MakeProcInstance", [](Call16& c) {
    uint32_t proc = c.ptr();
    c.w();
    c.ret32(proc);
  });
  r.impl(K, "FreeProcInstance", [](Call16& c) { c.ptr(); });

  // ---- resources ----
  r.impl(K, "FindResource", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t name = c.ptr(), type = c.ptr();
    Module16* m = c.rt.modules().by_handle(h);
    loader::ResId t = res_id(c.rt, type), n = res_id(c.rt, name);
    const loader::ne::Resource* res = c.rt.modules().find_resource(m, t, n);
    trace("res16", "FindResource(%s, %s, %s) -> %s", m ? m->name.c_str() : "?", n.to_string().c_str(),
          t.to_string().c_str(), res ? "found" : "none");
    if (!res) return c.ret(0);
    KernelState& s = ks(c.rt);
    for (size_t i = 0; i < s.resources.size(); i++) {
      if (s.resources[i].res == res && s.resources[i].image == m->image) return c.ret(uint16_t(i + 1));
    }
    s.resources.push_back({m->image, m->name, res, 0});
    c.ret(uint16_t(s.resources.size()));
  });
  r.impl(K, "LoadResource", [](Call16& c) {
    c.w();
    uint16_t hr = c.w();
    KernelState& s = ks(c.rt);
    if (!hr || hr > s.resources.size()) return c.ret(0);
    auto& e = s.resources[hr - 1];
    // Loaded once and kept (FreeResource is a no-op), unless the guest
    // GlobalFree'd the copy itself.
    if (!e.hglobal || !c.rt.global().find(e.hglobal)) {
      std::string_view data = e.image->resource_data(*e.res);
      uint16_t h = c.rt.global().alloc(GlobalHeap16::kMoveable, uint32_t(std::max<size_t>(data.size(), 1)));
      GlobalBlock* b = c.rt.global().find(h);
      if (!b) return c.ret(0);
      c.rt.mem().memcpy(b->base, data.data(), data.size());
      c.rt.ldt().set_tag(b->sel, "resource of " + e.module);
      e.hglobal = h;
    }
    c.ret(e.hglobal);
  });
  r.impl(K, "LockResource", [](Call16& c) { c.ret32(c.rt.global().lock(c.w())); });
  // Resources stay loaded for the run (FALSE = freed, in Win16's sense).
  r.impl(K, "FreeResource", [](Call16& c) {
    c.w();
    c.ret(0);
  });
  r.impl(K, "SizeofResource", [](Call16& c) {
    c.w();
    uint16_t hr = c.w();
    KernelState& s = ks(c.rt);
    c.ret32(hr && hr <= s.resources.size() ? s.resources[hr - 1].res->size : 0);
  });

  // ---- strings ----
  r.impl(K, "lstrcpy", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr();
    std::string s = c.rt.read_str(src);
    c.rt.write_bytes(dst, s.c_str(), s.size() + 1);
    c.ret32(dst);
  });
  r.impl(K, "lstrcpyn", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr();
    uint16_t n = c.w();
    if (n) c.rt.write_str(dst, c.rt.read_str(src, n), n);
    c.ret32(dst);
  });
  r.impl(K, "lstrcat", [](Call16& c) {
    uint32_t dst = c.ptr(), src = c.ptr();
    std::string d = c.rt.read_str(dst), s = c.rt.read_str(src);
    c.rt.write_bytes(dst + uint32_t(d.size()), s.c_str(), s.size() + 1);
    c.ret32(dst);
  });
  r.impl(K, "lstrlen", [](Call16& c) { c.ret(uint16_t(c.rt.read_str(c.ptr()).size())); });

  // ---- profiles ----
  r.impl(K, "GetProfileInt", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr();
    int16_t def = c.sw();
    c.ret(uint16_t(get_profile_int(c.rt, "WIN.INI", app, key, def)));
  });
  r.impl(K, "GetProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), def = c.ptr(), buf = c.ptr();
    uint16_t size = c.w();
    c.ret(get_profile_string(c.rt, "WIN.INI", app, key, def, buf, size));
  });
  r.impl(K, "WriteProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), val = c.ptr();
    c.ret_bool(write_profile_string(c.rt, "WIN.INI", app, key, val));
  });
  r.impl(K, "GetPrivateProfileInt", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr();
    int16_t def = c.sw();
    std::string file = c.rt.read_str(c.ptr());
    c.ret(uint16_t(get_profile_int(c.rt, file, app, key, def)));
  });
  r.impl(K, "GetPrivateProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), def = c.ptr(), buf = c.ptr();
    uint16_t size = c.w();
    std::string file = c.rt.read_str(c.ptr());
    c.ret(get_profile_string(c.rt, file, app, key, def, buf, size));
  });
  r.impl(K, "WritePrivateProfileString", [](Call16& c) {
    uint32_t app = c.ptr(), key = c.ptr(), val = c.ptr();
    std::string file = c.rt.read_str(c.ptr());
    c.ret_bool(write_profile_string(c.rt, file, app, key, val));
  });
  r.impl(K, "GetWindowsDirectory", [](Call16& c) {
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    const std::string& d = c.rt.options().windows_dir;
    if (n > d.size()) c.rt.write_str(buf, d, n);
    c.ret(uint16_t(d.size()));
  });
  r.impl(K, "GetDriveType", [](Call16& c) {
    uint16_t drive = c.w();
    c.ret(drive == 2 ? 3 : 0);  // C: fixed; nothing else exists
  });

  // ---- files ----
  r.impl(K, "OpenFile", [](Call16& c) { c.ret(open_file(c)); });
  r.impl(K, "_lopen", [](Call16& c) {
    std::string name = c.rt.read_str(c.ptr());
    uint16_t mode = c.w();
    int h = c.rt.state<DosFiles>().open(name, mode & 3, false);
    c.ret(h < 0 ? kHfileError : uint16_t(h));
  });
  r.impl(K, "_lcreat", [](Call16& c) {
    std::string name = c.rt.read_str(c.ptr());
    c.w();
    int h = c.rt.state<DosFiles>().open(name, 2, true);
    c.ret(h < 0 ? kHfileError : uint16_t(h));
  });
  r.impl(K, "_lclose", [](Call16& c) { c.ret(c.rt.state<DosFiles>().close(c.w()) < 0 ? kHfileError : 0); });
  r.impl(K, "_lread", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    int32_t got = c.rt.state<DosFiles>().read(h, buf, n);
    c.ret(got < 0 ? kHfileError : uint16_t(got));
  });
  r.impl(K, "_lwrite", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr();
    uint16_t n = c.w();
    int32_t put = c.rt.state<DosFiles>().write(h, buf, n);
    c.ret(put < 0 ? kHfileError : uint16_t(put));
  });
  r.impl(K, "_hread", [](Call16& c) {
    uint16_t h = c.w();
    uint32_t buf = c.ptr(), n = c.l();
    int32_t got = c.rt.state<DosFiles>().read(h, buf, n);
    c.ret32(got < 0 ? 0xFFFFFFFF : uint32_t(got));
  });
  r.impl(K, "_llseek", [](Call16& c) {
    uint16_t h = c.w();
    int32_t off = c.sl();
    uint16_t whence = c.w();
    int64_t p = c.rt.state<DosFiles>().seek(h, off, whence);
    c.ret32(p < 0 ? 0xFFFFFFFF : uint32_t(p));
  });
  r.impl(K, "GetTempFileName", [](Call16& c) {
    c.w();
    std::string prefix = c.rt.read_str(c.ptr());
    uint16_t unique = c.w();
    uint32_t buf = c.ptr();
    if (!unique) unique = 0x1234;
    char name[32];
    snprintf(name, sizeof(name), "~%.3s%04X.TMP", prefix.c_str(), unique);
    c.rt.write_str(buf, c.rt.options().windows_dir + "\\TEMP\\" + name, 144);
    c.ret(unique);
  });

  // ---- Catch/Throw ----
  // CATCHBUF (9 WORDs, opaque to its users): IP, CS, SP, BP, SI, DI, DS, the
  // host call level Catch ran at (call_depth()), SS. The level lets a Throw
  // from inside a callback the host made (a window or timer procedure called
  // from a shim) unwind the host frames in between (GuestUnwind16) instead of
  // jumping into a guest stack frame some host frame still owns.
  r.impl(K, "Catch", [](Call16& c) {
    uint32_t buf = c.ptr();
    auto& cpu = c.rt.cpu();
    auto& rr = cpu.registers();
    uint16_t words[9] = {c.ret_ip(),  c.ret_cs(),  uint16_t(c.sp + 4 + 4),      rr.r_bp(),
                         rr.r_si(),   rr.r_di(),   cpu.get_segment(SegReg::DS), uint16_t(c.rt.call_depth()),
                         cpu.get_segment(SegReg::SS)};
    for (int i = 0; i < 9; i++) c.rt.wr16(buf + 2u * uint32_t(i), words[i]);
    c.ret(0);
  });
  r.impl(K, "Throw", [](Call16& c) {
    uint32_t buf = c.ptr();
    uint16_t back = c.w();
    if (tracing("throw16")) trace("throw16", "Throw(%d) from %s", int16_t(back), c.rt.backtrace().c_str());
    std::array<uint16_t, 9> w;
    for (int i = 0; i < 9; i++) w[size_t(i)] = c.rt.rd16(buf + 2u * uint32_t(i));
    auto resume = [w, back](Runtime16& rt) {
      auto& cpu = rt.cpu();
      auto& rr = cpu.registers();
      cpu.load_segment(SegReg::SS, w[8]);
      rr.w_sp(w[2]);
      rr.w_bp(w[3]);
      rr.w_si(w[4]);
      rr.w_di(w[5]);
      if (w[6] & ~3) cpu.load_segment(SegReg::DS, w[6]);
      else cpu.set_segment_null(SegReg::DS);
      rr.w_ax(back);
      cpu.set_cs_eip(w[1], w[0]);
    };
    int level = int16_t(w[7]);
    if (level > 0 && level < c.rt.call_depth()) {
      trace("throw16", "Throw unwinds %d host call level(s) to the Catch", c.rt.call_depth() - level);
      throw GuestUnwind16{level, resume};
    }
    // Same level (or a buffer from a level that has returned — undefined on
    // Windows too): jump straight there.
    resume(c.rt);
    c.take_over();
  });

  // ---- SwitchStackTo/Back: run the caller's locals on a stack in its own data segment ----
  r.impl(K, "SwitchStackTo", [](Call16& c) {
    uint16_t new_ss = c.w(), new_sp = c.w();
    c.w();  // stack top (lowest address): only a stack probe's business
    auto& cpu = c.rt.cpu();
    auto& rr = cpu.registers();
    uint16_t rip = c.ret_ip(), rcs = c.ret_cs();
    uint16_t old_ss = cpu.get_segment(SegReg::SS);
    uint16_t sp_after = uint16_t(c.sp + 4 + 6);
    uint16_t bp = rr.r_bp();
    // The caller's locals and saved BP ([sp_after, bp+2)) move to the new
    // stack, so BP-relative code keeps working until SwitchStackBack.
    uint16_t frame = bp >= sp_after ? uint16_t(bp + 2 - sp_after) : 0;
    uint16_t nsp = uint16_t(new_sp - frame);
    std::vector<uint8_t> tmp(frame);
    if (frame) {
      c.rt.read_bytes((uint32_t(old_ss) << 16) | sp_after, tmp.data(), frame);
      c.rt.write_bytes((uint32_t(new_ss) << 16) | nsp, tmp.data(), frame);
    }
    ks(c.rt).stack_switches.push_back({old_ss, sp_after, bp, frame});
    cpu.load_segment(SegReg::SS, new_ss);
    rr.w_sp(nsp);
    rr.w_bp(uint16_t(nsp + (bp - sp_after)));
    cpu.set_cs_eip(rcs, rip);
    c.take_over();
  });
  r.impl(K, "SwitchStackBack", [](Call16& c) {
    auto& ss = ks(c.rt).stack_switches;
    if (ss.empty()) return;
    auto sw = ss.back();
    ss.pop_back();
    auto& cpu = c.rt.cpu();
    auto& rr = cpu.registers();
    uint16_t rip = c.ret_ip(), rcs = c.ret_cs();
    uint16_t cur_ss = cpu.get_segment(SegReg::SS);
    uint16_t sp_after = uint16_t(c.sp + 4);
    // Copy the (possibly changed) frame back to where it came from.
    if (sw.frame) {
      std::vector<uint8_t> tmp(sw.frame);
      c.rt.read_bytes((uint32_t(cur_ss) << 16) | sp_after, tmp.data(), sw.frame);
      c.rt.write_bytes((uint32_t(sw.ss) << 16) | sw.sp, tmp.data(), sw.frame);
    }
    cpu.load_segment(SegReg::SS, sw.ss);
    rr.w_sp(sw.sp);
    rr.w_bp(sw.bp);
    cpu.set_cs_eip(rcs, rip);
    c.take_over();
  });
}

}  // namespace adw::win16
