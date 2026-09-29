// Where a Classic module's support files come from (PACKAGES.md §7.1/§7.3,
// DESIGN.md §7): the package rule, the module's kind, the bridge and reader
// choices and the AD palettes.
//
//   module dir   = the folder of the module file
//   package root = its parent; engine dir = <package root>\ENGINE
//   packaged     = the package root's parent is named "packages" (any case)
//   windows dir  = <package root>\WINDOWS, when that folder exists (packaged
//                  only): what the original installer put in C:\WINDOWS
//                  (SWSE.INI), the read-only lower layer of the guest's
//                  C:\WINDOWS whatever the module's protocol — a folder
//                  rule, never a package-id rule. Never a module folder.
//
// Packaged modules resolve DLLs from the module dir, then the engine dir, and
// nothing outside their package; C:\WINDOWS\SYSTEM is the engine dir. Every
// other module is legacy and keeps the lane's original rule: the engine dir
// is <win>\FILES\ENGINE (or the module's own folder when only that holds
// OLDMOD16.DLL), and DLLs come from the module dir, <win>\FILES\CLASSIC, then
// the engine dir.
//
// Kind (ADNE16KIND=auto|ad3|imx; lane.hh "Module protocols"): auto reads the
// module's exports, resident or non-resident, in any case — MODULE is an
// After Dark 2.x/3.x module (ad3; it wins when both are there); SAVERINIT
// and SAVERDRAW an Intermission module (imx), unless it also exports
// SETCURRSAVER or its file name starts IMXX_, both of which Intermission's
// IMX reader refuses (IMIMXPLY 2:03c7, 2:044d); SAVERMAIN alone is an
// Intermission reader, not a module; anything else is not a module at all.
//
// Bridge (ADNE16BRIDGE=auto|oldmod16|native, ad3): auto runs the real
// OLDMOD16.DLL when the engine dir holds one, else the host-native AD3 bridge
// (bridge.hh) over the engine dir's AD_SND.DLL. Palettes: AFTERDAR.SCR's
// AD_PALETTE 101..104 for OLDMOD16; for the native bridge ADTASK.DLL's
// 5000/1..4 when the engine dir holds it (hpal[0..3] = 5000/3, 5000/1,
// 5000/4, 5000/2, so palette request 10+k selects 5000/(k+1)), else
// AFTERDAR.SCR's. After Dark 2.0 (Star Trek: The Screen Saver) has neither:
// AD.EXE 2.0b built its four palettes in code, and none is computed here —
// none of its modules makes a palette request (one would fail with 7).
//
// After Dark 2.0 (ad3): a module folder that holds AD_MOD.DLL, After Dark
// 2.0's module library (Star Trek: The Screen Saver's; no other release has
// one), is After Dark 2.0's — a rule by file, never a package-id rule. Its
// AD3 protocol seeds AD_PREFS.INI (win16/dos16.hh seed_after_dark2) and takes
// DRAWFRAME's result 5, which AD.EXE 2.0 took as its wake, as the module's
// wake (lane.hh "The AD3 protocol").
//
// Reader (ADNE16READER=auto|imq|native, imx): auto runs Intermission's own
// IMX reader, IMIMXPLY.IMQ, from the engine dir (the guest's
// C:\WINDOWS\SYSTEM), else from the module dir, when either holds it; else
// the host-native reader (imreader.hh).
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace adw::loader::ne {
class Image;
}

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
  std::string windows_dir;  // packaged only: <package root>\WINDOWS when it exists, else ""
  // Host directories the module table searches after the guest's directories.
  std::vector<std::string> search_dirs;
};

using FileExists = std::function<bool(const std::string&)>;

// The rule above. `win` is the assets' win dir (legacy modules only use it);
// `dir_exists` answers for the package root's WINDOWS folder (none: no
// windows dir).
Ne16Layout resolve_layout(const std::string& module_full_path, const std::string& win, const FileExists& exists,
                          const FileExists& dir_exists = {});

// ---- the module's kind ----------------------------------------------------------------------------------------

enum class ModuleKind { ad3, imx };
const char* kind_name(ModuleKind k);  // "ad3", "imx"
// What the exports say (the rule above). !ok: the lane refuses the module,
// and `why` says what it is instead.
struct KindProbe {
  bool ok = false;
  ModuleKind kind = ModuleKind::ad3;
  std::string why;
};
// `file_name` is the module file's name (the IMXX_ rule).
KindProbe detect_kind(const loader::ne::Image& img, const std::string& file_name);
// ADNE16KIND's value ("" = auto). False when it is not auto, ad3 or imx.
bool parse_kind_choice(const std::string& value, bool* is_auto, ModuleKind* forced);

// ---- the IMX reader -------------------------------------------------------------------------------------------

enum class ReaderKind { imq, native };
const char* reader_name(ReaderKind k);  // "imq", "native"
// ADNE16READER's value ("" = auto). False when it is not auto, imq or native.
bool parse_reader_choice(const std::string& value, bool* is_auto, ReaderKind* forced);
// IMIMXPLY.IMQ in the engine dir, else in the module dir (host = "" when neither has it).
struct ReaderFile {
  std::string host;
  bool in_engine_dir = false;  // else the module dir's
};
constexpr const char* kImxReader = "IMIMXPLY.IMQ";
ReaderFile find_reader(const Ne16Layout& layout, const FileExists& exists);

// ADNE16BRIDGE's value ("" = auto). False when the value is not one of the three.
bool parse_bridge_choice(const std::string& value, bool* is_auto, BridgeKind* forced);
// auto: OLDMOD16 when the engine dir has OLDMOD16.DLL, else native.
BridgeKind choose_bridge(const Ne16Layout& layout, const FileExists& exists);

// After Dark 2.0's module library; a module folder holding it is After Dark 2.0's (the rule above).
constexpr const char* kAfterDark2Library = "AD_MOD.DLL";
bool after_dark2(const Ne16Layout& layout, const FileExists& exists);

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
