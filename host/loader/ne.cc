#include "loader/ne.hh"

#include <set>

#include "loader/bytes.hh"

namespace adw::loader::ne {

using detail::Bytes;
using detail::fail;
using Kind = LoaderError::Kind;

const char* addr_type_name(AddrType t) {
  switch (t) {
    case AddrType::lobyte: return "lobyte";
    case AddrType::selector: return "selector";
    case AddrType::pointer32: return "pointer32";
    case AddrType::offset16: return "offset16";
    case AddrType::pointer48: return "pointer48";
    case AddrType::offset32: return "offset32";
  }
  return "?";
}

size_t addr_type_size(AddrType t) {
  switch (t) {
    case AddrType::lobyte: return 1;
    case AddrType::selector: return 2;
    case AddrType::pointer32: return 4;
    case AddrType::offset16: return 2;
    case AddrType::pointer48: return 6;
    case AddrType::offset32: return 4;
  }
  return 0;
}

const char* target_kind_name(TargetKind k) {
  switch (k) {
    case TargetKind::internal: return "internal";
    case TargetKind::import_ordinal: return "import_ordinal";
    case TargetKind::import_name: return "import_name";
    case TargetKind::os_fixup: return "os_fixup";
  }
  return "?";
}

const char* os_fixup_name(uint16_t type) {
  switch (type) {
    case 1: return "FIARQQ/FJARQQ";
    case 2: return "FISRQQ/FJSRQQ";
    case 3: return "FICRQQ/FJCRQQ";
    case 4: return "FIERQQ";
    case 5: return "FIDRQQ";
    case 6: return "FIWRQQ";
  }
  return "?";
}

// The Microsoft emulator's public constants. They are the differences between
// the real encodings and the INT 34h–3Dh ones: e.g. FWAIT+ESC0 "9B D8" (word
// 0xD89B) + FIDRQQ 0x5C32 = 0x34CD, i.e. "CD 34" (INT 34h); "90 9B"
// (NOP; FWAIT) + FIWRQQ 0xA23D = "CD 3D" (INT 3Dh). With a segment override,
// "9B 3E D8" (FWAIT DS: ESC0) + FIARQQ on the first word gives "CD 3C", and
// FJARQQ's high byte turns the ESC byte into the emulator's segment-tagged
// opcode byte: its top two bits become the segment (DS 00, SS 01, CS 10,
// ES 11; D8+40 = 18 for DS). ES is the one whose tag equals the ESC byte's own
// top bits, so there is no FJERQQ — but FIERQQ is still needed to turn
// "9B 26" (FWAIT ES:) into "CD 3C": 0x3CCD - 0x269B = 0x1632. (All 505 FIERQQ
// sites in the Classic corpus hold 9B 26 ESC.)
std::pair<uint16_t, uint16_t> os_fixup_constants(uint16_t type) {
  switch (type) {
    case 1: return {0xFE32, 0x4000}; // FIARQQ, FJARQQ
    case 2: return {0x0632, 0x8000}; // FISRQQ, FJSRQQ
    case 3: return {0x0E32, 0xC000}; // FICRQQ, FJCRQQ
    case 4: return {0x1632, 0x0000}; // FIERQQ
    case 5: return {0x5C32, 0x0000}; // FIDRQQ
    case 6: return {0xA23D, 0x0000}; // FIWRQQ
  }
  return {0, 0};
}

namespace {

// Pascal-string + WORD-ordinal table terminated by a zero length byte.
std::vector<Name> read_name_table(const Bytes& b, size_t p) {
  std::vector<Name> out;
  while (true) {
    uint8_t len = b.u8(p);
    if (!len) break;
    Name n;
    n.name = std::string(b.slice(p + 1, len));
    n.ordinal = b.u16(p + 1 + len);
    out.push_back(std::move(n));
    p += 1 + len + 2;
  }
  return out;
}

} // namespace

Image Image::from_file(const std::string& path) {
  return Image(read_file(path));
}

Image::Image(std::string data) : file_(std::move(data)) {
  parse_header();
  parse_segments();
  parse_names();
  parse_entries();
  parse_resources();
  parse_relocations();
}

void Image::parse_header() {
  Bytes f(file_, "NE file");
  if (file_.size() < 0x40 || file_.compare(0, 2, "MZ") != 0) {
    fail(Kind::bad_format, "not an MZ executable");
  }
  hdr_.ne_offset = f.u32(0x3C);
  size_t ne = hdr_.ne_offset;
  if (!f.contains(ne, 2) || f.slice(ne, 2) != "NE") {
    fail(Kind::bad_format, "no NE signature at e_lfanew");
  }
  f.need(ne, 0x40);
  hdr_.linker_major = f.u8(ne + 0x02);
  hdr_.linker_minor = f.u8(ne + 0x03);
  hdr_.entry_table_offset = f.u16(ne + 0x04);
  hdr_.entry_table_size = f.u16(ne + 0x06);
  hdr_.crc = f.u32(ne + 0x08);
  hdr_.flags = f.u16(ne + 0x0C);
  hdr_.autodata_segment = f.u16(ne + 0x0E);
  hdr_.heap_size = f.u16(ne + 0x10);
  hdr_.stack_size = f.u16(ne + 0x12);
  hdr_.ip = f.u16(ne + 0x14);
  hdr_.cs = f.u16(ne + 0x16);
  hdr_.sp = f.u16(ne + 0x18);
  hdr_.ss = f.u16(ne + 0x1A);
  hdr_.num_segments = f.u16(ne + 0x1C);
  hdr_.num_module_refs = f.u16(ne + 0x1E);
  hdr_.nonresident_names_size = f.u16(ne + 0x20);
  hdr_.segment_table_offset = f.u16(ne + 0x22);
  hdr_.resource_table_offset = f.u16(ne + 0x24);
  hdr_.resident_names_offset = f.u16(ne + 0x26);
  hdr_.module_ref_offset = f.u16(ne + 0x28);
  hdr_.imported_names_offset = f.u16(ne + 0x2A);
  hdr_.nonresident_names_file_offset = f.u32(ne + 0x2C);
  hdr_.num_moveable_entries = f.u16(ne + 0x30);
  hdr_.alignment_shift = f.u16(ne + 0x32);
  hdr_.num_resource_segments = f.u16(ne + 0x34);
  hdr_.target_os = f.u8(ne + 0x36);
  hdr_.os2_flags = f.u8(ne + 0x37);
  hdr_.gangload_offset = f.u16(ne + 0x38);
  hdr_.gangload_size = f.u16(ne + 0x3A);
  hdr_.min_code_swap = f.u16(ne + 0x3C);
  hdr_.expected_version = f.u16(ne + 0x3E);
  // A stored shift of 0 means the historical default of 512-byte sectors.
  sector_shift_ = hdr_.alignment_shift ? hdr_.alignment_shift : 9;
  if (sector_shift_ > 16) {
    fail(Kind::malformed, std::format("segment alignment shift {}", sector_shift_));
  }
}

void Image::parse_segments() {
  Bytes f(file_, "NE segment table");
  size_t st = size_t(hdr_.ne_offset) + hdr_.segment_table_offset;
  f.need(st, size_t(hdr_.num_segments) * 8);
  for (uint16_t i = 0; i < hdr_.num_segments; i++) {
    size_t e = st + 8 * size_t(i);
    Segment s;
    s.index = i + 1;
    uint16_t sector = f.u16(e), len = f.u16(e + 2);
    s.flags = f.u16(e + 4);
    uint16_t minalloc = f.u16(e + 6);
    s.file_offset = uint32_t(sector) << sector_shift_;
    s.file_size = sector ? (len ? len : 0x10000) : 0;
    s.min_alloc = minalloc ? minalloc : 0x10000;
    if (s.file_size) f.need(s.file_offset, s.file_size);
    segments_.push_back(std::move(s));
  }
}

const Segment& Image::segment(uint16_t index) const {
  if (index == 0 || index > segments_.size()) {
    fail(Kind::malformed, std::format("segment {} does not exist ({} segments)", index, segments_.size()));
  }
  return segments_[index - 1];
}

void Image::parse_names() {
  Bytes f(file_, "NE name tables");
  size_t ne = hdr_.ne_offset;
  resident_ = read_name_table(f, ne + hdr_.resident_names_offset);
  if (hdr_.nonresident_names_file_offset && hdr_.nonresident_names_size) {
    // Bounded by its declared size, not just by the file.
    Bytes nr(f.slice(hdr_.nonresident_names_file_offset, hdr_.nonresident_names_size),
        "NE non-resident name table");
    nonresident_ = read_name_table(nr, 0);
  }
  size_t mr = ne + hdr_.module_ref_offset;
  for (uint16_t i = 0; i < hdr_.num_module_refs; i++) {
    module_refs_.push_back(imported_name(f.u16(mr + 2 * size_t(i))));
  }
}

std::string Image::imported_name(uint16_t offset) const {
  Bytes f(file_, "NE imported-names table");
  return f.pstr(size_t(hdr_.ne_offset) + hdr_.imported_names_offset + offset);
}

std::string Image::module_name() const {
  return resident_.empty() ? std::string() : resident_[0].name;
}

std::string Image::description() const {
  return nonresident_.empty() ? std::string() : nonresident_[0].name;
}

void Image::parse_entries() {
  Bytes f(file_, "NE file");
  size_t start = size_t(hdr_.ne_offset) + hdr_.entry_table_offset;
  // The table ends at its declared size; files that declare 0 still carry a
  // terminated table, so fall back to "until the terminator".
  size_t len = hdr_.entry_table_size ? hdr_.entry_table_size : f.size() - std::min(start, f.size());
  Bytes t(f.slice(start, len), "NE entry table");
  size_t p = 0;
  uint32_t ordinal = 1;
  while (p < t.size()) {
    uint8_t count = t.u8(p);
    if (!count) break;
    uint8_t type = t.u8(p + 1);
    p += 2;
    if (type == 0) { // unused ordinals
      ordinal += count;
      continue;
    }
    for (uint8_t i = 0; i < count; i++, ordinal++) {
      if (ordinal > 0xFFFF) fail(Kind::malformed, "entry table exceeds 65535 ordinals");
      Entry e;
      e.ordinal = static_cast<uint16_t>(ordinal);
      if (type == 0xFF) {
        // flags, INT 3Fh (CD 3F), segment, offset — the INT 3Fh is the real
        // mode reload thunk and means nothing in protected mode.
        e.flags = t.u8(p);
        e.segment = t.u8(p + 3);
        e.offset = t.u16(p + 4);
        e.moveable = true;
        p += 6;
      } else {
        e.flags = t.u8(p);
        e.segment = type;
        e.offset = t.u16(p + 1);
        p += 3;
      }
      entry_index_[e.ordinal] = entries_.size();
      entries_.push_back(e);
    }
  }
}

const Entry* Image::find_entry(uint16_t ordinal) const {
  auto it = entry_index_.find(ordinal);
  return it == entry_index_.end() ? nullptr : &entries_[it->second];
}

std::optional<uint16_t> Image::find_ordinal(std::string_view name) const {
  for (const auto* table : {&resident_, &nonresident_}) {
    for (size_t i = 0; i < table->size(); i++) {
      // Entry 0 of each table is the module name / description, not an export.
      if (i == 0) continue;
      if (detail::iequals((*table)[i].name, name)) return (*table)[i].ordinal;
    }
  }
  return std::nullopt;
}

const Entry* Image::find_export(std::string_view name) const {
  auto ord = find_ordinal(name);
  return ord ? find_entry(*ord) : nullptr;
}

void Image::parse_resources() {
  if (hdr_.resource_table_offset == hdr_.resident_names_offset) return; // no resources
  Bytes f(file_, "NE resource table");
  size_t rt = size_t(hdr_.ne_offset) + hdr_.resource_table_offset;
  rsrc_shift_ = f.u16(rt);
  // A WORD shifted by more than 16 no longer fits the 32-bit offsets/sizes
  // (and no NE file is that large); refuse rather than silently wrap.
  if (rsrc_shift_ > 16) fail(Kind::malformed, std::format("resource alignment shift {}", rsrc_shift_));
  auto id = [&](uint16_t v) {
    // High bit: integer id. Otherwise an offset from the table start to a
    // Pascal string (the AD custom types: "RLEP", "PAL", "STRINGLIST", ...).
    if (v & 0x8000) return ResId::of(static_cast<uint16_t>(v & 0x7FFF));
    return ResId::of(f.pstr(rt + v));
  };
  size_t p = rt + 2;
  while (true) {
    uint16_t type_id = f.u16(p);
    if (!type_id) break;
    uint16_t count = f.u16(p + 2);
    p += 8;
    ResId type = id(type_id);
    f.need(p, size_t(count) * 12);
    for (uint16_t i = 0; i < count; i++, p += 12) {
      Resource r;
      r.type = type;
      r.file_offset = uint32_t(f.u16(p)) << rsrc_shift_;
      r.size = uint32_t(f.u16(p + 2)) << rsrc_shift_;
      r.flags = f.u16(p + 4);
      r.name = id(f.u16(p + 6));
      resources_.push_back(std::move(r));
    }
  }
}

const Resource* Image::find_resource(const ResId& type, const ResId& name) const {
  for (const auto& r : resources_) {
    if (r.type.matches(type) && r.name.matches(name)) return &r;
  }
  return nullptr;
}

std::string_view Image::resource_data(const Resource& r) const {
  // Sizes are in alignment units, so the last resource's padding can run past
  // the end of the file; the data itself cannot.
  Bytes f(file_, "NE resource");
  f.need(r.file_offset, 0);
  return f.slice(r.file_offset, std::min<size_t>(r.size, f.size() - r.file_offset));
}

std::map<uint32_t, std::string> Image::string_table() const {
  std::map<uint32_t, std::string> out;
  for (const auto& r : resources_) {
    // Block ids start at 1; a block 0 names no string ids.
    if (r.type.is_string || r.type.num != rt::string || r.name.is_string || !r.name.num) continue;
    // Block n holds ids (n-1)*16 ..+15, each a length byte and 8-bit text.
    Bytes b(resource_data(r), "RT_STRING block");
    size_t p = 0;
    for (uint32_t i = 0; i < 16 && p < b.size(); i++) {
      uint8_t len = b.u8(p);
      std::string s(b.slice(p + 1, len));
      p += 1 + size_t(len);
      // Some resource compilers count the terminating NUL; drop it.
      while (!s.empty() && s.back() == '\0') s.pop_back();
      if (!s.empty()) out[(uint32_t(r.name.num) - 1) * 16 + i] = std::move(s);
    }
  }
  return out;
}

std::optional<VersionInfo> Image::version_info() const {
  for (const auto& r : resources_) {
    if (!r.type.is_string && r.type.num == rt::version) {
      return parse_version_info(resource_data(r), false);
    }
  }
  return std::nullopt;
}

void Image::parse_relocations() {
  Bytes f(file_, "NE relocations");
  // Segments may point at the same file data, so the file size alone does not
  // bound the record count; a hostile table could otherwise ask for billions.
  // LUNATIC.AD, the largest in the corpus, has 2223.
  constexpr size_t max_records = 1u << 20;
  size_t total = 0;
  // Each import record copies its module (and imported) name.
  detail::NameBudget budget("NE relocations");
  for (auto& s : segments_) {
    if (!s.has_relocs()) continue;
    if (!s.file_size) {
      warnings_.push_back(std::format("segment {} has RELOCINFO but no file data", s.index));
      continue;
    }
    // The records follow the segment's file data directly.
    size_t p = size_t(s.file_offset) + s.file_size;
    uint16_t count = f.u16(p);
    p += 2;
    f.need(p, size_t(count) * 8);
    total += count;
    if (total > max_records) fail(Kind::malformed, std::format("more than {} relocation records", max_records));
    s.relocations.reserve(count);
    for (uint16_t i = 0; i < count; i++, p += 8) {
      Relocation r;
      r.raw_addr_type = f.u8(p);
      uint8_t rflags = f.u8(p + 1);
      r.offset = f.u16(p + 2);
      switch (r.raw_addr_type & 0x7F) {
        case 0: case 2: case 3: case 5: case 11: case 13:
          r.addr_type = static_cast<AddrType>(r.raw_addr_type & 0x7F);
          break;
        default:
          fail(Kind::unsupported, std::format("segment {} relocation {}: address type {}",
              s.index, i, r.raw_addr_type));
      }
      r.kind = static_cast<TargetKind>(rflags & 3);
      r.additive = rflags & 4;
      switch (r.kind) {
        case TargetKind::internal:
          r.segment = f.u8(p + 4);
          if (r.segment == 0xFF) r.entry_ordinal = f.u16(p + 6);
          else r.target_offset = f.u16(p + 6);
          break;
        case TargetKind::import_ordinal:
        case TargetKind::import_name:
          r.module_index = f.u16(p + 4);
          if (r.module_index == 0 || r.module_index > module_refs_.size()) {
            fail(Kind::malformed, std::format("segment {} relocation {}: module {} of {}",
                s.index, i, r.module_index, module_refs_.size()));
          }
          r.module = module_refs_[r.module_index - 1];
          if (r.kind == TargetKind::import_ordinal) {
            r.ordinal = f.u16(p + 6);
          } else {
            r.name_offset = f.u16(p + 6);
            r.name = imported_name(r.name_offset);
          }
          budget.spend(r.module.size() + r.name.size());
          break;
        case TargetKind::os_fixup:
          r.os_fixup = f.u16(p + 4);
          break;
      }
      s.relocations.push_back(std::move(r));
    }
  }
}

std::string_view Image::segment_file_data(const Segment& s) const {
  if (!s.file_size) return {};
  return Bytes(file_, "NE segment").slice(s.file_offset, s.file_size);
}

std::string Image::segment_image(const Segment& s, uint32_t size) const {
  std::string buf(size, '\0');
  std::string_view src = segment_file_data(s);
  if (s.flags & seg_iterated) {
    // Iterated data: records of (WORD repeat count, WORD length, bytes).
    Bytes b(src, "NE iterated segment");
    size_t p = 0, out = 0;
    while (p + 4 <= b.size()) {
      uint16_t reps = b.u16(p), len = b.u16(p + 2);
      std::string_view chunk = b.slice(p + 4, len);
      p += 4 + size_t(len);
      // An empty record expands to nothing however often it repeats; looping
      // over its repeats anyway let 64K of such records cost 10^9 no-op
      // copies (seconds per segment, and segments may share the data).
      if (!len) continue;
      if (uint64_t(reps) * len > buf.size() - out) {
        fail(Kind::malformed, std::format("segment {} iterated data overflows its 0x{:X} bytes", s.index, size));
      }
      for (uint16_t r = 0; r < reps; r++, out += len) buf.replace(out, len, chunk);
    }
  } else {
    if (src.size() > buf.size()) {
      fail(Kind::malformed, std::format("segment {} data (0x{:X}) exceeds its allocation 0x{:X}",
          s.index, src.size(), size));
    }
    buf.replace(0, src.size(), src);
  }
  return buf;
}

// ---- loading ----------------------------------------------------------------

const PlacedSegment& Placement::segment(uint16_t index) const {
  if (index == 0 || index > segments.size()) {
    fail(Kind::malformed, std::format("segment {} was not placed", index));
  }
  return segments[index - 1];
}

uint32_t segment_alloc_size(const Image& img, const Segment& s, const LoadOptions& opt) {
  const Header& h = img.header();
  uint32_t size = (s.flags & seg_iterated) ? s.min_alloc : std::max(s.file_size, s.min_alloc);
  if (opt.autodata_heap_stack && s.index == h.autodata_segment &&
      (h.flags & (mod_singledata | mod_multipledata))) {
    size += uint32_t(h.heap_size) + h.stack_size;
  }
  return std::min<uint32_t>(size, 0x10000);
}

Placement place(const Image& img, ImageSink& sink, const LoadOptions& opt) {
  if (img.header().flags & mod_selfload) {
    fail(Kind::unsupported, "self-loading NE application");
  }
  Placement pl;
  for (const auto& s : img.segments()) {
    PlacedSegment ps;
    ps.index = s.index;
    ps.flags = s.flags;
    ps.size = segment_alloc_size(img, s, opt);
    ps.reserved = opt.reserve_64k ? 0x10000 : ps.size;
    ps.base = sink.reserve(0, ps.reserved);
    if (uint64_t(ps.base) + ps.reserved > 0x100000000ull) {
      fail(Kind::placement, std::format("sink base 0x{:08X} for segment {} wraps the address space", ps.base, s.index));
    }
    pl.segments.push_back(ps);
  }
  return pl;
}

namespace {

// Stores (or, for additive records, adds) a resolved target at one source.
void patch(std::string& buf, size_t at, AddrType type, bool additive, const FarPtr& t,
    const char* what) {
  Bytes cur(buf, what);
  switch (type) {
    case AddrType::lobyte:
      detail::put8(buf, at, static_cast<uint8_t>((additive ? cur.u8(at) : 0) + t.offset), what);
      break;
    case AddrType::offset16:
      detail::put16(buf, at, static_cast<uint16_t>((additive ? cur.u16(at) : 0) + t.offset), what);
      break;
    case AddrType::offset32:
      detail::put32(buf, at, (additive ? cur.u32(at) : 0) + t.offset, what);
      break;
    case AddrType::selector:
      // A selector cannot meaningfully be added to; additive ones (Borland
      // emits them over a zero word) are simply stored.
      detail::put16(buf, at, t.selector, what);
      break;
    case AddrType::pointer32:
      detail::put16(buf, at, static_cast<uint16_t>((additive ? cur.u16(at) : 0) + t.offset), what);
      detail::put16(buf, at + 2, t.selector, what);
      break;
    case AddrType::pointer48:
      detail::put32(buf, at, (additive ? cur.u32(at) : 0) + t.offset, what);
      detail::put16(buf, at + 4, t.selector, what);
      break;
  }
}

} // namespace

LoadStats load_segments(const Image& img, const Placement& pl, ImageSink& sink,
    const Resolver& resolve, const LoadOptions& opt) {
  LoadStats st;
  for (const auto& s : img.segments()) {
    const PlacedSegment& ps = pl.segment(s.index);
    std::string buf = img.segment_image(s, ps.size);
    std::string what_s = std::format("NE segment {}", s.index);
    const char* what = what_s.c_str();

    for (const auto& r : s.relocations) {
      st.records++;
      if (r.kind == TargetKind::os_fixup) {
        // One instruction per record: never chained (the source holds code).
        st.os_fixups++;
        if (opt.os_fixups == OsFixupMode::emulate) {
          auto [fi, fj] = os_fixup_constants(r.os_fixup);
          Bytes cur(buf, what);
          detail::put16(buf, r.offset, static_cast<uint16_t>(cur.u16(r.offset) + fi), what);
          if (fj) detail::put8(buf, r.offset + 2u, static_cast<uint8_t>(cur.u8(r.offset + 2u) + (fj >> 8)), what);
          st.os_fixups_applied++;
        }
        st.by_kind[std::format("os_fixup/{}", os_fixup_name(r.os_fixup))]++;
        continue;
      }

      Target t;
      t.kind = r.kind;
      if (r.kind == TargetKind::internal) {
        if (r.segment == 0xFF) {
          const Entry* e = img.find_entry(r.entry_ordinal);
          if (!e) {
            fail(Kind::malformed, std::format("segment {} relocation at 0x{:X}: no entry ordinal {}",
                s.index, r.offset, r.entry_ordinal));
          }
          if (e->segment == 0xFE) {
            fail(Kind::unsupported, std::format("relocation to constant entry {}", r.entry_ordinal));
          }
          t.segment = e->segment;
          t.offset = e->offset;
          t.entry_ordinal = r.entry_ordinal;
        } else {
          t.segment = r.segment;
          t.offset = r.target_offset;
        }
        t.segment_base = pl.segment(t.segment).base; // validates the index
      } else {
        t.module_index = r.module_index;
        t.module = r.module;
        t.ordinal = r.ordinal;
        t.name = r.name;
      }
      if (!resolve) throw std::invalid_argument("ne::load_segments needs a resolver");
      FarPtr fp = resolve(t);

      if (r.additive) {
        patch(buf, r.offset, r.addr_type, true, fp, what);
        st.additive++;
        st.locations++;
      } else {
        // Non-additive: each source holds the offset of the next one in the
        // chain, 0xFFFF-terminated. Read the link before overwriting it.
        std::set<uint32_t> seen;
        uint32_t at = r.offset;
        while (true) {
          if (!seen.insert(at).second) {
            fail(Kind::malformed, std::format("segment {} relocation chain from 0x{:X} loops at 0x{:X}",
                s.index, r.offset, at));
          }
          // The link is a WORD even for LOBYTE sources (the documented rule;
          // no file in the corpus has LOBYTE records at all).
          uint16_t next = Bytes(buf, what).u16(at);
          patch(buf, at, r.addr_type, false, fp, what);
          st.locations++;
          if (next == 0xFFFF) break;
          if (next >= buf.size()) {
            fail(Kind::malformed, std::format("segment {} relocation chain from 0x{:X} leaves the segment (0x{:X})",
                s.index, r.offset, next));
          }
          at = next;
        }
      }
      st.by_kind[std::format("{}/{}", target_kind_name(r.kind), addr_type_name(r.addr_type))]++;
    }
    // Fixups are bounded by the segment's own size (above); the rest of a
    // larger reservation (reserve_64k) is zero, so every reserved byte is
    // written, as ImageSink promises.
    if (ps.reserved > buf.size()) buf.resize(ps.reserved, '\0');
    sink.write(ps.base, buf.data(), buf.size());
  }
  return st;
}

Loaded load(const Image& img, ImageSink& sink, const Resolver& resolve, const LoadOptions& opt) {
  Loaded out;
  out.placement = place(img, sink, opt);
  out.stats = load_segments(img, out.placement, sink, resolve, opt);
  return out;
}

} // namespace adw::loader::ne
