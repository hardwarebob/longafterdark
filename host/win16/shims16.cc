#include "win16/shims16.hh"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

#include "adw/core/log.h"
#include "win16/runtime16.hh"

namespace adw::win16 {

namespace {

std::string upper(std::string_view s) {
  std::string u(s);
  for (char& c : u) c = char(toupper(uint8_t(c)));
  return u;
}

// System DLLs the Classic lane emulates: every module a Classic binary
// imports that is not itself an After Dark image (API_SURFACE.md §2), plus
// the ones a module could reach through LoadLibrary/GetModuleHandle.
const char* const kSystemModules[] = {"KERNEL", "USER", "GDI", "MMSYSTEM", "WIN87EM", "COMMDLG", "KEYBOARD",
                                      "SHELL", "SOUND", "SYSTEM", "TOOLHELP", "VER", "LZEXPAND", "WING", "DISPLAY"};

}  // namespace

const char* conv16_name(Conv16 c) {
  switch (c) {
    case Conv16::pascal_: return "pascal";
    case Conv16::cdecl_: return "cdecl";
    case Conv16::register_: return "register";
    case Conv16::equate: return "equate";
    case Conv16::variable: return "variable";
    case Conv16::stub: return "stub";
  }
  return "?";
}

// ---- Call16 -------------------------------------------------------------------------------------

Call16::Call16(Runtime16& rt, Shim16Entry& fn, uint16_t sp) : rt(rt), fn(fn), sp(sp) { rewind(); }

MemoryContext& Call16::mem() const { return rt.mem(); }

uint16_t Call16::stack16(uint32_t off) const {
  return rt.rd16((uint32_t(rt.cpu().get_segment(cpu::X86Emulator::SegReg::SS)) << 16) | uint16_t(sp + 4 + off));
}

uint32_t Call16::stack32(uint32_t off) const { return uint32_t(stack16(off)) | (uint32_t(stack16(off + 2)) << 16); }

uint16_t Call16::ret_ip() const {
  return rt.rd16((uint32_t(rt.cpu().get_segment(cpu::X86Emulator::SegReg::SS)) << 16) | sp);
}
uint16_t Call16::ret_cs() const {
  return rt.rd16((uint32_t(rt.cpu().get_segment(cpu::X86Emulator::SegReg::SS)) << 16) | uint16_t(sp + 2));
}

void Call16::rewind() {
  // Pascal: the first argument is the deepest (pushed first); cdecl: the shallowest.
  cursor_ = fn.conv == Conv16::cdecl_ ? 0 : std::max(fn.arg_bytes, 0);
}

uint16_t Call16::w() {
  if (fn.conv == Conv16::cdecl_) {
    uint16_t v = stack16(uint32_t(cursor_));
    cursor_ += 2;
    return v;
  }
  cursor_ -= 2;
  return stack16(uint32_t(std::max(cursor_, 0)));
}

uint32_t Call16::l() {
  if (fn.conv == Conv16::cdecl_) {
    uint32_t v = stack32(uint32_t(cursor_));
    cursor_ += 4;
    return v;
  }
  cursor_ -= 4;
  return stack32(uint32_t(std::max(cursor_, 0)));
}

void Call16::ret(uint32_t v) {
  result_ = v;
  auto& r = rt.cpu().registers();
  r.w_ax(uint16_t(v));
  if (!fn.ret16) r.w_dx(uint16_t(v >> 16));
}

void Call16::ret32(uint32_t v) {
  result_ = v;
  auto& r = rt.cpu().registers();
  r.w_ax(uint16_t(v));
  r.w_dx(uint16_t(v >> 16));
}

// ---- Shim16Registry --------------------------------------------------------------------------

Shim16Registry::Shim16Registry(MemoryContext& mem, uint32_t thunk_base) : mem_(mem), thunk_base_(thunk_base) {
  for (const char* m : kSystemModules) modules_.push_back(m);
  for (const Sig16& s : win16_signatures()) {
    add(s.module, s.ordinal, s.name, s.conv, s.ret16, s.arg_bytes).value = s.value;
  }
  // Thunk id 0 is the call_far sentinel (Runtime16): reserve it.
  by_thunk_.push_back(nullptr);
}

std::string Shim16Registry::normalize_module(std::string_view m) {
  size_t slash = m.find_last_of("\\/:");
  if (slash != std::string_view::npos) m = m.substr(slash + 1);
  size_t dot = m.find('.');
  if (dot != std::string_view::npos) m = m.substr(0, dot);
  return upper(m);
}

Shim16Entry& Shim16Registry::add(std::string_view module, uint16_t ordinal, std::string_view name, Conv16 conv,
                                 bool ret16, int arg_bytes, Shim16Fn fn) {
  std::string mod = normalize_module(module);
  std::string key = mod + "." + std::to_string(ordinal);
  Shim16Entry* e;
  auto it = by_key_.find(key);
  if (it != by_key_.end()) {
    e = it->second;
  } else {
    entries_.push_back(Shim16Entry{});
    e = &entries_.back();
    e->module = mod;
    e->ordinal = ordinal;
    by_key_[key] = e;
  }
  e->name = std::string(name);
  e->conv = conv;
  e->ret16 = ret16;
  e->arg_bytes = arg_bytes;
  if (fn) e->fn = std::move(fn);
  if (!e->name.empty()) by_name_[mod + "!" + upper(e->name)] = e;
  if (std::find(modules_.begin(), modules_.end(), mod) == modules_.end() && mod[0] != '<') modules_.push_back(mod);
  return *e;
}

void Shim16Registry::impl(std::string_view module, std::string_view name, Shim16Fn fn) {
  Shim16Entry* e = find_name(module, name);
  if (!e) {
    throw std::logic_error("win16 shim " + normalize_module(module) + "!" + std::string(name) +
                           " has no signature: regenerate signatures16.cc (research/win/gen_sig16.py) or use add()");
  }
  if (e->conv == Conv16::stub || e->arg_bytes < 0) {
    throw std::logic_error("win16 shim " + e->label() + " has no argument list: declare it with add()");
  }
  e->fn = std::move(fn);
}

bool Shim16Registry::has_module(std::string_view module) const {
  std::string m = normalize_module(module);
  return std::find(modules_.begin(), modules_.end(), m) != modules_.end();
}

std::vector<std::string> Shim16Registry::modules() const { return modules_; }

Shim16Entry* Shim16Registry::find(std::string_view module, uint16_t ordinal) {
  auto it = by_key_.find(normalize_module(module) + "." + std::to_string(ordinal));
  return it == by_key_.end() ? nullptr : it->second;
}

Shim16Entry* Shim16Registry::find_name(std::string_view module, std::string_view name) {
  auto it = by_name_.find(normalize_module(module) + "!" + upper(name));
  return it == by_name_.end() ? nullptr : it->second;
}

Shim16Entry& Shim16Registry::get(std::string_view module, uint16_t ordinal) {
  if (Shim16Entry* e = find(module, ordinal)) return *e;
  return add(module, ordinal, "#" + std::to_string(ordinal), Conv16::stub, true, -1);
}

Shim16Entry* Shim16Registry::by_thunk(uint16_t id) { return id < by_thunk_.size() ? by_thunk_[id] : nullptr; }

uint16_t Shim16Registry::assign(Shim16Entry& e) {
  if (by_thunk_.size() >= 0x10000 / kThunkStride) throw std::runtime_error("win16 thunk segment is full");
  uint16_t id = uint16_t(by_thunk_.size());
  by_thunk_.push_back(&e);
  e.thunk = id;
  const uint8_t code[4] = {0xCD, 0xFE, uint8_t(id), uint8_t(id >> 8)};
  mem_.memcpy(thunk_base_ + offset_of(id), code, sizeof(code));
  return id;
}

uint16_t Shim16Registry::thunk_offset(Shim16Entry& e) {
  if (!e.thunk) assign(e);
  return offset_of(e.thunk);
}

uint16_t Shim16Registry::internal_thunk(std::string_view name, Shim16Fn fn) {
  Shim16Entry& e = add("<host>", uint16_t(entries_.size()), name, Conv16::register_, false, 0, std::move(fn));
  return thunk_offset(e);
}

std::vector<const Shim16Entry*> Shim16Registry::unimplemented_called() const {
  std::vector<const Shim16Entry*> out;
  for (const Shim16Entry& e : entries_) {
    if (!e.fn && e.calls) out.push_back(&e);
  }
  std::sort(out.begin(), out.end(), [](const Shim16Entry* a, const Shim16Entry* b) {
    return a->module != b->module ? a->module < b->module : a->ordinal < b->ordinal;
  });
  return out;
}

void Shim16Registry::print_census(std::string_view label) const {
  auto un = unimplemented_called();
  uint64_t total = 0, calls = 0;
  size_t used = 0;
  for (const Shim16Entry& e : entries_) {
    if (e.calls) used++;
    calls += e.calls;
  }
  for (const Shim16Entry* e : un) total += e->calls;
  std::string line = "[census16] " + std::string(label) + ": " + std::to_string(used) + " functions called (" +
                     std::to_string(calls) + " calls), " + std::to_string(un.size()) + " unimplemented (" +
                     std::to_string(total) + " calls)";
  for (const Shim16Entry* e : un) line += "\n[census16]   " + e->label() + " x" + std::to_string(e->calls);
  write_stderr(line + "\n");
  if (tracing("prof16")) {
    // Where host time went, by shim.
    std::vector<const Shim16Entry*> all;
    for (const Shim16Entry& e : entries_) {
      if (e.host_ticks) all.push_back(&e);
    }
    std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->host_ticks > b->host_ticks; });
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    for (size_t i = 0; i < all.size() && i < 15; i++) {
      trace("prof16", "%-40s %9llu calls %8.1f ms %7.2f us/call", all[i]->label().c_str(),
            (unsigned long long)all[i]->calls, double(all[i]->host_ticks) * 1000.0 / double(f.QuadPart),
            double(all[i]->host_ticks) * 1e6 / double(f.QuadPart) / double(std::max<uint64_t>(all[i]->calls, 1)));
    }
  }
}

}  // namespace adw::win16
