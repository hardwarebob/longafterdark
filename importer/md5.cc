#include "md5.h"

#include <windows.h>
#include <bcrypt.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "status.h"
#include "winutil.h"

namespace adw::import {

namespace {

// One algorithm provider for the process: opening it is the expensive part of
// CNG, and a provider handle is safe to share between threads.
BCRYPT_ALG_HANDLE md5_provider() {
  static BCRYPT_ALG_HANDLE alg = [] {
    BCRYPT_ALG_HANDLE h = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&h, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    return BCRYPT_SUCCESS(st) ? h : nullptr;
  }();
  if (!alg) throw std::runtime_error("bcrypt: MD5 provider unavailable");
  return alg;
}

void* new_hash() {
  BCRYPT_HASH_HANDLE h = nullptr;
  // Null object buffer: CNG allocates it (Windows 7+).
  NTSTATUS st = BCryptCreateHash(md5_provider(), &h, nullptr, 0, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(st)) throw std::runtime_error("bcrypt: BCryptCreateHash failed");
  return h;
}

}  // namespace

Md5::Md5() : hash_(new_hash()) {}

Md5::~Md5() {
  if (hash_) BCryptDestroyHash((BCRYPT_HASH_HANDLE)hash_);
}

void Md5::update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  // BCryptHashData takes a ULONG length; feed >4 GiB buffers in slices.
  while (size) {
    ULONG n = size > 0x40000000 ? 0x40000000 : (ULONG)size;
    if (!BCRYPT_SUCCESS(BCryptHashData((BCRYPT_HASH_HANDLE)hash_, (PUCHAR)p, n, 0)))
      throw std::runtime_error("bcrypt: BCryptHashData failed");
    p += n;
    size -= n;
  }
}

std::array<uint8_t, 16> Md5::finish() {
  std::array<uint8_t, 16> out{};
  if (!BCRYPT_SUCCESS(BCryptFinishHash((BCRYPT_HASH_HANDLE)hash_, out.data(), 16, 0)))
    throw std::runtime_error("bcrypt: BCryptFinishHash failed");
  // A finished CNG hash cannot be reused; replace it so finish() leaves the
  // object ready for the next stream.
  BCryptDestroyHash((BCRYPT_HASH_HANDLE)hash_);
  hash_ = nullptr;
  hash_ = new_hash();
  return out;
}

std::string Md5::finish_hex() {
  auto d = finish();
  return to_hex(d.data(), d.size());
}

std::string to_hex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string s(size * 2, '0');
  for (size_t i = 0; i < size; i++) {
    s[2 * i] = digits[data[i] >> 4];
    s[2 * i + 1] = digits[data[i] & 15];
  }
  return s;
}

std::string md5_hex(const void* data, size_t size) {
  Md5 h;
  h.update(data, size);
  return h.finish_hex();
}

std::string md5_file_hex(const std::filesystem::path& path,
                         const std::function<bool(uint64_t, uint64_t)>& progress) {
  Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!f.valid())
    throw ImportError(Status::error, "cannot open " + to_utf8(path.wstring()) + ": " +
                                         win_error_string(GetLastError()));
  LARGE_INTEGER sz{};
  GetFileSizeEx(f.get(), &sz);
  const uint64_t total = (uint64_t)sz.QuadPart;
  Md5 h;
  std::vector<uint8_t> buf(1 << 20);
  uint64_t done = 0;
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(f.get(), buf.data(), (DWORD)buf.size(), &got, nullptr))
      throw ImportError(Status::error, "read failed on " + to_utf8(path.wstring()) + ": " +
                                           win_error_string(GetLastError()));
    if (!got) break;
    h.update(buf.data(), got);
    done += got;
    if (progress && !progress(done, total)) throw ImportError(Status::cancelled, "cancelled");
  }
  return h.finish_hex();
}

}  // namespace adw::import
