// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/gpu_raster.h"
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_layout.h"
#include "core/types.h"

#include <memory>
#include <string>

// The lean GPU 3D raster on Vulkan (gpu_raster.h has the interface): the
// frame's polygon list (vk_layout.h, the same conversion as the old raster)
// drawn as fans through the hardware rasteriser at 1x with 4x MSAA, on a
// plain depth buffer with the DS rules in the stencil (bit 7 drawn, bit 6
// shadow mask, bits 0-5 the translucent id), per-pixel shading with fog in
// the fragment stage, a custom resolve into the 3D layer record, one submit
// and one fence per frame. No DS edge-row emulation of coverage, no
// per-sample shading, no attribute planes: what the old triangle path paid
// for (see gpu-raster-todo.md, G0). Attributes, though, are the DS's: the
// CPU raster's per-row span endpoints come in a table (span_buffer,
// vk_layout.h) and the fragment stage runs the hardware's fixed-point
// interpolation along the row, so every texel and vertex colour matches the
// CPU raster; only coverage is the GPU's.
namespace ds::gpu::vk {

class Lean final : public Raster3D {
public:
  // `msaa`: 4x MSAA where the device has it (video.aa on the GPU raster); else 1 sample.
  // `edges`: the edge cut instead: 1 sample, and the edge plane beside each record.
  // The device is kept alive by the Lean.
  static std::unique_ptr<Lean> create(std::shared_ptr<Device> dev, bool msaa, bool edges, std::string* why = nullptr);
  ~Lean() override;
  Lean(const Lean&) = delete;
  Lean& operator=(const Lean&) = delete;

  bool ready() const override { return ready_; }
  const char* api() const override { return "Vulkan"; }
  bool msaa() const override;
  u32 scale() const;   // the attachment scale (DS_LEAN_SCALE, probe-only): the record is 256*scale x 192*scale
  bool edges() const override;
  GpuPoly* poly_buffer() override;
  GpuVert* vert_buffer() override;
  u32*     texel_buffer(u32* capacity) override;
  GpuPost* post_buffer() override;
  u32*     span_buffer() override;
  bool submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& f) override;
  void wait() override;
  void wait_all() override;
  const u32* output() override;
  const u32* frame_ready(u64 gen, bool allow_lag) override;
  const u8* edges_for(const u32* out) const override;
  bool has_frame() const override;
  u64 gpu_ns() const override;
  u32 draws() const override;
  Stats stats(bool reset) override;

  struct Impl;   // public for the file-local helpers
private:
  Lean() = default;
  std::unique_ptr<Impl> d_;
  bool ready_ = false;
};

} // namespace ds::gpu::vk
