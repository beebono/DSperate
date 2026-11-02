// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstddef>
#include <cstdint>
#include <string>

// EGL and OpenGL ES 3.1 as the GL raster (gl_lean.cpp) uses them: the types,
// the enum values and a table of entry points resolved at run time from
// libEGL / libGLESv2 (dlopen; nothing is linked, so a build runs on a
// device without them and the raster declines). Values are the Khronos
// registry's; no headers are needed to build.
namespace ds::gpu::gl {

using EGLDisplay = void*; using EGLConfig = void*; using EGLContext = void*; using EGLSurface = void*; using EGLSyncKHR = void*;
using EGLint = int32_t; using EGLBoolean = unsigned; using EGLenum = unsigned; using EGLTimeKHR = uint64_t;
using GLenum = unsigned; using GLuint = unsigned; using GLint = int; using GLsizei = int; using GLfloat = float; using GLboolean = unsigned char;
using GLchar = char; using GLintptr = intptr_t; using GLsizeiptr = intptr_t; using GLuint64 = uint64_t; using GLbitfield = unsigned; using GLubyte = unsigned char;

constexpr void* EGL_DEFAULT_DISPLAY = nullptr;
constexpr void* EGL_NO_CONTEXT = nullptr;
constexpr void* EGL_NO_SURFACE = nullptr;
constexpr void* EGL_NO_SYNC_KHR = nullptr;
constexpr EGLint EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 1, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES3_BIT = 0x40, EGL_NONE = 0x3038;
constexpr EGLint EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056, EGL_CONTEXT_CLIENT_VERSION = 0x3098, EGL_CONTEXT_MINOR_VERSION_KHR = 0x30FB, EGL_EXTENSIONS = 0x3055, EGL_RED_SIZE = 0x3024;
constexpr EGLenum EGL_OPENGL_ES_API = 0x30A0;
constexpr EGLenum EGL_SYNC_FENCE_KHR = 0x30F9;
constexpr EGLint EGL_SYNC_STATUS_KHR = 0x30F1, EGL_SIGNALED_KHR = 0x30F2, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR = 1, EGL_CONDITION_SATISFIED_KHR = 0x30F6;
constexpr EGLTimeKHR EGL_FOREVER_KHR = 0xFFFFFFFFFFFFFFFFull;

constexpr GLenum GL_NO_ERROR = 0, GL_EXTENSIONS = 0x1F03, GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01, GL_NUM_EXTENSIONS = 0x821D;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892, GL_SHADER_STORAGE_BUFFER = 0x90D2, GL_UNIFORM_BUFFER = 0x8A11, GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_UNPACK_BUFFER = 0x88EC;
constexpr GLenum GL_STATIC_DRAW = 0x88E4, GL_DYNAMIC_DRAW = 0x88E8, GL_STREAM_DRAW = 0x88E0, GL_STREAM_READ = 0x88E1;
constexpr GLbitfield GL_MAP_READ_BIT = 1, GL_MAP_WRITE_BIT = 2, GL_MAP_INVALIDATE_RANGE_BIT = 4, GL_MAP_INVALIDATE_BUFFER_BIT = 8, GL_MAP_FLUSH_EXPLICIT_BIT = 0x10, GL_MAP_UNSYNCHRONIZED_BIT = 0x20;
constexpr GLbitfield GL_MAP_PERSISTENT_BIT_EXT = 0x40, GL_MAP_COHERENT_BIT_EXT = 0x80, GL_DYNAMIC_STORAGE_BIT_EXT = 0x100, GL_CLIENT_STORAGE_BIT_EXT = 0x200;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_NEAREST = 0x2600, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F, GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_R8 = 0x8229, GL_R8UI = 0x8232, GL_R16UI = 0x8234, GL_R32UI = 0x8236, GL_RGBA8 = 0x8058, GL_RGBA32UI = 0x8D70;
constexpr GLenum GL_DEPTH24_STENCIL8 = 0x88F0, GL_DEPTH32F_STENCIL8 = 0x8CAD, GL_DEPTH_COMPONENT = 0x1902, GL_DEPTH_STENCIL = 0x84F9;
constexpr GLenum GL_RED = 0x1903, GL_RGBA = 0x1908, GL_RED_INTEGER = 0x8D94, GL_RGBA_INTEGER = 0x8D99, GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406;
constexpr GLenum GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_STENCIL_ATTACHMENT = 0x821A, GL_DEPTH_ATTACHMENT = 0x8D00, GL_STENCIL_ATTACHMENT = 0x8D20;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_COLOR = 0x1800, GL_DEPTH = 0x1801, GL_STENCIL = 0x1802, GL_DEPTH_STENCIL_TEXTURE_MODE = 0x90EA;
constexpr GLenum GL_IMPLEMENTATION_COLOR_READ_FORMAT = 0x8B9B, GL_IMPLEMENTATION_COLOR_READ_TYPE = 0x8B9A;
constexpr GLenum GL_TRIANGLES = 4, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_CULL_FACE = 0x0B44, GL_DITHER = 0x0BD0;
constexpr GLenum GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_EQUAL = 0x0202, GL_LEQUAL = 0x0203, GL_GREATER = 0x0204, GL_NOTEQUAL = 0x0205, GL_GEQUAL = 0x0206, GL_ALWAYS = 0x0207;
constexpr GLenum GL_KEEP = 0x1E00, GL_REPLACE = 0x1E01, GL_ZERO = 0, GL_ONE = 1, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100, GL_STENCIL_BUFFER_BIT = 0x400;
constexpr GLenum GL_TIME_ELAPSED_EXT = 0x88BF, GL_QUERY_RESULT_EXT = 0x8866, GL_QUERY_RESULT_AVAILABLE_EXT = 0x8867, GL_GPU_DISJOINT_EXT = 0x8FBB;
constexpr GLenum GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_ALREADY_SIGNALED = 0x911A, GL_TIMEOUT_EXPIRED = 0x911B, GL_CONDITION_SATISFIED = 0x911C;
constexpr GLbitfield GL_SYNC_FLUSH_COMMANDS_BIT = 1;
constexpr GLenum GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0x90D6, GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS = 0x90DA, GL_MAX_SHADER_STORAGE_BLOCK_SIZE = 0x90DE;
constexpr GLenum GL_MAX_COLOR_ATTACHMENTS = 0x8CDF, GL_MAX_DRAW_BUFFERS = 0x8824, GL_MAX_TEXTURE_SIZE = 0x0D33, GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0x8B4C;
constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ROW_LENGTH = 0x0D02;
constexpr GLbitfield GL_SHADER_STORAGE_BARRIER_BIT = 0x2000, GL_TEXTURE_UPDATE_BARRIER_BIT = 0x100, GL_BUFFER_UPDATE_BARRIER_BIT = 0x200, GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT_EXT = 0x4000;

// The entry points. Null where the driver lacks one: callers test the
// optional ones (the *i* blend state, the sync and query extensions).
struct Api {
  // EGL
  EGLDisplay (*eglGetDisplay)(void*) = nullptr;
  EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
  EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
  EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;
  EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface) = nullptr;
  EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
  EGLContext (*eglGetCurrentContext)() = nullptr;
  EGLBoolean (*eglBindAPI)(EGLenum) = nullptr;
  const char* (*eglQueryString)(EGLDisplay, EGLint) = nullptr;
  EGLint (*eglGetError)() = nullptr;
  void* (*eglGetProcAddress)(const char*) = nullptr;
  EGLSyncKHR (*eglCreateSyncKHR)(EGLDisplay, EGLenum, const EGLint*) = nullptr;
  EGLBoolean (*eglDestroySyncKHR)(EGLDisplay, EGLSyncKHR) = nullptr;
  EGLint (*eglClientWaitSyncKHR)(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR) = nullptr;
  EGLBoolean (*eglGetSyncAttribKHR)(EGLDisplay, EGLSyncKHR, EGLint, EGLint*) = nullptr;
  // GL
  const GLubyte* (*glGetString)(GLenum) = nullptr;
  const GLubyte* (*glGetStringi)(GLenum, GLuint) = nullptr;
  void (*glGetIntegerv)(GLenum, GLint*) = nullptr;
  GLenum (*glGetError)() = nullptr;
  GLuint (*glCreateShader)(GLenum) = nullptr;
  void (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
  void (*glCompileShader)(GLuint) = nullptr;
  void (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
  void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void (*glDeleteShader)(GLuint) = nullptr;
  GLuint (*glCreateProgram)() = nullptr;
  void (*glAttachShader)(GLuint, GLuint) = nullptr;
  void (*glLinkProgram)(GLuint) = nullptr;
  void (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
  void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void (*glDeleteProgram)(GLuint) = nullptr;
  void (*glUseProgram)(GLuint) = nullptr;
  GLint (*glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
  void (*glUniform1i)(GLint, GLint) = nullptr;
  GLuint (*glGetUniformBlockIndex)(GLuint, const GLchar*) = nullptr;
  void (*glUniformBlockBinding)(GLuint, GLuint, GLuint) = nullptr;
  void (*glGenBuffers)(GLsizei, GLuint*) = nullptr;
  void (*glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
  void (*glBindBuffer)(GLenum, GLuint) = nullptr;
  void (*glBindBufferBase)(GLenum, GLuint, GLuint) = nullptr;
  void (*glBindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) = nullptr;
  void (*glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
  void (*glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;
  void (*glBufferStorageEXT)(GLenum, GLsizeiptr, const void*, GLbitfield) = nullptr;
  void* (*glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = nullptr;
  GLboolean (*glUnmapBuffer)(GLenum) = nullptr;
  void (*glFlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr) = nullptr;
  void (*glGenTextures)(GLsizei, GLuint*) = nullptr;
  void (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
  void (*glBindTexture)(GLenum, GLuint) = nullptr;
  void (*glActiveTexture)(GLenum) = nullptr;
  void (*glTexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
  void (*glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
  void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
  void (*glPixelStorei)(GLenum, GLint) = nullptr;
  void (*glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
  void (*glDeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
  void (*glBindFramebuffer)(GLenum, GLuint) = nullptr;
  void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
  GLenum (*glCheckFramebufferStatus)(GLenum) = nullptr;
  void (*glDrawBuffers)(GLsizei, const GLenum*) = nullptr;
  void (*glReadBuffer)(GLenum) = nullptr;
  void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
  void (*glInvalidateFramebuffer)(GLenum, GLsizei, const GLenum*) = nullptr;
  void (*glClearBufferfv)(GLenum, GLint, const GLfloat*) = nullptr;
  void (*glClearBufferuiv)(GLenum, GLint, const GLuint*) = nullptr;
  void (*glClearBufferfi)(GLenum, GLint, GLfloat, GLint) = nullptr;
  void (*glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  void (*glScissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  void (*glDepthRangef)(GLfloat, GLfloat) = nullptr;
  void (*glEnable)(GLenum) = nullptr;
  void (*glDisable)(GLenum) = nullptr;
  void (*glDepthFunc)(GLenum) = nullptr;
  void (*glDepthMask)(GLboolean) = nullptr;
  void (*glStencilFunc)(GLenum, GLint, GLuint) = nullptr;
  void (*glStencilOp)(GLenum, GLenum, GLenum) = nullptr;
  void (*glStencilMask)(GLuint) = nullptr;
  void (*glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
  void (*glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
  void (*glBlendEquationSeparate)(GLenum, GLenum) = nullptr;
  // Per draw buffer (ES 3.2 core, else EXT/OES_draw_buffers_indexed): null without.
  void (*glColorMaski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
  void (*glEnablei)(GLenum, GLuint) = nullptr;
  void (*glDisablei)(GLenum, GLuint) = nullptr;
  void (*glBlendFuncSeparatei)(GLuint, GLenum, GLenum, GLenum, GLenum) = nullptr;
  void (*glBlendEquationSeparatei)(GLuint, GLenum, GLenum) = nullptr;
  void (*glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
  void (*glDrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei) = nullptr;
  void (*glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
  void (*glDeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
  void (*glBindVertexArray)(GLuint) = nullptr;
  void (*glFlush)() = nullptr;
  void (*glFinish)() = nullptr;
  void (*glMemoryBarrier)(GLbitfield) = nullptr;
  // EXT_disjoint_timer_query: null without.
  void (*glGenQueriesEXT)(GLsizei, GLuint*) = nullptr;
  void (*glDeleteQueriesEXT)(GLsizei, const GLuint*) = nullptr;
  void (*glBeginQueryEXT)(GLenum, GLuint) = nullptr;
  void (*glEndQueryEXT)(GLenum) = nullptr;
  void (*glGetQueryObjectuivEXT)(GLuint, GLenum, GLuint*) = nullptr;
  void (*glGetQueryObjectui64vEXT)(GLuint, GLenum, GLuint64*) = nullptr;

  // Loads libEGL and libGLESv2 and resolves the table; false with the reason
  // when a required entry point is missing. Safe to call more than once.
  bool load(std::string* why);
  bool loaded = false;
};

Api& gl_api();   // the process's one table

} // namespace ds::gpu::gl
