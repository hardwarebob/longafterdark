// adw::import catalog — <assets>\win\catalog-win.json from the module
// binaries (DESIGN.md §6a, PACKAGES.md §6), without executing anything,
// merged over every installed package (packages.h).
//
// This is the C++ port of the prototype research/win/make_catalog.py and must
// stay semantically identical to it over the real Deluxe corpus
// (import.catalog_real compares the two, the PACKAGES.md §6 fields aside).
// What is read, per ABI.md §2.10 — the lane comes from the file header; the
// Deluxe folders are named here, every other package scans its own module
// dirs and ENGINE:
//
//   AD4 lane (PE32: FILES/AD40/*.AD, then FILES/ENGINE/STARRYNI.AD)
//     displayName  VERSIONINFO FileDescription (fallback: STRINGLIST 128[0], file name)
//     about        type 2000 name 40: RTF, reduced to plain text
//     controls     type 1000 names 1..4 (slot = name-1)
//     entry        "_Module@4" when exported (STARRYNI, MSVC), else "Module"
//   Classic lane (NE: FILES/CLASSIC/*.AD)
//     displayName  type 2000 name 20 (C string)
//     about        type 2000 name 30 (plain text); credits: name 10
//     controls     type 1000 names 1..4
//     entry        "MODULE"
//
// PE resources with several languages (STARRYNI ships de/en/fr/ja/es) are
// read as an English Windows loads them: US English, then neutral, then the
// lowest language id. Text is Windows-1252 (the modules' ANSI code page),
// written out as UTF-8.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "packages.h"
#include "status.h"

namespace adw::import {

// One type-1000 control record as the catalog lists it. Which optional
// fields are present depends on `kind` (ABI.md §2.10.5):
//   stringslider  type "slider", items, values, default (= values[defaultStop]),
//                 defaultStop, boldStop when the host appends a stop
//   numslider     type "slider", min, max, default (clamped), rawDefault,
//                 unit + unitPos when the record names a unit
//   popup         type "popup", items, default (clamped index)
//   checkbox      type "checkbox", default 0/1
//   button        type "button" (no value: the saver never presses buttons)
struct CatalogControl {
  int index = 0;              // slot: what SET <idx> / ADCVSET address
  std::string name;
  std::string kind;           // stringslider | numslider | popup | button | checkbox
  std::string type;           // slider | popup | button | checkbox
  std::vector<std::string> items;
  std::vector<int> values;    // stringslider: the value sent for each stop
  std::optional<int> def;     // "default"
  std::optional<int> default_stop, bold_stop;
  std::optional<int> min, max, raw_default;
  std::string unit;           // non-empty only when the record has one
  std::string unit_pos;       // "none" | "prefix" | "suffix" (with unit)
};

struct CatalogModule {
  std::string id;             // "ad40.toasters" / "classic.toast3" / "ad32.toilet"
  std::string display_name;   // unique within the lane over the whole catalog (PACKAGES.md §6)
  std::string lane;           // "pe32" | "ne16" (from the file header, never the folder)
  std::string path;           // relative to <assets>\win, '/'-separated, on-disk case
  std::string about;
  std::optional<std::string> credits;   // Classic only, when the module has 2000/10
  std::vector<CatalogControl> controls;
  std::string entry;
  std::vector<std::string> needs;       // non-system DLLs it imports (sorted, upper case)
  std::vector<std::string> system;      // system DLLs it imports (sorted, upper case)
  // PACKAGES.md §6 (optional for front-ends; every entry has them):
  std::string package, package_title;  // "deluxe", "After Dark 4.0 Deluxe"
  std::string module_name;              // the name before disambiguation (trimmed, overrides applied)
  std::string md5;                      // of the module file
  std::string same_as;                  // id of the first entry with the same md5 ("" = none)
};

// An installed package's box cover as the catalog lists it (COVERS.md §2.7).
// Paths are relative to <win>, '/'-separated. A generated cover (the
// front-ends draw it) is just { "origin": "generated" }.
struct CatalogCover {
  std::string origin = "generated";  // "user" | "download" | "disc" | "generated"
  std::string tile, tile_md5;        // the 640x800 tile.png and its md5 (changes whenever the tile does)
  std::string image;                 // the picture it shows: user.png or original.png
  int width = 0, height = 0;         // of `image`
  std::string art;                   // the original's "box" | "disc" | "splash" | "panel" (none for a user picture)
  std::string label, credit;         // for people: "Box front", "Wikisimpsons"; "Your own picture", ""
  std::string original = "generated";  // the origin of the original (under a user picture too)

  bool generated() const { return origin == "generated"; }
};

// One installed package as the catalog's top-level "packages" lists it.
struct CatalogPackage {
  std::string id, title, short_title, root, verified, imported_utc;
  size_t modules = 0;
  CatalogCover cover;
};

// ---- building blocks (exposed for the tests) ----------------------------------

// Windows-1252 bytes to UTF-8; the five undefined bytes become U+FFFD (as
// Python's cp1252 codec with errors='replace' does).
std::string cp1252_to_utf8(std::string_view s);
// The bytes before the first NUL (the whole input when there is none).
std::string_view c_string(std::string_view s);

// The About-box RTF reduced to plain text: paragraphs and line breaks become
// '\n', \tab a tab, \'xx the Windows-1252 character, \~ a space; font/colour
// tables, stylesheets, info, pictures, objects and \* destinations are
// dropped; reading stops at the end of the outermost group. Trailing blanks
// on a line are removed, runs of 3+ newlines collapse to 2, and the result is
// trimmed.
std::string rtf_to_text(std::string_view rtf);

// A type-1000 record (ABI.md §2.10.2) for `slot`; nullopt for kind 0 (none),
// an unknown kind, or a record too short to hold its kind.
std::optional<CatalogControl> parse_control_record(std::string_view rec, int slot);

// STRINGLIST: WORD count, then that many NUL-terminated strings (Latin-1).
std::vector<std::string> parse_stringlist(std::string_view data);

// Everything the catalog says about one module file. `rel_path` is the
// catalog "path"; the id comes from the file name and the lane — Deluxe's
// "ad40.<base>" / "classic.<base>" when `package` is null or Deluxe, else
// "<package>.<base>". The package fields and md5 are filled; module_name is
// the trimmed display name (overrides are the merge's job). Throws
// ImportError(source_invalid) for a file that is neither PE32 nor NE, and
// adw::loader::LoaderError for a damaged image.
CatalogModule catalog_module(const std::filesystem::path& file, const std::string& rel_path,
                             const Package* package = nullptr);

// ---- the whole catalog ----------------------------------------------------------

// One installed package for build_catalog: its registry entry, where its
// files are now (its root, or an import's staging copy of it) and what its
// import record says.
struct CatalogTree {
  const Package* package = nullptr;
  std::filesystem::path dir;
  std::string verified, imported_utc;
  CatalogCover cover;  // its box cover (installed_trees and imports fill it in)
};

struct CatalogDoc {
  std::vector<CatalogPackage> packages;
  std::vector<CatalogModule> modules;
};

// The merged catalog (PACKAGES.md §6) over `trees`, which are in registry
// order. Per package, its module dirs in registry order (Deluxe: AD40\*.AD
// sorted, ENGINE\STARRYNI.AD, CLASSIC\*.AD sorted; every other package also
// ENGINE\*.AD), paths "<package root>/<dir>/<file>" whatever the directory
// is called now. Ids are unique (a second file with a taken id is skipped
// and logged); name overrides apply; display names are made unique per lane;
// sameAs links byte-identical modules. A module that cannot be read is left
// out and reported through `log`, so one damaged file never costs the user
// every other module.
CatalogDoc build_catalog(const std::vector<CatalogTree>& trees,
                         const std::function<void(const std::string&)>& log = {});

// A Deluxe FILES tree alone (<assets>\win\FILES, or an import's staging copy
// of it): build_catalog over that one tree, modules only. Paths come out as
// "FILES/…" whatever the directory is called. Throws
// ImportError(source_invalid) when `files_dir` is not a directory.
std::vector<CatalogModule> scan_catalog(const std::filesystem::path& files_dir,
                                        const std::function<void(const std::string&)>& log = {});

// The JSON document (DESIGN.md §6a, PACKAGES.md §6, plus the prototype's
// extra fields), laid out as Python's json.dump(indent=1,
// ensure_ascii=False) lays out the prototype's, so the two diff cleanly.
std::string render_catalog_json(const std::vector<CatalogModule>& modules,
                                const std::vector<CatalogPackage>& packages = {});
std::string render_catalog(const CatalogDoc& doc);  // the same for a whole document

inline constexpr char kCatalogFileName[] = "catalog-win.json";
inline constexpr char kCatalogGenerator[] = "adimport 1.2";

struct CatalogResult {
  Status status = Status::error;
  std::string message;
  std::filesystem::path path;   // <win>\catalog-win.json
  size_t modules = 0, controls = 0;
};

// adimport --catalog-only: regenerates <win_assets_dir(root)>\catalog-win.json
// from every package already installed there (root empty =
// default_assets_root()); FILES is not required, one installed package is.
// Holds import.lock while it reads, so it never scans a tree an import is
// swapping, repairs what an interrupted import left first, and replaces the
// file atomically. Never throws.
CatalogResult regenerate_catalog(const std::filesystem::path& assets_root,
                                 const std::function<void(const std::string&)>& log = {},
                                 std::span<const Package> registry = {});

}  // namespace adw::import
