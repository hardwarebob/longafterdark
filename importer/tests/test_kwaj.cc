// KWAJ (kwaj.h; research/win/pkg/startrek/survey/survey_importer.md), on
// files from tests/kwaj_builder.h — nothing here compresses anything:
//   fixed vectors  eight files the research encoder made from data rebuilt
//                  here, and six written token by token (16-bit codes,
//                  symbols without codes, MATCHLEN2 symbol 0, distance 4096,
//                  a 70 KB file in two chunks, the disks' padding quirk,
//                  MATCHLEN2 after a run of 31 literals and MATCHLEN after
//                  32), pinned by their md5s: the research reference decoder,
//                  libmspack and Deark all give these bytes; and the zero
//                  padding that reads as a whole token (the strict rule)
//   the header     SZDD, another magic, a short file, methods 0/1/2/4, the
//                  optional header flags, another data offset
//   the tables     the sixth nibble, a type out of range, a length past 16 or
//                  below 0, incomplete, over-subscribed and empty codes, a
//                  stream cut inside them
//   the end rule   a token cut 8 or more bits before the end (an error), a
//                  cut on a token boundary (a clean prefix: only the manifest
//                  can tell), padding that reads as a partial token (a match;
//                  a literal run cut after its first literal: nothing of it)
//   max_size       exactly the size, one byte short
//   bit flips      every single-bit flip of the vectors of 1 KB or less: an
//                  error, other bytes, or (a flipped padding bit) the same
//                  bytes; never a crash, never past max_size
//   chunks         at most 64 KiB per sink call; the count returned
//
//   test_import_kwaj <scratch>
#include <functional>

#include "kwaj.h"
#include "kwaj_builder.h"
#include "md5.h"
#include "test_util.h"

using namespace adw::import;

namespace {

std::vector<uint8_t> expand(const std::vector<uint8_t>& f, uint64_t max_size = UINT64_MAX, size_t* max_chunk = nullptr,
                            size_t* chunks = nullptr) {
  std::vector<uint8_t> out;
  const uint64_t n = kwaj_expand(f, "T.XX_", max_size, [&](const uint8_t* p, size_t k) {
    out.insert(out.end(), p, p + k);
    if (max_chunk) *max_chunk = std::max(*max_chunk, k);
    if (chunks) ++*chunks;
  });
  CHECK_EQ(n, uint64_t(out.size()));
  return out;
}

bool fails(const char* what, const std::function<void()>& f, const std::string& want) {
  try {
    f();
    fprintf(stderr, "  %s: accepted, should have failed\n", what);
    return false;
  } catch (const KwajError& e) {
    const bool ok = std::string(e.what()).find(want) != std::string::npos;
    fprintf(stderr, "  %s -> %s\n", what, e.what());
    if (!ok) fprintf(stderr, "    expected the message to hold \"%s\"\n", want.c_str());
    return ok;
  }
}

std::string md5_of(const std::vector<uint8_t>& b) { return md5_hex(b.data(), b.size()); }

// expand() for a file that must expand: a refusal is a failure reported as
// one (with the reader's message), not an exception that ends the run.
std::vector<uint8_t> expand_ok(const char* what, const std::vector<uint8_t>& f, size_t* max_chunk = nullptr,
                               size_t* chunks = nullptr) {
  try {
    return expand(f, UINT64_MAX, max_chunk, chunks);
  } catch (const KwajError& e) {
    test::g_failures++;
    fprintf(stderr, "  %s: refused: %s\n", what, e.what());
    return {};
  }
}

// A method-3 stream of table headers only, bit by bit: the six type nibbles,
// then `lengths` (bits, value) pairs, padded with ones to a byte, then a few
// more bytes so that no read runs out before the error under test.
std::vector<uint8_t> tables(const std::vector<unsigned>& types6, const std::vector<std::pair<int, unsigned>>& fields) {
  test::KwajBitWriter b;
  for (unsigned t : types6) b.put(t, 4);
  for (auto [bits, v] : fields) b.put(v, bits);
  std::vector<uint8_t> s = b.finish(true);
  s.insert(s.end(), 64, 0x55);
  return test::kwaj_file(s);
}

}  // namespace

int main(int argc, char** argv) {
  // No default may reach the real data folder.
  test::sandbox_data_root(test::scratch(argc, argv, "adw-import-kwaj") / L"localappdata");

  // ---- the fixed vectors ----------------------------------------------------------------------
  {
    for (const test::KwajVector& v : test::kKwajVectors) {
      const auto file = test::unhex(v.packed);
      const auto plain = test::kwaj_vector_plain(v.name);
      CHECK_EQ(md5_of(plain), std::string(v.md5));  // the data is rebuilt as the research script built it
      const auto got = expand_ok(v.name, file);
      const bool ok = got == plain;
      fprintf(stderr, "  %-16s %5zu -> %5zu bytes: %s  (%s)\n", v.name, file.size(), got.size(), ok ? "ok" : "MISMATCH",
              v.what);
      CHECK(ok);
      CHECK(kwaj_header(file, v.name).method == 3);
      // The stream alone decodes the same.
      std::vector<uint8_t> raw;
      try {
        kwaj_detail::decode_lzh(std::span<const uint8_t>(file).subspan(14), v.name, UINT64_MAX,
                                [&](const uint8_t* p, size_t n) { raw.insert(raw.end(), p, p + n); });
      } catch (const KwajError& e) {
        fprintf(stderr, "  %s, the stream alone: refused: %s\n", v.name, e.what());
      }
      CHECK(raw == plain);
    }
    // Written token by token: the same files as the research script's (their
    // md5s), which the three decoders expanded to these bytes.
    struct Crafted {
      const char* name;
      test::KwajCraftedFile f;
      const char* file_md5;
      size_t size;
      const char* plain_md5;
    };
    const std::vector<Crafted> crafted = {
        {"k3_len16", test::kwaj_len16(), "72c97211e2cf45f45c48fd9a1f1da5df", 43, "c329cdb69065818acbb5d940ef72e22b"},
        {"k3_sparse", test::kwaj_sparse(), "720b19e334362263c0dedee47e1a0d74", 92, "97dee94ff05b8d03dcdc4b19b90ae6df"},
        {"k3_dist4096", test::kwaj_dist4096(), "53881409e5adfee85e53bd55668f431f", 4231,
         "b49bdfae8d6464ed40d9ab68b5b12bdd"},
        {"k3_chunk", test::kwaj_chunk(), "2ff9096a41f6820b5d52897c497edfee", 70004, "ae599418fcec22d6474686169257d7d0"},
        {"k3_pad_quirk", test::kwaj_pad_quirk(), "49430ec4f567f2a6a4cf5796aa157b96", 29,
         "fb197d1a2b849792ca6496ec1f27b260"},
        {"k3_run31", test::kwaj_run31(), "6de76904c170687c6b76c105171ebd05", 163, "736205bc55865c7b264ce9b779817032"},
    };
    for (const Crafted& c : crafted) {
      CHECK_EQ(md5_of(c.f.file), std::string(c.file_md5));
      CHECK_EQ(c.f.plain.size(), c.size);
      CHECK_EQ(md5_of(c.f.plain), std::string(c.plain_md5));
      size_t chunk = 0, chunks = 0;
      const auto got = expand_ok(c.name, c.f.file, &chunk, &chunks);
      const bool ok = got == c.f.plain;
      fprintf(stderr, "  %-16s %5zu -> %5zu bytes: %s\n", c.name, c.f.file.size(), got.size(), ok ? "ok" : "MISMATCH");
      CHECK(ok);
      CHECK(chunk <= 64 * 1024);
      if (std::string_view(c.name) == "k3_chunk") {
        CHECK_EQ(chunk, size_t(64 * 1024));  // 65,536 then 4,468
        CHECK_EQ(chunks, size_t(2));
      }
    }
    // The zero padding of k3_sparse's tokens reads as one more whole token (a
    // one-literal run "A"): every complete token is output, 93 bytes, as the
    // reference and Deark give (libmspack gives 92: it stops after a token
    // begun once its 16-bit look-ahead has run past the end).
    const test::KwajCraftedFile phantom = test::kwaj_phantom();
    CHECK_EQ(md5_of(phantom.file), std::string("9940d392f875aab954805b192237ac00"));
    const auto got = expand_ok("k3_phantom", phantom.file);
    CHECK_EQ(got.size(), size_t(93));
    CHECK_EQ(md5_of(got), std::string("1b2e055c6baaa96c9db02724897094db"));
    auto want = phantom.plain;
    want.push_back('A');
    CHECK(got == want);
  }

  // ---- kwaj_literals: what the package fixture writes ------------------------------------------
  for (size_t n : {size_t(0), size_t(1), size_t(31), size_t(32), size_t(33), size_t(1000), size_t(70000)}) {
    const auto data = test::pattern(n, uint32_t(n) + 3);
    CHECK(expand_ok("kwaj_literals", test::kwaj_literals(data)) == data);
  }

  // ---- the header -------------------------------------------------------------------------------
  {
    const auto good = test::kwaj_literals(test::pattern(40, 1));
    const std::vector<uint8_t> szdd = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33, 'A', 0, 1, 0, 0, 0, 0xFF, 'x'};
    CHECK(fails("SZDD", [&] { kwaj_header(szdd, "S.DL_"); }, "S.DL_: an SZDD file, not KWAJ"));
    CHECK(fails("junk", [] { kwaj_header(test::pattern(40, 2), "J.DL_"); }, "J.DL_: not a KWAJ file"));
    CHECK(fails("empty", [] { kwaj_header({}, "E.DL_"); }, "E.DL_: not a KWAJ file"));
    std::vector<uint8_t> cut(good.begin(), good.begin() + 12);
    CHECK(fails("cut short", [&] { kwaj_header(cut, "C.DL_"); }, "C.DL_: the KWAJ header is cut short"));
    std::vector<uint8_t> magic_only(good.begin(), good.begin() + 5);
    CHECK(fails("half a magic", [&] { kwaj_header(magic_only, "H.DL_"); }, "the KWAJ header is cut short"));
    const std::vector<uint8_t> payload(good.begin() + 14, good.end());
    for (uint16_t m : {0, 1, 2, 4, 5})
      CHECK(fails(("method " + std::to_string(m)).c_str(), [&] { kwaj_header(test::kwaj_file(payload, m), "M.DL_"); },
                  "M.DL_: KWAJ method " + std::to_string(m) + " is not supported"));
    CHECK(fails("flags 0x0001 (a length)", [&] { kwaj_header(test::kwaj_file(payload, 3, 14, 1), "F.DL_"); },
                "F.DL_: KWAJ header flags 0x0001 are not supported"));
    CHECK(fails("flags 0x0020 (text)", [&] { kwaj_header(test::kwaj_file(payload, 3, 14, 0x20), "F.DL_"); },
                "KWAJ header flags 0x0020 are not supported"));
    CHECK(fails("data at 18", [&] { kwaj_header(test::kwaj_file(payload, 3, 18), "O.DL_"); },
                "O.DL_: the KWAJ data starts at 18, not after the 14-byte header"));
    // kwaj_expand checks the header too.
    CHECK(fails("expand, method 2", [&] { expand(test::kwaj_file(payload, 2)); }, "KWAJ method 2 is not supported"));
    CHECK(expand(good) == test::pattern(40, 1));
  }

  // ---- the tables -------------------------------------------------------------------------------
  {
    CHECK(fails("the sixth nibble",
                [] {
                  expand(test::kwaj_file(test::kwaj_crafted({0, 0, 0, 0, 0}, std::vector<std::vector<unsigned>>(5),
                                                            {test::KwajToken::lit("X")}, true, 1)
                                             .stream));
                },
                "T.XX_: the sixth table type is 1, not 0"));
    CHECK(fails("type 4", [] { expand(tables({4, 0, 0, 0, 0, 0}, {})); },
                "T.XX_: MATCHLEN: code-length encoding 4 is not one of 0-3"));
    CHECK(fails("type 15 for OFFSET", [] { expand(tables({0, 0, 0, 15, 0, 0}, {})); },
                "T.XX_: OFFSET: code-length encoding 15 is not one of 0-3"));
    // Type 1: 15, then '10' (+1) twice.
    CHECK(fails("a length of 17", [] { expand(tables({1, 0, 0, 0, 0, 0}, {{4, 15}, {2, 0b10}, {2, 0b10}})); },
                "T.XX_: MATCHLEN: a code length of 17"));
    // Type 2: 0, then selector 0 (previous - 1).
    CHECK(fails("a length of -1", [] { expand(tables({0, 2, 0, 0, 0, 0}, {{4, 0}, {2, 0}})); },
                "T.XX_: MATCHLEN2: a code length of -1"));
    auto type3 = [](std::vector<unsigned> lens) {
      std::vector<std::pair<int, unsigned>> f;
      lens.resize(16, 0);
      for (unsigned l : lens) f.push_back({4, l});
      return tables({3, 0, 0, 0, 0, 0}, f);
    };
    CHECK(fails("an incomplete code (2,2)", [&] { expand(type3({2, 2})); }, "T.XX_: MATCHLEN is not a complete code"));
    CHECK(fails("an over-subscribed code (1,1,1)", [&] { expand(type3({1, 1, 1})); },
                "T.XX_: MATCHLEN is an over-subscribed code"));
    CHECK(fails("an empty code", [&] { expand(type3({})); }, "T.XX_: MATCHLEN is not a complete code"));
    // A one-symbol code is incomplete too (it would leave '1' without a symbol).
    CHECK(fails("a one-symbol code", [&] { expand(type3({1})); }, "MATCHLEN is not a complete code"));
    // The last table (LITERAL, type 3) incomplete: the name says which.
    {
      std::vector<std::vector<unsigned>> lens(5);
      lens[4].assign(256, 8);
      lens[4][255] = 0;
      CHECK(fails("LITERAL incomplete",
                  [&] { expand(test::kwaj_file(test::kwaj_crafted({0, 0, 0, 0, 3}, lens, {}, true).stream)); },
                  "T.XX_: LITERAL is not a complete code"));
    }
    // A stream cut inside the tables (k3_text_t2's are 20+ bytes).
    const auto t2 = test::unhex(test::kKwajVectors[2].packed);
    CHECK_EQ(std::string(test::kKwajVectors[2].name), std::string("k3_text_t2"));
    for (size_t n : {size_t(14), size_t(15), size_t(17), size_t(20), size_t(30)}) {
      std::vector<uint8_t> c(t2.begin(), t2.begin() + n);
      CHECK(fails(("tables cut at " + std::to_string(n)).c_str(), [&] { expand(c); },
                  "T.XX_: the compressed data ends inside the code tables"));
    }
  }

  // ---- the end rule -------------------------------------------------------------------------------
  {
    const auto t2 = test::unhex(test::kKwajVectors[2].packed);
    const auto plain = test::kwaj_vector_plain("k3_text_t2");
    // Cut on a token boundary: a clean prefix (KWAJ has no length or
    // checksum; the importer's manifest size check refuses it).
    std::vector<uint8_t> cut1(t2.begin(), t2.end() - 1);
    const auto prefix = expand(cut1);
    CHECK_EQ(prefix.size(), size_t(1994));
    CHECK(std::equal(prefix.begin(), prefix.end(), plain.begin()));
    // Cut inside a token that began 16, and exactly 8, bits before the end.
    std::vector<uint8_t> cut2(t2.begin(), t2.end() - 2), cut3(t2.begin(), t2.end() - 3);
    CHECK(fails("cut by 2", [&] { expand(cut2); },
                "T.XX_: the compressed data ends inside a token (16 bits before the end)"));
    CHECK(fails("cut by 3", [&] { expand(cut3); }, "ends inside a token (8 bits before the end)"));
    const auto one = test::unhex(test::kKwajVectors[0].packed);
    std::vector<uint8_t> one_cut(one.begin(), one.end() - 2);
    CHECK(fails("k3_one cut by 2", [&] { expand(one_cut); }, "ends inside a token (8 bits before the end)"));
    // The padding quirk: 7 one bits read as the start of a match, which
    // produces nothing (above: the file expands to exactly its tokens).
    const auto quirk = test::kwaj_pad_quirk();
    CHECK(expand_ok("k3_pad_quirk", quirk.file) == quirk.plain);
    std::vector<uint8_t> quirk_cut(quirk.file.begin(), quirk.file.end() - 1);
    CHECK(fails("the quirk cut by 1", [&] { expand(quirk_cut); }, "ends inside a token (14 bits before the end)"));
    // A literal run the end cuts short produces nothing either, not the
    // literals read before the end: runs "A", "A", "A", "AB" under 1-bit
    // codes (1,573 bits), then the last byte's 3 spare bits written as 010 —
    // MATCHLEN2 symbol 0 (a run), LITLEN symbol 1 (two literals), LITERAL
    // 'A', and the second literal runs out. The reference decoder's strict
    // policy gives "AAAAB" too; its Deark policy and Deark emit the 'A' read
    // ("AAAABA"), libmspack stops at its 16-bit look-ahead ("A"). File md5
    // 46a677c678defd2226cc2425f4d9967a (research/win/pkg/startrek/
    // fx-importer/kwaj_new_vectors.py).
    {
      std::vector<unsigned> two16(16, 0), two32(32, 0), two64(64, 0), ab(256, 0);
      two16[0] = two16[1] = 1;  // a literal run, or a match of 3
      two32[0] = two32[1] = 1;  // runs of one or two literals
      two64[0] = two64[1] = 1;
      ab[uint8_t('A')] = ab[uint8_t('B')] = 1;
      test::KwajCrafted c = test::kwaj_crafted(
          {3, 3, 3, 3, 3}, {two16, two16, two32, two64, ab},
          {test::KwajToken::lit("A"), test::KwajToken::lit("A"), test::KwajToken::lit("A"), test::KwajToken::lit("AB")},
          true);
      const std::vector<uint8_t> want = {'A', 'A', 'A', 'A', 'B'};
      CHECK(c.plain == want);
      CHECK(c.stream.size() == 197 && (c.stream.back() & 7) == 7);  // 3 bits of padding, written as ones
      c.stream.back() = uint8_t((c.stream.back() & ~7) | 0b010);
      const auto partial = test::kwaj_file(c.stream);
      CHECK_EQ(md5_of(partial), std::string("46a677c678defd2226cc2425f4d9967a"));
      CHECK(expand_ok("a partial literal run at the end", partial) == want);
    }
  }

  // ---- max_size ---------------------------------------------------------------------------------
  {
    const auto t2 = test::unhex(test::kKwajVectors[2].packed);
    CHECK_EQ(expand(t2, 2000).size(), size_t(2000));
    CHECK(fails("one byte short", [&] { expand(t2, 1999); }, "T.XX_: expands to more than 1999 bytes"));
    CHECK(fails("nothing allowed", [&] { expand(t2, 0); }, "expands to more than 0 bytes"));
    const auto chunk = test::kwaj_chunk();
    CHECK(fails("70 KB into 70,003", [&] { expand(chunk.file, 70003); }, "expands to more than 70003 bytes"));
    // Only the bound says KwajTooLarge (the importer then names its staging
    // budget); a damaged file is a plain KwajError.
    auto kind = [](const std::function<void()>& f) {
      try {
        f();
      } catch (const KwajTooLarge&) {
        return 2;
      } catch (const KwajError&) {
        return 1;
      }
      return 0;
    };
    CHECK_EQ(kind([&] { expand(t2, 1999); }), 2);
    std::vector<uint8_t> cut(t2.begin(), t2.end() - 2);
    CHECK_EQ(kind([&] { expand(cut, 2000); }), 1);
    CHECK_EQ(kind([&] { expand(t2, 2000); }), 0);
  }

  // ---- every single-bit flip of the small vectors ---------------------------------------------------
  {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> small;
    for (const test::KwajVector& v : test::kKwajVectors) {
      auto f = test::unhex(v.packed);
      if (f.size() <= 1024) small.push_back({v.name, std::move(f)});
    }
    small.push_back({"k3_len16", test::kwaj_len16().file});
    small.push_back({"k3_sparse", test::kwaj_sparse().file});
    small.push_back({"k3_phantom", test::kwaj_phantom().file});
    small.push_back({"k3_pad_quirk", test::kwaj_pad_quirk().file});
    size_t flips = 0, errors = 0, changed = 0, same = 0;
    for (const auto& [name, file] : small) {
      const auto plain = expand_ok(name.c_str(), file);
      const uint64_t max = plain.size();
      for (size_t bit = 0; bit < file.size() * 8; bit++) {
        auto f = file;
        f[bit / 8] ^= uint8_t(0x80 >> (bit % 8));
        flips++;
        try {
          size_t chunk = 0;
          const auto got = expand(f, max, &chunk);
          CHECK(got.size() <= max && chunk <= 64 * 1024);
          (got == plain ? same : changed)++;
        } catch (const KwajError&) {
          errors++;
        } catch (...) {
          test::g_failures++;
          fprintf(stderr, "  %s, bit %zu: not a KwajError\n", name.c_str(), bit);
        }
      }
    }
    fprintf(stderr, "  %zu single-bit flips: %zu refused, %zu other bytes, %zu the same (padding)\n", flips, errors,
            changed, same);
    CHECK(flips > 30000 && errors > flips / 4 && changed > flips / 4);
  }
  return test::finish("import.kwaj");
}
