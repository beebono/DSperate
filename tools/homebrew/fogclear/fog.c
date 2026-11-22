// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Fog on the rear plane where nothing is drawn. The clear colour has alpha 0
// (the 2D backdrop shows through) and the rear plane's fog bit is set, with
// a fog colour whose alpha is 31: the DS fogs the empty pixels too, so the
// 3D layer is a violet haze over the backdrop, as thick as the fog table
// makes it at the clear depth. A GPU raster that skips empty pixels shows
// the bare backdrop. Two opaque quads at different depths, fog bit set, are
// fogged as usual for comparison; a third has no fog bit.
//
// The fog offset moves each frame (a slow sweep), so the haze over the empty
// pixels changes while the polygons stay put.
//
// Build: tools/homebrew/fogclear/build.sh; run headless with --direct.
typedef unsigned int u32;
typedef unsigned short u16;
typedef int s32;
#define REG32(a) (*(volatile u32*)(a))
#define REG16(a) (*(volatile u16*)(a))

static void attr(u32 alpha, u32 id, u32 fog) { REG32(0x040004A4) = 0xC0u | (alpha << 16) | (id << 24) | (fog << 15); }
static void colour(u32 r, u32 g, u32 b) { REG32(0x04000480) = r | (g << 5) | (b << 10); }
static void vtx(s32 x, s32 y, s32 z) {
  REG32(0x0400048C) = ((u32)x & 0xFFFF) | (((u32)y & 0xFFFF) << 16);
  REG32(0x0400048C) = (u32)z & 0xFFFF;
}
static void quad(s32 x0, s32 y0, s32 x1, s32 y1, s32 z) {
  REG32(0x04000500) = 1;
  vtx(x0, y0, z); vtx(x1, y0, z); vtx(x1, y1, z); vtx(x0, y1, z);
  REG32(0x04000504) = 0;
}
static void wait_vblank(void) {
  while (REG16(0x04000006) == 192) {}
  while (REG16(0x04000006) != 192) {}
}

void fog_main(void) {
  REG16(0x04000304) = 0x820F;                              // POWCNT1
  REG32(0x04000000) = 0x00010000 | (1 << 8) | (1 << 3);    // DISPCNT: mode 0, BG0 = 3D
  REG16(0x04000008) = 0;
  REG16(0x05000000) = 0x03E0;                              // backdrop: green
  REG16(0x04000050) = 0x3F00 | (1 << 6) | 1;               // BLDCNT: alpha blending, BG0 first target, everything second
  REG16(0x04000052) = 0x1010;
  REG32(0x04000350) = 0x00008000 | (1u << 24);             // clear colour: black, alpha 0, fog bit, id 1
  REG16(0x04000354) = 0x7FFF;                              // clear depth: far
  REG32(0x04000358) = (31u << 16) | (20 << 10) | 20;       // FOG_COLOR: violet, alpha 31
  for (u32 i = 0; i < 8; ++i)                              // FOG_TABLE: 32 densities, 0 near to 127 far
    REG32(0x04000360 + i * 4) = ((i * 4 * 4) & 0x7F) | (((i * 4 + 1) * 4 & 0x7F) << 8) | (((i * 4 + 2) * 4 & 0x7F) << 16) | (((i * 4 + 3) * 4 & 0x7F) << 24);
  u32 frame = 0;
  for (;;) {
    REG16(0x04000060) = (1 << 3) | (1 << 7) | (5 << 8);    // DISP3DCNT: blending, fog on, fog shift 5
    REG16(0x0400035C) = 0x4000 + ((frame >> 1) & 0x3F) * 0x80;   // FOG_OFFSET: sweeps
    REG32(0x04000580) = (191u << 24) | (255u << 16);
    REG32(0x04000440) = 0; REG32(0x04000454) = 0;
    REG32(0x04000440) = 2; REG32(0x04000454) = 0;
    REG32(0x040004A8) = 0;
    attr(31, 2, 1); colour(31, 0, 0); quad(-3600, 3000, -1400, 600, -2000);    // near, fogged little
    attr(31, 3, 1); colour(31, 0, 0); quad(-1000, 3000, 1200, 600, 3000);      // far, fogged more
    attr(31, 4, 0); colour(31, 31, 0); quad(1600, 3000, 3600, 600, 3000);      // far, no fog bit
    REG32(0x04000540) = 0;
    wait_vblank();
    ++frame;
  }
}
