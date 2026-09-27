#include "importer.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#include "adw/core/data_root.h"
#include "cancel.h"
#include "catalog.h"
#include "covers_internal.h"
#include "download.h"
#include "install.h"
#include "md5.h"
#include "names.h"
#include "source.h"
#include "winutil.h"
#include "zip.h"

namespace adw::import {

namespace fs = std::filesystem;

const char* status_name(Status s) {
  switch (s) {
    case Status::ok: return "ok";
    case Status::error: return "error";
    case Status::source_invalid: return "source invalid";
    case Status::verify_failed: return "verify failed";
    case Status::network: return "network error";
    case Status::cancelled: return "cancelled";
  }
  return "?";
}

const char* phase_name(Progress::Phase p) {
  switch (p) {
    case Progress::Phase::download: return "Downloading";
    case Progress::Phase::check_image: return "Checking image";
    case Progress::Phase::copy: return "Copying";
    case Progress::Phase::verify: return "Verifying";
    case Progress::Phase::cover: return "cover";
    case Progress::Phase::finalize: return "Finishing";
  }
  return "?";
}

std::span<const KnownFile> known_files() { return find_package("deluxe")->manifest; }

// ---- locations ---------------------------------------------------------------

namespace {

fs::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
  CoTaskMemFree(p);
  return out;
}

}  // namespace

fs::path data_folder() {
  // The base as adhostwin and the .scr take it (data_root.h): AD_LOCALAPPDATA,
  // else LOCALAPPDATA, and only when neither is set the known folder. The
  // environment comes first everywhere, so all three programs use the same
  // folder.
  std::wstring base = adw::data_root_base();
  if (base.empty()) base = known_folder(FOLDERID_LocalAppData).wstring();
  const std::wstring d = adw::data_root_path(base);
  return d.empty() ? fs::path(adw::kDataDirName) : fs::path(d);
}

fs::path default_assets_root() {
  // Trimmed the way adhostwin trims it (host/core env.cc), so a stray space
  // in the variable cannot send the import somewhere the host never looks.
  // Set, it is the whole answer: the data folder is not even looked at.
  if (const wchar_t* e = _wgetenv(L"AD_ASSETS_DIR")) {
    std::wstring_view v(e);
    while (!v.empty() && iswspace(v.front())) v.remove_prefix(1);
    while (!v.empty() && iswspace(v.back())) v.remove_suffix(1);
    if (!v.empty()) return fs::path(v);
  }
  return data_folder() / L"assets";
}

fs::path default_download_dir() { return data_folder() / L"downloads"; }

fs::path win_assets_dir(const fs::path& root) {
  // PACKAGES.md §5.2, the host's rule too, so AD_ASSETS_DIR pointing straight
  // at an existing win\ is updated in place rather than growing a win\win\
  // nobody reads — and a win\ that holds only non-Deluxe packages still
  // counts.
  std::error_code ec;
  auto marked = [&](const fs::path& d) {
    return fs::is_directory(d / L"FILES", ec) || fs::is_directory(d / L"packages", ec) ||
           fs::exists(d / L"catalog-win.json", ec);
  };
  fs::path win = root / L"win";
  if (marked(win)) return win;
  if (marked(root)) return root;
  return win;
}

std::optional<fs::path> locate_files_dir(const fs::path& dir) {
  std::error_code ec;
  auto holds_tree = [&](const fs::path& d) {
    return fs::is_directory(d / L"AD40", ec) && fs::is_directory(d / L"ENGINE", ec);
  };
  for (const fs::path& c : {dir / L"ADE" / L"FILES", dir / L"FILES", dir})
    if (holds_tree(c)) return c;
  return std::nullopt;
}

// ---- identification (PACKAGES.md §3) -----------------------------------------------

namespace {

[[noreturn]] void invalid(const std::string& what) { throw ImportError(Status::source_invalid, what); }

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : b.empty() ? a : a + "/" + b; }

struct Identified {
  const Package* pkg = nullptr;
  SourceNode dir;        // tree: the FILES dir; ad3zip: the install dir
  std::string dir_path;  // its path in the source ("ADE/FILES", "INSTALL", "")
};

// tree: a FILES dir (ADE\FILES, FILES or the root) holding the first module
// dir and ENGINE, the marker when there is one, and none of the absent dirs.
std::optional<Identified> tree_location(const SourceFs& fs, const Package& p) {
  for (const char* c : {"ADE/FILES", "FILES", ""}) {
    auto d = fs.find(c);
    if (!d || !d->is_dir) continue;
    auto has_dir = [&](std::string_view n) {
      auto x = fs.child(*d, n);
      return x && x->is_dir;
    };
    if (p.module_dirs.empty() || !has_dir(p.module_dirs[0]) || !has_dir("ENGINE")) continue;
    if (p.marker) {
      auto m = fs.find(join(c, p.marker));
      if (!m || m->is_dir) continue;
    }
    bool clean = true;
    for (const char* a : p.absent)
      if (fs.child(*d, a)) clean = false;
    if (!clean) continue;
    return Identified{&p, std::move(*d), c};
  }
  return std::nullopt;
}

// ad3zip: an install dir (INSTALL, or the root of a floppy or a copy of one)
// holding the InstallShield script, its package list and the two archives
// every AD 3.x install has.
std::optional<Identified> install_location(const SourceFs& fs) {
  for (const char* c : {"INSTALL", ""}) {
    auto d = fs.find(c);
    if (!d || !d->is_dir) continue;
    bool ok = true;
    for (const char* f : {"INSTALL.INS", "SETUP.PKG", "ENGINE.ZIP", "MODMISC.ZIP"}) {
      auto x = fs.child(*d, f);
      if (!x || x->is_dir) ok = false;
    }
    if (ok) return Identified{nullptr, std::move(*d), c};
  }
  return std::nullopt;
}

std::shared_ptr<ZipArchive> load_zip(const SourceFs& fs, const SourceNode& n, const std::string& where) {
  auto data = std::make_shared<std::vector<uint8_t>>(fs.read_all(n, 64ull << 20));
  try {
    return std::make_shared<ZipArchive>(std::move(data), join(where, n.name));
  } catch (const ZipError& e) {
    invalid(e.what());
  }
}

Identified identify(const SourceFs& fs, std::span<const Package> registry, const std::string& image_md5,
                    const std::string& want) {
  std::vector<Identified> matches;
  auto inst = install_location(fs);
  std::shared_ptr<ZipArchive> modmisc;
  if (inst) {
    // Central-directory names are not encrypted: no password needed here.
    modmisc = load_zip(fs, *fs.child(inst->dir, "MODMISC.ZIP"), inst->dir_path);
  }
  for (const Package& p : registry) {
    if (p.recipe == Recipe::tree) {
      if (auto loc = tree_location(fs, p)) matches.push_back(std::move(*loc));
    } else if (inst && modmisc && p.engine_dll && modmisc->find(p.engine_dll)) {
      Identified id = *inst;
      id.pkg = &p;
      matches.push_back(std::move(id));
    }
  }
  const Package* by_md5 = nullptr;
  if (!image_md5.empty())
    for (const Package& p : registry)
      for (const KnownImage& k : p.images)
        if (image_md5 == k.md5) by_md5 = &p;
  const Package* wanted = want.empty() ? nullptr : find_package(want, registry);
  if (by_md5) {
    if (wanted && wanted != by_md5)
      invalid("this image is " + std::string(by_md5->title) + " (by its md5), not " + wanted->title);
    for (Identified& m : matches)
      if (m.pkg == by_md5) return std::move(m);
    invalid("the image has the md5 of " + std::string(by_md5->title) + " but not its contents");
  }
  if (wanted) {
    std::vector<Identified> keep;
    std::string others;
    for (Identified& m : matches) {
      if (m.pkg == wanted) keep.push_back(std::move(m));
      else others += std::string(others.empty() ? "" : ", ") + m.pkg->title;
    }
    if (keep.empty())
      invalid("the source is not " + std::string(wanted->title) + (others.empty() ? "" : " (it looks like " + others + ")"));
    matches = std::move(keep);
  }
  if (matches.empty()) invalid("not a known After Dark disc; known: " + known_releases(registry));
  if (matches.size() > 1) {
    std::string names;
    for (auto& m : matches) names += std::string(names.empty() ? "" : " and ") + m.pkg->title;
    invalid("ambiguous source: it looks like " + names + "; choose one with --package");
  }
  return std::move(matches.front());
}

}  // namespace

const Package* identify_folder(const fs::path& dir, std::string* why, std::span<const Package> registry) {
  try {
    auto src = open_folder(dir);
    return identify(*src, registry_or_builtin(registry), "", "").pkg;
  } catch (const std::exception& e) {
    if (why) *why = e.what();
    return nullptr;
  }
}

// ---- the import ----------------------------------------------------------------

namespace {

// One file to install, whatever the source.
struct Planned {
  std::string rel;  // relative to <win>: "FILES/AD40/TOASTERS.AD", "packages/ad32/AD32/GUTS.AD"
  uint64_t size = 0;
  std::function<void(const Sink&)> read;
  bool has_mtime = false;
  FILETIME mtime{};
  std::string md5;   // filled while copying
  std::string from;  // where it is in the source
};

// The modules' own folder may hold none of these (PACKAGES.md §4.2 I1): an
// older AD_SND beside the modules is found first and OLDMOD16 refuses it.
constexpr const char* kNeverBesideModules[] = {"AD_SND.DLL", "OLDMOD16.DLL", "OLDMOD32.DLL", "ADTASK.DLL", "ADW30.EXE"};

// What ENGINE.ZIP contributes to <root>\ENGINE: the package's sound DLL, its
// module tasker (the AD palettes), the host and its INI (kept for later), and
// EcoLogic. The rest is the AD 3.x host's own machinery.
constexpr const char* kEngineMembers[] = {"AD_SND.DLL", "ADTASK.DLL", "ADW30.EXE", "ADW30.INI", "ECOLOGIC.DLL"};

class Importer {
 public:
  Importer(const ImportOptions& o, ImportResult& r) : o_(o), r_(r) {}

  void set_package(const Package& p) {
    pkg_ = &p;
    root_ = p.root;
    title_ = p.title;
  }
  // What the progress reports name before the source is identified (a
  // download of a known package).
  void set_title(const std::string& t) { title_ = t; }

  void log(const std::string& s) const {
    if (o_.log) o_.log(s);
  }
  void progress(Progress::Phase ph, uint64_t done, uint64_t total, const std::string& item = {}) const {
    if (is_cancelled(o_.cancel)) throw ImportError(Status::cancelled, "cancelled");
    if (!o_.progress) return;
    Progress p;
    p.phase = ph;
    p.done = done;
    p.total = total;
    p.item = item;
    p.package = title_;
    if (!o_.progress(p)) throw ImportError(Status::cancelled, "cancelled");
  }

  // Every planned file passes here, whatever the recipe: the staging budget
  // (ImportOptions::max_files, max_bytes) is enforced while the plan is made,
  // so a source that lists far more than any release is refused before a
  // byte is written and before the plan itself grows large.
  void add(Planned p) {
    std::string key = ascii_upper(p.rel);
    if (!seen_.insert(key).second) throw ImportError(Status::source_invalid, "two source files map to " + p.rel);
    if (plan_.size() >= o_.max_files)
      invalid("the source lists more than " + std::to_string(o_.max_files) +
              " files to copy; no After Dark release has that many (a damaged or crafted source?)");
    if (p.size > o_.max_bytes - std::min(planned_bytes_, o_.max_bytes))
      invalid("the source's files add up to more than " +
              (o_.max_bytes % (1 << 20) ? std::to_string(o_.max_bytes) + " bytes"
                                        : std::to_string(o_.max_bytes >> 20) + " MB") +
              " (at " + p.from + "); no After Dark release is that large (a damaged or crafted source?)");
    planned_bytes_ += p.size;
    plan_.push_back(std::move(p));
  }

  // ---- recipe "tree": copy the FILES dir's copy dirs byte for byte ----
  void plan_tree(const SourceFs& fs, const Identified& id) {
    for (const char* want : pkg_->copy_dirs) {
      auto d = fs.child(id.dir, want);
      if (d && d->is_dir) walk(fs, *d, root_ + "/" + want, join(id.dir_path, d->name), 0);
    }
  }

  void walk(const SourceFs& fs, const SourceNode& dir, const std::string& rel, const std::string& from, int depth) {
    if (depth > 16) invalid("directory nesting too deep at " + rel);
    // One directory listed under two names (or inside itself) would be
    // planned once per name, and at every level: branching^depth copies.
    // No real disc does that; refuse it.
    if (std::string key = fs.dir_key(dir); !key.empty() && !dirs_seen_.insert(key).second)
      invalid("the source lists the folder " + from + " more than once (it is another folder of the source "
              "under a second name; a damaged or crafted image?)");
    for (SourceNode& e : fs.list(dir)) {
      check_component(e.name, rel);
      std::string child = rel + "/" + e.name, child_from = from + "/" + e.name;
      if (e.is_dir) {
        walk(fs, e, child, child_from, depth + 1);
        continue;
      }
      Planned p;
      p.rel = child;
      p.size = e.size;
      p.from = child_from;
      const SourceFs* src = &fs;
      p.read = [src, e](const Sink& sink) { src->read(e, sink); };
      if (e.mtime) p.has_mtime = true, p.mtime = *e.mtime;
      add(std::move(p));
    }
  }

  // ---- recipe "ad3zip": the InstallShield placement, flattened (§4.3) ----
  void plan_ad3zip(const SourceFs& fs, const Identified& id) {
    const Package& p = *pkg_;
    const std::string M = root_ + "/" + p.module_dir, E = root_ + "/ENGINE";
    struct Zip {
      std::string name;  // upper case
      std::shared_ptr<ZipArchive> zip;
    };
    // Only the archives and the script are read (I5): the Simpsons floppy's
    // CEREAL.TXT and SERIAL.TXT, the owner's notes, are never opened.
    std::vector<Zip> zips;
    for (SourceNode& n : fs.list(id.dir))
      if (!n.is_dir && ends_with_i(n.name, ".ZIP")) zips.push_back({ascii_upper(n.name), load_zip(fs, n, id.dir_path)});
    std::sort(zips.begin(), zips.end(), [](const Zip& a, const Zip& b) { return a.name < b.name; });
    auto find_zip = [&](std::string_view name) -> const Zip* {
      for (const Zip& z : zips)
        if (iequals(z.name, name)) return &z;
      return nullptr;
    };
    std::string missing;
    for (const char* a : p.required_archives)
      if (!find_zip(a)) missing += std::string(missing.empty() ? "" : ", ") + a;
    if (!missing.empty())
      invalid("the source is missing " + missing + "; importing " + p.title + " needs every install disk");

    // The archive password, derived from the script (never stored or logged).
    auto ins = fs.child(id.dir, "INSTALL.INS");
    std::vector<uint8_t> script = fs.read_all(*ins, 16ull << 20);
    std::vector<const ZipArchive*> all;
    for (const Zip& z : zips) all.push_back(z.zip.get());
    auto pw = derive_password(script, all);
    if (!pw) invalid("cannot find the archive password in " + join(id.dir_path, "INSTALL.INS"));
    password_ = std::move(*pw);
    log("recovered the archive password from " + join(id.dir_path, "INSTALL.INS"));

    const bool musicg = find_zip("MUSICG.ZIP") != nullptr;
    for (const Zip& z : zips) {
      auto put = [&](const ZipMember& m, const std::string& dir, const std::string& as = {}) {
        Planned pl;
        pl.rel = dir + "/" + (as.empty() ? ascii_upper(m.name) : as);
        pl.size = m.usize;
        pl.from = join(id.dir_path, z.name) + "!" + m.name;
        std::shared_ptr<ZipArchive> zip = z.zip;
        const std::string* pw = &password_;
        pl.read = [zip, m, pw](const Sink& sink) {
          try {
            zip->extract(m, *pw, sink);
          } catch (const ZipError& e) {
            invalid(e.what());  // a damaged member is a corrupt source, never a verify failure
          }
        };
        if (auto t = dos_filetime(m.mod_date, m.mod_time)) pl.has_mtime = true, pl.mtime = *t;
        add(std::move(pl));
      };
      const std::string& N = z.name;
      const auto& members = z.zip->members();
      if (N == "MODMISC.ZIP") {
        for (const ZipMember& m : members)
          if (!iequals(m.name, "EDITFILE.TXT")) put(m, M);
      } else if (N == "WIN.ZIP") {
        // The installer put AD_RSRC.DLL in C:\WINDOWS; the modules find it beside them.
        if (const ZipMember* m = z.zip->find("AD_RSRC.DLL")) put(*m, M);
      } else if (N == "BITMAPS.ZIP" || N == "TRACES.ZIP" || N == "SOUNDS.ZIP") {
        for (const ZipMember& m : members) put(m, M + "/" + N.substr(0, N.size() - 4));
      } else if (N == "MUSICG.ZIP" || (N == "MUSIC.ZIP" && !musicg)) {
        // General MIDI (MUSICG) over the dual-format MUSIC; a sound database
        // DLL goes beside the modules (I4).
        for (const ZipMember& m : members) {
          if (ends_with_i(m.name, ".MID")) put(m, M + "/MUSIC");
          else if (ends_with_i(m.name, ".DLL")) put(m, M);
          else log("skipped " + z.name + "!" + m.name);
        }
      } else if (N == "AFI.ZIP") {
        if (const ZipMember* m = p.folder_afi ? z.zip->find(p.folder_afi) : nullptr) put(*m, M, "FOLDER.AFI");
        else log("no " + std::string(p.folder_afi ? p.folder_afi : "folder AFI") + " in AFI.ZIP");
      } else if (N == "ENGINE.ZIP") {
        for (const char* want : kEngineMembers)
          if (const ZipMember* m = z.zip->find(want)) put(*m, E);
      } else if (N == "HELP.ZIP" || N == "MULTIS.ZIP" || N == "WINSYS.ZIP" || N == "WAVEMIX.ZIP" || N == "MUSIC.ZIP") {
        // Help, MultiModule presets, placeholders, sound drivers, the other MIDI set.
      } else if (std::any_of(members.begin(), members.end(), [](const ZipMember& m) { return ends_with_i(m.name, ".AD"); })) {
        for (const ZipMember& m : members) put(m, M);
      } else {
        log("skipped " + z.name + " (not in the recipe of " + p.title + ")");
      }
    }
  }

  void check_required() const {
    std::vector<std::string> missing;
    for (const char* req : pkg_->required) {
      std::string rel = root_ + "/" + req;
      if (!seen_.count(ascii_upper(rel))) missing.push_back(rel);
    }
    if (missing.empty()) return;
    std::string m = "source is missing required file(s):";
    for (auto& s : missing) m += " " + s;
    throw ImportError(Status::source_invalid, m);
  }

  // Before anything is written: a planned file under a path the release's
  // manifest lists, with another size, can only be a mismatch, which fails
  // the import anyway once it is verified (classify). Refusing it here keeps
  // an oversized file (a ZIP member recording 4 GB) from being streamed to
  // disk first.
  void check_sizes() const {
    if (!o_.check_known || pkg_->manifest.empty()) return;
    std::map<std::string_view, uint64_t> sizes;
    for (const KnownFile& k : pkg_->manifest) sizes.emplace(k.path, k.size);
    size_t mismatches = 0;
    std::string first_bad;
    for (const Planned& p : plan_) {
      auto it = sizes.find(p.rel);
      if (it == sizes.end() || it->second == p.size) continue;
      if (!mismatches++) first_bad = p.rel + " is " + std::to_string(p.size) + " bytes, not " + std::to_string(it->second);
    }
    if (mismatches)
      throw ImportError(Status::verify_failed, std::to_string(mismatches) + " file(s) differ from the release of " +
                                                   title_ + " (first: " + first_bad +
                                                   "); use --no-verify to import them anyway");
  }

  fs::path staged(const fs::path& stage, const std::string& rel) const {
    return stage / to_wide(rel.substr(root_.size() + 1));
  }

  void copy_to(const fs::path& stage) {
    std::sort(plan_.begin(), plan_.end(), [](const Planned& a, const Planned& b) { return a.rel < b.rel; });
    uint64_t total = 0, done = 0;
    for (auto& p : plan_) total += p.size;
    for (Planned& p : plan_) {
      fs::path out = staged(stage, p.rel);
      fs::create_directories(out.parent_path());
      Handle w(CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
      if (!w.valid())
        throw ImportError(Status::error, "cannot create " + to_utf8(out.wstring()) + ": " +
                                             win_error_string(GetLastError()));
      Md5 h;
      uint64_t got = 0;
      progress(Progress::Phase::copy, done, total, p.rel);
      p.read([&](const uint8_t* data, size_t n) {
        // No more than the source listed: what it stages is what was budgeted.
        if (n > p.size - got)
          throw ImportError(Status::source_invalid, p.rel + ": the source holds more than the " +
                                                        std::to_string(p.size) + " bytes it lists");
        h.update(data, n);
        DWORD wrote = 0;
        if (!WriteFile(w.get(), data, DWORD(n), &wrote, nullptr) || wrote != n)
          throw ImportError(Status::error, "write failed on " + to_utf8(out.wstring()) + ": " +
                                               win_error_string(GetLastError()));
        got += n;
        done += n;
        progress(Progress::Phase::copy, done, total, p.rel);
      });
      if (got != p.size)
        throw ImportError(Status::source_invalid, p.rel + ": read " + std::to_string(got) + " bytes, expected " +
                                                      std::to_string(p.size));
      if (p.has_mtime) SetFileTime(w.get(), nullptr, nullptr, &p.mtime);
      p.md5 = h.finish_hex();
    }
  }

  // Re-reads every staged file: the md5 recorded in import.json is then the
  // md5 of what is on disk, not merely of what we meant to write.
  void verify_stage(const fs::path& stage) {
    uint64_t total = 0, done = 0;
    for (auto& p : plan_) total += p.size;
    for (Planned& p : plan_) {
      fs::path f = staged(stage, p.rel);
      uint64_t base = done;
      std::string md5 = md5_file_hex(f, [&](uint64_t d, uint64_t) {
        try {
          progress(Progress::Phase::verify, base + d, total, p.rel);
          return true;
        } catch (const ImportError&) {
          return false;
        }
      });
      done += p.size;
      if (md5 != p.md5)
        throw ImportError(Status::verify_failed,
                          p.rel + ": staged copy reads back as " + md5 + ", source was " + p.md5);
    }
  }

  // Every file against the package's manifest; the fix-ups (made from files
  // that matched) in between, so the manifest's entries for them count too.
  void classify(const fs::path& stage, bool image_known) {
    std::map<std::string, const KnownFile*> known;
    for (const KnownFile& k : pkg_->manifest) known[k.path] = &k;
    size_t mismatches = 0, unknown = 0;
    std::string first_bad;
    auto judge = [&](ImportedFile& f) {
      auto it = known.find(f.path);
      if (it == known.end()) {
        f.known = ImportedFile::Known::unknown;
        unknown++;
      } else if (it->second->size == f.size && f.md5 == it->second->md5) {
        f.known = ImportedFile::Known::match;
        known.erase(it);
      } else {
        f.known = ImportedFile::Known::mismatch;
        if (!mismatches++) first_bad = f.path;
        known.erase(it);
      }
    };
    for (const Planned& p : plan_) {
      ImportedFile f;
      f.path = p.rel;
      f.size = p.size;
      f.md5 = p.md5;
      f.from = p.from;
      judge(f);
      r_.files.push_back(std::move(f));
    }
    for (ImportedFile& f : apply_fixups(stage, image_known)) {
      judge(f);
      r_.files.push_back(std::move(f));
    }
    std::sort(r_.files.begin(), r_.files.end(), [](const ImportedFile& a, const ImportedFile& b) { return a.path < b.path; });
    for (auto& [path, k] : known) r_.missing_known.push_back(path);

    if (image_known) {
      r_.verified = "image";
    } else if (pkg_->manifest.empty() || !o_.check_known) {
      r_.verified = "none";
    } else if (!mismatches && !unknown && r_.missing_known.empty()) {
      // Every file of the release is here and matched ("files" never means a
      // subset: a source that lacks some of them is "partial").
      r_.verified = "files";
    } else {
      r_.verified = "partial";
    }
    if (!r_.missing_known.empty())
      log(std::to_string(r_.missing_known.size()) + " file(s) of the release of " + title_ + " are not in this source");
    if (mismatches && o_.check_known)
      throw ImportError(Status::verify_failed, std::to_string(mismatches) + " file(s) differ from the release of " + title_ +
                                                   " (first: " + first_bad +
                                                   "); use --no-verify to import them anyway");
  }

  // §4.3: copies under the names the modules open, made only from a source
  // file that matched the release (or came from its known image).
  std::vector<ImportedFile> apply_fixups(const fs::path& stage, bool image_known) {
    std::vector<ImportedFile> out;
    for (const Fixup& fx : pkg_->fixups) {
      std::string src_rel = root_ + "/" + fx.copy_of, dst_rel = root_ + "/" + fx.create;
      const ImportedFile* src = nullptr;
      for (const ImportedFile& f : r_.files)
        if (f.path == src_rel) src = &f;
      if (!src) {
        log("fix-up " + dst_rel + " skipped: the source has no " + src_rel);
        continue;
      }
      if (!image_known && src->known != ImportedFile::Known::match) {
        log("fix-up " + dst_rel + " skipped: " + src_rel + " is not the release's");
        continue;
      }
      if (seen_.count(ascii_upper(dst_rel))) {
        log("fix-up " + dst_rel + " skipped: the source already has that file");
        continue;
      }
      fs::path from = staged(stage, src_rel), to = staged(stage, dst_rel);
      fs::create_directories(to.parent_path());
      if (!CopyFileW(from.c_str(), to.c_str(), TRUE))
        throw ImportError(Status::error, "cannot create " + to_utf8(to.wstring()) + ": " + win_error_string(GetLastError()));
      seen_.insert(ascii_upper(dst_rel));
      ImportedFile f;
      f.path = dst_rel;
      f.size = src->size;
      f.md5 = md5_file_hex(to);
      if (f.md5 != src->md5) throw ImportError(Status::verify_failed, dst_rel + ": the copy differs from " + src_rel);
      f.from = std::string("alias:") + fx.copy_of;
      out.push_back(std::move(f));
    }
    return out;
  }

  // §4.2 I1–I4 over the staged package (not Deluxe, whose layout is its own).
  void check_invariants(const fs::path& stage) const {
    const Package& p = *pkg_;
    std::set<std::string> have;  // upper case, relative to the package root
    for (const ImportedFile& f : r_.files) have.insert(ascii_upper(f.path.substr(root_.size() + 1)));
    std::vector<std::string> folders;
    for (const char* d : p.module_dirs)
      if (std::string_view(d) != "ENGINE") folders.push_back(ascii_upper(d));
    auto fail = [&](const std::string& inv, std::string what) {
      for (char& c : what)
        if (c == '/') c = '\\';
      invalid("the layout of " + std::string(p.title) + " breaks " + inv + ": " + what);
    };
    for (const std::string& d : folders)
      for (const char* n : kNeverBesideModules)
        if (have.count(d + "/" + n)) fail("I1", d + "\\" + n + " would shadow the engine's");
    if (!have.count("ENGINE/AD_SND.DLL")) fail("I3", "no ENGINE\\AD_SND.DLL");
    if (!(have.count("ENGINE/OLDMOD16.DLL") && have.count("ENGINE/AFTERDAR.SCR")) && !have.count("ENGINE/ADTASK.DLL"))
      fail("I3", "no ENGINE\\OLDMOD16.DLL + AFTERDAR.SCR, and no ENGINE\\ADTASK.DLL");
    CatalogTree t;
    t.package = &p;
    t.dir = stage;
    for (const CatalogModule& m : build_catalog({t}).modules) {
      std::string rel = ascii_upper(m.path.substr(root_.size() + 1));
      std::string folder = rel.substr(0, rel.rfind('/'));
      for (const std::string& need : m.needs) {
        std::string n = ascii_upper(need);
        if (n == "AD_SND" || n == "AD_SND.DLL") continue;
        if (!have.count(folder + "/" + n) && !have.count(folder + "/" + n + ".DLL"))
          fail("I2", rel + " needs " + need + ", which is not beside it");
      }
    }
    for (const std::string& f : have) {
      std::string name = f.substr(f.rfind('/') + 1);
      if (ends_with_i(name, "_SND.DLL") && name != "AD_SND.DLL") {
        bool beside = false;
        for (const std::string& d : folders) beside = beside || have.count(d + "/" + name);
        if (!beside) fail("I4", "the sound database " + name + " is not in a module folder");
      }
      if (p.recipe == Recipe::ad3zip && ends_with_i(name, ".MID") &&
          f.rfind(ascii_upper(std::string(p.module_dir)) + "/MUSIC/", 0) != 0)
        fail("I4", f + " is not in " + p.module_dir + "\\MUSIC");
    }
  }

  const std::vector<Planned>& plan() const { return plan_; }
  const ImportOptions& options() const { return o_; }

 private:
  const ImportOptions& o_;
  ImportResult& r_;
  const Package* pkg_ = nullptr;
  std::string root_, title_;
  std::vector<Planned> plan_;
  std::set<std::string> seen_;
  std::set<std::string> dirs_seen_;  // SourceFs::dir_key of every folder walked
  uint64_t planned_bytes_ = 0;
  std::string password_;  // in memory only
};

std::string json_escape(const std::string& s) {
  std::string o;
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
  return o;
}

std::string utc_now_iso8601() {
  SYSTEMTIME st;
  GetSystemTime(&st);
  char b[32];
  snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  return b;
}

const char* known_name(ImportedFile::Known k) {
  switch (k) {
    case ImportedFile::Known::match: return "match";
    case ImportedFile::Known::mismatch: return "mismatch";
    default: return "unknown";
  }
}

// Deluxe's import.json (version 1, unchanged): provenance for the tree next
// to it. Deterministic field and file order so two imports of the same
// source diff cleanly.
std::string render_import_json_v1(const Source& src, const ImportResult& r, const std::string& final_url,
                                  const std::string& volume_id, const std::string& utc) {
  const char* kind = src.kind == Source::Kind::iso ? "iso" : src.kind == Source::Kind::folder ? "folder" : "download";
  uint64_t total = 0;
  for (auto& f : r.files) total += f.size;
  std::string j = "{\n";
  j += "  \"version\": 1,\n";
  j += "  \"tool\": \"adimport 1.0\",\n";
  j += "  \"importedUtc\": \"" + utc + "\",\n";
  j += "  \"source\": {\n";
  j += "    \"kind\": \"" + std::string(kind) + "\",\n";
  j += "    \"path\": \"" + json_escape(r.source) + "\"";
  if (src.kind == Source::Kind::download) {
    j += ",\n    \"url\": \"" + json_escape(r.url) + "\"";
    if (!final_url.empty()) j += ",\n    \"finalUrl\": \"" + json_escape(final_url) + "\"";
  }
  if (!r.iso_md5.empty()) {
    j += ",\n    \"isoSize\": " + std::to_string(r.iso_size);
    j += ",\n    \"isoMd5\": \"" + r.iso_md5 + "\"";
    j += ",\n    \"isoMd5Known\": " + std::string(r.iso_md5_known ? "true" : "false");
    j += ",\n    \"joliet\": " + std::string(r.joliet ? "true" : "false");
    j += ",\n    \"volumeId\": \"" + json_escape(volume_id) + "\"";
  }
  j += "\n  },\n";
  j += "  \"verified\": \"" + r.verified + "\",\n";
  j += "  \"fileCount\": " + std::to_string(r.files.size()) + ",\n";
  j += "  \"totalBytes\": " + std::to_string(total) + ",\n";
  j += "  \"missingKnown\": [";
  for (size_t i = 0; i < r.missing_known.size(); i++)
    j += std::string(i ? ", " : "") + "\"" + json_escape(r.missing_known[i]) + "\"";
  j += "],\n";
  j += "  \"files\": [\n";
  for (size_t i = 0; i < r.files.size(); i++) {
    const ImportedFile& f = r.files[i];
    j += "    {\"path\": \"" + json_escape(f.path) + "\", \"size\": " + std::to_string(f.size) + ", \"md5\": \"" +
         f.md5 + "\", \"known\": \"" + known_name(f.known) + "\"}" + (i + 1 < r.files.size() ? ",\n" : "\n");
  }
  j += "  ]\n}\n";
  return j;
}

// Every other package's import.json (version 2, PACKAGES.md §5.3), inside
// its root. Never holds the archive password.
std::string render_import_json_v2(const Source& src, const Package& p, const ImportResult& r, const std::string& utc) {
  const bool image = !r.parts.empty();
  const bool floppy = r.format == "fat12" || r.format == "fat16";
  const bool download = src.kind == Source::Kind::download;
  const char* kind = download ? "download" : !image ? "folder" : floppy ? "floppy" : r.format == "zip" ? "zip" : "iso";
  uint64_t total = 0;
  for (auto& f : r.files) total += f.size;
  std::string j = "{\n";
  j += "  \"version\": 2,\n";
  j += "  \"tool\": \"" + std::string(kToolName) + "\",\n";
  j += "  \"importedUtc\": \"" + utc + "\",\n";
  j += "  \"package\": {\"id\": \"" + json_escape(p.id) + "\", \"title\": \"" + json_escape(p.title) +
       "\", \"recipe\": \"" + (p.recipe == Recipe::tree ? "tree" : "ad3zip") + "\", \"root\": \"" +
       json_escape(p.root) + "\"},\n";
  j += "  \"source\": {\n";
  j += "    \"kind\": \"" + std::string(kind) + "\",\n";
  j += "    \"format\": \"" + json_escape(r.format) + "\",\n";
  j += "    \"path\": \"" + json_escape(r.source) + "\"";
  if (download) {
    j += ",\n    \"url\": \"" + json_escape(r.url) + "\"";
    if (!r.final_url.empty()) j += ",\n    \"finalUrl\": \"" + json_escape(r.final_url) + "\"";
    j += ",\n    \"md5Checked\": " + std::string(r.download_md5_checked ? "true" : "false");
  }
  if (image && r.parts.size() == 1) {
    j += ",\n    \"imageSize\": " + std::to_string(r.iso_size);
    j += ",\n    \"imageMd5\": \"" + r.iso_md5 + "\"";
  }
  if (image) {
    j += ",\n    \"imageMd5Known\": " + std::string(r.iso_md5_known ? "true" : "false");
    j += ",\n    \"volumeId\": \"" + json_escape(r.volume_id) + "\"";
  }
  j += ",\n    \"parts\": [";
  if (r.parts.size() > 1) {
    for (size_t i = 0; i < r.parts.size(); i++)
      j += std::string(i ? ",\n" : "\n") + "      {\"path\": \"" + json_escape(r.parts[i].path) + "\", \"size\": " +
           std::to_string(r.parts[i].size) + ", \"md5\": \"" + r.parts[i].md5 + "\"}";
    j += "\n    ";
  }
  j += "]\n  },\n";
  j += "  \"verified\": \"" + r.verified + "\",\n";
  j += "  \"fileCount\": " + std::to_string(r.files.size()) + ",\n";
  j += "  \"totalBytes\": " + std::to_string(total) + ",\n";
  j += "  \"missingKnown\": [";
  for (size_t i = 0; i < r.missing_known.size(); i++)
    j += std::string(i ? ", " : "") + "\"" + json_escape(r.missing_known[i]) + "\"";
  j += "],\n";
  j += "  \"files\": [\n";
  for (size_t i = 0; i < r.files.size(); i++) {
    const ImportedFile& f = r.files[i];
    j += "    {\"path\": \"" + json_escape(f.path) + "\", \"size\": " + std::to_string(f.size) + ", \"md5\": \"" +
         f.md5 + "\", \"known\": \"" + known_name(f.known) + "\", \"from\": \"" + json_escape(f.from) + "\"}" +
         (i + 1 < r.files.size() ? ",\n" : "\n");
  }
  j += "  ]\n}\n";
  return j;
}

void write_file(const fs::path& p, const std::string& data) {
  DWORD err = 0;
  if (!write_whole_file(p, data, err))
    throw ImportError(Status::error, "cannot write " + to_utf8(p.wstring()) + ": " + win_error_string(err));
}

void remove_quietly(const fs::path& p) { remove_tree(p); }

// A custom --url keeps its own (percent-decoded) last path segment as the
// file name, made safe for Windows: no separators or reserved characters, no
// trailing dots or spaces, never "." or ".." (which would name the downloads
// directory's parent), never a DOS device ("NUL.iso", "COM1": on Windows 10
// <downloads>\CON opens the console): download.iso instead.
std::wstring download_file_name(const std::string& url) {
  std::string u = url.substr(0, url.find_first_of("?#"));
  std::string last = u.substr(u.rfind('/') + 1);
  auto hex = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  std::string dec;
  for (size_t i = 0; i < last.size(); i++) {
    if (last[i] == '%' && i + 2 < last.size() && hex(last[i + 1]) >= 0 && hex(last[i + 2]) >= 0) {
      dec.push_back(char(hex(last[i + 1]) << 4 | hex(last[i + 2])));
      i += 2;
    } else {
      dec.push_back(last[i]);
    }
  }
  std::wstring w = to_wide(dec);
  for (wchar_t& c : w)
    if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
  while (!w.empty() && (w.back() == L'.' || w.back() == L' ')) w.pop_back();
  if (w.empty() || is_device_name(to_utf8(w))) return L"download.iso";
  return w;
}

// One copy of a download: where it is published and what it must be.
struct Mirror {
  std::string url;
  std::wstring file_name;
  uint64_t size = 0;    // 0 = not enforced
  std::string md5;      // "" = unchecked
};

struct Fetched {
  fs::path path;
  std::string url, final_url, md5;
  bool md5_checked = false;
};

// Downloads `src` (the package's registry copies, or its custom URL) into
// the downloads directory. A copy whose file is already there, complete, is
// tried first, so nothing is fetched for a package downloaded before, from
// whichever copy. Each copy is checked against its published size and md5;
// when one cannot be fetched (network) or is not the published file
// (verify_failed), the next is tried. Anything else (cancel, local I/O, the
// download lock) ends it at once.
Fetched fetch_download(const Source& src, std::span<const Package> registry, Importer& imp) {
  const Package* pkg = find_package(src.package.empty() ? "deluxe" : src.package, registry);
  // A custom URL names no package until the source is identified.
  if (pkg && (src.url.empty() || !src.package.empty())) imp.set_title(pkg->title);
  std::vector<Mirror> mirrors;
  if (!src.url.empty()) {
    mirrors.push_back({src.url, download_file_name(src.url), 0, src.expected_md5});
  } else {
    if (!pkg || pkg->downloads.empty())
      throw ImportError(Status::error, "no Internet Archive copy of " + std::string(pkg ? pkg->title : src.package) +
                                           " is known; import it from its disc with --image or --from");
    // --md5 replaces the published md5, and with it the published size.
    for (const Download& d : pkg->downloads)
      mirrors.push_back({d.url, d.file_name, src.expected_md5.empty() ? d.size : 0,
                         src.expected_md5.empty() ? std::string(d.md5) : src.expected_md5});
  }
  const fs::path dir = src.path.empty() ? default_download_dir() : src.path;
  std::error_code ec;
  std::stable_partition(mirrors.begin(), mirrors.end(), [&](const Mirror& m) {
    return m.size && fs::is_regular_file(dir / m.file_name, ec) && fs::file_size(dir / m.file_name, ec) == m.size;
  });

  auto report = [&imp](Progress::Phase ph) {
    return [&imp, ph](uint64_t done, uint64_t total) {
      try {
        imp.progress(ph, done, total);
        return true;
      } catch (const ImportError&) {
        return false;
      }
    };
  };
  Status worst = Status::network;
  std::string failures;
  for (size_t i = 0; i < mirrors.size(); i++) {
    const Mirror& m = mirrors[i];
    DownloadOptions d;
    d.url = m.url;
    d.dest = dir / m.file_name;
    d.expected_md5 = m.md5;
    d.expected_size = m.size;
    d.cancel = imp.options().cancel;
    d.log = [&imp](const std::string& s) { imp.log(s); };
    d.progress = report(Progress::Phase::download);
    // Hashing an already-downloaded file (or the finished transfer) is the
    // same work as checking a local image, and should read that way.
    d.hash_progress = report(Progress::Phase::check_image);
    if (i) imp.log("trying another copy: " + m.url);
    try {
      DownloadResult dr = download(d);
      imp.log(dr.reused ? "using the already-downloaded " + to_utf8(d.dest.wstring())
                        : "downloaded " + std::to_string(dr.size) + " bytes from " + m.url + " to " +
                              to_utf8(d.dest.wstring()));
      return Fetched{d.dest, m.url, dr.final_url, dr.md5, !m.md5.empty()};
    } catch (const ImportError& e) {
      if (e.status() != Status::network && e.status() != Status::verify_failed) throw;
      if (e.status() == Status::verify_failed) worst = Status::verify_failed;
      if (!src.url.empty()) throw;  // a custom URL fails as it always has
      imp.log(m.url + ": " + e.what());
      failures += std::string(failures.empty() ? "" : "; ") + m.url + ": " + e.what();
    }
  }
  // The Internet Archive does withdraw items (ad10th was dark for a while).
  throw ImportError(worst, (mirrors.size() == 1 ? failures
                                                : "none of the " + std::to_string(mirrors.size()) +
                                                      " copies could be used (" + failures + ")") +
                               "; if the Internet Archive no longer has it, import " + pkg->title +
                               " from its disc with --image or --from");
}

// The trees the catalog is built over: every installed package except `pkg`,
// plus `pkg` as it is staged, in registry order.
std::vector<CatalogTree> trees_with_stage(const fs::path& win, std::span<const Package> registry, const Package& pkg,
                                          const fs::path& stage, const std::string& verified, const std::string& utc,
                                          const CatalogCover& cover, const LogFn& log) {
  std::vector<CatalogTree> trees = installed_trees(win, registry, log, &pkg);
  CatalogTree t;
  t.package = &pkg;
  t.dir = stage;
  t.verified = verified;
  t.imported_utc = utc;
  t.cover = cover;
  trees.push_back(std::move(t));
  auto index = [&](const Package* p) { return size_t(p - registry.data()); };
  std::stable_sort(trees.begin(), trees.end(),
                   [&](const CatalogTree& a, const CatalogTree& b) { return index(a.package) < index(b.package); });
  return trees;
}

// --no-cover-download, or AD_COVER_DOWNLOAD=0 (covers_internal.h).
bool cover_downloads_allowed(const ImportOptions& o) {
  return o.cover_download && covers_detail::downloads_allowed_by_environment();
}

}  // namespace

ImportResult run_import(const Source& src, const ImportOptions& opts) {
  ImportResult r;
  const std::span<const Package> registry = registry_or_builtin(opts.registry);
  fs::path assets = opts.assets_root.empty() ? default_assets_root() : opts.assets_root;
  fs::path win = win_assets_dir(assets);
  r.catalog = win / to_wide(kCatalogFileName);
  r.files_dir = win / L"FILES";
  r.import_json = win / L"import.json";
  const std::wstring tag = std::to_wstring(GetCurrentProcessId());
  fs::path stage, old, json_tmp;
  fs::path catalog_tmp = win / (to_wide(kCatalogFileName) + L".tmp-" + tag);
  Importer imp(opts, r);
  bool installed = false;  // the stage has become the package root
  bool deluxe = true;
  covers_detail::StagedCover cover_stage;  // the box cover, moved in with the package (COVERS.md §2.4)

  try {
    if (!src.package.empty() && !find_package(src.package, registry))
      throw ImportError(Status::error, "unknown package \"" + src.package + "\" (known: " + known_releases(registry) + ")");
    std::string final_url;
    std::vector<fs::path> images;
    std::string download_md5;

    if (src.kind == Source::Kind::download) {
      Fetched f = fetch_download(src, registry, imp);
      final_url = f.final_url;
      images.push_back(f.path);
      download_md5 = f.md5;
      r.url = f.url;
      r.final_url = f.final_url;
      r.download_md5_checked = f.md5_checked;
    } else if (src.kind == Source::Kind::iso) {
      images.push_back(src.path);
      for (const fs::path& p : src.more_images) images.push_back(p);
    }

    std::unique_ptr<SourceFs> sfs;
    if (!images.empty()) {
      std::error_code ec;
      uint64_t total = 0;
      for (const fs::path& p : images) {
        // Checked before fs::absolute, which throws on an empty path.
        if (!fs::is_regular_file(p, ec))
          throw ImportError(Status::source_invalid, "no such image file: " + to_utf8(p.wstring()));
        total += fs::file_size(p, ec);
      }
      std::vector<std::unique_ptr<SourceFs>> parts;
      uint64_t base = 0;
      for (size_t i = 0; i < images.size(); i++) {
        const fs::path& p = images[i];
        // The same file named twice is one image, not two floppies.
        bool again = false;
        for (size_t k = 0; k < i && !again; k++) again = fs::equivalent(images[k], p, ec);
        if (again) {
          imp.log("ignoring " + to_utf8(fs::absolute(p).make_preferred().wstring()) + ": it was already given");
          continue;
        }
        ImagePart part;
        part.path = to_utf8(fs::absolute(p).make_preferred().wstring());
        part.size = fs::file_size(p, ec);
        if (!download_md5.empty()) {
          part.md5 = download_md5;
        } else {
          part.md5 = md5_file_hex(p, [&](uint64_t done, uint64_t) {
            try {
              imp.progress(Progress::Phase::check_image, base + done, total);
              return true;
            } catch (const ImportError&) {
              return false;
            }
          });
        }
        base += part.size;
        // A copy of an image already given adds nothing either.
        const ImagePart* same = nullptr;
        for (const ImagePart& q : r.parts)
          if (q.md5 == part.md5 && q.size == part.size) same = &q;
        if (same) {
          imp.log("ignoring " + part.path + ": the same image as " + same->path);
          continue;
        }
        parts.push_back(open_image(p));
        r.parts.push_back(std::move(part));
      }
      if (r.parts.size() > 1) {
        // Several images are read as the disks of one release. Two known
        // images of different releases never are: say so, rather than
        // whichever file the two happen to disagree on first.
        std::string first_id, first_title;
        for (const ImagePart& q : r.parts)
          for (const Package& pk : registry)
            for (const KnownImage& k : pk.images) {
              if (q.md5 != k.md5) continue;
              if (first_id.empty()) {
                first_id = pk.id;
                first_title = pk.title;
              } else if (first_id != pk.id) {
                throw ImportError(Status::source_invalid,
                                  "these images are two different releases (" + first_title + " and " + pk.title +
                                      "); import each image on its own");
              }
            }
      }
      r.source = r.parts.front().path;
      for (size_t i = 1; i < r.parts.size(); i++) r.source += " + " + r.parts[i].path;
      if (r.parts.size() == 1) {
        r.iso_md5 = r.parts.front().md5;
        r.iso_size = r.parts.front().size;
      }
      r.format = parts.front()->format();
      r.volume_id = parts.front()->volume_id();
      for (auto& p : parts)
        if (p->format() != r.format) r.format = "mixed";
      sfs = union_of(std::move(parts));
    } else {
      std::string note;
      sfs = open_folder(src.path, &note);
      if (!note.empty()) imp.log(note);
      r.source = to_utf8(fs::absolute(src.path).make_preferred().wstring());
      r.format = sfs->format();  // "folder", or the disc's format for a CD drive
      r.volume_id = sfs->volume_id();
    }
    r.joliet = r.format == "iso9660+joliet";

    Identified id = identify(*sfs, registry, r.iso_md5, src.package);
    const Package& pkg = *id.pkg;
    deluxe = pkg.is_deluxe();
    imp.set_package(pkg);
    r.package_id = pkg.id;
    r.package_title = pkg.title;
    for (const KnownImage& k : pkg.images)
      if (!r.iso_md5.empty() && r.iso_md5 == k.md5) r.iso_md5_known = true;
    imp.log("identified " + std::string(pkg.title) + (id.dir_path.empty() ? "" : " (" + id.dir_path + ")"));
    if (r.format == "zip")
      imp.log("a ZIP of install files, not an image of the original disks" +
              std::string(opts.check_known ? ": checking every file against the release" : ""));
    else if (!r.iso_md5.empty() && !r.iso_md5_known)
      imp.log("image md5 " + r.iso_md5 + " is not a known image of " + pkg.title +
              (opts.check_known ? "; checking files individually" : ""));

    if (pkg.recipe == Recipe::tree) imp.plan_tree(*sfs, id);
    else imp.plan_ad3zip(*sfs, id);
    imp.check_required();
    imp.check_sizes();

    std::error_code ec;
    fs::create_directories(win, ec);
    if (!fs::is_directory(win, ec)) throw ImportError(Status::error, "cannot create " + to_utf8(win.wstring()));
    // One import per assets root at a time (the .scr's button and a script
    // could both start one); holding the lock is also what makes the sweep
    // below safe — any staging tree it finds is dead.
    Handle lock = lock_win_dir(win);
    recover(win, registry, opts.log);
    const std::string utc = utc_now_iso8601();
    // The box cover, captured after the files are verified and before the
    // catalog is rendered: it never fails the import, and a cancel is
    // honoured as anywhere else before the first rename.
    auto capture_cover = [&] {
      covers_detail::CaptureContext cc;
      cc.win = win;
      cc.package = &pkg;
      cc.source = sfs.get();
      cc.allow_download = cover_downloads_allowed(opts);
      // Empty: default_download_dir(), taken only when a cover is actually
      // downloaded (so an import with --dest touches no data folder otherwise).
      cc.download_dir = !opts.cover_download_dir.empty()                          ? opts.cover_download_dir
                        : src.kind == Source::Kind::download && !src.path.empty() ? src.path
                                                                                  : fs::path();
      cc.timeout_ms = opts.cover_timeout_ms;
      cc.cancel = opts.cancel;
      cc.progress = [&imp](uint64_t done, uint64_t total, const std::string& item) {
        imp.progress(Progress::Phase::cover, done, total, item);
      };
      cc.log = opts.log;
      cover_stage = covers_detail::stage_import_cover(cc);
    };
    // After the package swap: the cover's files go in; should that fail, the
    // catalog is rendered again from what is on disk.
    auto commit_cover = [&](CatalogDoc& doc) {
      if (covers_detail::commit_staged_cover(win, pkg, cover_stage, opts.log)) return;
      doc = write_catalog_tmp(win, installed_trees(win, registry, opts.log), catalog_tmp, opts.log);
    };

    if (deluxe) {
      // ---- Deluxe: exactly the flow it always had (FILES swapped whole) ----
      r.files_dir = win / L"FILES";
      r.import_json = win / L"import.json";
      stage = win / (L"FILES.importing-" + tag);
      old = win / (L"FILES.old-" + tag);
      json_tmp = win / (L"import.json.tmp-" + tag);
      fs::create_directories(stage);
      imp.copy_to(stage);
      imp.verify_stage(stage);
      imp.classify(stage, r.iso_md5_known);
      capture_cover();
      // The catalog describes exactly what will be installed: this tree from
      // the stage, every other package as it is. Modules it cannot read are
      // left out (and logged), never a reason to refuse the files themselves.
      CatalogDoc doc = build_catalog(
          trees_with_stage(win, registry, pkg, stage, r.verified, utc, cover_stage.catalog, opts.log), opts.log);
      r.catalog_modules = doc.modules.size();
      for (auto& m : doc.modules) r.package_modules += m.package == pkg.id;
      for (auto& p : doc.packages) r.installed.push_back(p.title);

      // The last point at which Cancel is honoured: from the first rename on,
      // the import runs to completion (or rolls back) whatever the user presses.
      imp.progress(Progress::Phase::finalize, 0, 1);
      write_file(json_tmp, render_import_json_v1(src, r, final_url, r.volume_id, utc));
      write_file(catalog_tmp, render_catalog(doc));
      bool had_old = fs::exists(r.files_dir, ec);
      DWORD err = 0;
      if (had_old && !move_with_retry(r.files_dir, old, 0, err))
        throw ImportError(Status::error, "cannot replace " + to_utf8(r.files_dir.wstring()) +
                                             " (a file in it is in use — close Long After Dark and try again): " +
                                             win_error_string(err));
      if (!move_with_retry(stage, r.files_dir, 0, err)) {
        DWORD e = err, rollback_err = 0;
        // Put the previous tree back (retried like the forward renames). Should
        // even that fail, it stays in FILES.old-<pid>, where the next import's
        // recovery restores it.
        if (had_old) move_with_retry(old, r.files_dir, 0, rollback_err);
        throw ImportError(Status::error, "cannot move the imported files into place: " + win_error_string(e));
      }
      installed = true;
      if (!move_with_retry(json_tmp, r.import_json, MOVEFILE_REPLACE_EXISTING, err))
        throw ImportError(Status::error, "cannot write " + to_utf8(r.import_json.wstring()) + ": " +
                                             win_error_string(err));
      commit_cover(doc);  // after the record, which a catalog rendered again would read
      if (!move_with_retry(catalog_tmp, r.catalog, MOVEFILE_REPLACE_EXISTING, err))
        throw ImportError(Status::error, "cannot write " + to_utf8(r.catalog.wstring()) + " (close Long After Dark's "
                                             "settings and try again): " + win_error_string(err));
      if (had_old) remove_quietly(old);
    } else {
      // ---- every other package: only packages\<id> is staged and swapped ----
      fs::path live = package_root(win, pkg);
      r.files_dir = live;
      r.import_json = live / L"import.json";
      stage = live;
      stage += L".importing-" + tag;
      old = live;
      old += L".old-" + tag;
      fs::create_directories(stage);
      imp.copy_to(stage);
      imp.verify_stage(stage);
      imp.classify(stage, r.iso_md5_known);
      imp.check_invariants(stage);
      write_file(stage / L"import.json", render_import_json_v2(src, pkg, r, utc));
      capture_cover();
      CatalogDoc doc = build_catalog(
          trees_with_stage(win, registry, pkg, stage, r.verified, utc, cover_stage.catalog, opts.log), opts.log);
      r.catalog_modules = doc.modules.size();
      for (auto& m : doc.modules) r.package_modules += m.package == pkg.id;
      for (auto& p : doc.packages) r.installed.push_back(p.title);

      imp.progress(Progress::Phase::finalize, 0, 1);  // the last point at which Cancel is honoured
      write_file(catalog_tmp, render_catalog(doc));
      bool had_old = fs::exists(live, ec);
      DWORD err = 0;
      if (had_old && !move_with_retry(live, old, 0, err))
        throw ImportError(Status::error, "cannot replace " + to_utf8(live.wstring()) +
                                             " (a file in it is in use — close Long After Dark and try again): " +
                                             win_error_string(err));
      if (!move_with_retry(stage, live, 0, err)) {
        DWORD e = err, rollback_err = 0;
        if (had_old) move_with_retry(old, live, 0, rollback_err);
        throw ImportError(Status::error, "cannot move the imported files into place: " + win_error_string(e));
      }
      installed = true;
      commit_cover(doc);
      if (!move_with_retry(catalog_tmp, r.catalog, MOVEFILE_REPLACE_EXISTING, err))
        throw ImportError(Status::error, "cannot write " + to_utf8(r.catalog.wstring()) + " (close Long After Dark's "
                                             "settings and try again): " + win_error_string(err));
      if (had_old) remove_quietly(old);
    }
    // Committed: a Cancel now must not turn a finished import into a
    // "cancelled" result (the .scr reads exit 5 as "nothing changed").
    if (opts.progress) {
      Progress done;
      done.phase = Progress::Phase::finalize;
      done.done = done.total = 1;
      done.package = r.package_title;
      opts.progress(done);
    }

    uint64_t bytes = 0;
    for (auto& f : r.files) bytes += f.size;
    r.status = Status::ok;
    r.message = "imported " + std::to_string(r.files.size()) + " files (" + std::to_string(bytes) + " bytes) of " +
                r.package_title + ", verified: " + r.verified + ", " + std::to_string(r.package_modules) +
                " modules; catalog: " + std::to_string(r.catalog_modules) + " modules";
  } catch (const ImportError& e) {
    r.status = e.status();
    r.message = e.what();
  } catch (const std::exception& e) {
    r.status = Status::error;
    r.message = e.what();
  }
  if (r.status != Status::ok) {
    // tmps before stage: Deluxe's recovery reads "tmp without stage" as "the
    // stage was installed", so a crash mid-cleanup must not leave that shape.
    // When it *was* installed and only a rename after it failed, the tmps
    // are kept for exactly that reason: the next operation completes them
    // (Deluxe) or rebuilds the catalog (packages).
    std::error_code ec;
    if (!installed) {
      if (!json_tmp.empty()) fs::remove(json_tmp, ec);
      fs::remove(catalog_tmp, ec);
    }
    if (!stage.empty()) remove_quietly(stage);
  }
  covers_detail::discard_staged_cover(cover_stage);  // nothing left of it once committed
  (void)deluxe;
  return r;
}

std::vector<ImportResult> import_downloads(const std::vector<std::string>& ids, const Source& base,
                                           const ImportOptions& opts, const std::function<void(size_t)>& starting) {
  std::vector<ImportResult> out;
  const std::span<const Package> registry = registry_or_builtin(opts.registry);
  for (size_t i = 0; i < ids.size(); i++) {
    const Package* p = find_package(ids[i], registry);
    if (opts.log)
      opts.log("[" + std::to_string(i + 1) + "/" + std::to_string(ids.size()) + "] " + (p ? p->title : ids[i]));
    if (starting) starting(i);
    Source s;
    s.kind = Source::Kind::download;
    s.path = base.path;
    s.package = ids[i];
    out.push_back(run_import(s, opts));
    if (out.back().status == Status::cancelled) break;
  }
  return out;
}

}  // namespace adw::import
