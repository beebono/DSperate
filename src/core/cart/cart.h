// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ds { struct NDS; }

namespace ds::cart {

struct Header {
  char game_title[12]; char game_code[4]; char maker_code[2];
  u8 unit_code, encryption_seed, device_capacity; u8 reserved1[7]; u8 reserved2; u8 region; u8 rom_version; u8 autostart;
  u32 arm9_rom_offset, arm9_entry, arm9_ram_address, arm9_size;
  u32 arm7_rom_offset, arm7_entry, arm7_ram_address, arm7_size;
  u32 fnt_offset, fnt_size, fat_offset, fat_size;
  u32 arm9_overlay_offset, arm9_overlay_size, arm7_overlay_offset, arm7_overlay_size;
  u32 rom_control_normal, rom_control_key1; u32 icon_offset;
  u16 secure_area_crc16, secure_area_delay;
  u32 arm9_auto_load, arm7_auto_load; u8 secure_area_disable[8];
  u32 rom_size, header_size; u8 reserved3[56]; u8 nintendo_logo[156];
  u16 logo_crc16, header_crc16;
  u32 game_code_u32() const { u32 v; __builtin_memcpy(&v, game_code, 4); return v; }
};
static_assert(sizeof(Header) == 0x160, "NDS header layout");

enum class SaveType : u8 { None, EepromTiny, Eeprom, Flash };

// Retail Slot-1 cartridge: ROM reads through the KEY1/KEY2 command protocol,
// and the save chip on the AUXSPI bus.
class Cart {
public:
  Cart(NDS& nds, std::vector<u8> rom);
  void reset();

  const Header& header() const { return header_; }
  const u8* rom() const { return rom_.data(); }
  u32 rom_size() const { return static_cast<u32>(rom_.size()); }
  u32 chip_id() const { return chip_id_; }

  // Cart bus.
  void set_reset(bool reset);                   // /RES line (ROMCTRL bit 29)
  void command_start(const u8 cmd[8]);
  u32  command_receive();                       // next data word
  void setup_direct_boot();                     // cart already past KEY1 negotiation

  // Save chip (AUXSPI).
  void spi_select() { spi_pos_ = 0; }
  void spi_release();
  u8   spi_transfer(u8 v);
  std::vector<u8>& sram() { return sram_; }
  bool sram_dirty() const { return sram_dirty_; }
  void clear_sram_dirty() { sram_dirty_ = false; }

  // Secure area for direct boot: 0x800 bytes from arm9_rom_offset, decrypted.
  void decrypt_secure_area(u8 out[0x800]);

  // KEY1 (Blowfish) with the key table from the ARM7 BIOS.
  void key1_init(u32 idcode, u32 level, u32 mod);
  void key1_encrypt(u32* data) const;
  void key1_decrypt(u32* data) const;

private:
  NDS& nds_;
  std::vector<u8> rom_;
  Header header_{};
  u32 chip_id_ = 0;
  u32 rom_mask_ = 0;

  bool in_reset_ = true;
  u32 cmd_mode_ = 0;        // 0 plain, 1 KEY1, 2 KEY2
  u32 data_mode_ = 0;
  u8  rom_cmd_[8]{};
  u32 rom_addr_ = 0;
  u32 rom_read32();

  std::array<u32, 0x412> key1_{};
  void key1_apply_keycode(u32* keycode, u32 mod);

  SaveType save_type_ = SaveType::None;
  std::vector<u8> sram_;
  bool sram_dirty_ = false;
  u32 spi_pos_ = 0; u8 spi_cmd_ = 0; u32 spi_addr_ = 0; u8 spi_status_ = 0;
  u8 spi_eeprom_tiny(u8 v); u8 spi_eeprom(u8 v); u8 spi_flash(u8 v);
};

// Save type per game code (a small list; default is 64 KB EEPROM).
SaveType save_type_for(u32 game_code, u32& size);

} // namespace ds::cart
