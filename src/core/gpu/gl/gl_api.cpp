// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gl/gl_api.h"

#include <dlfcn.h>
#include <mutex>
#include <string>

namespace ds::gpu::gl {

namespace {
void* g_egl = nullptr;
void* g_gles = nullptr;

// dlsym first (the exported ES core), eglGetProcAddress for the rest
// (EGL_KHR_get_all_proc_addresses lets it serve core names too).
template <typename F> void resolve(F& fn, const char* name, void* (*gpa)(const char*)) {
  void* p = g_gles ? dlsym(g_gles, name) : nullptr;
  if (!p && g_egl) p = dlsym(g_egl, name);
  if (!p && gpa) p = gpa(name);
  fn = reinterpret_cast<F>(p);
}
template <typename F> void resolve2(F& fn, const char* a, const char* b, void* (*gpa)(const char*)) {
  resolve(fn, a, gpa);
  if (!fn) resolve(fn, b, gpa);
}
}

bool Api::load(std::string* why) {
  if (loaded) return true;
  static std::mutex m;
  std::lock_guard<std::mutex> lk(m);
  if (loaded) return true;
  if (!g_egl) { g_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL); if (!g_egl) g_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL); }
  if (!g_gles) { g_gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL); if (!g_gles) g_gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL); }
  if (!g_egl || !g_gles) { if (why) *why = !g_egl ? "no libEGL" : "no libGLESv2"; return false; }
  resolve(eglGetProcAddress, "eglGetProcAddress", nullptr);
  void* (*gpa)(const char*) = eglGetProcAddress;
#define R(n) resolve(n, #n, gpa)
  R(eglGetDisplay); R(eglInitialize); R(eglChooseConfig); R(eglCreateContext); R(eglDestroyContext); R(eglCreatePbufferSurface); R(eglDestroySurface);
  R(eglMakeCurrent); R(eglGetCurrentContext); R(eglBindAPI); R(eglQueryString); R(eglGetError);
  R(eglCreateSyncKHR); R(eglDestroySyncKHR); R(eglClientWaitSyncKHR); R(eglGetSyncAttribKHR);
  R(glGetString); R(glGetStringi); R(glGetIntegerv); R(glGetError);
  R(glCreateShader); R(glShaderSource); R(glCompileShader); R(glGetShaderiv); R(glGetShaderInfoLog); R(glDeleteShader);
  R(glCreateProgram); R(glAttachShader); R(glLinkProgram); R(glGetProgramiv); R(glGetProgramInfoLog); R(glDeleteProgram); R(glUseProgram);
  R(glGetUniformLocation); R(glUniform1i); R(glGetUniformBlockIndex); R(glUniformBlockBinding);
  R(glGenBuffers); R(glDeleteBuffers); R(glBindBuffer); R(glBindBufferBase); R(glBindBufferRange); R(glBufferData); R(glBufferSubData);
  R(glMapBufferRange); R(glUnmapBuffer); R(glFlushMappedBufferRange);
  R(glGenTextures); R(glDeleteTextures); R(glBindTexture); R(glActiveTexture); R(glTexStorage2D); R(glTexSubImage2D); R(glTexParameteri); R(glPixelStorei);
  R(glGenFramebuffers); R(glDeleteFramebuffers); R(glBindFramebuffer); R(glFramebufferTexture2D); R(glCheckFramebufferStatus); R(glDrawBuffers); R(glReadBuffer); R(glReadPixels);
  R(glInvalidateFramebuffer); R(glClearBufferfv); R(glClearBufferuiv); R(glClearBufferfi);
  R(glViewport); R(glScissor); R(glDepthRangef); R(glEnable); R(glDisable); R(glDepthFunc); R(glDepthMask); R(glStencilFunc); R(glStencilOp); R(glStencilMask); R(glColorMask);
  R(glBlendFuncSeparate); R(glBlendEquationSeparate);
  R(glDrawArrays); R(glDrawArraysInstanced); R(glGenVertexArrays); R(glDeleteVertexArrays); R(glBindVertexArray); R(glFlush); R(glFinish); R(glMemoryBarrier);
  R(glGenQueriesEXT); R(glDeleteQueriesEXT); R(glBeginQueryEXT); R(glEndQueryEXT); R(glGetQueryObjectuivEXT); R(glGetQueryObjectui64vEXT);
#undef R
  resolve2(glBufferStorageEXT, "glBufferStorageEXT", "glBufferStorage", gpa);
  resolve2(glColorMaski, "glColorMaski", "glColorMaskiEXT", gpa); if (!glColorMaski) resolve(glColorMaski, "glColorMaskiOES", gpa);
  resolve2(glEnablei, "glEnablei", "glEnableiEXT", gpa); if (!glEnablei) resolve(glEnablei, "glEnableiOES", gpa);
  resolve2(glDisablei, "glDisablei", "glDisableiEXT", gpa); if (!glDisablei) resolve(glDisablei, "glDisableiOES", gpa);
  resolve2(glBlendFuncSeparatei, "glBlendFuncSeparatei", "glBlendFuncSeparateiEXT", gpa); if (!glBlendFuncSeparatei) resolve(glBlendFuncSeparatei, "glBlendFuncSeparateiOES", gpa);
  resolve2(glBlendEquationSeparatei, "glBlendEquationSeparatei", "glBlendEquationSeparateiEXT", gpa); if (!glBlendEquationSeparatei) resolve(glBlendEquationSeparatei, "glBlendEquationSeparateiOES", gpa);
  const void* required[] = {
    reinterpret_cast<void*>(eglGetDisplay), reinterpret_cast<void*>(eglInitialize), reinterpret_cast<void*>(eglChooseConfig), reinterpret_cast<void*>(eglCreateContext), reinterpret_cast<void*>(eglMakeCurrent), reinterpret_cast<void*>(eglBindAPI),
    reinterpret_cast<void*>(glGetString), reinterpret_cast<void*>(glGetStringi), reinterpret_cast<void*>(glGetIntegerv), reinterpret_cast<void*>(glCreateShader), reinterpret_cast<void*>(glLinkProgram), reinterpret_cast<void*>(glMapBufferRange),
    reinterpret_cast<void*>(glTexStorage2D), reinterpret_cast<void*>(glDrawBuffers), reinterpret_cast<void*>(glClearBufferfi), reinterpret_cast<void*>(glDrawArraysInstanced), reinterpret_cast<void*>(glGenVertexArrays), reinterpret_cast<void*>(glReadPixels), reinterpret_cast<void*>(glBindBufferBase)};
  for (const void* p : required) if (!p) { if (why) *why = "libGLESv2 is not OpenGL ES 3.1"; return false; }
  loaded = true;
  return true;
}

Api& gl_api() { static Api a; return a; }

} // namespace ds::gpu::gl
