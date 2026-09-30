// ARJ archives (arj.h; research/win/pkg/swse/survey/importer_design.md §2, §6.3),
// on synthetic archives from tests/arj_builder.h (which, like this program,
// holds no ARJ encoder: arj.cc is a modified version of UNARJ's DECODE.C,
// for programs that are not ARJ archivers):
//   stored     round trips: one member, many, an empty one, one over 64 KiB
//              (chunked), names found without case, code page 437 names, the
//              DOS time, 7-bit text members copied as stored
//   refused    every damaged or foreign header shape, each a clean ArjError
//              with its message: CRCs, sizes, flags, versions, methods, file
//              types, hostile names, truncation, the end marker, extended
//              headers, duplicates (case aside, code page 437's letters
//              too), too many members
//   volumes    a member split over three volumes joined byte for byte; every
//              joining rule broken; a flipped byte in segment 2 naming the
//              second volume
//   vectors    the thirteen fixed method-1/method-4 vectors (each decoded to
//              the same bytes by the Python reference and 7-Zip) through
//              arj_detail and through an archive, methods 1-3 alike; every
//              vector cut by 1-3 bytes, one size larger, and every single-bit
//              flip (sampled in the two 100,000-byte ones); one size smaller
//              for methods 1-3 (a block still holds symbols); and what any
//              smaller size does — nothing after the last symbol it needs is
//              read (a method-1 stream ending with a block, method 4 at the
//              size), so those bytes are the prefix the CRC-32 then checks
//   crafted    streams built bit by bit that break each decoder bound (never
//              an out-of-range access, always ArjError); blocks that end on
//              the 32 KiB chunk and 64 KiB edges; codes longer than every
//              lookup table (read_c_len's, decode_c's and decode_p's tree
//              walks) and every flip inside the longest NT code
//   joined     an LZH member split over two volumes, mixed methods, members
//              over 64 KiB through an archive, both methods
//   names      arj_next_volume, arj_continues
//
//   test_import_arj <scratch>
#include <algorithm>
#include <functional>
#include <memory>

#include "arj.h"
#include "arj_builder.h"
#include "md5.h"
#include "test_util.h"

using namespace adw::import;

namespace {

using Vol = std::pair<std::string, std::vector<uint8_t>>;

std::vector<ArjVolume> volumes_of(const std::vector<Vol>& vols) {
  std::vector<ArjVolume> v;
  for (const auto& [n, d] : vols) v.push_back({n, std::make_shared<const std::vector<uint8_t>>(d)});
  return v;
}

std::unique_ptr<ArjArchive> open(const std::vector<Vol>& vols) {
  return std::make_unique<ArjArchive>(volumes_of(vols));
}

std::unique_ptr<ArjArchive> open1(const std::vector<uint8_t>& vol, const std::string& name = "TEST.ARJ") {
  return open({{name, vol}});
}

// The whole member, and the largest chunk the sink was given.
std::vector<uint8_t> extract_all(const ArjArchive& a, const ArjMember& m, size_t* max_chunk = nullptr) {
  std::vector<uint8_t> v;
  a.extract(m, [&](const uint8_t* p, size_t n) {
    v.insert(v.end(), p, p + n);
    if (max_chunk) *max_chunk = std::max(*max_chunk, n);
  });
  return v;
}

// `f` must throw ArjError whose message holds `want`.
bool fails(const char* what, const std::function<void()>& f, const std::string& want) {
  try {
    f();
    fprintf(stderr, "  %s: accepted, should have failed\n", what);
    return false;
  } catch (const ArjError& e) {
    const bool ok = std::string(e.what()).find(want) != std::string::npos;
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    if (!ok) fprintf(stderr, "    expected the message to hold \"%s\"\n", want.c_str());
    return ok;
  } catch (const std::exception& e) {
    fprintf(stderr, "  %s: not an ArjError: %s\n", what, e.what());
    return false;
  }
}

bool refused(const char* what, const std::vector<Vol>& vols, const std::string& want) {
  return fails(what, [&] { open(vols); }, want);
}

bool refused1(const char* what, const std::vector<uint8_t>& vol, const std::string& want) {
  return refused(what, {{"TEST.ARJ", vol}}, want);
}

std::string md5_of(const std::vector<uint8_t>& v) { return md5_hex(v.data(), v.size()); }

// Decodes `packed` with the method's decoder, all of it.
std::vector<uint8_t> decode(int method, const std::vector<uint8_t>& packed, uint32_t osize, size_t* max_chunk = nullptr) {
  std::vector<uint8_t> out;
  auto sink = [&](const uint8_t* p, size_t n) {
    out.insert(out.end(), p, p + n);
    if (max_chunk) *max_chunk = std::max(*max_chunk, n);
  };
  if (method == 4) arj_detail::decode_fastest(packed, osize, sink);
  else arj_detail::decode_lzh(packed, osize, sink);
  return out;
}

// A one-member archive of a packed segment, its header describing `plain`.
std::vector<uint8_t> packed_archive(int method, const std::vector<uint8_t>& packed, uint32_t osize, uint32_t crc,
                                    const std::string& name = "V.BIN") {
  test::ArjEntry e;
  e.h.name = name;
  e.h.method = uint8_t(method);
  e.h.csize = uint32_t(packed.size());
  e.h.osize = osize;
  e.h.crc = crc;
  e.data = packed;
  return test::arj_volume("V.ARJ", false, {e});
}

// ---- stored members ------------------------------------------------------------------------

void test_stored() {
  const auto big = test::pattern(200000, 3), small = test::pattern(1000, 4);
  auto a = open1(test::arj_stored({{"ONE.DLL", small}, {"BIG.IMX", big}, {"EMPTY.TXT", {}}, {"last.txt", {'x'}}}));
  CHECK_EQ(a->members().size(), size_t(4));
  CHECK(a->find("big.imx") && a->find("BIG.IMX") && !a->find("NOPE"));
  CHECK(a->find("LAST.TXT") && a->find("LAST.TXT")->name == "last.txt");  // as stored
  size_t chunk = 0;
  CHECK(extract_all(*a, *a->find("BIG.IMX"), &chunk) == big);
  CHECK(chunk > 0 && chunk <= 64 * 1024);  // 200 KB in 64 KiB chunks
  CHECK(extract_all(*a, *a->find("ONE.DLL")) == small);
  CHECK(extract_all(*a, *a->find("EMPTY.TXT")).empty());
  const ArjMember& m = *a->find("ONE.DLL");
  CHECK_EQ(m.size, uint64_t(1000));
  CHECK_EQ(m.dos_datetime, test::kArjDosTime);
  CHECK_EQ(m.volumes, std::string("TEST.ARJ"));
  CHECK_EQ(m.segments.size(), size_t(1));
  CHECK_EQ(int(m.access_mode), 0x20);
  CHECK_EQ(int(m.host_os), 0);

  // A 7-bit text member (file type 1) is copied as stored; the CRC covers
  // the stored bytes. Extended headers are skipped, their CRCs checked.
  auto t = test::arj_stored_entry("TEXT.TXT", {'a', '\r', '\n'});
  t.h.file_type = 1;
  t.h.ext_headers = {"one", std::string(40, 'e')};
  test::ArjHeader main = test::arj_main_header("TEST.ARJ", false);
  main.ext_headers = {"main's own"};
  auto b = open1(test::arj_volume("TEST.ARJ", false, {t}, true, {}, &main));
  CHECK(extract_all(*b, b->members().at(0)) == std::vector<uint8_t>({'a', '\r', '\n'}));
  CHECK_EQ(int(b->members().at(0).file_type), 1);
  // Up to 16 extended headers per header.
  t.h.ext_headers.assign(16, "x");
  CHECK(open1(test::arj_volume("TEST.ARJ", false, {t}))->members().size() == 1);

  // Names in code page 437, as DOS wrote them: 0x81 is u-umlaut.
  auto oem = test::arj_stored_entry("M\x81SIK.MID", {1, 2, 3});
  auto c = open1(test::arj_volume("TEST.ARJ", false, {oem}));
  CHECK_EQ(c->members().at(0).name, std::string("M\xC3\xBCSIK.MID"));
  CHECK(c->find("M\xC3\xBCSIK.MID") != nullptr);

  // Bytes after the end marker are never looked at.
  auto d = open1(test::arj_volume("TEST.ARJ", false, {test::arj_stored_entry("A.TXT", {9})}, true,
                                  test::pattern(100, 9)));
  CHECK(extract_all(*d, d->members().at(0)) == std::vector<uint8_t>({9}));
  // An archive with no members at all is an empty archive.
  CHECK(open1(test::arj_volume("TEST.ARJ", false, {}))->members().empty());
}

// ---- refused shapes ------------------------------------------------------------------------------

void test_refused() {
  auto one = [](test::ArjEntry e) { return test::arj_volume("TEST.ARJ", false, {e}); };
  auto entry = [] { return test::arj_stored_entry("GOOD.DLL", test::pattern(100, 1)); };
  const auto good = one(entry());
  CHECK(open1(good)->members().size() == 1);

  CHECK(refused1("empty file", {}, "not an ARJ archive"));
  CHECK(refused1("random bytes", test::pattern(500, 2), "not an ARJ archive"));
  CHECK(refused1("a ZIP", {'P', 'K', 3, 4, 0, 0, 0, 0}, "not an ARJ archive"));
  // The main header.
  {
    test::ArjHeader m = test::arj_main_header("TEST.ARJ", false);
    m.bad_crc = true;
    CHECK(refused1("main header CRC", test::arj_volume("TEST.ARJ", false, {entry()}, true, {}, &m), "CRC-32"));
    m = test::arj_main_header("TEST.ARJ", false);
    m.file_type = 1;
    CHECK(refused1("main header of file type 1", test::arj_volume("TEST.ARJ", false, {entry()}, true, {}, &m),
                   "not a main header"));
    m = test::arj_main_header("TEST.ARJ", false);
    m.flags = 0x11;
    CHECK(refused1("garbled main header", test::arj_volume("TEST.ARJ", false, {entry()}, true, {}, &m),
                   "password-protected (garbled)"));
    m.flags = 0x18;  // EXTFILE belongs to members
    CHECK(refused1("main header flag 0x08", test::arj_volume("TEST.ARJ", false, {entry()}, true, {}, &m),
                   "unsupported ARJ flags 0x18"));
    m = test::arj_main_header("TEST.ARJ", false);
    m.min_version = 4;
    CHECK(refused1("needs ARJ version 4", test::arj_volume("TEST.ARJ", false, {entry()}, true, {}, &m), "version 4"));
    m = test::arj_main_header("TEST.ARJ", false);
    m.basic_size = 0;  // an end marker where the main header should be
    CHECK(refused1("no main header", test::arj_volume("TEST.ARJ", false, {}, true, {}, &m), "not an ARJ archive"));
  }
  // Local headers: the id, size and CRC.
  {
    auto e = entry();
    e.h.bad_crc = true;
    CHECK(refused1("local header CRC", one(e), "header CRC-32 mismatch"));
    e = entry();
    e.h.basic_size = 2601;
    CHECK(refused1("basic_hdr_size 2601", one(e), "damaged header"));
    e.h.basic_size = 29;
    CHECK(refused1("basic_hdr_size 29", one(e), "damaged header"));
    e = entry();
    e.h.first_size = 29;
    CHECK(refused1("first_hdr_size 29", one(e), "damaged header"));
    e.h.first_size = 250;  // past the basic header
    e.h.pad_to_first = false;
    CHECK(refused1("first_hdr_size past the basic header", one(e), "damaged header"));
    e = entry();
    e.h.unterminated_name = true;
    CHECK(refused1("unterminated name", one(e), "unterminated"));
    e = entry();
    e.h.ext_headers = {"abc"};
    e.h.bad_ext_crc = true;
    CHECK(refused1("extended header CRC", one(e), "extended header CRC-32 mismatch"));
    e.h.bad_ext_crc = false;
    e.h.ext_headers.assign(17, "x");
    CHECK(refused1("17 extended headers", one(e), "too many extended headers"));
    auto v = good;
    v[v.size() - 4] = 0x61;  // the end marker's id
    CHECK(refused1("damaged end marker", v, "no header"));
  }
  // Flags, versions, types, methods.
  {
    auto e = entry();
    e.h.flags = 0x11;
    CHECK(refused1("garbled member", one(e), "password-protected (garbled)"));
    e.h.flags = 0x50;
    CHECK(refused1("unknown flag 0x40", one(e), "unsupported ARJ flags 0x50"));
    e.h.flags = 0x12;
    CHECK(refused1("unknown flag 0x02", one(e), "unsupported ARJ flags 0x12"));
    e = entry();
    e.h.flags = 0x18;  // EXTFILE, and first_hdr_size 30: no room for the position
    CHECK(refused1("EXTFILE without the position", one(e), "no file position"));
    e = entry();
    e.h.ext_pos = 100;  // first_hdr_size 34, no EXTFILE
    CHECK(refused1("a position without EXTFILE", one(e), "without the continuation flag"));
    e.h.ext_pos = 0;  // present but 0: fine
    CHECK(open1(one(e))->members().size() == 1);
    e = entry();
    e.h.min_version = 4;
    CHECK(refused1("member needs ARJ version 4", one(e), "version 4"));
    for (int type : {2, 3, 4, 7}) {
      e = entry();
      e.h.file_type = uint8_t(type);
      CHECK(refused1(("file type " + std::to_string(type)).c_str(), one(e), "not a file"));
    }
    e = entry();
    e.h.method = 5;
    CHECK(refused1("method 5", one(e), "compression method 5 is not supported"));
    e = entry();
    e.h.osize += 1;  // stored, csize != osize
    CHECK(refused1("stored with two sizes", one(e), "sizes differ"));
    e = entry();
    e.h.fspos = 50;  // past the name
    CHECK(refused1("file name position past the name", one(e), "file name position"));
  }
  // Names that may not become a path component.
  for (const char* bad : {"A/B.DLL", "C:X.DLL", "A\\B.DLL", "..", ".", "CON", "NUL.TXT", "com1.dll", "TRAIL.",
                          "SPACE ", "", "X\x01Y", "Q?.DLL"}) {
    auto e = test::arj_stored_entry(bad, {1});
    CHECK(refused1(("name \"" + std::string(bad) + "\"").c_str(), one(e), "TEST.ARJ"));
  }
  CHECK(refused1("a path", one(test::arj_stored_entry("DIR/FILE.TXT", {1})), "is not a bare file name"));
  CHECK(refused1("a drive", one(test::arj_stored_entry("C:FILE.TXT", {1})), "is not a bare file name"));
  CHECK(refused1("a device", one(test::arj_stored_entry("AUX.DLL", {1})), "unusable file name"));
  // Truncation and the end marker.
  {
    auto v = good;
    v.resize(v.size() - 4);
    CHECK(refused1("no end marker", v, "no end-of-archive header"));
    v = good;
    v.resize(v.size() - 50);  // inside the member's data
    CHECK(refused1("cut in the data", v, "truncated"));
    v = good;
    v.resize(60);  // inside the local header
    CHECK(refused1("cut in a header", v, "truncated"));
    v = good;
    v.resize(3);
    CHECK(refused1("cut in the main header's id", v, "not an ARJ archive"));
  }
  // Duplicates, case aside — as Windows compares names, so code page 437's
  // letters fold too (both files would be planned, and the second could not
  // be created): u-umlaut/U-umlaut, e-acute/E-acute, sigma/Sigma.
  CHECK(refused1("a duplicate name",
                 test::arj_volume("TEST.ARJ", false,
                                  {test::arj_stored_entry("SAME.DLL", {1}), test::arj_stored_entry("same.dll", {2})}),
                 "two members are named"));
  for (auto [lower, upper] : std::vector<std::pair<const char*, const char*>>{
           {"\x81.DLL", "\x9A.DLL"}, {"CAF\x82.TXT", "CAF\x90.TXT"}, {"\xE5.DAT", "\xE4.DAT"}}) {
    auto pair = test::arj_volume("TEST.ARJ", false,
                                 {test::arj_stored_entry(lower, {1}), test::arj_stored_entry(upper, {2})});
    CHECK(refused1(("code page 437 case: " + oem437_to_utf8(lower) + ", " + oem437_to_utf8(upper)).c_str(), pair,
                   "TEST.ARJ: two members are named " + oem437_to_utf8(upper)));
  }
  // Two other letters are two names (u-umlaut, o-umlaut).
  CHECK_EQ(open1(test::arj_volume("TEST.ARJ", false,
                                  {test::arj_stored_entry("\x81.DLL", {1}), test::arj_stored_entry("\x94.DLL", {2})}))
               ->members()
               .size(),
           size_t(2));
  // More members than any archive holds (65,535, as a ZIP directory).
  {
    std::vector<test::ArjEntry> many;
    for (int i = 0; i < 65536; i++) many.push_back(test::arj_stored_entry("F" + std::to_string(i), {}));
    CHECK(refused1("65,536 members", test::arj_volume("TEST.ARJ", false, many), "more than 65535 members"));
    many.pop_back();
    CHECK_EQ(open1(test::arj_volume("TEST.ARJ", false, many))->members().size(), size_t(65535));
  }
  CHECK(fails("no volumes", [] { ArjArchive a({}); }, "no ARJ volumes"));
}

// ---- volumes ---------------------------------------------------------------------------------------

std::vector<Vol> split_set(const std::vector<uint8_t>& big) {
  // SPLIT.ARJ: FIRST.TXT, BIG.TXT (part 1); .A01: BIG.TXT (part 2); .A02:
  // BIG.TXT (part 3), LAST.TXT — the shape of the research SPLIT set.
  auto part = [&](size_t a, size_t b) { return std::vector<uint8_t>(big.begin() + a, big.begin() + b); };
  return {
      {"SPLIT.ARJ", test::arj_volume("SPLIT.ARJ", true,
                                     {test::arj_stored_entry("FIRST.TXT", {'f', 'i', 'r', 's', 't'}),
                                      test::arj_stored_entry("BIG.TXT", part(0, 1000), 0x14)})},
      {"SPLIT.A01", test::arj_volume("SPLIT.A01", true, {test::arj_stored_entry("BIG.TXT", part(1000, 2200), 0x1C, 1000)})},
      {"SPLIT.A02", test::arj_volume("SPLIT.A02", false,
                                     {test::arj_stored_entry("BIG.TXT", part(2200, big.size()), 0x18, 2200),
                                      test::arj_stored_entry("LAST.TXT", {'l', 'a', 's', 't'})})},
  };
}

void test_volumes() {
  const auto big = test::arj_vector_text(3000, 9);
  const auto set = split_set(big);
  {
    auto a = open(set);
    CHECK_EQ(a->members().size(), size_t(3));
    CHECK_EQ(a->members()[0].name, std::string("FIRST.TXT"));
    CHECK_EQ(a->members()[2].name, std::string("LAST.TXT"));
    const ArjMember& m = *a->find("BIG.TXT");
    CHECK_EQ(m.size, uint64_t(3000));
    CHECK_EQ(m.segments.size(), size_t(3));
    CHECK_EQ(m.volumes, std::string("SPLIT.ARJ+SPLIT.A01+SPLIT.A02"));
    CHECK_EQ(m.segments[1].position, uint64_t(1000));
    CHECK_EQ(m.segments[2].volume, size_t(2));
    CHECK(extract_all(*a, m) == big);
    CHECK_EQ(a->find("LAST.TXT")->volumes, std::string("SPLIT.A02"));
    // The same with arj_split (one member cut over three volumes).
    auto b = open(test::arj_split("JAWAS.IMX", big, {541, 2000}, {"S.ARJ", "S.A01", "S.A02"}));
    CHECK(extract_all(*b, *b->find("JAWAS.IMX")) == big);
    CHECK_EQ(b->find("JAWAS.IMX")->volumes, std::string("S.ARJ+S.A01+S.A02"));
  }
  auto with = [&](size_t k, std::vector<uint8_t> vol) {
    auto s = set;
    s[k].second = std::move(vol);
    return s;
  };
  auto part = [&](size_t a, size_t b) { return std::vector<uint8_t>(big.begin() + a, big.begin() + b); };
  // SPLIT.A01 with its one segment changed.
  auto a01 = [&](const std::string& name, uint8_t flags, std::optional<uint32_t> pos, bool continues = true) {
    return with(1, test::arj_volume("SPLIT.A01", continues, {test::arj_stored_entry(name, part(1000, 2200), flags, pos)}));
  };
  CHECK(refused("another name in the continuation", a01("OTHER.TXT", 0x1C, 1000),
                "SPLIT.ARJ!BIG.TXT continues, but SPLIT.A01 does not continue it"));
  CHECK(refused("a wrong position", a01("BIG.TXT", 0x1C, 999), "wrong position"));
  CHECK(refused("the continuation is missing", a01("BIG.TXT", 0x14, std::nullopt), "does not continue it"));
  CHECK(refused("the next volume has no members", with(1, test::arj_volume("SPLIT.A01", true, {})),
                "does not continue it"));
  CHECK(refused("EXTFILE on a first member",
                {{"X.ARJ", test::arj_volume("X.ARJ", false, {test::arj_stored_entry("A.TXT", {1}, 0x18, 0)})}},
                "a continuation without its start"));
  CHECK(refused("a continuation that is not first in its volume",
                with(2, test::arj_volume("SPLIT.A02", false,
                                         {test::arj_stored_entry("BIG.TXT", part(2200, 3000), 0x18, 2200),
                                          test::arj_stored_entry("BIG2.TXT", {1}, 0x18, 0)})),
                "a continuation without its start"));
  CHECK(refused("VOLUME on a member that is not last",
                with(0, test::arj_volume("SPLIT.ARJ", true,
                                         {test::arj_stored_entry("BIG.TXT", part(0, 1000), 0x14),
                                          test::arj_stored_entry("FIRST.TXT", {1})})),
                "is not the last member of SPLIT.ARJ"));
  CHECK(refused("a middle volume without the main VOLUME flag", a01("BIG.TXT", 0x1C, 1000, false),
                "SPLIT.A01 does not continue on another volume"));
  CHECK(refused("the last volume still continuing", {set[0], set[1]}, "the archive continues on SPLIT.A02"));
  CHECK(refused("the last volume's main flag", {{"ONE.ARJ", test::arj_volume("ONE.ARJ", true, {})}},
                "ONE.ARJ: the archive continues on ONE.A01"));
  CHECK(refused("volumes out of order", {set[1], set[0], set[2]}, "a continuation without its start"));
  CHECK(refused("a duplicate across volumes",
                with(2, test::arj_volume("SPLIT.A02", false,
                                         {test::arj_stored_entry("BIG.TXT", part(2200, 3000), 0x18, 2200),
                                          test::arj_stored_entry("FIRST.TXT", {1})})),
                "two members are named FIRST.TXT"));
  // A flipped byte in segment 2: the CRC-32 error names the second volume.
  {
    auto s = set;
    s[1].second[s[1].second.size() - 10] ^= 0x40;  // inside BIG.TXT's second segment
    auto a = open(s);
    CHECK(fails("a flipped byte in segment 2", [&] { extract_all(*a, *a->find("BIG.TXT")); },
                "SPLIT.A01!BIG.TXT: CRC-32 mismatch"));
  }
  // arj_continues, volume by volume.
  CHECK(arj_continues(set[0].second, "SPLIT.ARJ") && arj_continues(set[1].second, "SPLIT.A01"));
  CHECK(!arj_continues(set[2].second, "SPLIT.A02"));
  CHECK(fails("arj_continues on garbage", [] { arj_continues(test::pattern(100, 5), "JUNK.ARJ"); },
              "JUNK.ARJ: not an ARJ archive"));
}

// ---- the fixed vectors ---------------------------------------------------------------------------

void test_vectors() {
  for (const test::ArjVector& v : test::kArjVectors) {
    const auto packed = test::unhex(v.packed);
    const auto plain = test::arj_vector_plain(v.name);
    // The rebuilt plain bytes are the research script's (its md5).
    CHECK_EQ(md5_of(plain), std::string(v.md5));
    CHECK_EQ(plain.size(), size_t(v.size));
    size_t chunk = 0;
    auto got = decode(v.method, packed, v.size, &chunk);
    CHECK_EQ(got.size(), size_t(v.size));
    CHECK_EQ(md5_of(got), std::string(v.md5));
    CHECK_EQ(test::arj_crc(got), v.crc);
    CHECK(chunk <= 32 * 1024);
    // Through an archive, with its CRC-32 checked; methods 2 and 3 are the
    // same bitstream as 1.
    for (int method : v.method == 4 ? std::vector<int>{4} : std::vector<int>{1, 2, 3}) {
      auto a = open1(packed_archive(method, packed, v.size, v.crc));
      CHECK(extract_all(*a, a->members().at(0)) == plain);
    }
    fprintf(stderr, "  %-16s method %d  %5zu -> %5u bytes  ok\n", v.name, v.method, packed.size(), v.size);

    // Cut by 1-3 bytes: the stream ends early (each vector holds exactly the
    // bytes its last symbol needs, so the look-ahead then wants a third).
    for (size_t cut = 1; cut <= 3 && cut <= packed.size(); cut++) {
      std::vector<uint8_t> c(packed.begin(), packed.end() - cut);
      auto a = open1(packed_archive(v.method, c, v.size, v.crc));
      std::string what = std::string(v.name) + " cut by " + std::to_string(cut);
      CHECK(fails(what.c_str(), [&] { extract_all(*a, a->members().at(0)); }, "TEST.ARJ!V.BIN: "));
    }
    // One size off. One larger always ends early. One smaller is refused for
    // methods 1-3: the last symbol, a match, would pass it, or the block
    // still holds symbols when it is reached (every vector's last block ends
    // where its stream does). Method 4 has no blocks: it stops at the
    // recorded size and never looks further, so a smaller size is refused
    // only when the last symbol is a match that would pass it, which is not
    // a rule it checks (both method-4 vectors happen to end on a match); what
    // it does instead is below.
    if (v.method != 4) {
      CHECK(fails((std::string(v.name) + " osize - 1").c_str(), [&] { decode(v.method, packed, v.size - 1); }, ""));
    }
    CHECK(fails((std::string(v.name) + " osize + 1").c_str(), [&] { decode(v.method, packed, v.size + 1); }, ""));
  }
  // The messages for the two kinds of wrong size.
  const auto ss = test::unhex(test::arj_vector("m1_single_symbol").packed);
  CHECK(fails("40 literals for osize 39", [&] { decode(1, ss, 39); }, "goes on past the recorded size"));
  const auto ov = test::unhex(test::arj_vector("m1_overlap").packed);
  CHECK(fails("a 98-byte match for osize 50", [&] { decode(1, ov, 50); }, "more data than the recorded size"));
  CHECK(fails("osize + 1 ends early", [&] { decode(1, ss, 41); }, "ends early"));

  // Every smaller size, as UNARJ reads a stream: nothing after the last
  // symbol the recorded size needs is read, so what comes out is always a
  // prefix of the plain bytes, which an archive's CRC-32 then checks.
  // Method 1: a size that ends inside a block is refused; one that ends with
  // a block is that block's bytes — m1_two_blocks' first block makes 811,
  // and its second block is never read (nor is anything after it).
  {
    const auto& v = test::arj_vector("m1_two_blocks");
    const auto packed = test::unhex(v.packed), plain = test::arj_vector_plain(v.name);
    std::vector<uint32_t> accepted;
    for (uint32_t n = 0; n < v.size; n++) {
      try {
        auto got = decode(1, packed, n);
        accepted.push_back(n);
        CHECK(got == std::vector<uint8_t>(plain.begin(), plain.begin() + n));
      } catch (const ArjError&) {
      }
    }
    CHECK((accepted == std::vector<uint32_t>{0, 811}));
    std::vector<uint8_t> first(plain.begin(), plain.begin() + 811), tail = packed;
    tail.insert(tail.end(), {0xDE, 0xAD, 0xBE, 0xEF});  // not read either
    auto a = open1(packed_archive(1, tail, 811, test::arj_crc(first)));
    CHECK(extract_all(*a, a->members().at(0)) == first);
    a = open1(packed_archive(1, packed, 811, test::arj_crc(plain)));  // the whole file's CRC: not these bytes
    CHECK(fails("m1_two_blocks at 811 bytes, the CRC of 1500", [&] { extract_all(*a, a->members().at(0)); },
                "TEST.ARJ!V.BIN: CRC-32 mismatch"));
  }
  // Method 4: every smaller size gives exactly that many plain bytes, or is
  // refused because the match it ends in would pass it.
  {
    const auto& v = test::arj_vector("m4_text");
    const auto packed = test::unhex(v.packed), plain = test::arj_vector_plain(v.name);
    size_t prefixes = 0, in_match = 0;
    for (uint32_t n = 0; n < v.size; n++) {
      try {
        auto got = decode(4, packed, n);
        prefixes++;
        CHECK(got == std::vector<uint8_t>(plain.begin(), plain.begin() + n));
      } catch (const ArjError& e) {
        in_match++;
        CHECK(std::string(e.what()) == "more data than the recorded size");
      }
    }
    fprintf(stderr, "  m4_text, %u smaller sizes: %zu give their prefix, %zu end inside a match\n", v.size, prefixes,
            in_match);
    CHECK(prefixes > 0 && in_match > 0);
  }
}

// Every single-bit flip of every vector: a clean ArjError (a decoder bound,
// or the CRC-32), or — for a flip in the last byte's padding — the original
// bytes; never more than osize bytes emitted, never a crash. In the two
// 100,000-byte vectors (whose every flip decodes up to 100 KB) one bit of
// every fifth byte.
void test_flips() {
  size_t flips = 0, errors = 0, same = 0;
  for (const test::ArjVector& v : test::kArjVectors) {
    const auto packed = test::unhex(v.packed);
    const auto plain = test::arj_vector_plain(v.name);
    const bool sampled = v.size > 64 * 1024;
    for (size_t byte = 0; byte < packed.size(); byte += sampled ? 5 : 1) {
      for (int bit = 0; bit < 8; bit++) {
        if (sampled && size_t(bit) != byte % 8) continue;
        auto p = packed;
        p[byte] ^= uint8_t(1u << bit);
        auto a = open1(packed_archive(v.method, p, v.size, v.crc));
        size_t emitted = 0;
        std::vector<uint8_t> out;
        flips++;
        try {
          a->extract(a->members().at(0), [&](const uint8_t* d, size_t n) {
            emitted += n;
            out.insert(out.end(), d, d + n);
          });
          same++;
          if (out != plain) {
            test::g_failures++;
            fprintf(stderr, "  %s byte %zu bit %d: other bytes, and no error\n", v.name, byte, bit);
          }
        } catch (const ArjError&) {
          errors++;
        } catch (const std::exception& e) {
          test::g_failures++;
          fprintf(stderr, "  %s byte %zu bit %d: %s\n", v.name, byte, bit, e.what());
        }
        if (emitted > v.size) {
          test::g_failures++;
          fprintf(stderr, "  %s byte %zu bit %d: %zu bytes emitted, more than %u\n", v.name, byte, bit, emitted, v.size);
        }
      }
    }
  }
  fprintf(stderr, "  %zu single-bit flips: %zu ArjError, %zu the original bytes (padding bits)\n", flips, errors, same);
  CHECK(errors > flips * 9 / 10);
}

// ---- crafted streams -----------------------------------------------------------------------------

std::vector<uint8_t> bits(const std::function<void(test::ArjBitWriter&)>& f) {
  test::ArjBitWriter w;
  f(w);
  w.put(16, 0);  // enough look-ahead that the error is the one under test
  w.put(16, 0);
  return w.flush();
}

void test_crafted() {
  auto lzh = [](const char* what, const std::vector<uint8_t>& s, uint32_t osize, const std::string& want) {
    return fails(what, [&] { decode(1, s, osize); }, want);
  };
  // A pt table that is one constant symbol (n == 0).
  auto pt_constant = [](test::ArjBitWriter& w, uint32_t sym) {
    w.put(5, 0);
    w.put(5, sym);
  };
  CHECK(lzh("blocksize 0", bits([](auto& w) { w.put(16, 0); }), 10, "an empty block"));
  CHECK(lzh("pt n 31 > NT", bits([](auto& w) { w.put(16, 1), w.put(5, 31); }), 10, "too many code lengths"));
  CHECK(lzh("pt constant symbol 19", bits([&](auto& w) { w.put(16, 1), pt_constant(w, 19); }), 10,
            "constant symbol out of range"));
  CHECK(lzh("a code length of 17", bits([](auto& w) {
              w.put(16, 1);
              w.put(5, 1);
              w.put(3, 7);        // 7, then the unary extension:
              w.put(10, 0x3FF);   // ten more ones make 17
              w.put(1, 0);
            }),
            10, "a code length over 16"));
  CHECK(lzh("a code length of 16 is fine, up to the table check", bits([](auto& w) {
              w.put(16, 1);
              w.put(5, 1);
              w.put(3, 7);
              w.put(9, 0x1FF);  // 16
              w.put(1, 0);
            }),
            10, "not a complete code"));
  CHECK(lzh("c n 511 > NC", bits([&](auto& w) { w.put(16, 1), pt_constant(w, 3), w.put(9, 511); }), 10,
            "too many code lengths"));
  CHECK(lzh("a zero run past NC", bits([&](auto& w) {
              w.put(16, 1);
              pt_constant(w, 2);  // every item is a long zero run: getbits(9) + 20
              w.put(9, 510);
              w.put(9, 511);      // 531 zeros
            }),
            10, "a run of zero lengths passes the table"));
  CHECK(lzh("a 4-bit zero run past NC", bits([&](auto& w) {
              w.put(16, 1);
              pt_constant(w, 1);  // every item is a short zero run: getbits(4) + 3
              w.put(9, 510);
              for (int i = 0; i < 29; i++) w.put(4, 15);  // 29 runs of 18 zeros: 522
            }),
            10, "a run of zero lengths passes the table"));
  CHECK(lzh("c: three codes of length 1 (over-subscribed)", bits([&](auto& w) {
              w.put(16, 1);
              pt_constant(w, 3);  // every item is length 1 ...
              w.put(9, 3);        // ... for three symbols
            }),
            10, "not a complete code"));
  CHECK(lzh("an incomplete code (Kraft sum 2^16 - 2^10)", bits([](auto& w) {
              w.put(16, 1);
              w.put(5, 6);
              w.put(3, 1), w.put(3, 2), w.put(3, 3);
              w.put(2, 0);  // no zero run after the third length
              w.put(3, 4), w.put(3, 5), w.put(3, 6);
            }),
            10, "not a complete code"));
  CHECK(lzh("an over-subscribed code (Kraft sum 2^17)", bits([](auto& w) {
              w.put(16, 1);
              w.put(5, 4);
              w.put(3, 1), w.put(3, 1), w.put(3, 1);
              w.put(2, 0);
              w.put(3, 1);
            }),
            10, "not a complete code"));
  CHECK(lzh("a constant c symbol 510", bits([&](auto& w) {
              w.put(16, 1);
              pt_constant(w, 0);
              w.put(9, 0);
              w.put(9, 510);
            }),
            10, "constant symbol out of range"));
  CHECK(lzh("a constant p symbol 17", bits([&](auto& w) {
              w.put(16, 1);
              pt_constant(w, 0);
              w.put(9, 0);
              w.put(9, 'A');
              pt_constant(w, 17);
            }),
            10, "constant symbol out of range"));
  // A valid tiny block by hand: 'A' and a 3-byte match at distance 2 (the
  // constant p symbol 1), two symbols of length 1 each.
  auto two_symbols = [&](uint32_t p_symbol) {
    return bits([&](test::ArjBitWriter& w) {
      w.put(16, 2);
      // pt: symbols 2 (long zero run) and 3 (length 1), one bit each.
      w.put(5, 4);
      w.put(3, 0), w.put(3, 0), w.put(3, 1);
      w.put(2, 0);
      w.put(3, 1);
      // c_len: 65 zeros, 'A' = 1, 190 zeros, symbol 256 = 1.
      w.put(9, 257);
      w.put(1, 0), w.put(9, 65 - 20);
      w.put(1, 1);
      w.put(1, 0), w.put(9, 190 - 20);
      w.put(1, 1);
      pt_constant(w, p_symbol);
      w.put(1, 0);  // 'A'
      w.put(1, 1);  // a 3-byte match
    });
  };
  CHECK(lzh("distance 2 after 1 byte", two_symbols(1), 4, "a match reaches before the start"));
  {
    // Distance 1 is fine: "AAAA".
    auto out = decode(1, two_symbols(0), 4);
    CHECK(out == std::vector<uint8_t>({'A', 'A', 'A', 'A'}));
  }
  // Distances past the window: 'X', 104 matches of 256 bytes at distance 1,
  // 'Y', then 3 bytes from `distance` back (m1_window's bytes when that is
  // 26,624). c: 'X', 'Y', symbols 256 (a 3-byte match) and 509 (256 bytes),
  // two bits each: 00, 01, 10, 11. NT: symbols 2 (zero runs) and 4 (the
  // length 2), one bit each. p: symbols 0 (distance 1) and 15 (14 more bits).
  auto window = [&](uint32_t distance) {
    return bits([&](test::ArjBitWriter& w) {
      w.put(16, 1 + 104 + 1 + 1);
      w.put(5, 5);
      w.put(3, 0), w.put(3, 0), w.put(3, 1);
      w.put(2, 1);  // after the third length, one zero (symbol 3)
      w.put(3, 1);
      w.put(9, 510);
      w.put(1, 0), w.put(9, 88 - 20);  // 88 zeros, then 'X' and 'Y'
      w.put(1, 1), w.put(1, 1);
      w.put(1, 0), w.put(9, 166 - 20);  // 90..255
      w.put(1, 1);                      // 256
      w.put(1, 0), w.put(9, 252 - 20);  // 257..508
      w.put(1, 1);                      // 509
      w.put(5, 16);                     // p: 16 lengths, 1 for symbols 0 and 15
      w.put(3, 1);
      for (int i = 1; i < 15; i++) w.put(3, 0);
      w.put(3, 1);
      w.put(2, 0);                                             // 'X'
      for (int i = 0; i < 104; i++) w.put(2, 3), w.put(1, 0);  // 256 bytes from 1 back
      w.put(2, 1);                                             // 'Y'
      w.put(2, 2), w.put(1, 1);                                // 3 bytes, symbol 15:
      w.put(14, distance - 1 - (1u << 14));                    // distance - 1 = 2^14 + these
    });
  };
  CHECK(decode(1, window(26624), 26629) == test::arj_vector_plain("m1_window"));
  CHECK(lzh("distance 26,625", window(26625), 26629, "a match reaches beyond the 26624-byte window"));
  // Before the start: "ab", then 3 bytes from 3 back. c: symbol 256 one bit
  // (0), 'a' and 'b' two (10, 11); NT: 2 one bit, 3 and 4 two; p: the
  // constant symbol 2 (distance 3 or 4, one more bit).
  CHECK(lzh("distance 3 after 2 bytes", bits([&](test::ArjBitWriter& w) {
              w.put(16, 3);
              w.put(5, 5);
              w.put(3, 0), w.put(3, 0), w.put(3, 1);
              w.put(2, 0);
              w.put(3, 2), w.put(3, 2);
              w.put(9, 257);
              w.put(1, 0), w.put(9, 97 - 20);  // 97 zeros, then 'a' and 'b' (length 2: symbol 4, 11)
              w.put(2, 3), w.put(2, 3);
              w.put(1, 0), w.put(9, 157 - 20);  // 99..255
              w.put(2, 2);                      // 256: length 1 (symbol 3, 10)
              pt_constant(w, 2);
              w.put(2, 2), w.put(2, 3);  // 'a', 'b'
              w.put(1, 0), w.put(1, 0);  // a 3-byte match, distance 2 + 0 + 1
            }),
            5, "a match reaches before the start"));
  // Method 4 by hand: a match is a unary length prefix (1, 0: width 1) and
  // its bits (c = 0 + 1: 3 bytes), then a pointer (0: width 9) and 9 bits
  // (distance 0 + 1); a literal is 0 and its 8 bits.
  CHECK(fails("method 4: a pointer before the start", [&] {
          decode(4, bits([](test::ArjBitWriter& w) { w.put(3, 4), w.put(10, 0); }), 3);
        }, "a match reaches before the start"));
  CHECK(fails("method 4: a match past osize", [&] {
          decode(4, bits([](test::ArjBitWriter& w) {
                   w.put(1, 0), w.put(8, 'a');
                   w.put(4, 0xE), w.put(3, 1);  // width 3: c = 1 + 7, 10 bytes
                   w.put(10, 0);
                 }),
                 5);
        }, "more data than the recorded size"));
  CHECK(fails("method 4: ends early", [&] { decode(4, {}, 5); }, "ends early"));
  // An empty stream is 0 bytes (the look-ahead's two zero bytes).
  CHECK(decode(1, {}, 0).empty());
  CHECK(decode(4, {}, 0).empty());

  // Sizes on the output's edges (32 KiB chunks, the 64 KiB ring), blocks of
  // at most 65,535 symbols: 'Y' and 'Z', one bit each (NT: symbols 2 and 3,
  // one bit each), in a pseudo-random order — exactly those bytes, in
  // 32 KiB chunks and a last shorter one.
  for (uint32_t n : {32767u, 32768u, 32769u, 65535u, 65536u, 65537u, 131072u}) {
    const auto order = test::pattern(n, 20);
    std::vector<uint8_t> want;
    test::ArjBitWriter w;
    for (uint32_t done = 0; done < n;) {
      const uint32_t block = std::min<uint32_t>(n - done, 65535);
      w.put(16, block);
      w.put(5, 4);
      w.put(3, 0), w.put(3, 0), w.put(3, 1);
      w.put(2, 0);
      w.put(3, 1);
      w.put(9, 91);
      w.put(1, 0), w.put(9, 89 - 20);  // 89 zeros, then 'Y' and 'Z'
      w.put(1, 1), w.put(1, 1);
      pt_constant(w, 0);
      for (uint32_t i = done; i < done + block; i++) {
        w.put(1, order[i] & 1);
        want.push_back(order[i] & 1 ? 'Z' : 'Y');
      }
      done += block;
    }
    std::vector<size_t> chunks;
    std::vector<uint8_t> got;
    arj_detail::decode_lzh(w.flush(), n, [&](const uint8_t* p, size_t k) {
      chunks.push_back(k);
      got.insert(got.end(), p, p + k);
    });
    std::vector<size_t> want_chunks(n / 32768, 32768);
    if (n % 32768) want_chunks.push_back(n % 32768);
    CHECK(got == want);
    CHECK(chunks == want_chunks);
  }

  // Codes longer than every lookup table (m1_long_codes, explicit tables:
  // NT codes of 9..16 bits, c codes of 13..16, a 16-bit p code) walk
  // read_c_len's, decode_c's and decode_p's trees; test_vectors decodes it
  // and test_flips flips every bit. A flip inside its first 16-bit NT code
  // (the c length 1, as all ones but the last) is always a clean error of
  // the decoder itself, CRC-32 aside: the lengths it then reads are not a
  // complete code, or a zero run passes the table.
  {
    const auto& v = test::arj_vector("m1_long_codes");
    const auto packed = test::unhex(v.packed);
    for (size_t b = test::kArjLongCodesNtBit; b < test::kArjLongCodesNtBit + test::kArjLongCodesNtBits; b++) {
      auto p = packed;
      p[b / 8] ^= uint8_t(0x80 >> (b % 8));
      CHECK(fails(("m1_long_codes, bit " + std::to_string(b) + " of the 16-bit NT code").c_str(),
                  [&] { decode(1, p, v.size); }, "bad Huffman table ("));
    }
  }
}

// ---- members joined from segments --------------------------------------------------------------

void test_joined() {
  // An LZH member split over two volumes, each segment its own stream (as
  // the disc's JAWAS.IMX, STORYBRD.IMX and SWSFX.DLL are): the m1_text vector
  // then the m1_two_blocks vector.
  const auto& v1 = test::arj_vector("m1_text");
  const auto& v2 = test::arj_vector("m1_two_blocks");
  auto p1 = test::arj_vector_plain("m1_text"), p2 = test::arj_vector_plain("m1_two_blocks");
  std::vector<Vol> set = {
      {"SPLITM1.ARJ",
       test::arj_volume("SPLITM1.ARJ", true, {test::arj_packed_entry("LZH.TXT", 1, test::unhex(v1.packed), p1, 0x14)})},
      {"SPLITM1.A01", test::arj_volume("SPLITM1.A01", false,
                                       {test::arj_packed_entry("LZH.TXT", 1, test::unhex(v2.packed), p2, 0x18,
                                                               uint32_t(p1.size()))})},
  };
  auto a = open(set);
  auto want = p1;
  want.insert(want.end(), p2.begin(), p2.end());
  CHECK(extract_all(*a, *a->find("LZH.TXT")) == want);
  CHECK_EQ(a->find("LZH.TXT")->volumes, std::string("SPLITM1.ARJ+SPLITM1.A01"));
  // Mixed methods in one member: a method-4 first segment (m4_text, which is
  // as long as m1_text, so the second segment still resumes at byte 700).
  const auto& v4 = test::arj_vector("m4_text");
  auto p4 = test::arj_vector_plain("m4_text");
  set[0].second =
      test::arj_volume("SPLITM1.ARJ", true, {test::arj_packed_entry("LZH.TXT", 4, test::unhex(v4.packed), p4, 0x14)});
  auto b = open(set);
  want = p4;
  want.insert(want.end(), p2.begin(), p2.end());
  CHECK(extract_all(*b, *b->find("LZH.TXT")) == want);
  // Members over 64 KiB through an archive, both methods: the ring wraps,
  // the output comes in chunks of at most 32 KiB.
  for (const char* name : {"m1_big", "m4_big"}) {
    const auto& v = test::arj_vector(name);
    auto big = test::arj_vector_plain(name);
    auto c = open1(test::arj_volume("BIG.ARJ", false,
                                    {test::arj_packed_entry("BIG.TXT", v.method, test::unhex(v.packed), big)}));
    size_t chunk = 0;
    CHECK(extract_all(*c, c->members().at(0), &chunk) == big);
    CHECK(chunk <= 32 * 1024);
  }
}

// ---- volume names ------------------------------------------------------------------------------------

void test_names() {
  CHECK(arj_next_volume("SWSE2.ARJ") == std::optional<std::string>("SWSE2.A01"));
  CHECK(arj_next_volume("swse2.arj") == std::optional<std::string>("swse2.a01"));
  CHECK(arj_next_volume("X.A01") == std::optional<std::string>("X.A02"));
  CHECK(arj_next_volume("X.A09") == std::optional<std::string>("X.A10"));
  CHECK(arj_next_volume("X.A98") == std::optional<std::string>("X.A99"));
  CHECK(!arj_next_volume("X.A99"));
  CHECK(!arj_next_volume("X.A00"));
  CHECK(!arj_next_volume("X.ZIP"));
  CHECK(!arj_next_volume("X.AR"));
  CHECK(!arj_next_volume("ARJ"));
  CHECK(!arj_next_volume("X.A1X"));
}

}  // namespace

int main(int argc, char** argv) {
  // No default may reach the real data folder.
  test::sandbox_data_root(test::scratch(argc, argv, "adw-import-arj") / L"localappdata");
  test_stored();
  test_refused();
  test_volumes();
  test_vectors();
  test_flips();
  test_crafted();
  test_joined();
  test_names();
  return test::finish("import.arj");
}
