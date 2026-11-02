// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What sits in the DS's slot 2 (the GBA slot): a GBA game cartridge, with
// its save memory and the Boktai solar sensor, a DS Rumble Pak, or the
// Guitar Hero: On Tour Guitar Grip. Adapted
// from melonDS's GBACart.cpp (GPLv3, melonDS team), behaviour per GBATEK.
//
// The bus (mem/bus.cpp) owns the slot's address decode and EXMEMCNT
// ownership; a Device only answers the 16-bit ROM bus at 0x08000000-
// 0x09FFFFFF and the 8-bit SRAM bus at 0x0A000000-0x0AFFFFFF. An empty slot
// is no Device at all: the bus answers open bus itself.
#pragma once
#include "core/types.h"

#include <memory>
#include <string>
#include <vector>

namespace ds::state { class Writer; class Reader; }

namespace ds::slot2 {

enum class Kind : u8 { None = 0, Game = 1, RumblePak = 2, GuitarGrip = 3 };

class Device {
public:
  virtual ~Device() = default;
  virtual Kind kind() const = 0;
  // Which device this is, beyond its kind: a state names it, and loads only
  // into the same one (a game's header and size; 0 for an accessory).
  virtual u64 identity() const { return 0; }
  virtual void reset() {}

  // `addr` is the full bus address; the device masks what it decodes.
  virtual u16 rom_read16(u32 addr) const = 0;
  virtual void rom_write16(u32 addr, u16 v) { (void)addr; (void)v; }
  virtual u8 sram_read8(u32 addr) { (void)addr; return 0xFF; }
  virtual void sram_write8(u32 addr, u8 v) { (void)addr; (void)v; }

  // Bumped each time the Rumble Pak's motor line changes; the frontend
  // turns any change since its last look into a short pulse. 0 forever on
  // anything else. Not machine state: a load doesn't rewind it.
  virtual u32 rumble_events() const { return 0; }

  // The device's own state chunk body (the NDS writes the chunk header).
  virtual void sync_state(state::Writer& w) = 0;
  virtual void sync_state(state::Reader& r) = 0;
};

// GBA save memory, as the cartridge carries it. EEPROM sits on the ROM bus
// behind a serial protocol the DS doesn't drive; such a save is kept and
// written back unchanged, but the DS reads nothing from it.
enum class SaveType : u8 { None, Sram32K, Flash64K, Flash128K, Eeprom };
const char* save_type_name(SaveType t);
u32 save_type_bytes(SaveType t);
// From the size of a save file; None for a size no chip has. 128 KB + 16
// (an RTC footer some emulators append) reads as Flash128K.
SaveType save_type_for_size(size_t bytes);
// From the save library's ID string in the ROM ("SRAM_V", "FLASH1M_V", ...),
// how GBA emulators find a save chip a game has no file for yet.
SaveType save_type_for_rom(const u8* rom, size_t size);

// A GBA game cartridge. The ROM sits behind the 16-bit bus; the save chip
// behind the 8-bit one (SRAM, or Flash with its command protocol); Boktai
// carts add the solar sensor on the GPIO port at 0x080000C4.
class GameCart final : public Device {
public:
  // `rom` is the whole image; `save` the save file's bytes, empty for none.
  // `type` None picks from the save file's size, else from the ROM.
  GameCart(std::vector<u8> rom, std::vector<u8> save, SaveType type = SaveType::None);
  Kind kind() const override { return Kind::Game; }
  u64 identity() const override { return identity_; }
  void reset() override;

  u16 rom_read16(u32 addr) const override;
  void rom_write16(u32 addr, u16 v) override;
  u8 sram_read8(u32 addr) override;
  void sram_write8(u32 addr, u8 v) override;

  void sync_state(state::Writer& w) override { sync(w); }
  void sync_state(state::Reader& r) override { sync(r); }

  // Header fields, NUL-terminated.
  const char* title() const { return title_; }         // 12 characters at 0xA0
  const char* game_code() const { return code_; }      // 4 characters at 0xAC
  bool solar_sensor() const { return solar_; }

  // Boktai light level, 0 (dark) to 10 (full sun). The frontend sets it;
  // not part of a state, it is the player's surroundings.
  void set_light_level(int level) { light_level_ = static_cast<u8>(level < 0 ? 0 : level > 10 ? 10 : level); }
  int light_level() const { return light_level_; }

  SaveType save_type() const { return save_type_; }
  // The save memory as written to disk. Empty when the cart has no chip.
  const std::vector<u8>& save() const { return save_; }
  bool save_dirty() const { return save_dirty_; }
  u32 save_writes() const { return save_writes_; }   // bumped on every change
  void clear_save_dirty() { save_dirty_ = false; }

private:
  template <class S> void sync(S& s);
  void process_gpio();
  u8 flash_read(u32 addr);
  void flash_write(u32 addr, u8 v);
  void save_store(u32 offset, u8 v);
  void mark_dirty() { save_dirty_ = true; ++save_writes_; }

  std::vector<u8> rom_;         // padded to a power of two
  std::vector<u8> save_;
  SaveType save_type_ = SaveType::None;
  u64 identity_ = 0;
  char title_[13] = {};
  char code_[5] = {};
  bool solar_ = false;
  bool save_dirty_ = false;
  u32 save_writes_ = 0;

  // GPIO port (0x080000C4 data, C6 direction, C8 control; readable only with control bit 0).
  u16 gpio_data_ = 0, gpio_dir_ = 0, gpio_ctrl_ = 0;
  // Flash command state machine. `cmd` is the command in progress (0 none,
  // 0x90 ID mode, 0xA0 byte program, 0xB0 bank select), `state` how far
  // through the unlock sequence the next write is.
  u8 flash_state_ = 0, flash_cmd_ = 0, flash_bank_ = 0;
  u8 flash_maker_ = 0, flash_device_ = 0;
  // Solar sensor: a counter clocked by GPIO bit 0, compared against the
  // sample the light level sets on a reset (bit 1).
  bool light_edge_ = false;
  u8 light_counter_ = 0, light_sample_ = 0xFF;
  u8 light_level_ = 0;
};

// Loads a GBA ROM (a loose .gba) and its save. A missing save file is no
// save, an unreadable ROM is nullptr with the reason in `err`.
std::unique_ptr<GameCart> load_game(const std::string& rom_path, const std::string& save_path, std::string& err);
// Reads only the header of a GBA ROM: its game code and title. False when
// the file isn't one (too short, or no 0x96 fixed byte).
bool peek_game(const std::string& rom_path, char code[5], char title[13]);
// Writes the cart's save back (write-then-rename). False with `err` on failure.
bool write_save(const GameCart& cart, const std::string& path, std::string& err);

// The DS Rumble Pak (NTR-008), and the rumble of the Slide Controller and
// similar: the motor follows writes to the ROM bus, and the ROM bus reads
// back with A1 pulled low, which is how games find it.
class RumblePak final : public Device {
public:
  Kind kind() const override { return Kind::RumblePak; }
  void reset() override { state_ = 0; }
  u16 rom_read16(u32) const override { return 0xFFFD; }
  void rom_write16(u32 addr, u16 v) override;
  u32 rumble_events() const override { return events_; }
  void sync_state(state::Writer& w) override;
  void sync_state(state::Reader& r) override;

private:
  u16 state_ = 0;
  u32 events_ = 0;
};

// The Guitar Grip from Guitar Hero: On Tour: four fret buttons, read back on
// the SRAM bus active low (green bit 6, red 5, yellow 4, blue 3). The ROM bus
// answers with the ID the games look for.
class GuitarGrip final : public Device {
public:
  enum Fret : u8 { Green = 1, Red = 2, Yellow = 4, Blue = 8 };
  Kind kind() const override { return Kind::GuitarGrip; }
  u16 rom_read16(u32) const override { return 0xF9FF; }
  u8 sram_read8(u32) override;
  // The frets held this frame (Fret bits), set from the input frame like
  // the DS's own buttons; not machine state, so nothing goes in a state.
  void set_frets(u8 frets) { frets_ = frets & 0xF; }
  u8 frets() const { return frets_; }
  void sync_state(state::Writer&) override {}
  void sync_state(state::Reader&) override {}

private:
  u8 frets_ = 0;
};

} // namespace ds::slot2
