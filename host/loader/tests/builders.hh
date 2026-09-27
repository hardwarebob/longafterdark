// Test helpers: synthetic PE32 / NE images built byte by byte, a sink that
// records what the loader placed, and a minimal CHECK macro.
//
// The images are small but exercise every structure the loaders parse; the
// offsets are fixed so the tests can assert exact post-load bytes.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "loader/image.hh"

namespace adw::loader::test {

inline int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ::adw::loader::test::g_failures++;                                     \
    }                                                                        \
  } while (0)
#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    auto _va = (a);                                                          \
    auto _vb = (b);                                                          \
    if (!(_va == _vb)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (0x%llX vs 0x%llX)\n", __FILE__, \
          __LINE__, #a, #b, (unsigned long long)_va, (unsigned long long)_vb); \
      ::adw::loader::test::g_failures++;                                     \
    }                                                                        \
  } while (0)
#define CHECK_STR(a, b)                                                      \
  do {                                                                       \
    std::string _sa(a), _sb(b);                                              \
    if (_sa != _sb) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK_STR failed: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, \
          __LINE__, #a, #b, _sa.c_str(), _sb.c_str());                       \
      ::adw::loader::test::g_failures++;                                     \
    }                                                                        \
  } while (0)
// Evaluates expr and checks that it throws LoaderError of the given kind.
#define CHECK_THROWS_KIND(expr, k)                                           \
  do {                                                                       \
    bool _thrown = false;                                                    \
    try {                                                                    \
      (void)(expr);                                                          \
    } catch (const ::adw::loader::LoaderError& _e) {                         \
      _thrown = true;                                                        \
      if (_e.kind() != (k)) {                                                \
        std::fprintf(stderr, "%s:%d: %s threw kind %s (%s)\n", __FILE__, __LINE__, #expr, \
            ::adw::loader::error_kind_name(_e.kind()), _e.what());           \
        ::adw::loader::test::g_failures++;                                   \
      }                                                                      \
    }                                                                        \
    if (!_thrown) {                                                          \
      std::fprintf(stderr, "%s:%d: %s did not throw\n", __FILE__, __LINE__, #expr); \
      ::adw::loader::test::g_failures++;                                     \
    }                                                                        \
  } while (0)

inline int finish(const char* name) {
  if (g_failures) {
    std::fprintf(stderr, "%s: %d check(s) failed\n", name, g_failures);
    return 1;
  }
  std::printf("%s: all checks passed\n", name);
  return 0;
}

// A growable little-endian byte buffer addressed by absolute offset.
struct Buf {
  std::string d;
  explicit Buf(size_t n = 0) : d(n, '\0') {}
  void grow(size_t end) {
    if (d.size() < end) d.resize(end, '\0');
  }
  void u8(size_t off, uint8_t v) {
    grow(off + 1);
    d[off] = static_cast<char>(v);
  }
  void u16(size_t off, uint16_t v) {
    u8(off, v & 0xFF);
    u8(off + 1, v >> 8);
  }
  void u32(size_t off, uint32_t v) {
    u16(off, v & 0xFFFF);
    u16(off + 2, v >> 16);
  }
  void bytes(size_t off, std::string_view s) {
    grow(off + s.size());
    std::memcpy(d.data() + off, s.data(), s.size());
  }
  void cstr(size_t off, std::string_view s) {
    bytes(off, s);
    u8(off + s.size(), 0);
  }
  void pstr(size_t off, std::string_view s) {
    u8(off, static_cast<uint8_t>(s.size()));
    bytes(off + 1, s);
  }
  void utf16(size_t off, std::string_view ascii) {
    for (size_t i = 0; i < ascii.size(); i++) u16(off + 2 * i, static_cast<uint8_t>(ascii[i]));
  }
  uint8_t get8(size_t off) const { return static_cast<uint8_t>(d.at(off)); }
  uint16_t get16(size_t off) const { return get8(off) | (get8(off + 1) << 8); }
  uint32_t get32(size_t off) const { return get16(off) | (uint32_t(get16(off + 2)) << 16); }
};

// A sink with sparse emulated memory. Reservations start filled with 0xCC so
// a test notices any byte the loader failed to write.
struct TestSink : ImageSink {
  // PE: honor the preferred base unless `force_base` is set.
  uint32_t force_base = 0;
  // NE: segments go at next_free, 64K apart.
  uint32_t next_free = 0x00100000;
  std::map<uint32_t, std::string> blocks;
  size_t writes = 0;

  uint32_t reserve(uint32_t preferred, uint32_t size) override {
    uint32_t base = force_base ? force_base : (preferred ? preferred : next_free);
    if (!preferred || force_base) next_free = base + ((size + 0xFFFF) & ~0xFFFFu) + 0x10000;
    blocks[base] = std::string(size, '\xCC');
    return base;
  }
  void write(uint32_t addr, const void* data, size_t size) override {
    writes++;
    std::string& b = block(addr, size);
    uint32_t base = find_base(addr);
    std::memcpy(b.data() + (addr - base), data, size);
  }
  uint32_t find_base(uint32_t addr) const {
    auto it = blocks.upper_bound(addr);
    if (it == blocks.begin()) throw std::out_of_range("unreserved address");
    return std::prev(it)->first;
  }
  std::string& block(uint32_t addr, size_t size) {
    uint32_t base = find_base(addr);
    std::string& b = blocks[base];
    if (addr - base + size > b.size()) throw std::out_of_range("write past reservation");
    return b;
  }
  uint8_t u8(uint32_t a) { return static_cast<uint8_t>(block(a, 1)[a - find_base(a)]); }
  uint16_t u16(uint32_t a) { return u8(a) | (u8(a + 1) << 8); }
  uint32_t u32(uint32_t a) { return u16(a) | (uint32_t(u16(a + 2)) << 16); }
};

// ---- VERSIONINFO ------------------------------------------------------------

// One node of a VS_VERSIONINFO tree in either layout; children are padded to
// 4-byte boundaries as resource compilers do.
inline std::string vnode(bool wide, std::string_view key, std::string_view value, bool text,
    const std::vector<std::string>& children = {}) {
  Buf b;
  size_t p = wide ? 6 : 4;
  if (wide) {
    b.utf16(p, key);
    p += key.size() * 2;
    b.u16(p, 0);
    p += 2;
  } else {
    b.cstr(p, key);
    p += key.size() + 1;
  }
  p = (p + 3) & ~size_t(3);
  uint16_t vlen = 0;
  if (text) {
    if (wide) {
      b.utf16(p, value);
      b.u16(p + value.size() * 2, 0);
      p += value.size() * 2 + 2;
      vlen = static_cast<uint16_t>(value.size() + 1); // WCHARs
    } else {
      b.cstr(p, value);
      p += value.size() + 1;
      vlen = static_cast<uint16_t>(value.size() + 1);
    }
  } else {
    b.bytes(p, value);
    p += value.size();
    vlen = static_cast<uint16_t>(value.size());
  }
  b.grow(p);
  for (const auto& c : children) {
    p = (p + 3) & ~size_t(3);
    b.bytes(p, c);
    p += c.size();
  }
  b.d.resize(p);
  b.u16(0, static_cast<uint16_t>(p));
  b.u16(2, vlen);
  if (wide) b.u16(4, text ? 1 : 0);
  return b.d;
}

inline std::string version_resource(bool wide) {
  Buf fixed(52);
  fixed.u32(0, 0xFEEF04BD);
  fixed.u32(4, 0x00010000);
  fixed.u32(8, 0x00040000);  // 4.0
  fixed.u32(12, 0x00000001); // .0.1
  fixed.u32(16, 0x00040000);
  fixed.u32(20, 0x00000001);
  fixed.u32(24, 0x3F);
  fixed.u32(32, wide ? 0x40004 : 0x1); // VOS_NT_WINDOWS32 / VOS__WINDOWS16
  fixed.u32(36, 2);                    // VFT_DLL
  std::string strings = vnode(wide, "040904E4", "", false, {
      vnode(wide, "CompanyName", "Test Co", true),
      vnode(wide, "FileDescription", wide ? "Synthetic PE" : "Synthetic NE", true),
      vnode(wide, "FileVersion", "4.0.0.1", true),
  });
  Buf tr(4);
  tr.u32(0, 0x04E40409);
  return vnode(wide, "VS_VERSION_INFO", fixed.d, false, {
      vnode(wide, "StringFileInfo", "", false, {strings}),
      vnode(wide, "VarFileInfo", "", false, {vnode(wide, "Translation", tr.d, false)}),
  });
}

// ---- synthetic PE32 DLL -------------------------------------------------------
//
// ImageBase 0x10000000, SectionAlignment 0x1000, FileAlignment 0x200.
//   .text  RVA 0x1000  raw 0x0400  code with HIGHLOW/HIGH/LOW/HIGHADJ targets
//   .rdata RVA 0x2000  raw 0x0600  imports (one ILT'd DLL, one Borland-style
//                                  OFT=0 DLL), exports (alias, forwarder, hole,
//                                  ordinal-only), TLS directory
//   .data  RVA 0x3000  raw 0x0E00  0x200 raw, 0x1800 virtual (BSS tail)
//   .rsrc  RVA 0x5000  raw 0x1000  "RLEP"/"SPLASH", RT_STRING (2 langs),
//                                  RT_VERSION, 1000/5
//   .reloc RVA 0x6000  raw 0x1800
namespace pe_fixture {
constexpr uint32_t image_base = 0x10000000;
constexpr uint32_t size_of_image = 0x7000;
constexpr uint32_t slot_foo = 0x2140, slot_ord7 = 0x2144, slot_gettick = 0x2160;
constexpr uint16_t highadj_param = 0x9000;
} // namespace pe_fixture

inline std::string build_pe(bool relocs_stripped = false) {
  using namespace pe_fixture;
  Buf f(0x1A00);
  // DOS header + PE signature
  f.bytes(0, "MZ");
  f.u32(0x3C, 0x80);
  f.bytes(0x80, std::string_view("PE\0\0", 4));
  // COFF header
  size_t c = 0x84;
  f.u16(c, 0x014C);
  f.u16(c + 2, 5);
  f.u32(c + 4, 0x31C0FFEE);
  f.u16(c + 16, 0xE0);
  f.u16(c + 18, 0x2102 | (relocs_stripped ? 1 : 0)); // DLL | 32BIT | EXECUTABLE
  // Optional header
  size_t o = 0x98;
  f.u16(o, 0x10B);
  f.u8(o + 2, 2);
  f.u8(o + 3, 25);
  f.u32(o + 16, 0x1000); // entry
  f.u32(o + 20, 0x1000);
  f.u32(o + 24, 0x2000);
  f.u32(o + 28, image_base);
  f.u32(o + 32, 0x1000);
  f.u32(o + 36, 0x200);
  f.u16(o + 40, 4);
  f.u16(o + 48, 4);
  f.u32(o + 56, size_of_image);
  f.u32(o + 60, 0x400);
  f.u16(o + 68, 2);
  f.u32(o + 72, 0x100000);
  f.u32(o + 76, 0x1000);
  f.u32(o + 80, 0x100000);
  f.u32(o + 84, 0x1000);
  f.u32(o + 92, 16);
  auto dir = [&](int i, uint32_t rva, uint32_t size) {
    f.u32(o + 96 + 8 * i, rva);
    f.u32(o + 100 + 8 * i, size);
  };
  dir(0, 0x2300, 0x100); // export
  dir(1, 0x2000, 60);    // import
  dir(2, 0x5000, 0x800); // resource
  dir(5, 0x6000, 56);    // basereloc
  dir(9, 0x2500, 24);    // TLS
  dir(12, 0x2140, 0x28); // IAT
  // Section table
  auto sec = [&](int i, const char* name, uint32_t vsize, uint32_t rva, uint32_t raw_size,
      uint32_t raw_off, uint32_t chars) {
    size_t s = 0x178 + 40 * i;
    f.bytes(s, name);
    f.u32(s + 8, vsize);
    f.u32(s + 12, rva);
    f.u32(s + 16, raw_size);
    f.u32(s + 20, raw_off);
    f.u32(s + 36, chars);
  };
  sec(0, ".text", 0x100, 0x1000, 0x200, 0x400, 0x60000020);
  sec(1, ".rdata", 0x800, 0x2000, 0x800, 0x600, 0x40000040);
  sec(2, ".data", 0x1800, 0x3000, 0x200, 0xE00, 0xC0000040);
  sec(3, ".rsrc", 0x800, 0x5000, 0x800, 0x1000, 0x40000040);
  sec(4, ".reloc", 0x100, 0x6000, 0x200, 0x1800, 0x42000040);

  // RVA → file offset for this layout.
  auto at = [](uint32_t rva) -> size_t {
    if (rva >= 0x6000) return rva - 0x6000 + 0x1800;
    if (rva >= 0x5000) return rva - 0x5000 + 0x1000;
    if (rva >= 0x3000) return rva - 0x3000 + 0xE00;
    if (rva >= 0x2000) return rva - 0x2000 + 0x600;
    return rva - 0x1000 + 0x400;
  };

  // .text
  f.u8(at(0x1000), 0xB8); // mov eax, imm32 (.data)
  f.u32(at(0x1001), image_base + 0x3000);
  f.u8(at(0x1005), 0xFF); // jmp [slot_foo]
  f.u8(at(0x1006), 0x25);
  f.u32(at(0x1007), image_base + slot_foo);
  f.u8(at(0x100B), 0xC3);
  f.u8(at(0x1010), 0xC3);
  f.u16(at(0x1020), 0x1000);             // HIGH half of 0x1000xxxx
  f.u16(at(0x1022), 0x1234);             // LOW half
  f.u16(at(0x1024), 0x1001);             // HIGHADJ: 0x10010000 + (int16)0x9000 = 0x10009000

  // .rdata: import descriptors
  f.u32(at(0x2000), 0x2100);  // OFT
  f.u32(at(0x200C), 0x2200);  // Name
  f.u32(at(0x2010), 0x2140);  // FT
  f.u32(at(0x2014 + 0), 0);   // OFT = 0: Borland style
  f.u32(at(0x2014 + 12), 0x2210);
  f.u32(at(0x2014 + 16), 0x2160);
  for (uint32_t list : {0x2100u, 0x2140u}) {
    f.u32(at(list), 0x2230);
    f.u32(at(list + 4), 0x80000007);
  }
  f.u32(at(0x2160), 0x2240);
  f.cstr(at(0x2200), "ADXPL510.dll");
  f.cstr(at(0x2210), "KERNEL32.dll");
  f.u16(at(0x2230), 3);
  f.cstr(at(0x2232), "Foo");
  f.u16(at(0x2240), 0x1A5);
  f.cstr(at(0x2242), "GetTickCount");
  // exports
  size_t e = at(0x2300);
  f.u32(e + 12, 0x2380);
  f.u32(e + 16, 1);      // Base
  f.u32(e + 20, 4);      // NumberOfFunctions
  f.u32(e + 24, 3);      // NumberOfNames
  f.u32(e + 28, 0x2330);
  f.u32(e + 32, 0x2340);
  f.u32(e + 36, 0x2350);
  f.u32(at(0x2330), 0x1000); // #1 Module (+ alias)
  f.u32(at(0x2334), 0);      // #2 hole
  f.u32(at(0x2338), 0x23A0); // #3 forwarder
  f.u32(at(0x233C), 0x1010); // #4 by ordinal only
  f.u32(at(0x2340), 0x2360);
  f.u32(at(0x2344), 0x2368);
  f.u32(at(0x2348), 0x2370);
  f.u16(at(0x2350), 2);
  f.u16(at(0x2352), 0);
  f.u16(at(0x2354), 0);
  f.cstr(at(0x2360), "Fwd");
  f.cstr(at(0x2368), "Module");
  f.cstr(at(0x2370), "ModuleAlias");
  f.cstr(at(0x2380), "TEST.AD");
  f.cstr(at(0x23A0), "KERNEL32.GetTickCount");
  // TLS
  f.u32(at(0x2500), image_base + 0x3100);
  f.u32(at(0x2504), image_base + 0x3110);
  f.u32(at(0x2508), image_base + 0x3120);
  f.u32(at(0x250C), image_base + 0x2520);
  f.u32(at(0x2510), 0x10);
  f.u32(at(0x2520), image_base + 0x100B);

  // .data
  f.u32(at(0x3000), image_base + 0x1000);
  f.bytes(at(0x3100), "TLSDATA");

  // .rsrc (offsets relative to 0x5000)
  size_t r = at(0x5000);
  auto rdir = [&](size_t off, uint16_t named, uint16_t ids) {
    f.u16(r + off + 12, named);
    f.u16(r + off + 14, ids);
  };
  auto rent = [&](size_t dir_off, int i, uint32_t name, uint32_t target) {
    f.u32(r + dir_off + 16 + 8 * i, name);
    f.u32(r + dir_off + 20 + 8 * i, target);
  };
  constexpr uint32_t sub = 0x80000000;
  rdir(0x000, 1, 3);
  rent(0x000, 0, sub | 0x200, sub | 0x030); // "RLEP"
  rent(0x000, 1, 6, sub | 0x048);           // RT_STRING
  rent(0x000, 2, 16, sub | 0x060);          // RT_VERSION
  rent(0x000, 3, 1000, sub | 0x078);
  rdir(0x030, 1, 0);
  rent(0x030, 0, sub | 0x210, sub | 0x090); // "SPLASH"
  rdir(0x048, 0, 1);
  rent(0x048, 0, 1, sub | 0x0A8);           // block 1
  rdir(0x060, 0, 1);
  rent(0x060, 0, 1, sub | 0x0C8);
  rdir(0x078, 0, 1);
  rent(0x078, 0, 5, sub | 0x0E0);
  rdir(0x090, 0, 1);
  rent(0x090, 0, 0, 0x100);
  rdir(0x0A8, 0, 2);
  rent(0x0A8, 0, 0x409, 0x110);
  rent(0x0A8, 1, 0x407, 0x120);
  rdir(0x0C8, 0, 1);
  rent(0x0C8, 0, 0x409, 0x130);
  rdir(0x0E0, 0, 1);
  rent(0x0E0, 0, 0, 0x140);
  auto rdata = [&](size_t off, uint32_t rva, uint32_t size) {
    f.u32(r + off, rva);
    f.u32(r + off + 4, size);
    f.u32(r + off + 8, 1252);
  };
  f.u16(r + 0x200, 4);
  f.utf16(r + 0x202, "RLEP");
  f.u16(r + 0x210, 6);
  f.utf16(r + 0x212, "SPLASH");
  f.bytes(r + 0x300, "hello");
  f.bytes(r + 0x310, "\x01\x02\x03\x04");
  // RT_STRING block 1 (ids 0..15), en-US: 1 "One", 2 "Two", 15 "Fifteen"
  {
    size_t p = r + 0x320;
    const char* strs[16] = {"", "One", "Two", "", "", "", "", "", "", "", "", "", "", "", "", "Fifteen"};
    for (auto* s : strs) {
      size_t n = std::strlen(s);
      f.u16(p, static_cast<uint16_t>(n));
      f.utf16(p + 2, s);
      p += 2 + 2 * n;
    }
    rdata(0x110, 0x5320, static_cast<uint32_t>(p - (r + 0x320)));
    p = r + 0x380;
    const char* de[16] = {"", "Eins"};
    for (auto* s : de) {
      size_t n = s ? std::strlen(s) : 0;
      f.u16(p, static_cast<uint16_t>(n));
      if (n) f.utf16(p + 2, s);
      p += 2 + 2 * n;
    }
    rdata(0x120, 0x5380, static_cast<uint32_t>(p - (r + 0x380)));
  }
  std::string vi = version_resource(true);
  f.bytes(r + 0x400, vi);
  rdata(0x100, 0x5300, 5);
  rdata(0x130, 0x5400, static_cast<uint32_t>(vi.size()));
  rdata(0x140, 0x5310, 4);

  // .reloc
  size_t b = at(0x6000);
  f.u32(b, 0x1000);
  f.u32(b + 4, 24);
  const uint16_t page1[] = {0x3001, 0x3007, 0x1020, 0x2022, 0x4024, highadj_param, 0x0000, 0x0000};
  for (int i = 0; i < 8; i++) f.u16(b + 8 + 2 * i, page1[i]);
  b += 24;
  f.u32(b, 0x2000);
  f.u32(b + 4, 20);
  const uint16_t page2[] = {0x3500, 0x3504, 0x3508, 0x350C, 0x3520, 0x0000};
  for (int i = 0; i < 6; i++) f.u16(b + 8 + 2 * i, page2[i]);
  b += 20;
  f.u32(b, 0x3000);
  f.u32(b + 4, 12);
  f.u16(b + 8, 0x3000);
  return f.d;
}

// ---- synthetic NE DLL ---------------------------------------------------------
//
// Two segments (CODE 1, DATA 2 = DGROUP), sector shift 4, three imported
// modules, an entry table with a fixed bundle, a gap and a moveable bundle,
// string-typed and integer-typed resources, and one relocation record of every
// target kind and address type (chains, additive, OSFIXUP of each FP class).
//
// build_ne(true) adds what no corpus file has, so only this image covers it:
// a third segment with ITERATED data (and a fixup into the expanded bytes),
// and a constant (0xFE) entry bundle, ordinal 5 = 0x1234.
namespace ne_fixture {
constexpr uint32_t seg1_off = 0x200, seg1_len = 0x100;
constexpr uint32_t seg2_off = 0x400, seg2_len = 0x40, seg2_min = 0x80;
constexpr uint32_t seg3_off = 0x740, seg3_len = 14, seg3_min = 0x20;
constexpr uint16_t heap = 0x400;

struct Rec {
  uint8_t atype, flags;
  uint16_t off, a, b; // target bytes: internal (seg byte, 0, WORD), imports (WORD, WORD)
};
} // namespace ne_fixture

inline std::string build_ne(bool extras = false) {
  using namespace ne_fixture;
  Buf f;
  f.bytes(0, "MZ");
  f.u32(0x3C, 0x40);
  const size_t ne = 0x40;
  f.bytes(ne, "NE");
  f.u8(ne + 2, 5);
  f.u8(ne + 3, 60);
  f.u16(ne + 0x0C, 0x8009); // LIBRARY | PROTMODE | SINGLEDATA
  f.u16(ne + 0x0E, 2);      // DGROUP
  f.u16(ne + 0x10, heap);
  f.u16(ne + 0x14, 0x0000); // IP
  f.u16(ne + 0x16, 1);      // CS
  f.u16(ne + 0x1C, extras ? 3 : 2);
  f.u16(ne + 0x1E, 3);
  f.u16(ne + 0x30, 2);
  f.u16(ne + 0x32, 4);
  f.u8(ne + 0x36, 2);
  f.u16(ne + 0x3E, 0x030A);

  // Tables, in the usual order, right after the header.
  size_t p = ne + 0x40;
  f.u16(ne + 0x22, static_cast<uint16_t>(p - ne));
  // segment table
  f.u16(p, seg1_off >> 4);
  f.u16(p + 2, seg1_len);
  f.u16(p + 4, 0x0150); // MOVEABLE | PRELOAD | RELOCINFO
  f.u16(p + 6, seg1_len);
  f.u16(p + 8, seg2_off >> 4);
  f.u16(p + 10, seg2_len);
  f.u16(p + 12, 0x0141); // DATA | PRELOAD | RELOCINFO
  f.u16(p + 14, seg2_min);
  p += 16;
  if (extras) {
    f.u16(p, seg3_off >> 4);
    f.u16(p + 2, seg3_len);
    f.u16(p + 4, 0x0149); // DATA | ITERATED | PRELOAD | RELOCINFO
    f.u16(p + 6, seg3_min);
    p += 8;
  }
  // resource table
  size_t rt = p;
  f.u16(ne + 0x24, static_cast<uint16_t>(rt - ne));
  f.u16(rt, 4); // shift
  size_t q = rt + 2;
  // names area starts after 4 TYPEINFOs (8 bytes + 12 per NAMEINFO) + 0 WORD
  size_t names = rt + 2 + (8 + 2 * 12) + (8 + 12) * 3 + 2;
  size_t n_rlep = names, n_splash = names + 5;
  f.pstr(n_rlep, "RLEP");
  f.pstr(n_splash, "SPLASH");
  f.u8(n_splash + 7, 0);
  auto type = [&](uint16_t id, uint16_t count) {
    f.u16(q, id);
    f.u16(q + 2, count);
    q += 8;
  };
  auto res = [&](uint32_t file_off, uint32_t len, uint16_t id) {
    f.u16(q, static_cast<uint16_t>(file_off >> 4));
    f.u16(q + 2, static_cast<uint16_t>((len + 15) >> 4));
    f.u16(q + 4, 0x0030);
    f.u16(q + 6, id);
    q += 12;
  };
  std::string vi = version_resource(false);
  type(static_cast<uint16_t>(n_rlep - rt), 2);
  res(0x500, 5, static_cast<uint16_t>(n_splash - rt));
  res(0x510, 5, 0x8001);
  type(0x8006, 1);
  res(0x520, 0x30, 0x8001);
  type(0x8010, 1);
  res(0x580, static_cast<uint32_t>(vi.size()), 0x8001);
  type(0x8000 | 1000, 1);
  res(0x680, 4, 0x8000 | 2000);
  f.u16(q, 0);
  p = n_splash + 8;
  // resident names
  f.u16(ne + 0x26, static_cast<uint16_t>(p - ne));
  f.pstr(p, "TESTNE");
  f.u16(p + 7, 0);
  f.pstr(p + 9, "MODULE");
  f.u16(p + 16, 1);
  f.u8(p + 18, 0);
  p += 19;
  // module references → imported names
  size_t modref = p;
  f.u16(ne + 0x28, static_cast<uint16_t>(modref - ne));
  p += 6;
  size_t imp = p;
  f.u16(ne + 0x2A, static_cast<uint16_t>(imp - ne));
  f.u8(imp, 0);
  f.pstr(imp + 1, "KERNEL");
  f.pstr(imp + 8, "USER");
  f.pstr(imp + 13, "GDI");
  f.pstr(imp + 17, "MESSAGEBOX");
  f.u16(modref, 1);
  f.u16(modref + 2, 8);
  f.u16(modref + 4, 13);
  p = imp + 28;
  // entry table: #1 fixed seg 1, #2 unused, #3/#4 moveable (+ #5 constant)
  size_t et = p;
  f.u16(ne + 0x04, static_cast<uint16_t>(et - ne));
  std::vector<uint8_t> entries = {
      1, 1, 0x03, 0x00, 0x00,                                   // fixed bundle, seg 1
      1, 0,                                                     // skip ordinal 2
      2, 0xFF, 0x01, 0xCD, 0x3F, 1, 0xA0, 0x00, 0x01, 0xCD, 0x3F, 1, 0xB0, 0x00};
  if (extras) entries.insert(entries.end(), {1, 0xFE, 0x01, 0x34, 0x12}); // constant bundle
  entries.push_back(0);
  for (size_t i = 0; i < entries.size(); i++) f.u8(et + i, entries[i]);
  f.u16(ne + 0x06, static_cast<uint16_t>(entries.size()));
  // non-resident names at 0x700
  f.u32(ne + 0x2C, 0x700);
  f.pstr(0x700, "Test NE DLL");
  f.u16(0x70C, 0);
  f.pstr(0x70E, "HELPER");
  f.u16(0x715, 4);
  f.u8(0x717, 0);
  f.u16(ne + 0x20, 0x18);

  // Segment 1 (code) contents: 0x90 filler plus chain links and FP code.
  for (uint32_t i = 0; i < seg1_len; i++) f.u8(seg1_off + i, 0x90);
  auto s1 = [&](uint32_t off, uint16_t v) { f.u16(seg1_off + off, v); };
  s1(0x10, 0x0020); s1(0x20, 0x0030); s1(0x30, 0xFFFF);          // R1 chain of three
  s1(0x40, 0xFFFF);                                              // R2
  s1(0x44, 0xFFFF);                                              // R3
  s1(0x50, 0x0058); s1(0x58, 0xFFFF);                            // R4 chain of two
  s1(0x60, 0xFFFF);                                              // R5
  s1(0x68, 0xFFFF);                                              // R6 (LOBYTE; link is a WORD)
  f.u32(seg1_off + 0x6C, 0x00000010);                            // R7 additive base
  f.u32(seg1_off + 0x70, 0x0000FFFF);                            // R8 chain end in low word
  f.bytes(seg1_off + 0x80, "\x90\x9B");                          // R9  FIWRQQ: NOP; FWAIT
  f.bytes(seg1_off + 0x82, "\x9B\xD9\xC0");                      // R10 FIDRQQ: FLD ST(0)
  f.bytes(seg1_off + 0x86, "\x9B\x36\xD9\x07");                  // R11 FISRQQ: FLD SS:[BX]
  s1(0x90, 0x0004);                                              // R12 additive base
  s1(0x94, 0x0000);                                              // R13 Borland additive SELECTOR
  s1(0x98, 0x0002);                                              // R14 additive POINTER32
  f.bytes(seg1_off + 0x9C, "\x9B\x2E\xDD\x06");                  // R15 FICRQQ
  f.bytes(seg1_off + 0xC0, "\x9B\x3E\xD8\x06");                  // R16 FIARQQ
  f.bytes(seg1_off + 0xC4, "\x9B\x26\xD8\x06");                  // R17 FIERQQ
  const Rec recs1[] = {
      {5, 0, 0x10, 0x0002, 0x0010},   // R1  OFFSET16 internal 2:0010, chain
      {2, 0, 0x40, 0x0002, 0x0000},   // R2  SELECTOR internal seg 2
      {3, 0, 0x44, 0x00FF, 3},        // R3  POINTER32 moveable entry #3
      {3, 1, 0x50, 1, 3},             // R4  POINTER32 KERNEL.3, chain
      {3, 2, 0x60, 2, 17},            // R5  POINTER32 USER."MESSAGEBOX"
      {0, 0, 0x68, 0x0001, 0x1234},   // R6  LOBYTE internal 1:1234
      {13, 4, 0x6C, 0x0002, 0x0100},  // R7  OFFSET32 internal 2:0100, additive
      {11, 1, 0x70, 3, 1},            // R8  POINTER48 GDI.1
      {5, 7, 0x80, 6, 0},             // R9  OSFIXUP FIWRQQ (additive, as linkers emit)
      {5, 7, 0x82, 5, 0},             // R10 OSFIXUP FIDRQQ
      {5, 7, 0x86, 2, 0},             // R11 OSFIXUP FISRQQ/FJSRQQ
      {5, 5, 0x90, 1, 5},             // R12 OFFSET16 KERNEL.5 additive
      {2, 5, 0x94, 1, 5},             // R13 SELECTOR KERNEL.5 additive
      {3, 4, 0x98, 0x0002, 0x0020},   // R14 POINTER32 internal 2:0020 additive
      {5, 7, 0x9C, 3, 0},             // R15 OSFIXUP FICRQQ/FJCRQQ
      {5, 7, 0xC0, 1, 0},             // R16 OSFIXUP FIARQQ/FJARQQ
      {5, 7, 0xC4, 4, 0},             // R17 OSFIXUP FIERQQ
  };
  auto emit = [&](size_t at, const Rec* rs, size_t n) {
    f.u16(at, static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; i++) {
      size_t r = at + 2 + 8 * i;
      f.u8(r, rs[i].atype);
      f.u8(r + 1, rs[i].flags);
      f.u16(r + 2, rs[i].off);
      if ((rs[i].flags & 3) == 0) { // internal: segment byte, zero, WORD
        f.u8(r + 4, static_cast<uint8_t>(rs[i].a));
        f.u8(r + 5, 0);
      } else {
        f.u16(r + 4, rs[i].a);
      }
      f.u16(r + 6, rs[i].b);
    }
  };
  emit(seg1_off + seg1_len, recs1, sizeof(recs1) / sizeof(recs1[0]));

  // Segment 2 (data): a far pointer to code, as in a vtable.
  for (uint32_t i = 0; i < seg2_len; i++) f.u8(seg2_off + i, static_cast<uint8_t>(i));
  f.u16(seg2_off + 0x08, 0xFFFF);
  const Rec recs2[] = {{3, 0, 0x08, 0x0001, 0x0020}};
  emit(seg2_off + seg2_len, recs2, 1);

  // Resource data
  f.bytes(0x500, "hello");
  f.bytes(0x510, "world");
  {
    size_t s = 0x520;
    const char* strs[16] = {"", "One", "Two", "", "", "", "", "", "", "", "", "", "", "", "", "Fifteen"};
    for (auto* str : strs) {
      f.pstr(s, str);
      s += 1 + std::strlen(str);
    }
  }
  f.bytes(0x580, vi);
  f.bytes(0x680, "\x0A\x0B\x0C\x0D");
  f.grow(0x720);

  if (extras) {
    // Segment 3, iterated: 3 x "ABCD", then 1 x FFFF (a chain end that an
    // OFFSET16 fixup to 1:0042 overwrites after expansion); min_alloc 0x20
    // leaves 0x12 bytes of zero fill.
    size_t s = seg3_off;
    f.u16(s, 3);
    f.u16(s + 2, 4);
    f.bytes(s + 4, "ABCD");
    f.u16(s + 8, 1);
    f.u16(s + 10, 2);
    f.u16(s + 12, 0xFFFF);
    const Rec recs3[] = {{5, 0, 0x0C, 0x0001, 0x0042}};
    emit(seg3_off + seg3_len, recs3, 1);
  }
  return f.d;
}

} // namespace adw::loader::test
