// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstddef>
#include <string>

// The GL raster's GLSL ES sources, linked in from the tracked files beside
// gl_lean.cpp (gl/shaders/) and the Vulkan raster's shared pieces
// (vk/vk_layout.h, vk/ds_shade.glsl, vk/ds_spans.glsl). source() assembles
// one stage: the prelude (gl_common.glsl) and the variant's defines spliced
// after the leading #version / #extension lines, every #include resolved.
namespace ds::gpu::gl {

struct Text { const char* data = nullptr; size_t size = 0; };

Text text_gl_common();    // the prelude
Text text_gl_pull();      // vertex-stage polygon/vertex fetch
Text text_vk_layout();    // vk_layout.h
Text text_ds_shade();     // ds_shade.glsl
Text text_ds_spans();     // ds_spans.glsl
Text text_lean_vert();
Text text_lean_frag();
Text text_resolve_frag();
Text text_edge_vert();
Text text_edge_frag();
Text text_cutout_frag();
Text text_fs_vert();
Text text_null_frag();

// `defines`: "#define X 1\n" lines, may be empty.
std::string source(Text stage, const std::string& defines);

} // namespace ds::gpu::gl
