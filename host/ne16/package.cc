#include "ne16/package.hh"

#include <cctype>
#include <cstring>

#include "adw/core/log.h"
#include "loader/ne.hh"
#include "loader/pe.hh"

namespace adw::ne16 {

namespace {

std::string dir_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

bool ieq(const std::string& a, const char* b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++) {
    if (toupper(uint8_t(a[i])) != toupper(uint8_t(b[i]))) return false;
  }
  return true;
}

uint16_t u16(std::string_view s, size_t off) {
  if (off + 2 > s.size()) return 0;
  return uint16_t(uint8_t(s[off]) | (uint8_t(s[off + 1]) << 8));
}

// A LOGPALETTE resource: WORD version, WORD count, count PALETTEENTRYs.
bool parse_logpalette(std::string_view d, std::vector<PALETTEENTRY>* out) {
  uint16_t n = u16(d, 2);
  if (!n || n > 256 || d.size() < 4 + size_t(n) * 4) return false;
  out->resize(n);
  memcpy(out->data(), d.data() + 4, size_t(n) * 4);
  return true;
}

}  // namespace

const char* bridge_name(BridgeKind k) { return k == BridgeKind::oldmod16 ? "oldmod16" : "native"; }

Ne16Layout resolve_layout(const std::string& path, const std::string& win, const FileExists& exists) {
  Ne16Layout l;
  l.module_path = path;
  l.module_dir = dir_of(path);
  if (l.module_dir.empty()) l.module_dir = ".";
  std::string root = dir_of(l.module_dir);
  std::string above = root.empty() ? std::string() : dir_of(root);
  if (!root.empty() && !above.empty() && ieq(file_of(above), "packages")) {
    l.packaged = true;
    l.package_root = root;
    l.package_id = file_of(root);
    l.engine_dir = root + "\\ENGINE";
    l.search_dirs = {l.module_dir, l.engine_dir};
    return l;
  }
  // Legacy: exactly the lane's original rule.
  std::string engine = win + "\\FILES\\ENGINE", classic = win + "\\FILES\\CLASSIC";
  if (!exists(engine + "\\OLDMOD16.DLL") && exists(l.module_dir + "\\OLDMOD16.DLL")) engine = l.module_dir;
  l.engine_dir = engine;
  l.search_dirs = {l.module_dir, classic, engine};
  return l;
}

bool parse_bridge_choice(const std::string& v, bool* is_auto, BridgeKind* forced) {
  *is_auto = true;
  if (v.empty() || ieq(v, "auto")) return true;
  if (ieq(v, "oldmod16")) {
    *is_auto = false;
    *forced = BridgeKind::oldmod16;
    return true;
  }
  if (ieq(v, "native")) {
    *is_auto = false;
    *forced = BridgeKind::native;
    return true;
  }
  return false;
}

BridgeKind choose_bridge(const Ne16Layout& l, const FileExists& exists) {
  return exists(l.engine_dir + "\\OLDMOD16.DLL") ? BridgeKind::oldmod16 : BridgeKind::native;
}

AdPalettes palettes_from_scr(const std::string& scr) {
  AdPalettes out;
  try {
    loader::pe::Image img = loader::pe::Image::from_file(scr);
    for (uint16_t id = 101; id <= 104; id++) {
      const loader::pe::Resource* r = img.find_resource(loader::ResId::of("AD_PALETTE"), loader::ResId::of(id));
      std::vector<PALETTEENTRY> e;
      if (!r || !parse_logpalette(img.resource_data(*r), &e)) {
        out.error = scr + ": no AD_PALETTE " + std::to_string(id);
        out.pal.clear();
        return out;
      }
      out.pal.push_back(std::move(e));
    }
  } catch (const std::exception& e) {
    out.error = scr + ": " + e.what();
    out.pal.clear();
    return out;
  }
  out.source = scr + " AD_PALETTE 101..104";
  return out;
}

AdPalettes palettes_from_adtask(const std::string& adtask) {
  AdPalettes out;
  try {
    loader::ne::Image img = loader::ne::Image::from_file(adtask);
    // SETADPALETTE16 index i takes 5000/kOrder[i]: OLDMOD16's palette request
    // 10+k selects hpal[1, 3, 0, 2][k], and the AD 3.2 survey matched
    // AFTERDAR.SCR's AD_PALETTE 102/104/101/103 to 5000/1..4 byte for byte.
    static constexpr uint16_t kOrder[4] = {3, 1, 4, 2};
    for (uint16_t id : kOrder) {
      const loader::ne::Resource* r = img.find_resource(loader::ResId::of(5000), loader::ResId::of(id));
      std::vector<PALETTEENTRY> e;
      if (!r || !parse_logpalette(img.resource_data(*r), &e)) {
        out.error = adtask + ": no palette resource 5000/" + std::to_string(id);
        out.pal.clear();
        return out;
      }
      out.pal.push_back(std::move(e));
    }
  } catch (const std::exception& e) {
    out.error = adtask + ": " + e.what();
    out.pal.clear();
    return out;
  }
  out.source = adtask + " 5000/1..4";
  return out;
}

AdPalettes load_palettes(const Ne16Layout& l, BridgeKind bridge, const FileExists& exists) {
  std::string scr = l.engine_dir + "\\AFTERDAR.SCR";
  if (bridge == BridgeKind::native) {
    std::string adtask = l.engine_dir + "\\ADTASK.DLL";
    if (exists(adtask)) {
      AdPalettes p = palettes_from_adtask(adtask);
      if (!p.pal.empty() || !exists(scr)) return p;
      log("ne16: %s; using %s", p.error.c_str(), scr.c_str());
    }
  }
  if (!exists(scr)) {
    AdPalettes p;
    p.error = bridge == BridgeKind::native ? l.engine_dir + " holds neither ADTASK.DLL nor AFTERDAR.SCR"
                                           : scr + " is missing";
    return p;
  }
  return palettes_from_scr(scr);
}

}  // namespace adw::ne16
