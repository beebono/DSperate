// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/bus.h"
#include "core/nds.h"

#include <cstring>

namespace ds::mem {

Bus::Bus(NDS& nds)
    : main_ram(new u8[MAIN_RAM_SIZE]), shared_wram(new u8[SHARED_WRAM_SIZE]),
      arm7_wram(new u8[ARM7_WRAM_SIZE]), itcm(new u8[ITCM_SIZE]),
      dtcm(new u8[DTCM_SIZE]), vram(new u8[VRAM_TOTAL]), nds_(nds) {}

Bus::~Bus() = default;

void Bus::reset() {
  std::memset(main_ram.get(), 0, MAIN_RAM_SIZE);
  std::memset(shared_wram.get(), 0, SHARED_WRAM_SIZE);
  std::memset(arm7_wram.get(), 0, ARM7_WRAM_SIZE);
  std::memset(itcm.get(), 0, ITCM_SIZE);
  std::memset(dtcm.get(), 0, DTCM_SIZE);
  std::memset(vram.get(), 0, VRAM_TOTAL);
  map_fixed_regions();
}

void Bus::map_fixed_regions() {
  // Main RAM, 0x02000000, mirrored through 0x02FFFFFF on both CPUs.
  for (Cpu cpu : {Cpu::ARM9, Cpu::ARM7}) {
    PageTable& pt = nds_.cpu(cpu).page_table;
    for (u32 mirror = 0x02000000; mirror < 0x03000000; mirror += MAIN_RAM_SIZE)
      pt.map(mirror, MAIN_RAM_SIZE, main_ram.get(), PAGE_READABLE | PAGE_WRITABLE);
    // I/O, 0x04000000: always slow path.
    pt.map_mmio(0x04000000, 0x01000000);
  }
  // ARM7-private WRAM, 0x03800000, mirrored through 0x03FFFFFF.
  PageTable& pt7 = nds_.cpu(Cpu::ARM7).page_table;
  for (u32 mirror = 0x03800000; mirror < 0x04000000; mirror += ARM7_WRAM_SIZE)
    pt7.map(mirror, ARM7_WRAM_SIZE, arm7_wram.get(), PAGE_READABLE | PAGE_WRITABLE);
  // TODO: shared WRAM (WRAMCNT), VRAM banks (VRAMCNT), TCM (CP15), BIOS, GBA slot.
}

// ---- slow paths: MMIO dispatch lands here. Stubs until io/ exists. ----
u8  Bus::read8 (Cpu, u32)       { return 0; }
u16 Bus::read16(Cpu, u32)       { return 0; }
u32 Bus::read32(Cpu, u32)       { return 0; }
void Bus::write8 (Cpu, u32, u8)  {}
void Bus::write16(Cpu, u32, u16) {}
void Bus::write32(Cpu, u32, u32) {}

} // namespace ds::mem
