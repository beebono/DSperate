// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cpu/cpu.h"
#include "core/mem/bus.h"
#include "core/sched/scheduler.h"
#include "core/gpu/gpu.h"
#include "core/spu/spu.h"
#include "core/io/io.h"

#include <memory>
#include <string>
#include <vector>

namespace ds {

struct NDS {
  NDS();
  ~NDS();

  void reset();
  bool load_rom(const std::string& path);
  void run_frame();

  CpuContext& cpu(Cpu which) { return which == Cpu::ARM9 ? *arm9 : *arm7; }

  // Execution engines, swappable per CPU (interpreter / JIT).
  RunFn run_arm9;
  RunFn run_arm7;

  // CpuContexts are heap-allocated: each owns a 16 MiB page table mapping and
  // the JIT wants a stable address.
  std::unique_ptr<CpuContext> arm9, arm7;

  mem::Bus   bus;
  Scheduler  sched;
  gpu::Gpu   gpu;
  spu::Spu   spu;
  io::Io     io;

  std::vector<u8> rom;
  u64 frame_count = 0;
};

} // namespace ds
