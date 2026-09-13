// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What survives a DSi session (docs/dsiware-scoping.md 2.3). The dump is never
// written: the session's writes live in NandImage's memory, and what should
// outlast it is carried out as files and put back in before the next boot.
//
//   title saves   /title/00030004/<id>/data/{public,private,banner}.sav
//                 -> <saves>/<GAMECODE>.pub/.prv/.bnr, melonDS TitleManager's
//                 export format, for every DSiWare title on the NAND
//   system files  any other file on partition 0 the session wrote (TWLCFG,
//                 the system apps' own data) -> one sidecar beside the dump,
//                 as the DS firmware's settings go to firmware.bin.ovr
//   photos        files the session wrote on the photo partition -> plain
//                 files under a folder, JPEGs included, readable as they are
//
// Left out: /tmp and /import (the system's scratch space), and the title
// contents and tickets a virtual install puts in.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

struct NandPersistPaths {
  std::string saves_dir;     // <GAMECODE>.pub/.prv/.bnr
  std::string sidecar;       // the system files
  std::string photos_dir;    // the photo partition's files

  // The defaults: saves in `saves_dir` if given, else beside the dump;
  // "<nand>.ovr" and "<nand>.photos" beside the dump.
  static NandPersistPaths beside(const std::string& nand_path, const std::string& saves_dir = {});
};

struct NandPersistReport {
  int saves = 0, system_files = 0, photos = 0;   // files moved
  std::vector<std::string> notes;                // skipped files and why, one line each
};

// Put saved files into the session. Call after the NAND is loaded and before
// the boot that reads it. `bios7i` may be null (tickets are not touched here).
NandPersistReport nand_import(NandImage& nand, const u8* bios7i, const NandPersistPaths& paths);

// Carry out what the session changed. Cheap when nothing was written; only
// files whose sectors the session wrote are read, and a host file is only
// rewritten when its contents differ.
NandPersistReport nand_export(NandImage& nand, const u8* bios7i, const NandPersistPaths& paths);

}  // namespace ds::io
