// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// GPU present stage: the scanout tier's CMA buffers are imported into Vulkan
// as LINEAR images, and one render pass per frame lays out, scales, rotates
// and blends the core's two 256x192 frames straight into the buffer the tier
// presents (a quad per view, the overlay over its rectangle, the letterbox
// the pass's clear), replacing the CPU scanline scaler.
//
// Frames are pipelined one deep: present() submits frame N and returns; the
// fence wait and tier end_frame() happen at the start of frame N+1 (or in
// flush()), so the emulation thread stalls only if more than a frame behind.
#pragma once

#include "core/types.h"
#include "frontend/sdl/scanout.h"

#include <SDL.h>
#include <memory>
#include <string>

namespace ds::gpu::kern { struct ColourLut; }

namespace ds::sdl {
struct EdgePlanes;

class GpuPresent {
public:
  // Imports every buffer of `out`; null (with a reason) if unavailable.
  static std::unique_ptr<GpuPresent> open(ScanoutOut& out, std::string* why);
  ~GpuPresent();
  GpuPresent(const GpuPresent&) = delete;
  GpuPresent& operator=(const GpuPresent&) = delete;

  // Tier reopened at a new size: drop and re-import its buffers.
  bool reimport(ScanoutOut& out);

  struct View { int screen; SDL_Rect rect; bool shown; bool blends; bool grid = false; };   // grid: LCD grid applies to this view
  // One frame: take a buffer from the tier, upload fb[0..1], draw,
  // submit. `lw`x`lh` is the logical (unrotated) frame the rects are laid
  // out in; `rot` maps it onto the presented buffer. False if nothing shown.
  // `drawn`: rectangle the frontend's overlay drew this frame (see overlay()).
  // `grid`: LCD grid brightness on a seam, 0..256 (256 = off).
  // `edges`: the edge cut's planes (core/gpu/edge_cut.h), or null: no cut.
  // `colour`: colour correction applied to the frames (and the edge
  // planes' colours) as they are uploaded, by the core's own kernel; null: none.
  // `filter`: how a view samples its frame, as the CPU scanline scaler does
  // (core/gpu/gpu.cpp emit_scaled): nearest (with the grid), bilinear (no
  // grid, no edge cut), or box-filter seams that stand in for the grid.
  enum Filter : u32 { kNearest = 0, kBilinear = 1, kSeamBlend = 2, kSeamLinear = 3 };
  bool present(ScanoutOut& out, const u32* const fb[2], const View* views, int nviews, int rot, int lw, int lh, u8 inset_alpha,
               SDL_Rect drawn = SDL_Rect{0, 0, 0, 0}, u32 grid = 256, const EdgePlanes* edges = nullptr,
               const gpu::kern::ColourLut* colour = nullptr, u32 filter = kNearest);
  // Canvas for the frontend's overlays (OSD, pause menu, notices) for the
  // coming frame: `lw`x`lh`, pitch `lw`, 0xAARRGGBB, alpha honoured,
  // host-cached. Pre-cleared where the frame before last drew.
  u32* overlay(int lw, int lh);
  // Wait for the frame in flight and hand its buffer to the tier.
  void flush(ScanoutOut& out);

  const std::string& device_name() const;

private:
  GpuPresent() = default;
  struct Impl;
  std::unique_ptr<Impl> d_;
};

} // namespace ds::sdl
