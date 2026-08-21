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

// Blend of two records with per-lane weights, 4 pixels.
inline uint32x4_t blend4(uint32x4_t a, uint32x4_t b, uint32x4_t ea, uint32x4_t eb, u32 round_r, u32 shift) {
  const uint32x4_t mr = vdupq_n_u32(0x3F), mg = vdupq_n_u32(0x3F00), mb = vdupq_n_u32(0x3F0000);
  uint32x4_t r = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r), vandq_u32(a, mr), ea), vandq_u32(b, mr), eb);
  uint32x4_t g = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r << 8), vandq_u32(a, mg), ea), vandq_u32(b, mg), eb);
  uint32x4_t bl = vmlaq_u32(vmlaq_u32(vdupq_n_u32(round_r << 16), vandq_u32(a, mb), ea), vandq_u32(b, mb), eb);
  r = vshlq_u32(r, vdupq_n_s32(-static_cast<s32>(shift)));
  g = vandq_u32(vshlq_u32(g, vdupq_n_s32(-static_cast<s32>(shift))), vdupq_n_u32(0x7F00));
  bl = vandq_u32(vshlq_u32(bl, vdupq_n_s32(-static_cast<s32>(shift))), vdupq_n_u32(0x7F0000));
  r = vminq_u32(r, mr); g = vminq_u32(g, mg); bl = vminq_u32(bl, mb);
  return vorrq_u32(vorrq_u32(vorrq_u32(r, g), bl), vdupq_n_u32(0xFF000000));
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

} // namespace

void select_plane(const Pixel* px, const u8* op, const u8* win, u8 wbit, u8 id, bool is3d,
                  Pixel* top, Pixel* second, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  const uint8x16_t vwbit = vdupq_n_u8(wbit), vid = vdupq_n_u8(id), vkind = vdupq_n_u8(is3d ? K_3D : K_NORMAL), zero = vdupq_n_u8(0);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t m8 = vandq_u8(vmvnq_u8(vceqq_u8(vld1q_u8(op + i), zero)), vtstq_u8(vld1q_u8(win + i), vwbit));
    const Mask4 m = widen(m8);
    uint32x4_t a24[4];
    for (u32 k = 0; k < 4; ++k) {
      const uint32x4_t t = vld1q_u32(top + i + k * 4), p = vld1q_u32(px + i + k * 4), s = vld1q_u32(second + i + k * 4);
      vst1q_u32(second + i + k * 4, vbslq_u32(m.m[k], t, s));
      vst1q_u32(top + i + k * 4, vbslq_u32(m.m[k], p, t));
      a24[k] = vandq_u32(vshrq_n_u32(p, 24), vdupq_n_u32(0x1F));
    }
    const uint8x16_t tid = vld1q_u8(top_id + i);
    vst1q_u8(second_id + i, vbslq_u8(m8, tid, vld1q_u8(second_id + i)));
    vst1q_u8(top_id + i, vbslq_u8(m8, vid, tid));
    vst1q_u8(top_kind + i, vbslq_u8(m8, vkind, vld1q_u8(top_kind + i)));
    if (is3d) vst1q_u8(top_alpha + i, vbslq_u8(m8, narrow(a24[0], a24[1], a24[2], a24[3]), vld1q_u8(top_alpha + i)));
  }
}

void select_obj(const Pixel* col, const u8* attr, const u8* alpha, const u8* win, u32 prio,
                Pixel* top, Pixel* second, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio)), vid = vdupq_n_u8(L_OBJ);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vtstq_u8(a, vdupq_n_u8(OA_OPAQUE));
    m8 = vandq_u8(m8, vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, vtstq_u8(vld1q_u8(win + i), vdupq_n_u8(0x10)));
    const Mask4 m = widen(m8);
    for (u32 k = 0; k < 4; ++k) {
      const uint32x4_t t = vld1q_u32(top + i + k * 4), c = vld1q_u32(col + i + k * 4), s = vld1q_u32(second + i + k * 4);
      vst1q_u32(second + i + k * 4, vbslq_u32(m.m[k], t, s));
      vst1q_u32(top + i + k * 4, vbslq_u32(m.m[k], c, t));
    }
    const uint8x16_t tid = vld1q_u8(top_id + i);
    vst1q_u8(second_id + i, vbslq_u8(m8, tid, vld1q_u8(second_id + i)));
    vst1q_u8(top_id + i, vbslq_u8(m8, vid, tid));
    // kind: bitmap -> 2, else semi -> 1, else 0.
    uint8x16_t kind = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_SEMI)), vdupq_n_u8(K_OBJ_SEMI));
    kind = vbslq_u8(vtstq_u8(a, vdupq_n_u8(OA_BITMAP)), vdupq_n_u8(K_OBJ_BITMAP), kind);
    vst1q_u8(top_kind + i, vbslq_u8(m8, kind, vld1q_u8(top_kind + i)));
    vst1q_u8(top_alpha + i, vbslq_u8(m8, vld1q_u8(alpha + i), vld1q_u8(top_alpha + i)));
  }
}

void composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,
                    const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,
                    const u8* win, Pixel* out) {
  const u32 effect = (bldcnt >> 6) & 3;
  const uint8x16_t v_t1 = vdupq_n_u8(static_cast<u8>(bldcnt)), v_t2 = vdupq_n_u8(static_cast<u8>(bldcnt >> 8));
  const uint32x4_t veva = vdupq_n_u32(eva), vevb = vdupq_n_u32(evb), colour_mask = vdupq_n_u32(0x00FFFFFF), opaque = vdupq_n_u32(0xFF000000);
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t kind = vld1q_u8(top_kind + i);
    const uint8x16_t t2hit = vtstq_u8(vld1q_u8(second_id + i), v_t2);
    const uint8x16_t t1hit = vandq_u8(vtstq_u8(vld1q_u8(top_id + i), v_t1), vtstq_u8(vld1q_u8(win + i), vdupq_n_u8(0x20)));
    const uint8x16_t is_bitmap = vceqq_u8(kind, vdupq_n_u8(K_OBJ_BITMAP));
    const uint8x16_t objblend = vandq_u8(vorrq_u8(vceqq_u8(kind, vdupq_n_u8(K_OBJ_SEMI)), is_bitmap), t2hit);
    const uint8x16_t blend3d = vandq_u8(vceqq_u8(kind, vdupq_n_u8(K_3D)), t2hit);
    // Effect applies where neither override hit and the first target matches.
    const uint8x16_t fx = vbicq_u8(vbicq_u8(t1hit, objblend), blend3d);
    const Mask4 m_obj = widen(objblend), m_3d = widen(blend3d), m_fx = widen(fx), m_bitmap = widen(is_bitmap), m_t2 = widen(t2hit);
    const Mask4 alpha = widen(vld1q_u8(top_alpha + i));
    for (u32 k = 0; k < 4; ++k) {
      const uint32x4_t a = vld1q_u32(top + i + k * 4), b = vld1q_u32(second + i + k * 4);
      // OBJ blend: bitmap sprites use their own alpha as EVA, 16 - EVA as EVB.
      const uint32x4_t ea = vbslq_u32(m_bitmap.m[k], alpha.m[k], veva);
      const uint32x4_t eb = vbslq_u32(m_bitmap.m[k], vsubq_u32(vdupq_n_u32(16), ea), vevb);
      const uint32x4_t o_obj = blend4(a, b, ea, eb, 8, 4);
      // 3D blend with (alpha + 1) of 32; alpha 31 passes the top record through.
      const uint32x4_t a3 = vaddq_u32(vandq_u32(vshrq_n_u32(a, 24), vdupq_n_u32(0x1F)), vdupq_n_u32(1));
      uint32x4_t o_3d = blend4(a, b, a3, vsubq_u32(vdupq_n_u32(32), a3), 16, 5);
      o_3d = vbslq_u32(vceqq_u32(a3, vdupq_n_u32(32)), a, o_3d);
      uint32x4_t o_fx = a;
      if (effect == 1) o_fx = vbslq_u32(m_t2.m[k], blend4(a, b, veva, vevb, 8, 4), a);
      else if (effect == 2) o_fx = brighten4(a, evy, 0x8);
      else if (effect == 3) o_fx = darken4(a, evy, 0x7);
      uint32x4_t o = vbslq_u32(m_fx.m[k], o_fx, a);
      o = vbslq_u32(m_3d.m[k], o_3d, o);
      o = vbslq_u32(m_obj.m[k], o_obj, o);
      vst1q_u32(out + i + k * 4, vorrq_u32(vandq_u32(o, colour_mask), opaque));
    }
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
void tile_row_pal16(const u8* idx, const Pixel* pal18, Pixel* px, u8* op) {
  const uint8x8_t raw = vld1_u8(idx);
  const uint8x8_t i4 = vshl_n_u8(vand_u8(raw, vdup_n_u8(0xF)), 2);
  const uint8x8x2_t z1 = vzip_u8(i4, i4);                       // each index twice
  const uint8x16_t twice = vcombine_u8(z1.val[0], z1.val[1]);
  const uint8x16x2_t z2 = vzipq_u8(twice, twice);               // four times
  const uint8x16_t step = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
  const uint8x16x4_t table = {{vld1q_u8(reinterpret_cast<const u8*>(pal18)), vld1q_u8(reinterpret_cast<const u8*>(pal18) + 16),
                               vld1q_u8(reinterpret_cast<const u8*>(pal18) + 32), vld1q_u8(reinterpret_cast<const u8*>(pal18) + 48)}};
  vst1q_u8(reinterpret_cast<u8*>(px), vqtbl4q_u8(table, vaddq_u8(z2.val[0], step)));
  vst1q_u8(reinterpret_cast<u8*>(px) + 16, vqtbl4q_u8(table, vaddq_u8(z2.val[1], step)));
  vst1_u8(op, vand_u8(vmvn_u8(vceq_u8(raw, vdup_n_u8(0))), vdup_n_u8(1)));
}

void layer_3d(const u32* line3d, Pixel* px, u8* op) {
  for (u32 i = 0; i < 256; i += 16) {
    uint32x4_t a[4];
    for (u32 k = 0; k < 4; ++k) { a[k] = vld1q_u32(line3d + i + k * 4); vst1q_u32(px + i + k * 4, a[k]); }
    const uint8x16_t alpha = narrow(vshrq_n_u32(a[0], 24), vshrq_n_u32(a[1], 24), vshrq_n_u32(a[2], 24), vshrq_n_u32(a[3], 24));
    vst1q_u8(op + i, vandq_u8(vmvnq_u8(vceqq_u8(alpha, vdupq_n_u8(0))), vdupq_n_u8(1)));
  }
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

void expand_colours(u32* dst) {
  for (u32 i = 0; i < 256; i += 4) {
    const uint32x4_t c = vld1q_u32(dst + i);
    uint32x4_t v = vshlq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F)), 18);
    v = vorrq_u32(v, vshlq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F00)), 2));
    v = vorrq_u32(v, vshrq_n_u32(vandq_u32(c, vdupq_n_u32(0x3F0000)), 14));
    v = vorrq_u32(v, vshrq_n_u32(vandq_u32(v, vdupq_n_u32(0xC0C0C0)), 6));
    vst1q_u32(dst + i, vorrq_u32(v, vdupq_n_u32(0xFF000000)));
  }
}

} // namespace ds::gpu::kern::neon

#endif // DSPERATE_NEON
