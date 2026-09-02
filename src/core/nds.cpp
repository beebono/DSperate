// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/nds.h"
#include "core/state/state.h"
#include "core/cpu/idle_loop.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "core/cpu/interp/interp.h"
#include "core/cpu/cp15.h"
#include "core/cart/zip.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ds {

static std::vector<u8> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const auto size = f.tellg();
  if (size <= 0) return {};
  std::vector<u8> v(static_cast<size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(v.data()), size);
  return v;
}

NDS::NDS()
    : run_arm9(&interp::run), run_arm7(&interp::run),
      arm9(new CpuContext), arm7(new CpuContext),
      bus(*this), sched(*this), gpu(*this), gpu3d(*this), spu(*this), io(*this), dma(*this) {
  arm9->reset(Cpu::ARM9, this);
  arm7->reset(Cpu::ARM7, this);
  arm9->hot.other_cpu = reinterpret_cast<u64>(arm7.get());
  arm7->hot.other_cpu = reinterpret_cast<u64>(arm9.get());
  reset();
}

NDS::~NDS() = default;

void NDS::reset() {
  sched.reset();
  io.reset();
  dma.reset();
  if (cart) cart->reset();
  arm9->reset(Cpu::ARM9, this);
  arm7->reset(Cpu::ARM7, this);
  // Hardware reset state the BIOS expects: ARM9 vectors high (CP15 V set),
  // ITCM enabled (32 KB at 0), DTCM off until the BIOS programs it.
  arm9->cp15_control = 0x00012078 | (1u << 13);
  arm9->cp15_itcm = 0x00000020;
  arm9->hot.regs[15] = arm9->exception_base() + 8;
  bus.reset();
  gpu3d.reset();
  gpu.reset();
  spu.reset();
  frame_count = 0;
  frame_ready = false;
  power_off = false;
}

// DS firmware CRC16 (GBATEK "Firmware Header"; the polynomial table and the
// bit order follow melonDS's SPI.cpp).
static u16 fw_crc16(const u8* data, u32 len, u16 start) {
  static const u16 poly[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
  u32 crc = start;
  for (u32 i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      const bool carry = crc & 1;
      crc >>= 1;
      if (carry) crc ^= static_cast<u32>(poly[j]) << (7 - j);
    }
  }
  return static_cast<u16>(crc);
}

// Normalise the touchscreen calibration in both user-settings blocks so that
// an ADC reading is exactly the screen pixel << 4, and fix their checksums.
// The frontend then reports plain pixel coordinates instead of inverting
// whatever calibration the dumped firmware's owner happened to save; melonDS
// does the same at reset, which keeps traces against it comparable.
void NDS::normalise_touch_calibration() {
  if (firmware.size() < 0x40000) return;
  const u32 base = static_cast<u32>(firmware[0x20] | (firmware[0x21] << 8)) << 3;
  for (u32 blk = 0; blk < 2; ++blk) {
    const u32 off = base + blk * 0x100;
    if (off + 0x74 > firmware.size()) continue;
    u8* u = firmware.data() + off;
    auto w16 = [](u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); };
    w16(u + 0x58, 0);        // ADC x1
    w16(u + 0x5A, 0);        // ADC y1
    u[0x5C] = 0; u[0x5D] = 0;                    // pixel x1, y1
    w16(u + 0x5E, 255 << 4); // ADC x2
    w16(u + 0x60, 191 << 4); // ADC y2
    u[0x62] = 255; u[0x63] = 191;                // pixel x2, y2
    w16(u + 0x72, fw_crc16(u, 0x70, 0xFFFF));    // user settings CRC16
  }
}

bool NDS::load_bios(const std::string& p9, const std::string& p7, const std::string& pfw) {
  auto b9 = slurp(p9), b7 = slurp(p7), fw = slurp(pfw);
  if (b9.size() != mem::Bus::BIOS9_SIZE || b7.size() != mem::Bus::BIOS7_SIZE || fw.empty()) return false;
  std::memcpy(bus.bios9.get(), b9.data(), b9.size());
  std::memcpy(bus.bios7.get(), b7.data(), b7.size());
  firmware = std::move(fw);
  firmware_id = 1469598103934665603ull;
  for (u8 b : firmware) firmware_id = (firmware_id ^ b) * 1099511628211ull;
  fw_page_dirty.assign((firmware.size() + FW_PAGE - 1) / FW_PAGE, 0);
  fw_dirty_pages = 0;
  normalise_touch_calibration();
  return true;
}

void NDS::firmware_written(u32 offset) {
  const u32 page = offset / FW_PAGE;
  if (page >= fw_page_dirty.size() || fw_page_dirty[page]) return;
  fw_page_dirty[page] = 1;
  fw_dirty_pages++;
}

// Sidecar format: "DSFWOVR1", the size of the firmware it was made from, the
// identity of that dump, the page size, the page count, then that many
// (u32 page index, page bytes) records.
namespace {
constexpr char kFwOvrMagic[8] = {'D', 'S', 'F', 'W', 'O', 'V', 'R', '1'};
} // namespace

bool NDS::load_firmware_override(const std::string& path, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open"; return false; }
  char magic[8]; u32 head[4];
  if (!f.read(magic, 8) || std::memcmp(magic, kFwOvrMagic, 8) != 0) { err = "not a firmware override"; return false; }
  if (!f.read(reinterpret_cast<char*>(head), sizeof head)) { err = "truncated header"; return false; }
  if (head[0] != firmware.size()) { err = "made from a firmware of a different size"; return false; }
  // A dump mismatch is not fatal: the settings pages are the same shape in
  // every retail firmware, and refusing to boot because someone re-dumped
  // their console would be worse than the warning the frontend prints.
  if (head[1] != static_cast<u32>(firmware_id)) err = "made from a different firmware dump";
  if (head[2] != FW_PAGE) { err = "unknown page size"; return false; }
  for (u32 i = 0; i < head[3]; ++i) {
    u32 page;
    if (!f.read(reinterpret_cast<char*>(&page), 4)) { err = "truncated"; return false; }
    if (page >= fw_page_dirty.size()) { err = "page out of range"; return false; }
    if (!f.read(reinterpret_cast<char*>(firmware.data() + page * FW_PAGE), FW_PAGE)) { err = "truncated"; return false; }
    firmware_written(page * FW_PAGE);
  }
  // The user settings pages carry the touchscreen calibration, and the
  // frontend reports plain pixel coordinates on the strength of that being
  // normalised. Someone who ran the calibration wizard inside the firmware
  // has a real calibration in their override, which would put every touch in
  // the wrong place; normalise again over the top. The console keeps the
  // calibration screen, it just cannot mis-aim the pen with it.
  normalise_touch_calibration();
  return true;
}

bool NDS::save_firmware_override(const std::string& path, std::string& err) {
  if (!fw_dirty_pages) return true;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot write"; return false; }
    const u32 head[4] = {static_cast<u32>(firmware.size()), static_cast<u32>(firmware_id), FW_PAGE, fw_dirty_pages};
    f.write(kFwOvrMagic, 8);
    f.write(reinterpret_cast<const char*>(head), sizeof head);
    for (u32 page = 0; page < fw_page_dirty.size(); ++page) {
      if (!fw_page_dirty[page]) continue;
      f.write(reinterpret_cast<const char*>(&page), 4);
      f.write(reinterpret_cast<const char*>(firmware.data() + page * FW_PAGE), FW_PAGE);
    }
    if (!f) { err = "write failed"; return false; }
  }
  // Rename over the old one, so an interrupted write cannot leave a truncated
  // override that would boot the console with half of someone's settings.
  if (std::rename(tmp.c_str(), path.c_str()) != 0) { err = "cannot replace"; std::remove(tmp.c_str()); return false; }
  return true;
}

namespace { u64 rom_identity(const std::vector<u8>& rom) {
  // The header plus the size: enough to reject the wrong ROM without
  // hashing 100 MB on every save.
  u64 h = 1469598103934665603ull;
  for (size_t i = 0; i < 0x160 && i < rom.size(); ++i) h = (h ^ rom[i]) * 1099511628211ull;
  return h ^ rom.size();
}
} // namespace

bool NDS::load_rom(const std::string& path) {
  std::vector<u8> image = slurp(path);
  // A zipped ROM is unpacked here and nothing downstream can tell: rom_id is
  // hashed from the decompressed bytes below, so save states, .sav files and
  // scene hashes are interchangeable between a zipped and a loose copy of the
  // same game. Sniffed by magic rather than by extension.
  if (cart::is_zip(image.data(), image.size())) {
    std::vector<u8> rom;
    std::string err, chosen;
    if (!cart::extract_nds(image.data(), image.size(), rom, err, &chosen)) {
      std::fprintf(stderr, "rom: %s: %s\n", path.c_str(), err.c_str());
      return false;
    }
    rom_zip_entry = std::move(chosen);
    image = std::move(rom);
  }
  if (image.size() < 0x1000) return false;
  rom_id = rom_identity(image);   // before the move; Cart pads to a power of two
  cart = std::make_unique<cart::Cart>(*this, std::move(image));
  return true;
}

// Mirrors what the firmware leaves behind when it launches a card (values per
// GBATEK "DS Firmware Boot" and melonDS's direct-boot setup).
void NDS::setup_direct_boot() {
  if (!cart) return;
  const cart::Header& h = cart->header();
  const u8* r = cart->rom();
  auto w32 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM9, a, v); };
  auto w16 = [&](u32 a, u16 v) { bus.dma_write16(Cpu::ARM9, a, v); };
  auto rd32 = [&](const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; };

  io.wramcnt = 3; bus.update_wram();
  for (u32 i = 0; i < 0x170; i += 4) w32(0x027FFE00 + i, rd32(r + i));
  const u32 id = cart->chip_id();
  w32(0x027FF800, id); w32(0x027FF804, id); w16(0x027FF808, h.header_crc16); w16(0x027FF80A, h.secure_area_crc16);
  w16(0x027FF850, 0x5835);
  w32(0x027FFC00, id); w32(0x027FFC04, id); w16(0x027FFC08, h.header_crc16); w16(0x027FFC0A, h.secure_area_crc16);
  w16(0x027FFC10, 0x5835); w16(0x027FFC30, 0xFFFF); w16(0x027FFC40, 0x0001);

  u32 arm9_start = 0;
  if (h.arm9_rom_offset >= 0x4000 && h.arm9_rom_offset < 0x8000) {
    u8 secure[0x800];
    cart->decrypt_secure_area(secure);
    for (u32 i = 0; i < 0x800; i += 4) w32(h.arm9_ram_address + i, rd32(secure + i));
    arm9_start = 0x800;
  }
  for (u32 i = arm9_start; i < h.arm9_size; i += 4) w32(h.arm9_ram_address + i, rd32(r + h.arm9_rom_offset + i));
  for (u32 i = 0; i < h.arm7_size; i += 4) bus.dma_write32(Cpu::ARM7, h.arm7_ram_address + i, rd32(r + h.arm7_rom_offset + i));

  // Firmware user settings, as the firmware copies them.
  if (firmware.size() >= 0x40000) {
    const u16 user_off = static_cast<u16>(firmware[0x20] | (firmware[0x21] << 8));
    w32(0x027FF864, 0);
    w32(0x027FF868, static_cast<u32>(user_off) << 3);
    w16(0x027FF874, static_cast<u16>(firmware[0x06] | (firmware[0x07] << 8)));
    w16(0x027FF876, static_cast<u16>(firmware[0x04] | (firmware[0x05] << 8)));
    // Two copies of the user settings; the one with the higher update counter wins.
    const u32 u0 = static_cast<u32>(user_off) << 3, u1 = u0 + 0x100;
    auto counter = [&](u32 off) { return (off + 0x72 <= firmware.size()) ? (firmware[off + 0x70] | (firmware[off + 0x71] << 8)) & 0x7F : -1; };
    const int c0 = counter(u0), c1 = counter(u1);
    const u32 sel = (c1 > c0 && ((c1 - c0) & 0x7F) == 1) ? u1 : u0;
    for (u32 i = 0; i < 0x70; i += 4) w32(0x027FFC80 + i, rd32(firmware.data() + sel + i));
  }

  // CP15 state the firmware leaves: PU on, caches on, TCM placed.
  auto cp = [&](u32 crn, u32 crm, u32 opc2, u32 v) { cp15_write(*arm9, 0, crn, crm, opc2, v); };
  cp(2, 0, 0, 0x00000042); cp(2, 0, 1, 0x00000042); cp(3, 0, 0, 0x00000002);
  cp(5, 0, 2, 0x15111011); cp(5, 0, 3, 0x05100011);
  cp(6, 0, 0, 0x04000033); cp(6, 1, 0, 0x0200002B); cp(6, 2, 0, 0x00000000); cp(6, 3, 0, 0x08000035);
  cp(6, 4, 0, 0x0300001B); cp(6, 5, 0, 0x00000000); cp(6, 6, 0, 0xFFFF001D); cp(6, 7, 0, 0x027FF017);
  cp(9, 1, 0, 0x0300000A); cp(9, 1, 1, 0x00000020);
  cp(1, 0, 0, 0x00052078);

  // Register state as melonDS establishes it (the CPUs stay in SVC mode with
  // IRQ/FIQ masked; the game's own init switches modes and sets IME).
  arm9->set_cpsr(0xD3); arm7->set_cpsr(0xD3);
  arm9->hot.regs[12] = h.arm9_entry; arm9->hot.regs[13] = 0x03002F7C; arm9->hot.regs[14] = h.arm9_entry;
  arm9->bank_r13[0] = 0x03003FC0; arm9->bank_r13[2] = 0x03003F80;   // user/sys, IRQ
  arm7->hot.regs[12] = h.arm7_entry; arm7->hot.regs[13] = 0x0380FD80; arm7->hot.regs[14] = h.arm7_entry;
  arm7->bank_r13[0] = 0x0380FFC0; arm7->bank_r13[2] = 0x0380FF80;
  arm9->jump(h.arm9_entry, true);
  arm7->jump(h.arm7_entry, true);
  arm9->hot.cycle_budget = 0; arm7->hot.cycle_budget = 0;

  io.exmemcnt = 0xE880; bus.update_gba_slot_timings();
  io.cpu_io[0].postflg = 1; io.cpu_io[1].postflg = 1;
  io.powcnt1 = 0x820F; gpu.set_powcnt(0x820F);
  io.powcnt2 = 0x0001; spu.set_powcnt2(0x0001);      // sound on, SOUNDBIAS centred, as the firmware leaves them
  spu.write(0x04000504, 16, 0x200);
  io.cart.romctrl |= 1u << 29;
  cart->setup_direct_boot();
  io.arm7_bios_prot = 0x1204;
}

void NDS::run_frame() {
  if (!gpu.frame_begun()) gpu.begin_frame();   // first frame after reset/direct boot starts at line 0 without a line-0 event
  frame_ready = false;
  sched.run_until_frame();   // one entry into the slice loop per frame, not per event
  ++frame_count;
}


// ---- save states --------------------------------------------------------------

namespace {
constexpr u32 THUMB_W = 128, THUMB_H = 96;
} // namespace

bool NDS::save_state(state::Writer& w, std::string& err) {
  if (!cart) { err = "no cartridge"; return false; }
  if (!sched.at_slice_boundary() || gpu.line() != 0 || !gpu.at_line_start()) { err = "not at a frame boundary"; return false; }
  // Quiesce: nothing here changes what the guest observes.
  gpu.quiesce();
  gpu3d.sync_raster();
  spu.catch_up();
  io.cart_catch_up();

  w.blob("DSST", 4);
  w.put(state::FORMAT_VERSION);
  w.begin("HEAD");
  w.put(cart ? cart->header().game_code_u32() : 0u);   // 0: a firmware boot, no card in the slot
  w.put(rom_id);
  w.put(frame_count);
#if DSPERATE_JIT
  w.put(u32{1});
#else
  w.put(u32{0});
#endif
  // A thumbnail of the top screen, RGB565.
  w.put(THUMB_W); w.put(THUMB_H);
  const u32* fb = gpu.framebuffer(0);
  for (u32 y = 0; y < THUMB_H; ++y)
    for (u32 x = 0; x < THUMB_W; ++x) {
      const u32 p = fb[(y * 2) * SCREEN_W + x * 2];
      w.put(static_cast<u16>(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F)));
    }
  w.end();

  sched.sync_state(w);
  bus.sync_state(w);
  io.sync_state(w);
  arm9->sync_state(w);
  arm7->sync_state(w);
  dma.sync_state(w);
  spu.sync_state(w);
  gpu3d.sync_state(w);
  gpu.sync_state(w);
  if (cart) cart->sync_state(w);   // a firmware boot has no card to snapshot
  return true;
}

bool NDS::load_state(state::Reader& r, std::string& err) {
  char magic[4]; r.blob_raw(magic, 4);
  u32 version = 0; r.blob_raw(&version, 4);
  if (std::memcmp(magic, "DSST", 4) != 0) { err = "not a DSperate save state"; return false; }
  if (version != state::FORMAT_VERSION) { err = "save state format " + std::to_string(version) + ", this build reads " + std::to_string(state::FORMAT_VERSION); return false; }
  if (!r.begin("HEAD")) { err = r.error(); return false; }
  u32 code = 0; u64 ident = 0, frames = 0; u32 jit_built = 0;
  r.fields(code, ident, frames, jit_built);
  r.end();
  // A state taken on a firmware boot records a zero game code and identity;
  // it only loads back into another firmware boot, and vice versa.
  const u32 want_code = cart ? cart->header().game_code_u32() : 0u;
  if (code != want_code) { err = cart ? "save state is for another game" : "save state is for a game, not the firmware"; return false; }
  if (ident != rom_id) { err = "save state is for another ROM image"; return false; }

  // From here the machine is being overwritten: a failure leaves it broken.
  gpu.prepare_load();
  gpu3d.sync_raster();
  sched.sync_state(r);
  bus.sync_state(r);
  io.sync_state(r);
  arm9->sync_state(r);
  arm7->sync_state(r);
  if (!r.ok()) { err = r.error(); return false; }
  bus.relink();                 // page tables, VRAM map and timing from the restored registers
  dma.sync_state(r);
  spu.sync_state(r);
  gpu3d.sync_state(r);
  gpu.sync_state(r);
  if (cart) cart->sync_state(r);
  if (!r.ok()) { err = r.error(); return false; }
  gpu.after_load();
#if DSPERATE_JIT
  if (jit::has_runtime()) jit::flush_all();   // every block was translated from the old memory
#endif
  cpu::invalidate_idle_loops();
  if (!sched.after_load()) { err = "save state has an event without a handler"; return false; }
  frame_count = frames;
  frame_ready = false;
  return true;
}

} // namespace ds
