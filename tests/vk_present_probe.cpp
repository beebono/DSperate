// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// vk_present_probe [W H]: the fragment present (present.vert/.frag, what
// gpu_present.cpp draws: a render pass on the imported scanout dma-buf, a
// quad per view) against a nearest vkCmdBlitImage, the floor for a copy
// into the import. Two dma-bufs of W x H (default 1024x768), one view each,
// as --dual-window; one submit per view. Reports what the driver allows on the import, the GPU time
// (timestamps) and the submit-to-fence time of each path, and checks every
// pixel against nearest. Device-only, not a test.
#include "core/gpu/vk/vk_internal.h"
#include "frontend/sdl/dmaheap.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace ds;
using namespace ds::gpu::vk;

__asm__(".section .rodata\n.balign 4\n.globl ds_present_vert_data\nds_present_vert_data:\n"
        ".incbin \"" DSPERATE_PRESENT_SHADER_DIR "/present_vert.spv\"\n"
        ".globl ds_present_vert_end\nds_present_vert_end:\n"
        ".balign 4\n.globl ds_present_frag_data\nds_present_frag_data:\n"
        ".incbin \"" DSPERATE_PRESENT_SHADER_DIR "/present_frag.spv\"\n"
        ".globl ds_present_frag_end\nds_present_frag_end:\n.previous\n");
extern "C" const unsigned char ds_present_vert_data[], ds_present_vert_end[], ds_present_frag_data[], ds_present_frag_end[];

namespace {

constexpr u32 SW = 256, SH = 192, kFrameWords = SW * SH;
constexpr int kRuns = 300;
constexpr VkFormat kFmt = VK_FORMAT_B8G8R8A8_UNORM;   // XR24 in memory

struct Push { s32 rect[4]; u32 a[4]; u32 b[4]; float s[4]; };   // present.vert/.frag's
struct Rect { int x, y, w, h; };

const DeviceInternal* vk = nullptr;
const Api* a = nullptr;
VkPhysicalDeviceMemoryProperties memprops{};
bool mod = false;

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

u32 find_mem(u32 bits, VkMemoryPropertyFlags want) {
  for (u32 i = 0; i < memprops.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
  for (u32 i = 0; i < memprops.memoryTypeCount; ++i) if (bits & (1u << i)) return i;
  return ~0u;
}

struct DmaBuf {
  int fd = -1; u32* px = nullptr; size_t len = 0;
  // The frontend's allocator: DS_DMA_HEAP picks the source (a heap name,
  // `udmabuf`, `drm`), else its own order (contiguous heaps first).
  bool alloc(size_t bytes) {
    len = bytes;
    fd = sdl::dmaheap::alloc(bytes, [](int) { return true; }, "vk_present_probe");
    if (fd < 0) return false;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return false;
    px = static_cast<u32*>(p);
    return true;
  }
  void sync(bool begin, bool write) const {
    dma_buf_sync s{}; s.flags = (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
  }
  ~DmaBuf() { if (px) munmap(px, len); if (fd >= 0) close(fd); }
};

// Whether the driver takes an imported LINEAR dma-buf image with `usage`.
bool format_ok(u32 w, u32 h, VkImageUsageFlags usage) {
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi{};
  mi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
  mi.drmFormatModifier = 0; mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkPhysicalDeviceExternalImageFormatInfo ei{}; ei.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  ei.pNext = mod ? &mi : nullptr; ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkPhysicalDeviceImageFormatInfo2 fi{}; fi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2; fi.pNext = &ei;
  fi.format = kFmt; fi.type = VK_IMAGE_TYPE_2D; fi.usage = usage;
  fi.tiling = mod ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
  VkExternalImageFormatProperties ep{}; ep.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 fp{}; fp.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2; fp.pNext = &ep;
  if (a->vkGetPhysicalDeviceImageFormatProperties2(vk->phys, &fi, &fp) != VK_SUCCESS) return false;
  return fp.imageFormatProperties.maxExtent.width >= w && fp.imageFormatProperties.maxExtent.height >= h &&
         (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

struct Import { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };

bool import(const DmaBuf& b, u32 w, u32 h, VkImageUsageFlags usage, Import& out, const char** why) {
  VkSubresourceLayout plane{}; plane.rowPitch = w * 4;
  VkImageDrmFormatModifierExplicitCreateInfoEXT mex{};
  mex.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  mex.drmFormatModifier = 0; mex.drmFormatModifierPlaneCount = 1; mex.pPlaneLayouts = &plane;
  VkExternalMemoryImageCreateInfo ext{}; ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext.pNext = mod ? &mex : nullptr; ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.pNext = &ext;
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kFmt; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = mod ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
  ii.usage = usage; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (a->vkCreateImage(vk->dev, &ii, nullptr, &out.img) != VK_SUCCESS) { *why = "vkCreateImage"; return false; }
  if (!mod) {
    VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout got{};
    a->vkGetImageSubresourceLayout(vk->dev, out.img, &sr, &got);
    if (got.rowPitch != w * 4) { *why = "LINEAR pitch differs"; return false; }
  }
  VkMemoryRequirements mr{}; a->vkGetImageMemoryRequirements(vk->dev, out.img, &mr);
  VkMemoryFdPropertiesKHR fp{}; fp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
  if (a->vkGetMemoryFdPropertiesKHR(vk->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, b.fd, &fp) != VK_SUCCESS) { *why = "vkGetMemoryFdPropertiesKHR"; return false; }
  const u32 type = find_mem(fp.memoryTypeBits & mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == ~0u) { *why = "no memory type"; return false; }
  VkMemoryDedicatedAllocateInfo ded{}; ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; ded.image = out.img;
  VkImportMemoryFdInfoKHR imp{}; imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR; imp.pNext = &ded;
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = dup(b.fd);
  VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.pNext = &imp; ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
  if (a->vkAllocateMemory(vk->dev, &ai, nullptr, &out.mem) != VK_SUCCESS) { close(imp.fd); *why = "vkAllocateMemory (import)"; return false; }
  if (a->vkBindImageMemory(vk->dev, out.img, out.mem, 0) != VK_SUCCESS) { *why = "vkBindImageMemory"; return false; }
  VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = out.img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kFmt; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (a->vkCreateImageView(vk->dev, &vi, nullptr, &out.view) != VK_SUCCESS) { *why = "vkCreateImageView"; return false; }
  return true;
}

VkImageMemoryBarrier img_barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
  VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return b;
}

struct Stats { std::vector<double> v; void add(double x) { v.push_back(x); }
  double pct(double p) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; } };

// Every pixel of `d` against nearest from `src` in `r`, black outside.
bool check(const DmaBuf& d, u32 w, u32 h, const u32* src, const Rect& r) {
  d.sync(true, false);
  u32 bad = 0, fx = 0, fy = 0;
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
      const int lx = static_cast<int>(x) - r.x, ly = static_cast<int>(y) - r.y;
      const bool in = lx >= 0 && ly >= 0 && lx < r.w && ly < r.h;
      const u32 want = in ? src[static_cast<u32>(ly * static_cast<int>(SH) / r.h) * SW + static_cast<u32>(lx * static_cast<int>(SW) / r.w)] & 0xFFFFFFu : 0;
      if ((d.px[y * w + x] & 0xFFFFFFu) != want && !bad++) { fx = x; fy = y; }
    }
  d.sync(false, false);
  if (bad) std::printf("    MISMATCH: %u pixels, first at %u,%u\n", bad, fx, fy);
  return bad == 0;
}

} // namespace

int main(int argc, char** argv) {
  const u32 W = argc > 2 ? static_cast<u32>(std::atoi(argv[1])) : 1024, H = argc > 2 ? static_cast<u32>(std::atoi(argv[2])) : 768;
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash keeps what was printed
  std::string why;
  auto dev = Device::shared(&why);
  if (!dev) { std::printf("no Vulkan: %s\n", why.c_str()); return 1; }
  if (!dev->limits().dmabuf_import) { std::printf("%s: no dma-buf import\n", dev->name().c_str()); return 1; }
  vk = dev->internal(); a = vk->api; mod = dev->limits().drm_modifier;
  if (!a->vkCmdBlitImage || !a->vkCmdCopyBufferToImage || !a->vkCmdClearColorImage) { std::printf("blit/copy/clear entry points missing\n"); return 1; }
  a->vkGetPhysicalDeviceMemoryProperties(vk->phys, &memprops);
  VkPhysicalDeviceProperties pp{}; a->vkGetPhysicalDeviceProperties(vk->phys, &pp);
  const double tick_ns = pp.limits.timestampPeriod;
  std::printf("%s, %s import, two %ux%u dma-bufs\n", dev->name().c_str(), mod ? "DRM-modifier LINEAR" : "VK_IMAGE_TILING_LINEAR", W, H);

  // What the driver lets an imported scanout buffer be.
  VkFormatProperties fprops{}; a->vkGetPhysicalDeviceFormatProperties(vk->phys, kFmt, &fprops);
  const VkFormatFeatureFlags lf = fprops.linearTilingFeatures;
  std::printf("linear B8G8R8A8 features: storage %d, blit dst %d, transfer dst %d, colour attachment %d\n",
              !!(lf & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT), !!(lf & VK_FORMAT_FEATURE_BLIT_DST_BIT),
              !!(lf & VK_FORMAT_FEATURE_TRANSFER_DST_BIT), !!(lf & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
  const bool ok_store = format_ok(W, H, VK_IMAGE_USAGE_STORAGE_BIT), ok_xfer = format_ok(W, H, VK_IMAGE_USAGE_TRANSFER_DST_BIT),
             ok_col = format_ok(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  std::printf("dma-buf import usage: STORAGE %s, TRANSFER_DST %s, COLOR_ATTACHMENT %s\n", ok_store ? "yes" : "NO", ok_xfer ? "yes" : "NO", ok_col ? "yes" : "NO");
  if (!ok_xfer) std::printf("(the driver refuses TRANSFER_DST: trying the import anyway)\n");

  DmaBuf dbuf[2];
  Import imp[2];
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  for (int i = 0; i < 2; ++i) {
    if (!dbuf[i].alloc(static_cast<size_t>(W) * H * 4)) { std::printf("no dma-buf of %ux%u\n", W, H); return 1; }
    const char* w = "";
    if (!import(dbuf[i], W, H, usage, imp[i], &w)) { std::printf("import with TRANSFER_DST|COLOR_ATTACHMENT failed: %s\n", w); return 1; }
  }
  std::printf("dma-bufs from %s\n", sdl::dmaheap::chosen());

  // Source: the two screens, as present.comp reads them (SSBO) and as the
  // blit reads them (a 256x384 OPTIMAL image the frame is copied into).
  std::vector<u32> pattern(2 * kFrameWords);
  u32 seed = 0x12345678u;
  for (u32& p : pattern) { seed = seed * 1664525u + 1013904223u; p = 0xFF000000u | (seed >> 8); }
  Buffer ssbo = dev->alloc(sizeof(u32) * 2 * kFrameWords, Access::CpuWrite);
  Buffer stage = dev->alloc(sizeof(u32) * 2 * kFrameWords, Access::CpuWrite);
  Buffer over = dev->alloc(sizeof(u32) * W * H, Access::CpuWrite);
  Buffer tab = dev->alloc(sizeof(u32) * 128, Access::CpuWrite);
  if (!ssbo || !stage || !over || !tab) { std::printf("buffer allocation failed\n"); return 1; }
  std::memset(over.ptr, 0, over.size); std::memset(tab.ptr, 0, tab.size);
  dev->flush(over, 0, over.size); dev->flush(tab, 0, tab.size);

  VkImage simg = VK_NULL_HANDLE; VkDeviceMemory smem = VK_NULL_HANDLE;
  {
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kFmt;
    ii.extent = {SW, 2 * SH, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a->vkCreateImage(vk->dev, &ii, nullptr, &simg) != VK_SUCCESS) { std::printf("source image failed\n"); return 1; }
    VkMemoryRequirements mr{}; a->vkGetImageMemoryRequirements(vk->dev, simg, &mr);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (a->vkAllocateMemory(vk->dev, &ai, nullptr, &smem) != VK_SUCCESS || a->vkBindImageMemory(vk->dev, simg, smem, 0) != VK_SUCCESS) { std::printf("source memory failed\n"); return 1; }
  }

  // The fragment present: a render pass on each import, one triangle over it.
  VkRenderPass rp; VkFramebuffer fbs[2]; VkPipelineLayout gpl; VkPipeline gpipe; VkDescriptorSetLayout gdsl; VkDescriptorSet gset;
  {
    VkAttachmentDescription at{}; at.format = kFmt; at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // the letterbox
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkSubpassDependency dep{}; dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    VkRenderPassCreateInfo rci{}; rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sp; rci.dependencyCount = 1; rci.pDependencies = &dep;
    if (a->vkCreateRenderPass(vk->dev, &rci, nullptr, &rp) != VK_SUCCESS) { std::printf("render pass failed\n"); return 1; }
    for (int i = 0; i < 2; ++i) {
      VkFramebufferCreateInfo fci{}; fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fci.renderPass = rp; fci.attachmentCount = 1;
      fci.pAttachments = &imp[i].view; fci.width = W; fci.height = H; fci.layers = 1;
      if (a->vkCreateFramebuffer(vk->dev, &fci, nullptr, &fbs[i]) != VK_SUCCESS) { std::printf("framebuffer on the import failed\n"); return 1; }
    }
    VkDescriptorSetLayoutBinding b[3]{};
    for (u32 i = 0; i < 3; ++i) { b[i].binding = i + 1; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    VkDescriptorSetLayoutCreateInfo dl{}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dl.bindingCount = 3; dl.pBindings = b;
    a->vkCreateDescriptorSetLayout(vk->dev, &dl, nullptr, &gdsl);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    VkDescriptorPool gpool; a->vkCreateDescriptorPool(vk->dev, &dp, nullptr, &gpool);
    VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; da.descriptorPool = gpool; da.descriptorSetCount = 1; da.pSetLayouts = &gdsl;
    a->vkAllocateDescriptorSets(vk->dev, &da, &gset);
    VkDescriptorBufferInfo bi[3] = {{vk_buf(ssbo), 0, VK_WHOLE_SIZE}, {vk_buf(over), 0, VK_WHOLE_SIZE}, {vk_buf(tab), 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[3]{};
    for (u32 j = 0; j < 3; ++j) { w[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[j].dstSet = gset; w[j].dstBinding = j + 1; w[j].descriptorCount = 1;
      w[j].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[j].pBufferInfo = &bi[j]; }
    a->vkUpdateDescriptorSets(vk->dev, 3, w, 0, nullptr);
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 1; pli.pSetLayouts = &gdsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    a->vkCreatePipelineLayout(vk->dev, &pli, nullptr, &gpl);
    VkShaderModule vm, fm;
    VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = static_cast<size_t>(ds_present_vert_end - ds_present_vert_data); si.pCode = reinterpret_cast<const u32*>(ds_present_vert_data);
    a->vkCreateShaderModule(vk->dev, &si, nullptr, &vm);
    si.codeSize = static_cast<size_t>(ds_present_frag_end - ds_present_frag_data); si.pCode = reinterpret_cast<const u32*>(ds_present_frag_data);
    a->vkCreateShaderModule(vk->dev, &si, nullptr, &fm);
    VkPipelineShaderStageCreateInfo st[2]{};
    for (int i = 0; i < 2; ++i) { st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[i].pName = "main"; }
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fm;
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkViewport vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vs{}; vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vs.viewportCount = 1; vs.pViewports = &vp; vs.scissorCount = 1; vs.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;   // gpu_present's opaque view pipeline
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    const VkDynamicState dyn = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    VkPipelineDynamicStateCreateInfo dys{}; dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dys.dynamicStateCount = 1; dys.pDynamicStates = &dyn;
    VkGraphicsPipelineCreateInfo gp{}; gp.pDynamicState = &dys; gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vs; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs; gp.layout = gpl; gp.renderPass = rp;
    if (a->vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gp, nullptr, &gpipe) != VK_SUCCESS) { std::printf("present.frag pipeline failed\n"); return 1; }
  }

  VkCommandPool cpool; VkCommandBuffer cb; VkFence fence; VkQueryPool qp;
  {
    VkCommandPoolCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; ci.queueFamilyIndex = vk->qfam;
    a->vkCreateCommandPool(vk->dev, &ci, nullptr, &cpool);
    VkCommandBufferAllocateInfo ca{}; ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ca.commandPool = cpool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    a->vkAllocateCommandBuffers(vk->dev, &ca, &cb);
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; a->vkCreateFence(vk->dev, &fi, nullptr, &fence);
    VkQueryPoolCreateInfo qi{}; qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 3;
    a->vkCreateQueryPool(vk->dev, &qi, nullptr, &qp);
  }
  // Record, submit and wait; GPU ms between the timestamps (t1 - t0 and t2 - t1) and the wall time.
  auto run = [&](auto&& record, double* gpu_a, double* gpu_b, double* wall) {
    a->vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    a->vkBeginCommandBuffer(cb, &bi);
    a->vkCmdResetQueryPool(cb, qp, 0, 3);
    a->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
    record();
    a->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2);
    a->vkEndCommandBuffer(cb);
    VkSubmitInfo s{}; s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; s.commandBufferCount = 1; s.pCommandBuffers = &cb;
    a->vkResetFences(vk->dev, 1, &fence);
    const double t0 = now_ms();
    if (a->vkQueueSubmit(vk->queue, 1, &s, fence) != VK_SUCCESS) return false;
    if (a->vkWaitForFences(vk->dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS) return false;
    *wall = now_ms() - t0;
    u64 ts[3] = {};
    a->vkGetQueryPoolResults(vk->dev, qp, 0, 3, sizeof ts, ts, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    *gpu_a = static_cast<double>(ts[1] - ts[0]) * tick_ns / 1e6;
    *gpu_b = static_cast<double>(ts[2] - ts[1]) * tick_ns / 1e6;
    return true;
  };
  const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

  struct Case { const char* name; Rect r; };
  const Case cases[] = {
    {"4x at 0,0", {0, 0, 4 * static_cast<int>(SW), 4 * static_cast<int>(SH)}},
    {"3x at 129,97", {129, 97, 3 * static_cast<int>(SW), 3 * static_cast<int>(SH)}},
  };
  bool pass = true;
  for (const Case& c : cases) {
    if (c.r.x + c.r.w > static_cast<int>(W) || c.r.y + c.r.h > static_cast<int>(H)) { std::printf("%s: does not fit %ux%u, skipped\n", c.name, W, H); continue; }
    std::printf("%s (%dx%d) on %ux%u, both views\n", c.name, c.r.w, c.r.h, W, H);

    // B: nearest blit. The letterbox is cleared once (or every frame), as the RGA path clears it once per buffer.
    for (int v = 0; v < 2; ++v) {
      double ga, gb, wl;
      run([&] {
        VkImageMemoryBarrier ib = img_barrier(imp[v].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        a->vkCmdPipelineBarrier(cb, all, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        a->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 1);
        VkClearColorValue black{}; VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        a->vkCmdClearColorImage(cb, imp[v].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        ib = img_barrier(imp[v].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        a->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
      }, &ga, &gb, &wl);
    }
    for (int clear_each = 0; clear_each < 2; ++clear_each) {
      Stats up, cpy, gpu, wall;
      for (int it = 0; it < kRuns; ++it) {
        const double u0 = now_ms();
        std::memcpy(stage.ptr, pattern.data(), stage.size); dev->flush(stage, 0, stage.size);
        up.add(now_ms() - u0);
        double g = 0, k = 0, w = 0;
        for (int v = 0; v < 2; ++v) {
          double ga, gb, wl;
          if (!run([&] {
                // The screen's frame into the source image (the real present would do both at once).
                VkImageMemoryBarrier sb = img_barrier(simg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                a->vkCmdPipelineBarrier(cb, all, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sb);
                VkBufferImageCopy bc{}; bc.bufferOffset = sizeof(u32) * kFrameWords * static_cast<u32>(v);
                bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageOffset = {0, static_cast<s32>(SH) * v, 0}; bc.imageExtent = {SW, SH, 1};
                a->vkCmdCopyBufferToImage(cb, vk_buf(stage), simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
                sb = img_barrier(simg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                // The scanout keeps its contents (the letterbox) unless cleared each frame.
                VkImageMemoryBarrier db = img_barrier(imp[v].img, clear_each ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageMemoryBarrier two[2] = {sb, db};
                a->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, two);
                a->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, qp, 1);
                if (clear_each) {
                  VkClearColorValue black{}; VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                  a->vkCmdClearColorImage(cb, imp[v].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
                  VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                  a->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                }
                VkImageBlit bl{};
                bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                bl.srcOffsets[0] = {0, static_cast<s32>(SH) * v, 0}; bl.srcOffsets[1] = {static_cast<s32>(SW), static_cast<s32>(SH) * (v + 1), 1};
                bl.dstOffsets[0] = {c.r.x, c.r.y, 0}; bl.dstOffsets[1] = {c.r.x + c.r.w, c.r.y + c.r.h, 1};
                a->vkCmdBlitImage(cb, simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imp[v].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
                db = img_barrier(imp[v].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
                a->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &db);
              }, &ga, &gb, &wl)) { std::printf("  blit: submit failed\n"); return 1; }
          k += ga; g += ga + gb; w += wl;
        }
        cpy.add(k); gpu.add(g); wall.add(w);
      }
      std::printf("  B nearest blit%s GPU median %.3f ms p90 %.3f max %.3f (frame copy %.3f) | submit-to-fence median %.3f p90 %.3f | CPU upload %.3f\n",
                  clear_each ? " + clear      " : "              ", gpu.pct(0.5), gpu.pct(0.9), gpu.pct(1.0), cpy.pct(0.5), wall.pct(0.5), wall.pct(0.9), up.pct(0.5));
      for (int v = 0; v < 2; ++v) pass &= check(dbuf[v], W, H, pattern.data() + v * kFrameWords, c.r);
    }
    // C: the fragment present, as gpu_present.cpp records it.
    {
      Stats up, gpu, wall;
      for (int it = 0; it < kRuns; ++it) {
        const double u0 = now_ms();
        std::memcpy(ssbo.ptr, pattern.data(), ssbo.size); dev->flush(ssbo, 0, ssbo.size);
        up.add(now_ms() - u0);
        double g = 0, w = 0;
        for (int v = 0; v < 2; ++v) {
          Push pc{}; pc.rect[0] = c.r.x; pc.rect[1] = c.r.y; pc.rect[2] = c.r.w; pc.rect[3] = c.r.h;
          pc.a[0] = W; pc.a[1] = H; pc.a[2] = W; pc.a[3] = H;
          pc.b[0] = 0; pc.b[1] = 0; pc.b[2] = static_cast<u32>(v) * kFrameWords; pc.b[3] = 256u << 16;
          pc.s[0] = 256.0f / static_cast<float>(c.r.w); pc.s[1] = 192.0f / static_cast<float>(c.r.h);
          pc.s[2] = 0.5f / static_cast<float>(c.r.w); pc.s[3] = 0.5f / static_cast<float>(c.r.h);
          double ga, gb, wl;
          if (!run([&] {
                a->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 1);
                VkClearValue black{};
                VkRenderPassBeginInfo rb{}; rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass = rp; rb.framebuffer = fbs[v]; rb.renderArea = {{0, 0}, {W, H}};
                rb.clearValueCount = 1; rb.pClearValues = &black;
                a->vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
                a->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
                a->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpl, 0, 1, &gset, 0, nullptr);
                const float bc[4] = {1, 1, 1, 1};
                a->vkCmdSetBlendConstants(cb, bc);
                a->vkCmdPushConstants(cb, gpl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
                a->vkCmdDraw(cb, 4, 1, 0, 0);
                a->vkCmdEndRenderPass(cb);
              }, &ga, &gb, &wl)) { std::printf("  fragment: submit failed\n"); return 1; }
          g += gb; w += wl;
        }
        gpu.add(g); wall.add(w);
      }
      std::printf("  C fragment (present.frag)    GPU median %.3f ms p90 %.3f max %.3f | submit-to-fence median %.3f p90 %.3f | CPU upload %.3f\n",
                  gpu.pct(0.5), gpu.pct(0.9), gpu.pct(1.0), wall.pct(0.5), wall.pct(0.9), up.pct(0.5));
      for (int v = 0; v < 2; ++v) pass &= check(dbuf[v], W, H, pattern.data() + v * kFrameWords, c.r);
    }
  }
  a->vkDeviceWaitIdle(vk->dev);
  std::printf("%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
