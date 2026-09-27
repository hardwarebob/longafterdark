#include "loader/tools/inspect.hh"

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "loader/image.hh"
#include "loader/ne.hh"
#include "loader/pe.hh"

namespace adw::loader::inspect {

namespace fs = std::filesystem;

namespace {

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) {
    if (c >= 'a' && c <= 'z') c -= 32;
  }
  return out;
}

// Printable form of 8-bit text (resource strings, names): UTF-8 for display,
// with control characters escaped so one entry stays on one line.
std::string printable(std::string_view s, bool latin1) {
  std::string u = latin1 ? latin1_to_utf8(s) : std::string(s);
  std::string out;
  for (char c : u) {
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\x{:02X}", static_cast<unsigned char>(c));
    else out.push_back(c);
  }
  return out;
}

std::string res_type_label(const ResId& t) {
  if (!t.is_string) {
    const char* n = rt::name(t.num);
    return n ? n : std::to_string(t.num);
  }
  return "\"" + printable(t.str, true) + "\"";
}

// Names exported by DLLs next to the inspected file, keyed by module name.
class Siblings {
public:
  explicit Siblings(fs::path dir) : dir_(std::move(dir)) {}

  const std::string* lookup(std::string_view module, uint16_t ordinal) {
    auto& names = load(upper(module));
    if (!names) return nullptr;
    auto it = names->find(ordinal);
    return it == names->end() ? nullptr : &it->second;
  }

private:
  using Table = std::optional<std::map<uint16_t, std::string>>;
  Table& load(const std::string& module) {
    auto [it, fresh] = cache_.try_emplace(module);
    if (!fresh) return it->second;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
      if (!e.is_regular_file()) continue;
      std::string stem = upper(e.path().stem().string());
      std::string ext = upper(e.path().extension().string());
      if (stem != module || !(ext == ".DLL" || ext == ".EXE" || ext == ".DRV" || ext == ".AD")) continue;
      try {
        std::string data = read_file(e.path().string());
        std::map<uint16_t, std::string> names;
        Format f = detect_format(data);
        if (f == Format::ne) {
          ne::Image img(std::move(data));
          for (const auto* t : {&img.resident_names(), &img.nonresident_names()}) {
            for (size_t i = 1; i < t->size(); i++) names.emplace((*t)[i].ordinal, (*t)[i].name);
          }
        } else if (f == Format::pe32) {
          pe::Image img(std::move(data));
          for (const auto& ex : img.exports()) {
            if (!ex.name.empty()) names.emplace(ex.ordinal, ex.name);
          }
        } else {
          continue;
        }
        it->second = std::move(names);
        break;
      } catch (const std::exception&) {
        // A damaged sibling only costs us labels.
      }
    }
    return it->second;
  }

  fs::path dir_;
  std::map<std::string, Table> cache_;
};

struct Ctx {
  std::ostream& out;
  const Options& opt;
  Siblings siblings;
  std::string label(std::string_view module, uint16_t ordinal) {
    if (opt.specs) {
      if (const std::string* n = opt.specs->lookup(module, ordinal)) return *n;
    }
    if (opt.resolve_siblings) {
      if (const std::string* n = siblings.lookup(module, ordinal)) return *n;
    }
    return {};
  }
};

void dump_version(std::ostream& out, const VersionInfo& vi) {
  out << "  VERSIONINFO:";
  if (vi.has_fixed) {
    out << std::format(" file {} product {} flags 0x{:X} os 0x{:X} type {}",
        vi.file_version(), vi.product_version(), vi.file_flags, vi.file_os, vi.file_type);
  }
  out << "\n";
  for (const auto& t : vi.string_tables) {
    out << "    [" << t.key << "]\n";
    for (const auto& [k, v] : t.strings) out << "      " << k << " = \"" << printable(v, false) << "\"\n";
  }
  if (!vi.translations.empty()) {
    out << "    Translation:";
    for (uint32_t tr : vi.translations) out << std::format(" {:04X}/{}", tr & 0xFFFF, tr >> 16);
    out << "\n";
  }
}

// ---- PE -----------------------------------------------------------------------

const char* dir_name(size_t i) {
  static const char* names[16] = {"export", "import", "resource", "exception", "security",
      "basereloc", "debug", "architecture", "globalptr", "tls", "load_config", "bound_import",
      "iat", "delay_import", "clr", "reserved"};
  return i < 16 ? names[i] : "?";
}

std::map<std::string, size_t> resource_type_counts_pe(const pe::Image& img) {
  std::map<std::string, size_t> m;
  for (const auto& r : img.resources()) m[res_type_label(r.type)]++;
  return m;
}

std::string join_counts(const std::map<std::string, size_t>& m) {
  std::string s;
  for (const auto& [k, v] : m) {
    if (!s.empty()) s += ",";
    s += std::format("{}:{}", k, v);
  }
  return s;
}

void dump_pe(Ctx& c, const std::string& path, const pe::Image& img) {
  auto& out = c.out;
  const pe::Header& h = img.header();
  std::map<std::string, size_t> per_dll;
  for (const auto& imp : img.imports()) per_dll[imp.dll]++;

  if (c.opt.summary) {
    out << std::format("{}  PE32 {}  imports[{}]  exports={}  relocs={}  rsrc[{}]  tls={}\n",
        path, h.is_dll() ? "dll" : "exe", join_counts(per_dll), img.exports().size(),
        img.relocations().size(), join_counts(resource_type_counts_pe(img)), img.tls() ? 1 : 0);
    return;
  }

  out << std::format("format: PE32 i386 {}  ({} bytes)\n", h.is_dll() ? "DLL" : "EXE", img.file().size());
  out << std::format("  linker {}.{}  timestamp 0x{:08X}  image base 0x{:08X}  size 0x{:X}  headers 0x{:X}\n",
      h.linker_major, h.linker_minor, h.timestamp, h.image_base, h.size_of_image, h.size_of_headers);
  out << std::format("  entry rva 0x{:X}  subsystem {} ({}.{})  os {}.{}  characteristics 0x{:04X}  dll characteristics 0x{:04X}\n",
      h.entry_rva, h.subsystem, h.subsystem_major, h.subsystem_minor, h.os_major, h.os_minor,
      h.characteristics, h.dll_characteristics);
  out << std::format("  stack 0x{:X}/0x{:X}  heap 0x{:X}/0x{:X}  alignment 0x{:X}/0x{:X}\n",
      h.stack_reserve, h.stack_commit, h.heap_reserve, h.heap_commit, h.section_alignment, h.file_alignment);
  out << "  directories:";
  for (size_t i = 0; i < 16; i++) {
    if (h.dirs[i].rva || h.dirs[i].size) out << std::format(" {}=0x{:X}+0x{:X}", dir_name(i), h.dirs[i].rva, h.dirs[i].size);
  }
  out << "\n";

  out << std::format("sections ({}):\n", img.sections().size());
  out << "  name      rva       vsize     raw_off   raw_size  flags\n";
  for (const auto& s : img.sections()) {
    out << std::format("  {:<8}  {:08X}  {:08X}  {:08X}  {:08X}  {:08X}\n",
        s.name, s.rva, s.virtual_size, s.raw_offset, s.raw_size, s.characteristics);
  }

  out << std::format("imports ({} from {} DLLs):\n", img.imports().size(), per_dll.size());
  std::string cur;
  for (const auto& imp : img.imports()) {
    if (imp.dll != cur) {
      cur = imp.dll;
      out << std::format("  {} ({}):\n", cur, per_dll[cur]);
    }
    if (imp.by_ordinal) {
      std::string l = c.label(fs::path(imp.dll).stem().string(), imp.ordinal);
      out << std::format("    slot 0x{:08X}  #{}{}\n", imp.slot, imp.ordinal, l.empty() ? "" : "  " + l);
    } else {
      out << std::format("    slot 0x{:08X}  {} (hint {})\n", imp.slot, imp.name, imp.hint);
    }
  }

  const auto& ed = img.export_directory();
  if (ed.present) {
    out << std::format("exports ({}, \"{}\", base {}):\n", ed.exports.size(), ed.dll_name, ed.ordinal_base);
    for (const auto& e : ed.exports) {
      out << std::format("  #{:<4} {:<40} rva 0x{:08X}{}\n", e.ordinal, e.name.empty() ? "(by ordinal)" : e.name,
          e.rva, e.is_forwarder() ? "  -> " + e.forwarder : "");
    }
  }

  std::map<uint8_t, size_t> rel;
  std::set<uint32_t> pages;
  for (const auto& r : img.relocations()) {
    rel[r.type]++;
    pages.insert(r.rva & ~0xFFFu);
  }
  out << std::format("relocations: {} over {} pages", img.relocations().size(), pages.size());
  for (const auto& [t, n] : rel) {
    static const char* names[] = {"ABSOLUTE", "HIGH", "LOW", "HIGHLOW", "HIGHADJ"};
    out << std::format("  {}={}", t < 5 ? names[t] : std::to_string(t).c_str(), n);
  }
  out << (h.relocs_stripped() ? "  (RELOCS_STRIPPED)\n" : "\n");
  if (c.opt.verbose) {
    for (const auto& r : img.relocations()) out << std::format("    {:08X} type {}\n", r.rva, r.type);
  }

  if (img.tls()) {
    const auto& t = *img.tls();
    out << std::format("tls: data 0x{:08X}-0x{:08X} index 0x{:08X} callbacks 0x{:08X} ({}) zero_fill 0x{:X}\n",
        t.start_va, t.end_va, t.index_va, t.callbacks_va, t.callbacks.size(), t.zero_fill);
  }

  out << std::format("resources ({}):\n", img.resources().size());
  for (const auto& r : img.resources()) {
    out << std::format("  {:<14} {:<24} lang {:04X}  size {:>7}  rva 0x{:08X}\n", res_type_label(r.type),
        r.name.to_string(), r.language, r.size, r.data_rva);
  }
  auto strings = img.string_table();
  if (!strings.empty()) {
    out << std::format("  RT_STRING ({} strings):\n", strings.size());
    size_t shown = 0;
    for (const auto& [id, s] : strings) {
      if (!c.opt.verbose && shown++ >= 40) {
        out << "    ... (-v for all)\n";
        break;
      }
      out << std::format("    {:>5}: \"{}\"\n", id, printable(s, false));
    }
  }
  try {
    if (auto vi = img.version_info()) dump_version(out, *vi);
  } catch (const LoaderError& e) {
    out << "  VERSIONINFO: undecodable: " << e.what() << "\n";
  }
  for (const auto& w : img.warnings()) out << "warning: " << w << "\n";
}

// ---- NE -----------------------------------------------------------------------

std::string ne_flags(uint16_t f) {
  std::string s;
  auto add = [&](bool on, const char* n) {
    if (on) s += std::string(s.empty() ? "" : " ") + n;
  };
  static const char* data[] = {"NOAUTODATA", "SINGLEDATA", "MULTIPLEDATA", "DATA3"};
  add(true, data[f & 3]);
  add(f & ne::mod_global_init, "GLOBALINIT");
  add(f & ne::mod_protmode, "PROTMODE");
  add(f & ne::mod_i86, "8086");
  add(f & ne::mod_i286, "286");
  add(f & ne::mod_i386, "386");
  add(f & ne::mod_x87, "x87");
  add(f & ne::mod_selfload, "SELFLOAD");
  add(f & ne::mod_link_errors, "LINKERRORS");
  add(f & ne::mod_library, "LIBRARY");
  return s;
}

std::string seg_flags(uint16_t f) {
  std::string s = (f & ne::seg_data) ? "DATA" : "CODE";
  if (f & ne::seg_iterated) s += " ITERATED";
  if (f & ne::seg_moveable) s += " MOVEABLE";
  if (f & ne::seg_pure) s += " PURE";
  if (f & ne::seg_preload) s += " PRELOAD";
  if (f & ne::seg_readonly) s += (f & ne::seg_data) ? " READONLY" : " EXECONLY";
  if (f & ne::seg_relocinfo) s += " RELOCINFO";
  if (f & ne::seg_discardable) s += " DISCARDABLE";
  if (f & ne::seg_32bit) s += " USE32";
  return s;
}

std::map<std::string, size_t> resource_type_counts_ne(const ne::Image& img) {
  std::map<std::string, size_t> m;
  for (const auto& r : img.resources()) m[res_type_label(r.type)]++;
  return m;
}

// Import references aggregated from the relocation records.
struct NeImports {
  // module → (ordinal or "name") → reference count
  std::map<std::string, std::map<std::string, size_t>> refs;
  std::map<std::string, std::map<uint16_t, size_t>> ordinals;
  std::map<std::string, size_t> osfixups;
  size_t records = 0, additive = 0, osfixup_total = 0;
  std::map<std::string, size_t> kinds;
};

NeImports collect_ne(const ne::Image& img) {
  NeImports ni;
  for (const auto& s : img.segments()) {
    for (const auto& r : s.relocations) {
      ni.records++;
      if (r.additive) ni.additive++;
      ni.kinds[std::format("{}/{}", ne::target_kind_name(r.kind), ne::addr_type_name(r.addr_type))]++;
      if (r.kind == ne::TargetKind::import_ordinal) ni.ordinals[r.module][r.ordinal]++;
      if (r.kind == ne::TargetKind::import_name) ni.refs[r.module]["\"" + r.name + "\""]++;
      if (r.kind == ne::TargetKind::os_fixup) {
        ni.osfixup_total++;
        ni.osfixups[ne::os_fixup_name(r.os_fixup)]++;
      }
    }
  }
  return ni;
}

void dump_ne(Ctx& c, const std::string& path, const ne::Image& img) {
  auto& out = c.out;
  const ne::Header& h = img.header();
  NeImports ni = collect_ne(img);

  if (c.opt.summary) {
    std::map<std::string, size_t> per_mod;
    for (const auto& m : img.module_refs()) per_mod[m] = 0;
    for (const auto& [m, o] : ni.ordinals) per_mod[m] += o.size();
    for (const auto& [m, o] : ni.refs) per_mod[m] += o.size();
    out << std::format("{}  NE {} {}  segs={}  imports[{}]  relocs={}  osfixup={}{}  rsrc[{}]\n",
        path, h.is_dll() ? "dll" : "exe", img.module_name(), img.segments().size(), join_counts(per_mod),
        ni.records, ni.osfixup_total, ni.osfixups.empty() ? "" : "(" + join_counts(ni.osfixups) + ")",
        join_counts(resource_type_counts_ne(img)));
    return;
  }

  out << std::format("format: NE {}  module {}  \"{}\"  ({} bytes)\n", h.is_dll() ? "DLL" : "EXE",
      img.module_name(), printable(img.description(), true), img.file().size());
  out << std::format("  linker {}.{}  flags 0x{:04X} ({})  target os {}  windows {}.{}\n", h.linker_major,
      h.linker_minor, h.flags, ne_flags(h.flags), h.target_os, h.expected_version >> 8, h.expected_version & 0xFF);
  out << std::format("  autodata seg {}  heap 0x{:X}  stack 0x{:X}  CS:IP {}:{:04X}  SS:SP {}:{:04X}  align shift {}\n",
      h.autodata_segment, h.heap_size, h.stack_size, h.cs, h.ip, h.ss, h.sp, h.alignment_shift);

  out << std::format("segments ({}):\n", img.segments().size());
  out << "  #   file_off  file_size  min_alloc  relocs  flags\n";
  for (const auto& s : img.segments()) {
    out << std::format("  {:<3} {:08X}  {:>9X}  {:>9X}  {:>6}  0x{:04X} {}\n", s.index, s.file_offset, s.file_size,
        s.min_alloc, s.relocations.size(), s.flags, seg_flags(s.flags));
  }

  out << std::format("relocations: {} records ({} additive)\n", ni.records, ni.additive);
  for (const auto& [k, n] : ni.kinds) out << std::format("  {:<28} {}\n", k, n);
  if (ni.osfixup_total) out << std::format("  OSFIXUP: {}\n", join_counts(ni.osfixups));
  if (c.opt.verbose) {
    for (const auto& s : img.segments()) {
      std::string_view data = img.segment_file_data(s);
      for (const auto& r : s.relocations) {
        std::string tgt;
        switch (r.kind) {
          case ne::TargetKind::internal:
            tgt = r.segment == 0xFF ? std::format("entry #{}", r.entry_ordinal)
                                    : std::format("{}:{:04X}", r.segment, r.target_offset);
            break;
          case ne::TargetKind::import_ordinal:
            tgt = std::format("{}.{} {}", r.module, r.ordinal, c.label(r.module, r.ordinal));
            break;
          case ne::TargetKind::import_name:
            tgt = std::format("{}.\"{}\"", r.module, r.name);
            break;
          case ne::TargetKind::os_fixup: {
            tgt = std::format("{}", ne::os_fixup_name(r.os_fixup));
            // Show the bytes the fixup would patch (what the file holds).
            for (size_t i = 0; i < 4 && size_t(r.offset) + i < data.size(); i++) {
              tgt += std::format(" {:02X}", static_cast<uint8_t>(data[r.offset + i]));
            }
            break;
          }
        }
        out << std::format("    seg {} @{:04X} {:<9} {}{}\n", s.index, r.offset, ne::addr_type_name(r.addr_type),
            r.additive ? "+ " : "", tgt);
      }
    }
  }

  out << std::format("imports ({} modules):\n", img.module_refs().size());
  for (const auto& m : img.module_refs()) {
    size_t n = ni.ordinals[m].size() + ni.refs[m].size();
    out << std::format("  {} ({} distinct):\n", m, n);
    for (const auto& [ord, cnt] : ni.ordinals[m]) {
      std::string l = c.label(m, ord);
      out << std::format("    #{:<5} {:<32} x{}\n", ord, l.empty() ? "?" : l, cnt);
    }
    for (const auto& [name, cnt] : ni.refs[m]) out << std::format("    {:<38} x{}\n", name, cnt);
  }

  out << std::format("entries ({}):\n", img.entries().size());
  std::map<uint16_t, std::string> names;
  for (const auto* t : {&img.resident_names(), &img.nonresident_names()}) {
    for (size_t i = 1; i < t->size(); i++) names.emplace((*t)[i].ordinal, (*t)[i].name);
  }
  for (const auto& e : img.entries()) {
    auto it = names.find(e.ordinal);
    out << std::format("  #{:<5} {}:{:04X}  {}{}{} {}\n", e.ordinal, e.segment, e.offset,
        e.moveable ? "moveable" : "fixed   ", e.exported() ? " exported" : "", e.shared_data() ? " shared" : "",
        it == names.end() ? "" : it->second);
  }

  out << std::format("resources ({}, align shift {}):\n", img.resources().size(), img.resource_alignment_shift());
  for (const auto& r : img.resources()) {
    out << std::format("  {:<14} {:<24} size {:>7}  offset 0x{:08X}  flags 0x{:04X}\n", res_type_label(r.type),
        r.name.is_string ? "\"" + printable(r.name.str, true) + "\"" : std::to_string(r.name.num), r.size,
        r.file_offset, r.flags);
  }
  auto strings = img.string_table();
  if (!strings.empty()) {
    out << std::format("  RT_STRING ({} strings):\n", strings.size());
    size_t shown = 0;
    for (const auto& [id, s] : strings) {
      if (!c.opt.verbose && shown++ >= 40) {
        out << "    ... (-v for all)\n";
        break;
      }
      out << std::format("    {:>5}: \"{}\"\n", id, printable(s, true));
    }
  }
  try {
    if (auto vi = img.version_info()) dump_version(out, *vi);
  } catch (const LoaderError& e) {
    out << "  VERSIONINFO: undecodable: " << e.what() << "\n";
  }
  for (const auto& w : img.warnings()) out << "warning: " << w << "\n";
}

} // namespace

size_t SpecDb::load_dir(const std::string& dir) {
  size_t files = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".spec") continue;
    // "krnl386.exe16.spec" → KRNL386 (served to NE importers as KERNEL).
    std::string fname = e.path().filename().string();
    std::string module = upper(fname.substr(0, fname.find('.')));
    if (module == "KRNL386") module = "KERNEL";
    auto& table = modules_[module];
    std::ifstream f(e.path());
    std::string line;
    while (std::getline(f, line)) {
      line = line.substr(0, line.find('#'));
      std::istringstream ss(line);
      std::string ord, type, tok;
      // Ordinals are 1..65535; anything longer or larger is not a spec line
      // we understand (and std::stoul would throw on it).
      if (!(ss >> ord >> type) || ord.empty() || ord.size() > 5 ||
          ord.find_first_not_of("0123456789") != std::string::npos) {
        continue;
      }
      unsigned long ordinal = std::stoul(ord);
      if (ordinal > 0xFFFF) continue;
      while (ss >> tok && tok[0] == '-') {}
      if (tok.empty() || tok[0] == '-') continue;
      tok = tok.substr(0, tok.find('('));
      if (!tok.empty()) table.emplace(static_cast<uint16_t>(ordinal), tok);
    }
    files++;
  }
  return files;
}

const std::string* SpecDb::lookup(std::string_view module, uint16_t ordinal) const {
  auto it = modules_.find(upper(module));
  if (it == modules_.end()) return nullptr;
  auto o = it->second.find(ordinal);
  return o == it->second.end() ? nullptr : &o->second;
}

bool is_executable_name(std::string_view filename) {
  std::string ext = upper(fs::path(filename).extension().string());
  return ext == ".AD" || ext == ".DLL" || ext == ".EXE" || ext == ".SCR" || ext == ".DRV";
}

bool inspect_file(std::ostream& out, const std::string& path, const Options& opt) {
  Ctx c{out, opt, Siblings(fs::path(path).parent_path())};
  try {
    std::string data = read_file(path);
    Format f = detect_format(data);
    if (!opt.summary) out << "== " << path << "\n";
    switch (f) {
      case Format::pe32:
        dump_pe(c, path, pe::Image(std::move(data)));
        return true;
      case Format::ne:
        dump_ne(c, path, ne::Image(std::move(data)));
        return true;
      default:
        out << (opt.summary ? path + "  " : std::string("format: ")) << format_name(f) << " (not inspected)\n";
        return false;
    }
  } catch (const LoaderError& e) {
    out << path << ": error (" << error_kind_name(e.kind()) << "): " << e.what() << "\n";
  } catch (const std::exception& e) {
    out << path << ": error: " << e.what() << "\n";
  }
  return false;
}

} // namespace adw::loader::inspect
