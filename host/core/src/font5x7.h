// A 5x7 bitmap font for the test pattern's readouts (frame counter, control
// values, input state). Drawn for this project; covers space, digits, A-Z and
// a little punctuation. Each glyph is 7 rows, bit 4 = leftmost column.
#pragma once

#include <cstdint>

namespace adw::font5x7 {

inline constexpr int kW = 5, kH = 7;

// Returns the 7 row bitmaps for `c` (lower case folds to upper; anything
// unknown renders as a hollow box so gaps in coverage are obvious).
inline const uint8_t* glyph(char c) {
  static const uint8_t space[7] = {0, 0, 0, 0, 0, 0, 0};
  static const uint8_t unknown[7] = {0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111};
  static const uint8_t digits[10][7] = {
      {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},  // 0
      {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},  // 1
      {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},  // 2
      {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110},  // 3
      {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},  // 4
      {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},  // 5
      {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},  // 6
      {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},  // 7
      {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},  // 8
      {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},  // 9
  };
  static const uint8_t letters[26][7] = {
      {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // A
      {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110},  // B
      {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110},  // C
      {0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100},  // D
      {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
      {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000},  // F
      {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111},  // G
      {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // H
      {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},  // I
      {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100},  // J
      {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001},  // K
      {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111},  // L
      {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001},  // M
      {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001},  // N
      {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},  // O
      {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000},  // P
      {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101},  // Q
      {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001},  // R
      {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110},  // S
      {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},  // T
      {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110},  // U
      {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100},  // V
      {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010},  // W
      {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001},  // X
      {0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100},  // Y
      {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111},  // Z
  };
  static const uint8_t colon[7] = {0, 0b01100, 0b01100, 0, 0b01100, 0b01100, 0};
  static const uint8_t minus[7] = {0, 0, 0, 0b11111, 0, 0, 0};
  static const uint8_t equals[7] = {0, 0, 0b11111, 0, 0b11111, 0, 0};
  static const uint8_t dot[7] = {0, 0, 0, 0, 0, 0b01100, 0b01100};
  static const uint8_t comma[7] = {0, 0, 0, 0, 0b01100, 0b00100, 0b01000};
  static const uint8_t slash[7] = {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000};
  static const uint8_t hash[7] = {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010};

  if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  if (c == ' ') return space;
  if (c >= '0' && c <= '9') return digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
  switch (c) {
    case ':': return colon;
    case '-': return minus;
    case '=': return equals;
    case '.': return dot;
    case ',': return comma;
    case '/': return slash;
    case '#': return hash;
  }
  return unknown;
}

}  // namespace adw::font5x7
