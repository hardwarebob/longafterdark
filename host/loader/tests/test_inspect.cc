// adwinspect's dump library over the synthetic images, so the CLI is covered
// even where the After Dark files are not installed: Win16 ordinals named from
// a .spec file (and from a sibling DLL), exports and forwarders, relocation
// and OSFIXUP counts, string-typed resources, RT_STRING and VERSIONINFO.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <sstream>

#include "builders.hh"
#include "loader/tools/inspect.hh"

using namespace adw::loader;
using namespace adw::loader::test;
namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string dump(const fs::path& p, const inspect::Options& o, bool* ok = nullptr) {
  std::ostringstream out;
  bool r = inspect::inspect_file(out, p.string(), o);
  if (ok) *ok = r;
  return out.str();
}

#define CHECK_HAS(text, needle)                                                   \
  do {                                                                            \
    if ((text).find(needle) == std::string::npos) {                               \
      std::fprintf(stderr, "%s:%d: output lacks \"%s\"\n", __FILE__, __LINE__, needle); \
      ::adw::loader::test::g_failures++;                                          \
    }                                                                             \
  } while (0)

} // namespace

int main() {
  // A private directory, so the sibling lookup sees only what we put there.
  fs::path dir = fs::temp_directory_path() / std::format("adw_loader_inspect_{}", std::random_device{}());
  fs::create_directories(dir);
  fs::path specs = dir / "spec";
  fs::create_directories(specs);
  // Wine .spec syntax: ordinal, type, flags, name(args). krnl386 serves KERNEL.
  write_file(specs / "krnl386.exe16.spec",
      "# comment line\n"
      "3   pascal   GetVersion() GetVersion16\n"
      "5   pascal -ret16 LocalAlloc(word word) LocalAlloc16\n"
      "@   stub     NotAnOrdinal\n"
      "99999999999 stub TooBig\n");
  write_file(specs / "user.exe16.spec", "1 pascal -ret16 MessageBox(word str str word)\n");

  write_file(dir / "TEST.AD", build_ne());
  write_file(dir / "TEST.DLL", build_pe());
  // A sibling named like an imported module: its exports label GDI.<n>.
  write_file(dir / "GDI.DLL", build_ne());
  write_file(dir / "JUNK.DLL", "MZ not really");

  inspect::SpecDb db;
  CHECK_EQ(db.load_dir(specs.string()), 2u);
  CHECK(db.lookup("kernel", 3) && *db.lookup("kernel", 3) == "GetVersion");
  CHECK(db.lookup("KERNEL", 5) && *db.lookup("KERNEL", 5) == "LocalAlloc");
  CHECK(db.lookup("KERNEL", 4) == nullptr);
  CHECK(db.lookup("USER", 1) != nullptr);

  inspect::Options o;
  o.specs = &db;
  bool ok = false;

  // ---- NE
  std::string ne = dump(dir / "TEST.AD", o, &ok);
  CHECK(ok);
  CHECK_HAS(ne, "format: NE DLL  module TESTNE  \"Test NE DLL\"");
  CHECK_HAS(ne, "LIBRARY");
  CHECK_HAS(ne, "relocations: 18 records");
  CHECK_HAS(ne, "OSFIXUP: FIARQQ/FJARQQ:1,FICRQQ/FJCRQQ:1,FIDRQQ:1,FIERQQ:1,FISRQQ/FJSRQQ:1,FIWRQQ:1");
  CHECK_HAS(ne, "GetVersion");                // KERNEL.3 from the spec
  CHECK_HAS(ne, "LocalAlloc");                // KERNEL.5
  CHECK_HAS(ne, "\"MESSAGEBOX\"");            // USER by name
  CHECK_HAS(ne, "#1     MODULE");             // GDI.1 from the sibling GDI.DLL
  CHECK_HAS(ne, "moveable exported");         // entry #3
  CHECK_HAS(ne, "\"RLEP\"");                  // string-typed resource
  CHECK_HAS(ne, "\"SPLASH\"");
  CHECK_HAS(ne, "15: \"Fifteen\"");           // RT_STRING
  CHECK_HAS(ne, "FileDescription = \"Synthetic NE\"");
  CHECK_HAS(ne, "file 4.0.0.1");

  inspect::Options ov = o;
  ov.verbose = true;
  std::string nev = dump(dir / "TEST.AD", ov);
  CHECK_HAS(nev, "FIWRQQ 90 9B");             // the bytes an OSFIXUP patches
  CHECK_HAS(nev, "entry #3");

  inspect::Options os = o;
  os.summary = true;
  std::string nes = dump(dir / "TEST.AD", os);
  CHECK_HAS(nes, "NE dll TESTNE  segs=2");
  CHECK_HAS(nes, "osfixup=6(");
  CHECK_HAS(nes, "rsrc[\"RLEP\":2,1000:1,STRING:1,VERSION:1]");
  CHECK_EQ(std::count(nes.begin(), nes.end(), '\n'), 1);

  // Without siblings or specs the ordinals stay unnamed.
  inspect::Options bare;
  bare.resolve_siblings = false;
  std::string nb = dump(dir / "TEST.AD", bare);
  CHECK(nb.find("GetVersion") == std::string::npos);
  CHECK_HAS(nb, "#3     ?");

  // ---- PE
  std::string pe = dump(dir / "TEST.DLL", o, &ok);
  CHECK(ok);
  CHECK_HAS(pe, "format: PE32 i386 DLL");
  CHECK_HAS(pe, "image base 0x10000000");
  CHECK_HAS(pe, ".reloc");
  CHECK_HAS(pe, "ADXPL510.dll (2):");
  CHECK_HAS(pe, "Foo (hint 3)");
  CHECK_HAS(pe, "#7");
  CHECK_HAS(pe, "GetTickCount");
  CHECK_HAS(pe, "exports (4, \"TEST.AD\", base 1):");
  CHECK_HAS(pe, "-> KERNEL32.GetTickCount");
  CHECK_HAS(pe, "(by ordinal)");
  CHECK_HAS(pe, "relocations: 11 over 3 pages");
  CHECK_HAS(pe, "HIGHADJ=1");
  CHECK_HAS(pe, "tls: data 0x10003100");
  CHECK_HAS(pe, "\"RLEP\"");
  CHECK_HAS(pe, "\"SPLASH\"");
  CHECK_HAS(pe, "lang 0407");
  CHECK_HAS(pe, "2: \"Two\"");
  CHECK_HAS(pe, "CompanyName = \"Test Co\"");
  CHECK_HAS(pe, "Translation: 0409/1252");

  // ---- files that are not PE/NE, or are damaged, report and return false
  std::string junk = dump(dir / "JUNK.DLL", o, &ok);
  CHECK(!ok);
  CHECK_HAS(junk, "not inspected");
  std::string cut = build_pe();
  write_file(dir / "CUT.DLL", cut.substr(0, 0x200));
  std::string bad = dump(dir / "CUT.DLL", o, &ok);
  CHECK(!ok);
  CHECK_HAS(bad, "error (truncated)");
  std::string missing = dump(dir / "NOPE.DLL", o, &ok);
  CHECK(!ok);
  CHECK_HAS(missing, "error");

  CHECK(inspect::is_executable_name("toasters.ad"));
  CHECK(inspect::is_executable_name("AFTERDAR.SCR"));
  CHECK(!inspect::is_executable_name("LUNDATA.DAT"));

  std::error_code ec;
  fs::remove_all(dir, ec);
  return finish("test_loader_inspect");
}
