// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/vram_map.h"

#include <unordered_map>
#include <vector>

namespace ds::gpu {

// Decoded-texture cache for the 3D rasteriser.
//
// Sampling a DS texture from VRAM costs a texel load, a palette load and for
// the compressed format a palette-info load and a four-colour decode — a
// chain of dependent loads per pixel that the in-order core cannot hide.
// DS textures are small (a few KB) and most of them are reused every frame,
// so the cache decodes each (format, address, size, palette) once into one
// 32-bit word per texel — the 16-bit colour the sampler would return in
// the low half, the 5-bit alpha above it — and the span kernels sample
// with a single load per texel.
//
// Validity is checked by content, not by write tracking: the first time a
// texture is used in a frame, its source bytes (texels, the compressed
// format's palette-info slot, the palette range it touches) are compared
// with the copy taken at decode time and the texture is re-decoded on any
// difference. VRAM bank remaps, DMA, display capture and palette animation
// all fall out of that, and the comparison reads each texture once per frame
// instead of once per pixel. Entries unused for a frame are dropped when the
// cache exceeds its budget.
class TextureCache {
public:
  static constexpr size_t BUDGET_BYTES = 24u << 20;

  void begin_frame(u64 frame);
  // Decoded texels (width*height words) for a polygon's texture, decoding or
  // revalidating as needed; nullptr when the cache is disabled.
  const u32* lookup(const VramMap& vm, u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0);
  void clear();
  bool enabled() const { return enabled_; }
  u32  decodes_this_frame() const { return decodes_; }
  void set_enabled(bool on) { enabled_ = on; if (!on) clear(); }

private:
  struct Source { u32 addr = 0, len = 0; bool palette = false; };   // a validated range
  struct Entry {
    u32 fmt, base, width, height, texpal, alpha0;
    Source src[3]; u32 nsrc = 0;
    std::vector<u8> copy;        // the source ranges, concatenated
    std::vector<u32> texels;
    u64 validated = 0, used = 0; // frames
  };
  static u64 key(u32 fmt, u32 base, u32 width, u32 height, u32 texpal, u32 alpha0);
  void decode(const VramMap& vm, Entry& e);
  void snapshot(const VramMap& vm, Entry& e);
  bool unchanged(const VramMap& vm, const Entry& e) const;

  std::unordered_map<u64, Entry> entries_;
  size_t bytes_ = 0;
  u64 frame_ = 0;
  u32 decodes_ = 0;
  bool enabled_ = true;
};

} // namespace ds::gpu
