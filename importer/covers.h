// importer/covers.h — owned by C. These declarations are frozen by COVERS.md §2.9
// (T's GUI calls them); C may add to them but not change them.
//
// Each release's box cover (COVERS.md §2): the user's own picture (--set-cover), else the
// best of the package's registry cover sources captured so far (packages.h CoverSource:
// md5-checked downloads and on-disc art), else a generated cover the front-ends draw. The
// pictures live under <win>\covers\<id>\ (original.png, user.png, tile.png, cover.json),
// never inside a package root, and catalog-win.json describes them in packages[].cover.
#pragma once
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "importer.h"   // Progress
#include "packages.h"
#include "status.h"

namespace adw::import {

class CancelToken;  // cancel.h

enum class CoverOrigin { generated, disc, download, user };
const char* cover_origin_name(CoverOrigin o);   // "generated" | "disc" | "download" | "user"

struct CoverInfo {
  std::string package;                       // registry id
  bool installed = false;
  CoverOrigin origin = CoverOrigin::generated;   // what the tile shows now
  std::filesystem::path tile;                // absolute; empty when generated
  std::filesystem::path image;               // absolute: user.png or original.png; empty when generated
  std::string tile_md5;
  int width = 0, height = 0;                 // of `image`
  std::string label, credit;                 // UTF-8, for people ("Box front", "Wikisimpsons"; "Your own picture", "")
  bool has_user = false;                     // a --set-cover picture is in use
  CoverOrigin original = CoverOrigin::generated;  // the original under it
  bool can_download = false;                 // a registry download better than `original` exists
  // Added by C (not part of the frozen set), for people (UTF-8): the stored original's label
  // and credit, also under a user picture ("Disc label", "Internet Archive"; empty when there is
  // none), and the first registry download better than it, which "Download the original cover"
  // (refresh_covers) tries first ("Box front", "Wayback Machine"; empty when !can_download).
  std::string original_label, original_credit;
  std::string download_label, download_credit;
  // Added by C: why the cover could not be read (an I/O error, a damaged record, a picture WIC
  // cannot decode), UTF-8, for people and the log; empty when it was read. The other fields then
  // say what could be established (`installed` always), and the tile shows as generated.
  std::string error;
};
// Reads covers\<id>\cover.json and checks its files. No lock; never throws.
CoverInfo cover_info(const std::string& id, const std::filesystem::path& assets_root,
                     std::span<const Package> registry = {});

struct CoverOptions {
  std::filesystem::path assets_root;         // empty = default_assets_root()
  std::filesystem::path download_dir;        // empty = default_download_dir()
  bool allow_download = true;                // false = --no-cover-download
  bool force = false;                        // --refresh-covers --force
  std::function<bool(const Progress&)> progress;   // Phase::cover; return false to cancel
  std::function<void(const std::string&)> log;
  std::span<const Package> registry;         // empty = builtin_packages()
  // Added by C (not part of the frozen set): the connect / between-bytes timeout of each
  // cover download (COVERS.md §2.4: 15 s). Tests shorten it.
  int timeout_ms = 15000;
  // Added by C: cancels at once, even a download waiting on the network (cancel.h); `progress`
  // returning false is noticed only at its next report. Optional; the caller keeps it alive.
  const CancelToken* cancel = nullptr;
};
struct CoverResult {
  Status status = Status::error;
  std::string message;                       // one line for people
  CoverInfo info;                            // after the operation
  bool changed = false;                      // the tile or its origin changed
};
// All three take import.lock, write atomically, rewrite catalog-win.json when anything
// changed, and never throw.
CoverResult set_cover(const std::string& id, const std::filesystem::path& picture, const CoverOptions& o);
CoverResult clear_cover(const std::string& id, const CoverOptions& o);
// ids empty = every installed package; one result per package, in registry order.
std::vector<CoverResult> refresh_covers(const std::vector<std::string>& ids, const CoverOptions& o);

}  // namespace adw::import
