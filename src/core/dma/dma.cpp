// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/dma/dma.h"
#include "core/state/state.h"
#include "core/nds.h"
#include "core/profile.h"
#include "core/mem/timing.h"

#include <cstdio>
#include <cstring>

namespace ds::dma {

namespace {

// Main-RAM burst patterns (unit costs in system cycles; 0 ends the pattern).
// Generated from the run-length description of the hardware behaviour as
// measured by the melonDS project (GPLv3).
// `len` is the pattern's period and `prefix` its running sum, so the cost of
// a run of units can be closed-form instead of walked one at a time.
struct Burst { u8 data[256]; u32 len; u32 prefix[257]; };
Burst make(std::initializer_list<std::pair<u8, u16>> rle) {
  Burst b{}; u32 i = 0;
  for (auto [v, n] : rle) for (u32 k = 0; k < n && i < 255; ++k) b.data[i++] = v;
  b.data[i] = 0; b.len = i;
  b.prefix[0] = 0;
  for (u32 k = 0; k < i; ++k) b.prefix[k + 1] = b.prefix[k] + b.data[k];
  return b;
}
const Burst MRAM_DUMMY   = make({});
const Burst READ16       = make({{7,1},{3,1},{2,117},{7,1},{3,1},{2,117},{7,1},{3,1}});
const Burst READ32_N2    = make({{9,1},{4,1},{3,77},{9,1}});
const Burst READ32       = make({{9,1},{3,1},{2,116}});
const Burst WRITE16      = make({{8,1},{2,119}});
const Burst WRITE32_N2   = make({{10,1},{5,47}});
const Burst WRITE32      = make({{9,1},{7,34}});

// Shorter runs than this are left to the per-unit loop: the closed-form set-up
// costs more than it saves on them, and scenes whose DMA is mostly short runs
// (sm64) measured ~0.4 % worse without the floor while Golden Sun, whose
// per-scanline stream runs 128 units, is unaffected by it.
constexpr u32 kBulkMin = 16;

const Burst* burst_meta(const u8* t) {
  if (t == READ32.data)     return &READ32;
  if (t == READ32_N2.data)  return &READ32_N2;
  if (t == READ16.data)     return &READ16;
  if (t == WRITE32.data)    return &WRITE32;
  if (t == WRITE32_N2.data) return &WRITE32_N2;
  if (t == WRITE16.data)    return &WRITE16;
  return nullptr;
}

} // namespace

Dma::Dma(NDS& nds) : nds_(nds) { reset(); }

void Dma::reset() {
  running_mask_[0] = running_mask_[1] = 0;
  for (int i = 0; i < 8; ++i) { ch_[i] = Channel{}; ch_[i].cpu = i < 4 ? Cpu::ARM9 : Cpu::ARM7; ch_[i].num = i & 3; ch_[i].burst_table = MRAM_DUMMY.data; }
  cart_armed_ = false; gx_armed_ = false;
}

void Dma::write_src(Cpu cpu, int n, u32 v) { channel(cpu, n).src = v & (cpu == Cpu::ARM9 ? 0x0FFFFFFF : 0x07FFFFFF); }
void Dma::write_dst(Cpu cpu, int n, u32 v) { channel(cpu, n).dst = v & (cpu == Cpu::ARM9 ? 0x0FFFFFFF : 0x07FFFFFF); }

void Dma::write_cnt(Cpu cpu, int n, u32 v) {
  Channel& c = channel(cpu, n);
  const u32 old = c.cnt;
  c.cnt = v;
  if ((old & 0x80000000) || !(v & 0x80000000)) {
    // This path can clear the enable bit without the channel ever starting.
    update_cart_armed();
    return;
  }
  c.cur_src = c.src; c.cur_dst = c.dst;
  c.tim_key_src = c.tim_key_dst = ~0u;   // width may have changed: refill the timing cache
  switch (v & 0x00600000) { case 0x00000000: c.dst_inc = 1; break; case 0x00200000: c.dst_inc = -1; break; case 0x00400000: c.dst_inc = 0; break; default: c.dst_inc = 1; break; }
  switch (v & 0x01800000) { case 0x00000000: c.src_inc = 1; break; case 0x00800000: c.src_inc = -1; break; case 0x01000000: c.src_inc = 0; break; default: c.src_inc = 1; break; }
  c.start_mode = (cpu == Cpu::ARM9) ? ((v >> 27) & 7) : (((v >> 28) & 3) | 0x10);
  update_cart_armed();
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
  if (prof::enabled) {
    prof::add(prof::C_DMA_STARTS, 1);
    prof::add(c.cpu == Cpu::ARM9 && c.start_mode <= MODE9_GXFIFO
                ? static_cast<prof::Counter>(prof::C_DMA_M_IMM + c.start_mode) : prof::C_DMA_M_ARM7, 1);
  }
  c.iter_count = (c.start_mode == MODE9_GXFIFO && c.rem_count > 112 && !nds_.gpu3d.no_fifo()) ? 112 : c.rem_count;
  if ((c.cnt & 0x01800000) == 0x01800000) c.cur_src = c.src;
  if ((c.cnt & 0x00600000) == 0x00600000) c.cur_dst = c.dst;
  set_running(c, 2);
  c.in_progress = true;
  c.burst_table = MRAM_DUMMY.data; c.burst_pos = 0;
  nds_.sched.preempt(nds_.cpu(c.cpu));   // an immediate start stalls the CPU that issued it
}

void Dma::check(Cpu cpu, u32 mode) {
  for (int n = 0; n < 4; ++n) { Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000)) start(c); }
}
void Dma::stop(Cpu cpu, u32 mode) {
  for (int n = 0; n < 4; ++n) { Channel& c = channel(cpu, n); if (c.start_mode == mode) c.cnt &= ~0x80000000u; }
  update_cart_armed();
}
void Dma::update_cart_armed() {
  cart_armed_ = in_mode(Cpu::ARM9, MODE9_CART) || in_mode(Cpu::ARM7, MODE7_CART);
  gx_armed_ = in_mode(Cpu::ARM9, MODE9_GXFIFO);
}

bool Dma::in_mode(Cpu cpu, u32 mode) const {
  for (int n = 0; n < 4; ++n) { const Channel& c = channel(cpu, n); if (c.start_mode == mode && (c.cnt & 0x80000000)) return true; }
  return false;
}

// Unit cost in system cycles (ARM7 clock); the caller doubles for the ARM9.
u32 Dma::unit_cycles(Channel& c, bool burst_start, bool word) {
  const bool a9 = c.cpu == Cpu::ARM9;
  const u32 shift = a9 ? 14 : 15;
  // `word` is fixed for the life of a transfer (it is a CNT bit), so the
  // cached n/s costs are for the right width as long as the key matches;
  // write_cnt resets the keys.
  if (c.tim_key_src != (c.cur_src >> shift)) {
    const mem::Timing& t = nds_.bus.timing();
    c.tim_key_src = c.cur_src >> shift; c.src_rgn = t.region(a9, c.cur_src); t.dma_cost(a9, c.cur_src, word, c.src_n, c.src_s);
  }
  if (c.tim_key_dst != (c.cur_dst >> shift)) {
    const mem::Timing& t = nds_.bus.timing();
    c.tim_key_dst = c.cur_dst >> shift; c.dst_rgn = t.region(a9, c.cur_dst); t.dma_cost(a9, c.cur_dst, word, c.dst_n, c.dst_s);
  }
  const u32 src_rgn = c.src_rgn, dst_rgn = c.dst_rgn;
  const u32 src_n = c.src_n, src_s = c.src_s, dst_n = c.dst_n, dst_s = c.dst_s;
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

// Per-word cost inside a direct-mapped run. A run never leaves its 2 KB page,
// so it never crosses a 16 KB timing block: the region pair and the n/s costs
// unit_cycles() looked up for the run's first word hold for the rest of it.
// What can still vary per word is the main-RAM burst table, which is walked
// here exactly as unit_cycles() walks it (the table is re-selected to the
// same table when it wraps). Everything else is one constant per run.
struct Dma::RunCost {
  const u8* table; u32 constant; const Burst* meta;
  [[gnu::always_inline]] u32 next(Channel& c) {
    if (!table) return constant;
    u32 v = c.burst_table[c.burst_pos];
    if (v == 0) { c.burst_pos = 0; v = c.burst_table[0]; }
    ++c.burst_pos;
    return v;
  }
  bool closed_form() const { return !table || (meta && meta->len); }
  // The cost of the next `n` units, and the burst position they leave behind:
  // the same cyclic walk next() performs, summed by prefix instead of stepped.
  // next() consumes table[pos], table[pos+1], ... and restarts at 0 on the
  // terminator, leaving pos one past the last entry it took.
  u32 bulk(u32 pos, u32 n, u32& end_pos) const {
    if (!table) { end_pos = pos; return constant * n; }
    const u32 P = meta->len, S = meta->prefix[P];
    const u32 head = P > pos ? P - pos : 0;          // entries before the wrap
    if (n <= head) { end_pos = pos + n; return meta->prefix[pos + n] - meta->prefix[pos]; }
    const u32 m = n - head;                          // entries taken after it
    end_pos = ((m - 1) % P) + 1;
    return (S - meta->prefix[pos]) + (m / P) * S + meta->prefix[m % P];
  }
};
static_assert(mem::PAGE_SIZE <= (1u << 14), "a run must stay inside one DMA timing block");

Dma::RunCost Dma::run_cost(Channel& c, bool word) {
  const u32 MAIN = mem::REGION_MAIN_RAM;
  const bool burst = (c.src_rgn == MAIN && c.dst_rgn != MAIN && c.src_inc > 0) ||
                     (c.dst_rgn == MAIN && c.src_rgn != MAIN && c.dst_inc > 0);
  if (burst) return RunCost{c.burst_table, 0, burst_meta(c.burst_table)};
  // Not a burst: unit_cycles(c, false, word) is a pure function of the cached
  // regions/costs for src_inc == dst_inc == 1 (the runs' only shape).
  return RunCost{nullptr, unit_cycles(c, false, word), nullptr};
}


// Destination zone of a DMA run, for the census. VRAM is split the way the
// hardware maps it, because which surface a run feeds is what decides whether
// the lazy-2D trap has to fire for it.
static prof::Counter dma_zone(u32 addr, bool trap) {
  const u32 top = addr >> 24;
  if (top == 0x06) {
    const u32 v = addr & 0x00FFFFFF;
    prof::Counter c;
    if      (v < 0x200000) c = trap ? prof::C_DMA_T_BGA  : prof::C_DMA_D_BGA;
    else if (v < 0x400000) c = trap ? prof::C_DMA_T_BGB  : prof::C_DMA_D_BGB;
    else if (v < 0x600000) c = trap ? prof::C_DMA_T_OBJA : prof::C_DMA_D_OBJA;
    else if (v < 0x800000) c = trap ? prof::C_DMA_T_OBJB : prof::C_DMA_D_OBJB;
    else                   c = trap ? prof::C_DMA_T_LCDC : prof::C_DMA_D_LCDC;
    return c;
  }
  if (trap) return prof::C_DMA_D_OTHER;   // unreachable: the trap is VRAM-only
  switch (top) {
  case 0x02: return prof::C_DMA_D_MAIN;
  case 0x03: return prof::C_DMA_D_WRAM;
  case 0x04: return prof::C_DMA_D_IO;
  case 0x05: return prof::C_DMA_D_PAL;
  case 0x07: return prof::C_DMA_D_OAM;
  default:   return prof::C_DMA_D_OTHER;
  }
}

u32 Dma::run_channel(Channel& c, u32 budget) {
  const bool a9 = c.cpu == Cpu::ARM9;
  const bool word = c.cnt & (1u << 26);
  bool burst_start = (c.running == 2);
  set_running(c, 1);
  u32 used = 0;
  mem::Bus& bus = nds_.bus;
  // Once a run attempt fails on a destination page (palette, OAM, I/O, a
  // code-tagged or trapped page), the per-unit path is taken for the rest of
  // that page without re-walking the page table for every unit.
  u32 no_run_below = 0;
  while (c.iter_count > 0 && used < budget) {
    if (a9 && nds_.gpu3d.stalled()) break;      // a full GX FIFO stalls the ARM9's DMA too
    prof::add(prof::C_DMA_LOOP, 1);
    u32 cost = unit_cycles(c, burst_start, word);
    if (a9) cost <<= 1;
    used += cost;
    burst_start = false;
    // GXFIFO feed (fixed destination 0x04000400): the word goes straight to
    // the geometry engine. Through the bus it would be dma_write32 ->
    // Bus::io_write -> Io::write -> Gpu3D::write -> gxfifo_write, ~150
    // instructions of dispatch per word, for ~22k words a frame on Dragon
    // Ball Origins.
    if (word && a9 && c.cur_dst == 0x04000400 && c.dst_inc == 0) {
      // Run of words from one direct-mapped source page: skip the per-word
      // page-table walk and loop tests. Exactly what the generic loop below
      // would do per word (cost, stall check, feed, counters), unrolled over
      // the page; the FIFO-full stall and the budget still end it per word.
      if (c.src_inc == 1) {
        const u8* p = nds_.cpu(Cpu::ARM9).page_table.read_ptr(c.cur_src);
        if (p) {
          u32 room = (mem::PAGE_SIZE - (c.cur_src & (mem::PAGE_SIZE - 1))) >> 2;
          RunCost rc = run_cost(c, true);
          // The whole run in one call. gxfifo_dma_burst() is n x the per-word
          // feed with the FIFO cursors held in registers; it is only that while
          // no entry can stall, so the run is cut to the words that provably
          // cannot fill the FIFO (a word carries at most four commands), and
          // to what the budget covers -- the same closed-form test the
          // page-to-page run makes, since the per-word loop stops on the unit
          // that reaches the budget. stalled() is loop-invariant: only a feed
          // can set it, and a run this short cannot.
          if (const u32 cap = nds_.gpu3d.fifo_burst_room(),
                        lim = room < c.iter_count ? room : c.iter_count,
                        n0 = lim < cap ? lim : cap;
              n0 >= kBulkMin && rc.closed_form()) {
            u32 tmp, n = n0;
            if (used + (rc.bulk(c.burst_pos, n0 - 1, tmp) << 1) >= budget) {
              u32 lo = 1, hi = n0;
              while (lo < hi) {
                const u32 mid = lo + (hi - lo + 1) / 2;
                if (used + (rc.bulk(c.burst_pos, mid - 1, tmp) << 1) < budget) lo = mid; else hi = mid - 1;
              }
              n = lo;
            }
            if (n >= 2) {
              u32 pos_end;
              used += rc.bulk(c.burst_pos, n - 1, pos_end) << 1;
              c.burst_pos = pos_end;
              nds_.gpu3d.gxfifo_dma_burst(p, n);
              c.cur_src += 4 * n; c.iter_count -= n; c.rem_count -= n;
              prof::add(prof::C_DMA_GXF_WORDS, n); prof::add(prof::C_DMA_D_IO, n);
              prof::add(prof::C_DMA_GXF_RUNS, 1);
              continue;
            }
          }
          for (;;) {
            u32 v; std::memcpy(&v, p, 4);
            nds_.gpu3d.gxfifo_dma_write(v);
            prof::add(prof::C_DMA_GXF_WORDS, 1); prof::add(prof::C_DMA_D_IO, 1);
            c.cur_src += 4; c.iter_count--; c.rem_count--;
            if (--room == 0 || c.iter_count == 0 || used >= budget || nds_.gpu3d.stalled()) break;
            used += rc.next(c) << 1;
            p += 4;
          }
          continue;
        }
      }
      prof::add(prof::C_DMA_GXF_SLOW, 1);
      nds_.gpu3d.gxfifo_dma_write(bus.dma_read32(c.cpu, c.cur_src));
    }
    else if (word) {
      // Run of words between two direct-mapped pages (main RAM, WRAM, VRAM
      // without a write trap): one page-table walk per end per run instead
      // of two per word. Same per-word cost, stall check and budget as the
      // generic path; a code-tagged or trapped destination stays per word.
      if (c.src_inc == 1 && c.dst_inc == 1 && c.cur_dst >= no_run_below) {
        const u8* ps = nds_.cpu(c.cpu).page_table.read_ptr(c.cur_src);
        bool code = false;
        u8* pd = ps ? nds_.cpu(c.cpu).page_table.write_ptr(c.cur_dst, &code) : nullptr;
        // A destination page under the lazy-2D write trap: take the trap once
        // for the run rather than once per word through the bus. The render
        // catches up before any byte changes, and opens a window in which the
        // page is plain memory again -- so the run copies at full speed. (A
        // page still trapped afterwards falls to the per-word path below.)
        if (ps && !pd && a9 && (c.cur_dst >> 24) == 0x06) {
          prof::add(prof::C_DMA_VRAM_TRAP, 1); prof::add(dma_zone(c.cur_dst, true), 1);
          nds_.gpu.vram_store_trap(Cpu::ARM9, c.cur_dst);
          pd = nds_.cpu(c.cpu).page_table.write_ptr(c.cur_dst, &code);
        }
        if (pd && !code) {
          u32 room = (mem::PAGE_SIZE - (c.cur_src & (mem::PAGE_SIZE - 1))) >> 2;
          const u32 room_d = (mem::PAGE_SIZE - (c.cur_dst & (mem::PAGE_SIZE - 1))) >> 2;
          if (room_d < room) room = room_d;
          RunCost rc = run_cost(c, true);
          prof::add(prof::C_DMA_RUN_SEGS, 1);
          const u32 zdst = c.cur_dst; const u32 z0 = c.iter_count;
          // The whole run in one copy. The per-unit loop below stops on the unit
          // whose cost reaches the budget, so this is the same transfer only
          // while the budget cannot cut the run short; the unit costs are a
          // cyclic pattern, so both the total and that test are closed-form.
          // gpu3d.stalled() is loop-invariant here: the CPU is stopped for the
          // duration of a DMA and only a FIFO feed can set it, which is the
          // branch above and not this one.
          if (const u32 n = room < c.iter_count ? room : c.iter_count;
              n >= kBulkMin && rc.closed_form() && !(a9 && nds_.gpu3d.stalled())) {
            // The most units the budget can take: the loop breaks on the one
            // that reaches it, so every unit up to the last with used < budget
            // is copied for certain. Costs rise monotonically, so binary search
            // it -- and the doubling is the one the per-unit path applies, the
            // tables being in ARM7 system cycles. Anything left over goes round
            // the outer loop, which charges it through the same table walk.
            const int dbl = a9 ? 1 : 0;
            u32 lo, tmp;
            // The budget usually covers the run: test that before searching,
            // so the common case costs one bulk() rather than log2(n) of them.
            if (used + (rc.bulk(c.burst_pos, n - 1, tmp) << dbl) < budget) {
              lo = n;
            } else {
              lo = 1;
              u32 hi = n;
              while (lo < hi) {
                const u32 mid = lo + (hi - lo + 1) / 2;
                if (used + (rc.bulk(c.burst_pos, mid - 1, tmp) << dbl) < budget) lo = mid; else hi = mid - 1;
              }
            }
            if (lo >= 2) {
              u32 pos_end;
              used += rc.bulk(c.burst_pos, lo - 1, pos_end) << dbl;
              c.burst_pos = pos_end;
              std::memcpy(pd, ps, static_cast<size_t>(lo) * 4);
              c.cur_src += 4 * lo; c.cur_dst += 4 * lo;
              c.iter_count -= lo; c.rem_count -= lo;
              prof::add(prof::C_DMA_RUN_W, lo);
              prof::add(dma_zone(zdst, false), z0 - c.iter_count);
              continue;
            }
          }
          for (;;) {
            std::memcpy(pd, ps, 4);
            prof::add(prof::C_DMA_RUN_W, 1);
            c.cur_src += 4; c.cur_dst += 4; c.iter_count--; c.rem_count--;
            if (--room == 0 || c.iter_count == 0 || used >= budget || (a9 && nds_.gpu3d.stalled())) break;
            cost = rc.next(c); if (a9) cost <<= 1; used += cost;
            ps += 4; pd += 4;
          }
          prof::add(dma_zone(zdst, false), z0 - c.iter_count);
          continue;
        }
        no_run_below = (c.cur_dst | (mem::PAGE_SIZE - 1)) + 1;
      }
      prof::add(prof::C_DMA_SLOW_W, 1); prof::add(dma_zone(c.cur_dst, false), 1);
      bus.dma_write32(c.cpu, c.cur_dst, bus.dma_read32(c.cpu, c.cur_src));
    }
    else {
      // Halfword run between two direct-mapped pages, the shape of most VRAM
      // uploads (Golden Sun streams tiles by 16-bit DMA through the display
      // period): same per-unit cost, stall check and budget as the generic
      // path, one page-table walk per end per run, and the lazy-2D write trap
      // taken once for the run rather than per halfword through the bus.
      if (c.src_inc == 1 && c.dst_inc == 1 && c.cur_dst >= no_run_below) {
        const u8* ps = nds_.cpu(c.cpu).page_table.read_ptr(c.cur_src);
        bool code = false;
        u8* pd = ps ? nds_.cpu(c.cpu).page_table.write_ptr(c.cur_dst, &code) : nullptr;
        if (ps && !pd && a9 && (c.cur_dst >> 24) == 0x06) {
          prof::add(prof::C_DMA_VRAM_TRAP, 1); prof::add(dma_zone(c.cur_dst, true), 1);
          nds_.gpu.vram_store_trap(Cpu::ARM9, c.cur_dst);
          pd = nds_.cpu(c.cpu).page_table.write_ptr(c.cur_dst, &code);
        }
        if (pd && !code) {
          u32 room = (mem::PAGE_SIZE - (c.cur_src & (mem::PAGE_SIZE - 1))) >> 1;
          const u32 room_d = (mem::PAGE_SIZE - (c.cur_dst & (mem::PAGE_SIZE - 1))) >> 1;
          if (room_d < room) room = room_d;
          RunCost rc = run_cost(c, false);
          prof::add(prof::C_DMA_RUN_SEGS, 1);
          const u32 zdst = c.cur_dst; const u32 z0 = c.iter_count;
          // The whole run in one copy. The per-unit loop below stops on the unit
          // whose cost reaches the budget, so this is the same transfer only
          // while the budget cannot cut the run short; the unit costs are a
          // cyclic pattern, so both the total and that test are closed-form.
          // gpu3d.stalled() is loop-invariant here: the CPU is stopped for the
          // duration of a DMA and only a FIFO feed can set it, which is the
          // branch above and not this one.
          if (const u32 n = room < c.iter_count ? room : c.iter_count;
              n >= kBulkMin && rc.closed_form() && !(a9 && nds_.gpu3d.stalled())) {
            // The most units the budget can take: the loop breaks on the one
            // that reaches it, so every unit up to the last with used < budget
            // is copied for certain. Costs rise monotonically, so binary search
            // it -- and the doubling is the one the per-unit path applies, the
            // tables being in ARM7 system cycles. Anything left over goes round
            // the outer loop, which charges it through the same table walk.
            const int dbl = a9 ? 1 : 0;
            u32 lo, tmp;
            // The budget usually covers the run: test that before searching,
            // so the common case costs one bulk() rather than log2(n) of them.
            if (used + (rc.bulk(c.burst_pos, n - 1, tmp) << dbl) < budget) {
              lo = n;
            } else {
              lo = 1;
              u32 hi = n;
              while (lo < hi) {
                const u32 mid = lo + (hi - lo + 1) / 2;
                if (used + (rc.bulk(c.burst_pos, mid - 1, tmp) << dbl) < budget) lo = mid; else hi = mid - 1;
              }
            }
            if (lo >= 2) {
              u32 pos_end;
              used += rc.bulk(c.burst_pos, lo - 1, pos_end) << dbl;
              c.burst_pos = pos_end;
              std::memcpy(pd, ps, static_cast<size_t>(lo) * 2);
              c.cur_src += 2 * lo; c.cur_dst += 2 * lo;
              c.iter_count -= lo; c.rem_count -= lo;
              prof::add(prof::C_DMA_RUN_H, lo);
              prof::add(dma_zone(zdst, false), z0 - c.iter_count);
              continue;
            }
          }
          for (;;) {
            std::memcpy(pd, ps, 2);
            prof::add(prof::C_DMA_RUN_H, 1);
            c.cur_src += 2; c.cur_dst += 2; c.iter_count--; c.rem_count--;
            if (--room == 0 || c.iter_count == 0 || used >= budget || (a9 && nds_.gpu3d.stalled())) break;
            cost = rc.next(c); if (a9) cost <<= 1; used += cost;
            ps += 2; pd += 2;
          }
          prof::add(dma_zone(zdst, false), z0 - c.iter_count);
          continue;
        }
        no_run_below = (c.cur_dst | (mem::PAGE_SIZE - 1)) + 1;
      }
      prof::add(prof::C_DMA_SLOW_H, 1); prof::add(dma_zone(c.cur_dst, false), 1);
      bus.dma_write16(c.cpu, c.cur_dst, bus.dma_read16(c.cpu, c.cur_src));
    }
    const u32 step = word ? 4 : 2;
    c.cur_src += c.src_inc * step;
    c.cur_dst += c.dst_inc * step;
    c.iter_count--; c.rem_count--;
  }
  if (c.rem_count) {
    if (c.iter_count == 0) {
      set_running(c, 0);   // wait for the next trigger
      // GXFIFO mode is level-triggered: a burst that leaves the FIFO still
      // below half full (it was near empty) is followed by the next one at
      // once. Waiting for the engine to drain first can deadlock when the
      // words so far are an incomplete command (Spirit Tracks' intro).
      if (c.start_mode == MODE9_GXFIFO) nds_.gpu3d.check_fifo_dma();
    }
    return used;
  }
  if (!(c.cnt & (1u << 25))) { c.cnt &= ~0x80000000u; update_cart_armed(); }   // not repeating: disable
  if (c.cnt & (1u << 30)) nds_.io.request_irq(c.cpu, io::IRQ_DMA0 + c.num);
  set_running(c, 0);
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


template <class S> void Dma::sync_state(S& s) {
  s.begin("DMA ");
  for (Channel& c : ch_) {
    // The burst table is one of four static tables: travel as an index.
    u8 bt = c.burst_table == READ16.data ? 1 : c.burst_table == READ32.data ? 2 : c.burst_table == READ32_N2.data ? 3 : 0;
    s.fields(c.src, c.dst, c.cnt, c.cur_src, c.cur_dst, c.src_inc, c.dst_inc, c.start_mode, c.rem_count, c.iter_count, c.running, c.in_progress, bt, c.burst_pos);
    if constexpr (S::reading) {
      c.burst_table = bt == 1 ? READ16.data : bt == 2 ? READ32.data : bt == 3 ? READ32_N2.data : MRAM_DUMMY.data;
      c.tim_key_src = c.tim_key_dst = ~0u;   // timing cache: refilled on the next access
    }
  }
  s.end();
  if constexpr (S::reading) {
    running_mask_[0] = running_mask_[1] = 0;
    for (Channel& c : ch_) if (c.running) running_mask_[c.cpu == Cpu::ARM9 ? 0 : 1] |= static_cast<u8>(1u << c.num);
    update_cart_armed();
  }
}
template void Dma::sync_state<state::Writer>(state::Writer&);
template void Dma::sync_state<state::Reader>(state::Reader&);

} // namespace ds::dma
