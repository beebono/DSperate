// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/engine2d.h"

// Line-stage kernels of the 2D pipeline and the output stage, as free
// functions over plane pointers (docs/ARCHITECTURE.md §5): every stage is a
// straight pass over the line with no data-dependent control flow, so each
// has a portable C++ reference (kernels_ref.cpp) and, on AArch64, a NEON twin
// (kernels_neon.cpp) with the same name and signature. `kern::active` is the
// one the renderer calls; tests/kernels_test.cpp diffs the two vector by
// vector, and the AArch64 frame dumps are compared with the host's.
//
// All pixel arrays are 256 entries and 16-byte aligned; `n` counts in the
// palette kernel are multiples of 8.

namespace ds::gpu::kern {

#define DS_KERNEL_LIST(NS)                                                                                   \
  /* Priority select of one BG plane into the top/second records. */                                         \
  void NS##select_plane(const Pixel* px, const u8* op, const u8* win, u8 wbit, u8 id, bool is3d,             \
                        Pixel* top, Pixel* second, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id);  \
  /* Priority select of the resolved OBJ plane at one priority level. */                                     \
  void NS##select_obj(const Pixel* col, const u8* attr, const u8* alpha, const u8* win, u32 prio,             \
                      Pixel* top, Pixel* second, u8* top_id, u8* top_kind, u8* top_alpha, u8* second_id);    \
  /* Colour effects: blend / brighten / darken with the OBJ and 3D override rules. */                         \
  void NS##composite_line(u32 bldcnt, u32 eva, u32 evb, u32 evy, const Pixel* top, const Pixel* second,      \
                          const u8* top_id, const u8* top_kind, const u8* top_alpha, const u8* second_id,     \
                          const u8* win, Pixel* out);                                                        \
  /* BGR555 palette entries -> 18-bit records (bit 15 = low green bit). */                                   \
  void NS##palette_to_18(const u16* pal, Pixel* out, u32 n);                                                 \
  /* One 16-colour tile row: 8 indices through a 16-entry 18-bit palette. */                                 \
  void NS##tile_row_pal16(const u8* idx, const Pixel* pal18, Pixel* px, u8* op);                             \
  /* OBJ plane plot of one sprite row, n pixels at the plane pointers (which may be read and written up to  \
     15 entries past n; callers pad): an opaque pixel wins over a transparent one or a lower priority, a      \
     transparent pixel stamps priority and mosaic on a transparent one. Paletted: idx != 0 is opaque, colour  \
     pal_base | idx, alpha 0. Bitmap: col bit 15 is opaque, colour (col & 0x7FFF) | OP_DIRECT. */             \
  void NS##obj_row_idx(const u8* idx, u32 n, u32 pal_base, u8 attr, u32* px, u8* oattr, u8* oalpha);          \
  void NS##obj_row_bmp(const u16* col, u32 n, u8 attr, u8 alpha, u32* px, u8* oattr, u8* oalpha);             \
  /* 3D layer into a BG plane: alpha 0 is transparent. */                                                    \
  void NS##layer_3d(const u32* line3d, Pixel* px, u8* op);                                                   \
  /* Text BG row, 16-colour tiles: n tiles of 4 packed bytes (low nibble first); ctl[t] = palette number      \
     (bits 0-3) | 0x10 for a horizontally flipped tile. Writes 8*n pixels; returns whether any is opaque. */  \
  bool NS##text_tiles_16(const u8* packed, const u8* ctl, const Pixel* pal18, u32 n, Pixel* px, u8* op);     \
  /* Text BG row, 256-colour tiles: n tiles of 8 indices through pal18[ctl[t] & 0xF] (256 records each). */  \
  bool NS##text_tiles_256(const u8* rows, const u8* ctl, const Pixel* const* pal18, u32 n, Pixel* px, u8* op); \
  /* Output stage: master brightness on 18-bit records, then 6->8 bit expansion to 0xAARRGGBB. */            \
  void NS##master_brightness(u16 reg, u32* dst);                                                             \
  void NS##expand_colours(u32* dst);                                                                         \
  /* Both in one pass from the engine's composite line. */                                                   \
  void NS##output_line(const Pixel* src, u16 reg, u32* dst);                                                 \
  /* 3D span stages (render3d.cpp), `n` pixels from span offset `xv0`. Perspective factor with 8 fractional   \
     bits: num = (xv*w0n) << 8 (32-bit wrap), den = xv*w0d + (xdiff-xv)*w1d, 0 when den is 0. */             \
  void NS##span_factor(s32 xv0, u32 n, s32 xdiff, s32 w0n, s32 w0d, s32 w1d, u32* fac);                      \
  /* Attribute by factor: y0 + ((y1-y0)*f >> 8) for y0 < y1, else y1 + ((y0-y1)*(256-f) >> 8); y0 == y1 -> y0. */ \
  void NS##span_attr_persp(s32 y0, s32 y1, const u32* fac, u32 n, s32* out);                                \
  /* Linear attribute: y0 + (y1-y0)*xv/xdiff for y0 < y1, else y1 + (y0-y1)*(xdiff-xv)/xdiff (truncating);    \
     |y1-y0| * xdiff < 2^32 (colours and texture coordinates; depth goes through span_z_linear). */           \
  void NS##span_attr_linear(s32 y0, s32 y1, s32 xv0, u32 n, s32 xdiff, s32* out);                            \
  /* Z-buffer depth: base + ((disp>>9) * factor * xrecip >> 13) with base/disp/factor chosen by z0 < z1. */    \
  void NS##span_z_linear(s32 z0, s32 z1, s32 xv0, u32 n, s32 xdiff, s32 xrecip, s32* out);                    \
  /* Depth pre-pass over n pixels: pass[i] = 1 where z passes the test of `mode` against the top pixel        \
     (0: z < dst; 1: z <= dst when the destination is an opaque back-facing pixel, else z < dst; 2: within    \
     0x200 either way; 3: within 0xFF), 2 where it fails but the top pixel carries edge flags (the pixel      \
     underneath is then a candidate), else 0. Returns (first << 16) | (last + 1) of the non-zero entries,     \
     0 when there are none. `n` may be rounded up to a multiple of 4 (the arrays are padded). */              \
  u32  NS##depth_candidates(int mode, const s32* z, const u32* dstz, const u32* dstattr, u32 n, u8* pass);

namespace ref { DS_KERNEL_LIST() }
#if DSPERATE_NEON
namespace neon { DS_KERNEL_LIST() }
namespace active = neon;
#else
namespace active = ref;
#endif

#undef DS_KERNEL_LIST

} // namespace ds::gpu::kern
