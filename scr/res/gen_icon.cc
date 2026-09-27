// Build-time generator for LongAfterDark.scr's icon: a crescent moon and a few
// stars on a night-blue tile. Drawn procedurally (supersampled for
// anti-aliasing) so the repository holds no binary art — and none of Berkeley
// Systems' — only this recipe.
//
//   gen_icon <out.ico>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Rgba {
  double r = 0, g = 0, b = 0, a = 0;
};

Rgba over(Rgba dst, Rgba src) {
  double a = src.a + dst.a * (1 - src.a);
  if (a <= 0) return {};
  auto mix = [&](double s, double d) { return (s * src.a + d * dst.a * (1 - src.a)) / a; };
  return {mix(src.r, dst.r), mix(src.g, dst.g), mix(src.b, dst.b), a};
}

// Colour of one sample point in unit coordinates (0..1, y down).
Rgba sample(double x, double y) {
  Rgba c;
  // Rounded-square tile with a vertical night gradient.
  const double inset = 0.04, radius = 0.20;
  double qx = std::max(std::abs(x - 0.5) - (0.5 - inset - radius), 0.0);
  double qy = std::max(std::abs(y - 0.5) - (0.5 - inset - radius), 0.0);
  if (std::sqrt(qx * qx + qy * qy) <= radius) {
    double t = y;
    c = {0.03 + 0.05 * t, 0.05 + 0.10 * t, 0.16 + 0.26 * t, 1.0};
  } else {
    return c;
  }
  // Crescent: a disc minus an offset disc.
  double d1 = std::hypot(x - 0.54, y - 0.46), d2 = std::hypot(x - 0.67, y - 0.37);
  if (d1 <= 0.29 && d2 > 0.25) c = over(c, {0.98, 0.93, 0.70, 1.0});
  // Four-point stars: |dx|^p + |dy|^p <= r^p with p < 1 gives the pinched sparkle.
  struct Star { double x, y, r; };
  const Star stars[] = {{0.24, 0.26, 0.085}, {0.20, 0.62, 0.055}, {0.76, 0.76, 0.07}, {0.40, 0.82, 0.04}};
  for (const auto& s : stars) {
    double dx = std::abs(x - s.x) / s.r, dy = std::abs(y - s.y) / s.r;
    if (std::pow(dx, 0.5) + std::pow(dy, 0.5) <= 1.0) c = over(c, {1.0, 1.0, 0.95, 1.0});
  }
  return c;
}

std::vector<uint8_t> render_bgra(int size) {
  const int ss = 4;   // 4x4 supersampling
  std::vector<uint8_t> px((size_t)size * size * 4);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      double r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < ss; ++sy) {
        for (int sx = 0; sx < ss; ++sx) {
          Rgba c = sample((x + (sx + 0.5) / ss) / size, (y + (sy + 0.5) / ss) / size);
          r += c.r * c.a;
          g += c.g * c.a;
          b += c.b * c.a;
          a += c.a;
        }
      }
      double n = ss * ss;
      uint8_t* p = &px[((size_t)y * size + x) * 4];
      double alpha = a / n;
      auto ch = [&](double v) { return (uint8_t)std::lround(std::clamp(alpha > 0 ? v / a : 0.0, 0.0, 1.0) * 255); };
      p[0] = ch(b);
      p[1] = ch(g);
      p[2] = ch(r);
      p[3] = (uint8_t)std::lround(alpha * 255);
    }
  }
  return px;
}

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }

// One ICO image: BITMAPINFOHEADER (double height: XOR + AND), bottom-up
// 32-bit BGRA rows, then an all-zero 1-bit AND mask (alpha does the work).
std::vector<uint8_t> dib_image(int size) {
  std::vector<uint8_t> bgra = render_bgra(size), out;
  put32(out, 40);
  put32(out, (uint32_t)size);
  put32(out, (uint32_t)size * 2);
  put16(out, 1);
  put16(out, 32);
  put32(out, 0);
  int mask_stride = ((size + 31) / 32) * 4;
  put32(out, (uint32_t)(size * size * 4 + mask_stride * size));
  for (int i = 0; i < 4; ++i) put32(out, 0);
  for (int y = size - 1; y >= 0; --y) {
    out.insert(out.end(), bgra.begin() + (size_t)y * size * 4, bgra.begin() + (size_t)(y + 1) * size * 4);
  }
  out.insert(out.end(), (size_t)mask_stride * size, 0);
  return out;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: gen_icon <out.ico>\n");
    return 2;
  }
  const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128};
  const int n = sizeof(sizes) / sizeof(sizes[0]);
  std::vector<std::vector<uint8_t>> images;
  for (int s : sizes) images.push_back(dib_image(s));
  std::vector<uint8_t> ico;
  put16(ico, 0);
  put16(ico, 1);
  put16(ico, (uint16_t)n);
  uint32_t offset = 6 + 16 * n;
  for (int i = 0; i < n; ++i) {
    ico.push_back((uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]));
    ico.push_back((uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]));
    ico.push_back(0);
    ico.push_back(0);
    put16(ico, 1);
    put16(ico, 32);
    put32(ico, (uint32_t)images[i].size());
    put32(ico, offset);
    offset += (uint32_t)images[i].size();
  }
  for (auto& img : images) ico.insert(ico.end(), img.begin(), img.end());
  FILE* f = fopen(argv[1], "wb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  bool ok = fwrite(ico.data(), 1, ico.size(), f) == ico.size();
  ok = fclose(f) == 0 && ok;
  return ok ? 0 : 1;
}
