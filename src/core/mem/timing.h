// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

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
  Timing();
  void reset();

  // Region definitions. Addresses are in bytes; the table granularity rounds.
  void set_region9(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq);
  void set_region7(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq);

  // Rebuild the ARM9 per-4 KB CPU table from the PU map for [start, end).
  void update_cpu9(const CpuContext& cpu, u32 start, u32 end);

  const u8 (*cpu9() const)[4] { return reinterpret_cast<const u8 (*)[4]>(cpu9_.get()); }
  const u8 (*cpu7() const)[4] { return reinterpret_cast<const u8 (*)[4]>(bus7_.get()); }
  u32 region(bool arm9, u32 addr) const { return arm9 ? regions9_[addr >> 14] : regions7_[addr >> 15]; }
  void dma_cost(bool arm9, u32 addr, bool word, u32& n, u32& s) const {
    if (arm9) { const u8* t = &bus9_[(addr >> 14) * 8]; n = t[word ? 6 : 4]; s = t[word ? 7 : 5]; }
    else      { const u8* t = &bus7_[(addr >> 15) * 4]; n = t[word ? 2 : 0]; s = t[word ? 3 : 1]; }
  }

  // PU map for the ARM9: per 4 KB, bit 4 = data cacheable, bit 6 = code cacheable.
  std::unique_ptr<u8[]> pu_map;

private:
  std::unique_ptr<u8[]> bus9_;     // 0x40000 * 8
  std::unique_ptr<u8[]> regions9_; // 0x40000
  std::unique_ptr<u8[]> bus7_;     // 0x20000 * 4
  std::unique_ptr<u8[]> regions7_; // 0x20000
  std::unique_ptr<u8[]> cpu9_;     // 0x100000 * 4
};

} // namespace ds::mem
