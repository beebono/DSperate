// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Slot-1 retail cartridge. Protocol per GBATEK ("DS Cartridge Protocol",
// "DS Cartridge Secure Area", "DS Cartridge Backup"); melonDS (GPLv3) is the
// behavioural reference.
#include "core/cart/cart.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::cart {

namespace {
inline u32 bswap(u32 v) { return __builtin_bswap32(v); }
}

SaveType save_type_for(u32 code, u32& size) {
  // melonDS SaveMemType numbering: 1 = 512 B EEPROM, 2 = 8 KB, 3 = 64 KB,
  // 4 = 128 KB EEPROM, 5 = 256 KB, 6 = 512 KB, 7 = 1 MB, 8..10 = 8/16/64 MB FLASH.
  // The table is the melonDS ROM list (save_list.inc), sorted by game code.
  struct E { u32 code; u32 type; };
  static const E list[] = {
#include "core/cart/save_list.inc"
  };
  u32 t = 3;   // unknown title: 64 KB EEPROM, the most common chip
  size_t lo = 0, hi = sizeof list / sizeof list[0];
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (list[mid].code < code) lo = mid + 1;
    else if (list[mid].code > code) hi = mid;
    else { if (list[mid].type >= 1 && list[mid].type <= 10) t = list[mid].type; break; }
  }
  static const u32 sizes[] = {0, 512, 8192, 65536, 131072, 262144, 524288, 1048576,
                              8388608, 16777216, 67108864};
  size = sizes[t];
  if (t == 1) return SaveType::EepromTiny;
  if (t <= 4) return SaveType::Eeprom;
  return SaveType::Flash;
}

Cart::Cart(NDS& nds, std::vector<u8> rom) : nds_(nds), rom_(std::move(rom)) {
  // Pad to a power of two so address masking wraps like the hardware.
  u32 size = 0x1000; while (size < rom_.size()) size <<= 1;
  rom_.resize(size, 0xFF);
  rom_mask_ = size - 1;
  std::memcpy(&header_, rom_.data(), sizeof header_);
  chip_id_ = 0x000000C2;
  if (size >= 1024 * 1024 && size <= 128 * 1024 * 1024) chip_id_ |= ((size >> 20) - 1) << 8;
  else chip_id_ |= (0x100 - (size >> 28)) << 8;
  u32 sram_size = 0;
  save_type_ = save_type_for(header_.game_code_u32(), sram_size);
  ir_cart_ = (header_.game_code_u32() & 0xFF) == 'I';
  sram_.assign(sram_size, 0xFF);

  // Dumps often carry a decrypted secure area; the cart must hand out the
  // encrypted form, so re-encrypt if the "decrypted" marker is present.
  const u32 a9 = header_.arm9_rom_offset;
  if (a9 >= 0x4000 && a9 < 0x8000) {
    u32 w0, w4; std::memcpy(&w0, &rom_[a9], 4); std::memcpy(&w4, &rom_[a9 + 0x10], 4);
    if (w0 == 0xE7FFDEFF && w4 != 0xE7FFDEFF) {
      std::memcpy(&rom_[a9], "encryObj", 8);
      key1_init(header_.game_code_u32(), 3, 2);
      for (u32 i = 0; i < 0x800; i += 8) key1_encrypt(reinterpret_cast<u32*>(&rom_[a9 + i]));
      key1_init(header_.game_code_u32(), 2, 2);
      key1_encrypt(reinterpret_cast<u32*>(&rom_[a9]));
    }
  }
  reset();
}

void Cart::reset() {
  in_reset_ = true; cmd_mode_ = data_mode_ = 0; rom_addr_ = 0; std::memset(rom_cmd_, 0, 8);
  spi_pos_ = 0; spi_cmd_ = 0; spi_addr_ = 0; spi_status_ = 0;
}

void Cart::set_reset(bool reset) {
  if (reset == in_reset_) return;
  in_reset_ = reset;
  cmd_mode_ = data_mode_ = 0; rom_addr_ = 0; std::memset(rom_cmd_, 0, 8);
}

void Cart::setup_direct_boot() { cmd_mode_ = 2; data_mode_ = 2; in_reset_ = false; }

// ---- KEY1 -----------------------------------------------------------------
void Cart::key1_encrypt(u32* data) const {
  u32 y = data[0], x = data[1], z;
  for (u32 i = 0; i <= 0xF; ++i) {
    z = key1_[i] ^ x;
    x  = key1_[0x012 + (z >> 24)];
    x += key1_[0x112 + ((z >> 16) & 0xFF)];
    x ^= key1_[0x212 + ((z >> 8) & 0xFF)];
    x += key1_[0x312 + (z & 0xFF)];
    x ^= y; y = z;
  }
  data[0] = x ^ key1_[0x10];
  data[1] = y ^ key1_[0x11];
}
void Cart::key1_decrypt(u32* data) const {
  u32 y = data[0], x = data[1], z;
  for (u32 i = 0x11; i >= 0x2; --i) {
    z = key1_[i] ^ x;
    x  = key1_[0x012 + (z >> 24)];
    x += key1_[0x112 + ((z >> 16) & 0xFF)];
    x ^= key1_[0x212 + ((z >> 8) & 0xFF)];
    x += key1_[0x312 + (z & 0xFF)];
    x ^= y; y = z;
  }
  data[0] = x ^ key1_[0x1];
  data[1] = y ^ key1_[0x0];
}
void Cart::key1_apply_keycode(u32* keycode, u32 mod) {
  key1_encrypt(&keycode[1]);
  key1_encrypt(&keycode[0]);
  u32 temp[2] = {0, 0};
  for (u32 i = 0; i <= 0x11; ++i) key1_[i] ^= bswap(keycode[i % mod]);
  for (u32 i = 0; i <= 0x410; i += 2) {
    key1_encrypt(temp);
    key1_[i] = temp[1]; key1_[i + 1] = temp[0];
  }
}
void Cart::key1_init(u32 idcode, u32 level, u32 mod) {
  // Key table lives in the ARM7 BIOS at 0x30 (0x1048 bytes).
  std::memcpy(key1_.data(), nds_.bus.bios7.get() + 0x30, key1_.size() * 4);
  u32 keycode[3] = {idcode, idcode >> 1, idcode << 1};
  if (level >= 1) key1_apply_keycode(keycode, mod);
  if (level >= 2) key1_apply_keycode(keycode, mod);
  keycode[1] <<= 1; keycode[2] >>= 1;
  if (level >= 3) key1_apply_keycode(keycode, mod);
}

void Cart::decrypt_secure_area(u8 out[0x800]) {
  const u32 a9 = header_.arm9_rom_offset;
  std::memcpy(out, &rom_[a9], 0x800);
  key1_init(header_.game_code_u32(), 2, 2);
  key1_decrypt(reinterpret_cast<u32*>(&out[0]));
  key1_init(header_.game_code_u32(), 3, 2);
  for (u32 i = 0; i < 0x800; i += 8) key1_decrypt(reinterpret_cast<u32*>(&out[i]));
  if (!std::strncmp(reinterpret_cast<const char*>(out), "encryObj", 8)) {
    u32 marker = 0xE7FFDEFF; std::memcpy(&out[0], &marker, 4); std::memcpy(&out[4], &marker, 4);
  } else {
    std::fprintf(stderr, "[cart] secure area decryption failed\n");
    for (u32 i = 0; i < 0x800; i += 4) { u32 marker = 0xE7FFDEFF; std::memcpy(&out[i], &marker, 4); }
  }
}

// ---- ROM commands -----------------------------------------------------------
u32 Cart::rom_read32() {
  // Reads wrap within a 4 KB page.
  const u32 hi = rom_addr_ & rom_mask_ & ~0xFFFu;
  u32 lo = rom_addr_ & 0xFFF;
  u32 v = 0;
  for (int i = 0; i < 4; ++i) { v |= static_cast<u32>(rom_[hi | lo]) << (8 * i); lo = (lo + 1) & 0xFFF; }
  rom_addr_ = hi | lo;
  return v;
}

void Cart::command_start(const u8 cmd[8]) {
  if (in_reset_) return;
  if (cmd_mode_ == 0) {
    std::memcpy(rom_cmd_, cmd, 8);
    switch (rom_cmd_[0]) {
    case 0x00: rom_addr_ = ((rom_cmd_[1] << 24) | (rom_cmd_[2] << 16) | (rom_cmd_[3] << 8) | rom_cmd_[4]) & 0xFFF; return;
    case 0x3C: cmd_mode_ = 1; key1_init(header_.game_code_u32(), 2, 2); return;
    default: return;
    }
  }
  if (cmd_mode_ == 1) {
    // KEY1 commands arrive encrypted (the BIOS encrypts them); decrypt to dispatch.
    u8 dec[8]; u32 w;
    std::memcpy(&w, &cmd[4], 4); w = bswap(w); std::memcpy(&dec[0], &w, 4);
    std::memcpy(&w, &cmd[0], 4); w = bswap(w); std::memcpy(&dec[4], &w, 4);
    key1_decrypt(reinterpret_cast<u32*>(dec));
    u32 t0, t1; std::memcpy(&t0, &dec[0], 4); std::memcpy(&t1, &dec[4], 4);
    t0 = bswap(t0); t1 = bswap(t1);
    std::memcpy(&dec[0], &t1, 4); std::memcpy(&dec[4], &t0, 4);
    std::memcpy(rom_cmd_, dec, 8);
    switch (rom_cmd_[0] & 0xF0) {
    case 0x40: data_mode_ = 2; return;                          // enable KEY2 data
    case 0x20: rom_addr_ = (rom_cmd_[2] & 0xF0) << 8; return;   // secure area block
    case 0xA0: cmd_mode_ = 2; return;                           // leave KEY1 mode
    default: return;
    }
  }
  std::memcpy(rom_cmd_, cmd, 8);
  if (rom_cmd_[0] == 0xB7) {
    rom_addr_ = ((rom_cmd_[1] << 24) | (rom_cmd_[2] << 16) | (rom_cmd_[3] << 8) | rom_cmd_[4]) & rom_mask_;
    if (rom_addr_ < 0x8000) rom_addr_ = 0x8000 + (rom_addr_ & 0x1FF);   // secure area is not readable here
  }
}

u32 Cart::command_receive() {
  if (in_reset_) return 0;
  if (cmd_mode_ == 0) {
    switch (rom_cmd_[0]) { case 0x9F: return 0xFFFFFFFF; case 0x00: return rom_read32(); case 0x90: return chip_id_; }
  } else if (cmd_mode_ == 1) {
    switch (rom_cmd_[0] & 0xF0) { case 0x10: return chip_id_; case 0x20: return rom_read32(); }
  } else {
    switch (rom_cmd_[0]) { case 0xB7: return rom_read32(); case 0xB8: return chip_id_; }
  }
  return 0;
}

// ---- save chip --------------------------------------------------------------
// DS_AUXSPI_LOG=1: one line per save-chip transaction (command, address, bytes).
static const bool g_auxspi_log = std::getenv("DS_AUXSPI_LOG") != nullptr;
void Cart::spi_release() {
  if (g_auxspi_log && spi_pos_) std::fprintf(stderr, "[auxspi] cmd %02x addr %06x len %u status %02x%s\n", spi_cmd_, spi_addr_, spi_pos_, spi_status_, ir_cart_ ? " (ir)" : "");
  spi_pos_ = 0; ir_pos_ = 0;
}

u8 Cart::spi_transfer(u8 v) {
  if (ir_cart_) {
    if (ir_pos_++ == 0) { ir_cmd_ = v; if (g_auxspi_log && v != 0) std::fprintf(stderr, "[auxspi] ir cmd %02x\n", v); return 0; }
    if (ir_cmd_ == 0x08) return 0xAA;
    if (ir_cmd_ != 0x00) return 0;
  }
  if (save_type_ == SaveType::None) return 0;
  u8 ret = 0xFF;
  if (spi_pos_ == 0) {
    switch (v) {
    case 0x04: spi_status_ &= ~2; spi_pos_++; return 0;   // write disable
    case 0x06: spi_status_ |= 2;  spi_pos_++; return 0;   // write enable
    default: spi_cmd_ = v; spi_addr_ = 0; break;
    }
  } else {
    switch (save_type_) {
    case SaveType::EepromTiny: ret = spi_eeprom_tiny(v); break;
    case SaveType::Eeprom: ret = spi_eeprom(v); break;
    case SaveType::Flash: ret = spi_flash(v); break;
    default: break;
    }
  }
  spi_pos_++;
  return ret;
}

u8 Cart::spi_eeprom_tiny(u8 v) {
  switch (spi_cmd_) {
  case 0x01: if (spi_pos_ == 1) spi_status_ = (spi_status_ & 1) | (v & 0x0C); return 0;
  case 0x05: return spi_status_ | 0xF0;
  case 0x02: case 0x0A:
    if (spi_pos_ < 2) spi_addr_ = v;
    else { if (spi_status_ & 2) { sram_[(spi_addr_ + (spi_cmd_ == 0x0A ? 0x100 : 0)) & 0x1FF] = v; sram_dirty_ = true; } spi_addr_++; }
    return 0;
  case 0x03: case 0x0B:
    if (spi_pos_ < 2) { spi_addr_ = v; return 0; }
    return sram_[(spi_addr_++ + (spi_cmd_ == 0x0B ? 0x100 : 0)) & 0x1FF];
  case 0x9F: return 0xFF;
  default: return 0xFF;
  }
}

u8 Cart::spi_eeprom(u8 v) {
  const u32 addrsize = sram_.size() > 65536 ? 3 : 2;
  const u32 mask = static_cast<u32>(sram_.size() - 1);
  switch (spi_cmd_) {
  case 0x01: if (spi_pos_ == 1) spi_status_ = (spi_status_ & 1) | (v & 0x0C); return 0;
  case 0x05: return spi_status_;
  case 0x02:
    if (spi_pos_ <= addrsize) spi_addr_ = (spi_addr_ << 8) | v;
    else { if (spi_status_ & 2) { sram_[spi_addr_ & mask] = v; sram_dirty_ = true; } spi_addr_++; }
    return 0;
  case 0x03:
    if (spi_pos_ <= addrsize) { spi_addr_ = (spi_addr_ << 8) | v; return 0; }
    return sram_[(spi_addr_++) & mask];
  case 0x9F: return 0xFF;
  default: return 0xFF;
  }
}

u8 Cart::spi_flash(u8 v) {
  const u32 mask = static_cast<u32>(sram_.size() - 1);
  switch (spi_cmd_) {
  case 0x05: return spi_status_;
  case 0x02:   // page program: can only clear bits (an erased page reads 0xFF)
    if (spi_pos_ <= 3) spi_addr_ = (spi_addr_ << 8) | v;
    else { if (spi_status_ & 2) { sram_[spi_addr_ & mask] &= v; sram_dirty_ = true; } spi_addr_++; }
    return 0;
  case 0x0A:   // page write
    if (spi_pos_ <= 3) spi_addr_ = (spi_addr_ << 8) | v;
    else { if (spi_status_ & 2) { sram_[spi_addr_ & mask] = v; sram_dirty_ = true; } spi_addr_++; }
    return 0;
  case 0x03:
    if (spi_pos_ <= 3) { spi_addr_ = (spi_addr_ << 8) | v; return 0; }
    return sram_[(spi_addr_++) & mask];
  case 0x0B:   // fast read (dummy byte)
    if (spi_pos_ <= 3) { spi_addr_ = (spi_addr_ << 8) | v; return 0; }
    if (spi_pos_ == 4) return 0;
    return sram_[(spi_addr_++) & mask];
  case 0x9F: return 0xFF;
  case 0xD8:   // sector erase
    if (spi_pos_ <= 3) spi_addr_ = (spi_addr_ << 8) | v;
    if (spi_pos_ == 3 && (spi_status_ & 2)) { for (u32 i = 0; i < 0x10000; ++i) sram_[(spi_addr_++) & mask] = 0xFF; sram_dirty_ = true; }
    return 0;
  case 0xDB:   // page erase
    if (spi_pos_ <= 3) spi_addr_ = (spi_addr_ << 8) | v;
    if (spi_pos_ == 3 && (spi_status_ & 2)) { for (u32 i = 0; i < 0x100; ++i) sram_[(spi_addr_++) & mask] = 0xFF; sram_dirty_ = true; }
    return 0;
  default: return 0xFF;
  }
}

} // namespace ds::cart
