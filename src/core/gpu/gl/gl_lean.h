// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/gpu_raster.h"
#include "core/gpu/vk/vk_layout.h"
#include "core/types.h"

#include <memory>
#include <string>

// The lean GPU 3D raster on OpenGL ES 3.1 (gpu_raster.h has the interface;
// vk/vk_lean.h the Vulkan original this follows draw for draw): for devices
// whose driver has no Vulkan, the RG35XX SP's Mali-G31 blob first. The
// same shading (the Vulkan raster's ds_shade.glsl and ds_spans.glsl,
// included as they are), the same stencil rules, the same passes; GL's
// differences are kept to the edges of the frame:
// - no storage buffers in the vertex stage on Mali, so polygons and
//   vertices reach it as RGBA32UI textures (gl_pull.glsl); the fragment
//   stage reads them, the span table, the post tables and the texel arena
//   as storage buffers, all persistently mapped (EXT_buffer_storage,
//   required) so the CPU writes them as the Vulkan path does;
// - no subpasses: the edge passes and the resolve read the draw's
//   attachments as textures after it (0.3 MB of tile writeback);
// - DS row y draws on GL row y, so gl_FragCoord is the DS coordinate and
//   the record reads back top row first with no flip;
// - W-buffered frames keep wscale / w in the clip z, wscale the frame's
//   nearest w, so the far end keeps its precision through GL's fixed depth
//   mapping (the Vulkan path stores 1/w as is); DS_GL_WCLIP=0 writes
//   gl_FragDepth from the perspective-correct w instead, exact but late;
// - the record and the edge bytes leave through RGBA8 attachments and
//   glReadPixels into persistently mapped PBOs, their bytes the record's
//   own layout;
// - frames are fenced with EGL_KHR_fence_sync, which needs no current
//   context, so newest_ready()/wait() run on the compositor's thread as
//   with Vulkan, and GL itself is touched by submit() alone: the context
//   binds to the first thread that submits (the renderer's GPU job thread).
// Requires ES 3.1 with EXT_buffer_storage; uses EXT_draw_buffers_indexed
// (ES 3.2) for the per-attachment blend, EXT_disjoint_timer_query for
// DS_GL_TIMING=1, OES_shader_multisample_interpolation when there.
namespace ds::gpu::gl {

class Lean final : public Raster3D {
public:
  // `edges`: the edge cut (the GL raster's AA; `aa` without it draws plain: no MSAA here).
  static std::unique_ptr<Lean> create(bool aa, bool edges, std::string* why = nullptr);
  ~Lean() override;
  Lean(const Lean&) = delete;
  Lean& operator=(const Lean&) = delete;

  bool ready() const override { return ready_; }
  const char* api() const override { return "OpenGL ES"; }
  bool msaa() const override { return false; }
  bool edges() const override;
  vk::GpuPoly* poly_buffer() override;
  vk::GpuVert* vert_buffer() override;
  u32*         texel_buffer(u32* capacity) override;
  vk::GpuPost* post_buffer() override;
  u32*         span_buffer() override;
  bool submit(u32 npoly, u32 nvert, u32 ntexels, const vk::GpuFrame& f) override;
  void wait() override;
  void wait_all() override;
  const u32* output() override;
  const u32* frame_ready(u64 gen, bool allow_lag) override;
  const u8* edges_for(const u32* out) const override;
  bool has_frame() const override;
  u64 gpu_ns() const override;
  u32 draws() const override;
  Stats stats(bool reset) override;

  struct Impl;
private:
  Lean() = default;
  std::unique_ptr<Impl> d_;
  bool ready_ = false;
};

} // namespace ds::gpu::gl
