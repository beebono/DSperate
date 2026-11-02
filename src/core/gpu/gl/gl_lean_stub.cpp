// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gl/gl_lean.h"

// Built without the GL raster (DSPERATE_GLES=OFF): it declines.
namespace ds::gpu::gl {
struct Lean::Impl {};
Lean::~Lean() = default;
std::unique_ptr<Lean> Lean::create(bool, bool, std::string* why) { if (why) *why = "built without OpenGL ES"; return nullptr; }
bool Lean::edges() const { return false; }
vk::GpuPoly* Lean::poly_buffer() { return nullptr; }
vk::GpuVert* Lean::vert_buffer() { return nullptr; }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = 0; return nullptr; }
vk::GpuPost* Lean::post_buffer() { return nullptr; }
u32* Lean::span_buffer() { return nullptr; }
bool Lean::submit(u32, u32, u32, const vk::GpuFrame&) { return false; }
void Lean::wait() {}
void Lean::wait_all() {}
const u32* Lean::output() { return nullptr; }
const u32* Lean::frame_ready(u64, bool) { return nullptr; }
const u8* Lean::edges_for(const u32*) const { return nullptr; }
bool Lean::has_frame() const { return false; }
u64 Lean::gpu_ns() const { return 0; }
u32 Lean::draws() const { return 0; }
Lean::Stats Lean::stats(bool) { return {}; }
} // namespace ds::gpu::gl
