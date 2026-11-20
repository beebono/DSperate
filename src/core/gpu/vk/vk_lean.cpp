// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu_timeline.h"
#include "core/gpu/vk/vk_lean.h"

#if !DSPERATE_VULKAN
// Built without the Vulkan headers: the raster declines, and the renderer
// keeps drawing on the CPU (Renderer3D::set_gpu reports the reason).
namespace ds::gpu::vk {
struct Lean::Impl {};
Lean::~Lean() = default;
std::unique_ptr<Lean> Lean::create(std::shared_ptr<Device>, bool, std::string* why, u32) { if (why) *why = "built without Vulkan"; return nullptr; }
bool Lean::msaa() const { return false; }
void Lean::wait_pipelines() {}
u32 Lean::scale() const { return 1; }
const u32* Lean::fine_for(const u32*) const { return nullptr; }
GpuPoly* Lean::poly_buffer() { return nullptr; }
GpuVert* Lean::vert_buffer() { return nullptr; }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = 0; return nullptr; }
Raster3D::Scissor* Lean::scissor_buffer(u32* capacity) { *capacity = 0; return nullptr; }
void Lean::scissor_count(u32) {}
GpuPost* Lean::post_buffer() { return nullptr; }
bool Lean::submit(u32, u32, u32, const GpuFrame&) { return false; }
void Lean::wait() {}
void Lean::wait_all() {}
const u32* Lean::output() { return nullptr; }
const u32* Lean::frame_ready(u64, bool) { return nullptr; }
bool Lean::has_frame() const { return false; }
bool Lean::layer(u64, Layer3D*) { return false; }
bool Lean::has_layers() const { return false; }
void Lean::layer_wait(u64) {}
void Lean::pin(u64, u64) {}
u64 Lean::gpu_ns() const { return 0; }
u32 Lean::draws() const { return 0; }
Lean::Stats Lean::stats(bool) { return {}; }
} // namespace ds::gpu::vk
#else

#include "core/gpu/vk/vk_internal.h"
#include "core/gpu/vk/vk_shaders.h"
#include "core/gpu/vk/lean_runs.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ds::gpu::vk {

namespace {

// The attachment size: 256x192 times the scale (Lean::create). One raster
// draws at a time: a rebuilt one takes over after the old one's last frame.
u32 W = 256, H = 192;
constexpr u32 kTexelWords = 2u << 20;   // 8 MB of decoded texels
constexpr u32 kMaxDraws = 4096;
// Stencil layout: bit 7 drawn, bit 6 shadow mask, bit 5 the last write was
// translucent, bits 0-4 the pixel's polygon id as a per-frame code. The DS
// rules that read the id are "equal translucent ids don't blend" and
// "a shadow skips its own id (opaque or translucent)", so only the ids of
// translucent and shadow polygons need telling apart: they get codes 1..31
// per frame, every other id is code 0 (never equal to anything). A frame
// with more than 31 such ids folds them (id & 31: rare, and only a wrong
// same-id refusal).
constexpr u32 S_DRAWN = 0x80, S_MASK = 0x40, S_T = 0x20, S_CODE = 0x1F;
// Edge marking's word, a byte a field so a pass can write the depth alone: r the polygon id (bits 0-5), its fog bit (6)
// and "a pixel of the polygon's edge" (7); g, b, a the DS depth >> 1, low byte first (lean.frag o_id, lean_mark.frag).
constexpr VkFormat kIdFormat = VK_FORMAT_R8G8B8A8_UINT;
enum Pipe : u32 { P_OPAQUE = 0, P_TRANS_A, P_TRANS_B, P_TRANS_A_DW, P_TRANS_B_DW, P_MASK, P_SHADOW, P_SHADOW_PREP, P_MASK_CLEAR, P_OPAQUE_EARLY, P_TRANS_OPQ, P_TRANS_ONE, P_TRANS_ONE_DW, P_TRANS_KEEP, P_TRANS_KEEP_DW, P_COUNT };   // KEEP: the one-pass stage keeping the pixels of alpha 31 (Impl::fold)
}

struct Lean::Impl {
  const DeviceInternal* vk = nullptr;
  const Api* api = nullptr;
  Device* dev = nullptr;
  std::shared_ptr<Device> keep;   // the device outlives this raster
  bool msaa = false, timing = false, timing_stages = false;
  static constexpr u32 kTs = 5;   // timestamps a slot: start, before the main pass, after the main pass, after marking, end
  u32 scale = 1;
  double tick_ns = 0;
  VkFormat ds_format = VK_FORMAT_D24_UNORM_S8_UINT;

  static constexpr u32 kSlots = 6;   // four in rotation as the readers assume, and two that pin() may hold back
  // The frame's polygons, vertices and post tables are staged on the host
  // (built and read there: mapped write-combined memory is not for reading)
  // and copied once into the slot's own device buffers, so the next frame
  // is built while the GPU draws this one. The texel arena is shared and
  // append-only: a frame reads only what was in it when it was submitted.
  std::vector<GpuPoly> host_polys;
  std::vector<GpuVert> host_verts;
  GpuPost host_post{};
  Buffer polys[kSlots], verts[kSlots], post[kSlots], texels, out[kSlots];
  Buffer fine[kSlots];   // DS_LEAN_FINE=1 above 1x (debug): the whole record read back as well, for comparisons at the raster's own scale
  u32 tex_flushed = 0;   // arena words already flushed to the GPU
  VkBufferView texel_view = VK_NULL_HANDLE;
  struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
  Img col_ms, ds_ms, fog_ms;   // multisampled colour, depth/stencil and fog flag (transient)
  // The resolved record, one image per output slot: a compositor on this
  // device (the Vulkan present's 3D merge) samples a frame's while later
  // frames draw into the others. res8: the same texels a byte a channel
  // (RGBA8UI), which the Mali samples faster than it unpacks R32UI.
  Img res[kSlots];
  VkImageView res8[kSlots] = {};
  // Above 1x in the emulator (`down`): the CPU's record stays 256x192, one
  // texel of each scale x scale block (a pass of its own into rec1,
  // lean_down.frag, then the copy out): display capture and every CPU
  // composite read that, the image above is what a compositor shows. (A
  // nearest vkCmdBlitImage does the same and cost the Mali-G52 5-7 ms.)
  bool down = false;
  Img rec1;
  // The 1x record only where it is read. A frame composed for the
  // present's 3D merge takes its 3D from the raster's image, and the pass and copy for the CPU's record were
  // 1.5-1.8 ms of every 4x frame on the G52. A submit carries them while a reader is about (rec_keep: counts down
  // from the last read); a frame without them makes its record when one asks after all (record_now), through an
  // image, framebuffer and command buffer of its own so the submits in flight are not disturbed.
  bool lazy_rec = false;
  u32 rec_keep = 0;
  bool rec_ok[kSlots] = {};
  Img rec1b;
  VkFramebuffer fb_down2 = VK_NULL_HANDLE;
  VkCommandPool cpool_rec = VK_NULL_HANDLE;
  VkCommandBuffer cmd_rec = VK_NULL_HANDLE;
  VkFence fence_rec = VK_NULL_HANDLE;
  VkRenderPass rp_down = VK_NULL_HANDLE;
  VkFramebuffer fb_down = VK_NULL_HANDLE;
  VkPipeline pipe_down = VK_NULL_HANDLE;
  VkPipelineLayout layout_down = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_down = VK_NULL_HANDLE;
  VkDescriptorPool pool_down = VK_NULL_HANDLE;
  VkDescriptorSet set_down[kSlots] = {};
  VkSampler samp_down = VK_NULL_HANDLE;
  VkShaderModule mod_down = VK_NULL_HANDLE;
  // Edge marking (DISP3DCNT bit 5; lean_mark.frag): the draw subpass has a third colour attachment,
  // idm, written by the opaque passes and stored only in frames that mark edges (rp_keep: the same
  // render pass with that store), and a pass over the record then recolours the edge pixels. Under MSAA the attachment is multisampled and its resolve (one sample's word) is what is kept. Without
  // the direct output. DS_LEAN_EDGEMARK=0 leaves the pass out (the attachment
  // stays); mark_width is the second neighbour distance in raster pixels: the scale, one DS pixel
  bool mark = false, mark_on = true;
  u32 mark_width = 1;
  Img idm, idm_ms;   // idm_ms: under MSAA the draw's attachment, resolved (one sample's word) into idm
  VkRenderPass rp_keep = VK_NULL_HANDLE, rp_mark = VK_NULL_HANDLE;
  VkFramebuffer fb_mark[kSlots] = {};
  VkImageView resn[kSlots] = {};   // each record as normalised bytes: the marking pass's attachment, and the direct output's colour
  VkPipeline pipe_mark = VK_NULL_HANDLE;
  VkPipelineLayout layout_mark = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_mark = VK_NULL_HANDLE;
  VkDescriptorPool pool_mark = VK_NULL_HANDLE;
  VkDescriptorSet set_mark[kSlots] = {};
  VkSampler samp_mark = VK_NULL_HANDLE;
  VkShaderModule mod_mark = VK_NULL_HANDLE;
  VkImageView ds_depth_view = VK_NULL_HANDLE;   // the depth aspect alone, for the resolve's input attachment
  VkShaderModule mod_vert = VK_NULL_HANDLE, mod_frag = VK_NULL_HANDLE, mod_frag_early = VK_NULL_HANDLE, mod_frag_opq = VK_NULL_HANDLE, mod_frag_trans = VK_NULL_HANDLE, mod_frag_fetch = VK_NULL_HANDLE, mod_res = VK_NULL_HANDLE, mod_fs = VK_NULL_HANDLE;
  // A translucent run in one pass (B3) where the device has VK_EXT_rasterization_order_attachment_access:
  // subpass 0 reads its own colour attachment in rasterisation order and the shader blends (lean.frag
  // DS_FETCH). DS_LEAN_FETCH=2 (default) the DS's own blend (melonDS AlphaBlend, DISP3DCNT bit 3):
  // the blend unit rounds to nearest where the DS truncates, a pixel under a translucent layer one
  // or two of 63 brighter (Spirit Tracks: 47% of a frame's pixels), and it costs the G52 no frame
  // time at 1x. 1 the blend unit's maths (what the two-pass draws give); 0 off. 1x samples.
  u32 fetch = 0;
  // The direct output (GPU-RENDER-PLAN B9; with the one-pass translucency): no resolve
  // subpass. The subpass's last draw turns the tile into the record in place, with the fog (lean_fog.frag:
  // [1] for frames that fog, [0] for the rest), and the colour attachment is stored and copied out as the record.
  // The colour attachment is the slot's record itself, through its normalised byte view (resn), so layer(),
  // the 1x record pass and edge marking read it as they do the resolved one. On above 2x:
  // the resolve subpass and its copy cost the G52 1-3 ms of a 4x frame on the probe's dumps
  // and 0.2-0.4 at 1x, where the depth attachment readable in the draw subpass costs frames with heavy opaque
  // overdraw more than that (Golden Sun: Dark Dawn's attract mode at 1x: 3.3 -> 5.3 ms).
  bool direct = false;
  // A multisampled frame resolved by the hardware, on the tile (where the device resolves depth; else the resolve subpass):
  // the draw subpass alone, its colour, fog flag and depth resolved into msr, fogr and dsr, and a pass over those
  // that writes the record (lean_msdec.frag). The resolve subpass reads every sample of three attachments through
  // input attachments, and with it in the render pass the G52 took 5.3 ms for a 2x frame with nothing drawn
  // (0.9 with the hardware's resolve); Spirit Tracks' 2x frame 8.2 -> 3.5 ms. The fog is then one depth's a pixel.
  bool mshw = false;
  Img msr, fogr, dsr;
  VkImageView dsr_depth_view = VK_NULL_HANDLE;
  VkRenderPass rp_hw[2][2] = {};   // [the id word kept][the fog flag and depth kept]: compatible, the stores differ
  VkRenderPass rp_dec = VK_NULL_HANDLE;
  VkFramebuffer fb_dec[kSlots] = {};
  VkShaderModule mod_dec = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_dec = VK_NULL_HANDLE;
  VkPipelineLayout layout_dec = VK_NULL_HANDLE;
  VkDescriptorPool pool_dec = VK_NULL_HANDLE;
  VkDescriptorSet set_dec[kSlots] = {};
  VkSampler samp_dec = VK_NULL_HANDLE;
  VkPipeline pipe_dec = VK_NULL_HANDLE;
  VkPipeline pipe_fog[2] = {};
  VkShaderModule mod_fog = VK_NULL_HANDLE;
  bool nostencil = false;   // DS_LEAN_NOSTENCIL=1: translucent pass B alone, always (debug)
  std::vector<std::pair<s64, u32>> key;   // the opaque prefix's sort keys (front to back, alpha-tested last)
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE, dsl_in = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set[kSlots] = {}, set_in[kSlots] = {};   // set_in: per slot, as the direct output's colour is the slot's record
  VkRenderPass rp = VK_NULL_HANDLE;   // the draw and resolve
  VkFramebuffer fb[kSlots] = {};
  // The direct output's frames that mark no edges (most) draw in a pass without the id attachment (rp_nm, fb_nm,
  // pipe_nm, pipe_fog_nm): a third colour attachment in the draw subpass costs the G52 2-5 ms of a 4x frame.
  bool split = false;
  VkRenderPass rp_nm = VK_NULL_HANDLE;
  VkFramebuffer fb_nm[kSlots] = {};
  VkPipeline pipe_nm[2][2][2][P_COUNT] = {}, pipe_fog_nm[2] = {};
  VkPipeline pipe[2][2][2][P_COUNT] = {};   // [toon: the frame has mode-2 polygons][wbuffer][depth-equal][kind]
  // The set without mode-2 shading ([0]) is built on a thread after create()
  // (the toon set draws any frame meanwhile), for the kinds that shade colour
  // only; a null entry falls back to the toon set's.
  VkBool32 draw_sc[4][6] = {}, shadow_sc[4][6] = {};   // [toon * 2 + a frame that marks no edges]
  VkSpecializationMapEntry draw_me[6] = {};
  VkSpecializationInfo draw_si[4] = {}, shadow_si[4] = {};
  // The tile's alpha byte is the record's (the 5-bit alpha itself, not the blend unit's (a + 1) * 8): the direct
  // output with the shader's own blend, where nothing blends by the byte. A frame that fogs nothing then has no
  // last draw at all (1.4-2 ms of the G52's 4x frame), and the shadows blend in the shader as the translucent
  // runs do (the DS's truncating blend). The pixel's fog flag is bit 7 of the same byte (the shaders keep its
  // MIN), so the pass has no fog attachment: 1-5 ms more. Without it: the blend unit's byte, the fog attachment
  // and the last draw in every frame.
  bool raw = false;
  // The upscaled raster draws a translucent polygon's pixels of alpha 31 in its blended pass (the one-pass stage keeps
  // them: blended at 31 the colour is the polygon's own), not in a pass of their own: that pass shaded the polygon a
  // second time for 3-10% of its pixels (3.5-6.7 ms of the Solatorobo flight's 4x frame on the G52, 4 of GS:DD's).
  // Not the DS's rule: such a pixel is left translucent, so a later translucent polygon of the same id is kept off it
  // and, without the polygon's depth write, one behind it is not. The pass stays for the polygons whose every pixel
  // is at 31 (DS_PF_A31_ALL).
  bool fold = false;
  std::thread notoon_build;
  std::atomic<bool> notoon_ready{false};
  VkPipeline pipe_resolve = VK_NULL_HANDLE;
  VkCommandPool cpool = VK_NULL_HANDLE;
  VkCommandBuffer cmd[kSlots] = {};
  std::mutex m;                   // the slot state below: submit() on the emulation thread, newest_ready() on the compositor's
  VkFence fence[kSlots] = {};
  bool fence_live[kSlots] = {};   // submitted, not yet known finished
  bool valid[kSlots] = {};        // holds a finished, invalidated frame
  u64 gen = 0;                    // frames submitted
  u64 slot_gen[kSlots] = {};      // the frame each slot holds (0: none, or being redrawn)
  u64 pinned[2] = {};             // pin(): frames whose slots stay out of the rotation
  int slot_of(u64 g) const { if (g) for (u32 i = 0; i < kSlots; ++i) if (slot_gen[i] == g) return static_cast<int>(i); return -1; }
  // The slot the next frame draws into: the oldest not pinned (with nothing pinned, the slots in turn).
  u32 next_slot() const {
    u32 best = 0; u64 bg = ~0ull;
    for (u32 i = 0; i < kSlots; ++i) { const u64 g = slot_gen[i]; if (g && (g == pinned[0] || g == pinned[1])) continue; if (g < bg) { bg = g; best = i; } }
    return best;
  }
  double submit_at[kSlots] = {};  // wall ms of each slot's submit (trace)
  u64 asked_gen = 0;              // the generation newest_ready() was last asked about (trace: the read gap)
  VkQueryPool qpool = VK_NULL_HANDLE;
  // DS_LEAN_OCC=1 (debug): an occlusion query per draw, summed per pass kind
  // (Lean's PassKind) and printed once; DS_LEAN_SKIP=hex: pass kinds left out.
  VkQueryPool occ_pool = VK_NULL_HANDLE;
  // Polygons drawn under a scissor of their own (Raster3D::scissor_buffer): a draw each, out of their run.
  static constexpr u32 kScissors = 16;
  Raster3D::Scissor sc[kScissors];
  u32 sc_n = 0;
  std::vector<u8> sc_of;          // per polygon of the list as submitted: its entry + 1, or 0
  static constexpr u32 kOccMax = 4096 + 64;
  std::vector<u8> occ_kind[4];       // per slot: the kind of each query
  bool occ_printed = false;
  double occ_area[11] = {};          // the frame's rasterised area per kind, 1x px (host vertices only)
  u64 last_gpu_ns = 0;
  u32 last_draws = 0;
  Lean::Stats st;

  void destroy() {
    if (!vk) return;
    const Api& a = *api;
    if (notoon_build.joinable()) notoon_build.join();
    for (u32 i = 0; i < kSlots; ++i) if (fence_live[i]) { a.vkWaitForFences(vk->dev, 1, &fence[i], VK_TRUE, ~0ull); fence_live[i] = false; }
    for (auto& tn : pipe) for (auto& wb : tn) for (auto& row : wb) for (auto& p : row) if (p) a.vkDestroyPipeline(vk->dev, p, nullptr);
    for (auto& tn : pipe_nm) for (auto& wb : tn) for (auto& row : wb) for (auto& p : row) if (p) a.vkDestroyPipeline(vk->dev, p, nullptr);
    for (VkPipeline p : pipe_fog_nm) if (p) a.vkDestroyPipeline(vk->dev, p, nullptr);
    for (VkFramebuffer f : fb_nm) if (f) a.vkDestroyFramebuffer(vk->dev, f, nullptr);
    if (rp_nm) a.vkDestroyRenderPass(vk->dev, rp_nm, nullptr);
    if (pipe_resolve) a.vkDestroyPipeline(vk->dev, pipe_resolve, nullptr);
    if (pipe_mark) a.vkDestroyPipeline(vk->dev, pipe_mark, nullptr);
    for (VkFramebuffer f : fb_mark) if (f) a.vkDestroyFramebuffer(vk->dev, f, nullptr);
    for (VkImageView v : resn) if (v) a.vkDestroyImageView(vk->dev, v, nullptr);
    if (rp_mark) a.vkDestroyRenderPass(vk->dev, rp_mark, nullptr);
    if (pipe_dec) a.vkDestroyPipeline(vk->dev, pipe_dec, nullptr);
    for (VkFramebuffer f : fb_dec) if (f) a.vkDestroyFramebuffer(vk->dev, f, nullptr);
    if (rp_dec) a.vkDestroyRenderPass(vk->dev, rp_dec, nullptr);
    if (mshw) { rp = VK_NULL_HANDLE; rp_keep = VK_NULL_HANDLE; }   // rp_hw's
    for (auto& r2 : rp_hw) for (VkRenderPass r : r2) if (r) a.vkDestroyRenderPass(vk->dev, r, nullptr);
    if (pool_dec) a.vkDestroyDescriptorPool(vk->dev, pool_dec, nullptr);
    if (layout_dec) a.vkDestroyPipelineLayout(vk->dev, layout_dec, nullptr);
    if (dsl_dec) a.vkDestroyDescriptorSetLayout(vk->dev, dsl_dec, nullptr);
    if (samp_dec) a.vkDestroySampler(vk->dev, samp_dec, nullptr);
    if (mod_dec) a.vkDestroyShaderModule(vk->dev, mod_dec, nullptr);
    if (dsr_depth_view) a.vkDestroyImageView(vk->dev, dsr_depth_view, nullptr);
    for (Img* im : {&msr, &fogr, &dsr}) { if (im->view) a.vkDestroyImageView(vk->dev, im->view, nullptr); if (im->img) a.vkDestroyImage(vk->dev, im->img, nullptr); if (im->mem) a.vkFreeMemory(vk->dev, im->mem, nullptr); }
    if (rp_keep) a.vkDestroyRenderPass(vk->dev, rp_keep, nullptr);
    if (layout_mark) a.vkDestroyPipelineLayout(vk->dev, layout_mark, nullptr);
    if (pool_mark) a.vkDestroyDescriptorPool(vk->dev, pool_mark, nullptr);
    if (dsl_mark) a.vkDestroyDescriptorSetLayout(vk->dev, dsl_mark, nullptr);
    if (samp_mark) a.vkDestroySampler(vk->dev, samp_mark, nullptr);
    if (mod_mark) a.vkDestroyShaderModule(vk->dev, mod_mark, nullptr);
    if (pipe_down) a.vkDestroyPipeline(vk->dev, pipe_down, nullptr);
    if (fb_down) a.vkDestroyFramebuffer(vk->dev, fb_down, nullptr);
    if (fb_down2) a.vkDestroyFramebuffer(vk->dev, fb_down2, nullptr);
    if (fence_rec) a.vkDestroyFence(vk->dev, fence_rec, nullptr);
    if (cpool_rec) a.vkDestroyCommandPool(vk->dev, cpool_rec, nullptr);
    if (rp_down) a.vkDestroyRenderPass(vk->dev, rp_down, nullptr);
    if (layout_down) a.vkDestroyPipelineLayout(vk->dev, layout_down, nullptr);
    if (pool_down) a.vkDestroyDescriptorPool(vk->dev, pool_down, nullptr);
    if (dsl_down) a.vkDestroyDescriptorSetLayout(vk->dev, dsl_down, nullptr);
    if (samp_down) a.vkDestroySampler(vk->dev, samp_down, nullptr);
    if (mod_down) a.vkDestroyShaderModule(vk->dev, mod_down, nullptr);
    for (VkPipeline p : pipe_fog) if (p) a.vkDestroyPipeline(vk->dev, p, nullptr);
    if (mod_fog) a.vkDestroyShaderModule(vk->dev, mod_fog, nullptr);
    for (VkFramebuffer f : fb) if (f) a.vkDestroyFramebuffer(vk->dev, f, nullptr);
    for (VkImageView v : res8) if (v) a.vkDestroyImageView(vk->dev, v, nullptr);
    if (rp) a.vkDestroyRenderPass(vk->dev, rp, nullptr);
    if (ds_depth_view) a.vkDestroyImageView(vk->dev, ds_depth_view, nullptr);
    for (Img* im : {&col_ms, &ds_ms, &fog_ms, &res[0], &res[1], &res[2], &res[3], &res[4], &res[5], &rec1, &rec1b, &idm, &idm_ms}) {
      if (im->view) a.vkDestroyImageView(vk->dev, im->view, nullptr);
      if (im->img) a.vkDestroyImage(vk->dev, im->img, nullptr);
      if (im->mem) a.vkFreeMemory(vk->dev, im->mem, nullptr);
    }
    if (qpool) a.vkDestroyQueryPool(vk->dev, qpool, nullptr);
    if (occ_pool) a.vkDestroyQueryPool(vk->dev, occ_pool, nullptr);
    for (VkFence f : fence) if (f) a.vkDestroyFence(vk->dev, f, nullptr);
    if (cpool) a.vkDestroyCommandPool(vk->dev, cpool, nullptr);
    if (pool) a.vkDestroyDescriptorPool(vk->dev, pool, nullptr);
    if (layout) a.vkDestroyPipelineLayout(vk->dev, layout, nullptr);
    if (dsl) a.vkDestroyDescriptorSetLayout(vk->dev, dsl, nullptr);
    if (dsl_in) a.vkDestroyDescriptorSetLayout(vk->dev, dsl_in, nullptr);
    for (VkShaderModule* m : {&mod_vert, &mod_frag, &mod_frag_early, &mod_frag_opq, &mod_frag_trans, &mod_frag_fetch, &mod_res, &mod_fs}) if (*m) a.vkDestroyShaderModule(vk->dev, *m, nullptr);
    if (texel_view) a.vkDestroyBufferView(vk->dev, texel_view, nullptr);
    if (texels) dev->free(texels);
    for (u32 i = 0; i < kSlots; ++i) for (Buffer* b : {&polys[i], &verts[i], &post[i], &out[i], &fine[i]}) if (*b) dev->free(*b);
  }
};

Lean::~Lean() { if (d_) d_->destroy(); }
bool Lean::msaa() const { return d_->msaa; }
u32 Lean::scale() const { return d_->scale; }
const u32* Lean::fine_for(const u32* o) const {
  for (u32 i = 0; i < Impl::kSlots; ++i) if (o == d_->out[i].ptr) return static_cast<const u32*>(d_->fine[i].ptr);
  return nullptr;
}
GpuPoly* Lean::poly_buffer() { return d_->host_polys.data(); }
GpuVert* Lean::vert_buffer() { return d_->host_verts.data(); }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = kTexelWords; return static_cast<u32*>(d_->texels.ptr); }
GpuPost* Lean::post_buffer() { return &d_->host_post; }
Raster3D::Scissor* Lean::scissor_buffer(u32* capacity) { *capacity = Impl::kScissors; return d_->sc; }
void Lean::scissor_count(u32 n) { d_->sc_n = std::min<u32>(n, Impl::kScissors); }
const u32* Lean::output() { return frame_ready(0, false); }
bool Lean::has_frame() const { return d_->gen != 0; }
u64 Lean::gpu_ns() const { return d_->last_gpu_ns; }
u32 Lean::draws() const { return d_->last_draws; }
Lean::Stats Lean::stats(bool reset) { std::lock_guard<std::mutex> lk(d_->m); Stats s = d_->st; if (reset) d_->st = Stats{}; return s; }
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::unique_ptr<Lean> Lean::create(std::shared_ptr<Device> devp, bool msaa, std::string* why, u32 scale) {
  auto fail_null = [&](const char* m) { if (why) *why = m; return nullptr; };
  if (!devp) return fail_null("no Vulkan context");
  Device& dev = *devp;
  const DeviceInternal* vk = dev.internal();
  if (!vk || !vk->api) return fail_null("no Vulkan context");
  const Api& a = *vk->api;
  if (!dev.limits().graphics || !a.vkCreateGraphicsPipelines || !a.vkCreateRenderPass || !a.vkCmdCopyImageToBuffer) return fail_null("no graphics queue");
  std::unique_ptr<Lean> self(new Lean);
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  d.vk = vk; d.api = &a; d.dev = &dev; d.keep = devp;
  auto fail = [&](const char* m) { if (why) *why = m; return nullptr; };
  // scale 0 (the probes): DS_LEAN_SCALE, and the record read back at that size.
  { const char* e = std::getenv("DS_LEAN_SCALE"); d.scale = std::clamp(scale ? scale : e ? static_cast<u32>(std::atoi(e)) : 1u, 1u, 4u); W = 256 * d.scale; H = 192 * d.scale; }
  d.down = scale > 1;
  d.mark_on = !(std::getenv("DS_LEAN_EDGEMARK") && std::atoi(std::getenv("DS_LEAN_EDGEMARK")) == 0);
  d.mark_width = d.scale;   // one DS pixel
  d.msaa = msaa && dev.limits().msaa4 && !(std::getenv("DS_LEAN_MSAA") && std::atoi(std::getenv("DS_LEAN_MSAA")) == 0);   // DS_LEAN_MSAA=0: off regardless (debug)
  const VkSampleCountFlagBits samples = d.msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
  if (const char* e = std::getenv("DS_LEAN_FETCH"); (!e || std::atoi(e) > 0) && !d.msaa && dev.limits().ordered_attachments) {   // on by default
    d.fetch = e ? static_cast<u32>(std::min(2, std::atoi(e))) : 2u;
    if (!dev.limits().float16) d.fetch = 2;   // the blend unit's copy needs fp16 arithmetic; else the DS blend
    d.direct = dev.limits().ordered_depth && d.scale >= 3;   // above 2x: see Impl::direct
  }
  d.mshw = d.msaa && dev.limits().depth_resolve;
  d.mark = true;   // edge marking (Impl::mark)

  // Buffers.
  d.host_polys.resize(DS_MAX_POLYS);
  d.host_verts.resize(DS_MAX_VERTS);
  d.texels = dev.alloc(sizeof(u32) * kTexelWords, Access::CpuWrite);
  if (!d.texels) return fail("buffer allocation");
  for (u32 i = 0; i < Impl::kSlots; ++i) {
    d.polys[i] = dev.alloc(sizeof(GpuPoly) * DS_MAX_POLYS, Access::CpuWrite);
    d.verts[i] = dev.alloc(sizeof(GpuVert) * DS_MAX_VERTS, Access::CpuWrite);
    d.post[i] = dev.alloc(sizeof(GpuPost), Access::CpuWrite);
    d.out[i] = dev.alloc(sizeof(u32) * (d.down ? 256u * 192u : W * H), Access::CpuRead);
    if (!d.polys[i] || !d.verts[i] || !d.post[i] || !d.out[i]) return fail("buffer allocation");
    static const bool want_fine = std::getenv("DS_LEAN_FINE") && std::atoi(std::getenv("DS_LEAN_FINE")) != 0;
    if (d.down && want_fine && !(d.fine[i] = dev.alloc(sizeof(u32) * W * H, Access::CpuRead))) return fail("buffer allocation");
  }
  {
    VkBufferViewCreateInfo bv{}; bv.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO; bv.buffer = vk_buf(d.texels); bv.format = VK_FORMAT_R32_UINT; bv.offset = 0; bv.range = VK_WHOLE_SIZE;
    if (a.vkCreateBufferView(vk->dev, &bv, nullptr, &d.texel_view) != VK_SUCCESS) return fail("texel buffer view");
  }

  // Shaders.
  auto shader = [&](Spirv s, VkShaderModule* out) {
    VkShaderModuleCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = s.bytes; ci.pCode = s.code;
    return a.vkCreateShaderModule(vk->dev, &ci, nullptr, out) == VK_SUCCESS;
  };
  if (!shader(shader_lean_vert(), &d.mod_vert) || !shader(shader_lean_frag(), &d.mod_frag) || !shader(shader_lean_frag_early(), &d.mod_frag_early) || !shader(shader_lean_frag_opq(), &d.mod_frag_opq) || !shader(shader_lean_frag_trans(), &d.mod_frag_trans) || !shader(d.msaa ? shader_lean_resolve() : shader_lean_resolve_1x(), &d.mod_res) || !shader(shader_tri_fs(), &d.mod_fs))
    return fail("lean shaders rejected by the driver");
  d.raw = d.direct && d.fetch == 2 && !d.msaa;
  d.fold = d.raw;
  if (d.fetch && !shader(shader_lean_frag_fetch(d.fetch == 2), &d.mod_frag_fetch)) return fail("lean fetch shader rejected by the driver");
  if (d.direct && !shader(shader_lean_fog(), &d.mod_fog)) return fail("lean fog shader rejected by the driver");
  d.nostencil = std::getenv("DS_LEAN_NOSTENCIL") && std::atoi(std::getenv("DS_LEAN_NOSTENCIL")) != 0;

  // Descriptors: set 0 = polys, verts, texels (texel buffer), post; set 1 = the MS colour as input attachment.
  {
    VkDescriptorSetLayoutBinding b[4]{};
    for (u32 i = 0; i < 4; ++i) { b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; b[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    VkDescriptorSetLayoutCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci.bindingCount = 4; ci.pBindings = b;
    if (a.vkCreateDescriptorSetLayout(vk->dev, &ci, nullptr, &d.dsl) != VK_SUCCESS) return fail("descriptor layout");
    VkDescriptorSetLayoutBinding ib[4]{};
    for (u32 i = 0; i < 4; ++i) { ib[i].binding = i; ib[i].descriptorCount = 1; ib[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; ib[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; }
    VkDescriptorSetLayoutCreateInfo ci2{}; ci2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; ci2.bindingCount = 4; ci2.pBindings = ib;   // 3: the colour read back by subpass 0 (fetch)
    if (a.vkCreateDescriptorSetLayout(vk->dev, &ci2, nullptr, &d.dsl_in) != VK_SUCCESS) return fail("input attachment layout");
    VkDescriptorSetLayout sets[2] = {d.dsl, d.dsl_in};
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = sizeof(GpuFrame);
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 2; pli.pSetLayouts = sets; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout) != VK_SUCCESS) return fail("pipeline layout");
    VkDescriptorPoolSize psz[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * Impl::kSlots}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, Impl::kSlots}, {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 4 * Impl::kSlots}};
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = 2 * Impl::kSlots; dpi.poolSizeCount = 3; dpi.pPoolSizes = psz;
    if (a.vkCreateDescriptorPool(vk->dev, &dpi, nullptr, &d.pool) != VK_SUCCESS) return fail("descriptor pool");
    VkDescriptorSetLayout layouts[2 * Impl::kSlots];
    for (u32 i = 0; i < Impl::kSlots; ++i) { layouts[i] = d.dsl; layouts[Impl::kSlots + i] = d.dsl_in; }
    VkDescriptorSet got[2 * Impl::kSlots];
    VkDescriptorSetAllocateInfo dsa{}; dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa.descriptorPool = d.pool; dsa.descriptorSetCount = 2 * Impl::kSlots; dsa.pSetLayouts = layouts;
    if (a.vkAllocateDescriptorSets(vk->dev, &dsa, got) != VK_SUCCESS) return fail("descriptor sets");
    for (u32 i = 0; i < Impl::kSlots; ++i) { d.set[i] = got[i]; d.set_in[i] = got[Impl::kSlots + i]; }
    for (u32 sl = 0; sl < Impl::kSlots; ++sl) {
      VkDescriptorBufferInfo bi[4]{};
      const Buffer* src[4] = {&d.polys[sl], &d.verts[sl], &d.texels, &d.post[sl]};
      VkWriteDescriptorSet w[4]{};
      for (u32 i = 0; i < 4; ++i) {
        bi[i].buffer = vk_buf(*src[i]); bi[i].range = VK_WHOLE_SIZE;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = d.set[sl]; w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
      }
      w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; w[2].pBufferInfo = nullptr; w[2].pTexelBufferView = &d.texel_view;
      a.vkUpdateDescriptorSets(vk->dev, 4, w, 0, nullptr);
    }
  }


  // Images.
  VkPhysicalDeviceMemoryProperties mp{}; a.vkGetPhysicalDeviceMemoryProperties(vk->phys, &mp);
  auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, Impl::Img& o, VkSampleCountFlagBits smp, VkImageCreateFlags flags = 0, u32 div = 1) {
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.flags = flags; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {W / div, H / div, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = smp; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a.vkCreateImage(vk->dev, &ii, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{}; a.vkGetImageMemoryRequirements(vk->dev, o.img, &mr);
    u32 type = ~0u;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
      for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) type = t;
    for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if (mr.memoryTypeBits & (1u << t)) type = t;
    if (type == ~0u) return false;
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    if (a.vkAllocateMemory(vk->dev, &ai, nullptr, &o.mem) != VK_SUCCESS) return false;
    if (a.vkBindImageMemory(vk->dev, o.img, o.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return a.vkCreateImageView(vk->dev, &vi, nullptr, &o.view) == VK_SUCCESS;
  };
  {
    VkFormatProperties fp{};
    // Float depth first: W-buffer mode stores 1/w, and at DS depths of tens
    // of thousands neighbouring values are 1e-9 apart, below a 24-bit fixed
    // step (6e-8), so a shadow volume's face and the ground it meets compared
    // equal. A float keeps the relative precision. DS_LEAN_D24=1 forces D24S8.
    a.vkGetPhysicalDeviceFormatProperties(vk->phys, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    const bool d32 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(std::getenv("DS_LEAN_D24") && std::atoi(std::getenv("DS_LEAN_D24")) != 0);
    d.ds_format = d32 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
  }
  const VkImageUsageFlags transient = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_COLOR_BIT, d.col_ms, samples)) return fail("MSAA colour attachment");
  if (!make_image(d.ds_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, d.ds_ms, samples)) return fail("MSAA depth/stencil attachment");
  {
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = d.ds_ms.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = d.ds_format; vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (a.vkCreateImageView(vk->dev, &vi, nullptr, &d.ds_depth_view) != VK_SUCCESS) return fail("depth input view");
  }
  if (!make_image(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_COLOR_BIT, d.fog_ms, samples)) return fail("MSAA fog attachment");
  static_assert(Impl::kSlots == 6, "the destructor lists the record images");
  for (u32 i = 0; i < Impl::kSlots; ++i) {
    // The direct output draws into the record itself, through its normalised byte view (resn).
    const bool sampled = true;
    if (!make_image(VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VkImageUsageFlags{VK_IMAGE_USAGE_SAMPLED_BIT} : 0u) | (d.direct ? VkImageUsageFlags{VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT} : 0u), VK_IMAGE_ASPECT_COLOR_BIT, d.res[i], VK_SAMPLE_COUNT_1_BIT,
                    sampled ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0u)) return fail("resolve attachment");
    if (!sampled) continue;
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = d.res[i].img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UINT; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (a.vkCreateImageView(vk->dev, &vi, nullptr, &d.res8[i]) != VK_SUCCESS) return fail("record byte view");
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    if (a.vkCreateImageView(vk->dev, &vi, nullptr, &d.resn[i]) != VK_SUCCESS) return fail("record normalised view");
  }
  if (d.down) {
    // The 1x record's pass: rec1 its one attachment, the frame's record image sampled.
    if (!d.res8[0]) return fail("a scale above 1 needs the record as an image (not the direct output)");
    if (!make_image(VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.rec1, VK_SAMPLE_COUNT_1_BIT, 0, d.scale)) return fail("1x record image");
    if (!shader(shader_lean_down(), &d.mod_down)) return fail("1x record shader");
    VkAttachmentDescription at{}; at.format = VK_FORMAT_R32_UINT; at.samples = VK_SAMPLE_COUNT_1_BIT; at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    // The copy into the slot's buffer reads what this pass wrote, and the pass draws over what the frame before
    // copied: both said here. Without them (the implicit dependencies order nothing against a transfer) the
    // Mali copied the image while the pass was still drawing it wherever the raster ran long, and the record
    // came back with pieces of the frame before: in GS: Dark Dawn the other screen's picture, in blocks.
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 2; rpi.pDependencies = dep;
    if (a.vkCreateRenderPass(vk->dev, &rpi, nullptr, &d.rp_down) != VK_SUCCESS) return fail("1x record render pass");
    VkFramebufferCreateInfo fbi{}; fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fbi.renderPass = d.rp_down; fbi.attachmentCount = 1; fbi.pAttachments = &d.rec1.view; fbi.width = 256; fbi.height = 192; fbi.layers = 1;
    if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb_down) != VK_SUCCESS) return fail("1x record framebuffer");
    d.lazy_rec = true;
    if (d.lazy_rec) {
      if (!make_image(VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.rec1b, VK_SAMPLE_COUNT_1_BIT, 0, d.scale)) return fail("1x record image (late)");
      fbi.pAttachments = &d.rec1b.view;
      if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb_down2) != VK_SUCCESS) return fail("1x record framebuffer (late)");
      VkCommandPoolCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = vk->qfam;
      if (a.vkCreateCommandPool(vk->dev, &cpi, nullptr, &d.cpool_rec) != VK_SUCCESS) return fail("command pool (late record)");
      VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cai.commandPool = d.cpool_rec; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
      if (a.vkAllocateCommandBuffers(vk->dev, &cai, &d.cmd_rec) != VK_SUCCESS) return fail("command buffer (late record)");
      VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (a.vkCreateFence(vk->dev, &fi, nullptr, &d.fence_rec) != VK_SUCCESS) return fail("fence (late record)");
    }
    VkSamplerCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (a.vkCreateSampler(vk->dev, &sci, nullptr, &d.samp_down) != VK_SUCCESS) return fail("1x record sampler");
    VkDescriptorSetLayoutBinding lb{}; lb.binding = 0; lb.descriptorCount = 1; lb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{}; lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lci.bindingCount = 1; lci.pBindings = &lb;
    if (a.vkCreateDescriptorSetLayout(vk->dev, &lci, nullptr, &d.dsl_down) != VK_SUCCESS) return fail("1x record layout");
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = sizeof(u32);
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 1; pli.pSetLayouts = &d.dsl_down; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout_down) != VK_SUCCESS) return fail("1x record pipeline layout");
    const VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Impl::kSlots};
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = Impl::kSlots; dpi.poolSizeCount = 1; dpi.pPoolSizes = &psz;
    if (a.vkCreateDescriptorPool(vk->dev, &dpi, nullptr, &d.pool_down) != VK_SUCCESS) return fail("1x record descriptor pool");
    for (u32 i = 0; i < Impl::kSlots; ++i) {
      VkDescriptorSetAllocateInfo dsa{}; dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa.descriptorPool = d.pool_down; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &d.dsl_down;
      if (a.vkAllocateDescriptorSets(vk->dev, &dsa, &d.set_down[i]) != VK_SUCCESS) return fail("1x record descriptor set");
      const VkDescriptorImageInfo ii{d.samp_down, d.res[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = d.set_down[i]; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
      a.vkUpdateDescriptorSets(vk->dev, 1, &w, 0, nullptr);
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.mod_fs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d.mod_down; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, 256.f, 192.f, 0.f, 1.f}; VkRect2D sc{{0, 0}, {256, 192}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs; gpi.layout = d.layout_down; gpi.renderPass = d.rp_down; gpi.subpass = 0;
    if (a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, &d.pipe_down) != VK_SUCCESS) return fail("1x record pipeline");
  }
  if (!d.res8[0]) d.mark = false;   // no byte view of the record
  if (d.mark) {
    // Edge marking: the id word's image (a colour attachment of the draw, sampled by the marking pass), the
    // marking pass's render pass over each slot's record (loaded and stored, in the layout the main pass
    // leaves it and the copy-out expects), and its one pipeline.
    if (!make_image(kIdFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.idm, VK_SAMPLE_COUNT_1_BIT)) return fail("edge marking image");
    if (d.msaa && !make_image(kIdFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | transient, VK_IMAGE_ASPECT_COLOR_BIT, d.idm_ms, samples)) return fail("edge marking MSAA image");
    if (!shader(shader_lean_mark(), &d.mod_mark)) return fail("edge marking shader");
    VkAttachmentDescription at{}; at.format = VK_FORMAT_R8G8B8A8_UNORM; at.samples = VK_SAMPLE_COUNT_1_BIT; at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // the record through its byte view: the blend unit mixes
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0; dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL; dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 2; rpi.pDependencies = dep;
    if (a.vkCreateRenderPass(vk->dev, &rpi, nullptr, &d.rp_mark) != VK_SUCCESS) return fail("edge marking render pass");
    VkSamplerCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (a.vkCreateSampler(vk->dev, &sci, nullptr, &d.samp_mark) != VK_SUCCESS) return fail("edge marking sampler");
    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0].binding = 0; lb[0].descriptorCount = 1; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; lb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lb[1].binding = 1; lb[1].descriptorCount = 1; lb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{}; lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lci.bindingCount = 2; lci.pBindings = lb;
    if (a.vkCreateDescriptorSetLayout(vk->dev, &lci, nullptr, &d.dsl_mark) != VK_SUCCESS) return fail("edge marking layout");
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = 3 * sizeof(u32);
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 1; pli.pSetLayouts = &d.dsl_mark; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout_mark) != VK_SUCCESS) return fail("edge marking pipeline layout");
    const VkDescriptorPoolSize psz[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Impl::kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Impl::kSlots}};
    VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = Impl::kSlots; dpi.poolSizeCount = 2; dpi.pPoolSizes = psz;
    if (a.vkCreateDescriptorPool(vk->dev, &dpi, nullptr, &d.pool_mark) != VK_SUCCESS) return fail("edge marking descriptor pool");
    for (u32 i = 0; i < Impl::kSlots; ++i) {
      VkFramebufferCreateInfo fbi{}; fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fbi.renderPass = d.rp_mark; fbi.attachmentCount = 1; fbi.pAttachments = &d.resn[i]; fbi.width = W; fbi.height = H; fbi.layers = 1;
      if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb_mark[i]) != VK_SUCCESS) return fail("edge marking framebuffer");
      VkDescriptorSetAllocateInfo dsa{}; dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa.descriptorPool = d.pool_mark; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &d.dsl_mark;
      if (a.vkAllocateDescriptorSets(vk->dev, &dsa, &d.set_mark[i]) != VK_SUCCESS) return fail("edge marking descriptor set");
      const VkDescriptorImageInfo ii{d.samp_mark, d.idm.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      const VkDescriptorBufferInfo bi{vk_buf(d.post[i]), 0, sizeof(GpuPost)};
      VkWriteDescriptorSet w[2]{};
      w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = d.set_mark[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
      w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = d.set_mark[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bi;
      a.vkUpdateDescriptorSets(vk->dev, 2, w, 0, nullptr);
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.mod_fs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d.mod_mark; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0x7u;   // the colour bytes; the alpha byte stays the pixel's
    cba.blendEnable = VK_TRUE; cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs; gpi.layout = d.layout_mark; gpi.renderPass = d.rp_mark; gpi.subpass = 0;
    if (a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, &d.pipe_mark) != VK_SUCCESS) return fail("edge marking pipeline");
  }
  {
    for (u32 sl = 0; sl < Impl::kSlots; ++sl) {
      const VkImageView colv = d.direct ? d.resn[sl] : d.col_ms.view;
      VkDescriptorImageInfo di[3]{};
      di[0].imageView = colv; di[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      di[1].imageView = d.ds_depth_view; di[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      di[2].imageView = d.fog_ms.view; di[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (d.direct) di[0].imageLayout = di[1].imageLayout = di[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // read by the subpass that draws them
      VkWriteDescriptorSet w[3]{};
      for (u32 i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = d.set_in[sl]; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; w[i].pImageInfo = &di[i]; }
      a.vkUpdateDescriptorSets(vk->dev, 3, w, 0, nullptr);
      if (d.fetch) {
        VkDescriptorImageInfo fi{}; fi.imageView = colv; fi.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet fw = w[0]; fw.dstBinding = 3; fw.pImageInfo = &fi;
        a.vkUpdateDescriptorSets(vk->dev, 1, &fw, 0, nullptr);
      }
    }
  }

  // Render pass: subpass 0 draws into the MS colour + depth/stencil, subpass 1 resolves into the 1x record.
  {
    VkAttachmentDescription at[6]{};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM; at[0].samples = samples; at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].format = d.ds_format; at[1].samples = samples; at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    at[2].format = VK_FORMAT_R32_UINT; at[2].samples = VK_SAMPLE_COUNT_1_BIT; at[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    at[3] = at[0]; at[3].format = VK_FORMAT_R8_UNORM;   // the fog flag: cleared to the rear plane's, never stored
    // Edge marking: 4 the id / fog bit / depth word, cleared to the rear plane's; stored by rp_keep alone.
    // Under MSAA 4 is multisampled and never stored, and 5 its resolve (an integer format: one sample's word), the one kept.
    const bool mark_ms = d.mark && d.msaa;
    const u32 mark_at = mark_ms ? 5u : 4u;   // the attachment the marking pass samples
    if (d.mark) { at[4] = at[3]; at[4].format = kIdFormat; at[4].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[4].samples = d.msaa ? samples : VK_SAMPLE_COUNT_1_BIT; }
    if (mark_ms) { at[5] = at[4]; at[5].samples = VK_SAMPLE_COUNT_1_BIT; at[5].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; }
    const VkAttachmentReference rs0[3] = {{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}, {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}, {5, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference c0[3] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}, ds0{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference in1[3] = {{0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}, {3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkAttachmentReference c1{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp[2]{};
    sp[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp[0].colorAttachmentCount = d.mark ? 3 : 2; sp[0].pColorAttachments = c0; sp[0].pDepthStencilAttachment = &ds0;
    if (mark_ms) sp[0].pResolveAttachments = rs0;
    const VkAttachmentReference in0{0, VK_IMAGE_LAYOUT_GENERAL};
    if (d.fetch) {   // the colour is read back by its own subpass, in rasterisation order
      c0[0].layout = VK_IMAGE_LAYOUT_GENERAL;
      sp[0].inputAttachmentCount = 1; sp[0].pInputAttachments = &in0; sp[0].flags = VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT;
    }
    sp[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp[1].inputAttachmentCount = 3; sp[1].pInputAttachments = in1; sp[1].colorAttachmentCount = 1; sp[1].pColorAttachments = &c1;
    VkSubpassDependency dep[3]{};
    dep[0].srcSubpass = 0; dep[0].dstSubpass = 1; dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dep[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT; dep[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dep[1].srcSubpass = 1; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL; dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    // rp_keep (edge marking): the draw's id word to the marking pass's render pass.
    dep[2] = dep[0]; dep[2].dstSubpass = VK_SUBPASS_EXTERNAL; dep[2].dependencyFlags = 0;
    VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpi.attachmentCount = mark_ms ? 6 : d.mark ? 5 : 4; rpi.pAttachments = at; rpi.subpassCount = 2; rpi.pSubpasses = sp; rpi.dependencyCount = 2; rpi.pDependencies = dep;
    const VkAttachmentReference in0d[3] = {{0, VK_IMAGE_LAYOUT_GENERAL}, {1, VK_IMAGE_LAYOUT_GENERAL}, {3, VK_IMAGE_LAYOUT_GENERAL}};
    if (d.direct) {
      // One subpass: the colour stored as the output, the record attachment unused; depth and the fog flag are
      // read back in rasterisation order by the fog draw, as the colour is by it and by the translucent runs.
      at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE; at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      at[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL; at[3].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      at[2].format = VK_FORMAT_R8G8B8A8_UNORM; at[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[2].finalLayout = VK_IMAGE_LAYOUT_GENERAL;   // unused: the spare colour image stands in
      c0[1].layout = VK_IMAGE_LAYOUT_GENERAL; ds0.layout = VK_IMAGE_LAYOUT_GENERAL;
      sp[0].inputAttachmentCount = 3; sp[0].pInputAttachments = in0d;
      sp[0].flags = VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT | VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT;
      rpi.subpassCount = 1; dep[0] = dep[1]; dep[0].srcSubpass = 0; rpi.dependencyCount = 1;
      if (d.raw) { c0[1].attachment = VK_ATTACHMENT_UNUSED; sp[0].inputAttachmentCount = 2; }   // the fog flag rides in the colour's alpha byte
    }
    if (d.mshw) {
      // One subpass: 0 colour, 1 depth/stencil, 3 the fog flag, 4 the id word, all multisampled and never stored;
      // their resolves 2 (msr), 6 (fogr), 5 (idm: sample 0's word) and 7 (dsr: sample 0's depth).
      if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.msr, VK_SAMPLE_COUNT_1_BIT) ||
          !make_image(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.fogr, VK_SAMPLE_COUNT_1_BIT) ||
          !make_image(d.ds_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, d.dsr, VK_SAMPLE_COUNT_1_BIT)) return fail("resolve images");
      { VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = d.dsr.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = d.ds_format; vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (a.vkCreateImageView(vk->dev, &vi, nullptr, &d.dsr_depth_view) != VK_SUCCESS) return fail("resolved depth view"); }
      for (u32 keep = 0; keep < 2; ++keep) for (u32 fog = 0; fog < 2; ++fog) {
        VkAttachmentDescription2 t[8]{};
        for (auto& x : t) { x.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2; x.samples = VK_SAMPLE_COUNT_1_BIT; x.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; x.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
          x.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; x.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; x.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; x.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
        t[0].format = VK_FORMAT_R8G8B8A8_UNORM; t[0].samples = samples; t[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; t[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        t[1].format = d.ds_format; t[1].samples = samples; t[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; t[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; t[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        t[2].format = VK_FORMAT_R8G8B8A8_UNORM; t[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        t[3] = t[0]; t[3].format = VK_FORMAT_R8_UNORM;
        t[4] = t[0]; t[4].format = kIdFormat;
        t[5].format = kIdFormat; t[5].storeOp = keep ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        t[6].format = VK_FORMAT_R8_UNORM; t[6].storeOp = fog ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        t[7].format = d.ds_format; t[7].storeOp = t[6].storeOp; t[7].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        auto ref = [](u32 att, VkImageLayout l, VkImageAspectFlags as) { VkAttachmentReference2 r{}; r.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2; r.attachment = att; r.layout = l; r.aspectMask = as; return r; };
        const VkImageLayout cl = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dl = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkImageAspectFlags ca = VK_IMAGE_ASPECT_COLOR_BIT, da = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        const VkAttachmentReference2 col[3] = {ref(0, cl, ca), ref(3, cl, ca), ref(4, cl, ca)}, res[3] = {ref(2, cl, ca), ref(6, cl, ca), ref(5, cl, ca)};
        const VkAttachmentReference2 ds = ref(1, dl, da), dsres = ref(7, dl, da);
        VkSubpassDescriptionDepthStencilResolve dr{}; dr.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
        dr.depthResolveMode = dr.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; dr.pDepthStencilResolveAttachment = &dsres;
        VkSubpassDescription2 s2{}; s2.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2; s2.pNext = &dr; s2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        s2.colorAttachmentCount = 3; s2.pColorAttachments = col; s2.pResolveAttachments = res; s2.pDepthStencilAttachment = &ds;
        VkSubpassDependency2 dp{}; dp.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2; dp.srcSubpass = 0; dp.dstSubpass = VK_SUBPASS_EXTERNAL;
        dp.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; dp.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dp.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dp.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo2 r2{}; r2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2; r2.attachmentCount = 8; r2.pAttachments = t; r2.subpassCount = 1; r2.pSubpasses = &s2; r2.dependencyCount = 1; r2.pDependencies = &dp;
        if (a.vkCreateRenderPass2KHR(vk->dev, &r2, nullptr, &d.rp_hw[keep][fog]) != VK_SUCCESS) return fail("render pass (hardware resolve)");
      }
      d.rp = d.rp_hw[0][0]; d.rp_keep = d.rp_hw[1][0];
    } else
    if (a.vkCreateRenderPass(vk->dev, &rpi, nullptr, &d.rp) != VK_SUCCESS) return fail("render pass");
    d.split = d.direct && d.mark && !d.msaa;
    if (d.split) {
      VkRenderPassCreateInfo r2 = rpi; VkSubpassDescription s2 = sp[0]; s2.colorAttachmentCount = 2; r2.attachmentCount = 4; r2.pSubpasses = &s2;
      if (a.vkCreateRenderPass(vk->dev, &r2, nullptr, &d.rp_nm) != VK_SUCCESS) return fail("render pass (no id attachment)");
    }
    if (d.mark && !d.mshw) {   // the same pass keeping the id word for the marking pass (compatible: only the store differs)
      at[mark_at].storeOp = VK_ATTACHMENT_STORE_OP_STORE; rpi.dependencyCount = 3;
      if (a.vkCreateRenderPass(vk->dev, &rpi, nullptr, &d.rp_keep) != VK_SUCCESS) return fail("render pass (edge marking)");
    }
    for (u32 i = 0; i < Impl::kSlots; ++i) {
      VkImageView views[6] = {d.direct ? d.resn[i] : d.col_ms.view, d.ds_ms.view, d.direct ? d.col_ms.view : d.res[i].view, d.fog_ms.view, mark_ms ? d.idm_ms.view : d.idm.view, d.idm.view};
      VkFramebufferCreateInfo fbi{}; fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fbi.renderPass = d.rp; fbi.attachmentCount = mark_ms ? 6 : d.mark ? 5 : 4; fbi.pAttachments = views; fbi.width = W; fbi.height = H; fbi.layers = 1;
      const VkImageView views_hw[8] = {d.col_ms.view, d.ds_ms.view, d.msr.view, d.fog_ms.view, d.idm_ms.view, d.idm.view, d.fogr.view, d.dsr.view};
      if (d.mshw) { fbi.attachmentCount = 8; fbi.pAttachments = views_hw; }
      if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb[i]) != VK_SUCCESS) return fail("framebuffer");
      if (d.split) { fbi.renderPass = d.rp_nm; fbi.attachmentCount = 4; if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb_nm[i]) != VK_SUCCESS) return fail("framebuffer (no id attachment)"); }
    }
    if (d.mshw) {
      // The pass that writes the record from the resolved images (lean_msdec.frag): each slot's record its one attachment.
      if (!shader(shader_lean_msdec(), &d.mod_dec)) return fail("resolve decode shader");
      VkAttachmentDescription t{}; t.format = VK_FORMAT_R32_UINT; t.samples = VK_SAMPLE_COUNT_1_BIT; t.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; t.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      t.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; t.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; t.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; t.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      const VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription s1{}; s1.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; s1.colorAttachmentCount = 1; s1.pColorAttachments = &ar;
      VkSubpassDependency dd[2]{};   // after whatever read the record last, before what reads it next (as the 1x record's pass)
      dd[0].srcSubpass = VK_SUBPASS_EXTERNAL; dd[0].dstSubpass = 0;
      dd[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; dd[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dd[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT; dd[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      dd[1].srcSubpass = 0; dd[1].dstSubpass = VK_SUBPASS_EXTERNAL;
      dd[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dd[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dd[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dd[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo ri{}; ri.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; ri.attachmentCount = 1; ri.pAttachments = &t; ri.subpassCount = 1; ri.pSubpasses = &s1; ri.dependencyCount = 2; ri.pDependencies = dd;
      if (a.vkCreateRenderPass(vk->dev, &ri, nullptr, &d.rp_dec) != VK_SUCCESS) return fail("resolve decode render pass");
      VkSamplerCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      if (a.vkCreateSampler(vk->dev, &sci, nullptr, &d.samp_dec) != VK_SUCCESS) return fail("resolve decode sampler");
      VkDescriptorSetLayoutBinding lb[4]{};
      for (u32 k = 0; k < 4; ++k) { lb[k].binding = k; lb[k].descriptorCount = 1; lb[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
      VkDescriptorSetLayoutCreateInfo lci{}; lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; lci.bindingCount = 4; lci.pBindings = lb;
      if (a.vkCreateDescriptorSetLayout(vk->dev, &lci, nullptr, &d.dsl_dec) != VK_SUCCESS) return fail("resolve decode layout");
      VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = sizeof(GpuFrame);
      VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 1; pli.pSetLayouts = &d.dsl_dec; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
      if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout_dec) != VK_SUCCESS) return fail("resolve decode pipeline layout");
      const VkDescriptorPoolSize psz[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * Impl::kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Impl::kSlots}};
      VkDescriptorPoolCreateInfo dpi{}; dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = Impl::kSlots; dpi.poolSizeCount = 2; dpi.pPoolSizes = psz;
      if (a.vkCreateDescriptorPool(vk->dev, &dpi, nullptr, &d.pool_dec) != VK_SUCCESS) return fail("resolve decode descriptor pool");
      for (u32 i = 0; i < Impl::kSlots; ++i) {
        VkFramebufferCreateInfo fbi{}; fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fbi.renderPass = d.rp_dec; fbi.attachmentCount = 1; fbi.pAttachments = &d.res[i].view; fbi.width = W; fbi.height = H; fbi.layers = 1;
        if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb_dec[i]) != VK_SUCCESS) return fail("resolve decode framebuffer");
        VkDescriptorSetAllocateInfo dsa{}; dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa.descriptorPool = d.pool_dec; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &d.dsl_dec;
        if (a.vkAllocateDescriptorSets(vk->dev, &dsa, &d.set_dec[i]) != VK_SUCCESS) return fail("resolve decode descriptor set");
        const VkDescriptorImageInfo im[3] = {{d.samp_dec, d.msr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {d.samp_dec, d.fogr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {d.samp_dec, d.dsr_depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
        const VkDescriptorBufferInfo bi{vk_buf(d.post[i]), 0, sizeof(GpuPost)};
        VkWriteDescriptorSet w[4]{};
        for (u32 k = 0; k < 4; ++k) { w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = d.set_dec[i]; w[k].dstBinding = k; w[k].descriptorCount = 1;
          w[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; if (k < 3) w[k].pImageInfo = &im[k]; else w[k].pBufferInfo = &bi; }
        a.vkUpdateDescriptorSets(vk->dev, 4, w, 0, nullptr);
      }
      VkPipelineShaderStageCreateInfo st[2]{};
      st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.mod_fs; st[0].pName = "main";
      st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d.mod_dec; st[1].pName = "main";
      VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
      VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
      VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
      VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xFu;
      VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
      VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = 2; gpi.pStages = st;
      gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
      gpi.pColorBlendState = &cbs; gpi.layout = d.layout_dec; gpi.renderPass = d.rp_dec; gpi.subpass = 0;
      if (a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, &d.pipe_dec) != VK_SUCCESS) return fail("resolve decode pipeline");
    }
  }

  // (constant_id 0 was the span table's; the entry stays so the others keep their places.)
  // The draw's stages also take constant_id 2, ds_toon (ds_shade.glsl DS_TOON): a
  // second pipeline set without mode-2 shading, for the frames that have none.
  // And constant_id 4, ds_raw (Impl::raw), and 5, ds_keep31: the one-pass translucent stage drawing a shadow keeps
  // the pixels of alpha 31 too (shadow_si).
  // And constant_id 6, ds_mark: false in the pipelines of a frame that marks no edges (pipe_nm), whose stages then
  // carry none of the id word's code.
  for (u32 tn = 0; tn < 4; ++tn) {
    d.draw_sc[tn][0] = VK_FALSE; d.draw_sc[tn][1] = (tn & 2) ? VK_TRUE : VK_FALSE; d.draw_sc[tn][2] = d.raw ? VK_TRUE : VK_FALSE; d.draw_sc[tn][3] = VK_FALSE;
    d.draw_sc[tn][4] = (tn & 1) ? VK_FALSE : VK_TRUE;
    d.draw_sc[tn][5] = d.mshw ? VK_TRUE : VK_FALSE;
    d.draw_si[tn] = {6, d.draw_me, sizeof d.draw_sc[tn], d.draw_sc[tn]};
    std::memcpy(d.shadow_sc[tn], d.draw_sc[tn], sizeof d.draw_sc[tn]); d.shadow_sc[tn][3] = VK_TRUE;
    d.shadow_si[tn] = {6, d.draw_me, sizeof d.shadow_sc[tn], d.shadow_sc[tn]};
  }
  d.draw_me[0] = {0, 0, sizeof(VkBool32)}; d.draw_me[1] = {2, sizeof(VkBool32), sizeof(VkBool32)};
  d.draw_me[2] = {4, 2 * sizeof(VkBool32), sizeof(VkBool32)}; d.draw_me[3] = {5, 3 * sizeof(VkBool32), sizeof(VkBool32)};
  d.draw_me[4] = {6, 4 * sizeof(VkBool32), sizeof(VkBool32)}; d.draw_me[5] = {7, 5 * sizeof(VkBool32), sizeof(VkBool32)};
  // DS_MSAA_SHARP=0..1: the 4x resolve's pull towards each edge pixel's
  // majority surface (lean_resolve.frag), constant_id 1 of the resolve; 0 the
  // plain coverage mean.
  const float msaa_sharp = std::getenv("DS_MSAA_SHARP") ? std::clamp(static_cast<float>(std::atof(std::getenv("DS_MSAA_SHARP"))), 0.0f, 1.0f) : 0.0f;
  const VkSpecializationMapEntry sharp_me{1, 0, sizeof(float)};
  const VkSpecializationInfo sharp_si{1, &sharp_me, sizeof(float), &msaa_sharp};
  // Pipelines: one vertex/fragment pair, the DS rules in depth, stencil and blend state.
  // Built through the device's pipeline cache (~7 s from SPIR-V on the Mali
  // blob, a fraction from the cache), which is written back once they exist.
  const auto t_pipes = std::chrono::steady_clock::now();
  // Explicit captures: the no-toon set is built by a thread that outlives this function.
  auto pipeline = [&d, &a, vk, samples](bool toon, bool wbuf, bool eq, u32 kind, VkPipeline* out, bool nm = false) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // The mask clear is a full-screen triangle; it and the shadow prep touch the stencil only (no fragment stage).
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = kind == P_MASK_CLEAR ? d.mod_fs : d.mod_vert; st[0].pName = "main";
    const bool trans_pass = kind == P_TRANS_A || kind == P_TRANS_B || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW;
    const bool keep = kind == P_TRANS_KEEP || kind == P_TRANS_KEEP_DW;
    const bool one = kind == P_TRANS_ONE || kind == P_TRANS_ONE_DW || keep;
    if ((one && !d.fetch) || (keep && !d.fold)) { *out = VK_NULL_HANDLE; return true; }
    const bool shadow_one = d.raw && kind == P_SHADOW;   // the shadow blended by the shader (Impl::raw)
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = kind == P_OPAQUE_EARLY ? d.mod_frag_early : kind == P_TRANS_OPQ ? d.mod_frag_opq : (one || shadow_one) ? d.mod_frag_fetch : trans_pass ? d.mod_frag_trans : d.mod_frag; st[1].pName = "main";
    st[1].pSpecializationInfo = (shadow_one || keep) ? &d.shadow_si[(toon ? 2 : 0) + (nm ? 1 : 0)] : &d.draw_si[(toon ? 2 : 0) + (nm ? 1 : 0)];
    const bool stencil_only = kind == P_MASK_CLEAR || kind == P_SHADOW_PREP;
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo dss{}; dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = stencil_only ? VK_FALSE : VK_TRUE;
    dss.depthCompareOp = wbuf ? (eq ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER) : (eq ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS);
    dss.depthWriteEnable = (kind == P_OPAQUE || kind == P_OPAQUE_EARLY || kind == P_TRANS_OPQ || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW || kind == P_TRANS_ONE_DW || kind == P_TRANS_KEEP_DW) ? VK_TRUE : VK_FALSE;
    dss.stencilTestEnable = VK_TRUE;
    VkStencilOpState so{};
    so.failOp = VK_STENCIL_OP_KEEP; so.depthFailOp = VK_STENCIL_OP_KEEP; so.passOp = VK_STENCIL_OP_KEEP; so.compareOp = VK_COMPARE_OP_ALWAYS; so.compareMask = 0; so.writeMask = 0; so.reference = 0;
    switch (kind) {
      case P_OPAQUE: case P_OPAQUE_EARLY: case P_TRANS_OPQ:   // drawn, not translucent, this code (reference = drawn | code, set per draw)
        so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_DRAWN | S_T | S_CODE; break;
      case P_TRANS_A: case P_TRANS_A_DW:   // over an undrawn pixel: written as is, now drawn, translucent, this code. The reference
        // (drawn | T | code, set per draw) serves the compare and the replace both, so the "undrawn" test is NOT_EQUAL on the drawn bit.
        so.compareOp = VK_COMPARE_OP_NOT_EQUAL; so.compareMask = S_DRAWN; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_DRAWN | S_T | S_CODE; break;
      case P_TRANS_B: case P_TRANS_B_DW: case P_TRANS_ONE: case P_TRANS_ONE_DW: case P_TRANS_KEEP: case P_TRANS_KEEP_DW:   // over a drawn pixel that is not a translucent one of the same code: blended
        so.compareOp = d.nostencil ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_NOT_EQUAL; so.compareMask = S_DRAWN | S_T | S_CODE; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_DRAWN | S_T | S_CODE; break;
      case P_MASK:     // shadow mask: where the depth test fails the mask bit is set; nothing else changes
        so.depthFailOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK; so.reference = S_MASK; break;
      case P_MASK_CLEAR:   // a new mask set: the mask bit off everywhere (reference 0)
        so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK; break;
      case P_SHADOW_PREP:   // the shadow's own id: the mask bit off where the code matches (reference = code, its mask bit 0)
        so.compareOp = VK_COMPARE_OP_EQUAL; so.compareMask = S_CODE; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK; break;
      case P_SHADOW:   // shadow: drawn (blended) where the mask bit is set (NOT_EQUAL to the reference's clear bit), and the
        // pixel then takes the shadow's id with the mask bit cleared (reference = T | code, replaced into mask | T | code):
        // a face sharing an edge in the same draw finds the bit clear, and a later run of the same id is kept off by
        // the prep pass (code equal). Faces and overlapping volumes darken once, as on the DS.
        so.compareOp = VK_COMPARE_OP_NOT_EQUAL; so.compareMask = S_MASK; so.passOp = VK_STENCIL_OP_REPLACE; so.writeMask = S_MASK | S_T | S_CODE; break;
    }
    dss.front = so; dss.back = so;
    VkPipelineColorBlendAttachmentState cba[3]{};
    cba[0].colorWriteMask = (kind == P_MASK || stencil_only) ? 0u : 0xFu;
    cba[1].colorWriteMask = (kind == P_MASK || stencil_only) ? 0u : 0x1u;
    // Edge marking's word (kIdFormat): the opaque passes write all of it; a translucent polygon that writes depth writes the
    // word's depth bytes alone, as the DS's marking compares the depths the frame ends with (a sky's edge behind a
    // depth-writing translucent bar is not in front of its neighbour: Solatorobo's flight).
    cba[2].colorWriteMask = (kind == P_OPAQUE || kind == P_OPAQUE_EARLY || kind == P_TRANS_OPQ) ? 0xFu :
                            (kind == P_TRANS_A_DW || kind == P_TRANS_B_DW || kind == P_TRANS_ONE_DW || kind == P_TRANS_KEEP_DW) ? 0xEu : 0u;
    const bool blend = kind == P_TRANS_B || kind == P_TRANS_B_DW || kind == P_SHADOW;
    if (blend || one) {
      cba[0].blendEnable = VK_TRUE;
      cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba[0].colorBlendOp = VK_BLEND_OP_ADD;
      cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[0].alphaBlendOp = VK_BLEND_OP_MAX;
      // Multisampled (Impl::mshw): MAX and MIN are no blend the G52's blend unit does, and the stage that stands in
      // for it ran per sample: the blended pass of SM64's 2x frame 8.6 ms against 2.1 with sums alone, half of it
      // each attachment. The fog flag is the product of the two, which is the smaller. The alpha is a + b (1 - a):
      // the same wherever something opaque lies beneath (31 either way), up to a quarter more where two translucent
      // layers overlap over nothing (NSMB's storm cloud, a shade lighter than the DS's larger of the two, which cost
      // SM64's 2x frame 10.5 -> 14.2 ms).
      const bool sums = d.mshw;
      if (sums) { cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba[0].alphaBlendOp = VK_BLEND_OP_ADD; }
      if (one || shadow_one) cba[0].blendEnable = VK_FALSE;   // the shader blends the colour (DS_FETCH); the fog bit's MIN below stays
      // Fog flag: kept only if both the layer beneath and this polygon carry it.
      cba[1].blendEnable = VK_TRUE;
      cba[1].srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].dstColorBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].colorBlendOp = VK_BLEND_OP_MIN;
      cba[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba[1].alphaBlendOp = VK_BLEND_OP_MIN;
      if (sums) { cba[1].srcColorBlendFactor = cba[1].srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_COLOR; cba[1].dstColorBlendFactor = cba[1].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba[1].colorBlendOp = cba[1].alphaBlendOp = VK_BLEND_OP_ADD; }
    }
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = d.mark && !nm ? 3 : 2; cbs.pAttachments = cba;
    if (one || shadow_one) cbs.flags = VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_SCISSOR};   // the scissor: the whole image but for Impl::sc's polygons
    VkPipelineDynamicStateCreateInfo dys{}; dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dys.dynamicStateCount = 2; dys.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = stencil_only ? 1 : 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cbs; gpi.pDynamicState = &dys; gpi.layout = d.layout; gpi.renderPass = nm ? d.rp_nm : d.rp; gpi.subpass = 0;
    return a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, out) == VK_SUCCESS;
  };
  for (u32 wb = 0; wb < 2; ++wb) for (u32 eq = 0; eq < 2; ++eq) for (u32 k = 0; k < P_COUNT; ++k) if (!pipeline(true, wb != 0, eq != 0, k, &d.pipe[1][wb][eq][k])) return fail("graphics pipeline");
  if (d.split) for (u32 wb = 0; wb < 2; ++wb) for (u32 eq = 0; eq < 2; ++eq) for (u32 k = 0; k < P_COUNT; ++k) if (!pipeline(true, wb != 0, eq != 0, k, &d.pipe_nm[1][wb][eq][k], true)) return fail("graphics pipeline (no id attachment)");
  {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.mod_fs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = d.mod_res; st[1].pName = "main"; st[1].pSpecializationInfo = &sharp_si;
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs; gpi.layout = d.layout; gpi.renderPass = d.rp; gpi.subpass = 1;
    if (d.mshw) {
    } else if (!d.direct) {
      if (a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, &d.pipe_resolve) != VK_SUCCESS) return fail("resolve pipeline");
    } else {
      // The last draw: a fullscreen triangle in the draw subpass, no tests, the colour its only write.
      VkBool32 fog_scs[2] = {VK_FALSE, d.raw ? VK_TRUE : VK_FALSE}; VkBool32& fog_sc = fog_scs[0];   // constant_id 3 ds_fog, 5 ds_raw
      const VkSpecializationMapEntry fog_me[2] = {{3, 0, sizeof(VkBool32)}, {5, sizeof(VkBool32), sizeof(VkBool32)}}; const VkSpecializationInfo fog_si{2, fog_me, sizeof fog_scs, fog_scs};
      st[1].module = d.mod_fog; st[1].pSpecializationInfo = &fog_si;
      VkPipelineColorBlendAttachmentState fcba[3]{}; fcba[0].colorWriteMask = 0xFu;
      VkPipelineColorBlendStateCreateInfo fcbs = cbs; fcbs.attachmentCount = d.mark ? 3 : 2; fcbs.pAttachments = fcba; fcbs.flags = VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
      VkPipelineDepthStencilStateCreateInfo fdss{}; fdss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; fdss.flags = VK_PIPELINE_DEPTH_STENCIL_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_DEPTH_ACCESS_BIT_EXT;
      gpi.pColorBlendState = &fcbs; gpi.pDepthStencilState = &fdss; gpi.subpass = 0;
      for (u32 fg = 0; fg < 2; ++fg) { fog_sc = fg; if (a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, &d.pipe_fog[fg]) != VK_SUCCESS) return fail("fog pipeline"); }
      if (d.split) {
        fcbs.attachmentCount = 2; gpi.renderPass = d.rp_nm;
        for (u32 fg = 0; fg < 2; ++fg) { fog_sc = fg; if (a.vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gpi, nullptr, &d.pipe_fog_nm[fg]) != VK_SUCCESS) return fail("fog pipeline (no id attachment)"); }
        gpi.renderPass = d.rp;
      }
      st[1].pSpecializationInfo = &sharp_si;
      gpi.pColorBlendState = &cbs; gpi.pDepthStencilState = nullptr;
    }
  }

  // Commands, fence, timestamps.
  {
    VkCommandPoolCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = vk->qfam;
    if (a.vkCreateCommandPool(vk->dev, &cpi, nullptr, &d.cpool) != VK_SUCCESS) return fail("command pool");
    VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cai.commandPool = d.cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = Impl::kSlots;
    if (a.vkAllocateCommandBuffers(vk->dev, &cai, d.cmd) != VK_SUCCESS) return fail("command buffers");
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (u32 i = 0; i < Impl::kSlots; ++i) if (a.vkCreateFence(vk->dev, &fi, nullptr, &d.fence[i]) != VK_SUCCESS) return fail("fence");
    d.timing = std::getenv("DS_VK_TIMING") && dev.limits().timestamp_period_ns > 0 && a.vkCreateQueryPool;
    // DS_VK_TIMING=2: a timestamp between the stages too (outside render passes only: inside one, a tiler's
    // timestamps do not split the work), reported per stage by DS_GPU_TRACE.
    d.timing_stages = d.timing && std::atoi(std::getenv("DS_VK_TIMING")) >= 2;
    if (d.timing) {
      VkQueryPoolCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = Impl::kTs * Impl::kSlots;
      if (a.vkCreateQueryPool(vk->dev, &qi, nullptr, &d.qpool) != VK_SUCCESS) d.timing = false;
      d.tick_ns = dev.limits().timestamp_period_ns;
    }
    if (std::getenv("DS_LEAN_OCC") && std::atoi(std::getenv("DS_LEAN_OCC")) != 0 && a.vkCmdBeginQuery && a.vkCmdEndQuery) {
      VkQueryPoolCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_OCCLUSION; qi.queryCount = Impl::kOccMax * Impl::kSlots;
      if (a.vkCreateQueryPool(vk->dev, &qi, nullptr, &d.occ_pool) != VK_SUCCESS) d.occ_pool = VK_NULL_HANDLE;
    }
  }
  // The no-toon set, off this thread: the kinds that shade colour (the mask, the
  // shadow prep and the mask clear draw no colour; A/B or the one-pass kinds, whichever this raster draws).
  d.notoon_build = std::thread([&d, pipeline] {
    for (u32 wb = 0; wb < 2; ++wb) for (u32 eq = 0; eq < 2; ++eq) for (u32 k = 0; k < P_COUNT; ++k) {
      const bool one = k == P_TRANS_ONE || k == P_TRANS_ONE_DW || k == P_TRANS_KEEP || k == P_TRANS_KEEP_DW, ab = k == P_TRANS_A || k == P_TRANS_B || k == P_TRANS_A_DW || k == P_TRANS_B_DW;
      if (k == P_MASK || k == P_SHADOW_PREP || k == P_MASK_CLEAR || (one && !d.fetch) || (ab && d.fetch)) continue;
      if (d.split && !pipeline(false, wb != 0, eq != 0, k, &d.pipe_nm[0][wb][eq][k], true)) { d.pipe_nm[0][wb][eq][k] = VK_NULL_HANDLE; }
      if (!pipeline(false, wb != 0, eq != 0, k, &d.pipe[0][wb][eq][k])) { d.pipe[0][wb][eq][k] = VK_NULL_HANDLE; }
    }
    d.notoon_ready.store(true, std::memory_order_release);
    d.dev->save_pipeline_cache();
  });
  self->ready_ = true;
  const double pipes_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_pipes).count();
  dev.save_pipeline_cache();
  std::fprintf(stderr, "gpu lean: %ux%s, %s depth/stencil%s%s%s; pipelines %.0f ms\n", d.scale, d.msaa ? " with 4x MSAA" : " (no MSAA)", d.ds_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", "",
               !d.fetch ? "" : d.fetch == 2 ? ", one-pass translucency (DS blend)" : ", one-pass translucency", d.direct ? ", direct output" : "", pipes_ms);
  return self;
}

void Lean::wait_pipelines() { if (d_->notoon_build.joinable()) d_->notoon_build.join(); }

bool Lean::submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& fin) {
  Impl& d = *d_;
  const Api& a = *d.api;
  const DeviceInternal* vk = d.vk;
  if (npoly > DS_MAX_POLYS || nvert > DS_MAX_VERTS || ntexels > kTexelWords) return false;
  const double t_in = now_ms();
  // The slot's previous frame must be off the GPU (four back: long done).
  u32 slot;
  bool with_rec = true;
  {
    std::lock_guard<std::mutex> lk(d.m);
    slot = d.next_slot(); d.slot_gen[slot] = 0;
    if (d.fence_live[slot]) { a.vkWaitForFences(vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull); d.fence_live[slot] = false; }
    d.valid[slot] = false;
    with_rec = !d.lazy_rec || d.rec_keep > 0;   // the 1x record's pass and copy ride this submit
    if (d.rec_keep) --d.rec_keep;
    d.rec_ok[slot] = with_rec;
  }
  const double t_prep = now_ms();
  // The arena's new texels (append-only: earlier frames' ranges are unchanged).
  if (ntexels > d.tex_flushed) { d.dev->flush(d.texels, sizeof(u32) * d.tex_flushed, sizeof(u32) * (ntexels - d.tex_flushed)); d.tex_flushed = ntexels; }
  {
    std::memcpy(d.verts[slot].ptr, d.host_verts.data(), sizeof(GpuVert) * nvert);
    d.dev->flush(d.verts[slot], 0, sizeof(GpuVert) * nvert);
  }
  std::memcpy(d.post[slot].ptr, &d.host_post, sizeof(GpuPost));
  d.dev->flush(d.post[slot], 0, sizeof(GpuPost));

  GpuFrame f = fin;
  const bool wbuf = (f.flags & DS_FF_WBUFFER) != 0;
  GpuPoly* gpw = d.host_polys.data();
  const GpuPoly* gp = gpw;
  // Fog where a pixel can carry the flag only: an opaque pixel takes its
  // polygon's fog bit, a translucent one can only clear it (melonDS), so
  // with neither the rear plane's nor any polygon's bit set no pixel is
  // fogged, and DISP3DCNT bit 7 goes off for the frame: the resolve skips
  // fog (and the GLES raster drops depth and fog bit, below). Exact.
  if ((f.dispcnt & 0x80u) && !(f.clear_attr & 0x8000u) && std::none_of(gp, gp + npoly, [](const GpuPoly& p) { return ((p.attr >> 15) & 1u) != 0u; })) f.dispcnt &= ~0x80u;
  // Which pipeline set: with mode-2 (toon / highlight) shading only if a polygon uses it (DS_TOON).
  const u32 toon = !d.notoon_ready.load(std::memory_order_acquire) || std::any_of(gp, gp + npoly, [](const GpuPoly& p) { return ((p.attr >> 4) & 3u) == 2u; }) ? 1u : 0u;
  const GpuVert* gv = d.host_verts.data();
  // Each polygon's texcoord range for the fragment stage's clamp (row_base / pad_, unused here otherwise).
  for (u32 k = 0; k < npoly; ++k) {
    GpuPoly& p = gpw[k];
    s32 s0 = 0x7FFF, s1 = -0x8000, t0 = 0x7FFF, t1 = -0x8000;
    for (u32 v = 0; v < p.nverts; ++v) { const GpuVert& q = gv[p.first_vert + v]; s0 = std::min(s0, q.s); s1 = std::max(s1, q.s); t0 = std::min(t0, q.t); t1 = std::max(t1, q.t); }
    auto pk = [](s32 lo, s32 hi) { return static_cast<u32>(std::clamp(lo + 32768, 0, 65535)) | (static_cast<u32>(std::clamp(hi + 32768, 0, 65535)) << 16); };
    p.row_base = pk(s0, s1); p.pad_ = pk(t0, t1);
  }

  // The frame's id codes (see the stencil layout).
  u8 code[64] = {};
  {
    u32 next = 1;
    auto want = [&](u32 id) { if (!code[id]) code[id] = static_cast<u8>(next <= 31 ? next++ : (id & 31)); };
    for (u32 k = 0; k < npoly; ++k) if (gp[k].flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW)) want((gp[k].attr >> 24) & 0x3F);
  }
  auto code_of = [&](const GpuPoly& p) -> u32 { return code[(p.attr >> 24) & 0x3F]; };
  if (std::getenv("DS_LEAN_LIST") && std::atoi(std::getenv("DS_LEAN_LIST")) != 0) {   // id histogram per kind (debug)
    u32 hist[64][4] = {};
    for (u32 k = 0; k < npoly; ++k) { const u32 id = (gp[k].attr >> 24) & 0x3F, kind = (gp[k].flags & DS_PF_SHADOW_MASK) ? 3 : (gp[k].flags & DS_PF_SHADOW) ? 2 : (gp[k].flags & DS_PF_TRANSLUCENT) ? 1 : 0; ++hist[id][kind]; }
    std::fprintf(stderr, "lean ids gen %llu:", (unsigned long long)d.gen);
    for (u32 id = 0; id < 64; ++id) if (hist[id][0] | hist[id][1] | hist[id][2] | hist[id][3]) std::fprintf(stderr, " id%u[o%u t%u s%u m%u]", id, hist[id][0], hist[id][1], hist[id][2], hist[id][3]);
    std::fputc('\n', stderr);
  }

  const double t_rec = now_ms();
  VkCommandBuffer cb = d.cmd[slot];
  a.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
  if (d.timing) { a.vkCmdResetQueryPool(cb, d.qpool, Impl::kTs * slot, Impl::kTs); a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool, Impl::kTs * slot); }
  auto stamp = [&](u32 k) { if (d.timing_stages) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool, Impl::kTs * slot + k); };   // bottom of pipe: after the queue ahead, so the span is this frame's own

  // Clear: the rear plane's colour and alpha; depth; stencil drawn where the rear plane has alpha.
  VkClearValue cv[8]{};
  {
    const u32 c = f.clear_color, ca = (c >> 24) & 0x1F;
    cv[0].color.float32[0] = static_cast<float>(c & 0x3F) / 255.f; cv[0].color.float32[1] = static_cast<float>((c >> 8) & 0x3F) / 255.f; cv[0].color.float32[2] = static_cast<float>((c >> 16) & 0x3F) / 255.f;
    cv[0].color.float32[3] = ca == 0 ? 0.f : static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f;
    if (d.raw) {   // the record's own bytes: the alpha itself, and no colour where there is none
      cv[0].color.float32[3] = static_cast<float>(ca | (((f.dispcnt & 0x80u) && (f.clear_attr & 0x8000u)) ? 0x80u : 0u)) / 255.f;   // bit 7: the rear plane's fog flag, in a frame that fogs
      if (!ca) cv[0].color.float32[0] = cv[0].color.float32[1] = cv[0].color.float32[2] = 0.f;
    }
    if (d.mshw) for (int k = 0; k < 3; ++k) cv[0].color.float32[k] = ca ? cv[0].color.float32[k] + 192.25f / 255.f : 0.f;   // drawn: + 192; undrawn: nothing (lean_msdec.frag)
    const float cz = static_cast<float>(f.clear_depth & 0xFFFFFF);
    cv[1].depthStencil.depth = wbuf ? 1.f / std::max(cz, 1.f) : std::min(1.f, cz / 16777215.f);
    cv[1].depthStencil.stencil = ca ? (S_DRAWN | code[(f.clear_attr >> 24) & 0x3F]) : 0;
    cv[3].color.float32[0] = (f.clear_attr & 0x8000) ? 1.f : 0.f;   // the rear plane's fog bit
  }
  if (d.occ_pool) a.vkCmdResetQueryPool(cb, d.occ_pool, slot * Impl::kOccMax, Impl::kOccMax);
  // Edge marking this frame: the id word kept, and the marking pass after the record.
  // Edge marking where a pixel can be marked only: it takes two polygon ids side by side, so a frame whose
  // polygons all carry the rear plane's id marks nothing (Phantom Hourglass and Spirit Tracks at sea), and the
  // id attachment and the marking pass (5-6 ms of the G52's 4x frame) are left out. Exact.
  const bool one_id = std::all_of(gp, gp + npoly, [&](const GpuPoly& p) { return ((p.attr ^ f.clear_attr) & 0x3F000000u) == 0u; });
  const bool marking = d.mark && d.mark_on && (f.dispcnt & 0x20u) != 0u && !one_id;
  const u32 mark_clear = ((f.clear_attr >> 24) & 0x3Fu) | (((f.clear_depth & 0xFFFFFFu) >> 1) << 9);
  if (d.mark) { const u32 cz = mark_clear >> 9; cv[4].color.uint32[0] = mark_clear & 0x3Fu; cv[4].color.uint32[1] = cz & 0xFFu; cv[4].color.uint32[2] = (cz >> 8) & 0xFFu; cv[4].color.uint32[3] = cz >> 16; }
  else cv[4].color.uint32[0] = 0u;
  static const u32 skip = std::getenv("DS_LEAN_SKIP") ? static_cast<u32>(std::strtoul(std::getenv("DS_LEAN_SKIP"), nullptr, 16)) : 0u;   // DS_LEAN_SKIP (bisect): pass kinds below; bit 13: no edge-marking draw
  stamp(1);
  VkRenderPassBeginInfo rbi{}; rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; const bool nm = d.split && !marking; VkPipeline (*const P)[2][2][P_COUNT] = nm ? d.pipe_nm : d.pipe;
  rbi.renderPass = d.mshw ? d.rp_hw[marking ? 1 : 0][(f.dispcnt & 128u) ? 1 : 0] : marking ? d.rp_keep : nm ? d.rp_nm : d.rp; rbi.framebuffer = nm ? d.fb_nm[slot] : d.fb[slot]; rbi.renderArea = {{0, 0}, {W, H}}; rbi.clearValueCount = d.mshw ? 8 : (d.mark && d.msaa) ? 6 : d.mark ? 5 : 4; rbi.pClearValues = cv;
  a.vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &d.set[slot], 0, nullptr);
  if (d.fetch) a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 1, 1, &d.set_in[slot], 0, nullptr);   // binding 3: the tile's colour
  GpuFrame fp = f; fp.scale = d.scale;
  if (marking) fp.flags2 |= DS_FF2_EDGEMARK;
  fp.flags2 |= DS_FF2_CENTRE;   // polygons of four or more vertices as fans around their centre (lean.vert)
  a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &fp);
  u32 draws = 0;
  VkPipeline bound = VK_NULL_HANDLE;
  auto use = [&](VkPipeline p) { if (p != bound) { a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); bound = p; } };
  // Pass kinds (DS_LEAN_SKIP bits, DS_LEAN_OCC rows): 0 early opaque, 1 alpha-tested opaque, 2 translucent alpha-31,
  // 3 translucent A, 4 translucent B, 5 shadow prep, 6 shadow, 7 shadow mask (+ clear), 8 opaque in the tail, 9 resolve, 10 record copy, 11 the 1x record's pass
  // (and, not a kind: 13 the edge-marking draw; read above).
  u32 nocc = 0;
  // The frame's scissored polygons.
  const VkRect2D whole{{0, 0}, {W, H}};
  a.vkCmdSetScissor(cb, 0, 1, &whole);
  u32 n_sc = 0;
  if (d.sc_n) {
    d.sc_of.assign(npoly, 0);
    for (u32 k = 0; k < d.sc_n; ++k) if (d.sc[k].poly < npoly && d.sc[k].x1 > d.sc[k].x0 && d.sc[k].y1 > d.sc[k].y0 && d.sc[k].x1 <= 256 && d.sc[k].y1 <= 192) { d.sc_of[d.sc[k].poly] = static_cast<u8>(k + 1); ++n_sc; }
  }
  if (d.occ_pool) { d.occ_kind[slot].clear(); if (!d.occ_printed) std::fill(std::begin(d.occ_area), std::end(d.occ_area), 0.0); }
  auto occ = [&](u32 kind, auto&& rec) {
    if ((skip >> kind) & 1) return;
    const bool q = d.occ_pool && nocc < Impl::kOccMax;
    if (q) { a.vkCmdBeginQuery(cb, d.occ_pool, slot * Impl::kOccMax + nocc, VK_QUERY_CONTROL_PRECISE_BIT); d.occ_kind[slot].push_back(static_cast<u8>(kind)); }
    rec();
    if (q) a.vkCmdEndQuery(cb, d.occ_pool, slot * Impl::kOccMax + nocc++);
  };
  GpuPoly* const out_p = static_cast<GpuPoly*>(d.polys[slot].ptr);
  auto draw = [&](u32 kind, u32 first, u32 count) {
    const u32 vc = fan_vertices(kind <= 1 ? out_p : gp, first, count, true);   // the draw's largest fan (B7, lean_runs.h)
    occ(kind, [&] {
      if (n_sc) {
        // A polygon with a scissor of its own leaves its run for a draw under it; the rest as they were.
        const u32 end = first + count;
        for (u32 k = first; k < end;) {
          u32 e = k;
          while (e < end && !d.sc_of[kind <= 1 ? d.key[e].second : e]) ++e;
          if (e > k) a.vkCmdDraw(cb, vc, e - k, 0, k);
          if (e < end) {
            const Raster3D::Scissor& r = d.sc[d.sc_of[kind <= 1 ? d.key[e].second : e] - 1];
            const VkRect2D box{{static_cast<s32>(r.x0 * d.scale), static_cast<s32>(r.y0 * d.scale)}, {(r.x1 - r.x0) * d.scale, (r.y1 - r.y0) * d.scale}};
            a.vkCmdSetScissor(cb, 0, 1, &box); a.vkCmdDraw(cb, vc, 1, 0, e); a.vkCmdSetScissor(cb, 0, 1, &whole);
            ++e;
          }
          k = e;
        }
        return;
      }
      a.vkCmdDraw(cb, vc, count, 0, first);
    });
    if (d.occ_pool && !((skip >> kind) & 1)) {   // DS_LEAN_OCC: the rasterised area too (before any test or discard)
      const GpuPoly* src = kind <= 1 ? out_p : gp;
      for (u32 k = first; k < first + count; ++k) {
        const GpuVert* v = gv + src[k].first_vert; s64 a2 = 0;
        for (u32 n = 0; n < src[k].nverts; ++n) { const GpuVert& p0 = v[n]; const GpuVert& p1 = v[(n + 1) % src[k].nverts]; a2 += s64{p0.fx} * p1.fy - s64{p1.fx} * p0.fy; }
        d.occ_area[kind] += static_cast<double>(a2 < 0 ? -a2 : a2) / 512.0;   // 28.4 corners: px^2 at 1x
        if (kind == 2 && ((src[k].attr >> 16) & 0x1Fu) == 31u) d.occ_area[10] += static_cast<double>(a2 < 0 ? -a2 : a2) / 512.0;   // alpha-31 pass, polygon alpha 31 (the only ones it can draw)
      }
    }
    ++draws;
  };   // 8 fan triangles at most (10 vertices)

  // Opaque prefix: order is free (a depth test decides), so it is redrawn
  // for the hardware: polygons that cannot fail the alpha test first, front
  // to back with the early depth test, then the cut-out textured ones with
  // the discard. The GpuPoly records themselves are reordered on the way
  // into the slot's buffer (vertices are referenced by index, so nothing
  // else moves).
  const u32 first_ordered = std::min(f.first_ordered, npoly);
  u32 n_early = 0;
  if (first_ordered) {
    auto nearest = [&](const GpuPoly& p) -> s32 {   // the polygon's nearest vertex depth (W-buffer: smaller w is nearer too)
      const GpuVert* v = gv + p.first_vert;
      s32 zmin = 0x7FFFFFFF; for (u32 k = 0; k < p.nverts; ++k) zmin = std::min(zmin, v[k].z); return zmin;
    };
    d.key.clear(); d.key.reserve(first_ordered);
    const u32 noearly = DS_PF_TEX_ALPHA;
    // Key: alpha-tested last, then by stencil code (one reference per run; nearly all are code 0), then near to far.
    for (u32 k = 0; k < first_ordered; ++k) { const bool at = (gp[k].flags & noearly) != 0, eq = (gp[k].attr & 0x4000u) != 0; d.key.emplace_back((at ? (s64{1} << 40) : 0) + (eq ? (s64{1} << 38) : 0) + (s64{code_of(gp[k])} << 32) + nearest(gp[k]), k); }
    std::sort(d.key.begin(), d.key.end());
    // DS_LEAN_KEYREV=1: the prefix far to near instead (diagnostic: the
    // depth test makes the order free, so any output that changes is order-dependent).
    static const bool keyrev = std::getenv("DS_LEAN_KEYREV") && std::atoi(std::getenv("DS_LEAN_KEYREV")) != 0;
    if (keyrev) {   // within each class (the alpha-tested run stays last: the pipelines differ)
      auto mid = std::find_if(d.key.begin(), d.key.end(), [](const std::pair<s64, u32>& k) { return (k.first >> 40) != 0; });
      std::reverse(d.key.begin(), mid); std::reverse(mid, d.key.end());
    }
    for (u32 k = 0; k < first_ordered; ++k) { out_p[k] = gp[d.key[k].second]; if (!(gp[d.key[k].second].flags & noearly)) ++n_early; }
    for (u32 k = 0; k < first_ordered;) {
      const u32 c = code_of(out_p[k]); const bool early = k < n_early, eq = (out_p[k].attr & 0x4000u) != 0;
      u32 j = k + 1;
      while (j < first_ordered && code_of(out_p[j]) == c && (j < n_early) == early && ((out_p[j].attr & 0x4000u) != 0) == eq) ++j;
      use(P[toon][wbuf][eq][early ? P_OPAQUE_EARLY : P_OPAQUE] ? P[toon][wbuf][eq][early ? P_OPAQUE_EARLY : P_OPAQUE] : P[1][wbuf][eq][early ? P_OPAQUE_EARLY : P_OPAQUE]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | c); draw(early ? 0 : 1, k, j - k);
      k = j;
    }
  }
  // The tail, in list order: runs of polygons that take the same passes.
  u32 i = first_ordered;
  bool prev_mask = false, any_mask = false;
  while (i < npoly) {
    const GpuPoly& p = gp[i];
    const u32 mode = (p.attr >> 4) & 3, id = (p.attr >> 24) & 0x3F, c = code[id];
    const bool mask = (p.flags & DS_PF_SHADOW_MASK) != 0, shadow = (p.flags & DS_PF_SHADOW) != 0;
    const bool trans = (p.flags & DS_PF_TRANSLUCENT) != 0, dw = (p.attr & (1u << 11)) != 0, eq = (p.attr & 0x4000u) != 0;
    VkPipeline pipes[P_COUNT];
    for (u32 k = 0; k < P_COUNT; ++k) pipes[k] = P[toon][wbuf][eq][k] ? P[toon][wbuf][eq][k] : P[1][wbuf][eq][k];
    u32 j = i + 1;
    while (j < npoly) {
      const GpuPoly& q = gp[j];
      if (((q.attr >> 4) & 3) != mode || ((q.attr >> 24) & 0x3F) != id || ((q.attr & 0x4000u) != 0) != eq || ((q.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT)) != (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT))) || ((q.attr & (1u << 11)) != 0) != dw) break;
      ++j;
    }
    const u32 n = j - i;
    // DS_LEAN_LIST=1: the tail's runs (kind, id, count, depth range) for the frames with shadows, to stderr.
    static const bool list_trace = std::getenv("DS_LEAN_LIST") && std::atoi(std::getenv("DS_LEAN_LIST")) != 0;
    if (list_trace && (mask || shadow)) {
      s32 zlo = 0x7FFFFFFF, zhi = -1; for (u32 k = i; k < j; ++k) for (u32 v = 0; v < gp[k].nverts; ++v) { const s32 z = gv[gp[k].first_vert + v].z; zlo = std::min(zlo, z); zhi = std::max(zhi, z); }
      std::fprintf(stderr, "lean list gen %llu: %s id %u code %u x%u polys %u..%u z %d..%d attr %08x\n", (unsigned long long)d.gen, mask ? "MASK" : "SHADOW", id, c, n, i, j - 1, zlo, zhi, p.attr);
    }
    if (mask) {
      // A mask polygon after a non-mask one starts a new set: the DS clears the mask stencil (per line; here for the frame).
      static const bool no_clear = std::getenv("DS_LEAN_NOMASKCLEAR") != nullptr;   // debug
      if (any_mask && !prev_mask && !no_clear) { use(pipes[P_MASK_CLEAR]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0); occ(7, [&] { a.vkCmdDraw(cb, 3, 1, 0, 0); }); ++draws; }
      any_mask = true;
      use(pipes[P_MASK]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_MASK); draw(7, i, n);
    } else if (shadow) {
      static const bool no_prep = std::getenv("DS_LEAN_NOPREP") != nullptr;   // debug
      if (!no_prep) { use(pipes[P_SHADOW_PREP]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, c); draw(5, i, n); }
      use(pipes[P_SHADOW]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_T | c); draw(6, i, n);
    } else if (trans) {
      // The run's alpha-31 pixels first, as opaque (depth written, no
      // translucent flag), then the rest. Only polygons of alpha 31 can have
      // any (alpha_31_ranges), so the pass is drawn over those alone.
      u32 lo[kA31Ranges], cnt[kA31Ranges];
      if (const u32 nr = alpha_31_ranges(gp, i, j, lo, cnt, d.fold)) {
        use(pipes[P_TRANS_OPQ]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | c);
        for (u32 r = 0; r < nr; ++r) draw(2, lo[r], cnt[r]);
      }
      // The rest over the polygons that can have a pixel below 31 (alpha_part_ranges).
      const u32 npart = alpha_part_ranges(gp, i, j, lo, cnt);
      if (!npart) {}
      else if (d.fetch) {
        // Folded: the stage that keeps the pixels of alpha 31, for a run with a polygon that can have one and others (the
        // plain stage elsewhere: with the constant set, the G52 ran it 5% slower on runs it changes nothing for).
        const bool keep = d.fold && std::any_of(gp + i, gp + j, [](const GpuPoly& q) { return ((q.attr >> 16) & 0x1Fu) == 31u && (q.flags & (DS_PF_A31_NONE | DS_PF_A31_ALL)) == 0u; });
        use(pipes[keep ? (dw ? P_TRANS_KEEP_DW : P_TRANS_KEEP) : (dw ? P_TRANS_ONE_DW : P_TRANS_ONE)]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | S_T | c); for (u32 r = 0; r < npart; ++r) draw(4, lo[r], cnt[r]); }   // A and B in one pass, the shader blending
      else {
        // No pass A in a frame whose rear plane has alpha: every pixel is drawn from the clear on. (The fold in these two
        // passes, tried for Impl::mshw: nothing gained on the G52, and GS: Dark Dawn's name screen with pale frames.)
        const bool rear_drawn = ((f.clear_color >> 24) & 0x1Fu) != 0u;
        if (!d.nostencil && !rear_drawn) { use(pipes[dw ? P_TRANS_A_DW : P_TRANS_A]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | S_T | c); for (u32 r = 0; r < npart; ++r) draw(3, lo[r], cnt[r]); }
        use(pipes[dw ? P_TRANS_B_DW : P_TRANS_B]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | S_T | c); for (u32 r = 0; r < npart; ++r) draw(4, lo[r], cnt[r]);
      }
    } else {
      use(pipes[P_OPAQUE]); a.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, S_DRAWN | c); draw(8, i, n);
    }
    prev_mask = mask;
    i = j;
    if (draws > kMaxDraws) break;
  }
  if (npoly > first_ordered) std::memcpy(out_p + first_ordered, gp + first_ordered, sizeof(GpuPoly) * (npoly - first_ordered));
  if (npoly) d.dev->flush(d.polys[slot], 0, sizeof(GpuPoly) * npoly);
  if (d.raw && !(f.dispcnt & 128u)) {
    // The tile is the record already and nothing fogs: no last draw.
  } else if (d.direct) {   // the tile into the record, fogged in frames that fog (kind 9, as the resolve it replaces)
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (nm ? d.pipe_fog_nm : d.pipe_fog)[(f.dispcnt & 128u) ? 1 : 0]); occ(9, [&] { a.vkCmdDraw(cb, 3, 1, 0, 0); });
  } else if (d.mshw) {   // the hardware resolves at the subpass's end; the record is written from that below
  } else {
    a.vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_resolve);
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 1, 1, &d.set_in[slot], 0, nullptr);
    occ(9, [&] { a.vkCmdDraw(cb, 3, 1, 0, 0); });
  }
  a.vkCmdEndRenderPass(cb);
  if (d.mshw) {
    VkRenderPassBeginInfo rb{}; rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass = d.rp_dec; rb.framebuffer = d.fb_dec[slot]; rb.renderArea = {{0, 0}, {W, H}};
    a.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_dec);
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout_dec, 0, 1, &d.set_dec[slot], 0, nullptr);
    a.vkCmdPushConstants(cb, d.layout_dec, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &fp);
    occ(9, [&] { a.vkCmdDraw(cb, 3, 1, 0, 0); });
    a.vkCmdEndRenderPass(cb);
  }
  stamp(2);
  if (marking) {   // the record's edge pixels recoloured (lean_mark.frag), before anything reads it
    VkRenderPassBeginInfo rb{}; rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass = d.rp_mark; rb.framebuffer = d.fb_mark[slot]; rb.renderArea = {{0, 0}, {W, H}};
    a.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_mark);
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout_mark, 0, 1, &d.set_mark[slot], 0, nullptr);
    const u32 mpc[3] = {mark_clear, d.mark_width, (f.dispcnt & ~0x10u) | ((f.flags2 & DS_FF2_GAME_AA) ? 0x10u : 0u)};   // bit 4 the game's own
    a.vkCmdPushConstants(cb, d.layout_mark, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mpc), mpc);
    if (!((skip >> 13) & 1)) a.vkCmdDraw(cb, 3, 1, 0, 0);
    a.vkCmdEndRenderPass(cb);
  }
  stamp(3);
  // The record to the CPU-readable buffer.
  VkBufferImageCopy rg{}; rg.bufferRowLength = W; rg.bufferImageHeight = H; rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageExtent = {W, H, 1};
  const VkImage rec_img = d.res[slot].img;
  if (d.down) {
    if (d.fine[slot]) a.vkCmdCopyImageToBuffer(cb, d.res[slot].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf(d.fine[slot]), 1, &rg);
    // The 1x record: the image to its lasting layout first, then the texel under each DS pixel's centre into rec1 (lean_down.frag).
    VkImageMemoryBarrier ib{}; ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.image = d.res[slot].img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    if (with_rec) {
    VkRenderPassBeginInfo rb{}; rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass = d.rp_down; rb.framebuffer = d.fb_down; rb.renderArea = {{0, 0}, {256, 192}};
    a.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_down);
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout_down, 0, 1, &d.set_down[slot], 0, nullptr);
    a.vkCmdPushConstants(cb, d.layout_down, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32), &d.scale);
    if (!((skip >> 11) & 1)) a.vkCmdDraw(cb, 3, 1, 0, 0);
    a.vkCmdEndRenderPass(cb);
    VkBufferImageCopy r1{}; r1.bufferRowLength = 256; r1.bufferImageHeight = 192; r1.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r1.imageExtent = {256, 192, 1};
    if (!((skip >> 10) & 1)) a.vkCmdCopyImageToBuffer(cb, d.rec1.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf(d.out[slot]), 1, &r1);
    }
  } else if (!((skip >> 10) & 1)) a.vkCmdCopyImageToBuffer(cb, rec_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf(d.out[slot]), 1, &rg);
  if (d.res8[slot] && !d.down) {   // the record's image stays, readable by a compositor's fragment stage (layer())
    VkImageMemoryBarrier ib{}; ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.image = d.res[slot].img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
  }
  VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  if (d.timing) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool, Impl::kTs * slot + Impl::kTs - 1);
  if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
  const double t_q = now_ms();
  VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  a.vkResetFences(vk->dev, 1, &d.fence[slot]);
  // DS_LEAN_SLOW=ms (debug): hold the submit back, a raster slower than the frame rate on any machine.
  { static const int slow = std::getenv("DS_LEAN_SLOW") ? std::atoi(std::getenv("DS_LEAN_SLOW")) : 0; if (slow > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow)); }
  if (vk->queue2) {
    if (a.vkQueueSubmit(vk->queue2, 1, &si, d.fence[slot]) != VK_SUCCESS) return false;   // only this thread submits here
  } else {
    std::lock_guard<std::mutex> lk(d.dev->queue_mutex());
    if (a.vkQueueSubmit(vk->queue, 1, &si, d.fence[slot]) != VK_SUCCESS) return false;
  }
  tl::ev("R_submit", slot);
  std::lock_guard<std::mutex> lk(d.m);
  d.st.record_ms += t_q - t_rec; d.st.queue_ms += now_ms() - t_q; d.st.slot_wait_ms += t_prep - t_in; d.st.prep_ms += t_rec - t_prep;
  d.fence_live[slot] = true; d.submit_at[slot] = t_in;
  d.last_draws = draws;
  if (wbuf) ++d.st.wbuf;
  d.slot_gen[slot] = ++d.gen;
  return true;
}

// Slot `slot` finished (waited or seen signalled): make its record readable.
static void finish_slot(Lean::Impl& d, u32 slot) {
  const Api& a = *d.api;
  d.fence_live[slot] = false;
  if (d.occ_pool && !d.occ_printed && !d.occ_kind[slot].empty()) {   // DS_LEAN_OCC: samples passing per pass kind, once
    const u32 n = static_cast<u32>(d.occ_kind[slot].size());
    std::vector<u64> r(n);
    if (a.vkGetQueryPoolResults(d.vk->dev, d.occ_pool, slot * Lean::Impl::kOccMax, n, sizeof(u64) * n, r.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
      static const char* const names[] = {"early opaque", "alpha-tested opaque", "translucent a31", "translucent A", "translucent B", "shadow prep", "shadow", "shadow mask", "tail opaque", "resolve"};
      u64 sum[10] = {}; u32 cnt[10] = {};
      for (u32 i = 0; i < n; ++i) { sum[d.occ_kind[slot][i]] += r[i]; ++cnt[d.occ_kind[slot][i]]; }
      const double px = static_cast<double>(W) * H;
      std::fprintf(stderr, "lean occ (%ux%u, samples passing):", W, H);
      for (u32 k = 0; k < 10; ++k) if (cnt[k]) std::fprintf(stderr, " %s %llu (%.2f/px, area %.2f/px, %u draws);", names[k], static_cast<unsigned long long>(sum[k]), static_cast<double>(sum[k]) / px, d.occ_area[k] / (256.0 * 192.0), cnt[k]);
      std::fprintf(stderr, " a31 area at polygon alpha 31 %.2f/px\n", d.occ_area[10] / (256.0 * 192.0));
      d.occ_printed = true;
    }
  }
  d.dev->invalidate(d.out[slot], 0, sizeof(u32) * (d.down ? 256u * 192u : W * H));
  if (d.fine[slot]) d.dev->invalidate(d.fine[slot], 0, sizeof(u32) * W * H);
  d.valid[slot] = true;
  if (d.timing) {
    constexpr u32 n = Lean::Impl::kTs;
    u64 t[n] = {};
    // Only the start and the end are written without DS_VK_TIMING=2: read those two apart.
    const bool ok = d.timing_stages
        ? a.vkGetQueryPoolResults(d.vk->dev, d.qpool, n * slot, n, sizeof t, t, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS
        : (a.vkGetQueryPoolResults(d.vk->dev, d.qpool, n * slot, 1, sizeof(u64), &t[0], sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
           a.vkGetQueryPoolResults(d.vk->dev, d.qpool, n * slot + n - 1, 1, sizeof(u64), &t[n - 1], sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
    if (ok) tl::ev("R_gpu", static_cast<u64>(static_cast<double>(t[0]) * d.tick_ns), static_cast<u64>(static_cast<double>(t[n - 1]) * d.tick_ns));
    if (ok && t[n - 1] > t[0]) {
      d.last_gpu_ns = static_cast<u64>(static_cast<double>(t[n - 1] - t[0]) * d.tick_ns); d.st.gpu_ms += static_cast<double>(d.last_gpu_ns) / 1e6; ++d.st.timed;
      if (d.timing_stages)
        for (u32 k = 0; k < Raster3D::kStages; ++k) {
          const double ms = t[k + 1] >= t[k] ? static_cast<double>(t[k + 1] - t[k]) * d.tick_ns / 1e6 : 0.0;
          d.st.stage_ms[k] += ms; d.st.stage_max[k] = std::max(d.st.stage_max[k], ms);
        }
    }
  }
}

// A finished slot's 1x record, made now: its submit left the pass and the copy out (Impl::lazy_rec). Under d.m,
// which keeps the slot from being taken for a new frame meanwhile.
static void record_now(Lean::Impl& d, u32 slot) {
  const Api& a = *d.api;
  VkCommandBuffer cb = d.cmd_rec;
  a.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return;
  VkRenderPassBeginInfo rb{}; rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass = d.rp_down; rb.framebuffer = d.fb_down2; rb.renderArea = {{0, 0}, {256, 192}};
  a.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
  a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_down);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout_down, 0, 1, &d.set_down[slot], 0, nullptr);
  a.vkCmdPushConstants(cb, d.layout_down, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(u32), &d.scale);
  a.vkCmdDraw(cb, 3, 1, 0, 0);
  a.vkCmdEndRenderPass(cb);
  VkBufferImageCopy r1{}; r1.bufferRowLength = 256; r1.bufferImageHeight = 192; r1.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r1.imageExtent = {256, 192, 1};
  a.vkCmdCopyImageToBuffer(cb, d.rec1b.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf(d.out[slot]), 1, &r1);
  VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return;
  VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  a.vkResetFences(d.vk->dev, 1, &d.fence_rec);
  {
    std::lock_guard<std::mutex> lk(d.dev->queue_mutex());
    if (a.vkQueueSubmit(d.vk->queue, 1, &si, d.fence_rec) != VK_SUCCESS) return;
  }
  tl::ev("R_rec", slot);
  a.vkWaitForFences(d.vk->dev, 1, &d.fence_rec, VK_TRUE, ~0ull);
  d.dev->invalidate(d.out[slot], 0, sizeof(u32) * 256u * 192u);
  d.rec_ok[slot] = true;
  ++d.st.rec_late;
}
// The record a reader gets: slot `slot`'s (finished), made first if its submit left it out. The submits that follow carry theirs.
static const u32* record_of(Lean::Impl& d, u32 slot) {
  d.rec_keep = 8;
  if (d.lazy_rec && !d.rec_ok[slot]) record_now(d, slot);
  return static_cast<const u32*>(d.out[slot].ptr);
}

static void wait_locked(Lean::Impl& d) {
  const int found = d.slot_of(d.gen);
  if (found < 0) return;
  const u32 slot = static_cast<u32>(found);
  if (!d.fence_live[slot]) return;
  d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull);
  d.st.lat_ms += now_ms() - d.submit_at[slot]; ++d.st.lat_n;
  finish_slot(d, slot);
}

void Lean::wait() { std::lock_guard<std::mutex> lk(d_->m); wait_locked(*d_); }

void Lean::wait_all() {
  Impl& d = *d_;
  std::lock_guard<std::mutex> lk(d.m);
  for (u32 i = 0; i < Impl::kSlots; ++i) if (d.fence_live[i]) { d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[i], VK_TRUE, ~0ull); finish_slot(d, i); }
  d.tex_flushed = 0;
}

// Slot `slot` finished (seen signalled, no wait): make it readable.
static void poll_slot(Lean::Impl& d, u32 slot) {
  if (d.fence_live[slot] && d.api->vkGetFenceStatus(d.vk->dev, d.fence[slot]) == VK_SUCCESS) finish_slot(d, slot);
}

bool Lean::has_layers() const { return d_->res8[0] != VK_NULL_HANDLE; }
bool Lean::layer(u64 gen, Layer3D* out) {
  Impl& d = *d_;
  std::lock_guard<std::mutex> lk(d.m);
  const int found = d.slot_of(gen);
  if (found < 0) return false;
  const u32 slot = static_cast<u32>(found);
  if (!d.res8[slot]) return false;
  out->view = reinterpret_cast<u64>(d.res8[slot]); out->scale = d.scale; out->gen = gen;
  out->wait = d.vk->queue2 ? this : nullptr;   // the raster's own queue: nothing orders a reader's submit after this frame's
  return true;
}
void Lean::pin(u64 a, u64 b) { std::lock_guard<std::mutex> lk(d_->m); d_->pinned[0] = a; d_->pinned[1] = b; }
void Lean::layer_wait(u64 gen) {
  Impl& d = *d_;
  std::lock_guard<std::mutex> lk(d.m);
  const int found = d.slot_of(gen);
  if (found < 0) return;
  const u32 slot = static_cast<u32>(found);
  if (d.fence_live[slot]) { d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull); finish_slot(d, slot); }
}

const u32* Lean::frame_ready(u64 want, bool allow_lag) {
  Impl& d = *d_;
  std::lock_guard<std::mutex> lk(d.m);
  if (!d.gen) return nullptr;
  // The frame asked for, clamped to the resident ones (the newest when 0).
  // One not submitted yet: the newest under the lag rule if it is done, else nothing (the caller waits for the submit).
  static const bool no_lag = std::getenv("DS_GPU_NOLAG") && std::atoi(std::getenv("DS_GPU_NOLAG")) != 0;   // debug: never the frame before (deterministic dumps)
  if (want > d.gen) {
    if (!allow_lag || no_lag) return nullptr;
    const int nf = d.slot_of(d.gen);
    if (nf < 0) return nullptr;
    const u32 newest = static_cast<u32>(nf);
    if (d.fence_live[newest]) { d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[newest], VK_TRUE, ~0ull); d.st.lat_ms += now_ms() - d.submit_at[newest]; ++d.st.lat_n; finish_slot(d, newest); }
    ++d.st.lagged; return record_of(d, newest);
  }
  if (want == 0) want = d.gen;
  if (d.slot_of(want) < 0) {   // no longer resident: the oldest frame after it that is
    u64 best = 0;
    for (u64 g : d.slot_gen) if (g > want && (!best || g < best)) best = g;
    if (!best) return nullptr;
    want = best;
  }
  const u32 slot = static_cast<u32>(d.slot_of(want));
  if (d.asked_gen != want) { d.asked_gen = want; d.st.gap_ms += now_ms() - d.submit_at[slot]; ++d.st.gap_n; }
  poll_slot(d, slot);
  if (!d.fence_live[slot]) { ++d.st.newest; return record_of(d, slot); }
  if (const int pf = d.slot_of(want - 1); allow_lag && !no_lag && pf >= 0) {
    const u32 prev = static_cast<u32>(pf);
    poll_slot(d, prev);
    if (!d.fence_live[prev] && d.valid[prev]) { ++d.st.lagged; return record_of(d, prev); }
  }
  if (allow_lag) ++d.st.stalled; else ++d.st.nolag;
  d.api->vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull);
  d.st.lat_ms += now_ms() - d.submit_at[slot]; ++d.st.lat_n;
  finish_slot(d, slot);
  return record_of(d, slot);
}

} // namespace ds::gpu::vk

#endif // DSPERATE_VULKAN
