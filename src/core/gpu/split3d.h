// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/kernels.h"
#include "core/types.h"

// The 3D split: engine A's composite with the 3D layer (BG0) left out, and
// beside it what a later stage needs to put a 3D pixel in. A compositor that
// has the 3D image at another resolution (the GPU present) merges there; the
// CPU merge below is the reference, and with the 1x record it reproduces the
// one-pass composite exactly.
//
// Per pixel the 2D side hands over a colour and one word:
//   U  the finished line without BG0, colour effects applied (Engine2D's
//      output). It shows wherever the 3D pixel has alpha 0 or BG0 is hidden.
//   the word: a second colour C and two weights, the colour-effect rule
//      already worked out for "a 3D pixel T here", which is always
//          min((T * ka + C * kb + round) >> shift, 63)   per 6-bit channel.
// That one form covers every case, because BG0 sits either directly above
// the top pixel R of the stack without it, or below it:
//   T alone             ka 16, kb 0 (PLAIN when master brightness is off too:
//                       the pixel shows as it is, which a merge can shortcut)
//   T faded up / down   C white / black, ka 16 - EVY, kb EVY / 0
//   T blended over R    C = R, by T's own alpha: ka alpha + 1, kb 31 - alpha,
//                       of 32 (ALPHA3D; the weights come from T, not the word)
//   R blended over T    C = R, ka EVB, kb EVA (or a bitmap sprite's alpha)
//   R not blending      C = R through its own fade, ka 0, kb 16. Not U: with
//                       T in between, R no longer blends with what U put under it.
namespace ds::gpu::split3d {

enum Op : u32 { HIDDEN = 0, FIXED = 1, ALPHA3D = 2, PLAIN = 3 };

// The word: bits 0-17 C as r | g << 6 | b << 12; 18-22 ka; 23-27 kb; 28-29 Op.
constexpr u32 KA_SHIFT = 18, KB_SHIFT = 23, OP_SHIFT = 28;
// A captured image shown again (display capture, then the bank as a bitmap
// BG, bitmap sprites or VRAM display): the pixel's word is the one it had in
// the frame that captured it, T the 3D layer of that frame, and
//   Q15  the result loses each channel's low bit, as the 15-bit capture did
//        (so at 1x the merge gives the very pixel the VRAM holds);
//   XY   T is not at this pixel: a second word holds where in the captured
//        frame (x | y << 8).
constexpr u32 Q15 = 1u << 30, XY = 1u << 31;
inline u32 op_of(u32 w) { return (w >> OP_SHIFT) & 3; }

inline u32 pack(Pixel c, Op op, u32 ka, u32 kb) {
  return (c & 0x3F) | ((c >> 2) & 0xFC0) | ((c >> 4) & 0x3F000) | (ka << KA_SHIFT) | (kb << KB_SHIFT) | (op << OP_SHIFT);
}

// One pixel: `u` and `w` from the split, `p3d` the 3D layer's pixel there
// (RGB666, 5-bit alpha in bits 24-28). Red and blue go through the formula
// together, 16 bits apart (a sum is below 4096, so neither spills), and the
// whole is branch-free: display capture merges every line of a frame with it.
inline Pixel merge_pixel(u32 w, Pixel u, Pixel p3d) {
  const u32 op = (w >> OP_SHIFT) & 3;
  const u32 a3 = (p3d >> 24) & 0x1F;
  const u32 by_alpha = op == ALPHA3D;
  const u32 ka = by_alpha ? a3 + 1 : (w >> KA_SHIFT) & 0x1F, kb = by_alpha ? 31 - a3 : (w >> KB_SHIFT) & 0x1F;
  const u32 round = 8u << by_alpha, shift = 4 + by_alpha;
  const u32 t_rb = p3d & 0x003F003F, t_g = (p3d >> 8) & 0x3F;
  const u32 c_rb = (w & 0x3F) | ((w & 0x3F000) << 4), c_g = (w >> 6) & 0x3F;
  const u32 rb = t_rb * ka + c_rb * kb + round * 0x00010001u;
  u32 r = (rb & 0xFFFF) >> shift, g = (t_g * ka + c_g * kb + round) >> shift, b = rb >> (16 + shift);
  r = r > 63 ? 63 : r; g = g > 63 ? 63 : g; b = b > 63 ? 63 : b;
  Pixel o = 0xFF000000 | r | (g << 8) | (b << 16);
  if (w & Q15) o &= ~0x00010101u;
  return (op == HIDDEN) | (a3 == 0) ? u : o;
}

// A whole line: kern::active::split_merge_line (the NEON kernel on ARM).

} // namespace ds::gpu::split3d
