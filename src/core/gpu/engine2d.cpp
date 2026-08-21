// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 2D engine: registers and the portable line renderer. Hardware behaviour per
// GBATEK, with melonDS (GPLv3) used as the reference for the latching and
// blending corner cases that are verified against real hardware there.
#include "core/gpu/engine2d.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::gpu {

namespace {

// BGR555 palette entry -> 18-bit record. Bit 15 of a palette entry is the low
// green bit on paletted graphics (not on direct colour, VRAM or FIFO display).
inline Pixel rgb15_to_18(u16 c) {
  const u32 r = (c & 0x001F) << 1;
  const u32 g = ((c & 0x03E0) >> 4) | ((c & 0x8000) >> 15);
  const u32 b = (c & 0x7C00) >> 9;
  return r | (g << 8) | (b << 16);
}


} // namespace

Engine2D::Engine2D(NDS& nds, int num) : nds_(nds), num_(num) { reset(); }

void Engine2D::reset() {
  enabled_ = false;
  dispcnt_ = 0; dispcnt_hist_.fill(0);
  bgcnt_.fill(0); bghofs_.fill(0); bgvofs_.fill(0);
  pa_.fill(0); pb_.fill(0); pc_.fill(0); pd_.fill(0);
  ref_x_.fill(0); ref_y_.fill(0); ref_x_int_.fill(0); ref_y_int_.fill(0); ref_x_reload_.fill(0); ref_y_reload_.fill(0);
  win0_.fill(0); win1_.fill(0); wincnt_.fill(0);
  bg_mosaic_w_ = bg_mosaic_h_ = obj_mosaic_w_ = obj_mosaic_h_ = 0;
  bldcnt_ = 0; bldalpha_ = 0; eva_ = 16; evb_ = 0; evy_ = 0;
  layer_enable_ = obj_enable_ = 0; forced_blank_ = false;
  win0_active_ = win1_active_ = 0;
  bg_mosaic_y_ = bg_mosaic_ymax_ = obj_mosaic_y_ = 0;
  bg_mosaic_latch_ = obj_mosaic_latch_ = true;
  bg_mosaic_line_ = obj_mosaic_line_ = 0;
  for (auto& p : bg_) { p.px.fill(0); p.op.fill(0); }
  obj_px_.fill(0); obj_attr_.fill(0); obj_alpha_.fill(0); obj_win_.fill(0); num_sprites_ = 0;
  out_.fill(0);
  line3d_ = nullptr;
}

// ---- registers --------------------------------------------------------------

u32 Engine2D::read(u32 addr, u32 width) {
  const u32 r = addr & 0xFFF;
  if (width == 32) return read(addr, 16) | (read(addr + 2, 16) << 16);
  if (width == 8) { const u32 v = read(addr & ~1u, 16); return (addr & 1) ? (v >> 8) & 0xFF : v & 0xFF; }
  switch (r) {
  case 0x00: return dispcnt_ & 0xFFFF;
  case 0x02: return dispcnt_ >> 16;
  case 0x08: case 0x0A: case 0x0C: case 0x0E: return bgcnt_[(r - 8) / 2];
  case 0x48: return wincnt_[0] | (wincnt_[1] << 8);
  case 0x4A: return wincnt_[2] | (wincnt_[3] << 8);
  case 0x50: return bldcnt_;
  case 0x52: return bldalpha_;
  default: return 0;      // write-only registers read as zero
  }
}

void Engine2D::write(u32 addr, u32 width, u32 value) {
  const u32 r = addr & 0xFFF;
  if (width == 8) {
    if (r < 4) {                                   // DISPCNT bytes
      const u32 shift = r * 8;
      dispcnt_ = (dispcnt_ & ~(0xFFu << shift)) | ((value & 0xFF) << shift);
      if (num_) dispcnt_ &= 0xC0B1FFF7;
      return;
    }
    // BG0HOFS on engine A also scrolls the 3D layer, even with the engine powered down.
    if (!num_ && r == 0x10) nds_.gpu3d.set_render_xpos(static_cast<u16>(value & 0xFF), 0x00FF);
    if (!num_ && r == 0x11) nds_.gpu3d.set_render_xpos(static_cast<u16>(value << 8), 0xFF00);
    if (!enabled_) return;
    switch (r) {
    case 0x40: win0_[1] = value; return; case 0x41: win0_[0] = value; return;
    case 0x42: win1_[1] = value; return; case 0x43: win1_[0] = value; return;
    case 0x44: win0_[3] = value; return; case 0x45: win0_[2] = value; return;
    case 0x46: win1_[3] = value; return; case 0x47: win1_[2] = value; return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: wincnt_[r - 0x48] = value; return;
    case 0x4C: bg_mosaic_w_ = value & 0xF; bg_mosaic_h_ = value >> 4; return;
    case 0x4D: obj_mosaic_w_ = value & 0xF; obj_mosaic_h_ = value >> 4; return;
    case 0x52: bldalpha_ = (bldalpha_ & 0x1F00) | (value & 0x1F); eva_ = value & 0x1F; if (eva_ > 16) eva_ = 16; return;
    case 0x53: bldalpha_ = (bldalpha_ & 0x001F) | ((value & 0x1F) << 8); evb_ = value & 0x1F; if (evb_ > 16) evb_ = 16; return;
    case 0x54: evy_ = value & 0x1F; if (evy_ > 16) evy_ = 16; return;
    default: break;
    }
    // Other byte writes merge into the halfword (affine references included,
    // which go through the 16-bit path's sign extension).
    const u32 cur = read(addr & ~1u, 16);
    const u16 merged = (addr & 1) ? static_cast<u16>((cur & 0x00FF) | (value << 8)) : static_cast<u16>((cur & 0xFF00) | (value & 0xFF));
    write(addr & ~1u, 16, merged);
    return;
  }
  if (width == 32) {
    switch (r) {
    case 0x00: dispcnt_ = value; if (num_) dispcnt_ &= 0xC0B1FFF7; return;
    case 0x28: case 0x2C: case 0x38: case 0x3C:
      if (!enabled_) return;
      { const int i = r >= 0x38; s32 v = static_cast<s32>(value << 4) >> 4;
        if ((r & 0xF) == 0x8) { ref_x_[i] = v; ref_x_reload_[i] = v; } else { ref_y_[i] = v; ref_y_reload_[i] = v; } }
      return;
    default: write(addr, 16, value & 0xFFFF); write(addr + 2, 16, value >> 16); return;
    }
  }
  // 16-bit.
  switch (r) {
  case 0x00: dispcnt_ = (dispcnt_ & 0xFFFF0000) | value; if (num_) dispcnt_ &= 0xC0B1FFF7; return;
  case 0x02: dispcnt_ = (dispcnt_ & 0x0000FFFF) | (value << 16); if (num_) dispcnt_ &= 0xC0B1FFF7; return;
  default: break;
  }
  if (!num_ && r == 0x10) nds_.gpu3d.set_render_xpos(static_cast<u16>(value), 0xFFFF);
  // Everything below is ignored while the engine is powered down (POWCNT1),
  // which is the behaviour the reference implementation models.
  if (!enabled_) return;
  switch (r) {
  case 0x08: case 0x0A: case 0x0C: case 0x0E: bgcnt_[(r - 8) / 2] = value; return;
  case 0x10: case 0x14: case 0x18: case 0x1C: bghofs_[(r - 0x10) / 4] = value & 0x1FF; return;
  case 0x12: case 0x16: case 0x1A: case 0x1E: bgvofs_[(r - 0x12) / 4] = value & 0x1FF; return;
  case 0x20: pa_[0] = static_cast<s16>(value); return;
  case 0x22: pb_[0] = static_cast<s16>(value); return;
  case 0x24: pc_[0] = static_cast<s16>(value); return;
  case 0x26: pd_[0] = static_cast<s16>(value); return;
  case 0x30: pa_[1] = static_cast<s16>(value); return;
  case 0x32: pb_[1] = static_cast<s16>(value); return;
  case 0x34: pc_[1] = static_cast<s16>(value); return;
  case 0x36: pd_[1] = static_cast<s16>(value); return;
  case 0x28: case 0x2A: case 0x2C: case 0x2E: case 0x38: case 0x3A: case 0x3C: case 0x3E: {
    const int i = r >= 0x38;
    s32& ref = ((r & 0xF) < 0xC) ? ref_x_[i] : ref_y_[i];
    s32& reload = ((r & 0xF) < 0xC) ? ref_x_reload_[i] : ref_y_reload_[i];
    if (r & 2) { u32 hi = value & 0x0FFF; if (hi & 0x0800) hi |= 0xF000; ref = static_cast<s32>((static_cast<u32>(ref) & 0xFFFF) | (hi << 16)); }
    else ref = static_cast<s32>((static_cast<u32>(ref) & 0xFFFF0000) | value);
    reload = ref;
    return;
  }
  case 0x40: win0_[1] = value & 0xFF; win0_[0] = value >> 8; return;
  case 0x42: win1_[1] = value & 0xFF; win1_[0] = value >> 8; return;
  case 0x44: win0_[3] = value & 0xFF; win0_[2] = value >> 8; return;
  case 0x46: win1_[3] = value & 0xFF; win1_[2] = value >> 8; return;
  case 0x48: wincnt_[0] = value & 0xFF; wincnt_[1] = value >> 8; return;
  case 0x4A: wincnt_[2] = value & 0xFF; wincnt_[3] = value >> 8; return;
  case 0x4C: bg_mosaic_w_ = value & 0xF; bg_mosaic_h_ = (value >> 4) & 0xF; obj_mosaic_w_ = (value >> 8) & 0xF; obj_mosaic_h_ = value >> 12; return;
  case 0x50: bldcnt_ = value & 0x3FFF; return;
  case 0x52: bldalpha_ = value & 0x1F1F; eva_ = value & 0x1F; if (eva_ > 16) eva_ = 16; evb_ = (value >> 8) & 0x1F; if (evb_ > 16) evb_ = 16; return;
  case 0x54: evy_ = value & 0x1F; if (evy_ > 16) evy_ = 16; return;
  default: return;
  }
}

// ---- per-line state ---------------------------------------------------------

void Engine2D::update_windows(u32 vcount) {
  if (!enabled_) return;
  // Vertical window edges are evaluated every line, windows enabled or not.
  const u8 y = vcount & 0xFF;
  if (y == win0_[3]) win0_active_ &= ~1; else if (y == win0_[2]) win0_active_ |= 1;
  if (y == win1_[3]) win1_active_ &= ~1; else if (y == win1_[2]) win1_active_ |= 1;
}

void Engine2D::pre_draw(bool frame_reset) {
  if (!enabled_) return;
  // Turning a layer on takes two lines (sprites: one) to show; turning it off
  // and forcing blank apply at once.
  dispcnt_hist_[2] = dispcnt_hist_[1]; dispcnt_hist_[1] = dispcnt_hist_[0]; dispcnt_hist_[0] = dispcnt_;
  layer_enable_ = ((dispcnt_hist_[2] & dispcnt_) >> 8) & 0x1F;
  obj_enable_ = ((dispcnt_hist_[1] & dispcnt_) >> 12) & 1;
  forced_blank_ = ((dispcnt_hist_[2] | dispcnt_) >> 7) & 1;

  if (bg_mosaic_latch_) bg_mosaic_line_ = nds_.io.vcount;
  for (int i = 0; i < 2; ++i) {
    if (!(bgcnt_[2 + i] & (1 << 6)) || bg_mosaic_latch_) { ref_x_int_[i] = ref_x_[i]; ref_y_int_[i] = ref_y_[i]; }
  }
  if (dispcnt_ & (1 << 12)) {
    if (frame_reset || obj_mosaic_y_ == obj_mosaic_h_) { obj_mosaic_y_ = 0; obj_mosaic_latch_ = true; }
    else { obj_mosaic_y_ = (obj_mosaic_y_ + 1) & 0xF; obj_mosaic_latch_ = false; }
  }
  if (obj_mosaic_latch_) obj_mosaic_line_ = frame_reset ? 0 : (nds_.io.vcount + 1);
}

void Engine2D::post_draw(bool frame_reset) {
  if (!enabled_) return;
  // BG mosaic height is latched into an internal counter; OBJ mosaic compares
  // against the live register.
  if (frame_reset) { bg_mosaic_ymax_ = bg_mosaic_h_; bg_mosaic_y_ = 0; bg_mosaic_latch_ = true; }
  else if (bg_mosaic_y_ == bg_mosaic_ymax_) { bg_mosaic_ymax_ = bg_mosaic_h_; bg_mosaic_y_ = 0; bg_mosaic_latch_ = true; }
  else { bg_mosaic_y_ = (bg_mosaic_y_ + 1) & 0xF; bg_mosaic_latch_ = false; }
  for (int i = 0; i < 2; ++i) {
    if (!(layer_enable_ & (4 << i))) continue;        // reference points only advance for enabled layers
    if (frame_reset) { ref_x_[i] = ref_x_reload_[i]; ref_y_[i] = ref_y_reload_[i]; }
    else { ref_x_[i] += pb_[i]; ref_y_[i] += pd_[i]; }
  }
}

// ---- memory helpers ---------------------------------------------------------

const VramMap& Engine2D::vram() const { return nds_.bus.vram_map(); }
const VramView& Engine2D::bg_vram() const { return num_ ? vram().bbg : vram().abg; }
const VramView& Engine2D::obj_vram() const { return num_ ? vram().bobj : vram().aobj; }
const u16* Engine2D::palette() const { return reinterpret_cast<const u16*>(nds_.bus.palette.get() + (num_ ? 0x400 : 0)); }
u16 Engine2D::bg_extpal(u32 slot, u32 pal, u32 idx) const {
  return vram().read16(num_ ? vram().bbg_extpal : vram().abg_extpal, slot * 0x2000 + pal * 0x200 + idx * 2);
}
u16 Engine2D::obj_extpal(u32 idx) const {
  return vram().read16(num_ ? vram().bobj_extpal : vram().aobj_extpal, idx * 2);
}

void Engine2D::debug_dump(u32 line) {
  std::fprintf(stderr, "[eng%d] dispcnt %08x enabled %d layer_en %02x obj_en %d fb %d bgcnt %04x %04x %04x %04x hofs %u %u %u %u vofs %u %u %u %u wincnt %02x %02x %02x %02x bldcnt %04x eva %u evb %u evy %u mos %u %u\n",
               num_, dispcnt_, enabled_, layer_enable_, obj_enable_, forced_blank_, bgcnt_[0], bgcnt_[1], bgcnt_[2], bgcnt_[3],
               bghofs_[0], bghofs_[1], bghofs_[2], bghofs_[3], bgvofs_[0], bgvofs_[1], bgvofs_[2], bgvofs_[3], wincnt_[0], wincnt_[1], wincnt_[2], wincnt_[3], bldcnt_, eva_, evb_, evy_, bg_mosaic_w_, bg_mosaic_h_);
  const VramView& vv = bg_vram();
  std::fprintf(stderr, "[eng%d] bgvram blocks:", num_);
  for (u32 b = 0; b < vv.blocks(); ++b) std::fprintf(stderr, " %x", vv.mask[b]);
  std::fprintf(stderr, "\n[eng%d] map[0..8] ", num_);
  for (u32 i = 0; i < 8; ++i) std::fprintf(stderr, "%04x ", vram().read16(vv, ((bgcnt_[0] & 0x1F00) << 3) + i * 2));
  std::fprintf(stderr, " extpal(slot0..3, pal0, idx1) %04x %04x %04x %04x", bg_extpal(0, 0, 1), bg_extpal(1, 0, 1), bg_extpal(2, 0, 1), bg_extpal(3, 0, 1));
  std::fprintf(stderr, " pal[0..8] ");
  for (u32 i = 0; i < 8; ++i) std::fprintf(stderr, "%04x ", palette()[i]);
  render_sprites(line);
  u32 objop = 0; for (u32 i = 0; i < 256; ++i) objop += (obj_attr_[i] & 0x80) != 0;
  std::fprintf(stderr, "\n[eng%d] sprites on line %u: %u, opaque obj pixels %u, objvram blocks:", num_, line, num_sprites_, objop);
  for (u32 b = 0; b < obj_vram().blocks(); ++b) std::fprintf(stderr, " %x", obj_vram().mask[b]);
  const u16* oam = reinterpret_cast<const u16*>(nds_.bus.oam.get() + (num_ ? 0x400 : 0));
  std::fprintf(stderr, "\n[eng%d] oam[0..3]:", num_);
  for (int n = 0; n < 4; ++n) std::fprintf(stderr, " %04x/%04x/%04x", oam[n * 4], oam[n * 4 + 1], oam[n * 4 + 2]);
  render_line(line);
  u32 op = 0; for (int bg = 0; bg < 4; ++bg) for (u32 i = 0; i < 256; ++i) op += bg_[bg].op[i];
  u32 nonbd = 0; for (u32 i = 0; i < 256; ++i) nonbd += top_id_[i] != L_BACKDROP;
  std::fprintf(stderr, "\n[eng%d] line %u: opaque bg pixels %u, non-backdrop top %u, out[128] %08x top %08x id %02x kind %u win %02x second %08x\n", num_, line, op, nonbd, out_[128], top_[128], top_id_[128], top_kind_[128], win_[128], second_[128]);
}

// ---- line rendering ---------------------------------------------------------

void Engine2D::render_line(u32 line) {
  if (!enabled_) {
    // Powered-down engines output a fixed colour: black for A, white for B.
    out_.fill(num_ ? 0xFF3F3F3F : 0xFF000000);
    return;
  }
  if (forced_blank_) { out_.fill(0xFF3F3F3F); return; }

  for (auto& p : bg_) p.op.fill(0);
  prof::Scope* sc = prof::enabled ? new prof::Scope(prof::BG_DRAW) : nullptr;
  const int mode = dispcnt_ & 7;
  auto bg_on = [&](int n) { return (layer_enable_ >> n) & 1; };

  // 1. Rasterise each enabled background into its plane.
  switch (mode) {
  case 0: for (int n = 0; n < 4; ++n) if (bg_on(n)) draw_bg_text(line, n); break;
  case 1: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_text(line, 2); if (bg_on(3)) draw_bg_affine(line, 3); break;
  case 2: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_affine(line, 2); if (bg_on(3)) draw_bg_affine(line, 3); break;
  case 3: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_text(line, 2); if (bg_on(3)) draw_bg_extended(line, 3); break;
  case 4: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_affine(line, 2); if (bg_on(3)) draw_bg_extended(line, 3); break;
  case 5: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); if (bg_on(2)) draw_bg_extended(line, 2); if (bg_on(3)) draw_bg_extended(line, 3); break;
  case 6: if (bg_on(2)) draw_bg_large(line); break;
  case 7: if (bg_on(0)) draw_bg_text(line, 0); if (bg_on(1)) draw_bg_text(line, 1); break;
  }
  // BG0 is the 3D layer on engine A when DISPCNT bit 3 is set (modes 0-5, 7; mode 6 has no text BG0).
  if (!num_ && (dispcnt_ & 8) && bg_on(0)) draw_bg_3d();

  // 2. Window plane, 3. sprite X mosaic, 4. priority select, 5. colour effects.
  delete sc;
  { DS_PROF(WINDOW); build_window_plane(); apply_sprite_mosaic_x(); }
  { DS_PROF(SELECT); select_layers(); }
  { DS_PROF(EFFECTS); colour_effects(); }
}

// Text (tiled) background.
void Engine2D::draw_bg_text(u32 line, int bg) {
  const u16 cnt = bgcnt_[bg];
  BgPlane& plane = bg_[bg];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  const u16* pal = palette();

  u32 xoff = bghofs_[bg];
  u32 yoff = bgvofs_[bg] + ((cnt & (1 << 6)) ? bg_mosaic_line_ : line);
  const bool wide = cnt & (1 << 14), tall = cnt & (1 << 15);
  u32 tileset = (cnt & 0x003C) << 12, tilemap = (cnt & 0x1F00) << 3;
  if (!num_) { tileset += (dispcnt_ & 0x07000000) >> 8; tilemap += (dispcnt_ & 0x38000000) >> 11; }
  if (tall) { tilemap += (yoff & 0x1F8) << 3; if (wide) tilemap += (yoff & 0x100) << 3; }
  else tilemap += (yoff & 0xF8) << 3;
  const u32 widexmask = wide ? 0x100 : 0;
  const bool c256 = cnt & (1 << 7);
  const bool extpal = dispcnt_ & (1u << 30);
  const u32 extslot = (bg < 2 && (cnt & 0x2000)) ? (2 + bg) : bg;
  const bool mosaic = (cnt & (1 << 6)) && bg_mosaic_w_ > 0;
  const u32 mw = bg_mosaic_w_ + 1;

  u32 cur_tile = 0, cur_x = ~0u; u8 row[8] = {};   // decoded 8 pixels (palette indices) of the current tile row
  const u16* cur_palette = pal; u32 cur_pal_hi = 0;
  auto load_tile = [&](u32 x) {
    const u32 map_addr = tilemap + ((x & 0xF8) >> 2) + ((x & widexmask) << 3);
    cur_tile = vm.read16(vv, map_addr);
    const u32 ty = (cur_tile & (1 << 11)) ? (7 - (yoff & 7)) : (yoff & 7);
    if (c256) {
      const u32 a = tileset + ((cur_tile & 0x3FF) << 6) + (ty << 3);
      if (const u8* p = vv.direct(a, 8)) std::memcpy(row, p, 8); else for (int i = 0; i < 8; ++i) row[i] = vm.read8(vv, a + i);
      cur_pal_hi = cur_tile >> 12;
    } else {
      const u32 a = tileset + ((cur_tile & 0x3FF) << 5) + (ty << 2);
      u8 packed[4];
      if (const u8* p = vv.direct(a, 4)) std::memcpy(packed, p, 4); else for (int i = 0; i < 4; ++i) packed[i] = vm.read8(vv, a + i);
      for (int i = 0; i < 4; ++i) { row[i * 2] = packed[i] & 0xF; row[i * 2 + 1] = packed[i] >> 4; }
      cur_palette = pal + ((cur_tile & 0xF000) >> 8);
    }
    if (cur_tile & (1 << 10)) { for (int i = 0; i < 4; ++i) { const u8 t = row[i]; row[i] = row[7 - i]; row[7 - i] = t; } }
  };

  if (!mosaic && !c256) {
    // 16-colour tiles: whole tile rows through the palette kernel into a
    // padded line, then the visible window is copied out.
    kern::active::palette_to_18(pal, pal18_.data(), 256);
    alignas(16) Pixel tpx[264]; alignas(16) u8 top[264];
    const u32 shift = xoff & 7;
    for (u32 t = 0, x = xoff & ~7u; t < 33; ++t, x += 8) {
      load_tile(x);
      kern::active::tile_row_pal16(row, pal18_.data() + ((cur_tile & 0xF000) >> 8), tpx + t * 8, top + t * 8);
    }
    std::memcpy(plane.px.data(), tpx + shift, 256 * sizeof(Pixel));
    std::memcpy(plane.op.data(), top + shift, 256);
    return;
  }
  for (u32 i = 0; i < 256; ++i) {
    const u32 x = mosaic ? (xoff + i - (i % mw)) : (xoff + i);
    if ((x >> 3) != cur_x) { cur_x = x >> 3; load_tile(x); }
    const u8 idx = row[x & 7];
    if (!idx) continue;
    u16 c;
    if (c256) c = extpal ? bg_extpal(extslot, cur_pal_hi, idx) : pal[idx];
    else c = cur_palette[idx];
    plane.px[i] = rgb15_to_18(c); plane.op[i] = 1;
  }
}

// Affine (rotation/scaling) background: 8-bit map entries, 256-colour tiles.
void Engine2D::draw_bg_affine(u32 line, int bg) {
  (void)line;
  const u16 cnt = bgcnt_[bg];
  BgPlane& plane = bg_[bg];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  const u16* pal = palette();
  static const u32 coord_masks[4] = {0x07800, 0x0F800, 0x1F800, 0x3F800};
  const u32 coordmask = coord_masks[(cnt >> 14) & 3], yshift = 7 + ((cnt >> 14) & 3) - 3;
  const u32 overflow = (cnt & (1 << 13)) ? 0 : ~(coordmask | 0x7FF);
  u32 tileset = (cnt & 0x003C) << 12, tilemap = (cnt & 0x1F00) << 3;
  if (!num_) { tileset += (dispcnt_ & 0x07000000) >> 8; tilemap += (dispcnt_ & 0x38000000) >> 11; }
  const s32 dx = pa_[bg - 2], dy = pc_[bg - 2];
  s32 rx = ref_x_int_[bg - 2], ry = ref_y_int_[bg - 2];
  const bool mosaic = (cnt & (1 << 6)) && bg_mosaic_w_ > 0;
  const u32 mw = bg_mosaic_w_ + 1;

  for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy) {
    s32 fx = rx, fy = ry;
    if (mosaic) { const s32 m = i % mw; fx -= m * dx; fy -= m * dy; }
    if ((fx | fy) & overflow) continue;
    const u32 tile = vm.read8(vv, tilemap + (((fy & coordmask) >> 11) << yshift) + ((fx & coordmask) >> 11));
    const u8 idx = vm.read8(vv, tileset + (tile << 6) + (((fy >> 8) & 7) << 3) + ((fx >> 8) & 7));
    if (!idx) continue;
    plane.px[i] = rgb15_to_18(pal[idx]); plane.op[i] = 1;
  }
}

// Extended background: 16-bit-map affine, 8-bit bitmap or direct-colour bitmap.
void Engine2D::draw_bg_extended(u32 line, int bg) {
  (void)line;
  const u16 cnt = bgcnt_[bg];
  BgPlane& plane = bg_[bg];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  const u16* pal = palette();
  const s32 dx = pa_[bg - 2], dy = pc_[bg - 2];
  s32 rx = ref_x_int_[bg - 2], ry = ref_y_int_[bg - 2];
  const bool mosaic = (cnt & (1 << 6)) && bg_mosaic_w_ > 0;
  const u32 mw = bg_mosaic_w_ + 1;
  const bool extpal = dispcnt_ & (1u << 30);

  if (cnt & (1 << 7)) {
    static const u32 xm[4] = {0x07FFF, 0x0FFFF, 0x1FFFF, 0x1FFFF}, ym[4] = {0x07FFF, 0x0FFFF, 0x0FFFF, 0x1FFFF}, ys[4] = {7, 8, 9, 9};
    const u32 sz = (cnt >> 14) & 3, xmask = xm[sz], ymask = ym[sz], yshift = ys[sz];
    const u32 ofx = (cnt & (1 << 13)) ? 0 : ~xmask, ofy = (cnt & (1 << 13)) ? 0 : ~ymask;
    const u32 base = (cnt & 0x1F00) << 6;
    const bool direct = cnt & (1 << 2);
    for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy) {
      s32 fx = rx, fy = ry;
      if (mosaic) { const s32 m = i % mw; fx -= m * dx; fy -= m * dy; }
      if ((fx & ofx) || (fy & ofy)) continue;
      const u32 off = (((fy & ymask) >> 8) << yshift) + ((fx & xmask) >> 8);
      if (direct) {
        const u16 c = vm.read16(vv, base + (off << 1));
        if (!(c & 0x8000)) continue;
        plane.px[i] = rgb15_to_18(c & 0x7FFF);
      } else {
        const u8 idx = vm.read8(vv, base + off);
        if (!idx) continue;
        plane.px[i] = rgb15_to_18(pal[idx]);
      }
      plane.op[i] = 1;
    }
  } else {
    static const u32 coord_masks[4] = {0x07800, 0x0F800, 0x1F800, 0x3F800};
    const u32 coordmask = coord_masks[(cnt >> 14) & 3], yshift = 7 + ((cnt >> 14) & 3) - 3;
    const u32 overflow = (cnt & (1 << 13)) ? 0 : ~(coordmask | 0x7FF);
    u32 tileset = (cnt & 0x003C) << 12, tilemap = (cnt & 0x1F00) << 3;
    if (!num_) { tileset += (dispcnt_ & 0x07000000) >> 8; tilemap += (dispcnt_ & 0x38000000) >> 11; }
    for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy) {
      s32 fx = rx, fy = ry;
      if (mosaic) { const s32 m = i % mw; fx -= m * dx; fy -= m * dy; }
      if ((fx | fy) & overflow) continue;
      const u16 tile = vm.read16(vv, tilemap + ((((fy & coordmask) >> 11) << yshift) + ((fx & coordmask) >> 11)) * 2);
      u32 tx = (fx >> 8) & 7, ty = (fy >> 8) & 7;
      if (tile & (1 << 10)) tx = 7 - tx;
      if (tile & (1 << 11)) ty = 7 - ty;
      const u8 idx = vm.read8(vv, tileset + ((tile & 0x3FF) << 6) + (ty << 3) + tx);
      if (!idx) continue;
      plane.px[i] = rgb15_to_18(extpal ? bg_extpal(bg, tile >> 12, idx) : pal[idx]); plane.op[i] = 1;
    }
  }
}

// Mode 6: one large 8-bit bitmap on BG2.
void Engine2D::draw_bg_large(u32 line) {
  (void)line;
  const u16 cnt = bgcnt_[2];
  BgPlane& plane = bg_[2];
  const VramView& vv = bg_vram();
  const VramMap& vm = vram();
  const u16* pal = palette();
  static const u32 xm[4] = {0x1FFFF, 0x3FFFF, 0x1FFFF, 0x1FFFF}, ym[4] = {0x3FFFF, 0x1FFFF, 0x0FFFF, 0x1FFFF}, ys[4] = {9, 10, 9, 9};
  const u32 sz = (cnt >> 14) & 3, xmask = xm[sz], ymask = ym[sz], yshift = ys[sz];
  const u32 ofx = (cnt & (1 << 13)) ? 0 : ~xmask, ofy = (cnt & (1 << 13)) ? 0 : ~ymask;
  const s32 dx = pa_[0], dy = pc_[0];
  s32 rx = ref_x_int_[0], ry = ref_y_int_[0];
  const bool mosaic = (cnt & (1 << 6)) && bg_mosaic_w_ > 0;
  const u32 mw = bg_mosaic_w_ + 1;
  for (u32 i = 0; i < 256; ++i, rx += dx, ry += dy) {
    s32 fx = rx, fy = ry;
    if (mosaic) { const s32 m = i % mw; fx -= m * dx; fy -= m * dy; }
    if ((fx & ofx) || (fy & ofy)) continue;
    const u8 idx = vm.read8(vv, (((fy & ymask) >> 8) << yshift) + ((fx & xmask) >> 8));
    if (!idx) continue;
    plane.px[i] = rgb15_to_18(pal[idx]); plane.op[i] = 1;
  }
}

void Engine2D::draw_bg_3d() {
  BgPlane& plane = bg_[0];
  if (!line3d_) { plane.op.fill(0); return; }
  kern::active::layer_3d(line3d_, plane.px.data(), plane.op.data());
}

// ---- sprites ----------------------------------------------------------------

void Engine2D::put_sprite_pixel(s32 x, u32 colour, bool opaque, u8 attr, u8 alpha, bool window) {
  if (window) { if (opaque) obj_win_[x] = 1; return; }
  const u8 old = obj_attr_[x];
  if (opaque && (!(old & OA_OPAQUE) || (attr & OA_PRIO) < (old & OA_PRIO))) {
    obj_px_[x] = colour; obj_attr_[x] = attr | OA_OPAQUE; obj_alpha_[x] = alpha;
  } else if (!opaque && !(old & OA_OPAQUE)) {
    // A transparent pixel still stamps its priority and mosaic flag.
    obj_attr_[x] = (old & ~(OA_MOSAIC | OA_PRIO)) | (attr & (OA_TOUCHED | OA_MOSAIC | OA_PRIO));
  }
}

void Engine2D::render_sprites(u32 line) {
  if (!enabled_) return;      // the OBJ planes are left as they are
  num_sprites_ = 0;
  obj_px_.fill(0); obj_attr_.fill(0); obj_alpha_.fill(0); obj_win_.fill(0);
  if (!obj_enable_) return;

  const u16* oam = reinterpret_cast<const u16*>(nds_.bus.oam.get() + (num_ ? 0x400 : 0));
  static const s32 widths[16]  = {8, 16, 8, 8, 16, 32, 8, 8, 32, 32, 16, 8, 64, 64, 32, 8};
  static const s32 heights[16] = {8, 8, 16, 8, 16, 8, 32, 8, 32, 16, 32, 8, 64, 32, 64, 8};

  for (int n = 0; n < 128; ++n) {
    const u16* attr = &oam[n * 4];
    const u32 type = (attr[0] >> 8) & 3;      // bit 8 rotscale, bit 9 double-size / disable
    if (type == 2) continue;
    const bool window = ((attr[0] >> 10) & 3) == 2;
    const u32 shape_size = (attr[0] >> 14) | ((attr[1] & 0xC000) >> 12);
    const s32 w = widths[shape_size], h = heights[shape_size];
    s32 bw = w, bh = h;
    if (type == 3) { bw <<= 1; bh <<= 1; }
    s32 y = attr[0] & 0xFF;
    if (((line - y) & 0xFF) >= static_cast<u32>(bh)) continue;
    const s32 x = static_cast<s32>(static_cast<u32>(attr[1]) << 23) >> 23;
    if (x <= -bw) continue;
    if ((attr[0] & (1 << 12)) && !window) {
      // Mosaic: the line used is the one latched at the top of the mosaic row.
      y = (obj_mosaic_line_ - y) & 0xFF;
      if (y >= bh) y = 0;
    } else y = (line - y) & 0xFF;
    if (type & 1) draw_sprite_rotscale(attr, oam, bw, bh, w, h, x, y, window);
    else draw_sprite_normal(attr, w, h, x, y, window);
    ++num_sprites_;
  }
}

void Engine2D::draw_sprite_normal(const u16* attr, int w, int h, s32 x, s32 y, bool window) {
  const VramView& vv = obj_vram();
  const VramMap& vm = vram();
  const u32 tile = attr[2] & 0x3FF;
  const u32 mode = window ? 0 : ((attr[0] >> 10) & 3);
  u8 a = static_cast<u8>((attr[2] >> 10) & 3) | OA_TOUCHED;
  if ((attr[0] & (1 << 12)) && !window) a |= OA_MOSAIC;
  if (attr[1] & (1 << 13)) y = h - 1 - y;                   // vertical flip
  const bool hflip = attr[1] & (1 << 12);
  u32 xoff = 0, xend = w;
  if (x >= 0) { if (x + xend > 256) xend = 256 - x; } else { xoff = -x; x = 0; }

  if (mode == 3) {
    // Bitmap sprite: direct colour, alpha in the palette field.
    const u32 alpha = attr[2] >> 12;
    if (!alpha) return;
    a |= OA_BITMAP;
    u32 addr;
    if (dispcnt_ & 0x40) {
      if (dispcnt_ & 0x20) return;                           // reserved mapping: draws nothing
      addr = (tile << (7 + ((dispcnt_ >> 22) & 1))) + y * w * 2;
    } else if (dispcnt_ & 0x20) addr = ((tile & 0x1F) << 4) + ((tile & 0x3E0) << 7) + y * 256 * 2;
    else addr = ((tile & 0x0F) << 4) + ((tile & 0x3F0) << 7) + y * 128 * 2;
    for (u32 i = xoff; i < xend; ++i, ++x) {
      const u32 sx = hflip ? (w - 1 - i) : i;
      const u16 c = vm.read16(vv, addr + sx * 2);
      put_sprite_pixel(x, (c & 0x7FFF) | OP_DIRECT, c & 0x8000, a, static_cast<u8>(alpha + 1), window);
    }
    return;
  }

  if (mode == 1) a |= OA_SEMI;
  const bool c256 = attr[0] & (1 << 13);
  u32 base = tile;
  u32 row_stride;                                              // bytes between tile rows
  if (dispcnt_ & (1 << 4)) { base <<= (dispcnt_ >> 20) & 3; row_stride = (w >> 3) << (c256 ? 6 : 5); }
  else row_stride = 0x400;
  base = (base << 5) + (y >> 3) * row_stride;
  u32 pal_base = 0;
  if (c256) {
    base += (y & 7) << 3;
    pal_base = (dispcnt_ & (1u << 31)) ? ((attr[2] & 0xF000) >> 4) : OP_STDPAL;   // ext palette index or standard
    for (u32 i = xoff; i < xend; ++i, ++x) {
      const u32 sx = hflip ? (w - 1 - i) : i;
      const u8 idx = vm.read8(vv, base + (sx >> 3) * 64 + (sx & 7));
      put_sprite_pixel(x, pal_base | idx, idx != 0, a, 0, window);
    }
  } else {
    base += (y & 7) << 2;
    pal_base = OP_STDPAL | ((attr[2] & 0xF000) >> 8);
    for (u32 i = xoff; i < xend; ++i, ++x) {
      const u32 sx = hflip ? (w - 1 - i) : i;
      u8 idx = vm.read8(vv, base + (sx >> 3) * 32 + ((sx & 7) >> 1));
      idx = (sx & 1) ? (idx >> 4) : (idx & 0xF);
      put_sprite_pixel(x, pal_base | idx, idx != 0, a, 0, window);
    }
  }
}

void Engine2D::draw_sprite_rotscale(const u16* attr, const u16* oam, int bw, int bh, int w, int h, s32 x, s32 y, bool window) {
  const VramView& vv = obj_vram();
  const VramMap& vm = vram();
  const u16* rot = &oam[(((attr[1] >> 9) & 0x1F) * 16) + 3];
  const s16 pa = static_cast<s16>(rot[0]), pb = static_cast<s16>(rot[4]), pc = static_cast<s16>(rot[8]), pd = static_cast<s16>(rot[12]);
  const u32 tile = attr[2] & 0x3FF;
  const u32 mode = window ? 0 : ((attr[0] >> 10) & 3);
  u8 a = static_cast<u8>((attr[2] >> 10) & 3) | OA_TOUCHED;
  if ((attr[0] & (1 << 12)) && !window) a |= OA_MOSAIC;
  const s32 cx = bw >> 1, cy = bh >> 1;
  u32 xoff = 0;
  if (x >= 0) { if (x + bw > 256) bw = 256 - x; } else { xoff = -x; x = 0; }
  s32 rx = (static_cast<s32>(xoff) - cx) * pa + (y - cy) * pb + (w << 7);
  s32 ry = (static_cast<s32>(xoff) - cx) * pc + (y - cy) * pd + (h << 7);
  const u32 fw = w << 8, fh = h << 8;

  if (mode == 3) {
    const u32 alpha = attr[2] >> 12;
    if (!alpha) return;
    a |= OA_BITMAP;
    u32 addr, stride;
    if (dispcnt_ & 0x40) {
      if (dispcnt_ & 0x20) return;
      addr = tile << (7 + ((dispcnt_ >> 22) & 1)); stride = w * 2;
    } else if (dispcnt_ & 0x20) { addr = ((tile & 0x1F) << 4) + ((tile & 0x3E0) << 7); stride = 256 * 2; }
    else { addr = ((tile & 0x0F) << 4) + ((tile & 0x3F0) << 7); stride = 128 * 2; }
    for (; xoff < static_cast<u32>(bw); ++xoff, ++x, rx += pa, ry += pc) {
      if (static_cast<u32>(rx) >= fw || static_cast<u32>(ry) >= fh) continue;
      const u16 c = vm.read16(vv, addr + (ry >> 8) * stride + ((rx >> 8) << 1));
      put_sprite_pixel(x, (c & 0x7FFF) | OP_DIRECT, c & 0x8000, a, static_cast<u8>(alpha + 1), window);
    }
    return;
  }

  if (mode == 1) a |= OA_SEMI;
  const bool c256 = attr[0] & (1 << 13);
  u32 base = tile, row_stride;
  if (dispcnt_ & (1 << 4)) { base <<= (dispcnt_ >> 20) & 3; row_stride = (w >> 3) << (c256 ? 6 : 5); }
  else row_stride = 0x400;
  base <<= 5;
  if (c256) {
    const u32 pal_base = (dispcnt_ & (1u << 31)) ? ((attr[2] & 0xF000) >> 4) : OP_STDPAL;
    for (; xoff < static_cast<u32>(bw); ++xoff, ++x, rx += pa, ry += pc) {
      if (static_cast<u32>(rx) >= fw || static_cast<u32>(ry) >= fh) continue;
      const u8 idx = vm.read8(vv, base + (ry >> 11) * row_stride + ((ry & 0x700) >> 5) + (rx >> 11) * 64 + ((rx & 0x700) >> 8));
      put_sprite_pixel(x, pal_base | idx, idx != 0, a, 0, window);
    }
  } else {
    const u32 pal_base = OP_STDPAL | ((attr[2] & 0xF000) >> 8);
    for (; xoff < static_cast<u32>(bw); ++xoff, ++x, rx += pa, ry += pc) {
      if (static_cast<u32>(rx) >= fw || static_cast<u32>(ry) >= fh) continue;
      u8 idx = vm.read8(vv, base + (ry >> 11) * row_stride + ((ry & 0x700) >> 6) + (rx >> 11) * 32 + ((rx & 0x700) >> 9));
      idx = (rx & 0x100) ? (idx >> 4) : (idx & 0xF);
      put_sprite_pixel(x, pal_base | idx, idx != 0, a, 0, window);
    }
  }
}

// Sprite X mosaic runs over the finished OBJ plane, left to right: a pixel is
// re-latched at the start of each mosaic cell, whenever mosaic-ness changes
// between neighbours, or when a higher-priority pixel arrives.
void Engine2D::apply_sprite_mosaic_x() {
  const u32 mw = obj_mosaic_w_;
  if (!mw) return;
  u32 mx = 0; u32 lpx = 0; u8 lattr = 0, lalpha = 0;
  for (u32 i = 0; i < 256; ++i) {
    const u8 cur = obj_attr_[i];
    bool latch = mx == 0 || !(cur & OA_MOSAIC) || !(lattr & OA_MOSAIC) || (cur & OA_PRIO) < (lattr & OA_PRIO);
    if (latch) { lpx = obj_px_[i]; lattr = cur; lalpha = obj_alpha_[i]; }
    obj_px_[i] = lpx; obj_attr_[i] = lattr; obj_alpha_[i] = lalpha;
    mx = (mx == mw) ? 0 : mx + 1;
  }
}

// ---- compositing ------------------------------------------------------------

void Engine2D::build_window_plane() {
  if (!(dispcnt_ & 0xE000)) { win_.fill(0xFF); return; }
  win_.fill(wincnt_[2]);                                     // outside all windows
  if (dispcnt_ & (1 << 15)) for (u32 i = 0; i < 256; ++i) if (obj_win_[i]) win_[i] = wincnt_[3];
  // Horizontal edges are evaluated per pixel with the same edge rule as the
  // vertical ones, so x2 < x1 wraps and x1 == x2 covers nothing.
  if (dispcnt_ & (1 << 14)) {
    const u8 x1 = win1_[0], x2 = win1_[1];
    for (u32 i = 0; i < 256; ++i) {
      if (i == x2) win1_active_ &= ~2; else if (i == x1) win1_active_ |= 2;
      if (win1_active_ == 3) win_[i] = wincnt_[1];
    }
  }
  if (dispcnt_ & (1 << 13)) {
    const u8 x1 = win0_[0], x2 = win0_[1];
    for (u32 i = 0; i < 256; ++i) {
      if (i == x2) win0_active_ &= ~2; else if (i == x1) win0_active_ |= 2;
      if (win0_active_ == 3) win_[i] = wincnt_[0];
    }
  }
}

// Masked select of one BG plane into the top/second records.
void Engine2D::select_bg(int bg) {
  const BgPlane& p = bg_[bg];
  const bool is3d = bg == 0 && !num_ && (dispcnt_ & 8);    // 3D pixels carry their alpha and blend differently
  kern::active::select_plane(p.px.data(), p.op.data(), win_.data(), 1 << bg, 1 << bg, is3d,
                             top_.data(), second_.data(), top_id_.data(), top_kind_.data(), top_alpha_.data(), second_id_.data());
}

// The OBJ plane holds palette indices; they are resolved once per line at
// selection time (the palette can change between pre-render and display).
void Engine2D::resolve_obj_colours() {
  const u16* pal = palette() + 0x100;
  for (u32 i = 0; i < 256; ++i) {
    if (!(obj_attr_[i] & OA_OPAQUE)) continue;
    const u32 v = obj_px_[i];
    u16 c;
    if (v & OP_DIRECT) c = v & 0x7FFF;
    else if (v & OP_STDPAL) c = pal[v & 0xFF];
    else c = obj_extpal(v & 0xFFF);
    obj_col_[i] = rgb15_to_18(c);
  }
}

void Engine2D::select_obj(u32 prio) {
  kern::active::select_obj(obj_col_.data(), obj_attr_.data(), obj_alpha_.data(), win_.data(), prio,
                           top_.data(), second_.data(), top_id_.data(), top_kind_.data(), top_alpha_.data(), second_id_.data());
}

void Engine2D::select_layers() {
  const Pixel backdrop = rgb15_to_18(palette()[0]);
  top_.fill(backdrop); top_id_.fill(L_BACKDROP); top_kind_.fill(K_NORMAL); top_alpha_.fill(0);
  second_.fill(0); second_id_.fill(0);
  const bool objs = (layer_enable_ & 0x10) && num_sprites_;
  if (objs) resolve_obj_colours();
  // Lowest priority first; within a priority BG3..BG0 then OBJ, later wins.
  for (int prio = 3; prio >= 0; --prio) {
    for (int bg = 3; bg >= 0; --bg) {
      if (!(layer_enable_ & (1 << bg))) continue;
      if ((bgcnt_[bg] & 3) != prio) continue;
      select_bg(bg);
    }
    if (objs) select_obj(prio);
  }
}

void Engine2D::colour_effects() {
  kern::active::composite_line(bldcnt_, eva_, evb_, evy_, top_.data(), second_.data(), top_id_.data(), top_kind_.data(),
                               top_alpha_.data(), second_id_.data(), win_.data(), out_.data());
}

} // namespace ds::gpu
