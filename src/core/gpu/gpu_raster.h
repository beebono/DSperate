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
class Raster3D;
// What a frame's submit thread answers to a reader on another thread (Layer3D::later).
struct LayerJobs { virtual ~LayerJobs() = default; virtual void layer_job_wait(u64 job) = 0; };
// Raster3D::layer. With `later` set the image is not known yet (the frame's
// submit is still on its thread: `view` is 0, `scale` and `gen` are right)
// and the reader calls resolve() on its own thread before its submit.
struct Layer3D {
  u64 view = 0; u32 scale = 1; u64 gen = 0; Raster3D* wait = nullptr;
  LayerJobs* later = nullptr; u64 job = 0; Raster3D* from = nullptr; bool live = true;
  // Waits for the frame's submit and asks its raster. A live frame the raster
  // refused has the frame before's image (the picture that stands). False: no image, no merge.
  inline bool resolve();
};

class Raster3D {
public:
  virtual ~Raster3D() = default;
  // `aa`: video.aa for the GPU raster (4x MSAA on Vulkan where the device
  // has it).
  // `scale`: the 3D image at 256 * scale x 192 * scale (1-4), for a compositor
  // that takes it as an image (layer()); the record read here stays 256x192.
  // A backend that hands no image over draws at 1 whatever is asked (scale()).
  static std::unique_ptr<Raster3D> create(bool aa, u32 scale, std::string* why = nullptr);
  // Builds the Vulkan raster for these settings and lets it go, so its pipelines are in the device's pipeline
  // cache (and the cache's file) and the create() that follows is quick: the first build of a setting compiles
  // every shader, seconds on the Mali-G52. For a thread of its own while the frontend says what is going on.
  // False where there is no Vulkan raster to build (the OpenGL ES raster keeps no cache: nothing to warm).
  static bool warm(bool aa, u32 scale);

  virtual bool ready() const = 0;
  virtual const char* api() const = 0;   // "Vulkan" / "OpenGL ES"
  virtual bool msaa() const = 0;   // 4x MSAA in use (else 1 sample: the device could not)
  virtual u32 scale() const { return 1; }
  // The frames should reach the panel from the raster's own image (the 3D merge) at 1x too, not only above it:
  // where a CPU composite of the 1x record shows wrong (Imagination: the picture jumps back a frame now and then).
  virtual bool merge_at_1x() const { return false; }

  // Frame input: polygons, vertices and post tables are host staging
  // (rewritten freely between submits); the texel arena is mapped device
  // memory, append-only until wait_all() (write only, never read back).
  virtual vk::GpuPoly* poly_buffer() = 0;
  virtual vk::GpuVert* vert_buffer() = 0;
  virtual u32*         texel_buffer(u32* capacity) = 0;
  virtual vk::GpuPost* post_buffer() = 0;
  // Polygons of the list drawn inside a rectangle of the screen alone (render3d.cpp gpu_tex_clip: the part where
  // their texture has anything in it): polygon index in the list as submitted, then x0, y0, x1, y1 in DS pixels,
  // the last two exclusive. Staging as the others; scissor_count() before each submit (0: none). A backend
  // without it has no buffer and draws every polygon whole, which is the same picture.
  struct Scissor { u32 poly; u16 x0, y0, x1, y1; };
  virtual Scissor*     scissor_buffer(u32* capacity) { *capacity = 0; return nullptr; }
  virtual void         scissor_count(u32 /*n*/) {}

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
  // Debug (DS_LEAN_FINE=1, the headless --dump-3d-fine): the whole record at scale() beside
  // the 1x one `out`, 256 * scale words a row; null where the backend keeps none.
  virtual const u32* fine_for(const u32* /*out*/) const { return nullptr; }
  virtual bool has_frame() const = 0;
  // The record of submitted frame `gen` as an image, for a compositor on the
  // raster's own device (the Vulkan present's 3D merge, split3d.h): no
  // readback, and at the raster's scale. `view`: a VkImageView, the record's
  // texels a byte a channel (RGBA8UI: r, g, b of RGB666, then the 5-bit
  // alpha), in SHADER_READ_ONLY_OPTIMAL once the frame's submit has run. The
  // reader submits after this call, and on the device's shared queue the GPU
  // orders the two. Where the raster has a queue of its own nothing does:
  // `wait` is then the raster, and the reader calls layer_wait(gen) before
  // its submit (any thread; it blocks until the GPU has finished the frame).
  // The image holds until four more frames are submitted, or while pinned (pin()).
  // False: no such image (the frame is not submitted yet or no longer
  // resident, or the backend has none) and the caller merges from the record.
  virtual bool layer(u64 /*gen*/, Layer3D* /*out*/) { return false; }
  virtual bool has_layers() const { return false; }
  virtual void layer_wait(u64 /*gen*/) {}
  // Keep the images of up to two frames (0: none) beyond the four-frame
  // residency, until the next call: a captured frame the game goes on
  // showing while later ones are drawn. Any thread.
  virtual void pin(u64 /*a*/, u64 /*b*/) {}

  // GPU time of the last waited frame in ns (DS_VK_TIMING=1 and timestamps), else 0.
  virtual u64 gpu_ns() const = 0;
  // Draws issued for the last frame (opaque prefix + tail runs, both passes).
  virtual u32 draws() const = 0;
  // Diagnostics (DS_GPU_TRACE): CPU ms of the last submit's command recording
  // and queue submit; how newest_ready() resolved since the last reset.
  // stage_ms/stage_max (DS_VK_TIMING=2, Vulkan): GPU time per stage, summed over the `timed` frames and the worst
  // frame's: 0 what precedes the main render pass (nothing now: the row table's compute once), 1 the main render pass (draws and resolve), 2 edge marking, 3 the rest
  // (the 1x record's pass, the copies out).
  static constexpr unsigned kStages = 4;
  struct Stats { double stage_ms[kStages] = {}, stage_max[kStages] = {}; double record_ms = 0, queue_ms = 0, slot_wait_ms = 0, prep_ms = 0, gpu_ms = 0; u32 newest = 0, lagged = 0, stalled = 0, nolag = 0, timed = 0, wbuf = 0; u32 rec_late = 0;   /* 1x records made on a read, not with their frame */  double lat_ms = 0; u32 lat_n = 0; double gap_ms = 0; u32 gap_n = 0; };   // gap: submit to the first read of that frame   // lat: submit to signal, exact from blocking waits
  virtual Stats stats(bool reset) = 0;

  // The renderer may give this raster stream frames (Renderer3D::gpu_stream_job: the vertex stage on the
  // submit thread, the list it makes submitted there like any other).
  virtual bool takes_streams() const { return false; }
};

inline bool Layer3D::resolve() {
  if (!later) return view != 0;
  later->layer_job_wait(job);
  later = nullptr;
  const u64 g = gen; const bool was_live = live;
  Layer3D got;
  if (!from || !(from->layer(g, &got) || (was_live && g > 1 && from->layer(g - 1, &got)))) { view = 0; return false; }
  view = got.view; scale = got.scale; gen = got.gen; wait = got.wait;
  return true;
}

} // namespace ds::gpu
