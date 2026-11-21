// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu_raster.h"

#include "core/gpu/gl/gl_lean.h"
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_lean.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ds::gpu {

namespace {
// DS_GPU3D_API=null: a raster that takes every frame as the real ones do and draws none of it. Everything on the
// CPU's side of the GPU route then runs with no driver at all: the geometry stream and the list made from it,
// the texture arena, the submit thread, the lag rule's readers, and (DS_SPLIT3D=present) the split compose and
// its merge from the record. That is what the PGO training needs of the route (tools/pgo_refresh.sh trains under
// qemu, where no GPU answers), and it is the same for the Vulkan and the OpenGL ES raster. The record is not a
// picture: each polygon's bounding box in its first vertex's colour, so the readers have 3D pixels to merge.
class NullRaster final : public Raster3D {
public:
  NullRaster() : polys_(DS_MAX_POLYS), verts_(DS_MAX_VERTS), texels_(kTexelWords), scissors_(DS_MAX_POLYS) {
    for (auto& r : rec_) r.assign(256 * 192, 0);
  }
  bool ready() const override { return true; }
  const char* api() const override { return "none (DS_GPU3D_API=null)"; }
  bool msaa() const override { return false; }
  vk::GpuPoly* poly_buffer() override { return polys_.data(); }
  vk::GpuVert* vert_buffer() override { return verts_.data(); }
  u32* texel_buffer(u32* capacity) override { if (capacity) *capacity = kTexelWords; return texels_.data(); }
  vk::GpuPost* post_buffer() override { return &post_; }
  Scissor* scissor_buffer(u32* capacity) override { *capacity = DS_MAX_POLYS; return scissors_.data(); }
  bool submit(u32 npoly, u32, u32, const vk::GpuFrame& f) override {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<u32>& out = rec_[gen_ % kSlots];
    std::fill(out.begin(), out.end(), f.clear_color);
    for (u32 i = 0; i < npoly; ++i) {
      const vk::GpuPoly& p = polys_[i];
      if (p.flags & (DS_PF_SHADOW | DS_PF_SHADOW_MASK)) continue;
      const vk::GpuVert& v = verts_[p.first_vert];
      const u32 a = (p.attr >> 16) & 0x1Fu;
      const u32 word = static_cast<u32>((v.r >> 3) & 63) | (static_cast<u32>((v.g >> 3) & 63) << 8) | (static_cast<u32>((v.b >> 3) & 63) << 16) | ((a ? a : 31u) << 24);
      const s32 x0 = std::clamp<s32>(p.xmin, 0, 255), x1 = std::clamp<s32>(p.xmax, 0, 255);
      for (s32 y = std::clamp<s32>(p.ytop, 0, 191); y <= std::clamp<s32>(p.ybot, 0, 191); ++y)
        std::fill(out.begin() + y * 256 + x0, out.begin() + y * 256 + x1 + 1, word);
    }
    ++gen_; draws_ = npoly;
    return true;
  }
  void wait() override {}
  void wait_all() override {}
  const u32* output() override { return frame_ready(0, false); }
  const u32* frame_ready(u64 gen, bool) override {
    std::lock_guard<std::mutex> lk(m_);
    if (!gen_) return nullptr;
    if (gen == 0 || gen > gen_) gen = gen_;
    if (gen + kSlots <= gen_) gen = gen_ - kSlots + 1;   // no longer resident: the oldest that is
    ++st_.newest;
    return rec_[(gen - 1) % kSlots].data();
  }
  bool has_frame() const override { return gen_ != 0; }
  u64 gpu_ns() const override { return 0; }
  u32 draws() const override { return draws_; }
  Stats stats(bool reset) override { std::lock_guard<std::mutex> lk(m_); const Stats s = st_; if (reset) st_ = Stats{}; return s; }
  bool takes_streams() const override { return true; }
  // Under DS_SPLIT3D=present (the headless run of the split, where no presenter takes the image) it says it has
  // the frames as images, which is what turns the split compose on; the "image" is a token no one opens.
  bool has_layers() const override { return layers_; }
  bool layer(u64 gen, Layer3D* out) override {
    std::lock_guard<std::mutex> lk(m_);
    if (!layers_ || gen == 0 || gen > gen_ || gen + kSlots <= gen_) return false;
    *out = Layer3D{}; out->view = 1; out->scale = 1; out->gen = gen;
    return true;
  }

private:
  const bool layers_ = std::getenv("DS_SPLIT3D") && !std::strcmp(std::getenv("DS_SPLIT3D"), "present");
  static constexpr u32 kTexelWords = 2u << 20;   // as the real rasters' arenas
  static constexpr u64 kSlots = 4;
  std::vector<vk::GpuPoly> polys_;
  std::vector<vk::GpuVert> verts_;
  std::vector<u32> texels_;
  std::vector<Scissor> scissors_;
  vk::GpuPost post_{};
  std::vector<u32> rec_[kSlots];
  std::mutex m_;
  u64 gen_ = 0;
  u32 draws_ = 0;
  Stats st_{};
};
} // namespace

// Vulkan first (the reference backend, and on the RG DS Plus the one that
// shares the device with the present), then OpenGL ES. DS_GPU3D_API=vk|gles
// tries one alone; null is the raster above.
std::unique_ptr<Raster3D> Raster3D::create(bool aa, u32 scale, std::string* why) {
  const char* e = std::getenv("DS_GPU3D_API");
  const bool forced = e && *e;
  const bool want_vk = !forced || !std::strcmp(e, "vk") || !std::strcmp(e, "vulkan");
  const bool want_gl = !forced || !std::strcmp(e, "gl") || !std::strcmp(e, "gles");
  std::string wv = "not tried", wg = "not tried";
  if (forced && !std::strcmp(e, "null")) return std::make_unique<NullRaster>();
  if (want_vk) {
    std::shared_ptr<vk::Device> dev = vk::Device::shared(&wv);
    if (dev) {
      std::unique_ptr<vk::Lean> l = vk::Lean::create(dev, aa, &wv, scale < 1 ? 1 : scale);
      if (l && l->ready()) return l;
    }
  }
  if (want_gl) {
    std::unique_ptr<gl::Lean> l = gl::Lean::create(aa, &wg);
    if (l && l->ready()) return l;
  }
  if (why) *why = "Vulkan: " + wv + "; OpenGL ES: " + wg;
  return nullptr;
}

bool Raster3D::warm(bool aa, u32 scale) {
  const char* e = std::getenv("DS_GPU3D_API");
  if (e && *e && std::strcmp(e, "vk") != 0 && std::strcmp(e, "vulkan") != 0) return false;
  std::string why;
  std::shared_ptr<vk::Device> dev = vk::Device::shared(&why);
  if (!dev) return false;
  std::unique_ptr<vk::Lean> l = vk::Lean::create(dev, aa, &why, scale < 1 ? 1 : scale);
  if (!l || !l->ready()) return false;
  l->wait_pipelines();   // the set built on a thread too
  return true;
}

} // namespace ds::gpu
