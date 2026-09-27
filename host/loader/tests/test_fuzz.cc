// Robustness: every truncation and many thousands of random corruptions of
// the synthetic PE and NE images must either load or throw LoaderError —
// never crash, hang, or throw anything else. Deterministic (fixed seed).
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>

#include "builders.hh"
#include "loader/ne.hh"
#include "loader/pe.hh"

using namespace adw::loader;
using namespace adw::loader::test;

namespace {

// Accepts everything; bounds writes to what was reserved so a loader bug that
// writes outside its reservation is caught.
struct NullSink : ImageSink {
  uint32_t base_override = 0;
  std::map<uint32_t, uint32_t> sizes;
  uint32_t next = 0x100000;
  uint32_t reserve(uint32_t preferred, uint32_t size) override {
    uint32_t b = base_override ? base_override : (preferred ? preferred : next);
    next += 0x20000;
    sizes[b] = size;
    return b;
  }
  void write(uint32_t addr, const void*, size_t size) override {
    auto it = sizes.upper_bound(addr);
    if (it == sizes.begin() || addr - std::prev(it)->first + size > std::prev(it)->second) {
      throw std::logic_error("loader wrote outside its reservation");
    }
  }
};

size_t g_ok = 0, g_rejected = 0;

void try_one(const std::string& data, bool expect_pe) {
  try {
    if (expect_pe) {
      pe::Image img(data);
      NullSink sink;
      sink.base_override = 0x20000000;
      pe::load(img, sink, [](const pe::Import&) { return 0u; });
      (void)img.string_table();
      (void)img.version_info();
    } else {
      ne::Image img(data);
      NullSink sink;
      ne::LoadOptions opt;
      opt.os_fixups = ne::OsFixupMode::emulate;
      ne::load(img, sink, [](const ne::Target&) { return ne::FarPtr{1, 2}; }, opt);
      (void)img.string_table();
      (void)img.version_info();
    }
    g_ok++;
  } catch (const LoaderError&) {
    g_rejected++;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "non-LoaderError exception: %s\n", e.what());
    g_failures++;
  }
}

void fuzz(const std::string& seed, bool is_pe, uint32_t rng_seed, int iterations) {
  // Every prefix.
  for (size_t n = 0; n < seed.size(); n++) try_one(seed.substr(0, n), is_pe);
  // Random corruption: a few bytes, a random 16/32-bit field, or a run.
  std::mt19937 rng(rng_seed);
  for (int i = 0; i < iterations; i++) {
    std::string d = seed;
    int mode = rng() % 4;
    int count = 1 + rng() % 6;
    for (int k = 0; k < count; k++) {
      size_t at = rng() % d.size();
      switch (mode) {
        case 0:
          d[at] = static_cast<char>(rng());
          break;
        case 1:
          d[at] ^= static_cast<char>(1u << (rng() % 8));
          break;
        case 2: // a plausible-looking small or all-ones field
          for (size_t j = 0; j < 4 && at + j < d.size(); j++) d[at + j] = (rng() & 1) ? 0 : '\xFF';
          break;
        default: // header-heavy: aim at the first 1 KB where the tables live
          d[rng() % std::min<size_t>(d.size(), 0x400)] = static_cast<char>(rng());
          break;
      }
    }
    try_one(d, is_pe);
  }
}

// A small PE32 whose resource root holds `n` data entries that all name one
// 0xFFFF-character string: each entry would copy 64K of name, so ~4000 of
// them in 160 KB used to cost 250 MB (and the entry cap allowed ~16 GB).
std::string pe_with_shared_resource_name(uint32_t n) {
  const uint32_t rsrc_rva = 0x1000, raw = 0x400;
  const uint32_t str_off = 16 + 8 * n;
  const uint32_t data_off = str_off + 2 + 2 * 0xFFFF;
  const uint32_t rsize = data_off + 16;
  const uint32_t vsize = (rsize + 0xFFF) & ~0xFFFu;
  Buf f(raw + rsize);
  f.bytes(0, "MZ");
  f.u32(0x3C, 0x80);
  f.bytes(0x80, std::string_view("PE\0\0", 4));
  f.u16(0x84, 0x14C);
  f.u16(0x86, 1);
  f.u16(0x84 + 16, 0xE0);
  f.u16(0x84 + 18, 0x2102);
  const size_t o = 0x98;
  f.u16(o, 0x10B);
  f.u32(o + 28, 0x10000000);
  f.u32(o + 32, 0x1000);
  f.u32(o + 36, 0x200);
  f.u32(o + 56, rsrc_rva + vsize);
  f.u32(o + 60, 0x400);
  f.u32(o + 92, 16);
  f.u32(o + 96 + 8 * 2, rsrc_rva);
  f.u32(o + 100 + 8 * 2, rsize);
  f.bytes(0x178, ".rsrc");
  f.u32(0x178 + 8, vsize);
  f.u32(0x178 + 12, rsrc_rva);
  f.u32(0x178 + 16, rsize);
  f.u32(0x178 + 20, raw);
  f.u16(raw + 12, static_cast<uint16_t>(n)); // named entries
  for (uint32_t i = 0; i < n; i++) {
    f.u32(raw + 16 + 8 * i, 0x80000000u | str_off);
    f.u32(raw + 20 + 8 * i, data_off); // a data entry, straight from the root
  }
  f.u16(raw + str_off, 0xFFFF);
  for (uint32_t i = 0; i < 0xFFFF; i++) f.u16(raw + str_off + 2 + 2 * i, 'A');
  f.u32(raw + data_off, rsrc_rva + data_off);
  f.u32(raw + data_off + 4, 4);
  return f.d;
}

// Inputs whose tables stay small but ask for work or memory out of all
// proportion: they must fail (or finish) promptly with a LoaderError.
void test_amplification() {
  // A couple of long names is legitimate; thousands of copies are not.
  try {
    pe::Image few(pe_with_shared_resource_name(2));
    CHECK_EQ(few.resources().size(), 2u);
    if (!few.resources().empty()) CHECK_EQ(few.resources()[0].type.str.size(), 0xFFFFu);
  } catch (const LoaderError& e) {
    std::fprintf(stderr, "two long resource names rejected: %s\n", e.what());
    g_failures++;
  }
  CHECK_THROWS_KIND(pe::Image(pe_with_shared_resource_name(4000)), LoaderError::Kind::malformed);

  // An iterated segment made of 64K of empty records repeated 0xFFFF times:
  // it expands to nothing, and must do so without looping over the repeats
  // (that took ~3 s a segment, and every segment may share the same data).
  std::string d = build_ne(true);
  const size_t seg3_entry = 0x40 + 0x40 + 16;
  const uint32_t off = static_cast<uint32_t>((d.size() + 15) & ~size_t(15));
  Buf b;
  b.d = d;
  b.u16(seg3_entry, static_cast<uint16_t>(off >> 4));
  b.u16(seg3_entry + 2, 0);      // 64K of file data
  b.u16(seg3_entry + 4, 0x0049); // DATA | ITERATED | PRELOAD, no RELOCINFO
  for (uint32_t i = 0; i < 0x10000; i += 4) {
    b.u16(off + i, 0xFFFF);
    b.u16(off + i + 2, 0);
  }
  auto t0 = std::chrono::steady_clock::now();
  ne::Image img(b.d);
  std::string mem = img.segment_image(img.segment(3), ne_fixture::seg3_min);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(mem == std::string(ne_fixture::seg3_min, '\0'));
  // Generous: the fixed loop takes microseconds, the old one seconds.
  CHECK(secs < 1.0);
}

} // namespace

int main() {
  test_amplification();
  std::string pe = build_pe();
  std::string ne = build_ne();
  try_one(pe, true);
  try_one(ne, false);
  CHECK_EQ(g_ok, 2u); // the pristine images load
  try_one(build_ne(true), false);
  CHECK_EQ(g_ok, 3u);
  fuzz(pe, true, 1234, 30000);
  fuzz(ne, false, 5678, 30000);
  // The variant with an iterated segment and a constant entry bundle.
  fuzz(build_ne(true), false, 9012, 20000);
  std::printf("fuzz: %zu loaded, %zu rejected with LoaderError\n", g_ok, g_rejected);
  CHECK(g_rejected > 0);
  return finish("test_loader_fuzz");
}
