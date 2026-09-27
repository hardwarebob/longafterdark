// The box covers (COVERS.md §2): capture during an import, the stored pictures
// and their cover.json, the catalog's view of them, and the cover commands.
//
//   <win>\covers\<id>\original.png   the best registry source captured so far (normalized)
//   <win>\covers\<id>\user.png       the user's own picture (--set-cover), normalized the same way
//   <win>\covers\<id>\tile.png       640x800, rendered from user.png, else original.png
//   <win>\covers\<id>\cover.json     provenance: which source, the md5 of every file, the attempts
//   <download dir>\covers\<file>     the downloaded source files, md5-checked, reused later
//
// Every file is written as <name>.tmp-<pid> and renamed; an import stages its
// files in covers\<id>.importing-<pid> and moves them in with the package swap.
// A stored file counts only while its md5 matches cover.json.
#include "covers.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "cover_image.h"
#include "covers_internal.h"
#include "download.h"
#include "install.h"
#include "md5.h"
#include "minijson.h"
#include "source.h"
#include "winutil.h"

namespace adw::import {

namespace fs = std::filesystem;
using cover::Picture;

const char* cover_origin_name(CoverOrigin o) {
  switch (o) {
    case CoverOrigin::generated: return "generated";
    case CoverOrigin::disc: return "disc";
    case CoverOrigin::download: return "download";
    case CoverOrigin::user: return "user";
  }
  return "generated";
}

namespace covers_detail {

fs::path covers_dir(const fs::path& win) { return win / L"covers"; }
fs::path cover_dir(const fs::path& win, const Package& p) { return covers_dir(win) / to_wide(p.id); }

namespace {

constexpr int kNoOriginal = INT_MAX;  // "k = infinity": every source is better
constexpr char kUserLabel[] = "Your own picture";

std::wstring pid_tag() { return std::to_wstring(GetCurrentProcessId()); }

std::string utc_now() {
  SYSTEMTIME st;
  GetSystemTime(&st);
  char b[32];
  snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  return b;
}

// COM for a whole cover operation (WIC); a thread with an apartment keeps it.
class ComScope {
 public:
  ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  HRESULT hr_;
};

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = char(x - 32);
    if (y >= 'a' && y <= 'z') y = char(y - 32);
    if (x != y) return false;
  }
  return true;
}

// ---- cover.json, version 1 ------------------------------------------------------------------

struct OriginalRec {
  std::string origin;  // "download" | "disc"
  int source = -1;     // registry index when captured
  std::string art, label, credit;
  std::string url, file_md5;  // download
  uint64_t file_size = 0;
  std::string path, path_md5;  // disc
  int res_type = 0, res_id = 0;
  Crop crop;
  std::string md5;  // of original.png
  int width = 0, height = 0;
  std::string captured_utc;
};
struct UserRec {
  std::string md5;
  int width = 0, height = 0;
  std::string name, set_utc;
};
struct TileRec {
  std::string md5, from;  // from: "user" | "original"
  int renderer = 0;
};
struct Attempt {
  int source = -1;
  std::string status, message;
};
struct Record {
  std::optional<OriginalRec> original;
  std::optional<UserRec> user;
  std::optional<TileRec> tile;
  std::string attempts_utc;
  std::vector<Attempt> failed;
};

std::string q(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += char(c);
        }
    }
  }
  return o + "\"";
}

std::string num(int64_t v) { return std::to_string(v); }

std::string render_record(const Record& r, const std::string& id) {
  std::string j = "{\n  \"version\": 1,\n  \"package\": " + q(id) + ",\n  \"tool\": " + q(kToolName);
  if (r.original) {
    const OriginalRec& o = *r.original;
    j += ",\n  \"original\": {\"origin\": " + q(o.origin) + ", \"source\": " + num(o.source) + ", \"art\": " + q(o.art) +
         ", \"label\": " + q(o.label) + ", \"credit\": " + q(o.credit);
    if (o.origin == "download") {
      j += ", \"url\": " + q(o.url) + ", \"fileMd5\": " + q(o.file_md5) + ", \"fileSize\": " + num(int64_t(o.file_size));
    } else {
      j += ", \"path\": " + q(o.path);
      if (o.res_type) j += ", \"resource\": [" + num(o.res_type) + ", " + num(o.res_id) + "]";
      j += ", \"pathMd5\": " + q(o.path_md5);
    }
    j += ", \"crop\": " + (o.crop.w > 0 ? "{\"x\": " + num(o.crop.x) + ", \"y\": " + num(o.crop.y) + ", \"w\": " +
                                             num(o.crop.w) + ", \"h\": " + num(o.crop.h) + "}"
                                       : std::string("null"));
    j += ",\n               \"file\": \"original.png\", \"md5\": " + q(o.md5) + ", \"width\": " + num(o.width) +
         ", \"height\": " + num(o.height) + ", \"capturedUtc\": " + q(o.captured_utc) + "}";
  }
  if (r.user) {
    const UserRec& u = *r.user;
    j += ",\n  \"user\": {\"file\": \"user.png\", \"md5\": " + q(u.md5) + ", \"width\": " + num(u.width) +
         ", \"height\": " + num(u.height) + ", \"name\": " + q(u.name) + ", \"setUtc\": " + q(u.set_utc) + "}";
  }
  if (r.tile) {
    j += ",\n  \"tile\": {\"file\": \"tile.png\", \"md5\": " + q(r.tile->md5) + ", \"from\": " + q(r.tile->from) +
         ", \"renderer\": " + num(r.tile->renderer) + "}";
  }
  if (!r.attempts_utc.empty()) {
    j += ",\n  \"attempts\": {\"lastUtc\": " + q(r.attempts_utc) + ", \"failed\": [";
    for (size_t i = 0; i < r.failed.size(); i++)
      j += std::string(i ? ",\n" : "\n") + "    {\"source\": " + num(r.failed[i].source) + ", \"status\": " +
           q(r.failed[i].status) + ", \"message\": " + q(r.failed[i].message) + "}";
    j += r.failed.empty() ? "]}" : "\n  ]}";
  }
  return j + "\n}\n";
}

// `exists` is false only when there is no such file; a file that is there but
// cannot be read (access denied, an I/O error, larger than any record) sets
// `error` instead of passing for an absent one.
std::string read_small_file(const fs::path& p, bool* exists, std::string* error) {
  *exists = false;
  Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
  if (!h.valid()) {
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return {};
    *exists = true;
    *error = "cannot read " + to_utf8(p.wstring()) + ": " + win_error_string(e);
    return {};
  }
  *exists = true;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h.get(), &size) || size.QuadPart > (1 << 20)) {
    *error = to_utf8(p.wstring()) + " is not a cover record (larger than 1 MB, or its size cannot be read)";
    return {};
  }
  std::string data(size_t(size.QuadPart), '\0');
  DWORD got = 0;
  if (!data.empty() && (!ReadFile(h.get(), data.data(), DWORD(data.size()), &got, nullptr) || got != data.size())) {
    *error = "cannot read " + to_utf8(p.wstring()) + ": " + win_error_string(GetLastError());
    return {};
  }
  return data;
}

// A JSON number as an int, or `dflt` when it is none or out of int's range.
int int_of(const JsonValue* v, int dflt = 0) {
  return v ? v->as_int(dflt) : dflt;
}

// covers\<id>\cover.json; `damaged` when it is there but cannot be read or
// is not a version-1 record of this package (`error` says which).
Record read_record(const fs::path& dir, const std::string& id, bool* present, bool* damaged, std::string* error) {
  Record r;
  *damaged = false;
  std::string text = read_small_file(dir / L"cover.json", present, error);
  if (!*present) return r;
  if (!error->empty()) {
    *damaged = true;
    return r;
  }
  auto j = parse_json(text);
  if (!j || j->kind != JsonValue::Kind::object || j->integer("version") != 1 || j->str("package") != id) {
    *damaged = true;
    *error = "covers\\" + id + "\\cover.json is damaged (not a cover record of " + id + ")";
    return r;
  }
  if (const JsonValue* o = j->get("original"); o && o->kind == JsonValue::Kind::object) {
    OriginalRec rec;
    rec.origin = o->str("origin");
    rec.source = o->int_in("source", -1);
    rec.art = o->str("art");
    rec.label = o->str("label");
    rec.credit = o->str("credit");
    rec.url = o->str("url");
    rec.file_md5 = o->str("fileMd5");
    rec.file_size = uint64_t(std::max<int64_t>(0, o->integer("fileSize")));
    rec.path = o->str("path");
    rec.path_md5 = o->str("pathMd5");
    if (const JsonValue* res = o->get("resource"); res && res->kind == JsonValue::Kind::array && res->array.size() == 2) {
      rec.res_type = int_of(&res->array[0]);
      rec.res_id = int_of(&res->array[1]);
    }
    if (const JsonValue* c = o->get("crop"); c && c->kind == JsonValue::Kind::object)
      rec.crop = Crop{c->int_in("x"), c->int_in("y"), c->int_in("w"), c->int_in("h")};
    rec.md5 = o->str("md5");
    rec.width = o->int_in("width");
    rec.height = o->int_in("height");
    rec.captured_utc = o->str("capturedUtc");
    if ((rec.origin == "download" || rec.origin == "disc") && rec.md5.size() == 32) r.original = rec;
  }
  if (const JsonValue* u = j->get("user"); u && u->kind == JsonValue::Kind::object) {
    UserRec rec;
    rec.md5 = u->str("md5");
    rec.width = u->int_in("width");
    rec.height = u->int_in("height");
    rec.name = u->str("name");
    rec.set_utc = u->str("setUtc");
    if (rec.md5.size() == 32) r.user = rec;
  }
  if (const JsonValue* t = j->get("tile"); t && t->kind == JsonValue::Kind::object) {
    TileRec rec;
    rec.md5 = t->str("md5");
    rec.from = t->str("from");
    rec.renderer = t->int_in("renderer");
    if (rec.md5.size() == 32) r.tile = rec;
  }
  if (const JsonValue* a = j->get("attempts"); a && a->kind == JsonValue::Kind::object) {
    r.attempts_utc = a->str("lastUtc");
    if (const JsonValue* f = a->get("failed"); f && f->kind == JsonValue::Kind::array)
      for (const JsonValue& e : f->array)
        if (e.kind == JsonValue::Kind::object)
          r.failed.push_back({e.int_in("source", -1), e.str("status"), e.str("message")});
  }
  return r;
}

// A record checked against its files.
struct State {
  Record rec;
  bool present = false, damaged = false;
  std::string error;  // why the record, or one of its pictures, could not be read ("" = it could)
  bool original_ok = false, user_ok = false, tile_ok = false;  // the file matches its recorded md5

  // What the tile should show.
  std::string want_from() const { return user_ok ? "user" : original_ok ? "original" : ""; }
  bool tile_shows_it() const {
    std::string f = want_from();
    return !f.empty() && tile_ok && rec.tile && rec.tile->from == f;
  }
  bool tile_current() const { return tile_shows_it() && rec.tile->renderer == cover::kRendererVersion; }
};

// False for a missing file or other bytes; a file that is there but cannot
// be read also says why in `error` (the first such reason is kept).
bool file_matches(const fs::path& f, const std::string& md5, std::string& error) {
  std::error_code ec;
  if (!fs::is_regular_file(f, ec)) return false;
  try {
    return md5_file_hex(f) == md5;
  } catch (const std::exception& e) {
    if (error.empty()) error = e.what();
    return false;
  }
}

State load_state(const fs::path& dir, const std::string& id) {
  State st;
  st.rec = read_record(dir, id, &st.present, &st.damaged, &st.error);
  if (st.damaged) return st;
  if (st.rec.original) st.original_ok = file_matches(dir / L"original.png", st.rec.original->md5, st.error);
  if (st.rec.user) st.user_ok = file_matches(dir / L"user.png", st.rec.user->md5, st.error);
  if (st.rec.tile) st.tile_ok = file_matches(dir / L"tile.png", st.rec.tile->md5, st.error);
  return st;
}

// A state built from a record whose files are all known to be sound (what
// an import or a command has just written).
State sound_state(const Record& rec) {
  State st;
  st.rec = rec;
  st.present = true;
  st.original_ok = rec.original.has_value();
  st.user_ok = rec.user.has_value();
  st.tile_ok = rec.tile.has_value();
  return st;
}

CoverOrigin origin_of(const std::string& s) {
  return s == "download" ? CoverOrigin::download : s == "disc" ? CoverOrigin::disc : CoverOrigin::generated;
}

CatalogCover to_catalog(const State& st, const std::string& id) {
  CatalogCover c;
  if (st.damaged || !st.tile_shows_it()) return c;
  const std::string base = "covers/" + id + "/";
  c.tile = base + "tile.png";
  c.tile_md5 = st.rec.tile->md5;
  c.original = st.original_ok ? st.rec.original->origin : "generated";
  if (st.user_ok) {
    c.origin = "user";
    c.image = base + "user.png";
    c.width = st.rec.user->width;
    c.height = st.rec.user->height;
    c.label = kUserLabel;
  } else {
    const OriginalRec& o = *st.rec.original;
    c.origin = o.origin;
    c.image = base + "original.png";
    c.width = o.width;
    c.height = o.height;
    c.art = o.art;
    c.label = o.label;
    c.credit = o.credit;
  }
  return c;
}

// The index of the stored original's source in the registry now (k), or
// kNoOriginal when there is no sound original or the registry no longer
// lists its source.
int stored_rank(const Package& p, const State& st) {
  if (!st.original_ok) return kNoOriginal;
  const OriginalRec& o = *st.rec.original;
  int i = source_index(p, o.origin, o.url, o.path, o.res_id);
  return i < 0 ? kNoOriginal : i;
}

void fill_info(CoverInfo& info, const State& st, const fs::path& dir, const Package& p) {
  info.has_user = !st.damaged && st.user_ok;
  info.original = !st.damaged && st.original_ok ? origin_of(st.rec.original->origin) : CoverOrigin::generated;
  info.origin = CoverOrigin::generated;
  info.tile.clear();
  info.image.clear();
  info.tile_md5.clear();
  info.width = info.height = 0;
  info.label.clear();
  info.credit.clear();
  if (!st.damaged && st.tile_shows_it()) {
    info.tile = dir / L"tile.png";
    info.tile_md5 = st.rec.tile->md5;
    if (st.user_ok) {
      info.origin = CoverOrigin::user;
      info.image = dir / L"user.png";
      info.width = st.rec.user->width;
      info.height = st.rec.user->height;
      info.label = kUserLabel;
    } else {
      const OriginalRec& o = *st.rec.original;
      info.origin = origin_of(o.origin);
      info.image = dir / L"original.png";
      info.width = o.width;
      info.height = o.height;
      info.label = o.label;
      info.credit = o.credit;
    }
  }
  info.original_label.clear();
  info.original_credit.clear();
  if (!st.damaged && st.original_ok) {
    info.original_label = st.rec.original->label;
    info.original_credit = st.rec.original->credit;
  }
  int k = st.damaged ? kNoOriginal : stored_rank(p, st);
  info.can_download = false;
  info.download_label.clear();
  info.download_credit.clear();
  for (size_t i = 0; i < p.covers.size() && int(i) < k; i++) {
    if (p.covers[i].kind != CoverSource::Kind::download) continue;
    if (!info.can_download) {
      info.download_label = p.covers[i].label ? p.covers[i].label : "";
      info.download_credit = p.covers[i].credit ? p.covers[i].credit : "";
    }
    info.can_download = true;
  }
}

// ---- writing -------------------------------------------------------------------------------------

std::string_view as_text(const std::vector<uint8_t>& v) { return std::string_view((const char*)v.data(), v.size()); }

// `data` as dir\name: written to name.tmp-<pid>, then renamed over it.
void publish(const fs::path& dir, const std::wstring& name, std::string_view data) {
  fs::path final = dir / name, tmp = final;
  tmp += L".tmp-" + pid_tag();
  DWORD err = 0;
  if (!write_whole_file(tmp, data, err))
    throw ImportError(Status::error, "cannot write " + to_utf8(tmp.wstring()) + ": " + win_error_string(err));
  if (!move_with_retry(tmp, final, MOVEFILE_REPLACE_EXISTING, err)) {
    std::error_code ec;
    fs::remove(tmp, ec);
    throw ImportError(Status::error, "cannot replace " + to_utf8(final.wstring()) + ": " + win_error_string(err));
  }
}

void remove_file(const fs::path& p) {
  std::error_code ec;
  fs::remove(p, ec);
}

// The tile for what `st` should show, from the stored picture.
std::vector<uint8_t> tile_from_files(const fs::path& dir, const State& st) {
  if (st.user_ok) {
    Picture pic = cover::decode_picture(cover::read_picture_file(dir / L"user.png"));
    return cover::encode_png(cover::render_tile(pic, "picture"), false);
  }
  Picture pic = cover::decode_picture(cover::read_picture_file(dir / L"original.png"));
  return cover::encode_png(cover::render_tile(pic, st.rec.original->art), false);
}

// Drops the pictures that no longer match cover.json (logged): a bad user.png
// counts as absent, a bad original.png too. True when the record changed.
bool drop_unsound(State& st, const Package& p, const LogFn& log) {
  bool changed = false;
  if (st.rec.user && !st.user_ok) {
    if (log) log(std::string("covers\\") + p.id + "\\user.png is missing or damaged; it no longer counts");
    st.rec.user.reset();
    changed = true;
  }
  if (st.rec.original && !st.original_ok) {
    if (log) log(std::string("covers\\") + p.id + "\\original.png is missing or damaged; it no longer counts");
    st.rec.original.reset();
    changed = true;
  }
  return changed;
}

// Under the lock: the tile brought in line with what should show (rendered
// again when missing, damaged, stale or from the other picture), and
// cover.json with it. True when anything was written.
bool repair(const fs::path& dir, State& st, const Package& p, const LogFn& log) {
  if (st.damaged || !st.present) return false;
  bool changed = drop_unsound(st, p, log);
  std::string from = st.want_from();
  if (from.empty()) {
    if (st.rec.tile) {
      st.rec.tile.reset();
      st.tile_ok = false;
      changed = true;
    }
  } else if (!st.tile_current()) {
    std::vector<uint8_t> png = tile_from_files(dir, st);
    publish(dir, L"tile.png", as_text(png));
    st.rec.tile = TileRec{md5_hex(png.data(), png.size()), from, cover::kRendererVersion};
    st.tile_ok = true;
    changed = true;
    if (log) log(std::string("rendered the cover tile of ") + p.title + " again");
  }
  if (changed) publish(dir, L"cover.json", render_record(st.rec, p.id));
  return changed;
}

// ---- sources ------------------------------------------------------------------------------------

struct SourceFailure {
  std::string status, message;
  bool network = false;  // network-class: the remaining downloads are skipped
};

struct Captured {
  Picture pic;
  OriginalRec rec;
};

struct Fetch {
  // <this>\covers\<file>. Empty: default_download_dir(), taken when a download
  // actually starts, so a command that fetches nothing never uses the data
  // folder (importer.h data_folder()).
  fs::path download_dir;
  int timeout_ms = 15000;
  const CancelToken* cancel = nullptr;
  std::function<void(uint64_t, uint64_t, const std::string&)> progress;  // throws ImportError(cancelled)
  LogFn log;
};

std::string item_of(const CoverSource& s) { return std::string(s.label) + " from " + s.credit; }

OriginalRec rec_of(const CoverSource& s, int index) {
  OriginalRec r;
  r.origin = s.kind == CoverSource::Kind::download ? "download" : "disc";
  r.source = index;
  r.art = s.art;
  r.label = s.label;
  r.credit = s.credit;
  r.crop = s.crop;
  return r;
}

Picture decode_source(std::span<const uint8_t> bytes, const Crop& c) {
  try {
    return cover::normalize(bytes, c);
  } catch (const ImportError& e) {
    if (e.status() == Status::cancelled) throw;
    throw SourceFailure{"invalid", e.what()};
  }
}

Captured capture_download(const CoverSource& s, int index, const Fetch& f) {
  const std::string item = item_of(s);
  DownloadOptions d;
  d.url = s.url;
  d.dest = (f.download_dir.empty() ? default_download_dir() : f.download_dir) / L"covers" / s.file_name;
  d.expected_md5 = s.md5;
  d.expected_size = s.size;
  d.max_attempts = 2;
  d.timeout_ms = f.timeout_ms;
  d.cancel = f.cancel;
  d.log = f.log;
  d.progress = [&](uint64_t done, uint64_t total) {
    try {
      f.progress(done, total ? total : s.size, item);
      return true;
    } catch (const ImportError&) {
      return false;
    }
  };
  d.hash_progress = d.progress;
  try {
    DownloadResult r = download(d);
    if (f.log)
      f.log(r.reused ? "cover: using the already-downloaded " + to_utf8(d.dest.wstring())
                     : "cover: downloaded " + std::string(s.url));
  } catch (const ConnectionError& e) {
    throw SourceFailure{"network", e.what(), true};
  } catch (const ImportError& e) {
    switch (e.status()) {
      case Status::cancelled: throw;
      case Status::network: throw SourceFailure{"unavailable", e.what()};
      case Status::verify_failed: throw SourceFailure{"mismatch", e.what()};
      default: throw SourceFailure{"error", e.what()};
    }
  }
  std::vector<uint8_t> bytes;
  try {
    bytes = cover::read_picture_file(d.dest);
  } catch (const ImportError& e) {
    throw SourceFailure{"error", e.what()};
  }
  Captured c;
  c.pic = decode_source(bytes, s.crop);
  c.rec = rec_of(s, index);
  c.rec.url = s.url;
  c.rec.file_md5 = s.md5;
  c.rec.file_size = s.size;
  return c;
}

// The named file on the source: its path from the root, else (a copy of just
// the install folder, or of the ADE folder) with its leading folders dropped.
std::optional<SourceNode> find_on_source(const SourceFs& fs, std::string_view path) {
  std::string_view p = path;
  for (;;) {
    if (auto n = fs.find(p); n && !n->is_dir) return n;
    size_t slash = p.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    p.remove_prefix(slash + 1);
  }
}

Captured capture_disc(const CoverSource& s, int index, const SourceFs& fs, const Fetch& f) {
  f.progress(0, 0, item_of(s));
  auto node = find_on_source(fs, s.path);
  if (!node) throw SourceFailure{"missing", std::string("the source has no ") + s.path};
  std::vector<uint8_t> bytes;
  try {
    bytes = fs.read_all(*node, cover::kMaxPictureBytes);
  } catch (const ImportError& e) {
    if (e.status() == Status::cancelled) throw;
    throw SourceFailure{"invalid", e.what()};
  }
  std::string md5 = md5_hex(bytes.data(), bytes.size());
  if (s.path_md5 && *s.path_md5 && md5 != s.path_md5)
    throw SourceFailure{"mismatch", std::string(s.path) + " has other bytes (md5 " + md5 + "; another pressing?)"};
  if (s.resource_type == 2) {
    try {
      bytes = cover::bitmap_resource(as_text(bytes), s.resource_id);
    } catch (const ImportError& e) {
      throw SourceFailure{"invalid", std::string(s.path) + ": " + e.what()};
    }
  }
  Captured c;
  c.pic = decode_source(bytes, s.crop);
  c.rec = rec_of(s, index);
  c.rec.path = s.path;
  c.rec.path_md5 = md5;
  c.rec.res_type = s.resource_type;
  c.rec.res_id = s.resource_type ? s.resource_id : 0;
  return c;
}

struct Tried {
  std::optional<Captured> got;
  std::vector<Attempt> failed;
  int tried = 0;  // sources tried (or skipped) this run
  int download_failures = 0;
};

// The package's sources with index < k, in order: the first that works.
Tried try_sources(const Package& p, int k, const SourceFs* source, bool allow_download, bool& network_down,
                  const Fetch& f) {
  Tried t;
  for (size_t i = 0; i < p.covers.size() && int(i) < k; i++) {
    const CoverSource& s = p.covers[i];
    const bool dl = s.kind == CoverSource::Kind::download;
    if (dl && !allow_download) continue;
    if (!dl && !source) continue;  // no disc to read (--refresh-covers)
    t.tried++;
    if (dl && network_down) {
      t.failed.push_back({int(i), "skipped", "not tried after a network failure"});
      continue;
    }
    try {
      t.got = dl ? capture_download(s, int(i), f) : capture_disc(s, int(i), *source, f);
      if (f.log)
        f.log("cover of " + std::string(p.title) + ": " + item_of(s) + " (" + std::to_string(t.got->pic.w) + "x" +
              std::to_string(t.got->pic.h) + ")");
      return t;
    } catch (const SourceFailure& e) {
      t.failed.push_back({int(i), e.status, e.message});
      if (dl) t.download_failures++;
      if (e.network) network_down = true;
      if (f.log) f.log("cover of " + std::string(p.title) + ": " + item_of(s) + " (" + e.status + "): " + e.message);
    }
  }
  return t;
}

bool is_installed(const fs::path& win, const Package& p) {
  std::error_code ec;
  fs::path root = package_root(win, p);
  return p.is_deluxe() ? fs::is_directory(root, ec) : fs::is_regular_file(root / L"import.json", ec);
}

fs::path assets_of(const fs::path& root) { return root.empty() ? default_assets_root() : root; }

std::string describe(const CoverInfo& i) {
  switch (i.origin) {
    case CoverOrigin::generated: return "a generated cover";
    case CoverOrigin::user: return "your own picture";
    default: break;
  }
  return i.label + (i.credit.empty() ? "" : " from " + i.credit) + " (" + std::to_string(i.width) + "x" +
         std::to_string(i.height) + ")";
}

}  // namespace

bool downloads_allowed_by_environment() {
  const wchar_t* e = _wgetenv(L"AD_COVER_DOWNLOAD");
  return !(e && e[0] == L'0' && e[1] == 0);
}

int source_index(const Package& p, const std::string& origin, const std::string& url, const std::string& path,
                 int resource_id) {
  for (size_t i = 0; i < p.covers.size(); i++) {
    const CoverSource& s = p.covers[i];
    if (origin == "download" && s.kind == CoverSource::Kind::download && url == s.url) return int(i);
    if (origin == "disc" && s.kind == CoverSource::Kind::disc && iequals(path, s.path) &&
        (s.resource_type ? s.resource_id : 0) == resource_id)
      return int(i);
  }
  return -1;
}

CatalogCover catalog_cover(const fs::path& win, const Package& p, const LogFn& log) {
  fs::path dir = cover_dir(win, p);
  try {
    State st = load_state(dir, p.id);
    if (!st.present) return {};
    if (st.damaged) {
      if (log)
        log(st.error + ": " + p.title + " shows a generated cover until adimport --refresh-covers repairs it");
      return {};
    }
    try {
      repair(dir, st, p, log);
    } catch (const std::exception& e) {
      if (log) log(std::string("could not repair the cover of ") + p.title + ": " + e.what());
    }
    CatalogCover c = to_catalog(st, p.id);
    if (c.generated() && !st.want_from().empty() && log)
      log(std::string("the cover tile of ") + p.title + " is missing; it is listed as generated");
    return c;
  } catch (const std::exception& e) {
    if (log) log(std::string("the cover of ") + p.title + " is listed as generated: " + e.what());
    return {};
  }
}

bool recover_covers(const fs::path& win, const LogFn& log) {
  std::error_code ec;
  fs::path dir = covers_dir(win);
  if (!fs::is_directory(dir, ec)) return false;
  bool any = false;
  std::vector<fs::path> stages, subdirs;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    std::wstring n = e.path().filename().wstring();
    if (n.find(L".importing-") != std::wstring::npos) stages.push_back(e.path());
    else if (e.is_directory(ec)) subdirs.push_back(e.path());
  }
  for (const fs::path& s : stages) {
    remove_tree(s);
    any = true;
  }
  for (const fs::path& d : subdirs) {
    std::vector<fs::path> tmps;
    for (auto& e : fs::directory_iterator(d, ec))
      if (e.path().filename().wstring().find(L".tmp-") != std::wstring::npos) tmps.push_back(e.path());
    for (const fs::path& t : tmps) {
      remove_tree(t);
      any = true;
    }
  }
  if (any && log) log("removed the cover files an interrupted operation left behind");
  return any;
}

StagedCover stage_import_cover(const CaptureContext& c) {
  StagedCover out;
  const Package& p = *c.package;
  const fs::path dir = cover_dir(c.win, p);
  const State initial = load_state(dir, p.id);
  // What the catalog lists when nothing is staged: the cover as it is.
  out.catalog = to_catalog(initial, p.id);
  if (p.covers.empty() && !initial.present) return out;  // nothing to capture or keep
  ComScope com;
  try {
    c.progress(0, 0, "");  // the phase starts (and a cancel is honoured)
    State cur = initial;
    bool record_changed = false;
    if (cur.damaged) {
      if (c.log) c.log(cur.error + "; starting a new one");
      cur = State{};
      record_changed = true;
    }
    record_changed = drop_unsound(cur, p, c.log) || record_changed;
    Record rec = cur.rec;

    Fetch f;
    f.download_dir = c.download_dir;
    f.timeout_ms = c.timeout_ms;
    f.cancel = c.cancel;
    f.progress = c.progress;
    f.log = c.log;
    bool network_down = false;
    Tried t = try_sources(p, stored_rank(p, cur), c.source, c.allow_download, network_down, f);
    if (!t.got && !t.failed.empty() && c.log)
      c.log(std::string("the cover of ") + p.title +
            (cur.original_ok ? " stays as it was" : " stays generated") + " (adimport --refresh-covers tries again)");

    out.stage = covers_dir(c.win) / (to_wide(p.id) + L".importing-" + pid_tag());
    remove_tree(out.stage);
    fs::create_directories(out.stage);
    auto stage_file = [&](const std::wstring& name, std::string_view data) {
      DWORD err = 0;
      if (!write_whole_file(out.stage / name, data, err))
        throw ImportError(Status::error, "cannot write " + to_utf8((out.stage / name).wstring()) + ": " +
                                             win_error_string(err));
      out.files.push_back(name);
    };
    std::optional<Picture> fresh;
    if (t.got) {
      std::vector<uint8_t> png = cover::encode_png(t.got->pic, true);
      OriginalRec o = t.got->rec;
      o.md5 = md5_hex(png.data(), png.size());
      o.width = t.got->pic.w;
      o.height = t.got->pic.h;
      o.captured_utc = utc_now();
      rec.original = o;
      fresh = std::move(t.got->pic);
      stage_file(L"original.png", as_text(png));
      record_changed = true;
    }
    if (t.tried) {
      rec.attempts_utc = utc_now();
      rec.failed = t.failed;
      record_changed = true;
    }
    // The tile: user.png is never touched by an import, and wins.
    State next = cur;
    next.rec = rec;
    next.original_ok = rec.original.has_value();
    std::string from = next.want_from();
    if (from.empty()) {
      if (rec.tile) rec.tile.reset(), record_changed = true;
    } else if (fresh && from == "original") {
      std::vector<uint8_t> png = cover::encode_png(cover::render_tile(*fresh, rec.original->art), false);
      rec.tile = TileRec{md5_hex(png.data(), png.size()), from, cover::kRendererVersion};
      stage_file(L"tile.png", as_text(png));
      record_changed = true;
    } else if (!(cur.tile_current() && cur.rec.tile->from == from)) {
      std::vector<uint8_t> png = tile_from_files(dir, next);
      rec.tile = TileRec{md5_hex(png.data(), png.size()), from, cover::kRendererVersion};
      stage_file(L"tile.png", as_text(png));
      record_changed = true;
    }
    if (record_changed) stage_file(L"cover.json", render_record(rec, p.id));
    if (out.files.empty()) {
      discard_staged_cover(out);
      return out;
    }
    // Every file the record names is sound: staged now, or checked above.
    out.catalog = to_catalog(sound_state(rec), p.id);
  } catch (const ImportError& e) {
    discard_staged_cover(out);
    if (e.status() == Status::cancelled) throw;
    if (c.log) c.log(std::string("the cover of ") + p.title + " stays as it was: " + e.what());
    out.catalog = to_catalog(initial, p.id);
  } catch (const std::exception& e) {
    discard_staged_cover(out);
    if (c.log) c.log(std::string("the cover of ") + p.title + " stays as it was: " + e.what());
    out.catalog = to_catalog(initial, p.id);
  }
  return out;
}

bool commit_staged_cover(const fs::path& win, const Package& p, StagedCover& s, const LogFn& log) {
  if (s.stage.empty()) return true;
  bool ok = true;
  fs::path dir = cover_dir(win, p);
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const std::wstring& name : s.files) {
    DWORD err = 0;
    if (!move_with_retry(s.stage / name, dir / name, MOVEFILE_REPLACE_EXISTING, err)) {
      if (log)
        log("could not put the cover of " + std::string(p.title) + " in place (" + to_utf8(name) +
            "): " + win_error_string(err));
      ok = false;
      break;
    }
  }
  discard_staged_cover(s);
  return ok;
}

void discard_staged_cover(StagedCover& s) {
  if (!s.stage.empty()) remove_tree(s.stage);
  s.stage.clear();
  s.files.clear();
}

}  // namespace covers_detail

// ---- the public API (covers.h) ----------------------------------------------------------------

using namespace covers_detail;

CoverInfo cover_info(const std::string& id, const fs::path& assets_root, std::span<const Package> registry) {
  CoverInfo info;
  info.package = id;
  const Package* p = find_package(id, registry);
  if (!p) return info;
  // Whether it is installed does not depend on its cover: established first.
  const fs::path win = win_assets_dir(assets_of(assets_root));
  info.installed = is_installed(win, *p);
  // A cover that cannot be read shows as generated, and says why.
  try {
    const fs::path dir = cover_dir(win, *p);
    const State st = load_state(dir, id);
    fill_info(info, st, dir, *p);
    info.error = st.error;
  } catch (const std::exception& e) {
    info.error = std::string("cannot read the cover of ") + p->title + ": " + e.what();
  }
  return info;
}

namespace {

// The package for a cover command: known and installed, else ImportError(error).
const Package& installed_package(const std::string& id, const fs::path& win, std::span<const Package> registry) {
  const Package* p = find_package(id, registry);
  if (!p) throw ImportError(Status::error, "unknown package \"" + id + "\" (see --list-packages)");
  if (!is_installed(win, *p)) throw ImportError(Status::error, std::string(p->title) + " isn't imported");
  return *p;
}

Fetch fetch_for(const CoverOptions& o) {
  Fetch f;
  f.download_dir = o.download_dir;
  f.timeout_ms = o.timeout_ms;
  f.cancel = o.cancel;
  f.log = o.log;
  f.progress = [&o](uint64_t done, uint64_t total, const std::string& item) {
    if (is_cancelled(o.cancel)) throw ImportError(Status::cancelled, "cancelled");
    if (!o.progress) return;
    Progress p;
    p.phase = Progress::Phase::cover;
    p.done = done;
    p.total = total;
    p.item = item;
    if (!o.progress(p)) throw ImportError(Status::cancelled, "cancelled");
  };
  return f;
}

bool same_cover(const CoverInfo& a, const CoverInfo& b) { return a.origin == b.origin && a.tile_md5 == b.tile_md5; }

}  // namespace

CoverResult set_cover(const std::string& id, const fs::path& picture, const CoverOptions& o) {
  CoverResult r;
  ComScope com;
  const std::span<const Package> registry = registry_or_builtin(o.registry);
  const fs::path assets = assets_of(o.assets_root);
  try {
    const fs::path win = win_assets_dir(assets);
    const Package& p = installed_package(id, win, registry);
    fetch_for(o).progress(0, 0, kUserLabel);
    // The picture first: an unreadable one is the user's to fix (2), and
    // decoding takes no lock.
    Picture pic = cover::normalize(cover::read_picture_file(picture));
    std::vector<uint8_t> user_png = cover::encode_png(pic, true);
    std::vector<uint8_t> tile_png = cover::encode_png(cover::render_tile(pic, "picture"), false);

    Handle lock = lock_win_dir(win);
    recover(win, registry, o.log);
    const fs::path dir = cover_dir(win, p);
    fs::create_directories(dir);
    State st = load_state(dir, id);
    CoverInfo before = cover_info(id, assets, registry);
    if (st.damaged) {
      if (o.log) o.log(st.error + "; starting a new one");
      st = State{};
    }
    drop_unsound(st, p, o.log);
    Record rec = st.rec;
    rec.user = UserRec{md5_hex(user_png.data(), user_png.size()), pic.w, pic.h, to_utf8(picture.filename().wstring()),
                       utc_now()};
    rec.tile = TileRec{md5_hex(tile_png.data(), tile_png.size()), "user", cover::kRendererVersion};
    publish(dir, L"user.png", as_text(user_png));
    publish(dir, L"tile.png", as_text(tile_png));
    publish(dir, L"cover.json", render_record(rec, id));
    rewrite_catalog(win, registry, o.log);
    r.info = cover_info(id, assets, registry);
    r.changed = !same_cover(before, r.info);
    r.status = Status::ok;
    r.message = "the cover of " + std::string(p.title) + " is your own picture (" +
                to_utf8(picture.filename().wstring()) + ", " + std::to_string(pic.w) + "x" + std::to_string(pic.h) + ")";
    return r;
  } catch (const ImportError& e) {
    r.status = e.status();
    r.message = e.what();
  } catch (const std::exception& e) {
    r.status = Status::error;
    r.message = e.what();
  }
  r.info = cover_info(id, assets, registry);
  return r;
}

CoverResult clear_cover(const std::string& id, const CoverOptions& o) {
  CoverResult r;
  ComScope com;
  const std::span<const Package> registry = registry_or_builtin(o.registry);
  const fs::path assets = assets_of(o.assets_root);
  try {
    const fs::path win = win_assets_dir(assets);
    const Package& p = installed_package(id, win, registry);
    Handle lock = lock_win_dir(win);
    recover(win, registry, o.log);
    const fs::path dir = cover_dir(win, p);
    State st = load_state(dir, id);
    if (st.damaged || !st.rec.user) {
      r.status = Status::ok;
      r.message = std::string(p.title) + " has no picture of yours: nothing to clear";
      r.info = cover_info(id, assets, registry);
      return r;
    }
    CoverInfo before = cover_info(id, assets, registry);
    st.rec.user.reset();
    st.user_ok = false;
    drop_unsound(st, p, o.log);
    if (st.original_ok) {
      std::vector<uint8_t> png = tile_from_files(dir, st);
      st.rec.tile = TileRec{md5_hex(png.data(), png.size()), "original", cover::kRendererVersion};
      publish(dir, L"tile.png", as_text(png));
    } else {
      st.rec.tile.reset();
    }
    publish(dir, L"cover.json", render_record(st.rec, id));
    remove_file(dir / L"user.png");
    if (!st.rec.tile) remove_file(dir / L"tile.png");
    rewrite_catalog(win, registry, o.log);
    r.info = cover_info(id, assets, registry);
    r.changed = !same_cover(before, r.info);
    r.status = Status::ok;
    r.message = "the cover of " + std::string(p.title) + " is " + describe(r.info) + " again";
    return r;
  } catch (const ImportError& e) {
    r.status = e.status();
    r.message = e.what();
  } catch (const std::exception& e) {
    r.status = Status::error;
    r.message = e.what();
  }
  r.info = cover_info(id, assets, registry);
  return r;
}

std::vector<CoverResult> refresh_covers(const std::vector<std::string>& ids, const CoverOptions& o) {
  std::vector<CoverResult> out;
  ComScope com;
  const std::span<const Package> registry = registry_or_builtin(o.registry);
  const fs::path assets = assets_of(o.assets_root);
  const fs::path win = win_assets_dir(assets);
  // Which packages: every installed one, or those named (registry order).
  std::vector<const Package*> targets;
  std::vector<CoverResult> refused;
  for (const std::string& id : ids) {
    CoverResult r;
    r.info.package = id;
    const Package* p = find_package(id, registry);
    if (!p) r.message = "unknown package \"" + id + "\" (see --list-packages)";
    else if (!is_installed(win, *p)) r.message = std::string(p->title) + " isn't imported";
    if (!r.message.empty()) {
      r.status = Status::error;
      r.info = cover_info(id, assets, registry);
      refused.push_back(std::move(r));
    }
  }
  for (const Package& p : registry) {
    bool named = ids.empty();
    for (const std::string& id : ids) named = named || id == p.id;
    if (named && is_installed(win, p)) targets.push_back(&p);
  }
  const bool allow_download = o.allow_download && downloads_allowed_by_environment();
  if (!targets.empty()) {
    try {
      Handle lock = lock_win_dir(win);
      recover(win, registry, o.log);
      Fetch f = fetch_for(o);
      bool network_down = false, wrote = false;
      for (const Package* pp : targets) {
        const Package& p = *pp;
        CoverResult r;
        r.info.package = p.id;
        try {
          const fs::path dir = cover_dir(win, p);
          CoverInfo before = cover_info(p.id, assets, registry);
          State st = load_state(dir, p.id);
          bool record_changed = false;
          if (st.damaged) {
            if (o.log) o.log(st.error + "; starting a new one");
            st = State{};
            record_changed = true;
          }
          record_changed = drop_unsound(st, p, o.log) || record_changed;
          const int k = o.force ? kNoOriginal : stored_rank(p, st);
          f.progress(0, 0, "");
          Tried t = try_sources(p, k, nullptr, allow_download, network_down, f);
          Record rec = st.rec;
          std::optional<Picture> fresh;
          fs::create_directories(dir);
          if (t.got) {
            std::vector<uint8_t> png = cover::encode_png(t.got->pic, true);
            OriginalRec rr = t.got->rec;
            rr.md5 = md5_hex(png.data(), png.size());
            rr.width = t.got->pic.w;
            rr.height = t.got->pic.h;
            rr.captured_utc = utc_now();
            publish(dir, L"original.png", as_text(png));
            rec.original = rr;
            fresh = std::move(t.got->pic);
            record_changed = true;
          }
          if (t.tried) {
            rec.attempts_utc = utc_now();
            rec.failed = t.failed;
            record_changed = true;
          }
          State next = st;
          next.rec = rec;
          next.original_ok = rec.original.has_value();
          std::string from = next.want_from();
          if (from.empty()) {
            if (rec.tile) rec.tile.reset(), record_changed = true;
          } else if (fresh && from == "original") {
            std::vector<uint8_t> png = cover::encode_png(cover::render_tile(*fresh, rec.original->art), false);
            rec.tile = TileRec{md5_hex(png.data(), png.size()), from, cover::kRendererVersion};
            publish(dir, L"tile.png", as_text(png));
            record_changed = true;
          } else if (!(st.tile_current() && st.rec.tile->from == from)) {
            std::vector<uint8_t> png = tile_from_files(dir, next);
            rec.tile = TileRec{md5_hex(png.data(), png.size()), from, cover::kRendererVersion};
            publish(dir, L"tile.png", as_text(png));
            record_changed = true;
          }
          if (record_changed) {
            publish(dir, L"cover.json", render_record(rec, p.id));
            wrote = true;
          }
          r.info = cover_info(p.id, assets, registry);
          r.changed = !same_cover(before, r.info);
          if (t.got) {
            r.status = Status::ok;
            r.message = "the cover is now " + describe(r.info);
          } else if (t.download_failures) {
            // Nothing better could be fetched: the cover stays, and says why.
            r.status = Status::network;
            std::string why;
            for (const Attempt& a : t.failed)
              if (a.status != "skipped") why += (why.empty() ? "" : "; ") + a.message;
            r.message = "the download failed (" + why + "); the cover stays " + describe(r.info);
          } else {
            r.status = Status::ok;
            bool any_download = false;
            for (const CoverSource& s : p.covers) any_download = any_download || s.kind == CoverSource::Kind::download;
            r.message = (!allow_download && any_download ? "downloads are off; the cover stays "
                         : r.changed                       ? "repaired: the cover is "
                                                           : "already the best cover this can fetch: ") +
                        describe(r.info);
          }
        } catch (const ImportError& e) {
          r.status = e.status();
          r.message = e.what();
          r.info = cover_info(p.id, assets, registry);
        } catch (const std::exception& e) {
          r.status = Status::error;
          r.message = e.what();
          r.info = cover_info(p.id, assets, registry);
        }
        bool stop = r.status == Status::cancelled;
        out.push_back(std::move(r));
        if (stop) break;
      }
      if (wrote) rewrite_catalog(win, registry, o.log);
    } catch (const ImportError& e) {
      // The lock or the catalog: every package not done yet shares the failure.
      for (const Package* p : targets) {
        bool done = false;
        for (const CoverResult& r : out) done = done || r.info.package == p->id;
        if (done) continue;
        CoverResult r;
        r.status = e.status();
        r.message = e.what();
        r.info = cover_info(p->id, assets, registry);
        out.push_back(std::move(r));
      }
    } catch (const std::exception& e) {
      for (CoverResult& r : out)
        if (r.status == Status::ok) r.status = Status::error, r.message = e.what();
    }
  }
  for (CoverResult& r : refused) out.push_back(std::move(r));
  return out;
}

}  // namespace adw::import
