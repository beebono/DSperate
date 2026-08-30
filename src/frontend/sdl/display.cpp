// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"
#include "display_wl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {

namespace {
const char* const kModeNames[] = {"vertical", "horizontal", "single", "pip", "dominant_v", "dominant_h"};
const char* const kCornerNames[] = {"tl", "tr", "bl", "br"};
}

const char* Display::mode_name(Mode m) { return kModeNames[static_cast<int>(m)]; }
const char* Display::corner_name(Corner c) { return kCornerNames[static_cast<int>(c)]; }
bool Display::parse_mode(const std::string& s, Mode& m) {
  for (int i = 0; i < static_cast<int>(Mode::Count); ++i) if (s == kModeNames[i]) { m = static_cast<Mode>(i); return true; }
  return false;
}
bool Display::parse_corner(const std::string& s, Corner& c) {
  for (int i = 0; i < static_cast<int>(Corner::Count); ++i) if (s == kCornerNames[i]) { c = static_cast<Corner>(i); return true; }
  return false;
}

void Display::natural_size(const Layout& l, double scale, int& w, int& h) {
  const double sw = SCREEN_W * scale, sh = SCREEN_H * scale;
  double fw = sw, fh = sh;
  switch (l.mode) {
    case Mode::Vertical:   fh = sh * 2; break;
    case Mode::Horizontal: fw = sw * 2; break;
    case Mode::Single: case Mode::Pip: break;
    case Mode::DominantV:  fh = sh * (1 + l.dominant); break;
    case Mode::DominantH:  fw = sw * (1 + l.dominant); break;
    case Mode::Count: break;
  }
  w = static_cast<int>(fw); h = static_cast<int>(fh);
}

bool Display::open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, const Layout& layout_mode, bool accel, int only_screen, int display_index) {
  layout_ = layout_mode;
  only_screen_ = only_screen;
  display_index_ = display_index;
  nviews_ = only_screen_ >= 0 ? 1 : SCREENS;
  int w = 0, h = 0;
  if (only_screen_ >= 0) { w = static_cast<int>(SCREEN_W) * scale; h = static_cast<int>(SCREEN_H) * scale; }
  else natural_size(layout_, scale, w, h);
  const u32 flags = static_cast<u32>(SDL_WINDOW_RESIZABLE) | (fullscreen ? static_cast<u32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
  win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED_DISPLAY(display_index), SDL_WINDOWPOS_CENTERED_DISPLAY(display_index), w, h, flags);
  if (!win_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
  fullscreen_ = fullscreen;

  // Per-scanline scaling renders into the window surface, which cannot
  // coexist with an SDL_Renderer on the same window, so it is decided here
  // and the renderer is skipped entirely.
  //
  // Default on under Wayland only: there the window surface is a cheap shm
  // attach and the mode removes SDL's texture upload, scaled blit and
  // surface copy in favour of one write (etody, both boards: ~15 % less
  // emu+present work per frame, and the shoulders of the over-budget
  // clusters with it) -- and it is the write path the dmabuf tier builds
  // on. On KMSDRM the window surface is a shadow-buffer blit, not the
  // flipping renderer path, and defaulting to it measured 16 % *slower*
  // (etody 1800 frames: 13985 ms against 12269 through the renderer), so
  // the renderer stays the default there. --accel keeps the GLES renderer
  // and --linear the renderer's smooth scaling, both of which need draw();
  // DS_SCANLINE_SCALE=0/1 overrides either way.
  const char* vd = SDL_GetCurrentVideoDriver();
  const char* sl = std::getenv("DS_SCANLINE_SCALE");
  scaled_ = sl && *sl ? std::strcmp(sl, "0") != 0
                      : !accel && !linear && vd && !std::strcmp(vd, "wayland");
  if (scaled_) {
    if (accel) std::fprintf(stderr, "DS_SCANLINE_SCALE renders on the CPU; --accel ignored\n");
    if (!SDL_GetWindowSurface(win_)) {
      std::fprintf(stderr, "window surface unavailable (%s); using the framebuffer path\n", SDL_GetError());
      scaled_ = false;
    } else {
      layout();
      build_scale();
      // Tier 1 on top of the same scanline path: same targets, but the
      // pixels land in a CMA dmabuf instead of the shm surface.
      const char* dmenv = std::getenv("DS_DMABUF");
      const bool dm_forbidden = dmenv && !std::strcmp(dmenv, "0");
      const bool dm_required = dmenv && !std::strcmp(dmenv, "1");
      if (scaled_ && !dm_forbidden) {
        int w = 0, h = 0;
        out_size(w, h);
        auto dm = std::make_unique<DmabufOut>();
        if (dm->open(win_, w, h, only_screen_ >= 0 ? display_index_ : -1)) dm_ = std::move(dm);
        else if (dm_required) { std::fprintf(stderr, "DS_DMABUF=1 but the dmabuf path failed\n"); return false; }
      }
      std::fprintf(stderr, "video: %s, %s driver, scanline scaling\n",
                   dm_ ? "dmabuf" : "window surface", SDL_GetCurrentVideoDriver());
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
  if (dm_) { dm_->close(); dm_.reset(); }
  surf_ = nullptr;   // owned by SDL, freed with the window
  for (auto*& t : tex_) { if (t) SDL_DestroyTexture(t); t = nullptr; }
  if (ren_) { SDL_DestroyRenderer(ren_); ren_ = nullptr; }
  if (win_) { SDL_DestroyWindow(win_); win_ = nullptr; }
}

// Screen rects for the current mode: aspect preserved, centred. Integer
// scaling is not the default: a 1280x720 handheld panel fits the 256x384
// stack 1.875 times, and rounding that down to 1 would waste most of the
// screen.
void Display::layout() {
  int w = 0, h = 0;
  if (!out_size(w, h)) return;
  const double sw = SCREEN_W, sh = SCREEN_H;
  auto fit = [&](double cols, double rows) { return std::min(w / (sw * cols), h / (sh * rows)); };
  auto rect = [&](double x, double y, double s) { return SDL_Rect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(sw * s), static_cast<int>(sh * s)}; };
  if (only_screen_ >= 0) {
    const double s = fit(1, 1);
    views_[0] = View{only_screen_, rect((w - sw * s) / 2, (h - sh * s) / 2, s), true, true};
    return;
  }
  const int p = layout_.primary, q = 1 - p;
  // Views are drawn in order, so the inset goes last; map_point() looks from
  // the end, so the inset also wins the touch.
  switch (layout_.mode) {
    case Mode::Vertical: case Mode::Horizontal: {
      const bool across = layout_.mode == Mode::Horizontal;
      const double s = across ? fit(2, 1) : fit(1, 2);
      const double dw = sw * s, dh = sh * s;
      const double x = (w - dw * (across ? 2 : 1)) / 2, y = (h - dh * (across ? 1 : 2)) / 2;
      for (int i = 0; i < SCREENS; ++i) {
        const int screen = i == 0 ? p : q;
        views_[i] = View{screen, across ? rect(x + dw * i, y, s) : rect(x, y + dh * i, s), true, true};
      }
      break;
    }
    case Mode::Single: case Mode::Pip: {
      const double s = fit(1, 1);
      const SDL_Rect big = rect((w - sw * s) / 2, (h - sh * s) / 2, s);
      views_[0] = View{p, big, true, true};
      if (layout_.mode == Mode::Single) {
        views_[1] = View{q, SDL_Rect{0, 0, static_cast<int>(SCREEN_W), static_cast<int>(SCREEN_H)}, false, false};
      } else {
        const double s2 = s * layout_.pip;
        const int iw = static_cast<int>(sw * s2), ih = static_cast<int>(sh * s2);
        const bool right = layout_.corner == Corner::TopRight || layout_.corner == Corner::BottomRight;
        const bool bottom = layout_.corner == Corner::BottomLeft || layout_.corner == Corner::BottomRight;
        views_[1] = View{q, SDL_Rect{right ? big.x + big.w - iw : big.x, bottom ? big.y + big.h - ih : big.y, iw, ih}, false, true};
      }
      break;
    }
    case Mode::DominantV: {
      // DS order (top above bottom), the pair centred, each centred across.
      const double r = layout_.dominant, s = fit(1, 1 + r);
      const double bh = sh * s, lh = sh * s * r;
      const double y0 = (h - (bh + lh)) / 2;
      const double sc[2] = {p == 0 ? s : s * r, p == 1 ? s : s * r};
      double y = y0;
      for (int i = 0; i < SCREENS; ++i) { views_[i] = View{i, rect((w - sw * sc[i]) / 2, y, sc[i]), true, true}; y += sh * sc[i]; }
      break;
    }
    case Mode::DominantH: {
      // DS order (top left of bottom), the pair centred, bottoms aligned.
      const double r = layout_.dominant, s = fit(1 + r, 1);
      const double bw = sw * s, lw = sw * s * r, bh = sh * s;
      const double x0 = (w - (bw + lw)) / 2, bottom = (h - bh) / 2 + bh;
      const double sc[2] = {p == 0 ? s : s * r, p == 1 ? s : s * r};
      double x = x0;
      for (int i = 0; i < SCREENS; ++i) { views_[i] = View{i, rect(x, bottom - sh * sc[i], sc[i]), true, true}; x += sw * sc[i]; }
      break;
    }
    case Mode::Count: break;
  }
}

void Display::draw(const u32* const fb[SCREENS]) {
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
  SDL_RenderClear(ren_);
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (!v.shown) continue;
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

void Display::set_layout(const Layout& l) {
  if (only_screen_ >= 0) return;
  const Mode was = layout_.mode;
  layout_ = l;
  if (!fullscreen_ && was != l.mode) {
    // Keep the largest screen's size, resize the window around the new mode.
    int pw = 0;
    for (int i = 0; i < nviews_; ++i) if (views_[i].shown) pw = std::max(pw, views_[i].rect.w);
    int nw = 0, nh = 0;
    natural_size(l, std::max(1.0, static_cast<double>(pw) / SCREEN_W), nw, nh);
    SDL_SetWindowSize(win_, nw, nh);
  }
  layout();
  build_scale();
  margins_dirty_ = true;
}

bool Display::map_point(int wx, int wy, int& screen, int& sx, int& sy) const {
  for (int i = nviews_ - 1; i >= 0; --i) {
    const View& v = views_[i];
    if (!v.shown) continue;
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
  // On the dmabuf tier the window size is the truth: the shm surface can lag
  // a configure by a frame, and the two must not disagree mid-rebuild.
  if (dm_) { SDL_GetWindowSize(win_, &w, &h); return w > 0 && h > 0; }
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
  // source pixel s covers [xrun[s], xrun[s+1]). One table per view, since
  // the views differ in size in the inset and dominant modes.
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    std::vector<u16>& xr = xrun_[v.screen];
    xr.resize(static_cast<size_t>(SCREEN_W) + 1);
    for (u32 x = 0; x <= SCREEN_W; ++x)
      xr[x] = static_cast<u16>((x * static_cast<u32>(v.rect.w) + SCREEN_W - 1) / SCREEN_W);
    if (!v.direct) side_[v.screen].assign(static_cast<size_t>(v.rect.w) * v.rect.h, 0);
  }
}

// The screen rects are overwritten in full every frame, so the rest of the
// surface is cleared only when the layout changed under it -- the surface
// keeps its contents between frames. The dominant modes leave gaps beside
// the smaller screen, so it is simplest to clear everything outside the
// rects row by row; on a panel the screens fill exactly, nothing is written.
// `w`/`h` are the buffer's own size: on the dmabuf tier the shm surface (and
// so scaled_w_/h_) can lag a configure, and the dmabuf is the smaller one.
void Display::clear_margins(u32* px, u32 pitch, int w, int h) const {
  for (int y = 0; y < h; ++y) {
    u32* row = px + static_cast<size_t>(y) * pitch;
    int x = 0;
    // Direct views in x order on this row (at most two).
    int xs[SCREENS], xe[SCREENS], n = 0;
    for (int i = 0; i < nviews_; ++i) {
      const View& v = views_[i];
      if (!v.direct || y < v.rect.y || y >= v.rect.y + v.rect.h) continue;
      xs[n] = std::min(v.rect.x, w); xe[n] = std::min(v.rect.x + v.rect.w, w); ++n;
    }
    if (n == 2 && xs[1] < xs[0]) { std::swap(xs[0], xs[1]); std::swap(xe[0], xe[1]); }
    for (int i = 0; i < n; ++i) {
      if (xs[i] > x) std::memset(row + x, 0, static_cast<size_t>(xs[i] - x) * sizeof(u32));
      x = std::max(x, xe[i]);
    }
    if (x < w) std::memset(row + x, 0, static_cast<size_t>(w - x) * sizeof(u32));
  }
}

// Hands out one target per screen: the window buffer for direct views, the
// side buffer for the rest.
void Display::targets(u32* px, u32 stride, Target out[SCREENS]) {
  frame_px_ = px; frame_pitch_ = stride;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct)
      out[v.screen] = Target{px + static_cast<size_t>(v.rect.y) * stride + v.rect.x, stride, static_cast<u32>(v.rect.h), xrun_[v.screen].data()};
    else
      out[v.screen] = Target{side_[v.screen].data(), static_cast<u32>(v.rect.w), static_cast<u32>(v.rect.h), xrun_[v.screen].data()};
  }
}

// Copies the shown side-buffer views (the inset) into the frame.
void Display::blit_insets() {
  if (!frame_px_) return;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct || !v.shown) continue;
    for (int y = 0; y < v.rect.h; ++y)
      std::memcpy(frame_px_ + static_cast<size_t>(v.rect.y + y) * frame_pitch_ + v.rect.x,
                  side_[v.screen].data() + static_cast<size_t>(y) * v.rect.w, static_cast<size_t>(v.rect.w) * sizeof(u32));
  }
  frame_px_ = nullptr;
}

bool Display::begin_frame(Target out[SCREENS]) {
  if (!scaled_) return false;
  if (dm_) {
    // A configure (fullscreen granted, output reconfigured) resizes the
    // window under us; the buffers must follow before anything writes at the
    // new geometry. The shm path below re-checks its surface the same way.
    int w = 0, h = 0;
    SDL_GetWindowSize(win_, &w, &h);
    if (w != dm_->width() || h != dm_->height()) {
      SDL_Window* win = win_;
      dm_->close();
      if (!dm_->open(win, w, h, only_screen_ >= 0 ? display_index_ : -1)) {
        std::fprintf(stderr, "video: dmabuf resize failed; window surface from here\n");
        dm_.reset();
        margins_dirty_ = true;
        layout();
        build_scale();
        // fall through to the shm path below
      } else {
        layout();
        build_scale();
        dm_margins_ = 0;
      }
    }
  }
  if (dm_) {
    if (u32* px = dm_->begin_frame()) {
      const u32 stride = static_cast<u32>(dm_->width());
      if (margins_dirty_) { clear_margins(px, stride, dm_->width(), dm_->height()); margins_dirty_ = false; }
      // Every buffer needs its margins cleared once, not just the first.
      static_assert(DmabufOut::BUFS <= 8, "margin bookkeeping");
      if (dm_margins_ < DmabufOut::BUFS) { clear_margins(px, stride, dm_->width(), dm_->height()); ++dm_margins_; }
      targets(px, stride, out);
      dm_frame_ = true;
      return true;
    }
    // Protocol death mid-run: drop the tier, keep playing on shm.
    std::fprintf(stderr, "video: dmabuf path lost; window surface from here\n");
    dm_->close();
    dm_.reset();
    margins_dirty_ = true;
  }
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
  if (margins_dirty_) { clear_margins(base, stride, s->w, s->h); margins_dirty_ = false; }
  targets(base, stride, out);
  return true;
}

void Display::end_frame() {
  blit_insets();
  if (dm_frame_) { dm_frame_ = false; dm_->end_frame(); return; }
  if (SDL_MUSTLOCK(surf_)) SDL_UnlockSurface(surf_);
  SDL_UpdateWindowSurface(win_);
}

} // namespace ds::sdl
