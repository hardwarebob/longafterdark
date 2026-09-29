// Synthetic ARJ archives and streams for the importer tests (arj.h;
// research/win/pkg/swse/survey/importer_design.md §6):
//   * a container writer laid out as the Star Wars Screen Entertainment
//     disc's archives are (main header, local headers with CRC-32, optional
//     extended headers, the end marker; multi-volume segments), with knobs
//     for every damaged shape the reader must refuse;
//   * thirteen fixed method-1 / method-4 vectors: made-up data encoded by our
//     research encoder (research/win/pkg/swse/survey/importer/
//     arj_vectors.py, arj_vectors_more.py), and one block written from
//     explicit tables, every one decoded there to the same bytes by both the
//     Python reference decoder and 7-Zip, so they pin the C++ decoder
//     against two independent implementations;
//   * an MSB-first bit writer for streams crafted bit by bit.
// There is no encoder here, on purpose: importer/arj.cc is a modified
// version of UNARJ's DECODE.C, which may be used only in programs that are
// not ARJ archivers, so no test program that links it compresses anything.
// The encoder stays in research/.
// No bytes of any release: every archive here is built from made-up data.
#pragma once

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test {

// ---- bits ------------------------------------------------------------------------------------

// MSB first, as UNARJ's getbits() reads.
class ArjBitWriter {
 public:
  void put(int nbits, uint32_t value) {
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
  // Pads the last byte with zero bits.
  std::vector<uint8_t> flush() {
    if (n_) {
      out_.push_back(uint8_t(acc_ << (8 - n_)));
      acc_ = 0;
      n_ = 0;
    }
    return out_;
  }
  size_t bits() const { return bits_; }

 private:
  std::vector<uint8_t> out_;
  uint8_t acc_ = 0;
  int n_ = 0;
  size_t bits_ = 0;
};

inline uint32_t arj_crc(const std::vector<uint8_t>& d) {
  return uint32_t(crc32(crc32(0, nullptr, 0), d.data(), uInt(d.size())));
}

inline std::vector<uint8_t> unhex(std::string_view h) {
  auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(v(h[i]) << 4 | v(h[i + 1])));
  return out;
}

// ---- the fixed vectors (vectors/vectors.hex, vectors_more/vectors_more.hex) ------------------

struct ArjVector {
  const char* name;
  int method;
  uint32_t size;  // the plain bytes
  uint32_t crc;
  const char* md5;
  const char* packed;  // hex
};

inline constexpr ArjVector kArjVectors[] = {
    // dynamic tables, the i_special skip
    {"m1_text", 1, 700, 0xd6ace3cd, "ea91149958cdc5c1fb0c6a5bc8b6266b",
     "00bf5b7ad544e1b5c90be59ea1a4d45a8c9d4769a96d4f53f45397a2a1200bf1d7c7fef01cb15802c6d2777fefdbbe37"
     "6ef94f1e1fc29193a32d796b02eaefeffcf3b44cca5d1d47ce9ee76fd28f59a493cc4dacda3548c9df918724b197784e"
     "e55a1cac8426b2a836b0b0414e6f5bded1e3dfabe6a5a3cb58a075a1997838e0654a92a6a02b4de5c239846655417d40"
     "eafb6b39eb1808cd6064d8d70bbba1cccfcb1e7c24f7c150c1eb7625a844a36e68d627345092b84185a91499acb829a9"
     "450a62b3a224e17cc23ad93c1562bf1e0500906177defb620ac3db5b62e6e84455fac358505b80c5836b820bc75c4427"
     "298e3e04b52ae7825c87ef4d08995b4700"},
    // n == 0 constant tables for pt, c and p
    {"m1_single_symbol", 1, 40, 0x2ae98c30, "a9451e544b3ae4ad6baad228d5a46198", "00280000041000"},
    // a match overlapping its own output (distance 2, length 98)
    {"m1_overlap", 1, 103, 0x72150bd6, "829d849b94e787977d54eeca2b31a7b2", "0006300946c1134802734300657700"},
    // every literal, a 256-byte match, long zero runs in c_len
    {"m1_all_bytes", 1, 512, 0x1c613576, "f5c8e3c31c044bae0e65569560b54332",
     "0101600b0002bfde0000000000000000000000000000000000000000000000000000000000000004e9011feff8000810"
     "1820283038404850586068707880889098a0a8b0b8c0c8d0d8e0e8f0f901091119212931394149515961697179818991"
     "99a1a9b1b9c1c9d1d9e1e9f1fa020a121a222a323a424a525a626a727a828a929aa2aab2bac2cad2dae2eaf2fb030b13"
     "1b232b333b434b535b636b737b838b939ba3abb3bbc3cbd3dbe3ebf3fc040c141c242c343c444c545c646c747c848c94"
     "9ca4acb4bcc4ccd4dce4ecf4fd050d151d252d353d454d555d656d757d858d959da5adb5bdc5cdd5dde5edf5fe060e16"
     "1e262e363e464e565e666e767e868e969ea6aeb6bec6ced6dee6eef6ff070f171f272f373f474f575f676f777f878f97"
     "9fa7afb7bfc7cfd7dfe7eff7f0"},
    // a run of exactly 19 zero code lengths
    {"m1_gap19", 1, 24, 0x7c580940, "79869bed29dafeb0b0232b732bd54d9c", "001822402155f800659659"},
    // method 4 literals and matches
    {"m4_text", 4, 700, 0x404bb211, "39fcbd9c01ef144ba543524591404992",
     "249b8e865391b4d273399a4de6e101d8ca633a1bce420168808861391ac4061100c472391a0808266810834146d37990"
     "ea6c329cc4073319c8ca6537080825225080e865399d01a0ac03fa0e81a0aa09880de723499cd26e309b0406f3308098"
     "6f3719f20214cb44422603310741d388922d83c9ccc276826ec6cee03e01d0682b80be743419701fe0a8e82af008b00d"
     "6075391d4dc73c05ba363047e009d1a7a3fb00be02cd0210c5c081c8fae220520b64f40a3286e01ee40aec0acaa103fc"
     "84db05b206e124e05b98702cf321b43f6026c863449e0186539c03ba49b0e2f4457008e022c1a191176aa68e5e87cf17"
     "201a0ae63fe0faa3f602d81d53ea01da34743df683330e149ad964bde402e48ec337b0d3ac24e067e2c0034150d7"},
    // two blocks (blocksize 200, then the rest)
    {"m1_two_blocks", 1, 1500, 0x469aa14d, "5b8821753659fc974b35ca29d4b0b054",
     "00c85b7ad544e275c90be5928693516a32751da6a5b53d4c8e39dd1509005f8ea7bff78e57bb958ab0058d26efe7edd7"
     "33e2147cb86139e11867d3db56af76727eca464c9bb93e59ce9ec93df065ce96e66bbeb2191961b7083b7a0d97184f32"
     "51bd9afefe4c912ead594ea7fbd947ce8cfa8c369851e1283e11fee3bdf3b76373c902c071a20504ef35e950e2ff0089"
     "d56a1787388e3e75ad0e71b878b5492bbc069d92569045b56959468e22da9ad2e1b7b71e09a8519ad6a051ba636cb515"
     "8d13c0c7871818378ebb58730c78e3abf4c7a824201a592d027d8c3b4a60aecb4e37b2829f1be2ca2a5eb88cd96a296a"
     "0c2d4828612842cbb573e677be860216aa84432d4a15bd3722398466fafb8006005b4c7269a89c7dddbc7ec17d0a1685"
     "05768196002dad36aaab4ed57e1d2b6c4e56a4fd864065a0649e18dd463de19c442e90aac2ec57593874e71adfa15871"
     "8df8b9690a14b320af67cf5ef56fa262c2933f3610234d8ab14f569c76b1b8c2668541f1ee8d931663ba165275a8dbb0"
     "eb5ef38fb1ed1f5528716231b5a0221cdbe6247e7937993dd1c2e604089d9c7e7e225b374251f45105c830aaa9b8c041"
     "df007c5e7d9a48f9aeeb"},
    // distance 20,482: position symbol 15, 14 extra bits
    {"m1_far", 1, 20499, 0x0439be86, "aad518fc3373d4bf80ec62c9ae1a8c97",
     "0054300840ffa22093c9b7c080000000000600000000000000000000000000000000000000005f400700"},
    // distance 26,624, the window's edge (accepted)
    {"m1_window", 1, 26629, 0xbf194195, "85bd31489d64114a64a1636658963342",
     "006b30046d7f844a24b9d1a040000000000380000000000000000000000000000000000000000000000000003d9ffc"},
    // a 13-bit method-4 pointer (distance 9,001)
    {"m4_far", 4, 9010, 0x33acf38d, "9b603da2a6da390deb0c7b308b7623c5",
     "2c7ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe"
     "007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe007ffe"
     "007ffe007ffe007ffe007c7000b3bf2940"},
    // 100,000 bytes (the 64 KiB ring wraps, 32 KiB chunks): 1,500 bytes of text,
    // repeated with one byte changed in each copy
    {"m1_big", 1, 100000, 0x8cb25a23, "e0886c9d3ea703e98d3ded4cd3a06873",
     "02fd6a74e03b46dbff72660d1e4351b4d81a0c190d49024b498b6408dec8323e2e76d86b6406ddbe3af77ffbbeee91bd"
     "4c6d906de1b6f0bb20c0fc08a5287ff05ff407c3b592485f87dbfdbb345ba375bf1d9b746fd9bb7e8efdbdbecedeeb7d"
     "d9f353c9d3ddbf66df7f6eedddbdfdd778fd5b766cbbf79517dfdffa7c3ddb3768f97a7469a59ff7797cbf7e8f8ecf56"
     "fefdba36fc3bb757e76e8c6b34bf3dbb33e68a053037af1c2d77ca9f0d5f9f3f3df47e184f85c195e36df56e147a620a"
     "7b62e7e1ede1f4e6ecfc29655b5b634e180ab8cefa0d3c3cf6edfd6299e19eb1cc587c22939f37f183bd1dfddec8ca71"
     "f0adbd19a6314e9cf9aebaf3e6cb2cf1b16301c56f4e58ae28f7f933e6bb51297446728a447e38e19bb4aeb7d69d1cde"
     "bbae08c271538c176d773aea4d6b5e1a9f8e798db68b82b6cd7d3165e1b35856afd448a5e3b275e3a5f7e5d657efb4de"
     "38db8fa6b4e8d8fc2e08b1bfce67e58df8796ba15c79f55da4cf9b4d2e9d34c7917863645b1fead831c85c319f86b1f0"
     "c8b7daf1ab516a539ef8752e940aec8d32cc78abceebab57ea265ab787a236cf2d5ab629387affad00c7e1869d52e7e3"
     "f4cb8d7c2c3baddbf1f63c7dae658c5f971db1e725326e37861c5ffdb8ebe393697c61c57a6c17f3b5f17857e9184e2a"
     "35d57f1e846145c85f479697445f51f88c35c30257057105d18eb0ebb5b9f35f56cf9a306ff618eec218e8165a5318d6"
     "967e23f3c37f63f1961ab80e7cbbbb8f50766ed63b63b63b63b7276ff11dbc0e58ed8ed8edfccedfd83b68edc76c76e4"
     "edf58eda3b71db1db93b7a876d1db8ed8edc9dbe81db476e3b63b63b68edc76c76c76c76f91dbd43b63b63b63b7276ec"
     "1db476e3b63b7276fa076d1db8ed8edc9dbe81db476e3b63b7276fd23b68edc76c76e4edf58eda3b71db1db1db476e3b"
     "63b63b63b7c8edea1db1db1db1db93b7ac76d1db8ed8edc9dbf38eda3b71db1db93b760eda3b71db1db93b7ac76d1db8"
     "ed8edc9dbe61db476e3b63b63b68edc76c76c76c76f91dbb076c76c76c76e4edd83b68edc76c76e4edf48eda3b71db1d"
     "b93b7d43b68edc76c76e4edf9076d1db8ed8ed8eda3b71db1db1db1dbe476f9876c76c76c76e4edd83b68edc76c76e4e"
     "dfda3b68edc76c76e4edd83b68edc76c76e4edfa476d1db8ed8edc9dbec1db476e3b63b63b68edc76c76c76c76f91dbe"
     "c1db1db1db1db93b7ce3b68edc76c76e4edfa076d1db8ed8edc9dbec1db476e3b63b7276ec1db476e3b63b7276ebff3d"
     "751db8ed8ed8ed8eda3b71db1db1db1dbff8edf30ed8ed8ed8edc9dbb076d1db8ed8edc9dba7f22d0eda3b71db1dbfa1"
     "dbe81db476e3b63b7276fc83b68edc76c76c76d1db8ed8ed8ed8edf23b7d83b63b63b63b7276fa476d1db8ed8edc9dbd"
     "63b68edc76c76e4edde35076d1db8ed8edfd4edeb1db476e3b63b7276fcc3b68edc76c76c76d1db8ed8ed8ed8edf23b7"
     "d63b63b63b63b7276ec1db476e3b63b7276fcc3b68edc76c76e4edf48eda3b71db1db93b760eda3b71db1db93b7d43b6"
     "8edc76c76c76d1db8ed8ed8ed8edf23b7f90ed8ed8ed8edc9dbb076d1db8ed8edc9dbea1db476e3b63b7276fa076d1db"
     "8ed8edc9dbd43b68edc76c76e4eddffd0503b68edc76c76c76d1db8ed8ed8ed8edffe76ec1db1db1db1db93b7b876d1d"
     "b8ed8edc9dbd43b68edc76c76e4edf9c76d1db8ed8edc9dbb076d1db8ed8ed8eda3b71db1db1db1dbe476f58ed8ed8ed"
     "8edc9dbd63b68edc76c76e4edea1db476e3b63b7276fac76ffd8edc0"},
    // the same bytes, method 4
    {"m4_big", 4, 100000, 0x8cb25a23, "e0886c9d3ea703e98d3ded4cd3a06873",
     "3a1a0ca2030880e6613b194e4203a194e674101bce469339a4dc6136034159050926e3a194e46d349cce669379ba0266"
     "339194cb007e2129b4de643a9b0ca731077064402dc89e311c8e468203b194c674379c840723a9b8e7b02cc220e01ce4"
     "7c341941a0ac0419218cdc0968056408299688844a13f424fa0c1c47130d0dba04f928320ee067e80ef4030825225644"
     "a30ec5816e882d967421c82244309c8d780b391f5c0e2d01180a41a0af191c4c379b8cf834f0046c5ec1d382a45a0d05"
     "41ac1a0ae26cf5117041e03b16f12af013d0501a0a8a26468c1e580a300275266257b1e85a202099a0c1824f019e08b3"
     "0edce814ec3ae4a7f03e70db6060d8b3b067025e99ad075544c057c193dc3a51f2095e169b996dd419d85d863f430604"
     "6f5467f12dfa02bb42bac7d6da4068285b91a45b92f303870120c38007620e2217459f258fa18b457c2b5815331efa94"
     "bc841b64726ce012dc7740eaa89c55bd03186cf8ab461c053a06fca807002349d6467ea067186be821c17697dc21f767"
     "2e0afe30edc2393266f01c350bfda1d7999b91e350af960bc8d7e070d819824f018ed57e7211a0978d5f6015c8d181eb"
     "002609ae8426c8aeb45d82eb52ec610682a86c0d05608df4d31c2438205c529c16b913f857e727f70027126a50e779dd"
     "e12826539953fffbdbfffef6ffffbdbfffef6ffd3bdb2affff7b7abcbfffef6ffffbdbfffef6ffccbdb0a7fff7b7fffe"
     "b6ffffdedffff7b7fe9ded93ffffbdbffff5b7fffef6ffffbdbff4ef6ca7fffdedffffadbffff7b7fffdedffa77b645f"
     "ffef6ffffd6dffffbdbfffef6ffffbdbffff5b7fffef6ffffbdbfffef6ffffbdbfebef6ca7fffdedffff7b7fffdedfff"
     "f7b7fe9ded803fffbdbffff5b7fffef6ffffbdbff4ef6c8bfffdedffffadbffff7b7fffdedffa77b645fffef6ffffd6d"
     "ffffbdbfffef6ffd3bdb26ffff7b7fffeb6ffffdedffff7b7fe9ded93ffffbdbffff5b7fffef6ffffbdbfffef6ffffd6"
     "dffffbdbfffef6ffffbdbfffef6ffafbdb29ffff7b7fffdedffff7b7fffdedffa77b654fffef6ffffd6dffffbdbfffef"
     "6ffd3bdb23ffff7b7fffeb6ffffdedffff7b7fe9ded803fffbdbffff5b7fffef6ffffbdbff4ef6ca9fffdedffffadbff"
     "ff7b7fffdedffa77b641fffef6ffffd6dffffbdbfffef6ffffbdbffff5b7fffef6ffffbdbfffef6ffffbdbfebef6c01f"
     "ffdedffff7b7fffdedffff7b7fe9ded803fffbdbffff5b7fffef6ffffbdbff4ef6c93fffdedffffadbffff7b7fffdedf"
     "fa77b64efffef6ffffd6dffffbdbfffef6ffd3bdb0cffff7b7fffeb6ffffdedffff7b7fffdedffffadbffff7b7fffded"
     "ffff7b7fffdedff5f7b641fffef6ffffbdbfffef6ffffbdbff4ef6c01fffdedffffadbffff7b7fffdedffa77b62afffe"
     "f6ffffd6dffffbdbfffef6ffd3bdb007fff7b7fffeb6ffffdedffff7b7fe9ded937fffbdbffff5b7fffef6ffffbdbff4"
     "ef6ca5fffdedffffadbffff7b7fffdedffff7b7fffeb6ffffdedffff7b7fffdedffff7b7fd7ded94bfffbdbfffef6fff"
     "fbdbfffef6ffd3bdb227fff7b7fffeb6ffffdedffff7b7fe9ded933fffbdbffff5b7fffef6ffffbdbff4ef6ca5fffded"
     "ffffadbffff7b7fffdedffa77b600fffef6ffffd6dffffbdbfffef6ffd3bdb24ffff7b7fffeb6ffffdedffff7b7fffde"
     "dffffadbffff7b7fffdedffff7b7fffdedff5f7b641fffef6ffffbdbfffef6ffffbdbff4ef6c01fffdedffffadbffff7"
     "b7fffdedffa77b79e65afffef6ffffd6dffffbdbfffef6ffcebdb22ffff7b7fffeb6ffffdedffff7b7fe9ded867fffbd"
     "bffff5b7fffef6ffffbdbfffef6ffffd6dffffbdbfffef6ffffbdbfffef6ffafbdb297fff7b7fffdedffff7b7fffdedf"
     "fa77b649fffef6ffffd6dffffbdbfffef6ffd3bdb2a7fff7b7fffeb6ffffdedffff7b7fe9deddad4fffef6ffffd6dfff"
     "fbdbfffef6ffd0bdb2a7fff7b7fffeb6ffffdedffff7b7fe9ded91bfffbdbffff5b7fffef6ffffbdbfffef6ffffd6dff"
     "ffbdbfffef6ffffbdbfffef6ffafbdb27ffff7b7fffdedffff7b7fffdedffa77b600fffef6ffffd6dffffbdbfffef6ff"
     "d3bdb237fff7b7fffeb6ffffdedffff7b7fe9ded927fffbdbffff5b7fffef6ffffbdbff4ef6c01fffdedffffadbffff7"
     "b7fffdedffa77b64efffef6ffffd6dffffbdbfffef6ffffbdbffff5b7fffef6ffffbdbfffef6ffffbdbfebef6cadfffd"
     "edffff7b7fffdedffff7b7fe9ded803fffbdbffff5b7fffef6ffffbdbff4ef6c9dfffdedffffadbffff7b7fffdedffa7"
     "7b645fffef6ffffd6dffffbdbfffef6ffd3bdb29ffff7b7fffeb6ffffdedffff7b7fe9deddea28fffef6ffffd6dffffb"
     "dbfffef6ffffbdbffff5b7fffef6ffffbdbfffef6ffffbdbfeb2f6c01fffdedffff7b7fffdedffff7b7fe9ded9cffffb"
     "dbffff5b7fffef6ffffbdbff4ef6ca7fffdedffffadbffff7b7fffdedffa77b647fffef6ffffd6dffffbdbfffef6ffd3"
     "bdb007fff7b7fffeb6ffffdedffff7b7fffdedffffadbffff7b7fffdedffff7b7fffdedff5f7b654fffef6ffffbdbfff"
     "ef6ffffbdbff4ef6ca9fffdedffffadbffff7b7fffdedffa77b653fffef6ffffd6dffffbdbfffef6ffd3bdb27ffff7b7"
     "fa5d6dc0"},
    // one block from explicit tables, not the encoder: NT codes of 9..16 bits
    // (NT symbols 17 and 18 too), c codes of 13..16 bits, a 16-bit p code
    {"m1_long_codes", 1, 19, 0x079a7249, "6537a2c841fd6a8eb71d43ae7b4f8aa8",
     "00119804fff272eef7dfbfbfdff7feffefff4042dfffeb77befdfdfeffbff7ff7ffbffefffdfffe4dffffc7ffc539777"
     "befdfdfeffbff7ff7ff96ef7dfbfbfdff7feffefff7ffdfffbfffbfffffff8"},
};

// m1_long_codes: its first c_len item is the long zero run, then comes the
// c length 1, coded by NT symbol 3, whose code is 16 bits (all ones but the
// last): they are the stream's bits 164..179, read through read_c_len's tree.
inline constexpr size_t kArjLongCodesNtBit = 164, kArjLongCodesNtBits = 16;

inline const ArjVector& arj_vector(std::string_view name) {
  for (const ArjVector& v : kArjVectors)
    if (name == v.name) return v;
  abort();
}

// The made-up text the text vectors encode (arj_vectors.py text()).
inline std::vector<uint8_t> arj_vector_text(size_t n, uint32_t seed) {
  static const char* const kWords[] = {"Long",  "After", "Dark",   "runs", "the",  "original",     "modules",
                                       "of",    "a",     "screen", "saver", "test", "vector",      "-",
                                       "1994",  "Intermission", "ARJ", "SZDD"};
  const size_t words = sizeof(kWords) / sizeof(kWords[0]);
  uint64_t x = seed;
  std::string out;
  while (out.size() < n) {
    x = (x * 1103515245u + 12345u) & 0x7FFFFFFF;
    out += kWords[(x >> 16) % words];
    out += ((x >> 8) % 13 == 0) ? "\r\n" : " ";
  }
  out.resize(n);
  return std::vector<uint8_t>(out.begin(), out.end());
}

// A vector's plain bytes, rebuilt the way the research scripts built them.
inline std::vector<uint8_t> arj_vector_plain(std::string_view name) {
  auto rep = [](std::string_view unit, size_t times) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i < times; i++) v.insert(v.end(), unit.begin(), unit.end());
    return v;
  };
  auto cat = [](std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  if (name == "m1_text") return arj_vector_text(700, 7);
  if (name == "m1_single_symbol") return rep("A", 40);
  if (name == "m1_overlap") return cat(rep("ab", 50), rep("xyz", 1));
  if (name == "m1_all_bytes") {
    std::vector<uint8_t> v;
    for (int k = 0; k < 2; k++)
      for (int b = 0; b < 256; b++) v.push_back(uint8_t(b));
    return v;
  }
  if (name == "m1_gap19") return rep(std::string_view("\x00\x14\x14\x00\x00\x14", 6), 4);
  if (name == "m4_text") return arj_vector_text(700, 11);
  if (name == "m1_two_blocks") return arj_vector_text(1500, 3);
  if (name == "m1_far") return cat(cat(cat(rep("X", 20481), rep("Y", 1)), rep("X", 16)), rep("Z", 1));
  if (name == "m1_window") return cat(cat(rep("X", 26625), rep("Y", 1)), rep("X", 3));
  if (name == "m4_far") return cat(cat(rep("X", 9001), rep("Y", 1)), rep("X", 8));
  if (name == "m1_big" || name == "m4_big") {  // arj_vectors_more.py big_plain()
    const auto unit = arj_vector_text(1500, 21);
    std::vector<uint8_t> v;
    for (size_t k = 0; v.size() < 100000; k++) {
      v.insert(v.end(), unit.begin(), unit.end());
      v[v.size() - 1 - (k * 263) % 1500] ^= 0x20;
    }
    v.resize(100000);
    return v;
  }
  if (name == "m1_long_codes") return rep("ABCDEFGHIJKLMNOPPPP", 1);
  abort();
}

// ---- the container ------------------------------------------------------------------------------

// 1994-10-12 21:38:44, DOS date << 16 | time (the disc's members' day).
inline constexpr uint32_t kArjDosTime = (14u << 25) | (10u << 21) | (12u << 16) | (21u << 11) | (38u << 5) | 22u;

// One header, field by field (the local layout; the main header reuses it
// with file type 2), plus the damaged shapes the reader must refuse.
struct ArjHeader {
  uint8_t version = 4, min_version = 1, host_os = 0;
  uint8_t flags = 0x10;  // PATHSYM, as on the disc
  uint8_t method = 0;    // the main header's security version
  uint8_t file_type = 0;
  uint8_t reserved = 0;
  uint32_t datetime = kArjDosTime;
  uint32_t csize = 0, osize = 0, crc = 0;
  uint16_t fspos = 0, mode = 0x20, host_data = 0;
  std::optional<uint32_t> ext_pos;      // at offset 30 (first_hdr_size 34)
  std::optional<uint8_t> first_size;    // override (default 30, or 34 with ext_pos)
  std::string name, comment;
  std::vector<std::string> ext_headers;  // extended headers' data
  // Damage knobs.
  bool bad_crc = false;                   // the basic header's CRC-32 is off by one
  bool bad_ext_crc = false;               // so is every extended header's
  std::optional<uint16_t> basic_size;     // written instead of the real basic_hdr_size
  bool unterminated_name = false;         // no NUL after the name (nor a comment)
  bool pad_to_first = true;               // false: a first_size past the fields is not backed by bytes
};

inline void arj_put16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}
inline void arj_put32(std::vector<uint8_t>& v, uint32_t x) {
  arj_put16(v, x & 0xFFFF);
  arj_put16(v, x >> 16);
}

inline std::vector<uint8_t> arj_header_bytes(const ArjHeader& h) {
  const uint8_t first = h.first_size ? *h.first_size : uint8_t(h.ext_pos ? 34 : 30);
  std::vector<uint8_t> b = {first, h.version, h.min_version, h.host_os, h.flags, h.method, h.file_type, h.reserved};
  arj_put32(b, h.datetime);
  arj_put32(b, h.csize);
  arj_put32(b, h.osize);
  arj_put32(b, h.crc);
  arj_put16(b, h.fspos);
  arj_put16(b, h.mode);
  arj_put16(b, h.host_data);
  if (first >= 34) arj_put32(b, h.ext_pos.value_or(0));
  if (h.pad_to_first && b.size() < first) b.resize(first, 0);
  b.insert(b.end(), h.name.begin(), h.name.end());
  if (!h.unterminated_name) {
    b.push_back(0);
    b.insert(b.end(), h.comment.begin(), h.comment.end());
    b.push_back(0);
  }
  std::vector<uint8_t> out = {0x60, 0xEA};
  arj_put16(out, h.basic_size ? *h.basic_size : uint32_t(b.size()));
  out.insert(out.end(), b.begin(), b.end());
  arj_put32(out, arj_crc(b) ^ (h.bad_crc ? 1u : 0u));
  for (const std::string& e : h.ext_headers) {
    std::vector<uint8_t> d(e.begin(), e.end());
    arj_put16(out, uint32_t(d.size()));
    out.insert(out.end(), d.begin(), d.end());
    arj_put32(out, arj_crc(d) ^ (h.bad_ext_crc ? 1u : 0u));
  }
  arj_put16(out, 0);  // no more extended headers
  return out;
}

// One member (or segment of one) in a volume: its header and its data.
struct ArjEntry {
  ArjHeader h;
  std::vector<uint8_t> data;
};

// A stored segment: the header's sizes and CRC-32 describe `plain`.
inline ArjEntry arj_stored_entry(const std::string& name, const std::vector<uint8_t>& plain, uint8_t flags = 0x10,
                                 std::optional<uint32_t> ext_pos = std::nullopt) {
  ArjEntry e;
  e.h.name = name;
  e.h.flags = flags;
  e.h.method = 0;
  e.h.csize = e.h.osize = uint32_t(plain.size());
  e.h.crc = arj_crc(plain);
  e.h.ext_pos = ext_pos;
  e.data = plain;
  return e;
}

// A compressed segment: `packed` decodes to `plain` with `method`.
inline ArjEntry arj_packed_entry(const std::string& name, int method, const std::vector<uint8_t>& packed,
                                 const std::vector<uint8_t>& plain, uint8_t flags = 0x10,
                                 std::optional<uint32_t> ext_pos = std::nullopt) {
  ArjEntry e = arj_stored_entry(name, plain, flags, ext_pos);
  e.h.method = uint8_t(method);
  e.h.csize = uint32_t(packed.size());
  e.data = packed;
  return e;
}

// The main header of a volume (VOLUME_FLAG when another one follows).
inline ArjHeader arj_main_header(const std::string& name, bool continues) {
  ArjHeader m;
  m.name = name;
  m.file_type = 2;
  m.flags = uint8_t(0x10 | (continues ? 0x04 : 0));
  m.mode = 0;
  m.csize = m.osize = m.crc = 0;
  return m;
}

// A whole volume: main header, the entries, the end marker (optional), and
// any bytes after it.
inline std::vector<uint8_t> arj_volume(const std::string& name, bool continues, const std::vector<ArjEntry>& entries,
                                       bool end_marker = true, const std::vector<uint8_t>& trailing = {},
                                       const ArjHeader* main = nullptr) {
  std::vector<uint8_t> v = arj_header_bytes(main ? *main : arj_main_header(name, continues));
  for (const ArjEntry& e : entries) {
    auto h = arj_header_bytes(e.h);
    v.insert(v.end(), h.begin(), h.end());
    v.insert(v.end(), e.data.begin(), e.data.end());
  }
  if (end_marker) v.insert(v.end(), {0x60, 0xEA, 0x00, 0x00});
  v.insert(v.end(), trailing.begin(), trailing.end());
  return v;
}

// `vol` (as arj_volume writes it) with its main header's flags replaced and
// the header's CRC-32 made again: VOLUME_FLAG (0x04) on the last volume of
// a set makes it say that the archive goes on.
inline std::vector<uint8_t> arj_with_main_flags(std::vector<uint8_t> vol, uint8_t flags) {
  const size_t size = size_t(vol.at(2) | vol.at(3) << 8);  // basic header size
  vol.at(8) = flags;                                        // after 60 EA, the size and 4 bytes
  const uint32_t crc = arj_crc(std::vector<uint8_t>(vol.begin() + 4, vol.begin() + 4 + ptrdiff_t(size)));
  for (int i = 0; i < 4; i++) vol.at(4 + size + size_t(i)) = uint8_t(crc >> (8 * i));
  return vol;
}

// One volume of stored members.
inline std::vector<uint8_t> arj_stored(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& members,
                                       const std::string& name = "TEST.ARJ") {
  std::vector<ArjEntry> e;
  for (const auto& [n, d] : members) e.push_back(arj_stored_entry(n, d));
  return arj_volume(name, false, e);
}

// A stored member split over volumes at `cuts` (byte offsets, ascending):
// volume k holds bytes [cuts[k-1], cuts[k]). The segment that continues is
// the last member of its volume (VOLUME_FLAG); each continuation is the first
// member of the next (EXTFILE_FLAG with the position it resumes at) — the
// disc's layout.
inline std::vector<std::pair<std::string, std::vector<uint8_t>>> arj_split(const std::string& member,
                                                                           const std::vector<uint8_t>& data,
                                                                           const std::vector<size_t>& cuts,
                                                                           const std::vector<std::string>& volumes) {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
  size_t from = 0;
  for (size_t k = 0; k < volumes.size(); k++) {
    const size_t to = k < cuts.size() ? cuts[k] : data.size();
    const bool first = k == 0, last = k + 1 == volumes.size();
    uint8_t flags = uint8_t(0x10 | (last ? 0 : 0x04) | (first ? 0 : 0x08));
    std::optional<uint32_t> pos;
    if (!first) pos = uint32_t(from);
    auto e = arj_stored_entry(member, std::vector<uint8_t>(data.begin() + from, data.begin() + to), flags, pos);
    out.push_back({volumes[k], arj_volume(volumes[k], !last, {e})});
    from = to;
  }
  return out;
}

}  // namespace test
