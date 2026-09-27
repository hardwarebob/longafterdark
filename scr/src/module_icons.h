// Tiles for the settings dialog's module list and details header. Every
// module gets the same rounded tile (corners 6/28 of its side), filled with
// the first of:
//   * the module file's own ICON resource (read with adw::loader: PE32 and NE
//     alike): pixel art scaled sharply to the same share of the tile at every
//     DPI (nearest-neighbour up to a whole multiple, then smoothly down), on
//     the icon's own backdrop colour when it has one (a black-backed icon
//     fills its tile) or else the night blue;
//   * a thumbnail of the module running (thumbnails.h);
//   * until there is one, the night sky with a crescent moon and two stars
//     (the app's own mark), the same for every module.
#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace adw::scr {

struct Module;

class ModuleIcons {
 public:
  ModuleIcons();
  ~ModuleIcons();
  // Forget cached pictures (after an import).
  void clear();
  // Where thumbnails are kept ("" = none).
  void set_thumbs_dir(const std::wstring& dir);
  std::wstring thumb_path(const std::string& id) const;
  // A thumbnail was saved for `id`: pick it up on the next draw.
  void forget(const std::string& id);
  // Look again for thumbnails of every module still without a picture.
  void recheck_pictureless();
  // Draws module `m`'s tile into `r` (square). `dimmed` fades it (a module
  // that can't run yet or whose file is missing).
  void draw(HDC dc, const Module& m, const std::wstring& win_dir, const RECT& r, bool dimmed);
  // True when the file has an icon of its own (loads it if needed).
  bool has_own_icon(const Module& m, const std::wstring& win_dir);
  // An icon or a thumbnail (anything but the placeholder moon).
  bool has_picture(const Module& m, const std::wstring& win_dir);

 private:
  enum class Kind { none, icon, thumb };
  struct Source {
    Kind kind = Kind::none;
    int w = 0, h = 0;
    std::vector<uint32_t> argb;   // straight alpha, 0xAARRGGBB
  };
  const Source& source(const Module& m, const std::wstring& win_dir);
  std::map<std::string, Source> sources_;
  struct Scaled;
  std::map<std::string, std::unique_ptr<Scaled>> scaled_;
  std::wstring thumbs_dir_;
};

} // namespace adw::scr
