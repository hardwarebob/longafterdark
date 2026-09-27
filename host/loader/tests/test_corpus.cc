// Real-corpus test: parses every executable under the imported After Dark
// FILES tree, loads every PE32 image at a non-preferred base and checks every
// relocation and IAT slot, loads every NE image through a dummy resolver
// (with OSFIXUPs left and emulated, checking each FP fixup site), and asserts
// the layout facts later phases rely on. Prints a per-file census.
//
// Exits 77 (ctest SKIP) when the original files are not installed.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <set>
#include <sstream>

#include "builders.hh"
#include "loader/ne.hh"
#include "loader/pe.hh"
#include "loader/tools/inspect.hh"

using namespace adw::loader;
using namespace adw::loader::test;
namespace fs = std::filesystem;

namespace {

// AD_ASSETS_DIR, else the data folder (host/core data_root.h): <base>\LongAfterDark,
// where base is AD_LOCALAPPDATA or LOCALAPPDATA. Read-only; this library stays
// free of the Windows API, so it spells out that header's rule instead of
// calling it.
fs::path find_files_dir() {
  std::vector<fs::path> candidates;
  const char* over = std::getenv("AD_LOCALAPPDATA");
  const char* base = (over && *over) ? over : std::getenv("LOCALAPPDATA");
  if (const char* a = std::getenv("AD_ASSETS_DIR")) {
    candidates = {fs::path(a) / "win" / "FILES", fs::path(a) / "FILES", fs::path(a)};
  } else if (base) {
    candidates.push_back(fs::path(base) / "LongAfterDark" / "assets" / "win" / "FILES");
  }
  std::error_code ec;
  for (const auto& c : candidates) {
    if (fs::is_directory(c / "AD40", ec) && fs::is_directory(c / "CLASSIC", ec)) return c;
  }
  return {};
}

std::string upper(std::string s) {
  for (auto& c : s) {
    if (c >= 'a' && c <= 'z') c -= 32;
  }
  return s;
}

std::string join(const std::set<std::string>& s) {
  std::string out;
  for (const auto& x : s) out += (out.empty() ? "" : ",") + x;
  return out.empty() ? "(none)" : out;
}

// Returns a failure description, or "" when every relocation and IAT slot in
// the relocated image checks out.
std::string check_pe(const pe::Image& img) {
  const uint32_t pref = img.header().image_base;
  const uint32_t alt = pref == 0x20000000 ? 0x30000000 : 0x20000000;
  TestSink sink;
  sink.force_base = alt;
  std::map<uint32_t, uint32_t> bound; // slot rva → value
  pe::Loaded ld;
  try {
    ld = pe::load(img, sink, [&](const pe::Import& imp) {
      uint32_t v = 0xF0000000 | static_cast<uint32_t>(bound.size());
      bound[imp.slot_rva] = v;
      return v;
    });
  } catch (const LoaderError& e) {
    if (e.kind() == LoaderError::Kind::placement && img.header().relocs_stripped()) return "";
    return std::string("load: ") + e.what();
  }
  if (ld.relocations_applied != img.relocations().size()) return "not every relocation was applied";
  const uint32_t delta = alt - pref;
  std::set<uint32_t> touched; // rvas of fixups, to skip overlapping HIGHLOWs
  for (const auto& r : img.relocations()) {
    if (!touched.insert(r.rva).second) return std::format("two relocations at RVA 0x{:X}", r.rva);
  }
  for (const auto& r : img.relocations()) {
    if (r.type != pe::rel_highlow) continue;
    uint32_t orig = static_cast<uint8_t>(img.mapped()[r.rva]) | (static_cast<uint8_t>(img.mapped()[r.rva + 1]) << 8) |
        (static_cast<uint8_t>(img.mapped()[r.rva + 2]) << 16) | (uint32_t(static_cast<uint8_t>(img.mapped()[r.rva + 3])) << 24);
    // An IAT slot that is also relocated holds the bound value instead.
    if (bound.count(r.rva)) continue;
    // A neighbouring fixup overlapping these 4 bytes changes them too.
    bool overlap = touched.count(r.rva + 1) || touched.count(r.rva + 2) || touched.count(r.rva + 3) ||
        touched.count(r.rva - 1) || touched.count(r.rva - 2) || touched.count(r.rva - 3);
    if (overlap) continue;
    if (sink.u32(alt + r.rva) != orig + delta) {
      return std::format("HIGHLOW at RVA 0x{:X}: 0x{:08X} → 0x{:08X}, expected 0x{:08X}", r.rva, orig,
          sink.u32(alt + r.rva), orig + delta);
    }
  }
  for (const auto& [slot, v] : bound) {
    if (sink.u32(alt + slot) != v) return std::format("IAT slot 0x{:X} not bound", slot);
  }
  if (sink.u32(alt + img.header().pe_offset + 24 + 28) != alt) return "header ImageBase not updated";
  return "";
}

// The instruction an OSFIXUP site must hold in the file (real x87 code), and
// the bytes it must turn into when emulated. This is an independent statement
// of the emulator encoding, not a replay of os_fixup_constants: every FWAIT
// becomes INT 34h–3Dh, and a segment-overridden ESC byte keeps its low bits
// and gets the segment tag (DS 00, SS 01, CS 10, ES 11) in its top two.
bool fp_site_ok(uint16_t type, const uint8_t* before, const uint8_t* after) {
  bool esc = (before[2] & 0xF8) == 0xD8;
  auto seg_form = [&](uint8_t prefix, uint8_t tag) {
    return before[0] == 0x9B && before[1] == prefix && esc && after[0] == 0xCD && after[1] == 0x3C &&
        after[2] == ((tag << 6) | (before[2] & 0x3F));
  };
  switch (type) {
    case 1: return seg_form(0x3E, 0); // DS
    case 2: return seg_form(0x36, 1); // SS
    case 3: return seg_form(0x2E, 2); // CS
    case 4: return seg_form(0x26, 3); // ES
    case 5:
      return before[0] == 0x9B && (before[1] & 0xF8) == 0xD8 && after[0] == 0xCD && after[1] >= 0x34 &&
          after[1] <= 0x3B && after[1] - 0x34 == (before[1] & 7) && after[2] == before[2];
    case 6: return before[0] == 0x90 && before[1] == 0x9B && after[0] == 0xCD && after[1] == 0x3D;
  }
  return false;
}

// The dummy resolver's answer for a record, worked out from the record and the
// entry table directly rather than from the loader's Target.
ne::FarPtr expected_target(const ne::Image& img, const ne::Relocation& r) {
  if (r.kind == ne::TargetKind::internal) {
    uint16_t seg = r.segment, off = r.target_offset;
    if (seg == 0xFF) {
      const ne::Entry* e = img.find_entry(r.entry_ordinal);
      if (!e) return {0xDEAD, 0};
      seg = e->segment;
      off = e->offset;
    }
    return {static_cast<uint16_t>(0x100 + 8 * seg), off};
  }
  return {static_cast<uint16_t>(0x800 + 8 * r.module_index), r.ordinal};
}

// Checks one segment of a load with OSFIXUPs left alone: every source
// location of every record (chains walked in the unrelocated bytes) holds the
// resolved target, additive ones the original plus the offset, and every other
// byte is the file's. Bytes two records both claim are counted, not checked.
std::string check_ne_fixups(const ne::Image& img, const ne::Segment& s, const std::string& orig,
    const std::string& loaded, size_t& locations, size_t& shared) {
  std::vector<int> owner(orig.size(), 0);
  std::string want = orig;
  auto u16 = [&](uint32_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(orig[at]) | (static_cast<uint8_t>(orig[at + 1]) << 8));
  };
  auto u32 = [&](uint32_t at) { return u16(at) | (uint32_t(u16(at + 2)) << 16); };
  auto put = [&](uint32_t at, uint64_t v, int n) {
    for (int i = 0; i < n; i++) {
      want[at + i] = static_cast<char>(v >> (8 * i));
      owner[at + i]++;
    }
  };
  for (const auto& r : s.relocations) {
    if (r.kind == ne::TargetKind::os_fixup) continue;
    ne::FarPtr t = expected_target(img, r);
    uint32_t at = r.offset;
    for (size_t guard = 0; guard <= orig.size(); guard++) {
      size_t size = ne::addr_type_size(r.addr_type);
      if (at + size > orig.size()) return std::format("segment {} source 0x{:X} past its end", s.index, at);
      uint16_t next = u16(at);
      locations++;
      switch (r.addr_type) {
        case ne::AddrType::lobyte: put(at, (r.additive ? static_cast<uint8_t>(orig[at]) : 0) + t.offset, 1); break;
        case ne::AddrType::offset16: put(at, (r.additive ? u16(at) : 0) + t.offset, 2); break;
        case ne::AddrType::offset32: put(at, (r.additive ? u32(at) : 0) + t.offset, 4); break;
        case ne::AddrType::selector: put(at, t.selector, 2); break;
        case ne::AddrType::pointer32:
          put(at, (r.additive ? u16(at) : 0) + t.offset, 2);
          put(at + 2, t.selector, 2);
          break;
        case ne::AddrType::pointer48:
          put(at, (r.additive ? u32(at) : 0) + t.offset, 4);
          put(at + 4, t.selector, 2);
          break;
      }
      if (r.additive || next == 0xFFFF) break;
      at = next;
    }
  }
  for (size_t i = 0; i < orig.size(); i++) {
    if (owner[i] > 1) {
      shared++;
    } else if (loaded[i] != want[i]) {
      return std::format("segment {} byte 0x{:X} is 0x{:02X}, expected 0x{:02X} ({})", s.index, i,
          static_cast<uint8_t>(loaded[i]), static_cast<uint8_t>(want[i]), owner[i] ? "fixup" : "untouched");
    }
  }
  return "";
}

std::string check_ne(const ne::Image& img, size_t& os_fixups) {
  auto resolver = [](const ne::Target& t) -> ne::FarPtr {
    if (t.kind == ne::TargetKind::internal) return {static_cast<uint16_t>(0x100 + 8 * t.segment), t.offset};
    return {static_cast<uint16_t>(0x800 + 8 * t.module_index), static_cast<uint32_t>(t.ordinal)};
  };
  TestSink leave_sink, emu_sink;
  ne::Loaded leave, emu;
  try {
    leave = ne::load(img, leave_sink, resolver);
    ne::LoadOptions opt;
    opt.os_fixups = ne::OsFixupMode::emulate;
    emu = ne::load(img, emu_sink, resolver, opt);
  } catch (const LoaderError& e) {
    return std::string("load: ") + e.what();
  }
  size_t records = 0;
  for (const auto& s : img.segments()) records += s.relocations.size();
  if (leave.stats.records != records) return "record count mismatch";
  os_fixups = leave.stats.os_fixups;
  size_t locations = 0, shared = 0;
  for (const auto& s : img.segments()) {
    const auto& ps = leave.placement.segment(s.index);
    std::string orig = img.segment_image(s, ps.size);
    const std::string& loaded = leave_sink.blocks.at(ps.base);
    std::string err = check_ne_fixups(img, s, orig, loaded, locations, shared);
    if (!err.empty()) return err;
  }
  if (locations != leave.stats.locations) {
    return std::format("{} fixup locations in the chains, loader patched {}", locations, leave.stats.locations);
  }
  if (shared) return std::format("{} bytes claimed by two relocation records", shared);
  if (emu.stats.os_fixups_applied != emu.stats.os_fixups) return "not every OSFIXUP applied";
  for (const auto& s : img.segments()) {
    for (const auto& r : s.relocations) {
      if (r.kind != ne::TargetKind::os_fixup) continue;
      uint8_t before[3], after[3];
      for (int i = 0; i < 3; i++) {
        before[i] = leave_sink.u8(leave.placement.segment(s.index).base + r.offset + i);
        after[i] = emu_sink.u8(emu.placement.segment(s.index).base + r.offset + i);
      }
      if (!fp_site_ok(r.os_fixup, before, after)) {
        return std::format("segment {} OSFIXUP {} at 0x{:X}: {:02X} {:02X} {:02X} → {:02X} {:02X} {:02X}", s.index,
            ne::os_fixup_name(r.os_fixup), r.offset, before[0], before[1], before[2], after[0], after[1], after[2]);
      }
    }
  }
  return "";
}

} // namespace

int main() {
  fs::path files = find_files_dir();
  if (files.empty()) {
    std::printf("test_loader_corpus: SKIP (After Dark FILES not found; set AD_ASSETS_DIR)\n");
    return 77;
  }
  std::printf("corpus: %s\n", files.string().c_str());

  std::vector<fs::path> paths;
  for (const auto& e : fs::recursive_directory_iterator(files)) {
    if (e.is_regular_file() && inspect::is_executable_name(e.path().filename().string())) paths.push_back(e.path());
  }
  std::sort(paths.begin(), paths.end());

  // "AD4 modules" are the PE32 *.AD files: 22 in AD40 plus ENGINE/STARRYNI.AD.
  size_t pe_count = 0, ne_count = 0, ad4_modules = 0, ad40_ad_files = 0, ad40_pe_files = 0, classic_modules = 0,
         os_fixup_total = 0;
  std::set<std::string> ad4_no_module1, ad4_no_adxpl, ad40_not_pe, classic_not_ne, classic_module_not_1,
      classic_no_module, ad40_not_at_400000;
  std::map<std::string, Format> formats;

  for (const auto& p : paths) {
    std::string rel = fs::relative(p, files).generic_string();
    std::string dir = upper(p.parent_path().filename().string());
    std::string name = upper(p.filename().string());
    bool is_ad = p.extension().string().size() == 3 && upper(p.extension().string()) == ".AD";
    std::string data = read_file(p.string());
    Format f = detect_format(data);
    formats[upper(rel)] = f;

    std::string err;
    if (f == Format::pe32) {
      pe_count++;
      try {
        pe::Image img(std::move(data));
        err = check_pe(img);
        if (dir == "AD40") {
          ad40_pe_files++;
          // TLINK32 linked the engine and every AD4 module at 0x00400000, so
          // at most one of them can sit at its preferred base in one address
          // space: the win32 lane must relocate the rest (none is stripped).
          if (img.header().image_base != 0x00400000 || img.header().relocs_stripped()) {
            ad40_not_at_400000.insert(name);
          }
        }
        if (is_ad) {
          ad4_modules++;
          const pe::Export* m = img.find_export("Module");
          if (!m || m->ordinal != 1) ad4_no_module1.insert(name);
          // Whatever it is called, ordinal 1 is the entry in every AD4 module.
          const pe::Export* o1 = img.find_export(uint16_t(1));
          CHECK(o1 && (o1->name == "Module" || o1->name == "_Module@4"));
          bool adxpl = false;
          for (const auto& d : img.imported_dlls()) adxpl |= upper(d) == "ADXPL510.DLL";
          if (!adxpl) ad4_no_adxpl.insert(name);
        }
      } catch (const LoaderError& e) {
        err = std::string("parse: ") + e.what();
      }
    } else if (f == Format::ne) {
      ne_count++;
      try {
        ne::Image img(std::move(data));
        size_t fx = 0;
        err = check_ne(img, fx);
        os_fixup_total += fx;
        if (dir == "CLASSIC" && is_ad) {
          classic_modules++;
          auto ord = img.find_ordinal("MODULE");
          if (!ord || !img.find_export("MODULE")) classic_no_module.insert(name);
          else if (*ord != 1) classic_module_not_1.insert(std::format("{}#{}", name, *ord));
        }
      } catch (const LoaderError& e) {
        err = std::string("parse: ") + e.what();
      }
    } else {
      err = std::string("unexpected format ") + format_name(f);
    }
    if (dir == "CLASSIC" && is_ad && f != Format::ne) classic_not_ne.insert(name);
    if (dir == "AD40" && is_ad) {
      ad40_ad_files++;
      if (f != Format::pe32) ad40_not_pe.insert(name);
    }
    if (!err.empty()) {
      std::fprintf(stderr, "%s: %s\n", rel.c_str(), err.c_str());
      g_failures++;
    }

    // The dump code must handle every file too; its summary is the census.
    std::ostringstream full, line;
    inspect::Options o;
    CHECK(inspect::inspect_file(full, p.string(), o));
    o.summary = true;
    inspect::inspect_file(line, p.string(), o);
    std::string l = line.str();
    size_t cut = l.find(files.string());
    if (cut != std::string::npos) l.erase(cut, files.string().size() + 1);
    std::printf("%s", l.c_str());
  }

  std::printf("\n%zu files: %zu PE32, %zu NE; %zu OSFIXUP records\n", paths.size(), pe_count, ne_count, os_fixup_total);
  std::printf("AD4 (PE32 .AD) modules: %zu (%zu in AD40); without Module@1: %s; not importing adxpl510.dll: %s\n",
      ad4_modules, ad40_ad_files, join(ad4_no_module1).c_str(), join(ad4_no_adxpl).c_str());
  std::printf("CLASSIC modules: %zu; MODULE not at ordinal 1: %s\n", classic_modules, join(classic_module_not_1).c_str());

  // AD40 holds 23 PE32 files: 22 modules + ADXPL510.DLL. The 23rd AD4 module
  // is ENGINE/STARRYNI.AD.
  CHECK_EQ(ad40_pe_files, 23u);
  CHECK_EQ(ad4_modules, 23u);
  CHECK_EQ(ad40_ad_files, 22u);
  CHECK_STR(join(ad40_not_at_400000), "(none)");
  CHECK(ad40_not_pe.empty());
  // Starry Night is an MSVC build (linker 4.20, not Borland TLINK32) and
  // exports the decorated stdcall name "_Module@4" at ordinal 1.
  CHECK_STR(join(ad4_no_module1), "STARRYNI.AD");
  // Psycho and Starry Night are self-contained (GDI32/KERNEL32/USER32 only);
  // every other AD4 module links the engine.
  CHECK_STR(join(ad4_no_adxpl), "PSYCHO.AD,STARRYNI.AD");
  CHECK_EQ(classic_modules, 61u);
  CHECK(classic_not_ne.empty());
  CHECK(classic_no_module.empty());
  CHECK_STR(join(classic_module_not_1), "BADDOG3.AD#17,DOSSHELL.AD#2,MESSAGE3.AD#2,WMORPH.AD#2,ZOOM.AD#2");
  CHECK(formats["AD40/ADXPL510.DLL"] == Format::pe32);
  CHECK(formats["CLASSIC/ADXPL300.DLL"] == Format::ne);
  CHECK(formats["ENGINE/OLDMOD16.DLL"] == Format::ne);
  CHECK(formats["ENGINE/OLDMOD32.DLL"] == Format::pe32);
  return finish("test_loader_corpus");
}
