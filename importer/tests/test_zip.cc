// PKZIP + ZipCrypto + raw inflate (zip.h) and the archive password's
// derivation from an InstallShield script (PACKAGES.md §8.3, §8.4), on
// archives from tests/zip_builder.h: round trips, the header check byte,
// wrong passwords, damaged and foreign archives, hostile names, and the
// candidate order of the password search.
#include <memory>

#include "test_util.h"
#include "zip.h"
#include "zip_builder.h"

using namespace adw::import;

namespace {

// The whole member (the library streams only; fixtures are small).
std::vector<uint8_t> extract_all(const ZipArchive& z, const ZipMember& m, std::string_view pw) {
  std::vector<uint8_t> v;
  z.extract(m, pw, [&](const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); });
  return v;
}

std::shared_ptr<const std::vector<uint8_t>> own(std::vector<uint8_t> v) {
  return std::make_shared<const std::vector<uint8_t>>(std::move(v));
}

bool refused(const std::vector<uint8_t>& archive, const char* what) {
  try {
    ZipArchive z(own(archive), "T.ZIP");
    fprintf(stderr, "  %s: accepted, should have been refused\n", what);
    return false;
  } catch (const ZipError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    return true;
  }
}

bool extract_fails(const ZipArchive& z, const ZipMember& m, const std::string& pw, const char* what) {
  try {
    extract_all(z, m, pw);
    fprintf(stderr, "  %s: extracted, should have failed\n", what);
    return false;
  } catch (const ZipError& e) {
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    return true;
  }
}

std::vector<uint8_t> compressible(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = uint8_t("After Dark "[i % 11] + (i / 4096) % 3);
  return v;
}

// An InstallShield-like script: length-prefixed strings among opcodes.
std::vector<uint8_t> script(const std::vector<std::string>& strings_before, const std::string& marker_follow,
                            const std::vector<std::string>& strings_after) {
  std::vector<uint8_t> s = {0xFF, 0xFF, 0x0C, 0x00, 0x01, 0x02};
  auto lp = [&](const std::string& t) {
    s.push_back(uint8_t(t.size()));
    s.push_back(uint8_t(t.size() >> 8));
    s.insert(s.end(), t.begin(), t.end());
    s.push_back(0x13);  // an "opcode" byte, printable-adjacent on purpose
  };
  for (auto& t : strings_before) lp(t);
  lp("Cannot initialize for unzip!");
  s.insert(s.end(), {0x03, 0x00, 0xFF, 0xFF});
  lp(marker_follow);
  for (auto& t : strings_after) lp(t);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  // No default may reach the real data folder.
  test::sandbox_data_root(test::scratch(argc, argv, "adw-import-zip") / L"localappdata");

  // ---- round trips -----------------------------------------------------------------
  {
    test::ZipBuilder b;
    b.add("DEFLATE.AD", compressible(300000));  // several 64 KiB chunks each way
    b.add("RANDOM.DLL", test::pattern(70000, 5));
    b.add("STORED.AM3", test::pattern(16, 6), /*deflate=*/false);
    b.add("PLAIN.TXT", compressible(1000), true, /*encrypt=*/false);
    b.add("EMPTY.TXT", {}, false);
    b.add("EMPTYZ.TXT", {}, true);
    b.add("I&SSHOW.MID", test::pattern(3000, 7));
    ZipArchive z(own(b.build()), "ROUND.ZIP");
    CHECK_EQ(z.members().size(), size_t(7));
    for (const test::ZipSpec& s : b.members) {
      const ZipMember* m = z.find(s.name);
      CHECK(m != nullptr);
      if (!m) continue;
      CHECK_EQ(m->usize, uint32_t(s.data.size()));
      CHECK_EQ(m->encrypted(), s.encrypt);
      CHECK(extract_all(z, *m, test::kTestZipPassword) == s.data);
      if (s.encrypt) CHECK(z.header_check(*m, test::kTestZipPassword));
    }
    // Case-insensitive lookup; members keep their stored name.
    CHECK(z.find("deflate.ad") && z.find("deflate.ad")->name == "DEFLATE.AD");
    CHECK(!z.find("NOPE.AD"));
    // Streaming: chunks of at most 64 KiB, in order.
    size_t chunks = 0, biggest = 0;
    std::vector<uint8_t> joined;
    z.extract(*z.find("DEFLATE.AD"), test::kTestZipPassword, [&](const uint8_t* p, size_t n) {
      chunks++;
      biggest = std::max(biggest, n);
      joined.insert(joined.end(), p, p + n);
    });
    CHECK(chunks >= 5 && biggest <= 65536 && joined == compressible(300000));
    // An unencrypted member needs no password at all.
    CHECK(extract_all(z, *z.find("PLAIN.TXT"), "") == compressible(1000));
  }

  // ---- wrong password, header check byte ---------------------------------------------------
  {
    test::ZipBuilder b;
    b.add("A.AD", test::pattern(5000, 1));
    b.add("B.AM3", test::pattern(16, 2), false);
    ZipArchive z(own(b.build()), "PW.ZIP");
    const ZipMember& a = *z.find("A.AD");
    CHECK(z.password_opens(a, test::kTestZipPassword));
    CHECK(!z.password_opens(a, "not-the-password"));
    CHECK(extract_fails(z, a, "not-the-password", "wrong password"));
    // 1 in 256 wrong passwords pass the check byte; the CRC-32 catches them.
    int header_passes = 0, opened = 0;
    for (int i = 0; i < 2000; i++) {
      std::string pw = "wrong" + std::to_string(i);
      header_passes += z.header_check(a, pw);
      opened += z.password_opens(a, pw);
    }
    fprintf(stderr, "  2000 wrong passwords: %d pass the check byte, %d open the member\n", header_passes, opened);
    CHECK(header_passes > 0 && header_passes < 30);
    CHECK_EQ(opened, 0);
    // A stored member decrypts to the wrong bytes under a check-byte false
    // positive; the CRC-32 refuses it too.
    const ZipMember& s = *z.find("B.AM3");
    for (int i = 0; i < 5000; i++) {
      std::string pw = "stored" + std::to_string(i);
      if (z.header_check(s, pw)) {
        CHECK(!z.password_opens(s, pw));
        CHECK(extract_fails(z, s, pw, "check-byte false positive on a stored member"));
        break;
      }
    }
  }
  {
    // Flag bit 3 (data descriptor): the check byte is the time's high byte.
    test::ZipBuilder b;
    b.add("T.AD", test::pattern(700, 3));
    b.members[0].extra_flags = 0x08;
    ZipArchive z(own(b.build()), "BIT3.ZIP");
    CHECK(z.header_check(z.members()[0], test::kTestZipPassword));
    CHECK(extract_all(z, z.members()[0], test::kTestZipPassword) == test::pattern(700, 3));
  }

  // ---- damaged archives: corrupt, never a verify failure --------------------------------------
  {
    test::ZipBuilder b;
    b.add("C.AD", test::pattern(9000, 4));
    b.add("D.AD", compressible(20000));
    auto good = b.build();
    {
      auto bad = good;  // central CRC changed
      bad[b.central_offsets[0] + 16] ^= 0x5A;
      ZipArchive z(own(bad), "CRC.ZIP");
      // The check byte comes from the CRC too, so a changed CRC is refused
      // either as a wrong password or as a CRC mismatch; both are errors.
      CHECK(extract_fails(z, *z.find("C.AD"), test::kTestZipPassword, "bad CRC"));
    }
    {
      auto bad = good;  // one byte of compressed data flipped
      bad[30 + 4 + 12 + 100] ^= 0xFF;
      ZipArchive z(own(bad), "DATA.ZIP");
      CHECK(extract_fails(z, *z.find("C.AD"), test::kTestZipPassword, "damaged data"));
    }
    {
      auto bad = good;  // recorded size larger than the data
      bad[b.central_offsets[1] + 24] ^= 0x01;
      ZipArchive z(own(bad), "SIZE.ZIP");
      CHECK(extract_fails(z, *z.find("D.AD"), test::kTestZipPassword, "size mismatch"));
    }
    CHECK(refused(std::vector<uint8_t>(good.begin(), good.begin() + good.size() / 2), "truncated archive"));
    CHECK(refused(std::vector<uint8_t>(good.begin(), good.end() - 1), "end record cut"));
    CHECK(refused(test::pattern(5000, 9), "not a ZIP"));
    CHECK(refused({}, "empty file"));
    {
      // Central directory intact, but the member's data runs past the end.
      auto bad = good;
      bad[b.central_offsets[0] + 20] = 0xFF, bad[b.central_offsets[0] + 21] = 0xFF, bad[b.central_offsets[0] + 22] = 0x0F;
      ZipArchive z(own(bad), "TRUNC.ZIP");
      CHECK(extract_fails(z, *z.find("C.AD"), test::kTestZipPassword, "member past the end"));
    }
  }

  // ---- foreign shapes: refused ---------------------------------------------------------------
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.zip64_marker = true;
    CHECK(refused(b.build(), "ZIP64 end record"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    auto img = b.build();
    img[b.central_offsets[0] + 20] = img[b.central_offsets[0] + 21] = img[b.central_offsets[0] + 22] =
        img[b.central_offsets[0] + 23] = 0xFF;
    CHECK(refused(img, "ZIP64 member sizes"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.disk_number = 1;
    CHECK(refused(b.build(), "multi-disk"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.members[0].extra_flags = 0x40;
    CHECK(refused(b.build(), "strong encryption"));
  }
  {
    test::ZipBuilder b;
    b.add("A.AD", {1, 2, 3});
    b.members[0].method_override = 12;  // bzip2
    CHECK(refused(b.build(), "unsupported method"));
  }
  for (const char* hostile : {"../ESCAPE.AD", "SUB/X.AD", "SUB\\X.AD", "C:EVIL.AD", "CON", "NUL.AD", "COM1.DLL",
                              "TRAIL.", "SPACE ", ""}) {
    test::ZipBuilder b;
    b.add(hostile, {1, 2, 3});
    CHECK(refused(b.build(), hostile));
  }
  {
    test::ZipBuilder b;
    b.add("TWICE.AD", {1});
    b.add("twice.ad", {2});
    CHECK(refused(b.build(), "duplicate names"));
  }

  // ---- the password's candidates (§8.4) --------------------------------------------------------
  {
    auto s = script({"First decoy", "Another one"}, "follow-me", {"later string", "tail"});
    auto c = password_candidates(s);
    CHECK(!c.empty() && c[0] == "follow-me");
    // Then everything else in file order, once each.
    auto pos = [&](const std::string& t) { return std::find(c.begin(), c.end(), t) - c.begin(); };
    CHECK(pos("First decoy") < pos("Another one") && pos("Another one") < pos("later string"));
    CHECK(std::count(c.begin(), c.end(), "follow-me") == 1);
    // Too short and too long strings are not candidates.
    auto t = password_candidates(script({"abc", std::string(33, 'x')}, "okay", {}));
    CHECK(std::find(t.begin(), t.end(), "abc") == t.end());
    CHECK(std::find(t.begin(), t.end(), std::string(33, 'x')) == t.end());
    // A plain run of printable bytes counts too (no length prefix).
    std::vector<uint8_t> raw = {0, 0, 'p', 'l', 'a', 'i', 'n', 'r', 'u', 'n', 0, 1};
    auto u = password_candidates(raw);
    CHECK(std::find(u.begin(), u.end(), "plainrun") != u.end());
  }

  // ---- derive_password -------------------------------------------------------------------------
  {
    const std::string pw = "derive-me-42";
    auto a = test::zip_of({{"PLACE.TXT", test::pattern(30, 1)}, {"A.AD", test::pattern(4000, 2)}}, pw);
    auto bz = test::zip_of({{"MULTI.AM3", test::pattern(16, 3)}}, pw);
    ZipArchive za(own(a), "A.ZIP"), zb(own(bz), "B.ZIP");
    std::vector<const ZipArchive*> zips = {&za, &zb};
    // The preferred position: right after the marker, among decoys.
    auto got = derive_password(script({"decoy-one", "decoy-two"}, pw, {"decoy-three"}), zips);
    CHECK(got && *got == pw);
    // Elsewhere in the script, after a decoy that follows the marker.
    got = derive_password(script({"decoy-one"}, "not-it", {"x-decoy", pw, "tail-decoy"}), zips);
    CHECK(got && *got == pw);
    // A decoy that passes the check byte of the smallest member is still refused.
    std::string trap;
    const ZipMember* smallest = zb.find("MULTI.AM3");
    for (int i = 0; i < 100000 && trap.empty(); i++) {
      std::string c = "trap" + std::to_string(i);
      if (zb.header_check(*smallest, c)) trap = c;
    }
    CHECK(!trap.empty());
    got = derive_password(script({}, trap, {pw}), zips);
    CHECK(got && *got == pw);
    // No candidate works.
    CHECK(!derive_password(script({"a-decoy"}, "nothing-here", {}), zips));
    CHECK(!derive_password({}, zips));
    // Two archives under different passwords: nothing opens both.
    auto c2 = test::zip_of({{"OTHER.AD", test::pattern(500, 9)}}, "second-password");
    ZipArchive zc(own(c2), "C.ZIP");
    std::vector<const ZipArchive*> mixed = {&za, &zc};
    CHECK(!derive_password(script({}, pw, {"second-password"}), mixed));
    // Nothing encrypted: no password needed.
    test::ZipBuilder plain;
    plain.add("A.AD", {1, 2}, true, false);
    ZipArchive zp(own(plain.build()), "P.ZIP");
    got = derive_password(script({}, "whatever", {}), {&zp});
    CHECK(got && got->empty());
  }
  return test::finish("import.zip");
}
