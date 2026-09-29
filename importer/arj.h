// ARJ 2.x archives — what Presage's installer unpacks for Star Wars Screen
// Entertainment (SWSE1.ARJ; SWSE2.ARJ + .A01..A03, one archive over four
// install disks). The design is research/win/pkg/swse/survey/
// importer_design.md §2.
//
// The method 1-4 decoder and its bit reader (arj.cc) are a modified version
// of UNARJ's DECODE.C (the bit reader: of its UNARJ.C), Copyright (c)
// 1991-93 Robert K. Jung / ARJ Software, ported to C++ with bounds checks
// added — every bound UNARJ lacks, so crafted input is always a clean
// ArjError, never an out-of-bounds access. UNARJ's LZH routines derive from
// Haruhiko Okumura's ar002. UNARJ's terms: its source may be used freely in
// a product that is not an ARJ archiver (one that both compresses and
// extracts ARJ files); a modified version must say so, in the program and
// the source; the author's name stays. So nothing that links this file may
// also compress ARJ — the tests carry fixed vectors, not an encoder (the
// research encoder stays in research/).
//
// The volumes are held in memory (the largest is 1.4 MB, all five 6.1 MB).
// Only what ARJ 2.x writes is accepted: a main header at byte 0, local
// headers one after another, each followed by exactly its data, and an end
// marker; methods 0 (stored), 1-3 (LZH, one decoder) and 4 ("fastest"); bare
// member names; a member that spans volumes as its segments (the last member
// of one volume, VOLUME_FLAG, continued by the first member of the next,
// EXTFILE_FLAG with the byte position it resumes at). Everything else — a
// garbled (password) archive, a directory entry, an unknown flag or method,
// a damaged header — is refused as a damaged or foreign source, never
// guessed at. Extraction streams, checking every segment's size and CRC-32
// (ARJ stores no whole-file CRC).
//
// There is no expansion-ratio bound: a block whose Huffman tables are
// constants costs 0 bits per symbol. As with deflate, what bounds the output
// is the size each header records (a stream may never produce more), the
// staging budget and the manifest sizes (importer.cc).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace adw::import {

class ArjError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ArjVolume {
  std::string name;                                  // "SWSE2.A01" (messages, import.json `from`)
  std::shared_ptr<const std::vector<uint8_t>> data;  // the whole volume
};

// One piece of a member: all of it, or the part stored in one volume.
struct ArjSegment {
  size_t volume = 0;                        // index into the archive's volumes
  size_t data_offset = 0;                   // of its compressed bytes in that volume
  uint32_t csize = 0, osize = 0, crc = 0;   // of this segment
  uint8_t method = 0;                       // 0 stored, 1..3 LZH, 4 fastest
  uint64_t position = 0;                    // where these bytes go in the file (the ext file position)
};

struct ArjMember {
  std::string name;              // bare name as stored (code page 437, as UTF-8)
  uint64_t size = 0;             // the whole file: the sum of its segments' osize
  uint32_t dos_datetime = 0;     // DOS date << 16 | time (local time), of the first segment
  uint8_t host_os = 0, file_type = 0;
  uint16_t access_mode = 0;
  std::vector<ArjSegment> segments;  // in file order; one unless it spans volumes
  std::string volumes;           // "SWSE2.ARJ+SWSE2.A01": the volumes its segments are in
};

class ArjArchive {
 public:
  // The volumes of ONE archive, in order. Parses every header of every volume
  // and joins the members that continue across volumes. Throws ArjError.
  explicit ArjArchive(std::vector<ArjVolume> volumes);

  const std::vector<ArjVolume>& volumes() const { return volumes_; }
  const std::vector<ArjMember>& members() const { return members_; }  // archive order
  const ArjMember* find(std::string_view name) const;                 // case-insensitive

  // Streams the member's bytes to `sink` in chunks of at most 64 KiB,
  // segment after segment, checking each segment's size and CRC-32 and the
  // total. Throws ArjError ("<volume>!<member>: ...").
  void extract(const ArjMember& m, const std::function<void(const uint8_t*, size_t)>& sink) const;

 private:
  std::vector<ArjVolume> volumes_;
  std::vector<ArjMember> members_;
};

// The main header's VOLUME_FLAG: another volume follows this one (the recipe
// collects the volumes by name before building the archive). Throws ArjError
// when `data` does not start with a valid ARJ main header.
bool arj_continues(std::span<const uint8_t> data, std::string_view volume_name);

// ARJ's volume naming: X.ARJ -> X.A01 -> ... -> X.A99; nullopt after .A99
// and for any other extension. Case is kept ("swse2.arj" -> "swse2.a01").
std::optional<std::string> arj_next_volume(std::string_view name);

namespace arj_detail {  // exposed for tests/test_arj.cc

// One segment's compressed bytes to exactly `osize` bytes, in chunks of at
// most 32 KiB. Neither decoder produces a byte past `osize`, and neither
// reads past the last symbol it needs, as UNARJ: when `osize` is reached, a
// method 1-3 block may not still hold symbols (what follows it — further
// blocks, trailing bytes — is never read), and method 4 simply stops.
// Throws ArjError (without a volume or member name: the caller adds it). No
// CRC here: extract checks it over the bytes produced.
void decode_lzh(std::span<const uint8_t> in, uint32_t osize, const std::function<void(const uint8_t*, size_t)>& sink);
void decode_fastest(std::span<const uint8_t> in, uint32_t osize,
                    const std::function<void(const uint8_t*, size_t)>& sink);

}  // namespace arj_detail

}  // namespace adw::import
