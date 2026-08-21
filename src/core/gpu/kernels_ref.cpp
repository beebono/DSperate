// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Portable reference kernels. These define the behaviour; the NEON twins in
// kernels_neon.cpp must match them bit for bit.
#include "core/gpu/kernels.h"

namespace ds::gpu::kern::ref {

namespace {

inline Pixel blend(Pixel a, Pixel b, u32 eva, u32 evb) {
  u32 r = (((a & 0x00003F) * eva) + ((b & 0x00003F) * evb) + 0x000008) >> 4;
  u32 g = ((((a & 0x003F00) * eva) + ((b & 0x003F00) * evb) + 0x000800) >> 4) & 0x007F00;
  u32 bl = ((((a & 0x3F0000) * eva) + ((b & 0x3F0000) * evb) + 0x080000) >> 4) & 0x7F0000;
  if (r > 0x3F) r = 0x3F;
  if (g > 0x3F00) g = 0x3F00;
  if (bl > 0x3F0000) bl = 0x3F0000;
  return r | g | bl | 0xFF000000;
}

// 3D/2D blend with the 3D layer's 5-bit alpha (alpha+1 of 32).
inline Pixel blend_3d(Pixel a, Pixel b) {
  const u32 eva = ((a >> 24) & 0x1F) + 1, evb = 32 - eva;
  if (eva == 32) return a;
  u32 r = (((a & 0x00003F) * eva) + ((b & 0x00003F) * evb) + 0x000010) >> 5;
  u32 g = ((((a & 0x003F00) * eva) + ((b & 0x003F00) * evb) + 0x001000) >> 5) & 0x007F00;
  u32 bl = ((((a & 0x3F0000) * eva) + ((b & 0x3F0000) * evb) + 0x100000) >> 5) & 0x7F0000;
  if (r > 0x3F) r = 0x3F;
  if (g > 0x3F00) g = 0x3F00;
  if (bl > 0x3F0000) bl = 0x3F0000;
  return r | g | bl | 0xFF000000;
}

inline Pixel brighten(Pixel v, u32 factor, u32 bias) {
  u32 rb = v & 0x3F003F, g = v & 0x003F00;
  rb += (((((0x3F003F - rb) * factor) + (bias * 0x010001)) >> 4) & 0x3F003F);
  g  += (((((0x003F00 - g) * factor) + (bias * 0x000100)) >> 4) & 0x003F00);
  return rb | g | 0xFF000000;
}
inline Pixel darken(Pixel v, u32 factor, u32 bias) {
  u32 rb = v & 0x3F003F, g = v & 0x003F00;
  rb -= ((((rb * factor) + (bias * 0x010001)) >> 4) & 0x3F003F);
  g  -= ((((g * factor) + (bias * 0x000100)) >> 4) & 0x003F00);
  return rb | g | 0xFF000000;
}

} // namespace

void select_plane(const Pixel* px, const u8* op, const u8* win, u8 wbit, u8 id, bool is3d,
                  Pixel* top, Pixel* second, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  for (u32 i = 0; i < 256; ++i) {
    if (!op[i] || !(win[i] & wbit)) continue;
    second[i] = top[i]; second_id[i] = top_id[i];
    top[i] = px[i]; top_id[i] = id;
    top_kind[i] = is3d ? K_3D : K_NORMAL;
    if (is3d) top_alpha[i] = (px[i] >> 24) & 0x1F;
  }
}

void select_obj(const Pixel* col, const u8* attr, const u8* alpha, const u8* win, u32 prio,
                Pixel* top, Pixel* second, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio || !(win[i] & 0x10)) continue;
    second[i] = top[i]; second_id[i] = top_id[i];
    top[i] = col[i]; top_id[i] = L_OBJ;
    top_kind[i] = (a & OA_BITMAP) ? K_OBJ_BITMAP : (a & OA_SEMI) ? K_OBJ_SEMI : K_NORMAL;
    top_alpha[i] = alpha[i];
  }
}

void composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,
                    const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,
                    const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  for (u32 i = 0; i < 256; ++i) {
    const Pixel a = top[i], b = second[i];
    const u32 t1 = top_id[i], t2 = static_cast<u32>(second_id[i]) << 8;
    const u8 kind = top_kind[i];
    Pixel o = a;
    if ((kind == K_OBJ_SEMI || kind == K_OBJ_BITMAP) && (bldcnt & t2)) {
      // Semi-transparent and bitmap sprites blend whenever the layer below is
      // a second target, regardless of the selected effect.
      const u32 ea = (kind == K_OBJ_BITMAP) ? top_alpha[i] : eva;
      const u32 eb = (kind == K_OBJ_BITMAP) ? 16 - ea : evb;
      o = blend(a, b, ea, eb);
    } else if (kind == K_3D && (bldcnt & t2)) {
      o = blend_3d(a, b);
    } else if ((bldcnt & t1) && (win[i] & 0x20)) {
      switch (effect) {
      case 1: if (bldcnt & t2) o = blend(a, b, eva, evb); break;
      case 2: o = brighten(a, evy, 0x8); break;
      case 3: o = darken(a, evy, 0x7); break;
      default: break;
      }
    }
    out[i] = (o & 0x00FFFFFF) | 0xFF000000;
  }
}

void palette_to_18(const u16* pal, Pixel* out, u32 n) {
  for (u32 i = 0; i < n; ++i) {
    const u32 c = pal[i];
    out[i] = ((c & 0x001F) << 1) | ((((c & 0x03E0) >> 4) | ((c & 0x8000) >> 15)) << 8) | (((c & 0x7C00) >> 9) << 16);
  }
}

void tile_row_pal16(const u8* idx, const Pixel* pal18, Pixel* px, u8* op) {
  for (u32 i = 0; i < 8; ++i) { px[i] = pal18[idx[i] & 0xF]; op[i] = idx[i] != 0; }
}

void layer_3d(const u32* line3d, Pixel* px, u8* op) {
  for (u32 i = 0; i < 256; ++i) { px[i] = line3d[i]; op[i] = (line3d[i] >> 24) != 0; }
}

void master_brightness(u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  if (mode == 1) {
    for (u32 i = 0; i < 256; ++i) {
      const u32 v = dst[i]; u32 rb = v & 0x3F003F, g = v & 0x003F00;
      rb += ((((0x3F003F - rb) * factor) >> 4) & 0x3F003F);
      g  += ((((0x003F00 - g) * factor) >> 4) & 0x003F00);
      dst[i] = rb | g | 0xFF000000;
    }
  } else if (mode == 2) {
    for (u32 i = 0; i < 256; ++i) {
      const u32 v = dst[i]; u32 rb = v & 0x3F003F, g = v & 0x003F00;
      rb -= ((((rb * factor) + (0xF * 0x010001)) >> 4) & 0x3F003F);
      g  -= ((((g * factor) + (0xF * 0x000100)) >> 4) & 0x003F00);
      dst[i] = rb | g | 0xFF000000;
    }
  }
}

void expand_colours(u32* dst) {
  for (u32 i = 0; i < 256; ++i) {
    const u32 c = dst[i];
    const u32 v = ((c & 0x3F) << 18) | ((c & 0x3F00) << 2) | ((c & 0x3F0000) >> 14);
    dst[i] = v | ((v & 0xC0C0C0) >> 6) | 0xFF000000;
  }
}

} // namespace ds::gpu::kern::ref
