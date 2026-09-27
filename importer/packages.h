// The package registry (PACKAGES.md §2): every After Dark release the
// importer knows, compiled in. Registry order is the catalog's module order
// and the precedence order for display-name disambiguation (§6); the
// catalog's packages list goes by `released` instead.
//
//   deluxe    After Dark 4.0 Deluxe          tree    -> <win>\FILES\{AD40,CLASSIC,ENGINE,AFI}
//   ad10      After Dark 10th Anniversary    tree    -> <win>\packages\ad10\{AD10TH,ENGINE,AFI}
//   ad32      After Dark 3.2                 ad3zip  -> <win>\packages\ad32\{AD32,ENGINE}
//   tt        Totally Twisted After Dark     ad3zip  -> <win>\packages\tt\{TWISTED,ENGINE}
//   simpsons  The Simpsons Screen Saver      ad3zip  -> <win>\packages\simpsons\{SIMPSONS,ENGINE}
//
// Each entry carries what identifies the release (known image md5s,
// fingerprints), how to extract it (the recipe and its parameters), what an
// import must contain (`required`), the install-time fix-ups, catalog name
// overrides, the manifest (path, size, md5 of every installed file —
// never After Dark bytes) that a folder source is verified against, the
// Internet Archive copies `--download` fetches, and where its box cover
// comes from.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adw::import {

// A file of a verified import: path relative to <assets>\win ('/'-separated:
// "FILES/AD40/ADXPL510.DLL", "packages/ad32/AD32/GUTS.AD"), size and md5.
struct KnownFile {
  const char* path;
  uint64_t size;
  const char* md5;
};

// A disc or floppy image whose md5 names the package (and makes an import
// from it "verified": "image").
struct KnownImage {
  const char* md5;
  uint64_t size;
  const char* medium;     // human-readable
  const char* volume_id;  // "" when the medium has none
};

// An install-time copy (§4.3): `create` is made as a copy of `copy_of`, both
// relative to the package root, when the source matched the release.
struct Fixup {
  const char* create;
  const char* copy_of;
};

// A catalog moduleName replacement (§6), keyed by the module's path relative
// to the package root ("AD10TH/TOAST2K.AD").
struct NameOverride {
  const char* module;
  const char* name;
};

enum class Recipe { tree, ad3zip };

// A copy of the package on the Internet Archive that `adimport --download`
// fetches (verified 2026-09-26: research/win/pkg/sources/sources.json).
// A package lists its copies in the order they are tried; the next one is
// tried when a copy cannot be fetched (network) or is not the published file
// (size or md5). Copies with the same bytes share a file name, so a transfer
// interrupted on one resumes from the next.
struct Download {
  const char* url;           // archive.org/download/… as published, percent-encoded; redirects are followed
  const wchar_t* file_name;  // saved as <downloads dir>\<file_name>
  uint64_t size;
  const char* md5;           // what the Internet Archive publishes for the file; checked before use
  // "image": a disc image, identified by its md5 like one the user gives
  // (verified: image). "zip": a ZIP of the install files (no image of the
  // original medium exists online); read as a folder and verified file by
  // file against the manifest (verified: files).
  const char* kind;
};

// A rectangle of a decoded picture, in its pixels (after EXIF orientation);
// w == 0: no crop.
struct Crop {
  int x = 0, y = 0, w = 0, h = 0;
};

// One place a package's cover can come from (COVERS.md §2.2, §2.3), in the
// package's preference order. Only URLs, md5s, sizes, paths and crops live
// here: no picture bytes (covers are fetched or read onto the user's machine
// at import time).
struct CoverSource {
  enum class Kind { download, disc };
  Kind kind;
  const char* art;         // "box" | "disc" | "splash" | "panel" — how the tile is rendered (§2.6)
  const char* label;       // for people: "Box front", "Disc label", "Installer art"
  const char* credit;      // for people: "Internet Archive", "Wikisimpsons", "your disc"
  // download
  const char* url;         // https only; redirects followed; the URL as published (never a mirror node)
  const char* md5;         // of the downloaded file (not of the crop); required for downloads
  uint64_t size;           // published size; required for downloads
  const wchar_t* file_name;  // saved as <download dir>\covers\<file_name>
  // disc
  const char* path;        // in the source, as identification located it: "INSTALL/SETUP.BMP", "ADE/PAGE1.BMP", "SETUP.EXE"
  const char* path_md5;    // md5 of that file: a file with other bytes (another pressing) is skipped; "" = any bytes
  uint16_t resource_type;  // 0: the file itself is the picture; 2 (RT_BITMAP): a bitmap resource of an NE/PE file
  uint16_t resource_id;
  Crop crop;               // applied after decoding (and after EXIF orientation)
};

struct Package {
  const char* id;           // [a-z0-9]+; "ad40" and "classic" are reserved (Deluxe's id prefixes)
  const char* title;
  const char* short_title;  // what disambiguates a repeated display name
  Recipe recipe;
  const char* root;         // relative to <win>: "FILES" or "packages/<id>"
  std::span<const char* const> module_dirs;  // catalog scan order (Deluxe: AD40, ENGINE (STARRYNI only), CLASSIC)
  std::span<const KnownImage> images;
  std::span<const char* const> required;     // relative to the root; an import without them fails (2)
  // tree: the folders under the source's FILES dir that are copied. The
  // fingerprint (§3): a FILES dir (ADE\FILES, FILES or the root) holding the
  // first module dir and ENGINE, plus `marker` (relative to FILES) when set,
  // and none of `absent`.
  std::span<const char* const> copy_dirs;
  const char* marker;
  std::span<const char* const> absent;
  // ad3zip: the module folder, the MODMISC.ZIP member that identifies the
  // package, the AFI.ZIP member installed as <module dir>\FOLDER.AFI, and
  // the archives that must be present (so a split-floppy source is complete).
  const char* module_dir;
  const char* engine_dll;
  const char* folder_afi;
  std::span<const char* const> required_archives;
  std::span<const Fixup> fixups;
  std::span<const NameOverride> name_overrides;
  std::span<const KnownFile> manifest;
  std::span<const Download> downloads;  // its Internet Archive copies, in the order tried (empty: none known)
  // Where its box cover comes from (COVERS.md §2.3), tried in this order
  // (empty: the front-ends draw a generated cover).
  std::span<const CoverSource> covers;
  // When the release shipped (ISO 8601, YYYY-MM or YYYY-MM-DD). The catalog lists
  // releases oldest first, so the settings dialog's cover strip and its list groups
  // read as a timeline. Empty sorts last, keeping the registry's order.
  const char* released = "";

  bool is_deluxe() const;
};

// The built-in registry, in registry order.
std::span<const Package> builtin_packages();
// `id` in `registry` (the built-in one when empty); nullptr when unknown.
const Package* find_package(std::string_view id, std::span<const Package> registry = {});
// The registry to use: `registry` itself, or the built-in one when it is empty.
std::span<const Package> registry_or_builtin(std::span<const Package> registry);
// "After Dark 4.0 Deluxe, After Dark 10th Anniversary, …" for messages.
std::string known_releases(std::span<const Package> registry = {});
// The ids of the packages with an Internet Archive copy, in registry order
// (what `adimport --download all` fetches).
std::vector<std::string> downloadable_packages(std::span<const Package> registry = {});

}  // namespace adw::import
