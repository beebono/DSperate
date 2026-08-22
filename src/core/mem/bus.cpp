// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/bus.h"
#include "core/nds.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ds::mem {

constexpr u32 Bus::VRAM_BANK_SIZES[9];

// Debug watchpoint (DS_WATCH=<hex addr>): the 2 KB main-RAM page holding the
// address is taken out of both page tables so accesses come through here.
static u32 watch_addr = 0; static bool watch_on = false; static u32 watch_hits = 0;

Bus::Bus(NDS& nds)
    : main_ram(new u8[MAIN_RAM_SIZE]), shared_wram(new u8[SHARED_WRAM_SIZE]),
      arm7_wram(new u8[ARM7_WRAM_SIZE]), itcm(new u8[ITCM_SIZE]),
      dtcm(new u8[DTCM_SIZE]), vram(new u8[VRAM_TOTAL]), palette(new u8[PALETTE_SIZE]),
      oam(new u8[OAM_SIZE]), bios9(new u8[BIOS9_SIZE]), bios7(new u8[BIOS7_SIZE]), nds_(nds) {
  std::memset(bios9.get(), 0, BIOS9_SIZE);
  std::memset(bios7.get(), 0, BIOS7_SIZE);
}

Bus::~Bus() = default;

u8* Bus::vram_bank(int i) {
  u32 off = 0;
  for (int k = 0; k < i; ++k) off += VRAM_BANK_SIZES[k];
  return vram.get() + off;
}

void Bus::reset() {
  std::memset(main_ram.get(), 0, MAIN_RAM_SIZE);
  std::memset(shared_wram.get(), 0, SHARED_WRAM_SIZE);
  std::memset(arm7_wram.get(), 0, ARM7_WRAM_SIZE);
  std::memset(itcm.get(), 0, ITCM_SIZE);
  std::memset(dtcm.get(), 0, DTCM_SIZE);
  std::memset(vram.get(), 0, VRAM_TOTAL);
  std::memset(palette.get(), 0, PALETTE_SIZE);
  std::memset(oam.get(), 0, OAM_SIZE);
  timing_.reset();
  nds_.cpu(Cpu::ARM9).timing9 = timing_.cpu9();
  nds_.cpu(Cpu::ARM9).timing7 = timing_.cpu7();
  nds_.cpu(Cpu::ARM7).timing9 = timing_.cpu9();
  nds_.cpu(Cpu::ARM7).timing7 = timing_.cpu7();
  map_fixed_regions();
  update_wram();
  update_vram();
  update_tcm(nds_.cpu(Cpu::ARM9), true);
  update_gba_slot_timings();
}

void Bus::update_gba_slot_timings() {
  const u16 ex = nds_.io.exmemcnt;
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
    map_page_aligned(*pt, 0x02000000, MAIN_RAM_SIZE, main_ram.get(), RW, 0x03000000);
    pt->map_mmio(0x04000000, 0x01000000);
  }
  // ARM9: BIOS at FFFF0000 (mirrored over the top 64 KB), palette, OAM.
  pt9.unmap(0xFFFF0000, 0x10000);
  map_page_aligned(pt9, 0xFFFF0000, BIOS9_SIZE, bios9.get(), RO, 0xFFFFFFFFu - 0xFFF);
  pt9.map(0xFFFFF000, 0x1000, bios9.get(), RO);
  map_page_aligned(pt9, 0x05000000, PALETTE_SIZE, palette.get(), RW, 0x06000000);
  map_page_aligned(pt9, 0x07000000, OAM_SIZE, oam.get(), RW, 0x08000000);
  // ARM7: BIOS at 0, private WRAM at 03800000 (default mapping; WRAMCNT may
  // put shared WRAM in 03000000-037FFFFF, handled in update_wram).
  pt7.map(0x00000000, BIOS7_SIZE, bios7.get(), RO);
  map_page_aligned(pt7, 0x03800000, ARM7_WRAM_SIZE, arm7_wram.get(), RW, 0x04000000);
  // GBA slot (no cart): reads return open bus via the slow path.
  pt9.map_mmio(0x08000000, 0x02000000);
  pt7.map_mmio(0x08000000, 0x02000000);
}

void Bus::update_wram() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  // ARM9 side: 03000000-03FFFFFF.
  pt9.unmap(0x03000000, 0x01000000);
  // ARM7 side: 03000000-037FFFFF (03800000+ is always ARM7 WRAM, already mapped).
  pt7.unmap(0x03000000, 0x00800000);
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

void Bus::update_vram() {
  PageTable& pt9 = nds_.cpu(Cpu::ARM9).page_table;
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE;
  u8* banks[9];
  for (int i = 0; i < 9; ++i) banks[i] = vram_bank(i);
  vram_map_.rebuild(nds_.io.vramcnt, banks);
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
  pt9.remap(0x06000000, 0x01000000, h9, RW);
  pt7.remap(0x06000000, 0x01000000, h7, RW);
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
  if (tcm_prev_itcm_) pt.unmap(0, std::min(tcm_prev_itcm_, 0x02000000u));
  if (tcm_prev_dtcm_size_) pt.unmap(tcm_prev_dtcm_base_, tcm_prev_dtcm_size_);
  tcm_prev_itcm_ = 0; tcm_prev_dtcm_size_ = 0;
  map_page_aligned(pt, 0x02000000, MAIN_RAM_SIZE, main_ram.get(), RW, 0x03000000);
  pt.map_mmio(0x04000000, 0x01000000);
  map_page_aligned(pt, 0x05000000, PALETTE_SIZE, palette.get(), RW, 0x06000000);
  map_page_aligned(pt, 0x07000000, OAM_SIZE, oam.get(), RW, 0x08000000);
  pt.map_mmio(0x08000000, 0x02000000);
  (void)RO;
  update_wram();
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
  nds_.cpu(Cpu::ARM9).page_table.map_mmio(addr & ~0x7FFu, 0x800);
  nds_.cpu(Cpu::ARM7).page_table.map_mmio(addr & ~0x7FFu, 0x800);
}

u32 Bus::io_read(Cpu cpu, u32 addr, u32 width) {
  if (watch_on && (addr & 0xFF000000) == 0x02000000) { u32 v = 0; std::memcpy(&v, main_ram.get() + (addr & (MAIN_RAM_SIZE - 1)), width / 8); return v; }
  if ((addr & 0xFF000000) == 0x04000000) return nds_.io.read(cpu, addr, width);
  if ((addr & 0xFF000000) == 0x06000000) return vram_read(cpu, addr, width);
  if ((addr & 0xFF000000) == 0x08000000 || (addr & 0xFF000000) == 0x09000000) {
    // GBA slot, nothing inserted: open bus pattern per GBATEK.
    u32 v = static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
    return width == 8 ? (v >> ((addr & 1) * 8)) & 0xFF : width == 16 ? v & 0xFFFF : v;
  }
  return 0;
}
void Bus::io_write(Cpu cpu, u32 addr, u32 width, u32 v) {
  if (watch_on && (addr & 0xFF000000) == 0x02000000) {
    if (addr < watch_addr + 4 && addr + width / 8 > watch_addr)
      std::fprintf(stderr, "[watch] cpu%d write%u %08x = %08x pc %08x frame %llu line %u\n", cpu == Cpu::ARM9 ? 9 : 7, width, addr, v, nds_.cpu(cpu).hot.regs[15], (unsigned long long)nds_.frame_count, nds_.gpu.line());
    std::memcpy(main_ram.get() + (addr & (MAIN_RAM_SIZE - 1)), &v, width / 8); return;
  }
  if ((addr & 0xFF000000) == 0x04000000) nds_.io.write(cpu, addr, width, v);
  else if ((addr & 0xFF000000) == 0x06000000) vram_write(cpu, addr, width, v);
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
void Bus::dma_write16(Cpu cpu, u32 addr, u16 v) {
  addr &= ~1u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 2); if (code) code_written(p, 2); return; }
  io_write(cpu, addr, 16, v);
}
void Bus::dma_write32(Cpu cpu, u32 addr, u32 v) {
  addr &= ~3u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 4); if (code) code_written(p, 4); return; }
  io_write(cpu, addr, 32, v);
}

u8  Bus::read8 (Cpu cpu, u32 addr) { return static_cast<u8>(io_read(cpu, addr, 8)); }
u16 Bus::read16(Cpu cpu, u32 addr) { return static_cast<u16>(io_read(cpu, addr, 16)); }
u32 Bus::read32(Cpu cpu, u32 addr) { return io_read(cpu, addr, 32); }
void Bus::write8 (Cpu cpu, u32 addr, u8  v) { io_write(cpu, addr, 8, v); }
void Bus::write16(Cpu cpu, u32 addr, u16 v) { io_write(cpu, addr, 16, v); }
void Bus::write32(Cpu cpu, u32 addr, u32 v) { io_write(cpu, addr, 32, v); }

} // namespace ds::mem
