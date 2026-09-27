// adw::loader — the pieces shared by the PE32 and NE loaders: the sink the
// images are placed into, the one error type every parse/load failure throws,
// format detection, resource ids and VERSIONINFO decoding.
//
// The loader is pure parsing and placement (docs/DESIGN.md §3). It
// knows nothing about the CPU or the Windows API: it lays images out through an
// ImageSink and asks resolver callbacks what imported/internal targets are.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adw::loader {

// Every failure caused by the bytes of an image (as opposed to a bug in the
// loader or an exception thrown by a caller's resolver) is a LoaderError, so a
// host can say "this module is damaged" and the fuzz test can tell the two
// apart.
class LoaderError : public std::runtime_error {
public:
  enum class Kind {
    truncated,   // a structure runs past the end of the file or its table
    bad_format,  // wrong magic: not the kind of image the caller asked for
    unsupported, // well-formed, but uses something this loader does not do
    malformed,   // internally inconsistent (bad index, loop, overlap, ...)
    placement,   // cannot be placed where the sink put it (relocs stripped)
  };
  LoaderError(Kind kind, const std::string& what)
      : std::runtime_error(what), kind_(kind) {}
  Kind kind() const { return kind_; }

private:
  Kind kind_;
};

const char* error_kind_name(LoaderError::Kind kind);

// Where images go. The PE loader reserves the whole image once (preferred =
// ImageBase); the NE loader reserves each segment separately with preferred =
// 0 ("anywhere"), since NE segments are addressed through selectors the host
// allocates, not through linear addresses. The loader writes every byte of each
// reservation (zero-filling BSS/min-alloc tails itself), so a sink need not
// hand out zeroed memory.
struct ImageSink {
  virtual ~ImageSink() = default;
  virtual uint32_t reserve(uint32_t preferred, uint32_t size) = 0; // returns actual base (≠ preferred ⇒ relocate)
  virtual void write(uint32_t addr, const void* data, size_t size) = 0;
};

enum class Format { unknown, mz, ne, le, pe32, pe32plus };
const char* format_name(Format f);
// Looks only at the MZ stub and the signature at e_lfanew.
Format detect_format(std::string_view data);
// Reads a whole file; throws std::runtime_error if it cannot be opened.
std::string read_file(const std::string& path);

// A resource type or name: an integer id (MAKEINTRESOURCE) or a string. PE
// string ids are converted from UTF-16 to UTF-8; NE string ids keep the file's
// 8-bit bytes (they are ASCII in practice, e.g. the AD types "RLEP", "PAL").
struct ResId {
  bool is_string = false;
  uint16_t num = 0;
  std::string str;

  static ResId of(uint16_t n) { return ResId{false, n, {}}; }
  static ResId of(std::string_view s) { return ResId{true, 0, std::string(s)}; }
  // Windows compares string ids case-insensitively (the resource compiler
  // uppercases them), so lookups do too; "#123" also matches integer 123, as
  // FindResource does.
  bool matches(const ResId& other) const;
  std::string to_string() const; // 16 / "RLEP"
};

// Integer resource types (winuser.h RT_*), the same numbers in NE and PE.
namespace rt {
constexpr uint16_t cursor = 1, bitmap = 2, icon = 3, menu = 4, dialog = 5,
                   string = 6, fontdir = 7, font = 8, accelerator = 9,
                   rcdata = 10, messagetable = 11, group_cursor = 12,
                   group_icon = 14, nametable = 15 /* Win16 only */,
                   version = 16, dlginclude = 17,
                   plugplay = 19, vxd = 20, anicursor = 21, aniicon = 22,
                   html = 23, manifest = 24;
// "STRING", "VERSION", ... or nullptr for a non-standard integer type.
const char* name(uint16_t type);
} // namespace rt

// VS_VERSIONINFO decoded from either layout: the 32-bit one (PE; UTF-16 keys,
// wType field) or the 16-bit one (NE; 8-bit keys, no wType). Strings come out
// as UTF-8 (NE text is taken as Latin-1).
struct VersionInfo {
  bool has_fixed = false;
  uint32_t signature = 0, struct_version = 0;
  uint32_t file_version_ms = 0, file_version_ls = 0;
  uint32_t product_version_ms = 0, product_version_ls = 0;
  uint32_t file_flags_mask = 0, file_flags = 0, file_os = 0, file_type = 0,
           file_subtype = 0, file_date_ms = 0, file_date_ls = 0;
  struct StringTable {
    std::string key; // lang+codepage as hex, e.g. "040904E4"
    std::vector<std::pair<std::string, std::string>> strings;
  };
  std::vector<StringTable> string_tables;
  std::vector<uint32_t> translations; // LOWORD = language, HIWORD = codepage

  std::string file_version() const;    // "4.0.0.1"
  std::string product_version() const;
  // First value with this key across the string tables, or "".
  std::string string(std::string_view key) const;
};
// wide = true for the PE layout. Throws LoaderError on a malformed block.
VersionInfo parse_version_info(std::string_view data, bool wide);

// Helpers shared with the CLI and the host.
std::string utf16le_to_utf8(std::string_view bytes);
std::string latin1_to_utf8(std::string_view bytes);

} // namespace adw::loader
