// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_shaders.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Compute shaders linked in as SPIR-V via .incbin; no shader compiler needed to build.

#define DS_INCBIN(sym, file)                                     \
  __asm__(".section .rodata\n"                                   \
          ".balign 4\n"                                          \
          ".globl " #sym "_data\n"                               \
          #sym "_data:\n"                                        \
          ".incbin \"" DSPERATE_VK_SHADER_DIR "/" file "\"\n"    \
          ".globl " #sym "_end\n"                                \
          #sym "_end:\n"                                         \
          ".previous\n");                                        \
  extern "C" const unsigned char sym##_data[], sym##_end[]

DS_INCBIN(ds_vk_tri_fs, "tri_fs_vert.spv");
DS_INCBIN(ds_vk_lean_null, "lean_null_frag.spv");
DS_INCBIN(ds_vk_lean_vert, "lean_vert.spv");
DS_INCBIN(ds_vk_lean_frag, "lean_frag.spv");
DS_INCBIN(ds_vk_lean_frag_early, "lean_frag_early.spv");
DS_INCBIN(ds_vk_lean_frag_opq, "lean_frag_opq.spv");
DS_INCBIN(ds_vk_lean_frag_trans, "lean_frag_trans.spv");
DS_INCBIN(ds_vk_lean_frag_fetch, "lean_frag_fetch.spv");
DS_INCBIN(ds_vk_lean_frag_fetch_ds, "lean_frag_fetch_ds.spv");
DS_INCBIN(ds_vk_lean_resolve, "lean_resolve_frag.spv");
DS_INCBIN(ds_vk_lean_resolve_1x, "lean_resolve_1x_frag.spv");
DS_INCBIN(ds_vk_lean_fog, "lean_fog_frag.spv");
DS_INCBIN(ds_vk_lean_down, "lean_down_frag.spv");
DS_INCBIN(ds_vk_lean_msdec, "lean_msdec_frag.spv");
DS_INCBIN(ds_vk_lean_crack, "lean_crack_frag.spv");
DS_INCBIN(ds_vk_lean_mark, "lean_mark_frag.spv");

namespace ds::gpu::vk {

namespace {
// DS_VK_SPV_DIR=DIR (diagnostics): a shader whose file (the name it is built
// as, e.g. lean_frag.spv) is in DIR is taken from there instead of the built-in
// copy, so a variant can be timed on the device without a rebuild. Read once and kept.
Spirv pick(const char* file, const unsigned char* data, const unsigned char* end) {
  if (const char* dir = std::getenv("DS_VK_SPV_DIR"); dir && *dir) {
    const std::string path = std::string(dir) + "/" + file;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
      auto* v = new std::vector<u32>();
      u32 w;
      while (std::fread(&w, 4, 1, f) == 1) v->push_back(w);
      std::fclose(f);
      if (!v->empty()) { std::fprintf(stderr, "vk: shader %s from %s\n", file, path.c_str()); return { v->data(), v->size() * 4 }; }
      delete v;
    }
  }
  return { reinterpret_cast<const u32*>(data), static_cast<size_t>(end - data) };
}
}  // namespace

Spirv shader_tri_fs() { return pick("tri_fs_vert.spv", ds_vk_tri_fs_data, ds_vk_tri_fs_end); }
Spirv shader_lean_null() { return pick("lean_null_frag.spv", ds_vk_lean_null_data, ds_vk_lean_null_end); }

Spirv shader_lean_vert() { return pick("lean_vert.spv", ds_vk_lean_vert_data, ds_vk_lean_vert_end); }
Spirv shader_lean_frag() { return pick("lean_frag.spv", ds_vk_lean_frag_data, ds_vk_lean_frag_end); }
Spirv shader_lean_frag_opq() { return pick("lean_frag_opq.spv", ds_vk_lean_frag_opq_data, ds_vk_lean_frag_opq_end); }
Spirv shader_lean_frag_trans() { return pick("lean_frag_trans.spv", ds_vk_lean_frag_trans_data, ds_vk_lean_frag_trans_end); }
Spirv shader_lean_frag_early() { return pick("lean_frag_early.spv", ds_vk_lean_frag_early_data, ds_vk_lean_frag_early_end); }
Spirv shader_lean_frag_fetch(bool ds_blend) {
  return ds_blend ? pick("lean_frag_fetch_ds.spv", ds_vk_lean_frag_fetch_ds_data, ds_vk_lean_frag_fetch_ds_end) : pick("lean_frag_fetch.spv", ds_vk_lean_frag_fetch_data, ds_vk_lean_frag_fetch_end);
}
Spirv shader_lean_resolve_1x() { return pick("lean_resolve_1x_frag.spv", ds_vk_lean_resolve_1x_data, ds_vk_lean_resolve_1x_end); }
Spirv shader_lean_mark() { return pick("lean_mark_frag.spv", ds_vk_lean_mark_data, ds_vk_lean_mark_end); }
Spirv shader_lean_down() { return pick("lean_down_frag.spv", ds_vk_lean_down_data, ds_vk_lean_down_end); }
Spirv shader_lean_msdec() { return pick("lean_msdec_frag.spv", ds_vk_lean_msdec_data, ds_vk_lean_msdec_end); }
Spirv shader_lean_crack() { return pick("lean_crack_frag.spv", ds_vk_lean_crack_data, ds_vk_lean_crack_end); }
Spirv shader_lean_fog() { return pick("lean_fog_frag.spv", ds_vk_lean_fog_data, ds_vk_lean_fog_end); }
Spirv shader_lean_resolve() { return pick("lean_resolve_frag.spv", ds_vk_lean_resolve_data, ds_vk_lean_resolve_end); }
#define DS_SPIRV(fn, sym, file) Spirv fn() { return pick(file, sym##_data, sym##_end); }
#undef DS_SPIRV
} // namespace ds::gpu::vk
