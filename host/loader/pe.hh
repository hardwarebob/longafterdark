// adw::loader::pe — PE32 (i386) images: the AD4 modules (*.AD), ADXPL510.DLL,
// and the rest of the 32-bit corpus.
//
// Image parses a file completely up front (headers, sections, imports, exports,
// base relocations, the resource tree, TLS) and keeps a "mapped" copy laid out
// at ImageBase exactly as the Windows loader would, before fixups. load()
// copies that view, relocates it to wherever the sink put it, binds the IAT
// through a resolver, and writes it to the sink in one piece.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "loader/image.hh"

namespace adw::loader::pe {

constexpr uint16_t machine_i386 = 0x014C;
constexpr uint16_t file_relocs_stripped = 0x0001;
constexpr uint16_t file_executable = 0x0002;
constexpr uint16_t file_dll = 0x2000;

// Data directory slots.
enum : uint32_t {
  dir_export = 0, dir_import = 1, dir_resource = 2, dir_exception = 3,
  dir_security = 4, dir_basereloc = 5, dir_debug = 6, dir_architecture = 7,
  dir_globalptr = 8, dir_tls = 9, dir_load_config = 10, dir_bound_import = 11,
  dir_iat = 12, dir_delay_import = 13, dir_clr = 14,
};

// IMAGE_REL_BASED_* types that exist in i386 images.
enum : uint8_t {
  rel_absolute = 0, rel_high = 1, rel_low = 2, rel_highlow = 3, rel_highadj = 4,
};

struct DataDir {
  uint32_t rva = 0, size = 0;
};

struct Header {
  // COFF file header
  uint32_t pe_offset = 0; // e_lfanew
  uint16_t machine = 0, num_sections = 0;
  uint32_t timestamp = 0, symtab_offset = 0, num_symbols = 0;
  uint16_t optional_header_size = 0, characteristics = 0;
  // optional header (PE32)
  uint16_t magic = 0;
  uint8_t linker_major = 0, linker_minor = 0;
  uint32_t size_of_code = 0, size_of_initialized_data = 0, size_of_uninitialized_data = 0;
  uint32_t entry_rva = 0, base_of_code = 0, base_of_data = 0;
  uint32_t image_base = 0, section_alignment = 0, file_alignment = 0;
  uint16_t os_major = 0, os_minor = 0, image_major = 0, image_minor = 0;
  uint16_t subsystem_major = 0, subsystem_minor = 0;
  uint32_t win32_version = 0, size_of_image = 0, size_of_headers = 0, checksum = 0;
  uint16_t subsystem = 0, dll_characteristics = 0;
  uint32_t stack_reserve = 0, stack_commit = 0, heap_reserve = 0, heap_commit = 0;
  uint32_t loader_flags = 0, num_rva_and_sizes = 0;
  std::array<DataDir, 16> dirs{};

  bool is_dll() const { return characteristics & file_dll; }
  bool relocs_stripped() const { return characteristics & file_relocs_stripped; }
};

struct Section {
  std::string name; // up to 8 chars; "/n" long names are left as-is
  uint32_t virtual_size = 0, rva = 0, raw_size = 0, raw_offset = 0;
  uint32_t relocs_offset = 0, linenums_offset = 0;
  uint16_t num_relocs = 0, num_linenums = 0;
  uint32_t characteristics = 0;
  uint32_t mapped_size = 0; // bytes the section occupies in memory (aligned)
};

struct Import {
  std::string dll;        // as named in the image (case preserved)
  std::string name;       // empty when imported by ordinal
  uint16_t ordinal = 0;   // valid when by_ordinal
  uint16_t hint = 0;      // name-table hint when imported by name
  bool by_ordinal = false;
  uint32_t slot_rva = 0;  // IAT slot
  uint32_t slot = 0;      // absolute slot address (ImageBase-relative in Image, actual base in Loaded)
  uint32_t value = 0;     // what load() stored in the slot (0 in Image)
};

struct Export {
  uint16_t ordinal = 0;   // as importers name it (Base already added)
  std::string name;       // empty when exported by ordinal only
  uint32_t rva = 0;       // for a forwarder, the RVA of its string
  std::string forwarder;  // "DLL.Name" or "DLL.#12"; empty if not forwarded
  bool is_forwarder() const { return !forwarder.empty(); }
};

struct ExportDirectory {
  bool present = false;
  std::string dll_name;
  uint32_t timestamp = 0, ordinal_base = 0;
  uint16_t major = 0, minor = 0;
  std::vector<Export> exports; // by ordinal; unused slots are omitted
};

struct BaseReloc {
  uint32_t rva = 0;
  uint8_t type = 0;    // rel_*
  uint16_t param = 0;  // low half for rel_highadj
};

struct Resource {
  ResId type, name;
  uint16_t language = 0;
  uint32_t data_rva = 0, size = 0, codepage = 0;
};

struct Tls {
  uint32_t start_va = 0, end_va = 0, index_va = 0, callbacks_va = 0;
  uint32_t zero_fill = 0, characteristics = 0;
  std::vector<uint32_t> callbacks; // VAs at ImageBase
};

class Image {
public:
  // Parses everything; throws LoaderError (bad_format for non-PE32 input).
  explicit Image(std::string data);
  static Image from_file(const std::string& path);

  const Header& header() const { return hdr_; }
  const std::vector<Section>& sections() const { return sections_; }
  const std::vector<Import>& imports() const { return imports_; }
  const ExportDirectory& export_directory() const { return exports_; }
  const std::vector<Export>& exports() const { return exports_.exports; }
  const std::vector<BaseReloc>& relocations() const { return relocs_; }
  const std::vector<Resource>& resources() const { return resources_; }
  const std::optional<Tls>& tls() const { return tls_; }
  // Non-fatal oddities found while parsing (clamped raw data and the like).
  const std::vector<std::string>& warnings() const { return warnings_; }

  // DLL names in import-directory order.
  std::vector<std::string> imported_dlls() const;
  const Export* find_export(std::string_view name) const;  // case-sensitive, as GetProcAddress
  const Export* find_export(uint16_t ordinal) const;
  // lang empty ⇒ first language present (the order the tree stores them).
  const Resource* find_resource(const ResId& type, const ResId& name,
      std::optional<uint16_t> lang = {}) const;
  std::string_view resource_data(const Resource& r) const;
  // RT_STRING tables: string id → UTF-8 text (trailing NULs that the resource
  // compiler counted are dropped; empty strings omitted).
  std::map<uint32_t, std::string> string_table(std::optional<uint16_t> lang = {}) const;
  // The first RT_VERSION resource, decoded.
  std::optional<VersionInfo> version_info() const;

  std::string_view file() const { return file_; }
  // The image as the Windows loader lays it out at ImageBase, before fixups.
  std::string_view mapped() const { return mapped_; }
  // Bounds-checked view of mapped memory.
  std::string_view view(uint32_t rva, uint32_t size) const;
  // The section containing an RVA, or nullptr (headers).
  const Section* section_at(uint32_t rva) const;

private:
  void parse_headers();
  void map_sections();
  void parse_imports();
  void parse_exports();
  void parse_relocations();
  void parse_resources();
  void parse_tls();

  std::string file_;
  std::string mapped_;
  Header hdr_;
  std::vector<Section> sections_;
  std::vector<Import> imports_;
  ExportDirectory exports_;
  std::vector<BaseReloc> relocs_;
  std::vector<Resource> resources_;
  std::optional<Tls> tls_;
  std::vector<std::string> warnings_;
};

// Returns the value to store in an IAT slot (typically a host thunk address).
using Resolver = std::function<uint32_t(const Import&)>;

struct Loaded {
  uint32_t base = 0, size = 0;
  uint32_t delta = 0;  // base - ImageBase (mod 2^32)
  uint32_t entry = 0;  // absolute entry point, 0 if the image has none
  size_t relocations_applied = 0;
  std::vector<Import> imports; // slot absolute, value = what was stored
  std::optional<Tls> tls;      // VAs adjusted to base

  uint32_t address_of(uint32_t rva) const { return base + rva; }
};

// Reserves SizeOfImage at ImageBase through the sink, applies base relocations
// when the sink chose another base, binds every import through resolve (an
// empty resolver leaves the IAT as the file has it), and writes the whole image
// with one sink.write. Throws LoaderError(placement) if the image must move but
// carries no relocations.
Loaded load(const Image& img, ImageSink& sink, const Resolver& resolve = {});

} // namespace adw::loader::pe
