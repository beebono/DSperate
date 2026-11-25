// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The console owner's settings live twice on a DSi: in the firmware flash's
// user block (what DS titles read, the same block a DS has) and in the NAND's
// /shared1/TWLCFG0.dat and TWLCFG1.dat (what the DSi Menu and DSiWare read).
// sync_owner() carries a change made on one side over to the other, so a
// nickname set in the DS firmware's menu shows on the DSi Menu and one set in
// DSi System Settings reaches DS mode.
//
// Which side changed is told by a fingerprint of each side as the last sync
// left it (kept by the caller, in the firmware override): the side whose
// fingerprint moved is the newer. With no fingerprints yet, or both moved,
// `fw_customised` decides: the firmware wins when its settings were changed
// in DSperate (the override carries its user block), the NAND otherwise.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

struct OwnerFields {
  u16 nickname[10] = {};   // UTF-16, `nickname_len` units used
  u8  nickname_len = 0;
  u16 message[26] = {};
  u8  message_len = 0;
  u8  colour = 0, birthday_month = 1, birthday_day = 1, language = 1;
  bool operator==(const OwnerFields& o) const;
  bool operator!=(const OwnerFields& o) const { return !(*this == o); }
};

// Never 0, which callers use for "no fingerprint yet".
u32 owner_fingerprint(const OwnerFields& f);

// A firmware image's live user block. False when the image has none.
bool firmware_owner(const std::vector<u8>& fw, OwnerFields& out);
// Writes both copies of the user block (counter + 1, CRCs), keeping every
// other byte; `touched` gets the offsets of the 256-byte pages written. The
// DS-compatible language field takes `language`, and so does the DSi's
// extended block where the image has one and its language mask allows it.
bool set_firmware_owner(std::vector<u8>& fw, const OwnerFields& in, std::vector<u32>* touched);

// TWLCFG file contents (header at 0, data at 0x88).
bool twlcfg_owner(const std::vector<u8>& file, OwnerFields& out);
// The fields into the data, the update counter to `counter`, a fresh SHA-1.
// `language` is written only when `write_language`.
bool set_twlcfg_owner(std::vector<u8>& file, const OwnerFields& in, u8 counter, bool write_language);

enum class OwnerSync : u8 {
  Same,          // both sides already agree
  ToNand,        // the firmware's settings went into TWLCFG
  ToFirmware,    // TWLCFG's went into the firmware's user block
  Unavailable,   // a side could not be read or written; see notes
};

struct OwnerSyncResult {
  OwnerSync what = OwnerSync::Unavailable;
  u32 fw_stamp = 0, nand_stamp = 0;     // fingerprints to keep for the next sync
  std::vector<u32> fw_touched;          // firmware pages written (ToFirmware)
  std::vector<std::string> notes;
};

// `fw_stamp`/`nand_stamp`: the fingerprints the last sync returned (0: none).
OwnerSyncResult sync_owner(NandImage& nand, const u8* bios7i, std::vector<u8>& fw,
                           u32 fw_stamp, u32 nand_stamp, bool fw_customised);

}  // namespace ds::io
