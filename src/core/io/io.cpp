// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// I/O register file for both CPUs. Register semantics per GBATEK; melonDS is
// the behavioural reference where GBATEK is silent.
#include "core/io/io.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/dma/dma.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace ds::io {

namespace {
inline int ci(Cpu c) { return static_cast<int>(c); }
inline Cpu other(Cpu c) { return c == Cpu::ARM9 ? Cpu::ARM7 : Cpu::ARM9; }
}

Io::Io(NDS& nds) : nds_(nds) { reset(); }

static void ev_lcd_irq(NDS& nds, u32) { nds.io.flush_lcd_irq(); }
void Io::lcd_irq(Cpu cpu, u32 bit) {
  if (lcd_irq_delay == 0) { request_irq(cpu, bit); return; }
  lcd_irq_pending[ci(cpu)] |= 1u << bit;
  nds_.sched.schedule(EventId::LcdIrq, nds_.sched.now() + lcd_irq_delay, ev_lcd_irq);
}
void Io::flush_lcd_irq() {
  for (int i = 0; i < 2; ++i) for (u32 m = lcd_irq_pending[i]; m; m &= m - 1) request_irq(static_cast<Cpu>(i), static_cast<u32>(__builtin_ctz(m)));
  lcd_irq_pending[0] = lcd_irq_pending[1] = 0;
}

void Io::reset() {
  lcd_irq_pending[0] = lcd_irq_pending[1] = 0;
  if (const char* e = std::getenv("DS_LCD_IRQ_DELAY")) lcd_irq_delay = static_cast<u32>(std::atoi(e));
  if (const char* e = std::getenv("DS_CART_BULK")) cart_bulk_ = std::atoi(e) != 0;
  cpu_io[0] = CpuIo{}; cpu_io[1] = CpuIo{};
  dispstat[0] = dispstat[1] = 0; vcount = 0;
  wramcnt = 0; std::memset(vramcnt, 0, sizeof vramcnt);
  powcnt1 = 0; powcnt2 = 0; math = MathUnit{};
  keyinput = 0x03FF; extkeyin = 0x007F; keycnt[0] = keycnt[1] = 0;
  exmemcnt = 0;
  spicnt = 0; spidata = 0; spi_ready_at = 0;
  spi_fw = SpiFirmware{}; spi_tsc = SpiTouch{}; spi_pm = SpiPower{};
  mic_ = nullptr; mic_count_ = 0; mic_start_ = 0;
  rtc = Rtc{};
  if (rtc_host_clock_) rtc_seed();
  else if (rtc_power_lost_seen_) rtc.status1 = 0x02;   // a reboot, not a flat battery
  cart = Cart{};
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
  CpuIo& c = cpu_io[ci(cpu)];
  // A level source re-requests every time it is polled (the GX FIFO IRQ on
  // every pipe refill, 16 k a frame on Golden Sun). With the bit already set
  // and the CPU running, update_irq would recompute irq_pending from
  // unchanged IME/IE/IF -- every write to those recomputes it itself.
  if ((c.if_ & (1u << bit)) && !nds_.cpu(cpu).halted) return;
  c.if_ |= (1u << bit);
  update_irq(cpu);
}

// ---- display status -------------------------------------------------------
void Io::set_vcount(u16 line) {
  vcount = line;
  for (int i = 0; i < 2; ++i) {
    const u16 target = static_cast<u16>((dispstat[i] >> 8) | ((dispstat[i] & 0x80) << 1));
    if (line == target) {
      dispstat[i] |= 4;
      if (dispstat[i] & 0x20) lcd_irq(static_cast<Cpu>(i), IRQ_VCOUNT);
    } else {
      dispstat[i] &= ~4;
    }
  }
}
void Io::set_hblank(bool on) {
  for (int i = 0; i < 2; ++i) {
    if (on) { dispstat[i] |= 2; if (dispstat[i] & 0x10) lcd_irq(static_cast<Cpu>(i), IRQ_HBLANK); }
    else dispstat[i] &= ~2;
  }
}
void Io::set_vblank(bool on) {
  for (int i = 0; i < 2; ++i) {
    if (on) { dispstat[i] |= 1; if (dispstat[i] & 0x08) lcd_irq(static_cast<Cpu>(i), IRQ_VBLANK); }
    else dispstat[i] &= ~1;
  }
}

// ---- IPC ------------------------------------------------------------------
void Io::ipc_sync_write(Cpu cpu, u16 value) {
  static const bool log = std::getenv("DS_IPC_LOG") != nullptr;
  if (log) std::fprintf(stderr, "[ipcsync] %s out=%x irq=%d t=%llu frame %llu line %u pc %08x\n", cpu == Cpu::ARM9 ? "arm9" : "arm7", (value >> 8) & 0xF, (value >> 13) & 1,
                        (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.cpu(cpu).hot.regs[15]);
  CpuIo& me = cpu_io[ci(cpu)];
  CpuIo& them = cpu_io[ci(other(cpu))];
  me.ipc_sync = (me.ipc_sync & 0x000F) | (value & 0x4F00);
  them.ipc_sync = (them.ipc_sync & 0x4F00) | ((value >> 8) & 0xF);
  if ((value & 0x2000) && (them.ipc_sync & 0x4000)) request_irq(other(cpu), IRQ_IPC_SYNC);
  // The other CPU may be waiting on this with a tight timeout (the SDK boot
  // handshake: Pokémon Platinum, Zelda ST, DQIX hung white at quantum >= 2048).
  nds_.sched.yield(nds_.cpu(cpu));
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
  static const bool log = std::getenv("DS_IPC_LOG") != nullptr;
  if (log) std::fprintf(stderr, "[ipcfifo] %s send %08x frame %llu line %u\n", cpu == Cpu::ARM9 ? "arm9" : "arm7", value, (unsigned long long)nds_.frame_count, nds_.gpu.line());
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
  nds_.sched.schedule(id, t.start_time + cycles, timer_event, (static_cast<u32>(cpu) << 2) | idx);   // from the sample point, exact
}

void Io::timer_overflow(Cpu cpu, int idx) {
  Timer& t = cpu_io[ci(cpu)].timers[idx];
  t.counter = t.reload;
  t.start_time = nds_.sched.event_time();   // the overflow's nominal time, not the (late) slice end
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
  if (spicnt & 0x4000) request_irq(Cpu::ARM7, IRQ_SPI);
}

u64 Io::nds_sched_now() const { return nds_.sched.now(); }

void Io::set_cart_bulk(bool on) { if (!std::getenv("DS_CART_BULK")) cart_bulk_ = on; }

u8 Io::spi_transfer(u8 value) {
  const int dev = (spicnt >> 8) & 3;
  switch (dev) {
  case 0: {                                             // power management
    SpiPower& p = spi_pm;
    if (!p.hold) { p.hold = true; p.cmd = value; p.pos = 1; p.data = 0; return 0; }
    const u32 reg = p.cmd & 0x7F;
    if (p.cmd & 0x80) p.data = (reg < 8) ? p.regs[reg] : 0;
    else {
      if (reg < 8) p.regs[reg] = value;
      // Register 0 bit 6 is the system power line: the ARM7 drops it to shut
      // the console down. The firmware does this when it leaves its settings
      // pages, after the flash write that saves them -- so it is also the
      // moment the settings are complete on disk. Recorded rather than acted
      // on; the frontend decides what a power-off means (NDS::power_off).
      if (reg == 0 && (value & 0x40)) nds_.power_off = true;
      static const bool log = std::getenv("DS_MIC_LOG") != nullptr;
      if (log && (reg == 2 || reg == 3)) std::fprintf(stderr, "[mic] PMIC reg %u = %02x (frame %llu)\n", reg, value, (unsigned long long)nds_.frame_count);
      p.data = 0;
    }
    return p.data;
  }
  case 1: {                                             // firmware flash
    SpiFirmware& f = spi_fw;
    if (!f.hold) {
      f.hold = true; f.cmd = value; f.pos = 1; f.data = 0;
      if (value == 0x06) f.status |= 2;
      if (value == 0x04) f.status &= ~2;
      f.addr = 0;
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
    case 0x0A: {                                        // page write (WEL set): into the in-memory image
      auto& fw = nds_.firmware;
      if (f.pos < 4) { f.addr = (f.addr << 8) | value; f.data = 0; }
      else {
        if ((f.status & 2) && !fw.empty()) {
          const u32 off = f.addr % fw.size();
          if (fw[off] != value) { fw[off] = value; nds_.firmware_written(off); }
        }
        f.data = value;
        f.addr++;
      }
      f.pos++;
      return f.data;
    }
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
      case 1: sample = t.y; break;        // Y
      case 5: sample = t.x; break;        // X
      case 6: sample = mic_sample(); break;   // AUX: the microphone
      default: sample = 0; break;
      }
      // Bit 3 selects an 8-bit conversion (what the mic sampling loops use:
      // one byte less per sample). The result stream starts one clock after
      // the control byte, so the first data byte carries the top bits 7..1
      // of the conversion in bits 6..0 and the second byte the rest, MSB
      // first: 12-bit = 11..5 then 4..0 << 3, 8-bit = 7..1 then bit 0 << 7.
      if (value & 0x08) sample = static_cast<u16>((sample >> 4) << 4);
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

// ---- frontend input ---------------------------------------------------------

void Io::set_buttons(u32 pressed) {
  keyinput = static_cast<u16>(0x03FF & ~(pressed & 0x03FF));
  const u16 xy = static_cast<u16>(((pressed >> BTN_X) & 1) | (((pressed >> BTN_Y) & 1) << 1));
  extkeyin = static_cast<u16>((extkeyin & ~0x0003u) | (~xy & 0x0003u));
  update_key_irq();
}

void Io::set_touch(int x, int y, bool down) {
  if (!down) {                       // melonDS's release values; games test bit 6
    spi_tsc.x = 0; spi_tsc.y = 0xFFF;
    extkeyin |= 1u << 6;
    return;
  }
  x = x < 0 ? 0 : (x > 255 ? 255 : x);
  y = y < 0 ? 0 : (y > 191 ? 191 : y);
  spi_tsc.x = static_cast<u16>(x << 4);
  spi_tsc.y = static_cast<u16>(y << 4);
  extkeyin &= ~(1u << 6);
}

void Io::set_lid(bool closed) {
  const bool was = lid_closed();
  if (closed) extkeyin |= 1u << 7; else extkeyin &= ~(1u << 7);
  if (was && !closed) request_irq(Cpu::ARM7, IRQ_LID);
}

void Io::set_mic(const s16* samples, size_t count) {
  mic_ = samples; mic_count_ = count; mic_start_ = nds_.sched.now();
}

// The TSC's AUX input sits behind the PMIC's microphone amplifier, whose
// register 3 bits 0-1 pick the gain (x20/40/80/160). The frontend's samples
// are taken to be at the x20 level, so the higher gains scale up from there
// and clip the way the ADC would. Register 2 bit 0 is the amplifier enable;
// it is not honoured (melonDS does not either): a game that reads AUX with
// the amplifier off would get noise around the mid value on hardware, and
// silence there serves nobody. DS_MIC_LOG=1 prints the PMIC writes and a
// per-second count of AUX reads with their peak.
u16 Io::mic_sample() const {
  mic_used_ = true;
  static const bool log = std::getenv("DS_MIC_LOG") != nullptr;
  if (log) {
    static u64 reads = 0, last_frame = 0; static int peak = 0;
    ++reads;
    if (nds_.frame_count - last_frame >= 60) {
      std::fprintf(stderr, "[mic] %llu AUX reads in 60 frames, peak %d, buffer %zu samples, gain x%d (frame %llu)\n",
                   (unsigned long long)reads, peak, mic_count_, 20 << (spi_pm.regs[3] & 3), (unsigned long long)nds_.frame_count);
      reads = 0; peak = 0; last_frame = nds_.frame_count;
    }
    for (size_t k = 0; k < mic_count_; ++k) if (std::abs(int(mic_[k])) > peak) peak = std::abs(int(mic_[k]));
  }
  if (mic_count_ == 0) return 0x800;
  const u64 elapsed = nds_.sched.now() - mic_start_;
  size_t i = static_cast<size_t>((elapsed * mic_count_) / CYCLES_PER_FRAME);
  if (i >= mic_count_) i = mic_count_ - 1;
  const int gain = 1 << (spi_pm.regs[3] & 3);
  int v = 0x800 + ((static_cast<int>(mic_[i]) * gain) >> 4);
  return static_cast<u16>(v < 0 ? 0 : (v > 0xFFF ? 0xFFF : v));
}

// KEYCNT: bits 0-9 select keys, bit 14 enables the IRQ, bit 15 picks the
// condition (0 = any selected key held, 1 = all of them).
void Io::update_key_irq() {
  const u16 held = static_cast<u16>(~keyinput & 0x03FF);
  for (int c = 0; c < 2; ++c) {
    const u16 cnt = keycnt[c];
    if (!(cnt & 0x4000)) continue;
    const u16 sel = cnt & 0x03FF;
    const bool fire = (cnt & 0x8000) ? (sel != 0 && (held & sel) == sel) : ((held & sel) != 0);
    if (fire) request_irq(static_cast<Cpu>(c), IRQ_KEYPAD);
  }
}

void Io::spi_release() {
  spi_pm.hold = false; spi_fw.hold = false; spi_fw.addr = 0; spi_tsc.pos = 0;
}

u16 Io::spicnt_read_arm7() {
  if (!spi_busy()) { spi_poll_streak_ = 0; return spicnt_read(); }
  if (++spi_poll_streak_ >= SPI_POLL_STREAK && nds_.sched.idle_skip_enabled()) {
    CpuContext& a7 = nds_.cpu(Cpu::ARM7);
    if (nds_.sched.running() == &a7 && a7.hot.cycle_budget > 0) {
      // ARM9 cycles left until ready, in ARM7 cycles rounded up, capped at
      // the slice: past that the loop resumes in the next slice and either
      // the boundary sleep (Scheduler::arm7_spi_poll) or this catches it.
      const u64 rem9 = spi_ready_at - nds_.sched.now();
      s32 charge = static_cast<s32>((rem9 + 1) / 2);
      if (charge > a7.hot.cycle_budget) charge = a7.hot.cycle_budget;
      a7.hot.cycle_budget -= charge;
      prof::add(prof::C_CYC_A7_SPI_SLEPT, static_cast<u64>(charge) * 2);
      prof::add(prof::C_A7_SPI_SLEEP, 1);
    }
  }
  return spicnt_read();
}

void Io::spi_write_data(u8 value) {
  if (!(spicnt & 0x8000)) return;
  if (spi_busy()) return;
  spidata = spi_transfer(value);
  if (!(spicnt & 0x0800)) spi_release();
  const u32 delay = 8 * (8u << (spicnt & 3));
  spi_ready_at = nds_.sched.now() + delay * 2;
  if (spicnt & 0x4000) nds_.sched.schedule(EventId::Spi, spi_ready_at, spi_event, 0);
}

// ---- RTC (ARM7, bit-banged on 0x04000138) ----------------------------------
// The clock only free-runs when a frontend asks for it (start_rtc_clock); the
// harness leaves it at melonDS's frozen 2000-01-01 so runs stay comparable.
static u8 to_bcd(int v) { return static_cast<u8>(((v / 10) % 10) << 4 | (v % 10)); }
static int from_bcd(u8 v) { return (v >> 4) * 10 + (v & 0x0F); }

static void rtc_ev(NDS& nds, u32) { nds.io.rtc_event(); }

void Io::rtc_seed() {
  // DS_RTC_EPOCH pins the seed to a fixed Unix time: the clock still runs (so
  // the power-lost bit is clear and the firmware skips its setup wizard), but
  // two runs start from the same date and second and stay comparable.
  const char* pinned = std::getenv("DS_RTC_EPOCH");
  const std::time_t t = pinned ? static_cast<std::time_t>(std::strtoll(pinned, nullptr, 0))
                               : std::time(nullptr);
  std::tm lt{};
#if defined(_WIN32)
  localtime_s(&lt, &t);
#else
  localtime_r(&t, &lt);
#endif
  Rtc& r = rtc;
  r.datetime[0] = to_bcd(lt.tm_year % 100);     // the DS keeps two digits; 2000-2099
  r.datetime[1] = to_bcd(lt.tm_mon + 1);
  r.datetime[2] = to_bcd(lt.tm_mday);
  r.datetime[3] = static_cast<u8>(lt.tm_wday);  // 0 = Sunday, as the firmware reads it
  // Bit 1 of status1 is 24-hour mode; in 12-hour mode bit 6 of the hour byte
  // is the PM flag. We always run 24-hour, matching the melonDS default.
  r.datetime[4] = to_bcd(lt.tm_hour);
  r.datetime[5] = to_bcd(lt.tm_min);
  r.datetime[6] = to_bcd(std::min(lt.tm_sec, 59));   // no leap seconds on this chip
  r.status1 = 0x02;      // 24-hour mode, and *not* power-lost: see Rtc::ticking
  r.ticking = true;
  r.next_tick = nds_.sched.now() + ARM9_CLOCK_HZ;
  nds_.sched.schedule(EventId::Rtc, r.next_tick, rtc_ev, 0);
}

void Io::start_rtc_clock() {
  rtc_host_clock_ = true;
  rtc_seed();
}

// One second of carry. Written out rather than converting to a time_t and
// back because a game may have written its own date into the chip, and
// whatever it wrote is what has to advance -- including values no calendar
// would produce.
void Io::rtc_tick() {
  Rtc& r = rtc;
  int sec = from_bcd(r.datetime[6]) + 1;
  if (sec < 60) { r.datetime[6] = to_bcd(sec); return; }
  r.datetime[6] = 0;
  int min = from_bcd(r.datetime[5]) + 1;
  if (min < 60) { r.datetime[5] = to_bcd(min); return; }
  r.datetime[5] = 0;
  const u8 pm = r.datetime[4] & 0x40;
  int hour = from_bcd(static_cast<u8>(r.datetime[4] & 0x3F)) + 1;
  if (hour < 24) { r.datetime[4] = static_cast<u8>(to_bcd(hour) | pm); return; }
  r.datetime[4] = pm;
  r.datetime[3] = static_cast<u8>((r.datetime[3] + 1) % 7);
  const int year = from_bcd(r.datetime[0]), month = from_bcd(r.datetime[1]);
  // Years are two digits from 2000, so the century rule never bites: every
  // year divisible by 4 in 2000-2099 is a leap year.
  static const int len[13] = {31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int days = (month == 2 && (year % 4) == 0) ? 29 : len[month >= 1 && month <= 12 ? month : 1];
  int day = from_bcd(r.datetime[2]) + 1;
  if (day <= days) { r.datetime[2] = to_bcd(day); return; }
  r.datetime[2] = to_bcd(1);
  if (month < 12) { r.datetime[1] = to_bcd(month + 1); return; }
  r.datetime[1] = to_bcd(1);
  r.datetime[0] = to_bcd((year + 1) % 100);
}

void Io::rtc_event() {
  Rtc& r = rtc;
  if (!r.ticking) return;
  // Catch up whole seconds: a slice can overrun the due time, and a save
  // state reloaded into a fresh run can leave the mark far behind.
  do {
    rtc_tick();
    r.next_tick += ARM9_CLOCK_HZ;
  } while (r.next_tick <= nds_.sched.now());
  nds_.sched.schedule(EventId::Rtc, r.next_tick, rtc_ev, 0);
}

// Protocol per GBATEK; state and edge sampling mirror melonDS so traces match.
void Io::rtc_cmd_read() {
  Rtc& r = rtc;
  if ((r.cmd & 0x0F) != 0x06) return;
  switch (r.cmd & 0x70) {
  case 0x00: r.output[0] = r.status1; if (r.status1 & 0x80) rtc_power_lost_seen_ = true; r.status1 &= 0x0F; break;
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
  cart.fifo_count = 0; cart.late = false; cart.bulk = false;
  cart.romctrl &= ~0x00800000u;
  const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5;
  u32 cmddelay = 8 + (cart.romctrl & 0x1FFF);
  if (bytes) cmddelay += (cart.romctrl >> 16) & 0x3F;
  cart.event_armed = false;
  if (cart.romctrl & (1u << 30)) {            // write direction: not supported; end after the command
    nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * cmddelay, cart_ev, 0);
    cart.event_armed = true;
    return;
  }
  if (bytes == 0) {
    nds_.sched.schedule(EventId::Cart, nds_.sched.now() + 2 * xfer * cmddelay, cart_ev, 0);
    cart.event_armed = true;
    return;
  }
  // The first word: an event only if a DMA is waiting for it (see above).
  cart.next_word_at = nds_.sched.now() + 2 * xfer * (cmddelay + 4);
  if (cart_dma_armed()) {
    nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 1);
    cart.event_armed = true;
  }
}

// Cart words arrive on a fixed clock, and nothing observes one arriving: the
// CPU learns of them by reading ROMCTRL's DRQ bit or ROMDATA. So the words are
// produced at the read (`cart_catch_up`) instead of costing a scheduler event
// each. On SM64DS the cart was 634-1,033 events a frame -- half of everything
// we fire -- against DraStic's zero; it reads the card synchronously, and its
// whole scheduler runs ~1,390 events a frame. melonDS, which this model came
// from, schedules per word as we did.
//
// The exception is a cart-mode DMA channel: it is level-triggered on DRQ and
// has nothing to poll it, so while one is armed the per-word event stays and
// the behaviour is exactly what it was.
bool Io::cart_dma_armed() const { return nds_.dma.cart_armed(); }

u32 Io::cart_word_delay() const {
  const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5;
  u32 delay = 4;
  if (!(cart.transfer_pos & 0x1FF)) delay += (cart.romctrl >> 16) & 0x3F;
  return 2 * xfer * delay;
}

// `from` is the base the next word's delay counts from. On the lazy path that
// is the word's nominal arrival time, so a transfer keeps the card's cadence
// however late the reader looks. The event path passes now() instead, and must:
// a nominal deadline there can land in the past, fire again inside the same
// fire_due pass, and turn a clock-paced transfer into a burst -- measured at
// 95-104 differing frames against melonDS on SM64DS, against 16 for now().
void Io::cart_schedule_receive(u64 from) {
  if (cart.transfer_pos >= cart.transfer_len) return;
  cart.next_word_at = from + cart_word_delay();
  if (cart_dma_armed()) {
    nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 1);
    cart.event_armed = true;
  }
}

// Materialise every word whose time has passed. The FIFO holds two, and while
// it is full the transfer stalls (`late`) and resumes from the read that makes
// room -- as on hardware, and as melonDS's ROMDataLate does.
void Io::cart_catch_up_slow() {
  if (cart.event_armed || cart.late) return;
  while (cart.transfer_pos < cart.transfer_len && cart.fifo_count < 2 && nds_.sched.now() >= cart.next_word_at)
    cart_receive_word(cart.next_word_at);
  // A DMA armed part-way through a transfer takes the words from here on.
  if (!cart.event_armed && !cart.late && cart.transfer_pos < cart.transfer_len && cart_dma_armed()) {
    nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 1);
    cart.event_armed = true;
  }
}

void Io::cart_receive_word(u64 at) {
  cart.fifo[(cart.fifo_head + cart.fifo_count) & 1] = nds_.cart ? nds_.cart->command_receive() : 0;
  cart.fifo_count++;
  cart.transfer_pos += 4;
  cart.romctrl |= 0x00800000u;                 // DRQ
  // Only a cart-mode channel can be started by DRQ, and cart_drq() would
  // re-enter the catch-up; skip the scan when none is armed.
  if (cart_dma_armed()) {
    nds_.dma.check(Cpu::ARM9, dma::MODE9_CART);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_CART);
    // DS_CART_BULK: with a DMA taking the words, nothing observes their
    // cadence -- DRQ is consumed by the channel, ROMCTRL's busy bit holds to
    // the end either way, and the words the DMA has not yet read are not in
    // RAM on hardware either. So from here the words are produced as the DMA
    // reads them (cart_read_data) and the transfer keeps one event, at the
    // nominal time of the last word, for the done IRQ. What moves is where
    // the DMA's bus stall lands: all at once instead of a unit per word, so
    // the CPU interleave (and hashes) shift. Per 512-byte block this is 1
    // scheduler event and 1 slice instead of 128 of each; a loading screen
    // streams tens of blocks a frame. Opt-in until swept on the device.
    if (cart_bulk_ && cart.transfer_pos < cart.transfer_len) {
      const u32 xfer = (cart.romctrl & (1u << 27)) ? 8 : 5, gap2 = (cart.romctrl >> 16) & 0x3F;
      const u32 words = (cart.transfer_len - cart.transfer_pos) / 4;
      const u32 blocks = ((cart.transfer_len + 0x1FF) >> 9) - ((cart.transfer_pos + 0x1FF) >> 9);
      cart.next_word_at = at + 2 * xfer * (4 * words + gap2 * blocks);   // the last word's nominal arrival
      cart.bulk = true; cart.event_armed = true;
      nds_.sched.schedule(EventId::Cart, cart.next_word_at, cart_ev, 0);
      return;
    }
  }
  if (cart.fifo_count < 2) cart_schedule_receive(at); else cart.late = true;
}

void Io::cart_end_transfer() {
  cart.bulk = false;
  cart.romctrl &= ~0x80000000u;
  cart.transfer_pos = cart.transfer_len = 0;
  if (cart.auxspicnt & 0x4000) { request_irq(Cpu::ARM7, IRQ_CART_DONE); request_irq(Cpu::ARM9, IRQ_CART_DONE); }
}

void Io::cart_event(u32 param) {
  cart.event_armed = false;
  if (param == 0 && cart.bulk) {
    // The bulk transfer's end. If the DMA stopped taking words (disabled
    // mid-transfer), fall back to the exact model from the point reached.
    cart.bulk = false;
    if (cart.transfer_pos < cart.transfer_len) { if (cart.fifo_count < 2) cart_schedule_receive(nds_.sched.now()); else cart.late = true; return; }
    if (cart.fifo_count) return;   // ends with the read that empties the FIFO
  }
  if (param == 0) cart_end_transfer(); else cart_receive_word(nds_.sched.now());
}

u32 Io::cart_read_data() {
  cart_catch_up();
  const u32 v = cart.fifo[cart.fifo_head];
  if (cart.romctrl & (1u << 30)) return v;
  if (cart.fifo_count > 0) { cart.fifo_count--; cart.fifo_head ^= 1; }
  cart.romctrl &= ~0x00800000u;
  if (cart.transfer_pos < cart.transfer_len) {
    if (cart.bulk) {   // the next word, now: the DMA's re-trigger test (cart_drq) sees it
      cart.fifo[(cart.fifo_head + cart.fifo_count) & 1] = nds_.cart ? nds_.cart->command_receive() : 0;
      cart.fifo_count++; cart.transfer_pos += 4;
      cart.romctrl |= 0x00800000u;
    } else if (cart.late) { cart.late = false; cart_schedule_receive(nds_.sched.now()); }
  } else {
    if (cart.fifo_count == 0 && !cart.bulk) cart_end_transfer();   // bulk: the end event carries the IRQ at the nominal time
    else if (cart.fifo_count) cart.romctrl |= 0x00800000u;
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

// ---- divider / square root --------------------------------------------------
// Results appear after 18/34 (DIV, 32/64-bit) and 13 (SQRT) system cycles;
// the busy bit is set meanwhile. Edge cases follow the hardware as documented
// by GBATEK and melonDS: division by zero yields +/-1 with the numerator as
// remainder, and the most-negative / -1 overflow wraps.
//
// No event: the result is due at a recorded time and computed by the first
// read after it (see MathUnit). The Div/Sqrt event ids stay bound so a save
// state written while one was armed still loads; the handlers just settle.
static void ev_div(NDS& nds, u32)  { nds.io.div_settle(); }
static void ev_sqrt(NDS& nds, u32) { nds.io.sqrt_settle(); }

void Io::div_start() {
  math.div_pending = true;
  math.div_ready_at = nds_.sched.now() + (((math.divcnt & 3) == 0) ? 18 : 34) * 2;
}

void Io::div_done() {
  MathUnit& m = math;
  m.div_pending = false;
  m.divcnt &= ~0xC000;
  switch (m.divcnt & 3) {
  case 0: {
    const s32 num = static_cast<s32>(m.div_num), den = static_cast<s32>(m.div_den);
    if (den == 0) {
      m.div_quot = (num < 0) ? 0xFFFFFFFF00000001ull : 0x00000000FFFFFFFFull;
      m.div_rem = static_cast<u64>(static_cast<s64>(num));
    } else if (num == INT32_MIN && den == -1) {
      m.div_quot = 0x80000000ull;
    } else {
      m.div_quot = static_cast<u64>(static_cast<s64>(num / den));
      m.div_rem = static_cast<u64>(static_cast<s64>(num % den));
    }
    break;
  }
  default: {
    const s64 num = static_cast<s64>(m.div_num);
    const s64 den = ((m.divcnt & 3) == 2) ? static_cast<s64>(m.div_den) : static_cast<s64>(static_cast<s32>(m.div_den));
    if (den == 0) {
      m.div_quot = (num < 0) ? 1ull : ~0ull;
      m.div_rem = static_cast<u64>(num);
    } else if (num == INT64_MIN && den == -1) {
      m.div_quot = 0x8000000000000000ull; m.div_rem = 0;
    } else {
      m.div_quot = static_cast<u64>(num / den);
      m.div_rem = static_cast<u64>(num % den);
    }
    break;
  }
  }
  if (m.div_den == 0) m.divcnt |= 0x4000;
}

void Io::sqrt_start() {
  math.sqrt_pending = true;
  math.sqrt_ready_at = nds_.sched.now() + 13 * 2;
}

void Io::sqrt_done() {
  MathUnit& m = math;
  m.sqrt_pending = false;
  u64 val = (m.sqrtcnt & 1) ? m.sqrt_val : (m.sqrt_val & 0xFFFFFFFFull);
  // Digit-by-digit integer square root.
  u64 res = 0, rem = 0;
  const int nbits = (m.sqrtcnt & 1) ? 32 : 16;
  const int topshift = (m.sqrtcnt & 1) ? 62 : 30;
  for (int i = 0; i < nbits; ++i) {
    rem = (rem << 2) + ((val >> topshift) & 3);
    val <<= 2;
    res <<= 1;
    const u64 prod = (res << 1) + 1;
    if (rem >= prod) { rem -= prod; ++res; }
  }
  m.sqrt_res = static_cast<u32>(res);
}

void Io::set_irq_line(Cpu cpu, u32 bit, bool on) {
  if (on) request_irq(cpu, bit);
  else { cpu_io[ci(cpu)].if_ &= ~(1u << bit); update_irq(cpu); }
}

// ---- register dispatch ----------------------------------------------------
// Gpu3D owns r in [0x60,0x64), [0x320,0x3C0) and [0x400,0x6A4); Gpu owns
// r < 0x70 and [0x1000,0x1070); Spu owns [0x400,0x520). So [0x70,0x320) and
// everything from 0x1070 up belong to no subsystem -- which is where every
// register the recompiler's slow path actually hits lives (ROMCTRL, DIVCNT,
// SQRTCNT, IME/IE/IF, IPCSYNC, SPICNT, the timers and DMA, and ROMDATA at
// 0x04100010). One range test then replaces three probes for all of them.
static inline bool io_unowned(u32 r) { return (r - 0x70 < 0x2B0) || r >= 0x1070; }

// DS_IO_CENSUS=1: histogram of I/O accesses by address and CPU, printed at
// exit. Finds registers a game polls -- the cost of a poll loop is in the
// reads, which no write log sees.
namespace {
struct IoCensus {
  bool on = std::getenv("DS_IO_CENSUS") != nullptr;
  std::unordered_map<u32, u64> rd[2], wr[2];
  ~IoCensus() {
    if (!on) return;
    for (int c = 0; c < 2; ++c) {
      std::vector<std::pair<u64, u32>> v;
      for (auto& kv : rd[c]) v.push_back({kv.second, kv.first});
      std::sort(v.rbegin(), v.rend());
      std::fprintf(stderr, "[io] arm%d hottest READS:\n", c ? 7 : 9);
      for (size_t i = 0; i < v.size() && i < 12; ++i)
        std::fprintf(stderr, "[io]   %08x %12llu\n", v[i].second, (unsigned long long)v[i].first);
      v.clear();
      for (auto& kv : wr[c]) v.push_back({kv.second, kv.first});
      std::sort(v.rbegin(), v.rend());
      std::fprintf(stderr, "[io] arm%d hottest WRITES:\n", c ? 7 : 9);
      for (size_t i = 0; i < v.size() && i < 12; ++i)
        std::fprintf(stderr, "[io]   %08x %12llu\n", v[i].second, (unsigned long long)v[i].first);
    }
  }
};
IoCensus g_ioc;
}
bool Io::census_on() { return g_ioc.on; }

u32 Io::read(Cpu cpu, u32 addr, u32 width) {
  if (g_ioc.on) g_ioc.rd[cpu == Cpu::ARM9 ? 0 : 1][addr]++;
  if (cpu == Cpu::ARM7 && (addr & ~3u) != 0x040001C0) spi_poll_streak_ = 0;
  if (!io_unowned(addr - 0x04000000)) {
    if (cpu == Cpu::ARM9 && gpu::Gpu3D::owns_reg(addr)) return nds_.gpu3d.read(addr, width);
    if (cpu == Cpu::ARM9 && gpu::Gpu::owns_reg(addr)) return nds_.gpu.reg_read(addr, width);
    if (cpu == Cpu::ARM7 && spu::Spu::owns_reg(addr)) return nds_.spu.read(addr, width);
  }
  if (width == 32) { const Special s = read32_special(cpu, addr); if (s.handled) return s.value; return read16(cpu, addr) | (static_cast<u32>(read16(cpu, addr + 2)) << 16); }
  if (width == 16) return read16(cpu, addr);
  return read8(cpu, addr);
}

void Io::write(Cpu cpu, u32 addr, u32 width, u32 value) {
  if (g_ioc.on) g_ioc.wr[cpu == Cpu::ARM9 ? 0 : 1][addr]++;
  if (cpu == Cpu::ARM7) spi_poll_streak_ = 0;
  if (!io_unowned(addr - 0x04000000)) {
    if (cpu == Cpu::ARM9 && gpu::Gpu3D::owns_reg(addr)) { nds_.gpu3d.write(addr, width, value); return; }
    if (cpu == Cpu::ARM9 && gpu::Gpu::owns_reg(addr)) {
      static const bool dbg_gpureg = std::getenv("DS_DEBUG_GPUREG") != nullptr;
      if (dbg_gpureg && (addr & 0xFF) >= 0x50) std::fprintf(stderr, "[gpureg] frame %llu line %u write%u %08x = %08x\n", (unsigned long long)nds_.frame_count, nds_.gpu.line(), width, addr, value);
      nds_.gpu.reg_write(addr, width, value); return;
    }
    if (cpu == Cpu::ARM7 && spu::Spu::owns_reg(addr)) { nds_.spu.write(addr, width, value); return; }
  }
  if (width == 32) {
    if (cpu == Cpu::ARM9 && addr >= 0x04000240 && addr < 0x0400024A) { vramcnt_store(addr, value, 4); return; }
    if (write32_special(cpu, addr, value).handled) return;
    write16(cpu, addr, static_cast<u16>(value)); write16(cpu, addr + 2, static_cast<u16>(value >> 16)); return;
  }
  if (width == 16) { write16(cpu, addr, static_cast<u16>(value)); return; }
  write8(cpu, addr, static_cast<u8>(value));
}

Io::Special Io::read32_special(Cpu cpu, u32 addr) {
  CpuIo& c = cpu_io[ci(cpu)];
  if (addr == 0x04100000) return {ipc_fifo_recv(cpu), true};   // the two 0x0410xxxx ports, kept out of the dense index below
  if (addr == 0x04100010) return {cart_read_data(), true};
  switch ((addr - 0x04000000u) >> 2) {   // dense halfword/word index: GCC emits a jump table (a switch on the full address was a compare tree)
  case 0x82: return {c.ime, true};
  case 0x84: return {c.ie, true};
  case 0x85: return {c.if_, true};
  case 0x69: cart_catch_up(); return {cart.romctrl, true};
  case 0xa0: return {divcnt_read(), true};
  case 0xa4: return {static_cast<u32>(math.div_num), true};
  case 0xa5: return {static_cast<u32>(math.div_num >> 32), true};
  case 0xa6: return {static_cast<u32>(math.div_den), true};
  case 0xa7: return {static_cast<u32>(math.div_den >> 32), true};
  case 0xa8: div_settle(); return {static_cast<u32>(math.div_quot), true};
  case 0xa9: div_settle(); return {static_cast<u32>(math.div_quot >> 32), true};
  case 0xaa: div_settle(); return {static_cast<u32>(math.div_rem), true};
  case 0xab: div_settle(); return {static_cast<u32>(math.div_rem >> 32), true};
  case 0xac: return {sqrtcnt_read(), true};
  case 0xad: sqrt_settle(); return {math.sqrt_res, true};
  case 0xae: return {static_cast<u32>(math.sqrt_val), true};
  case 0xaf: return {static_cast<u32>(math.sqrt_val >> 32), true};
  case 0x2c: case 0x2f: case 0x32: case 0x35: return {nds_.dma.read_src(cpu, (addr - 0x040000B0) / 12), true};
  case 0x2d: case 0x30: case 0x33: case 0x36: return {nds_.dma.read_dst(cpu, (addr - 0x040000B4) / 12), true};
  case 0x2e: case 0x31: case 0x34: case 0x37: return {nds_.dma.read_cnt(cpu, (addr - 0x040000B8) / 12), true};
  case 0x38: case 0x39: case 0x3a: case 0x3b: return {c.dma_fill[(addr - 0x040000E0) / 4], true};
  }
  return {0, false};
}

Io::Special Io::write32_special(Cpu cpu, u32 addr, u32 value) {
  CpuIo& c = cpu_io[ci(cpu)];
  switch ((addr - 0x04000000u) >> 2) {   // dense halfword/word index: GCC emits a jump table (a switch on the full address was a compare tree)
  case 0x82: c.ime = value & 1; update_irq(cpu); return {0, true};
  case 0x84: c.ie = value; update_irq(cpu); return {0, true};
  case 0x85: c.if_ &= ~value; update_irq(cpu); if (cpu == Cpu::ARM9) nds_.gpu3d.check_fifo_irq_fast(); return {0, true};
  case 0x62: ipc_fifo_send(cpu, value); return {0, true};
  case 0x69: cart_write_romctrl(value); return {0, true};
  case 0xa0: math.divcnt = value & 0x3; div_start(); return {0, true};
  case 0xa4: math.div_num = (math.div_num & 0xFFFFFFFF00000000ull) | value; div_start(); return {0, true};
  case 0xa5: math.div_num = (math.div_num & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32); div_start(); return {0, true};
  case 0xa6: math.div_den = (math.div_den & 0xFFFFFFFF00000000ull) | value; div_start(); return {0, true};
  case 0xa7: math.div_den = (math.div_den & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32); div_start(); return {0, true};
  case 0xac: math.sqrtcnt = value & 0x1; sqrt_start(); return {0, true};
  case 0xae: math.sqrt_val = (math.sqrt_val & 0xFFFFFFFF00000000ull) | value; sqrt_start(); return {0, true};
  case 0xaf: math.sqrt_val = (math.sqrt_val & 0xFFFFFFFFull) | (static_cast<u64>(value) << 32); sqrt_start(); return {0, true};
  case 0x2c: case 0x2f: case 0x32: case 0x35: nds_.dma.write_src(cpu, (addr - 0x040000B0) / 12, value); return {0, true};
  case 0x2d: case 0x30: case 0x33: case 0x36: nds_.dma.write_dst(cpu, (addr - 0x040000B4) / 12, value); return {0, true};
  case 0x2e: case 0x31: case 0x34: case 0x37: nds_.dma.write_cnt(cpu, (addr - 0x040000B8) / 12, value); return {0, true};
  case 0x38: case 0x39: case 0x3a: case 0x3b: c.dma_fill[(addr - 0x040000E0) / 4] = value; return {0, true};
  }
  return {0, false};
}

u32 Io::read16(Cpu cpu, u32 addr) {
  CpuIo& c = cpu_io[ci(cpu)];
  const bool a9 = cpu == Cpu::ARM9;
  if (!a9 && addr >= 0x04800000 && addr < 0x04810000) return wifi_read16(addr);
  switch ((addr - 0x04000000u) >> 1) {   // dense halfword/word index: GCC emits a jump table (a switch on the full address was a compare tree)
  case 0x2: return dispstat[ci(cpu)];
  case 0x3: return vcount;
  case 0x80: case 0x82: case 0x84: case 0x86: return timer_value(cpu, (addr - 0x04000100) / 4);
  case 0x81: case 0x83: case 0x85: case 0x87: return c.timers[(addr - 0x04000102) / 4].control;
  case 0x98: return keyinput;
  case 0x99: return keycnt[ci(cpu)];
  case 0x9b: return a9 ? 0 : extkeyin;
  case 0x9c: return a9 ? 0 : rtc_read();
  case 0xc0: return c.ipc_sync;
  case 0xc2: return ipc_fifo_cnt_read(cpu);
  case 0xd0: return cart.auxspicnt;
  case 0xd1: return cart.auxspidata;
  case 0xd4: case 0xd5: case 0xd6: case 0xd7: {
    const u32 i = addr - 0x040001A8; return static_cast<u16>(cart.cmd[i] | (cart.cmd[i + 1] << 8));
  }
  case 0xe0: return a9 ? 0 : spicnt_read_arm7();
  case 0xe1: return a9 ? 0 : spidata;
  case 0x102: return exmemcnt;
  case 0x5d: case 0x63: case 0x69: case 0x6f: return static_cast<u16>(nds_.dma.read_cnt(cpu, (addr - 0x040000BA) / 12) >> 16);
  case 0x5c: case 0x62: case 0x68: case 0x6e: return static_cast<u16>(nds_.dma.read_cnt(cpu, (addr - 0x040000B8) / 12));
  case 0x104: return static_cast<u16>(c.ime);
  case 0x108: return static_cast<u16>(c.ie);
  case 0x109: return static_cast<u16>(c.ie >> 16);
  case 0x10a: return static_cast<u16>(c.if_);
  case 0x10b: return static_cast<u16>(c.if_ >> 16);
  case 0x120: return a9 ? static_cast<u16>(vramcnt[0] | (vramcnt[1] << 8)) : static_cast<u16>(((vramcnt[2] >> 0) & 7) == 2 ? 1 : 0) | ((((vramcnt[3] >> 0) & 7) == 2 ? 2 : 0)) | (wramcnt << 8);
  case 0x180: return c.postflg;
  case 0x182: return a9 ? powcnt1 : powcnt2;
  case 0x140: return divcnt_read();
  case 0x158: return sqrtcnt_read();
  default: break;
  }
  if (a9 && addr >= 0x04000290 && addr < 0x040002C0) { const u32 v = read32_special(cpu, addr & ~3u).value; return static_cast<u16>((addr & 2) ? v >> 16 : v); }
  if (addr >= 0x04000240 && addr < 0x0400024A && a9) {
    // 0x240-0x246 VRAMCNT A-G, 0x247 WRAMCNT, 0x248-0x249 VRAMCNT H-I.
    const u32 i = addr - 0x04000240;
    auto get = [&](u32 k) -> u8 { if (k == 7) return wramcnt; if (k > 9) return 0; return vramcnt[k < 7 ? k : k - 1]; };
    return static_cast<u16>(get(i) | (get(i + 1) << 8));
  }
  return 0;
}

void Io::write16(Cpu cpu, u32 addr, u16 value) {
  CpuIo& c = cpu_io[ci(cpu)];
  const bool a9 = cpu == Cpu::ARM9;
  if (!a9 && addr >= 0x04800000 && addr < 0x04810000) { wifi_write16(addr, value); return; }
  switch ((addr - 0x04000000u) >> 1) {   // dense halfword/word index: GCC emits a jump table (a switch on the full address was a compare tree)
  case 0x2: dispstat[ci(cpu)] = (dispstat[ci(cpu)] & 0x0007) | (value & 0xFFB8); return;
  case 0x80: case 0x82: case 0x84: case 0x86: c.timers[(addr - 0x04000100) / 4].reload = value; return;
  case 0x81: case 0x83: case 0x85: case 0x87: timer_write_control(cpu, (addr - 0x04000102) / 4, value); return;
  case 0x99: keycnt[ci(cpu)] = value; update_key_irq(); return;
  case 0x9a: return;                                   // RCNT
  case 0x9c: if (!a9) rtc_write(value, false); return;
  case 0xc0: ipc_sync_write(cpu, value); return;
  case 0xc2: ipc_fifo_cnt_write(cpu, value); return;
  case 0xd0:
    if (nds_.cart) {
      if (cart.auxspicnt & ~value & 0x2000) nds_.cart->spi_release();
      else if (~cart.auxspicnt & value & 0x2000) nds_.cart->spi_select();
    }
    cart.auxspicnt = (cart.auxspicnt & 0x0080) | (value & 0xE043);
    return;
  case 0xd1: {
    if (!(cart.auxspicnt & 0x8000) || !(cart.auxspicnt & 0x2000)) return;
    const bool hold = cart.auxspicnt & 0x0040;
    if (nds_.cart) { cart.auxspidata = nds_.cart->spi_transfer(static_cast<u8>(value)); if (!hold) nds_.cart->spi_release(); }
    else cart.auxspidata = 0;
    return;
  }
  case 0xe0:
    if (a9) return;
    if ((spicnt & 0x8000) && !(value & 0x8000)) spi_release();
    spicnt = value & 0xCF03;
    if ((spicnt & 0x4000) && spi_busy()) nds_.sched.schedule(EventId::Spi, spi_ready_at, spi_event, 0);   // IRQ enabled mid-transfer
    return;
  case 0xe1: if (!a9) spi_write_data(static_cast<u8>(value)); return;
  case 0x102: {
    const u16 old = exmemcnt;
    exmemcnt = a9 ? ((exmemcnt & 0x6000) | (value & 0x88FF)) : ((exmemcnt & 0xFF80) | (value & 0x007F));
    if ((old ^ exmemcnt) & 0xFF) nds_.bus.update_gba_slot_timings();
    return;
  }
  case 0xd4: case 0xd5: case 0xd6: case 0xd7: {
    const u32 i = addr - 0x040001A8; cart.cmd[i] = static_cast<u8>(value); cart.cmd[i + 1] = static_cast<u8>(value >> 8); return;
  }
  case 0x103: return;                                   // WIFIWAITCNT
  case 0x140: if (a9) { math.divcnt = value & 3; div_start(); } return;
  case 0x158: if (a9) { math.sqrtcnt = value & 1; sqrt_start(); } return;
  case 0x5c: case 0x62: case 0x68: case 0x6e: {
    const int i = (addr - 0x040000B8) / 12; nds_.dma.write_cnt(cpu, i, (nds_.dma.read_cnt(cpu, i) & 0xFFFF0000) | value); return;
  }
  case 0x5d: case 0x63: case 0x69: case 0x6f: {
    const int i = (addr - 0x040000BA) / 12; nds_.dma.write_cnt(cpu, i, (nds_.dma.read_cnt(cpu, i) & 0x0000FFFF) | (static_cast<u32>(value) << 16)); return;
  }
  case 0x58: case 0x59: case 0x5e: case 0x5f: case 0x64: case 0x65: case 0x6a: case 0x6b: {
    const int i = (addr - 0x040000B0) / 12; const u32 old = nds_.dma.read_src(cpu, i);
    nds_.dma.write_src(cpu, i, ((addr - 0x040000B0) % 12) ? ((old & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((old & 0xFFFF0000) | value)); return;
  }
  case 0x5a: case 0x5b: case 0x60: case 0x61: case 0x66: case 0x67: case 0x6c: case 0x6d: {
    const int i = (addr - 0x040000B4) / 12; const u32 old = nds_.dma.read_dst(cpu, i);
    nds_.dma.write_dst(cpu, i, ((addr - 0x040000B4) % 12) ? ((old & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((old & 0xFFFF0000) | value)); return;
  }
  case 0x104: c.ime = value & 1; update_irq(cpu); return;
  case 0x108: c.ie = (c.ie & 0xFFFF0000) | value; update_irq(cpu); return;
  case 0x109: c.ie = (c.ie & 0x0000FFFF) | (static_cast<u32>(value) << 16); update_irq(cpu); return;
  case 0x10a: c.if_ &= ~static_cast<u32>(value); update_irq(cpu); if (a9) nds_.gpu3d.check_fifo_irq_fast(); return;
  case 0x10b: c.if_ &= ~(static_cast<u32>(value) << 16); update_irq(cpu); if (a9) nds_.gpu3d.check_fifo_irq_fast(); return;
  case 0x180:
    c.postflg |= value & 1; if (a9) c.postflg = (c.postflg & 1) | (value & 2);
    if (!a9 && (value >> 8)) write8(cpu, 0x04000301, static_cast<u8>(value >> 8));
    return;
  case 0x182: if (a9) { powcnt1 = value & 0x820F; nds_.gpu.set_powcnt(powcnt1); } else { powcnt2 = value & 0x0003; nds_.spu.set_powcnt2(powcnt2); } return;
  default: break;
  }
  if (addr >= 0x04000240 && addr < 0x0400024A && a9) { vramcnt_store(addr, value, 2); return; }
  if (a9 && ((addr >= 0x04000290 && addr < 0x040002A0) || addr == 0x040002B8 || addr == 0x040002BA || addr == 0x040002BC || addr == 0x040002BE)) {
    const u32 cur = read32_special(cpu, addr & ~3u).value;
    write32_special(cpu, addr & ~3u, (addr & 2) ? ((cur & 0x0000FFFF) | (static_cast<u32>(value) << 16)) : ((cur & 0xFFFF0000) | value));
    return;
  }
}

u8 Io::read8(Cpu cpu, u32 addr) {
  const bool a9 = cpu == Cpu::ARM9;
  if (a9 && addr >= 0x04000240 && addr < 0x0400024A) { const u32 k = addr - 0x04000240; return k == 7 ? wramcnt : vramcnt[k < 7 ? k : k - 1]; }
  if (!a9 && addr == 0x04000241) return wramcnt;
  if (!a9 && addr == 0x04000240) return static_cast<u8>((((vramcnt[2] & 7) == 2) ? 1 : 0) | (((vramcnt[3] & 7) == 2) ? 2 : 0));
  if (addr == 0x04000300) return static_cast<u8>(cpu_io[ci(cpu)].postflg);
  if (!a9 && addr == 0x04000138) return static_cast<u8>(rtc_read());
  if (!a9 && addr == 0x04000139) return static_cast<u8>(rtc_read() >> 8);
  if (!a9 && addr == 0x040001C2) return spidata;
  const u16 v = read16(cpu, addr & ~1u);
  return static_cast<u8>((addr & 1) ? (v >> 8) : v);
}

// VRAMCNT A-I and WRAMCNT, `n` bytes from `addr`. A 32-bit store to 0x240
// (the usual way banks A-D are programmed) changes up to four banks at once;
// the remap -- 8 K page-table entries per CPU plus a 2D catch-up and a worker
// join -- runs once for the store, not once per byte.
void Io::vramcnt_store(u32 addr, u32 value, u32 n) {
  static const bool dbg_vramcnt = std::getenv("DS_DEBUG_VRAMCNT") != nullptr;
  bool vram_changed = false;
  for (u32 i = 0; i < n; ++i, ++addr, value >>= 8) {
    if (addr < 0x04000240 || addr >= 0x0400024A) continue;
    const u32 k = addr - 0x04000240;
    const u8 b = static_cast<u8>(value);
    if (k == 7) { if (wramcnt != (b & 3)) { wramcnt = b & 3; nds_.bus.update_wram(); } continue; }
    const u32 bank = k < 7 ? k : k - 1;                 // 0x248/0x249 are banks H/I
    if (dbg_vramcnt) std::fprintf(stderr, "[vramcnt] %c = %02x frame %llu line %u pc %08x\n", 'A' + bank, b, (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.cpu(Cpu::ARM9).hot.regs[15]);
    if (vramcnt[bank] != b) { vramcnt[bank] = b; vram_changed = true; }
  }
  if (vram_changed) nds_.bus.update_vram();
}

void Io::write8(Cpu cpu, u32 addr, u8 value) {
  const bool a9 = cpu == Cpu::ARM9;
  if (a9 && addr >= 0x04000240 && addr < 0x0400024A) { vramcnt_store(addr, value, 1); return; }
  if (addr == 0x04000300) { write16(cpu, addr, value); return; }
  if (!a9 && addr == 0x04000301) {                       // HALTCNT
    const u8 v = value & 0xC0;
    if (v == 0x80 || v == 0xC0) nds_.cpu(Cpu::ARM7).halted = true;   // the run loop ends the slice
    else if (v == 0x40) std::fprintf(stderr, "[io] GBA mode requested; not supported\n");
    return;
  }
  if (!a9 && addr == 0x04000138) { rtc_write(value, true); return; }
  if (!a9 && addr == 0x04000139) { rtc_write(static_cast<u16>((rtc.io & 0xFF) | (value << 8)), false); return; }
  if (!a9 && addr == 0x040001C2) { spi_write_data(value); return; }
  // Generic byte write: read-modify-write the halfword.
  const u32 base = addr & ~1u;
  u16 cur = read16(cpu, base);
  if (addr & 1) cur = static_cast<u16>((cur & 0x00FF) | (value << 8)); else cur = static_cast<u16>((cur & 0xFF00) | value);
  write16(cpu, base, cur);
}


template <class S> void Io::sync_state(S& s) {
  s.begin("IO  ");
  s.fields(lcd_irq_pending, dispstat, vcount, wramcnt, vramcnt, powcnt1, powcnt2, keyinput, extkeyin, keycnt, exmemcnt, spicnt, spidata, arm7_bios_prot);
  for (CpuIo& c : cpu_io) {
    s.fields(c.ime, c.ie, c.if_, c.ipc_sync, c.ipc_fifo_cnt, c.fifo_out.data, c.fifo_out.head, c.fifo_out.count, c.fifo_out.last, c.postflg, c.dma_fill);
    for (Timer& t : c.timers) s.fields(t.reload, t.control, t.counter, t.start_time);
  }
  s.fields(spi_fw.hold, spi_fw.cmd, spi_fw.pos, spi_fw.addr, spi_fw.status, spi_fw.data);
  s.fields(spi_tsc.hold, spi_tsc.pos, spi_tsc.cmd, spi_tsc.sample, spi_tsc.data, spi_tsc.x, spi_tsc.y);
  s.fields(spi_pm.hold, spi_pm.pos, spi_pm.cmd, spi_pm.regs, spi_pm.data);
  s.fields(rtc.io, rtc.input, rtc.input_bit, rtc.input_pos, rtc.output, rtc.output_bit, rtc.output_pos, rtc.cmd,
           rtc.status1, rtc.status2, rtc.datetime, rtc.alarm1, rtc.alarm2, rtc.clock_adjust, rtc.free_reg);
  s.fields(cart.auxspicnt, cart.auxspidata, cart.romctrl, cart.cmd, cart.transfer_pos, cart.transfer_len, cart.fifo_count, cart.fifo, cart.fifo_head,
           cart.late, cart.next_word_at, cart.event_armed);
  s.fields(math.divcnt, math.sqrtcnt, math.div_num, math.div_den, math.div_quot, math.div_rem, math.sqrt_val, math.sqrt_res);
  s.fields(wifi_ram, wifi_io, wifi_bb, wifi_bb_ro, wifi_rf, wifi_rf_version, wifi_random);
  s.fields(math.div_ready_at, math.sqrt_ready_at, math.div_pending, math.sqrt_pending, spi_ready_at, cart.bulk);   // appended: older states leave them at rest
  s.end();
  if constexpr (S::reading) {
    mic_ = nullptr; mic_count_ = 0; mic_start_ = 0;   // the frontend hands a new buffer every frame
    for (int i = 0; i < 4; ++i) {
      nds_.sched.rebind(static_cast<EventId>(static_cast<int>(EventId::Timer0) + i), timer_event);
      nds_.sched.rebind(static_cast<EventId>(static_cast<int>(EventId::Timer7_0) + i), timer_event);
    }
    nds_.sched.rebind(EventId::Spi, spi_event);
    nds_.sched.rebind(EventId::Cart, cart_ev);
    nds_.sched.rebind(EventId::Div, ev_div);
    nds_.sched.rebind(EventId::Sqrt, ev_sqrt);
    nds_.sched.rebind(EventId::LcdIrq, ev_lcd_irq);
    // The clock is a property of the session, not of the state: a state saved
    // on a console with a running clock must not stop it on a harness run, or
    // start one there. Whatever this run was set up with keeps going, rebased
    // onto the restored timeline.
    if (rtc.ticking) { rtc.next_tick = nds_.sched.now() + ARM9_CLOCK_HZ; nds_.sched.schedule(EventId::Rtc, rtc.next_tick, rtc_ev, 0); }
    else nds_.sched.cancel(EventId::Rtc);   // the saving session's clock event, armed in the state but with no handler here
  }
}
template void Io::sync_state<state::Writer>(state::Writer&);
template void Io::sync_state<state::Reader>(state::Reader&);

} // namespace ds::io
