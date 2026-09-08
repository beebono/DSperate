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
struct SpiTouch { bool hold = false; u32 pos = 0; u8 cmd = 0; u16 sample = 0; u8 data = 0;
  // Last position from the frontend in the TSC's 12-bit ADC units; the
  // firmware's calibration is normalised at load so ADC = pixel << 4 (nds.cpp).
  u16 x = 0, y = 0xFFF; };
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
  // Free-running clock. Off by default: the verification harness compares
  // frame hashes and traces against melonDS, and a clock seeded from the wall
  // would make every run differ from the last. A frontend that wants a real
  // console turns it on (Io::start_rtc_clock), which also clears the
  // power-lost bit -- a clock holding valid time has not lost power, and that
  // bit is exactly what sends the firmware into its first-boot setup wizard.
  bool ticking = false;
  u64  next_tick = 0;           // scheduler time of the next one-second carry
};

// Cart bus (Slot-1): ROMCTRL, the transfer timing and the DRQ/FIFO state
// machine. The cartridge itself is cart::Cart (cart.h); an empty slot reads
// every data word as zero.
struct Cart {
  u16 auxspicnt = 0; u8 auxspidata = 0;
  u32 romctrl = 0;
  std::array<u8, 8> cmd{};
  u32 transfer_pos = 0, transfer_len = 0;   // bytes
  u32 fifo_count = 0;                       // 0..2 words waiting for the CPU
  u32 fifo[2] = {0, 0}; u32 fifo_head = 0;
  bool late = false;                        // FIFO was full; receive paused
  u64 next_word_at = 0;                     // when the next word lands (nominal, not a slice end)
  bool event_armed = false;                 // a per-word Cart event is pending (DMA path only); in bulk mode, the end event
  bool bulk = false;                        // DS_CART_BULK: a DMA is taking the words as fast as it reads, one end event
};

// ARM9 hardware divider and square root unit (0x04000280-0x040002BF).
struct MathUnit {
  u16 divcnt = 0, sqrtcnt = 0;              // bit 15 (busy) is never stored: it is `pending && now < ready_at`; DIVCNT bit 14 division by zero
  u64 div_num = 0, div_den = 0, div_quot = 0, div_rem = 0;
  u64 sqrt_val = 0; u32 sqrt_res = 0;
  // A started operation and when its result is due. The result is computed
  // on the first read at or after that time, not by an event: a scheduled
  // event cannot fire before the current slice ends (the running CPU's
  // budget is never shortened), so the busy bit stayed set for the rest of
  // the slice -- up to 2048 cycles at --quantum 0 against 36-68 on hardware
  // -- and every game that polls DIVCNT/SQRTCNT spun on it. Time comparisons
  // against sched.now() are cycle-exact inside a slice.
  u64  div_ready_at = 0, sqrt_ready_at = 0;
  bool div_pending = false, sqrt_pending = false;
};

class Io {
public:
  explicit Io(NDS& nds);
  void reset();
  template <class S> void sync_state(S& s);

  u32  read (Cpu cpu, u32 addr, u32 width);
  void write(Cpu cpu, u32 addr, u32 width, u32 value);
  static bool census_on();   // DS_IO_CENSUS: a bypass around write() must keep counting
  // The frontend's fast_load: cart DMA without the card's clock (cart_bulk_); DS_CART_BULK in the environment wins.
  void set_cart_bulk(bool on);

  // IRQ lines.
  void request_irq(Cpu cpu, u32 bit);
  // LCD-originated IRQs (HBlank, VBlank, VCount match) reach the CPU
  // lcd_irq_delay cycles after their DISPSTAT flag (DS_LCD_IRQ_DELAY, ARM9
  // cycles, default 4; 0 = same instant). Hardware latches the flag first and the core
  // sees the request a little later; Art Academy's load screen polls VCOUNT
  // for 192 and needs to read it before the VBlank IRQ takes it away.
  void lcd_irq(Cpu cpu, u32 bit);
  void flush_lcd_irq();
  u32  lcd_irq_delay = 4;   // two bus cycles; 0 reproduces the old same-instant delivery
  u32  lcd_irq_pending[2] = {0, 0};
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
  u16 keyinput = 0x03FF, extkeyin = 0x007F;   // 0 = held
  u16 keycnt[2] = {0, 0};                     // 0x04000132, one per CPU

  // Frontend input. `pressed` is a mask of Button bits; the touch position is
  // in screen pixels (0-255, 0-191), and `down` false lifts the pen.
  enum Button : u32 {
    BTN_A = 0, BTN_B, BTN_SELECT, BTN_START, BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN,
    BTN_R, BTN_L, BTN_X, BTN_Y, BTN_COUNT
  };
  void set_buttons(u32 pressed);
  void set_touch(int x, int y, bool down);
  // Lid (hinge) state: EXTKEYIN bit 7 is set while closed, and opening it
  // raises the ARM7's lid IRQ (22), which is what ends the firmware's sleep.
  void set_lid(bool closed);
  bool lid_closed() const { return extkeyin & 0x80; }
  // Microphone samples for the coming frame (signed 16-bit mono at any
  // rate): a TSC AUX read picks the one at the same fraction of the frame as
  // the read's cycle position. Empty = silence. The pointer must stay valid
  // until the next call or the frame's end.
  void set_mic(const s16* samples, size_t count);
  u16  mic_sample() const;          // 12-bit ADC value after the PMIC amplifier
  // Whether the game has ever sampled the AUX input. The frontend opens the
  // host capture device only once this turns true: most titles never read the
  // mic, and on some handhelds opening the codec's capture PCM disturbs the
  // playback stream it shares a DAI with.
  bool mic_used() const { return mic_used_; }
  void update_key_irq();
  u16 exmemcnt = 0;
  u16 spicnt = 0; u8 spidata = 0;   // bit 7 never stored, see spi_busy()
  u64 spi_ready_at = 0;
  SpiFirmware spi_fw; SpiTouch spi_tsc; SpiPower spi_pm;
  const s16* mic_ = nullptr; size_t mic_count_ = 0; u64 mic_start_ = 0;
  mutable bool mic_used_ = false;
  Rtc rtc;
  Cart cart;
  u16 arm7_bios_prot = 0;    // ARM7 BIOS reads below this from outside the BIOS return garbage
  // DRQ as the DMA trigger sees it. Not const: on the lazy path the words a
  // transfer has produced so far are materialised when someone looks.
  bool cart_drq() { cart_catch_up(); return (cart.romctrl & 0x00800000) != 0; }
  // Called from every ROMCTRL and ROMDATA read, so the no-transfer case is a
  // compare and a branch at the call site.
  void cart_catch_up() { if (cart.transfer_pos < cart.transfer_len) cart_catch_up_slow(); }
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
  // Level-sensitive IRQ sources (the GX FIFO) set and clear their IF bit.
  void set_irq_line(Cpu cpu, u32 bit, bool on);
  MathUnit math;
  void div_start(); void div_done();
  void sqrt_start(); void sqrt_done();
  // Compute a due result; compose the control register with its live busy bit.
  void div_settle()  { if (math.div_pending  && nds_sched_now() >= math.div_ready_at)  div_done(); }
  void sqrt_settle() { if (math.sqrt_pending && nds_sched_now() >= math.sqrt_ready_at) sqrt_done(); }
  u16  divcnt_read()  { div_settle();  return static_cast<u16>(math.divcnt  | (math.div_pending  ? 0x8000 : 0)); }
  u16  sqrtcnt_read() { sqrt_settle(); return static_cast<u16>(math.sqrtcnt | (math.sqrt_pending ? 0x8000 : 0)); }
  u64  nds_sched_now() const;

  // Timers are sampled lazily from scheduler time.
  u16 timer_value(Cpu cpu, int idx);
  void timer_overflow(Cpu cpu, int idx);

  // SPI transfer completion (scheduled).
  void spi_done();
  // SPICNT bit 7 (busy) is time-derived like the divider's, for the same
  // reason; the completion event is only armed when the SPI IRQ is enabled.
  bool spi_busy() const { return nds_sched_now() < spi_ready_at; }
  u16  spicnt_read() const { return static_cast<u16>(spicnt | (spi_busy() ? 0x0080 : 0)); }
  // The ARM7 polling SPICNT's busy bit: after SPI_POLL_STREAK consecutive
  // busy reads with no other I/O access between them, the rest of the wait
  // is charged to the ARM7's slice budget instead of being spun through.
  // Scheduler::now() interpolates from the consumed budget, so the ARM7
  // reaches the ready time at the same guest instant, only without the
  // iterations (Spirit Tracks: ~9 k polls a frame, each a slow-path read).
  // DS_IDLE_SKIP=0 turns it off with the other idle-loop skips.
  static constexpr u32 SPI_POLL_STREAK = 4;
  u32 spi_poll_streak_ = 0;
  u16  spicnt_read_arm7();

private:
  NDS& nds_;
  u32  read16(Cpu cpu, u32 addr);
  void write16(Cpu cpu, u32 addr, u16 value);
  u8   read8(Cpu cpu, u32 addr);
  void write8(Cpu cpu, u32 addr, u8 value);
  void vramcnt_store(u32 addr, u32 value, u32 n);   // VRAMCNT/WRAMCNT bytes, one remap per store
  // Returned by value rather than through a `bool&`: taking the address of a
  // local made -fstack-protector-strong put a guard on every Io::read and
  // Io::write, which are on the recompiler's slow memory path.
  struct Special { u32 value; bool handled; };
  Special write32_special(Cpu cpu, u32 addr, u32 value);
  Special read32_special(Cpu cpu, u32 addr);

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
  void rtc_tick();              // one second of carry through datetime[]
  void rtc_seed();              // datetime[] <- host local time, clock armed
  bool rtc_host_clock_ = false; // survives reset(), which clears Rtc itself
  // The chip clears status1 bit 7 (power was lost) when the guest reads it,
  // and only real loss of battery power sets it again. reset() is the power
  // button, not the battery coming out, so once the console has been told
  // about it the bit must not come back -- otherwise a reboot sends the
  // firmware into its first-boot setup wizard every time. Survives reset()
  // for that reason; a fresh NDS starts over.
  bool rtc_power_lost_seen_ = false;
public:
  // Seed the clock from the host's local time and start it running. The
  // setting survives reset(); the frontend calls it once at startup.
  void start_rtc_clock();
  bool rtc_host_clock() const { return rtc_host_clock_; }
  void rtc_event();
private:

  void cart_write_romctrl(u32 value);
  bool cart_bulk_ = false;   // DS_CART_BULK=1 or the frontend's fast_load, see cart_receive_word
  u32  cart_read_data();
  void cart_end_transfer();
  void cart_receive_word(u64 at);
  void cart_schedule_receive(u64 from);
  void cart_catch_up_slow();
  bool cart_dma_armed() const;
  u32  cart_word_delay() const;
public:
  void cart_event(u32 param);
};

} // namespace ds::io
