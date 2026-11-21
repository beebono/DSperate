// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gl/gl_lean.h"

#include "core/gpu/gl/gl_api.h"
#include "core/gpu/gl/gl_shaders.h"
#include "core/gpu/vk/lean_runs.h"

#include <algorithm>
#include <atomic>
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
u32 W = 256, H = 192;   // 256 * scale x 192 * scale (DS_LEAN_SCALE, probe-only: the renderer reads the record as 1x)
// DS_LEAN_SKIP=hex (debug, vk_lean.cpp's pass-kind bits): 0 early opaque, 1 alpha-tested opaque, 2 translucent alpha-31,
// 3 translucent A, 4 translucent B, 5 shadow prep, 6 shadow, 7 shadow mask (+ clear), 8 opaque in the tail, 9 resolve, 10 record readback.
const u32 g_skip = std::getenv("DS_LEAN_SKIP") ? static_cast<u32>(std::strtoul(std::getenv("DS_LEAN_SKIP"), nullptr, 16)) : 0u;
constexpr u32 kTexelWords = 2u << 20;   // 8 MB of decoded texels, as the Vulkan raster's arena
constexpr u32 kMaxDraws = 4096;
constexpr u32 kPullW = 1024;            // the polygon/vertex textures' width in texels
// The stencil layout and the pass kinds, vk_lean.cpp's.
constexpr u32 S_DRAWN = 0x80, S_MASK = 0x40, S_T = 0x20, S_CODE = 0x1F;
enum Pipe : u32 { P_OPAQUE = 0, P_TRANS_A, P_TRANS_B, P_TRANS_A_DW, P_TRANS_B_DW, P_MASK, P_SHADOW, P_SHADOW_PREP, P_MASK_CLEAR, P_OPAQUE_EARLY, P_TRANS_OPQ, P_TRANS_ONE, P_TRANS_ONE_DW, P_COUNT };
enum Variant : u32 { V_PLAIN = 0, V_EARLY, V_OPQ, V_TRANS, V_FETCH, V_COUNT };   // the fragment stage's build (V_FETCH: a translucent run in one pass, d.fetch)
double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

struct Lean::Impl {
  Api* a = nullptr;
  EGLDisplay dpy = nullptr;
  EGLContext ctx = nullptr;
  EGLSurface surf = nullptr;
  bool timing = false, indexed = false, egl_sync = false, nostencil = false, wclip = false;
  u32 fetch = 0;   // translucent runs in one pass through GL_ARM_shader_framebuffer_fetch: 1 the blend unit's maths, 2 the DS's (DS_LEAN_FETCH)
  static constexpr u32 kSlots = 4;
  u32 scale = 1;
  double flush_at[kSlots] = {};      // when each slot's frame was flushed to the GPU
  std::atomic<u64> last_fence_ns{0}; // flush to fence signal of the last collected frame (an idle-GPU probe's GPU time)
  // Host staging as the Vulkan raster has it; the slot's device copies are
  // the persistently mapped buffers below (and the vertex stage's textures).
  std::vector<GpuPoly> host_polys, sorted;
  std::vector<GpuVert> host_verts;
  GpuPost host_post{};
  std::vector<std::pair<s64, u32>> key;
  GLuint ssbo_polys[kSlots] = {}, ssbo_post[kSlots] = {}, ubo_frame[kSlots] = {}, ssbo_texels = 0;
  void* map_polys[kSlots] = {}; void* map_post[kSlots] = {}; void* map_frame[kSlots] = {}; u32* map_texels = nullptr;
  GLuint tex_polys[kSlots] = {}, tex_verts[kSlots] = {};
  // [1]: the frames that anti-alias edges (aa, aa_now): the fog attachment with the edge byte and the polygon's id
  // beside the fog bit, and programs that write and read them. Their own set, because every frame paid for them
  // otherwise: on the Mali-G31, Spirit Tracks' GPU time 5.22 -> 5.57 ms with nothing anti-aliased.
  GLuint t_col = 0, t_fog[2] = {}, t_ids = 0, t_ds = 0, t_res = 0;
  GLuint fbo_draw[2] = {}, fbo_res = 0;
  bool aa = false, aa_now = false;
  GLuint pbo_out[kSlots] = {};
  const u32* map_out[kSlots] = {};
  // The record copied out of the mapped PBOs (uncached memory on Mali) into host memory the compositor can read (finish_slot).
  std::vector<u32> out_host[kSlots];
  // The direct output (GPU-RENDER-PLAN B9): a frame without fog has no resolve pass. The
  // draw's colour attachment is read back as the record, its alpha byte decoded in the copy out of the PBO.
  bool direct = true;
  bool direct_slot[kSlots] = {};
  void copy_out(u32 slot) {
    u32* dst = out_host[slot].data();
    std::memcpy(dst, map_out[slot], sizeof(u32) * W * H);   // out of the uncached map in one go; decoded in cached memory
    if (!direct_slot[slot]) return;
    for (u32 i = 0, n = W * H; i < n; ++i) {   // as gl_resolve.frag: undrawn 0, else the channels and the 5-bit alpha out of (a + 1) * 8
      const u32 w = dst[i], ab = w >> 24;
      const u32 a5 = ab < 4u ? 0u : std::min(31u, (ab + 4u) / 8u - 1u);
      dst[i] = ab ? ((w & 0x00FFFFFFu) | (a5 << 24)) : 0u;
    }
  }
  // Program sets: 0 with mode-2 (toon / highlight) shading, 1 without (DS_TOON false: a frame without mode-2
  // polygons). [set][W-depth][variant].
  GLuint prog_draw[2][2][2][2][V_COUNT] = {};   // [wireframe][edge AA][no toon][W depth][variant]
  // The wireframe programs (DS_WIRE: a run of alpha-0 polygons, whose edges alone are drawn) are built when a frame
  // first has such a run: few games draw one, and each is a program more to compile at start otherwise.
  GLuint vs_lean[2] = {};   // the vertex stage, [edge AA], kept for them
  bool build_draw(u32 wire, u32 ea, u32 sp, u32 wd, u32 v, std::string* err);
  bool toon_now = true;     // ... has mode-2 (toon / highlight) polygons
  GLuint prog_prep = 0, prog_mask = 0, prog_resolve[2] = {};
  // Edge marking (vk_lean.cpp's Impl::mark has the design): t_ids as the draw's third attachment holds the id word,
  // and prog_mark recolours the record's edge pixels, in fbo_res or, for a frame whose colour attachment is the
  // record, in fbo_col. DS_LEAN_EDGEMARK=0 leaves the pass out.
  bool mark = false, mark_on = true;
  GLuint prog_mark = 0, fbo_col = 0;
  GLint loc_base_draw[2][2][2][2][V_COUNT] = {}, loc_base_prep = -1;
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
  GLint use_kind(u32 kind, bool wbuf, bool eq, u32 ref, bool wire = false);
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
      last_fence_ns.store(static_cast<u64>((now_ms() - flush_at[slot]) * 1e6), std::memory_order_relaxed);
      if (!((g_skip >> 10) & 1)) copy_out(slot);
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
      for (GLuint v : vs_lean) if (v) a->glDeleteShader(v);
      for (auto& w : prog_draw) for (auto& e : w) for (auto& set : e) for (auto& row : set) for (GLuint p : row) if (p) a->glDeleteProgram(p);
      for (GLuint p : {prog_prep, prog_mask, prog_resolve[0], prog_resolve[1], prog_mark}) if (p) a->glDeleteProgram(p);
      if (fbo_col) a->glDeleteFramebuffers(1, &fbo_col);
      for (u32 i = 0; i < kSlots; ++i) {
        GLuint bufs[4] = {ssbo_polys[i], ssbo_post[i], ubo_frame[i], pbo_out[i]};
        a->glDeleteBuffers(4, bufs);
        GLuint texs[2] = {tex_polys[i], tex_verts[i]};
        a->glDeleteTextures(2, texs);
      }
      if (ssbo_texels) a->glDeleteBuffers(1, &ssbo_texels);
      GLuint texs[6] = {t_col, t_fog[0], t_fog[1], t_ids, t_ds, t_res};
      a->glDeleteTextures(6, texs);
      GLuint fbos[3] = {fbo_draw[0], fbo_draw[1], fbo_res};
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
GpuPoly* Lean::poly_buffer() { return d_->host_polys.data(); }
GpuVert* Lean::vert_buffer() { return d_->host_verts.data(); }
u32* Lean::texel_buffer(u32* cap) { if (cap) *cap = kTexelWords; return d_->map_texels; }
GpuPost* Lean::post_buffer() { return &d_->host_post; }
const u32* Lean::output() { return frame_ready(0, false); }
bool Lean::has_frame() const { return d_->gen != 0; }
u64 Lean::gpu_ns() const { return d_->last_gpu_ns; }
u32 Lean::scale() const { return d_->scale; }
u64 Lean::fence_ns() const { return d_->last_fence_ns.load(std::memory_order_relaxed); }
u32 Lean::draws() const { return d_->last_draws; }
Lean::Stats Lean::stats(bool reset) { std::lock_guard<std::mutex> lk(d_->m); Stats s = d_->st; if (reset) d_->st = Stats{}; return s; }

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
  // DS_GL_DUMP_SHADERS=DIR: every stage as it is compiled, includes and defines in, to DIR/NN.vert|frag in the
  // order of Lean::create (for the Mali offline compiler; a value that is no directory only prints a rejected one).
  if (const char* dir = std::getenv("DS_GL_DUMP_SHADERS"); dir && std::strchr(dir, '/')) {
    static int n = 0;
    char name[512]; std::snprintf(name, sizeof name, "%s/%02d.%s", dir, n++, kind == GL_VERTEX_SHADER ? "vert" : "frag");
    if (std::FILE* f = std::fopen(name, "w")) { std::fwrite(src.data(), 1, src.size(), f); std::fclose(f); }
  }
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

std::unique_ptr<Lean> Lean::create(bool aa, std::string* why) {
  auto fail_null = [&](const char* m) { if (why) *why = m; return nullptr; };
  if (std::getenv("DS_GL3D") && std::atoi(std::getenv("DS_GL3D")) == 0) return fail_null("disabled (DS_GL3D=0)");
  Api& a = gl_api();
  if (!a.load(why)) return nullptr;
  std::unique_ptr<Lean> self(new Lean);
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  d.a = &a;
  { const char* e = std::getenv("DS_LEAN_SCALE"); d.scale = std::clamp(e ? static_cast<u32>(std::atoi(e)) : 1u, 1u, 4u); W = 256 * d.scale; H = 192 * d.scale; }
  auto fail = [&](const char* m) { if (why) { *why = m; if (a.glGetError) { const GLenum e = a.glGetError(); if (e) *why += " (GL error " + std::to_string(e) + ")"; } } return nullptr; };

  // EGL: the default display, an ES 3.1 context on no surface (a 1x1
  // pbuffer where the driver wants one). The display is initialised once
  // and never terminated: the frontend may share it.
  // A GBM build of the driver (the RK3326 images' Mali blob) has no default display: it takes a gbm_device, and
  // offers window configs only, so the context there has to do without a surface.
  EGLint maj = 0, min = 0;
  d.dpy = a.eglGetDisplay(EGL_DEFAULT_DISPLAY);
  bool up = d.dpy && a.eglInitialize(d.dpy, &maj, &min);
  bool on_gbm = false;
  if (!up) {
    if (void* gbm = a.gbm_device()) {
      d.dpy = a.eglGetPlatformDisplayEXT ? a.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, nullptr) : nullptr;
      if (!d.dpy) d.dpy = a.eglGetDisplay(gbm);
      up = on_gbm = d.dpy && a.eglInitialize(d.dpy, &maj, &min);
    }
  }
  if (!d.dpy) return fail("eglGetDisplay failed");
  if (!up) return fail("eglInitialize failed");
  const char* egl_exts = a.eglQueryString(d.dpy, EGL_EXTENSIONS);
  const bool surfaceless = has_egl_ext(egl_exts, "EGL_KHR_surfaceless_context");
  a.eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
  const EGLint cfg_any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_NONE};
  EGLConfig cfg = nullptr; EGLint ncfg = 0;
  if ((!a.eglChooseConfig(d.dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) && !(surfaceless && a.eglChooseConfig(d.dpy, cfg_any, &cfg, 1, &ncfg) && ncfg >= 1))
    return fail(on_gbm ? "no OpenGL ES 3 config on the GBM display" : "no OpenGL ES 3 config");
  const bool create_ctx = has_egl_ext(egl_exts, "EGL_KHR_create_context");
  const EGLint ctx_attrs31[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
  const EGLint ctx_attrs3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  d.ctx = a.eglCreateContext(d.dpy, cfg, EGL_NO_CONTEXT, create_ctx ? ctx_attrs31 : ctx_attrs3);
  if (!d.ctx && create_ctx) d.ctx = a.eglCreateContext(d.dpy, cfg, EGL_NO_CONTEXT, ctx_attrs3);
  if (!d.ctx) return fail("eglCreateContext failed");
  if (!surfaceless) {
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
  if (const char* e = std::getenv("DS_GL_EXTS"); e && std::atoi(e)) {   // debug: the context's GL extensions, to stderr
    GLint n = 0; a.glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) std::fprintf(stderr, "gl ext: %s\n", reinterpret_cast<const char*>(a.glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))));
  }
  if (!a.glBufferStorageEXT || !(has_gl_ext(a, "GL_EXT_buffer_storage") || gmaj > 3)) return fail("no EXT_buffer_storage");
  d.indexed = a.glColorMaski && a.glEnablei && a.glDisablei && a.glBlendFuncSeparatei && a.glBlendEquationSeparatei &&
              (has_gl_ext(a, "GL_EXT_draw_buffers_indexed") || has_gl_ext(a, "GL_OES_draw_buffers_indexed") || (gmaj == 3 && gmin >= 2) || gmaj > 3);
  d.direct = true;
  d.egl_sync = has_egl_ext(egl_exts, "EGL_KHR_fence_sync") && a.eglCreateSyncKHR && a.eglClientWaitSyncKHR && a.eglGetSyncAttribKHR;
  d.timing = (std::getenv("DS_GL_TIMING") || std::getenv("DS_VK_TIMING")) && has_gl_ext(a, "GL_EXT_disjoint_timer_query") && a.glGenQueriesEXT && a.glGetQueryObjectui64vEXT;
  d.nostencil = std::getenv("DS_LEAN_NOSTENCIL") && std::atoi(std::getenv("DS_LEAN_NOSTENCIL")) != 0;
  // A translucent run in one pass (B3, vk_lean.cpp) through GL_ARM_shader_framebuffer_fetch, on by default:
  // DS_LEAN_FETCH=2 (default, as the Vulkan raster) the DS's blend, 1 the blend unit's maths, 0 off.
  if (const char* e = std::getenv("DS_LEAN_FETCH"); (!e || std::atoi(e) > 0) && d.indexed && !d.nostencil && has_gl_ext(a, "GL_ARM_shader_framebuffer_fetch")) d.fetch = e ? static_cast<u32>(std::min(2, std::atoi(e))) : 2u;

  // Staging and the mapped buffers.
  d.host_polys.resize(DS_MAX_POLYS); d.sorted.resize(DS_MAX_POLYS);
  d.host_verts.resize(DS_MAX_VERTS);
  d.map_texels = static_cast<u32*>(persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_texels, sizeof(u32) * kTexelWords, false));
  if (!d.map_texels) return fail("texel arena");
  for (u32 i = 0; i < Impl::kSlots; ++i) {
    d.map_polys[i] = persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_polys[i], sizeof(GpuPoly) * DS_MAX_POLYS, false);
    d.map_post[i] = persistent(a, GL_SHADER_STORAGE_BUFFER, &d.ssbo_post[i], sizeof(GpuPost), false);
    d.map_frame[i] = persistent(a, GL_UNIFORM_BUFFER, &d.ubo_frame[i], sizeof(GpuFrame), false);
    d.map_out[i] = static_cast<const u32*>(persistent(a, GL_PIXEL_PACK_BUFFER, &d.pbo_out[i], sizeof(u32) * W * H, true));
    if (!d.map_polys[i] || !d.map_post[i] || !d.map_frame[i] || !d.map_out[i]) return fail("buffer mapping");
    d.out_host[i].assign(W * H, 0);
    d.tex_polys[i] = tex2d(a, GL_RGBA32UI, kPullW, DS_MAX_POLYS * 4 / kPullW);
    d.tex_verts[i] = tex2d(a, GL_RGBA32UI, kPullW, DS_MAX_VERTS * 3 / kPullW);
  }
  static_assert(sizeof(GpuPoly) == 64 && sizeof(GpuVert) == 48, "gl_pull.glsl reads four and three texels");
  static_assert((DS_MAX_POLYS * 4) % kPullW == 0 && (DS_MAX_VERTS * 3) % kPullW == 0, "whole texture rows");

  // Attachments: colour, fog bit, edge marking's id word, depth/stencil;
  // the resolved record. Float depth first, as vk_lean.cpp.
  d.t_col = tex2d(a, GL_RGBA8, W, H);
  d.t_fog[0] = tex2d(a, GL_R8, W, H);
  d.mark = d.indexed;   // the third attachment needs its own write mask
  d.mark_on = !(std::getenv("DS_LEAN_EDGEMARK") && std::atoi(std::getenv("DS_LEAN_EDGEMARK")) == 0);
  // Edge anti-aliasing (video.aa on this raster; gl_lean.frag): its byte is kept from the opaque passes by a write
  // mask of the fog attachment's own. DS_GL_EDGEAA=0 for A/B.
  d.aa = aa && d.indexed && !(std::getenv("DS_GL_EDGEAA") && std::getenv("DS_GL_EDGEAA")[0] == '0');
  // r the fog bit, g the edge byte, b the polygon's id. (RGBA4, twelve bits packed over three channels, measured
  // slower on the G31 than this: Spirit Tracks 6.0 ms against 5.64, with 5.23 for a frame that marks no edge.)
  if (d.aa) d.t_fog[1] = tex2d(a, GL_RGBA8, W, H);
  if (d.mark) d.t_ids = tex2d(a, GL_R32UI, W, H);
  d.t_res = tex2d(a, GL_RGBA8, W, H);
  const bool d24 = std::getenv("DS_LEAN_D24") && std::atoi(std::getenv("DS_LEAN_D24")) != 0;
  auto draw_fbo = [&](GLuint fog, GLuint ds) {
    GLuint fbo = 0; a.glGenFramebuffers(1, &fbo); a.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.t_col, 0);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, fog, 0);
    if (d.mark) a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 2, GL_TEXTURE_2D, d.t_ids, 0);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, ds, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2};
    a.glDrawBuffers(d.mark ? 3 : 2, bufs);
    if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) return fbo;
    a.glDeleteFramebuffers(1, &fbo);
    return GLuint{0};
  };
  for (int attempt = d24 ? 1 : 0; attempt < 2 && !d.fbo_draw[0]; ++attempt) {
    GLuint ds = tex2d(a, attempt == 0 ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8, W, H);
    if (GLuint fbo = draw_fbo(d.t_fog[0], ds)) { d.fbo_draw[0] = fbo; d.t_ds = ds; }
    else a.glDeleteTextures(1, &ds);
  }
  if (d.fbo_draw[0] && d.aa && !(d.fbo_draw[1] = draw_fbo(d.t_fog[1], d.t_ds))) d.aa = false;
  if (!d.fbo_draw[0]) return fail("draw framebuffer incomplete");
  a.glGenFramebuffers(1, &d.fbo_res); a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_res);
  a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.t_res, 0);
  if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return fail("resolve framebuffer incomplete");
  if (d.mark) {
    a.glGenFramebuffers(1, &d.fbo_col); a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_col);
    a.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.t_col, 0);
    if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return fail("edge marking framebuffer incomplete");
  }
  a.glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Programs: the draw's vertex stage with the fragment variants (x the
  // W-buffer depth write), the stencil-only pair, the resolve, edge marking.
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
    const std::string aadef = "#define DS_EDGE_AA 1\n";
    const GLuint vs_lean = compile(a, GL_VERTEX_SHADER, source(text_lean_vert(), vdefs), &err);
    const GLuint vs_lean_aa = d.aa ? compile(a, GL_VERTEX_SHADER, source(text_lean_vert(), vdefs + aadef), &err) : 0;
    if (d.aa && !vs_lean_aa) { if (why) *why = err; return nullptr; }
    const GLuint vs_fs = compile(a, GL_VERTEX_SHADER, source(text_fs_vert(), ""), &err);
    const GLuint fs_null = compile(a, GL_FRAGMENT_SHADER, source(text_null_frag(), ""), &err);
    if (!vs_lean || !vs_fs || !fs_null) { if (why) *why = err; return nullptr; }
    d.vs_lean[0] = vs_lean; d.vs_lean[1] = vs_lean_aa;
    for (u32 ea = 0; ea < (d.aa ? 2u : 1u); ++ea) for (u32 sp = 0; sp < 2; ++sp) for (u32 wd = 0; wd < 2; ++wd) for (u32 v = 0; v < V_COUNT; ++v) {
      if (v == V_FETCH && !d.fetch) continue;
      if (!d.build_draw(0, ea, sp, wd, v, &err)) { if (why) *why = err; return nullptr; }
    }
    d.prog_prep = link(a, vs_lean, fs_null, &err);
    d.prog_mask = link(a, vs_fs, fs_null, &err);
    if (!d.prog_prep || !d.prog_mask) { if (why) *why = err; return nullptr; }
    a.glUseProgram(d.prog_prep); sampler(a, d.prog_prep, "polytex", 0); sampler(a, d.prog_prep, "verttex", 1); d.loc_base_prep = a.glGetUniformLocation(d.prog_prep, "u_base");
    for (u32 ea = 0; ea < (d.aa ? 2u : 1u); ++ea) {
      const GLuint fs_res = compile(a, GL_FRAGMENT_SHADER, source(text_resolve_frag(), vdefs + (ea ? aadef : "")), &err);
      GLuint& pr = d.prog_resolve[ea];
      pr = link(a, vs_fs, fs_res, &err);
      if (fs_res) a.glDeleteShader(fs_res);
      if (!pr) { if (why) *why = err; return nullptr; }
      a.glUseProgram(pr); sampler(a, pr, "s_col", 2); sampler(a, pr, "s_depth", 3); sampler(a, pr, "s_fog", 4);
    }
    if (d.mark) {
      const GLuint fs_mark = compile(a, GL_FRAGMENT_SHADER, source(text_mark_frag(), vdefs), &err);
      d.prog_mark = link(a, vs_fs, fs_mark, &err);
      if (fs_mark) a.glDeleteShader(fs_mark);
      if (!d.prog_mark) { if (why) *why = err; return nullptr; }
      a.glUseProgram(d.prog_mark); sampler(a, d.prog_mark, "s_ids", 5);
    }
    a.glDeleteShader(vs_fs); a.glDeleteShader(fs_null);
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
  std::fprintf(stderr, "gpu lean (OpenGL ES): %s | %s; 1x%s, %s depth/stencil%s%s%s%s; warm-up %.0f ms\n", a.glGetString(GL_RENDERER), a.glGetString(GL_VERSION), " (no MSAA)",
               d24 ? "D24S8" : "D32S8", "", d.fetch ? (d.fetch == 2 ? ", one-pass translucency (DS blend)" : ", one-pass translucency") : "", d.indexed ? "" : ", no per-attachment blend", d.egl_sync ? "" : ", no EGL fences (frames finish synchronously)", now_ms() - t_warm);
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
  a.glDepthMask((kind == P_OPAQUE || kind == P_OPAQUE_EARLY || kind == P_TRANS_OPQ || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW || kind == P_TRANS_ONE_DW) ? 1 : 0);
  GLenum cmp = GL_ALWAYS, zfail = GL_KEEP, pass = GL_KEEP; GLuint cmask = 0, wmask = 0;
  switch (kind) {
    case P_OPAQUE: case P_OPAQUE_EARLY: case P_TRANS_OPQ: pass = GL_REPLACE; wmask = S_DRAWN | S_T | S_CODE; break;
    case P_TRANS_A: case P_TRANS_A_DW: cmp = GL_NOTEQUAL; cmask = S_DRAWN; pass = GL_REPLACE; wmask = S_DRAWN | S_T | S_CODE; break;
    case P_TRANS_B: case P_TRANS_B_DW: case P_TRANS_ONE: case P_TRANS_ONE_DW: cmp = d.nostencil ? GL_ALWAYS : GL_NOTEQUAL; cmask = S_DRAWN | S_T | S_CODE; pass = GL_REPLACE; wmask = S_DRAWN | S_T | S_CODE; break;
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
    a.glColorMaski(0, col, col, col, col); a.glColorMaski(1, col, ids, ids, 0); a.glColorMaski(2, ids, 0, 0, 0);   // the edge byte and the id: the opaque passes' alone
    if (kind == P_TRANS_ONE || kind == P_TRANS_ONE_DW) {   // the shader blends the colour (framebuffer fetch); the fog bit's MIN stays here
      a.glDisablei(GL_BLEND, 0);
      a.glEnablei(GL_BLEND, 1); a.glBlendFuncSeparatei(1, GL_ONE, GL_ONE, GL_ONE, GL_ONE); a.glBlendEquationSeparatei(1, GL_MIN, GL_MIN);
    } else if (blend) {
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

bool Lean::Impl::build_draw(u32 wire, u32 ea, u32 sp, u32 wd, u32 v, std::string* err) {
  Api& g = *a;
  const std::string fetch_defs = "#extension GL_ARM_shader_framebuffer_fetch : require\n#define DS_ALPHA_SEL 2\n#define DS_FETCH " + std::to_string(fetch) + "\n";
  const std::string variant_defs[V_COUNT] = {"", "#define DS_EARLY 1\n", "#define DS_ALPHA_SEL 1\n", "#define DS_ALPHA_SEL 2\n", fetch_defs};
  const std::string defs = variant_defs[v] + (wd && !wclip ? "#define DS_WDEPTH 1\n" : "") + (sp == 1 ? "#define DS_TOON false\n" : "") + (ea ? "#define DS_EDGE_AA 1\n" : "") + (wire ? "#define DS_WIRE 1\n" : "");
  const GLuint fs = compile(g, GL_FRAGMENT_SHADER, source(text_lean_frag(), defs), err);
  GLuint& prog = prog_draw[wire][ea][sp][wd][v];
  prog = link(g, vs_lean[ea], fs, err);
  if (fs) g.glDeleteShader(fs);
  if (!prog) return false;
  g.glUseProgram(prog); bound_prog = prog; sampler(g, prog, "polytex", 0); sampler(g, prog, "verttex", 1);
  loc_base_draw[wire][ea][sp][wd][v] = g.glGetUniformLocation(prog, "u_base");
  return true;
}

GLint Lean::Impl::use_kind(u32 kind, bool wbuf, bool eq, u32 ref, bool wire) {
  GLuint prog; GLint loc;
  if (kind == P_MASK_CLEAR) { prog = prog_mask; loc = -1; }
  else if (kind == P_SHADOW_PREP) { prog = prog_prep; loc = loc_base_prep; }
  else {
    const u32 v = kind == P_OPAQUE_EARLY ? V_EARLY : kind == P_TRANS_OPQ ? V_OPQ : (kind == P_TRANS_ONE || kind == P_TRANS_ONE_DW) ? V_FETCH : (kind == P_TRANS_A || kind == P_TRANS_B || kind == P_TRANS_A_DW || kind == P_TRANS_B_DW) ? V_TRANS : V_PLAIN;
    const u32 set = toon_now ? 0u : 1u;
    u32 wi = wire && v != V_EARLY ? 1u : 0u;
    const u32 ea = aa_now ? 1u : 0u, wd = wbuf ? 1u : 0u;
    if (wi && !prog_draw[1][ea][set][wd][v]) {   // the first such run: built here
      std::string err;
      if (!build_draw(1, ea, set, wd, v, &err)) { std::fprintf(stderr, "gpu lean (OpenGL ES): wireframe program: %s\n", err.c_str()); wi = 0; }   // drawn filled
    }
    prog = prog_draw[wi][ea][set][wd][v]; loc = loc_base_draw[wi][ea][set][wd][v];
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
  g.glBindVertexArray(vao);
  g.glViewport(0, 0, W, H);
  g.glEnable(GL_STENCIL_TEST);
  for (u32 ea = 0; ea < (aa ? 2u : 1u); ++ea)
  for (u32 sp = 0; sp < 2; ++sp)
  for (u32 wd = 0; wd < 2; ++wd)
    for (u32 kind = 0; kind < P_COUNT; ++kind) {
      if ((kind == P_TRANS_ONE || kind == P_TRANS_ONE_DW) && !fetch) continue;
      if (kind == 0) g.glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw[ea]);
      toon_now = sp == 0; aa_now = ea != 0;
      const GLint loc = use_kind(kind, wd != 0, false, 0);
      if (loc >= 0) g.glUniform1i(loc, 0);
      if (kind == P_MASK_CLEAR) g.glDrawArrays(GL_TRIANGLES, 0, 3); else g.glDrawArraysInstanced(GL_TRIANGLES, 0, 24, 1);
    }
  g.glDisable(GL_STENCIL_TEST); g.glDisable(GL_DEPTH_TEST); g.glDepthMask(0);
  if (indexed) { g.glDisablei(GL_BLEND, 0); g.glDisablei(GL_BLEND, 1); g.glColorMaski(0, 1, 1, 1, 1); g.glColorMaski(1, 1, 1, 1, 1); g.glColorMaski(2, 1, 1, 1, 1); }
  else g.glDisable(GL_BLEND);
  g.glColorMask(1, 1, 1, 1);
  g.glBindFramebuffer(GL_FRAMEBUFFER, fbo_res);
  g.glActiveTexture(GL_TEXTURE0 + 2); g.glBindTexture(GL_TEXTURE_2D, t_col);
  g.glActiveTexture(GL_TEXTURE0 + 3); g.glBindTexture(GL_TEXTURE_2D, t_ds);
  for (u32 ea = 0; ea < (aa ? 2u : 1u); ++ea) {
    g.glActiveTexture(GL_TEXTURE0 + 4); g.glBindTexture(GL_TEXTURE_2D, t_fog[ea]);
    g.glUseProgram(prog_resolve[ea]); g.glDrawArrays(GL_TRIANGLES, 0, 3);
    bound_prog = prog_resolve[ea];
  }
  aa_now = false;
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
  // Polygons of four or more vertices as fans around their centre (lean.vert).
  const bool centre = true;
  const bool marking = d.mark && d.mark_on && (f.dispcnt & 0x20u) != 0u;   // edge marking this frame
  if (marking) f.flags2 |= DS_FF2_EDGEMARK;
  d.aa_now = d.aa && (f.flags2 & DS_FF2_GAME_AA) != 0u;   // the game's bit under video.aa: this frame's edges are anti-aliased
  if (centre) f.flags2 |= DS_FF2_CENTRE;
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
  // Fog where a pixel can carry the flag only: an opaque pixel takes its
  // polygon's fog bit, a translucent one can only clear it (melonDS), so
  // with neither the rear plane's nor any polygon's bit set no pixel is
  // fogged, and DISP3DCNT bit 7 goes off for the frame: the resolve skips
  // fog (and the GLES raster drops depth and fog bit, below). Exact.
  if ((f.dispcnt & 0x80u) && !(f.clear_attr & 0x8000u) && std::none_of(gp, gp + npoly, [](const GpuPoly& p) { return ((p.attr >> 15) & 1u) != 0u; })) f.dispcnt &= ~0x80u;
  const GpuVert* gv = d.host_verts.data();
  // Each polygon's texcoord range for the fragment stage's clamp.
  for (u32 k = 0; k < npoly; ++k) {
    GpuPoly& p = gpw[k];
    s32 s0 = 0x7FFF, s1 = -0x8000, t0 = 0x7FFF, t1 = -0x8000;
    for (u32 v = 0; v < p.nverts; ++v) { const GpuVert& q = gv[p.first_vert + v]; s0 = std::min(s0, q.s); s1 = std::max(s1, q.s); t0 = std::min(t0, q.t); t1 = std::max(t1, q.t);; }
    auto pk = [](s32 lo, s32 hi) { return static_cast<u32>(std::clamp(lo + 32768, 0, 65535)) | (static_cast<u32>(std::clamp(hi + 32768, 0, 65535)) << 16); };
    p.row_base = pk(s0, s1); p.pad_ = pk(t0, t1);
  }
  // The frame's id codes (vk_lean.cpp: the stencil layout).
  u8 code[64] = {};
  {
    u32 next = 1;
    auto want = [&](u32 id) { if (!code[id]) code[id] = static_cast<u8>(next <= 31 ? next++ : (id & 31)); };
    for (u32 k = 0; k < npoly; ++k) if (gp[k].flags & (DS_PF_TRANSLUCENT | DS_PF_SHADOW)) want((gp[k].attr >> 24) & 0x3F);
  }
  auto code_of = [&](const GpuPoly& p) -> u32 { return code[(p.attr >> 24) & 0x3F]; };
  auto wire_poly = [](const GpuPoly& p) { return ((p.attr >> 16) & 0x1Fu) == 0u && !(p.flags & (DS_PF_SHADOW | DS_PF_SHADOW_MASK)); };   // gl_lean.vert has the same
  d.toon_now = std::any_of(gp, gp + npoly, [](const GpuPoly& p) { return ((p.attr >> 4) & 3u) == 2u; });   // which program set draws

  // The opaque prefix reordered for the hardware (vk_lean.cpp): polygons
  // that cannot fail the alpha test first, front to back, then the cut-out
  // textured ones. Into `sorted`, which is the slot's polygon list.
  const u32 first_ordered = std::min(f.first_ordered, npoly);
  u32 n_early = 0;
  GpuPoly* const out_p = d.sorted.data();
  if (first_ordered) {
    auto nearest = [&](const GpuPoly& p) -> s32 { const GpuVert* v = gv + p.first_vert; s32 zmin = 0x7FFFFFFF; for (u32 k = 0; k < p.nverts; ++k) zmin = std::min(zmin, v[k].z); return zmin; };
    d.key.clear(); d.key.reserve(first_ordered);
    // Not for the early depth test: polygons that discard (the alpha test; a wireframe's inside, gl_lean.frag).
    auto late = [](const GpuPoly& p) { return (p.flags & DS_PF_TEX_ALPHA) != 0 || ((p.attr >> 16) & 0x1Fu) == 0u; };
    for (u32 k = 0; k < first_ordered; ++k) { const bool at = late(gp[k]), eq = (gp[k].attr & 0x4000u) != 0; d.key.emplace_back((at ? (s64{1} << 40) : 0) + (eq ? (s64{1} << 38) : 0) + (s64{code_of(gp[k])} << 32) + nearest(gp[k]), k); }
    std::sort(d.key.begin(), d.key.end());
    for (u32 k = 0; k < first_ordered; ++k) { out_p[k] = gp[d.key[k].second]; if (!late(out_p[k])) ++n_early; }
  }
  if (npoly > first_ordered) std::memcpy(out_p + first_ordered, gp + first_ordered, sizeof(GpuPoly) * (npoly - first_ordered));

  // The slot's device copies.
  if (npoly) std::memcpy(d.map_polys[slot], out_p, sizeof(GpuPoly) * npoly);
  std::memcpy(d.map_post[slot], &d.host_post, sizeof(GpuPost));
  { GpuFrame fs = f; fs.scale = d.scale; std::memcpy(d.map_frame[slot], &fs, sizeof(GpuFrame)); }
  a.glActiveTexture(GL_TEXTURE0); a.glBindTexture(GL_TEXTURE_2D, d.tex_polys[slot]);
  if (npoly) a.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kPullW, static_cast<GLsizei>((npoly * 4 + kPullW - 1) / kPullW), GL_RGBA_INTEGER, GL_UNSIGNED_INT, out_p);
  a.glActiveTexture(GL_TEXTURE0 + 1); a.glBindTexture(GL_TEXTURE_2D, d.tex_verts[slot]);
  if (nvert) a.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kPullW, static_cast<GLsizei>((nvert * 3 + kPullW - 1) / kPullW), GL_RGBA_INTEGER, GL_UNSIGNED_INT, gv);
  a.glBindBufferBase(GL_UNIFORM_BUFFER, 0, d.ubo_frame[slot]);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, d.ssbo_polys[slot]);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, d.ssbo_texels);
  a.glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, d.ssbo_post[slot]);
  a.glBindVertexArray(d.vao);

  const double t_rec = now_ms();
  if (d.timing) a.glBeginQueryEXT(GL_TIME_ELAPSED_EXT, d.query[slot]);
  a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_draw[d.aa_now ? 1 : 0]);
  a.glViewport(0, 0, W, H);
  a.glDisable(GL_SCISSOR_TEST); a.glDisable(GL_CULL_FACE); a.glDisable(GL_DITHER);
  // Clear: the rear plane's colour and alpha; depth; stencil drawn where the rear plane has alpha; the fog bit; no record.
  {
    a.glColorMask(1, 1, 1, 1); a.glDepthMask(1); a.glStencilMask(0xFF);
    if (d.indexed) { a.glColorMaski(0, 1, 1, 1, 1); a.glColorMaski(1, 1, 1, 1, 1); a.glColorMaski(2, 1, 1, 1, 1); }
    const u32 c = f.clear_color, ca = (c >> 24) & 0x1F;
    const float col[4] = {static_cast<float>(c & 0x3F) / 255.f, static_cast<float>((c >> 8) & 0x3F) / 255.f, static_cast<float>((c >> 16) & 0x3F) / 255.f, ca == 0 ? 0.f : static_cast<float>(std::min(255u, (ca + 1) * 8)) / 255.f};
    const float fog[4] = {(f.clear_attr & 0x8000) ? 1.f : 0.f, 1.f, static_cast<float>((f.clear_attr >> 24) & 0x3F) / 255.f, 0.f};   // no edge; the rear plane's id
    const float cz = static_cast<float>(f.clear_depth & 0xFFFFFF);
    a.glClearBufferfv(GL_COLOR, 0, col);
    a.glClearBufferfv(GL_COLOR, 1, fog);
    if (d.mark) { const GLuint cw[4] = {((f.clear_attr >> 24) & 0x3Fu) | ((f.clear_depth & 0xFFFFFFu) << 8), 0, 0, 0}; a.glClearBufferuiv(GL_COLOR, 2, cw); }
    a.glClearBufferfi(GL_DEPTH_STENCIL, 0, wbuf && d.wclip ? std::min(1.f, wscale_f / std::max(cz, 1.f)) : std::min(1.f, cz / 16777215.f), ca ? static_cast<GLint>(S_DRAWN | code[(f.clear_attr >> 24) & 0x3F]) : 0);
  }
  a.glEnable(GL_STENCIL_TEST);
  u32 draws = 0;
  auto use = [&](u32 kind, bool eq, u32 ref, bool wire = false) { return d.use_kind(kind, wbuf, eq, ref, wire); };
  // A draw's vertex count is its largest polygon's fan (B7): most runs hold triangles and quads only, 6 vertices
  // an instance instead of the 24 a 10-gon needs (lean_runs.h).
  auto draw = [&](u32 kind, GLint loc, u32 first, u32 count) { if ((g_skip >> kind) & 1) return; if (loc >= 0) a.glUniform1i(loc, static_cast<GLint>(first)); a.glDrawArraysInstanced(GL_TRIANGLES, 0, static_cast<GLsizei>(fan_vertices(out_p, first, count, centre)), static_cast<GLsizei>(count)); ++draws; };
  if (first_ordered) {
    for (u32 k = 0; k < first_ordered;) {
      const u32 c = code_of(out_p[k]); const bool early = k < n_early, eq = (out_p[k].attr & 0x4000u) != 0, wire = wire_poly(out_p[k]);
      u32 j = k + 1;
      while (j < first_ordered && code_of(out_p[j]) == c && (j < n_early) == early && ((out_p[j].attr & 0x4000u) != 0) == eq && wire_poly(out_p[j]) == wire) ++j;
      draw(early ? 0 : 1, use(early ? P_OPAQUE_EARLY : P_OPAQUE, eq, S_DRAWN | c, wire), k, j - k);
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
    const bool trans = (p.flags & DS_PF_TRANSLUCENT) != 0, dw = (p.attr & (1u << 11)) != 0, eq = (p.attr & 0x4000u) != 0, wire = wire_poly(p);
    u32 j = i + 1;
    while (j < npoly) {
      const GpuPoly& q = gp[j];
      if (((q.attr >> 4) & 3) != mode || ((q.attr >> 24) & 0x3F) != id || ((q.attr & 0x4000u) != 0) != eq || ((q.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT)) != (p.flags & (DS_PF_SHADOW_MASK | DS_PF_SHADOW | DS_PF_TRANSLUCENT))) || ((q.attr & (1u << 11)) != 0) != dw || wire_poly(q) != wire) break;
      ++j;
    }
    const u32 n = j - i;
    if (mask) {
      if (any_mask && !prev_mask && !((g_skip >> 7) & 1)) { use(P_MASK_CLEAR, eq, 0); a.glDrawArrays(GL_TRIANGLES, 0, 3); ++draws; }
      any_mask = true;
      draw(7, use(P_MASK, eq, S_MASK), i, n);
    } else if (shadow) {
      draw(5, use(P_SHADOW_PREP, eq, c), i, n);
      draw(6, use(P_SHADOW, eq, S_T | c), i, n);
    } else if (trans) {
      u32 lo[vk::kA31Ranges], cnt[vk::kA31Ranges];   // the alpha-31 pass over the run's alpha-31 polygons alone (vk_lean.cpp)
      if (const u32 nr = vk::alpha_31_ranges(gp, i, j, lo, cnt)) { const GLint loc = use(P_TRANS_OPQ, eq, S_DRAWN | c, wire); for (u32 r = 0; r < nr; ++r) draw(2, loc, lo[r], cnt[r]); }
      const u32 npart = vk::alpha_part_ranges(gp, i, j, lo, cnt);   // the rest over the polygons that can have a pixel below 31
      if (!npart) {}
      else if (d.fetch) { const GLint loc = use(dw ? P_TRANS_ONE_DW : P_TRANS_ONE, eq, S_DRAWN | S_T | c, wire); for (u32 r = 0; r < npart; ++r) draw(4, loc, lo[r], cnt[r]); }   // A and B in one pass, the shader blending
      else {
        if (!d.nostencil) { const GLint loc = use(dw ? P_TRANS_A_DW : P_TRANS_A, eq, S_DRAWN | S_T | c, wire); for (u32 r = 0; r < npart; ++r) draw(3, loc, lo[r], cnt[r]); }
        const GLint loc = use(dw ? P_TRANS_B_DW : P_TRANS_B, eq, S_DRAWN | S_T | c, wire); for (u32 r = 0; r < npart; ++r) draw(4, loc, lo[r], cnt[r]);
      }
    } else {
      draw(8, use(P_OPAQUE, eq, S_DRAWN | c, wire), i, n);
    }
    prev_mask = mask;
    i = j;
    if (draws > kMaxDraws) break;
  }
  a.glDisable(GL_STENCIL_TEST); a.glDisable(GL_DEPTH_TEST); a.glDepthMask(0);
  if (d.indexed) { a.glDisablei(GL_BLEND, 0); a.glDisablei(GL_BLEND, 1); a.glColorMaski(0, 1, 1, 1, 1); a.glColorMaski(1, 1, 1, 1, 1); a.glColorMaski(2, 1, 1, 1, 1); }
  else a.glDisable(GL_BLEND);
  a.glColorMask(1, 1, 1, 1);
  // What the resolve won't read stays in the tile: the stencil always, depth
  // and the fog bit unless the frame fogs.
  // Without this the Mali writes them to memory at the switch.
  {
    const GLenum all[2] = {GL_DEPTH_STENCIL_ATTACHMENT, GL_COLOR_ATTACHMENT1}, stencil = GL_STENCIL_ATTACHMENT;
    if (!(f.dispcnt & 0x80u)) a.glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, all); else a.glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &stencil);
    if (d.mark && !marking) { const GLenum idw = GL_COLOR_ATTACHMENT0 + 2; a.glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &idw); }   // the id word, unless the marking pass reads it
  }
  // The resolve into the record, and the record to the slot's PBO. A frame without fog
  // has nothing to resolve: its colour attachment is the record (Impl::copy_out decodes the alpha).
  const bool direct = d.direct && !(f.dispcnt & 0x80u);
  d.direct_slot[slot] = direct;
  if (!direct) {
    a.glBindFramebuffer(GL_FRAMEBUFFER, d.fbo_res);
    a.glActiveTexture(GL_TEXTURE0 + 2); a.glBindTexture(GL_TEXTURE_2D, d.t_col);
    a.glActiveTexture(GL_TEXTURE0 + 3); a.glBindTexture(GL_TEXTURE_2D, d.t_ds);
    a.glActiveTexture(GL_TEXTURE0 + 4); a.glBindTexture(GL_TEXTURE_2D, d.t_fog[d.aa_now ? 1 : 0]);
    a.glUseProgram(d.prog_resolve[d.aa_now ? 1 : 0]); d.bound_prog = d.prog_resolve[d.aa_now ? 1 : 0];
    if (!((g_skip >> 9) & 1)) { a.glDrawArrays(GL_TRIANGLES, 0, 3); ++draws; }
  }
  if (marking) {   // the record's edge pixels recoloured (gl_mark.frag); the framebuffer stays bound for the read below
    a.glBindFramebuffer(GL_FRAMEBUFFER, direct ? d.fbo_col : d.fbo_res);
    a.glActiveTexture(GL_TEXTURE0 + 5); a.glBindTexture(GL_TEXTURE_2D, d.t_ids);
    a.glUseProgram(d.prog_mark); d.bound_prog = d.prog_mark;
    a.glEnable(GL_BLEND); a.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE); a.glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    a.glColorMask(1, 1, 1, 0);
    a.glDrawArrays(GL_TRIANGLES, 0, 3); ++draws;
    a.glDisable(GL_BLEND); a.glColorMask(1, 1, 1, 1);
  }
  a.glPixelStorei(GL_PACK_ALIGNMENT, 4);
  a.glBindBuffer(GL_PIXEL_PACK_BUFFER, d.pbo_out[slot]);
  a.glReadBuffer(GL_COLOR_ATTACHMENT0);
  if (!((g_skip >> 10) & 1)) a.glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  a.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  if (d.timing) a.glEndQueryEXT(GL_TIME_ELAPSED_EXT);
  const double t_q = now_ms();
  EGLSyncKHR sync = nullptr;
  if (d.egl_sync) { sync = a.eglCreateSyncKHR(d.dpy, EGL_SYNC_FENCE_KHR, nullptr); a.glFlush(); }
  d.flush_at[slot] = now_ms();
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
    if (!sync) { d.valid[slot] = true; d.copy_out(slot); }
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
