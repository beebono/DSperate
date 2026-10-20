// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/video/nearest_scale.h"
#include "frontend/video/layout.h"
#include "check.h"

#include <vector>

using namespace ds;

int main() {
  std::vector<u32> src(SCREEN_W * SCREEN_H);
  u32 rng = 0xA1B2C3D4;
  for (u32& p : src) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; p = rng; }
  constexpr u32 guard = 0xDEADBEEF;
  const u32 sizes[][2] = {{256,192}, {512,384}, {768,576}, {1024,768}, {1280,960},
                           {640,480}, {853,640}, {213,160}, {257,193}, {600,1000}, {1,1}};
  for (const auto& size : sizes) {
    const u32 width = size[0], height = size[1];
    // Padded rows and guard rows expose any writes outside the active image.
    const u32 pitch = width + 7;
    std::vector<u32> dst(static_cast<size_t>(pitch) * (height + 2), guard);
    frontend::scale_nearest(src.data(), dst.data() + pitch, pitch, width, height);
    for (u32 y = 0; y < height + 2; ++y)
      for (u32 x = 0; x < pitch; ++x) {
        const u32 expected = y == 0 || y == height + 1 || x >= width
                               ? guard : src[(((2 * (y - 1) + 1) * SCREEN_H) / (2 * height)) * SCREEN_W + ((2 * x + 1) * SCREEN_W) / (2 * width)];
        CHECK(dst[static_cast<size_t>(y) * pitch + x] == expected);
      }
  }
  // Nearest doesn't force integer placement: 640x480 fills at 2.5x.
  // Explicit integer scaling still fits with borders when requested.
  const auto a = frontend::place_single(0, 1024, 768, frontend::IntScale::Under, true).rect;
  CHECK(a.x == 0 && a.y == 0 && a.w == 1024 && a.h == 768);
  const auto b = frontend::place_single(0, 640, 480, frontend::IntScale::Off, true).rect;
  CHECK(b.x == 0 && b.y == 0 && b.w == 640 && b.h == 480);
  const auto c = frontend::place_single(0, 600, 1000, frontend::IntScale::Under, true).rect;
  CHECK(c.x == 44 && c.y == 308 && c.w == 512 && c.h == 384);
  std::puts("nearest_scale: ok");
}
