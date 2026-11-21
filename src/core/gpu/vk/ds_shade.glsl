// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_SHADE_GLSL
#define DS_SHADE_GLSL
// Texture sampling, colour combine, alpha blend, shared by the resolve pass
// and raster's ordered tail. Includer must declare Texels/Post and `pc` first.

// DS_MP: the precision of the colour path (B2 of GPU-RENDER-PLAN). mediump
// (RelaxedPrecision in SPIR-V) runs it in 16 bits on Mali, exact for DS
// colour maths (below 2^12): 4-8% off the G52's 4x frame. The GLES raster
// keeps highp (gl_common.glsl): the RG35XX SP's driver gained nothing from it.
// DS_TOON: whether the program shades mode 2 (toon / highlight). A constant:
// the Vulkan stages define it from a specialisation constant, GLES by #define.
#ifndef DS_TOON
#define DS_TOON true
#endif
#ifndef DS_MP
#define DS_MP mediump
#endif

// ---- texture ---------------------------------------------------------------

// Texel: 16-bit RGB555 in low half, 5-bit alpha above.
// The texel coordinate's wrap, clamp and flip run at DS_MP (16-bit on
// Mali: texels are at most 1024 apart, so a 12.4 coordinate >> 4 fits);
// the address into the arena stays highp.
#ifdef DS_WRAP_VARY
// The wrap from the vertex stage's words (lean.vert poly_out): the clamp is already in the
// polygon's texcoord bounds, a repeat is the mask and a mirrored repeat the mask over two periods folded by the
// min. On the G52 the per-axis branches below were 8% of a 4x frame.
uint sample_tex(GpuPoly p, int s12, int t12) {
  ivec2 st = (ivec2(s12, t12) >> 4) & DS_WRAP_VARY.xy;
  st = min(st, DS_WRAP_VARY.zw - st);
  highp int a = int(p.tex_offset) + st.y * int(p.tex_w) + st.x;
#ifdef DS_TEXEL_BUFFER
  return texelFetch(texels_tb, a).r;
#else
  return texels[uint(a)];   // the GLES raster's arena is a storage buffer
#endif
}
#else
uint sample_tex(GpuPoly p, int s12, int t12) {
  DS_MP int w = int(p.tex_w), h = int(p.tex_h);
  DS_MP int s = s12 >> 4, t = t12 >> 4;
  bool rep_s = (p.texparam & 0x10000u) != 0u, rep_t = (p.texparam & 0x20000u) != 0u;
  bool flp_s = (p.texparam & 0x40000u) != 0u, flp_t = (p.texparam & 0x80000u) != 0u;
  if (rep_s) { if (flp_s && (s & w) != 0) s = (w - 1) - (s & (w - 1)); else s &= w - 1; }
  else       s = clamp(s, 0, w - 1);
  if (rep_t) { if (flp_t && (t & h) != 0) t = (h - 1) - (t & (h - 1)); else t &= h - 1; }
  else       t = clamp(t, 0, h - 1);
  // highp copies first: a GLSL ES constructor keeps its argument's
  // precision, and t * w reaches 2^20.
  highp int hs = s, ht = t, hw = w;
#ifdef DS_TEXEL_BUFFER
  return texelFetch(texels_tb, int(p.tex_offset) + ht * hw + hs).r;
#else
  return texels[p.tex_offset + uint(ht * hw + hs)];
#endif
}

#endif

// 15-bit channel -> 6 bits: bits 1-5 of the field, +1 when non-zero.
DS_MP uint c15_to_18(DS_MP uint c, uint shift) {
  DS_MP uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu);
  return v != 0u ? v + 1u : v;
}

// ---- the pixel -------------------------------------------------------------

// The pixel's 6-bit r, g, b and 5-bit alpha. All of it at mediump: DS
// colour maths stays below 2^12, exact in 16 bits, which Bifrost runs two
// to a lane (B2 of GPU-RENDER-PLAN).
// shade_texel takes the texel word (sample_tex's) and shade_alpha makes the alpha alone from it, for a stage
// that tests the alpha before it pays for the colour (lean.frag: a discarded pixel stops there).
DS_MP uint shade_alpha(DS_MP uint blendmode, DS_MP uint polyalpha, bool textured, uint texw) {
  if (!textured || (blendmode & 1u) != 0u) return polyalpha;
  return ((((texw >> 16) & 0x1Fu) + 1u) * (polyalpha + 1u) - 1u) >> 5;
}
DS_MP uvec4 shade_texel(DS_MP uint blendmode, DS_MP uint polyalpha, bool textured,
                          DS_MP int vr9, DS_MP int vg9, DS_MP int vb9, uint texw) {
  DS_MP uint vr = uint(vr9 >> 3) & 0x3Fu, vg = uint(vg9 >> 3) & 0x3Fu, vb = uint(vb9 >> 3) & 0x3Fu;
  DS_MP uint r, g, b, a;
  // Mode 2 = toon or highlight (DISP3DCNT bit 1). Toon replaces vertex colour
  // via the 32-entry table; highlight greys to its own red and ADDS the table
  // entry after texture combine (below); `vr` stays untouched for both lookups.
  // DS_TOON false (a frame without mode-2 polygons) compiles this out:
  // untaken, it still cost the Malis 3-13% of the frame (B4 of GPU-RENDER-PLAN).
  if (!DS_TOON) blendmode &= 1u;
  bool highlight = DS_TOON && (pc.f.dispcnt & 2u) != 0u;
  if (DS_TOON && blendmode == 2u) {
    if (highlight) { vg = vr; vb = vr; }
    else {
      DS_MP uint tc = ps.toon[vr >> 1];
      vr = c15_to_18(tc, 0u); vg = c15_to_18(tc, 4u); vb = c15_to_18(tc, 9u);
    }
  }
  if (textured) {
    DS_MP uint tcol = texw & 0xFFFFu, ta = (texw >> 16) & 0x1Fu;
    DS_MP uint tr = c15_to_18(tcol, 0u), tg = c15_to_18(tcol, 4u), tb = c15_to_18(tcol, 9u);
    if ((blendmode & 1u) != 0u) {          // decal
      if (ta == 0u)       { r = vr; g = vg; b = vb; }
      else if (ta == 31u) { r = tr; g = tg; b = tb; }
      else {
        r = ((tr * ta) + (vr * (31u - ta))) >> 5;
        g = ((tg * ta) + (vg * (31u - ta))) >> 5;
        b = ((tb * ta) + (vb * (31u - ta))) >> 5;
      }
      a = polyalpha;
    } else {                                // modulate
      r = ((tr + 1u) * (vr + 1u) - 1u) >> 6;
      g = ((tg + 1u) * (vg + 1u) - 1u) >> 6;
      b = ((tb + 1u) * (vb + 1u) - 1u) >> 6;
      a = ((ta + 1u) * (polyalpha + 1u) - 1u) >> 5;
    }
  } else {
    r = vr; g = vg; b = vb; a = polyalpha;
  }
  if (DS_TOON && blendmode == 2u && highlight) {
    DS_MP uint tc = ps.toon[vr >> 1];
    r = min(r + c15_to_18(tc, 0u), 63u);
    g = min(g + c15_to_18(tc, 4u), 63u);
    b = min(b + c15_to_18(tc, 9u), 63u);
  }
  return uvec4(r, g, b, a);
}
DS_MP uvec4 shade_pixel(GpuPoly p, DS_MP uint blendmode, DS_MP uint polyalpha, bool textured,
                          DS_MP int vr9, DS_MP int vg9, DS_MP int vb9, int s, int t) {
  uint texw = 0u;
  if (textured) texw = sample_tex(p, s, t);
  return shade_texel(blendmode, polyalpha, textured, vr9, vg9, vb9, texw);
}

uint alpha_blend(uint dispcnt, uint src, uint dst, uint alpha) {
  uint dsta = dst >> 24;
  if (dsta == 0u) return src;
  uint r = src & 0x3Fu, g = (src >> 8) & 0x3Fu, b = (src >> 16) & 0x3Fu;
  if ((dispcnt & 8u) != 0u) {
    uint a1 = alpha + 1u;
    r = ((r * a1) + ((dst & 0x3Fu) * (32u - a1))) >> 5;
    g = ((g * a1) + (((dst >> 8) & 0x3Fu) * (32u - a1))) >> 5;
    b = ((b * a1) + (((dst >> 16) & 0x3Fu) * (32u - a1))) >> 5;
  }
  if (alpha > dsta) dsta = alpha;
  return r | (g << 8) | (b << 16) | (dsta << 24);
}


#endif // DS_SHADE_GLSL
