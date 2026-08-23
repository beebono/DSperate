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

bool line_has_translucent_3d(const Pixel* line3d) {
  for (u32 i = 0; i < 256; ++i) { const u32 a = (line3d[i] >> 24) & 0x1F; if (a != 0 && a != 31) return true; }
  return false;
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




namespace {
inline void obj_plot(u32 i, u16 value, bool opaque, u8 attr, u8 alpha, u16* px, u8* oattr, u8* oalpha) {
  const u8 old = oattr[i];
  if (opaque && (!(old & OA_OPAQUE) || (attr & OA_PRIO) < (old & OA_PRIO))) {
    px[i] = value; oattr[i] = attr | OA_OPAQUE; oalpha[i] = alpha;
  } else if (!opaque && !(old & OA_OPAQUE)) {
    oattr[i] = (old & ~(OA_MOSAIC | OA_PRIO)) | (attr & (OA_TOUCHED | OA_MOSAIC | OA_PRIO));
  }
}
}




namespace {
inline u8 obj_tid(u8 attr) { return (attr & OA_BITMAP) ? T_OBJ_DIRECT : (attr & OA_STDPAL) ? T_OBJ_STD : T_OBJ_EXT; }
}

void select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  for (u32 i = 0; i < 256; ++i) {
    if (!(v[i] & LV_OPAQUE) || !(win[i] & wbit)) continue;
    second[i] = top[i]; second_tid[i] = top_tid[i];
    top[i] = v[i]; top_tid[i] = tid;
  }
}

void select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio || !(win[i] & 0x10)) continue;
    second[i] = top[i]; second_tid[i] = top_tid[i];
    top[i] = v[i]; top_tid[i] = obj_tid(a);
  }
}

void select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid) {
  for (u32 i = 0; i < 256; ++i) {
    if (!(v[i] & LV_OPAQUE) || !(win[i] & wbit)) continue;
    top[i] = v[i]; top_tid[i] = tid;
  }
}

void select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid) {
  for (u32 i = 0; i < 256; ++i) {
    const u8 a = attr[i];
    if (!(a & OA_OPAQUE) || (a & OA_PRIO) != prio || !(win[i] & 0x10)) continue;
    top[i] = v[i]; top_tid[i] = obj_tid(a);
  }
}

void resolve16(const u16* top, const u8* top_tid, const Pixel* const* tables, Pixel* out) {
  for (u32 i = 0; i < 256; ++i) out[i] = tables[top_tid[i]][top[i] & 0x7FFF] | 0xFF000000;
}

void resolve16_one(const u16* v, const Pixel* table, Pixel* out) {
  for (u32 i = 0; i < 256; ++i) out[i] = table[v[i] & 0x7FFF] | 0xFF000000;
}

void resolve16_full(const u16* top, const u8* top_tid, const u16* second, const u8* second_tid,
                    const Pixel* const* tables, const u8* attr, const u8* alpha, const Pixel* line3d,
                    Pixel* top_px, Pixel* second_px, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  static const u8 id_of[T_COUNT] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0};
  for (u32 i = 0; i < 256; ++i) {
    const u8 tt = top_tid[i], st = second_tid[i];
    top_px[i] = tables[tt][top[i] & 0x7FFF] | 0xFF000000;
    second_px[i] = tables[st][second[i] & 0x7FFF] | 0xFF000000;
    top_id[i] = id_of[tt]; second_id[i] = id_of[st];
    u8 kind = K_NORMAL, a = 0;
    if (tt == T_BG0 && line3d) { kind = K_3D; a = (line3d[i] >> 24) & 0x1F; top_px[i] = line3d[i]; }
    else if (tt >= T_OBJ_STD && tt <= T_OBJ_DIRECT) {
      const u8 at = attr[i];
      kind = (at & OA_BITMAP) ? K_OBJ_BITMAP : (at & OA_SEMI) ? K_OBJ_SEMI : K_NORMAL;
      a = alpha[i];
    }
    top_kind[i] = kind; top_alpha[i] = a;
  }
}

void obj_row_idx16(const u8* idx, u32 n, u16 pal_base, u8 attr, u16* v, u8* oattr, u8* oalpha) {
  for (u32 i = 0; i < n; ++i) obj_plot(i, static_cast<u16>(LV_OPAQUE | pal_base | idx[i]), idx[i] != 0, attr, 0, v, oattr, oalpha);
}

void obj_row_bmp16(const u16* col, u32 n, u8 attr, u8 alpha, u16* v, u8* oattr, u8* oalpha) {
  for (u32 i = 0; i < n; ++i) obj_plot(i, col[i], col[i] & 0x8000, attr, alpha, v, oattr, oalpha);
}

void layer16_3d(const u32* line3d, u16* v) {
  for (u32 i = 0; i < 256; ++i) v[i] = (line3d[i] >> 24) ? static_cast<u16>(LV_OPAQUE | i) : 0;
}

bool text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v) {
  bool any = false;
  for (u32 t = 0; t < n; ++t, packed += 4, v += 8) {
    const u16 base = static_cast<u16>((ctl[t] & 0xF) << 4);
    const bool flip = ctl[t] & 0x10;
    for (u32 i = 0; i < 8; ++i) {
      const u32 j = flip ? 7 - i : i;
      const u8 idx = (packed[j >> 1] >> ((j & 1) * 4)) & 0xF;
      v[i] = idx ? static_cast<u16>(LV_OPAQUE | base | idx) : 0; any |= idx != 0;
    }
  }
  return any;
}

bool text_row_256(const u8* rows, const u8* ctl, u32 n, bool ext, u16* v) {
  bool any = false;
  for (u32 t = 0; t < n; ++t, rows += 8, v += 8) {
    const u16 base = ext ? static_cast<u16>((ctl[t] & 0xF) << 8) : 0;
    const bool flip = ctl[t] & 0x10;
    for (u32 i = 0; i < 8; ++i) {
      const u8 idx = rows[flip ? 7 - i : i];
      v[i] = idx ? static_cast<u16>(LV_OPAQUE | base | idx) : 0; any |= idx != 0;
    }
  }
  return any;
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

void output_line(const Pixel* src, u16 reg, u32* dst) {
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  master_brightness(reg, dst);
  expand_colours(dst);
}


// ---- 3D span stages ------------------------------------------------------------
// These are Renderer3D::Interp<0> (render3d.cpp) applied to every pixel of a
// span; the scalar forms here are the specification.

void span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac) {
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    const u32 num = static_cast<u32>(xv * w0n) << 8;
    const u32 den = static_cast<u32>(xv * w0d) + static_cast<u32>((xdiff - xv) * w1d);
    fac[i] = den == 0 ? 0 : num / den;
  }
}

void span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out) {
  if (y0 == y1) { for (u32 i = 0; i < n; ++i) out[i] = y0; return; }
  if (y0 < y1) { const s64 d = y1 - y0; for (u32 i = 0; i < n; ++i) out[i] = y0 + static_cast<s32>((d * fac[i]) >> 8); return; }
  const s64 d = y0 - y1;
  for (u32 i = 0; i < n; ++i) out[i] = y1 + static_cast<s32>((d * (256 - fac[i])) >> 8);
}

void span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out) {
  for (int k = 0; k < 5; ++k) span_attr_persp(y0[k], y1[k], fac, n, out[k]);
}

void span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out) {
  if (y0 == y1) { for (u32 i = 0; i < n; ++i) out[i] = y0; return; }
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    if (y0 < y1) out[i] = y0 + static_cast<s32>(static_cast<s64>(y1 - y0) * xv / xdiff);
    else out[i] = y1 + static_cast<s32>(static_cast<s64>(y0 - y1) * (xdiff - xv) / xdiff);
  }
}

void span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out) {
  if (z0 == z1) { for (u32 i = 0; i < n; ++i) out[i] = z0; return; }
  s32 base, disp;
  if (z0 < z1) { base = z0; disp = z1 - z0; } else { base = z1; disp = z0 - z1; }
  disp >>= 9;
  for (u32 i = 0; i < n; ++i) {
    const s32 xv = xv0 + static_cast<s32>(i);
    const s32 factor = z0 < z1 ? xv : xdiff - xv;
    out[i] = base + static_cast<s32>((static_cast<s64>(disp) * factor * xrecip) >> 13);
  }
}

u32 depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass) {
  u32 first = n, last = 0;
  for (u32 i = 0; i < n; ++i) {
    const s32 d = static_cast<s32>(dstz[i]);
    bool ok;
    switch (mode) {
    case 0: ok = z[i] < d; break;
    case 1: ok = (dstattr[i] & 0x00400010) == 0x00000010 ? z[i] <= d : z[i] < d; break;
    case 2: ok = static_cast<u32>((d - z[i]) + 0x200) <= 0x400; break;
    default: ok = static_cast<u32>((d - z[i]) + 0xFF) <= 0x1FE; break;
    }
    const u8 v = ok ? 1 : ((dstattr[i] & 0xF) ? 2 : 0);
    pass[i] = v;
    if (v) { if (i < first) first = i; last = i; }
  }
  return first < n ? (first << 16) | (last + 1) : 0;
}

} // namespace ds::gpu::kern::ref
