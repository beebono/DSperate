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
#include "core/bios/freebios.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace ds {

namespace {

std::vector<u8> slurp_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
    DsiAes::derive_normal_key(kx, ky, tmp);
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

bool NDS::load_dsi_nand(const std::string& path, std::string* err) {
  if (!dsi_nand.open(path)) {
    if (err) *err = path + ": not a DSi NAND image (needs the nocash footer holding the eMMC CID and console ID)";
    return false;
  }
  return true;
}

// Boot the DSi the way the console does, from the NAND, instead of staging a
// direct boot from the card image. With a *half* BIOS dump -- which is all we
// have, and all melonDS needs -- the boot ROM's own boot2 loader is not
// present, so melonDS does its job by hand and so do we (DSi.cpp LoadNAND, the
// !FullBIOSBoot branch): read boot2's location from the NAND's boot info,
// apply the NWRAM mapping it wants, decrypt boot2 into place, seed the bits of
// state the missing BIOS code would have left, and enter boot2 directly.
//
// This is the path melonDS DS (libretro) relies on for DSiWare: boot2 brings up
// the launcher, which reads the TLNC autoload block and launches the installed
// title itself. Nothing here is reverse-engineered -- the console does it.
bool NDS::boot_dsi_nand() {
  if (!dsi_nand.valid()) return false;
  io::DsiIo& d = io.dsi;

  // NWRAM has to be reachable before the mapping below means anything; reset
  // leaves these bits at their startup values, which need not include it.
  d.scfg_ext[0] |= 1u << 25;
  d.scfg_ext[1] |= 1u << 25;
  for (int i = 0; i < 3; ++i) std::memset(bus.nwram[i].get(), 0, mem::Bus::NWRAM_BANK_SIZE);

  // The boot info block: where boot2 lives and where it goes. Raw NAND bytes --
  // this area is outside the AES-CTR'd filesystem.
  u32 bp[8], mbk[12];
  dsi_nand.read(0x220, sizeof bp, reinterpret_cast<u8*>(bp));
  dsi_nand.read(0x380, sizeof mbk, reinterpret_cast<u8*>(mbk));

  // The NWRAM mapping boot2 expects, in our MBK register layout: slots 0-4 are
  // shared, 5-7 are each CPU's own windows, 8 is the write protect.
  for (int c = 0; c < 2; ++c) {
    for (int i = 0; i < 5; ++i) d.mbk[c][i] = mbk[i];
    for (int i = 0; i < 3; ++i) d.mbk[c][5 + i] = mbk[(c == 0 ? 5 : 8) + i];
    d.mbk[c][8] = mbk[11] & 0x00FFFF0F;
  }
  bus.update_nwram();

  // boot2 itself: AES-CTR with a fixed key, the IV derived from the aligned
  // size, over byte-reversed 16-byte blocks (the DSi's AES engine works on
  // big-endian blocks, so every block is swapped in and back out).
  auto load_boot2 = [&](u32 offset, u32 size_aligned, u32 dst, Cpu cpu) {
    static const u8 key[16] = {0xAD, 0x34, 0xEC, 0xF9, 0x62, 0x6E, 0xC2, 0x3A,
                               0xF6, 0xB4, 0x6C, 0x00, 0x80, 0x80, 0xEE, 0x98};
    u8 tmp[16], iv[16];
    const u32 sz = size_aligned;
    std::memcpy(&tmp[0], &sz, 4);
    const u32 neg = ~sz + 1, inv = ~sz, zero = 0;
    std::memcpy(&tmp[4], &neg, 4);
    std::memcpy(&tmp[8], &inv, 4);
    std::memcpy(&tmp[12], &zero, 4);
    bswap128(iv, tmp);

    AES_ctx ctx;
    AES_init_ctx_iv(&ctx, key, iv);
    for (u32 i = 0; i < size_aligned; i += 16) {
      u8 blk[16];
      dsi_nand.read(offset + i, 16, blk);
      bswap128(tmp, blk);
      AES_CTR_xcrypt_buffer(&ctx, tmp, 16);
      bswap128(blk, tmp);
      for (u32 k = 0; k < 16; k += 4) {
        u32 v;
        std::memcpy(&v, &blk[k], 4);
        bus.dma_write32(cpu, dst, v);
        dst += 4;
      }
    }
  };
  if (dsi_boot2_override.empty()) {
    load_boot2(bp[0], bp[3], bp[2], Cpu::ARM9);
    load_boot2(bp[4], bp[7], bp[6], Cpu::ARM7);
  } else {
    // A boot2 replacement (Unlaunch and friends) ships as a plain DSi
    // multiboot SRL: unencrypted, no modcrypt, its own ARM9/ARM7 load
    // addresses in the header. Load those in place of the NAND's boot2 and
    // enter them the same way -- the entry state below is boot2's, which is
    // exactly what a replacement is written against.
    std::vector<u8> img = slurp_file(dsi_boot2_override);
    if (img.size() < 0x200) { std::fprintf(stderr, "dsi: %s is not an SRL\n", dsi_boot2_override.c_str()); return false; }
    auto hdr32 = [&](u32 o) { u32 v; std::memcpy(&v, &img[o], 4); return v; };
    const u32 r9 = hdr32(0x20), e9 = hdr32(0x24), a9 = hdr32(0x28), s9 = hdr32(0x2C);
    const u32 r7 = hdr32(0x30), e7 = hdr32(0x34), a7 = hdr32(0x38), s7 = hdr32(0x3C);
    if (static_cast<u64>(r9) + s9 > img.size() || static_cast<u64>(r7) + s7 > img.size()) {
      std::fprintf(stderr, "dsi: %s: ARM9/ARM7 sections run past the file\n", dsi_boot2_override.c_str());
      return false;
    }
    for (u32 i = 0; i + 3 < s9; i += 4) { u32 v; std::memcpy(&v, &img[r9 + i], 4); bus.dma_write32(Cpu::ARM9, a9 + i, v); }
    for (u32 i = 0; i + 3 < s7; i += 4) { u32 v; std::memcpy(&v, &img[r7 + i], 4); bus.dma_write32(Cpu::ARM7, a7 + i, v); }
    bp[2] = e9; bp[6] = e7;
    bp[3] = s9; bp[7] = s7;
    std::fprintf(stderr, "dsi: boot2 replaced by %s\n", dsi_boot2_override.c_str());
  }

  // What the boot ROM code we do not have would have left behind: the eMMC CID
  // and a handful of constants the ARM7 side reads back, plus the BIOS routines
  // boot2 calls but which live in the missing halves -- copied into ITCM and
  // ARM7 WRAM at the addresses melonDS uses.
  const u8* cid = dsi_nand.emmc_cid();
  auto w7 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM7, a, v); };
  auto w7h = [&](u32 a, u16 v) { bus.dma_write16(Cpu::ARM7, a, v); };
  const u32 e = 0x03FFE6E4;
  for (u32 i = 0; i < 16; i += 4) { u32 v; std::memcpy(&v, cid + i, 4); w7(e + i, v); }
  w7h(e + 0x2C, 0x0001); w7h(e + 0x2E, 0x0001);
  w7h(e + 0x3C, 0x0100); w7h(e + 0x3E, 0x40E0); w7h(e + 0x42, 0x0001);

  const u8* b9 = bus.bios9i.get();
  u8* itcm = bus.itcm.get();
  std::memcpy(itcm + 0x4400, b9 + 0x87F4, 0x400);
  std::memcpy(itcm + 0x4800, b9 + 0x9920, 0x80);
  std::memcpy(itcm + 0x4894, b9 + 0x99A0, 0x1048);
  std::memcpy(itcm + 0x58DC, b9 + 0xA9E8, 0x1048);

  const u8* b7 = bus.bios7i.get();
  std::vector<u8> init(0x3C00, 0);
  std::memcpy(&init[0x0000], b7 + 0x8188, 0x200);
  std::memcpy(&init[0x0200], b7 + 0xB5D8, 0x40);
  std::memcpy(&init[0x0254], b7 + 0xC6D0, 0x1048);
  std::memcpy(&init[0x129C], b7 + 0xD718, 0x1048);
  for (u32 i = 0; i < init.size(); i += 4) { u32 v; std::memcpy(&v, &init[i], 4); w7(0x03FFC400 + i, v); }

  cpu(Cpu::ARM9).jump(bp[2], false);
  cpu(Cpu::ARM7).jump(bp[6], false);
  std::fprintf(stderr, "dsi: boot2 from NAND -- ARM9 %08X (%u bytes), ARM7 %08X (%u bytes)\n",
               bp[2], bp[3], bp[6], bp[7]);
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
  // Boot indicator: 1 = from the card, 3 = handed over by a DSi launcher.
  // With a NAND attached we stand in for the launcher (below), so the title
  // mounts nand:/ and reads its own .app and save from there; without one it
  // runs in card mode, which is what melonDS's direct boot always does.
  // UNPROVEN, so off by default (DS_DSI_HANDOFF=1 to try it): the pieces are
  // all written as the recipe describes, but Shantae stalls just after its
  // SD controller init instead of mounting, where card mode at least reaches
  // its "Save Data has been corrupted" prompt. Default stays card mode, which
  // is what melonDS's direct boot does and what the trace gate compares.
  // DS_DSI_HANDOFF picks which launcher hand-off to stage on top of our direct
  // boot. Neither is a complete launch; see the phase 3 status block.
  //   1 = the mount table in ARM7 WRAM (reaches the NAND -- app directory walk
  //       and save I/O -- then wedges the ARM7 in a dead-end IRQ handler)
  //   2 = the TLNC autoload block (what melonDS DS actually uses, but it only
  //       means something to the real boot ROM, which our direct boot skips)
  const char* handoff_env = getenv("DS_DSI_HANDOFF");
  const int handoff = (dsi_nand.valid() && handoff_env) ? std::atoi(handoff_env) : 0;
  const bool launcher_handoff = handoff == 1 && t.param_block_address >= 0x03800000 &&
                                t.param_block_address < 0x0380FC00;
  const bool tlnc_handoff = handoff == 2;
  w16(0x02FFFC40, 0x0001);
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

  // ---- hand-off 1: the launcher's mount table in ARM7 WRAM ----
  // Reverse-engineered from launcher launches of KS3E and KMGE
  // (dsperate-research tools/melonds/dsiware_params.py). This one does reach
  // the NAND: the title mounts nand:/, walks to its own CONTENT/ and DATA/,
  // and reads and writes PUBLIC.SAV -- then wedges (see the doc).
  if (launcher_handoff) {
    d.scfg_ext[1] |= 1u << 18;

    auto w32_7 = [&](u32 a, u32 v) { bus.dma_write32(Cpu::ARM7, a, v); };
    auto w8_7  = [&](u32 a, u8 v)  { bus.dma_write8(Cpu::ARM7, a, v); };
    const u32 tbl = t.param_block_address;
    for (u32 z = 0; z < 0x500; z += 4) w32_7(tbl + z, 0);
    auto put_str = [&](u32 addr, const char* str) { for (const char* c = str; *c; ++c) w8_7(addr++, static_cast<u8>(*c)); w8_7(addr, 0); };
    char title[64];
    std::snprintf(title, sizeof title, "nand:/title/%08x/%08x", t.title_id_hi, t.title_id_lo);
    char pub[96];
    std::snprintf(pub, sizeof pub, "%s/data/public.sav", title);
    struct { u32 hdr; const char* name; const char* path; } entries[] = {
      {0x00008141, "nand",    "/"},
      {0x0000A142, "nand2",   "/"},
      {0x00041144, "shared1", "nand:/shared1"},
      {0x00063146, "photo",   "nand2:/photo"},
      {0x00060948, "dataPub", pub},
    };
    u32 o = tbl;
    for (const auto& e : entries) { w32_7(o, e.hdr); put_str(o + 4, e.name); put_str(o + 20, e.path); o += 0x54; }
    char app[96];
    std::snprintf(app, sizeof app, "%s/content/%08x.app", title, 0);
    put_str(tbl + 0x3C0, app);
    w32_7(0x0380FFC4, 0x13FFFF06);
    w8_7(0x0380FFC8, 0x44);
    w8_7(0x0380FFC9, 0xF8);
    w16(0x02FFFC40, 0x0003);
  }

  // ---- hand-off 2: the TLNC autoload block ----
  // What the launcher actually leaves behind for a DSiWare title is a 0x100-byte
  // "TLNC" autoload block at 0x02000300 in main RAM, plus the BPTWL boot flag.
  // GBATEK "DSi Autoload"; the working reference is melonDS DS (libretro),
  // console/dsi.cpp SetUpDSiWareDirectBoot, which direct-boots DSiWare with
  // just these two things.
  //
  //   +0x00 "TLNC"   +0x04 unknown (01h)   +0x05 length (18h, from PrevTitleID)
  //   +0x06 CRC16 over Length bytes from +0x08, seed 0xFFFF
  //   +0x08 PrevTitleID (0 = anonymous)    +0x10 NewTitleID (this title)
  //   +0x18 flags: bit 0 valid, bits 1-3 boot type (3 = DSiWare), bit 4 unknown
  //                but required -- titles error out without it
  //   +0x1C unused (still checksummed)     +0x20 unused, zero filled
  if (tlnc_handoff) {
    d.scfg_ext[1] |= 1u << 18;                       // the launcher's SCFG_EXT7: NAND access for the ARM7

    u8 tlnc[0x100] = {};
    tlnc[0] = 'T'; tlnc[1] = 'L'; tlnc[2] = 'N'; tlnc[3] = 'C';
    tlnc[4] = 0x01;
    tlnc[5] = 0x18;
    // PrevTitleID stays zero ("anonymous" -- nothing launched us).
    std::memcpy(&tlnc[0x10], &t.title_id_lo, 4);
    std::memcpy(&tlnc[0x14], &t.title_id_hi, 4);
    const u32 flags = 0x01u | (0x03u << 1) | (1u << 4);
    std::memcpy(&tlnc[0x18], &flags, 4);
    const u16 crc = bios::crc16(&tlnc[0x08], 0x18, 0xFFFF);
    std::memcpy(&tlnc[0x06], &crc, 2);
    for (u32 i = 0; i < sizeof tlnc; ++i) w8(0x02000300 + i, tlnc[i]);

    // The BPTWL boot flag (register 0x70) the launcher sets on its way out.
    io.dsi.bptwl_regs[0x70] = 1;
  }
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
