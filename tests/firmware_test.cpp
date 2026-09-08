// SPDX-License-Identifier: GPL-3.0-or-later
// The console's own settings inside a firmware image: reading them back, and
// editing one field without disturbing the others or the checksums.
#include "core/nds.h"
#include "core/bios/freebios.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using ds::u8;
using ds::u16;
using ds::u32;

const char* kFw = "firmware_test.bin";
const char* kOvr = "firmware_test.bin.ovr";

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

// A generated image written to disk, so it loads as a dump rather than as the
// synthetic firmware: the dirty-page tracking the sidecar needs is only kept
// for a real one.
void write_dump(const ds::bios::UserSettings& u) {
  const std::vector<u8> fw = ds::bios::generate_firmware(u);
  std::ofstream f(kFw, std::ios::binary);
  f.write(reinterpret_cast<const char*>(fw.data()), static_cast<std::streamsize>(fw.size()));
}

void load(ds::NDS& nds) {
  std::string err;
  CHECK(nds.load_bios("", "", kFw, {}, &err));
  CHECK(!nds.firmware_synthetic);
}

// Both copies of the settings, checked the way the console does: the CRC16
// over the first 0x70 bytes has to match the halfword at 0x72, or the block
// is rejected and the settings silently revert.
void check_crcs(const ds::NDS& nds) {
  const u32 base = nds.user_settings_offset();
  CHECK(base != 0);
  for (u32 blk = 0; blk < 2; ++blk) {
    const u8* u = nds.firmware.data() + base + blk * 0x100;
    CHECK(rd16(u + 0x72) == ds::bios::crc16(u, 0x70, 0xFFFF));
  }
}

void test_round_trip() {
  ds::bios::UserSettings in;
  in.nickname = "Ada";
  in.message = "Hello there";
  in.favourite_colour = 11;
  in.birthday_month = 7;
  in.birthday_day = 5;
  in.language = 3;
  write_dump(in);
  ds::NDS nds;
  load(nds);
  ds::bios::UserSettings out;
  CHECK(nds.read_user_settings(out));
  CHECK(out.nickname == "Ada");
  CHECK(out.message == "Hello there");
  CHECK(out.favourite_colour == 11);
  CHECK(out.birthday_month == 7);
  CHECK(out.birthday_day == 5);
  CHECK(out.language == 3);
  check_crcs(nds);
}

// Changing one field must leave the rest exactly as they were: the menu edits
// one at a time, and a dump's nickname can hold characters these strings
// cannot carry, so rewriting all of them would corrupt one the player never
// touched.
void test_one_field_at_a_time() {
  ds::bios::UserSettings in;
  in.nickname = "Setya";
  in.message = "Keep me";
  in.favourite_colour = 0;
  in.birthday_month = 7;
  in.birthday_day = 5;
  in.language = 1;
  write_dump(in);
  ds::NDS nds;
  load(nds);

  ds::bios::UserSettings u;
  CHECK(nds.read_user_settings(u));
  u.favourite_colour = 2;
  CHECK(nds.write_user_settings(ds::NDS::UserField::Colour, u));

  ds::bios::UserSettings after;
  CHECK(nds.read_user_settings(after));
  CHECK(after.favourite_colour == 2);
  CHECK(after.nickname == "Setya");      // untouched, and still its own case
  CHECK(after.message == "Keep me");
  CHECK(after.birthday_month == 7);
  CHECK(after.birthday_day == 5);
  CHECK(after.language == 1);
  check_crcs(nds);

  // A string field, including one that gets shorter: the length halfword has
  // to follow it, or the console reads the tail of the old name after the new.
  u = after;
  u.nickname = "Bo";
  CHECK(nds.write_user_settings(ds::NDS::UserField::Nickname, u));
  CHECK(nds.read_user_settings(after));
  CHECK(after.nickname == "Bo");
  CHECK(after.message == "Keep me");
  CHECK(after.favourite_colour == 2);
  check_crcs(nds);

  // Longer than the firmware keeps: cut, not overflowed into the next field.
  u = after;
  u.nickname = "ABCDEFGHIJKLMNOP";
  CHECK(nds.write_user_settings(ds::NDS::UserField::Nickname, u));
  CHECK(nds.read_user_settings(after));
  CHECK(after.nickname == "ABCDEFGHIJ");   // ten characters
  CHECK(after.message == "Keep me");
  check_crcs(nds);

  // The language shares its halfword with the backlight bits, which are the
  // console's and must survive.
  const u32 base = nds.user_settings_offset();
  const u16 before_bits = static_cast<u16>(rd16(nds.firmware.data() + base + 0x64) & ~7);
  u = after;
  u.language = 5;
  CHECK(nds.write_user_settings(ds::NDS::UserField::Language, u));
  CHECK(nds.read_user_settings(after));
  CHECK(after.language == 5);
  CHECK((rd16(nds.firmware.data() + base + 0x64) & ~7) == before_bits);
  check_crcs(nds);
}

// An edit has to reach the sidecar and come back, because that is the only
// place it is kept: the dump itself is never written.
void test_sidecar_round_trip() {
  ds::bios::UserSettings in;
  in.nickname = "Setya";
  in.favourite_colour = 0;
  write_dump(in);
  std::remove(kOvr);

  std::vector<u8> dump_before;
  {
    std::ifstream f(kFw, std::ios::binary);
    dump_before.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }

  {
    ds::NDS nds;
    load(nds);
    ds::bios::UserSettings u;
    CHECK(nds.read_user_settings(u));
    u.favourite_colour = 9;
    CHECK(nds.write_user_settings(ds::NDS::UserField::Colour, u));
    CHECK(nds.firmware_override_dirty());
    std::string err;
    CHECK(nds.save_firmware_override(kOvr, err));
  }

  // The dump is untouched: that is the whole point of the sidecar.
  std::vector<u8> dump_after;
  {
    std::ifstream f(kFw, std::ios::binary);
    dump_after.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  CHECK(dump_before == dump_after);

  // A fresh console with the sidecar sees the change.
  {
    ds::NDS nds;
    load(nds);
    ds::bios::UserSettings fresh;
    CHECK(nds.read_user_settings(fresh));
    CHECK(fresh.favourite_colour == 0);      // not until the sidecar is applied
    std::string err;
    CHECK(nds.load_firmware_override(kOvr, err));
    CHECK(nds.read_user_settings(fresh));
    CHECK(fresh.favourite_colour == 9);
    CHECK(fresh.nickname == "Setya");
    check_crcs(nds);
  }
}

} // namespace

int main() {
  test_round_trip();
  test_one_field_at_a_time();
  test_sidecar_round_trip();
  std::remove(kFw);
  std::remove(kOvr);
  std::printf("firmware: ok\n");
  return 0;
}
