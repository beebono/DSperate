// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gl/gl_lean.h"

#include "core/gpu/gl/gl_api.h"
#include "core/gpu/gl/gl_shaders.h"
#include "core/gpu/vk/vk_seams.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace ds::gpu::gl {

using namespace ds::gpu::vk;   // the layout types (vk_layout.h)

namespace {
constexpr u32 W = 256, H = 192;
constexpr u32 kTexelWords = 2u << 20;   // 8 MB of decoded texels, as the Vulkan raster's arena
constexpr u32 kMaxDraws = 4096;
constexpr u32 kPullW = 1024;            // the polygon/vertex textures' width in texels
// The stencil layout and the pass kinds, vk_lean.cpp's.
constexpr u32 S_DRAWN = 0x80, S_MASK = 0x40, S_T = 0x20, S_CODE = 0x1F;
enum Pipe : u32 { P_OPAQUE = 0, P_TRANS_A, P_TRANS_B, P_TRANS_A_DW, P_TRANS_B_DW, P_MASK, P_SHADOW, P_SHADOW_PREP, P_MASK_CLEAR, P_OPAQUE_EARLY, P_TRANS_OPQ, P_COUNT };
enum Variant : u32 { V_PLAIN = 0, V_EARLY, V_OPQ, V_TRANS, V_COUNT };   // the fragment stage's build
double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

struct Lean::Impl {
  Api* a = nullptr;
  EGLDisplay dpy = nullptr;
  EGLContext ctx = nullptr;
  EGLSurface surf = nullptr;
  bool edges = false, timing = false, indexed = false, egl_sync = false, nostencil = false, wclip = false, span_exact = false;
  static constexpr u32 kSlots = 4;
  // Host staging as the Vulkan raster has it; the slot's device copies are
  // the persistently mapped buffers below (and the vertex stage's textures).
  std::vector<GpuPoly> host_polys, sorted;
  std::vector<GpuVert> host_verts;
  vk::SeamTable seams;   // the band pass's shared-edge marks (vk_seams.h)
  std::vector<u32> host_spans;
  GpuPost host_post{};
  std::vector<std::pair<s64, u32>> key;
  GLuint ssbo_polys[kSlots] = {}, ssbo_spans[kSlots] = {}, ssbo_post[kSlots] = {}, ubo_frame[kSlots] = {}, ssbo_texels = 0;
  void* map_polys[kSlots] = {}; void* map_spans[kSlots] = {}; void* map_post[kSlots] = {}; void* map_frame[kSlots] = {}; u32* map_texels = nullptr;
  GLuint tex_polys[kSlots] = {}, tex_verts[kSlots] = {};
  GLuint t_col = 0, t_fog = 0, t_ids = 0, t_ds = 0, t_ebyte = 0, t_res = 0;
  GLuint fbo_draw = 0, fbo_edge = 0, fbo_res = 0;
  GLuint pbo_out[kSlots] = {}, pbo_edge[kSlots] = {};
  const u32* map_out[kSlots] = {}; const u8* map_edge[kSlots] = {};
  // The record and the edge bytes copied out of the mapped PBOs (uncached memory on Mali) into host memory the compositor can read (finish_slot).
  std::vector<u32> out_host[kSlots];
  std::vector<u8> edge_bytes[kSlots];
  GLuint prog_draw[2][V_COUNT] = {};   // [wdepth][variant]
  GLuint prog_prep = 0, prog_mask = 0, prog_resolve = 0, prog_band = 0, prog_cutout = 0;
  GLint loc_base_draw[2][V_COUNT] = {}, loc_base_prep = -1, loc_base_band = -1, loc_base_cutout = -1;
  GLuint vao = 0;
  GLuint query[kSlots] = {};
  // The slot state: submit() on the job thread, the readers on any. A
  // collector thread waits each frame's fence in order (EGL syncs need no
  // context) and copies its record out of the mapped PBO (uncached memory
  // on Mali: ~2.5 ms of reads a frame, which the compositor must not pay)
  // into out_host; `live` clears when the copy is in, and `cv` wakes the
  // readers and the submit reusing a slot.
  std::mutex m;
  std::condition_variable cv;
  std::thread collector;
  bool quit = false;
  u64 collected = 0;   // frames copied out (the collector's gen)
  EGLSyncKHR sync[kSlots] = {};
  bool live[kSlots] = {}, valid[kSlots] = {};
  u64 gen = 0, asked_gen = 0;
  double submit_at[kSlots] = {};
  u64 last_gpu_ns = 0;
  u32 last_draws = 0;
  Stats st;
  // State cache for the draw passes.
  GLuint bound_prog = 0;

  bool signaled(u32 slot) {
    if (!sync[slot]) return true;
    EGLint v = 0;
    return a->eglGetSyncAttribKHR(dpy, sync[slot], EGL_SYNC_STATUS_KHR, &v) && v == EGL_SIGNALED_KHR;
  }
  void wait_sync(u32 slot) { if (sync[slot]) a->eglClientWaitSyncKHR(dpy, sync[slot], 0, EGL_FOREVER_KHR); }
  void drop_sync(u32 slot) { if (sync[slot]) { a->eglDestroySyncKHR(dpy, sync[slot]); sync[slot] = nullptr; } }
  void bind_thread() { if (a->eglGetCurrentContext() != ctx) a->eglMakeCurrent(dpy, surf, surf, ctx); }
  // A pass kind's program and state (set_state), the stencil reference per run; the program's u_base location.
  GLint use_kind(u32 kind, bool wbuf, bool eq, u32 ref);
  // Every program drawn once in every pass kind's state at creation, so the
  // driver compiles its variants (Mali: blend and masks fold into the
  // fragment shader) before the first frame that needs them, not during it
  // (20-45 ms stalls on the G31). The polygon texture holds zeros: nothing is drawn.
  void warm();

  // The collector: frames in submit order, each waited and copied out.
  void collect_loop() {
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
      cv.wait(lk, [this] { return quit || collected < gen; });
      if (quit) return;
      const u64 g = collected + 1;
      const u32 slot = static_cast<u32>((g - 1) % kSlots);
      EGLSyncKHR sy = sync[slot];
      lk.unlock();
      if (sy) a->eglClientWaitSyncKHR(dpy, sy, 0, EGL_FOREVER_KHR);
      std::memcpy(out_host[slot].data(), map_out[slot], sizeof(u32) * W * H);
      if (edges) { const u8* e = map_edge[slot]; u8* o = edge_bytes[slot].data(); for (u32 k = 0; k < W * H; ++k) o[k] = e[4 * k]; }
      lk.lock();
      live[slot] = false; valid[slot] = true; collected = g;
      cv.notify_all();
    }
  }
  void destroy() {
    if (!a || !dpy) return;
    if (collector.joinable()) { { std::lock_guard<std::mutex> lk(m); quit = true; } cv.notify_all(); collector.join(); }
    for (u32 i = 0; i < kSlots; ++i) { if (live[i]) wait_sync(i); drop_sync(i); live[i] = false; }
    if (ctx && a->eglGetCurrentContext() == ctx) {
      // Ours on this thread: the objects go now. On another thread (the
      // job thread, which keeps its last context) EGL frees them with the
      // context once that thread binds another.
      for (auto& row : prog_draw) for (GLuint p : row) if (p) a->glDeleteProgram(p);
      for (GLuint p : {prog_prep, prog_mask, prog_resolve, prog_band, prog_cutout}) if (p) a->glDeleteProgram(p);
      for (u32 i = 0; i < kSlots; ++i) {
        GLuint bufs[6] = {ssbo_polys[i], ssbo_spans[i], ssbo_post[i], ubo_frame[i], pbo_out[i], pbo_edge[i]};
        a->glDeleteBuffers(6, bufs);
        GLuint texs[2] = {tex_polys[i], tex_verts[i]};
        a->glDeleteTextures(2, texs);
      }
      if (ssbo_texels) a->glDeleteBuffers(1, &ssbo_texels);
      GLuint texs[6] = {t_col, t_fog, t_ids, t_ds, t_ebyte, t_res};
      a->glDeleteTextures(6, texs);
      GLuint fbos[3] = {fbo_draw, fbo_edge, fbo_res};
      a->glDeleteFramebuffers(3, fbos);
      if (vao) a->glDeleteVertexArrays(1, &vao);
      if (timing && a->glDeleteQueriesEXT) a->glDeleteQueriesEXT(kSlots, query);
      a->eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (ctx) a->eglDestroyContext(dpy, ctx);
    if (surf) a->eglDestroySurface(dpy, surf);
    ctx = nullptr; surf = nullptr;
  }
};

Lean::~Lean() { if (d_) d_->destroy(); }
bool Lean::edges() const { return d_->edges; }
GpuPoly* Lean::poly_buffer() { return d_->host_polys.data(); }
GpuVert* Lean::vert_buffer() { return d_->host_verts.data(); }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = kTexelWords; return d_->map_texels; }
GpuPost* Lean::post_buffer() { return &d_->host_post; }
u32* Lean::span_buffer() { return d_->host_spans.data(); }
const u32* Lean::output() { return frame_ready(0, false); }
bool Lean::has_frame() const { return d_->gen != 0; }
u64 Lean::gpu_ns() const { return d_->last_gpu_ns; }
u32 Lean::draws() const { return d_->last_draws; }
Lean::Stats Lean::stats(bool reset) { std::lock_guard<std::mutex> lk(d_->m); Stats s = d_->st; if (reset) d_->st = Stats{}; return s; }
const u8* Lean::edges_for(const u32* o) const {
  if (!d_->edges) return nullptr;
  for (u32 i = 0; i < Impl::kSlots; ++i) if (o == d_->out_host[i].data()) return d_->edge_bytes[i].data();
  return nullptr;
}

namespace {
bool has_gl_ext(Api& a, const char* name) {
  GLint n = 0; a.glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for (GLint i = 0; i < n; ++i) { const char* e = reinterpret_cast<const char*>(a.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))); if (e && !std::strcmp(e, name)) return true; }
  return false;
}
bool has_egl_ext(const char* list, const char* name) {
  if (!list) return false;
  const size_t n = std::strlen(name);
  for (const char* p = list; (p = std::strstr(p, name)) != nullptr; p += n) if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
  return false;
}
GLuint compile(Api& a, GLenum kind, const std::string& src, std::string* why) {
  GLuint s = a.glCreateShader(kind);
  const char* p = src.c_str(); const GLint len = static_cast<GLint>(src.size());
  a.glShaderSource(s, 1, &p, &len);
  a.glCompileShader(s);
  GLint ok = 0; a.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048] = {}; a.glGetShaderInfoLog(s, sizeof log - 1, nullptr, log);
    if (why) *why = std::string("shader: ") + log;
    if (std::getenv("DS_GL_DUMP_SHADERS")) std::fprintf(stderr, "---- rejected shader ----\n%s\n---- log ----\n%s\n", src.c_str(), log);
    a.glDeleteShader(s);
    return 0;
  }
  return s;
}
GLuint link(Api& a, GLuint vs, GLuint fs, std::string* why) {
  if (!vs || !fs) return 0;
  GLuint p = a.glCreateProgram();
  a.glAttachShader(p, vs); a.glAttachShader(p, fs);
  a.glLinkProgram(p);
  GLint ok = 0; a.glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { char log[2048] = {}; a.glGetProgramInfoLog(p, sizeof log - 1, nullptr, log); if (why) *why = std::string("link: ") + log; a.glDeleteProgram(p); return 0; }
  return p;
}
void sampler(Api& a, GLuint prog, const char* name, int unit) { const GLint l = a.glGetUniformLocation(prog, name); if (l >= 0) a.glUniform1i(l, unit); }
GLuint tex2d(Api& a, GLenum fmt, u32 w, u32 h) {
  GLuint t = 0; a.glGenTextures(1, &t); a.glBindTexture(GL_TEXTURE_2D, t);
  a.glTexStorage2D(GL_TEXTURE_2D, 1, fmt, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
  a.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); a.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  a.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); a.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}
// A persistently mapped buffer (EXT_buffer_storage): `read` for the PBOs.
// The storage buffers are created through the ARRAY_BUFFER target: the
// Mali-G31 blob (r20p0) refuses MAP_COHERENT on a buffer first bound as
// SHADER_STORAGE_BUFFER (GL_OUT_OF_MEMORY), and a buffer's target is not a type.
void* persistent(Api& a, GLenum target, GLuint* buf, size_t size, bool read) {
  if (target == GL_SHADER_STORAGE_BUFFER) target = GL_ARRAY_BUFFER;
  a.glGenBuffers(1, buf); a.glBindBuffer(target, *buf);
  const GLbitfield fl = (read ? GL_MAP_READ_BIT : GL_MAP_WRITE_BIT) | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
  a.glBufferStorageEXT(target, static_cast<GLsizeiptr>(size), nullptr, fl);
  void* p = a.glMapBufferRange(target, 0, static_cast<GLsizeiptr>(size), fl);
  a.glBindBuffer(target, 0);
  return p;
}
}

std::unique_ptr<Lean> Lean::create(bool aa, bool edges, std::string* why) {
  (void)aa;   // no MSAA on this backend: the edge cut is its AA
  auto fail_null = [&](const char* m) { if (why) *why = m; return nullptr; };
  if (std::getenv("DS_GL3D") && std::atoi(std::getenv("DS_GL3D")) == 0) return fail_null("disabled (DS_GL3D=0)");
  Api& a = gl_api();
  if (!a.load(why)) return nullptr;
  std::unique_ptr<Lean> self(new Lean);
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  d.a = &a; d.edges = edges;
  auto fail = [&](const char* m) { if (why) { *why = m; if (a.glGetError) { const GLenum e = a.glGetError(); if (e) *why += " (GL error " + std::to_string(e) + ")"; } } return nullptr; };

  // EGL: the default display, an ES 3.1 context on no surface (a 1x1
  // pbuffer where the driver wants one). The display is initialised once
  // and never terminated: the frontend may share it.
  d.dpy = a.eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!d.dpy) return fail("eglGetDisplay failed");
  EGLint maj = 0, min = 0;
  if (!a.eglInitialize(d.dpy, &maj, &min)) return fail("eglInitialize failed");
  const char* egl_exts = a.eglQueryString(d.dpy, EGL_EXTENSIONS);
  a.eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
  EGLConfig cfg = nullptr; EGLint ncfg = 0;
  if (!a.eglChooseConfig(d.dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) return fail("no OpenGL ES 3 config");
  const bool create_ctx = has_egl_ext(egl_exts, "EGL_KHR_create_context");
  const EGLint ctx_attrs31[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
  const EGLint ctx_attrs3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  d.ctx = a.eglCreateContext(d.dpy, cfg, EGL_NO_CONTEXT, create_ctx ? ctx_attrs31 : ctx_attrs3);
  if (!d.ctx && create_ctx) d.ctx = a.eglCreateContext(d.dpy, cfg, EGL_NO_CONTEXT, ctx_attrs3);
  if (!d.ctx) return fail("eglCreateContext failed");
  if (!has_egl_ext(egl_exts, "EGL_KHR_surfaceless_context")) {
    const EGLint pb[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    d.surf = a.eglCreatePbufferSurface(d.dpy, cfg, pb);
    if (!d.surf) return fail("eglCreatePbufferSurface failed");
  }
  if (!a.eglMakeCurrent(d.dpy, d.surf, d.surf, d.ctx)) return fail("eglMakeCurrent failed");
  struct Release { Api& a; Impl& d; ~Release() { a.eglMakeCurrent(d.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); } } release{a, d};
  // ES 3.1: storage buffers in the fragment stage, instanced draws, integer attachments.
  GLint gmaj = 0, gmin = 0; a.glGetIntegerv(0x821B /* GL_MAJOR_VERSION */, &gmaj); a.glGetIntegerv(0x821C /* GL_MINOR_VERSION */, &gmin);
  if (gmaj < 3 || (gmaj == 3 && gmin < 1)) return fail("OpenGL ES 3.1 needed");
  GLint fssbo = 0; a.glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fssbo);
  if (fssbo < 4) return fail("fewer than four fragment storage blocks");
  if (!a.glBufferStorageEXT || !(has_gl_ext(a, "GL_EXT_buffer_storage") || gmaj > 3)) return fail("no EXT_buffer_storage");
  d.indexed = a.glColorMaski && a.glEnablei && a.glDisablei && a.glBlendFuncSeparatei && a.glBlendEquationSeparatei &&
              (has_gl_ext(a, "GL_EXT_draw_buffers_indexed") || has_gl_ext(a, "GL_OES_draw_buffers_indexed") || (gmaj == 3 && gmin >= 2) || gmaj > 3);
  d.egl_sync = has_egl_ext(egl_exts, "EGL_KHR_fence_sync") && a.eglCreateSyncKHR && a.eglClientWaitSyncKHR && a.eglGetSyncAttribKHR;
  d.timing = (std::getenv("DS_GL_TIMING") || std::getenv("DS_VK_TIMING")) && has_gl_ext(a, "GL_EXT_disjoint_timer_query") && a.glGenQueriesEXT && a.glGetQueryObjectui64vEXT;
  d.nostencil = std::getenv("DS_LEAN_NOSTENCIL") && std::atoi(std::getenv("DS_LEAN_NOSTENCIL")) != 0;

  // Staging and the mapped buffers.
  d.host_polys.resize(DS_MAX_POLYS); d.sorted.resize(DS_MAX_POLYS);
  d.host_verts.resize(DS_MAX_VERTS);
  d.host_spans.resize(DS_SPAN_TABLE_WORDS);
  d.map_texels = static_cast<u32*>(persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_texels, sizeof(u32) * kTexelWords, false));
  if (!d.map_texels) return fail("texel arena");
  for (u32 i = 0; i < Impl::kSlots; ++i) {
    d.map_polys[i] = persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_polys[i], sizeof(GpuPoly) * DS_MAX_POLYS, false);
    d.map_spans[i] = persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_spans[i], sizeof(u32) * DS_SPAN_TABLE_WORDS, false);
    d.map_post[i] = persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_post[i], sizeof(GpuPost), false);
    d.map_frame[i] = persistent(a, GL_UNIFORM_BUFFER, &d.ubo_frame[i], sizeof(GpuFrame), false);
    d.map_out[i] = static_cast<const u32*>(persistent(a, GL_PIXEL_PACK_BUFFER, &d.pbo_out[i], sizeof(u32) * W * H, true));
    if (!d.map_polys[i] || !d.map_spans[i] || !d.map_post[i] || !d.map_frame[i] || !d.map_out[i]) return fail("buffer mapping");
    d.out_host[i].assign(W * H, 0);
    if (d.edges) {
      d.map_edge[i] = static_cast<const u8*>(persistent(a, GL_PIXEL_PACK_BUFFER, &d.pbo_edge[i], 4 * W * H, true));
      if (!d.map_edge[i]) return fail("edge buffer mapping");
      d.edge_bytes[i].assign(W * H, 0);
    }
    d.tex_polys[i] = tex2d(a, GL_RGBA32UI, kPullW, DS_MAX_POLYS * 4 / kPullW);
    d.tex_verts[i] = tex2d(a, GL_RGBA32UI, kPullW, DS_MAX_VERTS * 3 / kPullW);
  }
  static_assert(sizeof(GpuPoly) == 64 && sizeof(GpuVert) == 48, "gl_pull.glsl reads four and three texels");
  static_assert((DS_MAX_POLYS * 4) % kPullW == 0 && (DS_MAX_VERTS * 3) % kPullW == 0, "whole texture rows");

  // Attachments: colour, fog bit, the polygon record (edges), depth/stencil;
  // the edge bytes; the resolved record. Float depth first, as vk_lean.cpp.
  d.t_col = tex2d(a, GL_RGBA8, W, H);
  d.t_fog = tex2d(a, GL_R8, W, H);
  if (d.edges) { d.t_ids = tex2d(a, GL_R16UI, W, H); d.t_ebyte = tex2d(a, GL_RGBA8, W, H); }
  d.t_res = tex2d(a, GL_RGBA8, W, H);
  const bool d24 = std::getenv("DS_LEAN_D24") && std::atoi(std::getenv("DS_LEAN_D24")) != 0;
  for (int attempt = d24 ? 1 : 0; attempt < 2 && !d.fbo_draw; ++attempt) {
    GLuint ds = tex2d(a, attempt == 0 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8, W, H);
    GLuint fbo = 0; a.glGenFramebuffers(1, &fbo); a.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.t_col, 0);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, d.t_fog, 0);
    if (d.edges) a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 2, GL_TEXTURE_2D, d.t_ids, 0);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2};
    a.glDrawBuffers(d.edges ? 3 : 2, bufs);
    if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) { d.fbo_draw = fbo; d.t_ds = ds; }
    else { a.glDeleteFramebuffers(1, &fbo); a.glDeleteTextures(1, &ds); }
  }
  if (!d.fbo_draw) return fail("draw framebuffer incomplete");
  a.glGenFramebuffers(1, &d.fbo_res); a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_res);
  a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.t_res, 0);
  if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return fail("resolve framebuffer incomplete");
  if (d.edges) {
    a.glGenFramebuffers(1, &d.fbo_edge); a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_edge);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.t_ebyte, 0);
    if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return fail("edge framebuffer incomplete");
  }
  a.glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Programs: the draw's vertex stage with the fragment variants (x the
  // W-buffer depth write), the stencil-only pair, the resolve, the edge passes.
  {
    std::string err;
    // W-buffered frames: the clip z carries wscale / w (early depth test;
    // ~25% less GPU time on On Tour than gl_FragDepth, which forces late
    // depth) with the frame's nearest w as the scale so the far end keeps
    // its precision (gl_pull.glsl, ds_clip_z). DS_GL_WCLIP=0: gl_FragDepth
    // from the perspective-correct w instead, exact to the DS's 24 bits.
    const bool wclip = !(std::getenv("DS_GL_WCLIP") && std::atoi(std::getenv("DS_GL_WCLIP")) == 0);
    d.wclip = wclip;
    const std::string vdefs = wclip ? "#define DS_WCLIP 1\n" : "";
    // DS_SPAN_EXACT=1: the span table's exact integer interpolation along a
    // row instead of its float form (ds_spans.glsl), for the draw and the cut-out pass.
    d.span_exact = std::getenv("DS_SPAN_EXACT") && std::atoi(std::getenv("DS_SPAN_EXACT")) != 0;
    const std::string sdefs = d.span_exact ? "#define DS_SPAN_EXACT 1\n" : "";
    const GLuint vs_lean = compile(a, GL_VERTEX_SHADER, source(text_lean_vert(), vdefs), &err);
    const GLuint vs_fs = compile(a, GL_VERTEX_SHADER, source(text_fs_vert(), ""), &err);
    const GLuint fs_null = compile(a, GL_FRAGMENT_SHADER, source(text_null_frag(), ""), &err);
    if (!vs_lean || !vs_fs || !fs_null) { if (why) *why = err; return nullptr; }
    static const char* const variant_defs[V_COUNT] = {"", "#define DS_EARLY 1\n", "#define DS_ALPHA_SEL 1\n", "#define DS_ALPHA_SEL 2\n"};
    for (u32 wd = 0; wd < 2; ++wd) for (u32 v = 0; v < V_COUNT; ++v) {
      const std::string defs = std::string(variant_defs[v]) + (wd && !wclip ? "#define DS_WDEPTH 1\n" : "") + sdefs;
      const GLuint fs = compile(a, GL_FRAGMENT_SHADER, source(text_lean_frag(), defs), &err);
      d.prog_draw[wd][v] = link(a, vs_lean, fs, &err);
      if (fs) a.glDeleteShader(fs);
      if (!d.prog_draw[wd][v]) { if (why) *why = err; return nullptr; }
      a.glUseProgram(d.prog_draw[wd][v]); sampler(a, d.prog_draw[wd][v], "polytex", 0); sampler(a, d.prog_draw[wd][v], "verttex", 1);
      d.loc_base_draw[wd][v] = a.glGetUniformLocation(d.prog_draw[wd][v], "u_base");
    }
    d.prog_prep = link(a, vs_lean, fs_null, &err);
    d.prog_mask = link(a, vs_fs, fs_null, &err);
    if (!d.prog_prep || !d.prog_mask) { if (why) *why = err; return nullptr; }
    a.glUseProgram(d.prog_prep); sampler(a, d.prog_prep, "polytex", 0); sampler(a, d.prog_prep, "verttex", 1); d.loc_base_prep = a.glGetUniformLocation(d.prog_prep, "u_base");
    const GLuint fs_res = compile(a, GL_FRAGMENT_SHADER, source(text_resolve_frag(), vdefs), &err);
    d.prog_resolve = link(a, vs_fs, fs_res, &err);
    if (fs_res) a.glDeleteShader(fs_res);
    if (!d.prog_resolve) { if (why) *why = err; return nullptr; }
    a.glUseProgram(d.prog_resolve); sampler(a, d.prog_resolve, "s_col", 2); sampler(a, d.prog_resolve, "s_depth", 3); sampler(a, d.prog_resolve, "s_fog", 4);
    if (d.edges) {
      const GLuint vs_edge = compile(a, GL_VERTEX_SHADER, source(text_edge_vert(), vdefs), &err);
      const GLuint fs_edge = compile(a, GL_FRAGMENT_SHADER, source(text_edge_frag(), vdefs), &err);
      const GLuint fs_cut = compile(a, GL_FRAGMENT_SHADER, source(text_cutout_frag(), vdefs + sdefs), &err);
      d.prog_band = link(a, vs_edge, fs_edge, &err);
      d.prog_cutout = link(a, vs_lean, fs_cut, &err);
      for (GLuint s : {vs_edge, fs_edge, fs_cut}) if (s) a.glDeleteShader(s);
      if (!d.prog_band || !d.prog_cutout) { if (why) *why = err; return nullptr; }
      for (GLuint p : {d.prog_band, d.prog_cutout}) { a.glUseProgram(p); sampler(a, p, "polytex", 0); sampler(a, p, "verttex", 1); sampler(a, p, "s_ids", 2); sampler(a, p, "s_depth", 3); }
      d.loc_base_band = a.glGetUniformLocation(d.prog_band, "u_base"); d.loc_base_cutout = a.glGetUniformLocation(d.prog_cutout, "u_base");
    }
    a.glDeleteShader(vs_lean); a.glDeleteShader(vs_fs); a.glDeleteShader(fs_null);
    a.glUseProgram(0);
  }
  a.glGenVertexArrays(1, &d.vao);
  if (d.timing) a.glGenQueriesEXT(Impl::kSlots, d.query);
  if (a.glGetError() != GL_NO_ERROR) return fail("GL error during setup");
  static const bool no_warm = std::getenv("DS_GL_NOWARM") && std::atoi(std::getenv("DS_GL_NOWARM")) != 0;   // debug: measure the first-use stalls
  const double t_warm = now_ms();
  if (!no_warm) d.warm();
  if (a.glGetError() != GL_NO_ERROR) return fail("GL error during warm-up");
  if (d.egl_sync) d.collector = std::thread([&d] { d.collect_loop(); });
  self->ready_ = true;
  std::fprintf(stderr, "gpu lean (OpenGL ES): %s | %s; 1x%s, %s depth/stencil%s%s%s; warm-up %.0f ms\n", a.glGetString(GL_RENDERER), a.glGetString(GL_VERSION), d.edges ? " with the edge cut" : " (no MSAA)",
               d24 ? "D24S8" : "D32S8", d.span_exact ? ", exact spans" : "", d.indexed ? "" : ", no per-attachment blend", d.egl_sync ? "" : ", no EGL fences (frames finish synchronously)", now_ms() - t_warm);
  return self;
}

namespace {
// Depth, stencil, colour mask and blend state for a pass kind, as
// vk_lean.cpp's pipeline() builds the pipeline of the same kind.
void set_state(Lean::Impl& d, u32 kind, bool eq, u32 ref, bool wbuf) {
  Api& a = *d.a;
  const bool stencil_only = kind == P_MASK_CLEAR || kind == P_SHADOW_PREP;
  // Nearer is smaller (z, or w by gl_FragDepth); under DS_GL_WCLIP a W-buffered frame holds 1/w: nearer is larger.
  const bool inv = wbuf && d.wclip;
  if (stencil_only) a.glDisable(GL_DEPTH_TEST); else { a.glEnable(GL_DEPTH_TEST); a.glDepthFunc(inv ? (eq ? GL_GEQUAL : GL_GREATER) : (eq ? GL_LEQUAL : GL_LESS)); }
  a.glDepthMask((kind == P_OPAQUE || kind == P_OPAQUE_EARLY || kind == P_TRANS_OPQ || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW) ? 1 : 0);
  GLenum cmp = GL_ALWAYS, zfail = GL_KEEP, pass = GL_KEEP; GLuint cmask = 0, wmask = 0;
  switch (kind) {
    case P_OPAQUE: case P_OPAQUE_EARLY: case P_TRANS_OPQ: pass = GL_REPLACE; wmask = S_DRAWN | S_T | S_CODE; break;
    case P_TRANS_A: case P_TRANS_A_DW: cmp = GL_NOTEQUAL; cmask = S_DRAWN; pass = GL_REPLACE; wmask = S_DRAWN | S_T | S_CODE; break;
    case P_TRANS_B: case P_TRANS_B_DW: cmp = d.nostencil ? GL_ALWAYS : GL_NOTEQUAL; cmask = S_DRAWN | S_T | S_CODE; pass = GL_REPLACE; wmask = S_DRAWN | S_T | S_CODE; break;
    case P_MASK: zfail = GL_REPLACE; wmask = S_MASK; break;
    case P_MASK_CLEAR: pass = GL_REPLACE; wmask = S_MASK; break;
    case P_SHADOW_PREP: cmp = GL_EQUAL; cmask = S_CODE; pass = GL_REPLACE; wmask = S_MASK; break;
    case P_SHADOW: cmp = GL_NOTEQUAL; cmask = S_MASK; pass = GL_REPLACE; wmask = S_MASK | S_T | S_CODE; break;
  }
  a.glStencilFunc(cmp, static_cast<GLint>(ref), cmask);
  a.glStencilOp(GL_KEEP, zfail, pass);
  a.glStencilMask(wmask);
  const GLboolean col = (kind == P_MASK || stencil_only) ? 0 : 1;
  const GLboolean ids = (kind == P_OPAQUE || kind == P_OPAQUE_EARLY || kind == P_TRANS_OPQ) ? 1 : 0;
  const bool blend = kind == P_TRANS_B || kind == P_TRANS_B_DW || kind == P_SHADOW;
  if (d.indexed) {
    a.glColorMaski(0, col, col, col, col); a.glColorMaski(1, col, 0, 0, 0); a.glColorMaski(2, ids, 0, 0, 0);
    if (blend) {
      a.glEnablei(GL_BLEND, 0); a.glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE); a.glBlendEquationSeparatei(0, GL_FUNC_ADD, GL_MAX);
      a.glEnablei(GL_BLEND, 1); a.glBlendFuncSeparatei(1, GL_ONE, GL_ONE, GL_ONE, GL_ONE); a.glBlendEquationSeparatei(1, GL_MIN, GL_MIN);   // fog kept only if every layer had it
    } else { a.glDisablei(GL_BLEND, 0); a.glDisablei(GL_BLEND, 1); }
  } else {
    // One mask and blend for every attachment: the record takes the
    // translucent passes' ids and the fog bit blends instead of MIN.
    a.glColorMask(col, col, col, col);
    if (blend) { a.glEnable(GL_BLEND); a.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE); a.glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX); }
    else a.glDisable(GL_BLEND);
  }
}
}

GLint Lean::Impl::use_kind(u32 kind, bool wbuf, bool eq, u32 ref) {
  GLuint prog; GLint loc;
  if (kind == P_MASK_CLEAR) { prog = prog_mask; loc = -1; }
  else if (kind == P_SHADOW_PREP) { prog = prog_prep; loc = loc_base_prep; }
  else {
    const u32 v = kind == P_OPAQUE_EARLY ? V_EARLY : kind == P_TRANS_OPQ ? V_OPQ : (kind == P_TRANS_A || kind == P_TRANS_B || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW) ? V_TRANS : V_PLAIN;
    prog = prog_draw[wbuf ? 1 : 0][v]; loc = loc_base_draw[wbuf ? 1 : 0][v];
  }
  if (prog != bound_prog) { a->glUseProgram(prog); bound_prog = prog; }
  set_state(*this, kind, eq, ref, wbuf);
  return loc;
}

void Lean::Impl::warm() {
  Api& g = *a;
  std::vector<u32> zero(kPullW * 4, 0);
  g.glActiveTexture(GL_TEXTURE0); g.glBindTexture(GL_TEXTURE_2D, tex_polys[0]);
  g.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kPullW, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, zero.data());
  g.glActiveTexture(GL_TEXTURE0 + 1); g.glBindTexture(GL_TEXTURE_2D, tex_verts[0]);
  std::memset(map_frame[0], 0, sizeof(GpuFrame));
  g.glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_frame[0]);
  g.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_polys[0]);
  g.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_texels);
  g.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_post[0]);
  g.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_spans[0]);
  g.glBindVertexArray(vao);
  g.glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
  g.glViewport(0, 0, W, H);
  g.glEnable(GL_STENCIL_TEST);
  for (u32 wd = 0; wd < 2; ++wd)
    for (u32 kind = 0; kind < P_COUNT; ++kind) {
      const GLint loc = use_kind(kind, wd != 0, false, 0);
      if (loc >= 0) g.glUniform1i(loc, 0);
      if (kind == P_MASK_CLEAR) g.glDrawArrays(GL_TRIANGLES, 0, 3); else g.glDrawArraysInstanced(GL_TRIANGLES, 0, 24, 1);
    }
  g.glDisable(GL_STENCIL_TEST); g.glDisable(GL_DEPTH_TEST); g.glDepthMask(0);
  if (indexed) { g.glDisablei(GL_BLEND, 0); g.glDisablei(GL_BLEND, 1); g.glColorMaski(0, 1, 1, 1, 1); g.glColorMaski(1, 1, 1, 1, 1); g.glColorMaski(2, 1, 1, 1, 1); }
  else g.glDisable(GL_BLEND);
  g.glColorMask(1, 1, 1, 1);
  if (edges) {
    g.glBindFramebuffer(GL_FRAMEBUFFER, fbo_edge);
    g.glActiveTexture(GL_TEXTURE0 + 2); g.glBindTexture(GL_TEXTURE_2D, t_ids);
    g.glActiveTexture(GL_TEXTURE0 + 3); g.glBindTexture(GL_TEXTURE_2D, t_ds);
    g.glUseProgram(prog_band); g.glUniform1i(loc_base_band, 0); g.glDrawArraysInstanced(GL_TRIANGLES, 0, 60, 1);
    g.glUseProgram(prog_cutout); g.glUniform1i(loc_base_cutout, 0); g.glDrawArraysInstanced(GL_TRIANGLES, 0, 24, 1);
  }
  g.glBindFramebuffer(GL_FRAMEBUFFER, fbo_res);
  g.glActiveTexture(GL_TEXTURE0 + 2); g.glBindTexture(GL_TEXTURE_2D, t_col);
  g.glActiveTexture(GL_TEXTURE0 + 3); g.glBindTexture(GL_TEXTURE_2D, t_ds);
  g.glActiveTexture(GL_TEXTURE0 + 4); g.glBindTexture(GL_TEXTURE_2D, t_fog);
  g.glUseProgram(prog_resolve); g.glDrawArrays(GL_TRIANGLES, 0, 3);
  bound_prog = prog_resolve;
  g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
  g.glFinish();
}

bool Lean::submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& fin) {
  Impl& d = *d_;
  Api& a = *d.a;
  if (npoly > DS_MAX_POLYS || nvert > DS_MAX_VERTS || ntexels > kTexelWords) return false;
  const u32 slot = static_cast<u32>(d.gen % Impl::kSlots);
  const double t_in = now_ms();
  {
    std::unique_lock<std::mutex> lk(d.m);
    d.cv.wait(lk, [&d, slot] { return !d.live[slot]; });   // its previous frame copied out (four back: long done)
    d.drop_sync(slot);
    d.valid[slot] = false;
  }
  d.bind_thread();
  const double t_prep = now_ms();
  if (d.timing) {   // the slot's last frame is done: its time
    GLuint avail = 0; a.glGetQueryObjectuivEXT(d.query[slot], GL_QUERY_RESULT_AVAILABLE_EXT, &avail);
    GLint disjoint = 0; a.glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (avail && d.gen >= Impl::kSlots) { GLuint64 ns = 0; a.glGetQueryObjectui64vEXT(d.query[slot], GL_QUERY_RESULT_EXT, &ns); if (!disjoint && ns < 1000000000ull) { std::lock_guard<std::mutex> lk(d.m); d.last_gpu_ns = ns; d.st.gpu_ms += static_cast<double>(ns) / 1e6; ++d.st.timed; } }
  }

  GpuFrame f = fin;
  const bool wbuf = (f.flags & DS_FF_WBUFFER) != 0;
  // The clip z of a W-buffered frame carries wscale / w (gl_pull.glsl, ds_clip_z).
  f.wscale = 0x3F800000u;   // 1.0f
  if (wbuf && d.wclip) {
    s32 wmin = 0x7FFFFFFF;
    for (u32 k = 0; k < nvert; ++k) wmin = std::min(wmin, d.host_verts[k].z);
    const float ws = std::max(1.0f, static_cast<float>(wmin) - 300.0f);   // room for a decal's pull
    std::memcpy(&f.wscale, &ws, sizeof ws);
  }
  float wscale_f = 1.0f; std::memcpy(&wscale_f, &f.wscale, sizeof wscale_f);
  GpuPoly* gpw = d.host_polys.data();
  const GpuPoly* gp = gpw;
  const GpuVert* gv = d.host_verts.data();
  // Each polygon's texcoord range for the fragment stage's clamp, and its
  // centroid (28.4, s16 x | y << 16) in vbot for the vertex stage's grow
  // and the band pass's inside point.
  for (u32 k = 0; k < npoly; ++k) {
    GpuPoly& p = gpw[k];
    s32 s0 = 0x7FFF, s1 = -0x8000, t0 = 0x7FFF, t1 = -0x8000, cx = 0, cy = 0;
    for (u32 v = 0; v < p.nverts; ++v) { const GpuVert& q = gv[p.first_vert + v]; s0 = std::min(s0, q.s); s1 = std::max(s1, q.s); t0 = std::min(t0, q.t); t1 = std::max(t1, q.t); cx += q.fx; cy += q.fy; }
    auto pk = [](s32 lo, s32 hi) { return static_cast<u32>(std::clamp(lo + 32768, 0, 65535)) | (static_cast<u32>(std::clamp(hi + 32768, 0, 65535)) << 16); };
    p.row_base = pk(s0, s1); p.pad_ = pk(t0, t1);
    if (p.nverts) { cx /= static_cast<s32>(p.nverts); cy /= static_cast<s32>(p.nverts); }
    p.vbot = (static_cast<u32>(std::clamp(cx, -32768, 32767)) & 0xFFFF) | (static_cast<u32>(std::clamp(cy, -32768, 32767)) << 16);
  }
  // The frame's id codes (vk_lean.cpp: the stencil layout).
  u8 code[64] = {};
  {
    u32 next = 1;
    auto want = [&](u32 id) { if (!code[id]) code[id] = static_cast<u8>(next <= 31 ? next++ : (id & 31)); };
    for (u32 k = 0; k < npoly; ++k) if (gp[k].flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW)) want((gp[k].attr >> 24) & 0x3F);
  }
  auto code_of = [&](const GpuPoly& p) -> u32 { return code[(p.attr >> 24) & 0x3F]; };

  // The opaque prefix reordered for the hardware (vk_lean.cpp): polygons
  // that cannot fail the alpha test first, front to back, then the cut-out
  // textured ones. Into `sorted`, which is the slot's polygon list.
  const u32 first_ordered = std::min(f.first_ordered, npoly);
  u32 n_early = 0;
  GpuPoly* const out_p = d.sorted.data();
  if (first_ordered) {
    auto nearest = [&](const GpuPoly& p) -> s32 { const GpuVert* v = gv + p.first_vert; s32 zmin = 0x7FFFFFFF; for (u32 k = 0; k < p.nverts; ++k) zmin = std::min(zmin, v[k].z); return zmin; };
    d.key.clear(); d.key.reserve(first_ordered);
    for (u32 k = 0; k < first_ordered; ++k) { const bool at = (gp[k].flags & DS_PF_TEX_ALPHA) != 0, eq = (gp[k].attr & 0x4000u) != 0; d.key.emplace_back((at ? (s64{1} << 40) : 0) + (eq ? (s64{1} << 38) : 0) + (s64{code_of(gp[k])} << 32) + nearest(gp[k]), k); }
    std::sort(d.key.begin(), d.key.end());
    for (u32 k = 0; k < first_ordered; ++k) { out_p[k] = gp[d.key[k].second]; if (!(out_p[k].flags & DS_PF_TEX_ALPHA)) ++n_early; }
  }
  if (npoly > first_ordered) std::memcpy(out_p + first_ordered, gp + first_ordered, sizeof(GpuPoly) * (npoly - first_ordered));

  // The slot's device copies.
  if (npoly) std::memcpy(d.map_polys[slot], out_p, sizeof(GpuPoly) * npoly);
  std::memcpy(d.map_post[slot], &d.host_post, sizeof(GpuPost));
  std::memcpy(d.map_frame[slot], &f, sizeof(GpuFrame));
  {
    const u32 nrows = std::min<u32>(f.nrows, DS_MAX_SPAN_ROWS);
    if (nrows) std::memcpy(d.map_spans[slot], d.host_spans.data(), sizeof(u32) * DS_SPAN_WORDS * nrows);
  }
  a.glActiveTexture(GL_TEXTURE0); a.glBindTexture(GL_TEXTURE_2D, d.tex_polys[slot]);
  if (npoly) a.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kPullW, static_cast<GLsizei>((npoly * 4 + kPullW - 1) / kPullW), GL_RGBA_INTEGER, GL_UNSIGNED_INT, out_p);
  if (d.edges) vk::mark_shared_edges(gpw, npoly, d.host_verts.data(), d.seams);
  a.glActiveTexture(GL_TEXTURE0 + 1); a.glBindTexture(GL_TEXTURE_2D, d.tex_verts[slot]);
  if (nvert) a.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kPullW, static_cast<GLsizei>((nvert * 3 + kPullW - 1) / kPullW), GL_RGBA_INTEGER, GL_UNSIGNED_INT, gv);
  a.glBindBufferBase(GL_UNIFORM_BUFFER, 0, d.ubo_frame[slot]);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, d.ssbo_polys[slot]);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, d.ssbo_texels);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, d.ssbo_post[slot]);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, d.ssbo_spans[slot]);
  a.glBindVertexArray(d.vao);

  const double t_rec = now_ms();
  if (d.timing) a.glBeginQueryEXT(GL_TIME_ELAPSED_EXT, d.query[slot]);
  a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_draw);
  a.glViewport(0, 0, W, H);
  a.glDisable(GL_SCISSOR_TEST); a.glDisable(GL_CULL_FACE); a.glDisable(GL_DITHER);
  // Clear: the rear plane's colour and alpha; depth; stencil drawn where the rear plane has alpha; the fog bit; no record.
  {
    a.glColorMask(1, 1, 1, 1); a.glDepthMask(1); a.glStencilMask(0xFF);
    if (d.indexed) { a.glColorMaski(0, 1, 1, 1, 1); a.glColorMaski(1, 1, 1, 1, 1); a.glColorMaski(2, 1, 1, 1, 1); }
    const u32 c = f.clear_color, ca = (c >> 24) & 0x1F;
    const float col[4] = {static_cast<float>(c & 0x3F) / 255.f, static_cast<float>((c >> 8) & 0x3F) / 255.f, static_cast<float>((c >> 16) & 0x3F) / 255.f, ca == 0 ? 0.f : static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f};
    const float fog[4] = {(f.clear_attr & 0x8000) ? 1.f : 0.f, 0.f, 0.f, 0.f};
    const GLuint zero[4] = {0, 0, 0, 0};
    const float cz = static_cast<float>(f.clear_depth & 0xFFFFFF);
    a.glClearBufferfv(GL_COLOR, 0, col);
    a.glClearBufferfv(GL_COLOR, 1, fog);
    if (d.edges) a.glClearBufferuiv(GL_COLOR, 2, zero);
    a.glClearBufferfi(GL_DEPTH_STENCIL, 0, wbuf && d.wclip ? std::min(1.f, wscale_f / std::max(cz, 1.f)) : std::min(1.f, cz / 16777215.f), ca ? static_cast<GLint>(S_DRAWN | code[(f.clear_attr >> 24) & 0x3F]) : 0);
  }
  a.glEnable(GL_STENCIL_TEST);
  u32 draws = 0;
  auto use = [&](u32 kind, bool eq, u32 ref) { return d.use_kind(kind, wbuf, eq, ref); };
  auto draw = [&](GLint loc, u32 first, u32 count) { if (loc >= 0) a.glUniform1i(loc, static_cast<GLint>(first)); a.glDrawArraysInstanced(GL_TRIANGLES, 0, 24, static_cast<GLsizei>(count)); ++draws; };
  if (first_ordered) {
    for (u32 k = 0; k < first_ordered;) {
      const u32 c = code_of(out_p[k]); const bool early = k < n_early, eq = (out_p[k].attr & 0x4000u) != 0;
      u32 j = k + 1;
      while (j < first_ordered && code_of(out_p[j]) == c && (j < n_early) == early && ((out_p[j].attr & 0x4000u) != 0) == eq) ++j;
      draw(use(early ? P_OPAQUE_EARLY : P_OPAQUE, eq, S_DRAWN | c), k, j - k);
      k = j;
    }
  }
  // The tail, in list order: runs of polygons that take the same passes (vk_lean.cpp).
  u32 i = first_ordered;
  bool prev_mask = false, any_mask = false;
  while (i < npoly) {
    const GpuPoly& p = gp[i];
    const u32 mode = (p.attr >> 4) & 3, id = (p.attr >> 24) & 0x3F, c = code[id];
    const bool mask = (p.flags & DS_PF_SHADOW_MASK) != 0, shadow = (p.flags & DS_PF_SHADOW) != 0;
    const bool trans = (p.flags & DS_PF_TRANSLUCENT) != 0, dw = (p.attr & (1u << 11)) != 0, eq = (p.attr & 0x4000u) != 0;
    u32 j = i + 1;
    while (j < npoly) {
      const GpuPoly& q = gp[j];
      if (((q.attr >> 4) & 3) != mode || ((q.attr >> 24) & 0x3F) != id || ((q.attr & 0x4000u) != 0) != eq || ((q.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT)) != (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT))) || ((q.attr & (1u << 11)) != 0) != dw) break;
      ++j;
    }
    const u32 n = j - i;
    if (mask) {
      if (any_mask && !prev_mask) { use(P_MASK_CLEAR, eq, 0); a.glDrawArrays(GL_TRIANGLES, 0, 3); ++draws; }
      any_mask = true;
      draw(use(P_MASK, eq, S_MASK), i, n);
    } else if (shadow) {
      draw(use(P_SHADOW_PREP, eq, c), i, n);
      draw(use(P_SHADOW, eq, S_T | c), i, n);
    } else if (trans) {
      draw(use(P_TRANS_OPQ, eq, S_DRAWN | c), i, n);
      if (!d.nostencil) draw(use(dw ? P_TRANS_A_DW : P_TRANS_A, eq, S_DRAWN | S_T | c), i, n);
      draw(use(dw ? P_TRANS_B_DW : P_TRANS_B, eq, S_DRAWN | S_T | c), i, n);
    } else {
      draw(use(P_OPAQUE, eq, S_DRAWN | c), i, n);
    }
    prev_mask = mask;
    i = j;
    if (draws > kMaxDraws) break;
  }
  a.glDisable(GL_STENCIL_TEST); a.glDisable(GL_DEPTH_TEST); a.glDepthMask(0);
  if (d.indexed) { a.glDisablei(GL_BLEND, 0); a.glDisablei(GL_BLEND, 1); a.glColorMaski(0, 1, 1, 1, 1); a.glColorMaski(1, 1, 1, 1, 1); a.glColorMaski(2, 1, 1, 1, 1); }
  else a.glDisable(GL_BLEND);
  a.glColorMask(1, 1, 1, 1);
  if (d.edges) {
    // The edge passes (vk_lean.cpp): the band pass over every polygon's
    // edges, the cut-out pass over the alpha-tested opaque ones; the draw's
    // record and depth as textures.
    a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_edge);
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    a.glClearBufferfv(GL_COLOR, 0, zero);
    a.glActiveTexture(GL_TEXTURE0 + 2); a.glBindTexture(GL_TEXTURE_2D, d.t_ids);
    a.glActiveTexture(GL_TEXTURE0 + 3); a.glBindTexture(GL_TEXTURE_2D, d.t_ds);
    a.glUseProgram(d.prog_band); d.bound_prog = d.prog_band;
    if (npoly) {
      constexpr u32 kMaxBandRuns = 32;
      u32 runs = 0, run_first[kMaxBandRuns], run_count[kMaxBandRuns];
      for (u32 k = 0; k < npoly && runs <= kMaxBandRuns; ++k) {
        if (out_p[k].nverts <= 4) continue;
        if (runs && run_first[runs - 1] + run_count[runs - 1] == k) ++run_count[runs - 1];
        else if (runs < kMaxBandRuns) { run_first[runs] = k; run_count[runs] = 1; ++runs; }
        else ++runs;
      }
      static const bool one_draw = std::getenv("DS_LEAN_BANDRUNS") && std::atoi(std::getenv("DS_LEAN_BANDRUNS")) == 0;
      for (GLint pass = 0; pass < 120; pass += 60) {   // seams (vertex ids 0-59) first, the rest after: vk_lean.cpp
        a.glUniform1i(d.loc_base_band, 0);
        if (runs > kMaxBandRuns || one_draw) { a.glDrawArraysInstanced(GL_TRIANGLES, pass, 60, static_cast<GLsizei>(npoly)); ++draws; }
        else {
          a.glDrawArraysInstanced(GL_TRIANGLES, pass, 24, static_cast<GLsizei>(npoly)); ++draws;
          for (u32 r = 0; r < runs; ++r) { a.glUniform1i(d.loc_base_band, static_cast<GLint>(run_first[r])); a.glDrawArraysInstanced(GL_TRIANGLES, pass + 24, 36, static_cast<GLsizei>(run_count[r])); ++draws; }
        }
      }
    }
    static const bool no_cutout = std::getenv("DS_LEAN_NOCUTOUT") && std::atoi(std::getenv("DS_LEAN_NOCUTOUT")) != 0;
    if (first_ordered > n_early && (f.flags2 & DS_FF2_CUTOUT) && !no_cutout) { a.glUseProgram(d.prog_cutout); d.bound_prog = d.prog_cutout; a.glUniform1i(d.loc_base_cutout, static_cast<GLint>(n_early)); a.glDrawArraysInstanced(GL_TRIANGLES, 0, 24, static_cast<GLsizei>(first_ordered - n_early)); ++draws; }
  }
  // The resolve into the record, and the record (and the edge bytes) to the slot's PBO.
  a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_res);
  a.glActiveTexture(GL_TEXTURE0 + 2); a.glBindTexture(GL_TEXTURE_2D, d.t_col);
  a.glActiveTexture(GL_TEXTURE0 + 3); a.glBindTexture(GL_TEXTURE_2D, d.t_ds);
  a.glActiveTexture(GL_TEXTURE0 + 4); a.glBindTexture(GL_TEXTURE_2D, d.t_fog);
  a.glUseProgram(d.prog_resolve); d.bound_prog = d.prog_resolve;
  a.glDrawArrays(GL_TRIANGLES, 0, 3); ++draws;
  a.glPixelStorei(GL_PACK_ALIGNMENT, 4);
  a.glBindBuffer(GL_PIXEL_PACK_BUFFER, d.pbo_out[slot]);
  a.glReadBuffer(GL_COLOR_ATTACHMENT0);
  a.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  if (d.edges) {
    a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_edge);
    a.glBindBuffer(GL_PIXEL_PACK_BUFFER, d.pbo_edge[slot]);
    a.glReadBuffer(GL_COLOR_ATTACHMENT0);
    a.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  a.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  if (d.timing) a.glEndQueryEXT(GL_TIME_ELAPSED_EXT);
  const double t_q = now_ms();
  EGLSyncKHR sync = nullptr;
  if (d.egl_sync) { sync = a.eglCreateSyncKHR(d.dpy, EGL_SYNC_FENCE_KHR, nullptr); a.glFlush(); }
  if (!sync) a.glFinish();   // no fence: the frame is done when this returns
  static const bool check = std::getenv("DS_GL_CHECK") && std::atoi(std::getenv("DS_GL_CHECK")) != 0;   // debug: glGetError per frame
  if (check) { const GLenum e = a.glGetError(); if (e) std::fprintf(stderr, "gl lean: GL error 0x%x after frame %llu\n", e, (unsigned long long)d.gen); }
  {
    std::lock_guard<std::mutex> lk(d.m);
    d.st.record_ms += t_q - t_rec; d.st.queue_ms += now_ms() - t_q; d.st.slot_wait_ms += t_prep - t_in; d.st.prep_ms += t_rec - t_prep;
    d.sync[slot] = sync;
    d.live[slot] = sync != nullptr;
    d.submit_at[slot] = t_in;
    d.last_draws = draws;
    if (wbuf) ++d.st.wbuf;
    // No fence: finished here (glFinish); copied out on this thread.
    if (!sync) { d.valid[slot] = true; std::memcpy(d.out_host[slot].data(), d.map_out[slot], sizeof(u32) * W * H); if (d.edges) for (u32 k = 0; k < W * H; ++k) d.edge_bytes[slot][k] = d.map_edge[slot][4 * k]; }
    ++d.gen;
    if (!sync) d.collected = d.gen;
  }
  d.cv.notify_all();
  return true;
}

// Wait for slot `slot` to be copied out (the collector's work), the lock held.
static void wait_slot(Lean::Impl& d, std::unique_lock<std::mutex>& lk, u32 slot) {
  if (!d.live[slot]) return;
  const double t0 = now_ms();
  d.cv.wait(lk, [&d, slot] { return !d.live[slot]; });
  (void)t0;
  d.st.lat_ms += now_ms() - d.submit_at[slot]; ++d.st.lat_n;
}

void Lean::wait() {
  Impl& d = *d_;
  std::unique_lock<std::mutex> lk(d.m);
  if (!d.gen) return;
  wait_slot(d, lk, static_cast<u32>((d.gen - 1) % Impl::kSlots));
}

void Lean::wait_all() {
  Impl& d = *d_;
  std::unique_lock<std::mutex> lk(d.m);
  d.cv.wait(lk, [&d] { return d.collected == d.gen; });
}

const u32* Lean::frame_ready(u64 want, bool allow_lag) {
  Impl& d = *d_;
  std::unique_lock<std::mutex> lk(d.m);
  if (!d.gen) return nullptr;
  // The frame asked for, clamped to the resident ones (the newest when 0).
  // One not submitted yet: the newest under the lag rule (waited for), else nothing (the caller waits for the submit).
  static const bool no_lag = std::getenv("DS_GPU_NOLAG") && std::atoi(std::getenv("DS_GPU_NOLAG")) != 0;   // debug: never the frame before (deterministic dumps)
  if (want > d.gen) {
    if (!allow_lag || no_lag) return nullptr;
    const u32 newest = static_cast<u32>((d.gen - 1) % Impl::kSlots);
    wait_slot(d, lk, newest);
    ++d.st.lagged; return d.out_host[newest].data();
  }
  if (want == 0) want = d.gen;
  if (d.gen - want >= Impl::kSlots) want = d.gen - Impl::kSlots + 1;
  const u32 slot = static_cast<u32>((want - 1) % Impl::kSlots);
  if (d.asked_gen != want) { d.asked_gen = want; d.st.gap_ms += now_ms() - d.submit_at[slot]; ++d.st.gap_n; }
  if (!d.live[slot]) { ++d.st.newest; return d.out_host[slot].data(); }
  if (allow_lag && !no_lag && want >= 2 && d.gen - (want - 1) < Impl::kSlots) {
    const u32 prev = static_cast<u32>((want - 2) % Impl::kSlots);
    if (!d.live[prev] && d.valid[prev]) { ++d.st.lagged; return d.out_host[prev].data(); }
  }
  if (allow_lag) ++d.st.stalled; else ++d.st.nolag;
  wait_slot(d, lk, slot);
  return d.out_host[slot].data();
}

} // namespace ds::gpu::gl
