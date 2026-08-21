// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// I/O register file for both CPUs. Register semantics per GBATEK; melonDS is
// the behavioural reference where GBATEK is silent.
#include "core/io/io.h"
#include "core/nds.h"
#include "core/dma/dma.h"

#include <cstdio>
#include <cstring>

namespace ds::io {

namespace {
inline int ci(Cpu c) { return static_cast<int>(c); }
inline Cpu other(Cpu c) { return c == Cpu::ARM9 ? Cpu::ARM7 : Cpu::ARM9; }
}

Io::Io(NDS& nds) : nds_(nds) { reset(); }

void Io::reset() {
  cpu_io[0] = CpuIo{}; cpu_io[1] = CpuIo{};
  dispstat[0] = dispstat[1] = 0; vcount = 0;
  wramcnt = 0; std::memset(vramcnt, 0, sizeof vramcnt);
  powcnt1 = 0; powcnt2 = 0;
  keyinput = 0x03FF; extkeyin = 0x007F;
  exmemcnt = 0;
  spicnt = 0; spidata = 0;
  spi_fw = SpiFirmware{}; spi_tsc = SpiTouch{}; spi_pm = SpiPower{};
  rtc = Rtc{};
  cart = Cart{};
  sound_cnt = 0; sound_bias = 0x200;
  sound_regs.fill(0); gx_regs.fill(0);
  wifi_reset();
}

// ---- IRQ ------------------------------------------------------------------
void Io::update_irq(Cpu cpu) {
  CpuIo& c = cpu_io[ci(cpu)];
  CpuContext& ctx = nds_.cpu(cpu);
  const bool any = (c.ie & c.if_) != 0;
  ctx.hot.irq_pending = ((c.ime & 1) && any) ? 1 : 0;
  // Halt exits on IE&IF; the ARM9 additionally needs IME (per hardware / melonDS).
  if (any && ctx.halted && (cpu == Cpu::ARM7 || (c.ime & 1))) ctx.halted = false;
}

void Io::request_irq(Cpu cpu, u32 bit) {
  cpu_io[ci(cpu)].if_ |= (1u << bit);
  update_irq(cpu);
}

// ---- display status -------------------------------------------------------
void Io::set_vcount(u16 line) {
  vcount = line;
  for (int i = 0; i < 2; ++i) {
    const u16 target = static_cast<u16>((dispstat[i] >> 8) | ((dispstat[i] & 0x80) << 1));
    if (line == target) {
      dispstat[i] |= 4;
      if (dispstat[i] & 0x20) request_irq(static_cast<Cpu>(i), IRQ_VCOUNT);
    } else {
      dispstat[i] &= ~4;
    }
  }
}
void Io::set_hblank(bool on) {
  for (int i = 0; i < 2; ++i) {
    if (on) { dispstat[i] |= 2; if (dispstat[i] & 0x10) request_irq(static_cast<Cpu>(i), IRQ_HBLANK); }
    else dispstat[i] &= ~2;
  }
}
void Io::set_vblank(bool on) {
  for (int i = 0; i < 2; ++i) {
    if (on) { dispstat[i] |= 1; if (dispstat[i] & 0x08) request_irq(static_cast<Cpu>(i), IRQ_VBLANK); }
    else dispstat[i] &= ~1;
  }
}

// ---- IPC ------------------------------------------------------------------
void Io::ipc_sync_write(Cpu cpu, u16 value) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  me.ipc_sync = (me.ipc_sync & 0x000F) | (value & 0x4F00);
  them.ipc_sync = (them.ipc_sync & 0x4F00) | ((value >> 8) & 0xF);
  if ((value & 0x2000) && (them.ipc_sync & 0x4000)) request_irq(other(cpu), IRQ_IPC_SYNC);
}

u16 Io::ipc_fifo_cnt_read(Cpu cpu) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  u16 v = me.ipc_fifo_cnt & 0x8404;
  if (me.fifo_out.empty()) v |= 0x0001;
  if (me.fifo_out.full())  v |= 0x0002;
  if (them.fifo_out.empty()) v |= 0x0100;
  if (them.fifo_out.full())  v |= 0x0200;
  if (me.ipc_fifo_cnt & 0x4000) v |= 0x4000;
  return v;
}

void Io::ipc_fifo_cnt_write(Cpu cpu, u16 value) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  const bool was_send_irq = me.ipc_fifo_cnt & 0x0004;
  const bool was_recv_irq = me.ipc_fifo_cnt & 0x0400;
  if (value & 0x0008) { me.fifo_out.clear(); }        // flush send FIFO
  if (value & 0x4000) me.ipc_fifo_cnt &= ~0x4000;      // ack error
  me.ipc_fifo_cnt = (me.ipc_fifo_cnt & 0x4000) | (value & 0x8404);
  if (!was_send_irq && (value & 0x0004) && me.fifo_out.empty()) request_irq(cpu, IRQ_IPC_SEND_EMPTY);
  if (!was_recv_irq && (value & 0x0400) && !them.fifo_out.empty()) request_irq(cpu, IRQ_IPC_RECV);
  if (value & 0x0008) {
    // flushing makes the other side's receive FIFO empty; our send FIFO is empty -> send IRQ if enabled
    if (me.ipc_fifo_cnt & 0x0004) request_irq(cpu, IRQ_IPC_SEND_EMPTY);
  }
}

void Io::ipc_fifo_send(Cpu cpu, u32 value) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  if (!(me.ipc_fifo_cnt & 0x8000)) return;
  if (me.fifo_out.full()) { me.ipc_fifo_cnt |= 0x4000; return; }
  const bool was_empty = me.fifo_out.empty();
  me.fifo_out.push(value);
  if (was_empty && (them.ipc_fifo_cnt & 0x0400)) request_irq(other(cpu), IRQ_IPC_RECV);
}

u32 Io::ipc_fifo_recv(Cpu cpu) {
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  if (them.fifo_out.empty()) { me.ipc_fifo_cnt |= 0x4000; return them.fifo_out.last; }
  if (!(me.ipc_fifo_cnt & 0x8000)) return them.fifo_out.data[them.fifo_out.head];
  u32 v = them.fifo_out.pop();
  if (them.fifo_out.empty() && (them.ipc_fifo_cnt & 0x0004)) request_irq(other(cpu), IRQ_IPC_SEND_EMPTY);
  return v;
}

// ---- timers ---------------------------------------------------------------
static void timer_event(NDS& nds, u32 param) {
  nds.io.timer_overflow(static_cast<Cpu>(param >> 2), static_cast<int>(param & 3));
}

u16 Io::timer_value(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  if (!t.running() || t.count_up()) return t.counter;
  u64 elapsed = (nds_.sched.now() - t.start_time) >> 1;   // timers run on the 33 MHz system clock
  u64 ticks = elapsed >> t.prescaler_shift();
  u64 v = t.counter + ticks;
  return static_cast<u16>(v);   // overflow handling happens in the event
}

void Io::timer_schedule(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  const EventId id = static_cast<EventId>(static_cast<int>(cpu == Cpu::ARM9 ? EventId::Timer0 : EventId::Timer7_0) + idx);
  if (!t.running() || t.count_up()) { nds_.sched.cancel(id); return; }
  u64 ticks_left = 0x10000 - t.counter;
  u64 cycles = (ticks_left << t.prescaler_shift()) << 1;   // system -> ARM9 cycles
  nds_.sched.schedule(id, nds_.sched.now() + cycles, timer_event, (static_cast<u32>(cpu) << 2) | idx);
}

void Io::timer_overflow(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  t.counter = t.reload;
  t.start_time = nds_.sched.now();
  if (t.control & 0x40) request_irq(cpu, IRQ_TIMER0 + idx);
  if (idx < 3) {
    Timer& n = cpu_io[ci(cpu)].timers[idx + 1];
    if (n.running() && n.count_up()) {
      if (++n.counter == 0) timer_overflow(cpu, idx + 1);
    }
  }
  timer_schedule(cpu, idx);
}

void Io::timer_write_control(Cpu cpu, int idx, u16 value) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  const bool was = t.running();
  if (was) t.counter = timer_value(cpu, idx);
  t.control = value & 0xC7;
  t.start_time = nds_.sched.now();
  if (!was && t.running()) t.counter = t.reload;
  timer_schedule(cpu, idx);
}

// ---- SPI (ARM7) -----------------------------------------------------------
static void spi_event(NDS& nds, u32) { nds.io.spi_done(); }

void Io::spi_done() {
  spicnt &= ~0x0080;
  if (spicnt & 0x4000) request_irq(Cpu::ARM7, IRQ_SPI);
}

u8 Io::spi_transfer(u8 value) {
  const int dev = (spicnt >> 8) & 3;
  switch (dev) {
  case 0: {                                             // power management
    SpiPower& p = spi_pm;
    if (!p.hold) { p.hold = true; p.cmd = value; p.pos = 1; p.data = 0; return 0; }
    const u32 reg = p.cmd & 0x7F;
    if (p.cmd & 0x80) p.data = (reg < 8) ? p.regs[reg] : 0;
    else { if (reg < 8) p.regs[reg] = value; p.data = 0; }
    return p.data;
  }
  case 1: {                                             // firmware flash
    SpiFirmware& f = spi_fw;
    if (!f.hold) {
      f.hold = true; f.cmd = value; f.pos = 1; f.data = 0;
      if (value == 0x06) f.status |= 2;
      if (value == 0x04) f.status &= ~2;
      return 0;
    }
    switch (f.cmd) {
    case 0x03:                                          // read
      if (f.pos < 4) { f.addr = (f.addr << 8) | value; f.data = 0; }
      else {
        const auto& fw = nds_.firmware;
        f.data = fw.empty() ? 0xFF : fw[f.addr % fw.size()];
        f.addr++;
      }
      f.pos++;
      return f.data;
    case 0x05: return f.status;                         // read status
    case 0x9F: {                                        // JEDEC id
      static const u8 id[3] = {0x20, 0x40, 0x12};
      f.data = (f.pos >= 1 && f.pos <= 3) ? id[f.pos - 1] : 0;
      f.pos++;
      return f.data;
    }
    case 0x0A:                                          // page write: accept, ignore
      if (f.pos < 4) f.addr = (f.addr << 8) | value;
      f.pos++;
      return 0;
    default:
      return 0;
    }
  }
  case 2: {                                             // touchscreen
    SpiTouch& t = spi_tsc;
    if (value & 0x80) {                                 // control byte
      t.cmd = value; t.pos = 0;
      const u32 channel = (value >> 4) & 7;
      u16 sample = 0;
      switch (channel) {
      case 1: sample = 0; break;          // Y (pen up -> 0)
      case 5: sample = 0; break;          // X
      case 6: sample = 0x800; break;      // AUX / mic: mid
      default: sample = 0; break;
      }
      t.sample = sample;
      t.data = static_cast<u8>(t.sample >> 5);          // first byte: bits 11:5
      t.pos = 1;
      return 0;
    }
    if (t.pos == 1) { t.pos = 2; return t.data; }
    t.pos = 0;
    return static_cast<u8>((t.sample << 3) & 0xF8);
  }
  default:
    return 0;
  }
}

void Io::spi_release() {
  spi_pm.hold = false; spi_fw.hold = false; spi_fw.addr = 0; spi_tsc.pos = 0;
}

void Io::spi_write_data(u8 value) {
  if (!(spicnt & 0x8000)) return;
  if (spicnt & 0x0080) return;
  spicnt |= 0x0080;
  spidata = spi_transfer(value);
  if (!(spicnt & 0x0800)) spi_release();
  const u32 delay = 8 * (8u << (spicnt & 3));
  nds_.sched.schedule(EventId::Spi, nds_.sched.now() + delay * 2, spi_event, 0);
}

// ---- RTC (ARM7, bit-banged on 0x04000138) ----------------------------------
// Protocol per GBATEK; state and edge sampling mirror melonDS so traces match.
void Io::rtc_cmd_read() {
  Rtc& r = rtc;
  if ((r.cmd & 0x0F) != 0x06) return;
  switch (r.cmd & 0x70) {
  case 0x00: r.output[0] = r.status1; r.status1 &= 0x0F; break;
  case 0x40: r.output[0] = r.status2; break;
  case 0x20: std::memcpy(r.output, &r.datetime[0], 7); break;
  case 0x60: std::memcpy(r.output, &r.datetime[4], 3); break;
  case 0x10: if (r.status2 & 0x04) std::memcpy(r.output, r.alarm1, 3); else r.output[0] = r.alarm1[2]; break;
  case 0x50: std::memcpy(r.output, r.alarm2, 3); break;
  case 0x30: r.output[0] = r.clock_adjust; break;
  case 0x70: r.output[0] = r.free_reg; break;
  }
}

void Io::rtc_cmd_write(u8 v) {
  Rtc& r = rtc;
  if ((r.cmd & 0x0F) != 0x06) return;
  const u32 pos = r.input_pos;
  switch (r.cmd & 0x70) {
  case 0x00:
    if (pos == 1) {
      if (v & 1) { r.status1 = 0; r.status2 = 0; std::memset(r.datetime, 0, 7); r.datetime[1] = r.datetime[2] = 1; }
      r.status1 = (r.status1 & 0xF0) | (v & 0x0E);
    }
    break;
  case 0x40: if (pos == 1) r.status2 = v; break;
  case 0x20: if (pos >= 1 && pos <= 7) r.datetime[pos - 1] = v; break;
  case 0x60: if (pos >= 1 && pos <= 3) r.datetime[3 + pos] = v; break;
  case 0x10: if (r.status2 & 0x04) { if (pos >= 1 && pos <= 3) r.alarm1[pos - 1] = v; } else if (pos == 1) r.alarm1[2] = v; break;
  case 0x50: if (pos >= 1 && pos <= 3) r.alarm2[pos - 1] = v; break;
  case 0x30: if (pos == 1) r.clock_adjust = v; break;
  case 0x70: if (pos == 1) r.free_reg = v; break;
  }
}

void Io::rtc_byte_in(u8 v) {
  Rtc& r = rtc;
  if (r.input_pos == 0) {
    if ((v & 0xF0) == 0x60) {
      static const u8 rev[16] = {0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6};
      r.cmd = rev[v & 0xF];
    } else r.cmd = v;
    if (r.cmd & 0x80) rtc_cmd_read();
    return;
  }
  rtc_cmd_write(v);
}

void Io::rtc_write(u16 value, bool byte) {
  Rtc& r = rtc;
  if (byte) value |= (r.io & 0xFF00);
  if (value & 0x0004) {
    if (!(r.io & 0x0004)) {                          // CS rising: start transfer
      r.input = 0; r.input_bit = 0; r.input_pos = 0;
      std::memset(r.output, 0, sizeof r.output); r.output_bit = 0; r.output_pos = 0;
    } else if (!(value & 0x0002)) {                  // clock low
      if (value & 0x0010) {                          // host drives SIO: write bit
        if (value & 1) r.input |= static_cast<u8>(1u << r.input_bit);
        if (++r.input_bit >= 8) { r.input_bit = 0; rtc_byte_in(r.input); r.input = 0; r.input_pos++; }
      } else {                                       // read bit
        if (r.output[r.output_pos] & (1u << r.output_bit)) r.io |= 1; else r.io &= 0xFFFE;
        if (++r.output_bit >= 8) { r.output_bit = 0; if (r.output_pos < 7) r.output_pos++; }
      }
    }
  }
  if (value & 0x0010) r.io = value; else r.io = (r.io & 0x0001) | (value & 0xFFFE);
}

// ---- cart bus (empty slot) ---------------------------------------------------
// Timing per GBATEK / melonDS: the 8-bit command takes 8 clocks, each data
// word 4, gap1 before data, gap2 before each 0x200-byte block; clock is
// 5 or 8 system cycles per transfer clock (ROMCTRL bit 27). System cycles
// are half ARM9 cycles.
static void cart_ev(NDS& nds, u32 param) { nds.io.cart_event(param); }

void Io::cart_write_romctrl(u32 value) {
  const bool start = (value & ~cart.romctrl) & 0x80000000;
  const bool release = (value & ~cart.romctrl) & (1u << 29);
  cart.romctrl = (cart.romctrl & 0x00800000) | (value & 0xFF7F7FFF) | (cart.romctrl & (1u << 29));
  if (value & (1u << 29)) cart.romctrl |= 1u << 29;
  if (release && nds_.cart) nds_.cart->set_reset(false);
  if (!(cart.auxspicnt & 0x8000) || (cart.auxspicnt & 0x2000) || !start) return;
  if (nds_.cart) nds_.cart->command_start(cart.cmd.data());
  u32 size_code = (cart.romctrl >> 24) & 7;
  u32 bytes = size_code == 7 ? 4 : size_code ? (0x100u << size_code) : 0;
  cart.transfer_pos = 0; cart.transfer_len = bytes;
  cart.fifo_count = 0; cart.late = false;
  cart.romctrl &= ~0x00800000u;
  const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5;
  u32 cmddelay = 8 + (cart.romctrl & 0x1FFF);
  if (bytes) cmddelay += (cart.romctrl >> 16) & 0x3F;
  if (cart.romctrl & (1u << 30)) {            // write direction: not supported; end after the command
    nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * cmddelay, cart_ev, 0);
    return;
  }
  if (bytes == 0) nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * cmddelay, cart_ev, 0);
  else nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * (cmddelay + 4), cart_ev, 1);
}

void Io::cart_schedule_receive() {
  if (cart.transfer_pos >= cart.transfer_len) return;
  const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5;
  u32 delay = 4;
  if (!(cart.transfer_pos & 0x1FF)) delay += (cart.romctrl >> 16) & 0x3F;
  nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * delay, cart_ev, 1);
}

void Io::cart_receive_word() {
  cart.fifo[(cart.fifo_head + cart.fifo_count) & 1] = nds_.cart ? nds_.cart->command_receive() : 0;
  cart.fifo_count++;
  cart.transfer_pos += 4;
  cart.romctrl |= 0x00800000u;                 // DRQ
  nds_.dma.check(Cpu::ARM9, dma::MODE9_CART);
  nds_.dma.check(Cpu::ARM7, dma::MODE7_CART);
  if (cart.fifo_count < 2) cart_schedule_receive(); else cart.late = true;
}

void Io::cart_end_transfer() {
  cart.romctrl &= ~0x80000000u;
  cart.transfer_pos = cart.transfer_len = 0;
  if (cart.auxspicnt & 0x4000) { request_irq(Cpu::ARM7, IRQ_CART_DONE); request_irq(Cpu::ARM9, IRQ_CART_DONE); }
}

void Io::cart_event(u32 param) {
  if (param == 0) cart_end_transfer(); else cart_receive_word();
}

u32 Io::cart_read_data() {
  const u32 v = cart.fifo[cart.fifo_head];
  if (cart.romctrl & (1u << 30)) return v;
  if (cart.fifo_count > 0) { cart.fifo_count--; cart.fifo_head ^= 1; }
  cart.romctrl &= ~0x00800000u;
  if (cart.transfer_pos < cart.transfer_len) {
    if (cart.late) { cart.late = false; cart_schedule_receive(); }
  } else {
    if (cart.fifo_count == 0) cart_end_transfer();
    else cart.romctrl |= 0x00800000u;
  }
  return v;
}

// ---- Wi-Fi ------------------------------------------------------------------
namespace {
constexpr u32 W_ID = 0x000, W_IF = 0x010, W_IE = 0x012, W_PowerUS = 0x036, W_Random = 0x044,
  W_Preamble = 0x0BC, W_USCount0 = 0x0F8, W_USCompare0 = 0x0F0, W_CmdCount = 0x118,
  W_BBCnt = 0x158, W_BBWrite = 0x15A, W_BBRead = 0x15C, W_BBBusy = 0x15E,
  W_RFData2 = 0x17C, W_RFData1 = 0x17E, W_RFBusy = 0x180, W_TXBusy = 0x0B6,
  W_CMDStat0 = 0x1D0, W_IFSet = 0x21C;
}

void Io::wifi_reset() {
  wifi_ram.fill(0); wifi_io.fill(0); wifi_bb.fill(0); wifi_bb_ro.fill(0); wifi_rf.fill(0);
  wifi_random = 1;
  auto fixed = [&](u32 id, u8 v) { wifi_bb[id] = v; wifi_bb_ro[id] = 1; };
  fixed(0x00, 0x6D);
  for (u32 id : {0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x27, 0x4D, 0x5E, 0x5F, 0x60, 0x61, 0x66}) fixed(id, 0x00);
  fixed(0x5D, 0x01); fixed(0x64, 0xFF);
  for (u32 id = 0x69; id < 0x100; ++id) fixed(id, 0x00);
  // Chip ID and RF type from the firmware header.
  u8 console = 0xFF;
  if (nds_.firmware.size() > 0x1D) { console = nds_.firmware[0x1D]; wifi_rf_version = nds_.firmware[0x1C]; }
  wifi_io[W_ID / 2] = (console == 0x20 || console == 0x35) ? 0xC340 : 0x1440;
  for (u32 a = 0x018; a < 0x01E; a += 2) wifi_io[a / 2] = 0xFFFF;   // MAC
  for (u32 a = 0x020; a < 0x026; a += 2) wifi_io[a / 2] = 0xFFFF;   // BSSID
  wifi_io[W_PowerUS / 2] = 0x0001;
}

u16 Io::wifi_read16(u32 addr) {
  if (!(powcnt2 & 2)) return 0;
  const u32 a = addr & 0x7FFE;
  if (a >= 0x4000 && a < 0x6000) { u16 v; std::memcpy(&v, &wifi_ram[a & 0x1FFE], 2); return v; }
  if (a >= 0x2000 && a < 0x4000) return 0xFFFF;
  const u32 r = a & 0xFFF;
  switch (r) {
  case W_Random:
    wifi_random = static_cast<u16>((wifi_random & 1) ^ (((wifi_random & 0x3FF) << 1) | (wifi_random >> 10)));
    return wifi_random;
  case W_Preamble: return wifi_io[r / 2] & 3;
  case W_USCount0: case W_USCount0 + 2: case W_USCount0 + 4: case W_USCount0 + 6: return 0;
  case W_USCompare0: case W_USCompare0 + 2: case W_USCompare0 + 4: case W_USCompare0 + 6: return 0;
  case W_CmdCount: return 0;
  case W_BBRead:
    if ((wifi_io[W_BBCnt / 2] & 0xF000) != 0x6000) return 0;
    return wifi_bb[wifi_io[W_BBCnt / 2] & 0xFF];
  case W_BBBusy: case W_RFBusy: return 0;
  case W_TXBusy: return wifi_io[r / 2] & 0x1F;
  case W_CMDStat0: case W_CMDStat0 + 2: case W_CMDStat0 + 4: case W_CMDStat0 + 6:
  case W_CMDStat0 + 8: case W_CMDStat0 + 10: case W_CMDStat0 + 12: case W_CMDStat0 + 14: {
    const u16 v = wifi_io[r / 2]; wifi_io[r / 2] = 0; return v;
  }
  default: return wifi_io[r / 2];
  }
}

void Io::wifi_write16(u32 addr, u16 value) {
  if (!(powcnt2 & 2)) return;
  const u32 a = addr & 0x7FFE;
  if (a >= 0x4000 && a < 0x6000) { std::memcpy(&wifi_ram[a & 0x1FFE], &value, 2); return; }
  if (a >= 0x2000 && a < 0x4000) return;
  const u32 r = a & 0xFFF;
  switch (r) {
  case W_ID: return;
  case W_IF: wifi_io[r / 2] &= ~value; return;
  case W_IFSet: wifi_io[W_IF / 2] |= value & 0xFBFF; return;
  case W_BBCnt:
    wifi_io[r / 2] = value;
    if ((value & 0xF000) == 0x5000) { const u32 id = value & 0xFF; if (!wifi_bb_ro[id]) wifi_bb[id] = static_cast<u8>(wifi_io[W_BBWrite / 2]); }
    return;
  case W_RFData2: {
    wifi_io[r / 2] = value;
    if (wifi_rf_version == 3) {
      const u32 id = (wifi_io[W_RFData1 / 2] >> 8) & 0x3F, cmd = value & 0xF;
      if (cmd == 6) wifi_io[W_RFData1 / 2] = static_cast<u16>((wifi_io[W_RFData1 / 2] & 0xFF00) | (wifi_rf[id] & 0xFF));
      else if (cmd == 5) wifi_rf[id] = wifi_io[W_RFData1 / 2] & 0xFF;
    } else {
      const u32 id = (value >> 2) & 0x1F;
      if (value & 0x80) { const u32 d = wifi_rf[id]; wifi_io[W_RFData1 / 2] = static_cast<u16>(d); wifi_io[r / 2] = static_cast<u16>((value & 0xFFFC) | ((d >> 16) & 3)); }
      else wifi_rf[id] = wifi_io[W_RFData1 / 2] | ((value & 3) << 16);
    }
    return;
  }
  default: wifi_io[r / 2] = value; return;
  }
}

// ---- register dispatch ----------------------------------------------------
u32 Io::read(Cpu cpu, u32 addr, u32 width) {
  bool handled = false;
  if (width == 32) { u32 v = read32_special(cpu, addr, handled); if (handled) return v; return read16(cpu, addr) | (static_cast<u32>(read16(cpu, addr + 2)) << 16); }
  if (width == 16) return read16(cpu, addr);
  return read8(cpu, addr);
}

void Io::write(Cpu cpu, u32 addr, u32 width, u32 value) {
  bool handled = false;
  if (width == 32) { write32_special(cpu, addr, value, handled); if (handled) return; write16(cpu, addr, static_cast<u16>(value)); write16(cpu, addr + 2, static_cast<u16>(value >> 16)); return; }
  if (width == 16) { write16(cpu, addr, static_cast<u16>(value)); return; }
  write8(cpu, addr, static_cast<u8>(value));
}

u32 Io::read32_special(Cpu cpu, u32 addr, bool& handled) {
  handled = true;
  CpuIo& c = cpu_io[ci(cpu)];
  switch (addr) {
  case 0x04000208: return c.ime;
  case 0x04000210: return c.ie;
  case 0x04000214: return c.if_;
  case 0x04100000: return ipc_fifo_recv(cpu);
  case 0x04100010: return cart_read_data();
  case 0x040001A4: return cart.romctrl;
  case 0x040000B0: case 0x040000BC: case 0x040000C8: case 0x040000D4: return nds_.dma.read_src(cpu, (addr - 0x040000B0) / 12);
  case 0x040000B4: case 0x040000C0: case 0x040000CC: case 0x040000D8: return nds_.dma.read_dst(cpu, (addr - 0x040000B4) / 12);
  case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC: return nds_.dma.read_cnt(cpu, (addr - 0x040000B8) / 12);
  case 0x040000E0: case 0x040000E4: case 0x040000E8: case 0x040000EC: return c.dma_fill[(addr - 0x040000E0) / 4];
  }
  handled = false;
  return 0;
}

void Io::write32_special(Cpu cpu, u32 addr, u32 value, bool& handled) {
  handled = true;
  CpuIo& c = cpu_io[ci(cpu)];
  switch (addr) {
  case 0x04000208: c.ime = value & 1; update_irq(cpu); return;
  case 0x04000210: c.ie = value; update_irq(cpu); return;
  case 0x04000214: c.if_ &= ~value; update_irq(cpu); return;
  case 0x04000188: ipc_fifo_send(cpu, value); return;
  case 0x040001A4: cart_write_romctrl(value); return;
  case 0x040000B0: case 0x040000BC: case 0x040000C8: case 0x040000D4: nds_.dma.write_src(cpu, (addr - 0x040000B0) / 12, value); return;
  case 0x040000B4: case 0x040000C0: case 0x040000CC: case 0x040000D8: nds_.dma.write_dst(cpu, (addr - 0x040000B4) / 12, value); return;
  case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC: nds_.dma.write_cnt(cpu, (addr - 0x040000B8) / 12, value); return;
  case 0x040000E0: case 0x040000E4: case 0x040000E8: case 0x040000EC: c.dma_fill[(addr - 0x040000E0) / 4] = value; return;
  }
  handled = false;
}

u32 Io::read16(Cpu cpu, u32 addr) {
  CpuIo& c = cpu_io[ci(cpu)];
  const bool a9 = cpu == Cpu::ARM9;
  if (!a9 && addr >= 0x04800000 && addr < 0x04810000) return wifi_read16(addr);
  switch (addr) {
  case 0x04000004: return dispstat[ci(cpu)];
  case 0x04000006: return vcount;
  case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: return timer_value(cpu, (addr - 0x04000100) / 4);
  case 0x04000102: case 0x04000106: case 0x0400010A: case 0x0400010E: return c.timers[(addr - 0x04000102) / 4].control;
  case 0x04000130: return keyinput;
  case 0x04000136: return a9 ? 0 : extkeyin;
  case 0x04000138: return a9 ? 0 : rtc_read();
  case 0x04000180: return c.ipc_sync;
  case 0x04000184: return ipc_fifo_cnt_read(cpu);
  case 0x040001A0: return cart.auxspicnt;
  case 0x040001A2: return cart.auxspidata;
  case 0x040001A8: case 0x040001AA: case 0x040001AC: case 0x040001AE: {
    const u32 i = addr - 0x040001A8; return static_cast<u16>(cart.cmd[i] | (cart.cmd[i + 1] << 8));
  }
  case 0x040001C0: return a9 ? 0 : spicnt;
  case 0x040001C2: return a9 ? 0 : spidata;
  case 0x04000204: return exmemcnt;
  case 0x040000BA: case 0x040000C6: case 0x040000D2: case 0x040000DE: return static_cast<u16>(nds_.dma.read_cnt(cpu, (addr - 0x040000BA) / 12) >> 16);
  case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC: return static_cast<u16>(nds_.dma.read_cnt(cpu, (addr - 0x040000B8) / 12));
  case 0x04000208: return static_cast<u16>(c.ime);
  case 0x04000210: return static_cast<u16>(c.ie);
  case 0x04000212: return static_cast<u16>(c.ie >> 16);
  case 0x04000214: return static_cast<u16>(c.if_);
  case 0x04000216: return static_cast<u16>(c.if_ >> 16);
  case 0x04000240: return a9 ? static_cast<u16>(vramcnt[0] | (vramcnt[1] << 8)) : static_cast<u16>(((vramcnt[2] >> 0) & 7) == 2 ? 1 : 0) | ((((vramcnt[3] >> 0) & 7) == 2 ? 2 : 0)) | (wramcnt << 8);
  case 0x04000300: return c.postflg;
  case 0x04000304: return a9 ? powcnt1 : powcnt2;
  case 0x04000500: return sound_cnt;
  case 0x04000504: return sound_bias;
  default: break;
  }
  if (addr >= 0x04000240 && addr < 0x0400024A && a9) {
    u32 i = addr - 0x04000240; u16 v = 0;
    auto get = [&](u32 k) -> u8 { if (k == 7) return wramcnt; return k < 9 ? vramcnt[k] : 0; };
    v = get(i) | (get(i + 1) << 8);
    return v;
  }
  if (!a9 && addr >= 0x04000400 && addr < 0x04000500) return sound_regs[addr - 0x04000400] | (sound_regs[addr - 0x04000400 + 1] << 8);
  return 0;
}

void Io::write16(Cpu cpu, u32 addr, u16 value) {
  CpuIo& c = cpu_io[ci(cpu)];
  const bool a9 = cpu == Cpu::ARM9;
  if (!a9 && addr >= 0x04800000 && addr < 0x04810000) { wifi_write16(addr, value); return; }
  switch (addr) {
  case 0x04000004: dispstat[ci(cpu)] = (dispstat[ci(cpu)] & 0x0007) | (value & 0xFFB8); return;
  case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: c.timers[(addr - 0x04000100) / 4].reload = value; return;
  case 0x04000102: case 0x04000106: case 0x0400010A: case 0x0400010E: timer_write_control(cpu, (addr - 0x04000102) / 4, value); return;
  case 0x04000132: return;                                   // KEYCNT
  case 0x04000134: return;                                   // RCNT
  case 0x04000138: if (!a9) rtc_write(value, false); return;
  case 0x04000180: ipc_sync_write(cpu, value); return;
  case 0x04000184: ipc_fifo_cnt_write(cpu, value); return;
  case 0x040001A0:
    if (nds_.cart) {
      if (cart.auxspicnt & ~value & 0x2000) nds_.cart->spi_release();
      else if (~cart.auxspicnt & value & 0x2000) nds_.cart->spi_select();
    }
    cart.auxspicnt = (cart.auxspicnt & 0x0080) | (value & 0xE043);
    return;
  case 0x040001A2: {
    if (!(cart.auxspicnt & 0x8000) || !(cart.auxspicnt & 0x2000)) return;
    const bool hold = cart.auxspicnt & 0x0040;
    if (nds_.cart) { cart.auxspidata = nds_.cart->spi_transfer(static_cast<u8>(value)); if (!hold) nds_.cart->spi_release(); }
    else cart.auxspidata = 0;
    return;
  }
  case 0x040001C0:
    if (a9) return;
    if ((spicnt & 0x8000) && !(value & 0x8000)) spi_release();
    spicnt = (spicnt & 0x0080) | (value & 0xCF03);
    return;
  case 0x040001C2: if (!a9) spi_write_data(static_cast<u8>(value)); return;
  case 0x04000204: {
    const u16 old = exmemcnt;
    exmemcnt = a9 ? ((exmemcnt & 0x6000) | (value & 0x88FF)) : ((exmemcnt & 0xFF80) | (value & 0x007F));
    if ((old ^ exmemcnt) & 0xFF) nds_.bus.update_gba_slot_timings();
    return;
  }
  case 0x040001A8: case 0x040001AA: case 0x040001AC: case 0x040001AE: {
    const u32 i = addr - 0x040001A8; cart.cmd[i] = static_cast<u8>(value); cart.cmd[i + 1] = static_cast<u8>(value >> 8); return;
  }
  case 0x04000206: return;                                   // WIFIWAITCNT
  case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC: {
    const int i = (addr - 0x040000B8) / 12; nds_.dma.write_cnt(cpu, i, (nds_.dma.read_cnt(cpu, i) & 0xFFFF0000) | value); return;
  }
  case 0x040000BA: case 0x040000C6: case 0x040000D2: case 0x040000DE: {
    const int i = (addr - 0x040000BA) / 12; nds_.dma.write_cnt(cpu, i, (nds_.dma.read_cnt(cpu, i) & 0x0000FFFF) | (static_cast<u32>(value) << 16)); return;
  }
  case 0x040000B0: case 0x040000B2: case 0x040000BC: case 0x040000BE: case 0x040000C8: case 0x040000CA: case 0x040000D4: case 0x040000D6: {
    const int i = (addr - 0x040000B0) / 12; const u32 old = nds_.dma.read_src(cpu, i);
    nds_.dma.write_src(cpu, i, ((addr - 0x040000B0) % 12) ? ((old & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((old & 0xFFFF0000) | value)); return;
  }
  case 0x040000B4: case 0x040000B6: case 0x040000C0: case 0x040000C2: case 0x040000CC: case 0x040000CE: case 0x040000D8: case 0x040000DA: {
    const int i = (addr - 0x040000B4) / 12; const u32 old = nds_.dma.read_dst(cpu, i);
    nds_.dma.write_dst(cpu, i, ((addr - 0x040000B4) % 12) ? ((old & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((old & 0xFFFF0000) | value)); return;
  }
  case 0x04000208: c.ime = value & 1; update_irq(cpu); return;
  case 0x04000210: c.ie = (c.ie & 0xFFFF0000) | value; update_irq(cpu); return;
  case 0x04000212: c.ie = (c.ie & 0x0000FFFF) | (static_cast<u32>(value) << 16); update_irq(cpu); return;
  case 0x04000214: c.if_ &= ~static_cast<u32>(value); update_irq(cpu); return;
  case 0x04000216: c.if_ &= ~(static_cast<u32>(value) << 16); update_irq(cpu); return;
  case 0x04000300:
    c.postflg |= value & 1; if (a9) c.postflg = (c.postflg & 1) | (value & 2);
    if (!a9 && (value >> 8)) write8(cpu, 0x04000301, static_cast<u8>(value >> 8));
    return;
  case 0x04000304: if (a9) powcnt1 = value; else powcnt2 = value; return;
  case 0x04000500: sound_cnt = value; return;
  case 0x04000504: sound_bias = value & 0x3FF; return;
  default: break;
  }
  if (addr >= 0x04000240 && addr < 0x0400024A && a9) { write8(cpu, addr, static_cast<u8>(value)); write8(cpu, addr + 1, static_cast<u8>(value >> 8)); return; }
  if (!a9 && addr >= 0x04000400 && addr < 0x04000500) { sound_regs[addr - 0x04000400] = static_cast<u8>(value); sound_regs[addr - 0x04000400 + 1] = static_cast<u8>(value >> 8); return; }
  if (a9 && addr >= 0x04000000 && addr < 0x04000070) return;   // 2D engine A regs: GPU owns these later
  if (a9 && addr >= 0x04001000 && addr < 0x04001070) return;   // engine B
  if (a9 && addr >= 0x04000320 && addr < 0x040006A4) { if (addr - 0x04000320 < 0x100) gx_regs[addr - 0x04000320] = static_cast<u8>(value); return; }
}

u8 Io::read8(Cpu cpu, u32 addr) {
  const bool a9 = cpu == Cpu::ARM9;
  if (a9 && addr >= 0x04000240 && addr < 0x0400024A) { u32 k = addr - 0x04000240; return k == 7 ? wramcnt : vramcnt[k]; }
  if (!a9 && addr == 0x04000241) return wramcnt;
  if (!a9 && addr == 0x04000240) return static_cast<u8>((((vramcnt[2] & 7) == 2) ? 1 : 0) | (((vramcnt[3] & 7) == 2) ? 2 : 0));
  if (addr == 0x04000300) return static_cast<u8>(cpu_io[ci(cpu)].postflg);
  if (!a9 && addr == 0x04000138) return static_cast<u8>(rtc_read());
  if (!a9 && addr == 0x04000139) return static_cast<u8>(rtc_read() >> 8);
  if (!a9 && addr == 0x040001C2) return spidata;
  const u16 v = read16(cpu, addr & ~1u);
  return static_cast<u8>((addr & 1) ? (v >> 8) : v);
}

void Io::write8(Cpu cpu, u32 addr, u8 value) {
  const bool a9 = cpu == Cpu::ARM9;
  if (a9 && addr >= 0x04000240 && addr < 0x0400024A) {
    const u32 k = addr - 0x04000240;
    if (k == 7) { if (wramcnt != (value & 3)) { wramcnt = value & 3; nds_.bus.update_wram(); } return; }
    if (vramcnt[k] != value) { vramcnt[k] = value; nds_.bus.update_vram(); }
    return;
  }
  if (addr == 0x04000300) { write16(cpu, addr, value); return; }
  if (!a9 && addr == 0x04000301) {                       // HALTCNT
    const u8 v = value & 0xC0;
    if (v == 0x80 || v == 0xC0) { CpuContext& ctx = nds_.cpu(Cpu::ARM7); ctx.halted = true; ctx.hot.cycle_budget = -1; }
    else if (v == 0x40) std::fprintf(stderr, "[io] GBA mode requested; not supported\n");
    return;
  }
  if (!a9 && addr == 0x04000138) { rtc_write(value, true); return; }
  if (!a9 && addr == 0x04000139) { rtc_write(static_cast<u16>((rtc.io & 0xFF) | (value << 8)), false); return; }
  if (!a9 && addr == 0x040001C2) { spi_write_data(value); return; }
  if (!a9 && addr >= 0x04000400 && addr < 0x04000500) { sound_regs[addr - 0x04000400] = value; return; }
  // Generic byte write: read-modify-write the halfword.
  const u32 base = addr & ~1u;
  u16 cur = read16(cpu, base);
  if (addr & 1) cur = static_cast<u16>((cur & 0x00FF) | (value << 8)); else cur = static_cast<u16>((cur & 0xFF00) | value);
  write16(cpu, base, cur);
}

} // namespace ds::io
