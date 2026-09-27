// The US keyboard the Classic lane's guest sees (input16.hh): scan codes and
// TranslateMessage's characters, fixed tables so a run never depends on the
// host's layout.
#include <windows.h>

#include "win16/input16.hh"

namespace adw::win16 {

uint8_t vk_scan_code(uint8_t vk) {
  // Letters: the QWERTY rows.
  static const uint8_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                       0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
  if (vk >= 'A' && vk <= 'Z') return kLetters[vk - 'A'];
  if (vk >= '1' && vk <= '9') return uint8_t(0x02 + (vk - '1'));
  if (vk == '0') return 0x0B;
  if (vk >= VK_F1 && vk <= VK_F10) return uint8_t(0x3B + (vk - VK_F1));
  switch (vk) {
    case VK_ESCAPE: return 0x01;
    case VK_BACK: return 0x0E;
    case VK_TAB: return 0x0F;
    case VK_RETURN: return 0x1C;
    case VK_CONTROL: return 0x1D;
    case VK_SHIFT: return 0x2A;
    case VK_MENU: return 0x38;
    case VK_SPACE: return 0x39;
    case VK_CAPITAL: return 0x3A;
    case VK_NUMLOCK: return 0x45;
    case VK_SCROLL: return 0x46;
    case VK_F11: return 0x57;
    case VK_F12: return 0x58;
    case VK_HOME: case VK_NUMPAD7: return 0x47;
    case VK_UP: case VK_NUMPAD8: return 0x48;
    case VK_PRIOR: case VK_NUMPAD9: return 0x49;
    case VK_SUBTRACT: return 0x4A;
    case VK_LEFT: case VK_NUMPAD4: return 0x4B;
    case VK_CLEAR: case VK_NUMPAD5: return 0x4C;
    case VK_RIGHT: case VK_NUMPAD6: return 0x4D;
    case VK_ADD: return 0x4E;
    case VK_END: case VK_NUMPAD1: return 0x4F;
    case VK_DOWN: case VK_NUMPAD2: return 0x50;
    case VK_NEXT: case VK_NUMPAD3: return 0x51;
    case VK_INSERT: case VK_NUMPAD0: return 0x52;
    case VK_DELETE: case VK_DECIMAL: return 0x53;
    case VK_MULTIPLY: return 0x37;
    case VK_DIVIDE: return 0x35;
    case VK_OEM_1: return 0x27;       // ;:
    case VK_OEM_PLUS: return 0x0D;    // =+
    case VK_OEM_COMMA: return 0x33;   // ,<
    case VK_OEM_MINUS: return 0x0C;   // -_
    case VK_OEM_PERIOD: return 0x34;  // .>
    case VK_OEM_2: return 0x35;       // /?
    case VK_OEM_3: return 0x29;       // `~
    case VK_OEM_4: return 0x1A;       // [{
    case VK_OEM_5: return 0x2B;       // \|
    case VK_OEM_6: return 0x1B;       // ]}
    case VK_OEM_7: return 0x28;       // '"
    default: return 0;
  }
}

bool vk_extended(uint8_t vk) {
  switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_RCONTROL: case VK_RMENU:
      return true;
    default:
      return false;
  }
}

int vk_to_char(uint8_t vk, bool shift, bool caps) {
  if (vk >= 'A' && vk <= 'Z') return (shift != caps) ? vk : vk + ('a' - 'A');
  if (vk >= '0' && vk <= '9') {
    static const char kShifted[] = ")!@#$%^&*(";
    return shift ? kShifted[vk - '0'] : vk;
  }
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);
  struct Pair { uint8_t vk; char plain, shifted; };
  static const Pair kKeys[] = {
      {VK_SPACE, ' ', ' '},     {VK_RETURN, '\r', '\r'},  {VK_BACK, '\b', '\b'},     {VK_TAB, '\t', '\t'},
      {VK_ESCAPE, 0x1B, 0x1B},  {VK_OEM_1, ';', ':'},     {VK_OEM_PLUS, '=', '+'},   {VK_OEM_COMMA, ',', '<'},
      {VK_OEM_MINUS, '-', '_'}, {VK_OEM_PERIOD, '.', '>'}, {VK_OEM_2, '/', '?'},     {VK_OEM_3, '`', '~'},
      {VK_OEM_4, '[', '{'},     {VK_OEM_5, '\\', '|'},    {VK_OEM_6, ']', '}'},      {VK_OEM_7, '\'', '"'},
      {VK_MULTIPLY, '*', '*'},  {VK_ADD, '+', '+'},       {VK_SUBTRACT, '-', '-'},   {VK_DECIMAL, '.', '.'},
      {VK_DIVIDE, '/', '/'},
  };
  for (const Pair& p : kKeys) {
    if (p.vk == vk) return uint8_t(shift ? p.shifted : p.plain);
  }
  return -1;
}

uint32_t key_lparam(uint8_t vk, bool down, bool was_down) {
  uint32_t lp = 1 | (uint32_t(vk_scan_code(vk)) << 16);
  if (vk_extended(vk)) lp |= 1u << 24;
  if (was_down || !down) lp |= 1u << 30;  // a release always follows a down
  if (!down) lp |= 1u << 31;
  return lp;
}

}  // namespace adw::win16
