// SPDX-License-Identifier: GPL-3.0-or-later
// LDM/STM with the base register in the list: the two CPUs differ (GBATEK
// "ARM Opcodes: Memory: Block Data Transfer", as melonDS implements it). One
// place, so the interpreter and both recompilers cannot drift apart.
#pragma once

#include "core/types.h"

namespace ds::cpu {

// LDM with writeback: does the written-back base replace what the list loaded
// into the base register?  ARM7 (ARMv4): never, the loaded value stays.
// ARM9 (ARMv5), ARM state: yes when the base is the only register in the list
// or not the last one.  Thumb LDMIA keeps the loaded value on both.
inline bool ldm_writeback_wins(bool arm9, bool thumb, u32 list, u32 rn) {
  const u32 bit = 1u << rn;
  if (!(list & bit)) return true;
  if (!arm9 || thumb) return false;
  return !(list & ~bit & 0xFFFF) || (list & ~((2u << rn) - 1) & 0xFFFF);
}

// STM with writeback storing its own base: the written-back value instead of
// the original?  ARM7: yes unless the base is the first register stored.
// ARM9: never, the original base is stored.
inline bool stm_stores_new_base(bool arm9, bool first) { return !arm9 && !first; }

} // namespace ds::cpu
