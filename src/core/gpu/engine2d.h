// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::gpu {

class VramMap;
struct VramView;

// Pixel record used throughout the 2D pipeline: 18-bit colour (R bits 0-5,
// G bits 8-13, B bits 16-21 — the hardware blends at 6 bits per channel) with
// bits 24-31 free for per-stage attributes.
using Pixel = u32;

// Layer identifiers, laid out like BLDCNT's first-target bits so a record's
// id can be masked against the register directly.
enum LayerId : u8 { L_BG0 = 0x01, L_BG1 = 0x02, L_BG2 = 0x04, L_BG3 = 0x08, L_OBJ = 0x10, L_BACKDROP = 0x20 };

// Kind of the winning pixel, which decides how colour effects apply.
enum PixelKind : u8 { K_NORMAL = 0, K_OBJ_SEMI = 1, K_OBJ_BITMAP = 2, K_3D = 3 };

// OBJ plane attribute byte and colour-word flags (shared with the kernels).
constexpr u8 OA_PRIO = 0x03, OA_SEMI = 0x04, OA_BITMAP = 0x08, OA_MOSAIC = 0x10, OA_TOUCHED = 0x20, OA_OPAQUE = 0x80;
constexpr u32 OP_DIRECT = 1u << 15, OP_STDPAL = 1u << 12;

// One 2D engine (A at 0x04000000, B at 0x04001000).
//
// Rendering is a mask-plane pipeline (docs/ARCHITECTURE.md §5): every enabled
// background is rasterised into its own 256-pixel plane with an opacity mask,
// sprites are pre-rendered one line ahead into their own plane, then a window
// plane, a priority select and a colour-effects pass run as straight passes
// over the line. Nothing in the later stages branches on per-pixel layer
// identity in a way that can't be expressed as a masked select.
class Engine2D {
public:
  Engine2D(NDS& nds, int num);
  void reset();

  // Register access (addr is the full 0x040000xx / 0x040010xx address).
  u32  read(u32 addr, u32 width);
  void write(u32 addr, u32 width, u32 value);

  // POWCNT1 gating (bit 1 for A, bit 9 for B).
  void set_enabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }

  // Per-line hooks, in the order the hardware applies them.
  void update_windows(u32 vcount);         // at scanline start
  void pre_draw(bool frame_reset);         // at HBlank, before drawing
  void render_sprites(u32 line);           // sprites for `line` (pre-rendered one line ahead)
  void render_line(u32 line);              // BG + composite for `line` into output()
  void post_draw(bool frame_reset);        // at HBlank, after drawing

  // Composite output of the last render_line(): Pixel records, top byte
  // non-zero for every drawn pixel (display capture uses it as alpha).
  const Pixel* output() const { return out_.data(); }

  // 3D layer input for engine A (256 RGB666 records with 5-bit alpha in
  // bits 24-28, alpha 0 = transparent); nullptr when there is no 3D output.
  void set_3d_line(const Pixel* line) { line3d_ = line; }

  u32 dispcnt() const { return dispcnt_; }
  void debug_dump(u32 line);   // stderr dump of register/latch state and a rendered line (debug builds of the CLI)
  bool forced_blank() const { return forced_blank_; }

private:
  NDS& nds_;
  const int num_;
  bool enabled_ = false;

  // Registers.
  u32 dispcnt_ = 0;
  std::array<u16, 4> bgcnt_{};
  std::array<u16, 4> bghofs_{}, bgvofs_{};
  std::array<s16, 2> pa_{}, pb_{}, pc_{}, pd_{};
  std::array<s32, 2> ref_x_{}, ref_y_{};             // as written (28-bit signed)
  std::array<s32, 2> ref_x_int_{}, ref_y_int_{};     // running value for the current line
  std::array<s32, 2> ref_x_reload_{}, ref_y_reload_{};
  std::array<u8, 4> win0_{}, win1_{};                // x1, x2, y1, y2
  std::array<u8, 4> wincnt_{};                       // WININ lo/hi, WINOUT lo/hi
  u8 bg_mosaic_w_ = 0, bg_mosaic_h_ = 0, obj_mosaic_w_ = 0, obj_mosaic_h_ = 0;
  u16 bldcnt_ = 0, bldalpha_ = 0;
  u8 eva_ = 16, evb_ = 0, evy_ = 0;

  // Latches (enables take effect with a delay; disables immediately).
  std::array<u32, 3> dispcnt_hist_{};
  u8 layer_enable_ = 0, obj_enable_ = 0;
  bool forced_blank_ = false;
  u8 win0_active_ = 0, win1_active_ = 0;            // bit0 y-range, bit1 x-range
  u8 bg_mosaic_y_ = 0, bg_mosaic_ymax_ = 0, obj_mosaic_y_ = 0;
  bool bg_mosaic_latch_ = true, obj_mosaic_latch_ = true;
  u32 bg_mosaic_line_ = 0, obj_mosaic_line_ = 0;

  // Planes.
  struct BgPlane { alignas(16) std::array<Pixel, 256> px; alignas(16) std::array<u8, 256> op; };
  std::array<BgPlane, 4> bg_;
  // OBJ plane: colour/index word plus attribute byte and window flag.
  alignas(16) std::array<u32, 256> obj_px_{};        // bit 15 set: direct colour; else palette index (+ bit 12: standard palette)
  alignas(16) std::array<u8, 256> obj_attr_{};       // bits 0-1 priority, bit 2 semi, bit 3 bitmap, bit 4 mosaic, bit 5 sprite-touched, bit 7 opaque
  alignas(16) std::array<u8, 256> obj_alpha_{};      // bitmap sprites: EVA (alpha+1)
  alignas(16) std::array<u8, 256> obj_win_{};
  alignas(16) std::array<Pixel, 256> obj_col_{};     // OBJ plane resolved through the palettes, per line
  alignas(16) std::array<Pixel, 256> pal18_{};       // standard BG palette as 18-bit records, per line
  u32 num_sprites_ = 0;
  alignas(16) std::array<u8, 256> win_{};            // bits 0-3 BG, 4 OBJ, 5 effects
  alignas(16) std::array<Pixel, 256> top_{}, second_{};
  alignas(16) std::array<u8, 256> top_id_{}, top_kind_{}, top_alpha_{}, second_id_{};
  alignas(16) std::array<Pixel, 256> out_{};
  const Pixel* line3d_ = nullptr;

  // Helpers.
  const VramMap& vram() const;
  const VramView& bg_vram() const;
  const VramView& obj_vram() const;
  const u16* palette() const;                      // 512 entries (BG) ; +256 = OBJ
  u16 bg_extpal(u32 slot, u32 pal, u32 idx) const;
  u16 obj_extpal(u32 idx) const;

  void draw_bg_text(u32 line, int bg);
  void draw_bg_affine(u32 line, int bg);
  void draw_bg_extended(u32 line, int bg);
  void draw_bg_large(u32 line);
  void draw_bg_3d();
  void draw_sprite_normal(const u16* attr, int w, int h, s32 x, s32 y, bool window);
  void draw_sprite_rotscale(const u16* attr, const u16* oam, int bw, int bh, int w, int h, s32 x, s32 y, bool window);
  void put_sprite_pixel(s32 x, u32 colour, bool opaque, u8 attr, u8 alpha, bool window);
  void apply_sprite_mosaic_x();
  void build_window_plane();
  void select_layers();
  void select_bg(int bg);
  void resolve_obj_colours();
  void select_obj(u32 prio);
  void colour_effects();
};

} // namespace ds::gpu
