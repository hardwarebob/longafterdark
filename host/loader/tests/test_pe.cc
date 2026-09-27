// Synthetic PE32 DLL: parse every structure, then load at the preferred base
// and at two other bases (one 64K-aligned, one that exercises LOW/HIGHADJ
// carries) and check every fixup and IAT slot byte for byte.
#include <cstdio>

#include "loader/pe.hh"
#include "builders.hh"

using namespace adw::loader;
using namespace adw::loader::test;
using namespace adw::loader::test::pe_fixture;

static void test_parse() {
  pe::Image img(build_pe());
  const auto& h = img.header();
  CHECK_EQ(h.machine, pe::machine_i386);
  CHECK(h.is_dll());
  CHECK_EQ(h.image_base, image_base);
  CHECK_EQ(h.size_of_image, size_of_image);
  CHECK_EQ(img.sections().size(), 5u);
  CHECK_STR(img.sections()[0].name, ".text");
  CHECK_EQ(img.sections()[2].mapped_size, 0x2000u); // 0x1800 aligned up
  CHECK(img.warnings().empty());
  CHECK(detect_format(img.file()) == Format::pe32);

  // Imports: by name, by ordinal, and a Borland-style OFT=0 descriptor.
  const auto& imps = img.imports();
  CHECK_EQ(imps.size(), 3u);
  if (imps.size() == 3) {
    CHECK_STR(imps[0].dll, "ADXPL510.dll");
    CHECK_STR(imps[0].name, "Foo");
    CHECK_EQ(imps[0].hint, 3);
    CHECK(!imps[0].by_ordinal);
    CHECK_EQ(imps[0].slot_rva, slot_foo);
    CHECK_EQ(imps[0].slot, image_base + slot_foo);
    CHECK(imps[1].by_ordinal);
    CHECK_EQ(imps[1].ordinal, 7);
    CHECK_EQ(imps[1].slot_rva, slot_ord7);
    CHECK_STR(imps[2].dll, "KERNEL32.dll");
    CHECK_STR(imps[2].name, "GetTickCount");
    CHECK_EQ(imps[2].slot_rva, slot_gettick);
  }
  auto dlls = img.imported_dlls();
  CHECK_EQ(dlls.size(), 2u);

  // Exports: alias names share an ordinal, #2 is a hole, #3 forwards.
  const auto& ed = img.export_directory();
  CHECK(ed.present);
  CHECK_STR(ed.dll_name, "TEST.AD");
  CHECK_EQ(ed.ordinal_base, 1u);
  CHECK_EQ(ed.exports.size(), 4u); // Module, ModuleAlias, Fwd, #4
  const pe::Export* m = img.find_export("Module");
  CHECK(m && m->ordinal == 1 && m->rva == 0x1000 && !m->is_forwarder());
  const pe::Export* alias = img.find_export("ModuleAlias");
  CHECK(alias && alias->ordinal == 1 && alias->rva == 0x1000);
  CHECK(img.find_export(uint16_t(1)) != nullptr);
  CHECK(img.find_export(uint16_t(2)) == nullptr);
  const pe::Export* fwd = img.find_export("Fwd");
  CHECK(fwd && fwd->ordinal == 3 && fwd->is_forwarder());
  if (fwd) CHECK_STR(fwd->forwarder, "KERNEL32.GetTickCount");
  const pe::Export* o4 = img.find_export(uint16_t(4));
  CHECK(o4 && o4->name.empty() && o4->rva == 0x1010);
  CHECK(img.find_export("module") == nullptr); // GetProcAddress is case-sensitive

  // Base relocations (ABSOLUTE padding dropped, HIGHADJ keeps its parameter).
  const auto& rel = img.relocations();
  CHECK_EQ(rel.size(), 11u);
  size_t highadj = 0;
  for (const auto& r : rel) {
    if (r.type == pe::rel_highadj) {
      highadj++;
      CHECK_EQ(r.rva, 0x1024u);
      CHECK_EQ(r.param, highadj_param);
    }
  }
  CHECK_EQ(highadj, 1u);

  // Resources: string type + string name, RT_STRING in two languages,
  // RT_VERSION, a custom integer type.
  CHECK_EQ(img.resources().size(), 5u);
  const pe::Resource* rlep = img.find_resource(ResId::of("rlep"), ResId::of("Splash"));
  CHECK(rlep != nullptr);
  if (rlep) {
    CHECK(rlep->type.is_string);
    CHECK_STR(rlep->type.str, "RLEP");
    CHECK_STR(std::string(img.resource_data(*rlep)), "hello");
  }
  const pe::Resource* custom = img.find_resource(ResId::of(uint16_t(1000)), ResId::of("#5"));
  CHECK(custom && custom->size == 4 && custom->codepage == 1252);
  const pe::Resource* de = img.find_resource(ResId::of(rt::string), ResId::of(uint16_t(1)), 0x407);
  CHECK(de && de->language == 0x407);
  auto st = img.string_table();
  CHECK_EQ(st.size(), 3u);
  CHECK_STR(st[1], "One");
  CHECK_STR(st[2], "Two");
  CHECK_STR(st[15], "Fifteen");
  auto st_de = img.string_table(0x407);
  CHECK_EQ(st_de.size(), 1u);
  CHECK_STR(st_de[1], "Eins");
  auto vi = img.version_info();
  CHECK(vi.has_value());
  if (vi) {
    CHECK(vi->has_fixed);
    CHECK_STR(vi->file_version(), "4.0.0.1");
    CHECK_EQ(vi->file_type, 2u);
    CHECK_STR(vi->string("CompanyName"), "Test Co");
    CHECK_STR(vi->string("FileDescription"), "Synthetic PE");
    CHECK_EQ(vi->string_tables.size(), 1u);
    if (!vi->string_tables.empty()) CHECK_STR(vi->string_tables[0].key, "040904E4");
    CHECK_EQ(vi->translations.size(), 1u);
    if (!vi->translations.empty()) CHECK_EQ(vi->translations[0], 0x04E40409u);
  }

  // TLS is reported, not run.
  CHECK(img.tls().has_value());
  if (img.tls()) {
    CHECK_EQ(img.tls()->start_va, image_base + 0x3100);
    CHECK_EQ(img.tls()->callbacks.size(), 1u);
    if (!img.tls()->callbacks.empty()) CHECK_EQ(img.tls()->callbacks[0], image_base + 0x100B);
  }

  // The mapped view: sections placed, BSS zero.
  CHECK_EQ(img.mapped().size(), size_t(size_of_image));
  CHECK_STR(std::string(img.view(0x3100, 7)), "TLSDATA");
  bool bss_zero = true;
  for (uint32_t a = 0x3200; a < 0x5000; a++) bss_zero &= img.mapped()[a] == 0;
  CHECK(bss_zero);
  CHECK_THROWS_KIND(img.view(size_of_image - 2, 4), LoaderError::Kind::truncated);
}

static uint32_t fake_thunk(const pe::Import& imp) {
  // Distinct, recognizable values per import.
  if (imp.by_ordinal) return 0xFE000000 | imp.ordinal;
  return 0xFE100000 | static_cast<uint32_t>(imp.name.size());
}

static void check_loaded(uint32_t base) {
  pe::Image img(build_pe());
  TestSink sink;
  if (base != image_base) sink.force_base = base;
  std::vector<std::string> seen;
  pe::Loaded ld = pe::load(img, sink, [&](const pe::Import& imp) {
    seen.push_back(imp.dll + "!" + (imp.by_ordinal ? "#" + std::to_string(imp.ordinal) : imp.name));
    CHECK_EQ(imp.slot, base + imp.slot_rva);
    return fake_thunk(imp);
  });
  uint32_t delta = base - image_base;
  CHECK_EQ(ld.base, base);
  CHECK_EQ(ld.delta, delta);
  CHECK_EQ(ld.size, size_of_image);
  CHECK_EQ(ld.entry, base + 0x1000);
  CHECK_EQ(sink.writes, 1u);
  CHECK_EQ(seen.size(), 3u);
  CHECK_EQ(ld.relocations_applied, delta ? 11u : 0u);

  // HIGHLOW targets
  CHECK_EQ(sink.u32(base + 0x1001), image_base + 0x3000 + delta);
  CHECK_EQ(sink.u32(base + 0x1007), image_base + slot_foo + delta);
  CHECK_EQ(sink.u32(base + 0x3000), image_base + 0x1000 + delta);
  CHECK_EQ(sink.u32(base + 0x2500), image_base + 0x3100 + delta);
  CHECK_EQ(sink.u32(base + 0x2520), image_base + 0x100B + delta);
  // HIGH / LOW / HIGHADJ
  CHECK_EQ(sink.u16(base + 0x1020), static_cast<uint16_t>(0x1000 + (delta >> 16)));
  CHECK_EQ(sink.u16(base + 0x1022), static_cast<uint16_t>(0x1234 + delta));
  uint32_t full = 0x10009000u + delta;
  CHECK_EQ(sink.u16(base + 0x1024), static_cast<uint16_t>((full + 0x8000) >> 16));
  // The HIGHADJ contract: stored high half + the sign-extended *relocated*
  // low half (which its paired LOW fixup produces) = the relocated address.
  CHECK_EQ((uint32_t(sink.u16(base + 0x1024)) << 16) + static_cast<int16_t>(highadj_param + delta), full);
  // IAT bound through the resolver
  CHECK_EQ(sink.u32(base + slot_foo), 0xFE100003u);
  CHECK_EQ(sink.u32(base + slot_ord7), 0xFE000007u);
  CHECK_EQ(sink.u32(base + slot_gettick), 0xFE10000Cu);
  CHECK_EQ(ld.imports.size(), 3u);
  if (ld.imports.size() == 3) CHECK_EQ(ld.imports[1].value, 0xFE000007u);
  // Header ImageBase reflects the actual base; BSS zeroed over the 0xCC fill.
  CHECK_EQ(sink.u32(base + 0x80 + 24 + 28), base);
  CHECK_EQ(sink.u8(base + 0x4FFF), 0);
  CHECK_EQ(sink.u8(base + size_of_image - 1), 0);
  // TLS reported at the actual base
  CHECK(ld.tls.has_value());
  if (ld.tls) {
    CHECK_EQ(ld.tls->index_va, image_base + 0x3120 + delta);
    CHECK_EQ(ld.tls->callbacks.size(), 1u);
  }
}

static void test_load() {
  check_loaded(image_base);  // preferred
  check_loaded(0x30000000);  // 64K-aligned move
  check_loaded(0x1000F000);  // page-aligned: LOW changes, HIGHADJ carries
}

static void test_no_resolver() {
  pe::Image img(build_pe());
  TestSink sink;
  pe::Loaded ld = pe::load(img, sink);
  // The IAT keeps the file's lookup entries.
  CHECK_EQ(sink.u32(image_base + slot_foo), 0x2230u);
  if (!ld.imports.empty()) CHECK_EQ(ld.imports[0].value, 0x2230u);
}

static void test_relocs_stripped() {
  pe::Image img(build_pe(true));
  CHECK(img.header().relocs_stripped());
  TestSink ok;
  pe::load(img, ok); // at ImageBase it loads fine
  TestSink moved;
  moved.force_base = 0x20000000;
  CHECK_THROWS_KIND(pe::load(img, moved), LoaderError::Kind::placement);
  // A base whose image would wrap past 4 GB is refused before any write.
  pe::Image movable(build_pe());
  TestSink wrap;
  wrap.force_base = 0xFFFFC000;
  CHECK_THROWS_KIND(pe::load(movable, wrap), LoaderError::Kind::placement);
  CHECK_EQ(wrap.writes, 0u);
}

static void test_rejects() {
  std::string d = build_pe();
  // Not PE / wrong magic / PE32+ / not i386
  CHECK_THROWS_KIND(pe::Image(std::string("hello")), LoaderError::Kind::bad_format);
  std::string ne = d;
  ne[0x80] = 'N';
  ne[0x81] = 'E';
  CHECK_THROWS_KIND(pe::Image(ne), LoaderError::Kind::bad_format);
  std::string plus = d;
  plus[0x98] = 0x0B;
  plus[0x99] = 0x02;
  CHECK(detect_format(plus) == Format::pe32plus);
  CHECK_THROWS_KIND(pe::Image(plus), LoaderError::Kind::unsupported);
  std::string arm = d;
  arm[0x84] = 0xC0;
  arm[0x85] = 0x01;
  CHECK_THROWS_KIND(pe::Image(arm), LoaderError::Kind::unsupported);
  // A truncated file
  CHECK_THROWS_KIND(pe::Image(d.substr(0, 0x100)), LoaderError::Kind::truncated);
  // Section past SizeOfImage
  std::string big = d;
  big[0x178 + 40 * 4 + 8 + 1] = 0x20; // .reloc vsize 0x2000 at 0x6000 > 0x7000
  CHECK_THROWS_KIND(pe::Image(big), LoaderError::Kind::malformed);
  // Export name index out of range
  std::string badexp = d;
  badexp[0x600 + 0x350] = 9;
  CHECK_THROWS_KIND(pe::Image(badexp), LoaderError::Kind::malformed);
  // Unknown relocation type is fine to parse but refuses to apply.
  std::string rel = d;
  rel[0x1800 + 8 + 1] = static_cast<char>(0xA0); // entry 0 → type 10 (DIR64)
  pe::Image img(rel);
  TestSink sink;
  sink.force_base = 0x20000000;
  CHECK_THROWS_KIND(pe::load(img, sink), LoaderError::Kind::unsupported);
  // A descriptor without an IAT ends the import list, as on Windows (the
  // KERNEL32 one here; its Name is still set). Walking on would bind slots
  // at RVA 0 + 4i, inside the headers.
  std::string noft = d;
  for (int i = 0; i < 4; i++) noft[0x600 + 0x14 + 16 + i] = 0;
  pe::Image cut(noft);
  CHECK_EQ(cut.imports().size(), 2u);
  CHECK_EQ(cut.imported_dlls().size(), 1u);
  // Resource directory that loops back to the root
  std::string loop = d;
  loop[0x1000 + 0x030 + 16 + 4] = 0x00; // "SPLASH" entry → root dir
  loop[0x1000 + 0x030 + 16 + 5] = 0x00;
  CHECK_THROWS_KIND(pe::Image(loop), LoaderError::Kind::malformed);
}

// String values whose node header misdescribes them, as some resource
// compilers write them: the text must still come out whole.
static void test_version_quirks() {
  auto wide = [](std::string_view s) {
    std::string w;
    for (char c : s) w += {c, '\0'};
    return w + std::string(2, '\0');
  };
  // wType 0 ("binary") but wValueLength counted in WCHARs (13 for 12 chars
  // + NUL): taken as bytes, that is only the first 6 characters.
  std::string halved = vnode(true, "FileDescription", wide("Screen saver"), false);
  halved[2] = 13;
  halved[3] = 0;
  // wType 1 with wValueLength counted in bytes (26).
  std::string bytes = vnode(true, "ProductName", "After Dark 4", true); // vnode widens text
  bytes[2] = 26;
  bytes[3] = 0;
  std::string vi = vnode(true, "VS_VERSION_INFO", "", false, {
      vnode(true, "StringFileInfo", "", false, {vnode(true, "040904E4", "", false, {halved, bytes})})});
  VersionInfo v = parse_version_info(vi, true);
  CHECK(!v.has_fixed);
  CHECK_STR(v.string("FileDescription"), "Screen saver");
  CHECK_STR(v.string("ProductName"), "After Dark 4");
}

int main() {
  test_version_quirks();
  test_parse();
  test_load();
  test_no_resolver();
  test_relocs_stripped();
  test_rejects();
  return finish("test_loader_pe");
}
