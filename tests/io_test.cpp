// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// ARM9 I/O block tests: hardware divider / square root, VRAMCNT register
// layout, GXSTAT placeholder, and the frontend input path (buttons, KEYCNT
// interrupts, touchscreen samples over SPI).
#include "core/nds.h"
#include "core/input/input_log.h"

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

// Reads one 12-bit sample from the touchscreen controller the way a game
// does: control byte with the channel, then two data bytes.
static u16 tsc_read(NDS& nds, u32 channel, bool bits8 = false) {
  auto spi = [&](u8 v) {
    nds.io.write(Cpu::ARM7, 0x040001C0, 16, 0x8000 | 0x0800 | 0x0200);   // enable, hold, device 2
    nds.io.write(Cpu::ARM7, 0x040001C2, 8, v);
    nds.sched.run_until(nds.sched.now() + 4000);                         // transfer latency
    return static_cast<u8>(nds.io.read(Cpu::ARM7, 0x040001C2, 8));
  };
  spi(static_cast<u8>(0x80 | (channel << 4) | (bits8 ? 0x08 : 0)));
  const u8 hi = spi(0), lo = spi(0);
  nds.io.write(Cpu::ARM7, 0x040001C0, 16, 0);                            // deselect
  return bits8 ? static_cast<u16>((hi << 1) | (lo >> 7)) : static_cast<u16>((hi << 5) | (lo >> 3));
}

// Microphone through the PMIC amplifier and the TSC's AUX channel, and the
// hinge bit with its wake-up interrupt.
static void test_mic_and_lid() {
  NDS nds;
  static const s16 buf[4] = {0, 1024, -2048, 32767};
  nds.io.set_mic(buf, 4);
  CHECK_EQ(tsc_read(nds, 6), 0x800u);                       // first quarter of the frame: sample 0, gain x20
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME / 4);
  CHECK_EQ(tsc_read(nds, 6), 0x800u + 64);                  // 1024 >> 4
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME / 4);
  CHECK_EQ(tsc_read(nds, 6), 0x800u - 128);
  CHECK_EQ(tsc_read(nds, 6, true), (0x800u - 128) >> 4);    // 8-bit conversion
  nds.io.spi_pm.regs[3] = 3;                                // x160: 8x the x20 level, clipped
  CHECK_EQ(tsc_read(nds, 6), 0x800u - 1024);
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME / 4);
  CHECK_EQ(tsc_read(nds, 6), 0xFFFu);
  nds.io.set_mic(nullptr, 0);                               // silence
  CHECK_EQ(tsc_read(nds, 6), 0x800u);

  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x80, 0u);        // open
  nds.io.cpu_io[1].if_ = 0;
  nds.io.set_lid(true);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x80, 0x80u);
  CHECK_EQ(nds.io.cpu_io[1].if_ & (1u << 22), 0u);                     // closing is not an IRQ
  nds.io.set_lid(false);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x80, 0u);
  CHECK_EQ(nds.io.cpu_io[1].if_ & (1u << 22), 1u << 22);               // opening is
  CHECK_EQ(nds.io.read(Cpu::ARM9, 0x04000136, 16), 0u);
}

static void test_input() {
  NDS nds;
  using B = io::Io::Button;
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu);                 // nothing held
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16), 0x007Fu);

  nds.io.set_buttons((1u << B::BTN_A) | (1u << B::BTN_DOWN) | (1u << B::BTN_X));
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu & ~((1u << 0) | (1u << 7)));
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16), 0x007Eu);   // X held, pen up
  nds.io.set_buttons(0);
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16), 0x007Fu);

  // KEYCNT: IRQ when any selected key is held (bit 14 on, bit 15 off).
  nds.cpu(Cpu::ARM9).hot.regs[15] = 0;
  w16(nds, 0x04000132, 0x4000 | (1u << 1));                // B
  nds.io.cpu_io[0].if_ = 0;
  nds.io.set_buttons(1u << B::BTN_A);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 0u);         // A is not selected
  nds.io.set_buttons(1u << B::BTN_B);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 1u << 12);
  // AND condition: both selected keys must be held.
  nds.io.cpu_io[0].if_ = 0;
  w16(nds, 0x04000132, 0xC000 | (1u << 0) | (1u << 1));
  nds.io.set_buttons(1u << B::BTN_A);
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 0u);
  nds.io.set_buttons((1u << B::BTN_A) | (1u << B::BTN_B));
  CHECK_EQ(nds.io.cpu_io[0].if_ & (1u << 12), 1u << 12);
  w16(nds, 0x04000132, 0);
  nds.io.set_buttons(0);

  // Touchscreen: calibration is normalised at load, so ADC = pixel << 4.
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x40, 0x40u);   // pen up
  nds.io.set_touch(100, 50, true);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x40, 0u);      // pen down
  CHECK_EQ(tsc_read(nds, 5), 100u << 4);                            // X
  CHECK_EQ(tsc_read(nds, 1), 50u << 4);                             // Y
  nds.io.set_touch(400, -8, true);                                  // clamped to the screen
  CHECK_EQ(tsc_read(nds, 5), 255u << 4);
  CHECK_EQ(tsc_read(nds, 1), 0u);
  nds.io.set_touch(0, 0, false);
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0x40, 0x40u);
  CHECK_EQ(tsc_read(nds, 1), 0xFFFu);
}

static void test_input_log() {
  const char* path = "/tmp/dsperate_input_log_test.bin";
  {
    input::Log w;
    CHECK_EQ(w.open_write(path), true);
    for (u32 i = 0; i < 1000; ++i) {
      input::Frame f{static_cast<u16>(i * 7), static_cast<u8>(i), static_cast<u8>(255 - i), (i & 3) == 0, (i & 7) == 1};
      for (int k = 0; k < input::Frame::MIC_SAMPLES; ++k) f.mic[k] = static_cast<s8>(i + k);
      w.write(f);
    }
  }
  input::Log r;
  CHECK_EQ(r.open_read(path), true);
  CHECK_EQ(r.frames(), 1000u);
  input::Frame f; u32 n = 0;
  while (r.read(f)) {
    input::Frame want{static_cast<u16>(n * 7), static_cast<u8>(n), static_cast<u8>(255 - n), (n & 3) == 0, (n & 7) == 1};
    for (int k = 0; k < input::Frame::MIC_SAMPLES; ++k) want.mic[k] = static_cast<s8>(n + k);
    CHECK_EQ(f == want, true);
    ++n;
  }
  CHECK_EQ(n, 1000u);
  // Applying a frame drives the registers the games read.
  NDS nds;
  input::Frame fr{1u << io::Io::BTN_START, 10, 20, true, true};
  fr.mic[2] = 16;
  input::apply(nds, fr);
  CHECK_EQ(r16(nds, 0x04000130), 0x03FFu & ~(1u << 3));
  CHECK_EQ(nds.io.read(Cpu::ARM7, 0x04000136, 16) & 0xC0, 0x80u);   // pen down, lid closed
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME * 5 / 16);   // slot 2 of 8
  CHECK_EQ(tsc_read(nds, 6), 0x800u + 256);                           // 16 * 256 >> 4
  // A version-1 log (8-byte records, no mic or lid) still reads.
  {
    FILE* v1 = std::fopen(path, "wb");
    const u8 h[16] = {'D', 'S', 'I', 'N', 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
    const u8 r[16] = {3, 0, 5, 6, 1, 0, 0, 0,  0, 0, 7, 8, 0, 0, 0, 0};
    std::fwrite(h, 1, 16, v1); std::fwrite(r, 1, 16, v1); std::fclose(v1);
  }
  input::Log old;
  CHECK_EQ(old.open_read(path), true);
  CHECK_EQ(old.frames(), 2u);
  CHECK_EQ(old.read(f), true);
  CHECK_EQ((f == input::Frame{3, 5, 6, true}), true);
  CHECK_EQ(old.read(f), true);
  CHECK_EQ((f == input::Frame{0, 7, 8, false}), true);
  CHECK_EQ(old.read(f), false);
  // Decimation keeps the loudest sample of each span.
  s16 cap[80] = {};
  cap[25] = -12800; cap[27] = 3000; cap[79] = 25600;
  input::decimate_mic(fr, cap, 80);
  CHECK_EQ(fr.mic[0], 0); CHECK_EQ(fr.mic[2], -50); CHECK_EQ(fr.mic[7], 100);
  std::remove(path);
}

int main() {
  test_div();
  test_sqrt();
  test_vramcnt_layout();
  test_gxstat_irq();
  test_input();
  test_mic_and_lid();
  test_input_log();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("io: ok");
  return 0;
}
