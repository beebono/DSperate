// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's settings between a DSi firmware image and the NAND's TWLCFG
// (io/dsi_user_sync): whichever side moved since the last sync wins, both
// TWLCFG copies stay valid, UTF-16 survives, and a language the console's
// region lacks stays out of TWLCFG without the two sides flipping each boot.
#include "core/io/dsi_user_sync.h"
#include "core/io/dsi_nand_fs.h"
#include "core/io/dsi_nand_synth.h"
#include "core/io/dsi_sd.h"
#include "core/bios/freebios.h"
#include "core/crypto/sha1.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ds;
using ds::io::FatVolume;
using ds::io::OwnerFields;
using ds::io::OwnerSync;

namespace {

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

OwnerFields fields(const char* nick) {
  OwnerFields f;
  for (; nick[f.nickname_len]; ++f.nickname_len) f.nickname[f.nickname_len] = static_cast<u8>(nick[f.nickname_len]);
  return f;
}

std::vector<u8> twlcfg(io::NandImage& nand, int which) {
  io::NandFs fs;
  std::string err;
  CHECK(fs.mount(nand, nullptr, &err));
  FatVolume::Entry e;
  std::vector<u8> out;
  CHECK(fs.main().lookup(which ? "/shared1/TWLCFG1.dat" : "/shared1/TWLCFG0.dat", e) && fs.main().read(e, out));
  return out;
}

void check_twlcfg_valid(io::NandImage& nand) {
  const std::vector<u8> a = twlcfg(nand, 0), b = twlcfg(nand, 1);
  CHECK(a == b);
  u8 digest[20];
  crypto::sha1(&a[0x88], 0x128, digest);
  CHECK(std::memcmp(digest, a.data(), 20) == 0);
}

void check_fw_valid(const std::vector<u8>& fw) {
  const u32 base = static_cast<u32>(rd16(fw.data() + 0x20)) << 3;
  for (u32 blk = 0; blk < 2; ++blk) {
    const u8* u = fw.data() + base + blk * 0x100;
    CHECK(rd16(u + 0x72) == bios::crc16(u, 0x70, 0xFFFF));
    CHECK(rd16(u + 0xFE) == bios::crc16(u + 0x74, 0x8A, 0xFFFF));
  }
}

void build(io::NandImage& nand, const char* nick) {
  std::vector<u8> srl(0x6000);
  const u32 lo = 0x4B535445, hi = 0x00030004;
  std::memcpy(&srl[0x230], &lo, 4); std::memcpy(&srl[0x234], &hi, 4);
  bios::UserSettings user;
  user.nickname = nick;
  const io::DsiRegion region = io::dsi_region_for(0x00000002, 1);   // USA: en fr es
  io::DsiConsoleFiles files = io::make_dsi_console_files(user, region, io::kSynthConsoleId);
  files.font = std::vector<u8>(0x300, 0xAB);
  std::string err;
  CHECK(io::build_synthetic_nand(nand, nullptr, srl, files, &err));
}

}  // namespace

int main() {
  bios::UserSettings fw_user;
  fw_user.nickname = "Tester";
  io::NandImage nand;
  build(nand, "Tester");
  std::vector<u8> fw = bios::generate_firmware_dsi(fw_user, 1, 0x26);

  // Agreeing sides: nothing written, both fingerprints handed back.
  io::OwnerSyncResult r = io::sync_owner(nand, nullptr, fw, 0, 0, false);
  CHECK(r.what == OwnerSync::Same && r.fw_stamp && r.nand_stamp && r.fw_stamp == r.nand_stamp);
  u32 sf = r.fw_stamp, sn = r.nand_stamp;

  // Changed in the DS firmware's menu (firmware moved, NAND did not): into TWLCFG, UTF-16 and all.
  OwnerFields f = fields("Shared");
  f.nickname[5] = 0x00E9;                 // "Sharee" with an e-acute
  f.message_len = 2; f.message[0] = 'H'; f.message[1] = 'i';
  f.colour = 4; f.birthday_month = 12; f.birthday_day = 24; f.language = 2;
  CHECK(io::set_firmware_owner(fw, f, nullptr));
  check_fw_valid(fw);
  const u8 counter_before = twlcfg(nand, 0)[0x81];
  r = io::sync_owner(nand, nullptr, fw, sf, sn, true);
  CHECK(r.what == OwnerSync::ToNand);
  check_twlcfg_valid(nand);
  OwnerFields t;
  CHECK(io::twlcfg_owner(twlcfg(nand, 0), t));
  CHECK(t == f);
  CHECK(twlcfg(nand, 0)[0x81] == static_cast<u8>(counter_before + 1));
  sf = r.fw_stamp; sn = r.nand_stamp;
  CHECK(io::sync_owner(nand, nullptr, fw, sf, sn, true).what == OwnerSync::Same);

  // Changed in DSi System Settings without the flash (NAND moved): into the firmware.
  {
    std::vector<u8> file = twlcfg(nand, 0);
    CHECK(io::set_twlcfg_owner(file, fields("DsiSide"), static_cast<u8>(file[0x81] + 1), true));
    io::NandFs fs;
    std::string err;
    CHECK(fs.mount(nand, nullptr, &err));
    CHECK(fs.main().write("/shared1/TWLCFG1.dat", file.data(), static_cast<u32>(file.size()), &err));   // the newer copy
  }
  r = io::sync_owner(nand, nullptr, fw, sf, sn, true);
  CHECK(r.what == OwnerSync::ToFirmware);
  CHECK(!r.fw_touched.empty());
  check_fw_valid(fw);
  OwnerFields now;
  CHECK(io::firmware_owner(fw, now));
  CHECK(now == fields("DsiSide"));
  sf = r.fw_stamp; sn = r.nand_stamp;
  CHECK(io::sync_owner(nand, nullptr, fw, sf, sn, true).what == OwnerSync::Same);

  // Japanese on this image: its own language mask (a USA DSi's) lacks it, so
  // only the DS field takes it and the DSi side has nothing new.
  OwnerFields jp = now;
  jp.language = 0;
  CHECK(io::set_firmware_owner(fw, jp, nullptr));
  CHECK(io::firmware_owner(fw, now) && now.language != 0);
  CHECK(io::sync_owner(nand, nullptr, fw, sf, sn, true).what == OwnerSync::Same);

  // An image whose mask has it, on a USA console's NAND: TWLCFG keeps its own
  // language, and the next boot leaves the pair alone rather than flipping.
  {
    std::vector<u8> wide = bios::generate_firmware_dsi(fw_user, 1, 0x3F);
    r = io::sync_owner(nand, nullptr, wide, 0, 0, false);   // first: the NAND's settings into it
    CHECK(r.what == OwnerSync::ToFirmware);
    u32 wf = r.fw_stamp, wn = r.nand_stamp;
    CHECK(io::firmware_owner(wide, now));
    now.language = 0;
    CHECK(io::set_firmware_owner(wide, now, nullptr));
    r = io::sync_owner(nand, nullptr, wide, wf, wn, true);
    CHECK(r.what == OwnerSync::ToNand && !r.notes.empty());
    CHECK(io::twlcfg_owner(twlcfg(nand, 0), t) && t.language == 1);
    wf = r.fw_stamp; wn = r.nand_stamp;
    r = io::sync_owner(nand, nullptr, wide, wf, wn, true);
    CHECK(r.what == OwnerSync::Same && r.fw_touched.empty());
    CHECK(io::firmware_owner(wide, now) && now.language == 0);
  }

  // First sync ever, sides apart: the firmware wins only if it was changed in DSperate.
  {
    io::NandImage n2;
    build(n2, "NandOwner");
    std::vector<u8> fw2 = bios::generate_firmware_dsi(fw_user, 1, 0x26);
    CHECK(io::sync_owner(n2, nullptr, fw2, 0, 0, false).what == OwnerSync::ToFirmware);
    CHECK(io::firmware_owner(fw2, now) && now == fields("NandOwner"));
    io::NandImage n3;
    build(n3, "NandOwner");
    std::vector<u8> fw3 = bios::generate_firmware_dsi(fw_user, 1, 0x26);
    CHECK(io::sync_owner(n3, nullptr, fw3, 0, 0, true).what == OwnerSync::ToNand);
    CHECK(io::twlcfg_owner(twlcfg(n3, 1), t) && t == fields("Tester"));
  }
  std::printf("dsi_user_sync: ok\n");
}
