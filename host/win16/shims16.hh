// Win16 import thunks and the shim registry (DESIGN.md §5, ABI.md §3.6).
//
// Every function a 16-bit image imports from a system DLL we emulate (KERNEL,
// USER, GDI, MMSYSTEM, …) is bound to a 4-byte far thunk in the thunk code
// segment (layout16.hh):
//
//     CD FE        int 0xFE
//     id id        16-bit thunk id (read by the trap handler at CS:IP)
//
// The trap handler looks the id up here and gives the handler a Call16: the
// arguments are read from the 16-bit stack above the far return address, the
// result goes to AX (or DX:AX), and the dispatcher then returns FAR to the
// caller the way the convention says — Pascal (and the "register" functions)
// pop their argument bytes, cdecl ones (wsprintf) leave them to the caller.
// An import with no handler logs "[unimpl16] KERNEL.123 Name" once, is counted
// for the census, and returns 0 — still popping the right number of bytes,
// because every system-DLL import of the lane has a signature
// (signatures16.cc, generated from the Win16 interface as research/win/spec
// records it).
//
// Keys are "MODULE.ordinal" (imports are by ordinal); names are for logs and
// for GetProcAddress/import by name, compared case-insensitively.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "MemoryContext.hh"

namespace adw::win16 {

using cpu::MemoryContext;

class Runtime16;
class Call16;

using Shim16Fn = std::function<void(Call16&)>;

enum class Conv16 : uint8_t {
  pascal_,    // PASCAL FAR: arguments pushed left to right, the callee pops them
  cdecl_,     // cdecl FAR (the varargs ones): the caller pops
  register_,  // takes its input in registers (DOS3Call, Catch, Throw…): the handler may rewrite any register;
              // stack arguments, when there are any, are still popped Pascal-style
  equate,     // a constant export (__AHINCR…): resolved at load time, never called
  variable,   // exported data (never imported by the lane)
  stub,       // no known argument list
};
const char* conv16_name(Conv16 c);

// One row of the signature table.
struct Sig16 {
  const char* module;  // "KERNEL"
  uint16_t ordinal;
  const char* name;
  Conv16 conv;
  bool ret16;          // result in AX only (else DX:AX)
  int16_t arg_bytes;   // -1 = unknown (stub)
  int32_t value;       // equates
};
std::span<const Sig16> win16_signatures();

struct Shim16Entry {
  std::string module;  // upper case, no extension: "KERNEL"
  uint16_t ordinal = 0;
  std::string name;
  Conv16 conv = Conv16::stub;
  bool ret16 = true;
  int arg_bytes = -1;
  int32_t value = 0;
  Shim16Fn fn;          // empty = unimplemented
  uint16_t thunk = 0;   // 0 = no thunk yet
  uint64_t calls = 0;
  uint64_t host_ticks = 0;  // time spent in the handler (QueryPerformanceCounter ticks; ADTRACE=prof16)
  bool unimpl_reported = false;
  // False: GetProcAddress by name answers 0 (the entry still resolves by
  // ordinal): an export this machine's MMSYSTEM behaves as if it lacked.
  bool by_name = true;
  std::string key() const { return module + "." + std::to_string(ordinal); }
  std::string label() const { return key() + " " + name; }
};

// What a shim handler sees. The argument readers walk the arguments in their
// DECLARATION order whatever the convention: for Pascal the first argument is
// the deepest on the stack, for cdecl the shallowest.
class Call16 {
 public:
  Call16(Runtime16& rt, Shim16Entry& fn, uint16_t sp);

  Runtime16& rt;
  Shim16Entry& fn;
  const uint16_t sp;  // SP at entry: [SS:SP] = return IP, [SS:SP+2] = return CS

  MemoryContext& mem() const;
  uint16_t ret_ip() const;
  uint16_t ret_cs() const;
  // Raw stack word/dword `off` bytes above the far return address (SS:SP+4+off).
  uint16_t stack16(uint32_t off) const;
  uint32_t stack32(uint32_t off) const;

  // Next argument in declaration order.
  uint16_t w();
  int16_t sw() { return int16_t(w()); }
  uint32_t l();
  int32_t sl() { return int32_t(l()); }
  uint32_t ptr() { return l(); }  // a far pointer, packed selector:offset
  // Skip/peek helpers for handlers that re-read.
  void rewind();

  // Results. ret() writes AX (and DX = high word for a DX:AX function);
  // ret32() always writes DX:AX.
  void ret(uint32_t v);
  void ret32(uint32_t v);
  void ret_bool(bool b) { ret(b ? 1 : 0); }
  uint32_t result() const { return result_; }

  // The handler set CS:IP / SS:SP itself (Throw, SwitchStackTo…): the
  // dispatcher must not return to the caller.
  void take_over() { took_over_ = true; }
  bool took_over() const { return took_over_; }

 private:
  int32_t cursor_;
  uint32_t result_ = 0;
  bool took_over_ = false;
};

class Shim16Registry {
 public:
  // The thunk segment lives at `thunk_base` (linear) in `mem`, 64 KiB.
  Shim16Registry(MemoryContext& mem, uint32_t thunk_base);

  static std::string normalize_module(std::string_view m);  // "user.exe" → "USER"

  // ---- registration ----
  // Implements a function whose signature is in win16_signatures() (looked up
  // by module + name). Unknown names throw std::logic_error: regenerate
  // signatures16.cc or use add().
  void impl(std::string_view module, std::string_view name, Shim16Fn fn);
  // Declares (and optionally implements) a function with an explicit signature.
  Shim16Entry& add(std::string_view module, uint16_t ordinal, std::string_view name, Conv16 conv, bool ret16,
                   int arg_bytes, Shim16Fn fn = {});

  // ---- lookup ----
  // A system DLL we emulate (KERNEL, USER, GDI, MMSYSTEM, …).
  bool has_module(std::string_view module) const;
  std::vector<std::string> modules() const;
  Shim16Entry* find(std::string_view module, uint16_t ordinal);
  Shim16Entry* find_name(std::string_view module, std::string_view name);  // case-insensitive
  // Finds or declares an unknown-signature entry (reported as such when called).
  Shim16Entry& get(std::string_view module, uint16_t ordinal);
  Shim16Entry* by_thunk(uint16_t id);

  // ---- thunks ----
  // The thunk offset (in the thunk segment) that calls `e`, written on first use.
  uint16_t thunk_offset(Shim16Entry& e);
  // A host routine with its own thunk (not importable); returns its offset.
  uint16_t internal_thunk(std::string_view name, Shim16Fn fn);
  static constexpr uint16_t kThunkStride = 4;
  static uint16_t offset_of(uint16_t id) { return uint16_t(id * kThunkStride); }

  // ---- census ----
  std::vector<const Shim16Entry*> unimplemented_called() const;
  void print_census(std::string_view label) const;

 private:
  uint16_t assign(Shim16Entry& e);

  MemoryContext& mem_;
  uint32_t thunk_base_;
  std::deque<Shim16Entry> entries_;                       // stable addresses
  std::unordered_map<std::string, Shim16Entry*> by_key_;  // "KERNEL.15"
  std::unordered_map<std::string, Shim16Entry*> by_name_; // "KERNEL!GLOBALALLOC"
  std::vector<Shim16Entry*> by_thunk_;                    // [0] = the sentinel
  std::vector<std::string> modules_;
};

}  // namespace adw::win16
