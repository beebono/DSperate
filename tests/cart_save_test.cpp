// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The save chip of a game the save list does not know: a ROM hack that changed
// its game code, or homebrew. The chip follows an existing save file's size,
// or is detected from the game's first access; a guess must never destroy what
// the game stores. A listed game keeps its fixed chip and exact FLASH rules.
#include "core/nds.h"
#include "core/cart/cart.h"
#include "core/state/state.h"
#include "check.h"

#include <cstring>
#include <memory>
#include <initializer_list>
#include <vector>

using namespace ds;
using cart::Cart;
using cart::SaveType;

namespace {

Cart make_cart(NDS& nds, const char code[4]) {
  std::vector<u8> rom(1 << 20, 0);
  std::memcpy(rom.data() + 0x0C, code, 4);    // arm9_rom_offset stays 0: no secure area to re-encrypt
  return Cart(nds, std::move(rom));
}

// One chip-select period: every byte clocked, then the release.
std::vector<u8> tx(Cart& c, std::initializer_list<u8> bytes) {
  std::vector<u8> out;
  for (u8 b : bytes) out.push_back(c.spi_transfer(b));
  c.spi_release();
  return out;
}

void test_size_classes() {
  CHECK(cart::save_type_for_size(512) == SaveType::EepromTiny);
  CHECK(cart::save_type_for_size(8192) == SaveType::Eeprom);
  CHECK(cart::save_type_for_size(65536) == SaveType::Eeprom);
  CHECK(cart::save_type_for_size(131072) == SaveType::Eeprom);
  CHECK(cart::save_type_for_size(262144) == SaveType::Flash);
  CHECK(cart::save_type_for_size(524288) == SaveType::Flash);
  CHECK(cart::save_type_for_size(8388608) == SaveType::Flash);
  CHECK(cart::save_type_for_size(0) == SaveType::None);
  CHECK(cart::save_type_for_size(1000) == SaveType::None);
  CHECK(cart::save_type_for_size(300000) == SaveType::None);
}

// A one-byte first read is command + address + 1 (DeSmuME's rule).
void test_detect_from_first_read() {
  NDS nds;
  CHECK(!cart::known_game_code(0x5A5A5A5A));
  {
    Cart c = make_cart(nds, "ZZZZ");
    CHECK(!c.save_type_listed() && c.save_type() == SaveType::Detect && c.sram().empty());
    tx(c, {0x05, 0x00});                            // a status poll says nothing
    CHECK(c.save_type() == SaveType::Detect);
    CHECK(tx(c, {0x03, 0x00, 0x00, 0x00})[3] == 0xFF);   // blank chip
    CHECK(c.save_type() == SaveType::Eeprom && c.sram().size() == 65536);
    tx(c, {0x06});
    tx(c, {0x02, 0x01, 0x23, 0xAB});
    CHECK(c.sram()[0x123] == 0xAB);
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x03, 0x00, 0x00});
    CHECK(c.save_type() == SaveType::EepromTiny && c.sram().size() == 512);
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x03, 0x00, 0x00, 0x00, 0x00});
    CHECK(c.save_type() == SaveType::Flash && c.sram().size() == 524288);
  }
  {
    // A long first read cannot be split, and the chip is blank anyway: wait.
    Cart c = make_cart(nds, "ZZZZ");
    CHECK(tx(c, {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})[7] == 0xFF);
    CHECK(c.save_type() == SaveType::Detect);
    tx(c, {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});   // undecided either
    tx(c, {0x06});                                  // must not re-read the transaction before it
    CHECK(c.save_type() == SaveType::Detect);
    tx(c, {0xD8, 0x00, 0x00, 0x00});                // an erase: FLASH
    CHECK(c.save_type() == SaveType::Flash);
  }
}

// A write that decides the chip is replayed into it.
void test_detecting_write_is_kept() {
  NDS nds;
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x06});
    tx(c, {0x02, 0x40, 0x5A});
    CHECK(c.save_type() == SaveType::EepromTiny && c.sram()[0x40] == 0x5A && c.sram_dirty());
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x06});
    tx(c, {0x02, 0x01, 0x02, 0x03, 0x77});
    CHECK(c.save_type() == SaveType::Flash && c.sram()[0x10203] == 0x77);
  }
  {
    // Too long to split: the commonest chip, and the bytes land under it.
    Cart c = make_cart(nds, "ZZZZ");
    tx(c, {0x06});
    tx(c, {0x02, 0x00, 0x10, 1, 2, 3, 4});
    CHECK(c.save_type() == SaveType::Eeprom);
    CHECK(c.sram()[0x10] == 1 && c.sram()[0x13] == 4);
  }
}

// An unlisted FLASH overwrites (an EEPROM game on it never erases) and grows
// to fit; a listed one ANDs as the chip does.
void test_lenient_flash() {
  NDS nds;
  Cart c = make_cart(nds, "ZZZZ");
  tx(c, {0x03, 0x00, 0x00, 0x00, 0x00});
  tx(c, {0x06});
  tx(c, {0x02, 0x00, 0x00, 0x10, 0x0F});
  tx(c, {0x02, 0x00, 0x00, 0x10, 0xAA});          // the latch dropped with the first program: nothing written
  CHECK(c.sram()[0x10] == 0x0F);
  tx(c, {0x06});
  tx(c, {0x02, 0x00, 0x00, 0x10, 0xF0});
  CHECK(c.sram()[0x10] == 0xF0);
  tx(c, {0x06});
  tx(c, {0x02, 0x0F, 0x00, 0x00, 0x42});
  CHECK(c.sram().size() == 0x100000 && c.sram()[0x0F0000] == 0x42 && c.sram()[0x10] == 0xF0);

  const char listed[4] = {'A', 'D', 'A', 'D'};
  u32 code; std::memcpy(&code, listed, 4);
  u32 size = 0;
  CHECK(cart::known_game_code(code) && cart::save_type_for(code, size) == SaveType::Flash);
  Cart l = make_cart(nds, listed);
  CHECK(l.save_type_listed());
  tx(l, {0x06});
  tx(l, {0x02, 0x00, 0x00, 0x10, 0x0F});
  tx(l, {0x06});
  tx(l, {0x02, 0x00, 0x00, 0x10, 0xF0});
  CHECK(l.sram()[0x10] == 0x00);
}

void test_load_save() {
  NDS nds;
  {
    Cart c = make_cart(nds, "ZZZZ");
    std::vector<u8> file(524288, 0x11);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(r.fitted && r.chip_bytes == 524288 && c.save_type() == SaveType::Flash && c.sram()[5] == 0x11);
  }
  {
    // DeSmuME .dsv: the image plus a 122-byte footer ending in the cookie.
    Cart c = make_cart(nds, "ZZZZ");
    std::vector<u8> file(65536 + 122, 0x22);
    std::memcpy(file.data() + file.size() - 16, "|-DESMUME SAVE-|", 16);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(r.fitted && c.save_type() == SaveType::Eeprom && c.sram().size() == 65536);
  }
  {
    Cart c = make_cart(nds, "ZZZZ");
    std::vector<u8> file(1000, 0x33);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(!r.fitted && r.chip_bytes == 0 && c.save_type() == SaveType::Detect);
  }
  {
    // A listed chip does not move: the part that fits, and the mismatch said.
    const char listed[4] = {'A', 'D', 'A', 'D'};
    Cart c = make_cart(nds, listed);
    const u32 chip = static_cast<u32>(c.sram().size());
    std::vector<u8> file(65536, 0x44);
    const Cart::SaveLoad r = c.load_save(file.data(), file.size());
    CHECK(!r.fitted && r.chip_bytes == chip && c.sram()[0] == 0x44 && c.sram()[65536] == 0xFF);
  }
}

// A state names an unlisted chip by its size, either way round.
void test_state_carries_the_chip() {
  NDS nds;
  Cart a = make_cart(nds, "ZZZZ");
  tx(a, {0x03, 0x00, 0x00, 0x00, 0x00});
  tx(a, {0x06});
  tx(a, {0x02, 0x00, 0x00, 0x08, 0x99});
  state::Writer w;
  a.sync_state(w);

  Cart b = make_cart(nds, "ZZZZ");
  state::Reader r(w.data().data(), w.data().size());
  b.sync_state(r);
  CHECK(r.ok() && b.save_type() == SaveType::Flash && b.sram().size() == 524288 && b.sram()[8] == 0x99);

  Cart fresh = make_cart(nds, "ZZZZ");
  state::Writer w0;
  fresh.sync_state(w0);
  state::Reader r0(w0.data().data(), w0.data().size());
  a.sync_state(r0);
  CHECK(r0.ok() && a.save_type() == SaveType::Detect && a.sram().empty());
}

} // namespace

// A header is the file's word. A "decrypted" secure area that starts late in
// a page is re-encrypted across the page boundary, not past the page; a
// binary size no card could carry is held to the card.
void test_crafted_header() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  std::vector<u8> rom(1 << 20, 0);
  std::memcpy(rom.data() + 0x0C, "ZZZZ", 4);
  const u32 a9 = 0x4F00, marker = 0xE7FFDEFF, huge = 0xFFFFFFF0;
  std::memcpy(rom.data() + 0x20, &a9, 4);          // arm9_rom_offset
  std::memcpy(rom.data() + 0x2C, &huge, 4);        // arm9_size
  std::memcpy(rom.data() + a9, &marker, 4);
  const std::vector<u8> plain = rom;
  Cart c(nds, std::move(rom));
  u8 got[0x1000];
  for (u32 i = 0; i < sizeof got; i += 4) { const u32 w = c.rom_read32_at(a9 - 0x100 + i); std::memcpy(got + i, &w, 4); }
  CHECK(!std::memcmp(got, plain.data() + a9 - 0x100, 0x100));                       // before the area: untouched
  CHECK(std::memcmp(got + 0x100, plain.data() + a9, 0x800) != 0);                   // the area: encrypted, both pages
  CHECK(std::memcmp(got + 0x100 + 0x700, plain.data() + a9 + 0x700, 0x100) != 0);   // its tail in the second page too
  CHECK(!std::memcmp(got + 0x900, plain.data() + a9 + 0x800, 0x700));               // after it: untouched
  CHECK(c.boot_size(huge) == (1u << 20) && c.boot_size(0x1234) == 0x1234);
}

// A NAND cart: the save is part of the ROM chip, behind card commands.
namespace {
std::unique_ptr<Cart> make_nand(NDS& nds) {
  std::vector<u8> rom(32u << 20, 0);
  std::memcpy(rom.data() + 0x0C, "UORE", 4);       // WarioWare D.I.Y.: listed with the 16 MB size
  const u16 start = 0x40;                          // the save at 8 MB (in 128 KB units)
  std::memcpy(rom.data() + 0x96, &start, 2);
  for (u32 i = 0x100000; i < 0x100100; ++i) rom[i] = static_cast<u8>(i);
  auto c = std::make_unique<Cart>(nds, std::move(rom));
  c->setup_direct_boot();
  return c;
}
void cmd(Cart& c, u8 op, u32 addr = 0) {
  const u8 b[8] = {op, static_cast<u8>(addr >> 24), static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 8), static_cast<u8>(addr), 0, 0, 0};
  c.command_start(b);
}
u32 status(Cart& c) { cmd(c, 0xD6); return c.command_receive() & 0xFF; }
void write_page(Cart& c, u32 addr, u8 fill) {
  cmd(c, 0x85);
  for (int rep = 0; rep < 4; ++rep) {              // as the games do: four commands, 0x200 bytes each
    cmd(c, 0x81, addr);
    for (int i = 0; i < 0x80; ++i) c.command_transmit(0x01010101u * static_cast<u8>(fill + rep));
  }
  cmd(c, 0x82);
}
} // namespace

void test_nand() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  const u32 base = 0x800000, size = 16u << 20;
  {
    auto cp = make_nand(nds); Cart& c = *cp;
    CHECK(c.save_type() == SaveType::Nand && c.sram().size() == size && (c.chip_id() & 0x08000000));
    CHECK(c.spi_transfer(0x05) == 0xFF);                         // nothing on the SPI bus
    CHECK(status(c) == 0x20);
    // ROM mode: the ROM reads, the save region does not.
    cmd(c, 0xB7, 0x100000); CHECK(c.command_receive() == 0x03020100);
    cmd(c, 0xB7, base);     CHECK(c.command_receive() == 0xFFFFFFFF);
    // No window: write enable does nothing, and neither does a write.
    cmd(c, 0x85); CHECK(status(c) == 0x20);
    // The window, a page, the commit.
    cmd(c, 0xB2, base);
    write_page(c, base + 0x3000, 0x40);
    CHECK(c.sram_dirty() && c.sram()[0x3000] == 0x40 && c.sram()[0x31FF] == 0x40 && c.sram()[0x3200] == 0x41 && c.sram()[0x37FF] == 0x43);
    CHECK(c.sram()[0x2FFF] == 0xFF && c.sram()[0x3800] == 0xFF);
    CHECK(status(c) == 0x20);                                    // the commit drops write enable
    // Read back through the window; outside it, nothing.
    cmd(c, 0xB7, base + 0x3200); CHECK(c.command_receive() == 0x41414141);
    cmd(c, 0xB7, base + 0x20000); CHECK(c.command_receive() == 0xFFFFFFFF);
    // Without write enable a page is not taken.
    cmd(c, 0x81, base + 0x4000); c.command_transmit(0x12345678); cmd(c, 0x82);
    CHECK(c.sram()[0x4000] == 0xFF && c.sram_writes() == 1);
    // A discarded page is not committed.
    cmd(c, 0x85); cmd(c, 0x81, base + 0x4000); c.command_transmit(0x12345678); cmd(c, 0x84); cmd(c, 0x82);
    CHECK(c.sram()[0x4000] == 0xFF && c.sram_writes() == 1);
    // The last 128 KB is the cart's: an ID, and read-only.
    CHECK(c.sram()[size - 0x800] == 0xEC && c.sram()[size - 0x7FF] == 0x00 && c.sram()[size - 0x20000] == 0xFF);
    cmd(c, 0xB2, base + size - 0x20000);
    write_page(c, base + size - 0x800, 0x77);
    CHECK(c.sram()[size - 0x800] == 0xEC && c.sram_writes() == 1);
    cmd(c, 0x94);
    CHECK((c.command_receive() & 0xFFFF) == 0xF1EC);
    // Back to the ROM.
    cmd(c, 0x8B);
    cmd(c, 0xB7, 0x100000); CHECK(c.command_receive() == 0x03020100);

    // A state carries the window and a half-written page; the save file keeps the cart's ID.
    cmd(c, 0xB2, base);
    cmd(c, 0x85); cmd(c, 0x81, base + 0x5000); c.command_transmit(0xCAFEF00D);
    state::Writer w; c.sync_state(w);
    auto dp = make_nand(nds); Cart& d = *dp;
    state::Reader r(w.data().data(), w.data().size()); d.sync_state(r);
    CHECK(r.ok() && d.sram()[0x3000] == 0x40);
    for (int i = 1; i < 0x200; ++i) d.command_transmit(0); 
    cmd(d, 0x82);
    CHECK(d.sram()[0x5000] == 0x0D && d.sram()[0x5003] == 0xCA && d.sram()[0x5004] == 0x00);
    std::vector<u8> file(size, 0x00);
    const Cart::SaveLoad l = d.load_save(file.data(), file.size());
    CHECK(l.fitted && d.sram()[0] == 0x00 && d.sram()[size - 0x800] == 0xEC && d.sram()[size - 0x20000] == 0xFF);
  }
  {
    // The 8 MB size alone does not make a NAND cart: the header must place the save.
    const char listed[4] = {'A', '3', '6', 'E'};
    u32 code; std::memcpy(&code, listed, 4); u32 sz = 0;
    CHECK(cart::save_type_for(code, sz) == SaveType::Flash && sz == 8388608);
    std::vector<u8> rom(1 << 20, 0);
    std::memcpy(rom.data() + 0x0C, listed, 4);
    Cart c(nds, std::move(rom));
    CHECK(c.save_type() == SaveType::Flash && !(c.chip_id() & 0x08000000));
  }
  {
    // Through the I/O registers: a write-direction transfer asks for each word and ends after the last.
    nds.cart = make_nand(nds);
    Cart& c = *nds.cart;
    auto command = [&](std::initializer_list<u8> bytes, u32 romctrl) {
      u32 a = 0x040001A8;
      for (u8 b : bytes) nds.io.write(Cpu::ARM9, a++, 8, b);
      for (; a < 0x040001B0; ++a) nds.io.write(Cpu::ARM9, a, 8, 0);
      nds.io.write(Cpu::ARM9, 0x040001A4, 32, romctrl);
    };
    nds.io.write(Cpu::ARM9, 0x040001A0, 16, 0x8000);
    nds.io.write(Cpu::ARM9, 0x040001A4, 32, 1u << 29);          // out of reset
    c.setup_direct_boot();
    cmd(c, 0xB2, base); cmd(c, 0x85);
    command({0x81, 0x00, 0x80, 0x60, 0x00}, 0xE1000000u);        // start, write, 0x200 bytes
    CHECK(nds.io.read(Cpu::ARM9, 0x040001A4, 32) & 0x00800000);  // DRQ: the first word is wanted
    for (int i = 0; i < 0x80; ++i) {
      CHECK(nds.io.read(Cpu::ARM9, 0x040001A4, 32) & 0x80000000);
      if (i & 1) nds.io.write(Cpu::ARM9, 0x04100010, 32, 0xA5A5A5A5);
      else { nds.io.write(Cpu::ARM9, 0x04100010, 16, 0xA5A5); nds.io.write(Cpu::ARM9, 0x04100012, 16, 0xA5A5); }
    }
    CHECK(!(nds.io.read(Cpu::ARM9, 0x040001A4, 32) & 0x80800000));   // done, and nothing more asked for
    cmd(c, 0x82);
    CHECK(c.sram()[0x6000] == 0xA5 && c.sram()[0x61FF] == 0xA5 && c.sram_dirty());
    nds.cart.reset();
  }
}

// A DSi-enhanced card: its DSi area is the DSi's to read, and only past the
// first 0x3000 bytes; to a DS, or woken as a DS card, it is not there.
void test_dsi_card() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  const u32 base = 0x100000;   // the DSi area at 1 MB (two 512 KB units)
  auto make = [&](bool cut_dump) {
    std::vector<u8> rom(2u << 20, 0);
    std::memcpy(rom.data() + 0x0C, "VAAE", 4);                 // a listed retail code (Art Academy)
    rom[0x12] = 2;                                             // runs on both
    const u16 start = 2; std::memcpy(rom.data() + 0x92, &start, 2);
    const u32 region = cut_dump ? 0 : 2; std::memcpy(rom.data() + 0x1B0, &region, 4);
    for (u32 a : {0x8000u, 0x80000u, base, base + 0x2FFCu, base + 0x3000u}) std::memcpy(rom.data() + a, &a, 4);
    return std::make_unique<Cart>(nds, std::move(rom));
  };
  auto read = [](Cart& c, u32 addr) { cmd(c, 0xB7, addr); return c.command_receive(); };
  {
    // In a DS: a DS card, whatever is asked.
    auto c = make(false);
    CHECK(c->dsi_capable() && (c->chip_id() & 0x40000000));
    c->set_reset(false);
    cmd(*c, 0x3D);                                             // a DS has no such wake-up
    CHECK(!c->dsi_mode());
    c->setup_direct_boot();
    CHECK(!c->dsi_mode());
    CHECK(read(*c, 0x80000) == 0x80000);
    CHECK(read(*c, base + 0x3000) == 0x8000);                  // the DSi area: the DS area's first readable page instead
    CHECK(read(*c, base) == 0x8000);
  }
  nds.set_dsi(true);
  {
    auto c = make(false);
    c->setup_direct_boot();                                    // a DSi's direct boot leaves the card in its DSi mode
    CHECK(c->dsi_mode());
    CHECK(read(*c, 0x80000) == 0x80000);
    CHECK(read(*c, base + 0x3000) == base + 0x3000);           // readable now
    CHECK(read(*c, base) == 0x8000 && read(*c, base + 0x2FFC) != base + 0x2FFC);   // but not its key area
    state::Writer w; c->sync_state(w);
    auto d = make(false);
    d->set_reset(false);
    CHECK(!d->dsi_mode());
    state::Reader r(w.data().data(), w.data().size()); d->sync_state(r);
    CHECK(r.ok() && d->dsi_mode() && read(*d, base + 0x3000) == base + 0x3000);
    d->set_reset(true); d->set_reset(false);                   // a reset is a DS card again until woken
    CHECK(!d->dsi_mode());
    cmd(*d, 0x3D);
    CHECK(d->dsi_mode());
    d->set_reset(true); d->set_reset(false);
    cmd(*d, 0x3C);
    CHECK(!d->dsi_mode());
  }
  {
    // A dump with the DSi part cut off (no region flags) is a plain DS card: nothing is blocked or woken.
    auto c = make(true);
    CHECK(!(c->chip_id() & 0x40000000));
    c->setup_direct_boot();
    CHECK(!c->dsi_mode() && read(*c, base + 0x3000) == base + 0x3000 && read(*c, base) == base);
  }
  nds.set_dsi(false);
}

int main() {
  test_crafted_header();
  test_nand();
  test_dsi_card();
  test_size_classes();
  test_detect_from_first_read();
  test_detecting_write_is_kept();
  test_lenient_flash();
  test_load_save();
  test_state_carries_the_chip();
  std::puts("cart_save: ok");
  return 0;
}
