// The profile store (INTERACTION.md §7.3, §10): Get/WritePrivateProfileString,
// Get/WriteProfileString, GetPrivateProfileInt, Win32 and Win16 alike, over the
// Vfs.
//
//   read   the lane's seed entries ⊕ the file (the file wins per key; the file
//          is the overlay's upper copy if any, else the lower one). Sections
//          and keys enumerate in the file's order, then the seeds the file
//          does not have.
//   write  read-modify-write of the upper file only (copied up from the lower
//          file first), under the state mutex, through the Vfs's atomic
//          replace. The rest of the file (comments, other sections, spacing)
//          is kept line for line; lines end in CRLF. Seeds are never written
//          out. Deleting a key (or section) removes it from the file; a seed
//          of the same name then shows again, and deleting a key that only a
//          seed has is not persisted (logged).
//
// Parsing follows Windows: a "[section]" line opens a section; "key=value"
// lines (blanks around both trimmed) belong to the last section; lines before
// the first section, comments (';') and lines without '=' are kept but never
// match. Names compare case-insensitively; the first occurrence wins.
//
// Parses are cached per file and re-validated before every read by the
// file's layer, size, write time and (memory upper) version, so another host
// writing the same file is seen at the next read.
//
// Paths are guest paths (the caller turns a bare "X.INI" into
// "C:\WINDOWS\X.INI"); they are normalized with Vfs::full_path.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "win32/vfs.hh"

namespace adw::win32 {

class IniStore {
 public:
  explicit IniStore(Vfs& vfs);

  void add_seed(std::string_view guest_path, std::string_view section, std::string_view key, std::string_view value);
  std::optional<std::string> get(std::string_view guest_path, std::string_view section, std::string_view key);
  std::vector<std::string> sections(std::string_view guest_path);
  std::vector<std::string> keys(std::string_view guest_path, std::string_view section);
  // key nullopt = delete section; value nullopt = delete key. False when the
  // file cannot be written (outside every writable mount, a read-only layer).
  bool set(std::string_view guest_path, std::string_view section, std::optional<std::string_view> key,
           std::optional<std::string_view> value);

  // The parsed form, for tests and for GetPrivateProfileSection-style readers.
  struct Line {
    enum class Kind : uint8_t { other, section, key } kind = Kind::other;
    std::string text;         // the line as stored (no line end)
    std::string name, value;  // section: name; key: key and value (trimmed)
  };
  static std::vector<Line> parse(std::string_view text);
  static std::string serialize(const std::vector<Line>& lines);

 private:
  struct Seed {
    std::string section, key, value;
  };
  struct Cached {
    Vfs::Stat stamp;
    bool present = false;
    std::vector<Line> lines;
  };
  const std::vector<Line>& load(const std::string& full);
  std::string key_of(std::string_view guest_path) const;

  Vfs& vfs_;
  std::map<std::string, std::vector<Seed>> seeds_;  // upper-case full path
  std::map<std::string, Cached> cache_;
};

}  // namespace adw::win32
