// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>
#include <vector>

namespace ds { struct NDS; struct CpuContext; }

namespace ds::io {

// IRQ bit numbers (IE/IF), per GBATEK.
enum Irq : u32 {
  IRQ_VBLANK = 0, IRQ_HBLANK = 1, IRQ_VCOUNT = 2,
  IRQ_TIMER0 = 3, IRQ_TIMER1 = 4, IRQ_TIMER2 = 5, IRQ_TIMER3 = 6,
  IRQ_RTC = 7, IRQ_DMA0 = 8, IRQ_DMA1 = 9, IRQ_DMA2 = 10, IRQ_DMA3 = 11,
  IRQ_KEYPAD = 12, IRQ_GBA_SLOT = 13,
  IRQ_IPC_SYNC = 16, IRQ_IPC_SEND_EMPTY = 17, IRQ_IPC_RECV = 18,
  IRQ_CART_DONE = 19, IRQ_CART_IREQ = 20, IRQ_GX_FIFO = 21,
  IRQ_LID = 22, IRQ_SPI = 23, IRQ_WIFI = 24,
};

struct Timer {
  u16 reload = 0;
  u16 control = 0;
  u16 counter = 0;
  u64 start_time = 0;     // scheduler time when `counter` was last sampled
  bool running() const { return control & 0x80; }
  bool count_up() const { return control & 0x04; }
  u32 prescaler_shift() const { static const u8 s[4] = {0, 6, 8, 10}; return s[control & 3]; }
};

struct IpcFifo {
  std::array<u32, 16> data{};
  u32 head = 0, count = 0;
  u32 last = 0;             // last value read; returned on empty read
  bool empty() const { return count == 0; }
  bool full() const { return count == 16; }
  void push(u32 v) { data[(head + count) & 15] = v; ++count; }
  u32 pop() { u32 v = data[head]; head = (head + 1) & 15; --count; last = v; return v; }
  void clear() { head = count = 0; }
};

// Per-CPU I/O state.
struct CpuIo {
  u32 ime = 0, ie = 0, if_ = 0;
  u16 ipc_sync = 0;       // bits 11:8 = our output
  u16 ipc_fifo_cnt = 0;   // bits 2 (send irq), 10 (recv irq), 14 (error), 15 (enable)
  IpcFifo fifo_out;       // what this CPU sends
  std::array<Timer, 4> timers;
  u16 postflg = 0;
  std::array<u32, 4> dma_fill{};
};

// SPI bus devices (ARM7).
struct SpiFirmware {
  bool hold = false; u8 cmd = 0; u32 pos = 0; u32 addr = 0; u8 status = 0; u8 data = 0;
};
struct SpiTouch { bool hold = false; u32 pos = 0; u8 cmd = 0; u16 sample = 0; u8 data = 0; };
struct SpiPower { bool hold = false; u32 pos = 0; u8 cmd = 0; std::array<u8, 8> regs{}; u8 data = 0; };

struct Rtc {
  u16 io = 0;                   // RTC register (bit0 SIO, bit1 SCK, bit2 CS, bits 4-6 direction)
  u8  input = 0, input_bit = 0, input_pos = 0;
  u8  output[8] = {}; u8 output_bit = 0, output_pos = 0;
  u8  cmd = 0;
  // Device state (melonDS reset values: 2000-01-01 00:00:00, status clear).
  u8  status1 = 0x82, status2 = 0;   // bit7: power was lost; bit1: 24-hour mode (melonDS defaults)
  u8  datetime[7] = {0, 1, 1, 0, 0, 0, 0};   // yy mm dd dow hh mm ss (BCD)
  u8  alarm1[3] = {}, alarm2[3] = {};
  u8  clock_adjust = 0, free_reg = 0;
};

// Cart bus (Slot-1). No cartridge is emulated yet; the transfer timing and
// the DRQ/FIFO state machine are modelled so the BIOS sees a plausible empty
// slot (all data words read as zero).
struct Cart {
  u16 auxspicnt = 0; u8 auxspidata = 0;
  u32 romctrl = 0;
  std::array<u8, 8> cmd{};
  u32 transfer_pos = 0, transfer_len = 0;   // bytes
  u32 fifo_count = 0;                       // 0..2 words waiting for the CPU
  u32 fifo[2] = {0, 0}; u32 fifo_head = 0;
  bool late = false;                        // FIFO was full; receive paused
};

// ARM9 hardware divider and square root unit (0x04000280-0x040002BF).
struct MathUnit {
  u16 divcnt = 0, sqrtcnt = 0;              // bit 15 busy; DIVCNT bit 14 division by zero
  u64 div_num = 0, div_den = 0, div_quot = 0, div_rem = 0;
  u64 sqrt_val = 0; u32 sqrt_res = 0;
};

class Io {
public:
  explicit Io(NDS& nds);
  void reset();

  u32  read (Cpu cpu, u32 addr, u32 width);
  void write(Cpu cpu, u32 addr, u32 width, u32 value);

  // IRQ lines.
  void request_irq(Cpu cpu, u32 bit);
  void update_irq(Cpu cpu);

  // Display status, driven by the GPU timing events.
  u16 dispstat[2] = {0, 0};
  u16 vcount = 0;
  void set_vcount(u16 line);
  void set_hblank(bool on);
  void set_vblank(bool on);

  CpuIo cpu_io[2];
  u8  wramcnt = 0;         // 0x04000247
  u8  vramcnt[9] = {};     // 0x04000240-0x04000249 (skipping 0x247)
  u16 powcnt1 = 0;         // 0x04000304 (ARM9)
  u16 powcnt2 = 0;         // 0x04000304 (ARM7)
  u16 keyinput = 0x03FF, extkeyin = 0x007F;
  u16 exmemcnt = 0;
  u16 spicnt = 0; u8 spidata = 0;
  SpiFirmware spi_fw; SpiTouch spi_tsc; SpiPower spi_pm;
  Rtc rtc;
  Cart cart;
  u16 arm7_bios_prot = 0;    // ARM7 BIOS reads below this from outside the BIOS return garbage
  bool cart_drq() const { return (cart.romctrl & 0x00800000) != 0; }
  u16 sound_cnt = 0, sound_bias = 0x200;
  std::array<u8, 0x100> sound_regs{};
  // Wi-Fi (ARM7, 0x04800000-0x0480FFFF). Register file, 8 KB RAM, baseband
  // and RF register indirection — enough for games' hardware probing. No
  // frames, timers or interrupts yet. Power-gated by POWCNT2 bit 1.
  std::array<u8, 0x2000> wifi_ram{};
  std::array<u16, 0x800> wifi_io{};
  std::array<u8, 0x100> wifi_bb{}, wifi_bb_ro{};
  std::array<u32, 0x40> wifi_rf{};
  u8  wifi_rf_version = 2;
  u16 wifi_random = 1;
  void wifi_reset();
  u16  wifi_read16(u32 addr);
  void wifi_write16(u32 addr, u16 value);
  std::array<u8, 0x100> gx_regs{};
  // Geometry engine placeholder: no 3D yet, so the command FIFO is always
  // empty and GXSTAT's FIFO IRQ conditions (bits 30-31) hold permanently.
  u32 gxstat = 0;
  void gx_check_irq();
  MathUnit math;
  void div_start(); void div_done();
  void sqrt_start(); void sqrt_done();

  // Timers are sampled lazily from scheduler time.
  u16 timer_value(Cpu cpu, int idx);
  void timer_overflow(Cpu cpu, int idx);

  // SPI transfer completion (scheduled).
  void spi_done();

private:
  NDS& nds_;
  u32  read16(Cpu cpu, u32 addr);
  void write16(Cpu cpu, u32 addr, u16 value);
  u8   read8(Cpu cpu, u32 addr);
  void write8(Cpu cpu, u32 addr, u8 value);
  void write32_special(Cpu cpu, u32 addr, u32 value, bool& handled);
  u32  read32_special(Cpu cpu, u32 addr, bool& handled);

  void ipc_sync_write(Cpu cpu, u16 value);
  void ipc_fifo_cnt_write(Cpu cpu, u16 value);
  void ipc_fifo_send(Cpu cpu, u32 value);
  u32  ipc_fifo_recv(Cpu cpu);
  u16  ipc_fifo_cnt_read(Cpu cpu);

  void timer_write_control(Cpu cpu, int idx, u16 value);
  void timer_schedule(Cpu cpu, int idx);

  void spi_write_data(u8 value);
  u8   spi_transfer(u8 value);
  void spi_release();

  void rtc_write(u16 value, bool byte);
  u16  rtc_read() const { return rtc.io; }
  void rtc_byte_in(u8 value);
  void rtc_cmd_read();
  void rtc_cmd_write(u8 value);

  void cart_write_romctrl(u32 value);
  u32  cart_read_data();
  void cart_end_transfer();
  void cart_receive_word();
  void cart_schedule_receive();
public:
  void cart_event(u32 param);
};

} // namespace ds::io
