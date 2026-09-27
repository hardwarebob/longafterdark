#include "loader/image.hh"

#include <fstream>
#include <iterator>
#include <sstream>

#include "loader/bytes.hh"

namespace adw::loader {

using detail::Bytes;
using detail::fail;
using Kind = LoaderError::Kind;

const char* error_kind_name(LoaderError::Kind kind) {
  switch (kind) {
    case Kind::truncated: return "truncated";
    case Kind::bad_format: return "bad_format";
    case Kind::unsupported: return "unsupported";
    case Kind::malformed: return "malformed";
    case Kind::placement: return "placement";
  }
  return "?";
}

const char* format_name(Format f) {
  switch (f) {
    case Format::unknown: return "unknown";
    case Format::mz: return "MZ";
    case Format::ne: return "NE";
    case Format::le: return "LE";
    case Format::pe32: return "PE32";
    case Format::pe32plus: return "PE32+";
  }
  return "?";
}

Format detect_format(std::string_view data) {
  Bytes b(data, "file");
  if (data.size() < 2 || data.substr(0, 2) != "MZ") return Format::unknown;
  if (!b.contains(0x3C, 4)) return Format::mz;
  size_t lfanew = b.u32(0x3C);
  if (!b.contains(lfanew, 4)) return Format::mz;
  std::string_view sig = data.substr(lfanew, 4);
  if (sig == std::string_view("PE\0\0", 4)) {
    // The optional header magic decides PE32 vs PE32+; an unreadable one is
    // still "PE32" so the parser, not detection, reports the damage.
    if (b.contains(lfanew + 24, 2) && b.u16(lfanew + 24) == 0x20B) return Format::pe32plus;
    return Format::pe32;
  }
  if (sig.substr(0, 2) == "NE") return Format::ne;
  if (sig.substr(0, 2) == "LE" || sig.substr(0, 2) == "LX") return Format::le;
  return Format::mz;
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return std::move(ss).str();
}

// ---- resource ids -----------------------------------------------------------

bool ResId::matches(const ResId& other) const {
  auto as_num = [](const ResId& r, uint16_t& out) {
    if (!r.is_string) {
      out = r.num;
      return true;
    }
    // FindResource accepts "#123" for integer id 123.
    if (r.str.size() < 2 || r.str[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < r.str.size(); i++) {
      if (r.str[i] < '0' || r.str[i] > '9') return false;
      v = v * 10 + (r.str[i] - '0');
      if (v > 0xFFFF) return false;
    }
    out = static_cast<uint16_t>(v);
    return true;
  };
  if (is_string && other.is_string) return detail::iequals(str, other.str);
  uint16_t a, b;
  return as_num(*this, a) && as_num(other, b) && a == b;
}

std::string ResId::to_string() const {
  if (!is_string) return std::to_string(num);
  return "\"" + str + "\"";
}

const char* rt::name(uint16_t type) {
  switch (type) {
    case cursor: return "CURSOR";
    case bitmap: return "BITMAP";
    case icon: return "ICON";
    case menu: return "MENU";
    case dialog: return "DIALOG";
    case string: return "STRING";
    case fontdir: return "FONTDIR";
    case font: return "FONT";
    case accelerator: return "ACCELERATOR";
    case rcdata: return "RCDATA";
    case messagetable: return "MESSAGETABLE";
    case group_cursor: return "GROUP_CURSOR";
    case group_icon: return "GROUP_ICON";
    case nametable: return "NAMETABLE";
    case version: return "VERSION";
    case dlginclude: return "DLGINCLUDE";
    case plugplay: return "PLUGPLAY";
    case vxd: return "VXD";
    case anicursor: return "ANICURSOR";
    case aniicon: return "ANIICON";
    case html: return "HTML";
    case manifest: return "MANIFEST";
  }
  return nullptr;
}

// ---- text -------------------------------------------------------------------

static void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string utf16le_to_utf8(std::string_view bytes) {
  std::string out;
  size_t n = bytes.size() / 2;
  auto unit = [&](size_t i) {
    return static_cast<uint32_t>(static_cast<uint8_t>(bytes[2 * i])) |
        (static_cast<uint32_t>(static_cast<uint8_t>(bytes[2 * i + 1])) << 8);
  };
  for (size_t i = 0; i < n; i++) {
    uint32_t c = unit(i);
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && unit(i + 1) >= 0xDC00 && unit(i + 1) < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (unit(i + 1) - 0xDC00);
      i++;
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD; // unpaired surrogate
    }
    append_utf8(out, c);
  }
  return out;
}

std::string latin1_to_utf8(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (char ch : bytes) append_utf8(out, static_cast<uint8_t>(ch));
  return out;
}

// ---- VERSIONINFO ------------------------------------------------------------

namespace {

// One node of the version tree. Both layouts are
//   WORD length; WORD value_length; [WORD type — 32-bit only]; key\0; pad4;
//   value; pad4; children...
// with alignment relative to the start of the resource (the loaders hand us
// data that begins on a 4-byte boundary, so offsets relative to it suffice).
struct VNode {
  std::string key;
  size_t value_off = 0, value_len = 0; // value_len in bytes, clamped to the node
  bool text = false;
  size_t children = 0, end = 0;
};

VNode parse_vnode(const Bytes& b, size_t off, size_t limit, bool wide) {
  VNode n;
  uint16_t len = b.u16(off);
  uint16_t vlen = b.u16(off + 2);
  size_t hdr = wide ? 6 : 4;
  if (len < hdr) {
    fail(Kind::malformed, std::format("VERSIONINFO node at 0x{:X} has length {}", off, len));
  }
  // Some resource compilers overstate the length of the last node; the parent
  // bounds it, the way VerQueryValue effectively does.
  n.end = std::min(off + len, limit);
  n.text = wide ? (b.u16(off + 4) == 1) : true;
  size_t p = off + hdr;
  if (wide) {
    size_t k = p;
    while (true) {
      if (k + 2 > n.end) fail(Kind::truncated, std::format("VERSIONINFO key at 0x{:X} is not terminated", p));
      if (b.u16(k) == 0) break;
      k += 2;
    }
    n.key = utf16le_to_utf8(b.slice(p, k - p));
    p = k + 2;
  } else {
    size_t k = p;
    while (true) {
      if (k + 1 > n.end) fail(Kind::truncated, std::format("VERSIONINFO key at 0x{:X} is not terminated", p));
      if (b.u8(k) == 0) break;
      k++;
    }
    n.key = latin1_to_utf8(b.slice(p, k - p));
    p = k + 1;
  }
  p = detail::align_up(p, 4);
  // 32-bit text values count WCHARs, binary ones bytes (and some compilers
  // store bytes for text too); clamping to the node covers both.
  size_t vbytes = (wide && n.text) ? size_t(vlen) * 2 : vlen;
  n.value_off = std::min(p, n.end);
  n.value_len = std::min(vbytes, n.end - n.value_off);
  n.children = std::min<size_t>(detail::align_up(n.value_off + vbytes, 4), n.end);
  return n;
}

template <typename Fn>
void for_each_child(const Bytes& b, const VNode& parent, bool wide, Fn&& fn) {
  size_t hdr = wide ? 6 : 4;
  size_t c = parent.children;
  while (c + hdr <= parent.end) {
    if (b.u16(c) == 0) break; // padding at the end of a block
    VNode child = parse_vnode(b, c, parent.end, wide);
    fn(child);
    c = detail::align_up(c + b.u16(c), 4);
  }
}

std::string vtext(const Bytes& b, const VNode& n, bool wide) {
  // A String node has no children, so its text runs from the value to its
  // terminator within the node. Reading that far rather than value_len
  // survives compilers that mark text wType 0 (value_len would then count
  // WCHARs as bytes and cut the string in half) or count bytes for text.
  std::string_view v = b.slice(n.value_off, n.end - n.value_off);
  if (wide) {
    size_t i = 0;
    while (i + 1 < v.size() && !(v[i] == 0 && v[i + 1] == 0)) i += 2;
    return utf16le_to_utf8(v.substr(0, i));
  }
  size_t i = v.find('\0');
  return latin1_to_utf8(v.substr(0, i == std::string_view::npos ? v.size() : i));
}

} // namespace

VersionInfo parse_version_info(std::string_view data, bool wide) {
  Bytes b(data, "VERSIONINFO");
  VersionInfo vi;
  VNode root = parse_vnode(b, 0, data.size(), wide);
  if (root.key != "VS_VERSION_INFO") {
    fail(Kind::bad_format, "VERSIONINFO root key is \"" + root.key + "\"");
  }
  if (root.value_len >= 52 && b.u32(root.value_off) == 0xFEEF04BD) {
    size_t f = root.value_off;
    vi.has_fixed = true;
    vi.signature = b.u32(f);
    vi.struct_version = b.u32(f + 4);
    vi.file_version_ms = b.u32(f + 8);
    vi.file_version_ls = b.u32(f + 12);
    vi.product_version_ms = b.u32(f + 16);
    vi.product_version_ls = b.u32(f + 20);
    vi.file_flags_mask = b.u32(f + 24);
    vi.file_flags = b.u32(f + 28);
    vi.file_os = b.u32(f + 32);
    vi.file_type = b.u32(f + 36);
    vi.file_subtype = b.u32(f + 40);
    vi.file_date_ms = b.u32(f + 44);
    vi.file_date_ls = b.u32(f + 48);
  }
  for_each_child(b, root, wide, [&](const VNode& block) {
    if (block.key == "StringFileInfo") {
      for_each_child(b, block, wide, [&](const VNode& table) {
        VersionInfo::StringTable st;
        st.key = table.key;
        for_each_child(b, table, wide, [&](const VNode& s) {
          st.strings.emplace_back(s.key, vtext(b, s, wide));
        });
        vi.string_tables.push_back(std::move(st));
      });
    } else if (block.key == "VarFileInfo") {
      for_each_child(b, block, wide, [&](const VNode& var) {
        if (var.key != "Translation") return;
        for (size_t i = 0; i + 4 <= var.value_len; i += 4) {
          vi.translations.push_back(b.u32(var.value_off + i));
        }
      });
    }
  });
  return vi;
}

static std::string dotted(uint32_t ms, uint32_t ls) {
  return std::format("{}.{}.{}.{}", ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF);
}
std::string VersionInfo::file_version() const {
  return dotted(file_version_ms, file_version_ls);
}
std::string VersionInfo::product_version() const {
  return dotted(product_version_ms, product_version_ls);
}
std::string VersionInfo::string(std::string_view key) const {
  for (const auto& t : string_tables) {
    for (const auto& [k, v] : t.strings) {
      if (k == key) return v;
    }
  }
  return {};
}

} // namespace adw::loader
