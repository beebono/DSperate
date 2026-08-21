// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>

namespace ds::gpu {

// Engine-side views of VRAM.
//
// The nine VRAM banks are switched between the CPUs, the two 2D engines and
// the 3D unit by VRAMCNT. Each consumer sees a linear address space made of
// 16 KB blocks; a block may be backed by zero, one or several banks (the
// hardware ORs overlapping banks on read and writes to all of them). The maps
// below record, per block, the set of banks (bit i = bank A+i) and a direct
// pointer when exactly one bank backs it, which is the common case and the
// only one the renderers' fast paths take.
struct VramView {
  static constexpr u32 BLOCK = 0x4000;
  u32 size = 0;                       // bytes covered (power of two)
  std::array<u16, 32> mask{};         // per block
  std::array<u8*, 32> ptr{};          // per block, nullptr unless exactly one bank

  u32 blocks() const { return size / BLOCK; }
  u32 addr_mask() const { return size - 1; }

  // Direct pointer for `len` bytes at `addr` when the block is unique and the
  // range does not cross a block; nullptr otherwise (caller uses read8/16).
  const u8* direct(u32 addr, u32 len) const {
    addr &= addr_mask();
    if (((addr & (BLOCK - 1)) + len) > BLOCK) return nullptr;
    return ptr[addr / BLOCK] ? ptr[addr / BLOCK] + (addr & (BLOCK - 1)) : nullptr;
  }
};

class VramMap {
public:
  // Rebuild every view from VRAMCNT A..I. `banks[i]` points at bank i's storage.
  void rebuild(const u8 vramcnt[9], u8* const banks[9]);

  VramView abg, aobj, bbg, bobj;          // 512 K, 256 K, 128 K, 128 K
  VramView abg_extpal, bbg_extpal;        // 32 K each (4 slots x 8 K)
  VramView aobj_extpal, bobj_extpal;      // 8 K each
  VramView texture;                       // 512 K (4 x 128 K slots)
  VramView texpal;                        // 128 K (8 x 16 K)
  VramView arm7;                          // 256 K (2 x 128 K)
  u32 lcdc_mask = 0;                      // banks in LCDC mode

  u8  read8 (const VramView& v, u32 addr) const;
  u16 read16(const VramView& v, u32 addr) const;
  u32 read32(const VramView& v, u32 addr) const;
  void write8 (const VramView& v, u32 addr, u8 val) const;
  void write16(const VramView& v, u32 addr, u16 val) const;
  void write32(const VramView& v, u32 addr, u32 val) const;

  u8* bank(int i) const { return banks_[i]; }
  static constexpr u32 BANK_MASK[9] = {0x1FFFF, 0x1FFFF, 0x1FFFF, 0x1FFFF, 0xFFFF, 0x3FFF, 0x3FFF, 0x7FFF, 0x3FFF};

private:
  u8* banks_[9] = {};
  void add(VramView& v, u32 base, u32 len, int bank);
  void finish(VramView& v);
};

} // namespace ds::gpu
