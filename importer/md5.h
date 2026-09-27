// MD5 through Windows CNG (bcrypt.dll).
//
// MD5 is only an integrity fingerprint here — it is the hash the Internet
// Archive publishes for its downloads — so the OS implementation is the
// right call: no vendored crypto, and CNG's MD5 streams at disk speed.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "status.h"

namespace adw::import {

class Md5 {
 public:
  Md5();  // throws std::runtime_error if CNG refuses (never on a stock Windows)
  ~Md5();
  Md5(const Md5&) = delete;
  Md5& operator=(const Md5&) = delete;

  void update(const void* data, size_t size);
  // Returns the digest and resets the object so it can hash a new stream.
  std::array<uint8_t, 16> finish();
  std::string finish_hex();

 private:
  void* hash_ = nullptr;  // BCRYPT_HASH_HANDLE
};

std::string to_hex(const uint8_t* data, size_t size);
std::string md5_hex(const void* data, size_t size);

// Streams the file through MD5 in large sequential reads. `progress`, when
// set, is told the running byte count and may return false to cancel (the
// function then throws ImportError(Status::cancelled)); I/O failures throw
// ImportError(Status::error).
std::string md5_file_hex(const std::filesystem::path& path,
                         const std::function<bool(uint64_t done, uint64_t total)>& progress = {});

}  // namespace adw::import
