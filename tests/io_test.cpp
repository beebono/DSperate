// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM9 I/O block tests: hardware divider / square root, VRAMCNT register
// layout, GXSTAT placeholder.
#include "core/nds.h"

#include <cstdio>

using namespace ds;

static int failures = 0;
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)

static void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM9, a, v); }
static u32  r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM9, a); }
static void w16(NDS& nds, u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM9, a, v); }
static u16  r16(NDS& nds, u32 a) { return nds.bus.dma_read16(Cpu::ARM9, a); }

// Let scheduled events (the unit's latency) fire.
static void settle(NDS& nds) { nds.sched.run_until(nds.sched.now() + 400); }

static void test_div() {
  NDS nds;
  // 32/32: 100 / 7 = 14 r 2.
  w32(nds, 0x04000280, 0);
  w32(nds, 0x04000290, 100); w32(nds, 0x04000298, 7);
  CHECK_EQ(r16(nds, 0x04000280) & 0x8000, 0x8000u);          // busy
  settle(nds);
  CHECK_EQ(r16(nds, 0x04000280) & 0xC000, 0u);
  CHECK_EQ(r32(nds, 0x040002A0), 14u); CHECK_EQ(r32(nds, 0x040002A4), 0u);
  CHECK_EQ(r32(nds, 0x040002A8), 2u);
  // Negative 32-bit: -100 / 7 = -14 r -2, sign-extended into 64 bits.
  w32(nds, 0x04000290, static_cast<u32>(-100)); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), static_cast<u32>(-14)); CHECK_EQ(r32(nds, 0x040002A4), 0xFFFFFFFFu);
  CHECK_EQ(r32(nds, 0x040002A8), static_cast<u32>(-2)); CHECK_EQ(r32(nds, 0x040002AC), 0xFFFFFFFFu);
  // Division by zero (32-bit mode): quotient +/-1 with the odd upper half, remainder = numerator, flag set.
  w32(nds, 0x04000290, 5); w32(nds, 0x04000298, 0); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), 0xFFFFFFFFu); CHECK_EQ(r32(nds, 0x040002A4), 0u);
  CHECK_EQ(r32(nds, 0x040002A8), 5u);
  CHECK_EQ(r16(nds, 0x04000280) & 0x4000, 0x4000u);
  // 64/32: 0x1_0000_0000 / 2.
  w32(nds, 0x04000280, 1);
  w32(nds, 0x04000290, 0); w32(nds, 0x04000294, 1); w32(nds, 0x04000298, 2); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), 0x80000000u); CHECK_EQ(r32(nds, 0x040002A4), 0u);
  CHECK_EQ(r16(nds, 0x04000280) & 0x4000, 0u);
  // 64/64 overflow: INT64_MIN / -1.
  w32(nds, 0x04000280, 2);
  w32(nds, 0x04000290, 0); w32(nds, 0x04000294, 0x80000000); w32(nds, 0x04000298, 0xFFFFFFFF); w32(nds, 0x0400029C, 0xFFFFFFFF); settle(nds);
  CHECK_EQ(r32(nds, 0x040002A0), 0u); CHECK_EQ(r32(nds, 0x040002A4), 0x80000000u);
  CHECK_EQ(r32(nds, 0x040002A8), 0u);
  // 16-bit halves of the operand registers are writable too.
  w32(nds, 0x04000280, 0);
  w16(nds, 0x04000290, 81); w16(nds, 0x04000292, 0); w16(nds, 0x04000298, 9); w16(nds, 0x0400029A, 0); settle(nds);
  CHECK_EQ(r16(nds, 0x040002A0), 9u);
}

static void test_sqrt() {
  NDS nds;
  w32(nds, 0x040002B0, 0);
  w32(nds, 0x040002B8, 1024); settle(nds);
  CHECK_EQ(r32(nds, 0x040002B4), 32u);
  w32(nds, 0x040002B8, 0xFFFFFFFF); settle(nds);
  CHECK_EQ(r32(nds, 0x040002B4), 65535u);
  w32(nds, 0x040002B0, 1);                                  // 64-bit
  w32(nds, 0x040002B8, 0); w32(nds, 0x040002BC, 1); settle(nds);   // sqrt(2^32) = 65536
  CHECK_EQ(r32(nds, 0x040002B4), 65536u);
  w32(nds, 0x040002B8, 0xFFFFFFFF); w32(nds, 0x040002BC, 0xFFFFFFFF); settle(nds);
  CHECK_EQ(r32(nds, 0x040002B4), 0xFFFFFFFFu);
  CHECK_EQ(r16(nds, 0x040002B0) & 0x8000, 0u);
}

static void test_vramcnt_layout() {
  NDS nds;
  // 0x247 is WRAMCNT; 0x248/0x249 are banks H and I.
  w16(nds, 0x04000248, 0x8382);
  CHECK_EQ(nds.io.vramcnt[7], 0x82u); CHECK_EQ(nds.io.vramcnt[8], 0x83u);
  CHECK_EQ(r16(nds, 0x04000248), 0x8382u);
  nds.bus.dma_write16(Cpu::ARM9, 0x04000246, 0x0381);        // G = 0x81, WRAMCNT = 3
  CHECK_EQ(nds.io.vramcnt[6], 0x81u); CHECK_EQ(static_cast<u32>(nds.io.wramcnt), 3u);
  CHECK_EQ(nds.io.powcnt1, 0u);                             // untouched by the neighbouring writes
}

static void test_gxstat_irq() {
  NDS nds;
  nds.io.cpu_io[0].ie = 1u << 21; nds.io.cpu_io[0].ime = 1;
  w32(nds, 0x04000600, 0x40000000);                         // ignored: geometry engine powered down
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 0u);
  w16(nds, 0x04000304, 0x820F);
  w32(nds, 0x04000600, 0x40000000);                         // IRQ while the FIFO is less than half full
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 1u << 21);
  CHECK_EQ(r32(nds, 0x04000600) & 0x06000000, 0x06000000u); // FIFO empty + less than half
  w32(nds, 0x04000214, 1u << 21);                           // level-sensitive: acknowledging re-raises it
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 1u << 21);
  w32(nds, 0x04000600, 0);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 21), 0u);
}

int main() {
  test_div();
  test_sqrt();
  test_vramcnt_layout();
  test_gxstat_irq();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("io: ok");
  return 0;
}
