# DSiWare support -- scope and status

Status at branch `dsiware` @ `868c8f4` (2026-09-14). `dsiware` is 82 commits
ahead of `main` and contains all of it, the Wi-Fi/slirp work included.
`main` has nothing that `dsiware` lacks. References: melonDS
`dsperate-research/melonDS` @ `d3cd6164` (the exactness oracle) and melonDS
DS `dsperate-research/melonds-ds-libretro` (how a shipping frontend launches
DSiWare).

**Where we are.** Every 2.0.0 feature in the table below has landed:
- **Launch routes.** DSiWare boots from a real NAND (DSi Menu, TLNC
  auto-launch, one injected title, persistence). Without a NAND, the
  launcher hand-off HLE starts all 15 oracle titles on the DSi BIOS pair
  alone.
- **NAND shortcuts** start a NAND title through the hand-off, skipping the
  DSi Menu.
- **Hardware:** SD card slot, microphone, DSi Wi-Fi.
- **Machine:** save states, and the JIT with idle skip.

What is left is fidelity work and checks that have not been run yet
(section 5). Everything prints "EXPERIMENTAL".

## 1. Scope (target: DSperate 2.0.0)

| feature | 2.0.0 | notes |
|---------|-------|-------|
| DSiWare from a real `nand.bin`, and the DSi Menu itself | done | NAND boot + TLNC autoload (2.1) |
| No-NAND fallback | done | launcher hand-off HLE, synthesised NAND and firmware, own system font (2.2) |
| Title install (`.nds`/`.dsi`/`.cia`), save and settings persistence | done | one virtual title per boot, over a read-only dump (2.3) |
| SD card slot | done | a host folder, synced back (2.6) |
| Microphone (I2S) | done | 2.7 |
| DSi Wi-Fi networking | done | both radios on slirp, DSperate's AP stamped into the firmware (2.8) |
| Frontend | done | CLI, loader list, loader cart on the DSi Menu, ini keys, NAND shortcuts (2.4) |
| Save states | done | `FORMAT_VERSION` 3 (2.5) |
| JIT + idle skip | done | `4a3ba16`, `78f9c51` (2.9) |
| DSP HLE (G.711, graphics) + camera images from the CLI | after 2.0.0 | one feature commit. USER DECISION: no stub for 2.0.0; a title that starts the DSP gets a warning |
| DSi-enhanced retail carts in DSi mode | after 2.0.0 | they run in DS mode |
| `.app` streaming (title held in memory) | after 2.0.0 | USER DECISION |
| SD insert/remove mid-session | after 2.0.0 | USER DECISION; the card is in for the whole session |
| RetroAchievements DSi console table (78) | after 2.0.0, if wanted | |
| DSP LLE (Teakra), AAC ucode | undecided | only if a wanted title needs it |
| Second card slot, DSi Shop | out | |

## 2. Design as built

### 2.1 NAND boot and TLNC launch

`NDS::boot_dsi_nand()` (`--dsi-nand-boot`) boots the way the console does.
The DSi BIOS dumps are low-32 KB half dumps, so they lack the boot ROM's
boot2 loader. We do its job instead, following melonDS `LoadNAND`
(`!FullBIOSBoot`):
1. read the boot info (raw NAND 0x220) and the MBK/NWRAM mapping (0x380);
2. decrypt boot2 with AES-CTR under the fixed key;
3. place the eMMC CID at 0x03FFE6E4, plus the BIOS fragments the missing
   halves would have left;
4. jump into boot2, with the first-instruction pipeline charge as
   `defer_cost` 64/4.

A DSiWare launch is this boot plus melonDS DS's **TLNC autoload block**
(`NDS::dsi_autoload`): 0x100 bytes at 0x02000300 and BPTWL register
0x70 = 1. The launcher then skips Health and Safety and the menu. Without
the block, the same boot lands on the DSi Menu.

Why not direct boot:
- melonDS's DSi direct boot (and ours, trace-identical to it) runs DSiWare
  in card mode. The title never touches the NAND, so it has no saves, and
  Plants vs Zombies refuses to run.
- TLNC on a direct boot is inert, because only the boot ROM reads it.

Direct boot stays for the card-mode exactness gate (7.1) and for the
hand-off HLE (2.2).

Unlaunch is not a fallback. `unlaunch.dsi` is the *installer* app, and
Unlaunch proper needs a NAND as much as the stock menu does.

### 2.2 Required files and the no-NAND hand-off HLE

**Files.**
- Required: DS `bios9`/`bios7`, and DSi `bios9i`/`bios7i`. Half dumps are
  enough: their low-32 KB CRCs match melonDS's.
- NAND boot only: `dsifirmware.bin` and `nand.bin` with its nocash footer,
  which carries the CID and console ID.
- The console ID seeds AES key slots 1 and 3, so it must be set before
  `aes.reset()`.
- `--dsi-boot <blob>`: the 0x154-byte TWLCFG/HWINFO blob from
  `tools/dsi_nand.py bootblobs`. It is a direct-boot input only; a NAND boot
  reads the console's own copies.

**Hand-off HLE** (`--dsi-hle-launch`, `setup_direct_boot_dsi` under
`dsi_hle_launch`). Direct boot stages exactly what the DSi Launcher leaves at
a title's entry point. The reference was captured from real launches of
KS3E and KMGE (headless `DS_ENTRY_SNAP=<dir>:<pc9>:<pc7>`); both titles
showed the same 18 RAM ranges of difference, and the HLE closes them:
- **Main RAM:** header copies only; indicator 0x02FFFC40 = 3; TWLCFG
  last-title stretches cleared; Wi-Fi MAC and channel mask.
- **ARM7 WRAM:** the mount table at header 0x1D4 (nand, nand2, shared1,
  photo, dataPub, plus `sdmc` when header 0x1B4 bit 3 is set). It includes
  the title's image path, with the content ID looked up on the NAND. Also
  the locked SCFG_EXT7 and `44 F8` at 0x0380FFC4.
- **I/O:** SCFG_BIOS 0x0501, and SCFG_CLK9 0x0084, so **the title starts at
  67 MHz**. Also SCFG_EXT7, SCFG_MC, SNDEXCNT, EXMEMCNT, RCNT, GPIO, BPTWL
  and the power-management register.
- **CPUs:** System mode, IRQs masked, the launcher's stacks.
- **Modcrypt** decrypts only up to each binary's size.

Left out on purpose: the launcher's leftover jump code, stack bytes and AES
slots; our normalised touch calibration; `"00000009\0E"` at 0x02FFD7B0; the
launcher title-ID list at 0x02FFD800 (not understood).

Result with the real NAND: Shantae matches the real launch on 1618 of 2000
frames at a 224-frame offset (the rest are fades up to 3 frames apart) and
makes the same 7 `PUBLIC.SAV` writes.

**With only the BIOS pair**, `NDS::prepare_dsi_hle` (`io/dsi_nand_synth`)
generates the rest:
- a 128 KB DSi firmware carrying `[user]`, including the DSi's extended
  user settings (without them Shantae shows "Localization not found");
- TWLCFG, HWINFO_N and HWINFO_S for a region the header allows;
- an in-memory NAND (`NandImage::create_in_memory`, `NandFs::format`) with
  retail MBR/FAT16 geometry, a made-up console ID and CID, the title as
  content 00000000, and empty saves.

Saves persist as `<CODE>.pub/.prv/.bnr` in `paths.saves`. **All 15 titles
start.** A soft reset has nothing to reset into, so it sets
`NDS::exit_requested` and the frontends end the session (user decision).

**System font** (`io/dsi_font/`, built by `tools/make_dsi_font.py` from Noto
Sans and WenQuanYi Micro Hei; provenance and licences beside it):
- **Format.** The retail table layout: three 2-bpp Nitro fonts, 7383
  characters each, compressed with the DSi's backwards LZ. Cell geometry
  was measured from the console's font; no glyph data comes from it.
- **Symbols.** The control-button glyphs were drawn for DSperate.
- **Regions.** Chinese and Korean titles get nine-entry tables
  (`TWLFontTable-cn/-kr.dat`, GB 2312 / KS X 1001), chosen by the title's
  region.
- **Override.** `--dsi-font` / `paths.dsi_font` substitutes a console's own
  file.
- **Signature.** The table's RSA signature cannot be made, and it is the
  only thing titles check. The generated file carries every hash plus a
  marker in the signature's place, and `NDS::dsi_hle_swi` answers SWI 0x22
  (`RSA_Decrypt_Unpad`) with the header's SHA-1 only for that marker. The
  interpreter and both recompilers call it.
- **Checked:** system-font text draws at the console font's positions and
  weight.
- **Unchecked:** a Korean or Chinese title drawing with its font. KUWK
  waits on the DSP before it loads one.

Not generated: `cert.sys` and other system files. No oracle title reads them.

### 2.3 Virtual NAND: install and persistence

The NAND is the user's dump and is never written. The session sits on top
of it.

- **Writes.** `NandImage` opens the dump read-only and holds guest writes as
  in-memory sectors. The dump's MD5 stays unchanged, and runs are
  byte-identical to write-through. Headless `--dsi-nand-write` writes
  through, for diffing against melonDS (use a copy).
- **Layout.** The dump's layout is kept: existing files stay on their
  sectors, and injected files take free clusters.
- **Filesystem layer.** `io/dsi_nand_fs.*` (FAT12/16/32 `FatVolume`; `NandFs`
  for MBR, sector AES-CTR and ES ticket crypto) and `crypto/sha1.*`. It is
  tested against `tools/dsi_nand.py` in `tests/nand_fs_test.cpp`, and all 20
  of the dump's tickets decrypt with our ES key.
- **Titles.** The NAND's own titles show, plus at most one injected title
  per boot (`io/dsi_title_install.*`). Injection happens only when the title
  ID is not installed already (`nand_has_title`). It writes the TMD,
  `.app`, sized saves (`make_dsi_save`) and a fabricated ticket, in
  80-250 ms. `.cia` content encrypted with a title key is refused, because
  DSperate does not carry the 3DS common key.
- **Launcher rule 1: the DSiWare quota of 1024 × 128 KB blocks.** Over quota,
  the launcher shows "An error has occurred" after Health and Safety. The
  dump sits at 1001.5 blocks. The installer hides the dump's own titles
  (largest first) until the new one fits, and names them.
- **Launcher rule 2: the TMD's RSA signature is checked at launch**, not
  when the title is listed. A synthesised TMD fails with `RED FATAL` in
  `/sys/log/sysmenu.log`. `find_signed_tmd` looks, in order, at: a DSi TMD
  embedded in the CIA; `<saves>/<GAMECODE>.tmd`; the NUS CDN (the 2.3 KB TMD
  only, via the dlopen'd libcurl); `<game>.tmd` / `--dsi-tmd`.
  `check_signed_tmd` validates type, issuer, title ID, content size and
  SHA-1.
- **Persistence** (`io/dsi_nand_persist.*`): only files whose sectors the
  session wrote are exported, and a host file is rewritten only when it
  changed. What goes where:
  - title saves → `<saves>/<CODE>.pub/.prv/.bnr` (melonDS TitleManager
    format);
  - `shared1/TWLCFG*` and `sys/` → `<nand>.ovr`;
  - the photo partition → `<nand>.photos/`.

  SDL imports before boot and exports on quiet writes (2 s), pause, lid and
  exit. Headless uses `--dsi-persist DIR`.
- **Headless flags:** `--dsi-install F [--dsi-tmd F] [--dsi-offline]
  [--dsi-hide-installed] [--dsi-autoload]`, `--dsi-autoload-id <16 hex>`
  (any installed title, system apps included).

### 2.4 Frontend (SDL)

- **DSiWare named on the command line** (`.nds`/`.dsi` by header, `.cia`)
  runs on the DSi machine, through the hand-off unless `--dsi-mode` has a
  NAND. With `--dsi-mode --dsi-nand` the title is installed and
  auto-launched; `--dsi-menu` boots to the menu with it installed instead.
- **The loader's list** shows DSiWare as `[DSi] name` and launches it with
  the hand-off.
- **`--dsi-mode --dsi-nand` with no title** boots the DSi Menu with the
  loader cart in the slot.
  - The menu refuses the loader (`WHITELIST_NOTFOUND`), as a real DSi
    refuses an old flashcart.
  - The picker's launch signal is a white fade *after* the launcher hashed
    the loader header (SWI 27h over 0x160 bytes, `NDS::dsi_loader_watch`).
    The menu also fades white at boot and after Health and Safety, so a
    fade alone is not enough.
  - A DS game picked there leaves the DSi machine for a DS.
- **NAND shortcuts** (`emu.dsi_nand_shortcuts`, row NAND DSIWARE SHORTCUTS,
  greyed out without `paths.games`):
  - For each title on `paths.dsi_nand`, the frontend keeps a 64-byte
    `<banner title>.dspr.nds` in the games folder. It holds `DSPRSTUB`, a
    version, the title ID and the NAND's CID and console ID.
  - The set is updated at every start. Switching the row off deletes every
    `.dspr.nds`.
  - Opening a shortcut runs the hand-off with the real NAND behind it
    (`NDS::load_dsi_nand_title`, `io/dsi_nand_launch`). It saves where that
    NAND's DSi Menu sessions do, and a shortcut from another NAND is
    refused.
  - Headless: a `.dspr.nds` ROM with `--dsi-nand`, `--dsi-shortcuts DIR`,
    `--dsi-shortcuts-clear DIR`.
- **Ini keys:**
  - `paths.bios9i`, `paths.bios7i`, `paths.dsi_font`, `paths.dsi_sd`.
  - `paths.dsi_nand`: stands in for `--dsi-nand` **only under `--dsi-mode`**
    (USER DECISION).
  - `paths.dsi_firmware`: used whenever the machine is a DSi, and swapped in
    when DSiWare is picked from a DS session. `--firmware` wins.
  - `emu.dsi_hide_installed`: restart-only row HIDE NAND DSIWARE.
- **Recompiler settings.** `emu.jit`, `emu.quantum` and `emu.idle_skip`
  apply to DSi sessions as to DS games; `--interp`/`--lockstep` give the
  reference configuration. The recompiler's strict timing applies to the DS
  firmware menu only.
- **DSP warning.** A title that starts the DSP (`NDS::dsi_dsp_started`) gets
  the toast "GAME LIKELY WON'T WORK / DSP IS NOT EMULATED".
- **Headless:** `--dsi` is required with no ROM, or the machine is a DS and
  draws nothing. `DS_LAUNCH_AT=<frame>:<path>` (SDL) scripts a picker
  launch.

### 2.5 Save states

`FORMAT_VERSION` 3 holds every DSi addition. Version 2 still loads as a DS
state. A DSi state is the DS layout plus these chunks:
- **HEAD:** DSi or DS. A state loads only into the same machine.
- **NAND:** the sectors written since the *state base*, plus the base's
  identity.
  - The base is the NAND as the session built it, before imported saves
    (`NandImage::mark_state_base`).
  - The identity hashes the CID, console ID, size and the base's sectors.
    A state therefore refuses another dump, another installed or hidden
    title, and a hand-off NAND built from another font or other `[user]`
    settings.
  - Loading reverts every sector written since the base, then applies the
    state's.
  - A `--dsi-nand-write` session cannot save a state.
- **SDCD:** the SD card's in-memory sectors and extents. USER DECISION: the
  card is taken only if every backing host file is unchanged (size and
  mtime to the nanosecond). Otherwise the state loads with an **empty SD
  slot until reset**, with a warning (`NDS::sd_card_note`).
- **DSI / DSIH:** mic and SCFG state, the loader and exit flags, and
  `dsi_dsp_started`.
- **After a load:** `Bus::relink` re-applies SCFG_CLK9 and the VRAM timing.

Checked with frame, RAM and register hashes and save-after-load identity, in
card mode, the hand-off (with an SD card) and a real-NAND TLNC boot. The
refusals and the SD fallback were checked too. Unit tests:
`test_nand_state`, `test_card_state`.

### 2.6 SD card slot

USER DECISIONS: a host folder, not an image; guest changes are synced back,
deletions included; the card size is automatic.
- **Card image** (`io/dsi_sd_card.*`): built in memory at boot, with an MBR,
  FAT16 below 1 GB and FAT32 above. The size is content + 128 MB, rounded
  to the next power of two, at most 32 GB.
- **Data.** Files get cluster chains but no data. Unwritten sectors are read
  from the host file that owns them.
- **Skipped** (with a note): names FAT cannot hold, names that differ only
  by case, files of 4 GB or more, links.
- **Sync** (`SdCard::sync`): writes go to a temporary file, then a rename.
  Guest deletions are applied. A file that changed on the host is never
  overwritten; the card keeps its own copy in memory instead. SDL syncs on
  the NAND's occasions and never under a replay; headless syncs at exit.
- **Guest side:** a second `MmcStorage` on host 0 port 0, with melonDS's SD
  variants of the commands.
- **The test is System Settings (HNBE), Data Management.** Blocks Free
  matches melonDS on the same image, and copying a title to the card
  writes `HNB_.lst` byte-identical to melonDS's. The DSi Menu never touches
  port 0 during boot. DSi Sound reads the card, then stalls on the DSP.
- **Tools:** `DS_SD_DUMP=<file>`, `DS_SD_LOG=<file>`; trace_melonds `--sd`,
  `--autoload`.

### 2.7 Microphone

A port of melonDS DSi_I2S into `Io`:
- **Registers.** `MIC_CNT`/`MIC_DATA` with a 16-word FIFO. `IRQ2_MIC_EXT`
  fires on half-full and on overrun, alongside NDMA 0x2C.
- **Clock.** The samples are clocked by the SPU mixer and read from the
  frontend's per-frame capture buffer (`Io::mic_at`).
- **Output stage.** SNDEXCNT's output stage (enable, mute, NITRO/DSP ratio)
  is applied as in melonDS. Shantae's audio stays sample-identical over
  3000 frames.
- **Checked** with System Settings' Mic Test against melonDS (headless
  `--mic-tone`, trace_melonds `TRACE_MIC_TONE`). `DS_MIC_LOG=1` logs
  `MIC_CNT` writes.

### 2.8 Wi-Fi

- **Two radios.** DSi Connections 1-3 use the DS Wi-Fi block (`io/wifi.cpp`).
  Connections 4-6 and TWL-SDK titles use the Atheros module over SDIO
  (`io/dsi_nwifi.*`, melonDS DSi_NWifi). Both drive the slirp `NetDriver`.
- **The access point in the firmware** (USER DECISION): at every firmware
  load, `bios::stamp_access_point` writes an open DHCP profile `DSperate-AP`
  into the first unconfigured slot. It happens in memory only and never
  overwrites a player's network.
- **The Atheros module** advertises the same SSID and answers a directed
  scan.
- **Beacons are every 100 TU**, not melonDS's 131 ms. The DS block's passive
  scan listens for about 110 ms per channel. This is a deliberate departure,
  so Wi-Fi register traces diverge from the oracle at the first received
  beacon.
- **Checked:** the connection test passes on Connection 1 (DS block) and
  Connection 5 (Atheros, after a search) with `--internet --dns host`.
  `DS_DEBUG_NWIFI=1` logs WMI.

### 2.9 JIT and idle skip

Two bugs stopped every DSi session under both recompilers (`4a3ba16`):
- The translators fetched the unmapped DSi ARM7 BIOS as data and got
  0xFFFFFFFF back. They now use `Bus::fetch`.
- A SWI answered by `dsi_hle_swi` returned into a block that ended at the
  SWI.

Checked (aarch64 build under qemu, JIT against the interpreter, snapshots at
600/1200/1799):
- all 20 DSiWare titles on hand reach the same screens;
- NAND I/O is identical on 19 of them;
- so do the NAND boot to the DSi Menu and a TLNC launch, also with
  quantum 0 and idle skip on;
- the ARMv7 JIT matches on Shantae.

Strict mode (`DS_JIT_STRICT`) is **not** cycle-exact on a DSi. The DSi-only
melonDS parity rules (`code_latch`, `defer_cost`, `irq_skip_once`) are
interpreter-only. They matter for traces, not for play.

RG DS, unpaced, quantum 0, idle skip, 1800 frames of title and attract
screens (not gameplay):

| title | JIT median | JIT over budget | interp median | interp over budget |
|-------|-----------:|----------------:|--------------:|-------------------:|
| Shantae | 4.4 ms | 9.4 % | 9.3 ms | 12.9 % |
| Mario vs. Donkey Kong | 19.0 ms | 71 % | 37.5 ms | 99.6 % |
| Plants vs. Zombies | 3.8 ms | 3.2 % | 7.5 ms | 8.1 % |
| Petit Computer | 5.1 ms | 3.1 % | 9.9 ms | 5.6 % |
| Space Invaders Extreme Z | 7.0 ms | 7.3 % | 21.1 ms | 77.7 % |
| DSi Menu | 9.0 ms | 3.5 % | 39.8 ms | 69.8 % |

Found on the device and fixed in `f895c44`, with the interpreter affected
too: a NAND title launched from the DSi Menu hung on white whenever a card
was in the slot, which in SDL is always. The launcher waits for SCFG_MC
power state 0, and writes had been stored raw. melonDS's `SetScfgMC` power
states and power-off timer are now ported.

## 3. What is built (all on `dsiware`)

| area | where | state |
|------|-------|-------|
| Machine: 16 MB RAM, NWRAM/MBK, SCFG (per-width reads, SCFG_MC power states), IE2/IF2, NDMA, DSi CP15, BIOS protection, SNDEXCNT, DSi CODEC/TSC, GPIO, I2C + BPTWL | phase 1, `f895c44` | trace-exact vs melonDS in card mode |
| Soft reset (`NDS::dsi_soft_reset`; synthesised NAND: `exit_requested`) | `d026ef5`, `6b5e546` | not compared with melonDS; headless `DS_DSI_SOFT_RESET_AT` |
| Scheduler: melonDS 64/8 slice grid, 134/67 MHz ARM9, soft timers per CPU | `f8de6bf`, `50d4f87`, `e0b4f79` | exact through the launcher boot |
| Modcrypt; AES engine (CTR/CCM, NDMA) | `io/dsi_aes.*`, `tests/aes_test.cpp` | unit-tested; oracle titles never use it at runtime |
| SD/MMC host: port 1 NAND, port 0 SD card | `io/dsi_sd.*`, `io/dsi_sd_card.*` | raw sectors; `DS_NAND_LOG`, `DS_SD_LOG` |
| SDIO + Atheros | `io/dsi_nwifi.*` | connection test passes |
| Cameras (2 Aptina sensors, module at 0x04004200) | `io/dsi_camera.*` | black frames |
| DSP host interface, no core (`PSTS` 0x0100) | `io/dsi_dsp.*`, `9c94862` | a program start is flagged and warned about |
| NAND boot | `d30ed62`..`e0b4f79` | DSi Menu top screen pixel-identical at frame 1200 |
| Virtual NAND, install, persistence, TLNC | `7e1b954` | KD9E installs and launches; saves round-trip; dump untouched |
| Hand-off HLE, synthesised NAND and firmware | `1bc7eee`, `5e96574`, `6b5e546`, `nds_dsi.cpp`, `io/dsi_nand_synth.*`, `bios/firmware_gen.cpp` | 15/15 titles on the BIOS pair |
| System font + SWI 22h HLE | `456ed04`, `ca65833`, `io/dsi_font/` | titles accept it |
| SDL: CLI, loader list, loader cart on the DSi Menu, ini keys, shortcuts | `6b5e546`, `cd13967`, `e7e22d6`, `868c8f4` | checked on the SDL binary |
| Mic, Wi-Fi, SD, save states | `ec522df`, `dd943b3`, `15d58d9`, `e82e7d3`, `301f0c5` | 2.5-2.8 |
| JIT under DSi | `4a3ba16`, `78f9c51` | functionally equal to the interpreter |

Not built: DSP core, NWRAM dual-slot writes, the `0x02FE71B0`/SCFG_EXT
RAM-size hacks, JIT parity for the melonDS trace rules, `.app` streaming.

## 4. Corrections to the 2026-09-10 plan

- **"Synthesise the NAND instead of requiring a dump."** Not possible on
  the route that works, because boot2 and the launcher live on the NAND.
  The FAT builder now serves the installer, the hand-off NAND and the SD
  card.
- **"Titles decrypt saves through AES slot 3."** No. NAND crypto is
  software above a raw-sector SD host.
- **"The SD host encrypts per sector."** No; the guest path is raw.
- **"Cameras and SDIO/Wi-Fi can stay stubs."** No. The launcher configures
  both sensors and waits on the Atheros module, so both were ported whole.
- **"The CIA TMD's first 0x208 bytes are the DSi TMD."** No. Keep 0x1E4
  bytes, patch the title ID and save sizes, and append a 36-byte content
  record.
- **The ARM7 byte at 0x0380FFC8.** Not a mystery: melonDS `DSi::Reset` maps
  WRAMCNT 3.

## 5. Open items

Unchecked, before calling 2.0.0 done:
- gameplay past title screens, on device;
- the SDL save-state flow on device;
- an exported `.pub` imported into melonDS;
- non-USA titles in general, and a CN/KR title drawing with its font.

Fidelity against melonDS (none known to affect play):
- **2D render/present.** Rendered frames differ from about frame 25 of the
  NAND boot while the CPU slice grid matches. Not examined.
- **Touch sampling.** The grid splits at the Health and Safety tap (and at
  Settings taps).
- **FAT mtimes.** We stamp 00:00:00 where melonDS stamps 04:16:00. The RTC
  or the time read path is the suspect.
- **ARM7 start.** The hand-off starts both CPUs together; the real launch
  starts the ARM7 about a frame later.
- **Launcher data.** The title list at 0x02FFD800 is not understood.
- **Card-present NAND boot.** Launching from the menu with a card now works
  (`f895c44`). Trace parity (last split on SPIDATA at frame 37) has not been
  re-checked.
- **Save-state RAM gap.** A state saved after menu taps differs by about
  71 bytes of RAM 141 frames after the load (0x02FFFCD8 and a copy at
  0x02183480, touch samples by their look). Frames are unaffected.
- **Homebrew on the SD card.** The Unlaunch installer through the hand-off
  (title ID 0) crawls to about 22 frames in 15 minutes. This is a hand-off
  problem, not the card's.
- **SD extras.** Not built: a read-only card option, a card image as input.

## 6. Oracles and assets

Assets in `<project_root>/dsi-binary/` (never committed):
- `bios/biosdsi7.bin` and `biosdsi9.bin` (half dumps);
- `dsinand.bin`, with 15 DSiWare titles installed;
- `dsifirmware.bin`;
- `dsiboot.bin` (boot blobs);
- `games/*.cia`, with the extracted `.nds`/`.tmd`;

`games/*.bin` are console SD exports under the ES key; ignore them.

| title | code | why |
|-------|------|-----|
| Shantae: Risky's Revenge | KS3E | in NAND and CIA; saves; the card-mode gate |
| Mighty Flip Champs | KMGE | cross-title save read |
| Instrument Tuner | KTUE | mic, G.711 DSP ucode |
| Petit Computer | KNAE | 10 MB save, SD header bits |
| Dr Mario Express, Plants vs Zombies | KD9E, KZLE | CIA-only: installer targets |
| Mario vs DK: Minis March Again | KDME | 416 KB save |
| System Settings, DSi Sound | HNBE, HNKE | SD, mic, Wi-Fi tests (`--dsi-autoload-id 00030015484E4245`) |

All are USA titles from one console.

**Harness:** `dsperate-research/tools/melonds/trace_melonds --dsi <bios9i>
<bios7i> <nand>`, built with `melonds-trace-hook.patch`. Append hunks to the
patch; never regenerate it. The harness offers `--touch`/`--key`,
`--install`, `--sd`, `--autoload`, `--nand-inplace`, `TRACE_SLICES`,
`TRACE_TIME_FINE`, `TRACE_WATCH7` and `TRACE_SD_REGS`. `tools/dsi_nand.py`
has `info`, `titles`, `extract`, `map` and `bootblobs`. `tools/mkdsin.py`
turns the same touch/key arguments into a `.dsin` replay.

## 7. Verification

### 7.1 Gates, every commit

- **DS side:** the six DS scenes unmoved (`tools/all_scene_hashes.sh`),
  `tools/state_roundtrip.sh`, and the unit tests (24). The mlbis 600-frame
  hash moved on unmodified HEAD builds too, so re-baseline from HEAD before
  reading an mlbis diff.
- **DSi card-mode gate:** Shantae, 60 frames, both CPU traces byte-identical
  to melonDS, and 400 rendered frames identical:

      DS_IDLE_SKIP=0 DS_MELON_STM=1 DS_STORE_BUS=0 TRACE_TIME=1 TRACE_START_FRAME=20 \
        dsperate-headless --direct --interp --bios9 .. --bios7 .. --firmware dsifirmware.bin \
        --bios9i biosdsi9.bin --bios7i biosdsi7.bin --dsi-boot dsiboot.bin \
        --frames 60 --trace out/ours --max 40000000 "Shantae - Risky's Revenge.nds"
      TRACE_TIME=1 TRACE_START_FRAME=20 trace_melonds bios9 bios7 dsifirmware.bin out/melon \
        --dsi biosdsi9.bin biosdsi7.bin dsinand.bin --rom Shantae.nds --direct --frames 60 --max 40000000

  The three `DS_` knobs are gate-only: idle skip off, melonDS's ARM7 STM
  quirk, and melonDS's store pricing.

### 7.2 NAND boot to the launcher

    dsperate-headless --direct --interp --dsi --touch 600:128,100:10 \
      --bios9 bios9.bin --bios7 bios7.bin --firmware dsifirmware.bin \
      --bios9i biosdsi9.bin --bios7i biosdsi7.bin \
      --dsi-nand dsinand.bin --dsi-nand-boot --frames 1300 --dump-frames out/o.frames
    trace_melonds bios9.bin bios7.bin dsifirmware.bin out/m --touch 600:128,100:10 \
      --dsi biosdsi9.bin biosdsi7.bin out/n2.bin --nand-inplace --frames 1300 --max 1 \
      --dump-frames out/m.frames

Menu launch of Shantae uses the same command plus these touches, over 7000
frames:
- 14 scroll taps `F:60,113` for F = 800, 1000, ..., 3400;
- the launch tap `3600:128,113`;
- the title taps `5000/5600/6200:128,140`.

Frames match melonDS from frame 5235, and the 19 NAND writes are identical.
With 13 scrolls it launches Mighty Flip Champs instead.

### 7.3 Finding a divergence

Identical per-CPU traces do **not** mean an identical interleave.
1. Dump frames on both sides to see how far each gets.
2. **Slice-grid diff** (about a minute, no traces): compare `DS_DEBUG_SLICES=1`
   against `TRACE_SLICES=1` on the slice-end column.
3. At the first split, take full traces with `TRACE_START_FRAME`,
   `TRACE_TIME_FINE=1` and **both** `DS_TRACE_NODEDUP=1` and
   `TRACE_NODEDUP=1`. With only one set, the divergence is reported
   thousands of lines early. The CPU that diverges earlier in time is the
   cause.
4. Usual causes:
   - an unmodelled I/O page;
   - a register at the wrong width;
   - an IRQ one slice early or late (read IE&IF in the handler);
   - a quarter-cycle cost difference;
   - a DMA or stall length (`DS_DEBUG_DMA=2`).

   For a byte that changes, use `DS_WATCH7`/`TRACE_WATCH7`.

### 7.4 Traps

- Reset state that direct boot overwrites but real boot code reads back
  (EXMEMCNT, CP15, WRAMCNT). Check `DSi::Reset`, not only `NDS::Reset`.
- Anything that re-lays shared WRAM must re-apply NWRAM on top.
- `0x0380xxxx` is ARM7-only; never stage it through ARM9 bus writes.
- melonDS's `ScheduleEvent` does not re-arm a live event
  (`Scheduler::armed()`).
- Headless runs have an empty card slot. SDL always has the loader card, so
  test card-slot behaviour with a ROM inserted.
- Time a default-knob run too, and run the card-mode control before blaming
  new DSi code.
- Large buffers in `NDS` go on the heap, because unit tests put `NDS` on the
  stack.
- The headless DSi clock is stopped without `--rtc-host`. Without it,
  TWLCFG gets written on boot.
- Never hash or benchmark DSi with a DSi BIOS missing.

## 8. Risks

1. **Performance on device.** A title that saturates the 134 MHz ARM9 (Mario
   vs. Donkey Kong: 71 % of frames over budget on the RG DS) needs a
   dedicated campaign.
2. **The hand-off HLE is tuned to 15 USA titles from one console.** Titles
   that read unsynthesised `/sys` files, use the list at 0x02FFD800, or wait
   on the DSP will fail in new ways.
3. **Installer fidelity.** A wrong FAT or save geometry shows as "no save
   data" or a reformat. Gate against melonDS `ImportTitle`, sector for
   sector.
4. **Half-dump BIOS.** Code that reaches the missing upper halves is only
   noticed when it does.
5. **Timing model.** DSi RAM/NWRAM timings are melonDS's approximations, and
   exactness is measured against melonDS, not hardware.
6. **Console identity.** Exported saves carry the dump's console ID.
   melonDS does not care; a real console might.
