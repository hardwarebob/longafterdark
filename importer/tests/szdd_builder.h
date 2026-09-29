// Synthetic SZDD files (Microsoft COMPRESS 'A' mode) for the importer tests
// (szdd.h): a port of our research encoder (research/win/pkg/swse/survey/
// importer/szdd_vectors.py), a greedy LZSS over the 4096-byte window that
// starts filled with spaces and is written from 0xFF0, taking matches from
// anywhere in it — the initial spaces, and the bytes the match itself is
// writing. Its research outputs expand identically with Windows' own
// EXPAND.EXE. Made-up data only.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace test {

inline std::vector<uint8_t> szdd_encode(const std::vector<uint8_t>& data, uint8_t missing = 0,
                                        bool literal_only = false) {
  std::array<uint8_t, 4096> win;
  win.fill(0x20);
  size_t pos = 0xFF0;
  std::vector<uint8_t> out = {'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33, 'A', missing};
  const uint32_t size = uint32_t(data.size());
  for (int k = 0; k < 4; k++) out.push_back(uint8_t(size >> (8 * k)));
  const size_t n = data.size();
  size_t i = 0;
  while (i < n) {
    const size_t flag_at = out.size();
    out.push_back(0);
    uint8_t flags = 0;
    for (int bit = 0; bit < 8 && i < n; bit++) {
      size_t best_len = 0, best_pos = 0;
      if (!literal_only) {
        for (size_t start = 0; start < 4096; start++) {
          // A match starting where the next byte will land reads that byte.
          if (win[start] != data[i] && ((start - pos) & 0xFFF) != 0) continue;
          size_t k = 0;
          while (k < 18 && i + k < n) {
            // What position start+k holds once the first k bytes of this
            // match have landed.
            const size_t q = (start + k) & 0xFFF;
            const size_t ahead = (q - pos) & 0xFFF;
            const uint8_t c = ahead < k ? data[i + ahead] : win[q];
            if (c != data[i + k]) break;
            k++;
          }
          if (k > best_len) {
            best_len = k;
            best_pos = start;
            if (k == 18) break;
          }
        }
      }
      if (best_len >= 3) {
        out.push_back(uint8_t(best_pos & 0xFF));
        out.push_back(uint8_t(((best_pos >> 4) & 0xF0) | (best_len - 3)));
        for (size_t k = 0; k < best_len; k++) {
          win[pos] = data[i + k];
          pos = (pos + 1) & 0xFFF;
        }
        i += best_len;
      } else {
        flags = uint8_t(flags | (1u << bit));
        out.push_back(data[i]);
        win[pos] = data[i];
        pos = (pos + 1) & 0xFFF;
        i++;
      }
    }
    out[flag_at] = flags;
  }
  return out;
}

inline std::vector<uint8_t> szdd_encode(std::string_view text, uint8_t missing = 0) {
  return szdd_encode(std::vector<uint8_t>(text.begin(), text.end()), missing);
}

}  // namespace test
