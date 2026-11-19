// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/engine2d_regs.h"
#include "core/gpu/line_regs.h"
#include "core/types.h"

#include <array>
#include <vector>

namespace ds { struct NDS; }

namespace ds::gpu {

class VramMap;
struct VramView;

// 18-bit colour: R 0-5, G 8-13, B 16-21; bits 24-31 per-stage attributes.
using Pixel = u32;

// Same bits as BLDCNT targets.
enum LayerId : u8 { L_BG0 = 0x01, L_BG1 = 0x02, L_BG2 = 0x04, L_BG3 = 0x08, L_OBJ = 0x10, L_BACKDROP = 0x20 };

enum PixelKind : u8 { K_NORMAL = 0, K_OBJ_SEMI = 1, K_OBJ_BITMAP = 2, K_3D = 3 };

constexpr u8 OA_PRIO = 0x03, OA_SEMI = 0x04, OA_BITMAP = 0x08, OA_MOSAIC = 0x10, OA_TOUCHED = 0x20, OA_STDPAL = 0x40, OA_OPAQUE = 0x80;

// Layer line value: bit 15 opaque; palette index (bits 0-11), RGB555, or (3D) x position.
// Only the priority winner is resolved through its table.
constexpr u16 LV_OPAQUE = 0x8000;
enum TableId : u8 { T_BG0 = 0, T_BG1, T_BG2, T_BG3, T_OBJ_STD, T_OBJ_EXT, T_OBJ_DIRECT, T_BACKDROP, T_NONE, T_COUNT };   // T_NONE: nothing beneath (colour 0, no layer id)

// The 3D split (core/gpu/split3d.h).
// Off: BG0 is composed in one pass. Gpu: the output is U and split() the
// words, for a compositor that has the 3D image. Cpu: split, then merged
// here with the 3D line (the output is the finished line). Verify: both
// ways, compared; the output is the one-pass line.
enum class Split3D : u8 { Off, Gpu, Cpu, Verify };

// 2D engine (A at 0x04000000, B at 0x04001000): the registers (Engine2DRegs)
// and the software line renderer, which draws each line from the LineRegs
// snapshot taken for it (cur_; sprites, drawn a line ahead, from spr_).
class Engine2D {
public:
  Engine2D(NDS& nds, int num);
  void reset();
  template <class S> void sync_state(S& s);   // journal must be drained

  // Guest side; addr is the full address.
  u32  read(u32 addr, u32 width) { return regs_.read(addr, width); }
  void write(u32 addr, u32 width, u32 value) { regs_.write(addr, width, value); }
  void powcnt_write(u16 value) { regs_.powcnt_write(value); }
  void master_bright_write(u16 value) { regs_.master_bright_write(value); }
  void palette_written(u32 off, u32 width, u32 value) { regs_.palette_written(off, width, value); }
  void oam_written(u32 off, u32 width, u32 value) { regs_.oam_written(off, width, value); }

  // Render side.
  void replay_to(u32 stamp) { regs_.replay_to(stamp); }   // inclusive
  void vram_remapped() { extpal_checked_ = 0; objext_checked_ = 0; }
  // A job in flight reads this map instead of the bus's live one (Gpu's
  // per-job snapshot), so a VRAMCNT remap can rebuild the live map meanwhile.
  void set_vram_override(const VramMap* m) { vm_override_ = m; }
  void apply_pending() { regs_.apply_pending(); }
  void frame_done() { regs_.frame_done(); }                // journal must be drained by now
  bool enabled() const { return regs_.enabled(); }
  u16  master_bright() const { return regs_.master_bright(); }
  int  screen() const { return regs_.screen(); }  // 0 = top, 1 = bottom
  const Engine2DRegs& regs() const { return regs_; }

  // Per-line hooks, in hardware order; journaled so they stay ordered with register writes.
  using Latch = Engine2DRegs::Latch;
  static constexpr Latch L_WINDOWS = Engine2DRegs::L_WINDOWS, L_PREDRAW = Engine2DRegs::L_PREDRAW,
                         L_POSTDRAW = Engine2DRegs::L_POSTDRAW, L_SPRITES = Engine2DRegs::L_SPRITES;
  void latch(Latch k, u32 line, bool reset) { regs_.latch(k, line, reset); }
  void update_windows(u32 line) { regs_.update_windows(line); }            // scanline start
  void pre_draw(u32 line, bool frame_reset) { regs_.pre_draw(line, frame_reset); }   // HBlank, before drawing `line`
  void render_sprites(u32 line);           // runs one line ahead
  void render_line(u32 line);
  void post_draw(bool frame_reset) { regs_.post_draw(frame_reset); }       // HBlank, after drawing

  // Top byte non-zero for every drawn pixel (display capture's alpha).
  const Pixel* output() const { return out_.data(); }

  // Engine A: 256 RGB666 with 5-bit alpha in bits 24-28 (0 = transparent), or null.
  void set_3d_line(const Pixel* line) { line3d_ = line; }
  // Captured images shown again (the 3D split, Gpu::set_split3d_present): a
  // direct-colour bitmap BG or bitmap sprite read straight from one of banks
  // A-D carries which VRAM pixel each of its pixels is. cap_refs: after a
  // line, per pixel the reference of the layer on top (CAP_REF | bank << 16
  // | the 16-bit pixel's index in the bank), 0 where it carried none.
  // Whether that pixel shows unchanged is the caller's to check (output()).
  // False: none on this line.
  static constexpr u32 CAP_REF = 0x80000000u;
  // "Bank" in a reference is a slot: A-D, then H (engine B's small BG bank,
  // where a game may feed a captured image back line by line).
  static constexpr u32 CAP_SLOTS = 5;
  static constexpr int cap_slot_bank(u32 slot) { return slot < 4 ? static_cast<int>(slot) : 7; }
  static constexpr u32 cap_slot_size(u32 slot) { return slot < 4 ? 0x20000u : 0x8000u; }
  void set_cap_track(bool on) { cap_track_ = on; }
  bool cap_refs(u32* refs) const;

  // The 3D split, engine A only. A line is split when BG0 is the 3D layer
  // and enabled; split_on() says so.
  void set_split3d(Split3D m) { split_mode_ = m; }
  const u32* split() const { return split_.data(); }
  bool split_on() const { return split_on_; }   // the line just rendered was split
  u64 split_verified() const { return split_lines_; }   // Verify: lines compared
  u64 split_mismatches() const { return split_bad_; }   // Verify: pixels where split + merge differed

  u32 bldcnt() const { return regs_.bldcnt(); }
  u32 eva() const { return regs_.eva(); }
  u32 evb() const { return regs_.evb(); }
  u32 evy() const { return regs_.evy(); }

  u32 dispcnt() const { return regs_.dispcnt(); }   // render side
  void debug_dump(u32 line);
  void debug_outhash(u32 line);   // DS_DEBUG_OUTHASH
  bool forced_blank() const { return regs_.forced_blank(); }

private:
  NDS& nds_;
  const int num_;
  Engine2DRegs regs_;
  LineRegs cur_{};    // the line render_line() draws
  LineRegs spr_{};    // the state render_sprites() draws with

  // 8 px padding each side lets text BGs write whole tiles at any scroll.
  struct Layer {
    alignas(16) std::array<u16, 8 + 256 + 8> vs;
    bool any = false;
    const Pixel* table = nullptr;
    u16* v() { return vs.data() + 8; }
    const u16* v() const { return vs.data() + 8; }
  };
  std::array<Layer, 4> bg_;
  // OBJ line (+16 slack: kernels write whole vectors). Table chosen by OA_STDPAL/OA_BITMAP.
  alignas(16) std::array<u16, 256 + 16> obj_v_{};
  alignas(16) std::array<u8, 256 + 16> obj_attr_{};  // OA_*
  alignas(16) std::array<u8, 256 + 16> obj_alpha_{}; // bitmap sprites: EVA = alpha+1
  alignas(16) std::array<u8, 256> obj_win_{};
  alignas(16) std::array<Pixel, 256> objpal18_{};
  u32 objpal18_gen_ = 0;
  alignas(16) std::array<Pixel, 4096> objext18_{};
  alignas(16) std::array<u16, 4096> objext_copy_{};
  u16 objext_checked_ = 0, objext_have_ = 0;   // per 256-entry palette
  const Pixel* obj_std_pal18();
  void obj_ext_pal18(u32 pal);
  const Pixel* tables_[T_COUNT] = {};
  static const Pixel zero_table_[256];
  // Extended palettes live in VRAM outside the journal: conversion is reused
  // while the source still matches its copy, rechecked after a VRAMCNT remap.
  alignas(16) std::array<Pixel, 256> pal18_{};
  u32 pal18_gen_ = 0;
  alignas(16) std::array<Pixel, 4 * 16 * 256> extpal18_{};
  alignas(16) std::array<u16, 4 * 16 * 256> extpal_copy_{};
  u64 extpal_checked_ = 0, extpal_have_ = 0;
  u8 obj_prio_mask_ = 0;                             // priorities with an opaque sprite pixel
  const Pixel* std_pal18();
  const Pixel* ext_pal18(u32 slot, u32 pal);
  u32 num_sprites_ = 0;
  u32 oam_lists_gen_ = 0;
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
  Split3D split_mode_ = Split3D::Off;
  bool split_bg0_ = false;       // this line composes without BG0
  bool top_backdrop_ = false;    // the line resolved as backdrop only
  bool split_on_ = false;
  alignas(16) std::array<u32, 256> split_{};
  u64 split_bad_ = 0, split_lines_ = 0;
  void compose_line();
  void build_split();
  bool split_under_blend() const;
  bool cap_track_ = false;
  alignas(16) u32 bg_cap_[4][256]{};     // set_cap_track: per BG, the references its bitmap row carried this line (bg_cap_any_)
  bool bg_cap_any_[4] = {};
  alignas(16) std::array<u32, 256> obj_cap_{};
  bool obj_cap_any_ = false;
  // How render_line resolved the line, for cap_refs: which of the
  // per-pixel records above hold this line's values (TOP_ALL: one layer,
  // top_all_bg_, everywhere).
  enum : u8 { TOP_NONE, TOP_ALL, TOP_FLAT, TOP_FADE, TOP_FULL } top_mode_ = TOP_NONE;
  int top_all_bg_ = -1;

  const VramMap& vram() const;
  const VramMap* vm_override_ = nullptr;
  const VramView& bg_vram() const;
  const VramView& obj_vram() const;
  const u16* palette() const { return regs_.palette(); }
  u16 bg_extpal(u32 slot, u32 pal, u32 idx) const;
  u16 obj_extpal(u32 idx) const;

  void draw_bg_text(u32 line, int bg);
  void draw_bg_affine(u32 line, int bg);
  void draw_bg_extended(u32 line, int bg);
  void draw_bg_large(u32 line);
  // Rotscale fast path for pa = 1.0, pc = 0.
  void bitmap_row_degenerate(int bg, Layer& plane, u32 base, u32 xmask, u32 ymask, u32 yshift, bool wrap, bool direct, s32 rx, s32 ry);
  void tile_row_degenerate(Layer& plane, u32 tilemap, u32 tileset, u32 coordmask, u32 yshift, bool wrap, bool map16, bool ext, int bg, s32 rx, s32 ry);
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
