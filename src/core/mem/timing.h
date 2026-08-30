// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <atomic>
#include <memory>

namespace ds { struct CpuContext; }

namespace ds::mem {

enum Region : u8 {
  REGION_NONE = 0, REGION_BIOS, REGION_MAIN_RAM, REGION_WRAM, REGION_IO, REGION_PALETTE,
  REGION_VRAM, REGION_OAM, REGION_GBA_ROM, REGION_GBA_RAM, REGION_WIFI0, REGION_WIFI1,
};

// Bus access timing per region, in system (ARM7) cycles.
//   ARM9 bus table: per 16 KB, [0..3] CPU N16,S16,N32,S32 (N includes the
//   3-cycle non-sequential penalty outside main RAM), [4..7] DMA N16,S16,N32,S32.
//   ARM7 bus table: per 32 KB, N16,S16,N32,S32 (CPU and DMA alike).
//   ARM9 CPU table: per 4 KB, derived from the bus table and the PU's
//   cacheability map: [0] code cost in ARM9 cycles (0xFF = cacheable),
//   [1] data N16, [2] data N32, [3] data S32, in ARM9 cycles.
class Timing {
public:
  // Bumped at the END of every retime (set_region9/7): translation bakes
  // values from these tables, so a block built under one stamp is not the
  // block that would be built under another. The JIT pre-translation worker
  // records the stamp (acquire) before it reads the tables and its output is
  // discarded at adoption when the stamp has moved -- bumping after the
  // writes (release) makes that a seqlock: a build overlapping a retime can
  // never be adopted, whichever halves of the tables it saw.
  std::atomic<u64> stamp{0};

  // ARM7 precomputed data-cost table. The ARM7 rule --
  // costs add when code and data share a region, overlap into a max when they
  // do not -- has exactly one dynamic input, the data page; `nc`, `cdi`,
  // `code_main` and the access width are all known when a block is translated.
  // So the whole model is evaluated here once and the recompiler spends a
  // load, replacing a mispredictable branch over up to fourteen instructions.
  //
  // 32 bytes per 32 KB page: [code_main][cdi][nc_idx][word]. It is allocated
  // after the raw ARM7 bus table so the pinned timing register still points at
  // the raw table and every existing user of it is unchanged; the recompiler
  // reaches the cost table with one add of COST7_OFFSET.
  static constexpr u32 COST7_STRIDE = 32;
  static constexpr u32 BUS7_BYTES   = 0x20000 * 4;
  static constexpr u32 COST7_OFFSET = BUS7_BYTES;
  static constexpr u32 COST7_BYTES  = 0x20000 * COST7_STRIDE;
  static constexpr u32 NC7_SLOTS    = 4;

  Timing();
  void reset();

  // Region definitions. Addresses are in bytes; the table granularity rounds.
  void set_region9(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq);
  void set_region7(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq);

  // Rebuild [start, end) of the ARM9 per-4 KB CPU table from the PU map; `notify` reports the change to
  // the recompiler (pass false while rebuilding several ranges, then call
  // notify_cpu9 once).
  void update_cpu9(const CpuContext& cpu, u32 start, u32 end, bool notify = true);
  void notify_cpu9(const CpuContext& cpu);

  const u8 (*cpu9() const)[8] { return reinterpret_cast<const u8 (*)[8]>(cpu9_.get()); }
  const u8 (*cpu7() const)[4] { return reinterpret_cast<const u8 (*)[4]>(bus7()); }

  // Base of the ARM7 cost table. It sits at +COST7_OFFSET from the raw bus
  // table, which is what the recompiler pins, so one add reaches it.
  const u8* cost7() const { return tim7_.get() + COST7_OFFSET; }
  // Slot for a code-fetch cost, or -1 when this `nc` was not one of the values
  // the region table produces (the recompiler then keeps the inline model).
  int nc7_index(u32 nc) const {
    for (u32 i = 0; i < NC7_SLOTS; ++i) if (nc7_values_[i] == nc) return static_cast<int>(i);
    return -1;
  }
  static u32 cost7_offset(bool code_main, bool cdi, u32 nc_idx, bool word) {
    return (code_main ? 16u : 0u) + (cdi ? 8u : 0u) + nc_idx * 2u + (word ? 1u : 0u);
  }
  u32 region(bool arm9, u32 addr) const { return arm9 ? regions9_[addr >> 14] : regions7_[addr >> 15]; }
  void dma_cost(bool arm9, u32 addr, bool word, u32& n, u32& s) const {
    if (arm9) { const u8* t = &bus9_[(addr >> 14) * 8]; n = t[word ? 6 : 4]; s = t[word ? 7 : 5]; }
    else      { const u8* t = &bus7()[(addr >> 15) * 4]; n = t[word ? 2 : 0]; s = t[word ? 3 : 1]; }
  }

  // PU map for the ARM9: per 4 KB, bit 4 = data cacheable, bit 6 = code cacheable.
  std::unique_ptr<u8[]> pu_map;

private:
  std::unique_ptr<u8[]> bus9_;     // 0x40000 * 8
  std::unique_ptr<u8[]> regions9_; // 0x40000
  // [0, BUS7_BYTES) the raw ARM7 bus table, then the precomputed cost table.
  std::unique_ptr<u8[]> tim7_;
  u8* bus7() const { return tim7_.get(); }
  u8* cost7_rw() const { return tim7_.get() + COST7_OFFSET; }
  void build_cost7();                                  // full: rescan nc values, then all pages
  void build_cost7_range(u32 first_page, u32 last_page);
  u32  nc7_values_[NC7_SLOTS] = {};
  bool cost7_ready_ = false;   // set_region7 keeps the table current once this is true
  std::unique_ptr<u8[]> regions7_; // 0x20000
  std::unique_ptr<u8[]> cpu9_;     // 0x100000 * 4
};

} // namespace ds::mem
