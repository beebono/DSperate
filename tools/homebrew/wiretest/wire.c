// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Wireframe test: polygons with POLYGON_ATTR alpha 0, which the DS draws as
// their outline alone, beside ordinary ones. No game on hand uses them (27
// checked, 2026-11-21), so this is what the GPU rasters' wireframe drawing is
// checked against; the CPU raster is the reference picture.
//
// Top screen, identity matrices (a vertex's x, y are the screen's -1..1):
//   left    an opaque filled triangle (red), for reference
//   middle  a wireframe triangle (white) and, over the filled one's right
//           half and nearer, a wireframe quad (yellow): only its outline may
//           hide the red
//   right   a wireframe quad with a colour at each corner
//   bottom  a translucent filled quad (alpha 16, green) with a wireframe
//           pentagon-ish strip of two quads over it (cyan)
// A small wireframe square slides left to right, a pixel every other frame,
// so no frame is the same list as the last.
//
// Build: tools/homebrew/wiretest/build.sh (the dev box's arm-linux-gnueabihf
// gcc, freestanding ARMv5TE); run headless with --direct.
typedef unsigned int u32;
typedef unsigned short u16;
typedef int s32;
#define REG32(a) (*(volatile u32*)(a))
#define REG16(a) (*(volatile u16*)(a))

static void attr(u32 alpha, u32 id) { REG32(0x040004A4) = 0xC0u | (alpha << 16) | (id << 24); }   // both faces
static void colour(u32 r, u32 g, u32 b) { REG32(0x04000480) = r | (g << 5) | (b << 10); }
static void vtx(s32 x, s32 y, s32 z) {                                                           // 4.12 each
  REG32(0x0400048C) = ((u32)x & 0xFFFF) | (((u32)y & 0xFFFF) << 16);
  REG32(0x0400048C) = (u32)z & 0xFFFF;
}
static void begin(u32 type) { REG32(0x04000500) = type; }   // 0 triangles, 1 quads, 2 triangle strip, 3 quad strip
static void end(void) { REG32(0x04000504) = 0; }

static void wait_vblank(void) {
  while (REG16(0x04000006) == 192) {}
  while (REG16(0x04000006) != 192) {}
}

void wire_main(void) {
  REG16(0x04000304) = 0x820F;                     // POWCNT1: LCD, engine A, 3D rendering and geometry, A on top
  REG32(0x04000000) = 0x00010000 | (1 << 8) | (1 << 3);   // DISPCNT: mode 0, BG0 on, BG0 is the 3D layer
  REG16(0x04000008) = 0;
  REG16(0x04000060) = 1 << 3;                     // DISP3DCNT: alpha blending
  REG32(0x04000350) = 0x001F0000 | (4 << 10);     // clear colour: dark blue, alpha 31
  REG16(0x04000354) = 0x7FFF;                     // clear depth: far
  u32 frame = 0;
  for (;;) {
    REG32(0x04000580) = (191u << 24) | (255u << 16);   // VIEWPORT: the whole screen
    REG32(0x04000440) = 0; REG32(0x04000454) = 0;      // projection: identity
    REG32(0x04000440) = 2; REG32(0x04000454) = 0;      // position and vector: identity
    REG32(0x040004A8) = 0;                             // no texture

    attr(31, 1); begin(0); colour(31, 0, 0);           // the filled triangle
    vtx(-3900, 2600, 800); vtx(-1700, 2600, 800); vtx(-2800, 300, 800); end();

    attr(0, 2); begin(0); colour(31, 31, 31);          // a wireframe triangle
    vtx(-1100, 3300, 0); vtx(900, 2900, 0); vtx(-300, 600, 0); end();

    attr(0, 3); begin(1); colour(31, 31, 0);           // a wireframe quad over the filled triangle, nearer
    vtx(-2900, 3000, -800); vtx(-1300, 3000, -800); vtx(-1300, 900, -800); vtx(-2900, 900, -800); end();

    attr(0, 4); begin(1);                              // a wireframe quad, a colour a corner
    colour(31, 0, 0); vtx(1500, 3300, 0); colour(0, 31, 0); vtx(3700, 2700, 0);
    colour(0, 0, 31); vtx(3300, 500, 0); colour(31, 31, 31); vtx(1300, 1100, 0); end();

    attr(16, 5); begin(1); colour(0, 31, 0);           // a translucent filled quad
    vtx(-3300, -600, 400); vtx(600, -600, 400); vtx(600, -3300, 400); vtx(-3300, -3300, 400); end();

    attr(0, 6); begin(3); colour(0, 31, 31);           // a wireframe quad strip over it: two quads, the shared edge once each
    vtx(-2600, -1000, 0); vtx(-2600, -2900, 0); vtx(-1000, -1300, 0); vtx(-1000, -2600, 0);
    vtx(300, -1000, 0); vtx(300, -2900, 0); end();

    {                                                  // a small wireframe square that slides
      const s32 x = -3900 + (s32)((frame >> 1) % 220) * 32;
      attr(0, 7); begin(1); colour(31, 16, 31);
      vtx(x, -3500, -1200); vtx(x + 400, -3500, -1200); vtx(x + 400, -3900, -1200); vtx(x, -3900, -1200); end();
    }

    REG32(0x04000540) = 0;                             // SWAP_BUFFERS
    wait_vblank();
    ++frame;
  }
}
