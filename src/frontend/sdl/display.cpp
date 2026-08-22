// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"

#include <algorithm>
#include <cstdio>

namespace ds::sdl {

bool Display::open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, Layout layout_mode) {
  layout_ = layout_mode;
  const bool across = layout_ == Layout::Horizontal;
  const int w = static_cast<int>(SCREEN_W) * (across ? 2 : 1) * scale, h = static_cast<int>(SCREEN_H) * (across ? 1 : 2) * scale;
  const u32 flags = static_cast<u32>(SDL_WINDOW_RESIZABLE) | (fullscreen ? static_cast<u32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
  win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
  if (!win_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
  fullscreen_ = fullscreen;

  const u32 rflags = SDL_RENDERER_ACCELERATED | (vsync ? static_cast<u32>(SDL_RENDERER_PRESENTVSYNC) : 0u);
  ren_ = SDL_CreateRenderer(win_, -1, rflags);
  if (!ren_) {   // KMSDRM without GLES, or a headless test box
    std::fprintf(stderr, "accelerated renderer unavailable (%s); falling back to software\n", SDL_GetError());
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!ren_) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "linear" : "nearest");
  for (int i = 0; i < SCREENS; ++i) {
    // The core's framebuffers are 0xAARRGGBB words, which is ARGB8888.
    tex_[i] = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, static_cast<int>(SCREEN_W), static_cast<int>(SCREEN_H));
    if (!tex_[i]) { std::fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return false; }
  }
  layout();

  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(ren_, &info) == 0)
    std::fprintf(stderr, "video: %s renderer, %s driver, vsync %s\n", info.name, SDL_GetCurrentVideoDriver(), (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "on" : "off");

  // Which GL stack actually ended up driving the window (Mesa/panfrost or a
  // vendor blob) is the thing that changes underneath us, so name it.
  using GetString = const unsigned char* (*)(unsigned);
  if (auto gl_get_string = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"))) {
    const unsigned char* rend = gl_get_string(0x1F01);      // GL_RENDERER
    const unsigned char* ver  = gl_get_string(0x1F02);      // GL_VERSION
    if (rend) std::fprintf(stderr, "gl: %s | %s\n", rend, ver ? reinterpret_cast<const char*>(ver) : "?");
  }
  return true;
}

void Display::close() {
  for (auto*& t : tex_) { if (t) SDL_DestroyTexture(t); t = nullptr; }
  if (ren_) { SDL_DestroyRenderer(ren_); ren_ = nullptr; }
  if (win_) { SDL_DestroyWindow(win_); win_ = nullptr; }
}

// Both screens stacked or side by side, aspect preserved, centred. Integer
// scaling is not the default: a 1280x720 handheld panel fits the 256x384
// stack 1.875 times, and rounding that down to 1 would waste most of the
// screen.
void Display::layout() {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(ren_, &w, &h);
  const int sw = static_cast<int>(SCREEN_W), sh = static_cast<int>(SCREEN_H);
  const bool across = layout_ == Layout::Horizontal;
  const int cols = across ? 2 : 1, rows = across ? 1 : 2;
  const double s = std::min(static_cast<double>(w) / (sw * cols), static_cast<double>(h) / (sh * rows));
  const int dw = static_cast<int>(sw * s), dh = static_cast<int>(sh * s);
  const int x = (w - dw * cols) / 2, y = (h - dh * rows) / 2;
  for (int i = 0; i < SCREENS; ++i) views_[i] = View{i, across ? SDL_Rect{x + dw * i, y, dw, dh} : SDL_Rect{x, y + dh * i, dw, dh}};
}

void Display::draw(const u32* const fb[SCREENS]) {
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
  SDL_RenderClear(ren_);
  for (const View& v : views_) {
    SDL_UpdateTexture(tex_[v.screen], nullptr, fb[v.screen], static_cast<int>(SCREEN_W) * 4);
    SDL_RenderCopy(ren_, tex_[v.screen], nullptr, &v.rect);
  }
  SDL_RenderPresent(ren_);
}

void Display::toggle_fullscreen() {
  fullscreen_ = !fullscreen_;
  SDL_SetWindowFullscreen(win_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  layout();
}

bool Display::map_point(int wx, int wy, int& screen, int& sx, int& sy) const {
  for (const View& v : views_) {
    if (wx < v.rect.x || wx >= v.rect.x + v.rect.w || wy < v.rect.y || wy >= v.rect.y + v.rect.h) continue;
    screen = v.screen;
    sx = (wx - v.rect.x) * static_cast<int>(SCREEN_W) / v.rect.w;
    sy = (wy - v.rect.y) * static_cast<int>(SCREEN_H) / v.rect.h;
    return true;
  }
  return false;
}

} // namespace ds::sdl
