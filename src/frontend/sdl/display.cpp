// SPDX-License-Identifier: GPL-3.0-or-later
#include "display.h"
#include "display_wl.h"

#include <algorithm>
#include <cmath>
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

const char* Display::int_scale_name(IntScale m) {
  switch (m) { case IntScale::Under: return "under"; case IntScale::Over: return "over"; default: return "off"; }
}
bool Display::parse_int_scale(const std::string& s, IntScale& m) {
  if (s == "off" || s == "false" || s == "0") { m = IntScale::Off; return true; }
  if (s == "under" || s == "true" || s == "1") { m = IntScale::Under; return true; }
  if (s == "over") { m = IntScale::Over; return true; }
  return false;
}
double Display::snap_scale(double s, IntScale m) {
  if (m == IntScale::Off || s <= 0.0) return s;
  const double f = std::floor(s + 1e-9);          // 2.9999 from a division is 3
  if (f == s || (s - f) < 1e-9) return f;
  return m == IntScale::Under ? std::max(1.0, f) : f + 1.0;
}

void Display::natural_size(const Layout& l, double scale, int& w, int& h) {
  const double sw = SCREEN_W * scale, sh = SCREEN_H * scale;
  double fw = sw, fh = sh;
  switch (l.mode) {
    case Mode::Vertical:   fh = sh * 2; break;
    case Mode::Horizontal: fw = sw * 2; break;
    case Mode::Single: case Mode::Pip: break;
    // Auto has no ratio until there is a window to fit; the smallest it
    // would accept sizes the window.
    case Mode::DominantV:  fh = sh * (1 + (l.dominant_auto ? l.dominant_min : l.dominant)); break;
    case Mode::DominantH:  fw = sw * (1 + (l.dominant_auto ? l.dominant_min : l.dominant)); break;
    case Mode::Count: break;
  }
  w = static_cast<int>(fw); h = static_cast<int>(fh);
}

double Display::dominant_ratio() const {
  if (layout_.mode != Mode::DominantV && layout_.mode != Mode::DominantH) return 1.0;
  if (!layout_.dominant_auto) return layout_.dominant;
  const int p = layout_.primary, q = 1 - p;
  return views_[p].rect.w > 0 ? static_cast<double>(views_[q].rect.w) / views_[p].rect.w : layout_.dominant_min;
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
    d->set_grid(disp_grid_);
    d->set_nearest(!linear);
    d->set_integer_scale(static_cast<int>(int_scale_));
    if (d->open(rot, vsync)) {
      disp_ = std::move(d);
      layout();
      if (chunky_) build_source_scale();
      std::fprintf(stderr, "video: display-engine scaler (%s), rot %d, layout %s, %s driver, vsync %s%s%s\n",
                   disp_->nearest() ? "nearest" : "driver filter", rot, mode_name(layout_.mode), SDL_GetCurrentVideoDriver(), vsync ? "on" : "off",
                   !chunky_ ? "" : disp_->divisor() > 1 ? ", chunky in the scaler" : ", chunky at source",
                   disp_->grid() ? ", grid layer" : "");
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
  frame_px_ = nullptr;
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
    const double sw = SCREEN_W, sh = SCREEN_H, s = snap_scale(std::min(w / sw, h / sh), int_scale_);
    const int dw = static_cast<int>(sw * s), dh = static_cast<int>(sh * s);
    // Overscale on a dual-window screen crops away from the edge it shares
    // with the other panel: the top screen keeps its bottom row and loses
    // rows at the top, the bottom screen the reverse. Columns are centred.
    int y = (h - dh) / 2;
    if (dh > h) y = only_screen_ == 0 ? h - dh : 0;
    views_[0] = View{only_screen_, SDL_Rect{(w - dw) / 2, y, dw, dh}, true, true};
    return;
  }
  place(layout_, w, h, views_, int_scale_);
  if (verbose()) for (int i = 0; i < nviews_; ++i)
    std::fprintf(stderr, "video: view %d screen %d at %d,%d %dx%d%s (%dx%d, integer %s%s)\n", i, views_[i].screen, views_[i].rect.x, views_[i].rect.y, views_[i].rect.w, views_[i].rect.h,
                 views_[i].shown ? "" : " hidden", w, h, int_scale_name(int_scale_), layout_.dominant_auto && (layout_.mode == Mode::DominantV || layout_.mode == Mode::DominantH) ? ", dominant auto" : "");
  if (disp_) for (int i = 0; i < nviews_; ++i) disp_->set_view(i, views_[i].rect.x, views_[i].rect.y, views_[i].rect.w, views_[i].rect.h, views_[i].shown);
}

// Auto dominant: the primary's scale `s` is the largest whole number that
// leaves the secondary at least `dominant_min` of it; the secondary `s2`
// is then the largest that fits the room left beside it (the whole
// width/height, not a ratio of the primary -- the point is a crisp primary,
// the secondary takes what remains), capped at the primary's size. Under
// an integer-scale setting the secondary is floored too, when that leaves
// it at least one panel pixel per DS pixel. When no whole scale leaves
// room enough the fractional fit at `dominant_min` is what is left.
void Display::dominant_auto(const Layout& l, int w, int h, bool across, IntScale snap, double& s, double& s2) {
  const double sw = SCREEN_W, sh = SCREEN_H;
  const double along = across ? w / sw : h / sh;      // room along the pair, in screens
  const double side  = across ? h / sh : w / sw;      // room across it
  for (int k = static_cast<int>(std::floor(std::min(along, side) + 1e-9)); k >= 1; --k) {
    double rest = std::min(along - k, side);          // the secondary's fit in the leftover
    rest = std::min(rest, static_cast<double>(k));
    if (snap != IntScale::Off && rest >= 1.0) rest = std::floor(rest + 1e-9);
    if (rest + 1e-9 >= k * l.dominant_min) { s = k; s2 = rest; return; }
  }
  const double r = l.dominant_min;
  s = snap_scale(std::min(across ? w / (sw * (1 + r)) : w / sw, across ? h / sh : h / (sh * (1 + r))), snap);
  s2 = s * r;
}

void Display::place(const Layout& layout_, int w, int h, View views_[SCREENS], IntScale snap) {
  const double sw = SCREEN_W, sh = SCREEN_H;
  // The fit is snapped whole here, so every rect below -- and the scale
  // tables, touch map and margins built from them -- follows. The pair and
  // dominant layouts stay centred as a whole, so an overscale crop takes
  // equally from the outer edges and the edge between the screens is kept.
  auto fit = [&](double cols, double rows) { return snap_scale(std::min(w / (sw * cols), h / (sh * rows)), snap); };
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
    case Mode::DominantV: case Mode::DominantH: {
      // DS order (top above / left of bottom), the pair centred; across the
      // stack each screen is centred, along the row bottoms are aligned.
      const bool across = layout_.mode == Mode::DominantH;
      double s = 0, s2 = 0;    // the primary's and the secondary's scale
      if (!layout_.dominant_auto) { s = across ? fit(1 + layout_.dominant, 1) : fit(1, 1 + layout_.dominant); s2 = s * layout_.dominant; }
      else dominant_auto(layout_, w, h, across, snap, s, s2);
      const double sc[2] = {p == 0 ? s : s2, p == 1 ? s : s2};
      if (!across) {
        double y = (h - sh * (s + s2)) / 2;
        for (int i = 0; i < SCREENS; ++i) { views_[i] = View{i, rect((w - sw * sc[i]) / 2, y, sc[i]), true, true}; y += sh * sc[i]; }
      } else {
        double x = (w - sw * (s + s2)) / 2;
        const double bottom = (h - sh * s) / 2 + sh * s;
        for (int i = 0; i < SCREENS; ++i) { views_[i] = View{i, rect(x, bottom - sh * sc[i], sc[i]), true, true}; x += sw * sc[i]; }
      }
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

void Display::set_page(bool on) {
  page_ = on;
  if (disp_) disp_->set_divisor(on ? 1 : disp_divisor_);
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
  if (disp_) { layout_ = l; layout(); if (chunky_) build_source_scale(); return; }
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

// Chunky on the display-engine tier. The panel cell is P panel pixels
// that is a whole number D of DS pixels under the DE's fit: P dividing the
// view's panel rect into a count that divides 256 x 192 -- on a 320x240
// view 5 px is 4 DS pixels, 4 px would be 3.2 and cannot be. Auto takes the
// smallest such P in 4..16, as the panel tiers do; an explicit P steps down.
//
// Two ways to draw it. In the scaler (DispOut::set_divisor): when every
// shown view and the canvas divide by D, the composite is the canvas at
// cell resolution -- each view box-downscaled by D, the cells' mean -- and
// the DE's nearest table enlarges the cells whole. No work at DS
// resolution and a rotate of a quarter the pixels; mean is the only mode.
// Otherwise (the PiP inset does not divide; no whole cell exists; the pair
// setting) at source: the scanline scaler runs at 1:1 into src_side_,
// flattening cells (any mode) or pairs, which end_frame hands to the layer
// in place of the core's framebuffer.
void Display::build_source_scale() {
  const double s = disp_ ? disp_->fit_scale() : 0.0;
  // First the cell each view would get, and whether the scaler can draw them.
  u32 D_hw = 0;
  bool hw = disp_ != nullptr && chunky_cell_ != 0 && s > 0.0;
  int cw = 0, chh = 0;
  natural_size(layout_, 1.0, cw, chh);
  for (int i = 0; hw && i < nviews_; ++i) {
    const View& v = views_[i];
    if (!v.shown) continue;
    const u32 pw = static_cast<u32>(std::lround(v.rect.w * s)), ph = static_cast<u32>(std::lround(v.rect.h * s));
    u32 D = 0;
    if (pw >= SCREEN_W && ph >= SCREEN_H) {
      auto fits = [&](u32 p) {
        if (p < 2 || pw % p || ph % p) return false;
        const u32 cx = pw / p, cy = ph / p;
        return SCREEN_W % cx == 0 && SCREEN_H % cy == 0 && SCREEN_W / cx == SCREEN_H / cy && SCREEN_W / cx <= ds::gpu::Gpu::CELL_TAPS;
      };
      u32 P = 0;
      if (chunky_cell_ > 0) { for (u32 p = static_cast<u32>(chunky_cell_); p >= 2 && !P; --p) if (fits(p)) P = p; }
      else { for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p; }
      D = P ? SCREEN_W / (pw / P) : 0;
      if (D < 2) hw = false;                       // no whole cell: pairs at source
      else if (D_hw && D != D_hw) hw = false;      // views disagree
      else D_hw = D;
    }
    // Every view (a downscaled one too) and the canvas must divide.
    if (D_hw && (v.rect.w % D_hw || v.rect.h % D_hw || v.rect.x % D_hw || v.rect.y % D_hw)) hw = false;
  }
  if (hw && (!D_hw || cw % D_hw || chh % D_hw)) hw = false;
  disp_divisor_ = hw ? static_cast<int>(D_hw) : 1;
  if (disp_) disp_->set_divisor(page_ ? 1 : disp_divisor_);
  if (hw) {
    scaled_ = false;   // the core's framebuffers go to the layer as they are
    for (int i = 0; i < nviews_; ++i) { disp_->set_view_cell(i, 1); src_chunky_[views_[i].screen] = false; }
    if (verbose()) std::fprintf(stderr, "video: chunky cells of %u DS pixels drawn by the scaler\n", D_hw);
    return;
  }
  scaled_ = true;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    std::vector<u16>& xr = xrun_[v.screen];
    xr.resize(static_cast<size_t>(SCREEN_W) + 1);
    for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(x);
    xrun_plain_[v.screen] = xr;
    seam_w_[v.screen].assign(SCREEN_W, 0);
    cells_[v.screen] = {};
    src_side_[v.screen].assign(static_cast<size_t>(SCREEN_W) * SCREEN_H, 0xFF000000u);
    int cell = 2;
    const u32 pw = static_cast<u32>(std::lround(v.rect.w * s)), ph = static_cast<u32>(std::lround(v.rect.h * s));
    // Shown smaller than the screen: flattening blocks before a downscale
    // is only blur, so the view stays plain (the grid skips it too).
    src_chunky_[v.screen] = s > 0.0 && pw >= SCREEN_W && ph >= SCREEN_H;
    if (!src_chunky_[v.screen]) { if (disp_) disp_->set_view_cell(i, 1); continue; }
    if (chunky_cell_ != 0) {
      u32 P = 0;
      auto fits = [&](u32 p) {
        if (p < 2 || pw % p || ph % p) return false;
        const u32 cx = pw / p, cy = ph / p;
        return SCREEN_W % cx == 0 && SCREEN_H % cy == 0 && SCREEN_W / cx == SCREEN_H / cy && SCREEN_W / cx <= ds::gpu::Gpu::CELL_TAPS;
      };
      if (chunky_cell_ > 0) { for (u32 p = static_cast<u32>(chunky_cell_); p >= 2 && !P; --p) if (fits(p)) P = p; }
      else { for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p; }
      ds::gpu::Gpu::CellMap m;
      const u32 D = P ? SCREEN_W / (pw / P) : 0;
      if (P && D > 1 && ds::gpu::Gpu::build_cell_axis(SCREEN_W, SCREEN_W / D, D, m.x) && ds::gpu::Gpu::build_cell_axis(SCREEN_H, SCREEN_H / D, D, m.y)) {
        cells_[v.screen] = std::move(m);
        cell = static_cast<int>(D);
        for (u32 x = 0; x <= SCREEN_W; ++x) xr[x] = static_cast<u16>(std::min(x, SCREEN_W / D) * D);
        if (verbose() || (chunky_cell_ > 0 && P != static_cast<u32>(chunky_cell_)))
          std::fprintf(stderr, "video: chunky cells of %u px on %ux%u = %u DS pixels at source\n", P, pw, ph, D);
      } else {
        std::fprintf(stderr, "video: no %s cell is whole DS pixels on %ux%u; 2x2 pairs at source\n", chunky_cell_ > 0 ? "such" : "auto", pw, ph);
      }
    }
    if (cell == 2) for (u32 x = 1; x < SCREEN_W; x += 2) xr[x] = xr[x + 1];
    if (disp_) disp_->set_view_cell(i, cell);
  }
}

void Display::build_scale() {
  if (!scaled_ || !win_ || disp_) return;   // the display-engine tier's tables are fixed at open()
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
  // The largest shown view sets the chunky cell (in DS pixels) the others follow.
  int ref_view = 0;
  for (int i = 1; i < nviews_; ++i) if (views_[i].shown && (!views_[ref_view].shown || views_[i].rect.w > views_[ref_view].rect.w)) ref_view = i;
  const int order[SCREENS] = {ref_view, 1 - ref_view};   // the reference first, so ref_D is set
  double ref_D = 2.0;
  for (int oi = 0; oi < nviews_; ++oi) {
    const int i = order[oi];
    const View& v = views_[i];
    std::vector<u16>& xr = xrun_[v.screen];
    xr.resize(static_cast<size_t>(SCREEN_W) + 1);
    // A direct view scaled past the buffer (integer overscale) is cropped to
    // it: runs outside [x0, x1) collapse to empty ones, which scale_row
    // skips, and the rest are relative to x0, where the target's px points.
    // Rows are cropped the same way by targets(). An inset (a side buffer)
    // is never cropped here; blit_insets clips it at the edge.
    const u32 x0 = v.direct ? static_cast<u32>(std::max(0, -v.rect.x)) : 0u;
    const u32 x1 = v.direct ? static_cast<u32>(std::clamp(scaled_w_ - v.rect.x, 0, v.rect.w)) : static_cast<u32>(v.rect.w);
    const bool cropped = v.direct && (x0 > 0 || x1 < static_cast<u32>(v.rect.w) || v.rect.y < 0 || v.rect.y + v.rect.h > scaled_h_);
    for (u32 x = 0; x <= SCREEN_W; ++x) {
      const u32 raw = (x * static_cast<u32>(v.rect.w) + SCREEN_W - 1) / SCREEN_W;
      xr[x] = static_cast<u16>(std::clamp(raw, x0, x1) - x0);
    }
    xrun_plain_[v.screen] = xr;
    // Chunky: the even pixel's run is widened over the odd one's, which is
    // left empty (a zero-length run, which scale_row skips). The destination
    // coverage is unchanged, so map_point still agrees.
    cells_[v.screen] = {};
    src_chunky_[v.screen] = chunky_;
    bool pair = chunky_;
    if (chunky_) {
      // A cell of P panel pixels needs P to divide both dimensions and no
      // more than 256 cells across; auto takes the smallest P >= 4.
      const u32 w = static_cast<u32>(v.rect.w), h = static_cast<u32>(v.rect.h);
      u32 P = 0;
      // The cell map is laid over the whole view; a cropped one takes the
      // pairs (at a whole scale the pairs are exact cells anyway).
      auto fits = [&](u32 p) { return !cropped && p >= 2 && w % p == 0 && h % p == 0 && w / p <= SCREEN_W; };
      // The largest view chooses the cell. An explicit cell that does not
      // divide the screen steps down to the nearest one that does (5 on
      // 640x480 -> 4), so a size chosen for one panel is still close on
      // another -- but no further than auto's floor of 4: a view like the
      // dominant layouts' 426x320 divides by nothing but 2, and a 2-px cell
      // there is 1.2 DS pixels, no chunky at all. Below the floor the 2x2
      // pairs take over, as with auto.
      // The other views (the PiP inset, the dominant layouts' secondary)
      // match its cell in DS pixels, not panel pixels: the same 4 px that
      // is 2 DS pixels on a 512-wide view is 8 on a 128-wide one, and the
      // smaller screen would come out the coarsest. So they take the P
      // whose DS pixels per cell is nearest the large view's; and a view
      // shown smaller than the screen goes plain, as it does on the
      // display-engine tier -- flattening cells before a downscale is only
      // blur.
      if (i == ref_view) {
        if (chunky_cell_ > 0) { for (u32 p = static_cast<u32>(chunky_cell_); p >= 4 && !P; --p) if (fits(p)) P = p; }
        else if (chunky_cell_ < 0) { for (u32 p = 4; p <= 16 && !P; ++p) if (fits(p)) P = p; }
        ref_D = P ? static_cast<double>(SCREEN_W) * P / w : 2.0;
      } else if (w < SCREEN_W || h < SCREEN_H) {
        src_chunky_[v.screen] = false; pair = false;
        if (verbose()) std::fprintf(stderr, "video: view %ux%u is below the screen; no chunky\n", w, h);
      } else if (chunky_cell_ != 0) {
        double best = 1e9;
        for (u32 p = 2; p <= 64; ++p) if (fits(p)) {
          const double d = std::fabs(static_cast<double>(SCREEN_W) * p / w - ref_D);
          if (d < best - 1e-9) { best = d; P = p; }
        }
      }
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
        if (i == ref_view && chunky_cell_ > 0 && P != static_cast<u32>(chunky_cell_) && !told) {
          told = true;
          std::fprintf(stderr, "video: no %d px cell divides %ux%u; using %u\n", chunky_cell_, w, h, P);
        } else if (verbose()) std::fprintf(stderr, "video: chunky cells %ux%u of %u px (%.2f DS pixels)\n", w / P, h / P, P, static_cast<double>(SCREEN_W) * P / w);
      } else if (!src_chunky_[v.screen]) {}
      else if (chunky_cell_ && cropped) { if (verbose()) std::fprintf(stderr, "video: view cropped by the overscale; 2x2 pairs\n"); }
      else if (chunky_cell_) std::fprintf(stderr, "video: no %s cell divides %ux%u; 2x2 pairs\n", chunky_cell_ > 0 ? "such" : "auto", w, h);
    }
    if (pair)
      for (u32 x = 1; x < SCREEN_W; x += 2) xr[x] = xr[x + 1];
    // Box-filter weights: the boundary between source pixels s and s+1 lies
    // at b = (s+1) * w / 256; when that is fractional the panel pixel it
    // falls in (the last of run s, index floor(b)) covers pixel s from its
    // left edge up to the boundary -- frac(b) of it -- and pixel s+1 for the
    // rest, 1 - frac(b). The weight stored is s+1's. (An earlier version
    // stored frac(b) itself, which leaned every seam pixel the wrong way:
    // invisible at 2.5x where every fraction is a half, plain at 3.75x, and
    // on a view near 1x it left each seam pixel showing the pixel before it,
    // so the small screens looked scaled like the large one.) Chunky pairs
    // share a boundary at s+2's, so the odd boundaries are not seams.
    std::vector<u8>& sw = seam_w_[v.screen];
    sw.assign(SCREEN_W, 0);
    for (u32 s = 0; s + 1 < SCREEN_W; ++s) {
      if (pair && !(s & 1)) continue;
      const u32 b = (s + 1) * static_cast<u32>(v.rect.w);
      const u32 frac = b % SCREEN_W;
      if (frac && xr[s + 1] > xr[s]) sw[s] = static_cast<u8>(256 - (frac * 256) / SCREEN_W);
    }
    // Bilinear: destination column x samples source u = (x + 0.5) * 256 / w
    // - 0.5, between pixels floor(u) and floor(u)+1. Clamped at both edges;
    // the right edge leans on pixel 254 at weight 255 so that the kernel's
    // pair load never reads past the row.
    // Indexed by the kept destination column, so a cropped view's tables
    // start at x0.
    std::vector<u16>& lsx = lin_sx_[v.screen];
    std::vector<u8>& lwx = lin_wx_[v.screen];
    lsx.assign(static_cast<size_t>(x1 - x0), 0); lwx.assign(static_cast<size_t>(x1 - x0), 0);
    for (u32 x = x0; x < x1; ++x) {
      const s32 u = static_cast<s32>(((2 * x + 1) * SCREEN_W * 128) / static_cast<u32>(v.rect.w)) - 128;  // u * 256
      if (u <= 0) continue;
      u32 s = static_cast<u32>(u) >> 8, f = static_cast<u32>(u) & 255;
      if (s >= SCREEN_W - 1) { s = SCREEN_W - 2; f = 255; }
      lsx[x - x0] = static_cast<u16>(s); lwx[x - x0] = static_cast<u8>(f);
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
void Display::targets(u32* px, u32 stride, int w, int h, Target out[SCREENS]) {
  frame_px_ = px; frame_pitch_ = stride; frame_w_ = w; frame_h_ = h;
  insets_done_ = false;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct) {
      // The part of the rect inside the buffer: px at its first kept row
      // and column (the runs are relative to that column, see build_scale).
      const int x0 = std::max(0, -v.rect.x), y0 = std::max(0, -v.rect.y);
      const int y1 = std::clamp(h - v.rect.y, 0, v.rect.h);
      out[v.screen] = Target{px + static_cast<size_t>(v.rect.y + y0) * stride + v.rect.x + x0, stride, static_cast<u32>(v.rect.h), xrun_[v.screen].data(), seam_w_[v.screen].data(), lin_sx_[v.screen].data(), lin_wx_[v.screen].data(), grid_on(v.screen), xrun_plain_[v.screen].data(),
                             static_cast<u32>(y0), static_cast<u32>(std::max(y0, y1))};
    } else
      out[v.screen] = Target{side_[v.screen].data(), static_cast<u32>(v.rect.w), static_cast<u32>(v.rect.h), xrun_[v.screen].data(), seam_w_[v.screen].data(), lin_sx_[v.screen].data(), lin_wx_[v.screen].data(), grid_on(v.screen), xrun_plain_[v.screen].data()};
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
  if (!frame_px_ || insets_done_) return;
  insets_done_ = true;
  for (int i = 0; i < nviews_; ++i) {
    const View& v = views_[i];
    if (v.direct || !v.shown) continue;
    // Clipped to the buffer: with the large screen overscaled the inset's
    // corner can sit past the panel.
    const int x0 = std::max(0, -v.rect.x), x1 = std::min(v.rect.w, frame_w_ - v.rect.x);
    if (x1 <= x0) continue;
    for (int y = std::max(0, -v.rect.y); y < std::min(v.rect.h, frame_h_ - v.rect.y); ++y) {
      u32* dst = frame_px_ + static_cast<size_t>(v.rect.y + y) * frame_pitch_ + v.rect.x + x0;
      const u32* src = side_[v.screen].data() + static_cast<size_t>(y) * v.rect.w + x0;
      if (inset_alpha_ == 255) std::memcpy(dst, src, static_cast<size_t>(x1 - x0) * sizeof(u32));
      else blend_row(dst, src, static_cast<size_t>(x1 - x0), inset_alpha_);
    }
  }
}

bool Display::begin_frame(Target out[SCREENS]) {
  if (!scaled_) return false;
  if (disp_) {
    for (int i = 0; i < SCREENS; ++i)
      out[i] = Target{src_side_[i].data(), SCREEN_W, SCREEN_H, xrun_[i].data(), seam_w_[i].data(), nullptr, nullptr, false, xrun_plain_[i].data()};
    return true;
  }
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
        out_clean_ = 0;
      }
    }
  }
  if (out_) {
    if (u32* px = out_->begin_frame()) {
      const u32 stride = static_cast<u32>(out_->stride());
      // Every buffer needs its margins cleared once, not just the one in
      // hand, or the others keep the old layout and flash it back as they
      // come round. Tracked per buffer index, not as a count of frames:
      // the tiers hand out the lowest free buffer, so the same one can
      // come back three frames running while another holds the old layout
      // until a hiccup brings it round -- the stale frame seen after a
      // layout switch.
      if (margins_dirty_) { out_clean_ = 0; margins_dirty_ = false; }
      const int idx = out_->current();
      const u32 bit = idx >= 0 && idx < 32 ? 1u << idx : 0u;
      if (!(out_clean_ & bit)) { clear_margins(px, stride, out_->width(), out_->height()); out_clean_ |= bit; }
      targets(px, stride, out_->width(), out_->height(), out);
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
  targets(base, stride, s->w, s->h, out);
  return true;
}

// The insets go down before anything the frontend draws over the frame, so
// that an overlay cannot be buried by the PiP inset the way it was when the
// overlays lived in DS space and were drawn before this.
void Display::finish_views() {
  if (disp_) return;
  blit_insets();
}

void Display::present() {
  if (disp_) {
    const u32* fb[SCREENS];
    for (int i = 0; i < SCREENS; ++i) fb[i] = src_side_[i].data();
    draw(fb);
    return;
  }
  frame_px_ = nullptr;
  if (out_frame_) { out_frame_ = false; out_->end_frame(); return; }
  if (SDL_MUSTLOCK(surf_)) SDL_UnlockSurface(surf_);
  SDL_UpdateWindowSurface(win_);
}

void Display::end_frame() {
  finish_views();
  present();
}

} // namespace ds::sdl
