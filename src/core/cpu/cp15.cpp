// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/cp15.h"
#include "core/nds.h"

#include <cstring>

namespace ds {

// Rebuild the per-4 KB cacheability map from the PU regions and refresh the
// ARM9 timing table. Region 7 has the highest priority.
static void update_pu_map(CpuContext& cpu) {
  mem::Timing& t = cpu.nds->bus.timing();
  u8* map = t.pu_map.get();
  const u32 ctl = cpu.cp15_control;
  if (!(ctl & 1)) {                                   // PU disabled: caches apply everywhere if enabled
    u8 m = 0; if (ctl & (1u << 2)) m |= 0x10; if (ctl & (1u << 12)) m |= 0x40;
    std::memset(map, m, 0x100000);
    t.update_cpu9(cpu, 0, 0xFFFFFFFF);
    return;
  }
  std::memset(map, 0, 0x100000);
  for (int n = 0; n < 8; ++n) {
    const u32 rgn = cpu.pu_region[n];
    if (!(rgn & 1)) continue;
    const int size_bits = static_cast<int>((rgn >> 1) & 0x1F) - 11;   // in 4 KB pages
    const u32 size = size_bits <= 0 ? 1 : (size_bits >= 20 ? 0x100000 : (1u << size_bits));
    const u32 start = ((rgn >> 12) / size) * size;
    u8 m = 0;
    if ((ctl & (1u << 2)) && ((cpu.pu_data_cacheable >> n) & 1)) m |= 0x10;
    if ((ctl & (1u << 12)) && ((cpu.pu_code_cacheable >> n) & 1)) m |= 0x40;
    for (u32 i = start; i < start + size && i < 0x100000; ++i) map[i] = m;
  }
  t.update_cpu9(cpu, 0, 0xFFFFFFFF);
}

u32 cp15_read(CpuContext& cpu, u32 opc1, u32 crn, u32 crm, u32 opc2) {
  switch ((crn << 8) | (crm << 4) | opc2) {
  case 0x000: return 0x41059461;            // main ID: ARM946E-S rev 1
  case 0x001: return 0x0F0D2112;            // cache type: 8K I / 4K D, 4-way, 32-byte lines
  case 0x002: return 0x00140180;            // TCM size: 32K ITCM, 16K DTCM
  case 0x100: return cpu.cp15_control;
  case 0x200: return cpu.pu_data_cacheable;
  case 0x201: return cpu.pu_code_cacheable;
  case 0x300: return cpu.pu_data_bufferable;
  case 0x502: return cpu.pu_data_perm;
  case 0x503: return cpu.pu_code_perm;
  case 0x600: case 0x610: case 0x620: case 0x630: case 0x640: case 0x650: case 0x660: case 0x670:
  case 0x601: case 0x611: case 0x621: case 0x631: case 0x641: case 0x651: case 0x661: case 0x671:
    return cpu.pu_region[crm];
  case 0x910: return cpu.cp15_dtcm;
  case 0x911: return cpu.cp15_itcm;
  default:    return 0;
  }
}

void cp15_write(CpuContext& cpu, u32 opc1, u32 crn, u32 crm, u32 opc2, u32 value) {
  switch ((crn << 8) | (crm << 4) | opc2) {
  case 0x100: {
    // Writable bits: M, big-endian(not), D-cache, I-cache, V (vector base),
    // RR, DTCM enable/load, ITCM enable/load.
    const u32 mask = 0x000FF085;
    const u32 old = cpu.cp15_control;
    cpu.cp15_control = (old & ~mask) | (value & mask) | 0x00000078;   // bits 3-6 read as 1
    if ((old ^ cpu.cp15_control) & 0x000D0000) cpu.nds->bus.update_tcm(cpu);
    if ((old ^ cpu.cp15_control) & 0x00001005) update_pu_map(cpu);    // PU / caches toggled
    return;
  }
  case 0x200: cpu.pu_data_cacheable = value; update_pu_map(cpu); return;
  case 0x201: cpu.pu_code_cacheable = value; update_pu_map(cpu); return;
  case 0x300: cpu.pu_data_bufferable = value; return;
  case 0x502: cpu.pu_data_perm = value; return;
  case 0x503: cpu.pu_code_perm = value; return;
  case 0x600: case 0x610: case 0x620: case 0x630: case 0x640: case 0x650: case 0x660: case 0x670:
  case 0x601: case 0x611: case 0x621: case 0x631: case 0x641: case 0x651: case 0x661: case 0x671:
    cpu.pu_region[crm] = value; update_pu_map(cpu); return;
  case 0x910: cpu.cp15_dtcm = value & 0xFFFFF03E; cpu.nds->bus.update_tcm(cpu); return;
  case 0x911: cpu.cp15_itcm = value & 0x0000003E; cpu.nds->bus.update_tcm(cpu); return;
  case 0x704: case 0x782:                   // wait for interrupt
    cpu.halted = true; cpu.hot.cycle_budget = -1; return;
  default:
    return;                                 // PU regions, cache maintenance: accepted, ignored
  }
}

} // namespace ds
