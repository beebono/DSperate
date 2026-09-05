// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"
#include "display_wl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {

namespace {
// The same DS_VERBOSE gate as main.cpp's VLOG.
bool verbose() { static const bool v = std::getenv("DS_VERBOSE") != nullptr; return v; }
} // namespace

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

bool Display::open(const char* title, int scale, bool fullscreen, bool linear, bool vsync, const Layout& layout_mode, int only_screen, int display_index) {
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

  // Display-engine tier: the hardware scales a DS-resolution canvas, so
  // there is no renderer and no scaling here at all; the views are laid out
  // on the canvas (the layout's natural size at scale 1) and draw() draws
  // them into the layer's source.
  if (disp_wanted_ && only_screen_ < 0 && DispOut::available()) {
    int rot = 0;
    if (const char* r = std::getenv("DS_ROTATE")) rot = std::atoi(r);
    auto d = std::make_unique<DispOut>();
    if (d->open(rot, vsync)) {
      disp_ = std::move(d);
      layout();
      std::fprintf(stderr, "video: display-engine scaler, rot %d, layout %s, %s driver, vsync %s\n",
                   rot, mode_name(layout_.mode), SDL_GetCurrentVideoDriver(), vsync ? "on" : "off");
      return true;
    }
  }

  // fbdev tier: the scanline path straight into fb0's buffers, for the SDL2s
  // whose only video driver is Mali EGL over fbdev (the H700 handhelds under
  // BaseOS). Sizes the window to the panel; nothing SDL draws reaches it.
  if (fbdev_wanted_ && only_screen_ < 0) {
    auto fo = std::make_unique<FbdevOut>();
    if (fo->open(win_, vsync)) {
      out_ = std::move(fo);
      scaled_ = true;
      layout();
      build_scale();
      std::fprintf(stderr, "video: fbdev scanout %dx%d, %s driver, scanline scaling, vsync %s\n",
                   out_->width(), out_->height(), SDL_GetCurrentVideoDriver(), vsync ? "on" : "off");
      return true;
    }
    std::fprintf(stderr, "video.fbdev: /dev/fb0 not usable; using SDL\n");
  }

  // Per-scanline scaling renders into the presented buffer directly, which
  // cannot coexist with an SDL_Renderer on the same window, so it is decided
  // here and the renderer is skipped entirely.
  //
  // Default on wherever a zero-copy destination exists for it:
  //
  //  - Wayland: the window surface is a cheap shm attach and the mode removes
  //    SDL's texture upload, scaled blit and surface copy in favour of one
  //    write (etody, both boards: ~15 % less emu+present work per frame, and
  //    the shoulders of the over-budget clusters with it) -- and it is the
  //    write path the dmabuf tier builds on.
  //  - KMSDRM: the DrmOut tier below page-flips our own CMA buffers, which
  //    takes the present from 13.4 ms to ~0.05 ms (etody, 900 frames, both
  //    panels). Even when that tier is unavailable and this falls back to
  //    SDL's window surface, the scanline path still wins there now: 14.8 ms
  //    of emu+present work against the renderer's 17.9. (An older comment
  //    here said the opposite. It was measured before the dual-window default
  //    and before SDL's KMSDRM window surface was understood to be a hidden
  //    GLES renderer rather than a shadow blit -- see display_drm.h.)
  //
  // --linear is bilinear on this path (Gpu::emit_bilinear), and the
  // renderer's own filter on the fallback. DS_SCANLINE_SCALE=0/1 overrides.
  const char* vd = SDL_GetCurrentVideoDriver();
  const bool wayland = vd && !std::strcmp(vd, "wayland");
  const bool kms = vd && !std::strcmp(vd, "KMSDRM");
  const char* sl = std::getenv("DS_SCANLINE_SCALE");
  scaled_ = sl && *sl ? std::strcmp(sl, "0") != 0
                      : (wayland || kms);
  if (scaled_) {
    const char* dmenv = std::getenv("DS_DMABUF");
    const bool dm_forbidden = dmenv && !std::strcmp(dmenv, "0");
    const bool dm_required = dmenv && !std::strcmp(dmenv, "1");

    // KMSDRM first, and before anything asks for a window surface: on that
    // driver SDL_GetWindowSurface *succeeds* by quietly building a GLES
    // renderer for the window, which is the cost this tier exists to avoid.
    if (kms && !dm_forbidden) {
      int ow = 0, oh = 0;
      SDL_GetWindowSize(win_, &ow, &oh);
      auto dr = std::make_unique<DrmOut>();
      if (dr->open(win_, ow, oh, display_index_)) {
        out_ = std::move(dr);
        layout();
        build_scale();
        std::fprintf(stderr, "video: kms scanout, %s driver, scanline scaling\n", vd);
        return true;
      }
      if (dm_required) { std::fprintf(stderr, "DS_DMABUF=1 but the kms scanout path failed\n"); return false; }
    }

    if (!SDL_GetWindowSurface(win_)) {
      std::fprintf(stderr, "window surface unavailable (%s); using the framebuffer path\n", SDL_GetError());
      scaled_ = false;
    } else {
      layout();
      build_scale();
      // Tier 1 on top of the same scanline path: same targets, but the
      // pixels land in a CMA dmabuf instead of the shm surface.
      if (scaled_ && wayland && !dm_forbidden) {
        int ow = 0, oh = 0;
        out_size(ow, oh);
        auto dm = std::make_unique<DmabufOut>();
        if (dm->open(win_, ow, oh, only_screen_ >= 0 ? display_index_ : -1)) out_ = std::move(dm);
        else if (dm_required) { std::fprintf(stderr, "DS_DMABUF=1 but the dmabuf path failed\n"); return false; }
      }
      std::fprintf(stderr, "video: %s, %s driver, scanline scaling\n",
                   out_ ? "dmabuf" : "window surface", SDL_GetCurrentVideoDriver());
      return true;
    }
  }

  // The SDL_Renderer fallback: reached only where none of the tiers above
  // applies -- a video driver with no zero-copy destination (X11), --linear,
  // or DS_SCANLINE_SCALE=0. The GPU renderer is tried first and software is
  // taken when it cannot be created (KMSDRM without GLES, a headless test
  // box). It is not selectable: on the handhelds the scanline tiers are the
  // measured winners (the GL driver's threads cost more than the scale on a
  // four-core board where the emulation thread, the engine-B worker and the
  // raster workers already want every core -- etody, 1800 frames, two RG DS
  // boards: software 14173/14452 ms against opengles2 15426/15508, with a
  // third of the over-budget frames), and those tiers are what the
  // handhelds now get; where this fallback is reached at all the GPU is
  // unlikely to be sharing a die with four A55s.
  const u32 vflag = vsync ? static_cast<u32>(SDL_RENDERER_PRESENTVSYNC) : 0u;
  ren_ = SDL_CreateRenderer(win_, -1, static_cast<u32>(SDL_RENDERER_ACCELERATED) | vflag);
  if (!ren_) {
    std::fprintf(stderr, "accelerated renderer unavailable (%s); falling back to software\n", SDL_GetError());
    ren_ = SDL_CreateRenderer(win_, -1, static_cast<u32>(SDL_RENDERER_SOFTWARE) | vflag);
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
  if (disp_) { disp_->close(); disp_.reset(); }
  if (out_) { out_->close(); out_.reset(); }
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
  if (disp_) { int cw = 0, ch = 0; natural_size(layout_, 1.0, cw, ch); disp_->set_canvas(cw, ch); }
  int w = 0, h = 0;
  if (!out_size(w, h)) return;
  if (only_screen_ >= 0) {
    const double sw = SCREEN_W, sh = SCREEN_H, s = std::min(w / sw, h / sh);
    views_[0] = View{only_screen_, SDL_Rect{static_cast<int>((w - sw * s) / 2), static_cast<int>((h - sh * s) / 2), static_cast<int>(sw * s), static_cast<int>(sh * s)}, true, true};
    return;
  }
  place(layout_, w, h, views_);
  if (disp_) for (int i = 0; i < nviews_; ++i) disp_->set_view(i, views_[i].rect.x, views_[i].rect.y, views_[i].rect.w, views_[i].rect.h, views_[i].shown);
}

void Display::place(const Layout& layout_, int w, int h, View views_[SCREENS]) {
  const double sw = SCREEN_W, sh = SCREEN_H;
  auto fit = [&](double cols, double rows) { return std::min(w / (sw * cols), h / (sh * rows)); };
  auto rect = [&](double x, double y, double s) { return SDL_Rect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(sw * s), static_cast<int>(sh * s)}; };
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
  if (disp_) {
    // In view order: view i is layer i, later views on top.
    const u32* slots[DispOut::VIEWS] = {nullptr, nullptr};
    for (int i = 0; i < nviews_ && i < DispOut::VIEWS; ++i) slots[i] = views_[i].shown ? fb[views_[i].screen] : nullptr;
    disp_->set_inset_alpha(inset_alpha_);
    disp_->present(slots);
    return;
  }
  // The renderer tier only: open() returns before creating a renderer on the
  // scanline tiers, where begin_frame/end_frame is the way to the screen.
  if (!ren_) return;
  SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
  SDL_RenderClear(ren_);
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (!v.shown) continue;
    SDL_UpdateTexture(tex_[v.screen], nullptr, fb[v.screen], static_cast<int>(SCREEN_W) * 4);
    // The GPU blends a translucent inset. Textures are per screen, not per
    // view, so the mod is set around the inset's copy and cleared after it,
    // or a screen swap would carry it to the large view.
    const bool translucent = !v.direct && inset_alpha_ < 255;
    if (translucent) { SDL_SetTextureBlendMode(tex_[v.screen], SDL_BLENDMODE_BLEND); SDL_SetTextureAlphaMod(tex_[v.screen], inset_alpha_); }
    SDL_RenderCopy(ren_, tex_[v.screen], nullptr, &v.rect);
    if (translucent) { SDL_SetTextureAlphaMod(tex_[v.screen], 255); SDL_SetTextureBlendMode(tex_[v.screen], SDL_BLENDMODE_NONE); }
  }
  SDL_RenderPresent(ren_);
}

void Display::toggle_fullscreen() {
  if (disp_) return;   // the panel is the window
  fullscreen_ = !fullscreen_;
  SDL_SetWindowFullscreen(win_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  layout();
  build_scale();
  margins_dirty_ = true;
}

void Display::set_layout(const Layout& l) {
  if (only_screen_ >= 0) return;
  if (disp_) { layout_ = l; layout(); return; }
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
  if (disp_) { w = disp_->logical_w(); h = disp_->logical_h(); return true; }
  if (ren_) return SDL_GetRendererOutputSize(ren_, &w, &h) == 0;
  if (!win_) return false;
  // On a scanout tier the window size is the truth: the shm surface can lag a
  // configure by a frame, and the two must not disagree mid-rebuild. On
  // KMSDRM there is no window surface to ask at all.
  if (out_) { SDL_GetWindowSize(win_, &w, &h); return w > 0 && h > 0; }
  SDL_Surface* s = SDL_GetWindowSurface(win_);
  if (!s) return false;
  w = s->w; h = s->h;
  return true;
}

void Display::build_scale() {
  if (!scaled_ || !win_) return;
  if (out_) {
    // The scanout buffer is the destination; there may be no window surface
    // to fetch (KMSDRM), and asking for one there would build a renderer.
    surf_ = nullptr;
    scaled_w_ = out_->width(); scaled_h_ = out_->height();
  } else {
    surf_ = SDL_GetWindowSurface(win_);      // recreated by SDL on resize
    if (!surf_) { scaled_ = false; return; }
    if (surf_->format->BytesPerPixel != 4) {
      std::fprintf(stderr, "window surface is %d bpp, not 32; using the framebuffer path\n",
                   surf_->format->BytesPerPixel);
      scaled_ = false;
      return;
    }
    scaled_w_ = surf_->w; scaled_h_ = surf_->h;
  }

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
    // Chunky: the even pixel's run is widened over the odd one's, which is
    // left empty (a zero-length run, which scale_row skips). The destination
    // coverage is unchanged, so map_point still agrees.
    cells_[v.screen] = {};
    bool pair = chunky_;
    if (chunky_) {
      // A cell of P panel pixels needs P to divide both dimensions and no
      // more than 256 cells across; auto takes the smallest P >= 4.
      const u32 w = static_cast<u32>(v.rect.w), h = static_cast<u32>(v.rect.h);
      u32 P = 0;
      auto fits = [&](u32 p) { return p >= 2 && w % p == 0 && h % p == 0 && w / p <= SCREEN_W; };
      // An explicit cell that does not divide the screen steps down to the
      // nearest one that does (5 on 640x480 -> 4), so a size chosen for one
      // panel is still close on another.
      if (chunky_cell_ > 0) for (u32 p = static_cast<u32>(chunky_cell_); p >= 2 && !P; --p) if (fits(p)) P = p;
      else if (chunky_cell_ < 0) for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p;
      ds::gpu::Gpu::CellMap m;
      if (P && ds::gpu::Gpu::build_cell_axis(SCREEN_W, w / P, P, m.x) && ds::gpu::Gpu::build_cell_axis(SCREEN_H, h / P, P, m.y)) {
        cells_[v.screen] = std::move(m);
        pair = false;
        // The cells' xrun: cell i covers [i*P, (i+1)*P); entries past the
        // last cell are empty runs.
        for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(std::min(x, w / P) * P);
        // The chosen cell is chatter unless it is not the one asked for;
        // that is said once, with what was used instead.
        static bool told = false;
        if (chunky_cell_ > 0 && P != static_cast<u32>(chunky_cell_) && !told) {
          told = true;
          std::fprintf(stderr, "video: no %d px cell divides %ux%u; using %u\n", chunky_cell_, w, h, P);
        } else if (verbose()) std::fprintf(stderr, "video: chunky cells %ux%u of %u px\n", w / P, h / P, P);
      } else if (chunky_cell_) std::fprintf(stderr, "video: no %s cell divides %ux%u; 2x2 pairs\n", chunky_cell_ > 0 ? "such" : "auto", w, h);
    }
    if (pair)
      for (u32 x = 1; x < SCREEN_W; x += 2) xr[x] = xr[x + 1];
    // Box-filter weights: the boundary between source pixels s and s+1 lies
    // at (s+1) * w / 256; when that is fractional the panel pixel it falls in
    // (the last of run s) covers pixel s+1 by the fractional part. Chunky
    // pairs share a boundary at s+2's, so the odd boundaries are not seams.
    std::vector<u8>& sw = seam_w_[v.screen];
    sw.assign(SCREEN_W, 0);
    for (u32 s = 0; s + 1 < SCREEN_W; ++s) {
      if (pair && !(s & 1)) continue;
      const u32 b = (s + 1) * static_cast<u32>(v.rect.w);
      const u32 frac = b % SCREEN_W;
      if (frac && xr[s + 1] > xr[s]) sw[s] = static_cast<u8>((frac * 256) / SCREEN_W);
    }
    // Bilinear: destination column x samples source u = (x + 0.5) * 256 / w
    // - 0.5, between pixels floor(u) and floor(u)+1. Clamped at both edges;
    // the right edge leans on pixel 254 at weight 255 so that the kernel's
    // pair load never reads past the row.
    std::vector<u16>& lsx = lin_sx_[v.screen];
    std::vector<u8>& lwx = lin_wx_[v.screen];
    lsx.assign(static_cast<size_t>(v.rect.w), 0); lwx.assign(static_cast<size_t>(v.rect.w), 0);
    for (u32 x = 0; x < static_cast<u32>(v.rect.w); ++x) {
      const s32 u = static_cast<s32>(((2 * x + 1) * SCREEN_W * 128) / static_cast<u32>(v.rect.w)) - 128;  // u * 256
      if (u <= 0) continue;
      u32 s = static_cast<u32>(u) >> 8, f = static_cast<u32>(u) & 255;
      if (s >= SCREEN_W - 1) { s = SCREEN_W - 2; f = 255; }
      lsx[x] = static_cast<u16>(s); lwx[x] = static_cast<u8>(f);
    }
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
  last_px_ = px; last_pitch_ = stride;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct)
      out[v.screen] = Target{px + static_cast<size_t>(v.rect.y) * stride + v.rect.x, stride, static_cast<u32>(v.rect.h), xrun_[v.screen].data(), seam_w_[v.screen].data(), lin_sx_[v.screen].data(), lin_wx_[v.screen].data()};
    else
      out[v.screen] = Target{side_[v.screen].data(), static_cast<u32>(v.rect.w), static_cast<u32>(v.rect.h), xrun_[v.screen].data(), seam_w_[v.screen].data(), lin_sx_[v.screen].data(), lin_wx_[v.screen].data()};
  }
}

void Display::blend_row(u32* dst, const u32* src, size_t n, u32 alpha) {
  // Per-channel lerp with the two channel pairs masked apart; the compiler
  // vectorises the plain loop. 0..255 alpha scaled to 0..256 so 255 is exact.
  const u32 a = alpha + (alpha >> 7), b = 256 - a;
  for (size_t i = 0; i < n; ++i) {
    const u32 d = dst[i], s = src[i];
    // Each channel product is under 2^16, so the two packed channels of a
    // pair never carry into each other.
    const u32 rb = (((s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * b) >> 8) & 0x00FF00FFu;
    const u32 g = (((s & 0x0000FF00u) * a + (d & 0x0000FF00u) * b) >> 8) & 0x0000FF00u;
    dst[i] = 0xFF000000u | rb | g;
  }
}

// Copies the shown side-buffer views (the inset) into the frame, or blends
// them over it when the inset is translucent. The blend reads the frame
// back, which on the scanout tiers is uncached memory: the inset is small
// (a ninth of the large screen by default) and the rows are read
// sequentially, so it stays cheap, but an opaque inset takes the copy.
void Display::blit_insets() {
  if (!frame_px_) return;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct || !v.shown) continue;
    for (int y = 0; y < v.rect.h; ++y) {
      u32* dst = frame_px_ + static_cast<size_t>(v.rect.y + y) * frame_pitch_ + v.rect.x;
      const u32* src = side_[v.screen].data() + static_cast<size_t>(y) * v.rect.w;
      if (inset_alpha_ == 255) std::memcpy(dst, src, static_cast<size_t>(v.rect.w) * sizeof(u32));
      else blend_row(dst, src, static_cast<size_t>(v.rect.w), inset_alpha_);
    }
  }
  frame_px_ = nullptr;
}

bool Display::read_screen(int screen, u32* dst) const {
  if (!scaled_ || !last_px_) return false;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.screen != screen || !v.shown || v.rect.w <= 0 || v.rect.h <= 0) continue;
    // Direct views live in the frame at their rect; the rest in a side
    // buffer of the rect's size. Nearest sample: the tier's grid, chunky
    // and seam treatment come along, which a thumbnail can live with.
    const u32* src = v.direct ? last_px_ + static_cast<size_t>(v.rect.y) * last_pitch_ + v.rect.x : side_[screen].data();
    const u32 pitch = v.direct ? last_pitch_ : static_cast<u32>(v.rect.w);
    for (u32 y = 0; y < SCREEN_H; ++y) {
      const u32 sy = static_cast<u32>(static_cast<u64>(y) * static_cast<u32>(v.rect.h) / SCREEN_H);
      const u32* row = src + static_cast<size_t>(sy) * pitch;
      for (u32 x = 0; x < SCREEN_W; ++x)
        dst[y * SCREEN_W + x] = row[static_cast<u64>(x) * static_cast<u32>(v.rect.w) / SCREEN_W];
    }
    return true;
  }
  return false;
}

bool Display::begin_frame(Target out[SCREENS]) {
  if (!scaled_) return false;
  if (out_) {
    // A configure (fullscreen granted, output reconfigured) resizes the
    // window under us; the buffers must follow before anything writes at the
    // new geometry. The shm path below re-checks its surface the same way.
    int w = 0, h = 0;
    SDL_GetWindowSize(win_, &w, &h);
    if (w != out_->width() || h != out_->height()) {
      if (!out_->reopen(win_, w, h)) {
        std::fprintf(stderr, "video: scanout resize failed; window surface from here\n");
        out_.reset();
        margins_dirty_ = true;
        layout();
        build_scale();
        // fall through to the shm path below
      } else {
        layout();
        build_scale();
        out_margins_ = 0;
      }
    }
  }
  if (out_) {
    if (u32* px = out_->begin_frame()) {
      const u32 stride = static_cast<u32>(out_->stride());
      // Every buffer needs its margins cleared once, not just the one in
      // hand: a layout change restarts the count, or the other buffers keep
      // the old layout and flicker it back as they come round.
      if (margins_dirty_) { out_margins_ = 0; margins_dirty_ = false; }
      if (out_margins_ < out_->bufs()) { clear_margins(px, stride, out_->width(), out_->height()); ++out_margins_; }
      targets(px, stride, out);
      out_frame_ = true;
      return true;
    }
    // Protocol/driver death mid-run: drop the tier, keep playing on shm.
    std::fprintf(stderr, "video: scanout path lost; window surface from here\n");
    out_->close();
    out_.reset();
    margins_dirty_ = true;
    build_scale();
    if (!scaled_) return false;
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
  if (out_frame_) { out_frame_ = false; out_->end_frame(); return; }
  if (SDL_MUSTLOCK(surf_)) SDL_UnlockSurface(surf_);
  SDL_UpdateWindowSurface(win_);
}

} // namespace ds::sdl
