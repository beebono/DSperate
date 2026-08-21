// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/bus.h"
#include "core/nds.h"

#include <cstdio>
#include <cstring>

namespace ds::mem {

constexpr u32 Bus::VRAM_BANK_SIZES[9];

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
  update_tcm(nds_.cpu(Cpu::ARM9));
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
  pt9.unmap(0x06000000, 0x01000000);
  pt7.unmap(0x06000000, 0x01000000);
  // Later mappings override earlier ones at the same address; hardware ORs
  // overlapping banks, which we do not model.
  for (int i = 0; i < 9; ++i) {
    const u8 cnt = nds_.io.vramcnt[i];
    if (!(cnt & 0x80)) continue;
    const u32 mst = cnt & 7, ofs = (cnt >> 3) & 3, size = VRAM_BANK_SIZES[i];
    u8* host = vram_bank(i);
    // LCDC (MST 0): fixed addresses at 06800000.
    static const u32 lcdc_base[9] = {0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000, 0x06890000, 0x06894000, 0x06898000, 0x068A0000};
    if (mst == 0) { pt9.map(lcdc_base[i], size, host, RW); continue; }
    switch (i) {
    case 0: case 1: case 2: case 3:   // A-D
      if (mst == 1) pt9.map(0x06000000 + ofs * 0x20000, size, host, RW);           // BG-A
      else if (mst == 2 && (i == 2 || i == 3)) pt7.map(0x06000000 + (ofs & 1) * 0x20000, size, host, RW);  // ARM7
      else if (mst == 2) pt9.map(0x06400000 + (ofs & 1) * 0x20000, size, host, RW); // OBJ-A (A,B)
      else if (mst == 4 && i == 2) pt9.map(0x06200000, size, host, RW);            // BG-B
      else if (mst == 4 && i == 3) pt9.map(0x06600000, size, host, RW);            // OBJ-B
      break;
    case 4:                            // E
      if (mst == 1) pt9.map(0x06000000, size, host, RW);
      else if (mst == 2) pt9.map(0x06400000, size, host, RW);
      break;
    case 5: case 6: {                  // F, G
      const u32 o = (ofs & 1) * 0x4000 + (ofs & 2) * 0x8000;
      if (mst == 1) pt9.map(0x06000000 + o, size, host, RW);
      else if (mst == 2) pt9.map(0x06400000 + o, size, host, RW);
      break;
    }
    case 7:                            // H
      if (mst == 1) pt9.map(0x06200000, size, host, RW);
      break;
    case 8:                            // I
      if (mst == 1) pt9.map(0x06208000, size, host, RW);
      else if (mst == 2) pt9.map(0x06600000, size, host, RW);
      break;
    }
  }
}

void Bus::update_tcm(CpuContext& cpu) {
  // Rebuild the ARM9 map from scratch so a moved/shrunk TCM window releases
  // its old pages, then overlay TCM. TODO: track the previous window instead.
  PageTable& pt = cpu.page_table;
  const u32 RW = PAGE_READABLE | PAGE_WRITABLE, RO = PAGE_READABLE;
  pt.unmap(0x00000000, 0x10000000);
  map_page_aligned(pt, 0x02000000, MAIN_RAM_SIZE, main_ram.get(), RW, 0x03000000);
  pt.map_mmio(0x04000000, 0x01000000);
  map_page_aligned(pt, 0x05000000, PALETTE_SIZE, palette.get(), RW, 0x06000000);
  map_page_aligned(pt, 0x07000000, OAM_SIZE, oam.get(), RW, 0x08000000);
  pt.map_mmio(0x08000000, 0x02000000);
  (void)RO;
  update_wram();
  update_vram();
  cpu.update_tcm_windows();
  const u32 ctl = cpu.cp15_control;
  if (ctl & (1u << 16)) {                                       // DTCM enabled
    const u32 base = cpu.cp15_dtcm & 0xFFFFF000;
    u32 size = 512u << ((cpu.cp15_dtcm >> 1) & 0x1F);
    if (size < DTCM_SIZE) size = DTCM_SIZE;
    for (u32 off = 0; off < size; off += DTCM_SIZE) {
      pt.map(base + off, DTCM_SIZE, dtcm.get(), RW);   // load mode (write-only) approximated as RW
    }
  }
  if (ctl & (1u << 18)) {                                       // ITCM enabled (base fixed at 0)
    u32 size = 512u << ((cpu.cp15_itcm >> 1) & 0x1F);
    if (size < ITCM_SIZE) size = ITCM_SIZE;
    if (size > 0x02000000) size = 0x02000000;
    for (u32 off = 0; off < size; off += ITCM_SIZE) {
      pt.map(off, ITCM_SIZE, itcm.get(), RW);
    }
  }
}

// ---- slow paths -------------------------------------------------------------
u32 Bus::io_read(Cpu cpu, u32 addr, u32 width) {
  if ((addr & 0xFF000000) == 0x04000000) return nds_.io.read(cpu, addr, width);
  if ((addr & 0xFF000000) == 0x08000000 || (addr & 0xFF000000) == 0x09000000) {
    // GBA slot, nothing inserted: open bus pattern per GBATEK.
    u32 v = static_cast<u32>((addr >> 1) & 0xFFFF) | (static_cast<u32>(((addr + 2) >> 1) & 0xFFFF) << 16);
    return width == 8 ? (v >> ((addr & 1) * 8)) & 0xFF : width == 16 ? v & 0xFFFF : v;
  }
  return 0;
}
void Bus::io_write(Cpu cpu, u32 addr, u32 width, u32 v) {
  if ((addr & 0xFF000000) == 0x04000000) nds_.io.write(cpu, addr, width, v);
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
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 2); return; }
  io_write(cpu, addr, 16, v);
}
void Bus::dma_write32(Cpu cpu, u32 addr, u32 v) {
  addr &= ~3u; bool code = false;
  if (u8* p = nds_.cpu(cpu).page_table.write_ptr(addr, &code)) { std::memcpy(p, &v, 4); return; }
  io_write(cpu, addr, 32, v);
}

u8  Bus::read8 (Cpu cpu, u32 addr) { return static_cast<u8>(io_read(cpu, addr, 8)); }
u16 Bus::read16(Cpu cpu, u32 addr) { return static_cast<u16>(io_read(cpu, addr, 16)); }
u32 Bus::read32(Cpu cpu, u32 addr) { return io_read(cpu, addr, 32); }
void Bus::write8 (Cpu cpu, u32 addr, u8  v) { io_write(cpu, addr, 8, v); }
void Bus::write16(Cpu cpu, u32 addr, u16 v) { io_write(cpu, addr, 16, v); }
void Bus::write32(Cpu cpu, u32 addr, u32 v) { io_write(cpu, addr, 32, v); }

} // namespace ds::mem
