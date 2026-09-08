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
  // Save states: the physical memories, and the rebuild of every mapping
  // and timing table from the restored control registers (WRAMCNT,
  // VRAMCNT, EXMEMCNT, CP15) once those are back.
  template <class S> void sync_state(S& s);
  void relink();

  // Remaps after control-register changes.
  void update_tcm(CpuContext& cpu, bool force = false);
  u32 tcm_prev_itcm_ = 0, tcm_prev_dtcm_base_ = 0, tcm_prev_dtcm_size_ = 0;   // windows mapped by the last update_tcm   // CP15 (ARM9)
  void update_wram();                 // WRAMCNT
  void update_vram();                 // VRAMCNT A-I
  Entry lcdc_read_save_[8 * 64] = {};   // set_lcdc_read_trap: 8 mirrors x 128 KB / PAGE_SIZE
  // Lazy 2D (see gpu.h): trap ARM9 stores into the 2D engines' VRAM windows
  // (and, with `lcdc`, the LCDC window) so the first one in a frame can force
  // the deferred render to catch up before the bytes change.
  // `a_only`: engine A's BG/OBJ windows only (lag mode, where engine B's
  // lines are drawn on the emulation thread and its window may stream).
  void set_vram_trap(bool on, bool lcdc, bool a_only = false);
  // Read trap on one LCDC bank (A-D), all eight mirrors: ARM9 loads and DMA
  // reads of it take the slow path while a batched display capture that
  // writes the bank is still in flight on the compositor thread, so the
  // reader can be joined before it sees stale bytes (Gpu::join_worker).
  // The entries are saved and restored; a VRAMCNT remap lifts it first.
  void set_lcdc_read_trap(int bank, bool on);

  // DMA accesses (no CPU cycle accounting; page-table fast path then MMIO).
  // The 8-bit pair is not reachable by a real DMA, which is 16- or 32-bit
  // only; it is here for the cheat engine, whose byte writes are not the
  // guest's and must not be priced (core/cheat/ar_engine.h).
  u8  dma_read8 (Cpu cpu, u32 addr);
  void dma_write8(Cpu cpu, u32 addr, u8 v);
  u16 dma_read16(Cpu cpu, u32 addr);
  u32 dma_read32(Cpu cpu, u32 addr);
  void dma_write16(Cpu cpu, u32 addr, u16 v);
  void dma_write32(Cpu cpu, u32 addr, u32 v);

  Timing& timing() { return timing_; }
  const Timing& timing() const { return timing_; }
  void update_gba_slot_timings();    // EXMEMCNT
  int gba_slot_applied_ = -1;        // EXMEMCNT timing bits the tables currently hold
  void enable_watch(u32 addr);       // debug: log writes to a main-RAM word (see DS_WATCH, headless)

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
  static constexpr u32 VRAM_PAGES = 0x01000000 / PAGE_SIZE;   // the 0x06000000 region, per CPU
  std::unique_ptr<u8*[]> vram_hosts_[2] = {std::make_unique<u8*[]>(VRAM_PAGES), std::make_unique<u8*[]>(VRAM_PAGES)};   // update_vram scratch (next)
  std::unique_ptr<u8*[]> vram_hosts_prev_[2] = {std::make_unique<u8*[]>(VRAM_PAGES), std::make_unique<u8*[]>(VRAM_PAGES)};   // what the page tables hold now
  bool vram_hosts_valid_ = false;
  // Which pages of the ARM9 0x06000000 window have a host (rebuilt by
  // update_vram): the write trap toggles visit these, not all 8 K entries.
  u64 vram_mapped9_[VRAM_PAGES / 64] = {};   // prev arrays describe the tables (false after reset / a table rebuild)
  static constexpr u32 VRAM_BANK_SIZES[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
  static constexpr u32 VRAM_TOTAL = 0x20000 * 4 + 0x10000 + 0x4000 * 2 + 0x8000 + 0x4000;

  const gpu::VramMap& vram_map() const { return vram_map_; }

  PageBuf main_ram, shared_wram, arm7_wram, itcm, dtcm, vram, palette, oam, bios9, bios7;   // PAGE_SIZE-aligned, see alloc_page_buf
  u8* vram_bank(int i);

private:
  NDS& nds_;
  Timing timing_;
  gpu::VramMap vram_map_;
  u32 vram_read(Cpu cpu, u32 addr, u32 width);
  void vram_write(Cpu cpu, u32 addr, u32 width, u32 v);
  void map_fixed_regions();
  void map_page_aligned(PageTable& pt, u32 guest, u32 size, u8* host, u32 flags, u32 mirror_end);
public:
  // The non-RAM access path (I/O, VRAM slow blocks, GBA slot). The JIT's
  // slow-store helper enters here for I/O so it gets the GXFIFO fast path.
  u32 io_read(Cpu cpu, u32 addr, u32 width);
  void io_write(Cpu cpu, u32 addr, u32 width, u32 v);
};

} // namespace ds::mem
