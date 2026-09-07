// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/bios/freebios.h"

#include <cstring>

namespace ds::bios {

u16 crc16(const u8* data, u32 len, u16 start) {
  static const u16 poly[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
  u32 crc = start;
  for (u32 i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      const bool carry = crc & 1;
      crc >>= 1;
      if (carry) crc ^= static_cast<u32>(poly[j]) << (7 - j);
    }
  }
  return static_cast<u16>(crc);
}

namespace {
void w16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }

// Field offsets follow GBATEK "DS Firmware Header" / "User Settings" and the
// values melonDS's generated firmware uses (SPI_Firmware.cpp).
void fill_header(u8* h) {
  std::memset(h, 0, 0x200);
  std::memcpy(h + 0x08, "DSPR", 4);            // identifier: not "MACP", so nothing mistakes it for a dump
  h[0x1D] = 0x20;                              // console type: DS Lite
  w16(h + 0x20, 0x3FE00 >> 3);                 // user settings offset (/8)
  w16(h + 0x2C, 0x138);                        // wifi config length
  h[0x2F] = 6;                                 // wifi version W006
  static const u8 unused3[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  std::memcpy(h + 0x30, unused3, 6);
  static const u8 mac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};
  std::memcpy(h + 0x36, mac, 6);
  w16(h + 0x3C, 0x3FFE);                       // enabled channels
  h[0x3E] = 0xFF; h[0x3F] = 0xFF;
  h[0x40] = 3;                                 // RF chip type 3
  h[0x41] = 0x94;                              // RF bits per entry
  h[0x42] = 0x29;                              // RF entries
  h[0x43] = 0x02;
  static const u16 init[9] = {0x0002, 0x0017, 0x0026, 0x1818, 0x0048, 0x4840, 0x0058, 0x0042, 0x0146};
  for (u32 i = 0; i < 9; ++i) w16(h + 0x44 + 2 * i, init[i]);
  w16(h + 0x2A, crc16(h + 0x2C, 0x138, 0x0000));   // wifi config checksum
}

void put_utf16(u8* dst, const std::string& s, u32 max_chars, u16* len_out) {
  const u32 n = static_cast<u32>(s.size() < max_chars ? s.size() : max_chars);
  for (u32 i = 0; i < n; ++i) w16(dst + 2 * i, static_cast<u8>(s[i]));
  w16(reinterpret_cast<u8*>(len_out), static_cast<u16>(n));
}

void fill_user(u8* u, const UserSettings& s) {
  std::memset(u, 0, 0x100);
  w16(u + 0x00, 5);                            // version
  u[0x02] = static_cast<u8>(s.favourite_colour & 15);
  u[0x03] = (s.birthday_month >= 1 && s.birthday_month <= 12) ? s.birthday_month : 1;
  u[0x04] = (s.birthday_day >= 1 && s.birthday_day <= 31) ? s.birthday_day : 1;
  u16 len;
  put_utf16(u + 0x06, s.nickname, 10, &len); std::memcpy(u + 0x1A, &len, 2);
  put_utf16(u + 0x1C, s.message, 26, &len);  std::memcpy(u + 0x50, &len, 2);
  // Touch calibration: identity in ADC<<4 form (normalise_touch_calibration
  // writes the same values over a dump).
  w16(u + 0x58, 0); w16(u + 0x5A, 0); u[0x5C] = 0; u[0x5D] = 0;
  w16(u + 0x5E, 255 << 4); w16(u + 0x60, 191 << 4); u[0x62] = 255; u[0x63] = 191;
  w16(u + 0x64, static_cast<u16>((s.language & 7) | (7 << 3)));   // language, backlight max
  w16(u + 0x70, 0);                            // update counter
  w16(u + 0x72, crc16(u, 0x70, 0xFFFF));
}

void fill_access_point(u8* ap, bool configured) {
  std::memset(ap, 0, 0x100);
  if (configured) {
    std::strncpy(reinterpret_cast<char*>(ap + 0x40), "DSperate-AP", 32);   // SSID
    ap[0xE7] = 0x00;                           // status: normal
    ap[0xEF] = 0x01;                           // connection configured
  } else {
    ap[0xE7] = 0xFF;                           // status: not configured
  }
  w16(ap + 0xFE, crc16(ap, 0xFE, 0x0000));
}
} // namespace

std::vector<u8> generate_firmware(const UserSettings& user) {
  std::vector<u8> fw(0x40000, 0xFF);
  fill_header(fw.data());
  fw[0x2FF] = 0x80;                            // boot0: NAND as stage-2 medium (as melonDS)
  // Wifi access points sit just below the user settings.
  fill_access_point(fw.data() + 0x3FA00, true);
  fill_access_point(fw.data() + 0x3FB00, false);
  fill_access_point(fw.data() + 0x3FC00, false);
  for (u32 blk = 0; blk < 2; ++blk) fill_user(fw.data() + 0x3FE00 + blk * 0x100, user);
  return fw;
}

} // namespace ds::bios
