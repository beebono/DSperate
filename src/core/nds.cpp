// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/nds.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/cp15.h"

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
}

bool NDS::load_bios(const std::string& p9, const std::string& p7, const std::string& pfw) {
  auto b9 = slurp(p9), b7 = slurp(p7), fw = slurp(pfw);
  if (b9.size() != mem::Bus::BIOS9_SIZE || b7.size() != mem::Bus::BIOS7_SIZE || fw.empty()) return false;
  std::memcpy(bus.bios9.get(), b9.data(), b9.size());
  std::memcpy(bus.bios7.get(), b7.data(), b7.size());
  firmware = std::move(fw);
  return true;
}

bool NDS::load_rom(const std::string& path) {
  rom = slurp(path);
  if (rom.size() < 0x1000) return false;
  cart = std::make_unique<cart::Cart>(*this, rom);
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
  while (!frame_ready) sched.run_until(sched.next_deadline());
  ++frame_count;
}

} // namespace ds
