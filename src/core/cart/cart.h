// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/cart/rom_source.h"

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

// DSi extension of the header, ROM bytes 0x180..0x377 (GBATEK "DSi cartridge
// header"); consulted only when unit_code has bit 1 (DSi-capable).
struct TwlHeader {
  u32 mbk[12];                                  // 0x180: MBK1-5 slot maps, MBK6-8 ARM9 windows, MBK6-8 ARM7 windows, MBK9 (with WRAMCNT in the top byte)
  u32 region_flags;                             // 0x1B0
  u32 access_control;                           // 0x1B4
  u32 scfg_ext7;                                // 0x1B8: the ARM7 SCFG_EXT setting the launcher applies
  u8  reserved0[3]; u8 app_flags;               // 0x1BF: bit 0 = DSi touchscreen mode, bit 7 = dev key
  u32 arm9i_rom_offset, reserved1, arm9i_ram_address, arm9i_size;          // 0x1C0
  u32 arm7i_rom_offset, param_block_address, arm7i_ram_address, arm7i_size;   // 0x1D0 (0x1D4: launcher parameter block in ARM7 WRAM)
  u32 digest_ntr_offset, digest_ntr_size, digest_twl_offset, digest_twl_size; // 0x1E0
  u32 digest_sector_ht_offset, digest_sector_ht_size, digest_block_ht_offset, digest_block_ht_size;   // 0x1F0
  u32 digest_sector_size, digest_block_sectors; // 0x200
  u32 banner_size, shared2_sizes;               // 0x208
  u32 total_rom_size; u32 reserved2[3];         // 0x210
  u32 modcrypt1_offset, modcrypt1_size, modcrypt2_offset, modcrypt2_size;  // 0x220
  u32 title_id_lo, title_id_hi;                 // 0x230
  u32 public_sav_size, private_sav_size;        // 0x238
  u8  reserved3[0xB0];                          // 0x240
  u8  parental[0x10];                           // 0x2F0
  u8  arm9_hash[20], arm7_hash[20], digest_master_hash[20], banner_hash[20], arm9i_hash[20], arm7i_hash[20];   // 0x300
  static constexpr u32 ROM_OFFSET = 0x180;
};
static_assert(sizeof(TwlHeader) == 0x378 - 0x180, "TWL header layout");

enum class SaveType : u8 { None, EepromTiny, Eeprom, Flash, Detect, Nand };  // Detect: chip still unknown; Nand: in the ROM chip, see below

// Retail Slot-1 cartridge: ROM reads through the KEY1/KEY2 command protocol,
// and the save chip on the AUXSPI bus.
class Cart {
public:
  Cart(NDS& nds, std::unique_ptr<RomSource> rom);
  Cart(NDS& nds, std::vector<u8> rom) : Cart(nds, RomSource::from_memory(std::move(rom))) {}
  void reset();
  template <class S> void sync_state(S& s);

  const Header& header() const { return header_; }
  const TwlHeader& twl() const { return twl_; }
  bool dsi_capable() const { return (header_.unit_code & 2) != 0; }
  bool dsi_mode() const { return dsi_mode_; }   // the card is in its DSi mode (see dsi_cart_)   // unit_code: 0 DS, 2 DS+DSi, 3 DSi only
  // Bounded reads of the image; past the end the bytes are 0xFF, as on a card.
  void rom_read(u32 addr, u8* dst, u32 n) const { rom_->read(addr, dst, n); }
  u32  rom_read32_at(u32 addr) const { return rom_->read32(addr); }
  u32 rom_size() const { return rom_->size(); }          // bytes in the image
  // A boot binary's size as the header claims it, held to what a card can
  // carry into memory: no more than the card, nor than the largest main RAM.
  u32 boot_size(u32 claimed) const { const u32 most = rom_mask_ + 1 < (16u << 20) ? rom_mask_ + 1 : (16u << 20); return claimed < most ? claimed : most; }
  u32 rom_padded_size() const { return rom_mask_ + 1; }  // what the card wraps at
  const RomSource& source() const { return *rom_; }
  u32 chip_id() const { return chip_id_; }

  // Cart bus.
  void set_reset(bool reset);                   // /RES line (ROMCTRL bit 29)
  void command_start(const u8 cmd[8]);
  u32  command_receive();                       // next data word
  void command_transmit(u32 word);              // a data word of a write-direction command (ROMCTRL bit 30)
  void setup_direct_boot();                     // cart already past KEY1 negotiation

  // Save chip (AUXSPI).
  void spi_select() { spi_pos_ = 0; }
  void spi_release();
  u8   spi_transfer(u8 v);
  std::vector<u8>& sram() { return sram_; }
  SaveType save_type() const { return save_type_; }
  // Whether save_list.inc names this game's chip. When it doesn't, the chip
  // follows the save file's size, or is detected from the first save access,
  // and is lenient where a wrong guess would destroy data (FLASH programs
  // overwrite rather than AND, and grow to fit an address past the end).
  bool save_type_listed() const { return listed_; }
  // `fitted` is false when the file isn't the chip's size: the part that fits
  // was loaded, and the next write replaces the file, so keep the original.
  struct SaveLoad { bool fitted; u32 chip_bytes; };
  SaveLoad load_save(const u8* data, size_t n);
  bool sram_dirty() const { return sram_dirty_; }
  u32  sram_writes() const { return sram_writes_; }   // bumped on every chip write
  void clear_sram_dirty() { sram_dirty_ = false; }
  // Set when a save state brought in save memory other than what was there:
  // the save file on disk is then not what the game continues from, and the
  // next write of it replaces something the game never saw.
  bool sram_from_state() const { return sram_from_state_; }
  void clear_sram_from_state() { sram_from_state_ = false; }
  void set_sram_flags(bool dirty, bool from_state) { sram_dirty_ = dirty; sram_from_state_ = from_state; }

  // A `B7` read at the cart's own arm9_rom_offset, which only happens when
  // the firmware's menu launches the card. Sticky until cleared; not in the
  // save state (describes what the frontend is waiting for, not the machine).
  bool launch_read() const { return launch_read_; }
  void clear_launch_read() { launch_read_ = false; }

  // Secure area for direct boot: 0x800 bytes from arm9_rom_offset, decrypted.
  void decrypt_secure_area(u8 out[0x800]);

  // KEY1 (Blowfish) with the key table from the ARM7 BIOS.
  // `dsi_key`: the DSi's key table (ARM7i BIOS 0xC6D0), which a DSi card's
  // DSi-mode secure area is under; otherwise the DS's (ARM7 BIOS 0x30).
  void key1_init(u32 idcode, u32 level, u32 mod, bool dsi_key = false);
  void key1_encrypt(u32* data) const;
  void key1_decrypt(u32* data) const;

private:
  NDS& nds_;
  std::unique_ptr<RomSource> rom_;
  u32 page_base_ = 0xFFFFFFFFu; const u8* page_ = nullptr;   // last page rom_read32 fetched
  Header header_{};
  TwlHeader twl_{};
  u32 chip_id_ = 0;
  u32 rom_mask_ = 0;

  // A DSi-enhanced or DSi-only card (and not a dump with the DSi part cut
  // off): on a DSi the launcher wakes it with 3D instead of 3C, and from then
  // on its DSi area (from dsi_base_) can be read, the first 0x3000 bytes of
  // it only as the KEY1 secure area. Woken with 3C it is a DS card, and the
  // whole DSi area reads as the DS area's last page does. Adapted from
  // melonDS's NDSCart (GPLv3, melonDS team).
  bool dsi_cart_ = false;
  u32  dsi_base_ = 0;       // where the DSi area starts (header 0x92, in 512 KB units)
  bool dsi_mode_ = false;   // woken with 3D (or direct-booted on a DSi)
  bool dsi_area_blocked(u32 addr) const { return dsi_cart_ && listed_ && addr >= dsi_base_ && (!dsi_mode_ || addr < dsi_base_ + 0x3000); }

  bool in_reset_ = true;
  u32 cmd_mode_ = 0;        // 0 plain, 1 KEY1, 2 KEY2
  u32 data_mode_ = 0;
  u8  rom_cmd_[8]{};
  u32 rom_addr_ = 0;
  bool launch_read_ = false;
  u32 rom_read32();

  std::array<u32, 0x412> key1_{};
  void key1_apply_keycode(u32* keycode, u32 mod);

  SaveType save_type_ = SaveType::None;
  // Infrared carts (game code 'I???'): AUXSPI reaches the IR chip first; its
  // first byte is a command, 0x00 = pass through to save chip, 0x08 = ID (0xAA).
  bool ir_cart_ = false; u8 ir_cmd_ = 0; u32 ir_pos_ = 0;
  std::vector<u8> sram_;
  bool sram_dirty_ = false;
  bool sram_from_state_ = false;
  u32  sram_writes_ = 0;
  void mark_dirty() { sram_dirty_ = true; ++sram_writes_; }
  u32 spi_pos_ = 0; u8 spi_cmd_ = 0; u32 spi_addr_ = 0; u8 spi_status_ = 0;
  u8 spi_eeprom_tiny(u8 v); u8 spi_eeprom(u8 v); u8 spi_flash(u8 v);
  u8 spi_chip(u8 v);    // the byte to the chip the save type names

  // NAND carts (WarioWare D.I.Y., Jam with the Band, Face Training): the save
  // is a region of the ROM chip itself, reached with card commands and not the
  // AUXSPI bus. The game opens a 128 KB window on it (B2), reads through B7,
  // and writes 2 KB pages: write enable (85), the page's words (81), commit
  // (82). 8B goes back to reading the ROM. Adapted from melonDS's
  // CartRetailNAND (GPLv3, melonDS team).
  u32 nand_base_ = 0;       // where the save starts in the card's address space (header 0x96 << 17)
  u32 nand_window_ = 0;     // the open window's address; 0 = ROM mode
  u32 nand_addr_ = 0;       // read or write position inside the window; 0 = none
  u8  nand_status_ = 0x20;  // bit 5 ready, bit 4 write enabled
  std::array<u8, 0x800> nand_wbuf_{};
  u32 nand_wpos_ = 0, nand_wlen_ = 0;
  std::array<u8, 0x30> nand_id_{};
  void nand_build_id();
  bool nand_command(const u8 cmd[8]);           // false: not a NAND command, the ROM's own
  u32  nand_read32();
  bool nand_window_ok() const { return nand_window_ >= nand_base_ && nand_window_ - nand_base_ < sram_.size(); }

  bool listed_ = true;
  std::vector<u8> detect_buf_;   // SaveType::Detect transaction so far; not saved to state
  u8 spi_detect(u8 v);
  void detect_release();
  void set_chip(SaveType type, u32 bytes);
  void fit_flash(u32 addr);
};

// Save type per game code; default is 64 KB EEPROM.
SaveType save_type_for(u32 game_code, u32& size);
// The chip a save file of `bytes` bytes came from, or None.
SaveType save_type_for_size(u32 bytes);
// Whether save_list.inc carries this game code (save_type_for always
// answers, falling back to the default, so can't be used for this).
bool known_game_code(u32 game_code);

} // namespace ds::cart
