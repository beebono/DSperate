// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One DSiWare title put into the session's NAND (docs/dsiware-scoping.md 2.3):
// at most one per boot, and only when its title ID is not already installed.
// What goes in is what melonDS's NANDMount::ImportTitle writes -- a ticket,
// the title and content directories, the save files formatted the way the SDK
// expects, title.tmd and the .app -- into NandImage's memory, never the dump.
//
// The TMD has to be the title's real one (see check_signed_tmd).
#pragma once
#include "core/types.h"

#include <functional>
#include <string>
#include <vector>

namespace ds::io {

class NandImage;
class FatVolume;

// What each DSiWare title on the NAND occupies (every file under its title
// directory, rounded to clusters): the measure the launcher's quota is kept in.
struct DsiWareUsage { std::string id; std::string code; u64 bytes = 0; };   // id "4b533345", code "KS3E"
std::vector<DsiWareUsage> dsiware_usage(const FatVolume& vol);

// Take a DSiWare title out of the session: its title directory and ticket.
// The dump is untouched, so it is back at the next boot.
bool hide_dsiware_title(FatVolume& vol, const std::string& id, std::string* err);

// Hide every DSiWare title the dump has installed (the frontends' option).
// Returns how many; call before nand_install_title and nand_import.
int nand_hide_installed_dsiware(NandImage& nand, const u8* bios7i, std::string* err);

// A DSiWare SRL from a .nds/.app/.srl, or the one content of a CIA whose content
// is not title-key encrypted (an encrypted one is refused: decrypting it needs
// a console key DSperate does not carry). False, with the reason, for anything
// that is not DSiWare (unit code bit 1 and title ID high 00030004).
// When the CIA carries a DSi-signed TMD (not the usual 3DS one), it is put in
// `embedded_tmd`; otherwise that is left empty.
bool read_dsiware(const std::string& path, std::vector<u8>& srl, std::string* err, std::vector<u8>* embedded_tmd = nullptr);

// The launcher checks the TMD's RSA signature when it starts a title (not when
// it lists one): a TMD made up here lists the title and fails to launch it. So
// an install needs the title's real, Nintendo-signed DSi TMD. This accepts one
// that is signed for the DSi, names this title, and describes exactly this SRL
// (size and SHA-1).
bool check_signed_tmd(const std::vector<u8>& tmd, const std::vector<u8>& srl, std::string* err);

// Where that TMD comes from, in order: embedded in the CIA; `cache_path`; the
// Nintendo update CDN through `fetch` (stored to `cache_path` when it works);
// `beside_rom`, a user-supplied file. Empty when none is usable; `log` says
// what was tried. `fetch` may be empty (no network client).
using TmdFetch = std::function<bool(const std::string& url, std::vector<u8>& body)>;
std::string nus_tmd_url(u32 title_lo);
std::vector<u8> find_signed_tmd(const std::vector<u8>& srl, const std::vector<u8>& embedded, const std::string& cache_path,
                                const std::string& beside_rom, const TmdFetch& fetch, std::vector<std::string>& log);

// Whether the NAND (as the session sees it) already has this DSiWare title
// installed: then it needs no install, and no TMD.
bool nand_has_title(NandImage& nand, const u8* bios7i, u32 title_lo);

// Whether a file on disk holds DSiWare, cheaply enough to ask of every file in
// a game list: an .nds/.dsi/.srl by its header (a DSi unit code and title ID
// high 00030004), a .cia by its extension alone (read_dsiware checks it
// properly when it is opened).
bool file_is_dsiware(const std::string& path);
// The content ID an installed title's .app is named by (its CONTENT/xxxxxxxx.APP),
// which the launcher hands the title as the path to its own image.
bool nand_title_content_id(NandImage& nand, const u8* bios7i, u32 title_lo, u32& content_id);

struct TitleInstall {
  enum class Result { Installed, AlreadyInstalled, Failed } result = Result::Failed;
  u32 title_lo = 0;          // e.g. 0x4B443945 ("KD9E")
  std::string message;       // why it failed, or what was done
  std::vector<std::string> hidden;   // the dump's titles hidden this session to fit the quota, by game code
};

// `bios7i` is required: the ticket is ES-encrypted with a key from it.
// `signed_tmd` must pass check_signed_tmd; it is installed as it is, and the
// .app is named by its content ID.
TitleInstall nand_install_title(NandImage& nand, const u8* bios7i, const std::vector<u8>& srl, const std::vector<u8>& signed_tmd);

// melonDS NANDMount::CreateSaveFile: an empty FAT12 volume `len` bytes long
// (geometry after NTM's sav.c). Empty for len 0; exposed for the tests.
std::vector<u8> make_dsi_save(u32 len);

}  // namespace ds::io
