// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gl/gl_shaders.h"

#include <cstring>

// GLSL ES text linked in with .incbin, as vk_shaders.cpp links the SPIR-V.

#define DS_INCBIN(sym, file)                                     \
  __asm__(".section .rodata\n"                                   \
          ".balign 4\n"                                          \
          ".globl " #sym "_data\n"                               \
          #sym "_data:\n"                                        \
          ".incbin \"" file "\"\n"                               \
          ".globl " #sym "_end\n"                                \
          #sym "_end:\n"                                         \
          ".byte 0\n"                                            \
          ".previous\n");                                        \
  extern "C" const char sym##_data[], sym##_end[]

DS_INCBIN(ds_gl_common, DSPERATE_GL_SHADER_DIR "/gl_common.glsl");
DS_INCBIN(ds_gl_pull, DSPERATE_GL_SHADER_DIR "/gl_pull.glsl");
DS_INCBIN(ds_gl_vk_layout, DSPERATE_VK_DIR "/vk_layout.h");
DS_INCBIN(ds_gl_ds_shade, DSPERATE_VK_DIR "/ds_shade.glsl");
DS_INCBIN(ds_gl_ds_spans, DSPERATE_VK_DIR "/ds_spans.glsl");
DS_INCBIN(ds_gl_lean_vert, DSPERATE_GL_SHADER_DIR "/gl_lean.vert");
DS_INCBIN(ds_gl_lean_frag, DSPERATE_GL_SHADER_DIR "/gl_lean.frag");
DS_INCBIN(ds_gl_resolve_frag, DSPERATE_GL_SHADER_DIR "/gl_resolve.frag");
DS_INCBIN(ds_gl_edge_vert, DSPERATE_GL_SHADER_DIR "/gl_edge.vert");
DS_INCBIN(ds_gl_edge_frag, DSPERATE_GL_SHADER_DIR "/gl_edge.frag");
DS_INCBIN(ds_gl_cutout_frag, DSPERATE_GL_SHADER_DIR "/gl_cutout.frag");
DS_INCBIN(ds_gl_fs_vert, DSPERATE_GL_SHADER_DIR "/gl_fs.vert");
DS_INCBIN(ds_gl_null_frag, DSPERATE_GL_SHADER_DIR "/gl_null.frag");

namespace ds::gpu::gl {

#define DS_TEXT(fn, sym) Text fn() { return { sym##_data, static_cast<size_t>(sym##_end - sym##_data) }; }
DS_TEXT(text_gl_common, ds_gl_common)
DS_TEXT(text_gl_pull, ds_gl_pull)
DS_TEXT(text_vk_layout, ds_gl_vk_layout)
DS_TEXT(text_ds_shade, ds_gl_ds_shade)
DS_TEXT(text_ds_spans, ds_gl_ds_spans)
DS_TEXT(text_lean_vert, ds_gl_lean_vert)
DS_TEXT(text_lean_frag, ds_gl_lean_frag)
DS_TEXT(text_resolve_frag, ds_gl_resolve_frag)
DS_TEXT(text_edge_vert, ds_gl_edge_vert)
DS_TEXT(text_edge_frag, ds_gl_edge_frag)
DS_TEXT(text_cutout_frag, ds_gl_cutout_frag)
DS_TEXT(text_fs_vert, ds_gl_fs_vert)
DS_TEXT(text_null_frag, ds_gl_null_frag)
#undef DS_TEXT

namespace {
// #include "name" lines replaced by the named text; an unknown name (the
// C++ side of vk_layout.h) by nothing. Line-by-line, recursive.
void resolve(const std::string& in, std::string& out) {
  size_t pos = 0;
  while (pos < in.size()) {
    size_t nl = in.find('\n', pos);
    if (nl == std::string::npos) nl = in.size();
    const std::string line = in.substr(pos, nl - pos);
    size_t h = line.find_first_not_of(" \t");
    if (h != std::string::npos && line.compare(h, 8, "#include") == 0) {
      size_t q0 = line.find('"', h), q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 1);
      const std::string name = q1 == std::string::npos ? "" : line.substr(q0 + 1, q1 - q0 - 1);
      Text t;
      if (name == "vk_layout.h") t = text_vk_layout();
      else if (name == "ds_shade.glsl") t = text_ds_shade();
      else if (name == "ds_spans.glsl") t = text_ds_spans();
      else if (name == "gl_common.glsl") t = text_gl_common();
      else if (name == "gl_pull.glsl") t = text_gl_pull();
      if (t.data) resolve(std::string(t.data, t.size), out);
      out += '\n';
    } else { out += line; out += '\n'; }
    pos = nl + 1;
  }
}
}

std::string source(Text stage, const std::string& defines) {
  const std::string in(stage.data, stage.size);
  // The prelude goes after #version and the #extension lines that must lead.
  size_t pos = 0;
  for (;;) {
    size_t nl = in.find('\n', pos);
    if (nl == std::string::npos) { pos = in.size(); break; }
    const std::string line = in.substr(pos, nl - pos);
    const bool lead = pos == 0 || line.compare(0, 10, "#extension") == 0;
    if (!lead) break;
    pos = nl + 1;
  }
  std::string spliced = in.substr(0, pos) + defines + "#include \"gl_common.glsl\"\n" + in.substr(pos);
  std::string out;
  out.reserve(spliced.size() + 16384);
  resolve(spliced, out);
  return out;
}

} // namespace ds::gpu::gl
