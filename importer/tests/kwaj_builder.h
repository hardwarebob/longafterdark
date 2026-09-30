// Synthetic KWAJ files for the importer tests (kwaj.h;
// research/win/pkg/startrek/survey/survey_importer.md):
//   * an MSB-first bit writer that pads the last byte with ones (as
//     Microsoft's encoder does) or zeros, and kwaj_file() for the header;
//   * a token writer: explicit code lengths and table types, canonical codes,
//     and tokens — literal runs, matches — written as the format's rule
//     codes them (MATCHLEN2 right after a literal run shorter than 32). It is
//     a port of the research script's crafted() (research/win/pkg/startrek/
//     importer/tools/kwaj_vectors.py) and does no match search: it compresses
//     nothing, it writes the tokens it is given;
//   * kwaj_literals(): any data as runs of 32 literals under fixed tables
//     (the package fixture's files);
//   * eight fixed vectors: made-up data (test_util.h pattern(),
//     arj_builder.h arj_vector_text()) encoded by the research encoder, and
//     six crafted streams rebuilt here, pinned by their files' md5s — every
//     one decoded to the same bytes by the research reference decoder (in its
//     three end policies), libmspack's msexpand and Deark, so they pin the
//     C++ reader against three implementations.
// No bytes of any release.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arj_builder.h"  // unhex, arj_vector_text
#include "test_util.h"    // pattern

namespace test {

// ---- bits ------------------------------------------------------------------------------------

class KwajBitWriter {
 public:
  void put(uint32_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
      acc_ = uint8_t(acc_ << 1 | ((value >> i) & 1));
      if (++n_ == 8) {
        out_.push_back(acc_);
        acc_ = 0;
        n_ = 0;
      }
    }
    bits_ += size_t(nbits);
  }
  // The last byte padded with ones (the encoder's habit) or zeros.
  std::vector<uint8_t> finish(bool pad_ones) {
    if (n_) put(pad_ones ? 0xFF : 0, 8 - n_);
    return out_;
  }
  size_t bits() const { return bits_; }

 private:
  std::vector<uint8_t> out_;
  uint8_t acc_ = 0;
  int n_ = 0;
  size_t bits_ = 0;
};

// The 14-byte header, then the payload: method, data offset, flags as given
// (so a test can write the ones the reader refuses).
inline std::vector<uint8_t> kwaj_file(const std::vector<uint8_t>& payload, uint16_t method = 3, uint16_t offset = 14,
                                      uint16_t flags = 0) {
  std::vector<uint8_t> f = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1};
  for (uint16_t v : {method, offset, flags}) {
    f.push_back(uint8_t(v));
    f.push_back(uint8_t(v >> 8));
  }
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}

// ---- the token writer ---------------------------------------------------------------------------

inline constexpr unsigned kKwajTableSize[5] = {16, 16, 32, 64, 256};  // MATCHLEN, MATCHLEN2, LITLEN, OFFSET, LITERAL

inline unsigned kwaj_fixed_length(unsigned n) { return n == 16 ? 4 : n == 32 ? 5 : n == 64 ? 6 : 8; }

// Canonical codes (shorter first, then by symbol) for these lengths; 0 = no code.
inline std::vector<uint32_t> kwaj_canonical(const std::vector<unsigned>& lens) {
  unsigned maxlen = 0;
  for (unsigned l : lens) maxlen = std::max(maxlen, l);
  std::vector<uint32_t> count(maxlen + 1, 0), next(maxlen + 2, 0), codes(lens.size(), 0);
  for (unsigned l : lens)
    if (l) count[l]++;
  uint32_t code = 0;
  for (unsigned l = 1; l <= maxlen; l++) {
    code = l > 1 ? (code + count[l - 1]) << 1 : 0;
    next[l] = code;
  }
  for (size_t s = 0; s < lens.size(); s++)
    if (lens[s]) codes[s] = next[lens[s]]++;
  return codes;
}

// A table's lengths in its encoding (0 writes nothing: the lengths are fixed).
inline void kwaj_write_lengths(KwajBitWriter& b, unsigned type, const std::vector<unsigned>& lens) {
  if (type == 0) return;
  if (type == 3) {
    for (unsigned l : lens) b.put(l, 4);
    return;
  }
  b.put(lens[0], 4);
  unsigned c = lens[0];
  for (size_t i = 1; i < lens.size(); i++) {
    const unsigned x = lens[i];
    if (type == 1) {
      if (x == c) b.put(0, 1);
      else if (x == c + 1) b.put(0b10, 2);
      else b.put(0b11, 2), b.put(x, 4);
    } else if (int(x) - int(c) >= -1 && int(x) - int(c) <= 1) {
      b.put(unsigned(int(x) - int(c) + 1), 2);
    } else {
      b.put(3, 2), b.put(x, 4);
    }
    c = x;
  }
}

// A token: a literal run of 1..32 bytes, or a match of 3..17 bytes at a
// distance of 1..4096.
struct KwajToken {
  std::vector<uint8_t> literals;
  unsigned length = 0, distance = 0;
  static KwajToken lit(std::vector<uint8_t> d) { return {std::move(d), 0, 0}; }
  static KwajToken lit(std::string_view s) { return {std::vector<uint8_t>(s.begin(), s.end()), 0, 0}; }
  static KwajToken match(unsigned length, unsigned distance) { return {{}, length, distance}; }
};

// The method-3 stream for these table types, lengths (a type-0 table gets
// its fixed lengths whatever is given) and tokens, and the plain bytes it
// decodes to. `sixth` is the sixth table-type nibble (0 in every real file).
struct KwajCrafted {
  std::vector<uint8_t> stream, plain;
};
inline KwajCrafted kwaj_crafted(const std::vector<unsigned>& types, std::vector<std::vector<unsigned>> lens,
                                const std::vector<KwajToken>& tokens, bool pad_ones, unsigned sixth = 0) {
  for (size_t i = 0; i < 5; i++)
    if (types[i] == 0) lens[i].assign(kKwajTableSize[i], kwaj_fixed_length(kKwajTableSize[i]));
  std::vector<std::vector<uint32_t>> codes;
  for (auto& l : lens) codes.push_back(kwaj_canonical(l));
  KwajBitWriter b;
  for (unsigned t : types) b.put(t, 4);
  b.put(sixth, 4);
  for (size_t i = 0; i < 5; i++) kwaj_write_lengths(b, types[i], lens[i]);
  auto sym = [&](int table, unsigned s) {
    if (lens[size_t(table)][s] == 0) abort();  // a symbol without a code
    b.put(codes[size_t(table)][s], int(lens[size_t(table)][s]));
  };
  KwajCrafted out;
  bool short_run = false;
  for (const KwajToken& t : tokens) {
    const int ml = short_run ? 1 : 0;
    if (!t.literals.empty()) {
      sym(ml, 0);
      sym(2, unsigned(t.literals.size() - 1));
      for (uint8_t c : t.literals) sym(4, c);
      out.plain.insert(out.plain.end(), t.literals.begin(), t.literals.end());
      short_run = t.literals.size() != 32;
    } else {
      const unsigned off = t.distance & 0xFFF;
      sym(ml, t.length - 2);
      sym(3, off >> 6);
      b.put(off & 63, 6);
      for (unsigned k = 0; k < t.length; k++) {
        const size_t n = out.plain.size();
        out.plain.push_back(n >= t.distance ? out.plain[n - t.distance] : uint8_t(0x20));
      }
      short_run = false;
    }
  }
  out.stream = b.finish(pad_ones);
  return out;
}

// Lengths 1, 2, ..., top-1, top, top for the first top+1 symbols, 0 after: a complete code.
inline std::vector<unsigned> kwaj_ladder(unsigned n, unsigned top) {
  std::vector<unsigned> l;
  for (unsigned k = 1; k < top; k++) l.push_back(k);
  l.push_back(top);
  l.push_back(top);
  l.resize(n, 0);
  return l;
}

// `used` symbols (a power of two) with equal lengths, the rest without codes.
inline std::vector<unsigned> kwaj_flat(unsigned n, unsigned used) {
  unsigned k = 0;
  while ((1u << k) < used) k++;
  std::vector<unsigned> l(used, k);
  l.resize(n, 0);
  return l;
}

// Any data as a KWAJ file: runs of (at most) 32 literals under fixed tables,
// padded with ones.
inline std::vector<uint8_t> kwaj_literals(const std::vector<uint8_t>& data) {
  std::vector<KwajToken> tokens;
  for (size_t i = 0; i < data.size(); i += 32)
    tokens.push_back(
        KwajToken::lit(std::vector<uint8_t>(data.begin() + i, data.begin() + std::min(data.size(), i + 32))));
  return kwaj_file(kwaj_crafted({0, 0, 0, 0, 0}, std::vector<std::vector<unsigned>>(5), tokens, true).stream);
}

// ---- the crafted vectors ------------------------------------------------------------------------
// kwaj_vectors.py vectors(), token for token. Each returns the whole file and
// its plain bytes.

struct KwajCraftedFile {
  std::vector<uint8_t> file, plain;
};

inline KwajCraftedFile kwaj_crafted_file(const std::vector<unsigned>& types, std::vector<std::vector<unsigned>> lens,
                                         const std::vector<KwajToken>& tokens, bool pad_ones) {
  KwajCrafted c = kwaj_crafted(types, std::move(lens), tokens, pad_ones);
  return {kwaj_file(c.stream), c.plain};
}

// 16-bit LITERAL codes (reached with type 1's "+1" steps), a 15-bit one, runs
// of 32 then MATCHLEN. File md5 72c97211e2cf45f45c48fd9a1f1da5df.
inline KwajCraftedFile kwaj_len16() {
  const auto ml = kwaj_ladder(16, 15);
  return kwaj_crafted_file({1, 1, 1, 1, 1}, {ml, ml, kwaj_flat(32, 32), kwaj_flat(64, 64), kwaj_ladder(256, 16)},
                           {KwajToken::lit({15, 16, 0, 16, 15, 1}), KwajToken::match(4, 3),
                            KwajToken::lit(std::vector<uint8_t>(32, 16)), KwajToken::lit(std::vector<uint8_t>{0})},
                           true);
}

// The tables of kwaj_sparse() and kwaj_phantom(): symbols without codes in
// every table, as real data has.
inline std::vector<std::vector<unsigned>> kwaj_sparse_tables() {
  std::vector<unsigned> lit(256, 0), ml(16, 0), ml2(16, 0), litlen(32, 0), off(64, 0);
  for (char c : std::string_view("ABCDEFGH")) lit[uint8_t(c)] = 3;
  ml[0] = 1, ml[1] = 2, ml[15] = 2;           // a literal run, length 3, length 17
  ml2[0] = 1, ml2[2] = 1;                     // a run (symbol 0), or length 4
  litlen[0] = 1, litlen[1] = 2, litlen[31] = 2;
  off[0] = 1, off[63] = 1;                    // offset high parts 0 and 63 only
  return {ml, ml2, litlen, off, lit};
}
inline std::vector<KwajToken> kwaj_sparse_tokens() {
  return {KwajToken::lit("AB"),
          KwajToken::lit("C"),  // MATCHLEN2 symbol 0: a run right after a short run
          KwajToken::match(4, 1),
          KwajToken::lit("EFGHABCDEFGHABCDEFGHABCDEFGHABCD"),
          KwajToken::match(17, 3),
          KwajToken::match(3, 4032),  // offset high part 63, into the initial spaces
          KwajToken::lit("H"),
          KwajToken::lit("ABCDEFGHABCDEFGHABCDEFGHABCDEFGH")};
}

// MATCHLEN2 symbol 0 twice, distance 4032; padded with ones. File md5
// 720b19e334362263c0dedee47e1a0d74.
inline KwajCraftedFile kwaj_sparse() {
  return kwaj_crafted_file({3, 2, 1, 2, 1}, kwaj_sparse_tables(), kwaj_sparse_tokens(), true);
}

// The same padded with zeros: the 6 zero bits after the last run read as a
// whole one-literal run ("A": MATCHLEN 0, LITLEN 0, LITERAL 'A'). The strict
// reader, the reference and Deark emit it (93 bytes, md5
// 1b2e055c6baaa96c9db02724897094db); libmspack does not (92: it stops after a
// token that starts once its 16-bit look-ahead has run past the end). File
// md5 9940d392f875aab954805b192237ac00.
inline KwajCraftedFile kwaj_phantom() {
  return kwaj_crafted_file({3, 2, 1, 2, 1}, kwaj_sparse_tables(), kwaj_sparse_tokens(), false);
}

// Distance 4096 (offset field 0) at the start (the initial spaces) and after
// 4,209 bytes; distance 64. File md5 53881409e5adfee85e53bd55668f431f.
inline KwajCraftedFile kwaj_dist4096() {
  std::vector<unsigned> off(64, 0);
  off[0] = 1, off[1] = 1;
  std::vector<KwajToken> t = {KwajToken::match(17, 4096), KwajToken::lit(pattern(32, 11))};
  for (int k = 0; k < 130; k++) t.push_back(KwajToken::lit(pattern(32, 12)));
  t.push_back(KwajToken::match(17, 4096));
  t.push_back(KwajToken::match(5, 64));
  const auto ml = kwaj_ladder(16, 15);
  return kwaj_crafted_file({2, 2, 2, 3, 0}, {ml, ml, kwaj_flat(32, 32), off, {}}, t, true);
}

// More than one 64 KiB chunk: 32 literals and 4,116 matches of 17 at
// distance 32, 70,004 bytes. File md5 2ff9096a41f6820b5d52897c497edfee.
inline KwajCraftedFile kwaj_chunk() {
  std::vector<unsigned> off(64, 0);
  off[0] = 1, off[1] = 1;
  std::vector<KwajToken> t = {KwajToken::lit(pattern(32, 5))};
  for (int k = 0; k < 4116; k++) t.push_back(KwajToken::match(17, 32));
  const auto ml = kwaj_ladder(16, 15);
  return kwaj_crafted_file({2, 2, 2, 3, 0}, {ml, ml, kwaj_flat(32, 32), off, {}}, t, true);
}

// The disks' quirk: the last byte keeps exactly 7 bits of 1-bit padding,
// which read as MATCHLEN's 7-bit all-ones code (symbol 7, a match) before the
// OFFSET read runs out — a partial token, nothing emitted (6 of the
// release's files end so). 192 bits of tables, a one-literal run (6 + 8),
// seven length-4 matches of 15 bits: 311 bits. File md5
// 49430ec4f567f2a6a4cf5796aa157b96.
inline KwajCraftedFile kwaj_pad_quirk() {
  std::vector<unsigned> ml7 = {1, 2, 3, 4, 5, 6, 7, 7};
  ml7.resize(16, 0);
  std::vector<KwajToken> t = {KwajToken::lit("Z")};
  for (int k = 0; k < 7; k++) t.push_back(KwajToken::match(4, 1));
  return kwaj_crafted_file({1, 1, 1, 1, 0}, {ml7, ml7, kwaj_flat(32, 32), kwaj_flat(64, 64), {}}, t, true);
}

// The rule's edge: MATCHLEN2 codes the token right after a run of 31
// literals (the longest short run), MATCHLEN the one after a run of 32. The
// one-bit code '1' is a match of 17 in MATCHLEN2 and of 3 in MATCHLEN, so a
// reader that took the other table after either run would give other bytes
// (and stay in step). It ends with a run of 32, which starts more than 16
// bits before the end (libmspack drops a token begun once its 16-bit
// look-ahead has run past the end). 163 bytes. File md5
// 6de76904c170687c6b76c105171ebd05 (research/win/pkg/startrek/fx-importer/
// kwaj_new_vectors.py).
inline KwajCraftedFile kwaj_run31() {
  std::vector<unsigned> ml(16, 0), ml2(16, 0), litlen(32, 0), off(64, 0), lit(256, 0);
  ml[0] = 1, ml[1] = 1;            // a literal run, or a match of 3
  ml2[0] = 1, ml2[15] = 1;         // a literal run, or a match of 17
  litlen[30] = 1, litlen[31] = 1;  // runs of 31 or 32 literals
  off[0] = 1, off[1] = 1;          // offset high parts 0 and 1
  for (char c : std::string_view("ABCDEFGH")) lit[uint8_t(c)] = 3;
  const std::string_view a31 = "ABCDEFGHABCDEFGHABCDEFGHABCDEFG", b32 = "HGFEDCBAHGFEDCBAHGFEDCBAHGFEDCBA";
  return kwaj_crafted_file({3, 2, 1, 2, 1}, {ml, ml2, litlen, off, lit},
                           {KwajToken::lit(a31), KwajToken::match(17, 8),   // MATCHLEN2 '1' after 31 literals
                            KwajToken::lit(b32), KwajToken::match(3, 5),    // MATCHLEN '1' after 32
                            KwajToken::lit(a31), KwajToken::match(17, 67),  // again, OFFSET high part 1
                            KwajToken::lit(b32)},
                           true);
}

// ---- the encoded vectors -------------------------------------------------------------------------

// Each byte of pattern(n, seed) repeated 1..40 times (by a second pattern).
inline std::vector<uint8_t> kwaj_runs(size_t n, uint32_t seed) {
  const auto a = pattern(n, seed), b = pattern(n, seed + 1);
  std::vector<uint8_t> v;
  for (size_t i = 0; i < n; i++) v.insert(v.end(), 1 + b[i] % 40, a[i]);
  return v;
}

struct KwajVector {
  const char* name;
  const char* what;
  const char* md5;     // of the plain bytes
  const char* packed;  // the whole file, hex
};

// kwaj_vectors/vectors.hex (research/win/pkg/startrek/importer).
inline constexpr KwajVector kKwajVectors[] = {
    {"k3_one", "one literal, every table fixed (type 0)", "02129bb861061d1a052c592e2dc6b383",
     "4b57414a88f027d103000e000000000000002c00"},
    {"k3_spaces", "matches into the initial window of spaces; type 1 lengths; padded with ones",
     "1ab76f4a059330685f0b0c312e6e17af",
     "4b57414a88f027d103000e000000111110500d20c500002800000002dc0000001b000000004000000000000000000000"
     "000000000000000000000000000000000000000000000804020100804020100804020100804020100c00c84cadcc9f"},
    {"k3_text_t2", "text, type 2 lengths, padded with ones", "08db9a41f873e0956ecfd4d47d3c1c95",
     "4b57414a88f027d103000e00000022222039165db4995b3d735976d2252b5256525555555550a97616e36895522b6995"
     "55555555555555559555575e5d795555555575e555555d795277e55df9555df977e55189546554955d79ded776db9047"
     "97659e55555555555555555555555555551555555555555555555555555555555555555555450e5225f35129eb0c4103"
     "0e344bc952a4131b2274e238f3151868622b1110af09c4d022e29e25c55089c7f90220904895045028920a0f294d0808"
     "691c00d9eb06240964d1f0c98722cc560e104002024ca4286444a0431eb4d4032b24819390f670161e3126626043ad08"
     "22b0e655aaca82210cc271aaa9cfa1be36d68d4ab2a3b32a662841b8f26010416872d6b6f13bf6125e12c4982a9856ff"
     "c9a35311f01319e00e07403e9c96d0a7129e4f82081cb285e809052d0289b89d65a8ca9e38a92304ea9c82d301d88803"
     "4c216f2c5498430846c04328c049888c1722765e963c66faa5b8fbb56ae880410154283136c5d3819c781c4aa512b268"
     "ade861a4302081c4b0221040a9ac35b2ea00600a0331acd6e5abaa65dd94e57e7279d267cb6067722ad6345aa1500db9"
     "7412535745658df2b2185808a18356dee447cb29d48b9344dfa63543803bc84665a94dc589173672d1882ca63b8a900f"
     "275136b2ba81b26970b6d9892c95d93e7198410484ff9f7db68938904e497ac627ad7c3b9227f54415f97a8e7a4a0a20"
     "c225b90e00e9da9042cbd2df2ebef40641758f5324a5f0732bff6f2ca2b4f00341da27c684a72b152023d2b979e2fa15"
     "6513ccd3d5de6f947ea90fd7559b526e63a9fca2772aa50f0c05cf9fb582f55bb2d7db8ea0f5875e61316e7914db0805"
     "3ab164cfff9c201ac4cc368fcd3722a617bd8fdd0bc9456febd498094e91780b2aadebb98b08202376e74271d61ffb05"
     "f30750e7"},
    {"k3_text_t3", "the same text, type 3, padded with zeros", "08db9a41f873e0956ecfd4d47d3c1c95",
     "4b57414a88f027d103000e00000033333034433344464556666353334446454555235455556665666666666666666666"
     "652344465556867677777767678677888888888888888888888888888888888888999999999959959999999999999999"
     "995999999999999599989979999799999997997999988989999988999999899999959785778698776699666779999999"
     "999999999999999999999999999999999999999999999999988888888888888888888888888888888888888888888888"
     "888888888888888888888888888888888814394897cd44a7ac31040c38d12f254a904c6c89d388e3cc5461a188ac4442"
     "bc2713408b8a78971542271fe408824122541140a248283ca5342021a4700367ac1890259347c3261c8b315838410008"
     "093290a19112810c7ad3500cac92064e43d9c05878c49989810eb4208ac39956ab2a08843309c6aaa73e86f8db5a352a"
     "ca8ecca998a106e3c9804105a1cb5adbc4efd849784b1260aa615bff268d4c47c04c6780381d00fa725b429c4a793e08"
     "2072ca17a02414b40a26e27596a32a78e2a48c13aa720b4c0762200d3085bcb152610c211b010ca3012622305c89d97a"
     "58f19bea96e3eed5aba201040550a0c4db174e0671e0712a944ac9a2b7a18690c0820712c0884102a6b0d6cba8018028"
     "0cc6b35b96aea997765395f9c9e7499f2d819dc8ab58d16a854036e5d0494d5d159637cac8616022860d5b7b911f2ca7"
     "522e4d137e98d50e00ef211996a53716245cd9cb4620b298ee2a403c9d44dacaea06c9a5c2db6624b25764f9c6610412"
     "13fe7df6da24e2413925eb189eb5f0ee489fd51057e5ea39e92828830896e43803a76a410b2f4b7cbafbd01905d63d4c"
     "9297c1ccaffdbcb28ad3c00d07689f1a129cac54808f4ae5e78be855944f334f5779be51faa43f5d566d49b98ea7f289"
     "dcaa943c30173e7ed60bd56ecb5f6e3a83d61d7984c5b9e4536c2014eac5933ffe70806b1330da3f34dc8a985ef63f74"
     "2f2515bfaf5260253a45e02caab7aee62c20808ddb9d09c7587fec17cc1d4380"},
    {"k3_text_mixed", "one table of each encoding (0, 1, 2, 3, 0)", "0b962d316b0e9ee61e3253868b2eda5c",
     "4b57414a88f027d103000e0000000123005cf5ccd7420d0b46b4a1955495255513334445566665676776966667779768"
     "8789998989998998999999999998888880836b7b23ab632b990168010337b31020a925102049c9d5b9cc8131bdb99c81"
     "0599d195c88125b801826dad2e6e6d2dedc40e6c6e4cacadc40dee4d2ced2dcc2d885a0ae8d0cac120ccae6e81a145ac"
     "72417062d20535a444425808a230b93590309039b0bb00e2922cb882a31da87c1c6272726848ee4023103c81d9958dd1"
     "bdc813096c6303579873ca442543a111441a8a530d9ad4c0a0a5a1a14dcf040d0a453a03188c06124167d5dc232e0286"
     "8530cb96227040d0a949005a974ac9984cc0a81127bd13feba981de1e28050930dc82068536590fc61862a80c289468e"
     "b309849209040d0a7088d43b32c84650c115ccc129636529d1604442862e47d8c206ec398ad1c73a5c3660e51f0b3696"
     "1824624424d36152a85c20f3114334f63ce260b42a967019a4c18c2a498cb60260d0187ab9bb2f89e92f49b1a929cc16"
     "60500b501d2521c175548c7e8186c983523cb54afb76d0d0cc31040d0aa4e2dbb52b8cee61600b6252a8260d44e0f466"
     "726a0cce28fbd91795467f4ed1bf97b18452fd79404a1e494bd36b4eb166bf56ee9b5a016d93f6c265ce30830be2f298"
     "24f248771df50e43bb4d664c1b9630999b8e4b3158c8a50da97dadd0066f50c2d075ad8302f51c32aad7f4e501600c69"
     "9895aabfa76613b714232e30964afbf24164cfd271f210018522a296b61f00c397e1e97a050f8aa6697e0400c2d34ac2"
     "1863e1e421a42dafc7ab6affa59af9b70448ccf248aef0a54ef99b133a6a2d48b0b916dde49fe301b1c016cd6e430003"
     "35724ccd3f76b376b98a2016ad1465115e5b6a5f6923ed596567e43ad5fde1ab6e196841927e4fbd794743f3758688be"
     "7a270cf94aaccc5af39200618aa49203ea0183aa6820963878bc41bcebbd7c5b4e8c887e302dd329c579b7bbfa282bdd"
     "8633bba31578a6ea172d2ed9d56501d7f7ea169b97b3d8d037e5e459ad3c91b358c7298609491187e7ab850512a6298d"
     "7ba40a91845e7c24406c66bee8f3cfb5979d728b24d37de60b7b99ae4d4b6efaacd4a18a6d4bd3b654c128b337dabe51"
     "74ece21f5c99b9d73a6c5190d5dcef"},
    {"k3_text_mixed2", "the encodings in another order (3, 2, 1, 0, 1)", "0b962d316b0e9ee61e3253868b2eda5c",
     "4b57414a88f027d103000e00000032101035434344444565645cf5cd7516550b45695aa03586b01aa401af2d7900006b"
     "c800d79389c437e40df96fc8e17107081c40d79ded95b73c6ed9cb6272000000000000e0000000000000000000000105"
     "47312b525202180020c71624abeba401048f566a025e38d5024a5a251e25c3400309aa31040c38d1209cf28a34471e62"
     "830d229405a15163298048629044201b03960170c313030a64c416045310f6411048116001c4091d05c702a2076807c3"
     "9692245b208f2080c0c41e25852744e3c026305b1030215e407341498095c1d18117801ae14a406c85a90050a3081887"
     "84081029e0031023010820c09ac881105c0a1012442f804e1020909200d05d20d320264015c04584bdc27f42eac51e45"
     "798505c12683e42040c359c1f880c402a80881288477418480938a4840818a8f081e0359c11900c302b9060b09621a98"
     "223001142a840b9c4d880811189062e84711e9901b201c80fb81b6e519c046608871d3802a414320212388806704b247"
     "8a005a82a90f0106920190054e38c8a04806809d0b9e957d0fa589490cd71a72016401403400ee348452f089201fa80c"
     "3c330429246d5c9ee43d0843401884082827117b60571077202c01c0d2a009a06a6101fc5cc8cd5033481469ecf04950"
     "19e99db87fd8964011589d6828701434125cf4e0b3ba059e13ae88e90ed00ec13fc513621c404200be8f2a004a464b88"
     "9da40ec077d09af9560885f054c83711968056208ac2b6817e0bd005f8c31971d12c1005fcc7082aa27eb5606481a5f3"
     "e02b613f59d85a18b82325216555d139601675bd6107f8540044115465b097c02644e24cbdc050b4a9069964801184d2"
     "0c42031a2bce086e2ade138d0adc3feb0dad1dba044a58f60914c05614f2b1b439d6416811682e781ef2327aa4043380"
     "3925ca0c00174e2641a74dd6cdd7e51100d2ea405131e5c3a5febc9282c86b3551d11ff52d611191841049d19f9272a7"
     "9fb36c2111d8347673fc29506cd16f4824009044960812e80403b0741c09699f1340f56afaef88b3a95225a811b32071"
     "5037e63a614ad11880679111a23c40dd805c8cbbc3d5d050e3bfcc22e07cc95ed2e1cb9e515ae09251dad29958ac3052"
     "50b86adac80a7022503158bafef818318c0bd2948e01b4b2fe5de91fb4379f9aa2193762f201726b38e3561ef2ead065"
     "100a7065ef5cae9254567282ff18bbcfc509afc39bf4b3b9ca404365ce80"},
    {"k3_binary", "mostly literals: runs of 32, MATCHLEN after them", "15d4c1cca14ef354911760a5314f9a81",
     "4b57414a88f027d103000e0000001222201d4001a22aaaaaaacaaaaaaaaaaa4aab8b2aaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaab10bd71d70adbdd71d709340f46ecaaca3d20b07291d6ef242b0b230c2abd7075c1475c2b10c22d116f75baf370ab"
     "22f5bc4f5c1740b0ab236ef22aaf424efca30a8cc0f248a4b0a3c8a35bc3c85241c8a4adbd6ec8f25b89fd93eee5261e"
     "4f1654c0fd2863b6e1687d678c4bd17edbaca5971c66bd8cf01e2d185569cb31d99b89188b38a7a6318efb3f9a4a4103"
     "e16b1f05dedad2f8e91efd0c7bda99b89ec5d09f72154ec2a4e58ee3538c8ecb8709bbaa22482924d40b6d8d2c6e6806"
     "6a96bb813d5dae52e5cac02ca7439045f11a8487debc99f9ed554465432e6cabb064f065c76fe0f12a1b8886381819aa"
     "f0c36511461302d1d84369606ee81a05c04979ac5e225a8669432d7e184d27fa3bd003b7c3d9f3463bfd1b401d95ef3d"
     "8bb8b487237d2be6f741c3d5ad8b4ee4a00c26c8ce56ea3d984226169760226decb2cf9f5b96363d40ac27fb0432cc5f"
     "1d14a1e51949adcd91f15a9491897bac1682fd2a0b0ad8c1992e6bb8dc47fb2c8ca39052a617228512fd2cef778ee5c5"
     "7f9b502ad75799300c9d704bda8140ca36030ee3babea62b01234cf8c3f4376c14d2099a4472ab27933813b384a28ac1"
     "06bc47bec45122b60a3f935b9b2157c3c25df5f5a4ed404b99ade7845acc80ed0db03830ce3f521488b0f517bc306101"
     "d6411a7abe1ec14e0a84c3589a30e04854fa09e512017324659c452c0c44cb499de77ba4875c0a80b4042a230e0f1f06"
     "2ea1ebd9f702378e201cbdf3c0d68d8d1645687e9841180d123e4c5c36f30626f22f84f4915061a701fd8ec535f96384"
     "e33200d347cd6d2fd054224cb0a6e6a896a2cbcac8b2bf01b761f4916b62b1ec4d342eee02dca3849eea7390135f253a"
     "7413b3edd379ce5e675ec64ed0e9d9c7a578f874fb51b52ead876ba0106922fa1bc6b1a0a6e0590339c5b97138a54519"
     "4a05313f"},
    {"k3_runs", "overlapping matches (distance 1)", "1596df158acb4f20642a1a7d574df1f4",
     "4b57414a88f027d103000e0000002031202d1954962c84d1519dddddddddddddddddddddddddc78da6d36b68dadad90d"
     "b8de37b704dc73b71bf3c6eb7b7b6e1ba48aab10ab28ac4ef30b2295bcaac910aaab0a92b0aa9c8b0722c8aac8aaab22"
     "c2b2273bf22c8aac2aab248b04c2a9c8aaa4a4a7242ceec35bb292dbc722aaaaaaac45bd032a8b3235bc0808e0413502"
     "2bfc0c8106fc0a04c8d9b02040ced0103c4086ec02197c0fe04338810bde070046d50107fc043350101fc0a04a0e1e98"
     "80876f03d02037814080c506a81b81176a044a3810f98044ae0100fc0fe06ac684091140434781b810af204b8012244a"
     "c181161e06408b2e02363c0dc098142856f10100bc0a04a040a1a80819e050241948804577814092b917c0f00899f028"
     "190231bc0a06c042d181017c0a04b0aacacac99e0740415303372040a046cf8140c01bc0d204a5a5acff0320459d8111"
     "3e07407d6ce137a0434881ed9ac70b0113fe0760411f02810bf80421c810c9e06c0417f03f01302c58b52f0281b01a8e"
     "e6e4429fc0a0588d4096142dcdcd2f81b810b3e0600898f03c0237140b2d68120306d8e840e4089ef0223600801e0600"
     "860f03b03142ba02313c0fe04656016feecd0640c5dc2008ccb020efc0d010efe048eed8207d6b62392eb81c5b804bc8"
     "16f8fc02391e0780442781b816b31c02dc90a2246d3c0f408c8a0230e40b2a300456d01175e06c07e86140c0118ee043"
     "7e811a7e07e02147c0e00efc3130102152bf770a02107c0d80896d020eb80871f03f8104040c4a7ea169937e011b7804"
     "6b881153e07f02c36640b8ec180e044678140c81067c090c994c81fe0465f81b8119de05030048489440d93038d40f31"
     "7481d812870e1df073903eda320720422781c81140c0425f81e01178a0438781f01c6b422229bc2580b3b59022d7c092"
     "d5e280b937a02304408e2c0389ce16ca043f701eec85034044590116ec063572d80f60462f8140c81151e050320441d8"
     "10f3e076042df81fc0875303794d0ffe0680863f03d03cc15a0680831e04860c7e57673a01abc0ec084af02c7f392c40"
     "c01159e05032042a28109c80464f8140c816bc7e03de7e700909d5781b011bbe07f02d10a0600ff724143e07c0429a01"
     "19a4043ef81f011a3e07405ca5d01100807380140f80bddb78083d503eef5ac06d58740c010e5e0503006dd0340e80bd"
     "094090102ff233a075877408a0ce21a0496702c07389440f40c438848f025a41a346b5bc0c815520424275a10281123e"
     "06408e0f0281b01dd2ec8108fe072063f7ae81bd45025605b4e501147e07805dd0d01a8ae67b240b30f4046178140c81"
     "0abe06e05e2d20484c9b4eacf8b681b0129414342c0c7afe1a3c0f80811e050241021b4ea2afc0e408d4f03b021cfc0c"
     "017421602d323062810d0587b6d07a037f8b903f01fb3453fd5207605a75f010c5e07c0423081113e049137d605eacd0"
     "10738044a08198680c32041c78140c811a5f38f4066cb021fc0f40df408810ebf5f340c81b58fb1b5e8039d66b3e1802"
     "23bc0951d1dc2c0406f03702d4d6808bf30229bc0fc05bd57c07fe9cdc0f6a7fc042e78122e24c81115e0503006f45c2"
     "4bc0f00b96c903e8ead6e6832781e0107c80457901135e06c048f4f5fc044090b975e0258084ff034041e4010d620414"
     "f0340442f01cdfd9d72fc0739ebe01191e05030045c781e8173b681b812a4e4d97c0bd9ff80e35dd40677bb151b60339"
     "22144a05c37640f3b38cdc7e075c9f21d9dd4844cdf600cdfeac05a4540c01177e07805b24c8113be06e05a2840dc0c6"
     "a56c046af81e8100fc0e40944080cf03602ded6c044cf81a02c86a048450bc0e00c79f025e80b115d02c6a600882f028"
     "1b017641d02d792408cff03e0312f7be2dc723c26f81c817a9bb0242163c07519afb9c53ebeeff01988cbed3bc043698"
     "111e6040ff0250f1e07c0dc08dcc03ed4b8dac02db7e40801e050240003e7dd0242853e80b60469781b8105bc0982c5b"
     "1af480809e05036021efc0d01fe4d9dea20413f985a02f0a1032043d201081e068089cf0300753da81a03d9030bd0044"
     "8781c01ec74fdeceec0d6629f4810251c1c3a78140d80e39eb1a647ed0e81037c09068df"},
};

// A vector's plain bytes, rebuilt the way the research script built them.
inline std::vector<uint8_t> kwaj_vector_plain(std::string_view name) {
  if (name == "k3_one") return {'X'};
  if (name == "k3_spaces") {
    std::vector<uint8_t> v(300, ' ');
    v.insert(v.end(), {'e', 'n', 'd'});
    return v;
  }
  if (name == "k3_text_t2" || name == "k3_text_t3") return arj_vector_text(2000, 5);
  if (name == "k3_text_mixed" || name == "k3_text_mixed2") return arj_vector_text(3000, 9);
  if (name == "k3_binary") return pattern(600, 3);
  if (name == "k3_runs") return kwaj_runs(400, 7);
  abort();
}

}  // namespace test
