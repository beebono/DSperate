# DSiWare support -- scope and status

Rewritten 2026-09-12 at branch `dsiware` @ `e0b4f79` (docs `f245cf9`); status
brought up to date 2026-09-13 at `d582f79`. This replaces the 2026-09-10 plan,
whose central design (direct boot + a synthesised NAND, no dump) did not
survive; section 3 says what was wrong.

**Where we are (2026-09-13).** Both launch routes work on the interpreter:
the real-NAND boot (DSi Menu, TLNC auto-launch, one injected title, save and
settings persistence) and the no-NAND launcher hand-off HLE, which starts all
15 oracle titles on the DSi BIOS pair alone with DSperate's own system font.
The SDL frontend runs DSiWare named on the command line or picked from the
loader's list, and the loader cart works on the real DSi Menu. The remaining
2.0.0 work is save states with the NAND and the JIT and idle skip under DSi
on the device (section 5). Since then the SD card slot (5.4), the microphone
(5.5), DSi Wi-Fi networking (5.6) and save states under DSi (2.5) have
landed. `dsiware` is 68 commits ahead of `main` and
contains all of it (including the Wi-Fi/slirp work); `main` has nothing
`dsiware` lacks.
References: melonDS `dsperate-research/melonDS` @ `d3cd6164` (the exactness
oracle) and melonDS DS `dsperate-research/melonds-ds-libretro` (how a
shipping frontend launches DSiWare).

## 1. Scope

The target is DSperate **2.0.0**.

| feature | 2.0.0 | notes |
|---------|-------|-------|
| DSiWare, launched from a real `nand.bin` | **in** -- done | NAND boot + TLNC auto-launch (section 2.1) |
| Booting the DSi menu itself | **in** -- done | same NAND boot with no autoload |
| No-NAND fallback: the launcher hand-off in HLE | **in** -- done | `--dsi-hle-launch` (2.2): synthesised NAND, generated settings and firmware, own system font; Unlaunch cannot be the fallback |
| Title install from `.nds`/`.app`/`.cia` | **in** -- done | at most one virtual title per boot, injected into the read-only dump's in-memory session (2.3) |
| Save export/import (`.pub`/`.prv`/`.bnr`) | **in** -- done | melonDS TitleManager extensions; not yet cross-checked by importing into melonDS |
| Microphone (I2S `MICCNT`/`MICDATA`) | **in** -- done | melonDS DSi_I2S, fed by the existing SDL/ALSA capture (5.5) |
| DSi Wi-Fi networking | **in** -- done | both chips reach slirp; the firmware carries DSperate's access point (5.6) |
| SD card slot (SD host port 0) | **in** -- done | a host folder (`--dsi-sd`, `paths.dsi_sd`), built into a card in memory and synced back (5.4) |
| Frontend | **in** -- done | CLI, loader list, loader cart on the DSi Menu, ini keys for the NAND and DSi firmware, a row for hiding the NAND's titles; `.app` streaming after 2.0.0 (2.4) |
| Save states under DSi | **in** -- done | NAND sectors past the state base, the SD card's in-memory part checked against its folder (2.5) |
| JIT + idle skip under DSi, on device | **in** -- not started | SDL forces the interpreter and lockstep; **slower than DS titles is accepted for 2.0.0** |
| DSP HLE (G.711, graphics) | after 2.0.0 | one feature commit, together with camera images passed from the CLI. USER DECISION (2026-09-13): no boot-only stub for 2.0.0; a title that starts the DSP gets a warning that it will likely not work (`NDS::dsi_dsp_started`, SDL toast) |
| Camera image source | after 2.0.0 | the cameras exist as hardware; frames are black until then |
| DSi-enhanced retail carts in DSi mode | after 2.0.0 | they run in DS mode, as today |
| DSP LLE (Teakra), AAC ucode | undecided | only if a wanted title needs an unlisted ucode |
| Second card slot, DSi Shop | out | nothing needs them |

## 2. Design

### 2.1 Boot the console from its NAND; do not direct-boot DSiWare

`NDS::boot_dsi_nand()` (`--dsi-nand-boot`) boots the way the console boots
itself. The DSi BIOS dumps we have are the common low-32 KB half dumps, so the
boot ROM's boot2 loader is missing. We do its job by hand, as melonDS's
`LoadNAND` `!FullBIOSBoot` branch does: boot info from raw NAND offset 0x220,
MBK/NWRAM mapping from 0x380, boot2 AES-CTR under the fixed key (IV from the
size, byte-reversed blocks), the eMMC CID block at 0x03FFE6E4, the BIOS
fragments the missing halves would leave (ARM9 ITCM 0x4400/0x4800/0x4894/
0x58DC, ARM7 0x3C00 bytes at 0x03FFC400), then a jump into boot2 with the
first-instruction pipeline charge as `defer_cost` 64/4.

A DSiWare launch is this boot plus melonDS DS's **`TLNC` autoload block**:
0x100 bytes at 0x02000300 ("TLNC", 01h, length 18h, CRC16 over 0x18 bytes
from +8 seeded 0xFFFF, prev title 0, new title = this title, flags
`0x01 | 3<<1 | 1<<4`) plus the BPTWL boot flag (register 0x70). The title must
be installed in the NAND the console boots. Booting to the DSi menu is the
same boot without the block.

Why not direct boot:
- melonDS's DSi direct boot (and ours, which is trace-identical to it) runs
  DSiWare in **card mode** (`0x02FFFC40 = 1`, SDMMC gate closed). Titles never
  touch the NAND, so there are no saves; Plants vs Zombies refuses to run.
- The first reverse-engineered launcher hand-off (indicator 3, `SCFG_EXT7`
  bit 18, the mount table at header word 0x1D4, 8 bytes at 0x0380FFC4) got
  the title onto the NAND, then wedged the ARM7 on the SDK's `b .` IRQ
  handler. It was incomplete; the captured hand-off in 2.2 replaces it and
  runs.
- TLNC on top of a direct boot is inert: only the boot ROM reads it.

Direct boot stays for two jobs: the phase-1 card-mode exactness gate
(section 5.1), and the launcher hand-off HLE (2.2), which is the no-NAND
fallback.

### 2.2 Required files, and the Unlaunch fallback

Required: the DS `bios9.bin`/`bios7.bin`, `bios9i`/`bios7i` (half dumps are
enough: their low-32 KB CRCs match melonDS's; full-64 KB CRCs do not), the
DSi firmware (`dsifirmware.bin`), and `nand.bin` (nocash footer, which supplies
the eMMC CID and the console ID). The console ID seeds AES key slots 1 and 3,
so it is set **before** `aes.reset()`. The user's `nand.bin` is never written:
it is opened read-only and the session's writes are held in memory (2.3).

`--dsi-boot <blob>` (0x154 bytes from `tools/dsi_nand.py bootblobs`: TWLCFG
0x128, HWINFO_N, HWINFO_S) is a **direct-boot** input only; a NAND boot reads
the console's own copies.

**Unlaunch is not a fallback.** `dsi-binary/unlaunch/unlaunch.dsi` is the
*Unlaunch DSi Installer* v2.0, a DSi homebrew app (unit code 3, ARM9 at
0x02200000). It is not a boot2 image. Unlaunch proper is written into the
launcher's `TITLE.TMD` and gains control while the NAND's own boot2 parses it,
so it needs a NAND as much as the stock menu does. `--dsi-boot2
unlaunch.dsi` still makes 2 NAND reads and stalls at `7e1b954`, as expected
for an app started with boot2's entry state. The installer stays useful as
an SD-card test title.

**No-NAND fallback: the launcher hand-off in HLE (`--dsi-hle-launch`).**
Direct boot stages exactly what the DSi Launcher leaves a title at its entry
point, instead of melonDS's card mode. The reference was captured, not
guessed. Real launches (NAND boot + TLNC autoload) of KS3E and KMGE were
snapshotted when each CPU reached its entry PC (headless
`DS_ENTRY_SNAP=<dir>:<pc9>:<pc7>`: RAM, device state, register summary) and
diffed against our direct boot. The gap was the same 18 RAM ranges for both
titles. `setup_direct_boot_dsi` now closes it under `dsi_hle_launch`:

- main RAM: header copies only at 0x02FFE000/0x02FFFE00; no chip ID;
  indicator 0x02FFFC40 = 3; 0x02FFFDFA = 0x81; two TWLCFG stretches cleared
  (0x02000407-0B, 0x02000420-2F, which held the last-launched title); the
  whole Wi-Fi MAC at 0x02FFFCF4 and channel mask 0x1041;
- ARM7 WRAM: the mount table at header 0x1D4 (nand, nand2, shared1, photo,
  dataPub) and the title's own image path, whose content ID is looked up on
  the NAND (`nand_title_content_id`); locked SCFG_EXT7 + `44 F8` at
  0x0380FFC4;
- I/O: SCFG_BIOS 0x0501, SCFG_CLK9 0x0084 (**the title starts at 67 MHz**),
  SCFG_EXT7 `(0x93FBFB06 | header 0x1B8)` locked, SCFG_MC empty slot,
  SNDEXCNT 0x800F, EXMEMCNT 0xE88C, RCNT 5, GPIO IE 0x40, BPTWL 0x12 = 3 and
  0x70 = 1, power-management register 0 = 0x0C, RTC power-lost flag clear;
  no cart slot setup;
- CPUs: System mode, IRQs masked, the launcher's stacks (ARM9 SYS 0x0E003F80,
  IRQ 0x0E003F7C, SVC 0x0E003FC0; ARM7 SYS 0x03FFFF80, IRQ 0x0380FF7C, SVC
  0x0380FFC0). CP15 already matched;
- modcrypt decrypts only up to each binary's size (the header's area size is
  rounded, and melonDS turns the bytes past the binary into keystream).

Left out on purpose:
- launcher leftovers: the loader's jump code at 0x023FEE00 and 0x0380F600,
  stack bytes at 0x0380FFB0, AES slots 0 and 2 overwritten with junk, NDMA,
  SD/SDIO host and DS Wi-Fi state;
- our normalised touch calibration at 0x02FFFCD8;
- two blocks not yet understood: `"00000009\0E"` at 0x02FFD7B0 and a
  launcher title-ID list at 0x02FFD800. The list is a subset of the menu's
  own data (`shared2/launcher/wrap.bin`, the launcher's `private.sav`).

Result with the real NAND attached: Shantae reaches its title screen, reads
its NitroFS through `nand:/.../00000001.app` and makes the same 7
`PUBLIC.SAV` writes as the real launch. 1618 of 2000 frames are identical to
the real launch at its 224-frame offset, and the rest are fades up to 3
frames apart. Mighty Flip Champs reaches its title screen too.

**With only the BIOS pair** (`--dsi-hle-launch` without `--dsi-nand`,
`--dsi-boot` or `--firmware`; SDL `--dsi-mode game` without `--dsi-nand`),
`NDS::prepare_dsi_hle` makes up the rest (`io/dsi_nand_synth`):
- a DSi firmware (128 KB, console type 0x57, W015 board) carrying `[user]`;
- TWLCFG, HWINFO_N and HWINFO_S generated from `[user]` in a region the
  title's header allows (the user's language picks among the regions; a
  language the region lacks falls back to its own). The RAM copies are their
  0x154-byte boot blobs;
- an in-memory NAND (`NandImage::create_in_memory`): the retail MBR and
  FAT16 geometry (`NandFs::format`) under a made-up console ID and CID, with
  the settings files, the title as content 00000000 and empty saves.
  `FatVolume` writes long file names now (`TWLFON~1.DAT`).
  `NandImage::mark_baseline` keeps the build itself out of the save export;
  saves persist as `<CODE>.pub/.prv/.bnr` in `paths.saves`.

Results over the 15 installed titles: **all 15 start with the BIOS pair
alone.** Shantae's frames match the real-NAND hand-off (1623/2000) and its
save round-trips. Four titles use the system font (EA Sudoku, Mario vs.
Donkey Kong, Paper Airplane Chase, Bird & Beans); without a font they stayed
on a white or black screen.

**The system font is DSperate's own** (`io/dsi_font/`, built by
`tools/make_dsi_font.py` from Noto Sans (OFL-1.1) and WenQuanYi Micro Hei
(GPL-3+ with the font exception); provenance and licences beside it). It is
the retail table layout with three 2-bpp Nitro fonts (16x21, 12x16, 10x12
cells), 7383 characters each, compressed with the DSi's backwards LZ. The
cell geometry and baselines were measured from the console's font; no glyph
data comes from it. Of the console's private-use symbols only the control
buttons are present (A/B/X/Y, L/R, D-pad, arrows at U+E000-E006 and
U+E019-E01C), from images drawn for DSperate (`io/dsi_font/control-glyphs`). `--dsi-font` / `paths.dsi_font` substitutes a console's own file.

**China and Korea have tables of their own** (2026-09-13). A Korean title
(KUWK, WarioWare: Snapped!) asks `OS_LoadSharedFont` for resources 6, 7 and
8. The SDK refuses an index at or past the table's entry count, and one of
3 or more while header byte 0x86 is zero, so no title of those regions can
use the normal three-entry table; Chinese titles ask for 3-5 the same way.
`TWLFontTable-cn.dat` and `-kr.dat` have the consoles' layout (GBATEK: nine
entries, six zero-filled, 0x86 = 4 or 5) with GB 2312 or KS X 1001 Hangul
glyphs from WenQuanYi Micro Hei, and `prepare_dsi_hle` picks the table for
the region the title runs in. A `--dsi-font` of another layout is reported.
Checked: KUWK hashes the nine-entry table and passes the signature check;
the tables decode (hashes, LZ, NFTR) and render Hangul and hanzi; KEVJ
(Space Invaders Extreme Z, Japan) reaches its title screen with the normal
table, frames unchanged. Not checked in the emulator: a Korean or Chinese
title drawing with its font. KUWK waits on the DSP (the TWL SDK's
`DSP_ReceiveData` polls PSTS for a reply the unmodelled core never sends)
before it loads a font.

The table's RSA signature cannot be made. Tamper tests on EA Sudoku showed
it is the only thing checked: flipped font data or resource hashes still
ran, a flipped signature byte gave a white screen. The SWI trace (headless
`DS_SWI_LOG=1`) shows the check going through the DSi BIOS:
1. SWI 0x27 hashes the header 0x80-0x9F;
2. SWI 0x20 initialises the RSA heap;
3. SWI 0x22 `RSA_Decrypt_Unpad(r0=heap, r1=dst, r2=signature)` writes the
   20-byte digest and returns 1 (0 for a bad signature, after which nothing
   more is read);
4. SWI 0x28 compares the two digests;
5. SWI 0x27/0x28 check the resource headers against the table's hash
   (GBATEK, "DSi SD/MMC Firmware Font File").

The generated file carries every hash correctly and a plain-text marker in
the signature's place. `NDS::dsi_hle_swi`, called from the interpreter's SWI
case (the recompilers fall back to it), answers SWI 0x22 with the header's
SHA-1 only when the signature buffer is that marker. Everything else goes to
the BIOS. Compared with the console's font on the same inputs, the titles'
system-font text (the "Exit / Help / Settings" row in Paper Airplane Chase
and Bird & Beans) draws at the same positions and weight.

Not generated: `cert.sys` and the other system files; no title of the 15
reads them.

### 2.3 The virtual NAND: title injection and persistence

Decided 2026-09-12. A dump-free NAND is not possible on this route (boot2,
the launcher and the system titles only exist in `nand.bin`), so the NAND is
the user's dump, never written, with a session layered on top:

- **Read-only dump, writes in memory.** `NandImage` opens `nand.bin`
  read-only and holds every guest write as a 512-byte sector in memory,
  overlaid on later reads. *Landed:* the 7000-frame Shantae menu launch is
  byte-identical to the write-through run and the dump's MD5 is unchanged.
  `--dsi-nand-write` (headless) writes through instead, for diffing the image
  against melonDS (use a copy).
- **The dump's layout is kept.** Existing files stay on their sectors, so the
  boot stays comparable to melonDS sector for sector. Injected files take
  free clusters; the FAT and directory changes are in-memory sectors like any
  guest write. Free space is the dump's (partition 0 is FAT16, 421 769
  sectors, about 206 MB; the photo partition about 33 MB).
- **Titles: the NAND's own plus at most one virtual title per boot.** The
  dump's installed DSiWare shows by default (an option hides it). A title
  passed in (`.nds`/`.app`/`.cia`) is injected only when its title ID is not
  already installed; otherwise the NAND's copy is used. That keeps any
  install limit (the partition's free space, the launcher's icon grid) out of
  the way by construction.
- **What an injection writes** (melonDS `NANDMount::ImportTitle`):
  `title/00030004/<id>/content/title.tmd` -- the title's **Nintendo-signed**
  TMD, which the launcher checks at launch (step 4 below) -- and
  `<content id>.app` (held in memory for now), `data/public.sav`/`private.sav`
  sized from header 0x238/0x23C and `banner.sav` when `AppFlags & 4`, each
  formatted as `CreateSaveFile` does, and a fabricated
  `ticket/00030004/<id>.tik` encrypted under the ES key (from the console ID
  and `bios7i[0x8308]`; verified: all 20 of the dump's tickets decrypt with
  it). The install also has to respect the launcher's 1024-block DSiWare
  quota (step 4). The FAT is
  AES-CTR under the console-ID key (`FATIV = bswap128(SHA1(CID))`), so a C++
  FAT16 reader/writer with that crypto is the core of this work;
  `tools/dsi_nand.py` is the independent reference to test it against.
- **Inputs:** `.nds`/`.app` with a TMD built from the header; `.cia`, whose
  single content is the SRL (title-key content decrypted with 3DS common key
  0; `dsperate-research/tools/melonds/cia_to_srl.py` is the reference,
  including the 520-byte DSi TMD).
- **Persistence, pulled out of the written sectors as files:**
  - DSi system settings (`shared1/TWLCFG*.dat`, `sys/HWINFO*`, and whatever
    else under `sys/` the settings app writes) to a sidecar beside the dump,
    the way `firmware.bin.ovr` holds the DS firmware's settings, applied again
    at the next boot;
  - title saves to `<saves>/<GAMECODE>.pub/.prv/.bnr`, melonDS TitleManager's
    export format, for the dump's own titles as much as the virtual one
    (imported over the NAND's copy at the next boot);
  - photos (`photo:/` partition) as plain image files in a folder.

  Extracted on a quiet period and at exit, like `.sav`.

Order:
1. read-only dump + in-memory writes (done);
2. C++ FAT16 + NAND crypto layer (done: `io/dsi_nand_fs.*`, `crypto/sha1.*`).
   `FatVolume` is FAT12/16 over any byte device; `NandFs` adds the MBR, the
   sector AES-CTR and the ES ticket crypto. The ES key from our `bios7i` is
   verified: all 20 tickets in the dump decrypt with a good MAC.
   `tests/nand_fs_test.cpp` covers SHA-1, a FAT12 image, and (with
   `DS_TEST_DSI_NAND` / `DS_TEST_DSI_BIOS7I`) the real dump. A multi-cluster
   write into a copy (`DS_TEST_DSI_NAND_COPY`) reads back byte-identical
   through `tools/dsi_nand.py`. Partition 0 has 3366 free 16 KB clusters
   (about 54 MB);
3. persistence (done: `io/dsi_nand_persist.*`). Only files whose data sectors
   the session wrote are exported, and a host file is only rewritten when it
   changed. Headless `--dsi-persist DIR` puts everything under DIR; the SDL
   frontend imports before boot and exports on quiet NAND writes, pause, lid
   and exit, with saves in `paths.saves` (else beside the dump), `<nand>.ovr`
   and `<nand>.photos/`.
   - Gate, headless: session 1 (the Shantae menu launch) exports `KS3E.pub`
     and TWLCFG0/1. Session 3 imports them, launches Shantae with the same 19
     writes, and its exported save is identical. The dump's MD5 is unchanged
     throughout.
   - The launcher keeps its selected icon in TWLCFG, so an imported sidecar
     starts the menu where the last session left it; replaying the old
     14-scroll script then opens Settings instead.
   - The unit test round-trips a changed save and a changed TWLCFG through
     export and a fresh import.
   - TWLCFG is only written on that boot because the headless clock starts at
     2000-01-01. With the host clock (`--rtc-host`, and always in SDL) a boot
     with no input writes nothing.
4. injection of one title (done: `io/dsi_title_install.*`; headless
   `--dsi-install F [--dsi-tmd F] [--dsi-offline] [--dsi-hide-installed]`, SDL
   `--dsi-mode <game.nds|.cia>` with the same flags). The ticket, directories,
   saves (`make_dsi_save`, melonDS `CreateSaveFile`), TMD and `.app` go into
   the session's memory in about 80-250 ms. Two launcher rules were found the
   hard way:
   - **The DSiWare quota: 1024 blocks of 128 KB.** When the files under
     `/title/00030004` exceed it, the launcher stops right after Health and
     Safety with "An error has occurred". The dump sits at 1001.5 blocks, so
     one more 3.8 MB title (KD9E) or 15 MB one (KZLE) triggers it. The same
     title booted once a 6 MB title was removed, and a forgotten, reinstalled
     KMGE booted too. The installer now hides the dump's own titles for that
     session, largest first, until the new one fits, and names them. (The
     user's first guess, that the CIAs were dumped from a 3DS, was not it: the
     CIA SRL matches the SHA-1 in Nintendo's own TMD.)
   - **The launcher checks the TMD's RSA signature when it starts a title**,
     not when it lists one. A synthesized TMD (field for field identical to
     the real one apart from the signature) lists and shows the title, then
     launching logs `menuRedIplManager.cpp RED FATAL ... (000300044b443945)`
     to `/sys/log/sysmenu.log`. Corrupting one signature byte of Shantae's
     real TMD fails the same way. So an install needs the title's real TMD.
     `find_signed_tmd` tries, in order: a DSi-signed TMD embedded in the CIA
     (ours carry 3DS TMDs), the cache `<saves>/<GAMECODE>.tmd`, the update CDN
     (`http://nus.cdn.t.shop.nintendowifi.net/ccs/download/00030004XXXXXXXX/tmd`,
     only the 2.3 KB TMD, through the dlopen'd libcurl RetroAchievements
     uses), and a user file (`<game>.tmd` or `--dsi-tmd`). `check_signed_tmd`
     requires the DSi signature type and issuer, this title ID, one content,
     and the SRL's size and SHA-1.
   - Title-key-encrypted CIA content is refused: decrypting it needs the 3DS
     common key, which DSperate does not carry.
   - Gate: Dr Mario Express (KD9E, CIA-only) appears as a new-title gift,
     opens, launches to its title screen, and exports `KD9E.pub`. Session 2
     is offline (cached TMD), imports the save and the sidecar, and boots with
     it selected. The dump's MD5 is unchanged throughout.
   - Not yet: gameplay past the title screen, the `.app` streamed from the ROM
     file instead of held in memory (16 MB of sectors for a large title), the
     exported `.pub` imported into melonDS, and SDL options/menu for hiding.
5. TLNC auto-launch (done: `NDS::dsi_autoload`, the block writer shared with
   the `DS_DSI_HANDOFF=2` experiment). Written after the NAND boot is set up:
   "TLNC" at 0x02000300 plus BPTWL register 0x70 = 1. The launcher then skips
   Health and Safety and the menu and starts the title. It works with no
   input: Dr Mario Express (injected) shows its developer logo by frame 500
   and its title screen by 700; Shantae (already on the NAND, so no install and
   no TMD needed, `--dsi-offline`) shows WayForward by 900, "Touch to Start"
   by 1200, and File Select after one tap. The SDL frontend auto-launches the
   title given to `--dsi-mode`; `--dsi-menu` boots to the menu with it
   installed instead. Headless: `--dsi-install F --dsi-autoload`. A title
   already installed is detected before any TMD lookup (`nand_has_title`).

### 2.4 Machine selection and frontend

As built (`6b5e546`, `cd13967`), which departs from the first plan in two
ways: DSiWare needs no `--dsi-mode`, and the picker lists it in every mode.

- **DSiWare named on the command line** (`.nds`/`.dsi` recognised by header,
  `.cia`) runs on the DSi machine. Without `--dsi-nand` it takes the hand-off
  HLE (2.2). With `--dsi-mode --dsi-nand` it is installed into the NAND
  session and auto-launched, or `--dsi-menu` boots to the menu with it
  installed. `--dsi-tmd`, `--dsi-offline` and `--dsi-hide-installed` apply.
- **The loader's game list** lists `.nds`/`.dsi`/`.cia` DSiWare as
  `[DSi] name` and launches it with the hand-off, detaching the JIT.
- **`--dsi-mode --dsi-nand` with no title** boots the DSi Menu with the
  loader cart in the slot (the whitelist gate in section 5, item 3). A DS game
  picked there leaves the DSi machine for a DS.
- DSi-enhanced carts (title-ID high `0x00030000`) stay in DS mode.
- Ini paths: `paths.bios9i`, `paths.bios7i`, `paths.dsi_font`,
  `paths.dsi_sd` (`--dsi-sd DIR`), and (2026-09-13):
  - `paths.dsi_nand` stands in for `--dsi-nand` **only under `--dsi-mode`**
    (USER DECISION): DSiWare named alone or picked from the list keeps the
    hand-off, which needs no signed TMD.
  - `paths.dsi_firmware` is loaded instead of `paths.firmware` whenever the
    machine is a DSi from the start (`--dsi-mode`, DSiWare on the command
    line), and swapped in when DSiWare is picked from a DS session's list
    (the DS firmware's settings sidecar is then left alone). `--firmware`
    wins over it; a missing file counts as unset.
  - `emu.dsi_hide_installed`, a restart-only settings row (HIDE NAND
    DSIWARE), stands in for `--dsi-hide-installed`.
- `.app` streaming (the title is held in memory, twice with the synthesised
  NAND): deferred past 2.0.0 (USER DECISION).
- A DSi session forces `emu.jit=false`, lockstep, and idle skip off on the
  NAND boot. It prints "EXPERIMENTAL: interpreter".
- Headless uses `--dsi` (required with no ROM, or the machine is a DS and
  draws nothing), plus `DS_LAUNCH_AT=<frame>:<path>` to script a picker
  launch.

### 2.5 Save states

Done 2026-09-13. `FORMAT_VERSION` 3 holds every DSi addition (none of the
interim DSi versions shipped; version 2, the last released, still loads, and
a version-2 state is a DS state). A DSi state is the DS layout plus:
- **HEAD** says DSi or DS; a state loads only into the same machine.
- **NAND**: the sectors written since the *state base*, and the base's
  identity. The base is the NAND as the session built it before any saves
  went in: the dump as opened, with its titles hidden or one installed, or
  the synthesised NAND (`NandImage::mark_state_base`, called by
  `prepare_dsi_hle` and by both frontends before `nand_import`). The
  identity hashes the CID, console ID, size and the base's written sectors,
  so a state refuses another dump, another installed or hidden title, and a
  hand-off NAND made from another title, font or `[user]` settings (a
  changed nickname invalidates hand-off states). Imported saves come after
  the base, so a state brings back the saves it was made with: loading
  reverts every sector written since the base (`since_base_` keeps what the
  base held) and applies the state's. The loaded sectors count as writes, so
  the frontend's export carries them out as usual. A `--dsi-nand-write`
  session cannot save a state.
- **SDCD**: the SD card's in-memory sectors (filesystem, unsynced guest
  writes), its extents onto host files and its record of the folder
  (`SdCard::state_snapshot`). USER DECISION: a state takes the card only if
  every host file backing it is unchanged (size and mtime to the
  nanosecond); otherwise, or when the session has no card, it loads anyway
  with an **empty SD slot until the next reset**, with a boxed warning in the
  log and a double-length toast in SDL (`NDS::sd_card_note`). A sync that
  rewrote a backing file after the state was made is enough to leave the
  card out. The SD host records whether a card was in, so its registers are
  skipped when the card is left out.
- **DSIH**: `dsi_loader_launched`, `exit_requested`, `dsi_soft_reset_pending`.
- `Bus::relink` re-applies SCFG_CLK9 (the ARM9 at 67 or 134 MHz) and the
  VRAM timing after a load; without it a hand-off state resumed at the wrong
  clock (the slice grid split 7700 slices in).
- `state::Reader::vec` now reads arithmetic vectors in one copy and fails on
  a length the chunk cannot hold; the bytes on disk are unchanged.

Checked, each with frame hashes, RAM/register hashes, save-after-load
identity and the whole state after the run (`--save-state-at` on both):
card mode (Shantae @300+200), the hand-off (Shantae @100+400 and @600+400,
with an SD card), a real-NAND TLNC boot with an SD card (@1500+600); a
state from before any save, loaded into a session that imported one, ends
byte-identical to a session that never had it. Refusals checked: another
NAND base (hidden titles), a hand-off state on a NAND session, other
`[user]` settings, a DSi state on a DS; the SD fallback for no card and for a
touched backing file. Unit tests: `nand_fs_test` (`test_nand_state`),
`sd_card_test` (`test_card_state`). Not checked: the SDL flow on a device.

## 3. Corrections to the 2026-09-10 plan

- **"Synthesise the NAND instead of requiring a dump."** Impossible on the
  route that works: boot2 and the launcher are on the NAND. The FAT builder
  survives only as the title installer (2.3) and the SD-card image (2.2).
- **"Direct boot only."** Superseded by 2.1.
- **"The title decrypts its saves through AES engine slot 3."** No. Every
  oracle title writes `AES_CNT` once at boot and never uses the engine at
  runtime. NAND crypto is software above a raw-sector SD host.
- **"The SD host encrypts per sector on read."** No. melonDS's guest path
  seeks the file directly; its FAT crypto serves only the host-side mount.
- **"Cameras: stub, I2C acks, never a frame."** No. The launcher configures
  both sensors and polls the module, so `DSi_Camera` was ported whole.
- **SDIO / Wi-Fi out of scope.** No. The launcher initialises the Atheros
  module over SDIO and waits on it, so both are in as hardware.
- **"The CIA TMD's first 0x208 bytes are the DSi TMD."** No. Keep 0x1E4 bytes,
  patch the title ID and save sizes, and append a 36-byte DSi content record.
- **The last ARM7 byte divergence (0x0380FFC8).** Resolved: melonDS
  `DSi::Reset` maps WRAMCNT 3. It is not an aliasing mystery.

## 4. What is built (all on `dsiware`, gate-green)

| area | files / commits | state |
|------|-----------------|-------|
| Machine: 16 MB RAM, NWRAM/MBK, SCFG (per-width reads), IE2/IF2, NDMA, DSi CP15, BIOS pairs + protection, SNDEXCNT, DSi CODEC/TSC, GPIO, I2C + BPTWL | phase 1 | trace-exact vs melonDS; BPTWL soft reset modelled on melonDS `DSi::SoftReset` (`NDS::dsi_soft_reset`; `DS_DSI_SOFT_RESET_AT=<frame>` forces one in headless): reboots to Health and Safety and the launcher, not compared with melonDS |
| Scheduler: melonDS 64/8 slice grid, 134/67 MHz ARM9 (`set_clock9_shift`), DMA iterations, pending cycles, soft timers per CPU, 8 us Wi-Fi timer | phase 1, `f8de6bf`, `50d4f87`, `e0b4f79` | exact through the launcher boot |
| Modcrypt at load; AES engine (CTR/CCM, 4 slots, FIFOs, NDMA 0x2A/0x2B) | phase 2, `io/dsi_aes.*`, `tests/aes_test.cpp` | unit-tested; unused at runtime by oracles |
| SD/MMC host (port 1 = NAND over `NandImage`; port 0 = the SD card) | `fe13f0a`, `io/dsi_sd.*` | exact; raw sectors; `DS_NAND_LOG` / `DS_SD_LOG` |
| SD card from a host folder; FatVolume FAT32, UTF-8 long names, bulk populate | `io/dsi_sd_card.*`, `io/dsi_nand_fs.*`, `tests/sd_card_test.cpp` | System Settings copies DSiWare to it and reads the export back after a sync; Free Blocks match melonDS on the same image |
| SDIO host + Atheros module (BMI/HTC/WMI, scan, data frames through the NetDriver) | `e0b4f79`, `io/dsi_nwifi.*` | System Settings' connection test passes on Connection 5 |
| Microphone (MIC_CNT/MIC_DATA, I2S sample clock) and SNDEXCNT's output stage | `io/io.cpp`, `spu/spu.cpp` | System Settings' Mic Test meters a tone; audio sample-identical to melonDS |
| Cameras (2 Aptina sensors on I2C, module at 0x04004200) | `e0b4f79`, `io/dsi_camera.*` | black frames |
| DSP host interface, no core (`PSTS` 0x0100) | `50d4f87`, `io/dsi_dsp.*` | core enable logged once; a DSP program start sets `NDS::dsi_dsp_started` (cleared by reset and soft reset, in the state's DSIH chunk), which SDL shows as a "GAME LIKELY WON'T WORK / DSP IS NOT EMULATED" toast. Seen on KUWK (stalls in `DSP_ReceiveData`) and DSi Sound; not on the launcher, Shantae or KEVJ |
| NAND boot (boot2 shortcut, reset-state fixes: EXMEMCNT 0x6000, CP15 0x2078, WRAMCNT 3, WRAMCNT write re-applies NWRAM) | `d30ed62`..`e0b4f79` | **no-cart boot reaches the DSi Launcher**; top screen at frame 1200 pixel-identical, slice grid identical to the tap |
| Virtual NAND: read-only dump + in-memory writes, FAT12/16 + NAND crypto, persistence, one-title install, TLNC auto-launch | `7e1b954`, `io/dsi_nand_fs.*`, `io/dsi_nand_persist.*`, `io/dsi_title_install.*`, `crypto/sha1.*` | KD9E injected and Shantae launch with no input; saves and settings round-trip; dump MD5 unchanged |
| Launcher hand-off HLE (`--dsi-hle-launch`, `DS_ENTRY_SNAP` capture) | `1bc7eee`, `nds_dsi.cpp` | Shantae 1618/2000 frames identical to the real launch |
| No-NAND boot: synthesised NAND, DSi firmware with extended user settings, TWLCFG/HWINFO by header region | `5e96574`, `6b5e546`, `io/dsi_nand_synth.*`, `bios/firmware_gen.cpp` | 15/15 oracle titles start on the BIOS pair |
| DSperate's DSi system font + SWI 22h HLE | `456ed04`, `2a154dd`, `d582f79`, `io/dsi_font/`, `tools/make_dsi_font.py` | titles accept it; control glyphs drawn for DSperate |
| Soft reset: `NDS::dsi_soft_reset` (NAND), `exit_requested` (synthesised NAND) | `d026ef5`, `6b5e546` | NAND reboot not compared with melonDS |
| SDL: DSiWare from CLI and loader list, loader cart on the DSi Menu (`dsi_loader_watch`) | `6b5e546`, `cd13967` | picker launches of `.cia`, `.dsi`, DS games checked |

Not built: JIT and idle skip under DSi (SDL forces them off), `.app` streaming (the
title is held in memory), DSP core, NWRAM dual-slot writes, the
`0x02FE71B0`/SCFG_EXT RAM-size hacks, JIT parity for `code_latch`/
`irq_skip_once`/`defer_cost` (the JIT falls back to the interpreter for the
unmapped DSi ARM7 BIOS), cart-present NAND boot.

## 5. Remaining work to 2.0.0, in order

Items 1-3 are done and kept as the record. Items 4-9 are open.

1. **Launch an installed title from the NAND boot.** *Menu launch works at
   `e0b4f79`, with no code changes* (2026-09-12). The script: Health and Safety
   tap `600:128,100`; 14 taps at `60,113`, every 200 frames from 800; launch
   tap `3600:128,113`; title taps `5000/5600/6200:128,140`; 7000 frames. With
   it, Shantae boots, reads and writes `PUBLIC.SAV` and reaches File Select /
   File Copy. Against melonDS on the same script:
   - rendered frames are identical from frame 5235 to 7000;
   - the 19 NAND writes are identical in sector and order;
   - the guest made 31 795 512-byte block reads, against melonDS's 31 859
     (our log also has 19 904 16-byte boot2-loader reads);
   - the written images differ in 3 bytes, all FAT modification times
     (`srrSaveData.bin` inside the save, and both TWLCFG entries): we stamp
     00:00:00, melonDS 04:16:00. Save data is identical. The cause is not
     diagnosed; the RTC or the time read path is the suspect.

   Earlier frames differ in about 900 frames between 25 and 5234 (item 9).
   (13 scrolls launches Mighty Flip Champs instead; it also matches.) TLNC
   auto-launch is done (item 2). Still to do: the cart-present NAND boot
   (last diverged on SPIDATA at frame 37).
2. **The virtual NAND** (2.3): done at `7e1b954` -- read-only dump with
   in-memory writes, the FAT/crypto layer, save and settings persistence,
   one-title injection (signed TMD, DSiWare quota) and TLNC auto-launch.
3. **No-NAND fallback** (2.2): done -- the hand-off HLE, synthesised NAND,
   generated settings, DSi firmware (with the DSi's extended user settings:
   without them Shantae shows "Localization not found") and DSperate's own
   system font; all 15 oracle titles start on the BIOS pair alone.
   - A soft reset on a synthesised NAND (DSi Menu button, power button,
     application jump) has nothing to reset into: `NDS::exit_requested`, and
     the frontends end the session (user decision, 2026-09-13).
   - SDL: DSiWare (`.nds`/`.dsi` by header, `.cia`) named on the command line
     runs this way without `--dsi-mode`, and the loader's game list marks it
     `[DSi]` and launches it on the DSi machine (the JIT is detached for it).
     The DSi BIOS pair can be configured as `paths.bios9i`/`paths.bios7i`.
     `DS_LAUNCH_AT=<frame>:<path>` scripts a picker launch.
   - The loader cart on the DSi Menu (`--dsi-mode`, no title): the menu lists
     it and fades to white when it is launched, then refuses it
     (`WHITELIST_NOTFOUND` in /sys/log/sysmenu.log: unsigned and on no
     whitelist, as a real DSi does with an old flashcart). The fade is the
     picker's signal as on the DS, but the DSi Menu also fades to white at
     its boot splash and after Health & Safety, so on the DSi only a fade
     after the launcher hashed the loader's header counts: DSi BIOS SWI 27h
     over its first 0x160 bytes, the whitelist key, 44 frames before the
     fade (`NDS::dsi_loader_watch`). A DS game picked there leaves the DSi
     machine for a DS (recompiler, interleave and idle skip given back);
     DSiWare stays on it with the hand-off. The DS menu path is unchanged.
   - China/Korea font tables: done (2.2), unverified in a title's drawing
     (KUWK needs the DSP first). Open: the launcher's title list at
     0x02FFD800; the JIT under DSi.
4. **SD card slot** (done, 2026-09-13). USER DECISIONS: a host folder, not
   an image (`--dsi-sd DIR`, `paths.dsi_sd`); the guest's changes are synced
   back including deletions; the card size is automatic only.
   - **The card** (`io/dsi_sd_card.*`) is built in memory at boot: an MBR
     (partition at sector 8192), FAT16 below 1 GB and FAT32 from 1 GB
     (melonDS's rule; 4 KB clusters to 2 GB, 32 KB above), sized as
     melonDS's FATStorage sizes one: content + 128 MB, next power of two,
     at most 32 GB. Files get cluster chains but no data. A data sector the
     guest has not written is read from the host file that owns it, so memory
     holds only the FAT, the directories and what the guest writes. Host
     timestamps become FAT timestamps. Skipped with a note: names FAT cannot
     hold, names equal but for case, files of 4 GB or more, directory links.
   - **Sync** (`SdCard::sync`): new and changed files are written to
     `<name>.dsperate-sync` and renamed over the host file; files and emptied
     directories the guest removed are removed. A host file that changed on
     the host since the card was built (size or mtime) is never overwritten
     or removed: the card keeps its own copy in memory, with a note. After a
     sync the card reads its files from the host again. Headless syncs at
     exit; SDL on the NAND's occasions (quiet writes after 2 s, pause, lid,
     exit) and never under a replay.
   - **Guest side**: the card is a second `MmcStorage` on host 0 port 0
     (`BlockStorage` interface shared with `NandImage`), melonDS's SD
     variants: CMD1 dropped, the CMD3 R6 answer, ACMD41 keeps bit 30, CID
     `DSiSDCardCID`, presence bits 0x20/0x80 in register 0x1C.
   - **FatVolume** grew FAT32 (28-bit entries, cluster-chained root, FSInfo,
     backup boot sector), UTF-8 long names (and the NT lower-case flags on
     read), case preservation for the SD card only, timestamps, `create()`
     without data, `populate()` for a whole directory in one pass (a
     per-file `create()` re-reads the directory each time), a generic
     `format()`, and a cached free count with an allocation hint that finds
     what a scan from cluster 2 would (so NAND installs are unchanged).
   - **The launcher hand-off** (2.2) gives a title with SD access (header
     0x1B4 bit 3) a sixth mount entry, `{0x00060049, "sdmc", "/"}`, captured
     from KNAE's real launch with and without a card: identical either way,
     and our table now matches it byte for byte.
   - **What uses the card.** The DSi Menu never selects port 0 during boot
     (melonDS neither), nor does KNAE's startup. DSi Sound (HNKE) reads it at
     start (144 blocks) and then stalls, waiting on the DSP (melonDS aborts on
     it). **System Settings** (HNBE) is the working test: Data Management
     shows "SD Card: Blocks Free 2,014" exactly as melonDS does on the same
     image; copying Aura-Aura Climber to the card makes 9 702 block writes
     and the sync writes `private/ds/title/4B535245.bin` (4 898 908 bytes)
     and `HNB_.lst` (1 208 bytes, byte-identical to melonDS's). The `.bin`
     differs from melonDS's in content, as a fresh encryption would, and a
     second session built from the synced folder lists the title under the
     SD Card tab with its icon (1 977 blocks free).
   - Tools: headless `--dsi-autoload-id <16 hex>` (TLNC-launch any installed
     title, system apps included), `DS_SD_DUMP=<file>` (the built card as an
     image), `DS_SD_LOG=<file>` (block log). trace_melonds `--sd <image>` and
     `--autoload <16 hex>`.
   - Script for the Settings copy (both emulators; `--autoload
     00030015484E4245`): taps `900:115,55` Data Management,
     `1500:130,88` the second title, `2100:80,133` Copy, `2900:80,152` Yes;
     4500 frames.
   - Open: a read-only option, a card image as an
     alternative input, photos from DSi Camera (DSP-dependent, untested),
     homebrew on the card (the Unlaunch installer through the hand-off, title
     ID 0 and no parameter block, crawls to about 22 frames in 15 minutes
     with or without a card, on the unmodified d582f79 build too: a hand-off
     problem, not the card's),
     and melonDS parity of the Settings run past the Data Management tap (our
     frames are a few behind after the tap, as at the Health and Safety tap).
5. **Microphone** (done, 2026-09-13). A port of melonDS DSi_I2S into `Io`:
   - `MIC_CNT` (0x04004600): bit 15 runs it; format and rate bits and the
     FIFO clear only take while stopped; bit 11 is the overrun latch; bits
     8-10 read the FIFO's empty/half/full state. `MIC_DATA` (0x04004604):
     every access of any width takes a word, an empty FIFO repeats the last.
     16-word FIFO; half-full raises `IRQ2_MIC_EXT` (bit 13) and NDMA 0x2C;
     overrun raises it with bit 14.
   - The sample clock is the SPU mixer's (32.7 or 47.6 kHz, one sample per
     event on the DSi): each output sample, while SNDEXCNT enables I2S, feeds
     `Io::mic_at(t)`, the frontend's per-frame capture buffer at that point
     of the frame. Format 0 keeps both of each duplicated sample, 1/2 one,
     3 none; the rate field divides the clocks.
   - The same stage now applies SNDEXCNT to the output as melonDS does:
     silent while bit 15 is clear, muted by bit 14, and scaled by the
     NITRO/DSP ratio (DSP output is silence). Shantae's audio stays
     sample-identical to melonDS over 3000 frames.
   - The frontend opens the host capture device when MIC_CNT starts
     (`Io::mic_used`), as for the DS's AUX reads; DS-mode titles on the DSi
     still read the mic through the TSC's AUX input.
   - Checked with System Settings' **Mic Test** (`MIC_CNT = E10E`: format 2,
     rate /4, both IRQs): silence leaves the meter empty, headless
     `--mic-tone 300` at amplitude 30000 fills it, and an 8000 tone shows the
     same one-bar level in melonDS (trace_melonds `TRACE_MIC_TONE`). Script:
     `--dsi-autoload-id 00030015484E4245`, taps `900:242,93`, `1300:242,93`,
     `1700:128,110`. `DS_MIC_LOG=1` logs MIC_CNT writes.
   - The mic state is in the DSI chunk (`FORMAT_VERSION` 3).
6. **DSi Wi-Fi networking** (done, 2026-09-13).
   - **Two radios.** DSi Connections 1-3 are DS-compatible profiles and use
     the DS Wi-Fi block (`io/wifi.cpp`), already on slirp; Connections 4-6
     (Advanced Setup, WPA) and TWL-SDK titles use the Atheros module.
     `Io::set_net_driver` attaches the driver to both.
   - **Atheros data path** (melonDS DSi_NWifi): `wmi_send_packet` rebuilds
     the Ethernet frame from the WMI data header (MACs, 802.3 length,
     LLC/SNAP, ethertype) and sends it; the 1 ms timer, while connected,
     takes one frame for this MAC or broadcast from the driver and frames it
     back into mailbox 8.
   - **The access point in the firmware** (USER, 2026-09-13: stamp it at
     load, as melonDS does for its generated firmware, without writing the
     firmware file). `bios::stamp_access_point` puts an open, DHCP profile
     named `DSperate-AP` (`bios::kAccessPointSsid`; MTU 1400 on a DSi) into
     the first unconfigured of the three slots, unless a slot already names
     it; a player's own networks are never overwritten. It runs on every
     firmware load, generated or dumped, and again over a `.ovr` sidecar,
     in memory only. `NDS::firmware_ap_slot` says where it went.
   - The Atheros module now advertises the same name, and answers a directed
     scan under the probed SSID (as the DS access point does) instead of
     melonDS's filter.
   - **Bug found on the way:** libslirp at config version 4 calls
     `register_poll_fd` for every new socket and the driver had left it null,
     so the first UDP socket (DNS through the host resolver) crashed. This
     was a latent bug in the DS internet path as well.
   - Checked on the real NAND, `--internet --dns host`:
     - Connection 1 (the stamped slot) reads "Setup Complete", and its
       connection test passes ("Connection test successful", support code
       11172) through the DS Wi-Fi block. Taps as for the Mic Test, then
       `1700:128,142` Internet, `2200:128,49` Connection Settings,
       `2900:175,41` Connection 1, `3500:128,55` Connection Test,
       `4000:80,152` Yes; 7000 frames.
     - Connection 5 through the Atheros module: Search for an Access Point
       lists DSperate-AP; saving it runs the test, which passes the same way
       (WMI connect, then data through slirp). Taps `2900:128,154` Advanced
       Setup, `3400:175,96` Connection 5, `4000:128,49` Search,
       `5200:100,49` DSperate-AP, `6200:175,152` OK, `6900:128,152` OK;
       11000 frames.
   - `DS_DEBUG_NWIFI=1` logs WMI commands and scan results.
   - **Fixed: Search for an Access Point on Connections 1-3 found nothing.**
     That scan is passive: the DSi powers the DS Wi-Fi block on each of 13
     channels for about 110 ms and listens, sending no probe requests. The
     emulated access point (melonDS's WifiAP, as ported) beaconed every
     0x20000 us, 131 ms, so a visit to its channel 6 saw a beacon only when
     the phases lined up: melonDS's run happened to, ours never did. Found by
     diffing the Wi-Fi register traces (`DS_WIFI_TRACE` / `TRACE_WIFI_REGS`):
     identical up to the channel-6 power-on, after which melonDS takes IRQ 6
     (receive start) 18 ms later and ours times out at 110 ms. The access
     point now beacons every 100 TU (102.4 ms, the interval real access
     points use, advertised in the beacon), which always fits a 110 ms
     visit. Checked with the search tap at four different frames: all list
     DSperate-AP; the Connection 1 test still passes. This deliberately
     departs from melonDS's AP timing, so Wi-Fi register traces against the
     oracle diverge at the first beacon a guest receives.
     (Compare on an empty slot: the stamped Connection 1 opens its options
     menu instead of the search, which invalidated a first comparison.)
   - Our DSi clock is stopped in headless without `--rtc-host` (status bar
     reads 00:00); expected, not a bug.
7. **Frontend leftovers** (2.4): done 2026-09-13 (`paths.dsi_nand`,
   `paths.dsi_firmware`, `emu.dsi_hide_installed` and its row); `.app`
   streaming deferred past 2.0.0. **Save states under DSi** (2.5): done.
8. **JIT and idle skip under DSi**: the SDL frontend forces the interpreter
   and lockstep for every DSi session, and idle skip off on the NAND boot.
   Steps: `test_jit` and a launcher boot and a hand-off launch under qemu
   for aarch64 and A32, with frame hashes against the interpreter; then idle
   skip on the oracle set; then measure it on the RG DS. The goal is correct
   and playable, not DS-level headroom (the 134 MHz ARM9 doubles the guest
   budget).
9. **Open fidelity items**: rendered frames differ from melonDS from about
   frame 25 of the NAND boot even while the CPU grid matches (2D
   render/present, unexamined); the grid splits at the Health and Safety tap
   (touch sampling timing); FAT mtimes 00:00:00 against melonDS's 04:16:00;
   the hand-off starts both CPUs together where the real launch starts the
   ARM7 about a frame later; the launcher title list at 0x02FFD800; `SCFG_MC`
   stores bit 0 raw; the cart-present NAND boot (last split on SPIDATA at
   frame 37); non-USA titles untested.

After 2.0.0: DSP HLE (G.711 from KTUE, graphics) plus CLI camera images as
one feature commit (the HLE core-enable path, the 4096-cycle catch-up event,
the ucode CRC table over NWRAM bank B); DSi-mode carts (`SetScfgMC` power
machine); RetroAchievements' DSi console table (78) if wanted; SD card
insertion and removal mid-session (USER, 2026-09-13: after 2.0.0, and it is
not known that the hardware's software handles it; the card is in for the
whole session, with no insert/remove IRQ).

## 6. Oracles and assets

Assets in `<project_root>/dsi-binary/` (never committed): `bios/biosdsi7.bin`,
`biosdsi9.bin` (half dumps), `dsinand.bin` (15 DSiWare titles installed),
`dsifirmware.bin`, `dsiboot.bin` (boot blobs), `games/*.cia` (+ extracted
`.nds`/`.tmd`), `unlaunch/unlaunch.dsi`. `games/*.bin` are console SD exports
under the ES key; ignore them.

| title | code | why |
|-------|------|-----|
| Shantae: Risky's Revenge | KS3E | in NAND and CIA; saves; the phase-1 card gate |
| Mighty Flip Champs | KMGE | cross-title save read (13th icon) |
| Instrument Tuner | KTUE | mic, and the G.711 v4 DSP ucode (`CRC 2A1D7F94`); encrypted CIA |
| Petit Computer | KNAE | 10 MB save, SD-card header bits; save opens via `--touch 400:80,104 --touch 700:60,40` |
| Dr Mario Express, Plants vs Zombies | KD9E, KZLE | CIA-only: installer targets |
| Mario vs DK: Minis March Again | KDME | 416 KB save; "make sure it works", not an oracle |

No title on hand except KTUE uses the DSP. All are USA and from one console:
enough to build against, but not a compatibility survey.

Harness: `dsperate-research/tools/melonds/trace_melonds --dsi <bios9i>
<bios7i> <nand>` (patch `melonds-trace-hook.patch`; append hunks, never
regenerate it). It provides NAND block log + `tools/dsi_nand.py map`,
`TRACE_SD_REGS`, `--touch`/`--key`, `--install <nds> <tmd>`, `--ram-at-pc9`,
`TRACE_SLICES`, `TRACE_TIME_FINE`, `TRACE_WATCH7`, `TRACE_DSP`. `dsi_nand.py`
also has `info`, `titles`, `extract`, `bootblobs`.

## 7. Verification

### 7.1 Gates, every commit

- Six DS scenes unmoved (`tools/all_scene_hashes.sh`), `state_roundtrip.sh`,
  the unit tests (23). mlbis's 600-frame hash moved between `6b5e546` and
  `cd13967` on unmodified HEAD builds too, so re-baseline from a HEAD build
  before reading an mlbis diff.
- **DSi card-mode gate**: Shantae, 60 frames, both CPU traces byte-identical
  to melonDS, 400 rendered frames identical:

      DS_IDLE_SKIP=0 DS_MELON_STM=1 DS_STORE_BUS=0 TRACE_TIME=1 TRACE_START_FRAME=20 \
        dsperate-headless --direct --interp --bios9 .. --bios7 .. --firmware dsifirmware.bin \
        --bios9i biosdsi9.bin --bios7i biosdsi7.bin --dsi-boot dsiboot.bin \
        --frames 60 --trace out/ours --max 40000000 "Shantae - Risky's Revenge.nds"
      TRACE_TIME=1 TRACE_START_FRAME=20 trace_melonds bios9 bios7 dsifirmware.bin out/melon \
        --dsi biosdsi9.bin biosdsi7.bin dsinand.bin --rom Shantae.nds --direct --frames 60 --max 40000000

  The three `DS_` knobs are gate-only: idle skip off, melonDS's ARM7 STM
  quirk, and melonDS's store pricing.

### 7.2 NAND boot to the launcher

    # the dump is opened read-only; add --dsi-nand-write on a copy to diff the written image
    dsperate-headless --direct --interp --dsi --touch 600:128,100:10 \
      --bios9 bios9.bin --bios7 bios7.bin --firmware dsifirmware.bin \
      --bios9i biosdsi9.bin --bios7i biosdsi7.bin \
      --dsi-nand dsinand.bin --dsi-nand-boot --frames 1300 --dump-frames out/o.frames
    trace_melonds bios9.bin bios7.bin dsifirmware.bin out/m --touch 600:128,100:10 \
      --dsi biosdsi9.bin biosdsi7.bin out/n2.bin --nand-inplace --frames 1300 --max 1 \
      --dump-frames out/m.frames

### 7.3 Finding a divergence

Identical per-CPU traces do **not** mean identical interleave (the NDMA
ping-pong once put the ARM7 55 k cycles behind with both traces identical).
1. Dump frames on both sides to see how far each gets.
2. **Slice-grid diff** (about a minute, no traces): `DS_DEBUG_SLICES=1` against
   `TRACE_SLICES=1`, comparing the slice-end column.
3. At the first split, full traces of that window with `TRACE_START_FRAME`,
   `TRACE_TIME_FINE=1` and **both** `DS_TRACE_NODEDUP=1` and `TRACE_NODEDUP=1`
   (setting only one reports the divergence thousands of lines early). The CPU
   that diverges earlier in time is the cause.
4. The usual causes: an unmodelled I/O page, a register at the wrong width, an
   IRQ one slice early or late (read IE&IF in the handler), a quarter-cycle
   cost, a DMA/stall length (`DS_DEBUG_DMA=2`). For a byte that changes, use
   the polled watches `DS_WATCH7`/`TRACE_WATCH7`, not `DS_WATCH`.

### 7.4 Traps

- Reset state that direct boot overwrites but real boot code reads back
  (EXMEMCNT, CP15, WRAMCNT). Check `DSi::Reset`, not only `NDS::Reset`.
- Anything that re-lays shared WRAM must re-apply NWRAM on top.
- `0x0380xxxx` is ARM7-only; never stage it through ARM9 bus writes.
- melonDS's `ScheduleEvent` does not re-arm a live event (`Scheduler::armed()`).
- Always time a default-knob run too (the SPI poll-streak underflow only showed
  with idle skip on), and run the card-mode control before blaming new DSi code.
- Large buffers in `NDS` go on the heap (unit tests put `NDS` on the stack).
- Never hash or benchmark DSi under a missing DSi BIOS.

## 8. Risks

1. **JIT on device.** Every exactness result is interpreter-only, and the
   device runs the JIT. A DSi title that saturates its 134 MHz ARM9 will not
   run full speed on the A55 without a dedicated campaign.
2. **The hand-off HLE is tuned to 15 USA titles from one console.** It
   replaces Unlaunch as the no-NAND fallback and works for all of them, but
   titles that read `/sys` files we do not synthesise (`cert.sys`), use the
   launcher title list at 0x02FFD800, or wait on the DSP will fail
   differently. The synthesised NAND reports a title reading `/sys`; watch
   for that first.
3. **Installer fidelity.** A wrong FAT or save geometry shows up as "no save
   data" or a reformat. Gate the installer against melonDS's `ImportTitle`
   result, sector for sector.
4. **Half-dump BIOS.** If code reaches the missing upper halves, we only find
   out when it does; log the first access to the padded region.
5. **Timing model.** The DSi RAM/NWRAM timings are melonDS's approximations.
   Exactness is against melonDS, not hardware.
6. **Console identity.** Exported saves carry the dump's console ID; melonDS
   import does not care, but a real console might.
