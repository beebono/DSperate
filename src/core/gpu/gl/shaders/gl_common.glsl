// The GL raster's shader prelude (gl_lean.cpp splices it after the #version
// line of every stage): precisions, the frame constants as a uniform block
// (Vulkan's push constants) and the texel arena as a storage buffer
// (ds_shade.glsl reads `texels[]`), so vk_layout.h, ds_shade.glsl and
// ds_spans.glsl are the Vulkan raster's own files, unchanged.
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp usampler2D;
#define DS_GLSL 1
#include "vk_layout.h"
layout(std140, binding = 0) uniform PCB { GpuFrame f; } pc;
// DS_WCLIP (gl_lean.cpp, the default): a W-buffered frame keeps wscale / w in the clip z (nearer is larger) instead of writing w by gl_FragDepth.
#ifndef DS_WCLIP
#define DS_WCLIP 0
#endif
