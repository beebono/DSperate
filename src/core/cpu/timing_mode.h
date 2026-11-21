// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The CPU timing model (emu.timing / DS_TIMING), process-wide and fixed at
// boot, before the recompiler emits its stubs.
//   exact  melonDS's per-access model (cpu_cycles.h): per-page costs, the
//          CD/CDI overlap, the ARM7 main-RAM rules, per-target refills.
//   fast   (the default where the recompiler implements it) every data
//          access costs a constant, loads and stores pay numC + data with no
//          overlap and no region rules, and every jump's refill is a constant.
//   turbo  fast with the ARM9 charged less than the hardware charges it: a
//          game that sets its frame rate by how long its frame took (Okamiden
//          drops from 30 to 20 fps in its wide views, on the hardware's
//          timing too) holds the higher rate. Not the console's speed, so a
//          game that leans on it may misbehave.
// What fast removes is the per-access timing code in translated code (a table
// load and the combine) and the refill lookup of every indirect branch; numC,
// resolved when a block is translated, stays exact. The cartridge keeps its
// exact timing under fast (games break without it: Io::cart_write_romctrl,
// Io::cart_word_delay); DS_CART_GAPS=0 / DS_CART_CLOCK=0 are the old shortcuts.
#pragma once
#include "core/cpu/cpu.h"

namespace ds {

// The ARM9's data cost depends on the base register, a translation-time fact:
// SP-based accesses (the stack, in DTCM in nearly every game) cost d9s, all
// others (main RAM, I/O) d9. One constant cannot fit both. The exact model
// overlaps a DTCM access with the fetch, so stack-heavy frame logic (ST, NSMB)
// pays almost nothing for data, while a main-RAM access costs ~5 over numC.
// A single constant low enough for the former made main-RAM-bound idle work
// (PW2's) iterate ~40% more per frame, and one at the whole-run average (4)
// pushed ST's and NSMB's frame logic past VBlank, halving their 3D rate.
//
// d9 and r9 were 5 and 4 until 2026-11-20: that charged main-RAM-bound games
// more than the exact model does. Okamiden's still 900-polygon view ran at
// 15 fps (exact: 20) and its pans dropped frames exact does not; Dragon Ball:
// Origins showed 415 new top-screen frames in 1800 against exact's 458. With
// 4 and 3 both match or pass exact, and the boot scenes (GS:DD, ST, PH, NSMB,
// Mario Kart) keep their frame counts.
// Turbo: Okamiden's wide views hold 30 fps from d9 2, r9 2 down (one slip in
// 240 frames at exactly that); 1 and 2 leave a margin. The ARM7 is left alone.
struct FastTiming { u32 d9s = 0, d9 = 4, d7 = 2, r9 = 3, r7 = 2; };
constexpr FastTiming kTurboTiming{0, 1, 2, 2, 2};
extern bool g_fast_timing;
extern FastTiming g_fast;
// "exact" | "fast" | "turbo": sets the two above. False (and nothing changes)
// for any other word. Before anything runs, as the recompiler bakes them in.
// DS_FAST's constants, when set, stay over fast's and turbo's.
bool set_timing(const char* name);
const char* timing_name();

// Whether a load/store addresses through SP. ARM: every load/store encoding
// has its base in bits 19-16 (and the cost is only read by those). Thumb: only
// SP-relative LDR/STR and PUSH/POP use SP; every other form has a low base.
inline bool fast_sp_based(u32 instr, bool thumb) {
  return thumb ? ((instr & 0xF000) == 0x9000 || (instr & 0xF600) == 0xB400) : ((instr >> 16) & 15) == 13;
}
inline u32 fast_data(const CpuContext& cpu, u32 instr, bool thumb) {
  if (cpu.which != Cpu::ARM9) return g_fast.d7;
  return fast_sp_based(instr, thumb) ? g_fast.d9s : g_fast.d9;
}

} // namespace ds
