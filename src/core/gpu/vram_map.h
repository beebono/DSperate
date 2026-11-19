// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>
#include <cstring>

namespace ds::gpu {

// Engine-side views of VRAM: nine banks switched between CPUs/2D/3D by VRAMCNT. Each consumer
// sees 16 KB blocks; a block may be backed by 0-several banks (hardware ORs on read, writes go
// to all). Per block: bank set (bit i = bank A+i) and a direct pointer when exactly one backs it.
struct VramView {
  static constexpr u32 BLOCK = 0x4000;
  u32 size = 0;                       // bytes covered (power of two)
  std::array<u16, 32> mask{};         // per block
  std::array<u8*, 32> ptr{};          // per block, nullptr unless exactly one bank

  u32 blocks() const { return size / BLOCK; }
  u32 addr_mask() const { return size - 1; }

  // Direct pointer for `len` bytes at `addr` if the block is unique and not crossed; else nullptr.
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
  // Point the views at other storage for the banks (a copy of this map reading a copy of the banks).
  void rebase(u8* const banks[9]);
  static constexpr u32 BANK_MASK[9] = {0x1FFFF, 0x1FFFF, 0x1FFFF, 0x1FFFF, 0xFFFF, 0x3FFF, 0x3FFF, 0x7FFF, 0x3FFF};

  // Change tracking for CPU-unmapped views: `generation` counts rebuilds; `bank_writable_gen(b)`
  // is the last generation bank b was CPU-reachable. Bytes read at generation g are unchanged if
  // the same banks back them and none has a writable generation above g. `block_signature`
  // identifies which banks back a range block by block, so a bank swap between slots counts.
  u32 generation() const { return gen_; }
  u32 bank_writable_gen(int bank) const { return writable_gen_[bank]; }
  u64 block_signature(const VramView& v, u32 addr, u32 len, u32& banks) const;

  // A map over a copy of the texture and palette views' bytes alone (512 K + 128 K, contiguous each), for a
  // reader on another thread (Renderer3D's submit thread). Every block has one backing, and the nine "banks"
  // of the change tracking stand for groups of blocks: copy_changed() says which blocks of the copy were
  // rewritten, and a reader's bytes are then unchanged unless a block of theirs is in a group that was.
  void over_copy(u8* texture_bytes, u8* texpal_bytes) {
    texture.size = 0x80000; texpal.size = 0x20000;
    for (u32 b = 0; b < 32; ++b) { texture.ptr[b] = texture_bytes + b * VramView::BLOCK; texture.mask[b] = static_cast<u16>(1u << (b % 9)); }
    for (u32 b = 0; b < 8; ++b) { texpal.ptr[b] = texpal_bytes + b * VramView::BLOCK; texpal.mask[b] = static_cast<u16>(1u << ((32 + b) % 9)); }
  }
  void copy_changed(u32 texture_blocks, u32 texpal_blocks) {
    ++gen_;
    for (u32 b = 0; b < 32; ++b) if ((texture_blocks >> b) & 1u) writable_gen_[b % 9] = gen_;
    for (u32 b = 0; b < 8; ++b) if ((texpal_blocks >> b) & 1u) writable_gen_[(32 + b) % 9] = gen_;
  }

private:
  u8* banks_[9] = {};
  u32 gen_ = 0, writable_gen_[9] = {};
  void add(VramView& v, u32 base, u32 len, int bank);
  void finish(VramView& v);
};

// Per-pixel fetch for scalar renderer paths: unique-bank case is one lookup, else OR-read.
inline u8 vram_fetch8(const VramMap& vm, const VramView& v, u32 a) {
  a &= v.addr_mask();
  const u8* p = v.ptr[a / VramView::BLOCK];
  return p ? p[a & (VramView::BLOCK - 1)] : vm.read8(v, a);
}
inline u16 vram_fetch16(const VramMap& vm, const VramView& v, u32 a) {
  a &= v.addr_mask() & ~1u;
  const u8* p = v.ptr[a / VramView::BLOCK];
  if (!p) return vm.read16(v, a);
  u16 r; std::memcpy(&r, p + (a & (VramView::BLOCK - 1)), 2); return r;
}

} // namespace ds::gpu
