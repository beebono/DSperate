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
#include "core/dma/ndma.h"
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
  // Load the boot images. A path that is empty or names no file is
  // substituted: the BIOS pair by the built-in FreeBIOS, the firmware by a
  // generated one carrying `user` (core/bios/freebios.h). A file that exists
  // but cannot be read or has the wrong size is an error (`err` says which),
  // as is one BIOS present without the other. What was substituted is
  // reported by bios_native / firmware_synthetic below; the frontends say so,
  // since the substitutes only support direct boot and do not match
  // Nintendo's timing.
  bool load_bios(const std::string& bios9, const std::string& bios7, const std::string& firmware,
                 const bios::UserSettings& user = {}, std::string* err = nullptr);
  bool bios_native = false;          // both BIOS images came from dumps
  // The DSi BIOS pair (64 KB each, dumps only: there is no DSi FreeBIOS).
  // Loaded separately since a DS game does not need them; both or neither.
  bool load_dsi_bios(const std::string& bios9i, const std::string& bios7i, std::string* err = nullptr);
  bool bios_native_dsi = false;      // the DSi pair is loaded
  // Console type. A DSi runs the DSi machine (core/nds_dsi.cpp: 16 MB main
  // RAM, NWRAM, SCFG, NDMA, the doubled ARM9 clock, the DSi BIOS pair, the
  // DSi TSC). Decided by the frontend before reset(): a DSi-capable header
  // (cart::Cart::dsi_capable) with the DSi BIOS loaded. Every DS-mode
  // path is unchanged when this is false.
  bool dsi = false;
  void set_dsi(bool on);             // before reset(); needs load_dsi_bios for true
  // What the launcher leaves in main RAM for a title, taken from the console's
  // NAND (melonDS DSi::SetupDirectBoot): the user settings block
  // (TWLCFG, 0x128 bytes at 0x02000400), HWINFO_N (0x14 at 0x02000600) and
  // HWINFO_S (0x18 at 0x02FFFD68). tools/dsi_nand.py bootblobs writes the
  // 0x154-byte file; absent, the areas stay zero.
  bool load_dsi_boot_blobs(const std::string& path, std::string* err = nullptr);
  std::vector<u8> dsi_boot_blobs;    // 0x154 bytes when loaded, else empty
  // The DSi NAND image (a real nand.bin with its nocash footer). It backs
  // the eMMC on the SD/MMC host's port 1 and supplies the console ID, which
  // seeds AES key slots 1 and 3; without one a DSiWare title cannot reach
  // its save data. Loaded before reset(), like the BIOS pair.
  bool load_dsi_nand(const std::string& path, std::string* err = nullptr, bool write_through = false);   // see NandImage
  io::NandImage dsi_nand;
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

  // The console's own settings, as the firmware holds them: the pages the DS
  // menu writes. With a dump these are the dump's, and editing them goes to
  // the sidecar like any other firmware write; with a generated firmware they
  // are what [user] built it from. Reading gives back what a game would see.
  //
  // A field at a time, deliberately. A dump's nickname may hold characters
  // this cannot represent (it is UTF-16, and these strings carry one byte per
  // code unit), and rewriting every field to change one would turn a name the
  // player never touched into mojibake.
  enum class UserField : u8 { Nickname, Message, Colour, BirthdayMonth, BirthdayDay, Language };
  bool read_user_settings(bios::UserSettings& out) const;
  // Where the two settings copies live, or 0 if the image has none.
  u32  user_settings_offset() const;
  bool write_user_settings(UserField field, const bios::UserSettings& in);
  void setup_direct_boot();          // skip the firmware: load the ROM's binaries and jump to them
  // Give this console its own Wi-Fi MAC: the firmware's OUI with `suffix`
  // as the low three bytes, checksum fixed. Every instance booted from one
  // dump shares a MAC otherwise, and local wireless titles tell players
  // apart by it (PictoChat drops a message from its own MAC). Call before
  // the boot; in-memory only, never written back.
  void set_wifi_mac_suffix(u32 suffix);
  void setup_direct_boot_dsi();      // the DSi machine's version (nds_dsi.cpp)
  // DSiWare without the launcher: set before setup_direct_boot, the DSi direct
  // boot stages what the DSi Launcher leaves a title at its entry point
  // (captured from real launches, see nds_dsi.cpp) instead of melonDS's card-
  // mode direct boot. The title then starts as if launched from the menu:
  // empty cart slot, nand:/ mounted, its own image read back from
  // nand:/title/.../content/<dsi_hle_content_id>.app.
  bool dsi_hle_launch = false;
  u32  dsi_hle_content_id = 0;
  // What the launcher hand-off needs beyond the BIOS pair, made up where the
  // user has no dump (io/dsi_nand_synth.h): with no NAND loaded, an in-memory
  // NAND holding this cart's title under a made-up console; with no
  // --dsi-boot data, the console's settings files from `user` in a region the
  // title's header allows; with a generated firmware, the DSi's. Call after
  // load_rom and load_dsi_bios, before setup_direct_boot (before or after
  // reset(): it re-attaches the NAND itself). `report` gets one line per
  // substitution.
  bool prepare_dsi_hle(const bios::UserSettings& user, std::string* err, std::vector<std::string>* report = nullptr);
  bool dsi_nand_synthetic = false;   // dsi_nand was built by prepare_dsi_hle, not loaded
  // The user's /sys/TWLFontTable.dat for a synthesised NAND (set before
  // prepare_dsi_hle); empty, DSperate's own font goes there instead.
  std::string dsi_font_path;
  // The built-in font is on the NAND: a title checking its signature with DSi
  // BIOS SWI 22h (RSA_Decrypt_Unpad) is answered with the font header's
  // SHA-1 when the signature it passes is the built-in font's marker. Called
  // by the interpreter (and the recompilers' fallback) for every SWI while
  // set; true when it handled the call.
  bool dsi_font_hle = false;
  u8   dsi_font_digest[20] = {};
  bool dsi_hle_swi(CpuContext& cpu, u32 number);
  // Boot the DSi from its NAND (boot2 -> launcher) instead of staging a direct
  // boot from the card. Needs a NAND image; false if there is none.
  bool boot_dsi_nand();
  bool dsi_nand_boot = false;        // set before reset() to take that path
  std::string dsi_boot2_override;    // an SRL to run instead of the NAND's boot2 (e.g. Unlaunch)
  // The DSi's autoload hand-off (GBATEK "DSi Autoload", melonDS DS
  // SetUpDSiWareDirectBoot): a 0x100-byte "TLNC" block at 0x02000300 naming
  // the title, plus the BPTWL boot flag. Written after a NAND boot is set up,
  // it makes the launcher start that installed title instead of the menu.
  void dsi_autoload(u32 title_lo, u32 title_hi = 0x00030004);
  // The BPTWL soft reset (register 0x11 <- 1). The write halts the ARM7, and
  // the scheduler calls dsi_soft_reset() as that CPU's run returns -- melonDS
  // Halt(4), which resets at the end of ARM7::Execute.
  bool dsi_soft_reset_pending = false;
  void dsi_soft_reset();

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
  // run_frame() in pieces: runs up to `cycles` (ARM9 clock) of the current
  // frame and returns true when the frame completed (frame_count then
  // advanced). Local wireless needs the emulation spread across the frame's
  // wall-clock period: a peer's CMD is answered within a slice rather than
  // after the frame's sleep (docs/wifi-scoping.md, pacing).
  bool run_frame_slice(u64 cycles);

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
  // Identity of the two BIOS images in bus.bios9/bios7, set by load_bios.
  // A save state restores the ARM7 mid-BIOS-routine nearly every time -- at a
  // frame boundary it idles in the BIOS's IntrWait loop -- and the BIOS is
  // not part of the state, so resuming one against a different pair lands the
  // PC in unrelated code (FreeBIOS fills only 0x2030 of the ARM7 region; the
  // rest is zero). The state header carries this so the load is refused
  // instead.
  u64 bios_id = 0;

  mem::Bus   bus;
  Scheduler  sched;
  gpu::Gpu   gpu;
  gpu::Gpu3D gpu3d;
  spu::Spu   spu;
  io::Io     io;
  dma::Dma   dma;
  dma::Ndma  ndma;                 // DSi only; idle on a DS
  std::unique_ptr<cart::Cart> cart;

  // Action Replay codes, run from the ARM7's VBlank IRQ (CpuContext::check_irq)
  // when any are enabled. Not part of a save state: which cheats are on is the
  // frontend's business, and a state should load the same either way.
  cheat::Engine cheats;

  u64  frame_count = 0;
  bool frame_ready = false;
  bool frame_in_slices = false;   // run_frame_slice has begun a frame not yet complete
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
