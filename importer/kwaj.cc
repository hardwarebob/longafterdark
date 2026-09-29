#include "kwaj.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace adw::import {

namespace {

constexpr uint8_t kMagic[8] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1};
constexpr uint8_t kSzdd[4] = {'S', 'Z', 'D', 'D'};
constexpr size_t kHeader = 14;
constexpr uint16_t kLzh = 3;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// A read needed a bit past the end of the stream: where that happened decides
// whether the file ended cleanly (decode_lzh).
struct End {};

// MSB first, a bit at a time.
class Bits {
 public:
  explicit Bits(std::span<const uint8_t> d) : d_(d), total_(uint64_t(d.size()) * 8) {}
  unsigned bit() {
    if (pos_ >= total_) throw End{};
    const unsigned b = (d_[size_t(pos_ >> 3)] >> (7 - (pos_ & 7))) & 1u;
    pos_++;
    return b;
  }
  unsigned bits(int n) {
    unsigned v = 0;
    while (n-- > 0) v = v << 1 | bit();
    return v;
  }
  uint64_t pos() const { return pos_; }
  uint64_t total() const { return total_; }

 private:
  std::span<const uint8_t> d_;
  uint64_t total_, pos_ = 0;
};

// The five codes of a method-3 stream, in the order its header gives their
// table types.
constexpr const char* kTableName[5] = {"MATCHLEN", "MATCHLEN2", "LITLEN", "OFFSET", "LITERAL"};
constexpr unsigned kTableSize[5] = {16, 16, 32, 64, 256};

unsigned fixed_length(unsigned symbols) { return symbols == 16 ? 4 : symbols == 32 ? 5 : symbols == 64 ? 6 : 8; }

// A canonical Huffman code (shorter codes first, then by symbol), decoded bit
// by bit by counting codes of each length, so no table larger than the code
// is ever built. Only complete codes are accepted: every bit pattern then
// leads to a symbol, and a code the release never uses (incomplete, or
// over-subscribed) is a damaged or foreign file.
class Code {
 public:
  Code(const std::vector<unsigned>& lengths, const std::string& what) {
    for (unsigned l : lengths)
      if (l) count_[l]++;
    uint64_t kraft = 0;
    for (unsigned l = 1; l <= 16; l++) kraft += uint64_t(count_[l]) << (16 - l);
    if (kraft < (1u << 16)) throw KwajError(what + " is not a complete code");
    if (kraft > (1u << 16)) throw KwajError(what + " is an over-subscribed code");
    for (unsigned l = 1; l <= 16; l++)
      for (size_t s = 0; s < lengths.size(); s++)
        if (lengths[s] == l) symbols_.push_back(uint16_t(s));
  }

  unsigned read(Bits& b) const {
    unsigned code = 0, first = 0, index = 0;
    for (unsigned l = 1; l <= 16; l++) {
      code |= b.bit();
      const unsigned n = count_[l];
      if (code - first < n) return symbols_[index + code - first];
      index += n;
      first = (first + n) << 1;
      code <<= 1;
    }
    throw KwajError("a bit pattern with no code");  // unreachable: the code is complete
  }

 private:
  std::array<unsigned, 17> count_{};
  std::vector<uint16_t> symbols_;
};

// One table's code lengths, by its type.
std::vector<unsigned> read_lengths(Bits& b, unsigned type, unsigned symbols, const std::string& what) {
  std::vector<unsigned> out;
  out.reserve(symbols);
  if (type == 0) return std::vector<unsigned>(symbols, fixed_length(symbols));
  if (type == 3) {
    for (unsigned i = 0; i < symbols; i++) out.push_back(b.bits(4));
    return out;
  }
  int c = int(b.bits(4));
  out.push_back(unsigned(c));
  for (unsigned i = 1; i < symbols; i++) {
    if (type == 1) {
      if (b.bit()) c = b.bit() ? int(b.bits(4)) : c + 1;
    } else {
      const int sel = int(b.bits(2));
      c = sel == 3 ? int(b.bits(4)) : c + sel - 1;
    }
    if (c < 0 || c > 16) throw KwajError(what + ": a code length of " + std::to_string(c));
    out.push_back(unsigned(c));
  }
  return out;
}

}  // namespace

KwajHeader kwaj_header(std::span<const uint8_t> f, std::string_view name) {
  const std::string n(name);
  if (f.size() >= 4 && std::equal(kSzdd, kSzdd + 4, f.begin())) throw KwajError(n + ": an SZDD file, not KWAJ");
  const size_t m = std::min<size_t>(f.size(), sizeof(kMagic));
  if (f.size() < 4 || !std::equal(f.begin(), f.begin() + m, kMagic)) throw KwajError(n + ": not a KWAJ file");
  if (f.size() < kHeader) throw KwajError(n + ": the KWAJ header is cut short");
  KwajHeader h;
  h.method = le16(&f[8]);
  h.data_offset = le16(&f[10]);
  h.flags = le16(&f[12]);
  if (h.method != kLzh) throw KwajError(n + ": KWAJ method " + std::to_string(h.method) + " is not supported");
  if (h.flags) {
    char flags[8];
    snprintf(flags, sizeof(flags), "0x%04X", h.flags);
    throw KwajError(n + ": KWAJ header flags " + flags + " are not supported");
  }
  if (h.data_offset != kHeader)
    throw KwajError(n + ": the KWAJ data starts at " + std::to_string(h.data_offset) +
                    ", not after the 14-byte header");
  return h;
}

uint64_t kwaj_expand(std::span<const uint8_t> f, std::string_view name, uint64_t max_size,
                     const std::function<void(const uint8_t*, size_t)>& sink) {
  kwaj_header(f, name);
  return kwaj_detail::decode_lzh(f.subspan(kHeader), name, max_size, sink);
}

namespace kwaj_detail {

uint64_t decode_lzh(std::span<const uint8_t> stream, std::string_view name, uint64_t max_size,
                    const std::function<void(const uint8_t*, size_t)>& sink) {
  const std::string n(name);
  Bits b(stream);
  std::vector<Code> codes;
  try {
    unsigned types[6];
    for (unsigned& t : types) t = b.bits(4);
    if (types[5]) throw KwajError(n + ": the sixth table type is " + std::to_string(types[5]) + ", not 0");
    for (int i = 0; i < 5; i++) {
      const std::string what = n + ": " + kTableName[i];
      if (types[i] > 3)
        throw KwajError(what + ": code-length encoding " + std::to_string(types[i]) + " is not one of 0-3");
      codes.emplace_back(read_lengths(b, types[i], kTableSize[i], what), what);
    }
  } catch (const End&) {
    throw KwajError(n + ": the compressed data ends inside the code tables");
  }
  const Code &matchlen = codes[0], &matchlen2 = codes[1], &litlen = codes[2], &offset = codes[3], &literal = codes[4];

  constexpr size_t kChunk = 64 * 1024;
  // The window: 4096 spaces, so a match may reach back before the first byte.
  std::array<uint8_t, 4096> win;
  win.fill(0x20);
  size_t wpos = 0;
  std::vector<uint8_t> out;
  out.reserve(size_t(std::min<uint64_t>(kChunk, max_size)));
  uint64_t produced = 0;
  auto put = [&](uint8_t v) {
    out.push_back(v);
    win[wpos] = v;
    wpos = (wpos + 1) & 0xFFF;
    if (out.size() == kChunk) {
      sink(out.data(), out.size());
      out.clear();
    }
  };
  auto room = [&](unsigned bytes) {
    if (bytes > max_size - produced)
      throw KwajTooLarge(n + ": expands to more than " + std::to_string(max_size) + " bytes");
  };
  bool short_run = false;  // the last token was a literal run shorter than 32: MATCHLEN2 codes the next
  uint8_t run[32];
  for (;;) {
    const uint64_t start = b.pos();
    try {
      const unsigned s = (short_run ? matchlen2 : matchlen).read(b);
      if (s) {
        const unsigned length = s + 2;
        unsigned distance = offset.read(b) << 6;
        distance |= b.bits(6);
        if (!distance) distance = 4096;
        room(length);
        for (unsigned k = 0; k < length; k++) put(win[(wpos - distance) & 0xFFF]);
        produced += length;
        short_run = false;
      } else {
        const unsigned count = litlen.read(b) + 1;
        for (unsigned k = 0; k < count; k++) run[k] = uint8_t(literal.read(b));
        room(count);
        for (unsigned k = 0; k < count; k++) put(run[k]);
        produced += count;
        short_run = count != 32;
      }
    } catch (const End&) {
      // The token that ran out produces nothing. Inside the last byte's
      // padding (fewer than 8 bits) the stream simply ended; anywhere else
      // the file was cut.
      const uint64_t tail = b.total() - start;
      if (tail >= 8)
        throw KwajError(n + ": the compressed data ends inside a token (" + std::to_string(tail) +
                        " bits before the end)");
      break;
    }
  }
  if (!out.empty()) sink(out.data(), out.size());
  return produced;
}

}  // namespace kwaj_detail

}  // namespace adw::import
