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
  void update_wram();                 // WRAMCNT (DS), or WRAMCNT beneath the NWRAM windows (DSi)
  // DSi: rebuild the two CPUs' 0x03000000 windows from MBK1-9 (io.dsi.mbk)
  // over the WRAMCNT split. A slot a window shows that no MBK entry backs
  // reads as 0 and drops writes, as on hardware; the fall-through to the
  // old shared WRAM only happens outside the windows.
  void update_nwram(bool windows_only = false);   // windows_only: only MBK1-8 changed (see apply_wram)
  // DSi: the BIOS pair as SCFG_BIOS exposes it (bits 0/8 hide the upper
  // 32 KB halves, bits 1/9 fall back to the DS images).
  void update_bios_map();
  // DSi: SCFG_CLK9 bit 0 doubles the ARM9 clock; the CPU timing table is in
  // ARM9 cycles, so it is rebuilt with the new bus-to-core ratio.
  void set_clock9_shift(u32 shift);
  // DSi: SCFG_EXT bit 13 widens the VRAM bus to 32 bits (melonDS UpdateVRAMTimings).
  void update_vram_timings();
  // DSi: the main RAM the CPUs address, 4 or 16 MB (the size SCFG_EXT7 bits
  // 14-15 carry, see Io::dsi_apply_ram_size). The buffer stays 16 MB.
  u32 main_ram_span() const;
  // Re-lays both CPUs' main RAM mirrors after the size changed.
  void update_main_ram();
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
  void update_wifi_timings();        // WIFIWAITCNT / POWCNT2 bit 1 (melonDS NDS::UpdateWifiTimings)
  int gba_slot_applied_ = -1;        // EXMEMCNT timing bits the tables currently hold
  void enable_watch(u32 addr);       // debug: log writes to a main-RAM word (see DS_WATCH, headless)
  static bool watch_active();       // enable_watch has been called (a bypass around io_read/io_write must yield)

  // Slow paths, reached when the page table returns nullptr.
  u8  read8 (Cpu cpu, u32 addr);
  u16 read16(Cpu cpu, u32 addr);
  u32 read32(Cpu cpu, u32 addr);
  // Instruction fetch fallback for pages the tables do not map directly. The
  // DSi ARM7 BIOS lives here: fetches see it (melonDS fetches with R15 at the
  // fetch address, so its protection rules never fire on one); the rules in
  // io_read apply to data reads only.
  u32 fetch(Cpu cpu, u32 addr, u32 width);
  void write8 (Cpu cpu, u32 addr, u8  v);
  void write16(Cpu cpu, u32 addr, u16 v);
  void write32(Cpu cpu, u32 addr, u32 v);

  // Physical memories (sizes per GBATEK). MAIN_RAM_SIZE is the DS's 4 MB,
  // which the RetroAchievements map and the DS mirror loop are written
  // against; the buffer is always the DSi's 16 MB and main_ram_size() says
  // how much of it the machine has.
  static constexpr u32 MAIN_RAM_SIZE    = 4 * 1024 * 1024;
  static constexpr u32 MAIN_RAM_SIZE_DSI = 16 * 1024 * 1024;
  static constexpr u32 NWRAM_BANK_SIZE  = 256 * 1024;      // three banks, 64 KB (A) / 32 KB (B, C) slots
  static constexpr u32 BIOS9I_SIZE      = 64 * 1024;
  static constexpr u32 BIOS7I_SIZE      = 64 * 1024;
  u32 main_ram_size() const;
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
  PageBuf nwram[3], bios9i, bios7i;   // DSi: NWRAM A/B/C and the 64 KB BIOS pair (zero on a DS)
  u8* vram_bank(int i);

private:
  NDS& nds_;
  Timing timing_;
  gpu::VramMap vram_map_;
  u32 vram_read(Cpu cpu, u32 addr, u32 width);
  void vram_write(Cpu cpu, u32 addr, u32 width, u32 v);
  void map_fixed_regions();
  void map_main_ram(PageTable& pt);
  void map_page_aligned(PageTable& pt, u32 guest, u32 size, u8* host, u32 flags, u32 mirror_end);
  // DSi NWRAM slot tables, rebuilt from MBK1-5 by update_nwram: [bank][cpu 0 ARM9 / 1 ARM7 / 2 DSP][slot].
  u8* nwram_map_[3][3][8] = {};
  void nwram_windows(int c, bool nwram, u32 win[3][2]) const;
  u8* wram_cell_host(int c, const u32 win[3][2], u32 a) const;   // the 16 KB cell at `a`: WRAMCNT under the windows
  u8* wram_cells_[2][0x400] = {};   // what the last apply mapped, per cell (see apply_wram)
  void apply_wram(bool nwram, bool windows_only);           // lay both CPUs and remap what changed
  u32 wram_key_[2] = {~0u, ~0u};   // what the last apply laid under the windows (~0: nothing yet)
  u32 wram_win_[2][3][2] = {};     // and the windows it laid
public:
  // The non-RAM access path (I/O, VRAM slow blocks, GBA slot). The JIT's
  // slow-store helper enters here for I/O so it gets the GXFIFO fast path.
  u32 io_read(Cpu cpu, u32 addr, u32 width);
  void io_write(Cpu cpu, u32 addr, u32 width, u32 v);
};

} // namespace ds::mem
