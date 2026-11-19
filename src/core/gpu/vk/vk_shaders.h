// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_VK_SHADERS_H
#define DS_VK_SHADERS_H
#include "core/types.h"
#include <cstddef>

// GPU raster's SPIR-V, linked in from the tracked blobs beside the .comp sources.

namespace ds::gpu::vk {

struct Spirv {
  const u32* code = nullptr;
  size_t     bytes = 0;      // vkCreateShaderModule wants a byte count
};

Spirv shader_tri_fs();       // a fullscreen triangle (the resolve subpass)
Spirv shader_lean_vert();    // lean path (vk_lean.cpp): each polygon a fan
Spirv shader_lean_frag();    // ... its one fragment stage
Spirv shader_lean_frag_early();   // ... without the alpha test (early depth test) for polygons that cannot fail it
Spirv shader_lean_frag_opq();     // ... a translucent polygon's alpha-31 pixels only (drawn as opaque)
Spirv shader_lean_frag_trans();   // ... its other pixels only (the translucent passes)
Spirv shader_lean_frag_fetch(bool ds_blend);   // a translucent run in one pass, the tile read back (-DDS_ALPHA_SEL=2 -DDS_FETCH=1, or =2: the DS blend)
Spirv shader_lean_resolve(); // ... and the resolve into the 3D layer record
Spirv shader_lean_resolve_1x();   // ... the same over single-sample attachments (aa off)
Spirv shader_lean_mark();         // ... edge marking over the finished record (lean_mark.frag)
Spirv shader_lean_down();         // ... the 1x record of a frame drawn above 1x (lean_down.frag)
Spirv shader_lean_msdec();        // ... the record of a multisampled frame from the hardware's resolve (lean_msdec.frag)
Spirv shader_lean_fog();          // ... the fog as the draw subpass's last draw (the direct output)

} // namespace ds::gpu::vk

#endif // DS_VK_SHADERS_H
