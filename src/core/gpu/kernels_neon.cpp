// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// NEON twins of kernels_ref.cpp. Same names, same signatures, bit-identical
// output (tests/kernels_test.cpp, run under qemu-aarch64, diffs them).
//
// Conventions: 18-bit colour records stay packed in u32 lanes (R 0-5, G 8-13,
// B 16-21) and channel maths is done on the masked lanes the way the scalar
// code does it; byte planes (ids, kinds, masks) are handled 16 at a time and
// widened to u32 lane masks where they gate colour lanes.
#include "core/gpu/kernels.h"

#if DSPERATE_NEON
#include <arm_neon.h>
#include <cstring>

namespace ds::gpu::kern::neon {

namespace {

// u8x16 mask -> four u32x4 lane masks.
struct Mask4 { uint32x4_t m[4]; };
// Sign-extending, so a 0xFF byte mask becomes an all-ones lane mask (values
// below 0x80, such as alphas, widen unchanged).
inline Mask4 widen(uint8x16_t m8) {
  const int8x16_t s = vreinterpretq_s8_u8(m8);
  const int16x8_t lo = vmovl_s8(vget_low_s8(s)), hi = vmovl_s8(vget_high_s8(s));
  return {{vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(lo))), vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(lo))),
           vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(hi))), vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(hi)))}};
}
// four u32x4 -> u8x16 (low bytes).
inline uint8x16_t narrow(uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t d) {
  return vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(a), vmovn_u32(b))), vmovn_u16(vcombine_u16(vmovn_u32(c), vmovn_u32(d))));
}

// Blend of two records with per-lane weights, 4 pixels. R and B share one
// multiply chain (each field reaches at most 63*32+16 < 2^11, so they do not
// meet); after the shift a field is at most 126, so its bit 6 is the
// overflow flag that selects the 0x3F clamp.
inline uint32x4_t blend4(uint32x4_t a, uint32x4_t b, uint32x4_t ea, uint32x4_t eb, u32 round_r, u32 shift) {
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x3F00);
  uint32x4_t rb = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r * 0x010001), vandq_u32(a, mrb), ea), vandq_u32(b, mrb), eb);
  uint32x4_t g = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r << 8), vandq_u32(a, mg), ea), vandq_u32(b, mg), eb);
  const int32x4_t sh = vdupq_n_s32(-static_cast<s32>(shift));
  rb = vshlq_u32(rb, sh); g = vshlq_u32(g, sh);
  const uint32x4_t orb = vandq_u32(vshrq_n_u32(rb, 6), vdupq_n_u32(0x010001)), og = vandq_u32(vshrq_n_u32(g, 6), vdupq_n_u32(0x100));
  rb = vorrq_u32(vandq_u32(rb, mrb), vsubq_u32(vshlq_n_u32(orb, 6), orb));
  g = vorrq_u32(vandq_u32(g, mg), vsubq_u32(vshlq_n_u32(og, 6), og));
  return vorrq_u32(vorrq_u32(rb, g), vdupq_n_u32(0xFF000000));
}

inline uint32x4_t brighten4(uint32x4_t v, u32 factor, u32 bias) {
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00);
  const uint32x4_t rb = vandq_u32(v, mrb), g = vandq_u32(v, mg);
  uint32x4_t drb = vmlaq_u32(vdupq_n_u32(bias * 0x010001), vsubq_u32(mrb, rb), vdupq_n_u32(factor));
  uint32x4_t dg = vmlaq_u32(vdupq_n_u32(bias * 0x000100), vsubq_u32(mg, g), vdupq_n_u32(factor));
  drb = vandq_u32(vshrq_n_u32(drb, 4), mrb);
  dg = vandq_u32(vshrq_n_u32(dg, 4), mg);
  return vorrq_u32(vorrq_u32(vaddq_u32(rb, drb), vaddq_u32(g, dg)), vdupq_n_u32(0xFF000000));
}
inline uint32x4_t darken4(uint32x4_t v, u32 factor, u32 bias) {
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00);
  const uint32x4_t rb = vandq_u32(v, mrb), g = vandq_u32(v, mg);
  uint32x4_t drb = vmlaq_u32(vdupq_n_u32(bias * 0x010001), rb, vdupq_n_u32(factor));
  uint32x4_t dg = vmlaq_u32(vdupq_n_u32(bias * 0x000100), g, vdupq_n_u32(factor));
  drb = vandq_u32(vshrq_n_u32(drb, 4), mrb);
  dg = vandq_u32(vshrq_n_u32(dg, 4), mg);
  return vorrq_u32(vorrq_u32(vsubq_u32(rb, drb), vsubq_u32(g, dg)), vdupq_n_u32(0xFF000000));
}

// The same maths on byte planes, 16 pixels a call: a record's r g b bytes
// are deinterleaved by ld4 into planes, the products live in 16-bit lanes and
// come back through a narrowing shift. Per-lane weights are bytes, so the
// bitmap-sprite and 3D blends (whose weights vary per pixel) cost the same as
// the fixed EVA/EVB blend. Inputs are the 6-bit fields (the caller masks).
//
// blend: (a*ea + b*eb + 2^(shift-1)) >> shift, clamped to 63. After the shift
// a field is at most 126, so it fits the byte and the clamp is one min.
template <int shift>
inline uint8x16_t blend16(uint8x16_t a, uint8x16_t b, uint8x16_t ea, uint8x16_t eb) {
  const uint16x8_t rnd = vdupq_n_u16(1u << (shift - 1));
  const uint16x8_t lo = vmlal_u8(vmlal_u8(rnd, vget_low_u8(a), vget_low_u8(ea)), vget_low_u8(b), vget_low_u8(eb));
  const uint16x8_t hi = vmlal_high_u8(vmlal_high_u8(rnd, a, ea), b, eb);
  return vminq_u8(vcombine_u8(vshrn_n_u16(lo, shift), vshrn_n_u16(hi, shift)), vdupq_n_u8(63));
}
// c + (((63 - c) * factor + bias) >> 4): the scalar's & 0x3F after the shift
// is a no-op there (the shifted term is at most 63), and so is dropped.
inline uint8x16_t brighten16(uint8x16_t c, uint8x8_t factor, u16 bias) {
  const uint8x16_t inv = vsubq_u8(vdupq_n_u8(63), c);
  const uint16x8_t lo = vmlal_u8(vdupq_n_u16(bias), vget_low_u8(inv), factor);
  const uint16x8_t hi = vmlal_u8(vdupq_n_u16(bias), vget_high_u8(inv), factor);
  return vaddq_u8(c, vcombine_u8(vshrn_n_u16(lo, 4), vshrn_n_u16(hi, 4)));
}
// c - ((c * factor + bias) >> 4), likewise.
inline uint8x16_t darken16(uint8x16_t c, uint8x8_t factor, u16 bias) {
  const uint16x8_t lo = vmlal_u8(vdupq_n_u16(bias), vget_low_u8(c), factor);
  const uint16x8_t hi = vmlal_u8(vdupq_n_u16(bias), vget_high_u8(c), factor);
  return vsubq_u8(c, vcombine_u8(vshrn_n_u16(lo, 4), vshrn_n_u16(hi, 4)));
}

} // namespace


bool line_has_translucent_3d(const Pixel* line3d) {
  const uint32x4_t m = vdupq_n_u32(0x1F), v31 = vdupq_n_u32(31), zero = vdupq_n_u32(0);
  uint32x4_t acc = zero;
  for (u32 i = 0; i < 256; i += 4) {
    const uint32x4_t a = vandq_u32(vshrq_n_u32(vld1q_u32(line3d + i), 24), m);
    acc = vorrq_u32(acc, vbicq_u32(vmvnq_u32(vceqq_u32(a, zero)), vceqq_u32(a, v31)));
  }
  return vmaxvq_u32(acc) != 0;
}




void composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,
                    const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,
                    const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  const uint8x16_t v_t1 = vdupq_n_u8(effect ? static_cast<u8>(bldcnt) : 0), v_t2 = vdupq_n_u8(static_cast<u8>(bldcnt >> 8));
  const uint8x16_t veva = vdupq_n_u8(static_cast<u8>(eva)), vevb = vdupq_n_u8(static_cast<u8>(evb));
  const uint8x8_t vevy = vdup_n_u8(static_cast<u8>(evy));
  const uint8x16_t v63 = vdupq_n_u8(63), vff = vdupq_n_u8(0xFF), v16 = vdupq_n_u8(16), v32 = vdupq_n_u8(32);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t kind = vld1q_u8(top_kind + i);
    const uint8x16_t t2hit = vtstq_u8(vld1q_u8(second_id + i), v_t2);
    // With no effect selected the first-target mask is empty (v_t1 = 0), so
    // `fx` only gates the three effect cases.
    const uint8x16_t t1hit = vandq_u8(vtstq_u8(vld1q_u8(top_id + i), v_t1), vtstq_u8(vld1q_u8(win + i), vdupq_n_u8(0x20)));
    const uint8x16_t is_bitmap = vceqq_u8(kind, vdupq_n_u8(K_OBJ_BITMAP));
    const uint8x16_t objblend = vandq_u8(vorrq_u8(vceqq_u8(kind, vdupq_n_u8(K_OBJ_SEMI)), is_bitmap), t2hit);
    const uint8x16_t blend3d = vandq_u8(vceqq_u8(kind, vdupq_n_u8(K_3D)), t2hit);
    const uint8x16_t fx = vbicq_u8(vbicq_u8(t1hit, objblend), blend3d);
    // The records as byte planes; lanes no effect touches pass through as
    // they are (bytes 0-2 whole, alpha 0xFF).
    uint8x16x4_t a = vld4q_u8(reinterpret_cast<const u8*>(top + i));
    // Most blocks of most lines blend nothing: copy through.
    if (vmaxvq_u8(vorrq_u8(vorrq_u8(objblend, blend3d), fx)) == 0) {
      a.val[3] = vff;
      vst4q_u8(reinterpret_cast<u8*>(out + i), a);
      continue;
    }
    const bool has_obj = vmaxvq_u8(objblend) != 0, has_3d = vmaxvq_u8(blend3d) != 0, has_fx = vmaxvq_u8(fx) != 0;
    const uint8x16x4_t b = vld4q_u8(reinterpret_cast<const u8*>(second + i));
    uint8x16_t a6[3], b6[3], o[3];
    for (u32 c = 0; c < 3; ++c) { a6[c] = vandq_u8(a.val[c], v63); b6[c] = vandq_u8(b.val[c], v63); o[c] = a.val[c]; }
    if (has_fx) {
      for (u32 c = 0; c < 3; ++c) {
        uint8x16_t o_fx = a.val[c];
        if (effect == 1) o_fx = vbslq_u8(t2hit, blend16<4>(a6[c], b6[c], veva, vevb), a.val[c]);
        else if (effect == 2) o_fx = brighten16(a6[c], vevy, 8);
        else if (effect == 3) o_fx = darken16(a6[c], vevy, 7);
        o[c] = vbslq_u8(fx, o_fx, a.val[c]);
      }
    }
    if (has_3d) {
      // 3D blend with (alpha + 1) of 32; alpha 31 passes the top record through.
      const uint8x16_t a3 = vaddq_u8(vandq_u8(a.val[3], vdupq_n_u8(0x1F)), vdupq_n_u8(1));
      const uint8x16_t eb3 = vsubq_u8(v32, a3), keep = vceqq_u8(a3, v32);
      for (u32 c = 0; c < 3; ++c) o[c] = vbslq_u8(blend3d, vbslq_u8(keep, a.val[c], blend16<5>(a6[c], b6[c], a3, eb3)), o[c]);
    }
    if (has_obj) {
      // OBJ blend: bitmap sprites use their own alpha as EVA, 16 - EVA as EVB.
      const uint8x16_t ea = vbslq_u8(is_bitmap, vld1q_u8(top_alpha + i), veva);
      const uint8x16_t eb = vbslq_u8(is_bitmap, vsubq_u8(v16, ea), vevb);
      for (u32 c = 0; c < 3; ++c) o[c] = vbslq_u8(objblend, blend16<4>(a6[c], b6[c], ea, eb), o[c]);
    }
    const uint8x16x4_t rec = {o[0], o[1], o[2], vff};
    vst4q_u8(reinterpret_cast<u8*>(out + i), rec);
  }
}

void palette_to_18(const u16* pal, Pixel* out, u32 n) {
  for (u32 i = 0; i < n; i += 8) {
    const uint16x8_t c = vld1q_u16(pal + i);
    const uint16x8_t r = vshlq_n_u16(vandq_u16(c, vdupq_n_u16(0x1F)), 1);
    const uint16x8_t g = vorrq_u16(vshrq_n_u16(vandq_u16(c, vdupq_n_u16(0x3E0)), 4), vshrq_n_u16(c, 15));
    const uint16x8_t b = vshrq_n_u16(vandq_u16(c, vdupq_n_u16(0x7C00)), 9);
    const uint32x4_t lo = vorrq_u32(vorrq_u32(vmovl_u16(vget_low_u16(r)), vshlq_n_u32(vmovl_u16(vget_low_u16(g)), 8)), vshlq_n_u32(vmovl_u16(vget_low_u16(b)), 16));
    const uint32x4_t hi = vorrq_u32(vorrq_u32(vmovl_u16(vget_high_u16(r)), vshlq_n_u32(vmovl_u16(vget_high_u16(g)), 8)), vshlq_n_u32(vmovl_u16(vget_high_u16(b)), 16));
    vst1q_u32(out + i, lo);
    vst1q_u32(out + i + 4, hi);
  }
}

// 16-entry palette lookup as a 64-byte table lookup: each index is expanded
// to the four byte offsets of its 32-bit record.

namespace {
inline uint8x16x4_t pal16_table(const Pixel* pal) {
  const u8* p = reinterpret_cast<const u8*>(pal);
  return {{vld1q_u8(p), vld1q_u8(p + 16), vld1q_u8(p + 32), vld1q_u8(p + 48)}};
}
// 8 indices -> 8 records through a 16-entry palette held as a 64-byte table.
inline void pal16_row(uint8x8_t idx, const uint8x16x4_t& table, Pixel* px) {
  const uint8x8_t i4 = vshl_n_u8(idx, 2);
  const uint8x8x2_t z1 = vzip_u8(i4, i4);
  const uint8x16_t twice = vcombine_u8(z1.val[0], z1.val[1]);
  const uint8x16x2_t z2 = vzipq_u8(twice, twice);
  const uint8x16_t step = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
  vst1q_u8(reinterpret_cast<u8*>(px), vqtbl4q_u8(table, vaddq_u8(z2.val[0], step)));
  vst1q_u8(reinterpret_cast<u8*>(px) + 16, vqtbl4q_u8(table, vaddq_u8(z2.val[1], step)));
}
}



namespace {
// The priority rule on 16 plane entries: `opq` marks the sprite's opaque
// pixels, `valid` the lanes inside the row; colour lanes come from `col`.
inline void obj_plot16(uint8x16_t opq, uint8x16_t valid, uint16x8_t v0, uint16x8_t v1, u8 attr, u8 alpha, u16* px, u8* oattr, u8* oalpha) {
  const uint8x16_t old = vld1q_u8(oattr);
  const uint8x16_t old_opaque = vtstq_u8(old, vdupq_n_u8(OA_OPAQUE));
  const uint8x16_t higher = vcgtq_u8(vandq_u8(old, vdupq_n_u8(OA_PRIO)), vdupq_n_u8(attr & OA_PRIO));
  const uint8x16_t win = vandq_u8(vandq_u8(opq, vorrq_u8(vmvnq_u8(old_opaque), higher)), valid);
  const uint8x16_t stamp = vandq_u8(vbicq_u8(vmvnq_u8(opq), old_opaque), valid);
  uint8x16_t a = vbslq_u8(stamp, vorrq_u8(vandq_u8(old, vdupq_n_u8(static_cast<u8>(~(OA_MOSAIC | OA_PRIO)))), vdupq_n_u8(attr & (OA_TOUCHED | OA_MOSAIC | OA_PRIO))), old);
  a = vbslq_u8(win, vdupq_n_u8(attr | OA_OPAQUE), a);
  vst1q_u8(oattr, a);
  vst1q_u8(oalpha, vbslq_u8(win, vdupq_n_u8(alpha), vld1q_u8(oalpha)));
  const int8x16_t ws = vreinterpretq_s8_u8(win);
  const uint16x8_t m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(ws))), m1 = vreinterpretq_u16_s16(vmovl_s8(vget_high_s8(ws)));
  vst1q_u16(px, vbslq_u16(m0, v0, vld1q_u16(px)));
  vst1q_u16(px + 8, vbslq_u16(m1, v1, vld1q_u16(px + 8)));
}
const uint8x16_t kLane16 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
}




// One gather per pixel instead of two, and the id table lookup vectorised.
// The table pointer is hoisted per 16 pixels when the ids agree, as resolve16
// does: a line is usually long runs of one layer.
void resolve16_top(const u16* top, const u8* top_tid, const Pixel* const* tables, const Pixel* line3d,
                   Pixel* top_px, u8* top_id) {
  static const u8 id_tab[16] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8x16_t ids = vld1q_u8(id_tab);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t tt = vld1q_u8(top_tid + i);
    vst1q_u8(top_id + i, vqtbl1q_u8(ids, tt));
    const u8 t0 = top_tid[i];
    if (vmaxvq_u8(tt) == vminvq_u8(tt) && !(line3d && t0 == T_BG0)) {
      const Pixel* tab = tables[t0];
      for (u32 k = 0; k < 16; ++k) top_px[i + k] = tab[top[i + k] & 0x7FFF] | 0xFF000000;
    } else {
      for (u32 k = 0; k < 16; ++k) {
        const u8 t = top_tid[i + k];
        top_px[i + k] = (line3d && t == T_BG0) ? line3d[i + k] : (tables[t][top[i + k] & 0x7FFF] | 0xFF000000);
      }
    }
  }
}

void composite_line_fade(u32 bldcnt, u32 evy, const Pixel* top, const u8* top_id, const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  const uint8x16_t v_t1 = vdupq_n_u8(static_cast<u8>(bldcnt));
  const uint8x16_t v63 = vdupq_n_u8(63), vff = vdupq_n_u8(0xFF);
  const uint8x8_t vevy = vdup_n_u8(static_cast<u8>(evy));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t hit = vandq_u8(vtstq_u8(vld1q_u8(top_id + i), v_t1), vtstq_u8(vld1q_u8(win + i), vdupq_n_u8(0x20)));
    uint8x16x4_t a = vld4q_u8(reinterpret_cast<const u8*>(top + i));
    for (u32 c = 0; c < 3; ++c) {
      const uint8x16_t a6 = vandq_u8(a.val[c], v63);
      a.val[c] = vbslq_u8(hit, effect == 2 ? brighten16(a6, vevy, 8) : darken16(a6, vevy, 7), a.val[c]);
    }
    a.val[3] = vff;
    vst4q_u8(reinterpret_cast<u8*>(out + i), a);
  }
}

void resolve16_full(const u16* top, const u8* top_tid, const u16* second, const u8* second_tid,
                    const Pixel* const* tables, const u8* attr, const u8* alpha, const Pixel* line3d,
                    Pixel* top_px, Pixel* second_px, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  static const u8 id_tab[16] = {L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_OBJ, L_OBJ, L_BACKDROP, 0, 0, 0, 0, 0, 0, 0, 0};
  const uint8x16_t ids = vld1q_u8(id_tab), zero = vdupq_n_u8(0);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t tt = vld1q_u8(top_tid + i), st = vld1q_u8(second_tid + i);
    vst1q_u8(top_id + i, vqtbl1q_u8(ids, tt));
    vst1q_u8(second_id + i, vqtbl1q_u8(ids, st));
    const uint8x16_t at = vld1q_u8(attr + i);
    const uint8x16_t isobj = vandq_u8(vcgeq_u8(tt, vdupq_n_u8(T_OBJ_STD)), vcleq_u8(tt, vdupq_n_u8(T_OBJ_DIRECT)));
    uint8x16_t kind = vbslq_u8(vtstq_u8(at, vdupq_n_u8(OA_BITMAP)), vdupq_n_u8(K_OBJ_BITMAP), vbslq_u8(vtstq_u8(at, vdupq_n_u8(OA_SEMI)), vdupq_n_u8(K_OBJ_SEMI), zero));
    kind = vandq_u8(kind, isobj);
    uint8x16_t al = vandq_u8(vld1q_u8(alpha + i), isobj);
    if (line3d) {
      const uint8x16_t is3d = vceqq_u8(tt, zero);
      kind = vbslq_u8(is3d, vdupq_n_u8(K_3D), kind);
      const uint8x16_t a3 = narrow(vshrq_n_u32(vld1q_u32(line3d + i), 24), vshrq_n_u32(vld1q_u32(line3d + i + 4), 24),
                                  vshrq_n_u32(vld1q_u32(line3d + i + 8), 24), vshrq_n_u32(vld1q_u32(line3d + i + 12), 24));
      al = vbslq_u8(is3d, vandq_u8(a3, vdupq_n_u8(0x1F)), al);
    }
    vst1q_u8(top_kind + i, kind);
    vst1q_u8(top_alpha + i, al);
    // Layer ids normally occur in runs.  Hoist both table pointers when the
    // whole block is uniform, avoiding two pointer-table loads per pixel.
    // The 3D override must remain per-pixel, so it disables only the top
    // lookup hoist when BG0 is the selected layer.
    const u8 t0 = top_tid[i], s0 = second_tid[i];
    const bool top_run = vmaxvq_u8(tt) == vminvq_u8(tt) && !(line3d && t0 == T_BG0);
    const bool second_run = vmaxvq_u8(st) == vminvq_u8(st);
    if (top_run && second_run) {
      const Pixel* top_tab = tables[t0];
      const Pixel* second_tab = tables[s0];
      for (u32 k = 0; k < 16; ++k) {
        top_px[i + k] = top_tab[top[i + k] & 0x7FFF] | 0xFF000000;
        second_px[i + k] = second_tab[second[i + k] & 0x7FFF] | 0xFF000000;
      }
    } else {
      for (u32 k = 0; k < 16; ++k) {
        const u8 t = top_tid[i + k];
        top_px[i + k] = (line3d && t == T_BG0) ? line3d[i + k] : (tables[t][top[i + k] & 0x7FFF] | 0xFF000000);
        second_px[i + k] = tables[second_tid[i + k]][second[i + k] & 0x7FFF] | 0xFF000000;
      }
    }
  }
}

void obj_row_idx16(const u8* idx, u32 n, u16 pal_base, u8 attr, u16* v, u8* oattr, u8* oalpha) {
  const uint16x8_t base = vdupq_n_u16(static_cast<u16>(LV_OPAQUE | pal_base));
  for (u32 i = 0; i < n; i += 16) {
    const uint8x16_t x = vld1q_u8(idx + i);
    const uint8x16_t valid = vcltq_u8(kLane16, vdupq_n_u8(static_cast<u8>(n - i > 16 ? 16 : n - i)));
    obj_plot16(vmvnq_u8(vceqq_u8(x, vdupq_n_u8(0))), valid, vorrq_u16(base, vmovl_u8(vget_low_u8(x))), vorrq_u16(base, vmovl_u8(vget_high_u8(x))),
               attr, 0, v + i, oattr + i, oalpha + i);
  }
}

void obj_row_bmp16(const u16* col, u32 n, u8 attr, u8 alpha, u16* v, u8* oattr, u8* oalpha) {
  for (u32 i = 0; i < n; i += 16) {
    const uint16x8_t a = vld1q_u16(col + i), b = vld1q_u16(col + i + 8);
    const uint8x16_t opaque = vtstq_u8(vcombine_u8(vshrn_n_u16(a, 8), vshrn_n_u16(b, 8)), vdupq_n_u8(0x80));
    const uint8x16_t valid = vcltq_u8(kLane16, vdupq_n_u8(static_cast<u8>(n - i > 16 ? 16 : n - i)));
    obj_plot16(opaque, valid, a, b, attr, alpha, v + i, oattr + i, oalpha + i);
  }
}

bool layer16_3d(const u32* line3d, u16* v) {
  const uint16x8_t lane = {0, 1, 2, 3, 4, 5, 6, 7}, opq = vdupq_n_u16(LV_OPAQUE);
  uint16x8_t any = vdupq_n_u16(0);
  for (u32 i = 0; i < 256; i += 8) {
    const uint16x8_t alpha = vcombine_u16(vmovn_u32(vshrq_n_u32(vld1q_u32(line3d + i), 24)), vmovn_u32(vshrq_n_u32(vld1q_u32(line3d + i + 4), 24)));
    const uint16x8_t m = vmvnq_u16(vceqq_u16(alpha, vdupq_n_u16(0)));
    any = vorrq_u16(any, m);
    vst1q_u16(v + i, vandq_u16(m, vorrq_u16(opq, vaddq_u16(lane, vdupq_n_u16(static_cast<u16>(i))))));
  }
  return vmaxvq_u16(any) != 0;
}

bool text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v) {
  uint8x8_t any = vdup_n_u8(0);
  for (u32 t = 0; t < n; ++t, packed += 4, v += 8) {
    u32 w; std::memcpy(&w, packed, 4);
    const uint8x8_t raw = vreinterpret_u8_u32(vdup_n_u32(w));
    const uint8x8x2_t nib = vzip_u8(vand_u8(raw, vdup_n_u8(0xF)), vshr_n_u8(raw, 4));
    uint8x8_t idx = nib.val[0];
    const uint8x8_t rev = vrev64_u8(idx);
    const uint8x8_t mask = vbsl_u8(vdup_n_u8((ctl[t] & 0x10) ? 0xFF : 0), vdup_n_u8(0xFF), vdup_n_u8(0x00));
    idx = vbsl_u8(mask, rev, idx);
    const uint16x8_t i16 = vmovl_u8(idx);
    const uint16x8_t m = vmvnq_u16(vceqq_u16(i16, vdupq_n_u16(0)));
    vst1q_u16(v, vandq_u16(m, vorrq_u16(i16, vdupq_n_u16(static_cast<u16>(LV_OPAQUE | ((ctl[t] & 0xF) << 4))))));
    any = vorr_u8(any, idx);
  }
  return vmaxv_u8(any) != 0;
}

bool text_row_256(const u8* rows, const u8* ctl, u32 n, bool ext, u16* v) {
  uint8x8_t any = vdup_n_u8(0);
  for (u32 t = 0; t < n; ++t, rows += 8, v += 8) {
    uint8x8_t idx = vld1_u8(rows);
    const uint8x8_t rev = vrev64_u8(idx);
    const uint8x8_t mask = vbsl_u8(vdup_n_u8((ctl[t] & 0x10) ? 0xFF : 0), vdup_n_u8(0xFF), vdup_n_u8(0x00));
    idx = vbsl_u8(mask, rev, idx);
    const uint16x8_t i16 = vmovl_u8(idx);
    const uint16x8_t m = vmvnq_u16(vceqq_u16(i16, vdupq_n_u16(0)));
    vst1q_u16(v, vandq_u16(m, vorrq_u16(i16, vdupq_n_u16(static_cast<u16>(LV_OPAQUE | (ext ? (ctl[t] & 0xF) << 8 : 0))))));
    any = vorr_u8(any, idx);
  }
  return vmaxv_u8(any) != 0;
}

namespace {
// 16 lanes of the window test for a BG (bit wbit) or OBJ (bit 4).
inline uint8x16_t win_mask(const u8* win, u8 wbit) { return vtstq_u8(vld1q_u8(win), vdupq_n_u8(wbit)); }
// u16 opaque bits of 16 values as a byte mask.
inline uint8x16_t opaque_mask(uint16x8_t a, uint16x8_t b) { return vtstq_u8(vcombine_u8(vshrn_n_u16(a, 8), vshrn_n_u16(b, 8)), vdupq_n_u8(0x80)); }
inline void split16(uint8x16_t m8, uint16x8_t& m0, uint16x8_t& m1) {
  const int8x16_t ws = vreinterpretq_s8_u8(m8);
  m0 = vreinterpretq_u16_s16(vmovl_s8(vget_low_s8(ws))); m1 = vreinterpretq_u16_s16(vmovl_s8(vget_high_s8(ws)));
}
// The OBJ table id per lane from the attribute byte.
inline uint8x16_t obj_tid16(uint8x16_t a) {
  const uint8x16_t bitmap = vtstq_u8(a, vdupq_n_u8(OA_BITMAP)), std = vtstq_u8(a, vdupq_n_u8(OA_STDPAL));
  return vbslq_u8(bitmap, vdupq_n_u8(T_OBJ_DIRECT), vbslq_u8(std, vdupq_n_u8(T_OBJ_STD), vdupq_n_u8(T_OBJ_EXT)));
}
inline void merge16(uint8x16_t m8, uint16x8_t v0, uint16x8_t v1, uint8x16_t tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  uint16x8_t m0, m1; split16(m8, m0, m1);
  const uint16x8_t t0 = vld1q_u16(top), t1 = vld1q_u16(top + 8);
  if (second) {
    vst1q_u16(second, vbslq_u16(m0, t0, vld1q_u16(second)));
    vst1q_u16(second + 8, vbslq_u16(m1, t1, vld1q_u16(second + 8)));
    vst1q_u8(second_tid, vbslq_u8(m8, vld1q_u8(top_tid), vld1q_u8(second_tid)));
  }
  vst1q_u16(top, vbslq_u16(m0, v0, t0));
  vst1q_u16(top + 8, vbslq_u16(m1, v1, t1));
  vst1q_u8(top_tid, vbslq_u8(m8, tid, vld1q_u8(top_tid)));
}
}

// The selects step 32 pixels and never test the mask: a `vmaxvq` + branch is
// a vector-to-scalar readback, which on an in-order core costs more than the
// merge it skips even on a line that is mostly transparent (measured on the
// A55: -17 % at every density, and -22 % with the wider step).
void select16_nowin(const u16* v, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 16)
    merge16(opaque_mask(vld1q_u16(v + i), vld1q_u16(v + i + 8)), vld1q_u16(v + i), vld1q_u16(v + i + 8), vtid,
            top + i, top_tid + i, second + i, second_tid + i);
}

void select16_obj_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    const uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, second + i, second_tid + i);
  }
}

void select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 32) {
    for (u32 j = i; j < i + 32; j += 16) {
      const uint16x8_t a = vld1q_u16(v + j), b = vld1q_u16(v + j + 8);
      const uint8x16_t m8 = vandq_u8(opaque_mask(a, b), win_mask(win + j, wbit));
      merge16(m8, a, b, vtid, top + j, top_tid + j, second + j, second_tid + j);
    }
  }
}

void select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, win_mask(win + i, 0x10));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, second + i, second_tid + i);
  }
}

void select16_flat_nowin(const u16* v, u8 tid, u16* top, u8* top_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 16)
    merge16(opaque_mask(vld1q_u16(v + i), vld1q_u16(v + i + 8)), vld1q_u16(v + i), vld1q_u16(v + i + 8), vtid,
            top + i, top_tid + i, nullptr, nullptr);
}

void select16_obj_flat_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    const uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, nullptr, nullptr);
  }
}

void select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 32) {
    for (u32 j = i; j < i + 32; j += 16) {
      const uint16x8_t a = vld1q_u16(v + j), b = vld1q_u16(v + j + 8);
      const uint8x16_t m8 = vandq_u8(opaque_mask(a, b), win_mask(win + j, wbit));
      merge16(m8, a, b, vtid, top + j, top_tid + j, nullptr, nullptr);
    }
  }
}

void select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, win_mask(win + i, 0x10));
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, nullptr, nullptr);
  }
}

void resolve16(const u16* top, const u8* top_tid, const Pixel* const* tables, Pixel* out) {
  // A gather; runs of one table are the common case, so the table pointer
  // is hoisted per 16 pixels when the ids agree.
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t t = vld1q_u8(top_tid + i);
    if (vmaxvq_u8(t) == vminvq_u8(t)) {
      const Pixel* tab = tables[top_tid[i]];
      for (u32 k = 0; k < 16; ++k) out[i + k] = tab[top[i + k] & 0x7FFF] | 0xFF000000;
    } else {
      for (u32 k = 0; k < 16; ++k) out[i + k] = tables[top_tid[i + k]][top[i + k] & 0x7FFF] | 0xFF000000;
    }
  }
}

// One layer through one table: a gather with nothing to dispatch on. Eight
// independent loads per iteration, which is what keeps an in-order core busy
// through the load latency; the address arithmetic vectorises around them.
void resolve16_one(const u16* v, const Pixel* table, Pixel* out) {
  for (u32 i = 0; i < 256; i += 8) {
    const uint16x8_t idx = vandq_u16(vld1q_u16(v + i), vdupq_n_u16(0x7FFF));
    alignas(16) u16 ix[8];
    vst1q_u16(ix, idx);
    const uint32x4_t alpha = vdupq_n_u32(0xFF000000);
    uint32x4_t lo = vdupq_n_u32(0), hi = vdupq_n_u32(0);
    lo = vsetq_lane_u32(table[ix[0]], lo, 0); lo = vsetq_lane_u32(table[ix[1]], lo, 1);
    lo = vsetq_lane_u32(table[ix[2]], lo, 2); lo = vsetq_lane_u32(table[ix[3]], lo, 3);
    hi = vsetq_lane_u32(table[ix[4]], hi, 0); hi = vsetq_lane_u32(table[ix[5]], hi, 1);
    hi = vsetq_lane_u32(table[ix[6]], hi, 2); hi = vsetq_lane_u32(table[ix[7]], hi, 3);
    vst1q_u32(out + i, vorrq_u32(lo, alpha));
    vst1q_u32(out + i + 4, vorrq_u32(hi, alpha));
  }
}

// Sixteen texels a step, then eight, then scalar: the runs start and end at
// wrap or overflow boundaries, so nothing may be written past n.
bool bmp_row_8(const u8* idx, u32 n, u16* v) {
  const uint16x8_t op = vdupq_n_u16(0x8000);
  uint8x16_t acc = vdupq_n_u8(0);
  u32 i = 0;
  auto eight = [&](uint8x8_t x) {
    const uint16x8_t w = vmovl_u8(x), nz = vandq_u16(vshll_n_u8(vtst_u8(x, x), 8), op);
    return vorrq_u16(w, nz);
  };
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t x = vld1q_u8(idx + i);
    acc = vorrq_u8(acc, x);
    vst1q_u16(v + i, eight(vget_low_u8(x)));
    vst1q_u16(v + i + 8, eight(vget_high_u8(x)));
  }
  if (i + 8 <= n) {
    const uint8x8_t x = vld1_u8(idx + i);
    acc = vorrq_u8(acc, vcombine_u8(x, x));
    vst1q_u16(v + i, eight(x));
    i += 8;
  }
  bool any = vmaxvq_u8(acc) != 0;
  for (; i < n; ++i) { const u8 x = idx[i]; v[i] = x ? static_cast<u16>(LV_OPAQUE | x) : 0; any |= x != 0; }
  return any;
}
bool bmp_row_16(const u16* col, u32 n, u16* v) {
  const uint16x8_t op = vdupq_n_u16(0x8000);
  uint16x8_t acc = vdupq_n_u16(0);
  u32 i = 0;
  for (; i + 8 <= n; i += 8) {
    const uint16x8_t c = vld1q_u16(col + i);
    const uint16x8_t o = vandq_u16(c, vtstq_u16(c, op));
    acc = vorrq_u16(acc, o);
    vst1q_u16(v + i, o);
  }
  bool any = vmaxvq_u16(acc) != 0;
  for (; i < n; ++i) { const u16 c = col[i]; v[i] = (c & 0x8000) ? c : 0; any |= (c & 0x8000) != 0; }
  return any;
}

void master_brightness(u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  if (mode != 1 && mode != 2) return;
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00), vf = vdupq_n_u32(factor), opaque = vdupq_n_u32(0xFF000000);
  for (u32 i = 0; i < 256; i += 4) {
    const uint32x4_t v = vld1q_u32(dst + i);
    const uint32x4_t rb = vandq_u32(v, mrb), g = vandq_u32(v, mg);
    uint32x4_t o;
    if (mode == 1) {
      const uint32x4_t drb = vandq_u32(vshrq_n_u32(vmulq_u32(vsubq_u32(mrb, rb), vf), 4), mrb);
      const uint32x4_t dg = vandq_u32(vshrq_n_u32(vmulq_u32(vsubq_u32(mg, g), vf), 4), mg);
      o = vorrq_u32(vaddq_u32(rb, drb), vaddq_u32(g, dg));
    } else {
      const uint32x4_t drb = vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(0xF * 0x010001), rb, vf), 4), mrb);
      const uint32x4_t dg = vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(0xF * 0x000100), g, vf), 4), mg);
      o = vorrq_u32(vsubq_u32(rb, drb), vsubq_u32(g, dg));
    }
    vst1q_u32(dst + i, vorrq_u32(o, opaque));
  }
}

namespace {
inline uint32x4_t expand4(uint32x4_t c) {
  uint32x4_t v = vshlq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F)), 18);
  v = vorrq_u32(v, vshlq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F00)), 2));
  v = vorrq_u32(v, vshrq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F0000)), 14));
  v = vorrq_u32(v, vshrq_n_u32(vandq_u32(v, vdupq_n_u32(0xC0C0C0)), 6));
  return vorrq_u32(v, vdupq_n_u32(0xFF000000));
}
}

void expand_colours(u32* dst) {
  for (u32 i = 0; i < 256; i += 4) vst1q_u32(dst + i, expand4(vld1q_u32(dst + i)));
}

// Byte planes: the 6-bit fields, the master brightness in 16-bit lanes, the
// 6 -> 8 expansion as (c << 2) | (c >> 4) per plane, and the planes stored in
// 0xAARRGGBB order (b g r 0xFF) by one st4 -- no channel is shifted through
// a 32-bit word.
void output_line(const Pixel* src, u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  const uint8x8_t vf = vdup_n_u8(static_cast<u8>(factor));
  const uint8x16_t v63 = vdupq_n_u8(63), vff = vdupq_n_u8(0xFF);
  const bool bright = mode == 1, dark = mode == 2;
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16x4_t p = vld4q_u8(reinterpret_cast<const u8*>(src + i));
    uint8x16_t c[3];
    for (u32 k = 0; k < 3; ++k) {
      c[k] = vandq_u8(p.val[k], v63);
      if (bright) c[k] = brighten16(c[k], vf, 0);
      else if (dark) c[k] = darken16(c[k], vf, 15);
      c[k] = vorrq_u8(vshlq_n_u8(c[k], 2), vshrq_n_u8(c[k], 4));
    }
    const uint8x16x4_t rec = {c[2], c[1], c[0], vff};
    vst4q_u8(reinterpret_cast<u8*>(dst + i), rec);
  }
}

// BGR555 in: each 5-bit field is narrowed to a byte plane already doubled
// (the 18-bit record's channel), then the same brightness and expansion.
void output_vram_line(const u16* src, u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  const uint8x8_t vf = vdup_n_u8(static_cast<u8>(factor));
  const uint8x16_t vff = vdupq_n_u8(0xFF);
  const uint16x8_t m3e = vdupq_n_u16(0x3E);
  const bool bright = mode == 1, dark = mode == 2;
  for (u32 i = 0; i < 256; i += 16) {
    const uint16x8_t lo = vld1q_u16(src + i), hi = vld1q_u16(src + i + 8);
    uint8x16_t c[3];
    c[0] = vcombine_u8(vmovn_u16(vandq_u16(vshlq_n_u16(lo, 1), m3e)), vmovn_u16(vandq_u16(vshlq_n_u16(hi, 1), m3e)));
    c[1] = vcombine_u8(vmovn_u16(vandq_u16(vshrq_n_u16(lo, 4), m3e)), vmovn_u16(vandq_u16(vshrq_n_u16(hi, 4), m3e)));
    c[2] = vcombine_u8(vmovn_u16(vandq_u16(vshrq_n_u16(lo, 9), m3e)), vmovn_u16(vandq_u16(vshrq_n_u16(hi, 9), m3e)));
    for (u32 k = 0; k < 3; ++k) {
      if (bright) c[k] = brighten16(c[k], vf, 0);
      else if (dark) c[k] = darken16(c[k], vf, 15);
      c[k] = vorrq_u8(vshlq_n_u8(c[k], 2), vshrq_n_u8(c[k], 4));
    }
    const uint8x16x4_t rec = {c[2], c[1], c[0], vff};
    vst4q_u8(reinterpret_cast<u8*>(dst + i), rec);
  }
}

// Runs are short (2-3 pixels at the scales the handhelds use, more on a wide
// panel), so this stores a quad when one fits and falls back to scalar for the
// tail rather than setting up a vector loop that rarely runs.
// Sixteen pixels a step in byte planes: one vld4 gives the record's four
// channels, the 5-bit channels are a shift and a mask, and the alpha bit
// lands in bit 15 by widening the 0/0xFF byte mask and shifting it 15 (only
// bit 0 survives the halfword).
void capture_a15(const Pixel* src, u32 n, u16* dst) {
  const uint8x16_t v1f = vdupq_n_u8(0x1F);
  for (u32 i = 0; i < n; i += 16) {
    const uint8x16x4_t p = vld4q_u8(reinterpret_cast<const u8*>(src + i));
    const uint8x16_t r = vandq_u8(vshrq_n_u8(p.val[0], 1), v1f);
    const uint8x16_t g = vandq_u8(vshrq_n_u8(p.val[1], 1), v1f);
    const uint8x16_t b = vandq_u8(vshrq_n_u8(p.val[2], 1), v1f);
    const uint8x16_t a = vtstq_u8(p.val[3], p.val[3]);
    auto half = [&](u32 o, uint8x8_t r8, uint8x8_t g8, uint8x8_t b8, uint8x8_t a8) {
      uint16x8_t w = vmovl_u8(r8);
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(g8), 5));
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(b8), 10));
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(a8), 15));
      vst1q_u16(dst + i + o, w);
    };
    half(0, vget_low_u8(r), vget_low_u8(g), vget_low_u8(b), vget_low_u8(a));
    half(8, vget_high_u8(r), vget_high_u8(g), vget_high_u8(b), vget_high_u8(a));
  }
}

// The blend in 16-bit lanes off 8-bit sources: each product is at most
// 31 * 16, both terms plus the rounding bias fit a halfword, so one
// multiply-long and one multiply-accumulate-long per channel per eight
// pixels do the arithmetic. The per-source alpha becomes a byte mask
// applied to the channels before the multiply (aa and ab are 0 or 1).
void capture_blend(const Pixel* srca, const u16* srcb, u32 n, u32 eva, u32 evb, u16* dst) {
  const uint8x16_t v1f = vdupq_n_u8(0x1F);
  const uint8x8_t va8 = vdup_n_u8(static_cast<u8>(eva)), vb8 = vdup_n_u8(static_cast<u8>(evb));
  const uint8x16_t ea = vdupq_n_u8(eva ? 0xFF : 0), eb = vdupq_n_u8(evb ? 0xFF : 0);
  const uint16x8_t bias = vdupq_n_u16(8), v31 = vdupq_n_u16(31), topbit = vdupq_n_u16(0x8000);
  for (u32 i = 0; i < n; i += 16) {
    const uint8x16x4_t p = vld4q_u8(reinterpret_cast<const u8*>(srca + i));
    const uint8x16_t aam = vtstq_u8(p.val[3], p.val[3]);
    const uint8x16_t ra = vandq_u8(vandq_u8(vshrq_n_u8(p.val[0], 1), v1f), aam);
    const uint8x16_t ga = vandq_u8(vandq_u8(vshrq_n_u8(p.val[1], 1), v1f), aam);
    const uint8x16_t ba = vandq_u8(vandq_u8(vshrq_n_u8(p.val[2], 1), v1f), aam);
    const uint16x8_t w0 = vld1q_u16(srcb + i), w1 = vld1q_u16(srcb + i + 8);
    const uint8x16_t abm = vcombine_u8(vmovn_u16(vtstq_u16(w0, topbit)), vmovn_u16(vtstq_u16(w1, topbit)));
    auto bnarrow = [&](uint16x8_t c0, uint16x8_t c1) {
      return vandq_u8(vandq_u8(vcombine_u8(vmovn_u16(c0), vmovn_u16(c1)), v1f), abm);
    };
    const uint8x16_t rb = bnarrow(w0, w1);
    const uint8x16_t gb = bnarrow(vshrq_n_u16(w0, 5), vshrq_n_u16(w1, 5));
    const uint8x16_t bb = bnarrow(vshrq_n_u16(w0, 10), vshrq_n_u16(w1, 10));
    const uint8x16_t ad = vorrq_u8(vandq_u8(aam, ea), vandq_u8(abm, eb));
    auto half = [&](u32 o, auto lane) {
      auto blend1 = [&](uint8x16_t ca, uint8x16_t cb) {
        uint16x8_t s = vmlal_u8(vmull_u8(lane(ca), va8), lane(cb), vb8);
        return vminq_u16(vshrq_n_u16(vaddq_u16(s, bias), 4), v31);
      };
      uint16x8_t w = blend1(ra, rb);
      w = vorrq_u16(w, vshlq_n_u16(blend1(ga, gb), 5));
      w = vorrq_u16(w, vshlq_n_u16(blend1(ba, bb), 10));
      w = vorrq_u16(w, vshlq_n_u16(vmovl_u8(lane(ad)), 15));
      vst1q_u16(dst + i + o, w);
    };
    half(0, [](uint8x16_t v) { return vget_low_u8(v); });
    half(8, [](uint8x16_t v) { return vget_high_u8(v); });
  }
}

void scale_row(const u32* src, const u16* xrun, u32* dst) {
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s];
    u32 x = xrun[s];
    const u32 end = xrun[s + 1];
    const uint32x4_t v = vdupq_n_u32(c);
    for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
    for (; x < end; ++x) dst[x] = c;
  }
}

void scale_row_straddle(const u32* src, const u32* seam, const u8* w, const u16* xrun, u32* dst) {
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s];
    u32 x = xrun[s];
    const u32 end = xrun[s + 1];
    const u32 n = end - x;
    if (!w[s] || n == 0) {
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
      for (; x < end; ++x) dst[x] = c;
      continue;
    }
    const u32 cs = seam[s];
    switch (n) {
    case 1: dst[x] = cs; break;
    case 2: vst1_u32(dst + x, uint32x2_t{c, cs}); break;
    case 3: vst1_u32(dst + x, vdup_n_u32(c)); dst[x + 2] = cs; break;
    default: {
      const u32 last = end - 1;
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= last; x += 4) vst1q_u32(dst + x, v);
      for (; x < last; ++x) dst[x] = c;
      dst[last] = cs;
    }
    }
  }
}

// (a * 256 + b * w - a * w + 128) >> 8 per byte, in 16-bit lanes: the
// intermediate wraps but the true value is at most 255 * 256 + 128, so the
// wrapped sum is exact. Each pixel's weight is spread over its four bytes.
void blend_line_w(const u32* a, const u32* b, const u8* w, u32* out) {
  const uint8x8_t k128 = vdup_n_u8(128);
  for (u32 i = 0; i < 256; i += 4) {
    const uint8x16_t va = vreinterpretq_u8_u32(vld1q_u32(a + i)), vb = vreinterpretq_u8_u32(vld1q_u32(b + i));
    const uint8x8_t w4 = vreinterpret_u8_u32(vdup_n_u32(*reinterpret_cast<const u32*>(w + i)));
    const uint8x8x2_t z1 = vzip_u8(w4, w4);              // w0 w0 w1 w1 w2 w2 w3 w3 ...
    const uint8x8x2_t z2 = vzip_u8(z1.val[0], z1.val[0]); // w0 w0 w0 w0 w1 w1 w1 w1 | w2 w2 w2 w2 w3 w3 w3 w3
    const uint8x8_t wlo = z2.val[0], whi = z2.val[1];
    uint16x8_t tlo = vshll_n_u8(vget_low_u8(va), 8), thi = vshll_n_u8(vget_high_u8(va), 8);
    tlo = vmlal_u8(tlo, vget_low_u8(vb), wlo);  thi = vmlal_u8(thi, vget_high_u8(vb), whi);
    tlo = vmlsl_u8(tlo, vget_low_u8(va), wlo);  thi = vmlsl_u8(thi, vget_high_u8(va), whi);
    tlo = vaddw_u8(tlo, k128);                  thi = vaddw_u8(thi, k128);
    vst1q_u32(out + i, vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(tlo, 8), vshrn_n_u16(thi, 8))));
  }
}

// The dimmed copies of the 256 source pixels are made first, four at a time
// (bytes widened to 16 bits, multiplied by f with alpha's lane at 256, and
// narrowed back), then the runs are filled as scale_row does. Every
// destination pixel is stored exactly once: the destination is usually a
// write-combined dmabuf, where a scalar store landing on top of a vector
// store's last lane costs a second bus write. Runs of two and three (every
// scale the handhelds use) are one or two stores each.
void scale_row_grid(const u32* src, const u16* xrun, u32 f, u32 min_run, bool seam_row, u32* dst) {
  if (min_run < 2) min_run = 2;
  alignas(16) u32 dimmed[256];
  // f == 0 (black seams: the "integer scale, leave the spare pixels dark"
  // look) needs no dimmed copies at all: every seam is the one constant.
  if (f == 0) {
    if (seam_row) {
      const u32 w = xrun[256];
      const uint32x4_t k = vdupq_n_u32(0xFF000000u);
      u32 x = 0;
      for (; x + 4 <= w; x += 4) vst1q_u32(dst + x, k);
      for (; x < w; ++x) dst[x] = 0xFF000000u;
      return;
    }
    for (u32& d : dimmed) d = 0xFF000000u;
  } else {
    const uint16x8_t fv = {static_cast<u16>(f), static_cast<u16>(f), static_cast<u16>(f), 256,
                           static_cast<u16>(f), static_cast<u16>(f), static_cast<u16>(f), 256};
    for (u32 s = 0; s < 256; s += 4) {
      const uint8x16_t c = vreinterpretq_u8_u32(vld1q_u32(src + s));
      const uint16x8_t lo = vmulq_u16(vmovl_u8(vget_low_u8(c)), fv), hi = vmulq_u16(vmovl_u8(vget_high_u8(c)), fv);
      vst1q_u32(dimmed + s, vreinterpretq_u32_u8(vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8))));
    }
    if (seam_row) { scale_row(dimmed, xrun, dst); return; }
  }
  for (u32 s = 0; s < 256; ++s) {
    const u32 c = src[s], cd = dimmed[s];
    u32 x = xrun[s];
    const u32 end = xrun[s + 1];
    const u32 n = end - x;
    if (n < min_run) {                                   // plain run, as scale_row
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
      for (; x < end; ++x) dst[x] = c;
      continue;
    }
    switch (n) {
    case 2: vst1_u32(dst + x, uint32x2_t{cd, c}); break;
    case 3: dst[x] = cd; vst1_u32(dst + x + 1, vdup_n_u32(c)); break;
    default: {
      dst[x++] = cd;
      const uint32x4_t v = vdupq_n_u32(c);
      for (; x + 4 <= end; x += 4) vst1q_u32(dst + x, v);
      for (; x < end; ++x) dst[x] = c;
    }
    }
  }
}


// ---- 3D span stages ------------------------------------------------------------
// Four pixels per step (the buffers are 256 wide, so rounding `n` up is safe).
// Divisions: a correctly rounded f64 quotient of two u32 values, truncated, is
// the exact integer quotient (the error is below num * 2^-53 < 1/den), so
// vdivq_f64 reproduces the reference's integer division with no fix-up.

namespace {
inline uint32x4_t udiv_exact(uint32x4_t num, uint32x4_t den) {
  const float64x2_t nlo = vcvtq_f64_u64(vmovl_u32(vget_low_u32(num))), nhi = vcvtq_f64_u64(vmovl_u32(vget_high_u32(num)));
  const float64x2_t dlo = vcvtq_f64_u64(vmovl_u32(vget_low_u32(den))), dhi = vcvtq_f64_u64(vmovl_u32(vget_high_u32(den)));
  const uint64x2_t qlo = vcvtq_u64_f64(vdivq_f64(nlo, dlo)), qhi = vcvtq_u64_f64(vdivq_f64(nhi, dhi));
  return vcombine_u32(vmovn_u64(qlo), vmovn_u64(qhi));
}
// (u64 lanes) / d, exact, for products below 2^53.
inline uint64x2_t udiv64_exact(uint64x2_t n, float64x2_t d) { return vcvtq_u64_f64(vdivq_f64(vcvtq_f64_u64(n), d)); }
inline int32x4_t mul_hi8_add(int32x4_t base, uint32x4_t d, uint32x4_t f) {   // base + ((d * f) >> 8), 64-bit product, low 32 bits kept
  const uint64x2_t lo = vshrq_n_u64(vmull_u32(vget_low_u32(d), vget_low_u32(f)), 8);
  const uint64x2_t hi = vshrq_n_u64(vmull_high_u32(d, f), 8);
  return vaddq_s32(base, vreinterpretq_s32_u32(vcombine_u32(vmovn_u64(lo), vmovn_u64(hi))));
}
const int32x4_t kLane = {0, 1, 2, 3};
}

void span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac) {
  // num and den are both linear in xv, so they step by a constant instead of
  // being multiplied out per pixel. The quotient is a blend factor that stays
  // inside 8 bits for real content, so one Newton step leaves the estimate
  // within a small fraction of 1 and the two correction rounds land it
  // exactly; lanes where that cannot be shown are collected in a vector and
  // the whole span redone exactly -- once per span, not a scalar readback
  // once per four pixels.
  const int32x4_t x0 = vaddq_s32(vdupq_n_s32(xv0), kLane);
  uint32x4_t num = vshlq_n_u32(vreinterpretq_u32_s32(vmulq_s32(x0, vdupq_n_s32(w0n))), 8);
  const uint32x4_t dnum = vdupq_n_u32(static_cast<u32>(w0n * 4) << 8);
  const uint32x4_t dden = vdupq_n_u32(static_cast<u32>(w0d * 4) - static_cast<u32>(w1d * 4));
  const uint32x4_t one = vdupq_n_u32(1), big = vdupq_n_u32(0x3FFFFF);
  uint32x4_t bad = vdupq_n_u32(0);
  uint32x4_t den;
  if (w0d == w1d) {
    // W does not vary across the span, so the denominator collapses to the
    // constant xdiff*w0d and the factor num/den is *linear in x*. DraStic's
    // render_polygon_setup_perspective_steps_w_constant_asm exploits that
    // fully: it multiplies an iota vector by the span's step once and the
    // inner loop is a single add, with no division anywhere. This is that.
    //
    // The step is held in 16.16, so the ramp accumulates a rounded step and
    // drifts from the exact quotient by up to a couple of units over a long
    // span. That is a deliberate departure from bit-exactness -- see
    // docs/techniques/02 and the measurements in the commit.
    const u32 dscalar = static_cast<u32>(xdiff) * static_cast<u32>(w0d);
    if (dscalar == 0) {
      const uint32x4_t z = vdupq_n_u32(0);
      for (u32 i = 0; i < n; i += 4) vst1q_u32(fac + i, z);
      return;
    }
    const u32 A = static_cast<u32>(w0n) << 8;
    const u32 step = static_cast<u32>((static_cast<u64>(A) << 16) / dscalar);
    uint32x4_t acc = vmulq_u32(vreinterpretq_u32_s32(vaddq_s32(vdupq_n_s32(xv0), kLane)), vdupq_n_u32(step));
    const uint32x4_t inc = vdupq_n_u32(step * 4);
    for (u32 i = 0; i < n; i += 4) {
      vst1q_u32(fac + i, vshrq_n_u32(acc, 16));
      acc = vaddq_u32(acc, inc);
    }
    return;
  } else {
    den = vreinterpretq_u32_s32(vaddq_s32(vmulq_s32(x0, vdupq_n_s32(w0d)),
                                          vmulq_s32(vsubq_s32(vdupq_n_s32(xdiff), x0), vdupq_n_s32(w1d))));
    for (u32 i = 0; i < n; i += 4) {
      const uint32x4_t zero = vceqzq_u32(den);
      const uint32x4_t d = vorrq_u32(den, vandq_u32(zero, one));
      const float32x4_t fd = vcvtq_f32_u32(d);
      float32x4_t rcp = vrecpeq_f32(fd);
      rcp = vmulq_f32(rcp, vrecpsq_f32(fd, rcp));                       // ~16 bits, enough for an 8-bit quotient
      uint32x4_t q = vcvtq_u32_f32(vmulq_f32(vcvtq_f32_u32(num), rcp));
      const uint32x4_t unsafe = vorrq_u32(vcgeq_u32(q, big), vcltq_u32(vaddq_u32(num, d), num));
      uint32x4_t r = vsubq_u32(num, vmulq_u32(q, d));                   // wraps negative when q is one too many
      const uint32x4_t over = vcgtq_u32(r, num);
      q = vaddq_u32(q, over);                                           // -1 on those lanes
      r = vaddq_u32(r, vandq_u32(over, d));
      const uint32x4_t under = vcgeq_u32(r, d);
      q = vsubq_u32(q, under);                                          // +1 on those lanes
      r = vsubq_u32(r, vandq_u32(under, d));
      bad = vorrq_u32(bad, vorrq_u32(unsafe, vorrq_u32(vcgeq_u32(r, d), vcgtq_u32(r, num))));
      vst1q_u32(fac + i, vbicq_u32(q, zero));
      num = vaddq_u32(num, dnum);
      den = vaddq_u32(den, dden);
    }
  }
  if (vmaxvq_u32(bad) == 0) return;
  // Anything the fast path could not prove exact: redo the span by division.
  num = vshlq_n_u32(vreinterpretq_u32_s32(vmulq_s32(x0, vdupq_n_s32(w0n))), 8);
  den = vreinterpretq_u32_s32(vaddq_s32(vmulq_s32(x0, vdupq_n_s32(w0d)),
                                        vmulq_s32(vsubq_s32(vdupq_n_s32(xdiff), x0), vdupq_n_s32(w1d))));
  for (u32 i = 0; i < n; i += 4) {
    const uint32x4_t zero = vceqzq_u32(den);
    const uint32x4_t d = vorrq_u32(den, vandq_u32(zero, one));
    vst1q_u32(fac + i, vbicq_u32(udiv_exact(num, d), zero));
    num = vaddq_u32(num, dnum);
    den = vaddq_u32(den, dden);
  }
}

void span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out) {
  if (y0 == y1) { const int32x4_t v = vdupq_n_s32(y0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = y0 < y1;
  const int32x4_t base = vdupq_n_s32(up ? y0 : y1);
  const uint32x4_t d = vdupq_n_u32(static_cast<u32>(up ? y1 - y0 : y0 - y1));
  const uint32x4_t k256 = vdupq_n_u32(256);
  for (u32 i = 0; i < n; i += 4) {
    uint32x4_t f = vld1q_u32(fac + i);
    if (!up) f = vsubq_u32(k256, f);
    vst1q_s32(out + i, mul_hi8_add(base, d, f));
  }
}

// The five attributes in one pass over the span: `fac` is loaded once per
// four pixels instead of once per attribute, and the endpoint constants for
// all five stay in registers.
void span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  int32x4_t base[5]; uint32x4_t d[5]; bool up[5], flat[5];
  for (int k = 0; k < 5; ++k) {
    flat[k] = y0[k] == y1[k];
    up[k] = y0[k] < y1[k];
    base[k] = vdupq_n_s32(flat[k] ? y0[k] : (up[k] ? y0[k] : y1[k]));
    d[k] = vdupq_n_u32(static_cast<u32>(up[k] ? y1[k] - y0[k] : y0[k] - y1[k]));
  }
  for (u32 i = 0; i < n; i += 4) {
    const uint32x4_t f = vld1q_u32(fac + i);
    const uint32x4_t fi = vsubq_u32(k256, f);
    for (int k = 0; k < 5; ++k) {
      if (flat[k]) vst1q_s32(out[k] + i, base[k]);
      else vst1q_s32(out[k] + i, mul_hi8_add(base[k], d[k], up[k] ? f : fi));
    }
  }
}

// The same interpolation, narrowed as it is stored: colour to the 6-bit
// channel the shader reads and texture coordinates to the s16 the sampler
// truncates to. Eight pixels a step, so the stores are whole vectors and the
// span buffers are a third of the size.
// s and t only: three of the five attribute chains, and three of the five
// stores per eight pixels, go with the colour.
void span_attrs2n(const s32* y0, const s32* y1, const u32* fac, u32 n, s16* sc, s16* tc) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  int32x4_t base[2]; uint32x4_t d[2]; bool up[2], flat[2];
  for (int k = 0; k < 2; ++k) {
    const s32 a = y0[k + 3], b = y1[k + 3];
    flat[k] = a == b;
    up[k] = a < b;
    base[k] = vdupq_n_s32(flat[k] ? a : (up[k] ? a : b));
    d[k] = vdupq_n_u32(static_cast<u32>(up[k] ? b - a : a - b));
  }
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const uint32x4_t f0 = vld1q_u32(fac + i), f1 = vld1q_u32(fac + i + 4);
    const uint32x4_t i0 = vsubq_u32(k256, f0), i1 = vsubq_u32(k256, f1);
    for (int k = 0; k < 2; ++k) {
      int32x4_t v0, v1;
      if (flat[k]) { v0 = base[k]; v1 = base[k]; }
      else { v0 = mul_hi8_add(base[k], d[k], up[k] ? f0 : i0); v1 = mul_hi8_add(base[k], d[k], up[k] ? f1 : i1); }
      vst1q_s16(tout[k] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                vmovn_u32(vreinterpretq_u32_s32(v1)))));
    }
  }
}

void span_attrs5n(const s32* y0, const s32* y1, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc) {
  const uint32x4_t k256 = vdupq_n_u32(256);
  int32x4_t base[5]; uint32x4_t d[5]; bool up[5], flat[5];
  for (int k = 0; k < 5; ++k) {
    flat[k] = y0[k] == y1[k];
    up[k] = y0[k] < y1[k];
    base[k] = vdupq_n_s32(flat[k] ? y0[k] : (up[k] ? y0[k] : y1[k]));
    d[k] = vdupq_n_u32(static_cast<u32>(up[k] ? y1[k] - y0[k] : y0[k] - y1[k]));
  }
  u8* const cout[3] = {vr, vg, vb};
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const uint32x4_t f0 = vld1q_u32(fac + i), f1 = vld1q_u32(fac + i + 4);
    const uint32x4_t i0 = vsubq_u32(k256, f0), i1 = vsubq_u32(k256, f1);
    for (int k = 0; k < 5; ++k) {
      int32x4_t v0, v1;
      if (flat[k]) { v0 = base[k]; v1 = base[k]; }
      else { v0 = mul_hi8_add(base[k], d[k], up[k] ? f0 : i0); v1 = mul_hi8_add(base[k], d[k], up[k] ? f1 : i1); }
      if (k < 3) {   // (v >> 3) & 0xFF: the two narrowing moves mask it
        const uint16x8_t w = vcombine_u16(vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v0), 3)),
                                          vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v1), 3)));
        vst1_u8(cout[k] + i, vmovn_u16(w));
      } else {       // (s16)v
        vst1q_s16(tout[k - 3] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                     vmovn_u32(vreinterpretq_u32_s32(v1)))));
      }
    }
  }
}

// Linear interpolation of one attribute over four pixels, the vector form of
// span_attr_linear's body: q = d * xv / xdiff by the reciprocal, with the
// same one-compare fix-up, and xv mirrored when the endpoints descend.
struct LinAttr {
  int32x4_t base;
  uint32x4_t d;
  bool flat, up;
  void set(s32 y0, s32 y1) {
    flat = y0 == y1;
    up = y0 < y1;
    base = vdupq_n_s32(flat ? y0 : (up ? y0 : y1));
    d = vdupq_n_u32(static_cast<u32>(up ? y1 - y0 : y0 - y1));
  }
  [[gnu::always_inline]] int32x4_t at(int32x4_t xv, int32x4_t vxdiff, uint32x4_t vm, uint32x4_t vxd, bool has_m) const {
    if (flat) return base;
    const int32x4_t x = up ? xv : vsubq_s32(vxdiff, xv);
    const uint32x4_t num = vmulq_u32(d, vreinterpretq_u32_s32(x));
    uint32x4_t q = num;
    if (has_m) {
      q = vcombine_u32(vshrn_n_u64(vmull_u32(vget_low_u32(num), vget_low_u32(vm)), 32), vshrn_n_u64(vmull_high_u32(num, vm), 32));
      q = vaddq_u32(q, vcgtq_u32(vmulq_u32(q, vxd), num));   // all-ones lane = -1
    }
    return vaddq_s32(base, vreinterpretq_s32_u32(q));
  }
};

// The reciprocal span_attr_linear derives per call. Hoisted here because the
// fused kernels need it once for all five attributes, not once each: it is a
// 64-bit division, and five of them per span was the cost this kernel exists
// to remove.
static inline u32 lin_recip(s32 xdiff) {
  return xdiff >= 2 ? static_cast<u32>(((1ull << 32) + static_cast<u32>(xdiff) - 1) / static_cast<u32>(xdiff)) : 0;
}

void span_attrs5n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc) {
  LinAttr a[5];
  for (int k = 0; k < 5; ++k) a[k].set(y0[k], y1[k]);
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  const u32 m = lin_recip(xdiff);
  const uint32x4_t vm = vdupq_n_u32(m), vxd = vreinterpretq_u32_s32(vxdiff);
  u8* const cout[3] = {vr, vg, vb};
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const int32x4_t xv0v = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    const int32x4_t xv1v = vaddq_s32(xv0v, vdupq_n_s32(4));
    for (int k = 0; k < 5; ++k) {
      int32x4_t v0, v1;
      if (m) { v0 = a[k].at(xv0v, vxdiff, vm, vxd, true);  v1 = a[k].at(xv1v, vxdiff, vm, vxd, true); }
      else   { v0 = a[k].at(xv0v, vxdiff, vm, vxd, false); v1 = a[k].at(xv1v, vxdiff, vm, vxd, false); }
      if (k < 3) {   // (v >> 3) & 0xFF: the two narrowing moves mask it
        const uint16x8_t w = vcombine_u16(vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v0), 3)),
                                          vmovn_u32(vshrq_n_u32(vreinterpretq_u32_s32(v1), 3)));
        vst1_u8(cout[k] + i, vmovn_u16(w));
      } else {       // (s16)v
        vst1q_s16(tout[k - 3] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                     vmovn_u32(vreinterpretq_u32_s32(v1)))));
      }
    }
  }
}

void span_attrs2n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, s16* sc, s16* tc) {
  LinAttr a[2];
  for (int k = 0; k < 2; ++k) a[k].set(y0[k + 3], y1[k + 3]);
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  const u32 m = lin_recip(xdiff);
  const uint32x4_t vm = vdupq_n_u32(m), vxd = vreinterpretq_u32_s32(vxdiff);
  s16* const tout[2] = {sc, tc};
  for (u32 i = 0; i < n; i += 8) {
    const int32x4_t xv0v = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    const int32x4_t xv1v = vaddq_s32(xv0v, vdupq_n_s32(4));
    for (int k = 0; k < 2; ++k) {
      int32x4_t v0, v1;
      if (m) { v0 = a[k].at(xv0v, vxdiff, vm, vxd, true);  v1 = a[k].at(xv1v, vxdiff, vm, vxd, true); }
      else   { v0 = a[k].at(xv0v, vxdiff, vm, vxd, false); v1 = a[k].at(xv1v, vxdiff, vm, vxd, false); }
      vst1q_s16(tout[k] + i, vreinterpretq_s16_u16(vcombine_u16(vmovn_u32(vreinterpretq_u32_s32(v0)),
                                                                vmovn_u32(vreinterpretq_u32_s32(v1)))));
    }
  }
}

void span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out) {
  if (y0 == y1) { const int32x4_t v = vdupq_n_s32(y0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = y0 < y1;
  const int32x4_t base = vdupq_n_s32(up ? y0 : y1);
  const uint32x4_t d = vdupq_n_u32(static_cast<u32>(up ? y1 - y0 : y0 - y1));
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  // d * f / xdiff with the product below 2^32: q = (n * ceil(2^32 / xdiff)) >> 32
  // is exact or one too many; the compare q * xdiff > n fixes it. xdiff == 1
  // (reciprocal would not fit) divides by nothing.
  const u32 m = xdiff >= 2 ? static_cast<u32>(((1ull << 32) + static_cast<u32>(xdiff) - 1) / static_cast<u32>(xdiff)) : 0;
  const uint32x4_t vm = vdupq_n_u32(m), vxd = vreinterpretq_u32_s32(vxdiff);
  for (u32 i = 0; i < n; i += 4) {
    int32x4_t xv = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    if (!up) xv = vsubq_s32(vxdiff, xv);
    const uint32x4_t num = vmulq_u32(d, vreinterpretq_u32_s32(xv));
    uint32x4_t q = num;
    if (m) {
      q = vcombine_u32(vshrn_n_u64(vmull_u32(vget_low_u32(num), vget_low_u32(vm)), 32), vshrn_n_u64(vmull_high_u32(num, vm), 32));
      q = vaddq_u32(q, vcgtq_u32(vmulq_u32(q, vxd), num));   // all-ones lane = -1
    }
    vst1q_s32(out + i, vaddq_s32(base, vreinterpretq_s32_u32(q)));
  }
}

void span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out) {
  if (z0 == z1) { const int32x4_t v = vdupq_n_s32(z0); for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v); return; }
  const bool up = z0 < z1;
  const int32x4_t base = vdupq_n_s32(up ? z0 : z1);
  const uint32x4_t disp = vdupq_n_u32(static_cast<u32>((up ? z1 - z0 : z0 - z1) >> 9));
  const uint32x4_t vrecip = vdupq_n_u32(static_cast<u32>(xrecip));
  const int32x4_t vxdiff = vdupq_n_s32(xdiff);
  for (u32 i = 0; i < n; i += 4) {
    int32x4_t xv = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    if (!up) xv = vsubq_s32(vxdiff, xv);
    const uint32x4_t df = vmulq_u32(disp, vreinterpretq_u32_s32(xv));          // disp * factor < 2^24
    const uint64x2_t lo = vshrq_n_u64(vmull_u32(vget_low_u32(df), vget_low_u32(vrecip)), 13);
    const uint64x2_t hi = vshrq_n_u64(vmull_high_u32(df, vrecip), 13);
    vst1q_s32(out + i, vaddq_s32(base, vreinterpretq_s32_u32(vcombine_u32(vmovn_u64(lo), vmovn_u64(hi)))));
  }
}

void span_z_const(s32 z, u32 n, s32* out) {
  const int32x4_t v = vdupq_n_s32(z);
  for (u32 i = 0; i < n; i += 4) vst1q_s32(out + i, v);
}

void clear_image_run(const u16* col, const u16* dep, u32 n, u32 polyid, u32* color, u32* depth, u32* attr) {
  // Sixteen pixels a step as byte planes (r g b a, then st4), the 5-bit
  // channel to 6 bits as c * 2 + (c != 0) in 16-bit lanes; the depth and
  // attribute words widened from the depth row. Exactly n are written, so
  // the tail is scalar.
  const uint16x8_t m5 = vdupq_n_u16(0x1F), one16 = vdupq_n_u16(1), bit15 = vdupq_n_u16(0x8000);
  const uint32x4_t vpid = vdupq_n_u32(polyid), v1ff = vdupq_n_u32(0x1FF);
  auto c6 = [&](uint16x8_t c5) { return vaddq_u16(vshlq_n_u16(c5, 1), vandq_u16(vtstq_u16(c5, c5), one16)); };
  auto plane = [&](uint16x8_t lo, uint16x8_t hi) { return vcombine_u8(vmovn_u16(lo), vmovn_u16(hi)); };
  u32 i = 0;
  for (; i + 16 <= n; i += 16) {
    const uint16x8_t c0 = vld1q_u16(col + i), c1 = vld1q_u16(col + i + 8);
    uint8x16x4_t o;
    o.val[0] = plane(c6(vandq_u16(c0, m5)), c6(vandq_u16(c1, m5)));
    o.val[1] = plane(c6(vandq_u16(vshrq_n_u16(c0, 5), m5)), c6(vandq_u16(vshrq_n_u16(c1, 5), m5)));
    o.val[2] = plane(c6(vandq_u16(vshrq_n_u16(c0, 10), m5)), c6(vandq_u16(vshrq_n_u16(c1, 10), m5)));
    o.val[3] = plane(vandq_u16(vtstq_u16(c0, bit15), m5), vandq_u16(vtstq_u16(c1, bit15), m5));
    vst4q_u8(reinterpret_cast<u8*>(color + i), o);
    for (u32 k = 0; k < 2; ++k) {
      const uint16x8_t d = vld1q_u16(dep + i + k * 8);
      const uint16x8_t dz = vbicq_u16(d, bit15), da = vandq_u16(d, bit15);
      vst1q_u32(depth + i + k * 8, vaddq_u32(vshll_n_u16(vget_low_u16(dz), 9), v1ff));
      vst1q_u32(depth + i + k * 8 + 4, vaddq_u32(vshll_high_n_u16(dz, 9), v1ff));
      vst1q_u32(attr + i + k * 8, vorrq_u32(vmovl_u16(vget_low_u16(da)), vpid));
      vst1q_u32(attr + i + k * 8 + 4, vorrq_u32(vmovl_high_u16(da), vpid));
    }
  }
  for (; i < n; ++i) {
    const u32 c = col[i], d = dep[i];
    auto s6 = [](u32 c5) { return c5 ? c5 * 2 + 1 : 0; };
    color[i] = s6(c & 0x1F) | (s6((c >> 5) & 0x1F) << 8) | (s6((c >> 10) & 0x1F) << 16) | ((c & 0x8000) ? 0x1F000000u : 0);
    depth[i] = ((d & 0x7FFF) * 0x200) + 0x1FF;
    attr[i] = polyid | (d & 0x8000);
  }
}

// One instantiation per depth mode: the test is decided once per span, not
// once per four pixels inside the loop. `under` names the lower layer as a
// candidate where the top pixel carries edge flags; off, only bit 0 is set.
template <int mode, bool under>
static u32 depth_candidates_m(const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass) {
  const uint32x4_t one = vdupq_n_u32(1), two = vdupq_n_u32(2);
  uint32x4_t any = vdupq_n_u32(0);
  for (u32 i = 0; i < n; i += 4) {
    const int32x4_t zv = vld1q_s32(z + i), d = vreinterpretq_s32_u32(vld1q_u32(dstz + i));
    const uint32x4_t a = vld1q_u32(dstattr + i);
    uint32x4_t ok;
    if constexpr (mode == 0) ok = vcltq_s32(zv, d);
    else if constexpr (mode == 1) {
      const uint32x4_t back = vceqq_u32(vandq_u32(a, vdupq_n_u32(0x00400010)), vdupq_n_u32(0x10));
      ok = vbslq_u32(back, vcleq_s32(zv, d), vcltq_s32(zv, d));
    } else if constexpr (mode == 2) ok = vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0x200))), vdupq_n_u32(0x400));
    else ok = vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0xFF))), vdupq_n_u32(0x1FE));
    uint32x4_t v;
    if constexpr (under) v = vbslq_u32(ok, one, vandq_u32(vtstq_u32(a, vdupq_n_u32(0xF)), two));
    else { v = vandq_u32(ok, one); (void)two; }
    const uint16x4_t v16 = vmovn_u32(v);
    const uint8x8_t v8 = vmovn_u16(vcombine_u16(v16, v16));
    vst1_lane_u32(reinterpret_cast<u32*>(pass + i), vreinterpret_u32_u8(v8), 0);
    any = vorrq_u32(any, v);
  }
  if (vmaxvq_u32(any) == 0) return 0;
  u32 first = 0; while (!pass[first]) ++first;
  u32 last = n; while (last && !pass[last - 1]) --last;
  if (!last) return 0;   // the only hits were in the rounded-up tail
  return (first << 16) | last;
}

u32 depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass, bool under) {
  if (under) {
    switch (mode) {
    case 0:  return depth_candidates_m<0, true>(z, dstz, dstattr, n, pass);
    case 1:  return depth_candidates_m<1, true>(z, dstz, dstattr, n, pass);
    case 2:  return depth_candidates_m<2, true>(z, dstz, dstattr, n, pass);
    default: return depth_candidates_m<3, true>(z, dstz, dstattr, n, pass);
    }
  }
  switch (mode) {
  case 0:  return depth_candidates_m<0, false>(z, dstz, dstattr, n, pass);
  case 1:  return depth_candidates_m<1, false>(z, dstz, dstattr, n, pass);
  case 2:  return depth_candidates_m<2, false>(z, dstz, dstattr, n, pass);
  default: return depth_candidates_m<3, false>(z, dstz, dstattr, n, pass);
  }
}

} // namespace ds::gpu::kern::neon

#endif // DSPERATE_NEON
