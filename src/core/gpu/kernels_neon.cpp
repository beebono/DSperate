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
  const uint32x4_t veva = vdupq_n_u32(eva), vevb = vdupq_n_u32(evb), colour_mask = vdupq_n_u32(0x00FFFFFF), opaque = vdupq_n_u32(0xFF000000);
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
    // Most blocks of most lines blend nothing: copy through.
    if (vmaxvq_u8(vorrq_u8(vorrq_u8(objblend, blend3d), fx)) == 0) {
      for (u32 k = 0; k < 4; ++k) vst1q_u32(out + i + k * 4, vorrq_u32(vandq_u32(vld1q_u32(top + i + k * 4), colour_mask), opaque));
      continue;
    }
    const bool has_obj = vmaxvq_u8(objblend) != 0, has_3d = vmaxvq_u8(blend3d) != 0, has_fx = vmaxvq_u8(fx) != 0;
    const Mask4 m_obj = widen(objblend), m_3d = widen(blend3d), m_fx = widen(fx), m_bitmap = widen(is_bitmap), m_t2 = widen(t2hit);
    const Mask4 alpha = widen(vld1q_u8(top_alpha + i));
    for (u32 k = 0; k < 4; ++k) {
      const uint32x4_t a = vld1q_u32(top + i + k * 4), b = vld1q_u32(second + i + k * 4);
      uint32x4_t o = a;
      if (has_fx) {
        uint32x4_t o_fx = a;
        if (effect == 1) o_fx = vbslq_u32(m_t2.m[k], blend4(a, b, veva, vevb, 8, 4), a);
        else if (effect == 2) o_fx = brighten4(a, evy, 0x8);
        else if (effect == 3) o_fx = darken4(a, evy, 0x7);
        o = vbslq_u32(m_fx.m[k], o_fx, a);
      }
      if (has_3d) {
        // 3D blend with (alpha + 1) of 32; alpha 31 passes the top record through.
        const uint32x4_t a3 = vaddq_u32(vandq_u32(vshrq_n_u32(a, 24), vdupq_n_u32(0x1F)), vdupq_n_u32(1));
        uint32x4_t o_3d = blend4(a, b, a3, vsubq_u32(vdupq_n_u32(32), a3), 16, 5);
        o_3d = vbslq_u32(vceqq_u32(a3, vdupq_n_u32(32)), a, o_3d);
        o = vbslq_u32(m_3d.m[k], o_3d, o);
      }
      if (has_obj) {
        // OBJ blend: bitmap sprites use their own alpha as EVA, 16 - EVA as EVB.
        const uint32x4_t ea = vbslq_u32(m_bitmap.m[k], alpha.m[k], veva);
        const uint32x4_t eb = vbslq_u32(m_bitmap.m[k], vsubq_u32(vdupq_n_u32(16), ea), vevb);
        o = vbslq_u32(m_obj.m[k], blend4(a, b, ea, eb, 8, 4), o);
      }
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
    for (u32 k = 0; k < 16; ++k) {
      const u8 t = top_tid[i + k];
      top_px[i + k] = (line3d && t == 0) ? line3d[i + k] : (tables[t][top[i + k] & 0x7FFF] | 0xFF000000);
      second_px[i + k] = tables[second_tid[i + k]][second[i + k] & 0x7FFF] | 0xFF000000;
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

void layer16_3d(const u32* line3d, u16* v) {
  const uint16x8_t lane = {0, 1, 2, 3, 4, 5, 6, 7}, opq = vdupq_n_u16(LV_OPAQUE);
  for (u32 i = 0; i < 256; i += 8) {
    const uint16x8_t alpha = vcombine_u16(vmovn_u32(vshrq_n_u32(vld1q_u32(line3d + i), 24)), vmovn_u32(vshrq_n_u32(vld1q_u32(line3d + i + 4), 24)));
    const uint16x8_t m = vmvnq_u16(vceqq_u16(alpha, vdupq_n_u16(0)));
    vst1q_u16(v + i, vandq_u16(m, vorrq_u16(opq, vaddq_u16(lane, vdupq_n_u16(static_cast<u16>(i))))));
  }
}

bool text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v) {
  uint8x8_t any = vdup_n_u8(0);
  for (u32 t = 0; t < n; ++t, packed += 4, v += 8) {
    u32 w; std::memcpy(&w, packed, 4);
    const uint8x8_t raw = vreinterpret_u8_u32(vdup_n_u32(w));
    const uint8x8x2_t nib = vzip_u8(vand_u8(raw, vdup_n_u8(0xF)), vshr_n_u8(raw, 4));
    uint8x8_t idx = nib.val[0];
    if (ctl[t] & 0x10) idx = vrev64_u8(idx);
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
    if (ctl[t] & 0x10) idx = vrev64_u8(idx);
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

void select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 16) {
    const uint16x8_t a = vld1q_u16(v + i), b = vld1q_u16(v + i + 8);
    const uint8x16_t m8 = vandq_u8(opaque_mask(a, b), win_mask(win + i, wbit));
    if (vmaxvq_u8(m8) == 0) continue;
    merge16(m8, a, b, vtid, top + i, top_tid + i, second + i, second_tid + i);
  }
}

void select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, win_mask(win + i, 0x10));
    if (vmaxvq_u8(m8) == 0) continue;
    merge16(m8, vld1q_u16(v + i), vld1q_u16(v + i + 8), obj_tid16(a), top + i, top_tid + i, second + i, second_tid + i);
  }
}

void select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid) {
  const uint8x16_t vtid = vdupq_n_u8(tid);
  for (u32 i = 0; i < 256; i += 16) {
    const uint16x8_t a = vld1q_u16(v + i), b = vld1q_u16(v + i + 8);
    const uint8x16_t m8 = vandq_u8(opaque_mask(a, b), win_mask(win + i, wbit));
    if (vmaxvq_u8(m8) == 0) continue;
    merge16(m8, a, b, vtid, top + i, top_tid + i, nullptr, nullptr);
  }
}

void select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid) {
  const uint8x16_t vprio = vdupq_n_u8(static_cast<u8>(prio));
  for (u32 i = 0; i < 256; i += 16) {
    const uint8x16_t a = vld1q_u8(attr + i);
    uint8x16_t m8 = vandq_u8(vtstq_u8(a, vdupq_n_u8(OA_OPAQUE)), vceqq_u8(vandq_u8(a, vdupq_n_u8(OA_PRIO)), vprio));
    m8 = vandq_u8(m8, win_mask(win + i, 0x10));
    if (vmaxvq_u8(m8) == 0) continue;
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

void output_line(const Pixel* src, u16 reg, u32* dst) {
  const u32 mode = reg >> 14;
  u32 factor = reg & 0x1F;
  if (factor > 16) factor = 16;
  if (mode != 1 && mode != 2) { for (u32 i = 0; i < 256; i += 4) vst1q_u32(dst + i, expand4(vld1q_u32(src + i))); return; }
  const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x003F00), vf = vdupq_n_u32(factor);
  for (u32 i = 0; i < 256; i += 4) {
    const uint32x4_t v = vld1q_u32(src + i);
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
    vst1q_u32(dst + i, expand4(o));
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
  const int32x4_t vw0n = vdupq_n_s32(w0n), vw0d = vdupq_n_s32(w0d), vw1d = vdupq_n_s32(w1d), vxdiff = vdupq_n_s32(xdiff);
  for (u32 i = 0; i < n; i += 4) {
    const int32x4_t xv = vaddq_s32(vdupq_n_s32(xv0 + static_cast<s32>(i)), kLane);
    const uint32x4_t num = vshlq_n_u32(vreinterpretq_u32_s32(vmulq_s32(xv, vw0n)), 8);
    const uint32x4_t den = vreinterpretq_u32_s32(vaddq_s32(vmulq_s32(xv, vw0d), vmulq_s32(vsubq_s32(vxdiff, xv), vw1d)));
    const uint32x4_t zero = vceqzq_u32(den);
    // Lanes with den == 0 divide by 1 and are masked to 0 afterwards.
    const uint32x4_t d = vorrq_u32(den, vandq_u32(zero, vdupq_n_u32(1)));
    // f32 quotient, then the remainder decides: one step up or down covers
    // the f32 error for quotients below 2^22; the f64 path takes the rest.
    // Valid when num + d does not wrap (q*d <= num + d then cannot either)
    // and the quotient is below 2^22 (the estimate is then within one).
    const float32x4_t fd = vcvtq_f32_u32(d);
    float32x4_t rcp = vrecpeq_f32(fd);
    rcp = vmulq_f32(rcp, vrecpsq_f32(fd, rcp));
    rcp = vmulq_f32(rcp, vrecpsq_f32(fd, rcp));                         // ~23 bits after two Newton steps
    uint32x4_t q = vcvtq_u32_f32(vmulq_f32(vcvtq_f32_u32(num), rcp));
    const uint32x4_t unsafe = vorrq_u32(vcgeq_u32(q, vdupq_n_u32(0x3FFFFF)), vcltq_u32(vaddq_u32(num, d), num));
    uint32x4_t r = vsubq_u32(num, vmulq_u32(q, d));                    // wraps negative when q is one too many
    const uint32x4_t over = vcgtq_u32(r, num);                           // r "negative"
    q = vaddq_u32(q, over);                                              // -1 on those lanes
    r = vaddq_u32(r, vandq_u32(over, d));
    const uint32x4_t under = vcgeq_u32(r, d);
    q = vsubq_u32(q, under);                                             // +1 on those lanes
    r = vsubq_u32(r, vandq_u32(under, d));
    if (vmaxvq_u32(vorrq_u32(unsafe, vorrq_u32(vcgeq_u32(r, d), vcgtq_u32(r, num)))) != 0) q = udiv_exact(num, d);
    vst1q_u32(fac + i, vbicq_u32(q, zero));
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

u32 depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass) {
  const uint32x4_t one = vdupq_n_u32(1), two = vdupq_n_u32(2);
  uint32x4_t any = vdupq_n_u32(0);
  for (u32 i = 0; i < n; i += 4) {
    const int32x4_t zv = vld1q_s32(z + i), d = vreinterpretq_s32_u32(vld1q_u32(dstz + i));
    const uint32x4_t a = vld1q_u32(dstattr + i);
    uint32x4_t ok;
    if (mode == 0) ok = vcltq_s32(zv, d);
    else if (mode == 1) {
      const uint32x4_t back = vceqq_u32(vandq_u32(a, vdupq_n_u32(0x00400010)), vdupq_n_u32(0x10));
      ok = vbslq_u32(back, vcleq_s32(zv, d), vcltq_s32(zv, d));
    } else if (mode == 2) ok = vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0x200))), vdupq_n_u32(0x400));
    else ok = vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(d, zv), vdupq_n_s32(0xFF))), vdupq_n_u32(0x1FE));
    const uint32x4_t edge = vtstq_u32(a, vdupq_n_u32(0xF));
    const uint32x4_t v = vbslq_u32(ok, one, vandq_u32(edge, two));
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

} // namespace ds::gpu::kern::neon

#endif // DSPERATE_NEON
