// KWAJ: the second format of Microsoft's COMPRESS.EXE, which the Microsoft
// Setup Toolkit 2.0 expanded on the way (Star Trek: The Screen Saver's "*.XX_"
// files; research/win/pkg/startrek/survey/expand_kwaj.md). Only what that
// release uses is read, strictly, and everything else is refused rather than
// guessed at: method 3 (LZ + Huffman) with header flags 0, so the compressed
// data starts right after the 14-byte header. Written from the public format
// description (the research reference research/win/pkg/startrek/tools/kwaj.py
// follows libmspack's documentation of it); no third-party code.
//
// The header: magic "KWAJ" 88 F0 27 D1, the method, the data offset and the
// header flags (little-endian words). Method 3's stream is read MSB first:
// six 4-bit table types (0-3; the sixth must be 0), then five canonical
// Huffman codes — MATCHLEN (16 symbols), MATCHLEN2 (16), LITLEN (32), OFFSET
// (64), LITERAL (256) — whose lengths (0..16) are fixed (type 0: 4/5/6/8
// bits), delta-coded (1: '0' same, '10' +1, '11' + 4 bits; 2: a 2-bit
// selector, 3 = 4 bits, else previous + selector - 1) or plain (3: 4 bits
// each). Every code must be complete (Kraft sum exactly 1). Tokens follow, in
// a 4096-byte window that starts filled with spaces: a MATCHLEN symbol (a
// MATCHLEN2 symbol right after a literal run shorter than 32) s > 0 is a match
// of s + 2 bytes at distance (OFFSET symbol << 6 | 6 raw bits, 0 meaning
// 4096), copied byte by byte (it may overlap itself and read the initial
// spaces); s = 0 is a run of LITLEN + 1 LITERAL symbols.
//
// KWAJ records no length and has no checksum, and the stream simply ends. The
// end rule: a token is emitted only once it is complete; when a read needs a
// bit past the end, the file ends cleanly if the unfinished token began fewer
// than 8 bits before the end (the encoder pads its last byte with 1-bits, which
// in 6 of the release's 59 files read as the start of a token), and that token
// produces nothing; otherwise the data ends inside a token (an error). A
// damaged file from a source without a known image is caught only by the
// manifest (a verify failure, 3), as with SZDD; the output is bounded by the
// caller's max_size (at most 17 bytes a token, so 17x the input anyway).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace adw::import {

class KwajError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
// The output would pass the caller's max_size (the file may be sound: the
// caller's bound says why it is refused).
class KwajTooLarge : public KwajError {
 public:
  using KwajError::KwajError;
};

struct KwajHeader {
  uint16_t method = 0, data_offset = 0, flags = 0;
};

// The header. Throws KwajError for another magic (an SZDD file says so), a
// file shorter than the header, a method other than 3, any header flag (the
// optional header fields) or a data offset other than 14; `name` is for
// messages.
KwajHeader kwaj_header(std::span<const uint8_t> file, std::string_view name);

// Streams the expanded bytes to `sink` in chunks of at most 64 KiB and returns
// how many there were. Throws KwajError for a header kwaj_header refuses, a
// table type or code length out of range, an incomplete or over-subscribed
// code, or data that ends inside the tables or inside a token (see above);
// KwajTooLarge for output that would pass `max_size` (checked before each
// token's bytes).
uint64_t kwaj_expand(std::span<const uint8_t> file, std::string_view name, uint64_t max_size,
                     const std::function<void(const uint8_t*, size_t)>& sink);

namespace kwaj_detail {
// Method 3's stream alone (what follows the header), for the tests.
uint64_t decode_lzh(std::span<const uint8_t> stream, std::string_view name, uint64_t max_size,
                    const std::function<void(const uint8_t*, size_t)>& sink);
}  // namespace kwaj_detail

}  // namespace adw::import
