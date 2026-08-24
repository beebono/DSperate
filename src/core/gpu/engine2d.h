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
constexpr u8 OA_PRIO = 0x03, OA_SEMI = 0x04, OA_BITMAP = 0x08, OA_MOSAIC = 0x10, OA_TOUCHED = 0x20, OA_STDPAL = 0x40, OA_OPAQUE = 0x80;

// Layer lines are u16 per pixel with bit 15 = opaque: a palette index in
// bits 0-11 (extended palettes: palette number << 8 | index), or an RGB555
// colour as VRAM holds it (bit 15 is the hardware's own opaque bit there),
// or for the 3D layer the pixel's position. Each layer resolves through a
// table (the standard palette, an extended-palette slot, the RGB555 table
// or the 3D line), once, on the pixel that wins the priority select.
constexpr u16 LV_OPAQUE = 0x8000;
// Resolve table ids carried per pixel by the select.
enum TableId : u8 { T_BG0 = 0, T_BG1, T_BG2, T_BG3, T_OBJ_STD, T_OBJ_EXT, T_OBJ_DIRECT, T_BACKDROP, T_NONE, T_COUNT };   // T_NONE: nothing beneath (colour 0, no layer id)

// One 2D engine (A at 0x04000000, B at 0x04001000).
//
// Rendering is a deferred-palette line pipeline (docs/ARCHITECTURE.md §5.1):
// every enabled background is rasterised into a u16 line of palette indices
// or RGB555 (bit 15 = opaque), sprites are pre-rendered one line ahead into
// their own u16 line with an attribute byte, then a window plane and a
// priority select on those 16-bit values run as straight passes over the
// line, and the winning value of each pixel is resolved through its layer's
// palette table once. Lines a colour effect can reach also keep the second
// value and go through the composite pass on 18-bit records.
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
  void debug_outhash(u32 line);   // DS_DEBUG_OUTHASH: per-frame / per-line output hashes
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

  // Layer lines (u16, see LV_OPAQUE). BG lines carry 8 pixels of padding on
  // each side so the text renderer can write whole tile rows at the scroll
  // offset; `any` records whether the line has an opaque pixel at all;
  // `table` is what the layer resolves through this line.
  struct Layer {
    alignas(16) std::array<u16, 8 + 256 + 8> vs;
    bool any = false;
    const Pixel* table = nullptr;
    u16* v() { return vs.data() + 8; }
    const u16* v() const { return vs.data() + 8; }
  };
  std::array<Layer, 4> bg_;
  // OBJ line: value (index with OA_STDPAL/OA_BITMAP in the attribute byte choosing the table, or RGB555),
  // attribute byte and window flag. (16 entries of slack: the row kernels write whole vectors.)
  alignas(16) std::array<u16, 256 + 16> obj_v_{};
  alignas(16) std::array<u8, 256 + 16> obj_attr_{};  // bits 0-1 priority, bit 2 semi, bit 3 bitmap, bit 4 mosaic, bit 5 sprite-touched, bit 6 standard palette, bit 7 opaque
  alignas(16) std::array<u8, 256 + 16> obj_alpha_{}; // bitmap sprites: EVA (alpha+1)
  alignas(16) std::array<u8, 256> obj_win_{};
  // OBJ palettes as 18-bit records, validated per line on first use.
  alignas(16) std::array<Pixel, 256> objpal18_{};
  alignas(16) std::array<u16, 256> objpal_copy_{};
  bool objpal_checked_ = false, objpal_have_ = false;
  alignas(16) std::array<Pixel, 4096> objext18_{};
  alignas(16) std::array<u16, 4096> objext_copy_{};
  u16 objext_checked_ = 0, objext_have_ = 0;   // per 256-entry palette
  const Pixel* obj_std_pal18();
  static const Pixel* rgb555_table();
  void obj_ext_pal18(u32 pal);                 // validate / convert one OBJ extended palette
  // Per-line resolve tables by TableId.
  const Pixel* tables_[T_COUNT] = {};
  static const Pixel zero_table_[256];
  // Palettes as 18-bit records: the standard BG palette and the extended
  // palettes by slot and number. A conversion is reused across lines while
  // the source bytes still equal the copy taken at conversion time (checked
  // once per line on first use; palette RAM and VRAM can change mid-frame).
  alignas(16) std::array<Pixel, 256> pal18_{};
  alignas(16) std::array<u16, 256> pal_copy_{};
  bool pal18_checked_ = false, pal18_have_ = false;
  alignas(16) std::array<Pixel, 4 * 16 * 256> extpal18_{};
  alignas(16) std::array<u16, 4 * 16 * 256> extpal_copy_{};
  u64 extpal_checked_ = 0, extpal_have_ = 0;
  u8 obj_prio_mask_ = 0;                             // priorities with an opaque sprite pixel on the line
  const Pixel* std_pal18();
  const Pixel* ext_pal18(u32 slot, u32 pal);
  u32 num_sprites_ = 0;
  // Sprite candidates per line, rebuilt when the OAM bytes change (compared
  // against a copy on every line, as OAM can be written mid-frame).
  alignas(16) std::array<u16, 512> oam_copy_{};
  bool oam_lists_valid_ = false;
  struct LineSprites { u8 count; u8 idx[128]; };
  std::array<LineSprites, 256> line_sprites_{};
  void rebuild_sprite_lists(const u16* oam);
  alignas(16) std::array<u8, 256> win_{};            // bits 0-3 BG, 4 OBJ, 5 effects
  alignas(16) std::array<u16, 256> top16_{}, second16_{};
  alignas(16) std::array<u8, 256> top_tid_{}, second_tid_{};
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
  inline void put_sprite_pixel(s32 x, u16 value, bool opaque, u8 attr, u8 alpha, bool window);
  void apply_sprite_mosaic_x();
  void build_window_plane();
  void select_layers();
  void select_layers_flat();
  static bool line_all_opaque(const Layer& p);
  bool effect_possible() const;
  bool needs_second() const;
  void select_top_only();
  void select_layers_top();
  void setup_tables();
  void resolve_full();
  void colour_effects();
};

} // namespace ds::gpu
