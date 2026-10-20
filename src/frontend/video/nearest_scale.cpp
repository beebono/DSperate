// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/video/nearest_scale.h"

#include <cassert>
#include <cstring>
#include <vector>
#if DSPERATE_NEON
#include <arm_neon.h>
#endif

namespace ds::frontend {

static void scale_integer(const u32* src, u32* dst, u32 pitch, u32 factor) {
  assert(factor && pitch >= SCREEN_W * factor);
  const size_t row_bytes = SCREEN_W * factor * sizeof(u32);
  for (u32 y = 0; y < SCREEN_H; ++y) {
    const u32* s = src + y * SCREEN_W;
    u32* row = dst + static_cast<size_t>(y) * factor * pitch;
    u32 x = 0;
#if DSPERATE_NEON
    if (factor == 4) {
      for (; x < SCREEN_W; x += 4) {
        const uint32x4_t v = vld1q_u32(s + x);
        vst1q_u32(row + x * 4,      vdupq_n_u32(vgetq_lane_u32(v, 0)));
        vst1q_u32(row + x * 4 + 4,  vdupq_n_u32(vgetq_lane_u32(v, 1)));
        vst1q_u32(row + x * 4 + 8,  vdupq_n_u32(vgetq_lane_u32(v, 2)));
        vst1q_u32(row + x * 4 + 12, vdupq_n_u32(vgetq_lane_u32(v, 3)));
      }
    } else if (factor == 2) {
      for (; x < SCREEN_W; x += 4) {
        const uint32x4_t v = vld1q_u32(s + x);
        const uint32x4x2_t z = vzipq_u32(v, v);
        vst1q_u32(row + x * 2, z.val[0]);
        vst1q_u32(row + x * 2 + 4, z.val[1]);
      }
    }
#endif
    if (factor == 1) std::memcpy(row, s, row_bytes);
    else for (; x < SCREEN_W; ++x)
      for (u32 k = 0; k < factor; ++k) row[x * factor + k] = s[x];
    for (u32 k = 1; k < factor; ++k)
      std::memcpy(row + static_cast<size_t>(k) * pitch, row, row_bytes);
  }
}


void scale_nearest(const u32* src, u32* dst, u32 pitch, u32 width, u32 height) {
  assert(width && height && pitch >= width);
  if (width % SCREEN_W == 0 && height % SCREEN_H == 0 && width / SCREEN_W == height / SCREEN_H) {
    scale_integer(src, dst, pitch, width / SCREEN_W);
    return;
  }
  // Sample the source pixel nearest each destination pixel's centre. Build
  // the horizontal map once, then reuse rows whenever the source y repeats.
  std::vector<u32> xs(width);
  for (u32 x = 0; x < width; ++x)
    xs[x] = static_cast<u32>((static_cast<u64>(x) * 2 + 1) * SCREEN_W / (static_cast<u64>(width) * 2));
  u32 last_y = SCREEN_H;
  for (u32 y = 0; y < height; ++y) {
    const u32 sy = static_cast<u32>((static_cast<u64>(y) * 2 + 1) * SCREEN_H / (static_cast<u64>(height) * 2));
    u32* row = dst + static_cast<size_t>(y) * pitch;
    if (sy == last_y) std::memcpy(row, row - pitch, width * sizeof(u32));
    else {
      const u32* s = src + sy * SCREEN_W;
      for (u32 x = 0; x < width; ++x) row[x] = s[xs[x]];
    }
    last_y = sy;
  }
}

} // namespace ds::frontend
