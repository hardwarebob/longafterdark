// adw::loader::ne — 16-bit NE images: the Classic modules (*.AD), ADXPL300.DLL,
// OLDMOD16.DLL and the helper DLLs they load.
//
// Image parses every table up front (segments with their relocation records,
// entry table, resident/non-resident names, module references, imported
// names, resources). Loading is two steps so a host can allocate selectors in
// between: place() reserves one linear block per segment; load_segments()
// builds each segment (file data, zero-filled to its allocation), applies its
// relocations through a Resolver that maps targets to selector:offset, and
// writes it to the sink. load() does both when the resolver can work from the
// segment bases alone (Target carries them).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "loader/image.hh"

namespace adw::loader::ne {

// Segment-table flags (NEWEXE.H NSxxx).
constexpr uint16_t seg_data = 0x0001;
constexpr uint16_t seg_iterated = 0x0008;
constexpr uint16_t seg_moveable = 0x0010;
constexpr uint16_t seg_pure = 0x0020;       // shareable
constexpr uint16_t seg_preload = 0x0040;
constexpr uint16_t seg_readonly = 0x0080;   // execute-only for code, read-only for data
constexpr uint16_t seg_relocinfo = 0x0100;
constexpr uint16_t seg_dpl_mask = 0x0C00;
constexpr uint16_t seg_discardable = 0x1000;
constexpr uint16_t seg_32bit = 0x2000;      // 16:32 segment (rare in Windows NE)

// Module flags (header word at 0x0C).
constexpr uint16_t mod_singledata = 0x0001;
constexpr uint16_t mod_multipledata = 0x0002;
constexpr uint16_t mod_global_init = 0x0004;
constexpr uint16_t mod_protmode = 0x0008;
constexpr uint16_t mod_i86 = 0x0010, mod_i286 = 0x0020, mod_i386 = 0x0040, mod_x87 = 0x0080;
constexpr uint16_t mod_selfload = 0x0800;
constexpr uint16_t mod_link_errors = 0x2000;
constexpr uint16_t mod_library = 0x8000;

// Relocation source ("address") types.
enum class AddrType : uint8_t {
  lobyte = 0,      // low byte of the target offset
  selector = 2,    // 16-bit selector
  pointer32 = 3,   // 16:16 far pointer (offset, then selector)
  offset16 = 5,    // 16-bit offset
  pointer48 = 11,  // 16:32 far pointer (32-bit offset, then selector)
  offset32 = 13,   // 32-bit offset
};
const char* addr_type_name(AddrType t);
// Bytes a source location occupies.
size_t addr_type_size(AddrType t);

enum class TargetKind : uint8_t {
  internal = 0,        // fixed segment:offset, or a moveable entry by ordinal
  import_ordinal = 1,
  import_name = 2,
  os_fixup = 3,        // floating-point emulator fixup (FIxRQQ/FJxRQQ)
};
const char* target_kind_name(TargetKind k);

// OSFIXUP types: which Microsoft floating-point emulator constants apply. The
// file holds real x87 code (FWAIT/ESC); a Windows without a coprocessor adds
// FIxRQQ to the word at the source (and FJxRQQ to the byte after it) to turn
// it into INT 34h–3Dh calls into WIN87EM. See os_fixup_constants.
enum class OsFixup : uint16_t {
  fiarqq = 1, // FIARQQ/FJARQQ — DS-overridden ESC
  fisrqq = 2, // FISRQQ/FJSRQQ — SS-overridden ESC
  ficrqq = 3, // FICRQQ/FJCRQQ — CS-overridden ESC
  fierqq = 4, // FIERQQ         — ES-overridden ESC (no FJ: ES's tag is 11)
  fidrqq = 5, // FIDRQQ         — plain FWAIT+ESC
  fiwrqq = 6, // FIWRQQ         — standalone FWAIT
};
const char* os_fixup_name(uint16_t type);
// FI value (added to the word at the source) and FJ value (whose high byte is
// added to the byte at source+2); {0,0} for an unknown type.
std::pair<uint16_t, uint16_t> os_fixup_constants(uint16_t type);

struct Relocation {
  AddrType addr_type = AddrType::offset16;
  uint8_t raw_addr_type = 0;  // as stored (bit 7 is sometimes set; ignored)
  TargetKind kind = TargetKind::internal;
  bool additive = false;
  uint16_t offset = 0;        // source; the head of a chain unless additive
  // internal
  uint8_t segment = 0;        // 1-based segment, or 0xFF: moveable, see entry_ordinal
  uint16_t target_offset = 0; // fixed internal: offset in segment
  uint16_t entry_ordinal = 0; // moveable internal
  // imports
  uint16_t module_index = 0;  // 1-based into module_refs()
  uint16_t ordinal = 0;       // import_ordinal
  uint16_t name_offset = 0;   // import_name: offset into the imported-names table
  std::string module, name;   // resolved from the tables
  // os_fixup
  uint16_t os_fixup = 0;
};

struct Segment {
  uint16_t index = 0;        // 1-based
  uint32_t file_offset = 0;  // bytes; 0 ⇒ no file data (all zero)
  uint32_t file_size = 0;    // bytes of file data (a stored 0 with data means 64K)
  uint16_t flags = 0;
  uint32_t min_alloc = 0;    // bytes (a stored 0 means 64K)
  std::vector<Relocation> relocations;

  bool is_data() const { return flags & seg_data; }
  bool is_moveable() const { return flags & seg_moveable; }
  bool has_relocs() const { return flags & seg_relocinfo; }
};

struct Entry {
  uint16_t ordinal = 0;
  uint8_t segment = 0;   // 1-based; 0xFE = constant (offset is the value)
  uint16_t offset = 0;
  uint8_t flags = 0;     // bit 0 exported, bit 1 shared data, bits 3-7 parameter words
  bool moveable = false; // from a moveable (INT 3Fh) bundle

  bool exported() const { return flags & 1; }
  bool shared_data() const { return flags & 2; }
  uint8_t param_words() const { return flags >> 3; }
};

struct Name {
  std::string name;
  uint16_t ordinal = 0;
};

struct Resource {
  ResId type, name;
  uint32_t file_offset = 0, size = 0; // bytes (already shifted)
  uint16_t flags = 0;                 // 0x10 moveable, 0x20 pure, 0x40 preload
};

struct Header {
  uint32_t ne_offset = 0;
  uint8_t linker_major = 0, linker_minor = 0;
  uint16_t entry_table_offset = 0, entry_table_size = 0;
  uint32_t crc = 0;
  uint16_t flags = 0, autodata_segment = 0, heap_size = 0, stack_size = 0;
  uint16_t ip = 0, cs = 0, sp = 0, ss = 0;
  uint16_t num_segments = 0, num_module_refs = 0, nonresident_names_size = 0;
  uint16_t segment_table_offset = 0, resource_table_offset = 0, resident_names_offset = 0;
  uint16_t module_ref_offset = 0, imported_names_offset = 0;
  uint32_t nonresident_names_file_offset = 0;
  uint16_t num_moveable_entries = 0, alignment_shift = 0, num_resource_segments = 0;
  uint8_t target_os = 0, os2_flags = 0;
  uint16_t gangload_offset = 0, gangload_size = 0, min_code_swap = 0;
  uint16_t expected_version = 0; // 0x030A = Windows 3.10

  bool is_dll() const { return flags & mod_library; }
};

class Image {
public:
  // Parses everything; throws LoaderError (bad_format for non-NE input).
  explicit Image(std::string data);
  static Image from_file(const std::string& path);

  const Header& header() const { return hdr_; }
  const std::vector<Segment>& segments() const { return segments_; }
  const Segment& segment(uint16_t index) const; // 1-based; throws malformed
  const std::vector<Entry>& entries() const { return entries_; }
  const std::vector<Name>& resident_names() const { return resident_; }
  const std::vector<Name>& nonresident_names() const { return nonresident_; }
  const std::vector<std::string>& module_refs() const { return module_refs_; }
  const std::vector<Resource>& resources() const { return resources_; }
  uint16_t resource_alignment_shift() const { return rsrc_shift_; }
  const std::vector<std::string>& warnings() const { return warnings_; }

  // First resident name (ordinal 0) and first non-resident name.
  std::string module_name() const;
  std::string description() const;

  const Entry* find_entry(uint16_t ordinal) const;
  // Case-insensitive search of the resident then non-resident names, as
  // GetProcAddress does for a Win16 module.
  std::optional<uint16_t> find_ordinal(std::string_view name) const;
  // find_ordinal + find_entry. The Classic modules' "MODULE" entry is found
  // this way: it is not always ordinal 1 (BADDOG3 #17; DOSSHELL, MESSAGE3,
  // WMORPH, ZOOM #2).
  const Entry* find_export(std::string_view name) const;
  // Pascal string at an offset in the imported-names table.
  std::string imported_name(uint16_t offset) const;

  const Resource* find_resource(const ResId& type, const ResId& name) const;
  std::string_view resource_data(const Resource& r) const;
  // RT_STRING tables: string id → text (8-bit, as stored, minus counted
  // trailing NULs; empty ones omitted).
  std::map<uint32_t, std::string> string_table() const;
  std::optional<VersionInfo> version_info() const;

  // The raw file bytes of a segment, and the segment as it is laid out in
  // memory before fixups (iterated data expanded, zero-filled to size).
  std::string_view segment_file_data(const Segment& s) const;
  std::string segment_image(const Segment& s, uint32_t size) const;

  std::string_view file() const { return file_; }

private:
  void parse_header();
  void parse_segments();
  void parse_names();
  void parse_entries();
  void parse_resources();
  void parse_relocations();

  std::string file_;
  Header hdr_;
  uint32_t sector_shift_ = 9;
  uint16_t rsrc_shift_ = 0;
  std::vector<Segment> segments_;
  std::vector<Entry> entries_;
  std::map<uint16_t, size_t> entry_index_;
  std::vector<Name> resident_, nonresident_;
  std::vector<std::string> module_refs_;
  std::vector<Resource> resources_;
  std::vector<std::string> warnings_;
};

// What a Resolver is asked to map to selector:offset. For internal targets the
// loader has already followed the entry table, so segment/offset always name
// the final location and segment_base is where place() put that segment.
struct Target {
  TargetKind kind = TargetKind::internal;
  uint16_t segment = 0;        // internal: 1-based
  uint32_t offset = 0;         // internal: offset in that segment
  uint32_t segment_base = 0;   // internal: linear base from place()
  uint16_t entry_ordinal = 0;  // internal: nonzero when named by a moveable entry
  uint16_t module_index = 0;   // imports: 1-based
  std::string_view module;     // imports: module name from the module-ref table
  uint16_t ordinal = 0;        // import_ordinal
  std::string_view name;       // import_name
};

struct FarPtr {
  uint16_t selector = 0;
  uint32_t offset = 0;
};

using Resolver = std::function<FarPtr(const Target&)>;

enum class OsFixupMode {
  leave,   // keep the real x87 instructions in the file (a Windows with an FPU)
  emulate, // add the FIxRQQ/FJxRQQ constants: INT 34h–3Dh emulator calls
};

struct LoadOptions {
  OsFixupMode os_fixups = OsFixupMode::leave;
  // The automatic data segment gets the local heap and (for tasks) the stack
  // appended to its min-alloc, as the Windows loader allocates it, capped at 64K.
  bool autodata_heap_stack = true;
  // Reserve a full 64K for every segment so the host can grow segments in
  // place (GlobalReAlloc of DGROUP) without moving them.
  bool reserve_64k = false;
};

struct PlacedSegment {
  uint16_t index = 0;
  uint32_t base = 0;       // linear address from the sink
  uint32_t size = 0;       // bytes written (file data + zero fill)
  uint32_t reserved = 0;   // bytes reserved (≥ size)
  uint16_t flags = 0;
};

struct Placement {
  std::vector<PlacedSegment> segments; // index i ↔ segment i+1
  const PlacedSegment& segment(uint16_t index) const; // 1-based; throws malformed
};

struct LoadStats {
  size_t records = 0;            // relocation records processed
  size_t locations = 0;          // source locations patched (chains expanded)
  size_t additive = 0;
  size_t os_fixups = 0;          // OSFIXUP records seen
  size_t os_fixups_applied = 0;  // ... and patched (OsFixupMode::emulate)
  std::map<std::string, size_t> by_kind; // "import_ordinal/pointer32" → count
};

// The per-segment allocation place() reserves under these options.
uint32_t segment_alloc_size(const Image& img, const Segment& s, const LoadOptions& opt);

Placement place(const Image& img, ImageSink& sink, const LoadOptions& opt = {});
LoadStats load_segments(const Image& img, const Placement& pl, ImageSink& sink,
    const Resolver& resolve, const LoadOptions& opt = {});

struct Loaded {
  Placement placement;
  LoadStats stats;
};
Loaded load(const Image& img, ImageSink& sink, const Resolver& resolve,
    const LoadOptions& opt = {});

} // namespace adw::loader::ne
