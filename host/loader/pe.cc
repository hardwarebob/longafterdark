#include "loader/pe.hh"

#include <set>

#include "loader/bytes.hh"

namespace adw::loader::pe {

using detail::Bytes;
using detail::fail;
using Kind = LoaderError::Kind;

namespace {

// Real images are a few hundred KB; the cap only keeps a hostile SizeOfImage
// from turning a parse into a multi-GB allocation.
constexpr uint32_t max_image_size = 64u << 20;
constexpr uint32_t max_sections = 1024;
// Descriptors and directory entries may share (or overlap) the tables they
// point at, so the image size alone does not bound these; without a cap a
// small hostile file can ask for billions of entries. The corpus's largest
// are 194 imports (UNINSTAL.EXE) and 323 resources in one file.
constexpr size_t max_imports = 1u << 16;
constexpr size_t max_resources = 1u << 18;

bool power_of_two(uint32_t v) { return v && !(v & (v - 1)); }

} // namespace

Image Image::from_file(const std::string& path) {
  return Image(read_file(path));
}

Image::Image(std::string data) : file_(std::move(data)) {
  parse_headers();
  map_sections();
  parse_imports();
  parse_exports();
  parse_relocations();
  parse_resources();
  parse_tls();
}

void Image::parse_headers() {
  Bytes f(file_, "PE file");
  if (file_.size() < 0x40 || file_.compare(0, 2, "MZ") != 0) {
    fail(Kind::bad_format, "not an MZ executable");
  }
  hdr_.pe_offset = f.u32(0x3C);
  size_t pe = hdr_.pe_offset;
  if (!f.contains(pe, 4) || f.slice(pe, 4) != std::string_view("PE\0\0", 4)) {
    fail(Kind::bad_format, "no PE signature at e_lfanew");
  }
  size_t coff = pe + 4;
  hdr_.machine = f.u16(coff);
  hdr_.num_sections = f.u16(coff + 2);
  hdr_.timestamp = f.u32(coff + 4);
  hdr_.symtab_offset = f.u32(coff + 8);
  hdr_.num_symbols = f.u32(coff + 12);
  hdr_.optional_header_size = f.u16(coff + 16);
  hdr_.characteristics = f.u16(coff + 18);

  size_t opt = coff + 20;
  hdr_.magic = f.u16(opt);
  if (hdr_.magic == 0x20B) fail(Kind::unsupported, "PE32+ (64-bit) image");
  if (hdr_.magic != 0x10B) {
    fail(Kind::bad_format, std::format("optional header magic 0x{:X} is not PE32", hdr_.magic));
  }
  if (hdr_.machine != machine_i386) {
    fail(Kind::unsupported, std::format("machine 0x{:04X} is not i386", hdr_.machine));
  }
  // Everything up to NumberOfRvaAndSizes is mandatory in a PE32 header.
  if (hdr_.optional_header_size < 96) {
    fail(Kind::malformed, std::format("optional header is only {} bytes", hdr_.optional_header_size));
  }
  f.need(opt, hdr_.optional_header_size);
  hdr_.linker_major = f.u8(opt + 2);
  hdr_.linker_minor = f.u8(opt + 3);
  hdr_.size_of_code = f.u32(opt + 4);
  hdr_.size_of_initialized_data = f.u32(opt + 8);
  hdr_.size_of_uninitialized_data = f.u32(opt + 12);
  hdr_.entry_rva = f.u32(opt + 16);
  hdr_.base_of_code = f.u32(opt + 20);
  hdr_.base_of_data = f.u32(opt + 24);
  hdr_.image_base = f.u32(opt + 28);
  hdr_.section_alignment = f.u32(opt + 32);
  hdr_.file_alignment = f.u32(opt + 36);
  hdr_.os_major = f.u16(opt + 40);
  hdr_.os_minor = f.u16(opt + 42);
  hdr_.image_major = f.u16(opt + 44);
  hdr_.image_minor = f.u16(opt + 46);
  hdr_.subsystem_major = f.u16(opt + 48);
  hdr_.subsystem_minor = f.u16(opt + 50);
  hdr_.win32_version = f.u32(opt + 52);
  hdr_.size_of_image = f.u32(opt + 56);
  hdr_.size_of_headers = f.u32(opt + 60);
  hdr_.checksum = f.u32(opt + 64);
  hdr_.subsystem = f.u16(opt + 68);
  hdr_.dll_characteristics = f.u16(opt + 70);
  hdr_.stack_reserve = f.u32(opt + 72);
  hdr_.stack_commit = f.u32(opt + 76);
  hdr_.heap_reserve = f.u32(opt + 80);
  hdr_.heap_commit = f.u32(opt + 84);
  hdr_.loader_flags = f.u32(opt + 88);
  hdr_.num_rva_and_sizes = f.u32(opt + 92);
  size_t ndirs = std::min<size_t>({hdr_.num_rva_and_sizes, 16, (hdr_.optional_header_size - 96u) / 8});
  for (size_t i = 0; i < ndirs; i++) {
    hdr_.dirs[i].rva = f.u32(opt + 96 + 8 * i);
    hdr_.dirs[i].size = f.u32(opt + 100 + 8 * i);
  }

  if (!power_of_two(hdr_.section_alignment) || !power_of_two(hdr_.file_alignment)) {
    fail(Kind::malformed, std::format("alignment 0x{:X}/0x{:X} is not a power of two",
        hdr_.section_alignment, hdr_.file_alignment));
  }
  if (hdr_.size_of_image == 0 || hdr_.size_of_image > max_image_size) {
    fail(Kind::unsupported, std::format("SizeOfImage 0x{:X} is out of range", hdr_.size_of_image));
  }
  if (hdr_.num_sections > max_sections) {
    fail(Kind::malformed, std::format("{} sections", hdr_.num_sections));
  }

  size_t st = opt + hdr_.optional_header_size;
  f.need(st, size_t(hdr_.num_sections) * 40);
  for (size_t i = 0; i < hdr_.num_sections; i++) {
    size_t s = st + 40 * i;
    Section sec;
    std::string_view name = f.slice(s, 8);
    sec.name = std::string(name.substr(0, std::min(name.find('\0'), name.size())));
    sec.virtual_size = f.u32(s + 8);
    sec.rva = f.u32(s + 12);
    sec.raw_size = f.u32(s + 16);
    sec.raw_offset = f.u32(s + 20);
    sec.relocs_offset = f.u32(s + 24);
    sec.linenums_offset = f.u32(s + 28);
    sec.num_relocs = f.u16(s + 32);
    sec.num_linenums = f.u16(s + 34);
    sec.characteristics = f.u32(s + 36);
    sections_.push_back(std::move(sec));
  }
}

void Image::map_sections() {
  mapped_.assign(hdr_.size_of_image, '\0');
  // The headers are mapped too; modules (and the Borland RTL) may read them.
  size_t hn = std::min<size_t>({hdr_.size_of_headers, file_.size(), hdr_.size_of_image});
  mapped_.replace(0, hn, file_, 0, hn);

  uint64_t prev_end = 0;
  for (auto& sec : sections_) {
    // A zero VirtualSize means "same as the raw data" (old linkers).
    uint64_t vsize = sec.virtual_size ? sec.virtual_size : sec.raw_size;
    uint64_t end = uint64_t(sec.rva) + vsize;
    if (end > hdr_.size_of_image) {
      fail(Kind::malformed, std::format("section {} (0x{:X}+0x{:X}) extends past SizeOfImage 0x{:X}",
          sec.name, sec.rva, vsize, hdr_.size_of_image));
    }
    if (sec.rva < prev_end) {
      warnings_.push_back(std::format("section {} at 0x{:X} overlaps the previous one", sec.name, sec.rva));
    }
    uint64_t aligned = std::min<uint64_t>(detail::align_up(vsize, hdr_.section_alignment),
        hdr_.size_of_image - sec.rva);
    sec.mapped_size = static_cast<uint32_t>(aligned);
    prev_end = sec.rva + aligned;

    // Raw data past the virtual extent is file-alignment padding, not image.
    uint64_t copy = std::min<uint64_t>(sec.raw_size, aligned);
    if (copy && sec.raw_offset >= file_.size()) {
      warnings_.push_back(std::format("section {} raw data at 0x{:X} is past the end of the file",
          sec.name, sec.raw_offset));
      copy = 0;
    } else if (copy > file_.size() - sec.raw_offset) {
      warnings_.push_back(std::format("section {} raw data is truncated (0x{:X} of 0x{:X} bytes)",
          sec.name, file_.size() - sec.raw_offset, copy));
      copy = file_.size() - sec.raw_offset;
    }
    if (copy) mapped_.replace(sec.rva, copy, file_, sec.raw_offset, copy);
  }
}

std::string_view Image::view(uint32_t rva, uint32_t size) const {
  return Bytes(mapped_, "PE image").slice(rva, size);
}

const Section* Image::section_at(uint32_t rva) const {
  for (const auto& s : sections_) {
    if (rva >= s.rva && rva - s.rva < s.mapped_size) return &s;
  }
  return nullptr;
}

void Image::parse_imports() {
  const DataDir& d = hdr_.dirs[dir_import];
  if (!d.rva) return;
  Bytes m(mapped_, "PE import directory");
  detail::NameBudget budget("PE imports");
  for (size_t desc = d.rva;; desc += 20) {
    uint32_t oft = m.u32(desc), name_rva = m.u32(desc + 12), ft = m.u32(desc + 16);
    // The Windows loader stops at the first descriptor missing either its
    // name or its IAT (normally the all-zero terminator); going on would
    // bind "slots" at RVA 0, i.e. into the headers.
    if (!name_rva || !ft) break;
    std::string dll = m.cstr(name_rva);
    budget.spend(dll.size() + 1);
    // Borland's TLINK32 leaves OriginalFirstThunk zero; the IAT then carries
    // the lookup entries itself until it is bound.
    uint32_t lookup = oft ? oft : ft;
    for (uint32_t i = 0;; i++) {
      if (i > 0x10000) fail(Kind::malformed, "import thunk list for " + dll + " does not end");
      if (imports_.size() >= max_imports) fail(Kind::malformed, std::format("more than {} imports", max_imports));
      uint32_t v = m.u32(size_t(lookup) + 4 * size_t(i));
      if (!v) break;
      Import imp;
      imp.dll = dll;
      imp.slot_rva = ft + 4 * i;
      m.need(imp.slot_rva, 4);
      imp.slot = hdr_.image_base + imp.slot_rva;
      if (v & 0x80000000) {
        imp.by_ordinal = true;
        imp.ordinal = static_cast<uint16_t>(v);
      } else {
        imp.hint = m.u16(v);
        imp.name = m.cstr(size_t(v) + 2);
      }
      // Every Import carries its own copy of the DLL name as well.
      budget.spend(imp.dll.size() + imp.name.size());
      imports_.push_back(std::move(imp));
    }
  }
}

std::vector<std::string> Image::imported_dlls() const {
  std::vector<std::string> out;
  for (const auto& imp : imports_) {
    if (out.empty() || out.back() != imp.dll) out.push_back(imp.dll);
  }
  return out;
}

void Image::parse_exports() {
  const DataDir& d = hdr_.dirs[dir_export];
  if (!d.rva) return;
  Bytes m(mapped_, "PE export directory");
  size_t e = d.rva;
  exports_.present = true;
  exports_.timestamp = m.u32(e + 4);
  exports_.major = m.u16(e + 8);
  exports_.minor = m.u16(e + 10);
  uint32_t name_rva = m.u32(e + 12);
  exports_.ordinal_base = m.u32(e + 16);
  uint32_t nfuncs = m.u32(e + 20), nnames = m.u32(e + 24);
  uint32_t funcs = m.u32(e + 28), names = m.u32(e + 32), ords = m.u32(e + 36);
  if (name_rva) exports_.dll_name = m.cstr(name_rva);
  if (nfuncs > 0x10000 || nnames > 0x10000) {
    fail(Kind::malformed, std::format("export directory claims {} functions / {} names", nfuncs, nnames));
  }
  if (nfuncs && uint64_t(exports_.ordinal_base) + nfuncs - 1 > 0xFFFF) {
    fail(Kind::malformed, std::format("export ordinals {}+{} exceed 16 bits", exports_.ordinal_base, nfuncs));
  }
  m.need(funcs, size_t(nfuncs) * 4);
  m.need(names, size_t(nnames) * 4);
  m.need(ords, size_t(nnames) * 2);

  // A function can carry several names (aliases), each its own Export.
  detail::NameBudget budget("PE exports");
  std::vector<std::vector<std::string>> fnames(nfuncs);
  for (uint32_t j = 0; j < nnames; j++) {
    uint16_t idx = m.u16(size_t(ords) + 2 * j);
    if (idx >= nfuncs) {
      fail(Kind::malformed, std::format("export name {} refers to function {} of {}", j, idx, nfuncs));
    }
    fnames[idx].push_back(m.cstr(m.u32(size_t(names) + 4 * j)));
    budget.spend(fnames[idx].back().size() + 1);
  }
  for (uint32_t i = 0; i < nfuncs; i++) {
    uint32_t rva = m.u32(size_t(funcs) + 4 * i);
    if (!rva) continue; // hole in the ordinal range
    Export ex;
    ex.ordinal = static_cast<uint16_t>(exports_.ordinal_base + i);
    ex.rva = rva;
    // An RVA inside the export directory itself is a forwarder string.
    if (rva >= d.rva && rva - d.rva < d.size) ex.forwarder = m.cstr(rva);
    if (fnames[i].empty()) {
      budget.spend(ex.forwarder.size());
      exports_.exports.push_back(std::move(ex));
    } else {
      for (auto& n : fnames[i]) {
        Export named = ex;
        named.name = std::move(n);
        budget.spend(named.forwarder.size()); // each alias copies it
        exports_.exports.push_back(std::move(named));
      }
    }
  }
}

const Export* Image::find_export(std::string_view name) const {
  for (const auto& e : exports_.exports) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

const Export* Image::find_export(uint16_t ordinal) const {
  for (const auto& e : exports_.exports) {
    if (e.ordinal == ordinal) return &e;
  }
  return nullptr;
}

void Image::parse_relocations() {
  const DataDir& d = hdr_.dirs[dir_basereloc];
  if (!d.rva || !d.size) return;
  Bytes m(mapped_, "PE base relocations");
  m.need(d.rva, d.size);
  size_t p = d.rva, end = size_t(d.rva) + d.size;
  while (p + 8 <= end) {
    uint32_t page = m.u32(p), bsize = m.u32(p + 4);
    if (bsize == 0) break; // some linkers pad the directory with zeros
    if (bsize < 8 || bsize > end - p) {
      fail(Kind::malformed, std::format("base relocation block at 0x{:X} has size 0x{:X}", p, bsize));
    }
    size_t n = (bsize - 8) / 2;
    for (size_t i = 0; i < n; i++) {
      uint16_t w = m.u16(p + 8 + 2 * i);
      BaseReloc r;
      r.type = static_cast<uint8_t>(w >> 12);
      r.rva = page + (w & 0xFFF);
      if (r.type == rel_absolute) continue; // alignment padding
      if (r.type == rel_highadj) {
        // HIGHADJ carries the low half of the full value in the next slot.
        if (++i >= n) fail(Kind::malformed, "HIGHADJ relocation without its parameter");
        r.param = m.u16(p + 8 + 2 * i);
      }
      relocs_.push_back(r);
    }
    p += bsize;
  }
}

void Image::parse_resources() {
  const DataDir& d = hdr_.dirs[dir_resource];
  if (!d.rva) return;
  Bytes m(mapped_, "PE resource directory");
  size_t root = d.rva;
  std::set<size_t> seen;
  size_t entries = 0;
  // Pays for decoding each string id and for the type/name copies every
  // Resource below it stores.
  detail::NameBudget budget("PE resource directory");

  auto read_id = [&](uint32_t v) {
    if (!(v & 0x80000000)) return ResId::of(static_cast<uint16_t>(v));
    size_t s = root + (v & 0x7FFFFFFF);
    uint16_t len = m.u16(s);
    budget.spend(size_t(len) * 2);
    return ResId::of(utf16le_to_utf8(m.slice(s + 2, size_t(len) * 2)));
  };

  // Levels: 0 = type, 1 = name, 2 = language. The tree is walked with an
  // explicit visited set so a directory that points back at itself fails
  // instead of recursing forever.
  auto walk = [&](auto& self, size_t dir, int level, const ResId& type, const ResId& name) -> void {
    if (level > 2) fail(Kind::malformed, "resource directory nested deeper than type/name/language");
    if (!seen.insert(dir).second) fail(Kind::malformed, "resource directory loop");
    uint16_t nnamed = m.u16(dir + 12), nids = m.u16(dir + 14);
    size_t n = size_t(nnamed) + nids;
    m.need(dir + 16, n * 8);
    entries += n;
    if (entries > max_resources) fail(Kind::malformed, std::format("more than {} resource entries", max_resources));
    for (size_t i = 0; i < n; i++) {
      size_t ent = dir + 16 + 8 * i;
      uint32_t id = m.u32(ent), off = m.u32(ent + 4);
      ResId rid = read_id(id);
      if (off & 0x80000000) {
        size_t sub = root + (off & 0x7FFFFFFF);
        if (level == 0) self(self, sub, 1, rid, name);
        else if (level == 1) self(self, sub, 2, type, rid);
        else fail(Kind::malformed, "resource language entry points to a directory");
      } else {
        size_t de = root + off;
        Resource r;
        // A data entry above the language level (seen in some old tools)
        // gets the ids it has; the rest default to 0.
        r.type = level == 0 ? rid : type;
        r.name = level == 1 ? rid : (level == 2 ? name : ResId::of(uint16_t(0)));
        r.language = level == 2 ? static_cast<uint16_t>(id) : 0;
        r.data_rva = m.u32(de);
        r.size = m.u32(de + 4);
        r.codepage = m.u32(de + 8);
        budget.spend(r.type.str.size() + r.name.str.size());
        resources_.push_back(std::move(r));
      }
    }
  };
  walk(walk, root, 0, ResId{}, ResId{});
}

const Resource* Image::find_resource(const ResId& type, const ResId& name,
    std::optional<uint16_t> lang) const {
  for (const auto& r : resources_) {
    if (r.type.matches(type) && r.name.matches(name) && (!lang || r.language == *lang)) return &r;
  }
  return nullptr;
}

std::string_view Image::resource_data(const Resource& r) const {
  return view(r.data_rva, r.size);
}

std::map<uint32_t, std::string> Image::string_table(std::optional<uint16_t> lang) const {
  std::map<uint32_t, std::string> out;
  std::set<uint16_t> blocks_done;
  for (const auto& r : resources_) {
    // Block ids start at 1; a block 0 names no string ids.
    if (r.type.is_string || r.type.num != rt::string || r.name.is_string || !r.name.num) continue;
    if (lang ? r.language != *lang : !blocks_done.insert(r.name.num).second) continue;
    // Block n holds string ids (n-1)*16 .. (n-1)*16+15, each a WORD count
    // followed by that many UTF-16 units.
    Bytes b(resource_data(r), "RT_STRING block");
    size_t p = 0;
    for (uint32_t i = 0; i < 16 && p + 2 <= b.size(); i++) {
      uint16_t len = b.u16(p);
      std::string s = utf16le_to_utf8(b.slice(p + 2, size_t(len) * 2));
      p += 2 + size_t(len) * 2;
      // Borland's resource compiler counts the terminating NUL; drop it.
      while (!s.empty() && s.back() == '\0') s.pop_back();
      if (!s.empty()) out[(uint32_t(r.name.num) - 1) * 16 + i] = std::move(s);
    }
  }
  return out;
}

std::optional<VersionInfo> Image::version_info() const {
  for (const auto& r : resources_) {
    if (!r.type.is_string && r.type.num == rt::version) {
      return parse_version_info(resource_data(r), true);
    }
  }
  return std::nullopt;
}

void Image::parse_tls() {
  const DataDir& d = hdr_.dirs[dir_tls];
  if (!d.rva) return;
  Bytes m(mapped_, "PE TLS directory");
  Tls t;
  t.start_va = m.u32(d.rva);
  t.end_va = m.u32(d.rva + 4);
  t.index_va = m.u32(d.rva + 8);
  t.callbacks_va = m.u32(d.rva + 12);
  t.zero_fill = m.u32(d.rva + 16);
  t.characteristics = m.u32(d.rva + 20);
  // TLS is reported, not run; an unreadable callback list is only a warning.
  if (t.callbacks_va) {
    uint32_t rva = t.callbacks_va - hdr_.image_base;
    for (uint32_t i = 0; i < 1024; i++) {
      size_t at = size_t(rva) + 4 * size_t(i);
      if (!m.contains(at, 4)) {
        warnings_.push_back("TLS callback list runs out of the image");
        break;
      }
      uint32_t cb = m.u32(at);
      if (!cb) break;
      t.callbacks.push_back(cb);
    }
  }
  tls_ = std::move(t);
}

// ---- loading ----------------------------------------------------------------

Loaded load(const Image& img, ImageSink& sink, const Resolver& resolve) {
  const Header& h = img.header();
  Loaded out;
  out.size = h.size_of_image;
  out.base = sink.reserve(h.image_base, h.size_of_image);
  if (uint64_t(out.base) + h.size_of_image > 0x100000000ull) {
    fail(Kind::placement, std::format("sink base 0x{:08X} + 0x{:X} wraps the address space", out.base, h.size_of_image));
  }
  out.delta = out.base - h.image_base;
  if (out.delta && h.relocs_stripped()) {
    fail(Kind::placement, std::format("image must load at 0x{:08X} (relocations stripped) but the sink chose 0x{:08X}",
        h.image_base, out.base));
  }

  std::string buf(img.mapped());
  const char* what = "PE image";
  Bytes cur(buf, what); // reads see earlier fixups (HIGH/LOW pairs on one word)
  if (out.delta) {
    uint32_t delta = out.delta;
    for (const auto& r : img.relocations()) {
      switch (r.type) {
        case rel_highlow:
          detail::put32(buf, r.rva, cur.u32(r.rva) + delta, what);
          break;
        case rel_high:
          detail::put16(buf, r.rva, static_cast<uint16_t>(cur.u16(r.rva) + (delta >> 16)), what);
          break;
        case rel_low:
          detail::put16(buf, r.rva, static_cast<uint16_t>(cur.u16(r.rva) + delta), what);
          break;
        case rel_highadj: {
          // Rebuild the full 32-bit value from the stored high half and the
          // parameter's low half, relocate, and round back to a high half.
          uint32_t full = (uint32_t(cur.u16(r.rva)) << 16) + static_cast<int16_t>(r.param);
          full += delta + 0x8000;
          detail::put16(buf, r.rva, static_cast<uint16_t>(full >> 16), what);
          break;
        }
        default:
          fail(Kind::unsupported, std::format("base relocation type {} at RVA 0x{:X}", r.type, r.rva));
      }
      out.relocations_applied++;
    }
    // The mapped header records where the image actually is, as on Windows.
    size_t ib = size_t(h.pe_offset) + 24 + 28;
    if (ib + 4 <= std::min<size_t>(h.size_of_headers, buf.size())) {
      detail::put32(buf, ib, out.base, what);
    }
  }

  out.imports.reserve(img.imports().size());
  for (const auto& src : img.imports()) {
    Import imp = src;
    imp.slot = out.base + imp.slot_rva;
    if (resolve) {
      imp.value = resolve(imp);
      detail::put32(buf, imp.slot_rva, imp.value, what);
    } else {
      imp.value = cur.u32(imp.slot_rva);
    }
    out.imports.push_back(std::move(imp));
  }

  if (img.tls()) {
    Tls t = *img.tls();
    auto adj = [&](uint32_t& va) { if (va) va += out.delta; };
    adj(t.start_va);
    adj(t.end_va);
    adj(t.index_va);
    adj(t.callbacks_va);
    for (auto& cb : t.callbacks) adj(cb);
    out.tls = std::move(t);
  }

  out.entry = h.entry_rva ? out.base + h.entry_rva : 0;
  sink.write(out.base, buf.data(), buf.size());
  return out;
}

} // namespace adw::loader::pe
