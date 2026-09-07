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
#include "core/cart/zip.h"
#include "core/cheat/ar_engine.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "core/bios/freebios.h"

namespace ds {
namespace state { class Writer; class Reader; }

// Per-instruction trace callback: called with r15 already pipeline-adjusted
// (instruction address + 8 / + 4) before the instruction executes.
using TraceFn = void (*)(CpuContext& cpu, u32 instr, void* user);

struct NDS {
  NDS();
  ~NDS();

  void reset();
  // Load the boot images. Any path may be empty: an empty BIOS path takes the
  // built-in FreeBIOS, an empty firmware path a generated firmware carrying
  // `user` (core/bios/freebios.h). Fails when a given path cannot be read or
  // has the wrong size. What was substituted is reported by bios_native /
  // firmware_synthetic below; the frontends say so, since the substitutes
  // only support direct boot and do not match Nintendo's timing.
  bool load_bios(const std::string& bios9, const std::string& bios7, const std::string& firmware,
                 const bios::UserSettings& user = {});
  bool bios_native = false;          // both BIOS images came from dumps
  bool firmware_synthetic = false;   // the firmware was generated, not dumped (set by load_bios)
  // Direct boot is the only boot the substitutes support: FreeBIOS has no
  // boot code, and the generated firmware has no DS menu to boot into.
  bool can_boot_firmware() const { return bios_native && !firmware_synthetic; }
  bool load_rom(const std::string& path);
  // A ROM the caller already has in memory -- the frontend's built-in loader
  // cart. Identical to the path form from the slot's side: the same identity
  // hash, so saves and states are keyed the same way.
  bool load_rom_image(std::vector<u8> image);
  // The general form: any RomSource (a mapping, a range of a zip, memory).
  bool load_rom_source(std::unique_ptr<cart::RomSource> src);
  // Which entry a zipped ROM came from, empty when it was a loose .nds. Only
  // for reporting -- nothing about the machine depends on it.
  std::string rom_zip_entry;
  // Loading a zip: where the extracted image goes when the archive's own
  // directory cannot be written (see cart/zip_cache.h), and a progress
  // callback for the extraction. Both optional; set before load_rom.
  std::string rom_cache_dir;
  u64 rom_cache_max_bytes = 0;                 // 0: no limit
  cart::ZipProgress rom_progress = nullptr;
  void* rom_progress_user = nullptr;
  std::atomic<bool>* rom_cancel = nullptr;     // set from another thread to abandon the extraction
  // The extracted image the current cart was mapped from, empty otherwise.
  std::string rom_cache_path;
  void normalise_touch_calibration();   // see nds.cpp; called by load_bios
  void setup_direct_boot();          // skip the firmware: load the ROM's binaries and jump to them

  // Firmware settings persistence.
  //
  // Booting the firmware lets the console be set up from inside it -- the
  // nickname, birthday, favourite colour, message, language -- and the
  // firmware saves those by writing its own flash over SPI. Rather than write
  // those bytes back into the user's firmware.bin, which is a dump they
  // cannot regenerate, the changed 256-byte pages are kept in a sidecar file
  // and re-applied over the pristine image at load. Deleting the sidecar
  // restores the console to whatever the dump says.
  //
  // Both take the sidecar's path and report the reason on failure. save_
  // returns true and writes nothing when no page has changed.
  static constexpr u32 FW_PAGE = 256;   // the flash's page, and the sidecar's granularity
  bool load_firmware_override(const std::string& path, std::string& err);
  bool save_firmware_override(const std::string& path, std::string& err);
  bool firmware_override_dirty() const { return fw_dirty_pages > 0; }
  void firmware_written(u32 offset);   // called from the SPI page-write path
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
  // Which 256-byte pages of `firmware` differ from the dump on disk: one flag
  // per page, set by the SPI write path and by a loaded override (so a page
  // written in an earlier session is still written out by this one).
  std::vector<u8> fw_page_dirty;
  u32 fw_dirty_pages = 0;
  u64 firmware_id = 0;           // identity of the pristine dump; an override names it

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
  // The ARM7 has pulled the power line down (PMIC register 0 bit 6): the
  // console has switched itself off. The firmware does it on the way out of
  // its settings pages, which is the point at which the settings it just
  // wrote are complete. Cleared by reset(); what a power-off means is the
  // frontend's decision -- SDL saves the settings sidecar and reboots.
  bool power_off = false;

  TraceFn trace = nullptr;
  void*   trace_user = nullptr;
};

} // namespace ds
