// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The DSi machine's boot: the DSi BIOS pair, the launcher's main-RAM
// leftovers, the NWRAM mapping from the header, the ARM9i/ARM7i binaries
// and their modcrypt, and the CP15/SCFG state a DSiWare title starts from.
// Mirrors melonDS DSi::SetupDirectBoot (DSi-mode branch) so the melonDS
// trace harness is the oracle; see docs/dsiware-scoping.md.
#include "core/nds.h"
#include "core/cpu/cp15.h"

extern "C" {
#include "core/crypto/aes.h"
}

#include <cstdio>
#include <cstring>
#include <fstream>

namespace ds {

namespace {

std::vector<u8> slurp_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The DSi's AES key scrambler (melonDS DSi_AES::DeriveNormalKey): the
// normal key is ((X ^ Y) + C) rotated left by 42 bits, all 128-bit
// little-endian arithmetic.
void rol16(u8* v, u32 n) {
  const u32 coarse = n >> 3, fine = n & 7;
  u8 t[16];
  for (u32 i = 0; i < 16; ++i) t[i] = v[(i - coarse) & 0xF];
  for (u32 i = 0; i < 16; ++i) v[i] = static_cast<u8>((t[i] << fine) | (t[(i - 1) & 0xF] >> (8 - fine)));
}

void derive_normal_key(const u8* kx, const u8* ky, u8* out) {
  static const u8 key_const[16] = {0xFF, 0xFE, 0xFB, 0x4E, 0x29, 0x59, 0x02, 0x58, 0x2A, 0x68, 0x0F, 0x5F, 0x1A, 0x4F, 0x3E, 0x79};
  u8 t[16];
  for (int i = 0; i < 16; ++i) t[i] = kx[i] ^ ky[i];
  u32 carry = 0;
  for (int i = 0; i < 16; ++i) { const u32 r = t[i] + key_const[15 - i] + carry; t[i] = static_cast<u8>(r); carry = r >> 8; }
  rol16(t, 42);
  std::memcpy(out, t, 16);
}

void bswap128(u8* dst, const u8* src) { for (int i = 0; i < 16; ++i) dst[i] = src[15 - i]; }

// melonDS DSi::DecryptModcryptArea: AES-CTR over the binary the area covers,
// in place in main RAM (the area is described in ROM offsets, and lands in
// RAM where the matching binary was loaded). The DSi's AES engine works on
// byte-reversed 128-bit blocks, hence the swaps.
void decrypt_modcrypt_area(NDS& nds, u32 offset, u32 size, const u8* iv) {
  if (!offset || !size) return;
  const cart::Header& h = nds.cart->header();
  const cart::TwlHeader& t = nds.cart->twl();
  u8 key[16], tmp[16];
  if ((h.reserved2 & (1u << 4)) || (t.app_flags & (1u << 7))) {
    nds.cart->rom_read(0, tmp, 16);                      // dev key: the first 16 header bytes
  } else {
    u8 kx[16], ky[16];
    std::memcpy(kx, "Nintendo", 8);
    kx[8] = static_cast<u8>(h.game_code[0]); kx[9] = static_cast<u8>(h.game_code[1]); kx[10] = static_cast<u8>(h.game_code[2]); kx[11] = static_cast<u8>(h.game_code[3]);
    kx[12] = static_cast<u8>(h.game_code[3]); kx[13] = static_cast<u8>(h.game_code[2]); kx[14] = static_cast<u8>(h.game_code[1]); kx[15] = static_cast<u8>(h.game_code[0]);
    std::memcpy(ky, t.arm9i_hash, 16);
    derive_normal_key(kx, ky, tmp);
  }
  bswap128(key, tmp);
  u8 ivr[16]; bswap128(ivr, iv);
  AES_ctx ctx;
  AES_init_ctx_iv(&ctx, key, ivr);

  const u32 rounded = (size + 0xF) & ~0xFu;
  auto covers = [&](u32 rom_off, u32 bin_size) { return offset >= rom_off && offset + rounded <= rom_off + ((bin_size + 0xF) & ~0xFu); };
  u32 addr;
  if (covers(h.arm9_rom_offset, h.arm9_size)) addr = h.arm9_ram_address;
  else if (covers(h.arm7_rom_offset, h.arm7_size)) addr = h.arm7_ram_address;
  else if (covers(t.arm9i_rom_offset, t.arm9i_size)) addr = t.arm9i_ram_address;
  else if (covers(t.arm7i_rom_offset, t.arm7i_size)) addr = t.arm7i_ram_address;
  else return;
  // melonDS decrypts from where the binary starts, not from the area's
  // offset within it (the areas always start at the binary in practice).
  for (u32 i = 0; i < size; i += 16) {
    u32 data[4];
    for (int k = 0; k < 4; ++k) data[k] = nds.bus.dma_read32(Cpu::ARM9, addr + i + k * 4);
    bswap128(tmp, reinterpret_cast<const u8*>(data));
    AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
    bswap128(reinterpret_cast<u8*>(data), tmp);
    for (int k = 0; k < 4; ++k) nds.bus.dma_write32(Cpu::ARM9, addr + i + k * 4, data[k]);
  }
}

} // namespace

void NDS::set_dsi(bool on) { dsi = on; }

bool NDS::load_dsi_bios(const std::string& p9i, const std::string& p7i, std::string* err) {
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  auto exists = [](const std::string& p) { return !p.empty() && std::ifstream(p).good(); };
  const bool have9 = exists(p9i), have7 = exists(p7i);
  bios_native_dsi = false;
  if (!have9 && !have7) return true;   // no DSi: fine for a DS game
  if (have9 != have7) return fail(std::string(have9 ? p7i : p9i) + ": the other DSi BIOS half is present; both or neither");
  std::vector<u8> b9 = slurp_file(p9i), b7 = slurp_file(p7i);
  if (b9.size() != mem::Bus::BIOS9I_SIZE) return fail(p9i + ": not a 64 KB DSi ARM9 BIOS");
  if (b7.size() != mem::Bus::BIOS7I_SIZE) return fail(p7i + ": not a 64 KB DSi ARM7 BIOS");
  std::memcpy(bus.bios9i.get(), b9.data(), b9.size());
  std::memcpy(bus.bios7i.get(), b7.data(), b7.size());
  bios_native_dsi = true;
  // Fold the DSi pair into the BIOS identity a save state checks (nds.cpp).
  for (u32 i = 0; i < mem::Bus::BIOS9I_SIZE; ++i) bios_id = (bios_id ^ bus.bios9i.get()[i]) * 1099511628211ull;
  for (u32 i = 0; i < mem::Bus::BIOS7I_SIZE; ++i) bios_id = (bios_id ^ bus.bios7i.get()[i]) * 1099511628211ull;
  return true;
}

bool NDS::load_dsi_boot_blobs(const std::string& path, std::string* err) {
  std::vector<u8> b = slurp_file(path);
  if (b.size() != 0x128 + 0x14 + 0x18) { if (err) *err = path + ": not a 0x154-byte DSi boot blob (tools/dsi_nand.py bootblobs)"; return false; }
  dsi_boot_blobs = std::move(b);
  return true;
}

void NDS::setup_direct_boot_dsi() {
  const cart::Header& h = cart->header();
  const cart::TwlHeader& t = cart->twl();
  auto w32 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM9, a, v); };
  auto w16 = [&](u32 a, u16 v) { bus.dma_write16(Cpu::ARM9, a, v); };
  auto w8  = [&](u32 a, u8 v)  { bus.dma_write8(Cpu::ARM9, a, v); };
  auto rd32 = [&](const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; };
  auto rom32 = [&](u32 off) { return cart->rom_read32_at(off); };
  io::DsiIo& d = io.dsi;

  // ---- SCFG_BIOS and the NWRAM mapping from the header's MBK block ----
  d.scfg_bios = 0x0101;
  d.mbk[0][8] = d.mbk[1][8] = 0;                     // protection off while the header's map goes in
  for (int i = 0; i < 4; ++i) io.mbk_map_slot(0, i, static_cast<u8>(t.mbk[0] >> (i * 8)));
  for (int i = 0; i < 8; ++i) io.mbk_map_slot(1, i, static_cast<u8>(t.mbk[1 + (i >> 2)] >> ((i & 3) * 8)));
  for (int i = 0; i < 8; ++i) io.mbk_map_slot(2, i, static_cast<u8>(t.mbk[3 + (i >> 2)] >> ((i & 3) * 8)));
  for (int i = 0; i < 3; ++i) { io.mbk_map_range(Cpu::ARM9, i, t.mbk[5 + i]); io.mbk_map_range(Cpu::ARM7, i, t.mbk[8 + i]); }
  d.mbk[0][8] = d.mbk[1][8] = t.mbk[11] & 0x00FFFF0F;
  io.wramcnt = static_cast<u8>((t.mbk[11] >> 24) & 3);
  bus.update_nwram();
  if (!(t.app_flags & 1)) io.spi_tsc.dsi_mode = 0;    // DS-compatibility touchscreen unless the title asks for the CODEC

  // ---- main RAM: header copies, launcher leftovers ----
  for (u32 i = 0; i < 0x160; i += 4) { const u32 v = rom32(i); w32(0x02FFFA80 + i, v); w32(0x02FFFE00 + i, v); }
  for (u32 i = 0; i < 0x1000; i += 4) { const u32 v = rom32(i); w32(0x02FFC000 + i, v); w32(0x02FFE000 + i, v); }
  if (dsi_boot_blobs.size() == 0x154) {
    const u8* b = dsi_boot_blobs.data();
    for (u32 i = 0; i < 0x128; i += 4) w32(0x02000400 + i, rd32(b + i));            // TWLCFG user data, bytes 0x88..
    for (u32 i = 0; i < 0x14; i += 4) w32(0x02000600 + i, rd32(b + 0x128 + i));    // HWINFO_N
    for (u32 i = 0; i < 0x18; i += 4) w32(0x02FFFD68 + i, rd32(b + 0x13C + i));    // HWINFO_S
  }
  // Wi-Fi board words, from the firmware header's board byte (0x1FD), as
  // melonDS writes them (it notes they should come from the NAND's wifi
  // firmware).
  const u8 board = firmware.size() > 0x1FD ? firmware[0x1FD] : 0;
  w8(0x020005E0, board);
  if (board == 0x01) { w16(0x020005E2, 0xB57E); w32(0x020005E4, 0x00500400); w32(0x020005E8, 0x00500000); w32(0x020005EC, 0x0002E000); }
  else               { w16(0x020005E2, 0x5BCA); w32(0x020005E4, 0x00520000); w32(0x020005E8, 0x00520000); w32(0x020005EC, 0x00020000); }
  w32(0x02FFFC00, cart->chip_id());
  w16(0x02FFFC40, 0x0001);                           // boot indicator: card (the launcher's 3 is a later phase)
  w8(0x02FFFDFA, 0x80);                              // BPTWL boot flag (0) | 0x80
  w8(0x02FFFDFB, 0x01);

  // ---- the binaries ----
  u32 arm9_start = 0;
  if (h.arm9_rom_offset >= 0x4000 && h.arm9_rom_offset < 0x8000) {
    u8 secure[0x800];
    cart->decrypt_secure_area(secure);
    for (u32 i = 0; i < 0x800; i += 4) w32(h.arm9_ram_address + i, rd32(secure + i));
    arm9_start = 0x800;
  }
  for (u32 i = arm9_start; i < h.arm9_size; i += 4) w32(h.arm9_ram_address + i, rom32(h.arm9_rom_offset + i));
  for (u32 i = 0; i < h.arm7_size; i += 4) bus.dma_write32(Cpu::ARM7, h.arm7_ram_address + i, rom32(h.arm7_rom_offset + i));
  if (h.reserved2 & 1) {                             // DSi crypto flags bit 0: the ARM9i/ARM7i binaries exist
    for (u32 i = 0; i < t.arm9i_size; i += 4) w32(t.arm9i_ram_address + i, rom32(t.arm9i_rom_offset + i));
    for (u32 i = 0; i < t.arm7i_size; i += 4) bus.dma_write32(Cpu::ARM7, t.arm7i_ram_address + i, rom32(t.arm7i_rom_offset + i));
    if (h.reserved2 & 2) {                           // bit 1: modcrypted
      decrypt_modcrypt_area(*this, t.modcrypt1_offset, t.modcrypt1_size, t.arm9_hash);
      decrypt_modcrypt_area(*this, t.modcrypt2_offset, t.modcrypt2_size, t.arm7_hash);
    }
  }
  io.arm7_bios_prot = 0x20;

  // ---- what the firmware leaves for a DSi (melonDS FirmwareMem::SetupDirectBoot) ----
  if (firmware.size() >= 0x20000) {   // the DSi firmware image is 128 KB
    // melonDS writes the three MAC halves to one address (its loop has no
    // stride); reproduced so the oracle traces agree. The value a title
    // sees there is the MAC's last two bytes.
    for (u32 i = 0; i < 6; i += 2) w16(0x02FFFCF4, static_cast<u16>(firmware[0x36 + i] | (firmware[0x37 + i] << 8)));
    w16(0x02FFFCFA, static_cast<u16>(firmware[0x3C] | (firmware[0x3D] << 8)));   // enabled channels
    const u32 u0 = static_cast<u32>(firmware[0x20] | (firmware[0x21] << 8)) << 3, u1 = u0 + 0x100;
    auto counter = [&](u32 off) { return (off + 0x72 <= firmware.size()) ? (firmware[off + 0x70] | (firmware[off + 0x71] << 8)) & 0x7F : -1; };
    const int c0 = counter(u0), c1 = counter(u1);
    const u32 sel = (c1 > c0 && ((c1 - c0) & 0x7F) == 1) ? u1 : u0;
    for (u32 i = 0; i < 0x70; i += 4) w32(0x02FFFC80 + i, rd32(firmware.data() + sel + i));
  }

  d.scfg_mc = 0x0018;
  d.cart_insert_delay = 0x1988; d.cart_poweroff_delay = 0x264C;
  spu.write_sndexcnt(0x8008, 0xFFFF);
  d.scfg_ext[0] = 0x8307F100; d.scfg_ext[1] = 0x93FBFB06;

  // CP15, in melonDS's order (control first, so the DTCM window is placed
  // twice; the end state is what matters).
  auto cp = [&](u32 crn, u32 crm, u32 opc2, u32 v) { cp15_write(*arm9, 0, crn, crm, opc2, v); };
  cp(1, 0, 0, 0x00056078);
  cp(2, 0, 0, 0x0000004A); cp(2, 0, 1, 0x0000004A); cp(3, 0, 0, 0x0000000A);
  cp(5, 0, 2, 0x15111011); cp(5, 0, 3, 0x05101011);
  cp(6, 0, 0, 0x04000033); cp(6, 1, 0, 0x02000031); cp(6, 2, 0, 0x00000000); cp(6, 3, 0, 0x08000033);
  cp(6, 4, 0, 0x0E00001B); cp(6, 5, 0, 0x00000000); cp(6, 6, 0, 0xFFFF001D); cp(6, 7, 0, 0x02FFC01B);
  cp(9, 1, 0, 0x0E00000A); cp(9, 1, 1, 0x00000020);
  bus.update_vram_timings();

  // Register state as for a DS direct boot (melonDS NDS::SetupDirectBoot).
  arm9->set_cpsr(0xD3); arm7->set_cpsr(0xD3);
  arm9->hot.regs[12] = h.arm9_entry; arm9->hot.regs[13] = 0x03002F7C; arm9->hot.regs[14] = h.arm9_entry;
  arm9->bank_r13[0] = 0x03003FC0; arm9->bank_r13[2] = 0x03003F80;
  arm7->hot.regs[12] = h.arm7_entry; arm7->hot.regs[13] = 0x0380FD80; arm7->hot.regs[14] = h.arm7_entry;
  arm7->bank_r13[0] = 0x0380FFC0; arm7->bank_r13[2] = 0x0380FF80;
  arm9->jump(h.arm9_entry, true);
  arm7->jump(h.arm7_entry, true);
  arm9->hot.cycle_budget = 0; arm7->hot.cycle_budget = 0;
  // melonDS's pending pipeline-fill cycles at the first instruction (its
  // reset and direct-boot JumpTo costs, measured with trace_melonds
  // TRACE_CYC=1): the ARM9 starts 34 bus cycles late and the ARM7 15.
  arm9->boot_stall = 136; arm7->boot_stall = 15;

  io.exmemcnt = 0xE880; bus.update_gba_slot_timings();
  io.cpu_io[0].postflg = 1; io.cpu_io[1].postflg = 1;
  io.powcnt1 = 0x820F; gpu.set_powcnt(0x820F);
  io.powcnt2 = 0x0001; spu.set_powcnt2(0x0001);
  spu.write(0x04000504, 16, 0x200);
  io.cart.romctrl |= 1u << 29;
  io.cart.auxspicnt = 0x8000;   // melonDS NDS::SetupDirectBoot: WriteSPICnt(0x8000) on both slots (the DS path leaves it to the game)
  io.rcnt = 0x8000;             // melonDS NDS::SetupDirectBoot ("checkme"); the DS path leaves 0
  cart->setup_direct_boot();
}

} // namespace ds
