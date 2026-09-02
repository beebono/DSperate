// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cpu/cpu.h"
#include "core/mem/bus.h"
#include "core/sched/scheduler.h"
#include "core/gpu/gpu.h"
#include "core/gpu/gpu3d.h"
#include "core/spu/spu.h"
#include "core/io/io.h"
#include "core/dma/dma.h"
#include "core/cart/cart.h"
#include "core/cheat/ar_engine.h"

#include <memory>
#include <string>
#include <vector>

namespace ds {
namespace state { class Writer; class Reader; }

// Per-instruction trace callback: called with r15 already pipeline-adjusted
// (instruction address + 8 / + 4) before the instruction executes.
using TraceFn = void (*)(CpuContext& cpu, u32 instr, void* user);

struct NDS {
  NDS();
  ~NDS();

  void reset();
  bool load_bios(const std::string& bios9, const std::string& bios7, const std::string& firmware);
  bool load_rom(const std::string& path);
  void normalise_touch_calibration();   // see nds.cpp; called by load_bios
  void setup_direct_boot();          // skip the firmware: load the ROM's binaries and jump to them
  void run_frame();

  // Save states (core/state/state.h): whole-machine snapshots, taken only
  // where run_frame() returns. save_state does not disturb the run; a
  // failed load_state leaves the machine unusable (reset it). `err` gets
  // the reason on failure.
  bool save_state(state::Writer& w, std::string& err);
  bool load_state(state::Reader& r, std::string& err);

  CpuContext& cpu(Cpu which) { return which == Cpu::ARM9 ? *arm9 : *arm7; }

  // Execution engines, swappable per CPU (interpreter / JIT).
  RunFn run_arm9;
  RunFn run_arm7;

  // CpuContexts are heap-allocated: each owns a 16 MiB page table mapping and
  // the JIT wants a stable address.
  std::unique_ptr<CpuContext> arm9, arm7;

  // Loaded images (declared before the subsystems, which read them in reset()).
  // The ROM image itself lives only in the Cart -- a 256 MB dump held twice is
  // half a gigabyte of resident memory on a handheld -- so what survives here
  // is the identity a save state checks against.
  u64 rom_id = 0;
  std::vector<u8> firmware;

  mem::Bus   bus;
  Scheduler  sched;
  gpu::Gpu   gpu;
  gpu::Gpu3D gpu3d;
  spu::Spu   spu;
  io::Io     io;
  dma::Dma   dma;
  std::unique_ptr<cart::Cart> cart;

  // Action Replay codes, run from the ARM7's VBlank IRQ (CpuContext::check_irq)
  // when any are enabled. Not part of a save state: which cheats are on is the
  // frontend's business, and a state should load the same either way.
  cheat::Engine cheats;

  u64  frame_count = 0;
  bool frame_ready = false;

  TraceFn trace = nullptr;
  void*   trace_user = nullptr;
};

} // namespace ds
