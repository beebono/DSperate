// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.

#pragma once
#include "core/cpu/cpu.h"
#include "core/mem/bus.h"
#include "core/nds.h"

#include <cstring>

namespace ds {

inline u32 rotr32(u32 v, u32 n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

// DS_CENSUS=1 (interp.cpp): every executed data access, for sizing the JIT's
// per-access cost accounting. Null unless the census is on; the interpreter is
// the only caller of data_cost, so this never reaches a shipped fast path.
extern void (*g_census_access)(bool a9, u32 addr, bool seq);

// ---- cost model -------------------------------------------------------------
// width: 0 = 8/16-bit, 1 = 32-bit.
inline void data_cost(CpuContext& cpu, u32 addr, int width, bool seq) {
  if (g_census_access) g_census_access(cpu.which == Cpu::ARM9, addr, seq);
  u32 c;
  if (cpu.which == Cpu::ARM9) {
    const u8* t = cpu.timing9[addr >> 12];   // TCM windows are baked into the table
    c = seq ? t[3] : t[width ? 2 : 1];
  } else {
    const u8* t = cpu.timing7[addr >> 15];
    c = seq ? t[width ? 3 : 1] : t[width ? 2 : 0];
  }
  if (seq) cpu.data_cycles += c; else { cpu.data_cycles = c; cpu.data_region = addr >> 24; }
}

inline u8 mem_read8(CpuContext& cpu, u32 addr, bool seq = false) {
  data_cost(cpu, addr, 0, seq);
  if (u8* p = cpu.page_table.read_ptr(addr)) return *p;
  return cpu.nds->bus.read8(cpu.which, addr);
}
inline u16 mem_read16(CpuContext& cpu, u32 addr, bool seq = false) {
  addr &= ~1u;
  data_cost(cpu, addr, 0, seq);
  if (u8* p = cpu.page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return cpu.nds->bus.read16(cpu.which, addr);
}
inline u32 mem_read32(CpuContext& cpu, u32 addr, bool seq = false) {
  addr &= ~3u;
  data_cost(cpu, addr, 1, seq);
  if (u8* p = cpu.page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return cpu.nds->bus.read32(cpu.which, addr);
}

inline void mem_write8(CpuContext& cpu, u32 addr, u8 v, bool seq = false) {
  data_cost(cpu, addr, 0, seq);
  bool code = false;
  if (u8* p = cpu.page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &v, 1); else *p = v; return; }
  cpu.nds->bus.write8(cpu.which, addr, v);
}
inline void mem_write16(CpuContext& cpu, u32 addr, u16 v, bool seq = false) {
  addr &= ~1u;
  data_cost(cpu, addr, 0, seq);
  bool code = false;
  if (u8* p = cpu.page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &v, 2); else std::memcpy(p, &v, 2); return; }
  cpu.nds->bus.write16(cpu.which, addr, v);
}
inline void mem_write32(CpuContext& cpu, u32 addr, u32 v, bool seq = false) {
  addr &= ~3u;
  data_cost(cpu, addr, 1, seq);
  bool code = false;
  if (u8* p = cpu.page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &v, 4); else std::memcpy(p, &v, 4); return; }
  cpu.nds->bus.write32(cpu.which, addr, v);
}

// Instruction fetch (no cost: the prefetch cost is charged by the run loop).
inline u32 fetch32(CpuContext& cpu, u32 addr) {
  if (u8* p = cpu.page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  return cpu.nds->bus.read32(cpu.which, addr);
}
inline u16 fetch16(CpuContext& cpu, u32 addr) {
  if (u8* p = cpu.page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  return cpu.nds->bus.read16(cpu.which, addr);
}

} // namespace ds
