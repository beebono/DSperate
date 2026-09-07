// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/engine2d.h"

// Line-stage kernels of the 2D pipeline and the output stage, as free
// functions over plane pointers: every stage is a straight pass over
// the line with no data-dependent control flow, so each has a portable
// C++ reference (kernels_ref.cpp) and, on AArch64, a NEON twin
// (kernels_neon.cpp) with the same name and signature. `kern::active` is the
// one the renderer calls; tests/kernels_test.cpp diffs the two vector by
// vector, and the AArch64 frame dumps are compared with the host's.
//
// All pixel arrays are 256 entries and 16-byte aligned; `n` counts in the
// palette kernel are multiples of 8.

namespace ds::gpu::kern {

#define DS_KERNEL_LIST(NS)                                                                                   \
  /* Priority select of one layer line (u16, bit 15 opaque) into the top/second values and table ids. */    \
  void NS##select16(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  /* The OBJ line at one priority level; the table id comes from the attribute byte (bitmap / standard / extended). */ \
  void NS##select16_obj(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  /* The same without the second record, for lines no colour effect can touch. */                           \
  void NS##select16_flat(const u16* v, const u8* win, u8 wbit, u8 tid, u16* top, u8* top_tid);              \
  void NS##select16_obj_flat(const u16* v, const u8* attr, const u8* win, u32 prio, u16* top, u8* top_tid);  \
  /* Window-free variants: the common no-window line skips loading and testing the window plane. */ \
  void NS##select16_nowin(const u16* v, u8 tid, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  void NS##select16_obj_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid, u16* second, u8* second_tid); \
  void NS##select16_flat_nowin(const u16* v, u8 tid, u16* top, u8* top_tid); \
  void NS##select16_obj_flat_nowin(const u16* v, const u8* attr, u32 prio, u16* top, u8* top_tid);  \
  /* Winning values through their tables (index = value & 0x7FFF) to 18-bit records with alpha 0xFF. */     \
  void NS##resolve16(const u16* top, const u8* top_tid, const Pixel* const* tables, Pixel* out);             \
  /* The same for a line that is one layer through one table (the single-opaque-layer fast path). */         \
  void NS##resolve16_one(const u16* v, const Pixel* table, Pixel* out);                                      \
  /* Top and second values to the composite's records: colours through the tables (a 3D pixel keeps its own \
     word when line3d is given), layer ids as BLDCNT masks, kind (3D, semi / bitmap OBJ from attr) and alpha. */ \
  void NS##resolve16_full(const u16* top, const u8* top_tid, const u16* second, const u8* second_tid,        \
                          const Pixel* const* tables, const u8* attr, const u8* alpha, const Pixel* line3d,  \
                          Pixel* top_px, Pixel* second_px, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id); \
  /* Top records only, for a line whose composite can never read the second target (a fade over plain      \
     layers): colours through the tables, layer ids as BLDCNT masks. No second gather, no kind, no alpha. */ \
  void NS##resolve16_top(const u16* top, const u8* top_tid, const Pixel* const* tables, const Pixel* line3d, \
                         Pixel* top_px, u8* top_id);                                                        \
  /* Any 3D pixel with alpha strictly between 0 and 31 on the line (alpha in bits 24-28). */                 \
  bool NS##line_has_translucent_3d(const Pixel* line3d);                                                    \
  /* Colour effects for a fade-only line: brighten / darken the first target inside the effect window.      \
     Reachable only where no blend path is live, so there is no second target, kind or alpha to consult. */  \
  void NS##composite_line_fade(u32 bldcnt, u32 evy, const Pixel* top, const u8* top_id, const u8* win, Pixel* out); \
  /* Colour effects: blend / brighten / darken with the OBJ and 3D override rules. */                         \
  void NS##composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,      \
                          const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,     \
                          const u8* win, Pixel* out);                                                        \
  /* BGR555 palette entries -> 18-bit records (bit 15 = low green bit). */                                   \
  void NS##palette_to_18(const u16* pal, Pixel* out, u32 n);                                                 \
  /* OBJ line plot of one sprite row, n pixels at the line pointers (which may be read and written up to   \
     15 entries past n; callers pad): an opaque pixel wins over a transparent one or a lower priority, a      \
     transparent pixel stamps priority and mosaic on a transparent one. Paletted: idx != 0 is opaque, value   \
     0x8000 | pal_base | idx, alpha 0. Bitmap: col bit 15 is opaque, value col. */                            \
  void NS##obj_row_idx16(const u8* idx, u32 n, u16 pal_base, u8 attr, u16* v, u8* oattr, u8* oalpha);        \
  void NS##obj_row_bmp16(const u16* col, u32 n, u8 attr, u8 alpha, u16* v, u8* oattr, u8* oalpha);           \
  /* 3D layer as a layer line: 0x8000 | x where the alpha is non-zero, 0 elsewhere. */                       \
  bool NS##layer16_3d(const u32* line3d, u16* v);                                                           \
  /* Text BG row, 16-colour tiles: n tiles of 4 packed bytes (low nibble first); ctl[t] = palette number      \
     (bits 0-3) | 0x10 for a horizontally flipped tile. Writes 8*n values 0x8000 | pal << 4 | idx (0 for       \
     index 0); returns whether any is opaque. */                                                             \
  bool NS##text_row_16(const u8* packed, const u8* ctl, u32 n, u16* v);                                      \
  /* Text BG row, 256-colour tiles: n tiles of 8 indices; values 0x8000 | (ext ? pal << 8 : 0) | idx. */     \
  bool NS##text_row_256(const u8* rows, const u8* ctl, u32 n, bool ext, u16* v);                             \
  /* A bitmap BG row read left to right (a rotscale layer whose matrix is the identity within the line):   \
     n texels, any n, nothing written past n. 8-bit: 0x8000 | idx (0 for index 0); direct colour: the        \
     BGR555 word where bit 15 is set, else 0. Returns whether any is opaque. */                              \
  bool NS##bmp_row_8(const u8* idx, u32 n, u16* v);                                                          \
  bool NS##bmp_row_16(const u16* col, u32 n, u16* v);                                                        \
  /* Output stage: master brightness on 18-bit records, then 6->8 bit expansion to 0xAARRGGBB. */            \
  void NS##master_brightness(u16 reg, u32* dst);                                                             \
  void NS##expand_colours(u32* dst);                                                                         \
  /* Both in one pass from the engine's composite line. */                                                   \
  void NS##output_line(const Pixel* src, u16 reg, u32* dst);                                                 \
  /* The same from a BGR555 line (VRAM or FIFO display: bit 15 ignored, no low green bit). */                 \
  void NS##output_vram_line(const u16* src, u16 reg, u32* dst);                                               \
  /* Display capture, source A only: 18-bit records (alpha in bits 24-31, non-zero = opaque) packed to      \
     BGR555 with bit 15 = the alpha bit; n is the capture width, a multiple of 16. */                        \
  void NS##capture_a15(const Pixel* src, u32 n, u16* dst);                                                   \
  /* Capture blend: A as above, B a BGR555 line with its alpha in bit 15. Per pixel and channel             \
     ((ca*aa*eva) + (cb*ab*evb) + 8) >> 4 clamped to 31; the alpha bit is (eva ? aa : 0) | (evb ? ab : 0).  \
     eva and evb are 0..16. A missing B source is a zeroed line. */                                          \
  void NS##capture_blend(const Pixel* srca, const u16* srcb, u32 n, u32 eva, u32 evb, u16* dst);             \
  /* Nearest-neighbour scale of one output line to `xrun[256]` destination pixels, for frontends that own a  \
     panel-sized buffer and scale as the line is produced rather than rescaling the framebuffer afterwards.  \
     `xrun` has 257 entries: source pixel s covers destination [xrun[s], xrun[s+1]), which is the inverse of \
     the usual dst_x -> src_x = dst_x * 256 / width map, so each source pixel is one contiguous run and no   \
     gather is needed. Monotonic and non-decreasing; a zero-length run (destination narrower than 256) is    \
     skipped. */                                                                                             \
  void NS##scale_row(const u32* src, const u16* xrun, u32* dst);                                             \
  /* scale_row with the LCD grid: a run of at least `min_run` pixels has its FIRST written with its RGB   \
     scaled by f/256 (alpha kept); shorter runs are written plain. With min_run = ceil(width/256) only the \
     runs the fractional part of the scale widened carry a seam, so every lit cell keeps the integer      \
     width; the seam leads its run so that at 2.5x (runs 3,2,3,2,...) the cells between seams are always  \
     a pair of DS pixels (a trailing seam would leave a lone pixel at each edge). At an integer scale      \
     every run qualifies. `pitch` seams every pitch-th source pixel only (2 at exactly 2x, where a seam   \
     per pixel would leave one lit pixel in four). `seam_row` writes every pixel dimmed instead (the row \
     a source line's first destination row becomes). f in 0..255; 0 writes opaque black (0xFF000000)     \
     rather than scaling. */                                                                             \
  void NS##scale_row_grid(const u32* src, const u16* xrun, u32 f, u32 min_run, u32 pitch, bool seam_row, u32* dst); \
  /* Box-filter seams for a fractional scale: the last pixel of run s straddles source pixels s and s+1  \
     when w[s] != 0, and is written as seam[s] instead of src[s]; the other pixels are src[s] as in       \
     scale_row. (w is the same table blend_line_w takes; only its zero/non-zero pattern matters here.) */   \
  void NS##scale_row_straddle(const u32* src, const u32* seam, const u8* w, const u16* xrun, u32* dst);     \
  /* out[i] = a[i] + (b[i] - a[i]) * w[i] / 256 per byte, rounded: the area-weighted blend of two pixels,  \
     256 of them. w[i] = 128 is the midpoint. */                                                            \
  void NS##blend_line_w(const u32* a, const u32* b, const u8* w, u32* out);                                 \
  /* Bilinear, horizontal pass: out[x] = src[sx[x]] + (src[sx[x]+1] - src[sx[x]]) * wx[x] / 256 per byte,   \
     rounded as blend_line_w, for n destination pixels. sx[x] <= 254 (the caller clamps the right edge). */ \
  void NS##lerp_row_gather(const u32* src, const u16* sx, const u8* wx, u32 n, u32* out);                   \
  /* Bilinear, vertical pass: out[i] = a[i] + (b[i] - a[i]) * w / 256 per byte, rounded, n pixels, one     \
     weight (0..255) for the row. */                                                                        \
  void NS##lerp_rows(const u32* a, const u32* b, u32 w, u32 n, u32* out);                                   \
  /* 3D span stages (render3d.cpp), `n` pixels from span offset `xv0`. Perspective factor with 8 fractional   \
     bits: num = (xv*w0n) << 8 (32-bit wrap), den = xv*w0d + (xdiff-xv)*w1d, 0 when den is 0. */             \
  void NS##span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac);                      \
  /* Attribute by factor: y0 + ((y1-y0)*f >> 8) for y0 < y1, else y1 + ((y0-y1)*(256-f) >> 8); y0 == y1 -> y0. */ \
  void NS##span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out);                                \
  /* The five span attributes (r g b s t) in one pass: the perspective factor is loaded once instead of     \
     once per attribute. y0/y1 are the five endpoint pairs, out the five destination pointers. */            \
  void NS##span_attrs5(const s32* y0, const s32* y1, const u32* fac, u32 n, s32* const* out);                \
  /* The same, narrowed on store to what the pixel stages read: colour as the 6-bit channel the shader      \
     uses ((v >> 3) & 0xFF) and texture coordinates as the s16 the sampler truncates to. Eight pixels a     \
     step; the span buffers carry eight entries of slack. */                                                \
  void NS##span_attrs5n(const s32* y0, const s32* y1, const u32* fac, u32 n, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc); \
  /* The same for a span whose three colour endpoints are equal: only s and t vary, and the caller fills  \
     the colour buffers with the constant. DraStic takes this path for 83 % of SM64DS's spans and all of  \
     Meteos's (render_polygon_set_buffer8_asm against render_polygon_interpolate_rgb_asm). */             \
  void NS##span_attrs2n(const s32* y0, const s32* y1, const u32* fac, u32 n, s16* sc, s16* tc);           \
  /* Linear attribute: y0 + (y1-y0)*xv/xdiff for y0 < y1, else y1 + (y0-y1)*(xdiff-xv)/xdiff (truncating);    \
     |y1-y0| * xdiff < 2^32 (colours and texture coordinates; depth goes through span_z_linear). */           \
  void NS##span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out);                            \
  /* The linear twins of span_attrs5n / span_attrs2n: same narrowing, same results as five (or two) calls  \
     to span_attr_linear followed by the narrowing store, in one pass. Worth its own kernel because the      \
     per-call reciprocal is a 64-bit division -- five of them per span on the path that staged the five      \
     attributes as s32 first -- and because the s32 staging buffer disappears with them. */                  \
  void NS##span_attrs5n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, u8* vr, u8* vg, u8* vb, s16* sc, s16* tc); \
  void NS##span_attrs2n_lin(const s32* y0, const s32* y1, s32 xv0, u32 n, s32 xdiff, s16* sc, s16* tc);      \
  /* Z-buffer depth: base + ((disp>>9) * factor * xrecip >> 13) with base/disp/factor chosen by z0 < z1. */    \
  void NS##span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out);                    \
  /* Constant depth over n pixels (rounded up to 4; the span buffers are padded). */                          \
  void NS##span_z_const(s32 z, u32 n, s32* out);                                                              \
  /* Clear image (DISP3DCNT bit 14): n pixels of one scanline from the colour row (texture slot 2) and the   \
     depth row (slot 3), both BGR555. colour -> 18-bit record (5-bit channel c to c*2+1, 0 stays 0) with     \
     alpha 31 where bit 15 is set; depth -> (d & 0x7FFF) * 0x200 + 0x1FF; attr -> polyid | (d & 0x8000).    \
     Exactly n entries are written (the ring's border pixel follows the line). */                           \
  void NS##clear_image_run(const u16* col, const u16* dep, u32 n, u32 polyid, u32* color, u32* depth, u32* attr); \
  /* Depth pre-pass over n pixels: pass[i] = 1 where z passes the test of `mode` against the top pixel        \
     (0: z < dst; 1: z <= dst when the destination is an opaque back-facing pixel, else z < dst; 2: within    \
     0x200 either way; 3: within 0xFF), 2 where it fails but the top pixel carries edge flags (the pixel      \
     underneath is then a candidate, and only when `under` -- without AA the lower layer is never read),   \
     else 0. Returns (first << 16) | (last + 1) of the non-zero entries, 0 when there are none. `n` may be   \
     rounded up to a multiple of 4 (the arrays are padded). */                                               \
  u32  NS##depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass, bool under);

namespace ref { DS_KERNEL_LIST() }
#if DSPERATE_NEON
namespace neon { DS_KERNEL_LIST() }
namespace active = neon;
#else
namespace active = ref;
#endif

#undef DS_KERNEL_LIST

} // namespace ds::gpu::kern
