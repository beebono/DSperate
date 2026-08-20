// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/nds.h"
#include "core/cpu/interp/interp.h"

#include <fstream>
#include <iterator>

namespace ds {

NDS::NDS()
    : run_arm9(&interp::run), run_arm7(&interp::run),
      arm9(new CpuContext), arm7(new CpuContext),
      bus(*this), sched(*this), gpu(*this), spu(*this), io(*this) {
  arm9->reset(Cpu::ARM9, this);
  arm7->reset(Cpu::ARM7, this);
  arm9->hot.other_cpu = reinterpret_cast<u64>(arm7.get());
  arm7->hot.other_cpu = reinterpret_cast<u64>(arm9.get());
  reset();
}

NDS::~NDS() = default;

void NDS::reset() {
  arm9->reset(Cpu::ARM9, this);
  arm7->reset(Cpu::ARM7, this);
  bus.reset();
  sched.reset();
  gpu.reset();
  spu.reset();
  io.reset();
  frame_count = 0;
}

bool NDS::load_rom(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  rom.assign(std::istreambuf_iterator<char>(f), {});
  return !rom.empty();
}

void NDS::run_frame() {
  sched.run_until(sched.now() + CYCLES_PER_FRAME);
  ++frame_count;
}

} // namespace ds
