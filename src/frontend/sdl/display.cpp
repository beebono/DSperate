// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {

bool Display::open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, Layout layout_mode, bool accel) {
  layout_ = layout_mode;
  const bool across = layout_ == Layout::Horizontal;
  const int w = static_cast<int>(SCREEN_W) * (across ? 2 : 1) * scale, h = static_cast<int>(SCREEN_H) * (across ? 1 : 2) * scale;
  const u32 flags = static_cast<u32>(SDL_WINDOW_RESIZABLE) | (fullscreen ? static_cast<u32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
  win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
  if (!win_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
  fullscreen_ = fullscreen;

  // Per-scanline scaling renders into the window surface, which cannot
  // coexist with an SDL_Renderer on the same window, so it is decided here
  // and the renderer is skipped entirely.
  //
  // On by default: against the software renderer it removes SDL's texture
  // upload, scaled blit and surface copy in favour of one write (etody, both
  // boards: ~15 % less emu+present work per frame, and the shoulders of the
  // over-budget clusters with it). --accel keeps the GLES renderer and
  // --linear the renderer's smooth scaling, both of which need draw();
  // DS_SCANLINE_SCALE=0/1 overrides either way.
  const char* sl = std::getenv("DS_SCANLINE_SCALE");
  scaled_ = sl && *sl ? std::strcmp(sl, "0") != 0 : !accel && !linear;
  if (scaled_) {
    if (accel) std::fprintf(stderr, "DS_SCANLINE_SCALE renders on the CPU; --accel ignored\n");
    if (!SDL_GetWindowSurface(win_)) {
      std::fprintf(stderr, "window surface unavailable (%s); using the framebuffer path\n", SDL_GetError());
      scaled_ = false;
    } else {
      layout();
      build_scale();
      std::fprintf(stderr, "video: window surface, %s driver, scanline scaling\n", SDL_GetCurrentVideoDriver());
      return true;
    }
  }

  // Software by default, which is not the obvious choice and was measured.
  //
  // Everything the emulator draws is already in a CPU buffer, so the only
  // work the GPU does is scale 256x192 per screen up to the panel -- which a
  // Mali does nearly for free, while the CPU does not. That reasoning is
  // wrong on these handhelds: the GLES path also costs a texture upload per
  // frame and, more to the point, the driver's own threads, on a four-core
  // board where the emulation thread, the 2D engine-B worker and two or three
  // raster workers already want every core.
  //
  // etody, 1800 frames, SDL fullscreen on two RG DS boards -- software
  // against opengles2: 14173 ms and 14452 against 15426 and 15508, with
  // over-budget frames 112 and 142 against 305 and 294, and p99 ~2.7 ms
  // lower. The median is marginally *worse* (the CPU scale is a small fixed
  // cost per frame) and the tail is much better (the driver is not competing
  // for a core). --accel selects GLES, which is likely the better choice
  // anywhere the GPU is not sharing a die with four A55s.
  const u32 rflags = (accel ? static_cast<u32>(SDL_RENDERER_ACCELERATED) : static_cast<u32>(SDL_RENDERER_SOFTWARE))
                   | (vsync ? static_cast<u32>(SDL_RENDERER_PRESENTVSYNC) : 0u);
  ren_ = SDL_CreateRenderer(win_, -1, rflags);
  if (!ren_ && accel) {   // KMSDRM without GLES, or a headless test box
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
  surf_ = nullptr;   // owned by SDL, freed with the window
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
  if (!out_size(w, h)) return;
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
  build_scale();
  margins_dirty_ = true;
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

// ---- per-scanline scaling ---------------------------------------------------

// The renderer's output size, or the window surface's when there is no
// renderer. Both are in pixels, which is what the views are in.
bool Display::out_size(int& w, int& h) const {
  if (ren_) return SDL_GetRendererOutputSize(ren_, &w, &h) == 0;
  if (!win_) return false;
  SDL_Surface* s = SDL_GetWindowSurface(win_);
  if (!s) return false;
  w = s->w; h = s->h;
  return true;
}

void Display::build_scale() {
  if (!scaled_ || !win_) return;
  surf_ = SDL_GetWindowSurface(win_);        // recreated by SDL on resize
  if (!surf_) { scaled_ = false; return; }
  if (surf_->format->BytesPerPixel != 4) {
    std::fprintf(stderr, "window surface is %d bpp, not 32; using the framebuffer path\n",
                 surf_->format->BytesPerPixel);
    scaled_ = false;
    return;
  }
  scaled_w_ = surf_->w; scaled_h_ = surf_->h;

  // Inverse of the dst_x -> src_x = dst_x * SCREEN_W / rect.w map used by
  // draw() and map_point(), so the two paths land pixels in the same places:
  // source pixel s covers [xrun[s], xrun[s+1]). Both views are the same
  // width, so one table serves both.
  const int rw = views_[0].rect.w;
  xrun_.resize(static_cast<size_t>(SCREEN_W) + 1);
  for (u32 i = 0; i <= SCREEN_W; ++i)
    xrun_[i] = static_cast<u16>((static_cast<u32>(i) * rw + SCREEN_W - 1) / SCREEN_W);
}

// The screen rects are overwritten in full every frame, so only the letterbox
// around them is cleared, and only when the layout changed under it -- the
// surface keeps its contents between frames. On a panel the screens fill
// exactly there is nothing to clear at all.
void Display::clear_margins(u32* px, u32 pitch) const {
  int x0 = scaled_w_, y0 = scaled_h_, x1 = 0, y1 = 0;
  for (const View& v : views_) {
    x0 = std::min(x0, v.rect.x); y0 = std::min(y0, v.rect.y);
    x1 = std::max(x1, v.rect.x + v.rect.w); y1 = std::max(y1, v.rect.y + v.rect.h);
  }
  auto band = [&](int by0, int by1, int bx0, int bx1) {
    if (by1 <= by0 || bx1 <= bx0) return;
    for (int y = by0; y < by1; ++y)
      std::memset(px + static_cast<size_t>(y) * pitch + bx0, 0, static_cast<size_t>(bx1 - bx0) * sizeof(u32));
  };
  band(0, y0, 0, scaled_w_);                 // above
  band(y1, scaled_h_, 0, scaled_w_);         // below
  band(y0, y1, 0, x0);                       // left
  band(y0, y1, x1, scaled_w_);               // right
}

bool Display::begin_frame(Target out[SCREENS]) {
  if (!scaled_) return false;
  // SDL hands back a new surface after a resize; the pointer is only valid
  // until then, so it is fetched every frame rather than cached across one.
  SDL_Surface* s = SDL_GetWindowSurface(win_);
  if (!s) return false;
  if (s != surf_ || s->w != scaled_w_ || s->h != scaled_h_) {
    layout();
    build_scale();
    if (!scaled_) return false;
    margins_dirty_ = true;
  }
  if (SDL_MUSTLOCK(s) && SDL_LockSurface(s) != 0) {
    std::fprintf(stderr, "SDL_LockSurface: %s\n", SDL_GetError());
    return false;
  }
  u32* base = static_cast<u32*>(s->pixels);
  const u32 stride = static_cast<u32>(s->pitch) / sizeof(u32);
  if (margins_dirty_) { clear_margins(base, stride); margins_dirty_ = false; }
  for (const View& v : views_)
    out[v.screen] = Target{base + static_cast<size_t>(v.rect.y) * stride + v.rect.x,
                           stride, static_cast<u32>(v.rect.h), xrun_.data()};
  return true;
}

void Display::end_frame() {
  if (SDL_MUSTLOCK(surf_)) SDL_UnlockSurface(surf_);
  SDL_UpdateWindowSurface(win_);
}

} // namespace ds::sdl
