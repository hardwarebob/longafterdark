// MD5 (CNG) against the RFC 1321 appendix A.5 test suite, plus streaming and
// file hashing agreeing with one-shot hashing.
#include <algorithm>
#include <cstring>

#include "md5.h"
#include "test_util.h"

using namespace adw::import;

int main(int argc, char** argv) {
  struct Vec {
    const char* in;
    const char* md5;
  } vecs[] = {
      {"", "d41d8cd98f00b204e9800998ecf8427e"},
      {"a", "0cc175b9c0f1b6a831c399e269772661"},
      {"abc", "900150983cd24fb0d6963f7d28e17f72"},
      {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
      {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
      {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
      {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
       "57edf4a22be3c955ac49da2e2107b67a"},
      {"The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6"},
  };
  for (const Vec& v : vecs) CHECK_EQ(md5_hex(v.in, strlen(v.in)), std::string(v.md5));

  // Streaming in uneven slices == one shot; finish() resets for reuse.
  auto data = test::pattern(3 * 1024 * 1024 + 17, 7);
  std::string one = md5_hex(data.data(), data.size());
  Md5 h;
  size_t at = 0, step = 1;
  while (at < data.size()) {
    size_t n = std::min(step, data.size() - at);
    h.update(data.data() + at, n);
    at += n;
    step = step * 3 + 1;
  }
  CHECK_EQ(h.finish_hex(), one);
  h.update("abc", 3);
  CHECK_EQ(h.finish_hex(), std::string("900150983cd24fb0d6963f7d28e17f72"));
  auto raw = Md5().finish();
  CHECK_EQ(to_hex(raw.data(), raw.size()), std::string("d41d8cd98f00b204e9800998ecf8427e"));

  // File hashing (with progress) matches, and a progress veto cancels.
  auto dir = test::scratch(argc, argv, "adw-import-md5");
  auto f = dir / L"blob.bin";
  test::write_bytes(f, data);
  uint64_t last = 0;
  CHECK_EQ(md5_file_hex(f, [&](uint64_t done, uint64_t total) {
             CHECK_EQ(total, uint64_t(data.size()));
             last = done;
             return true;
           }),
           one);
  CHECK_EQ(last, uint64_t(data.size()));
  bool cancelled = false;
  try {
    md5_file_hex(f, [](uint64_t, uint64_t) { return false; });
  } catch (const ImportError& e) {
    cancelled = e.status() == Status::cancelled;
  }
  CHECK(cancelled);
  bool missing = false;
  try {
    md5_file_hex(dir / L"nope.bin");
  } catch (const ImportError& e) {
    missing = e.status() == Status::error;
  }
  CHECK(missing);
  // Its 3 MB blob is not left behind (in %TEMP% when run without a scratch
  // folder; ctest gives it one in the build tree).
  adw::import::remove_tree(dir);
  CHECK(!std::filesystem::exists(dir));
  return test::finish("import.md5");
}
