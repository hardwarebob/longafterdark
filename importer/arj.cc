#include "arj.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>

#include "names.h"
#include "status.h"
#include "winutil.h"

namespace adw::import {

namespace {

using Sink = std::function<void(const uint8_t*, size_t)>;

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

uint32_t crc32_of(const uint8_t* p, size_t n) {
  return uint32_t(crc32(crc32(0, nullptr, 0), p, uInt(n)));
}

std::string hex2(unsigned v) {
  char b[8];
  snprintf(b, sizeof(b), "0x%02X", v & 0xFF);
  return b;
}

// ---- the container (importer_design.md §2.2) --------------------------------------------

// The basic header's size: 30 bytes of fields at least; UNARJ's
// HEADERSIZE_MAX (30 + 10 + 512 + 2048) at most. The disc's largest is 48.
constexpr size_t kMinBasic = 30, kMaxBasic = 2600;
constexpr int kMaxExtHeaders = 16;
// The minimum version needed to extract: 3 is the ARJ 2.x decoder
// generation (every header on the disc says 1).
constexpr uint8_t kMaxMinVersion = 3;
// Members per archive (a ZIP directory holds at most 65535 too); the staging
// budget bounds what is copied, this bounds what is listed.
constexpr size_t kMaxMembers = 65535;

constexpr uint8_t kGarbled = 0x01, kVolume = 0x04, kExtFile = 0x08, kPathSym = 0x10, kBackup = 0x20;

struct Header {
  size_t start = 0;  // offset of its 60 EA id
  size_t end = 0;    // just past it: basic header, CRC, extended headers (a local header's data starts here)
  bool end_marker = false;
  uint8_t first_size = 0, min_ver = 0, host_os = 0, flags = 0, method = 0, file_type = 0;
  uint32_t datetime = 0, csize = 0, osize = 0, crc = 0;
  uint16_t fspos = 0, mode = 0;
  bool has_ext_pos = false;
  uint32_t ext_pos = 0;
  std::string raw_name;  // as stored (code page 437)
  std::string name;      // UTF-8
};

// One header at `pos`, its id, size, CRC-32 and extended headers checked.
// `main`: the archive's first header (a 0 size is no archive at all, not an
// end marker).
Header read_header(std::span<const uint8_t> d, size_t pos, bool main, const std::string& vol) {
  Header h;
  h.start = pos;
  auto at = [&] { return " at offset " + std::to_string(pos); };
  if (pos > d.size() || d.size() - pos < 4) {
    if (main) throw ArjError(vol + ": not an ARJ archive (too small)");
    throw ArjError(vol + ": no end-of-archive header (truncated)");
  }
  if (d[pos] != 0x60 || d[pos + 1] != 0xEA) {
    if (main) throw ArjError(vol + ": not an ARJ archive (no main header)");
    throw ArjError(vol + ": damaged archive (no header" + at() + ")");
  }
  const size_t size = le16(&d[pos + 2]);
  if (size == 0) {
    if (main) throw ArjError(vol + ": not an ARJ archive (no main header)");
    h.end_marker = true;
    h.end = pos + 4;
    return h;
  }
  if (size < kMinBasic || size > kMaxBasic)
    throw ArjError(vol + ": damaged header" + at() + " (" + std::to_string(size) + " bytes)");
  if (d.size() - pos - 4 < size + 4) throw ArjError(vol + ": the archive is truncated");
  const uint8_t* b = &d[pos + 4];
  if (crc32_of(b, size) != le32(b + size)) throw ArjError(vol + ": header CRC-32 mismatch" + at());
  h.first_size = b[0];
  if (h.first_size < kMinBasic || h.first_size > size) throw ArjError(vol + ": damaged header" + at());
  h.min_ver = b[2];
  h.host_os = b[3];
  h.flags = b[4];
  h.method = b[5];  // the main header's security version
  h.file_type = b[6];
  h.datetime = le32(b + 8);
  h.csize = le32(b + 12);
  h.osize = le32(b + 16);
  h.crc = le32(b + 20);
  h.fspos = le16(b + 24);
  h.mode = le16(b + 26);
  if (h.first_size >= 34) {
    h.has_ext_pos = true;
    h.ext_pos = le32(b + 30);
  }
  // The name, then the comment, each NUL-terminated inside the basic header.
  size_t p = h.first_size;
  const uint8_t* nul = std::find(b + p, b + size, uint8_t(0));
  if (nul == b + size) throw ArjError(vol + ": damaged header" + at() + " (unterminated name)");
  h.raw_name.assign(reinterpret_cast<const char*>(b + p), size_t(nul - (b + p)));
  p = size_t(nul - b) + 1;
  if (std::find(b + p, b + size, uint8_t(0)) == b + size)
    throw ArjError(vol + ": damaged header" + at() + " (unterminated comment)");
  h.name = oem437_to_utf8(h.raw_name);
  // Extended headers: a WORD size, the data and its CRC-32; size 0 ends them.
  size_t q = pos + 4 + size + 4;
  for (int n = 0;; n++) {
    if (d.size() - q < 2) throw ArjError(vol + ": the archive is truncated");
    const size_t esz = le16(&d[q]);
    q += 2;
    if (esz == 0) break;
    if (n == kMaxExtHeaders) throw ArjError(vol + ": damaged header" + at() + " (too many extended headers)");
    if (d.size() - q < esz + 4) throw ArjError(vol + ": the archive is truncated");
    if (crc32_of(&d[q], esz) != le32(&d[q + esz]))
      throw ArjError(vol + ": extended header CRC-32 mismatch" + at());
    q += esz + 4;
  }
  h.end = q;
  return h;
}

void check_flags(const Header& h, uint8_t allowed, const std::string& what) {
  if (h.flags & kGarbled) throw ArjError(what + ": password-protected (garbled) archives are not supported");
  if (h.flags & ~allowed) throw ArjError(what + ": unsupported ARJ flags " + hex2(h.flags));
}

void check_version(const Header& h, const std::string& what) {
  if (h.min_ver > kMaxMinVersion)
    throw ArjError(what + ": needs ARJ version " + std::to_string(h.min_ver) + " to extract, which is not supported");
}

Header read_main(std::span<const uint8_t> d, const std::string& vol) {
  Header m = read_header(d, 0, true, vol);
  if (m.file_type != 2) throw ArjError(vol + ": not an ARJ archive (the first header is not a main header)");
  check_version(m, vol);
  check_flags(m, kVolume | kPathSym | kBackup, vol);
  return m;
}

// A local header's own rules; its data must lie inside the volume.
void check_local(const Header& h, std::span<const uint8_t> d, const std::string& vol) {
  const std::string what = vol + "!" + h.name;
  check_version(h, what);
  check_flags(h, kVolume | kExtFile | kPathSym | kBackup, what);
  if ((h.flags & kExtFile) && !h.has_ext_pos) throw ArjError(what + ": continued, but its header has no file position");
  if (!(h.flags & kExtFile) && h.has_ext_pos && h.ext_pos != 0)
    throw ArjError(what + ": a file position without the continuation flag");
  // 0 binary and 1 7-bit text are both copied as stored (the CRC covers the
  // stored bytes); 2 is a comment header, 3 a directory, 4 a volume label.
  if (h.file_type > 1) throw ArjError(what + ": not a file (type " + std::to_string(h.file_type) + ")");
  if (h.method > 4) throw ArjError(what + ": compression method " + std::to_string(h.method) + " is not supported");
  if (h.method == 0 && h.csize != h.osize) throw ArjError(what + ": stored, but its sizes differ");
  if (h.name.find_first_of("/\\:") != std::string::npos)
    throw ArjError(vol + ": member \"" + h.name + "\" is not a bare file name");
  try {
    check_component(h.name, vol);
  } catch (const ImportError& e) {
    throw ArjError(e.what());
  }
  if (h.fspos > h.raw_name.size()) throw ArjError(what + ": damaged header (file name position)");
  if (h.end > d.size() || d.size() - h.end < h.csize) throw ArjError(vol + ": the archive is truncated");
}

struct Scanned {
  Header main;
  std::vector<Header> locals;
};

// Every header of one volume: the main header at byte 0, each local header
// right after the previous one's data, up to the end marker (anything after
// it is never looked at).
Scanned scan_volume(const ArjVolume& v) {
  if (!v.data) throw ArjError(v.name + ": no data");
  std::span<const uint8_t> d(*v.data);
  Scanned s;
  s.main = read_main(d, v.name);
  size_t pos = s.main.end;
  for (;;) {
    Header h = read_header(d, pos, false, v.name);
    if (h.end_marker) break;
    check_local(h, d, v.name);
    pos = h.end + h.csize;
    s.locals.push_back(std::move(h));
    if (s.locals.size() > kMaxMembers) throw ArjError(v.name + ": more than " + std::to_string(kMaxMembers) + " members");
  }
  return s;
}

// ---- decoding (importer_design.md §2.4) ----------------------------------------------------
//
// A modified version of UNARJ's DECODE.C, with the bit reader of its
// UNARJ.C (UNARJ 2.41):
//
//   DECODE.C, UNARJ, R JUNG, 02/17/93
//   Copyright (c) 1991 by Robert K Jung.  All rights reserved.
//   This code may be freely used in programs that are NOT ARJ archivers
//   (both compress and extract ARJ archives).
//
// DECODE.C's make_table, read_pt_len, read_c_len, decode_c, decode_p and
// decode (methods 1-3) and decode_f with decode_len and decode_ptr (method
// 4), and UNARJ.C's fillbuf/getbits (Copyright (c) 1991-93 Robert K Jung,
// the same terms), ported to C++ and modified: every table size, code
// length, zero run, constant symbol, match distance and output size is
// checked before it is used (UNARJ trusts them), the reader goes at most 2
// bytes past the segment, method 4 reads through the same reader, and the
// output goes to a sink in chunks. UNARJ's LZH routines derive from Haruhiko
// Okumura's ar002. The port went through our research reference,
// research/win/pkg/swse/arj.py. arj.h has UNARJ's terms.

// UNARJ's bit reader: MSB first, a 16-bit look-ahead window. Past the end of
// the segment it supplies zero bytes, at most 2: the look-ahead of a stream
// that ends exactly where its last symbol does needs exactly 2 (bytes fetched
// = ceil((E + 16) / 8) for E bits used), so a third means the data was cut.
class BitReader {
 public:
  explicit BitReader(std::span<const uint8_t> in) : in_(in) { fill(16); }

  uint16_t peek() const { return bitbuf_; }  // the next 16 bits

  // Consumes n (0..16) bits.
  void fill(int n) {
    bitbuf_ = uint16_t(uint32_t(bitbuf_) << n);
    while (n > bitcount_) {
      n -= bitcount_;
      bitbuf_ = uint16_t(bitbuf_ | (uint32_t(subbitbuf_) << n));
      subbitbuf_ = next_byte();
      bitcount_ = 8;
    }
    bitcount_ -= n;
    bitbuf_ = uint16_t(bitbuf_ | (uint32_t(subbitbuf_) >> bitcount_));
  }

  // The next n (0..16) bits as a number.
  uint32_t get(int n) {
    uint32_t x = n ? uint32_t(bitbuf_) >> (16 - n) : 0;
    fill(n);
    return x;
  }

 private:
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  int past_ = 0;
  uint16_t bitbuf_ = 0;
  uint8_t subbitbuf_ = 0;
  int bitcount_ = 0;

  uint8_t next_byte() {
    if (pos_ < in_.size()) return in_[pos_++];
    if (++past_ > 2) throw ArjError("the compressed data ends early");
    return 0;
  }
};

constexpr uint32_t kDicSize = 26624;  // UNARJ's DDICSIZ: the farthest a match may reach
constexpr int kThreshold = 3, kMaxMatch = 256;

// The output: a 64 KiB ring (the 26,624-byte window plus up to 32 KiB not yet
// handed on), emitted in chunks of at most 32 KiB. No stream may produce a
// byte past `osize` or refer to a byte before its start.
class Output {
 public:
  Output(uint32_t osize, const Sink& sink) : osize_(osize), sink_(sink), ring_(kRing) {}

  bool full() const { return produced_ == osize_; }

  void literal(uint8_t b) {
    if (produced_ == osize_) throw ArjError("more data than the recorded size");
    put(b);
  }

  void match(uint32_t dist, uint32_t len) {
    if (dist > produced_) throw ArjError("a match reaches before the start of the data");
    if (dist > kDicSize) throw ArjError("a match reaches beyond the 26624-byte window");
    if (len > osize_ - produced_) throw ArjError("more data than the recorded size");
    const uint64_t from = produced_ - dist;
    for (uint32_t i = 0; i < len; i++) put(ring_[size_t((from + i) & kMask)]);
  }

  void flush() {
    while (emitted_ < produced_) {
      const size_t at = size_t(emitted_ & kMask);
      size_t n = size_t(std::min<uint64_t>(produced_ - emitted_, kRing - at));
      n = std::min(n, kChunk);
      sink_(&ring_[at], n);
      emitted_ += n;
    }
  }

 private:
  static constexpr size_t kRing = 65536, kMask = kRing - 1, kChunk = 32768;
  const uint64_t osize_;
  const Sink& sink_;
  std::vector<uint8_t> ring_;
  uint64_t produced_ = 0, emitted_ = 0;

  void put(uint8_t b) {
    ring_[size_t(produced_ & kMask)] = b;
    if (++produced_ - emitted_ >= kChunk) flush();
  }
};

// Methods 1-3 (one decoder: the method number only records the encoder's
// effort). UNARJ's decode.c with the bounds it lacks: every table size, code
// length, zero run and constant symbol is checked before it is used, and a
// Huffman table must be a complete code (its Kraft sum exactly 1), so no
// table, tree or array is ever written or read out of range.
class LzhDecoder {
 public:
  LzhDecoder(std::span<const uint8_t> in, uint32_t osize, const Sink& sink) : bits_(in), out_(osize, sink) {}

  void run() {
    while (!out_.full()) {
      const uint32_t c = decode_c();
      if (c <= 255) {
        out_.literal(uint8_t(c));
      } else {
        const uint32_t len = c - (256 - kThreshold);
        out_.match(decode_p() + 1, len);
      }
    }
    // The real streams end with a block (all 37 segments on the disc): one
    // that still holds symbols at the recorded size is refused. Whatever
    // follows the block (more blocks, trailing bytes) is never read, as in
    // UNARJ; the CRC-32 covers the bytes produced.
    if (blocksize_ != 0) throw ArjError("the compressed data goes on past the recorded size");
    out_.flush();
  }

 private:
  static constexpr int kNC = 255 + kMaxMatch + 2 - kThreshold;  // 510 literal and length symbols
  static constexpr int kNP = 17, kNT = 19, kCBit = 9, kPBit = 5, kTBit = 5;
  static constexpr int kTree = 2 * kNC - 1;

  BitReader bits_;
  Output out_;
  uint32_t blocksize_ = 0;
  std::array<uint8_t, kNC> c_len_{};
  std::array<uint8_t, kNT> pt_len_{};  // the NT table while c_len is read, then the NP table
  std::array<uint16_t, 4096> c_table_{};
  std::array<uint16_t, 256> pt_table_{};
  std::array<uint16_t, kTree> left_{}, right_{};

  [[noreturn]] static void bad_table(const char* why) { throw ArjError(std::string("bad Huffman table (") + why + ")"); }

  // The lookup table (and, for codes longer than `tablebits`, the tree) of a
  // canonical code with these lengths (each 0..16).
  void make_table(int nchar, const uint8_t* bitlen, int tablebits, uint16_t* table) {
    uint32_t count[17] = {}, weight[17] = {}, start[18] = {};
    for (int i = 0; i < nchar; i++) count[bitlen[i]]++;
    // Unmasked: UNARJ's 16-bit sum lets an over-subscribed code through.
    for (int i = 1; i <= 16; i++) start[i + 1] = start[i] + (count[i] << (16 - i));
    if (start[17] != (1u << 16)) bad_table("not a complete code");
    const int jutbits = 16 - tablebits;
    const uint32_t size = 1u << tablebits;
    for (int i = 1; i <= tablebits; i++) {
      start[i] >>= jutbits;
      weight[i] = 1u << (tablebits - i);
    }
    for (int i = tablebits + 1; i <= 16; i++) weight[i] = 1u << (16 - i);
    for (uint32_t i = start[tablebits + 1] >> jutbits; i < size; i++) table[i] = 0;
    uint32_t avail = uint32_t(nchar);
    const uint32_t mask = 1u << (15 - tablebits);
    for (int ch = 0; ch < nchar; ch++) {
      const int len = bitlen[ch];
      if (!len) continue;
      uint32_t k = start[len];
      const uint32_t next = k + weight[len];
      if (len <= tablebits) {
        if (next > size) bad_table("code out of range");
        for (uint32_t i = k; i < next; i++) table[i] = uint16_t(ch);
      } else {
        if ((k >> jutbits) >= size) bad_table("code out of range");
        uint16_t* p = &table[k >> jutbits];
        for (int i = len - tablebits; i; i--) {
          if (*p == 0) {
            if (avail >= uint32_t(kTree)) bad_table("too many tree nodes");
            right_[avail] = left_[avail] = 0;
            *p = uint16_t(avail++);
          }
          p = (k & mask) ? &right_[*p] : &left_[*p];
          k <<= 1;
        }
        *p = uint16_t(ch);
      }
      start[len] = next;
    }
  }

  // The code lengths of the NT table (which codes c_len) or the NP table.
  void read_pt_len(int nn, int nbit, int i_special) {
    const int n = int(bits_.get(nbit));
    if (n == 0) {
      const uint32_t c = bits_.get(nbit);
      if (c >= uint32_t(nn)) bad_table("constant symbol out of range");
      std::fill(pt_len_.begin(), pt_len_.begin() + nn, uint8_t(0));
      pt_table_.fill(uint16_t(c));
      return;
    }
    if (n > nn) bad_table("too many code lengths");
    int i = 0;
    while (i < n) {
      int c = bits_.peek() >> 13;
      if (c == 7) {
        for (uint32_t mask = 1u << 12; mask & bits_.peek(); mask >>= 1)
          if (++c > 16) bad_table("a code length over 16");
      }
      bits_.fill(c < 7 ? 3 : c - 3);
      pt_len_[i++] = uint8_t(c);
      if (i == i_special) {
        int z = int(bits_.get(2));
        if (i + z > nn) bad_table("a run of zero lengths passes the table");
        while (z-- > 0) pt_len_[i++] = 0;
      }
    }
    while (i < nn) pt_len_[i++] = 0;
    make_table(nn, pt_len_.data(), 8, pt_table_.data());
  }

  // The code lengths of the literal/length table, coded with the NT table.
  void read_c_len() {
    const int n = int(bits_.get(kCBit));
    if (n == 0) {
      const uint32_t c = bits_.get(kCBit);
      // A 510 or 511 would be a 257- or 258-byte match.
      if (c >= uint32_t(kNC)) bad_table("constant symbol out of range");
      c_len_.fill(0);
      c_table_.fill(uint16_t(c));
      return;
    }
    if (n > kNC) bad_table("too many code lengths");
    int i = 0;
    while (i < n) {
      uint32_t c = pt_table_[bits_.peek() >> 8];
      for (uint32_t mask = 1u << 7; c >= uint32_t(kNT); mask >>= 1) c = (bits_.peek() & mask) ? right_[c] : left_[c];
      bits_.fill(pt_len_[c]);
      if (c <= 2) {
        int z = c == 0 ? 1 : c == 1 ? int(bits_.get(4)) + 3 : int(bits_.get(kCBit)) + 20;
        if (i + z > kNC) bad_table("a run of zero lengths passes the table");
        while (z-- > 0) c_len_[i++] = 0;
      } else {
        c_len_[i++] = uint8_t(c - 2);
      }
    }
    while (i < kNC) c_len_[i++] = 0;
    make_table(kNC, c_len_.data(), 12, c_table_.data());
  }

  uint32_t decode_c() {
    if (blocksize_ == 0) {
      blocksize_ = bits_.get(16);
      // UNARJ's unsigned counter would take 0 as 65,536 symbols.
      if (blocksize_ == 0) throw ArjError("an empty block in the compressed data");
      read_pt_len(kNT, kTBit, 3);
      read_c_len();
      read_pt_len(kNP, kPBit, -1);
    }
    blocksize_--;
    uint32_t j = c_table_[bits_.peek() >> 4];
    for (uint32_t mask = 1u << 3; j >= uint32_t(kNC); mask >>= 1) j = (bits_.peek() & mask) ? right_[j] : left_[j];
    bits_.fill(c_len_[j]);
    return j;
  }

  // The match distance minus one.
  uint32_t decode_p() {
    uint32_t j = pt_table_[bits_.peek() >> 8];
    for (uint32_t mask = 1u << 7; j >= uint32_t(kNP); mask >>= 1) j = (bits_.peek() & mask) ? right_[j] : left_[j];
    bits_.fill(pt_len_[j]);
    if (j != 0) {
      j--;
      j = (1u << j) + bits_.get(int(j));
    }
    return j;
  }
};

}  // namespace

namespace arj_detail {

void decode_lzh(std::span<const uint8_t> in, uint32_t osize, const Sink& sink) {
  auto d = std::make_unique<LzhDecoder>(in, osize, sink);
  d->run();
}

// Method 4: a unary-prefixed length (0 = a literal byte follows, else a match
// of length + 2) and a unary-prefixed pointer (distance - 1, 9..13 bits).
// It has no blocks: it stops at the recorded size and reads nothing after
// it, as UNARJ's decode_f.
void decode_fastest(std::span<const uint8_t> in, uint32_t osize, const Sink& sink) {
  BitReader bits(in);
  Output out(osize, sink);
  while (!out.full()) {
    uint32_t plus = 0, pwr = 1;
    int width = 0;
    for (; width < 7; width++) {
      if (!bits.get(1)) break;
      plus += pwr;
      pwr <<= 1;
    }
    const uint32_t c = (width ? bits.get(width) : 0) + plus;
    if (c == 0) {
      out.literal(uint8_t(bits.get(8)));
      continue;
    }
    const uint32_t len = c - 1 + kThreshold;
    plus = 0;
    pwr = 1u << 9;
    width = 9;
    for (; width < 13; width++) {
      if (!bits.get(1)) break;
      plus += pwr;
      pwr <<= 1;
    }
    out.match(bits.get(width) + plus + 1, len);
  }
  out.flush();
}

}  // namespace arj_detail

// ---- the archive ----------------------------------------------------------------------------

ArjArchive::ArjArchive(std::vector<ArjVolume> volumes) : volumes_(std::move(volumes)) {
  if (volumes_.empty()) throw ArjError("no ARJ volumes");
  std::set<std::string> names;
  // The member whose last segment so far continues on the next volume.
  std::optional<size_t> open;
  for (size_t v = 0; v < volumes_.size(); v++) {
    const std::string& vol = volumes_[v].name;
    Scanned s = scan_volume(volumes_[v]);
    const bool last = v + 1 == volumes_.size();
    if (!last && !(s.main.flags & kVolume)) throw ArjError(vol + " does not continue on another volume");
    // A member cut at the end of the previous volume goes on in this one's
    // first member, which says where its bytes resume.
    if (open && (s.locals.empty() || !(s.locals.front().flags & kExtFile)))
      throw ArjError(volumes_[v - 1].name + "!" + members_[*open].name + " continues, but " + vol +
                     " does not continue it");
    for (size_t i = 0; i < s.locals.size(); i++) {
      const Header& h = s.locals[i];
      const std::string what = vol + "!" + h.name;
      ArjSegment seg;
      seg.volume = v;
      seg.data_offset = h.end;
      seg.csize = h.csize;
      seg.osize = h.osize;
      seg.crc = h.crc;
      seg.method = h.method;
      seg.position = h.has_ext_pos ? h.ext_pos : 0;
      if (h.flags & kExtFile) {
        if (i != 0 || !open) throw ArjError(what + ": a continuation without its start");
        ArjMember& m = members_[*open];
        if (!iequals(m.name, h.name))
          throw ArjError(volumes_[v - 1].name + "!" + m.name + " continues, but " + vol + " does not continue it");
        if (seg.position != m.size)
          throw ArjError(what + ": continues at byte " + std::to_string(seg.position) + ", but " +
                         std::to_string(m.size) + " bytes came before it (wrong position)");
        m.segments.push_back(seg);
        m.size += seg.osize;
        m.volumes += "+" + vol;
      } else {
        ArjMember m;
        m.name = h.name;
        m.size = seg.osize;
        m.dos_datetime = h.datetime;
        m.host_os = h.host_os;
        m.file_type = h.file_type;
        m.access_mode = h.mode;
        m.segments.push_back(seg);
        m.volumes = vol;
        // One name, as Windows compares them (code page 437's letters too):
        // both would be planned, and the second could not be created.
        if (!names.insert(name_key(m.name)).second) throw ArjError(vol + ": two members are named " + m.name);
        members_.push_back(std::move(m));
        if (members_.size() > kMaxMembers)
          throw ArjError(vol + ": more than " + std::to_string(kMaxMembers) + " members");
      }
      const size_t index = (h.flags & kExtFile) ? *open : members_.size() - 1;
      if (h.flags & kVolume) {
        if (i + 1 != s.locals.size())
          throw ArjError(what + ": continues on the next volume, but is not the last member of " + vol);
        open = index;
      } else {
        open.reset();
      }
    }
    if (last && ((s.main.flags & kVolume) || open)) {
      auto next = arj_next_volume(vol);
      throw ArjError((open ? vol + "!" + members_[*open].name : vol) + ": the archive continues on " +
                     (next ? *next : std::string("another volume")));
    }
  }
}

const ArjMember* ArjArchive::find(std::string_view name) const {
  for (const ArjMember& m : members_)
    if (iequals(m.name, name)) return &m;
  return nullptr;
}

void ArjArchive::extract(const ArjMember& m, const Sink& sink) const {
  constexpr size_t kChunk = 64 * 1024;
  uint64_t total = 0;
  for (const ArjSegment& seg : m.segments) {
    if (seg.volume >= volumes_.size()) throw ArjError(m.name + ": no such volume");
    const ArjVolume& vol = volumes_[seg.volume];
    const std::string what = vol.name + "!" + m.name;
    const std::vector<uint8_t>& d = *vol.data;
    if (seg.data_offset > d.size() || d.size() - seg.data_offset < seg.csize)
      throw ArjError(what + ": the archive is truncated");
    std::span<const uint8_t> in(d.data() + seg.data_offset, seg.csize);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t got = 0;
    // Messages from here and from the decoders are completed with `what`
    // below; whatever the caller's sink throws passes through unchanged.
    Sink emit = [&](const uint8_t* p, size_t n) {
      if (n > seg.osize - got) throw ArjError("more data than the recorded size");
      got += n;
      crc = crc32(crc, p, uInt(n));
      sink(p, n);
    };
    try {
      switch (seg.method) {
        case 0:
          for (size_t done = 0; done < in.size();) {
            const size_t n = std::min(kChunk, in.size() - done);
            emit(in.data() + done, n);
            done += n;
          }
          break;
        case 1:
        case 2:
        case 3: arj_detail::decode_lzh(in, seg.osize, emit); break;
        case 4: arj_detail::decode_fastest(in, seg.osize, emit); break;
        default: throw ArjError("compression method " + std::to_string(seg.method) + " is not supported");
      }
    } catch (const ArjError& e) {
      throw ArjError(what + ": " + e.what());
    }
    if (got != seg.osize)
      throw ArjError(what + ": " + std::to_string(got) + " bytes, the archive records " + std::to_string(seg.osize));
    if (uint32_t(crc) != seg.crc) throw ArjError(what + ": CRC-32 mismatch");
    total += got;
  }
  if (total != m.size)
    throw ArjError(m.volumes + "!" + m.name + ": " + std::to_string(total) + " bytes, the archive records " +
                   std::to_string(m.size));
}

bool arj_continues(std::span<const uint8_t> data, std::string_view volume_name) {
  return (read_main(data, std::string(volume_name)).flags & kVolume) != 0;
}

std::optional<std::string> arj_next_volume(std::string_view name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || name.size() - dot != 4) return std::nullopt;
  const char a = name[dot + 1], x = name[dot + 2], y = name[dot + 3];
  if (a != 'A' && a != 'a') return std::nullopt;
  std::string out(name.substr(0, dot + 2));  // through the 'A', its case kept
  if ((x == 'R' || x == 'r') && (y == 'J' || y == 'j')) return out + "01";
  if (x < '0' || x > '9' || y < '0' || y > '9') return std::nullopt;
  const int n = (x - '0') * 10 + (y - '0');
  if (n < 1 || n >= 99) return std::nullopt;  // ARJ names .A01 to .A99
  out += char('0' + (n + 1) / 10);
  out += char('0' + (n + 1) % 10);
  return out;
}

}  // namespace adw::import
