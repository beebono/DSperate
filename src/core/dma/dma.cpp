// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/dma/dma.h"
#include "core/nds.h"
#include "core/mem/timing.h"

#include <cstdio>

namespace ds::dma {

namespace {

// Main-RAM burst patterns (unit costs in system cycles; 0 ends the pattern).
// Generated from the run-length description of the hardware behaviour as
// measured by the melonDS project (GPLv3), see docs/ARCHITECTURE.md §4.
struct Burst { u8 data[256]; };
Burst make(std::initializer_list<std::pair<u8, u16>> rle) {
  Burst b{}; u32 i = 0;
  for (auto [v, n] : rle) for (u32 k = 0; k < n && i < 255; ++k) b.data[i++] = v;
  b.data[i] = 0; return b;
}
const Burst MRAM_DUMMY   = make({});
const Burst READ16       = make({{7,1},{3,1},{2,117},{7,1},{3,1},{2,117},{7,1},{3,1}});
const Burst READ32_N2    = make({{9,1},{4,1},{3,77},{9,1}});
const Burst READ32       = make({{9,1},{3,1},{2,116}});
const Burst WRITE16      = make({{8,1},{2,119}});
const Burst WRITE32_N2   = make({{10,1},{5,47}});
const Burst WRITE32      = make({{9,1},{7,34}});

} // namespace

Dma::Dma(NDS& nds) : nds_(nds) { reset(); }

void Dma::reset() {
  for (int i = 0; i < 8; ++i) { ch_[i] = Channel{}; ch_[i].cpu = i < 4 ? Cpu::ARM9 : Cpu::ARM7; ch_[i].num = i & 3; ch_[i].burst_table = MRAM_DUMMY.data; }
}

void Dma::write_src(Cpu cpu, int n, u32 v) { channel(cpu, n).src = v & (cpu == Cpu::ARM9 ? 0x0FFFFFFF : 0x07FFFFFF); }
void Dma::write_dst(Cpu cpu, int n, u32 v) { channel(cpu, n).dst = v & (cpu == Cpu::ARM9 ? 0x0FFFFFFF : 0x07FFFFFF); }

void Dma::write_cnt(Cpu cpu, int n, u32 v) {
  Channel& c = channel(cpu, n);
  const u32 old = c.cnt;
  c.cnt = v;
  if ((old & 0x80000000) || !(v & 0x80000000)) return;
  c.cur_src = c.src; c.cur_dst = c.dst;
  switch (v & 0x00600000) { case 0x00000000: c.dst_inc = 1; break; case 0x00200000: c.dst_inc = -1; break; case 0x00400000: c.dst_inc = 0; break; default: c.dst_inc = 1; break; }
  switch (v & 0x01800000) { case 0x00000000: c.src_inc = 1; break; case 0x00800000: c.src_inc = -1; break; case 0x01000000: c.src_inc = 0; break; default: c.src_inc = 1; break; }
  c.start_mode = (cpu == Cpu::ARM9) ? ((v >> 27) & 7) : (((v >> 28) & 3) | 0x10);
  if ((c.start_mode & 7) == 0) start(c);
  else if (c.start_mode == MODE9_CART || c.start_mode == MODE7_CART) { if (nds_.io.cart_drq()) start(c); }
  else if (c.start_mode == MODE9_GXFIFO) nds_.gpu3d.check_fifo_dma();
  if (c.start_mode == MODE9_GBA || c.start_mode == MODE7_WIFI_GBA)
    std::fprintf(stderr, "[dma] unimplemented start mode %02x on %s\n", c.start_mode, cpu == Cpu::ARM9 ? "arm9" : "arm7");
}

void Dma::start(Channel& c) {
  if (c.running) return;
  if (!c.in_progress) {
    const u32 mask = (c.cpu == Cpu::ARM9) ? 0x001FFFFF : (c.num == 3 ? 0x0000FFFF : 0x00003FFF);
    c.rem_count = c.cnt & mask;
    if (!c.rem_count) c.rem_count = mask + 1;
  }
  c.iter_count = (c.start_mode == MODE9_GXFIFO && c.rem_count > 112) ? 112 : c.rem_count;
  if ((c.cnt & 0x01800000) == 0x01800000) c.cur_src = c.src;
  if ((c.cnt & 0x00600000) == 0x00600000) c.cur_dst = c.dst;
  c.running = 2;
  c.in_progress = true;
  c.burst_table = MRAM_DUMMY.data; c.burst_pos = 0;
  nds_.sched.preempt(nds_.cpu(c.cpu));   // an immediate start stalls the CPU that issued it
}

void Dma::check(Cpu cpu, u32 mode) {
  for (int n = 0; n < 4; ++n) { Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000)) start(c); }
}
void Dma::stop(Cpu cpu, u32 mode) {
  for (int n = 0; n < 4; ++n) { Channel& c = channel(cpu, n); if (c.start_mode == mode) c.cnt &= ~0x80000000u; }
}
bool Dma::any_running(Cpu cpu) const {
  for (int n = 0; n < 4; ++n) if (channel(cpu, n).running) return true;
  return false;
}
bool Dma::in_mode(Cpu cpu, u32 mode) const {
  for (int n = 0; n < 4; ++n) { const Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000)) return true; }
  return false;
}

// Unit cost in system cycles (ARM7 clock); the caller doubles for the ARM9.
u32 Dma::unit_cycles(Channel& c, bool burst_start, bool word) {
  const mem::Timing& t = nds_.bus.timing();
  const bool a9 = c.cpu == Cpu::ARM9;
  const u32 src_rgn = t.region(a9, c.cur_src), dst_rgn = t.region(a9, c.cur_dst);
  u32 src_n, src_s, dst_n, dst_s;
  t.dma_cost(a9, c.cur_src, word, src_n, src_s);
  t.dma_cost(a9, c.cur_dst, word, dst_n, dst_s);
  const u32 MAIN = mem::REGION_MAIN_RAM;
  if (src_rgn == MAIN) {
    if (dst_rgn == MAIN) return word ? 18 : 16;
    if (c.src_inc > 0) {
      if (burst_start || c.burst_table[c.burst_pos] == 0) {
        c.burst_pos = 0;
        c.burst_table = word ? ((dst_n == 2) ? READ32_N2.data : READ32.data) : READ16.data;
      }
      return c.burst_table[c.burst_pos++];
    }
    if (word) return (((c.cur_src & 0x1F) == 0x1C) ? (dst_n == 2 ? 7 : 8) : 9) + (burst_start ? dst_n : dst_s);
    return (((c.cur_src & 0x1F) == 0x1E) ? 7 : 8) + (burst_start ? dst_n : dst_s);
  }
  if (dst_rgn == MAIN) {
    if (c.dst_inc > 0) {
      if (burst_start || c.burst_table[c.burst_pos] == 0) {
        c.burst_pos = 0;
        c.burst_table = word ? ((src_n == 2) ? WRITE32_N2.data : WRITE32.data) : WRITE16.data;
      }
      return c.burst_table[c.burst_pos++];
    }
    return (burst_start ? src_n : src_s) + (word ? 8 : 7);
  }
  if (src_rgn == dst_rgn && src_rgn != 0) return src_n + dst_n + 1;
  return burst_start ? src_n + dst_n : src_s + dst_s;
}

u32 Dma::run_channel(Channel& c, u32 budget) {
  const bool a9 = c.cpu == Cpu::ARM9;
  const bool word = c.cnt & (1u << 26);
  bool burst_start = (c.running == 2);
  c.running = 1;
  u32 used = 0;
  mem::Bus& bus = nds_.bus;
  while (c.iter_count > 0 && used < budget) {
    if (a9 && nds_.gpu3d.stalled()) break;      // a full GX FIFO stalls the ARM9's DMA too
    u32 cost = unit_cycles(c, burst_start, word);
    if (a9) cost <<= 1;
    used += cost;
    burst_start = false;
    if (word) bus.dma_write32(c.cpu, c.cur_dst, bus.dma_read32(c.cpu, c.cur_src));
    else      bus.dma_write16(c.cpu, c.cur_dst, bus.dma_read16(c.cpu, c.cur_src));
    const u32 step = word ? 4 : 2;
    c.cur_src += c.src_inc * step;
    c.cur_dst += c.dst_inc * step;
    c.iter_count--; c.rem_count--;
  }
  if (c.rem_count) {
    if (c.iter_count == 0) c.running = 0;       // wait for the next trigger
    return used;
  }
  if (!(c.cnt & (1u << 25))) c.cnt &= ~0x80000000u;   // not repeating: disable
  if (c.cnt & (1u << 30)) nds_.io.request_irq(c.cpu, io::IRQ_DMA0 + c.num);
  c.running = 0;
  c.in_progress = false;
  if (c.start_mode == MODE9_CART || c.start_mode == MODE7_CART) { if (nds_.io.cart_drq()) start(c); }
  return used;
}

u32 Dma::run(Cpu cpu, u32 budget) {
  u32 used = 0;
  for (int n = 0; n < 4 && used < budget; ++n) {
    Channel& c = channel(cpu, n);
    if (c.running) used += run_channel(c, budget - used);
  }
  return used;
}

} // namespace ds::dma
