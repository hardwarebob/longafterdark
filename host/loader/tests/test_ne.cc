// Synthetic NE DLL: parse every table, then place + relocate through a
// resolver and check each relocation kind, address type, chain, ADDITIVE
// record and OSFIXUP (left alone and emulated) byte for byte.
#include <cstdio>

#include "builders.hh"
#include "loader/ne.hh"

using namespace adw::loader;
using namespace adw::loader::test;
using namespace adw::loader::test::ne_fixture;

static void test_parse() {
  ne::Image img(build_ne());
  const auto& h = img.header();
  CHECK(detect_format(img.file()) == Format::ne);
  CHECK(h.is_dll());
  CHECK_EQ(h.autodata_segment, 2);
  CHECK_EQ(h.heap_size, heap);
  CHECK_EQ(h.cs, 1);
  CHECK_EQ(h.expected_version, 0x030A);
  CHECK_STR(img.module_name(), "TESTNE");
  CHECK_STR(img.description(), "Test NE DLL");

  CHECK_EQ(img.segments().size(), 2u);
  const ne::Segment& s1 = img.segment(1);
  CHECK_EQ(s1.file_offset, seg1_off);
  CHECK_EQ(s1.file_size, seg1_len);
  CHECK(!s1.is_data() && s1.is_moveable() && s1.has_relocs());
  CHECK_EQ(s1.relocations.size(), 17u);
  const ne::Segment& s2 = img.segment(2);
  CHECK(s2.is_data());
  CHECK_EQ(s2.min_alloc, seg2_min);
  CHECK_EQ(s2.relocations.size(), 1u);
  CHECK_THROWS_KIND(img.segment(3), LoaderError::Kind::malformed);

  // Relocation records decode with names resolved from the tables.
  if (s1.relocations.size() == 17) {
    const auto& r = s1.relocations;
    CHECK(r[0].kind == ne::TargetKind::internal && r[0].addr_type == ne::AddrType::offset16);
    CHECK_EQ(r[0].segment, 2);
    CHECK_EQ(r[0].target_offset, 0x10);
    CHECK_EQ(r[2].segment, 0xFF);
    CHECK_EQ(r[2].entry_ordinal, 3);
    CHECK(r[3].kind == ne::TargetKind::import_ordinal);
    CHECK_STR(r[3].module, "KERNEL");
    CHECK_EQ(r[3].ordinal, 3);
    CHECK(r[4].kind == ne::TargetKind::import_name);
    CHECK_STR(r[4].module, "USER");
    CHECK_STR(r[4].name, "MESSAGEBOX");
    CHECK(r[5].addr_type == ne::AddrType::lobyte);
    CHECK(r[6].addr_type == ne::AddrType::offset32 && r[6].additive);
    CHECK(r[7].addr_type == ne::AddrType::pointer48);
    CHECK_STR(r[7].module, "GDI");
    CHECK(r[8].kind == ne::TargetKind::os_fixup);
    CHECK_EQ(r[8].os_fixup, 6);
  }

  // Entry table: fixed #1, gap at #2, moveable #3/#4.
  CHECK_EQ(img.entries().size(), 3u);
  const ne::Entry* e1 = img.find_entry(1);
  CHECK(e1 && !e1->moveable && e1->segment == 1 && e1->offset == 0 && e1->exported() && e1->shared_data());
  CHECK(img.find_entry(2) == nullptr);
  const ne::Entry* e3 = img.find_entry(3);
  CHECK(e3 && e3->moveable && e3->segment == 1 && e3->offset == 0xA0);
  const ne::Entry* e4 = img.find_entry(4);
  CHECK(e4 && e4->offset == 0xB0);

  // Names: resident and non-resident, case-insensitive like GetProcAddress.
  CHECK_EQ(img.resident_names().size(), 2u);
  CHECK_EQ(img.nonresident_names().size(), 2u);
  CHECK(img.find_ordinal("module") == std::optional<uint16_t>(1));
  CHECK(img.find_ordinal("Helper") == std::optional<uint16_t>(4));
  CHECK(!img.find_ordinal("TESTNE").has_value()); // the module name is not an export
  CHECK(img.find_export("Module") == e1);
  CHECK(img.find_export("helper") == e4);
  CHECK(img.find_export("nothing") == nullptr);
  CHECK_EQ(img.module_refs().size(), 3u);
  CHECK_STR(img.imported_name(17), "MESSAGEBOX");

  // Resources: string type with a string and an integer name, RT_STRING,
  // RT_VERSION (16-bit layout), integer custom type.
  CHECK_EQ(img.resources().size(), 5u);
  CHECK_EQ(img.resource_alignment_shift(), 4);
  const ne::Resource* splash = img.find_resource(ResId::of("RLEP"), ResId::of("splash"));
  CHECK(splash != nullptr);
  if (splash) {
    CHECK(splash->type.is_string && splash->name.is_string);
    CHECK_EQ(splash->size, 16u); // alignment units
    CHECK_STR(std::string(img.resource_data(*splash).substr(0, 5)), "hello");
  }
  const ne::Resource* rl1 = img.find_resource(ResId::of("rlep"), ResId::of(uint16_t(1)));
  CHECK(rl1 && std::string(img.resource_data(*rl1).substr(0, 5)) == "world");
  const ne::Resource* custom = img.find_resource(ResId::of(uint16_t(1000)), ResId::of(uint16_t(2000)));
  CHECK(custom && img.resource_data(*custom)[0] == 0x0A);
  auto st = img.string_table();
  CHECK_EQ(st.size(), 3u);
  CHECK_STR(st[1], "One");
  CHECK_STR(st[15], "Fifteen");
  auto vi = img.version_info();
  CHECK(vi.has_value());
  if (vi) {
    CHECK(vi->has_fixed);
    CHECK_STR(vi->file_version(), "4.0.0.1");
    CHECK_STR(vi->string("FileDescription"), "Synthetic NE");
    CHECK_EQ(vi->translations.size(), 1u);
  }
  CHECK(img.warnings().empty());
}

static uint16_t sel_of(uint16_t seg) { return static_cast<uint16_t>(0x1000 + seg * 8); }

struct Fixture {
  ne::Image img{build_ne()};
  TestSink sink;
  ne::Loaded ld;
  size_t resolver_calls = 0;

  explicit Fixture(ne::LoadOptions opt = {}) {
    ld = ne::load(img, sink, [&](const ne::Target& t) -> ne::FarPtr {
      resolver_calls++;
      switch (t.kind) {
        case ne::TargetKind::internal:
          // The loader resolved moveable entries to segment:offset already
          // and says where it put the segment.
          CHECK_EQ(t.segment_base, ld_base(t.segment));
          return {sel_of(t.segment), t.offset};
        case ne::TargetKind::import_ordinal:
          if (t.module == "GDI") return {0x2000, 0x12345}; // a 32-bit offset for POINTER48
          return {static_cast<uint16_t>(0x2000 + t.module_index * 8), static_cast<uint32_t>(0x100 + t.ordinal)};
        case ne::TargetKind::import_name:
          CHECK(t.module == "USER" && t.name == "MESSAGEBOX");
          return {0x3000, 0x7777};
        default:
          CHECK(!"resolver asked about an OSFIXUP");
          return {};
      }
    }, opt);
  }
  // Placement is known once reserve() ran; the sink hands out bases in order,
  // 0x20000 apart for segments under 64K.
  uint32_t ld_base(uint16_t seg) { return 0x00100000u + 0x20000u * (seg - 1u); }
  uint32_t s1(uint32_t off) { return ld.placement.segment(1).base + off; }
  uint32_t s2(uint32_t off) { return ld.placement.segment(2).base + off; }
};

static void test_load() {
  Fixture fx;
  auto& sink = fx.sink;
  const auto& pl = fx.ld.placement;
  CHECK_EQ(pl.segments.size(), 2u);
  CHECK_EQ(pl.segment(1).base, 0x00100000u);
  CHECK_EQ(pl.segment(1).size, seg1_len);
  CHECK_EQ(pl.segment(2).base, 0x00120000u);
  CHECK_EQ(pl.segment(2).size, seg2_min + heap); // DGROUP gets the local heap
  CHECK_EQ(sink.writes, 2u);

  const auto& st = fx.ld.stats;
  CHECK_EQ(st.records, 18u);
  CHECK_EQ(st.os_fixups, 6u);
  CHECK_EQ(st.os_fixups_applied, 0u);
  CHECK_EQ(st.additive, 4u);
  CHECK_EQ(st.locations, 15u); // 12 non-OSFIXUP records + 3 extra chain links
  CHECK_EQ(fx.resolver_calls, 12u);

  // R1: OFFSET16 chain 0x10 → 0x20 → 0x30
  CHECK_EQ(sink.u16(fx.s1(0x10)), 0x0010);
  CHECK_EQ(sink.u16(fx.s1(0x20)), 0x0010);
  CHECK_EQ(sink.u16(fx.s1(0x30)), 0x0010);
  // R2: SELECTOR of segment 2
  CHECK_EQ(sink.u16(fx.s1(0x40)), sel_of(2));
  // R3: POINTER32 through moveable entry #3 → 1:00A0
  CHECK_EQ(sink.u16(fx.s1(0x44)), 0x00A0);
  CHECK_EQ(sink.u16(fx.s1(0x46)), sel_of(1));
  // R4: POINTER32 KERNEL.3, chain of two
  for (uint32_t at : {0x50u, 0x58u}) {
    CHECK_EQ(sink.u16(fx.s1(at)), 0x103);
    CHECK_EQ(sink.u16(fx.s1(at + 2)), 0x2008);
  }
  // R5: POINTER32 by name
  CHECK_EQ(sink.u16(fx.s1(0x60)), 0x7777);
  CHECK_EQ(sink.u16(fx.s1(0x62)), 0x3000);
  // R6: LOBYTE of 1:1234, the next byte untouched
  CHECK_EQ(sink.u8(fx.s1(0x68)), 0x34);
  CHECK_EQ(sink.u8(fx.s1(0x69)), 0xFF);
  // R7: additive OFFSET32: 0x10 + 0x100
  CHECK_EQ(sink.u32(fx.s1(0x6C)), 0x110u);
  // R8: POINTER48 GDI.1: 32-bit offset then selector
  CHECK_EQ(sink.u32(fx.s1(0x70)), 0x12345u);
  CHECK_EQ(sink.u16(fx.s1(0x74)), 0x2000);
  // R9-R11, R15-R17: OSFIXUPs left alone (real x87 code stays)
  CHECK_EQ(sink.u16(fx.s1(0x80)), 0x9B90);
  CHECK_EQ(sink.u16(fx.s1(0x82)), 0xD99B);
  CHECK_EQ(sink.u16(fx.s1(0x86)), 0x369B);
  // R12: additive OFFSET16 KERNEL.5: 4 + 0x105
  CHECK_EQ(sink.u16(fx.s1(0x90)), 0x109);
  // R13: additive SELECTOR (Borland style, over zero)
  CHECK_EQ(sink.u16(fx.s1(0x94)), 0x2008);
  // R14: additive POINTER32 internal: 2 + 0x20, selector stored
  CHECK_EQ(sink.u16(fx.s1(0x98)), 0x22);
  CHECK_EQ(sink.u16(fx.s1(0x9A)), sel_of(2));
  // Untouched filler, and bytes past the file data up to the allocation
  CHECK_EQ(sink.u8(fx.s1(0x00)), 0x90);
  CHECK_EQ(sink.u8(fx.s1(0xFF)), 0x90);
  // Segment 2: file data, a far pointer to 1:0020, then zero fill to 0x480
  CHECK_EQ(sink.u8(fx.s2(0x03)), 0x03);
  CHECK_EQ(sink.u16(fx.s2(0x08)), 0x0020);
  CHECK_EQ(sink.u16(fx.s2(0x0A)), sel_of(1));
  CHECK_EQ(sink.u8(fx.s2(0x40)), 0);
  CHECK_EQ(sink.u8(fx.s2(seg2_min + heap - 1)), 0);
}

static void test_os_fixups_emulated() {
  ne::LoadOptions opt;
  opt.os_fixups = ne::OsFixupMode::emulate;
  Fixture fx(opt);
  auto& sink = fx.sink;
  CHECK_EQ(fx.ld.stats.os_fixups_applied, 6u);
  // FIWRQQ: NOP FWAIT → INT 3Dh
  CHECK_EQ(sink.u8(fx.s1(0x80)), 0xCD);
  CHECK_EQ(sink.u8(fx.s1(0x81)), 0x3D);
  // FIDRQQ: FWAIT; D9 C0 → INT 35h; C0
  CHECK_EQ(sink.u8(fx.s1(0x82)), 0xCD);
  CHECK_EQ(sink.u8(fx.s1(0x83)), 0x35);
  CHECK_EQ(sink.u8(fx.s1(0x84)), 0xC0);
  // FISRQQ/FJSRQQ: FWAIT SS: D9 07 → INT 3Ch; 59 07
  CHECK_EQ(sink.u8(fx.s1(0x86)), 0xCD);
  CHECK_EQ(sink.u8(fx.s1(0x87)), 0x3C);
  CHECK_EQ(sink.u8(fx.s1(0x88)), 0x59);
  CHECK_EQ(sink.u8(fx.s1(0x89)), 0x07);
  // FICRQQ/FJCRQQ: FWAIT CS: DD 06 → INT 3Ch; 9D 06
  CHECK_EQ(sink.u16(fx.s1(0x9C)), 0x3CCD);
  CHECK_EQ(sink.u8(fx.s1(0x9E)), 0x9D);
  // FIARQQ/FJARQQ: FWAIT DS: D8 06 → INT 3Ch; 18 06
  CHECK_EQ(sink.u16(fx.s1(0xC0)), 0x3CCD);
  CHECK_EQ(sink.u8(fx.s1(0xC2)), 0x18);
  // FIERQQ: FWAIT ES: D8 06 → INT 3Ch; D8 06 (ES's segment tag is the ESC
  // byte's own top bits 11, so there is no FJ half)
  CHECK_EQ(sink.u16(fx.s1(0xC4)), 0x3CCD);
  CHECK_EQ(sink.u8(fx.s1(0xC6)), 0xD8);
  CHECK_EQ(sink.u8(fx.s1(0xC7)), 0x06);
  // No emulated site is left holding a real FWAIT.
  for (uint32_t at : {0x80u, 0x82u, 0x86u, 0x9Cu, 0xC0u, 0xC4u}) CHECK_EQ(sink.u8(fx.s1(at)), 0xCD);
}

static void test_place_options() {
  ne::Image img(build_ne());
  ne::LoadOptions opt;
  opt.reserve_64k = true;
  opt.autodata_heap_stack = false;
  TestSink sink;
  ne::Placement pl = ne::place(img, sink, opt);
  CHECK_EQ(pl.segment(1).reserved, 0x10000u);
  CHECK_EQ(pl.segment(2).size, seg2_min);
  CHECK_EQ(sink.blocks.size(), 2u);
  // Two-step load: the host may assign selectors between place and load.
  auto st = ne::load_segments(img, pl, sink, [](const ne::Target&) { return ne::FarPtr{0x77, 0}; }, opt);
  CHECK_EQ(st.records, 18u);
  CHECK_EQ(sink.u16(pl.segment(1).base + 0x40), 0x77);
  // The whole 64K reservation is written (the sink pre-fills 0xCC), so a host
  // growing the segment in place finds zeros past the segment's own size.
  CHECK_EQ(sink.u8(pl.segment(1).base + seg1_len), 0);
  CHECK_EQ(sink.u8(pl.segment(1).base + 0xFFFF), 0);
  CHECK_EQ(sink.u8(pl.segment(2).base + 0xFFFF), 0);
  CHECK_EQ(sink.writes, 2u);
}

// Iterated segment data and a constant entry bundle: no corpus file has
// either, so this synthetic image is their only coverage.
static void test_iterated_and_constant() {
  ne::Image img(build_ne(true));
  CHECK_EQ(img.segments().size(), 3u);
  CHECK(img.warnings().empty());
  const ne::Segment& s3 = img.segment(3);
  CHECK(s3.is_data() && (s3.flags & ne::seg_iterated) && s3.has_relocs());
  CHECK_EQ(s3.file_size, seg3_len);
  CHECK_EQ(s3.min_alloc, seg3_min);
  CHECK_EQ(s3.relocations.size(), 1u);
  CHECK_EQ(ne::segment_alloc_size(img, s3, {}), seg3_min);
  std::string mem = img.segment_image(s3, seg3_min);
  CHECK_EQ(mem.size(), size_t(seg3_min));
  CHECK_STR(mem.substr(0, 12), "ABCDABCDABCD");
  CHECK_EQ(static_cast<uint8_t>(mem[12]), 0xFF);
  CHECK_EQ(static_cast<uint8_t>(mem[13]), 0xFF);
  CHECK(mem.find_first_not_of('\0', 14) == std::string::npos);

  // Constant entry #5 follows the moveable bundle; earlier ordinals unchanged.
  CHECK_EQ(img.entries().size(), 4u);
  const ne::Entry* e5 = img.find_entry(5);
  CHECK(e5 && e5->segment == 0xFE && e5->offset == 0x1234 && e5->exported() && !e5->moveable);
  const ne::Entry* e4 = img.find_entry(4);
  CHECK(e4 && e4->moveable && e4->offset == 0xB0);

  auto resolve = [](const ne::Target& t) -> ne::FarPtr {
    if (t.kind == ne::TargetKind::internal) return {sel_of(t.segment), t.offset};
    return {0x2000, 0x100};
  };
  TestSink sink;
  ne::Loaded ld = ne::load(img, sink, resolve);
  CHECK_EQ(sink.writes, 3u);
  CHECK_EQ(ld.stats.records, 19u);
  const auto& p3 = ld.placement.segment(3);
  CHECK_EQ(p3.size, seg3_min);
  for (uint32_t i = 0; i < 12; i++) CHECK_EQ(sink.u8(p3.base + i), static_cast<uint8_t>("ABCD"[i % 4]));
  CHECK_EQ(sink.u16(p3.base + 0x0C), 0x0042); // fixed up after expansion
  for (uint32_t i = 14; i < seg3_min; i++) CHECK_EQ(sink.u8(p3.base + i), 0);

  auto load_throws = [&](std::string data, LoaderError::Kind k) {
    ne::Image bad(std::move(data));
    TestSink s;
    CHECK_THROWS_KIND(ne::load(bad, s, resolve), k);
  };
  std::string d = build_ne(true);
  // Iterated data that expands past its min_alloc (8 < 14 bytes)
  std::string small = d;
  size_t seg3_entry = 0x40 + 0x40 + 16;
  small[seg3_entry + 6] = 8;
  small[seg3_entry + 7] = 0;
  load_throws(small, LoaderError::Kind::malformed);
  // A moveable-style internal reference that names the constant entry
  std::string to_const = d;
  size_t r3 = seg1_off + seg1_len + 2 + 8 * 2;
  to_const[r3 + 6] = 5;
  load_throws(to_const, LoaderError::Kind::unsupported);
}

static void test_rejects() {
  std::string d = build_ne();
  CHECK_THROWS_KIND(ne::Image(std::string("MZ")), LoaderError::Kind::bad_format);
  std::string pe = d;
  pe[0x40] = 'P';
  CHECK_THROWS_KIND(ne::Image(pe), LoaderError::Kind::bad_format);
  // Relocation naming module 9 of 3
  std::string badmod = d;
  size_t r4 = seg1_off + seg1_len + 2 + 8 * 3; // R4 record
  badmod[r4 + 4] = 9;
  CHECK_THROWS_KIND(ne::Image(badmod), LoaderError::Kind::malformed);
  // Unknown address type
  std::string badtype = d;
  badtype[r4] = 7;
  CHECK_THROWS_KIND(ne::Image(badtype), LoaderError::Kind::unsupported);
  auto load_throws = [](std::string data, LoaderError::Kind k) {
    ne::Image img(std::move(data));
    TestSink sink;
    CHECK_THROWS_KIND(ne::load(img, sink, [](const ne::Target&) { return ne::FarPtr{}; }), k);
  };
  // Chain that loops: 0x30 → 0x10
  std::string loop = d;
  loop[seg1_off + 0x30] = 0x10;
  loop[seg1_off + 0x31] = 0x00;
  load_throws(loop, LoaderError::Kind::malformed);
  // Chain that leaves the segment
  std::string out = d;
  out[seg1_off + 0x31] = 0x40; // 0x30 → 0x4000, past the 0x100-byte segment
  out[seg1_off + 0x30] = 0x00;
  load_throws(out, LoaderError::Kind::malformed);
  // Moveable reference to an entry that does not exist
  std::string noent = d;
  size_t r3 = seg1_off + seg1_len + 2 + 8 * 2;
  noent[r3 + 6] = 2; // entry #2 is the gap
  load_throws(noent, LoaderError::Kind::malformed);
  // Internal reference to segment 5
  std::string noseg = d;
  size_t r1 = seg1_off + seg1_len + 2;
  noseg[r1 + 4] = 5;
  load_throws(noseg, LoaderError::Kind::malformed);
  // Self-loading applications are not supported
  std::string selfload = d;
  selfload[0x40 + 0x0D] |= 0x08;
  load_throws(selfload, LoaderError::Kind::unsupported);
  // Segment data past the end of the file
  CHECK_THROWS_KIND(ne::Image(d.substr(0, 0x2F0)), LoaderError::Kind::truncated);
}

int main() {
  test_parse();
  test_load();
  test_os_fixups_emulated();
  test_place_options();
  test_iterated_and_constant();
  test_rejects();
  return finish("test_loader_ne");
}
