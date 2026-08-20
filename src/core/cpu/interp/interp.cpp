// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cpu/interp/interp.h"

namespace ds::interp {

// TODO: ARM/Thumb decode + execute. Semantics from the ARM ARM (ARMv5TE) and
// GBATEK; melonDS (GPLv3) is the reference implementation and the test oracle.
void run(CpuContext& cpu) {
  // Burn the budget so the scheduler makes progress while this is a stub.
  cpu.hot.cycle_budget = -1;
}

} // namespace ds::interp
