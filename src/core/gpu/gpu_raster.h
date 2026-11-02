// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/vk/vk_layout.h"
#include "core/types.h"

#include <memory>
#include <string>

// The GPU 3D raster behind Renderer3D::set_gpu: the frame's polygon list
// (vk_layout.h) drawn by the hardware rasteriser at 1x into the 3D layer
// record. Two backends draw the same frame the same way: Vulkan (vk/vk_lean.h,
// the reference) and OpenGL ES 3.1 (gl/gl_lean.h, for devices whose driver
// has no Vulkan: the RG35XX SP's Mali-G31 blob). create() picks the first
// that comes up, Vulkan before GLES; DS_GPU3D_API=vk|gles forces one.
namespace ds::gpu {

class Raster3D {
public:
  virtual ~Raster3D() = default;
  // `aa`: video.aa for the GPU raster (4x MSAA on Vulkan where the device
  // has it); `edges`: the edge cut instead (core/gpu/edge_cut.h): 1 sample,
  // and beside each record the edge plane of the drawn polygons (edges_for).
  static std::unique_ptr<Raster3D> create(bool aa, bool edges, std::string* why = nullptr);

  virtual bool ready() const = 0;
  virtual const char* api() const = 0;   // "Vulkan" / "OpenGL ES"
  virtual bool msaa() const = 0;   // 4x MSAA in use (else 1 sample: the device could not)
  virtual bool edges() const = 0;  // the edge plane is drawn

  // Frame input: polygons, vertices and post tables are host staging
  // (rewritten freely between submits); the texel arena is mapped device
  // memory, append-only until wait_all() (write only, never read back).
  virtual vk::GpuPoly* poly_buffer() = 0;
  virtual vk::GpuVert* vert_buffer() = 0;
  virtual u32*         texel_buffer(u32* capacity) = 0;
  virtual vk::GpuPost* post_buffer() = 0;
  // The span table staging (vk_layout.h): DS_MAX_SPAN_ROWS rows of DS_SPAN_WORDS; the frame's row count travels in GpuFrame::nrows.
  virtual u32*         span_buffer() = 0;

  // Draw the frame into the next of four output slots; false if refused.
  // Frames are in flight until read: wait() blocks on the newest, output()
  // is the newest after a wait, and newest_ready() applies the lag rule.
  // Submits never block on an earlier frame (each slot has its own input
  // buffers); the caller only waits before rewriting the texel arena.
  // submit() runs on one thread (the renderer's GPU job thread) and may
  // bind that thread; the readers below may run on any.
  virtual bool submit(u32 npoly, u32 nvert, u32 ntexels, const vk::GpuFrame& f) = 0;
  virtual void wait() = 0;
  virtual void wait_all() = 0;   // every frame in flight done; the texel arena may be rewritten from the start
  // 256*192 words: RGB666 in bits 0-21, 5-bit alpha in bits 24-28 (the CPU raster's layout).
  virtual const u32* output() = 0;
  // The record of submitted frame `gen` (1 = the first submit; 0 = the
  // newest), if the GPU has finished it; else, when `allow_lag`, the frame
  // before it (one frame of latency instead of a stall); else a wait on it.
  // The compositor asks for the frame its display frame was rendered with
  // (Renderer3D::FrameRef::gpu_gen): the emulation thread may have submitted
  // the next one by the time a lazily composed line reads the 3D layer, and
  // "the newest" would be a frame ahead. A frame no longer resident (more
  // than four back) resolves to the oldest that is. A frame not submitted
  // yet (its submit still recording on the job thread): the one before it,
  // waited for, when `allow_lag`; else null, and the caller waits for the
  // submit and asks again. Null before the first frame.
  virtual const u32* frame_ready(u64 gen, bool allow_lag) = 0;
  const u32* newest_ready(bool allow_lag) { return frame_ready(0, allow_lag); }
  // The edge bytes drawn with record `out` (from output / newest_ready), one
  // per pixel, the edge byte of Renderer3D::set_edge_export's layout (the
  // pixel's colour is the record's); null without edges.
  virtual const u8* edges_for(const u32* out) const = 0;
  virtual bool has_frame() const = 0;

  // GPU time of the last waited frame in ns (DS_VK_TIMING=1 and timestamps), else 0.
  virtual u64 gpu_ns() const = 0;
  // Draws issued for the last frame (opaque prefix + tail runs, both passes).
  virtual u32 draws() const = 0;
  // Diagnostics (DS_GPU_TRACE): CPU ms of the last submit's command recording
  // and queue submit; how newest_ready() resolved since the last reset.
  struct Stats { double record_ms = 0, queue_ms = 0, slot_wait_ms = 0, prep_ms = 0, gpu_ms = 0; u32 newest = 0, lagged = 0, stalled = 0, nolag = 0, timed = 0, wbuf = 0; double lat_ms = 0; u32 lat_n = 0; double gap_ms = 0; u32 gap_n = 0; };   // gap: submit to the first read of that frame   // lat: submit to signal, exact from blocking waits
  virtual Stats stats(bool reset) = 0;
};

} // namespace ds::gpu
