// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/bus.h"
#include "core/cpu/cp15.h"
#include "core/state/state.h"
#include "core/profile.h"
#include "core/nds.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ds::mem {

constexpr u32 Bus::VRAM_BANK_SIZES[9];

// Debug watchpoint (DS_WATCH=<hex addr>): the 2 KB main-RAM page holding the
// address is taken out of both page tables so accesses come through here.
static u32 watch_addr = 0; static bool watch_on = false; static u32 watch_hits = 0;
static u8* watch_host[2] = {nullptr, nullptr};   // the watched page's host bytes per CPU (any directly mapped page; main RAM by default)

Bus::Bus(NDS& nds)
    : main_ram(alloc_page_buf(MAIN_RAM_SIZE_DSI)), shared_wram(alloc_page_buf(SHARED_WRAM_SIZE)),
      arm7_wram(alloc_page_buf(ARM7_WRAM_SIZE)), itcm(alloc_page_buf(ITCM_SIZE)),
      dtcm(alloc_page_buf(DTCM_SIZE)), vram(alloc_page_buf(VRAM_TOTAL)), palette(alloc_page_buf(PALETTE_SIZE)),
      oam(alloc_page_buf(OAM_SIZE)), bios9(alloc_page_buf(BIOS9_SIZE)), bios7(alloc_page_buf(BIOS7_SIZE)),
      nwram{alloc_page_buf(NWRAM_BANK_SIZE), alloc_page_buf(NWRAM_BANK_SIZE), alloc_page_buf(NWRAM_BANK_SIZE)},
      bios9i(alloc_page_buf(BIOS9I_SIZE)), bios7i(alloc_page_buf(BIOS7I_SIZE)), nds_(nds) {
  std::memset(bios9.get(), 0, BIOS9_SIZE);
  std::memset(bios7.get(), 0, BIOS7_SIZE);
}

u32 Bus::main_ram_size() const { return nds_.dsi ? MAIN_RAM_SIZE_DSI : MAIN_RAM_SIZE; }

Bus::~Bus() = default;

u8* Bus::vram_bank(int i) {
  u32 off = 0;
  for (int k = 0; k < i; ++k) off += VRAM_BANK_SIZES[k];
  return vram.get() + off;
}

void Bus::reset() {
  std::memset(main_ram.get(), 0, main_ram_size());
  for (auto& b : nwram) std::memset(b.get(), 0, NWRAM_BANK_SIZE);
  std::memset(nwram_map_, 0, sizeof nwram_map_);
  timing_.clock9_shift = nds_.dsi ? 2 : 1;   // SCFG_CLK9 bit 0 is set at a DSi reset; timing_.reset() builds with it
  std::memset(shared_wram.get(), 0, SHARED_WRAM_SIZE);
  std::memset(arm7_wram.get(), 0, ARM7_WRAM_SIZE);
  std::memset(itcm.get(), 0, ITCM_SIZE);
  std::memset(dtcm.get(), 0, DTCM_SIZE);
  std::memset(vram.get(), 0, VRAM_TOTAL);
  std::memset(palette.get(), 0, PALETTE_SIZE);
  std::memset(oam.get(), 0, OAM_SIZE);
  timing_.reset();
  if (nds_.dsi) {
    timing_.set_region9(0x0C000000, 0x0D000000, REGION_MAIN_RAM, 16, 8, 1);   // the uncached main-RAM alias
    timing_.set_region7(0x0C000000, 0x0D000000, REGION_MAIN_RAM, 16, 8, 1);
  }
  vram_hosts_valid_ = false;
  nds_.cpu(Cpu::ARM9).timing9 = timing_.cpu9();
  nds_.cpu(Cpu::ARM9).timing7 = timing_.cpu7();
  nds_.cpu(Cpu::ARM9).cost7 = timing_.cost7();
  nds_.cpu(Cpu::ARM7).timing9 = timing_.cpu9();
  nds_.cpu(Cpu::ARM7).timing7 = timing_.cpu7();
  nds_.cpu(Cpu::ARM7).cost7 = timing_.cost7();
  map_fixed_regions();
  update_nwram();
  update_vram();
  update_tcm(nds_.cpu(Cpu::ARM9), true);
  if (nds_.dsi) update_vram_timings();
  gba_slot_applied_ = -1;        // the timing tables were just reset
  update_gba_slot_timings();
}

void Bus::update_wifi_timings() {
  if (nds_.io.powcnt2 & 0x0002) {
    static const int ntimings[4] = {10, 8, 6, 18};
    const u16 v = nds_.io.wifiwaitcnt;
    timing_.set_region7(0x04800000, 0x04808000, REGION_WIFI0, 16, ntimings[v & 3], (v & 0x04) ? 4 : 6);
    timing_.set_region7(0x04808000, 0x04810000, REGION_WIFI1, 16, ntimings[(v >> 3) & 3], (v & 0x20) ? 4 : 10);
  } else {
    timing_.set_region7(0x04800000, 0x04808000, REGION_WIFI0, 32, 1, 1);
    timing_.set_region7(0x04808000, 0x04810000, REGION_WIFI1, 32, 1, 1);
  }
}

void Bus::update_gba_slot_timings() {
  // Only bits 0-4 and 7 reach the slot timings (5-6 are the PHI output);
  // a rewrite that leaves them alone would rebuild 12 K pages for nothing.
  // (Games probing the slot do flip ownership several times in one frame --
  // Super Mario 64 six times in frame 16 -- and those rebuilds are real.)
  const u16 ex = nds_.io.exmemcnt & 0x9F;
  if (gba_slot_applied_ == ex) return;
  gba_slot_applied_ = ex;
  prof::add(prof::C_BUS_GBA_TIMING, 1);
  static const bool debug = std::getenv("DS_DEBUG_TIMING") != nullptr;
  if (debug) std::fprintf(stderr, "[timing] exmemcnt %04x frame %llu\n", nds_.io.exmemcnt, (unsigned long long)nds_.frame_count);
  static const int rom_n[4] = {10, 8, 6, 18};
  const int rn = rom_n[(ex >> 2) & 3], rs = (ex & 0x10) ? 4 : 6;
  static const int ram_n[4] = {10, 8, 6, 18};
  const int ran = ram_n[ex & 3];
  if (ex & 0x80) {          // ARM7 owns the slot
    timing_.set_region9(0x08000000, 0x0A000000, REGION_NONE, 32, 1, 1);
    timing_.set_region9(0x0A000000, 0x0B000000, REGION_NONE, 32, 1, 1);
    timing_.set_region7(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, rn, rs);
    timing_.set_region7(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, ran, ran);
  } else {
    timing_.set_region9(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, rn, rs);
    timing_.set_region9(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, ran, ran);
    timing_.set_region7(0x08000000, 0x0A000000, REGION_NONE, 32, 1, 1);
    timing_.set_region7(0x0A000000, 0x0B000000, REGION_NONE, 32, 1, 1);
  }
  timing_.update_cpu9(nds_.cpu(Cpu::ARM9), 0x08000000, 0x0B000000);
}

// Map `host` of `size` bytes at `guest`, mirrored up to `mirror_end`.
void Bus::map_page_aligned(PageTable& pt, u32 guest, u32 size, u8* host, u32 flags, u32 mirror_end) {
  for (u32 a = guest; a < mirror_end; a += size) pt.map(a, size, host, flags);
}

void Bus::map_fixed_regions() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE, RO = PAGE_READABLE;

  for (PageTable* pt : {&pt9, &pt7}) {
    pt->unmap(0x00000000, 0x10000000);                         // everything below the wifi/cart window
    map_main_ram(*pt);
    pt->map_mmio(0x04000000, 0x01000000);
  }
  // ARM9: BIOS at FFFF0000 (mirrored over the top 64 KB), palette, OAM.
  update_bios_map();
  // Palette and OAM read directly but store through the slow path: the 2D
  // engines render lazily from their own copies, and every store has to be
  // journaled (Gpu::palette_store / oam_store).
  map_page_aligned(pt9, 0x05000000, PALETTE_SIZE, palette.get(), RO, 0x06000000);
  map_page_aligned(pt9, 0x07000000, OAM_SIZE, oam.get(), RO, 0x08000000);
  // ARM7: private WRAM at 03800000 (default mapping; WRAMCNT may put shared
  // WRAM in 03000000-037FFFFF, handled in update_wram; the BIOS is in
  // update_bios_map).
  map_page_aligned(pt7, 0x03800000, ARM7_WRAM_SIZE, arm7_wram.get(), RW, 0x04000000);
  // GBA slot (no cart): reads return open bus via the slow path.
  pt9.map_mmio(0x08000000, 0x02000000);
  pt7.map_mmio(0x08000000, 0x02000000);
}

// Main RAM: the DS's 4 MB mirrored over 02000000-02FFFFFF; the DSi's 16 MB
// once, and again at 0C000000 (the uncached alias both DSi CPUs decode).
void Bus::map_main_ram(PageTable& pt) {
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  map_page_aligned(pt, 0x02000000, main_ram_size(), main_ram.get(), RW, 0x03000000);
  if (nds_.dsi) pt.map(0x0C000000, MAIN_RAM_SIZE_DSI, main_ram.get(), RW);
}

// The BIOS pair. DS: 4 KB ARM9 image mirrored over the top 64 KB, 16 KB ARM7
// image at 0. DSi: the 64 KB images, unless SCFG_BIOS bit 1/9 has switched
// a CPU back to its DS image; bits 0/8 hide the upper 32 KB of each DSi
// image (reads come through the slow path as all ones, like hardware).
void Bus::update_bios_map() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RO = PAGE_READABLE;
  pt9.unmap(0xFFFF0000, 0x10000);
  pt7.unmap(0x00000000, 0x10000);
  const u16 scfg_bios = nds_.dsi ? nds_.io.dsi.scfg_bios : 0;
  if (nds_.dsi && !(scfg_bios & 0x0002)) {
    pt9.map(0xFFFF0000, (scfg_bios & 0x0001) ? 0x8000 : 0x10000, bios9i.get(), RO);
  } else {
    map_page_aligned(pt9, 0xFFFF0000, BIOS9_SIZE, bios9.get(), RO, 0xFFFFFFFFu - 0xFFF);
    pt9.map(0xFFFFF000, 0x1000, bios9.get(), RO);
  }
  // DSi ARM7 BIOS: left unmapped so every access (fetch included) comes
  // through io_read, which applies the BIOS protection rules (reads from
  // outside the BIOS, or below BIOSPROT from above it, return all ones).
  if (!(nds_.dsi && !(scfg_bios & 0x0200))) pt7.map(0x00000000, BIOS7_SIZE, bios7.get(), RO);
  // The ITCM window over 0 on the ARM9 is re-applied by update_tcm, which
  // runs after this at reset and relink; a later SCFG_BIOS change (ARM7
  // only, set-once bits) does not touch the ARM9's low pages.
}

void Bus::update_vram_timings() {
  const int width = (nds_.dsi && (nds_.io.dsi.scfg_ext[0] & (1u << 13))) ? 32 : 16;
  timing_.set_region9(0x06000000, 0x07000000, REGION_VRAM, width, 1, 1);
  timing_.set_region7(0x06000000, 0x07000000, REGION_VRAM, width, 1, 1);
  timing_.update_cpu9(nds_.cpu(Cpu::ARM9), 0x06000000, 0x07000000);
}

void Bus::set_clock9_shift(u32 shift) {
  if (timing_.clock9_shift == shift) return;
  timing_.clock9_shift = shift;
  timing_.update_cpu9(nds_.cpu(Cpu::ARM9), 0, 0xFFFFFFFF);
  nds_.dma.set_clock9_shift(shift);
  nds_.sched.set_clock9_shift(shift);
}

void Bus::update_wram() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  // ARM9 side: 03000000-03FFFFFF.
  pt9.unmap(0x03000000, 0x01000000);
  // ARM7 side: 03000000-037FFFFF is the shared split; 03800000-03FFFFFF the
  // private WRAM, re-laid here because a DSi NWRAM window may have covered it.
  pt7.unmap(0x03000000, 0x01000000);
  map_page_aligned(pt7, 0x03800000, ARM7_WRAM_SIZE, arm7_wram.get(), RW, 0x04000000);
  u8* half0 = shared_wram.get();
  u8* half1 = shared_wram.get() + 0x4000;
  switch (nds_.io.wramcnt & 3) {
  case 0:   // ARM9: all 32K; ARM7: none (ARM7 WRAM mirrors)
    map_page_aligned(pt9, 0x03000000, SHARED_WRAM_SIZE, shared_wram.get(), RW, 0x04000000);
    map_page_aligned(pt7, 0x03000000, ARM7_WRAM_SIZE, arm7_wram.get(), RW, 0x03800000);
    break;
  case 1:   // ARM9: second 16K; ARM7: first 16K
    map_page_aligned(pt9, 0x03000000, 0x4000, half1, RW, 0x04000000);
    map_page_aligned(pt7, 0x03000000, 0x4000, half0, RW, 0x03800000);
    break;
  case 2:   // ARM9: first 16K; ARM7: second 16K
    map_page_aligned(pt9, 0x03000000, 0x4000, half0, RW, 0x04000000);
    map_page_aligned(pt7, 0x03000000, 0x4000, half1, RW, 0x03800000);
    break;
  default:  // ARM9: none (unmapped -> slow path returns 0); ARM7: all 32K
    map_page_aligned(pt7, 0x03000000, SHARED_WRAM_SIZE, shared_wram.get(), RW, 0x03800000);
    break;
  }
}

// The NWRAM slot tables and windows, as melonDS derives them (DSi.cpp
// MapNWRAM_A/B/C, MapNWRAMRange, ARM9Read/ARM7Read): each MBK1-5 byte assigns
// its slot to a CPU and a position; a CPU's window (MBK6-8) shows the slots
// at positions (addr >> 16|15) & mask, A over B over C where they overlap.
// One hardware quirk is not modelled: two slots mapped to the same position
// are both written by a store there (melonDS writes every matching part);
// here the one that reads wins. No title on hand does it.
void Bus::update_nwram() {
  update_wram();
  // The watched page (enable_watch) is re-laid with the rest: take it out again after.
  struct Retrap { Bus& b; ~Retrap() { if (!watch_on) return; for (int c = 0; c < 2; ++c) if (watch_host[c]) b.nds_.cpu(c ? Cpu::ARM7 : Cpu::ARM9).page_table.map_mmio(watch_addr & ~0x7FFu, 0x800); } } retrap{*this};
  if (!nds_.dsi) return;
  const io::DsiIo& d = nds_.io.dsi;
  if (getenv("DS_DEBUG_MBK"))
    std::fprintf(stderr, "[nwram] rebuild scfg_ext %08x/%08x (bit25 a9=%d a7=%d) t=%llu\n",
                 d.scfg_ext[0], d.scfg_ext[1], (d.scfg_ext[0] >> 25) & 1, (d.scfg_ext[1] >> 25) & 1,
                 (unsigned long long)nds_.sched.now());
  std::memset(nwram_map_, 0, sizeof nwram_map_);
  for (int part = 3; part >= 0; --part) {
    const u8 v = static_cast<u8>((d.mbk[0][0] >> (part * 8)) & 0xFD);
    if (v & 0x80) nwram_map_[0][v & 3][(v >> 2) & 3] = nwram[0].get() + (part << 16);
  }
  for (int bank = 1; bank <= 2; ++bank)
    for (int part = 7; part >= 0; --part) {
      u8 v = static_cast<u8>((d.mbk[0][(bank == 1 ? 1 : 3) + (part >> 2)] >> ((part & 3) * 8)) & 0xFF);
      if (!(v & 0x80)) continue;
      if (v & 0x02) v &= 0xFE;
      nwram_map_[bank][v & 3][(v >> 2) & 7] = nwram[bank].get() + (part << 15);
    }
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  for (int c = 0; c < 2; ++c) {
    if (!(d.scfg_ext[c] & (1u << 25))) continue;
    PageTable& pt = nds_.cpu(c ? Cpu::ARM7 : Cpu::ARM9).page_table;
    for (int bank = 2; bank >= 0; --bank) {          // C first, A last: A has priority
      const u32 v = d.mbk[c][5 + bank];
      u32 start, end, mask;
      if (bank == 0) {
        start = 0x03000000 + (((v >> 4) & 0xFF) << 16); end = 0x03000000 + (((v >> 20) & 0x1FF) << 16);
        static const u32 masks[4] = {0, 0, 1, 3}; mask = masks[(v >> 12) & 3];
      } else {
        start = 0x03000000 + (((v >> 3) & 0x1FF) << 15); end = 0x03000000 + (((v >> 19) & 0x3FF) << 15);
        static const u32 masks[4] = {0, 1, 3, 7}; mask = masks[(v >> 12) & 3];
      }
      if (end > 0x04000000) end = 0x04000000;      // the window is cut at the end of the region
      const u32 shift = bank == 0 ? 16 : 15, unit = 1u << shift;
      for (u32 a = start; a < end; a += unit) {
        u8* host = nwram_map_[bank][c][(a >> shift) & mask];
        if (host) pt.map(a, unit, host, RW);
        else pt.unmap(a, unit);                    // shown but unbacked: reads 0, writes dropped (slow path)
      }
    }
  }
}

void Bus::update_vram() {
  prof::add(prof::C_BUS_UPDATE_VRAM, 1);
  // The escape hatch. A band worker reads texture and texture-palette VRAM
  // directly, and this is the only place either can move: neither view is
  // ever mapped into a CPU's address space, so a game that wants to write a
  // texture bank must first switch it out of texture mode, through here.
  // Rebuild into a copy first: whether the band workers must be joined
  // depends on whether the two views they index actually move, and most
  // VRAMCNT traffic (capture and BG bank swaps -- Spirit Tracks does two a
  // frame) leaves them alone. The copy is a few KB of plain arrays.
  u8* banks[9];
  for (int i = 0; i < 9; ++i) banks[i] = vram_bank(i);
  gpu::VramMap next = vram_map_;
  next.rebuild(nds_.io.vramcnt, banks);
  const auto differs = [](const gpu::VramView& a, const gpu::VramView& b) {
    return a.size != b.size || a.ptr != b.ptr || a.mask != b.mask;
  };
  const bool tex_moved = differs(next.texture, vram_map_.texture) || differs(next.texpal, vram_map_.texpal);
  if (tex_moved) {
    nds_.gpu3d.sync_raster();
    if (prof::enabled && prof::async_window) prof::add(prof::C_ASYNC_VRAMCNT_SWAP, 1);
  }
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  // A remap mid-frame changes what the deferred 2D render reads: the lines
  // whose HBlank has passed are rendered now, against the old views, before
  // anything is rebuilt; the write trap (which the remap below would drop)
  // is re-armed after it.
  const bool trapped = nds_.gpu.vram_remap_begin();
  vram_map_ = next;
  // The whole 16 MB region is described as one host pointer per page and
  // applied as a diff: games that rewrite VRAMCNT every few frames (bank
  // swaps for capture) would otherwise unmap and remap 8 K pages per CPU.
  // Blocks backed by exactly one bank map straight into the page table;
  // blocks where banks overlap stay unmapped so the slow path can OR the
  // banks on read and write all of them.
  u8** const h9 = vram_hosts_[0].get();
  u8** const h7 = vram_hosts_[1].get();
  std::memset(h9, 0, VRAM_PAGES * sizeof(u8*));
  std::memset(h7, 0, VRAM_PAGES * sizeof(u8*));
  auto set_pages = [](u8** hosts, u32 addr, u32 size, u8* host) {
    u8** pg = hosts + ((addr - 0x06000000) >> PAGE_SHIFT);
    for (u32 i = 0; i < size / PAGE_SIZE; ++i) pg[i] = host + i * PAGE_SIZE;
  };
  auto map_view = [&](u8** hosts, const gpu::VramView& v, u32 base, u32 end) {
    for (u32 mirror = base; mirror < end; mirror += v.size)
      for (u32 b = 0; b < v.blocks(); ++b)
        if (v.ptr[b]) set_pages(hosts, mirror + b * gpu::VramView::BLOCK, gpu::VramView::BLOCK, v.ptr[b]);
  };
  map_view(h9, vram_map_.abg,  0x06000000, 0x06200000);
  map_view(h9, vram_map_.bbg,  0x06200000, 0x06400000);
  map_view(h9, vram_map_.aobj, 0x06400000, 0x06600000);
  map_view(h9, vram_map_.bobj, 0x06600000, 0x06800000);
  map_view(h7, vram_map_.arm7, 0x06000000, 0x07000000);
  static const u32 lcdc_base[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
  for (int i = 0; i < 9; ++i) {
    if (!(vram_map_.lcdc_mask & (1u << i))) continue;
    for (u32 mirror = 0x06800000; mirror < 0x07000000; mirror += 0x100000) set_pages(h9, mirror + lcdc_base[i], VRAM_BANK_SIZES[i], banks[i]);
  }
  // Only the runs of pages whose host changed go through the page table:
  // the previous host arrays are kept and compared in 2 KB chunks, so a
  // one-bank swap costs one bank's worth of entries, not 16 K per CPU.
  auto apply = [&](PageTable& pt, u8** cur, u8** prev) {
    constexpr u32 CHUNK = 256;   // entries (512 KB of guest space)
    u32 run = 0; bool in_run = false;
    for (u32 i = 0; i <= VRAM_PAGES; i += CHUNK) {
      const bool d = i < VRAM_PAGES && std::memcmp(cur + i, prev + i, CHUNK * sizeof(u8*)) != 0;
      if (d) { if (!in_run) { run = i; in_run = true; } }
      else if (in_run) { pt.remap(0x06000000 + (run << PAGE_SHIFT), (i - run) << PAGE_SHIFT, cur + run, RW); in_run = false; }
    }
  };
  for (u32 w = 0; w < VRAM_PAGES / 64; ++w) {
    u64 m = 0;
    for (u32 b = 0; b < 64; ++b) if (h9[w * 64 + b]) m |= u64{1} << b;
    vram_mapped9_[w] = m;
  }
  if (!vram_hosts_valid_) { pt9.remap(0x06000000, 0x01000000, h9, RW); pt7.remap(0x06000000, 0x01000000, h7, RW); vram_hosts_valid_ = true; }
  else { apply(pt9, h9, vram_hosts_prev_[0].get()); apply(pt7, h7, vram_hosts_prev_[1].get()); }
  std::swap(vram_hosts_[0], vram_hosts_prev_[0]);
  std::swap(vram_hosts_[1], vram_hosts_prev_[1]);
  nds_.gpu.vram_remap_end(trapped);
  if (watch_on && (watch_addr >> 24) == 0x06) { pt9.map_mmio(watch_addr & ~0x7FFu, 0x800); pt7.map_mmio(watch_addr & ~0x7FFu, 0x800); }
}

// Slow-path VRAM access for blocks with overlapping banks (and LCDC gaps).
static const gpu::VramView* vram_view_for(const gpu::VramMap& m, Cpu cpu, u32 addr, int& lcdc_bank, u32& off) {
  lcdc_bank = -1;
  if (cpu == Cpu::ARM7) { off = addr; return &m.arm7; }
  switch ((addr >> 21) & 7) {
  case 0: off = addr; return &m.abg;
  case 1: off = addr; return &m.bbg;
  case 2: off = addr; return &m.aobj;
  case 3: off = addr; return &m.bobj;
  default: {
    const u32 o = addr & 0xFFFFF;
    static const u32 base[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
    for (int i = 0; i < 9; ++i)
      if (o >= base[i] && o < base[i] + Bus::VRAM_BANK_SIZES[i]) { lcdc_bank = i; off = o - base[i]; return nullptr; }
    return nullptr;
  }
  }
}

u32 Bus::vram_read(Cpu cpu, u32 addr, u32 width) {
  // A read of an LCDC bank under the capture read trap (set_lcdc_read_trap):
  // the batched capture writing it may still be in flight on the worker.
  if (addr >= 0x06800000 && nds_.gpu.lcdc_read_trapped()) nds_.gpu.lcdc_read_hit(addr);
  int bank; u32 off;
  const gpu::VramView* v = vram_view_for(vram_map_, cpu, addr, bank, off);
  if (v) return width == 8 ? vram_map_.read8(*v, off) : width == 16 ? vram_map_.read16(*v, off) : vram_map_.read32(*v, off);
  if (bank < 0 || !(vram_map_.lcdc_mask & (1u << bank))) return 0;
  u32 r = 0; std::memcpy(&r, vram_bank(bank) + off, width / 8); return r;
}

void Bus::vram_write(Cpu cpu, u32 addr, u32 width, u32 val) {
  if (watch_on && addr >= (watch_addr & ~0x7FFu) && addr < (watch_addr & ~0x7FFu) + 0x800 && watch_hits++ < 1000000)
    std::fprintf(stderr, "[watch] cpu%d vram write%u %08x = %08x pc %08x frame %llu line %u vramcnt_h %02x\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, val, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.frame_count, nds_.gpu.line(), nds_.io.vramcnt[7]);
  int bank; u32 off;
  const gpu::VramView* v = vram_view_for(vram_map_, cpu, addr, bank, off);
  if (v) { if (width == 8) vram_map_.write8(*v, off, val); else if (width == 16) vram_map_.write16(*v, off, val); else vram_map_.write32(*v, off, val); return; }
  if (bank < 0 || !(vram_map_.lcdc_mask & (1u << bank))) return;
  std::memcpy(vram_bank(bank) + off, &val, width / 8);
}

void Bus::update_tcm(CpuContext& cpu, bool force) {
  prof::add(prof::C_BUS_UPDATE_TCM, 1);
  // Rebuild the ARM9 map from scratch so a moved/shrunk TCM window releases
  // its old pages, then overlay TCM. TODO: track the previous window instead.
  // Nothing happens when the effective windows are unchanged: the full remap
  // and the timing-table rebuild (1 M entries) are expensive, and they
  // invalidate every translated block.
  const u32 old_itcm = cpu.itcm_size, old_dbase = cpu.dtcm_base, old_dmask = cpu.dtcm_mask;
  cpu.update_tcm_windows();
  if (!force && cpu.itcm_size == old_itcm && cpu.dtcm_base == old_dbase && cpu.dtcm_mask == old_dmask) return;
  PageTable& pt = cpu.page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE, RO = PAGE_READABLE;
  // Release the previous TCM windows, then reapply the fixed map: entries
  // that do not change are skipped by PageTable::map, so this costs only
  // the windows themselves rather than a 256 MB remap.
  if (force) pt.unmap(0x00000000, 0x10000000);
  vram_hosts_valid_ = false;   // the unmaps above may have touched VRAM pages: update_vram below re-applies in full
  if (tcm_prev_itcm_) pt.unmap(0, std::min(tcm_prev_itcm_, 0x02000000u));
  if (tcm_prev_dtcm_size_) pt.unmap(tcm_prev_dtcm_base_, tcm_prev_dtcm_size_);
  tcm_prev_itcm_ = 0; tcm_prev_dtcm_size_ = 0;
  map_main_ram(pt);
  pt.map_mmio(0x04000000, 0x01000000);
  map_page_aligned(pt, 0x05000000, PALETTE_SIZE, palette.get(), RO, 0x06000000);   // stores journaled, see map_fixed_regions
  map_page_aligned(pt, 0x07000000, OAM_SIZE, oam.get(), RO, 0x08000000);
  pt.map_mmio(0x08000000, 0x02000000);
  if (force) update_bios_map();   // the ARM9's top pages were unmapped above
  update_nwram();
  update_vram();
  // The TCM windows are baked into the cost table: rebuild the old and the
  // new windows (a forced rebuild covers everything).
  if (force) timing_.update_cpu9(cpu, 0, 0xFFFFFFFF);
  else {
    auto window = [&](u32 base, u32 mask, u32 itcm) {
      if (itcm) timing_.update_cpu9(cpu, 0, std::min(itcm, 0x10000000u), false);
      if (mask) { const u32 end = base + (~mask + 1); timing_.update_cpu9(cpu, base, end < base ? 0xFFFFFFFF : end, false); }   // ~mask + 1 = window size
    };
    window(old_dbase, old_dmask, old_itcm);
    window(cpu.dtcm_base, cpu.dtcm_mask, cpu.itcm_size);
    timing_.notify_cpu9(cpu);
  }
  if (watch_on) pt.map_mmio(watch_addr & ~0x7FFu, 0x800);
  const u32 ctl = cpu.cp15_control;
  if (ctl & (1u << 16)) {                                       // DTCM enabled
    const u32 base = cpu.cp15_dtcm & 0xFFFFF000;
    u32 size = 512u << ((cpu.cp15_dtcm >> 1) & 0x1F);
    if (size < DTCM_SIZE) size = DTCM_SIZE;
    for (u32 off = 0; off < size; off += DTCM_SIZE) {
      pt.map(base + off, DTCM_SIZE, dtcm.get(), RW);   // load mode (write-only) approximated as RW
    }
    tcm_prev_dtcm_base_ = base; tcm_prev_dtcm_size_ = size;
  }
  if (ctl & (1u << 18)) {                                       // ITCM enabled (base fixed at 0)
    u32 size = 512u << ((cpu.cp15_itcm >> 1) & 0x1F);
    if (size < ITCM_SIZE) size = ITCM_SIZE;
    if (size > 0x02000000) size = 0x02000000;
    for (u32 off = 0; off < size; off += ITCM_SIZE) {
      pt.map(off, ITCM_SIZE, itcm.get(), RW);
    }
    tcm_prev_itcm_ = size;
  }
}

// ---- slow paths -------------------------------------------------------------
void Bus::enable_watch(u32 addr) {
  watch_addr = addr; watch_on = true;
  // Any page the CPUs map directly (main RAM, WRAM, NWRAM): the host bytes are
  // recorded per CPU before the page is taken out of the tables, and the
  // slow path serves them below. A later remap of that page (a VRAMCNT /
  // MBK change) would re-map it and end the watch.
  for (int c = 0; c < 2; ++c) {
    PageTable& pt = nds_.cpu(c ? Cpu::ARM7 : Cpu::ARM9).page_table;
    u8* p = pt.read_ptr(addr & ~0x7FFu);
    watch_host[c] = p ? p : ((addr & 0xFF000000) == 0x02000000 ? main_ram.get() + (addr & (main_ram_size() - 1) & ~0x7FFu) : nullptr);
    if (watch_host[c]) pt.map_mmio(addr & ~0x7FFu, 0x800);
  }
}

u32 Bus::io_read(Cpu cpu, u32 addr, u32 width) {
  if (watch_on && (addr & ~0x7FFu) == (watch_addr & ~0x7FFu) && watch_host[cpu == Cpu::ARM9 ? 0 : 1]) {
    u32 v = 0; std::memcpy(&v, watch_host[cpu == Cpu::ARM9 ? 0 : 1] + (addr & 0x7FF), width / 8);
    if (addr < watch_addr + 4 && addr + width / 8 > watch_addr)
      std::fprintf(stderr, "[watch] cpu%d read%u %08x = %08x pc %08x t %llu frame %llu line %u\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, v, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line());
    return v;
  }
  if ((addr & 0xFF000000) == 0x04000000) return nds_.io.read(cpu, addr, width);
  // DSi: the BIOS halves SCFG_BIOS hides (update_bios_map leaves them
  // unmapped) read as all ones on both CPUs.
  if (nds_.dsi && (cpu == Cpu::ARM9 ? addr >= 0xFFFF0000 : addr < 0x00010000)) {
    const u32 ones = width == 32 ? 0xFFFFFFFFu : width == 16 ? 0xFFFFu : 0xFFu;
    if (cpu == Cpu::ARM9) return ones;
    // melonDS DSi::ARM7Read*: the hidden upper half, any access from outside
    // the BIOS, and a protected-range access from above BIOSPROT read as ones.
    const u16 scfg_bios = nds_.io.dsi.scfg_bios;
    if (scfg_bios & 0x0200) return ones;                     // DS BIOS selected: mapped directly, never here
    const u32 pc = nds_.cpu(cpu).hot.regs[15], prot = nds_.io.arm7_bios_prot;
    if (addr >= 0x8000 && (scfg_bios & 0x0100)) return ones;
    if (pc >= 0x10000) return ones;
    if (addr < prot && pc >= prot) return ones;
    u32 v = 0; std::memcpy(&v, bios7i.get() + (addr & 0xFFFF & ~(width / 8 - 1)), width / 8); return v;
  }
  if ((addr & 0xFF000000) == 0x06000000) return vram_read(cpu, addr, width);
  if ((addr & 0xFF000000) == 0x08000000 || (addr & 0xFF000000) == 0x09000000) {
    // GBA slot, nothing inserted: open bus pattern per GBATEK.
    u32 v = static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
    return width == 8 ? (v >> ((addr & 1) * 8)) & 0xFF : width == 16 ? v & 0xFFFF : v;
  }
  return 0;
}
void Bus::io_write(Cpu cpu, u32 addr, u32 width, u32 v) {
  if (watch_on && (addr & ~0x7FFu) == (watch_addr & ~0x7FFu) && watch_host[cpu == Cpu::ARM9 ? 0 : 1]) {
    if (addr < watch_addr + 4 && addr + width / 8 > watch_addr)
      std::fprintf(stderr, "[watch] cpu%d write%u %08x = %08x pc %08x t %llu frame %llu line %u\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, v, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.sched.now(), (unsigned long long)nds_.frame_count, nds_.gpu.line());
    std::memcpy(watch_host[cpu == Cpu::ARM9 ? 0 : 1] + (addr & 0x7FF), &v, width / 8); return;
  }
  switch (addr >> 24) {
  case 0x04:
    // ARM9 word stores to GXFIFO and the direct command ports: the one I/O
    // store a 3D frame makes tens of thousands of times. Straight to the
    // geometry engine, as the DMA path already goes, instead of through the
    // census test, io_unowned and two owns_reg probes (~110 instructions).
    if (width == 32 && cpu == Cpu::ARM9 && addr - 0x04000400 < 0x1CC && !io::Io::census_on()) { nds_.gpu3d.gx_port_write(addr, v); return; }
    nds_.io.write(cpu, addr, width, v); return;
  case 0x05: nds_.gpu.palette_store(cpu, addr, width, v); return;
  case 0x07: nds_.gpu.oam_store(cpu, addr, width, v); return;
  case 0x06: {
    // Overlapping-bank blocks always land here; directly mapped pages only
    // while the lazy-2D write trap holds them. The trap sees the store before
    // the bytes change, and a trapped code page still owes the SMC report.
    nds_.gpu.vram_store_trap(cpu, addr);
    const Entry e = nds_.cpu(cpu).page_table.entry(addr);
    if ((e & TAG_CODE) && (e & BASE_MASK)) { store_code(reinterpret_cast<u8*>(((e & BASE_MASK) << 2) + addr), &v, width / 8); return; }
    vram_write(cpu, addr, width, v);
    return;
  }
  default: return;
  }
}

void Bus::set_vram_trap(bool on, bool lcdc, bool a_only) {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  // Over the mapped pages only (~330 of the 8 K in the engine windows):
  // Golden Sun toggles this ~31 times a frame.
  auto range = [&](u32 addr, u32 size) {
    const u32 first = (addr - 0x06000000) >> PAGE_SHIFT;
    pt9.set_write_trap_bits(addr >> PAGE_SHIFT, size >> PAGE_SHIFT, vram_mapped9_ + first / 64, on);
  };
  if (a_only) { range(0x06000000, 0x00200000); range(0x06400000, 0x00200000); }   // BG-A, OBJ-A
  else range(0x06000000, 0x00800000);         // the four engine windows
  if (lcdc) range(0x06800000, 0x00800000);    // LCDC and its 1 MB mirrors
}

// A trapped entry keeps only its tags: read_ptr and write_ptr both see no
// base and fall to io_read/io_write, which resolve LCDC through vram_read /
// vram_write. The saved entry goes back on lift, with whatever code tag the
// page picked up meanwhile; an entry that gained a base in between (a remap
// that did not go through the join, which should not happen) is left alone.
void Bus::set_lcdc_read_trap(int bank, bool on) {
  assert(bank >= 0 && bank < 4);
  static const u32 lcdc_base[4] = {0x00000, 0x20000, 0x40000, 0x60000};
  Entry* const t = nds_.cpu(Cpu::ARM9).page_table.raw();
  const u32 pages = VRAM_BANK_SIZES[bank] >> PAGE_SHIFT;
  u32 k = 0;
  for (u32 mirror = 0x06800000; mirror < 0x07000000; mirror += 0x100000) {
    const u32 first = (mirror + lcdc_base[bank]) >> PAGE_SHIFT;
    for (u32 p = 0; p < pages; ++p, ++k) {
      Entry& e = t[first + p];
      if (on) { lcdc_read_save_[k] = e; if (e & BASE_MASK) e = TAG_SPECIAL | (e & TAG_CODE); }
      else if (!(e & BASE_MASK)) e = lcdc_read_save_[k] | (e & TAG_CODE);
    }
  }
}

u8 Bus::dma_read8(Cpu cpu, u32 addr) {
  if (u8* p = nds_.cpu(cpu).page_table.read_ptr(addr)) return *p;
  return static_cast<u8>(io_read(cpu, addr, 8));
}
u16 Bus::dma_read16(Cpu cpu, u32 addr) {
  addr &= ~1u;
  if (u8* p = nds_.cpu(cpu).page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return static_cast<u16>(io_read(cpu, addr, 16));
}
u32 Bus::dma_read32(Cpu cpu, u32 addr) {
  addr &= ~3u;
  if (u8* p = nds_.cpu(cpu).page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return io_read(cpu, addr, 32);
}
void Bus::dma_write8(Cpu cpu, u32 addr, u8 v) {
  bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { if (code) store_code(p, &v, 1); else *p = v; return; }
  io_write(cpu, addr, 8, v);
}
void Bus::dma_write16(Cpu cpu, u32 addr, u16 v) {
  addr &= ~1u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { if (code) store_code(p, &v, 2); else std::memcpy(p, &v, 2); return; }
  io_write(cpu, addr, 16, v);
}
void Bus::dma_write32(Cpu cpu, u32 addr, u32 v) {
  addr &= ~3u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { if (code) store_code(p, &v, 4); else std::memcpy(p, &v, 4); return; }
  io_write(cpu, addr, 32, v);
}

u8  Bus::read8 (Cpu cpu, u32 addr) { return static_cast<u8>(io_read(cpu, addr, 8)); }
u16 Bus::read16(Cpu cpu, u32 addr) { return static_cast<u16>(io_read(cpu, addr, 16)); }
u32 Bus::read32(Cpu cpu, u32 addr) { return io_read(cpu, addr, 32); }
void Bus::write8 (Cpu cpu, u32 addr, u8  v) { io_write(cpu, addr, 8, v); }
void Bus::write16(Cpu cpu, u32 addr, u16 v) { io_write(cpu, addr, 16, v); }
void Bus::write32(Cpu cpu, u32 addr, u32 v) { io_write(cpu, addr, 32, v); }
u32 Bus::fetch(Cpu cpu, u32 addr, u32 width) {
  if (nds_.dsi && cpu == Cpu::ARM7 && addr < 0x00010000 && !(nds_.io.dsi.scfg_bios & 0x0200)) {
    if (addr >= 0x8000 && (nds_.io.dsi.scfg_bios & 0x0100)) return width == 32 ? 0xFFFFFFFFu : 0xFFFFu;
    u32 v = 0; std::memcpy(&v, bios7i.get() + (addr & 0xFFFF & ~(width / 8 - 1)), width / 8); return v;
  }
  return io_read(cpu, addr, width);
}


template <class S> void Bus::sync_state(S& s) {
  s.begin("MEM ");
  s.blob(main_ram.get(), main_ram_size());
  s.blob(shared_wram.get(), SHARED_WRAM_SIZE);
  s.blob(arm7_wram.get(), ARM7_WRAM_SIZE);
  s.blob(itcm.get(), ITCM_SIZE);
  s.blob(dtcm.get(), DTCM_SIZE);
  s.blob(vram.get(), VRAM_TOTAL);
  s.blob(palette.get(), PALETTE_SIZE);
  s.blob(oam.get(), OAM_SIZE);
  if (nds_.dsi) for (auto& b : nwram) s.blob(b.get(), NWRAM_BANK_SIZE);
  s.end();
}
template void Bus::sync_state<state::Writer>(state::Writer&);
template void Bus::sync_state<state::Reader>(state::Reader&);

void Bus::relink() {
  CpuContext& a9 = nds_.cpu(Cpu::ARM9);
  CpuContext& a7 = nds_.cpu(Cpu::ARM7);
  a9.timing9 = a7.timing9 = timing_.cpu9();
  a9.timing7 = a7.timing7 = timing_.cpu7();
  a9.cost7 = a7.cost7 = timing_.cost7();
  cp15_update_pu_map(a9);
  update_tcm(a9, true);          // also update_bios_map(), update_nwram() and update_vram()
  gba_slot_applied_ = -1;        // the loaded EXMEMCNT is not what the tables hold
  update_gba_slot_timings();
  if (nds_.dsi) {
    // The loaded SCFG: the ARM9's clock (a hand-off starts at 67 MHz, a title
    // may have switched to 134) and the VRAM access width.
    set_clock9_shift((nds_.io.dsi.scfg_clock9 & 1) ? 2 : 1);
    update_vram_timings();
  }
}

} // namespace ds::mem
