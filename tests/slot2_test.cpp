// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Slot 2: the GBA cartridge's ROM bus, save chips and solar sensor, the
// Rumble Pak, the bus's EXMEMCNT ownership and open bus, save states, and the
// dual-slot game table.
#include "core/nds.h"
#include "core/slot2/compat.h"
#include "core/slot2/slot2.h"
#include "core/state/state.h"
#include "core/input/input_log.h"
#include "check.h"

#include <cstring>
#include <string>
#include <vector>

using namespace ds;
using slot2::GameCart;
using slot2::SaveType;

namespace {

std::vector<u8> make_rom(const char code[4], size_t size = 0x1000, const char* lib = nullptr) {
  std::vector<u8> rom(size, 0);
  std::memcpy(&rom[0xA0], "TESTGAME", 8);
  std::memcpy(&rom[0xAC], code, 4);
  rom[0xB2] = 0x96;
  for (size_t i = 0x100; i < size; ++i) rom[i] = static_cast<u8>(i);
  if (lib) std::memcpy(&rom[0x800], lib, std::strlen(lib));
  return rom;
}

// The standard Flash unlock, then `cmd` at 0x5555.
void flash_cmd(GameCart& c, u8 cmd) {
  c.sram_write8(0x5555, 0xAA);
  c.sram_write8(0x2AAA, 0x55);
  c.sram_write8(0x5555, cmd);
}

void test_save_types() {
  CHECK(slot2::save_type_for_size(32768) == SaveType::Sram32K);
  CHECK(slot2::save_type_for_size(65536) == SaveType::Flash64K);
  CHECK(slot2::save_type_for_size(131072) == SaveType::Flash128K);
  CHECK(slot2::save_type_for_size(131072 + 16) == SaveType::Flash128K);
  CHECK(slot2::save_type_for_size(8192) == SaveType::Eeprom);
  CHECK(slot2::save_type_for_size(1000) == SaveType::None);
  // No save file: the save library's ID string in the ROM says.
  CHECK(GameCart(make_rom("BPEE", 0x1000, "FLASH1M_V103"), {}).save_type() == SaveType::Flash128K);
  CHECK(GameCart(make_rom("AXVE", 0x1000, "FLASH_V126"), {}).save_type() == SaveType::Flash64K);
  CHECK(GameCart(make_rom("A2CE", 0x1000, "SRAM_V113"), {}).save_type() == SaveType::Sram32K);
  CHECK(GameCart(make_rom("AWRE", 0x1000, "EEPROM_V124"), {}).save_type() == SaveType::Eeprom);
  CHECK(GameCart(make_rom("ZZZZ"), {}).save_type() == SaveType::None);
  // A save file's size beats the ROM's string.
  CHECK(GameCart(make_rom("BPEE", 0x1000, "FLASH1M_V103"), std::vector<u8>(32768, 0)).save_type() == SaveType::Sram32K);
}

void test_rom_bus() {
  GameCart c(make_rom("AWRE", 0x1800), {});
  CHECK(std::strcmp(c.game_code(), "AWRE") == 0 && std::strcmp(c.title(), "TESTGAME") == 0);
  CHECK(c.rom_read16(0x080000AC) == ('A' | 'W' << 8));
  CHECK(c.rom_read16(0x08000102) == (0x02 | 0x03 << 8));
  CHECK(c.rom_read16(0x0A000102) == (0x02 | 0x03 << 8));   // the bus decodes 25 bits
  CHECK(c.rom_read16(0x09000102) == 0);                    // a small ROM doesn't mirror (melonDS)
  // Padded to a power of two with zeros, then nothing past it.
  CHECK(c.rom_read16(0x08001800) == 0 && c.rom_read16(0x08002000) == 0);
  // GPIO registers read back only with control bit 0 set.
  c.rom_write16(0x080000C6, 0x0005);
  CHECK(c.rom_read16(0x080000C6) == 0);   // the ROM's own (blank) header bytes
  c.rom_write16(0x080000C8, 1);
  CHECK(c.rom_read16(0x080000C6) == 0x0005 && c.rom_read16(0x080000C8) == 1);
}

void test_sram() {
  GameCart c(make_rom("A2CE", 0x1000, "SRAM_V113"), {});
  CHECK(c.save().size() == 32768 && !c.save_dirty());
  c.sram_write8(0x0A000010, 0x5A);
  CHECK(c.sram_read8(0x0A000010) == 0x5A && c.save()[0x10] == 0x5A && c.save_dirty());
  CHECK(c.sram_read8(0x0A008000) == 0xFF);   // past 32 KB
  c.clear_save_dirty();
  c.sram_write8(0x0A000010, 0x5A);           // the same byte again: nothing changed
  CHECK(!c.save_dirty());
}

void test_flash128() {
  std::vector<u8> save(131072, 0xFF);
  save[0x00010] = 0x11;
  save[0x10010] = 0x22;
  GameCart c(make_rom("BPEE"), save);
  CHECK(c.save_type() == SaveType::Flash128K);
  CHECK(c.sram_read8(0x10) == 0x11);
  // Chip ID, then out of ID mode.
  flash_cmd(c, 0x90);
  CHECK(c.sram_read8(0) == 0x62 && c.sram_read8(1) == 0x13);
  flash_cmd(c, 0xF0);
  CHECK(c.sram_read8(0x10) == 0x11);
  // Bank 1.
  flash_cmd(c, 0xB0);
  c.sram_write8(0x0000, 1);
  CHECK(c.sram_read8(0x10) == 0x22);
  // Byte program lands in the selected bank.
  flash_cmd(c, 0xA0);
  c.sram_write8(0x0020, 0x33);
  CHECK(c.sram_read8(0x20) == 0x33 && c.save()[0x10020] == 0x33 && c.save_dirty());
  // A write without the unlock does nothing.
  c.sram_write8(0x0021, 0x44);
  CHECK(c.save()[0x10021] == 0xFF);
  // Sector erase: 4 KB in the selected bank, nothing else.
  flash_cmd(c, 0x80);
  c.sram_write8(0x5555, 0xAA);
  c.sram_write8(0x2AAA, 0x55);
  c.sram_write8(0x0000, 0x30);
  CHECK(c.save()[0x10010] == 0xFF && c.save()[0x10020] == 0xFF && c.save()[0x00010] == 0x11);
  // Back to bank 0, chip erase.
  flash_cmd(c, 0xB0);
  c.sram_write8(0x0000, 0);
  CHECK(c.sram_read8(0x10) == 0x11);
  flash_cmd(c, 0x80);
  flash_cmd(c, 0x10);
  CHECK(c.save()[0x00010] == 0xFF);
}

void test_flash64_ignores_bank() {
  GameCart c(make_rom("AXVE"), std::vector<u8>(65536, 0xFF));
  flash_cmd(c, 0x90);
  CHECK(c.sram_read8(0) == 0x32 && c.sram_read8(1) == 0x1B);
  flash_cmd(c, 0xF0);
  flash_cmd(c, 0xB0);
  c.sram_write8(0x0000, 1);
  flash_cmd(c, 0xA0);
  c.sram_write8(0x0040, 0x77);
  CHECK(c.save()[0x40] == 0x77 && c.save().size() == 65536);
}

// The sensor's output bit goes high once the clocked count reaches the
// sample, and brighter light lowers the sample.
int clocks_until_bit(GameCart& c) {
  c.rom_write16(0x080000C8, 1);        // GPIO readable
  c.rom_write16(0x080000C6, 0x0007);   // bits 0-2 out, bit 3 in
  c.rom_write16(0x080000C4, 0x0002);   // reset
  c.rom_write16(0x080000C4, 0x0000);
  for (int n = 0; n < 256; ++n) {
    c.rom_write16(0x080000C4, 0x0001);
    c.rom_write16(0x080000C4, 0x0000);
    if (c.rom_read16(0x080000C4) & 8) return n;
  }
  return -1;
}

void test_solar_sensor() {
  GameCart dark(make_rom("U3IE"), {});
  CHECK(dark.solar_sensor());
  dark.set_light_level(0);
  GameCart sun(make_rom("U3IE"), {});
  sun.set_light_level(10);
  const int d = clocks_until_bit(dark), s = clocks_until_bit(sun);
  CHECK(d > 0 && s > 0 && s < d);
  CHECK(!GameCart(make_rom("AWRE"), {}).solar_sensor());
}

void test_rumble() {
  slot2::RumblePak p;
  CHECK(p.rom_read16(0x08000000) == 0xFFFD && p.rom_read16(0x09FFFFFE) == 0xFFFD);
  p.rom_write16(0x08000000, 2);
  p.rom_write16(0x08000000, 2);   // unchanged: no motor change
  p.rom_write16(0x08000000, 0);
  CHECK(p.rumble_events() == 2);
}

// Frets read back active low on the SRAM bus, fed from the input frame.
void test_guitar_grip() {
  using G = slot2::GuitarGrip;
  NDS nds;
  nds.reset();
  nds.io.exmemcnt = 0x0004;
  nds.slot2 = std::make_unique<G>();
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x080000AC) == 0xF9FF && nds.bus.dma_read16(Cpu::ARM9, 0x0801FFFE) == 0xF9FF);
  CHECK(nds.bus.dma_read8(Cpu::ARM9, 0x0A000000) == 0xFF);   // nothing held
  input::Frame f;
  f.frets = G::Green | G::Blue;
  input::apply(nds, f);
  CHECK(nds.bus.dma_read8(Cpu::ARM9, 0x0A000000) == static_cast<u8>(~(0x40 | 0x08)));
  f.frets = G::Red | G::Yellow;
  input::apply(nds, f);
  CHECK(nds.bus.dma_read8(Cpu::ARM9, 0x0A000000) == static_cast<u8>(~(0x20 | 0x10)));
  CHECK(slot2::uses_guitar_grip("YGHJ") && slot2::uses_guitar_grip("BGHP") && slot2::uses_guitar_grip("C6QX") && !slot2::uses_guitar_grip("CPUE"));
}

void test_bus() {
  NDS nds;
  nds.reset();
  // Empty slot: the address on the ROM bus, ORed with what decays at the
  // ROM's wait setting; 0xFF on the SRAM bus.
  nds.io.exmemcnt = 0x0004;   // ARM9 owns, ROM N=8: no decay
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x08001234) == 0x091A);
  CHECK(nds.bus.dma_read32(Cpu::ARM9, 0x08001234) == (0x091Au | 0x091Bu << 16));
  CHECK(nds.bus.dma_read8(Cpu::ARM9, 0x0A000000) == 0xFF);
  nds.io.exmemcnt = 0x000C;   // N=18: everything decays high
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x08001234) == 0xFFFF);
  nds.io.exmemcnt = 0x0000;   // N=10
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x08001234) == (0x091A | 0xFE08));
  // The CPU without the slot reads zeros.
  CHECK(nds.bus.dma_read16(Cpu::ARM7, 0x08001234) == 0);

  std::vector<u8> save(32768, 0);
  save[3] = 0xC3;
  nds.slot2 = std::make_unique<GameCart>(make_rom("A2CE", 0x1000), save);
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x080000AC) == ('A' | '2' << 8));
  CHECK(nds.bus.dma_read8(Cpu::ARM9, 0x080000AD) == '2');
  CHECK(nds.bus.dma_read8(Cpu::ARM9, 0x0A000003) == 0xC3);
  nds.bus.dma_write8(Cpu::ARM9, 0x0A000004, 0x4D);
  CHECK(nds.gba_cart()->save()[4] == 0x4D);
  // Handing the slot to the ARM7 (EXMEMCNT bit 7).
  nds.io.exmemcnt = 0x0080;
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x080000AC) == 0);
  CHECK(nds.bus.dma_read16(Cpu::ARM7, 0x080000AC) == ('A' | '2' << 8));
  nds.bus.dma_write8(Cpu::ARM9, 0x0A000005, 0x11);   // dropped
  CHECK(nds.gba_cart()->save()[5] == 0);

  nds.slot2 = std::make_unique<slot2::RumblePak>();
  nds.io.exmemcnt = 0x0004;
  nds.bus.dma_write16(Cpu::ARM9, 0x08000000, 1);
  CHECK(nds.slot2->rumble_events() == 1);
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x08000000) == 0xFFFD);
  // A DSi has no slot 2, whatever is in it.
  nds.dsi = true;
  nds.bus.dma_write16(Cpu::ARM9, 0x08000000, 0);
  CHECK(nds.slot2->rumble_events() == 1);
  CHECK(nds.bus.dma_read16(Cpu::ARM9, 0x08000000) == 0x0000);
}

void test_state() {
  std::vector<u8> save(131072, 0xFF);
  GameCart a(make_rom("BPEE"), save);
  flash_cmd(a, 0xA0);
  a.sram_write8(0x0100, 0x9C);
  flash_cmd(a, 0x90);   // left in ID mode
  state::Writer w;
  w.begin("SLT2");
  a.sync_state(w);
  w.end();
  GameCart b(make_rom("BPEE"), save);
  state::Reader r(w.data().data(), w.data().size());
  r.begin("SLT2");
  b.sync_state(r);
  r.end();
  CHECK(r.ok() && b.save()[0x100] == 0x9C && b.save_dirty());
  CHECK(b.sram_read8(0) == 0x62);   // still in ID mode
  CHECK(a.identity() == b.identity() && a.identity() != GameCart(make_rom("BPRE"), save).identity());
}

void test_linkage() {
  const slot2::Linkage* pt = slot2::linkage_for("CPUE");
  CHECK(pt && slot2::accepts(*pt, "BPEE") && slot2::accepts(*pt, "AXVJ") && !slot2::accepts(*pt, "A2CE"));
  const slot2::Linkage* bleach = slot2::linkage_for("ABLJ");
  CHECK(bleach && slot2::accepts(*bleach, "BLEJ"));
  CHECK(slot2::linkage_for("ASME") == nullptr);   // Super Mario 64 DS: no dual-slot features
}

} // namespace

int main() {
  test_save_types();
  test_rom_bus();
  test_sram();
  test_flash128();
  test_flash64_ignores_bank();
  test_solar_sensor();
  test_rumble();
  test_guitar_grip();
  test_bus();
  test_state();
  test_linkage();
  std::puts("slot2: ok");
  return 0;
}
