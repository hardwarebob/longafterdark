// The cover pipeline's internal side (COVERS.md §2.4, §2.5, §2.7, §2.10): what
// an import, a catalog write and the recovery sweep need from covers.cc. The
// public API is covers.h.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "catalog.h"   // CatalogCover
#include "packages.h"

namespace adw::import {

class CancelToken;
class SourceFs;

namespace covers_detail {

using LogFn = std::function<void(const std::string&)>;

std::filesystem::path covers_dir(const std::filesystem::path& win);                  // <win>\covers
std::filesystem::path cover_dir(const std::filesystem::path& win, const Package& p);  // <win>\covers\<id>

// What the catalog lists for an installed package, after checking
// covers\<id>: a missing, damaged or stale tile whose picture is sound is
// rendered again (the caller holds import.lock); a damaged cover.json, or a
// tile that cannot be made, lists as generated and is logged. Never throws.
CatalogCover catalog_cover(const std::filesystem::path& win, const Package& p, const LogFn& log);

// The recovery sweep (under the lock): deletes covers\*.importing-* and
// covers\<id>\*.tmp-*. True when anything was deleted.
bool recover_covers(const std::filesystem::path& win, const LogFn& log);

// An import's capture (COVERS.md §2.4), staged in covers\<id>.importing-<pid>.
struct CaptureContext {
  std::filesystem::path win;
  const Package* package = nullptr;
  const SourceFs* source = nullptr;   // the import's source (disc sources); null: downloads only
  bool allow_download = true;
  std::filesystem::path download_dir;  // cover downloads go to <download_dir>\covers (empty: default_download_dir())
  int timeout_ms = 15000;
  const CancelToken* cancel = nullptr;  // the import's (cancel.h); stops a cover download at once
  // Phase::cover progress (`item` names the source); throws ImportError(cancelled).
  std::function<void(uint64_t done, uint64_t total, const std::string& item)> progress;
  LogFn log;
};

struct StagedCover {
  std::filesystem::path stage;       // empty: nothing staged (the cover stays as it is)
  std::vector<std::wstring> files;   // staged names, cover.json last
  CatalogCover catalog;              // what the catalog lists once the stage is committed
};

// Tries the package's sources better than its stored original, and stages
// the new original, the tile and cover.json. A failed cover never fails the
// import: everything but a cancel (ImportError(cancelled), with the stage
// removed) is logged and leaves the stored cover as it was.
StagedCover stage_import_cover(const CaptureContext& c);
// With the package swap: moves the staged files into covers\<id>
// (MoveFileExW, cover.json last) and removes the stage. False (logged) when a
// move failed; the catalog must then be rendered from what is on disk.
bool commit_staged_cover(const std::filesystem::path& win, const Package& p, StagedCover& s, const LogFn& log);
void discard_staged_cover(StagedCover& s);

// False when AD_COVER_DOWNLOAD=0: no cover download at all, as with
// --no-cover-download (tests whose command lines are fixed use it).
bool downloads_allowed_by_environment();

// The download and disc sources' identity check the tests use: the registry
// index of a source recorded in cover.json (url, or path and resource), or
// -1 when the registry no longer lists it.
int source_index(const Package& p, const std::string& origin, const std::string& url, const std::string& path,
                 int resource_id);

}  // namespace covers_detail
}  // namespace adw::import
