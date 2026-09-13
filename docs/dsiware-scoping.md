# DSiWare support -- scope and status

Rewritten 2026-09-12 at branch `dsiware` @ `e0b4f79` (docs `f245cf9`). This
replaces the 2026-09-10 plan, whose central design (direct boot + a
synthesised NAND, no dump) did not survive; section 3 says what was wrong.
References: melonDS `dsperate-research/melonDS` @ `d3cd6164` (the exactness
oracle) and melonDS DS `dsperate-research/melonds-ds-libretro` (how a
shipping frontend launches DSiWare).

## 1. Scope

The target is DSperate **2.0.0**.

| feature | 2.0.0 | notes |
|---------|-------|-------|
| DSiWare, launched from a real `nand.bin` | **in** | NAND boot + TLNC auto-launch (section 2.1) |
| Booting the DSi menu itself | **in** | same NAND boot with no autoload |
| No-NAND fallback: the launcher hand-off in HLE | **in** | `--dsi-hle-launch` (2.2); needs a synthesised NAND with stub system files; Unlaunch cannot be the fallback |
| Title install from `.nds`/`.app`/`.cia` | **in** | at most one virtual title per boot, injected into the read-only dump's in-memory session (2.3) |
| Save export/import (`.pub`/`.prv`/`.bnr`) | **in** | melonDS TitleManager extensions |
| Microphone (I2S `MICCNT`/`MICDATA`) | **in** | fed by the existing SDL/ALSA capture |
| DSi Wi-Fi networking | **in** | Atheros module is modelled; attach it to the `wifi-emu` slirp backend |
| SD card slot (SD host port 0) | **in** | host folder or image |
| Frontend | **in** | `--dsi-mode` with the usual firmware boot options; ini paths; picker lists DSiWare only in DSi mode; `.cia` input |
| JIT + idle skip under DSi, on device | **in** | must work; **slower than DS titles is accepted for 2.0.0** |
| DSP HLE (G.711, graphics) | after 2.0.0 | one feature commit, together with camera images passed from the CLI |
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

Results over the 15 installed titles: 11 start with the BIOS pair alone, and
all 15 with the user's `TWLFontTable.dat` added (`--dsi-font`,
`paths.dsi_font`). The other 4 (EA Sudoku, Mario vs. Donkey Kong, Paper
Airplane Chase, Bird & Beans) use the system font. Shantae's frames match
the real-NAND hand-off (1623/2000) and its save round-trips.

**The font is signature-checked, and only the signature matters.** Tamper
tests on EA Sudoku: flipping font data or a resource hash still runs, while
flipping a signature byte gives a white screen. The SWI trace (headless
`DS_SWI_LOG=1`) shows the title's SDK doing, through the DSi BIOS:
SWI 0x27 SHA-1 over the header 0x80-0x9F, SWI 0x20 RSA heap init,
SWI 0x22 `RSA_Decrypt_Unpad(r0=heap, r1=dst, r2=signature)` (writes the
20-byte digest, returns 1; 0 on a bad signature, after which nothing more is
read), SWI 0x28 compare, then SWI 0x27/0x28 over the resource headers
against the table's hash (GBATEK, "DSi SD/MMC Firmware Font File"). A
generated font can carry correct hashes everywhere but the RSA; an HLE of
SWI 0x22 that recognises the generated font's signature bytes and returns
its header digest would make one acceptable (not built yet). Until then a
title reading `/sys` on a synthesised NAND is flagged
(`NDS::dsi_font_wanted`).

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

The SDL frontend gets `--dsi-mode`, used with the usual firmware-boot options.
In DSi mode the picker also lists DSiWare (`unit_code & 2` and title-ID high
`0x00030004`); outside it, nothing changes. DSi-enhanced carts (title-ID high
`0x00030000`) stay in DS mode in both. New ini paths: `bios9i`, `bios7i`,
`dsi_firmware`, `dsi_nand` (ini only, per `settings.h`). Headless today uses
`--dsi` (required with no ROM, or the machine is a DS and draws nothing).

### 2.5 Save states

`FORMAT_VERSION` 5 carries the DSi chunk, the SD/MMC, SDIO, Wi-Fi and camera
events, the Wi-Fi mailboxes (~44 KB) and two 32 KB camera register files. Not
covered yet: NAND contents. A state must carry the in-memory written sectors
(`NandImage::written_sectors`) and refuse to load against a different NAND
identity (CID + console ID + size), the way `bios_id` is checked.

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
| SD/MMC host (port 1 = NAND over `NandImage`; port 0 absent) | `fe13f0a`, `io/dsi_sd.*` | exact; raw sectors; `DS_NAND_LOG` |
| SDIO host + Atheros module (BMI/HTC/WMI, scan returns melonDS's AP) | `e0b4f79`, `io/dsi_nwifi.*` | no network backend |
| Cameras (2 Aptina sensors on I2C, module at 0x04004200) | `e0b4f79`, `io/dsi_camera.*` | black frames |
| DSP host interface, no core (`PSTS` 0x0100) | `50d4f87`, `io/dsi_dsp.*` | core enable logged once |
| NAND boot (boot2 shortcut, reset-state fixes: EXMEMCNT 0x6000, CP15 0x2078, WRAMCNT 3, WRAMCNT write re-applies NWRAM) | `d30ed62`..`e0b4f79` | **no-cart boot reaches the DSi Launcher**; top screen at frame 1200 pixel-identical, slice grid identical to the tap |
| Launcher hand-off HLE (`--dsi-hle-launch`, `DS_ENTRY_SNAP` capture) | `nds_dsi.cpp` | Shantae and KMGE reach their title screens on the real NAND; no synthesised NAND yet |

Not built: mic/I2S, SD port 0, TLNC launch on the NAND boot, title installer,
save export, frontend wiring, DSP core, NWRAM dual-slot writes, the
`0x02FE71B0`/SCFG_EXT RAM-size hacks, JIT parity for `code_latch`/
`irq_skip_once`/`defer_cost` (the JIT falls back to the interpreter for the
unmapped DSi ARM7 BIOS).

## 5. Remaining work to 2.0.0, in order

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
3. **No-NAND fallback** (2.2): the hand-off HLE, synthesised NAND,
   generated settings and DSi firmware are in (11/15 titles on the BIOS pair
   alone, 15/15 with the user's font). Next: a generated system font
   accepted through an SWI 0x22 HLE.
4. **SD card slot** (port 0): FAT image or host folder; `FatVolume` already
   reads and writes the filesystem. The Unlaunch installer is an SD test
   title.
5. **Mic / I2S**: `MICCNT`/`MICDATA`, 16-entry FIFO, half-full IRQ,
   `IRQ2_MicExt`, NDMA 0x2C. Oracle: Instrument Tuner (KTUE) uses the mic,
   though its DSP half waits for after 2.0.0. Pulled forward only if the SD
   or Unlaunch work turns out to need it.
6. **DSi Wi-Fi networking**: route the NWifi module's frames into the
   `wifi-emu` slirp backend. Same condition as 5.
7. **Frontend** (2.4) and **save states with NAND** (2.5).
8. **JIT and idle skip under DSi**: `test_jit` and a launcher boot under qemu
   for aarch64 and A32; then measure the oracle set on the RG DS. The goal is
   correct and playable, not DS-level headroom (the 134 MHz ARM9 doubles the
   guest budget).
9. **Open fidelity items**: rendered frames differ from melonDS from about
   frame 25 of the NAND boot even while the CPU grid matches (2D
   render/present, unexamined); the grid splits at the Health and Safety tap
   (touch sampling timing).

After 2.0.0: DSP HLE (G.711 from KTUE, graphics) plus CLI camera images as
one feature commit (the HLE core-enable path, the 4096-cycle catch-up event,
the ucode CRC table over NWRAM bank B); DSi-mode carts (`SetScfgMC` power
machine); RetroAchievements' DSi console table (78) if wanted.

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
  the unit tests (22).
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
2. **Unlaunch is unproven.** Its oracle depends on melonDS accepting it as
   boot2, which nobody has tried. Without that oracle, or if it needs more
   than 2.2 lists, the no-NAND fallback may slip.
3. **Installer fidelity.** A wrong FAT or save geometry shows up as "no save
   data" or a reformat. Gate the installer against melonDS's `ImportTitle`
   result, sector for sector.
4. **Half-dump BIOS.** If code reaches the missing upper halves, we only find
   out when it does; log the first access to the padded region.
5. **Timing model.** The DSi RAM/NWRAM timings are melonDS's approximations.
   Exactness is against melonDS, not hardware.
6. **Console identity.** Exported saves carry the dump's console ID; melonDS
   import does not care, but a real console might.
