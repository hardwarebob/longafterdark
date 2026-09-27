// Where a Classic module's support files come from (PACKAGES.md §7.1/§7.3,
// DESIGN.md §7): the package rule, the bridge choice and the AD palettes.
//
//   module dir   = the folder of the module file
//   package root = its parent; engine dir = <package root>\ENGINE
//   packaged     = the package root's parent is named "packages" (any case)
//
// Packaged modules resolve DLLs from the module dir, then the engine dir, and
// nothing outside their package; C:\WINDOWS\SYSTEM is the engine dir. Every
// other module is legacy and keeps the lane's original rule: the engine dir
// is <win>\FILES\ENGINE (or the module's own folder when only that holds
// OLDMOD16.DLL), and DLLs come from the module dir, <win>\FILES\CLASSIC, then
// the engine dir.
//
// Bridge (ADNE16BRIDGE=auto|oldmod16|native): auto runs the real OLDMOD16.DLL
// when the engine dir holds one, else the host-native AD3 bridge (bridge.hh)
// over the engine dir's AD_SND.DLL. Palettes: AFTERDAR.SCR's AD_PALETTE
// 101..104 for OLDMOD16; for the native bridge ADTASK.DLL's 5000/1..4 when
// the engine dir holds it (hpal[0..3] = 5000/3, 5000/1, 5000/4, 5000/2, so
// palette request 10+k selects 5000/(k+1)), else AFTERDAR.SCR's.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace adw::ne16 {

enum class BridgeKind { oldmod16, native };
const char* bridge_name(BridgeKind k);

struct Ne16Layout {
  std::string module_path;  // full host path
  std::string module_dir;
  std::string package_root;  // packaged only
  std::string package_id;    // packaged only: the root's folder name (logs)
  bool packaged = false;
  std::string engine_dir;
  // Host directories the module table searches after the guest's directories.
  std::vector<std::string> search_dirs;
};

using FileExists = std::function<bool(const std::string&)>;

// The rule above. `win` is the assets' win dir (legacy modules only use it).
Ne16Layout resolve_layout(const std::string& module_full_path, const std::string& win, const FileExists& exists);

// ADNE16BRIDGE's value ("" = auto). False when the value is not one of the three.
bool parse_bridge_choice(const std::string& value, bool* is_auto, BridgeKind* forced);
// auto: OLDMOD16 when the engine dir has OLDMOD16.DLL, else native.
BridgeKind choose_bridge(const Ne16Layout& layout, const FileExists& exists);

// The four palettes handed to SETADPALETTE16(hpal[i], i), and where they came from.
struct AdPalettes {
  std::vector<std::vector<PALETTEENTRY>> pal;  // 4 entries, or empty when unavailable
  std::string source;                          // "…\AFTERDAR.SCR AD_PALETTE 101..104", "…\ADTASK.DLL 5000/1..4"
  std::string error;                           // why they are unavailable
};
// AFTERDAR.SCR (PE) AD_PALETTE 101..104 in that order.
AdPalettes palettes_from_scr(const std::string& scr);
// ADTASK.DLL (NE) resources 5000/1..4, ordered for SETADPALETTE16: 5000/3, 5000/1, 5000/4, 5000/2.
AdPalettes palettes_from_adtask(const std::string& adtask);
// The bridge's palette source (see the header comment).
AdPalettes load_palettes(const Ne16Layout& layout, BridgeKind bridge, const FileExists& exists);

}  // namespace adw::ne16
