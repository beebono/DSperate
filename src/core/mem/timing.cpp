// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/timing.h"
#include "core/cpu/cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::mem {

constexpr u32 CACHE_CODE = 3, CACHE_DATA = 3;   // cycles for a cached fetch/access (line fill approximation)

Timing::Timing()
    : pu_map(new u8[0x100000]), bus9_(new u8[0x40000 * 8]), regions9_(new u8[0x40000]),
      bus7_(new u8[0x20000 * 4]), regions7_(new u8[0x20000]), cpu9_(new u8[0x100000 * 4]) {
  reset();
}

void Timing::reset() {
  std::memset(pu_map.get(), 0, 0x100000);
  set_region9(0x00000000, 0xFFFFFFFF, REGION_NONE, 32, 1, 1);
  set_region9(0xFFFF0000, 0xFFFFFFFF, REGION_BIOS, 32, 1, 1);
  set_region9(0x02000000, 0x03000000, REGION_MAIN_RAM, 16, 8, 1);
  set_region9(0x03000000, 0x04000000, REGION_WRAM, 32, 1, 1);
  set_region9(0x04000000, 0x05000000, REGION_IO, 32, 1, 1);
  set_region9(0x05000000, 0x06000000, REGION_PALETTE, 16, 1, 1);
  set_region9(0x06000000, 0x07000000, REGION_VRAM, 16, 1, 1);
  set_region9(0x07000000, 0x08000000, REGION_OAM, 32, 1, 1);
  set_region9(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, 10, 6);
  set_region9(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, 10, 10);
  set_region7(0x00000000, 0xFFFFFFFF, REGION_NONE, 32, 1, 1);
  set_region7(0x00000000, 0x00010000, REGION_BIOS, 32, 1, 1);
  set_region7(0x02000000, 0x03000000, REGION_MAIN_RAM, 16, 8, 1);
  set_region7(0x03000000, 0x04000000, REGION_WRAM, 32, 1, 1);
  set_region7(0x04000000, 0x04800000, REGION_IO, 32, 1, 1);
  set_region7(0x04800000, 0x04808000, REGION_WIFI0, 32, 1, 1);
  set_region7(0x04808000, 0x04810000, REGION_WIFI1, 32, 1, 1);
  set_region7(0x06000000, 0x07000000, REGION_VRAM, 16, 1, 1);
  // CPU table: no PU yet -> nothing cached.
  std::memset(cpu9_.get(), 0, 0x100000 * 4);
  for (u32 i = 0; i < 0x100000; ++i) {
    const u8* b = &bus9_[(i >> 2) * 8];
    u8* c = &cpu9_[i * 4];
    c[0] = static_cast<u8>(b[2] << 1); c[1] = static_cast<u8>(b[0] << 1); c[2] = static_cast<u8>(b[2] << 1); c[3] = static_cast<u8>(b[3] << 1);
  }
}

void Timing::set_region9(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq) {
  const int n16 = nonseq, s16 = seq;
  const int n32 = bus_width == 16 ? n16 + s16 : n16, s32 = bus_width == 16 ? s16 + s16 : s16;
  const int cpu_n = (r == REGION_MAIN_RAM) ? 0 : 3;
  const u32 first = start >> 14, last = (end == 0xFFFFFFFF) ? 0x40000 : (end >> 14);
  for (u32 i = first; i < last; ++i) {
    u8* t = &bus9_[i * 8];
    t[0] = static_cast<u8>(n16 + cpu_n); t[1] = static_cast<u8>(s16); t[2] = static_cast<u8>(n32 + cpu_n); t[3] = static_cast<u8>(s32);
    t[4] = static_cast<u8>(n16); t[5] = static_cast<u8>(s16); t[6] = static_cast<u8>(n32); t[7] = static_cast<u8>(s32);
    regions9_[i] = r;
  }
}

void Timing::set_region7(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq) {
  const int n16 = nonseq, s16 = seq;
  const int n32 = bus_width == 16 ? n16 + s16 : n16, s32 = bus_width == 16 ? s16 + s16 : s16;
  const u32 first = start >> 15, last = (end == 0xFFFFFFFF) ? 0x20000 : (end >> 15);
  for (u32 i = first; i < last; ++i) {
    u8* t = &bus7_[i * 4];
    t[0] = static_cast<u8>(n16); t[1] = static_cast<u8>(s16); t[2] = static_cast<u8>(n32); t[3] = static_cast<u8>(s32);
    regions7_[i] = r;
  }
}

// The TCM windows are baked into the table (4 KB pages; both windows are
// multiples of 4 KB and aligned to their size): an ITCM page costs 1 for code
// and data, a DTCM page 1 for data. Both engines then cost an access with one
// table lookup and the recompiler inherits the interpreter's model exactly.
void Timing::update_cpu9(const CpuContext& cpu, u32 start, u32 end, bool notify) {
  // DS_DEBUG_TIMING=1: log every rebuild (each one also drops every translated block).
  static const bool debug = std::getenv("DS_DEBUG_TIMING") != nullptr;
  if (debug) std::fprintf(stderr, "[timing] update_cpu9 %08x-%08x ctl %08x dtcm %08x itcm %08x pu %08x/%08x\n", start, end, cpu.cp15_control, cpu.cp15_dtcm, cpu.cp15_itcm, cpu.pu_data_cacheable, cpu.pu_code_cacheable);
  const u32 first = start >> 12, last = (end == 0xFFFFFFFF) ? 0x100000 : (end >> 12);
  for (u32 i = first; i < last; ++i) {
    const u8 pu = pu_map[i];
    const u8* b = &bus9_[(i >> 2) * 8];
    u8* c = &cpu9_[i * 4];
    const u32 addr = i << 12;
    const bool itcm = addr < cpu.itcm_size;
    const bool dtcm = (addr & cpu.dtcm_mask) == cpu.dtcm_base;
    c[0] = itcm ? 1 : (pu & 0x40) ? 0xFF : static_cast<u8>(b[2] << 1);
    if (itcm || dtcm) { c[1] = 1; c[2] = 1; c[3] = 1; }
    else if (pu & 0x10) { c[1] = CACHE_DATA; c[2] = CACHE_DATA; c[3] = 1; }
    else { c[1] = static_cast<u8>(b[0] << 1); c[2] = static_cast<u8>(b[2] << 1); c[3] = static_cast<u8>(b[3] << 1); }
  }
  if (notify) notify_cpu9(cpu);
}

void Timing::notify_cpu9(const CpuContext& cpu) {
  if (cpu.jit_timing_changed) cpu.jit_timing_changed(const_cast<CpuContext&>(cpu));
}

} // namespace ds::mem
