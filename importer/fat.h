// Read-only FAT12/FAT16 floppy-image reader (PACKAGES.md §8.2).
//
// The Simpsons Screen Saver shipped on two 1.44 MB floppies; the user's copy
// is both merged by WinImage into one 2.88 MB image. This reads such images
// (and any other FAT12/16 volume image) without mounting them: the BPB from
// the boot sector, the first FAT, the fixed root directory, subdirectories
// through their cluster chains. Directory data is read when a directory is
// listed, file data only when a file is read — a file that is never asked
// for is never touched (PACKAGES.md §4.2 I5).
//
// Validation is strict, because an image is untrusted input: a BPB with a
// zero or impossible field, a missing 55 AA signature, an image shorter than
// its sector count, a FAT32-sized volume, a chain that loops, runs into a
// free, reserved or out-of-range cluster, or ends before the file does —
// each throws FatError.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace adw::import {

class FatError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct FatEntry {
  std::string name;      // 8.3, upper case, "NAME.EXT" (no dot without an extension), UTF-8
  bool is_dir = false;
  uint32_t size = 0;     // bytes (0 for directories)
  uint32_t first_cluster = 0;
  uint16_t dos_time = 0, dos_date = 0;  // last write, local time as DOS stores it
  bool root = false;     // the root directory itself
};

class FatImage {
 public:
  // Throws FatError when the file is not a readable FAT12/16 image.
  explicit FatImage(const std::filesystem::path& path);
  ~FatImage();
  FatImage(const FatImage&) = delete;
  FatImage& operator=(const FatImage&) = delete;

  int fat_bits() const;          // 12 or 16
  uint64_t file_size() const;
  uint32_t bytes_per_sector() const;
  uint32_t sectors_per_cluster() const;
  uint32_t cluster_count() const;
  uint8_t media() const;
  const std::string& volume_label() const;  // from the root directory's label entry ("" when none)

  const FatEntry& root() const;
  // Children of a directory in on-disk order: deleted entries, LFN entries,
  // the volume label, "." and ".." are skipped.
  std::vector<FatEntry> list(const FatEntry& dir) const;
  // Streams a file's bytes in order, in chunks of at most 64 KiB. Throws
  // FatError on a damaged chain.
  void read(const FatEntry& file, const std::function<void(const uint8_t*, size_t)>& sink) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace adw::import
