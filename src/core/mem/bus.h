// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/mem/page_table.h"

#include <memory>

namespace ds {
struct NDS;
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

  // Slow paths, reached when the page table returns nullptr.
  u8  read8 (Cpu cpu, u32 addr);
  u16 read16(Cpu cpu, u32 addr);
  u32 read32(Cpu cpu, u32 addr);
  void write8 (Cpu cpu, u32 addr, u8  v);
  void write16(Cpu cpu, u32 addr, u16 v);
  void write32(Cpu cpu, u32 addr, u32 v);

  // Physical memories (sizes per GBATEK).
  static constexpr u32 MAIN_RAM_SIZE   = 4 * 1024 * 1024;
  static constexpr u32 SHARED_WRAM_SIZE = 32 * 1024;
  static constexpr u32 ARM7_WRAM_SIZE  = 64 * 1024;
  static constexpr u32 ITCM_SIZE       = 32 * 1024;
  static constexpr u32 DTCM_SIZE       = 16 * 1024;
  static constexpr u32 VRAM_TOTAL      = 656 * 1024;

  std::unique_ptr<u8[]> main_ram, shared_wram, arm7_wram, itcm, dtcm, vram;

private:
  NDS& nds_;
  void map_fixed_regions();
};

} // namespace ds::mem
