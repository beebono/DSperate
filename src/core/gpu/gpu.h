// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds {
struct NDS;
}

namespace ds::gpu {

// 2D engines A/B and the 3D geometry + rasteriser. Planned as a mask-plane
// deferred scanline compositor with statically specialised kernels and NEON
// implementations beside portable C++ references (docs/ARCHITECTURE.md §5).
class Gpu {
public:
  explicit Gpu(NDS& nds) : nds_(nds) {}
  void reset() {}
  void on_scanline(u32 line) {}
  const u16* framebuffer(int screen) const { return fb_[screen]; }

private:
  NDS& nds_;
  u16 fb_[2][SCREEN_W * SCREEN_H] = {};
};

} // namespace ds::gpu
