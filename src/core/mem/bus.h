// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"
#include "core/mem/timing.h"
#include "core/gpu/vram_map.h"

#include <memory>

namespace ds {
struct NDS;
struct CpuContext;
}

namespace ds::mem {

// Owns the physical memories and keeps both CPUs' page tables in sync with the
// guest-visible mapping (VRAM bank control, shared-WRAM split, ITCM/DTCM moves).
// Every mapping change goes through here so that the JIT's CODE tags and the
// page tables never drift apart.
class Bus {
public:
  explicit Bus(NDS& nds);
  ~Bus();

  void reset();

  // Remaps after control-register changes.
  void update_tcm(CpuContext& cpu);   // CP15 (ARM9)
  void update_wram();                 // WRAMCNT
  void update_vram();                 // VRAMCNT A-I

  // DMA accesses (no CPU cycle accounting; page-table fast path then MMIO).
  u16 dma_read16(Cpu cpu, u32 addr);
  u32 dma_read32(Cpu cpu, u32 addr);
  void dma_write16(Cpu cpu, u32 addr, u16 v);
  void dma_write32(Cpu cpu, u32 addr, u32 v);

  Timing& timing() { return timing_; }
  const Timing& timing() const { return timing_; }
  void update_gba_slot_timings();    // EXMEMCNT
  void enable_watch(u32 addr);       // debug: log writes to a main-RAM word (see DS_WATCH in the CLI)

  // Slow paths, reached when the page table returns nullptr.
  u8  read8 (Cpu cpu, u32 addr);
  u16 read16(Cpu cpu, u32 addr);
  u32 read32(Cpu cpu, u32 addr);
  void write8 (Cpu cpu, u32 addr, u8  v);
  void write16(Cpu cpu, u32 addr, u16 v);
  void write32(Cpu cpu, u32 addr, u32 v);

  // Physical memories (sizes per GBATEK).
  static constexpr u32 MAIN_RAM_SIZE    = 4 * 1024 * 1024;
  static constexpr u32 SHARED_WRAM_SIZE = 32 * 1024;
  static constexpr u32 ARM7_WRAM_SIZE   = 64 * 1024;
  static constexpr u32 ITCM_SIZE        = 32 * 1024;
  static constexpr u32 DTCM_SIZE        = 16 * 1024;
  static constexpr u32 PALETTE_SIZE     = 2 * 1024;
  static constexpr u32 OAM_SIZE         = 2 * 1024;
  static constexpr u32 BIOS9_SIZE       = 4 * 1024;
  static constexpr u32 BIOS7_SIZE       = 16 * 1024;
  static constexpr u32 VRAM_BANK_SIZES[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
  static constexpr u32 VRAM_TOTAL = 0x20000 * 4 + 0x10000 + 0x4000 * 2 + 0x8000 + 0x4000;

  const gpu::VramMap& vram_map() const { return vram_map_; }

  std::unique_ptr<u8[]> main_ram, shared_wram, arm7_wram, itcm, dtcm, vram, palette, oam, bios9, bios7;
  u8* vram_bank(int i);

private:
  NDS& nds_;
  Timing timing_;
  gpu::VramMap vram_map_;
  u32 vram_read(Cpu cpu, u32 addr, u32 width);
  void vram_write(Cpu cpu, u32 addr, u32 width, u32 v);
  void map_fixed_regions();
  void map_page_aligned(PageTable& pt, u32 guest, u32 size, u8* host, u32 flags, u32 mirror_end);
  u32 io_read(Cpu cpu, u32 addr, u32 width);
  void io_write(Cpu cpu, u32 addr, u32 width, u32 v);
};

} // namespace ds::mem
