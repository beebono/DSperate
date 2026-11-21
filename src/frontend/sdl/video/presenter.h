// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "frontend/video/layout.h"

#include <cstring>
#include <memory>
#include <string>

struct SDL_Window;

namespace ds::gpu::kern { struct ColourLut; }
namespace ds::gpu { class Raster3D; struct LayerJobs; }

namespace ds::sdl {

class ScanoutOut;

// The 3D merge (core/gpu/split3d.h): a screen's frame was composed without
// a 3D layer, and the presenter puts it in from the GPU raster's own image,
// at the raster's scale and with no readback: this frame's 3D on engine A's
// screen, or the 3D of a frame that was display-captured on a screen showing
// that capture. Per screen: `words`/`coords`/`lines` are Gpu::Split3DScreen's;
// `view`, `wait`, `gen` Layer3D's, an image of the presenter's device.
// `expect` (DS_MERGE3D_CHECK=1): the screen as the CPU merge leaves it, which
// the presented pixels are compared with. No view: no merge on that screen.
struct Merge3D {
  struct Screen {
    const u32* words = nullptr;
    const u32* coords = nullptr;     // null: no word has split3d::XY
    const u32* lines = nullptr;
    bool scrolled = false;           // some line has BG0's X offset in bits 20-28 of its `lines` word
    u64 view = 0;
    u32 scale = 1;
    gpu::Raster3D* wait = nullptr;   // Layer3D::wait: layer_wait(gen) before the submit
    u64 gen = 0;
    const u32* expect = nullptr;
    bool live = true;                // this frame's 3D (else a capture's, shown again)
    gpu::LayerJobs* later = nullptr; // Layer3D::later, job, from: the view is not known yet, the presenter finds it (Layer3D::resolve) on its own thread
    u64 job = 0;
    gpu::Raster3D* from = nullptr;
    bool has() const { return view || later; }
  } s[2];
  bool any() const { return s[0].has() || s[1].has(); }
};

// A presenter that takes the core's whole 256x192 frames and composes the
// output itself on the GPU: layout, scaling, rotation, LCD filters, the
// inset's alpha and the frontend's overlay. Display drives it in place of
// the CPU scanline scaler (the software presenter) when one opens.
class FramePresenter {
public:
  virtual ~FramePresenter() = default;

  struct View { int screen; frontend::Rect rect; bool shown; bool blends; bool grid = false; };   // grid: LCD grid applies to this view
  struct Params {
    int rot = 0;                 // presented = logical rotated by this
    int lw = 0, lh = 0;          // the logical (unrotated) frame the rects are laid out in
    u8 inset_alpha = 255;
    frontend::Rect drawn;        // where the frontend drew on overlay() this frame
    u32 grid = 256;              // LCD grid brightness on a seam, 0..256 (256 = off)
    bool linear = false;         // video.linear
    u8 seam_blend = 0;           // video.seam: 1 blend, 2 blend in linear light
    Merge3D merge;               // the 3D merge (a nearest view)
    // Colour correction to apply to the frames as they are uploaded
    // (kern::colour_correct on the present thread); null: the frames
    // are shown as they are, corrected by the core or not at all.
    std::shared_ptr<const gpu::kern::ColourLut> colour;
  };

  const std::string& name() const { return name_; }   // for the log: what presents, on which device
  // Which API presents: the menu greys out what a presenter cannot do.
  enum class Kind : u8 { Vulkan };
  virtual Kind kind() const = 0;

  // The output is `w`x`h` presented pixels now. Same: nothing to do.
  // Lost: the presenter cannot continue and Display falls back to software.
  enum class Fit : u8 { Same, Changed, Lost };
  virtual Fit fit(SDL_Window* win, int w, int h) = 0;
  // One frame. False if nothing was shown.
  virtual bool present(const u32* const fb[2], const View* views, int nviews, const Params& p) = 0;
  // The frontend's overlay for the coming frame: `lw`x`lh`, pitch `lw`,
  // 0xAARRGGBB with alpha honoured. Null if unavailable.
  virtual u32* overlay(int lw, int lh) = 0;
  // Everything presented is on its way to the panel (a present with no frame following).
  virtual void flush() = 0;
  // The frame just presented goes to the sink now, once the GPU is done with
  // it, rather than at the start of the next present(). Unlike flush(), it
  // does not wait for the sink to put it on the panel.
  virtual void retire() {}

protected:
  std::string name_;
};

// The dma-buf import presenter (GpuPresent): Vulkan writes the scanout
// sink's own buffers (Wayland dma-buf or KMS), which the sink presents.
// `sink` stays owned by the caller and must outlive the presenter.
std::unique_ptr<FramePresenter> open_vk_import_presenter(ScanoutOut& sink, std::string* why);

} // namespace ds::sdl
