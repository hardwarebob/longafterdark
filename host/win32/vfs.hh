// The guest's view of the file system: a small 1996 machine whose directories
// are mounted onto host directories, shared by both lanes (INTERACTION.md §7,
// §10). Three kinds of mount:
//
//   plain     mount(guest, host, writable): the guest directory IS the host
//             directory (read-only, or written in place). What the lanes used
//             before the state overlay; tests still use it.
//   overlay   mount_overlay(guest, lower, upper): copy-on-write over a
//             read-only lower host directory (may be "": only virtual files)
//             and a writable upper layer that is either a host directory (the
//             persistent per-user state, ADSTATE) or memory ("" upper: the
//             process's life, so headless runs never write).
//   H:        mount_host_drives(short_names): the host's drives, read-only,
//             as H:\<L>\… (§7.4), for paths picked in file dialogs.
//
// Paths are compared case-insensitively (as Windows does) and normalized
// ('/' → '\', "." and ".." resolved, relative paths against the current
// directory). A guest path outside every mount does not exist; file shims fail
// it with ERROR_PATH_NOT_FOUND. The drive root ("C:\") and every directory on
// the way to a mount point or a virtual file exist, empty but for those.
//
// Overlay semantics (§7.3):
//   * lookup: the upper layer, then virtual files, then the lower directory;
//     list() merges all three, the upper winning by name.
//   * any write access (or a create/truncate disposition) opens a copy of the
//     file's current bytes (upper, else virtual/lower: the copy-up) in memory;
//     a memory upper keeps it there, a persistent upper writes it back on
//     flush()/close with a temp file + MoveFileExW(REPLACE_EXISTING |
//     WRITE_THROUGH) under the state mutex, so readers in other hosts always
//     see a whole file. Two handles on one file in this process share the
//     bytes. Last writer wins between processes.
//   * new files and directories go to the upper layer (a persistent upper and
//     its parents are created only when something is written);
//   * removing or renaming a file the lower layer (or a virtual file) has
//     fails with ERROR_ACCESS_DENIED; upper-only files and empty upper-only
//     directories can go, and renames work within one overlay's upper layer.
//   * the state mutex is `Local\LongAfterDark-state-<fnv64 of the lower-cased
//     state root, 16 hex>` (set_state_root; without it, of each upper dir).
//
// Determinism: a memory upper stamps files with a fixed 1996 clock that
// advances one second per write (no clock reads), and listings are sorted by
// upper-cased name with "." and ".." first, as FindFirstFile on NTFS returns
// 8.3 upper-case names.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adw::win32 {

class VfsFile;

namespace vfs_detail {
struct Mount;
struct Resolved;
struct FileBuffer;
}  // namespace vfs_detail

class Vfs {
 public:
  // The largest a file written through an overlay may grow (a MemFile holds
  // it in memory until it is committed to its upper layer): the modules'
  // INI, score and data files are kilobytes; anything beyond is a full disk.
  static constexpr uint64_t kMaxFileSize = 64ull << 20;

  Vfs();
  ~Vfs();
  Vfs(const Vfs&) = delete;
  Vfs& operator=(const Vfs&) = delete;

  // ---- mounts ----
  // guest_dir: "C:\\AFTERDRK"; host_dir: a UTF-8 host path.
  void mount(std::string_view guest_dir, std::string_view host_dir, bool writable);
  // Upper-over-lower overlay (§7.3). lower_host may be "" (only virtual files);
  // upper_host "" = in-memory upper.
  void mount_overlay(std::string_view guest_dir, std::string_view lower_host, std::string_view upper_host);
  // The mount at guest_dir (mounted already) resolves as before — opens,
  // stats, its own listing — but its parent directory's listing leaves it
  // out: an alias of another mount (the Classic lane's C:\AFTERD~1).
  void hide_in_listing(std::string_view guest_dir);
  // Virtual lower-layer file (seeds that are real files, e.g. PROGMAN.INI, *.GRP).
  void add_virtual_file(std::string_view guest_path, std::vector<uint8_t> bytes);
  // H:\<L>\… ↔ <L>:\… (§7.4), read-only; short = 8.3 components for ne16.
  void mount_host_drives(bool short_names);
  // The persistent state root (Env::state_root) the uppers live under: names
  // the state mutex. Optional; without it each upper dir names its own.
  void set_state_root(std::string_view host_root);

  // ---- paths ----
  // Absolute, normalized guest path ("C:\\AFTERDRK\\X.INI").
  std::string full_path(std::string_view guest) const;
  // Where the guest path's bytes are on the host, or "" when no host file
  // backs it (no mount; a memory upper; a virtual file). For an overlay: the
  // upper file when it exists there, else the lower one, else the upper path
  // a write would create (persistent). `writable` reports whether the mount
  // takes writes (overlays do). Readers should prefer open()/read_file().
  std::string to_host(std::string_view guest, bool* writable = nullptr) const;
  // Guest path for a host path under a plain mount or an overlay's layers, or "".
  std::string to_guest(std::string_view host) const;
  // Inside a mount → its form; else H:\<L>\… (8.3 components when the drives
  // were mounted with short names); "" when neither applies (no H:, a UNC path).
  std::string host_to_guest(std::string_view host_path) const;

  const std::string& cwd() const { return cwd_; }
  bool set_cwd(std::string_view guest);  // false if it is not an existing directory

  // ---- files ----
  // One file API for both lanes (KERNEL32 CreateFileA/_lopen/_lcreat/OpenFile,
  // KERNEL/INT 21h 3Ch/3Dh/5Bh): reads come from the upper or lower layer; any
  // write access copies up first (a memory upper holds the bytes in memory).
  // On success *win32_error is 0, or ERROR_ALREADY_EXISTS when create_always /
  // open_always found the file (what CreateFile leaves in GetLastError).
  enum class Access { read, write, read_write };
  enum class Disposition { open_existing, create_always, create_new, open_always, truncate_existing };
  std::unique_ptr<VfsFile> open(std::string_view guest, Access, Disposition, uint32_t* win32_error);
  bool remove(std::string_view guest, uint32_t* win32_error);
  bool rename(std::string_view from, std::string_view to, uint32_t* win32_error);
  bool make_dir(std::string_view guest, uint32_t* win32_error);
  bool remove_dir(std::string_view guest, uint32_t* win32_error);  // an empty upper-only directory

  // Merged directory listing (upper wins by name). pattern: '*' and '?'
  // wildcards, matched case-insensitively against the long or the short name
  // ("" = "*"; "*.*" matches every name). "." and ".." come first below a
  // drive root; the rest are sorted by upper-cased name.
  struct DirEntry {
    std::string name, short_name;  // short_name: the 8.3 alias (upper case; = name when it is 8.3 already)
    uint32_t attributes;           // FILE_ATTRIBUTE_*
    uint64_t size, write_time;     // write_time: a FILETIME
  };
  std::vector<DirEntry> list(std::string_view guest_dir, std::string_view pattern) const;

  // ---- whole-file helpers and metadata (extensions to §10) ----
  enum class Layer : uint8_t { none, upper_memory, upper_host, virtual_file, lower, plain, drive, synthetic_dir };
  struct Stat {
    bool exists = false, dir = false, writable = false;
    Layer layer = Layer::none;
    uint32_t attributes = 0;
    uint64_t size = 0, write_time = 0;
    uint64_t version = 0;  // bumps on every write of a memory-upper file (0 elsewhere)
    std::string host;      // the host file behind it, if any
  };
  // False (and *out = {}) when the path does not exist.
  bool stat(std::string_view guest, Stat* out) const;
  bool exists(std::string_view guest) const;
  bool is_dir(std::string_view guest) const;
  // The file's current bytes (merged view, including a handle still open for
  // writing in this process). False when absent or a directory.
  bool read_file(std::string_view guest, std::string* out) const;
  // Replaces the whole file in the upper layer, atomically (the path's
  // overlay must take writes). Callers doing a read-modify-write hold
  // StateLock around both.
  bool write_file(std::string_view guest, std::string_view bytes, uint32_t* win32_error);

  // Guest paths written this run (created, replaced, renamed to), first-write
  // order, no repeats: the --configure JSON's "written".
  const std::vector<std::string>& written() const { return written_; }
  // True when some overlay has a persistent (host) upper.
  bool persistent() const;
  // The state mutex's name ("" while no persistent upper is mounted).
  std::string state_mutex_name() const;

  // The state mutex, held for a scope (recursive for one thread). A no-op
  // while no persistent upper is mounted. Waits up to 30 s, then goes on
  // (logged): a wedged writer must not wedge every host.
  class StateLock {
   public:
    explicit StateLock(const Vfs& vfs);
    ~StateLock();
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;

   private:
    void* mutex_ = nullptr;
    bool held_ = false;
  };

  // 8.3 helpers (exposed for tests and the ne16 lane).
  static bool is_short_name(std::string_view name);
  // A deterministic alias for `name` among `taken` (upper-cased names and
  // aliases already in the directory): BASIS~N.EXT.
  static std::string make_short_alias(std::string_view name, const std::vector<std::string>& taken);
  static bool wild_match(std::string_view pattern, std::string_view name);

  // ---- internals ----
 private:
  using Mount = vfs_detail::Mount;
  using Resolved = vfs_detail::Resolved;
  using FileBuffer = vfs_detail::FileBuffer;

  Resolved resolve(std::string_view guest) const;
  bool stat_resolved(const Resolved& r, Stat* out) const;
  bool parent_exists(const Resolved& r) const;
  bool lower_has(const Resolved& r) const;
  bool read_resolved(const Resolved& r, const Stat& st, std::string* out) const;
  std::string drive_host_path(const Mount& m, const std::string& rel) const;
  std::string short_host_path(const std::string& host) const;
  bool commit_host(const Mount& m, const std::string& rel, const std::string& guest_full, const std::string& bytes,
                   uint32_t* err);
  bool ensure_host_dirs(const Mount& m, const std::string& rel_dir, uint32_t* err);
  void note_written(const std::string& guest_full);
  uint64_t next_mem_time();
  bool synthetic_dir(const std::string& full) const;
  std::unique_ptr<VfsFile> open_read(const Resolved& r, const Stat& st, uint32_t* err) const;

  std::vector<std::unique_ptr<Mount>> mounts_;  // longest guest prefix first
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> virtual_files_;  // upper-case full path
  std::map<std::string, std::string> virtual_names_;  // upper-case full path → spelling of the name
  // Persistent-upper files open for writing in this process (upper-case host path).
  mutable std::map<std::string, std::weak_ptr<FileBuffer>> open_buffers_;
  std::vector<std::string> written_;
  std::string cwd_ = "C:\\";
  std::string state_root_;
  mutable void* mutex_ = nullptr;
  uint64_t mem_time_;
  uint32_t temp_counter_ = 0;

  friend class StateLock;
};

// An open file (§10). read/write/seek work on bytes; a memory upper or a
// file open for writing in an overlay is a byte buffer, anything else a host
// handle. Destroying it flushes.
class VfsFile {
 public:
  virtual ~VfsFile() = default;
  virtual int64_t read(void* dst, uint32_t n) = 0;       // bytes read, -1 on error
  virtual int64_t write(const void* src, uint32_t n) = 0;
  virtual int64_t seek(int64_t offset, int whence) = 0;  // SEEK_SET/CUR/END → new position (-1 on error)
  virtual uint64_t size() const = 0;
  virtual bool truncate() = 0;                           // at the current position
  virtual bool flush() = 0;                              // persistent upper: atomic replace on close
  // Extensions: the position, whether writes are allowed, the file's times
  // (FILETIMEs; 0 when unknown), and its normalized guest path.
  virtual uint64_t tell() const = 0;
  virtual bool writable() const = 0;
  virtual void times(uint64_t* creation, uint64_t* access, uint64_t* write) const = 0;
  virtual const std::string& guest_path() const = 0;
};

}  // namespace adw::win32
