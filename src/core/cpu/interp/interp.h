// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/cpu/cpu.h"

namespace ds::interp {

// Runs `cpu` until its cycle budget goes negative or it halts.
// Both CPUs share this entry point; ARMv5TE-only forms trap on the ARM7.
void run(CpuContext& cpu);

} // namespace ds::interp
