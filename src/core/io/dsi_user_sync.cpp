// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_user_sync.h"
#include "core/io/dsi_nand_fs.h"
#include "core/bios/freebios.h"
#include "core/crypto/sha1.h"

#include <cstring>

namespace ds::io {
namespace {

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24)); }
void wr16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }

constexpr u32 kBlock = 0x100;   // one copy of the firmware's user settings
constexpr u32 kPage = 256;      // the flash's page

// Firmware user block (GBATEK "DS Firmware User Settings").
constexpr u32 kFwColour = 0x02, kFwMonth = 0x03, kFwDay = 0x04, kFwNick = 0x06, kFwNickLen = 0x1A,
              kFwMsg = 0x1C, kFwMsgLen = 0x50, kFwLang = 0x64, kFwCounter = 0x70, kFwCrc = 0x72,
              kFwExt = 0x74, kFwExtLang = 0x75, kFwExtMask = 0x76, kFwExtCrc = 0xFE;

// TWLCFG: header to 0x88 (SHA-1 at 0, version 0x80, counter 0x81, data length 0x84), then the data.
constexpr u32 kCfgData = 0x88, kCfgCounter = 0x81, kCfgLen = 0x84;
constexpr u32 kCfgLang = 0x06, kCfgColour = 0x44, kCfgMonth = 0x46, kCfgDay = 0x47, kCfgNick = 0x48, kCfgMsg = 0x5E;
constexpr u32 kCfgMinSize = kCfgData + 0x128;

u32 user_base(const std::vector<u8>& fw) {
  if (fw.size() < 0x200) return 0;
  const u32 base = static_cast<u32>(rd16(fw.data() + 0x20)) << 3;
  return base == 0 || base + 2 * kBlock > fw.size() ? 0 : base;
}

// The live copy: the greater counter in the 0..0x7F ring (0 beats 0x7F).
int live_block(const u8* base) {
  const u16 a = rd16(base + kFwCounter) & 0x7F, b = rd16(base + kBlock + kFwCounter) & 0x7F;
  if (a == b) return 0;
  return ((b - a) & 0x7F) < 0x40 ? 1 : 0;
}

bool has_ext(const u8* u) { return u[kFwExt] == 0x01; }

}  // namespace

bool OwnerFields::operator==(const OwnerFields& o) const {
  return nickname_len == o.nickname_len && message_len == o.message_len &&
         std::memcmp(nickname, o.nickname, nickname_len * 2) == 0 && std::memcmp(message, o.message, message_len * 2) == 0 &&
         colour == o.colour && birthday_month == o.birthday_month && birthday_day == o.birthday_day && language == o.language;
}

u32 owner_fingerprint(const OwnerFields& f) {
  u32 h = 2166136261u;
  auto add = [&](u32 v) { h = (h ^ v) * 16777619u; };
  add(f.nickname_len); for (u32 i = 0; i < f.nickname_len; ++i) add(f.nickname[i]);
  add(f.message_len);  for (u32 i = 0; i < f.message_len; ++i) add(f.message[i]);
  add(f.colour); add(f.birthday_month); add(f.birthday_day); add(f.language);
  return h ? h : 1;
}

bool firmware_owner(const std::vector<u8>& fw, OwnerFields& out) {
  const u32 base = user_base(fw);
  if (!base) return false;
  const u8* u = fw.data() + base + static_cast<u32>(live_block(fw.data() + base)) * kBlock;
  out = OwnerFields{};
  out.nickname_len = static_cast<u8>(rd16(u + kFwNickLen) < 10 ? rd16(u + kFwNickLen) : 10);
  for (u32 i = 0; i < out.nickname_len; ++i) out.nickname[i] = rd16(u + kFwNick + 2 * i);
  out.message_len = static_cast<u8>(rd16(u + kFwMsgLen) < 26 ? rd16(u + kFwMsgLen) : 26);
  for (u32 i = 0; i < out.message_len; ++i) out.message[i] = rd16(u + kFwMsg + 2 * i);
  out.colour = u[kFwColour] & 15;
  out.birthday_month = u[kFwMonth];
  out.birthday_day = u[kFwDay];
  // The DSi's extended block holds the full language (Chinese, Korean); the DS field only 0-5.
  out.language = has_ext(u) ? static_cast<u8>(u[kFwExtLang] & 7) : static_cast<u8>(rd16(u + kFwLang) & 7);
  return true;
}

bool set_firmware_owner(std::vector<u8>& fw, const OwnerFields& in, std::vector<u32>* touched) {
  const u32 base = user_base(fw);
  if (!base) return false;
  const u16 counter = static_cast<u16>((rd16(fw.data() + base + static_cast<u32>(live_block(fw.data() + base)) * kBlock + kFwCounter) + 1) & 0x7F);
  const std::vector<u8> live(fw.begin() + base + live_block(fw.data() + base) * kBlock, fw.begin() + base + (live_block(fw.data() + base) + 1) * kBlock);
  for (u32 blk = 0; blk < 2; ++blk) {
    u8* u = fw.data() + base + blk * kBlock;
    std::memcpy(u, live.data(), kBlock);   // both copies from the live one, as NDS::write_user_settings does
    for (u32 i = 0; i < 10; ++i) wr16(u + kFwNick + 2 * i, i < in.nickname_len ? in.nickname[i] : 0);
    wr16(u + kFwNickLen, in.nickname_len);
    for (u32 i = 0; i < 26; ++i) wr16(u + kFwMsg + 2 * i, i < in.message_len ? in.message[i] : 0);
    wr16(u + kFwMsgLen, in.message_len);
    u[kFwColour] = static_cast<u8>((u[kFwColour] & 0xF0) | (in.colour & 15));
    u[kFwMonth] = in.birthday_month;
    u[kFwDay] = in.birthday_day;
    wr16(u + kFwLang, static_cast<u16>((rd16(u + kFwLang) & ~7) | (in.language & 7)));   // backlight and boot bits share it
    wr16(u + kFwCounter, counter);
    wr16(u + kFwCrc, bios::crc16(u, 0x70, 0xFFFF));
    if (has_ext(u)) {
      if (rd16(u + kFwExtMask) >> (in.language & 7) & 1) u[kFwExtLang] = static_cast<u8>(in.language & 7);
      wr16(u + kFwExtCrc, bios::crc16(u + kFwExt, 0x8A, 0xFFFF));
    }
    if (touched) for (u32 off = 0; off < kBlock; off += kPage) touched->push_back(base + blk * kBlock + off);
  }
  return true;
}

bool twlcfg_owner(const std::vector<u8>& file, OwnerFields& out) {
  if (file.size() < kCfgMinSize) return false;
  const u8* d = file.data() + kCfgData;
  out = OwnerFields{};
  while (out.nickname_len < 10 && rd16(d + kCfgNick + 2 * out.nickname_len)) { out.nickname[out.nickname_len] = rd16(d + kCfgNick + 2 * out.nickname_len); ++out.nickname_len; }
  while (out.message_len < 26 && rd16(d + kCfgMsg + 2 * out.message_len)) { out.message[out.message_len] = rd16(d + kCfgMsg + 2 * out.message_len); ++out.message_len; }
  out.colour = d[kCfgColour] & 15;
  out.birthday_month = d[kCfgMonth];
  out.birthday_day = d[kCfgDay];
  out.language = d[kCfgLang] & 7;
  return true;
}

bool set_twlcfg_owner(std::vector<u8>& file, const OwnerFields& in, u8 counter, bool write_language) {
  if (file.size() < kCfgMinSize) return false;
  u8* d = file.data() + kCfgData;
  for (u32 i = 0; i < 11; ++i) wr16(d + kCfgNick + 2 * i, i < in.nickname_len ? in.nickname[i] : 0);   // 11th: the terminator
  for (u32 i = 0; i < 27; ++i) wr16(d + kCfgMsg + 2 * i, i < in.message_len ? in.message[i] : 0);
  d[kCfgColour] = static_cast<u8>(in.colour & 15);
  d[kCfgMonth] = in.birthday_month;
  d[kCfgDay] = in.birthday_day;
  if (write_language) d[kCfgLang] = static_cast<u8>(in.language & 7);
  file[kCfgCounter] = counter;
  u32 len = rd32(file.data() + kCfgLen);
  if (len == 0 || kCfgData + len > file.size()) len = 0x128;
  crypto::sha1(d, len, file.data());
  return true;
}

OwnerSyncResult sync_owner(NandImage& nand, const u8* bios7i, std::vector<u8>& fw,
                           u32 fw_stamp, u32 nand_stamp, bool fw_customised) {
  OwnerSyncResult r;
  NandFs nfs;
  std::string why;
  if (!nfs.mount(nand, bios7i, &why)) { r.notes.push_back("the NAND cannot be read: " + why); return r; }
  FatVolume& vol = nfs.main();
  std::vector<u8> c[2];
  bool have[2];
  for (int i = 0; i < 2; ++i) {
    FatVolume::Entry e;
    have[i] = vol.lookup(i ? "/shared1/TWLCFG1.dat" : "/shared1/TWLCFG0.dat", e) && !e.dir() && vol.read(e, c[i]) && c[i].size() >= kCfgMinSize;
  }
  if (!have[0] && !have[1]) { r.notes.push_back("the NAND has no TWLCFG"); return r; }
  const int newer = (have[1] && (!have[0] || c[1][kCfgCounter] > c[0][kCfgCounter])) ? 1 : 0;   // as the launcher picks
  OwnerFields f, t;
  if (!firmware_owner(fw, f)) { r.notes.push_back("the firmware has no user settings"); return r; }
  twlcfg_owner(c[newer], t);
  const u32 hf = owner_fingerprint(f), ht = owner_fingerprint(t);
  r.fw_stamp = hf; r.nand_stamp = ht;
  if (f == t) { r.what = OwnerSync::Same; return r; }

  const bool fw_moved = fw_stamp == 0 || hf != fw_stamp, nand_moved = nand_stamp == 0 || ht != nand_stamp;
  bool to_nand;
  if (fw_moved && !nand_moved) to_nand = true;
  else if (nand_moved && !fw_moved) to_nand = false;
  else if (!fw_moved && !nand_moved) { r.what = OwnerSync::Same; return r; }   // apart since the last sync (a language the region lacks): leave them
  else to_nand = fw_customised;

  if (!to_nand) {
    if (!set_firmware_owner(fw, t, &r.fw_touched)) { r.notes.push_back("the firmware's user settings cannot be written"); return r; }
    OwnerFields now;
    firmware_owner(fw, now);
    r.fw_stamp = owner_fingerprint(now);
    r.what = OwnerSync::ToFirmware;
    return r;
  }

  // A language the console's region does not offer would leave the DSi Menu
  // without one it can show; TWLCFG keeps its own then.
  bool lang_ok = true;
  {
    FatVolume::Entry e;
    std::vector<u8> s;
    if (vol.lookup("/sys/HWINFO_S.dat", e) && vol.read(e, s) && s.size() >= kCfgData + 4)
      lang_ok = rd32(s.data() + kCfgData) >> (f.language & 7) & 1;
  }
  if (!lang_ok) r.notes.push_back("language " + std::to_string(f.language) + " is not one this console's region offers; the DSi Menu keeps its own");
  std::vector<u8> out = c[newer];
  set_twlcfg_owner(out, f, static_cast<u8>(c[newer][kCfgCounter] + 1), lang_ok);
  // Both copies the same, as melonDS's NAND editor writes them: whichever the launcher picks agrees.
  for (int i = 0; i < 2; ++i) {
    if (!vol.write(i ? "/shared1/TWLCFG1.dat" : "/shared1/TWLCFG0.dat", out.data(), static_cast<u32>(out.size()), &why)) {
      r.notes.push_back("TWLCFG cannot be written: " + why);
      r.what = OwnerSync::Unavailable;
      return r;
    }
  }
  OwnerFields now;
  twlcfg_owner(out, now);
  r.nand_stamp = owner_fingerprint(now);
  r.what = OwnerSync::ToNand;
  return r;
}

}  // namespace ds::io
