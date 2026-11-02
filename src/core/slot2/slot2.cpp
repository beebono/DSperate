// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Slot-2 devices. The GBA cartridge, its Flash protocol, the solar sensor,
// the Rumble Pak and the Guitar Grip are adapted from melonDS's GBACart.cpp (GPLv3, melonDS
// team; its Flash handling is in turn from DeSmuME).
#include "core/slot2/slot2.h"
#include "core/state/state.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace ds::slot2 {

namespace {

// Boktai carts, which carry the solar sensor (melonDS's list).
constexpr const char* SOLAR_CODES[] = {
  "U3IJ", "U3IE", "U3IP",   // Boktai: The Sun Is in Your Hand
  "U32J", "U32E", "U32P",   // Boktai 2: Solar Boy Django
  "U33J",                   // Shin Bokura no Taiyou: Gyakushuu no Sabata
  "A3IJ",                   // Boktai (USA) sample
};
// Sensor counts per light level (melonDS).
constexpr u8 LUX[11] = {0, 5, 11, 18, 27, 42, 62, 84, 109, 139, 183};

constexpr size_t HEADER_BYTES = 0xC0;
constexpr size_t MAX_ROM = 32u * 1024 * 1024;   // the whole ROM bus

bool header_ok(const u8* h) { return h[0xB2] == 0x96; }

bool contains(const u8* data, size_t size, const char* needle) {
  const size_t n = std::strlen(needle);
  for (size_t i = 0; i + n <= size; ++i) {
    const void* hit = std::memchr(data + i, needle[0], size - n + 1 - i);
    if (!hit) return false;
    i = static_cast<size_t>(static_cast<const u8*>(hit) - data);
    if (std::memcmp(data + i, needle, n) == 0) return true;
  }
  return false;
}

} // namespace

const char* save_type_name(SaveType t) {
  switch (t) {
  case SaveType::Sram32K: return "SRAM 32 KB";
  case SaveType::Flash64K: return "Flash 64 KB";
  case SaveType::Flash128K: return "Flash 128 KB";
  case SaveType::Eeprom: return "EEPROM";
  case SaveType::None: break;
  }
  return "none";
}

u32 save_type_bytes(SaveType t) {
  switch (t) {
  case SaveType::Sram32K: return 32 * 1024;
  case SaveType::Flash64K: return 64 * 1024;
  case SaveType::Flash128K: return 128 * 1024;
  case SaveType::Eeprom: return 8 * 1024;
  case SaveType::None: break;
  }
  return 0;
}

SaveType save_type_for_size(size_t bytes) {
  switch (bytes) {
  case 512: case 8 * 1024: return SaveType::Eeprom;
  case 32 * 1024: return SaveType::Sram32K;
  case 64 * 1024: return SaveType::Flash64K;
  case 128 * 1024: case 128 * 1024 + 0x10: return SaveType::Flash128K;
  default: return SaveType::None;
  }
}

SaveType save_type_for_rom(const u8* rom, size_t size) {
  // The longer names first: "FLASH_V" is not a prefix of the others, but
  // keeping the order explicit costs nothing.
  if (contains(rom, size, "FLASH1M_V")) return SaveType::Flash128K;
  if (contains(rom, size, "FLASH512_V") || contains(rom, size, "FLASH_V")) return SaveType::Flash64K;
  if (contains(rom, size, "SRAM_V") || contains(rom, size, "SRAM_F_V")) return SaveType::Sram32K;
  if (contains(rom, size, "EEPROM_V")) return SaveType::Eeprom;
  return SaveType::None;
}

GameCart::GameCart(std::vector<u8> rom, std::vector<u8> save, SaveType type) : rom_(std::move(rom)) {
  const size_t real_size = rom_.size();
  rom_.resize(HEADER_BYTES > rom_.size() ? HEADER_BYTES : rom_.size(), 0);
  size_t pow2 = 1;
  while (pow2 < rom_.size()) pow2 <<= 1;
  rom_.resize(pow2, 0);

  std::memcpy(title_, &rom_[0xA0], 12);
  std::memcpy(code_, &rom_[0xAC], 4);
  for (const char* c : SOLAR_CODES) if (std::memcmp(code_, c, 4) == 0) solar_ = true;
  u64 h = 1469598103934665603ull;
  for (size_t i = 0; i < HEADER_BYTES; ++i) h = (h ^ rom_[i]) * 1099511628211ull;
  identity_ = h ^ real_size;

  if (type == SaveType::None) type = save.empty() ? save_type_for_rom(rom_.data(), real_size) : save_type_for_size(save.size());
  save_type_ = type;
  if (type == SaveType::Eeprom && !save.empty()) save_ = std::move(save);   // kept as is, written back as is
  else if (type != SaveType::None) {
    const u32 bytes = save_type_bytes(type);
    // An RTC footer some emulators append after a 128 KB save is kept.
    if (save.size() < bytes) save.resize(bytes, 0xFF);
    save_ = std::move(save);
  }
  if (type == SaveType::Flash64K) { flash_maker_ = 0x32; flash_device_ = 0x1B; }   // Panasonic MN63F805MNP
  if (type == SaveType::Flash128K) { flash_maker_ = 0x62; flash_device_ = 0x13; }  // Sanyo LE26FV10N1TS
  reset();
}

void GameCart::reset() {
  gpio_data_ = gpio_dir_ = gpio_ctrl_ = 0;
  flash_state_ = flash_cmd_ = flash_bank_ = 0;
  light_edge_ = false;
  light_counter_ = 0;
  light_sample_ = 0xFF;
}

u16 GameCart::rom_read16(u32 addr) const {
  addr &= 0x01FFFFFE;
  if (addr >= 0xC4 && addr < 0xCA && (gpio_ctrl_ & 1)) {
    if (addr == 0xC4) return gpio_data_;
    if (addr == 0xC6) return gpio_dir_;
    return gpio_ctrl_;
  }
  if (addr < rom_.size()) { u16 v; std::memcpy(&v, &rom_[addr], 2); return v; }
  return 0;
}

void GameCart::rom_write16(u32 addr, u16 v) {
  switch (addr & 0x01FFFFFE) {
  case 0xC4:
    gpio_data_ = static_cast<u16>((gpio_data_ & ~gpio_dir_) | (v & gpio_dir_));
    process_gpio();
    break;
  case 0xC6: gpio_dir_ = v; break;
  case 0xC8: gpio_ctrl_ = v; break;
  default: break;   // ROM: writes go nowhere
  }
}

void GameCart::process_gpio() {
  if (!solar_) return;
  if (gpio_data_ & 4) return;   // sensor not selected
  if (gpio_data_ & 2) {         // reset: the sample the counter must reach
    light_counter_ = 0;
    light_sample_ = static_cast<u8>(0xFF - (0x16 + LUX[light_level_]));
  }
  if ((gpio_data_ & 1) && light_edge_) ++light_counter_;
  light_edge_ = !(gpio_data_ & 1);
  const bool bit = light_counter_ >= light_sample_;
  if (gpio_ctrl_ & 1) gpio_data_ = static_cast<u16>((gpio_data_ & gpio_dir_) | ((bit << 3) & ~gpio_dir_ & 0xF));
}

u8 GameCart::sram_read8(u32 addr) {
  addr &= 0xFFFF;
  switch (save_type_) {
  case SaveType::Sram32K: return addr < save_.size() ? save_[addr] : 0xFF;
  case SaveType::Flash64K: case SaveType::Flash128K: return flash_read(addr);
  default: return 0xFF;
  }
}

void GameCart::sram_write8(u32 addr, u8 v) {
  addr &= 0xFFFF;
  switch (save_type_) {
  case SaveType::Sram32K: if (addr < save_.size()) save_store(addr, v); return;
  case SaveType::Flash64K: case SaveType::Flash128K: flash_write(addr, v); return;
  default: return;
  }
}

void GameCart::save_store(u32 offset, u8 v) {
  if (offset >= save_.size() || save_[offset] == v) return;
  save_[offset] = v;
  mark_dirty();
}

u8 GameCart::flash_read(u32 addr) {
  if (flash_cmd_ == 0) {
    const u32 at = addr + 0x10000u * flash_bank_;
    return at < save_.size() ? save_[at] : 0xFF;
  }
  if (flash_cmd_ == 0x90) {   // chip ID
    if (addr == 0) return flash_maker_;
    if (addr == 1) return flash_device_;
  }
  return 0xFF;
}

void GameCart::flash_write(u32 addr, u8 v) {
  switch (flash_state_) {
  case 0x00:
    if (addr == 0x5555 && v == 0xF0) { flash_state_ = flash_cmd_ = 0; return; }   // reset / leave ID mode
    if (addr == 0x5555 && v == 0xAA) { flash_state_ = 1; return; }
    if (addr == 0x0000 && flash_cmd_ == 0xB0) {   // bank select (128 KB only)
      flash_bank_ = save_type_ == SaveType::Flash128K ? (v & 1) : 0;
      flash_cmd_ = 0;
      return;
    }
    break;
  case 0x01:
    if (addr == 0x2AAA && v == 0x55) { flash_state_ = 2; return; }
    flash_state_ = 0;
    break;
  case 0x02:
    if (addr == 0x5555) {
      flash_state_ = v == 0x80 ? 0x80 : v == 0x90 ? 0x90 : 0;
      flash_cmd_ = v;
      return;
    }
    flash_state_ = 0;
    break;
  case 0x80:   // erase: second unlock
    if (addr == 0x5555 && v == 0xAA) { flash_state_ = 0x81; return; }
    flash_state_ = 0;
    break;
  case 0x81:
    if (addr == 0x2AAA && v == 0x55) { flash_state_ = 0x82; return; }
    flash_state_ = 0;
    break;
  case 0x82:
    if (v == 0x30) {   // 4 KB sector
      const u32 start = (addr & 0xF000) + 0x10000u * flash_bank_;
      for (u32 i = 0; i < 0x1000; ++i) save_store(start + i, 0xFF);
    } else if (v == 0x10 && addr == 0x5555) {   // whole chip
      for (u32 i = 0; i < save_type_bytes(save_type_); ++i) save_store(i, 0xFF);
    }
    flash_state_ = flash_cmd_ = 0;
    return;
  case 0x90:   // in ID mode: an unlock leads to the exit command
    if (addr == 0x5555 && v == 0xAA) { flash_state_ = 0x91; return; }
    flash_state_ = 0;
    break;
  case 0x91:
    if (addr == 0x2AAA && v == 0x55) { flash_state_ = 0x92; return; }
    flash_state_ = 0;
    break;
  case 0x92:
    flash_state_ = flash_cmd_ = 0;
    return;
  default:
    break;
  }
  if (flash_cmd_ == 0xA0) {   // byte program
    save_store(addr + 0x10000u * flash_bank_, v);
    flash_state_ = flash_cmd_ = 0;
  }
}

template <class S> void GameCart::sync(S& s) {
  s.fields(gpio_data_, gpio_dir_, gpio_ctrl_, flash_state_, flash_cmd_, flash_bank_, light_edge_, light_counter_, light_sample_);
  u8 type = static_cast<u8>(save_type_);
  u32 n = static_cast<u32>(save_.size());
  s.fields(type, n);
  if constexpr (S::reading) {
    // The state's save wins, as the DS card's does: what the guest wrote
    // before the state was taken is what it will read back.
    if (n > 256 * 1024) { s.fail("GBA save too large"); return; }
    save_type_ = static_cast<SaveType>(type);
    save_.resize(n);
  }
  s.blob(save_.data(), save_.size());
  if constexpr (S::reading) if (n) mark_dirty();
}

std::unique_ptr<GameCart> load_game(const std::string& rom_path, const std::string& save_path, std::string& err) {
  std::ifstream f(rom_path, std::ios::binary | std::ios::ate);
  if (!f) { err = "cannot open " + rom_path; return nullptr; }
  const std::streamoff size = f.tellg();
  if (size < static_cast<std::streamoff>(HEADER_BYTES)) { err = rom_path + " is too small for a GBA ROM"; return nullptr; }
  if (size > static_cast<std::streamoff>(MAX_ROM)) { err = rom_path + " is larger than the 32 MB a GBA cartridge can hold"; return nullptr; }
  std::vector<u8> rom(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(rom.data()), size)) { err = "cannot read " + rom_path; return nullptr; }
  if (!header_ok(rom.data())) { err = rom_path + " has no GBA header"; return nullptr; }

  std::vector<u8> save;
  if (!save_path.empty()) {
    std::ifstream s(save_path, std::ios::binary | std::ios::ate);
    if (s) {
      const std::streamoff n = s.tellg();
      if (n > 0 && n <= 256 * 1024) {
        save.resize(static_cast<size_t>(n));
        s.seekg(0);
        if (!s.read(reinterpret_cast<char*>(save.data()), n)) save.clear();
      }
      if (!save.empty() && save_type_for_size(save.size()) == SaveType::None) {
        std::fprintf(stderr, "slot2: %s is %zu bytes, a size no GBA save chip has; ignored\n", save_path.c_str(), save.size());
        save.clear();
      }
    }
  }
  return std::make_unique<GameCart>(std::move(rom), std::move(save));
}

bool peek_game(const std::string& rom_path, char code[5], char title[13]) {
  std::ifstream f(rom_path, std::ios::binary);
  u8 h[HEADER_BYTES];
  if (!f || !f.read(reinterpret_cast<char*>(h), sizeof h) || !header_ok(h)) return false;
  std::memcpy(code, &h[0xAC], 4); code[4] = 0;
  std::memcpy(title, &h[0xA0], 12); title[12] = 0;
  return true;
}

bool write_save(const GameCart& cart, const std::string& path, std::string& err) {
  if (cart.save().empty()) return true;
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) { err = "cannot write " + tmp; return false; }
  const bool ok = std::fwrite(cart.save().data(), 1, cart.save().size(), f) == cart.save().size();
  if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) { err = "cannot write " + path; return false; }
  return true;
}

void RumblePak::rom_write16(u32, u16 v) {
  if (v == state_) return;
  state_ = v;
  ++events_;
}

u8 GuitarGrip::sram_read8(u32) {
  return static_cast<u8>(~(((frets_ & Green) ? 0x40 : 0) | ((frets_ & Red) ? 0x20 : 0) | ((frets_ & Yellow) ? 0x10 : 0) | ((frets_ & Blue) ? 0x08 : 0)));
}

void RumblePak::sync_state(state::Writer& w) { w.put(state_); }
void RumblePak::sync_state(state::Reader& r) { r.put(state_); }

} // namespace ds::slot2
