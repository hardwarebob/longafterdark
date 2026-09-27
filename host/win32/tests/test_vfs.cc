// Unit tests for the shared file layer (INTERACTION.md §7, §10): the Vfs
// overlay (lookup order, copy-up on write, create/delete/rename, merged
// listings, memory mode), the IniStore (seeds ⊕ file, section and key
// deletes, no lost updates between two concurrent writers), and the H: host
// drive mapping with and without 8.3 names. Linked into adw_win32_tests.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "adw/core/text.h"
#include "check.h"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win32;

namespace {

// A scratch directory under %TEMP%, removed (recursively) on destruction.
struct TempDir {
  std::string path;
  TempDir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    static std::atomic<int> n{0};
    path = narrow(tmp) + "adw_vfs_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(n++);
    CreateDirectoryW(widen(path).c_str(), nullptr);
  }
  ~TempDir() { remove_tree(path); }
  static void remove_tree(const std::string& p) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(p + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::string c = p + "\\" + narrow(n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(c);
        else DeleteFileW(widen(c).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(widen(p).c_str());
  }
  std::string sub(const std::string& s) const {
    std::string p = path + "\\" + s;
    CreateDirectoryW(widen(p).c_str(), nullptr);
    return p;
  }
};

void put(const std::string& path, const std::string& bytes) {
  FILE* f = _wfopen(widen(path).c_str(), L"wb");
  fwrite(bytes.data(), 1, bytes.size(), f);
  fclose(f);
}

std::string get(const std::string& path) {
  FILE* f = _wfopen(widen(path).c_str(), L"rb");
  if (!f) return "<absent>";
  std::string s;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
  fclose(f);
  return s;
}

bool host_exists(const std::string& path) { return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string vread(Vfs& v, const std::string& guest) {
  std::string s;
  return v.read_file(guest, &s) ? s : "<absent>";
}

std::vector<std::string> names(const std::vector<Vfs::DirEntry>& es) {
  std::vector<std::string> out;
  for (const auto& e : es) out.push_back(e.name);
  return out;
}

// Writes `bytes` through a VfsFile opened with `disp`.
bool vwrite(Vfs& v, const std::string& guest, const std::string& bytes, Vfs::Disposition disp) {
  uint32_t err = 0;
  auto f = v.open(guest, Vfs::Access::write, disp, &err);
  if (!f) return false;
  return f->write(bytes.data(), uint32_t(bytes.size())) == int64_t(bytes.size()) && f->flush();
}

}  // namespace

// Upper first, then virtual files, then the lower directory.
TEST(vfs_overlay_lookup_order) {
  for (bool persistent : {true, false}) {
    TempDir t;
    std::string lower = t.sub("lower"), upper = t.path + "\\state\\deluxe\\WINDOWS";
    put(lower + "\\A.TXT", "lower-a");
    put(lower + "\\B.TXT", "lower-b");
    put(lower + "\\V.INI", "lower-v");
    Vfs v;
    v.mount_overlay("C:\\WINDOWS", lower, persistent ? upper : "");
    v.add_virtual_file("C:\\WINDOWS\\V.INI", {'v', 'i', 'r', 't'});
    CHECK_EQ(vread(v, "C:\\WINDOWS\\A.TXT"), std::string("lower-a"));
    CHECK_EQ(vread(v, "c:/windows/v.ini"), std::string("virt"));  // virtual beats lower
    CHECK(vwrite(v, "C:\\WINDOWS\\A.TXT", "upper-a", Vfs::Disposition::create_always));
    CHECK_EQ(vread(v, "C:\\WINDOWS\\A.TXT"), std::string("upper-a"));  // upper beats lower
    CHECK_EQ(get(lower + "\\A.TXT"), std::string("lower-a"));         // the lower is never written
    CHECK(vwrite(v, "C:\\WINDOWS\\V.INI", "upper-v", Vfs::Disposition::create_always));
    CHECK_EQ(vread(v, "C:\\WINDOWS\\V.INI"), std::string("upper-v"));  // upper beats virtual
    CHECK_EQ(get(upper + "\\A.TXT"), persistent ? std::string("upper-a") : std::string("<absent>"));
    Vfs::Stat st;
    CHECK(v.stat("C:\\WINDOWS\\B.TXT", &st) && st.layer == Vfs::Layer::lower && st.size == 7);
    CHECK(v.stat("C:\\WINDOWS\\A.TXT", &st) &&
          st.layer == (persistent ? Vfs::Layer::upper_host : Vfs::Layer::upper_memory));
    CHECK(!v.exists("C:\\WINDOWS\\NOPE.TXT"));
    CHECK(v.is_dir("C:\\WINDOWS") && v.is_dir("C:\\"));
    CHECK(!v.exists("D:\\ELSEWHERE"));
    // The state mutex exists only for a persistent upper.
    CHECK_EQ(v.state_mutex_name().empty(), !persistent);
    CHECK(v.state_mutex_name().empty() || v.state_mutex_name().rfind("Local\\LongAfterDark-state-", 0) == 0);
  }
}

// Opening a lower file for writing copies it up; untouched bytes survive; the
// upper and its parents appear only when something is written.
TEST(vfs_copy_up_on_write) {
  TempDir t;
  std::string lower = t.sub("AD40"), upper = t.path + "\\state\\deluxe\\AD40";
  put(lower + "\\DATA.BIN", "0123456789");
  Vfs v;
  v.set_state_root(t.path + "\\state");
  v.mount_overlay("C:\\AFTERDRK", lower, upper);
  CHECK(!host_exists(t.path + "\\state"));
  uint32_t err = 0;
  {
    auto f = v.open("C:\\AFTERDRK\\DATA.BIN", Vfs::Access::read, Vfs::Disposition::open_existing, &err);
    CHECK(f && !f->writable());
  }
  CHECK(!host_exists(t.path + "\\state"));  // reading writes nothing
  {
    auto f = v.open("C:\\AFTERDRK\\DATA.BIN", Vfs::Access::read_write, Vfs::Disposition::open_existing, &err);
    CHECK(f != nullptr);
    CHECK_EQ(f->size(), 10u);
    CHECK_EQ(f->seek(4, SEEK_SET), 4);
    CHECK_EQ(f->write("AB", 2), 2);
    // Another handle in this process sees the bytes before the commit.
    CHECK_EQ(vread(v, "C:\\AFTERDRK\\DATA.BIN"), std::string("0123AB6789"));
    CHECK(f->seek(0, SEEK_END) == 10);
    CHECK_EQ(f->seek(-2, SEEK_END), 8);
    CHECK(f->truncate());
  }
  CHECK_EQ(get(upper + "\\DATA.BIN"), std::string("0123AB67"));
  CHECK_EQ(get(lower + "\\DATA.BIN"), std::string("0123456789"));
  CHECK_EQ(v.written().size(), size_t(1));
  CHECK_EQ(v.written()[0], std::string("C:\\AFTERDRK\\DATA.BIN"));
  // No temp files are left beside the committed one.
  std::vector<std::string> left;
  for (auto& e : v.list("C:\\AFTERDRK", "*")) left.push_back(e.name);
  CHECK(std::none_of(left.begin(), left.end(), [](const std::string& s) { return s.find(".tmp") != std::string::npos; }));
  // Opening for writing without writing still copies up (the file is in the upper).
  put(lower + "\\OTHER.DAT", "x");
  { auto f = v.open("C:\\AFTERDRK\\OTHER.DAT", Vfs::Access::write, Vfs::Disposition::open_existing, &err); }
  CHECK_EQ(get(upper + "\\OTHER.DAT"), std::string("x"));
}

// A guest's seek far out is not an allocation: a file written through an
// overlay stops at Vfs::kMaxFileSize (a full disk), so a write at 0x7FFFFF00
// neither takes 2 GB of host memory nor commits 2 GB to the state folder.
TEST(vfs_file_size_ceiling) {
  TempDir t;
  std::string upper = t.path + "\\state\\deluxe\\WINDOWS";
  Vfs v;
  v.set_state_root(t.path + "\\state");
  v.mount_overlay("C:\\WINDOWS", "", upper);
  uint32_t err = 0;
  {
    auto f = v.open("C:\\WINDOWS\\BIG.DAT", Vfs::Access::read_write, Vfs::Disposition::create_always, &err);
    CHECK(f != nullptr);
    if (f) {
      CHECK_EQ(f->seek(0x7FFFFF00, SEEK_SET), int64_t(0x7FFFFF00));
      CHECK_EQ(f->write("x", 1), int64_t(0));  // the disk is full
      CHECK(!f->truncate());                   // nor can SetEndOfFile grow it there
      CHECK_EQ(f->size(), uint64_t(0));
      // Up to the ceiling a write takes what fits.
      CHECK_EQ(f->seek(int64_t(Vfs::kMaxFileSize) - 2, SEEK_SET), int64_t(Vfs::kMaxFileSize) - 2);
      CHECK_EQ(f->write("abcd", 4), int64_t(2));
      CHECK_EQ(f->size(), Vfs::kMaxFileSize);
      CHECK_EQ(f->write("e", 1), int64_t(0));
      CHECK_EQ(f->seek(0, SEEK_SET), int64_t(0));
      CHECK(f->truncate());
    }
  }
  CHECK_EQ(get(upper + "\\BIG.DAT"), std::string());
}

// An alias mount (the Classic lane's C:\AFTERD~1 over the module folder)
// resolves like its twin, but a listing of C:\ shows only the real one: DOS
// Shell's DIR of C:\ lists what a 1996 install had.
TEST(vfs_hidden_alias_mount) {
  TempDir t;
  std::string mods = t.sub("MODS");
  put(mods + "\\BITMAPS.ADC", "c:\\afterd~1\\bitmaps");
  Vfs v;
  v.mount_overlay("C:\\AFTERDRK", mods, "");
  v.mount_overlay("C:\\AFTERD~1", mods, "");
  v.mount_overlay("C:\\WINDOWS", "", "");
  v.hide_in_listing("C:\\AFTERD~1\\");
  CHECK(names(v.list("C:\\", "*.*")) == (std::vector<std::string>{"AFTERDRK", "WINDOWS"}));
  CHECK(v.list("C:\\", "AFTERD~1").empty());
  CHECK_EQ(vread(v, "C:\\AFTERD~1\\BITMAPS.ADC"), std::string("c:\\afterd~1\\bitmaps"));
  Vfs::Stat st;
  CHECK(v.stat("C:\\afterd~1", &st) && st.dir);
  std::vector<std::string> inside = names(v.list("C:\\AFTERD~1", "*"));
  CHECK(std::find(inside.begin(), inside.end(), "BITMAPS.ADC") != inside.end());
}

// create_new / open_always / delete / rename / directories, both modes.
TEST(vfs_create_delete_rename) {
  for (bool persistent : {true, false}) {
    TempDir t;
    std::string lower = t.sub("lower"), upper = t.path + "\\up";
    put(lower + "\\LOW.TXT", "low");
    Vfs v;
    v.mount_overlay("C:\\AFTERDRK", lower, persistent ? upper : "");
    uint32_t err = 0;
    CHECK(!v.open("C:\\AFTERDRK\\LOW.TXT", Vfs::Access::write, Vfs::Disposition::create_new, &err));
    CHECK_EQ(err, uint32_t(ERROR_FILE_EXISTS));
    CHECK(!v.open("C:\\AFTERDRK\\NOPE\\X.TXT", Vfs::Access::write, Vfs::Disposition::create_always, &err));
    CHECK_EQ(err, uint32_t(ERROR_PATH_NOT_FOUND));
    CHECK(!v.open("C:\\AFTERDRK\\NEW.TXT", Vfs::Access::read, Vfs::Disposition::open_existing, &err));
    CHECK_EQ(err, uint32_t(ERROR_FILE_NOT_FOUND));
    CHECK(vwrite(v, "C:\\AFTERDRK\\New.txt", "new", Vfs::Disposition::create_new));
    CHECK_EQ(vread(v, "C:\\AFTERDRK\\NEW.TXT"), std::string("new"));
    {
      auto f = v.open("C:\\AFTERDRK\\NEW.TXT", Vfs::Access::write, Vfs::Disposition::open_always, &err);
      CHECK(f && err == uint32_t(ERROR_ALREADY_EXISTS));
    }
    // Deleting: the lower's file stays (ACCESS_DENIED); upper-only files go.
    CHECK(!v.remove("C:\\AFTERDRK\\LOW.TXT", &err));
    CHECK_EQ(err, uint32_t(ERROR_ACCESS_DENIED));
    CHECK(v.exists("C:\\AFTERDRK\\LOW.TXT"));
    // Renames within the upper; not from the lower; not onto an existing name.
    CHECK(v.rename("C:\\AFTERDRK\\NEW.TXT", "C:\\AFTERDRK\\MOVED.TXT", &err));
    CHECK(!v.exists("C:\\AFTERDRK\\NEW.TXT"));
    CHECK_EQ(vread(v, "C:\\AFTERDRK\\MOVED.TXT"), std::string("new"));
    CHECK(!v.rename("C:\\AFTERDRK\\LOW.TXT", "C:\\AFTERDRK\\L2.TXT", &err));
    CHECK_EQ(err, uint32_t(ERROR_ACCESS_DENIED));
    CHECK(!v.rename("C:\\AFTERDRK\\MOVED.TXT", "C:\\AFTERDRK\\LOW.TXT", &err));
    CHECK_EQ(err, uint32_t(ERROR_ALREADY_EXISTS));
    CHECK(v.remove("C:\\AFTERDRK\\MOVED.TXT", &err));
    CHECK(!v.exists("C:\\AFTERDRK\\MOVED.TXT"));
    // Directories: made in the upper, files inside, removable when empty.
    CHECK(v.make_dir("C:\\AFTERDRK\\SAVES", &err));
    CHECK(!v.make_dir("C:\\AFTERDRK\\SAVES", &err));
    CHECK_EQ(err, uint32_t(ERROR_ALREADY_EXISTS));
    CHECK(v.is_dir("C:\\AFTERDRK\\SAVES"));
    CHECK(vwrite(v, "C:\\AFTERDRK\\SAVES\\ONE.SAV", "1", Vfs::Disposition::create_always));
    CHECK(!v.remove_dir("C:\\AFTERDRK\\SAVES", &err));
    CHECK_EQ(err, uint32_t(ERROR_DIR_NOT_EMPTY));
    CHECK(v.rename("C:\\AFTERDRK\\SAVES", "C:\\AFTERDRK\\KEPT", &err));
    CHECK_EQ(vread(v, "C:\\AFTERDRK\\KEPT\\ONE.SAV"), std::string("1"));
    CHECK(v.remove("C:\\AFTERDRK\\KEPT\\ONE.SAV", &err));
    CHECK(v.remove_dir("C:\\AFTERDRK\\KEPT", &err));
    CHECK(!v.exists("C:\\AFTERDRK\\KEPT"));
    CHECK(!v.remove_dir("C:\\AFTERDRK", &err));  // the lower's own directory stays
    CHECK_EQ(persistent, host_exists(upper));
    CHECK_EQ(get(lower + "\\LOW.TXT"), std::string("low"));
  }
}

// Listings merge the layers (upper wins by name), include virtual files and
// mount points, put "." and ".." first and sort the rest.
TEST(vfs_merged_listing) {
  for (bool persistent : {true, false}) {
    TempDir t;
    std::string lower = t.sub("lower"), upper = t.path + "\\up";
    put(lower + "\\B.TXT", "lower-b");
    put(lower + "\\D.BMP", "dd");
    CreateDirectoryW(widen(lower + "\\SUB").c_str(), nullptr);
    Vfs v;
    v.mount_overlay("C:\\WINDOWS", lower, persistent ? upper : "");
    v.mount("C:\\WINDOWS\\SYSTEM", t.sub("engine"), false);
    v.add_virtual_file("C:\\WINDOWS\\PROGMAN.INI", {'p'});
    v.add_virtual_file("C:\\WINDOWS\\GRP\\MAIN.GRP", {'g'});
    CHECK(vwrite(v, "C:\\WINDOWS\\A.TXT", "a", Vfs::Disposition::create_always));
    CHECK(vwrite(v, "C:\\WINDOWS\\B.TXT", "upper-b!", Vfs::Disposition::create_always));
    auto all = v.list("C:\\WINDOWS", "*.*");
    std::vector<std::string> want = {".", "..", "A.TXT", "B.TXT", "D.BMP", "GRP", "PROGMAN.INI", "SUB", "SYSTEM"};
    CHECK(names(all) == want);
    for (const auto& e : all) {
      if (e.name == "B.TXT") CHECK_EQ(e.size, 8u);  // the upper's
      if (e.name == "SYSTEM" || e.name == "SUB" || e.name == "GRP") CHECK(e.attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    CHECK(names(v.list("C:\\WINDOWS", "*.TXT")) == (std::vector<std::string>{"A.TXT", "B.TXT"}));
    CHECK(names(v.list("C:\\WINDOWS", "d.bmp")) == (std::vector<std::string>{"D.BMP"}));
    CHECK(names(v.list("C:\\WINDOWS\\GRP", "*")) == (std::vector<std::string>{".", "..", "MAIN.GRP"}));
    // The drive root lists its mount points, without dots.
    CHECK(names(v.list("C:\\", "*")) == (std::vector<std::string>{"WINDOWS"}));
    CHECK(names(v.list("C:\\", "WINDOWS")) == (std::vector<std::string>{"WINDOWS"}));
    CHECK(v.list("C:\\NOPE", "*").empty());
    // 8.3 aliases for long names made in the upper.
    CHECK(vwrite(v, "C:\\WINDOWS\\Long File Name.text", "l", Vfs::Disposition::create_always));
    auto lf = v.list("C:\\WINDOWS", "LONGFI~1.TEX");
    CHECK_EQ(lf.size(), size_t(1));
    if (!lf.empty()) CHECK_EQ(lf[0].name, std::string("Long File Name.text"));
  }
}

// Memory mode: the same semantics, nothing ever reaches the disk.
TEST(vfs_memory_mode_writes_nothing) {
  TempDir t;
  std::string lower = t.sub("lower");
  put(lower + "\\MODULES.INI", "[X]\r\nA=1\r\n");
  Vfs v;
  v.mount_overlay("C:\\WINDOWS", lower, "");
  CHECK(!v.persistent());
  CHECK(vwrite(v, "C:\\WINDOWS\\MODULES.INI", "[X]\r\nA=2\r\n", Vfs::Disposition::create_always));
  CHECK(vwrite(v, "C:\\WINDOWS\\NEW.DAT", "n", Vfs::Disposition::create_new));
  uint32_t err;
  CHECK(v.make_dir("C:\\WINDOWS\\D", &err));
  CHECK_EQ(vread(v, "C:\\WINDOWS\\MODULES.INI"), std::string("[X]\r\nA=2\r\n"));
  CHECK_EQ(get(lower + "\\MODULES.INI"), std::string("[X]\r\nA=1\r\n"));
  CHECK(!host_exists(lower + "\\NEW.DAT") && !host_exists(lower + "\\D"));
  CHECK(v.to_host("C:\\WINDOWS\\NEW.DAT").empty());
  // Memory files carry a fixed clock: two runs stamp the same times.
  Vfs::Stat a, b;
  Vfs w;
  w.mount_overlay("C:\\WINDOWS", lower, "");
  CHECK(vwrite(w, "C:\\WINDOWS\\MODULES.INI", "[X]\r\nA=2\r\n", Vfs::Disposition::create_always));
  v.stat("C:\\WINDOWS\\MODULES.INI", &a);
  w.stat("C:\\WINDOWS\\MODULES.INI", &b);
  CHECK_EQ(a.write_time, b.write_time);
}

// Seeds under the file: the file wins per key, enumeration is file order then
// seeds; deletes edit the file and keep everything else line for line.
TEST(ini_store_seeds_and_deletes) {
  for (bool persistent : {true, false}) {
    TempDir t;
    std::string lower = t.sub("lower"), upper = t.path + "\\up";
    put(lower + "\\WIN.INI", "; comment\r\n[Berkeley Systems]\r\nAfter Dark = D:\\OLD\r\n\r\n[Other]\r\nk=v\r\n");
    Vfs v;
    v.mount_overlay("C:\\WINDOWS", lower, persistent ? upper : "");
    IniStore ini(v);
    const char* W = "C:\\WINDOWS\\WIN.INI";
    ini.add_seed(W, "Berkeley Systems", "AD Ini Files", "C:\\WINDOWS");
    ini.add_seed(W, "Berkeley Systems", "After Dark", "C:\\AFTERDRK");
    ini.add_seed(W, "Seeded", "Only", "1");
    CHECK_EQ(ini.get(W, "berkeley systems", "after dark").value_or("-"), std::string("D:\\OLD"));  // file wins
    CHECK_EQ(ini.get(W, "Berkeley Systems", "AD Ini Files").value_or("-"), std::string("C:\\WINDOWS"));
    CHECK(!ini.get(W, "Berkeley Systems", "Missing"));
    CHECK(ini.sections(W) == (std::vector<std::string>{"Berkeley Systems", "Other", "Seeded"}));
    CHECK(ini.keys(W, "Berkeley Systems") == (std::vector<std::string>{"After Dark", "AD Ini Files"}));
    // Set: an existing key keeps its spelling and place; a new key goes after
    // the section's last line; a new section at the end.
    CHECK(ini.set(W, "BERKELEY SYSTEMS", std::string_view("after dark"), std::string_view("C:\\NEW")));
    CHECK(ini.set(W, "Other", std::string_view("k2"), std::string_view("v2")));
    CHECK(ini.set(W, "Fresh", std::string_view("x"), std::string_view("y")));
    std::string text = vread(v, W);
    CHECK_EQ(text, std::string("; comment\r\n[Berkeley Systems]\r\nAfter Dark=C:\\NEW\r\n\r\n[Other]\r\nk=v\r\nk2=v2\r\n"
                               "[Fresh]\r\nx=y\r\n"));
    CHECK(get(lower + "\\WIN.INI").find("OLD") != std::string::npos);  // the lower is never written
    CHECK(text.find("AD Ini Files") == std::string::npos);            // seeds are never written out
    // Deleting a file key shows the seed again; deleting a seed-only key is not persisted.
    CHECK(ini.set(W, "Berkeley Systems", std::string_view("After Dark"), std::nullopt));
    CHECK_EQ(ini.get(W, "Berkeley Systems", "After Dark").value_or("-"), std::string("C:\\AFTERDRK"));
    CHECK(ini.set(W, "Seeded", std::string_view("Only"), std::nullopt));
    CHECK_EQ(ini.get(W, "Seeded", "Only").value_or("-"), std::string("1"));
    // Deleting a section removes its lines only.
    CHECK(ini.set(W, "Other", std::nullopt, std::nullopt));
    CHECK(!ini.get(W, "Other", "k"));
    CHECK_EQ(vread(v, W), std::string("; comment\r\n[Berkeley Systems]\r\n\r\n[Fresh]\r\nx=y\r\n"));
    // A file another writer replaced is re-read (the cache follows size + time).
    if (persistent) {
      put(upper + "\\WIN.INI", "[Fresh]\r\nx=from-elsewhere\r\n");
      CHECK_EQ(ini.get(W, "Fresh", "x").value_or("-"), std::string("from-elsewhere"));
    }
    // A brand-new file in the upper.
    CHECK(ini.set("C:\\WINDOWS\\MODULES.INI", "Messages", std::string_view("Text"), std::string_view("HELLO")));
    CHECK_EQ(ini.get("c:\\windows\\modules.ini", "messages", "text").value_or("-"), std::string("HELLO"));
    CHECK_EQ(persistent, host_exists(upper + "\\MODULES.INI"));
    // Outside every writable mount: refused.
    CHECK(!ini.set("D:\\NOWHERE.INI", "S", std::string_view("k"), std::string_view("v")));
  }
  // The parser keeps comments and odd lines and finds keys around blanks.
  auto lines = IniStore::parse("junk\n[ S ]\n ; c\n k = v \nnoequals\n");
  CHECK_EQ(lines.size(), size_t(5));
  CHECK(lines[1].kind == IniStore::Line::Kind::section && lines[1].name == "S");
  CHECK(lines[3].kind == IniStore::Line::Kind::key && lines[3].name == "k" && lines[3].value == "v");
  CHECK(lines[4].kind == IniStore::Line::Kind::other);
}

// Two hosts writing different keys of one file at once lose nothing: every
// read-modify-write holds the state mutex and replaces the file atomically.
TEST(ini_store_concurrent_writers) {
  TempDir t;
  std::string root = t.path + "\\state";
  std::atomic<int> failures{0};
  auto writer = [&](int id) {
    Vfs v;
    v.set_state_root(root);
    v.mount_overlay("C:\\WINDOWS", "", root + "\\deluxe\\WINDOWS");
    IniStore ini(v);
    for (int i = 0; i < 60; i++) {
      std::string k = "w" + std::to_string(id) + "_" + std::to_string(i);
      std::string val = std::to_string(i);
      if (!ini.set("C:\\WINDOWS\\MODULES.INI", "Race", std::string_view(k), std::string_view(val))) failures++;
    }
  };
  std::thread a(writer, 1), b(writer, 2);
  a.join();
  b.join();
  CHECK_EQ(failures.load(), 0);
  Vfs v;
  v.mount_overlay("C:\\WINDOWS", "", root + "\\deluxe\\WINDOWS");
  IniStore ini(v);
  auto ks = ini.keys("C:\\WINDOWS\\MODULES.INI", "Race");
  CHECK_EQ(ks.size(), size_t(120));
  CHECK_EQ(ini.get("C:\\WINDOWS\\MODULES.INI", "Race", "w2_59").value_or("-"), std::string("59"));
  bool temps = false;  // no temp files left behind
  for (auto& e : v.list("C:\\WINDOWS", "*")) temps |= e.name.find(".tmp") != std::string::npos;
  CHECK(!temps);
}

// H:\<L>\... <-> <L>:\... both ways, read-only, with long and 8.3 names.
TEST(vfs_host_drives_round_trip) {
  TempDir t;
  std::string dir = t.sub("Long Folder Name");
  std::string file = dir + "\\Picture File.jpeg";
  put(file, "JPEGDATA");
  put(dir + "\\PLAIN.TXT", "plain");
  for (bool short_names : {false, true}) {
    Vfs v;
    v.mount_overlay("C:\\AFTERDRK", t.sub("mod"), "");
    v.mount_host_drives(short_names);
    std::string g = v.host_to_guest(file);
    CHECK(g.rfind("H:\\", 0) == 0);
    if (short_names) {
      size_t p = 3;  // every component is 8.3
      bool all83 = true;
      while (p < g.size()) {
        size_t q = g.find('\\', p);
        if (q == std::string::npos) q = g.size();
        all83 = all83 && Vfs::is_short_name(g.substr(p, q - p));
        p = q + 1;
      }
      CHECK(all83);
      CHECK(g.find(' ') == std::string::npos);
    } else {
      CHECK(g.find("LONG FOLDER NAME\\PICTURE FILE.JPEG") != std::string::npos);
    }
    CHECK_EQ(vread(v, g), std::string("JPEGDATA"));
    // Back to the same host file.
    std::string h = v.to_host(g);
    wchar_t a[MAX_PATH * 2] = {}, b[MAX_PATH * 2] = {};
    GetLongPathNameW(widen(h).c_str(), a, MAX_PATH * 2);
    GetLongPathNameW(widen(file).c_str(), b, MAX_PATH * 2);
    CHECK_EQ(_wcsicmp(a, b), 0);
    // The folder lists, and nothing under H: is writable.
    std::string gdir = g.substr(0, g.find_last_of('\\'));
    auto es = v.list(gdir, "*");
    CHECK_EQ(es.size(), size_t(4));  // ., .., PLAIN.TXT, Picture File.jpeg
    uint32_t err = 0;
    CHECK(!v.open(g, Vfs::Access::write, Vfs::Disposition::open_existing, &err));
    CHECK_EQ(err, uint32_t(ERROR_ACCESS_DENIED));
    CHECK(!v.open(gdir + "\\NEW.TXT", Vfs::Access::write, Vfs::Disposition::create_new, &err));
    CHECK(!v.remove(g, &err));
    // A path inside a mount keeps its mount form.
    CHECK_EQ(v.host_to_guest(t.path + "\\mod\\X.BMP"), std::string("C:\\AFTERDRK\\X.BMP"));
  }
  // Aliases: deterministic, collision-free, 8.3.
  CHECK(Vfs::is_short_name("CRITIC.AD") && Vfs::is_short_name("a.b") && !Vfs::is_short_name("TOOLONGNAME.TXT") &&
        !Vfs::is_short_name("A.JPEG") && !Vfs::is_short_name("TWO WORDS"));
  CHECK_EQ(Vfs::make_short_alias("Picture File.jpeg", {}), std::string("PICTUR~1.JPE"));
  CHECK_EQ(Vfs::make_short_alias("Picture File.jpeg", {"PICTUR~1.JPE"}), std::string("PICTUR~2.JPE"));
  CHECK(Vfs::wild_match("*.*", "X") && Vfs::wild_match("A?C.*", "abc.txt") && Vfs::wild_match("ABC.*", "ABC") &&
        !Vfs::wild_match("*.BMP", "X.JPG") && Vfs::wild_match("*", ".."));
}
