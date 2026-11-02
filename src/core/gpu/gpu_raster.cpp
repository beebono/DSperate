// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu_raster.h"

#include "core/gpu/gl/gl_lean.h"
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_lean.h"

#include <cstdlib>
#include <cstring>

namespace ds::gpu {

// Vulkan first (the reference backend, and on the RG DS Plus the one that
// shares the device with the present), then OpenGL ES. DS_GPU3D_API=vk|gles
// tries one alone.
std::unique_ptr<Raster3D> Raster3D::create(bool aa, bool edges, std::string* why) {
  const char* e = std::getenv("DS_GPU3D_API");
  const bool forced = e && *e;
  const bool want_vk = !forced || !std::strcmp(e, "vk") || !std::strcmp(e, "vulkan");
  const bool want_gl = !forced || !std::strcmp(e, "gl") || !std::strcmp(e, "gles");
  std::string wv = "not tried", wg = "not tried";
  if (want_vk) {
    std::shared_ptr<vk::Device> dev = vk::Device::shared(&wv);
    if (dev) {
      std::unique_ptr<vk::Lean> l = vk::Lean::create(dev, aa, edges, &wv);
      if (l && l->ready()) return l;
    }
  }
  if (want_gl) {
    std::unique_ptr<gl::Lean> l = gl::Lean::create(aa, edges, &wg);
    if (l && l->ready()) return l;
  }
  if (why) *why = "Vulkan: " + wv + "; OpenGL ES: " + wg;
  return nullptr;
}

} // namespace ds::gpu
