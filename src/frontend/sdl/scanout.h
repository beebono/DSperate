// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The interface the scanline path presents through when it is not writing
// into SDL's window surface.
//
// Both implementations do the same thing by different means: allocate CMA
// dma-heap buffers, hand one out per frame for the core to scale into, and
// then get it in front of the user without a copy -- DmabufOut (display_wl.h)
// by attaching it to SDL's Wayland surface, DrmOut (display_drm.h) by
// page-flipping it onto the CRTC SDL's KMSDRM backend already drives. What
// Display needs from either is a buffer, its size, and a present.
//
// The waiting is deliberately asymmetric: begin_frame() blocks (for a buffer
// release, or for the previous flip) and end_frame() does not. The display's
// pacing is then felt before the frame's emulation rather than after its
// present, which is where the frame loop already has slack to absorb it.
#pragma once

#include "core/types.h"

struct SDL_Window;

namespace ds::sdl {

class ScanoutOut {
public:
  virtual ~ScanoutOut() = default;

  // Re-establish at a new size after a configure/mode change. False leaves
  // the object closed and the caller drops the tier.
  virtual bool reopen(SDL_Window* win, int w, int h) = 0;
  virtual void close() = 0;

  virtual int width() const = 0;
  virtual int height() const = 0;
  // Row pitch in pixels; the width unless the buffer is padded (fbdev).
  virtual int stride() const { return width(); }
  // How many buffers rotate, and which one begin_frame() last handed out
  // (0..bufs()-1; -1 outside a frame). No tier rotates round-robin -- each
  // takes the lowest free buffer -- so a caller that must touch every
  // buffer once (the letterbox clear after a layout change) keys off the
  // index, never off a count of frames.
  virtual int bufs() const = 0;
  virtual int current() const = 0;

  // Pixels of a free buffer to render the next frame into, or null on a
  // protocol/driver error, after which the caller falls back.
  virtual u32* begin_frame() = 0;
  virtual void end_frame() = 0;
};

} // namespace ds::sdl
