// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 2D renderer tests: VRAM bank views and a few end-to-end scanlines through
// the register file, compared against hand-computed expectations.
#include "core/nds.h"
#include "core/gpu/vram_map.h"

#include <cstdio>
#include <cstring>

using namespace ds;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)

static void write9(NDS& nds, u32 addr, u32 width, u32 v) { nds.bus.dma_write32(Cpu::ARM9, addr, v); (void)width; }
static void w16(NDS& nds, u32 addr, u16 v) { nds.bus.dma_write16(Cpu::ARM9, addr, v); }

// Render `line` on engine A and return the composite (before master brightness).
static const gpu::Pixel* render_a(NDS& nds, u32 line) {
  nds.gpu.engine[0].pre_draw(false); nds.gpu.engine[0].pre_draw(false); nds.gpu.engine[0].pre_draw(false);   // enable latches
  nds.gpu.engine[0].render_sprites(line);
  nds.gpu.engine[0].render_line(line);
  return nds.gpu.engine[0].output();
}

static gpu::Pixel rgb18(u16 c) { return ((c & 0x1F) << 1) | ((((c >> 5) & 0x1F) << 1 | (c >> 15)) << 8) | (((c >> 10) & 0x1F) << 1) << 16; }

static void test_vram_views() {
  NDS nds;
  u8 cnt[9] = {};
  u8* banks[9]; for (int i = 0; i < 9; ++i) banks[i] = nds.bus.vram_bank(i);
  gpu::VramMap m;

  // B -> BG-A slot 1, F -> BG-A at 0x10000 (mirrors at +0x8000), E -> OBJ-A, C -> LCDC.
  cnt[1] = 0x89; cnt[5] = 0x91; cnt[4] = 0x82; cnt[2] = 0x80;
  m.rebuild(cnt, banks);
  CHECK_EQ(m.abg.mask[0], 0u); CHECK_EQ(m.abg.mask[8], 2u); CHECK_EQ(m.abg.mask[4], 0x20u); CHECK_EQ(m.abg.mask[6], 0x20u);
  CHECK(m.abg.ptr[0] == nullptr); CHECK(m.abg.ptr[9] == banks[1] + 0x4000); CHECK(m.abg.ptr[6] == banks[5]);
  CHECK_EQ(m.aobj.mask[0], 0x10u); CHECK(m.aobj.ptr[3] == banks[4] + 0xC000);
  CHECK_EQ(m.lcdc_mask, 4u);
  // Overlap: G on top of F -> ORed read, no direct pointer, write hits both.
  cnt[6] = 0x91; m.rebuild(cnt, banks);
  CHECK(m.abg.ptr[4] == nullptr); CHECK_EQ(m.abg.mask[4], 0x60u);
  banks[5][0x10] = 0x0F; banks[6][0x10] = 0xF0;
  CHECK_EQ(m.read8(m.abg, 0x10010), 0xFFu);
  m.write16(m.abg, 0x10020, 0x1234);
  CHECK_EQ(banks[5][0x20], 0x34u); CHECK_EQ(banks[6][0x21], 0x12u);
  // I as OBJ-B mirrors every 16 K; H+I as BG-B interleave.
  cnt[8] = 0x82; cnt[7] = 0x81; m.rebuild(cnt, banks);
  CHECK(m.bobj.ptr[5] == banks[8]); CHECK(m.bbg.ptr[1] == banks[7] + 0x4000); CHECK(m.bbg.ptr[6] == nullptr || m.bbg.mask[6] == 0);
  cnt[8] = 0x81; m.rebuild(cnt, banks);
  CHECK(m.bbg.ptr[2] == banks[8]); CHECK(m.bbg.ptr[7] == banks[8]);
}

static void test_text_bg_and_backdrop() {
  NDS nds;
  nds.io.powcnt1 = 0x820F; nds.gpu.set_powcnt(0x820F);
  // Bank A as BG-A; 16-colour text BG0, tiles at 0x4000, map at 0.
  nds.bus.dma_write16(Cpu::ARM9, 0x04000240, 0x0081);       // VRAMCNT_A = BG-A (byte write path via 16-bit)
  write9(nds, 0x04000000, 32, 0x00010100);                   // mode 0, BG0 on, display mode 1
  w16(nds, 0x04000008, 0x0004);                              // BG0CNT: char base 1 (0x4000), map base 0, 16 colour
  w16(nds, 0x05000000, 0x7C00);                              // backdrop: blue
  w16(nds, 0x05000002, 0x001F);                              // pal 0 idx 1: red
  w16(nds, 0x05000022, 0x03E0 | 0x8000);                     // pal 1 idx 1: green (bit 15 -> low green bit)
  // Tile 1: left half index 1, right half index 0, every row.
  for (u32 y = 0; y < 8; ++y) write9(nds, 0x06004020 + y * 4, 32, 0x00001111);
  // Map: tile 1 pal 0 at (0,0); tile 1 pal 1 hflipped at (1,0).
  w16(nds, 0x06000000, 0x0001);
  w16(nds, 0x06000002, 0x0001 | (1 << 10) | (1 << 12));
  const gpu::Pixel* out = render_a(nds, 0);
  CHECK_EQ(out[0] & 0xFFFFFF, rgb18(0x001F));                // red
  CHECK_EQ(out[4] & 0xFFFFFF, rgb18(0x7C00));                // transparent -> backdrop
  CHECK_EQ(out[8] & 0xFFFFFF, rgb18(0x7C00));                // flipped: left half transparent
  CHECK_EQ(out[12] & 0xFFFFFF, rgb18(0x83E0));               // green with bit 15 -> 6-bit green 0x3F
  CHECK_EQ((out[12] >> 8) & 0xFF, 0x3Fu);
  CHECK_EQ(out[16] & 0xFFFFFF, rgb18(0x7C00));               // tile 0 (empty)
  // Scroll by 4: pixel 0 now shows tile column 4 (transparent).
  w16(nds, 0x04000010, 4);
  out = render_a(nds, 0);
  CHECK_EQ(out[0] & 0xFFFFFF, rgb18(0x7C00));
  CHECK_EQ(out[4] & 0xFFFFFF, rgb18(0x7C00));
  CHECK_EQ(out[8] & 0xFFFFFF, rgb18(0x83E0));
  // Brightness down on BG0 by 8/16.
  w16(nds, 0x04000050, 0x00C1);                              // effect 3, target BG0
  w16(nds, 0x04000054, 8);
  w16(nds, 0x04000010, 0);
  out = render_a(nds, 0);
  CHECK_EQ(out[0] & 0xFF, 0x3Eu - ((0x3Eu * 8 + 7) >> 4));   // red channel darkened
  CHECK_EQ(out[4] & 0xFFFFFF, rgb18(0x7C00));                // backdrop not a target
}

static void test_sprites_and_window() {
  NDS nds;
  nds.io.powcnt1 = 0x820F; nds.gpu.set_powcnt(0x820F);
  nds.bus.dma_write16(Cpu::ARM9, 0x04000240, 0x0082);       // VRAMCNT_A = OBJ-A
  write9(nds, 0x04000000, 32, 0x00011010);                   // mode 0, OBJ on, 1D mapping, display mode 1
  w16(nds, 0x05000000, 0x0000);                              // backdrop black
  w16(nds, 0x05000202, 0x7FFF);                              // OBJ pal 0 idx 1: white
  for (u32 i = 0; i < 32; i += 4) write9(nds, 0x06400020 + i, 32, 0x11111111);   // tile 1: solid index 1
  // Sprite 0: 8x8 at (10, 0), tile 1, priority 0.
  w16(nds, 0x07000000, 0x0000); w16(nds, 0x07000002, 10); w16(nds, 0x07000004, 0x0001);
  // Sprite 1: same but semi-transparent at (30, 0), priority 0.
  w16(nds, 0x07000008, 0x0400); w16(nds, 0x0700000A, 30); w16(nds, 0x0700000C, 0x0001);
  for (int n = 2; n < 128; ++n) w16(nds, 0x07000000 + n * 8, 0x0200);   // the rest disabled
  const gpu::Pixel* out = render_a(nds, 3);
  CHECK_EQ(out[9] & 0xFFFFFF, 0u);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));
  CHECK_EQ(out[17] & 0xFFFFFF, rgb18(0x7FFF));
  CHECK_EQ(out[18] & 0xFFFFFF, 0u);
  // Semi-transparent sprite over a backdrop that is a second target blends 50/50.
  w16(nds, 0x04000050, 0x2000);                              // backdrop as 2nd target, no effect selected
  w16(nds, 0x04000052, 0x0808);
  out = render_a(nds, 3);
  CHECK_EQ(out[30] & 0xFF, (0x3Eu * 8 + 8) >> 4);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));              // normal sprite unaffected
  // Window 0 covering x 0..19, OBJ only inside, nothing outside.
  w16(nds, 0x04000040, (0 << 8) | 20);                       // x1 = 0, x2 = 20
  w16(nds, 0x04000044, (0 << 8) | 192);
  w16(nds, 0x04000048, 0x0010);                              // WININ: OBJ
  w16(nds, 0x0400004A, 0x0000);                              // WINOUT: nothing
  write9(nds, 0x04000000, 32, 0x00011010 | (1 << 13));
  for (u32 l = 0; l <= 3; ++l) nds.gpu.engine[0].update_windows(l);   // the y1 edge at line 0 arms the window
  out = render_a(nds, 3);
  CHECK_EQ(out[10] & 0xFFFFFF, rgb18(0x7FFF));
  CHECK_EQ(out[30] & 0xFFFFFF, 0u);                          // outside window 0: OBJ hidden
}

static void test_register_access() {
  NDS nds;
  nds.gpu.set_powcnt(0x820F);
  write9(nds, 0x04000000, 32, 0xFFFFFFFF);
  CHECK_EQ(nds.bus.dma_read32(Cpu::ARM9, 0x04000000), 0xFFFFFFFFu);
  write9(nds, 0x04001000, 32, 0xFFFFFFFF);
  CHECK_EQ(nds.bus.dma_read32(Cpu::ARM9, 0x04001000), 0xC0B1FFF7u);   // engine B has fewer bits
  w16(nds, 0x04000054, 0x1F);                                           // BLDY write-only
  CHECK_EQ(nds.bus.dma_read16(Cpu::ARM9, 0x04000054), 0u);
  write9(nds, 0x04000028, 32, 0x08000000);                              // BG2X sign-extends from bit 27
  w16(nds, 0x0400006C, 0xFFFF);
  CHECK_EQ(nds.bus.dma_read16(Cpu::ARM9, 0x0400006C), 0xC01Fu);
  // Powered-down engine ignores everything but DISPCNT.
  nds.gpu.set_powcnt(0x0000);
  w16(nds, 0x04000008, 0x1234);
  CHECK_EQ(nds.bus.dma_read16(Cpu::ARM9, 0x04000008), 0u);
}

int main() {
  test_vram_views();
  test_text_bg_and_backdrop();
  test_sprites_and_window();
  test_register_access();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("gpu: ok");
  return 0;
}
