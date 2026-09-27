// Read-only ISO-9660 reader (level 1/2 primary volume + Joliet SVD).
//
// Enough of ECMA-119 to walk the After Dark Deluxe hybrid CD and any sane rip
// of it: volume descriptor scan, both-endian fields (with a fallback when a
// mastering tool filled only one half), multi-extent files, extended
// attribute records, associated (resource-fork) files skipped, and raw
// 2352-byte-sector images (MODE1 / MODE2 form 1) as well as cooked 2048 ones.
// The image is read through a small window cache and chunked reads — never
// loaded whole — so a 400 MB image costs a few hundred KB of memory.
//
// Names: when the image carries a Joliet tree it is the one walked (its names
// are what Windows shows for a mounted disc), and each Joliet entry is paired
// with its primary-volume twin so `short_name` still reports the 8.3 name the
// installer and the catalog use.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace adw::import {

class IsoError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct IsoExtent {
  uint32_t lba = 0;   // logical block where the data starts (extended attribute record skipped)
  uint32_t size = 0;  // bytes
};

// Directory-record recording time (ECMA-119 9.1.5): local time plus the
// offset from GMT in 15-minute units.
struct IsoTime {
  bool valid = false;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int gmt_offset_15min = 0;
};

struct IsoEntry {
  std::string name;        // preferred name (Joliet when present), UTF-8, ";1" and trailing '.' removed
  std::string short_name;  // primary-volume (ISO-9660) name; empty when it could not be paired
  bool is_dir = false;
  uint64_t size = 0;       // sum of the extents
  std::vector<IsoExtent> extents;  // in file order; >1 only for multi-extent files
  IsoTime mtime;
  // Bookkeeping so list() knows which tree a directory came from and, for a
  // Joliet directory, which primary-volume directory is its twin.
  bool in_joliet = false;
  IsoExtent primary_twin;  // size 0 = unknown
};

class IsoImage {
 public:
  // Throws IsoError when the file is not a readable ISO-9660 image. `path`
  // may also be a CD volume (\\.\E:), read raw: then every read is
  // sector-aligned through an aligned bounce buffer (volume handles are
  // non-cached) and the size comes from the device or the PVD.
  // `aligned_reads` forces that read path on a plain file (tests).
  explicit IsoImage(const std::filesystem::path& path, bool aligned_reads = false);
  ~IsoImage();
  IsoImage(const IsoImage&) = delete;
  IsoImage& operator=(const IsoImage&) = delete;

  bool joliet() const;            // a Joliet SVD was found (and is the tree being walked)
  bool raw_sectors() const;       // 2352-byte sectors
  uint32_t block_size() const;    // logical block size (2048 on every real disc)
  uint64_t file_size() const;
  const std::string& volume_id() const;

  const IsoEntry& root() const;
  // Children of a directory in on-disc order ("." and ".." omitted).
  std::vector<IsoEntry> list(const IsoEntry& dir) const;
  // Case-insensitive lookup of a '/'- or '\'-separated path; each component
  // may match either the entry's name or its short name.
  std::optional<IsoEntry> find(std::string_view path) const;

  // Streams a file's bytes in order, in chunks of at most 1 MiB. Not
  // thread-safe (the image shares one read cache).
  void read(const IsoEntry& file, const std::function<void(const uint8_t*, size_t)>& sink) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace adw::import
