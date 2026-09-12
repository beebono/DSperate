# DSiWare support -- scoping

2026-09-10. What it takes to run DSiWare titles in DSperate, using melonDS
(checkout `dsperate-research/melonDS`, `d3cd6164`, release 1.1 line) as the
reference implementation. Written before any code; the point is to fix the
scope, the order and the oracles so the branch does not turn into "emulate the
whole DSi".

Scope is **DSiWare only**: titles with `unit_code & 2` and title-ID-high
`0x00030004`, launched straight from a `.nds`/`.app` dump. Booting the DSi
system menu from a NAND dump, DSi-enhanced retail cartridges in DSi mode,
cameras, the second card slot, DSi Wi-Fi and the SD card slot are all **out**.
Each is named below only so the design leaves the door open.

> **Current state (2026-09-12, branch `dsiware` @ `e0b4f79`) -- read this
> first; parts of the plan below are superseded.** The work turned into a
> NAND boot rather than a direct boot (section 3.2, and "The NAND boot path"
> under phase 3), and that pulled three "out of scope" items in: the NAND
> boot itself, the SDIO host with the Atheros Wi-Fi module, and the cameras.
> **A real, cartless NAND boot now runs boot2, the launcher and the Health
> and Safety screen, and after a tap renders the DSi Launcher menu** -- the
> same top screen as melonDS, with the CPU interleave (slice grid) identical
> to melonDS up to the tap. Card-mode direct boot is unchanged and still
> gate-identical to melonDS. What is *not* done yet: launching a title from
> the launcher, the synthetic NAND (section 3.1), DSP cores, mic, a network
> backend, a camera image source, and DSi validation of the JIT. The new
> status sections are "NAND boot to the launcher" (phase 3), phase 4 and
> phase 5 status; the method that found each bug is in section 6.

## 1. Verdict in one paragraph

DSiWare is not "a DS with more RAM". A direct-booted title still touches
roughly half of the DSi's new hardware: the NWRAM banks and MBK mapping, the
16 MB main RAM and doubled ARM9 clock, SCFG, IE2/IF2 with new IRQ sources,
NDMA, the AES engine (its own binaries are modcrypted, and its saves live on an
AES-CTR-encrypted NAND), the SD/MMC host in front of that NAND, the I2C power
IC, the DSi touchscreen/CODEC, SNDEXCNT, and for a minority of titles the Teak
DSP. melonDS spends ~12 k lines on this (plus 13.7 k of vendored teakra and
~5 k of fatfs). Our version is smaller because our bus is a page table and
because we skip the system menu, but it is still the largest single feature
since the 3D rasteriser: **five phases, the first four each exactness-gated
against melonDS**, and a real risk that the final 10 % (DSP, odd titles) never
closes. The design decision that keeps it tractable is the same one FreeBIOS
made: **synthesise the NAND** instead of requiring the user to dump one. Real
`bios7i.bin`/`bios9i.bin` stay required for now; a DSi FreeBIOS is a stretch
goal at the end, not a premise.

**Update (2026-09-12).** Two parts of that verdict did not survive contact.
"Direct boot plus a synthetic NAND" stalled at the launcher hand-off (phase
3), while booting the console's own NAND -- boot2 and the launcher, the path
melonDS DS uses for DSiWare -- turned out to need no reverse engineering, only
exactness. And "roughly half of the DSi's new hardware" was an undercount for
that route: the launcher also initialises the Wi-Fi module over SDIO and
probes both cameras, and waits on them. The exactness gate held up as the
right tool: every blocker on the way to the launcher was found by diffing
against melonDS, not by reading.

## 2. What we have, what melonDS has

### 2.1 DSperate today (the DS-only assumptions to break)

Everything below is from a read of `main` at `86fa64b`.

| area | where | DS assumption |
|------|-------|---------------|
| main RAM | `bus.h:79`, `bus.cpp:105,300,349,363,463` | `MAIN_RAM_SIZE = 4 MB`, mirrored over `0x02000000..0x03FFFFFF`; the `MEM ` state chunk is sized by it |
| shared WRAM | `Bus::update_wram` `bus.cpp:126-154` | the four WRAMCNT modes; `io.wramcnt` is one `u8` |
| address decode | `Bus::io_write` `bus.cpp:365-388`, `io_read` `:348-358` | `switch (addr >> 24)` on 4/5/6/7; nothing at `0x04004xxx` |
| MMIO | `Io::read/write` `io.cpp:1051,1064`, `io_unowned` `:1020` | dense `switch` per width; everything `>= 0x1070` falls through to 0 |
| IRQ | `CpuIo{ime,ie,if_}` `io.h:46`, `Irq` 0..24 | one IE/IF word |
| DMA | `dma::Dma`, `std::array<Channel,8>` `dma.h:83`, `u8 running_mask_[2]` | 4+4 channels, 8-bit masks |
| timing | `Timing::reset` `timing.cpp:22-59`, tables per 16 KB/4 KB/32 KB | DS regions and wait states only |
| scheduler | `scheduler.h:23,179` | ARM7 is exactly half the ARM9 clock (`running_shift_`) |
| ARM9 BIOS | `bus.h:86`, `bus.cpp:109-111` | 4 KB at `0xFFFF0000`, mirrored |
| ARM7 BIOS | `bus.h:87` | 16 KB at 0 |
| cart header | `cart::Header` `cart.h:16-30`, `cart.cpp:62` | packed 0x160 bytes; `unit_code` declared, never read; nothing past 0x160 |
| direct boot | `NDS::setup_direct_boot` `nds.cpp:354-427` | DS addresses (`0x027FFxxx`), DS CP15 dump, no ARM9i/ARM7i |
| BIOS loading | `NDS::load_bios` `nds.cpp:90-121` | exact 4 KB / 16 KB sizes; FreeBIOS fallback |
| SPU | `spu.h` | 32768 Hz only, SOUNDBIAS applied, no SNDEXCNT |
| SPI TSC | `io.cpp` | DS TSC only |
| crypto | `Cart::key1_*` `cart.cpp:100-160` | Blowfish only; **no AES, no SHA-1** (rcheevos ships `rhash/aes.c` but we do not compile it) |
| RA memory | `cheevos_memory.h` | the 12 MB "DSi hole" is deliberately unbacked |
| save states | `state.h:14-27`, `FORMAT_VERSION = 2` | chunk layout depends on `MAIN_RAM_SIZE`; `HEAD` carries `bios_id`/`firmware_id` |

The GPU has no DSi differences to model (melonDS has none either; the only
one is VRAM bus width via `SCFG_EXT[0]` bit 13, which is timing).

### 2.2 melonDS's DSi (what we are porting the *behaviour* of)

| file | lines | models |
|------|-------|--------|
| `DSi.cpp/.h` | 3927 | the machine: reset, NAND boot, `SetupDirectBoot`, `SoftReset`, NWRAM/MBK, SCFG, bus overrides, modcrypt |
| `DSi_NDMA.cpp/.h` | 510 | 4+4 NDMA channels at `0x04004100/4104` |
| `DSi_AES.cpp/.h` | 697 | AES engine at `0x04004400`: CCM/CTR, 4 key slots, FIFOs with NDMA hooks |
| `DSi_SD.cpp/.h` | 1407 | two SD hosts (`0x04004800` SDMMC: SD card + NAND; `0x04004A00` SDIO: wifi); the MMC card model |
| `DSi_NAND.cpp/.h` + `DSi_TMD.h` | 1671 | NAND image: nocash footer, FAT AES-CTR crypto, ES (ticket) crypto, fatfs mount, title import/export, TWLCFG/HWINFO |
| `DSi_I2C.cpp/.h` | 786 | I2C host + BPTWL power IC (+ cameras) |
| `DSi_I2S.cpp/.h` | 351 | MICCNT/MICDATA FIFO and **SNDEXCNT** |
| `DSi_SPI_TSC.cpp/.h` | 272 | DSi TSC/CODEC, DS-compat mode |
| `DSi_DSP.cpp/.h` + `DSP_HLE/` | 2973 | Teak host interface; HLE for AAC/G.711/graphics ucodes, LLE via teakra |
| `teakra/` | 13 734 | vendored Teak core |
| `fatfs/` + `sha1/` | ~23 000 | vendored FAT and SHA-1 |
| `DSi_Camera.cpp/.h`, `DSi_NWifi.cpp/.h` | 2841 | planned out of scope; **ported 2026-09-12** because the launcher waits on both (`io/dsi_camera.*`, `io/dsi_nwifi.*`) |
| `NDS_Header.h`, `Args.h`, `MemConstants.h`, `NDS.cpp` bits | -- | DSi header fields, `IE2/IF2`, `ARM9ClockShift`, `MainRAMMask` |

The two facts that shape the plan:

1. **melonDS's "direct boot" of a DSiWare title does not go through NAND.**
   `DSi::SetupDirectBoot` (DSi.cpp:470) stages the title from the cart image:
   MBK words from ROM offset 0x180, header copies at `0x02FFFA80`/`0x02FFFE00`
   (0x160 bytes) and 0x1000-byte copies at `0x02FFC000`/`0x02FFE000`, ARM9i/ARM7i
   loaded when `DSiCryptoFlags & 1`, modcrypt undone in place, a fixed DSi CP15
   dump, `SNDEXCNT = 0x8008`, `SCFG_EXT = {0x8307F100, 0x93FBFB06}`. NAND is only
   consulted for three blobs: TWLCFG bytes `0x88..0x1AF` to `0x02000400`,
   HWINFO_N to `0x02000600`, HWINFO_S to `0x02FFFD68`. All three are
   synthesisable.
2. **The title's saves are files on the NAND FAT** (`0:/title/00030004/<id>/data/public.sav`,
   `private.sav`, optional `banner.sav`), read by the title through the SD/MMC
   host and decrypted by the title's own SDK code through the **AES engine
   key slot 3**, whose keys derive from the ConsoleID (`DSi_NAND.cpp:42-112`,
   `DSi_AES::Reset`). So a save-capable title needs the SD/MMC host, the AES
   engine, and a NAND whose FAT is encrypted under a ConsoleID we also present
   through the AES key slots. Nothing forces that NAND to be a real dump.

## 3. The design decisions

### 3.1 Synthesised NAND, not a dump (the FreeBIOS move)

melonDS requires `nand.bin` (a 240 MB console-unique image with a nocash
footer) and installs titles into it with a TitleManager. That is the right
thing for a full-system emulator and the wrong thing for us: a handheld user
has a folder of `.nds` files and no NAND.

We build, at load, an in-memory NAND containing exactly one title:

- a ConsoleID and eMMC CID of our choosing (fixed constants, so the FAT key,
  `FATIV = bswap128(SHA1(CID))` and the AES slot-3 keys are compile-time data);
- a FAT16 volume with `0:/title/00030004/<id>/{content,data}`,
  `content/title.tmd` and `content/<ver>.app` (the ROM bytes themselves, so no
  copy: the FAT cluster chain points into the `RomSource`), and
  `data/public.sav` / `private.sav` / `banner.sav` sized from header
  `0x238`/`0x23C`/`AppFlags & 4`, each a FAT12 volume built exactly as
  `NANDMount::CreateSaveFile` (DSi_NAND.cpp:952) does it -- the geometry maths
  there is credited to NTM and is what titles expect;
- `0:/shared1/TWLCFG0.dat`/`TWLCFG1.dat` (432 bytes, SHA-1 hashed) and
  `0:/sys/HWINFO_S.dat`/`HWINFO_N.dat`, generated from the same user settings
  our `firmware_gen` already produces (nickname, colour, birthday, language);
- **no ticket**: a direct-booted title never reads `0:/ticket`. The ES key (and
  therefore the `bios7i[0x8308]` keyY) is not needed on this path.

Encryption is done lazily per 512-byte sector when the SD host reads it
(AES-CTR with counter `sector_addr >> 4`), so the image never exists in the
clear or in full. Only the save files are writable; their sectors are backed by
host files `<saves>/<GAMECODE>.pub` / `.prv` / `.bnr` (the melonDS
TitleManager's export extensions, so saves round-trip with melonDS through its
export/import dialog) flushed on the same quiet-period rule as `.sav`.

This is a 512 KB..8 MB image instead of 240 MB, needs no fatfs (we write a FAT
once with a hand-rolled builder the way `cart/zip.cpp` hand-parses zips), and
turns "install a title" into "open the file".

A real `nand.bin` stays supported as an **opt-in oracle** (`[paths] dsi_nand`):
same SD/MMC host, sectors read through the same AES-CTR path with the CID and
ConsoleID from the nocash footer, and the title must already be installed in
it. That is how phase 3 is verified bit-for-bit against melonDS before the
synthetic NAND replaces it; it is not the product.

### 3.2 Direct boot only

No boot ROM, no boot2, no launcher -- but the state we hand the title is the
one the *launcher* leaves, not melonDS's direct-boot state (phase 0 showed
melonDS's gives DSiWare no NAND access). `DSi::LoadNAND`'s boot2 shortcut
(hard-coded boot2 key, BIOS fragments copied into ITCM, MBK values from NAND
offset 0x380) exists in melonDS to reach the system menu; we do not need it,
and the `FullBIOSBoot` path additionally needs the rare full 64 KB dumps.
Our entry is a DSi-flavoured `setup_direct_boot` matching melonDS's DSi
branch line for line; melonDS-with-the-same-title is then an instruction-trace
oracle from frame 0.

**Superseded (2026-09-12).** Direct boot runs a title in card mode, and the
launcher hand-off recipe that gets it onto the NAND wedges (phase 3 status).
The route that works is the one melonDS DS takes: boot the console from its
NAND (`--dsi-nand-boot`, melonDS's `!FullBIOSBoot` boot2 shortcut, with half
BIOS dumps). It now reaches the DSi Launcher; launching a title from there,
or auto-launching one with the `TLNC` block, is the remaining step. Direct
boot stays as the card-mode path and the phase-1 exactness gate.

### 3.3 Real DSi BIOS dumps required (for now)

`bios7i.bin`/`bios9i.bin` (64 KB each). Titles call BIOS SWIs and the ARM7
BIOS carries the Wi-Fi/sound init code paths that titles jump through. melonDS
accepts the widely circulated low-32 KB dumps for direct boot (it only needs
the full dumps for `FullBIOSBoot`), and so will we: pad to 64 KB, record which
we got. `load_bios` grows a second pair of paths; sizes are enforced exactly as
today; the FNV `bios_id` in the state `HEAD` covers both pairs so a DSiWare
state cannot resume under DS FreeBIOS (the `cross-bios` class of hang).

Stretch: a DSi FreeBIOS. The DS FreeBIOS covers the SWI table; the DSi BIOS
low half is the same set plus the AES/SHA-1 helper entry points that
`bios9i[0x8B8C]`-style key material sits beside. Not scoped; noted so the
`bios_native` plumbing does not assume it can never happen.

### 3.4 Per-title console type, decided by the header

melonDS makes console type a user setting. We decide it from the ROM:
`unit_code & 2` **and** `title_id_high == 0x00030004` selects the DSi machine;
everything else stays exactly the DS machine it is today (same code paths,
same hashes -- section 6 makes that a gate). DSi-enhanced cartridges
(`unit_code & 2`, title-ID-high `0x00030000`) are run in DS mode as now, which
is what melonDS's "bad dump mode" does for region-less DSi headers anyway.

### 3.5 DSP: HLE for known ucodes, LLE deferred

Most DSiWare never touches the DSP. Those that do load one of a handful of SDK
ucodes; melonDS identifies them by CRC32 of NWRAM bank B and HLEs three
families (AAC, G.711, "graphics"), falling back to teakra otherwise. We take
the HLE table and the fallback becomes "DSP absent: log once, `DSP_PSTS` never
ready", which is what a title sees on a DSi with the DSP held in reset.
Vendoring teakra (13.7 k lines, its own CMake) is a decision for after phase 4
when we know how many titles in the user's library actually need it. It is not
a hot loop on our targets either way: a Teak at 134 MHz interpreted on an A55
is a full core.

**Status (2026-09-12).** The "DSP absent" half is in: `io/dsi_dsp.*` is
melonDS's host interface with no core attached (see phase 5 status). The
launcher reads `DSP_PSTS` during boot and only needs that.

## 4. Phases

Each phase ends with the DS scenes bit-identical (`tools/all_scene_hashes.sh`)
and a `state_roundtrip.sh` pass. Phases 1..4 additionally end with a
melonDS-trace comparison on the DSiWare oracle titles (section 6). Line counts
are estimates from the melonDS equivalents scaled for our page-table bus.

### Phase 0 -- dumps and oracles (mostly done 2026-09-10)

Assets now live in `<project_root>/dsi-binary/` (outside the repo, never
committed):

| file | verified |
|------|----------|
| `bios/biosdsi7.bin`, `biosdsi9.bin` (64 KB each) | low-32 KB CRC32s match melonDS's `ARM7iBIOSLowCRC32`/`ARM9iBIOSLowCRC32`; the full-image CRCs do **not**, so these are the common half dumps: melonDS direct boot works, `FullBIOSBoot` does not. Exactly the class section 3.3 plans for |
| `bios/dsinand.bin` (240 MiB + 64) | nocash footer present; FAT key derived from its ConsoleID decrypts the MBR and the `TWL` FAT16 partition (`tools/dsi_nand.py info`) |
| `bios/dsifirmware.bin` (128 KB) | the DSi-mode DS firmware melonDS's Qt frontend wants; we do not need it for direct boot |
| `games/*.cia` | 3DS-format CIAs whose single content is the **plain, unencrypted DSiWare SRL**; extracted beside them as `.nds` + `.tmd` |
| `games/*.bin` | the DSi's own SD-card exports: AES-CCM under this console's ES key, no DS header. Ignore; melonDS cannot import them either |

The three CIA titles, all USA, all DSiWare (`unit_code 3`, title-ID high
`00030004`), all modcrypted (`0x1C = 0x3`), all `public.sav = 16 KB`, no
`private.sav`, and all with **`AppFlags & 1` set** -- so they run the
touchscreen in DSi mode, which promotes the DSi TSC from "port it anyway" to
required in phase 1:

| title | code | SRL | ARM9i / ARM7i | in NAND? |
|-------|------|-----|---------------|----------|
| Dr Mario Express | KD9E | 3.9 MB | 11 KB / 297 KB | no |
| Plants vs Zombies | KZLE | 15.7 MB | 21 KB / 300 KB | no |
| Shantae: Risky's Revenge | KS3E | 16.7 MB | 25 KB / 300 KB | **yes**, `00000001.app` bit-identical to the CIA SRL |

The NAND itself has **15 DSiWare titles installed** under `/title/00030004`
(`tools/dsi_nand.py titles`), each with `title.tmd`, `.app` and a
`public.sav`; two are interesting for the save path beyond the 16 KB norm:
KNAE with a 10 MB `public.sav` and KDME with 416 KB. They can be extracted
with `dsi_nand.py extract` for more oracle titles (the user's own console).
The 3DS-style CIA TMDs (2868 bytes, title-ID high `00048004`) are **not** what
`melonDS::ImportTitle` wants: it takes the 520-byte DSi TMD with the
`00030004` ID. For an install into the oracle NAND, keep the first 0x1E4 bytes,
patch the title ID at 0x18C, fill the save sizes at 0x1A0/0x1A4 from the SRL
header, and append one 36-byte DSi content record (the 3DS chunk at 0xB04 has
a SHA-256 and the wrong stride); melonDS never checks the signature.
`dsperate-research/tools/melonds/cia_to_srl.py` does this, and decrypts
title-key-encrypted content (3DS common key 0) on the way.

The oracle harness (done 2026-09-10): `dsperate-research/tools/melonds/
trace_melonds --dsi <bios9i> <bios7i> <nand.bin>` builds a `DSi` with
`DSPHLE`, works on a copy of the NAND, logs every eMMC block to
`<prefix>.nand.log` (resolve with `tools/dsi_nand.py map`) and every SD host
register access with `TRACE_SD_REGS=1`, takes scripted input (`--touch
F:x,y[:N]`, `--key F:mask[:N]`), and dumps main RAM plus a full melonDS
savestate the first time the ARM9 reaches an address (`--ram-at-pc9
02004800:file --state-file f.mln`). The melonDS patches are
`tools/melonds/melonds-trace-hook.patch`.

**What it found, and it changes section 3.2.** melonDS's own DSi direct boot
runs a DSiWare SRL in *card mode*: it writes the boot indicator
`0x02FFFC40 = 1` and `SCFG_EXT7 = 0x93FBFB06` (the SDMMC gate, bit 18, clear).
All three titles therefore never touch NAND: Plants vs Zombies shows "The save
data could not be accessed", Dr Mario Express stays black, Shantae plays but
cannot save. Forcing indicator 3 and the gate open is *not* enough -- the
titles then hang with the ARM7's SD thread never scheduled and the ARM9 polling
ready bits at `0x02FFFF88`. So **melonDS direct boot is not the save oracle**.

The launcher path is. Booting from NAND reaches the DSi menu; tapping through
(health screen at frame 400, then `(60,113)` every 200 frames scrolls one icon,
Shantae is the 14th, `(128,113)` launches) starts Shantae at frame 3791, and
after "Touch to Start" it reads and writes
`/TITLE/00030004/4B533345/DATA/PUBLIC.SAV` (24 reads, 7 writes), also reading
Mighty Flip Champs' save (cross-title unlock) and every title's `DATA/` dir.
Main RAM at the title's ARM9 entry differs from the direct boot in only 13
256-byte blocks: `0x02000400` (TWLCFG copy, the launcher blanks some fields),
`0x023FEE00` (launcher ARM7 code left behind), `0x02FFD7B0` (the launch
parameter block melonDS marks TODO: `"00000009"`, a title-ID list),
`0x02FFFC40` (3 vs 1), and the header copies at `0x02FFFA80`/`0x02FFC000` that
melonDS writes and the launcher does not. The ARM9 also starts in system mode
(CPSR `0x2000009F`, SP in DTCM at `0x0E003F80`) rather than melonDS's SVC/`0xD3`.

**The hand-off, reverse-engineered (2026-09-10, later the same day).** Bisecting
the launcher state against the direct boot found that none of the RAM blocks,
entry registers, ITCM contents, SCFG_BIOS/CLK9/MC values, SCFG lock or CPU
start order matter. Four things do, and with them a direct boot of Shantae
and of Mighty Flip Champs (KMGE, extracted from the NAND) reads its `.app`
and `public.sav` from NAND and writes the save back, block for block like the
launcher launch:

1. **Boot indicator** `0x02FFFC40 = 3` (melonDS writes 1 = card).
2. **`SCFG_EXT7` bit 18 set** (SDMMC access; melonDS's `0x93FBFB06` has it
   clear). Locking SCFG (bit 31) is irrelevant: the title's ARM7 crt0 builds
   its own boot info from SCFG when it finds it unlocked.
3. **The launcher's parameter block in ARM7 WRAM, at the address in DSi
   header word `0x1D4`** (GBATEK's "pointer to base address where various
   structures and parameters are passed to the title"; `0x03800EA8` for
   Shantae, `0x038023B8` for KMGE). It is a mount table of five 0x54-byte
   entries -- `u32 header`, 16-byte name, 0x40-byte path -- followed by the
   title's app path at +0x3C0:

   | header | name | path |
   |--------|------|------|
   | `00008141` | `nand` | `/` |
   | `0000A142` | `nand2` | `/` |
   | `00041144` | `shared1` | `nand:/shared1` |
   | `00063146` | `photo` | `nand2:/photo` |
   | `00060948` | `dataPub` | `nand:/title/<hi>/<lo>/data/public.sav` |
   | (+0x3C0) | | `nand:/title/<hi>/<lo>/content/<version>.app` |

   The header words are copied verbatim (meaning unknown). A title with a
   `private.sav` presumably gets a `dataPrv` entry; none of ours has one, so
   that is unverified. `dsperate-research/tools/melonds/dsiware_params.py`
   builds this image from a header alone, and the synthesised block boots
   Shantae identically to the launcher's.
4. **Eight bytes at `0x0380FFC4`**: the `SCFG_EXT7` value (`0x13FFFF06`) and
   two flag bytes `0x44 0xF8`. The crt0 checks `[0x0380FFC8] & 0xC == 4` and
   halts via SWI otherwise; `0x40` there is `SCFG_BIOS` bit 10 as the launcher
   sets it.

Two facts fall out for the synthetic NAND (section 3.1): the title **reads its
own `.app` from NAND** through this table (18 859 block reads for Shantae's
title screen -- overlays and file-system data come from the NAND copy, not
from the cart image), so the FAT entry for the app must be complete and point
at the ROM bytes; and the paths are what the title opens, so the synthetic
image needs exactly `/title/<hi>/<lo>/content/<version>.app` and
`data/public.sav` (plus `/shared1`, which must exist even if empty).

Consequences for our design: `setup_direct_boot` in DSi mode is melonDS's
staging plus the four items above; the exactness oracle for every DSiWare
gate is the launcher launch, which the harness scripts, and a direct boot
with this recipe now matches it in NAND traffic.

**DSP oracle (2026-09-10).** None of the 15 NAND titles nor the three CIAs
uses the DSP: no `DSP1` ucode header, no ucode file in any NitroFS, and no
aligned DSP-register literal in any plain ARM9 binary (the ARM9i binaries are
fully modcrypted *and* BLZ-compressed, so the "scan ARM9i for `0x04004300`"
idea only works after both are undone; `dsperate-research` now carries the
modcrypt/BLZ steps in `tools/melonds/`). melonDS's own DSP HLE knows exactly
three ucode classes (AAC, G.711, Graphics, 20 CRCs in `DSi_DSP.cpp`) and names
only Let's Golf (Graphics) and the DSi Sound app (AAC); its issue #2367 is the
de-facto list of DSP titles, nearly all camera-driven. **Nintendo DSi
Instrument Tuner (KTUE)** is the exception: mic-only, and its ARM9i embeds the
**G.711 SDK ucode v4** (`DSP_HLE: CRC = 2A1D7F94`), which melonDS HLEs. The
user's CIA is title-key encrypted; `tools/melonds/cia_to_srl.py` decrypts it
and builds the 520-byte DSi TMD, `trace_melonds --install` imports it into the
NAND copy, and with the hand-off recipe it reads its `.app` and `PUBLIC.SAV`
and re-loads the ucode on every DSP restart. It is the phase-5 oracle.

**Large-save oracle (2026-09-10): Petit Computer (KNAE, 10 MB `public.sav`,
the only header with SD-card access bits).** Extracted from the NAND
(`dsi_nand.py extract`) and direct-booted with the hand-off recipe it reaches
its home menu by frame 300 with no DSP and no SD-card host traffic (SD/MMC
port 0 only). The save is not opened at boot: the scripted path
`--touch 400:80,104` (File Management) then `--touch 700:60,40` (Rename) opens
it and reads 24 blocks of `PUBLIC.SAV` for the file list. Mario vs. Donkey Kong:
Minis March Again! (KDME, 416 KB save) is kept as the popular "make sure it
works" title for later, not an oracle.

Phase 0 is complete: oracle set = Shantae (KS3E, saves), Mighty Flip Champs
(KMGE, cross-title save read), Instrument Tuner (KTUE, G.711 DSP + mic),
Petit Computer (KNAE, 10 MB save + SD bits).

### Phase 1 -- the machine, exact (~1 500 lines)

Everything a title touches before it does any I/O of its own. All new state
behind a `dsi_` flag on `NDS`; DS mode must not change a byte.

1. **Header.** Extend `cart::Header` reading to 0x1000 with a `TwlHeader`
   view (MBK words at 0x180, ARM9i/ARM7i offsets/loads/sizes at 0x1C0,
   modcrypt areas 0x220, title ID 0x230, sav sizes 0x238/0x23C, `AppFlags`
   0x1BF, hashes). `rom_identity` stays 0x160 bytes; fine.
2. **Main RAM 16 MB, unmirrored.** `MAIN_RAM_SIZE` becomes a per-machine
   size; the `map_page_aligned(..., 0x03000000)` mirror loop, the watch masks
   and the `MEM ` chunk follow it. `FORMAT_VERSION` 3.
3. **NWRAM.** Three 256 KB banks, `MapNWRAM_A/B/C` and `MapNWRAMRange`
   ported as written (melonDS re-derives the whole mapping in fixed slice
   order so results are independent of write order -- keep that), then
   `PageTable::remap` of the `0x03000000..0x03FFFFFF` window per CPU and a
   `jit::flush_range` on the moved pages. Replaces `update_wram`'s switch
   when `dsi_`; MBK9 write protection lands as RO mappings.
4. **SCFG.** `SCFG_BIOS/Clock9/Clock7/EXT[2]/MC/RST` as registers, with
   `CheckIO9Access/CheckIO7Access` semantics so unmapped `0x04004xxx` pages
   read open-bus as they do on hardware. `SCFG_EXT[0]` bit 13 flips the
   VRAM wait state in `Timing`.
5. **ARM9 clock.** `ARM9ClockShift` 2: the scheduler's `running_shift_` gains a
   DSi setting, `Timing::reset` gets the DSi region table (main RAM, NWRAM,
   16 MB), and `SetScfgClock9` rescales timestamps like melonDS. This is the
   one change that touches the JIT cost tables' meaning; run `test_jit` under
   qemu after it (the stride-refill lesson).
6. **IE2/IF2** at `0x04000218/21C`, mask `0x7FF7`, folded into `update_irq`
   and the JIT's IRQ check; new IE bits 24..31 (DSP, camera, NDMA0..3).
7. **NDMA.** A sibling of `dma::Dma` with its own `Channel` (block/sub-block,
   fill, 32-bit only), started from the same points `RunSystem` starts DMA,
   with the `NDMAModes` translation table and the GX-FIFO stall hook.
8. **CP15 and boot.** `setup_direct_boot` grows the DSi branch: melonDS's CP15
   dump (control `0x00056078`, 16 MB region `0x0E00001B`, DTCM `0x0E00000A`),
   the `0x02FFxxxx` header copies, `0x02FFFC40 = 1`, `0x02FFFDFA/B`, the wifi
   board words at `0x020005E0`, POSTFLG, `ARM7BIOSProt = 0x20`, SNDEXCNT
   `0x8008`. TWLCFG/HWINFO blobs come from the generator (phase 3 ships the
   NAND copy of the same bytes).
9. **BIOS.** Second BIOS pair, 64 KB each, mapped at `0xFFFF0000` (no 4 KB
   mirror) and 0; `SCFG_BIOS` bits 0/8 hide the upper halves as on hardware.
10. **SPU:** `SNDEXCNT` (bit 15 enable, 14 mute, 13 = 47605 Hz, bits 0-3
   NITRO/DSP ratio) and SOUNDBIAS ignored in DSi mode. The frontend's audio
   pacer already resamples nothing -- 47 kHz output means a second `SAMPLE_RATE`
   the SDL device opens at, decided at load.
11. **TSC:** the DSi CODEC on SPI, bank-switched register file, `SetMode(0)`
   DS-compat mode. Direct boot leaves it in DS mode unless `AppFlags & 1`, and
   all three of our titles set that bit, so this is required, not optional
   (272 lines in melonDS).

Gate: a no-save, no-DSP DSiWare title runs to its title screen with the
melonDS instruction trace matching, and nothing else's hashes moved.

#### Phase 1 status (2026-09-11)

Landed on branch `dsiware` (uncommitted): items 1-11 above, plus what the
trace gate turned up that the list did not name: the DSi GPIO register file,
the I2C host with the BPTWL power IC, `WIFIWAITCNT`, `BIOSPROT` and ARM7 BIOS
protection, melonDS's 8 us Wi-Fi timer (USCOUNT/USCOMPARE, beacon and
command counters, the power-on countdown), and a long list of scheduler
behaviours (below).

**Result.** Shantae (KS3E) card-mode direct boot is instruction- and
register-identical to `trace_melonds --dsi` on both CPUs for the first 60
frames (23.4 M ARM9 / 0.4 M ARM7 lines with timestamps), and the rendered
frames are identical for 400 frames. Without a NAND the title reaches its
"Save Data has been corrupted" prompt (frame ~200 on), on both emulators
alike -- the title screen proper needs the save file, i.e. phases 2-3.
Save states round-trip (re-save byte-identical, 100/100 post-load frames
identical); `test_jit` passes under qemu; the 20 unit tests pass.

**Gate command.** Headless, interpreter, lockstep quantum:

    DS_IDLE_SKIP=0 DS_MELON_STM=1 DS_STORE_BUS=0 TRACE_TIME=1 TRACE_START_FRAME=20 \
      dsperate-headless --direct --interp --bios9 .. --bios7 .. --firmware dsifirmware.bin \
      --bios9i biosdsi9.bin --bios7i biosdsi7.bin --dsi-boot dsi-binary/bios/dsiboot.bin \
      --frames 60 --trace out/ours --max 40000000 "Shantae - Risky's Revenge.nds"
    TRACE_TIME=1 TRACE_START_FRAME=20 trace_melonds bios9 bios7 dsifirmware.bin out/melon \
      --dsi biosdsi9.bin biosdsi7.bin dsinand.bin --rom Shantae.nds --direct --frames 60 --max 40000000
    cmp out/melon.arm9.trace out/ours.arm9.trace; cmp out/melon.arm7.trace out/ours.arm7.trace

The three env knobs are gate-only: `DS_IDLE_SKIP=0` turns the idle-loop
skips off, `DS_MELON_STM=1` reproduces melonDS's ARM7 STM quirk (a non-first
base register without writeback stores the slot address), `DS_STORE_BUS=0`
prices cacheable ARM9 stores as hits the way melonDS does.

**What the DSi machine does differently from the DS (all DSi-only, chosen
so the DS scene hashes stay put).** The scheduler mirrors melonDS's loop:
64-ARM7-cycle targets with an 8-cycle margin (an event strictly inside the
margin extends the slice, equal does not), the ARM9's core time floored to
ARM7 cycles with the remainder carried, an ARM9 slice cut by an event the
ARM9 schedules earlier than its end, no idle extension of the slice, RTC
32768 Hz and camera-IRQ grid events, the SPU mixing one event per sample,
cart words as events counted from the ARM7's clock, DMA progress folded into
`now()` (a cart word read by a DMA schedules the next from the DMA's own
position), an ARM9 DMA given its own iteration (the ARM9 phase ends when the
DMA stops, the ARM7 catches up to that point, and a DMA the ARM9 starts runs
in the next iteration -- possibly after a zero-length phase), and the cost
of an instruction that starts a DMA, halts, or takes a wake-up IRQ charged
after the next instruction (melonDS's pending `Cycles`). Timers are "soft"
events stepped at slice ends, an ARM7 overflow being seen from the ARM7's
overshoot position; an IRQ raised while its CPU is off-slice or DMA-stopped
is taken after that CPU's next instruction (immediately if it was halted);
the LCD IRQ delay is 0; the ARM7 BIOS is fetched through the slow path so
data reads see the protection rules; the Wi-Fi timer runs whenever POWCNT2
powers the Wi-Fi.

**DS-visible changes made on the way (hardware-correct, they move hashes):**
SWP charging read N + write N (sm64 from frame 256, dbori frame 243: both
traced to the `swp` at `020ba874`), `WIFIWAITCNT` wait states and `0x0030`
at direct boot (sm64 from frame 38), the cart-done IRQ going to the slot
owner only, a `W_IE` write raising the Wi-Fi IRQ when IF&IE becomes
non-zero, W_USCOUNT/W_USCOMPARE/W_CMDCOUNT reading back their counters, the
timer prescaler phase surviving a control rewrite, DISPSTAT bit 6 read-only
(none of the last five moved a scene). etody's one-frame shift at frames
110-199 is its known timer race (docs: etody-timer-race) and did not follow
any single change. mlbis and meteos are identical over 1800 frames.

**Found on the way, fixed on main (eb39c68):** `dma.cpp`'s 32-bit main-RAM
*write* burst tables were melonDS's 16-bit ones (`MRAMWrite16Bursts[1]/[2]`
where `MRAMWrite32Bursts[0]/[1]` = `{9, 4x59}` / `{9, 3x79}` belong). The
path fires thousands of times per scene on four of the five DS scenes and
none of their 1800-frame hashes moved; the DSi trace stayed identical.

**Fixed after the gate (a1e8967):** with the idle skip on (the default; the
gate turns it off) Shantae's frame 7 took 98 s of host time. On the DSi the
SPI busy bit is a flag cleared by the SPI event, and the ARM7 can pass
`spi_ready_at` inside its slice before that event fires; the SPICNT
poll-streak charge then underflowed, its `s32` cast went negative, and the
ARM7 was *given* budget every poll. The charge is now taken only while the
stamp is ahead. No DS hash moved (the DS prices busy by time).

**Not modelled yet (no DSiWare oracle hit them):** NWRAM dual-slot writes,
the `0x02FE71B0` and SCFG_EXT RAM-size hacks, mic and SD-card pages (read
0; the camera, DSP and SDIO pages have since landed -- phases 3-5 status),
Wi-Fi frames (a TX request is logged), the BPTWL soft-reset
request (logged), the DSi frontend/SDL wiring, JIT parity for `code_latch`,
`irq_skip_once` and `defer_cost` (the interpreter is the gate; the JIT
falls back to the interpreter for the unmapped DSi ARM7 BIOS).

### Phase 2 -- AES and modcrypt (~700 lines)

- Software AES-128 (CTR, CCM, and the `DeriveNormalKey`/`ROL16` scrambler).
  Enable rcheevos' `rhash/aes.c` or write the 300 lines; either is fine, the
  hot path is CTR over a few hundred KB at load and per-sector NAND reads.
- **Modcrypt at load** (`DecryptModcryptArea`, DSi.cpp:362): retail keyX from
  `"Nintendo"` + game code, keyY = ARM9i hash bytes; dev key when
  `DSiCryptoFlags & 0x10` or `AppFlags & 0x80`. Done on the staged bytes in
  main RAM as melonDS does, so the `RomSource` stays read-only (no second
  `patch` overlay).
- **The AES engine** at `0x04004400`: control/blocks-left, MAC/IV/counter,
  in/out FIFOs, key slots 0..3 with the fixed slot 0/1/2/3 material
  (`DSi_AES::Reset`; slot 2's keyX comes from `bios9i[0x8B8C]`), NDMA start
  modes for the FIFOs, `IRQ2_DSi_AES`. Exactness matters here: titles poll
  `AES_CNT` busy bits and melonDS processes blocks synchronously.

Gate: a title with modcrypted ARM9i boots (most retail dumps are); the AES
unit test decrypts the melonDS-vendored test vectors.

#### Phase 2 status (2026-09-11)

Landed on `dsiware`. Modcrypt at load was already in from phase 1 (all four
oracle dumps -- KS3E, KTUE, KD9E, KZLE -- carry crypto flags `0x03` and boot
through it). New: `io/dsi_aes.{h,cpp}`, a port of melonDS's `DSi_AES` --
AES_CNT with the live FIFO levels, BLKCNT, the 16-word in/out FIFOs, IV, MAC,
the four key slots with the fixed material (slot 0 "Nintendo", slots 1/3 from
the console ID, slot 2's KeyX from `bios9i[0x8B8C]`), normal-key derivation
on the last KeyY word, CTR and CCM (MAC verify into bit 21 on decrypt, MAC
appended to the output on encrypt), blocks processed synchronously on FIFO
writes, `IRQ2_AES`, NDMA start modes `0x2A`/`0x2B` (every ARM7 NDMA run end
re-polls the FIFOs as melonDS does), byte/halfword partial writes, ARM7-only
(the ARM9 reads 0). `AES_ECB_encrypt` added to the vendored tiny-AES-c for
the CCM MAC. The console ID register at `0x04004D00` reads a new
`DsiIo::console_id` (zero until phase 3's NAND provides one; it also seeds
key slots 1 and 3). Both are in the `DSI ` state chunk.

Gate: `tests/aes_test.cpp` (21st unit test) -- NIST SP 800-38A ECB/CTR
vectors, the scrambler, and the engine through its registers: a CTR block
against the vector, a CCM encrypt then decrypt with the engine's own MAC
(verified, then corrupted), the FIFO levels, the IRQ in IF2, KeyX/KeyY
derivation into a working slot. The 60-frame Shantae trace stays identical
to melonDS on both CPUs, and a DSi save state round-trips.

What the oracle titles do with it: every one writes AES_CNT once at boot
(FIFO flush + MAC length) and zeroes key slot 1's normal key, then nothing
for 300 frames -- the engine is not on their runtime path; the NAND's
AES-CTR runs above the SD host in software (phase 3). Not modelled: the
"CCM-DECRYPT MAC from WRFIFO" variant (melonDS logs it as TODO too).

### Phase 3 -- SD/MMC host and the NAND (~1 800 lines)

- `DSi_SDHost` for the SDMMC instance only (SDIO/wifi host absent: its SCFG_EXT
  bits read as disabled): command/response registers, 16- and 32-bit data
  FIFOs and `CheckSwapFIFO`, card/host IRQ masks, `Event_DSi_SDMMCTransfer`
  timing, NDMA start mode. Port 0 (SD card) absent -- reads as no card; port 1
  is the NAND as an `MMCStorage` with the CID/CSD/CSR/OCR the titles' SDK
  probes.
- `NandImage` interface: `read_sector(n, out)`, `write_sector(n, in)`, CID,
  ConsoleID. Two backers: `NandFile` (real `nand.bin`, oracle) and
  `SyntheticNand` (section 3.1). AES-CTR happens above both, in the host, with
  the FAT key derived from the ConsoleID exactly as `DSi_NAND.cpp:42-112`.
- The synthetic builder: FAT16 layout, directory entries, the three save
  volumes via the ported `CreateSaveFile`, TWLCFG/HWINFO files, ROM bytes as a
  cluster chain over the `RomSource`.
- Saves: dirty save sectors mirrored to `<GAMECODE>.pub/.prv/.bnr`, written
  atomically like `.sav`, flushed when writes go quiet. Save states carry the
  synthetic NAND's writable sectors (small) and, for a real NAND, a dirty-sector
  journal since load (melonDS carries nothing and diverges after a reload).

Gate: the save-heavy oracle title creates and re-reads its save; the same
title on melonDS with our exported `.pub` imported through its TitleManager
sees the same data; melonDS trace matches with the real-NAND backer.

#### Phase 3 status (2026-09-12)

Landed on `dsiware`: the SD/MMC host and the real-NAND backer.
`src/core/io/dsi_sd.{h,cpp}` is a port of melonDS's `DSi_SD.cpp` -- the SDMMC
host at `0x04004800-0x040049FF` (command/response registers, the two 16-bit
FIFOs and the 32-bit one they drain into, `CheckSwapFIFO`, the IRQ and
card-IRQ masks, `Event_DSi_SDMMCTransfer` as `EventId::SdMmc`, NDMA start
mode 0x28) with an `MmcStorage` device on port 1 over a `NandImage`. Port 0
(the SD card slot) is absent and reads as no card; the SDIO host at
`0x04004A00` was absent entirely at this point (it landed with the NAND boot
work: `SdHost` now takes a controller number). `--dsi-nand` loads a real `nand.bin`,
whose nocash footer supplies the eMMC CID and the **console ID** -- which had
been hardcoded zero and seeds AES key slots 1 and 3, so `dsi.console_id` is
now set *before* `aes.reset()`. Save-state FORMAT_VERSION 4 (one more
scheduler event).

**The guest sees raw eMMC sectors.** melonDS's `ReadBlock`/`WriteBlock` seek
the backing file directly; its NAND AES-CTR (`SetupFATCrypto`/`ReadFATBlock`)
serves only the host-side `NANDMount` FAT view. So nothing on the guest path
encrypts, and the synthetic builder's crypto is still ahead of us.

**Gate.** The phase-1 melonDS gate still passes with the host in and a NAND
attached: ARM9 and ARM7 traces byte-identical over 60 frames, 400 rendered
frames byte-identical, all six DS scenes unmoved over 600 frames, 22/22 unit
tests. Shantae reports `nand: 0 block reads, 0 block writes`, exactly as
melonDS does -- **direct boot runs the title in card mode, so nothing asks
the NAND for anything.**

**The launcher hand-off now works, and is still off by default**
(`DS_DSI_HANDOFF=1`) because it ends in a crash. Two real bugs were found by
tracing the oracle with the recipe's own knobs
(`TRACE_BOOT_INDICATOR=3 TRACE_SCFG=-1:-1:93fffb06:-1 TRACE_LOAD_WRAM7=...`):

1. **The param block was written through the ARM9.** `setup_direct_boot_dsi`'s
   `w32`/`w8` helpers go through `bus.dma_write*(Cpu::ARM9, ...)`, and
   `0x0380xxxx` is ARM7-only WRAM -- invisible to the ARM9. The mount table
   was never landing. Fixed with ARM7-side writers.
2. **The block was not cleared first.** The oracle's `TRACE_LOAD_WRAM7` copies
   a zero-filled 0x500-byte window, so every byte the title is not told about
   is zero; writing only the fields left boot residue in the gaps.

With both fixed, Shantae's NAND access **matches the oracle's walk exactly**
-- 8 sectors before partition 0, the boot sector, the FAT, the root dir, then
`/TITLE/ -> 00030004 -> 4B533345 -> DATA/ + CONTENT/` -- and then goes
*further* than the oracle, reading and writing
`/TITLE/00030004/4B533345/DATA/PUBLIC.SAV` (136 reads, 8 writes, landing in
exactly two sectors of the pristine image). `DS_NAND_LOG=<file>` writes the
block log in `trace_melonds`'s format, so `tools/dsi_nand.py map` reads ours
and the oracle's alike.

**Both emulators then crash, differently, so the recipe is still incomplete.**
The oracle takes an ARM9 data abort at `020D8444`. Ours: the ARM7 ends up in
IRQ mode (CPSR `0x...52`) on a `b .` at `037c7164` -- the SDK's dead-end
handler -- after which the ARM9 sits in its `MCR p15` wait-for-interrupt loop
at `020e97b0` forever. Frames still tick, so it presents as ~4 fps rather
than a hang, and the 2.8 M ARM7 instructions per 10 frames are that spin
(the idle skip cannot collapse an IRQ-mode loop). The SD host is *not*
implicated: only 3527 register accesses over the whole stall, ending
cleanly on CMD13 and SD_CLOCK <- 0.

**Where the wedge is, and what it is not (2026-09-12).** The ARM7 ends in
IRQ mode (CPSR mode 0x12) on a `b .` at `037c7164`; the ARM9 then sits in
its `MCR p15` wait-for-interrupt loop at `020e97b0` forever. Ruled out, each
by measurement:

- **Not the interleave or the quantum.** melonDS uses the same
  `kMaxIterationCycles = 64` / `kIterationCycleMargin = 8` in DS and DSi
  modes; only `ARM9ClockShift` differs (2 at 134 MHz), which phase 1 already
  models. `DS_IDLE_SKIP=0`, `DS_QUANTUM=0` and `DS_QUANTUM=128` each only
  move *when* the wedge happens -- at 2000 frames it wedges with idle skip
  on or off.
- **Not the SD host.** By frame 100 all NAND I/O is complete and identical
  between a wedging and a non-wedging config (136 reads, 8 writes), the only
  IRQ2 source ever raised is `sdmmc` (165 raises, all enabled -- nothing
  spurious), and the register trace ends cleanly on CMD13 then
  `SD_CLOCK <- 0`. The wedge is *after* the SD work finishes.
- **Not a pre-existing DSi fault.** Card mode (no hand-off) runs 2000 frames
  clean on the same build, both with and without the idle skip.

So the hand-off path specifically leads the ARM7 into a dead-end handler,
some time after the save I/O completes. One melonDS semantic *was* found and
fixed on the way: melonDS's `ScheduleEvent` refuses to re-arm a live event
and keeps the first timestamp, while ours overwrites -- which could silently
swap a pending RX completion for a TX one when two blocks land inside one
512-cycle delay. `Scheduler::armed()` plus `SdHost::schedule_transfer` now
match melonDS. It did not fix the wedge.

**How melonDS DS (libretro) actually does it -- and why our approach differs
(2026-09-12).** `dsperate-research/melonds-ds-libretro`
(`src/libretro/console/dsi.cpp`, `SetUpDSiWareDirectBoot`) launches DSiWare
with *no mount table at all*. It writes a 0x100-byte **`TLNC` autoload block
at 0x02000300** in main RAM -- ID "TLNC", unknown 01h, length 18h, CRC16 over
0x18 bytes from +0x08 seed 0xFFFF, PrevTitleID 0 ("anonymous"), NewTitleID =
this title, flags `0x01 | (3 << 1) | (1 << 4)` (bit 0 valid, bits 1-3 boot
type 3 = DSiWare, bit 4 undocumented but required) -- and sets the **BPTWL
boot flag** (register 0x70). Credited there to CasualPokePlayer.

The decisive detail is the branch around it:

    if (isDirectBootConfigured && isDsiMode && header.IsDSiWare())
        SetUpDSiWareDirectBoot(...);       // TLNC only
    else if (... && !header.IsDSiWare() && ...)
        Console->SetupDirectBoot(...);     // never reached for DSiWare

For DSiWare it **never stages a direct boot**. The console boots its real
BIOS/boot2 chain from the NAND, and the TLNC block tells the boot ROM to
auto-launch an installed title instead of showing the menu. The core also
*temporarily installs* the title into the NAND image first
(`config/console.cpp`, `NANDMount::ImportTitle`) and removes it afterwards.

So melonDS DS's "DSiWare direct boot" is **not direct boot in our sense** --
it is a full NAND boot with an autoload hint. Implemented here as
`DS_DSI_HANDOFF=2`, the TLNC block is inert exactly as that predicts: no
wedge, but no NAND access either, because our direct boot has already staged
the title from the card image before the boot ROM would ever read it.

**This puts section 3.2 ("Direct boot only") in tension with the one approach
known to work.** The options are (a) implement the DSi BIOS/boot2 NAND boot
so TLNC means something -- the path melonDS DS proves out, and the one that
needs no reverse engineering -- or (b) keep staging a direct boot and keep
chasing the mount-table wedge. That is a scoping decision, not a code one.

This is the boundary of what the recipe buys. A full launch is a hybrid --
binaries from the card image, filesystem from the NAND -- and something in
that seam is inconsistent for both emulators. Note this is *weaker* than the
scoping note's earlier claim that the recipe was proven end to end; what is
reproducible today is mount + directory walk (both) and save I/O (ours).

#### The NAND boot path (2026-09-12)

`NDS::boot_dsi_nand()` (`--dsi-nand-boot`) boots the console the way it boots
itself, instead of staging a direct boot from the card. With a *half* BIOS
dump -- all we have, and all melonDS needs -- the boot ROM's own boot2 loader
is absent, so melonDS does its job by hand and so do we (DSi.cpp `LoadNAND`,
the `!FullBIOSBoot` branch): read boot2's location from the NAND boot info at
offset 0x220 and the NWRAM mapping at 0x380, apply MBK, AES-CTR boot2 into
place under a fixed key with the IV derived from the aligned size (over
byte-reversed 16-byte blocks), seed the eMMC CID block at 0x03FFE6E4 and the
BIOS routines the missing halves would have left (ARM9 ITCM at 0x4400/0x4800/
0x4894/0x58DC, a 0x3C00 ARM7 block at 0x03FFC400), then enter boot2 directly.
None of this is reverse-engineered -- the console does it.

**It works structurally.** boot2 loads and runs (ARM9 and ARM7 both at
0x037B8000) and the launcher reads ~19 900 NAND blocks. The entry is
*instruction- and register-identical* to melonDS: the traces agree on every
field, differing only by a constant 32-cycle offset from the first
instruction (the pipeline-fill charge `setup_direct_boot_dsi` applies and
this path does not yet).

**Two reset-state bugs found and fixed by diffing against melonDS's own NAND
boot.** Both are cases where our reset leaves the state a *direct* boot wants
(because there is no BIOS to program it) and real BIOS code reads it back:

1. **EXMEMCNT.** boot2's ARM7 spins on `LDRH r2,[0x04000204]` / `TST r2,r1`
   with r1 = 0x6000. melonDS `NDS::Reset` sets `ExMemCnt[0] = ExMemCnt[1] =
   0x6000`; we reset to 0. Direct boot masks it by writing 0xE880 (which
   carries those bits), so it only ever showed here. Fixed in `Io::reset`;
   all six DS scenes unmoved over 600 frames, so nothing else depended on it.
2. **CP15.** `NDS::reset` sets the ARM9's control register to 0x00012078
   (DTCM enabled, vectors high) -- again what a direct boot needs. boot2 does
   `MRC p15,0,r0,c1,c0,0` and branches on the value, reading 0x00012078 where
   melonDS reads the ARM9's own reset value 0x00002078. `boot_dsi_nand` now
   restores control 0x2078 with no TCM before entering boot2. (The comment on
   that reset line also had it backwards: bit 16 is DTCM enable, bit 18 ITCM.)

With both fixed the ARM9 traces agree 76 instructions further, and the whole
~19 900-block NAND read now happens in the first two frames rather than
trickling out.

**Where it stands now:** the remaining first divergence is an IPCSYNC
handshake at `0x04000180`. The ARM9 loops on `LDRH` / `AND #0x0F` / `CMP r4`
waiting for the input nibble to reach 3; melonDS reads 0x0300 (input 0) and
keeps waiting ~11 000 instructions, we read 0x0303 (input 3) and fall
straight through -- and the ARM7's mirror-image wait at `037b9570` exits
early for the same reason. **A tracing mistake to avoid repeating:** our headless tracer deduplicates
repeated lines unless `DS_TRACE_NODEDUP=1`, and melonDS's needs
`TRACE_NODEDUP=1`. Setting only the melonDS side makes a polling loop look
like a huge execution gap and puts the "first divergence" thousands of lines
too early. **Always set both.** With both set, the real first divergence is
ARM9 line 151871 / ARM7 line 107571 -- not the line 16505 / line 30 an
asymmetric comparison reports.

**Entry phase: fixed.** melonDS charges its post-`JumpTo` pipeline fill to the
*first instruction* (cost 40 ARM9 / 10 ARM7 against our 8 / 2; every later
instruction already agreed exactly). `boot_stall` is the wrong tool -- it
delays the start instead, moving our timeline off melonDS's t=0 and not
changing the phase at all. `CpuContext::defer_cost`, which the DMA path
already uses for melonDS's pending `Cycles`, charges *after* the next
instruction, which is the right shape. `boot_dsi_nand` now adds 64 (ARM9) and
4 (ARM7); those are calibrated against the oracle rather than derived,
because the trace timeline scales each CPU's cycles differently. With them
the ARM9 reaches the IPCSYNC read site at **t=197628 on both sides, exactly**.

**The NWRAM bug: a WRAMCNT write was wiping the NWRAM windows.** The symptom
was the ARM7 fetching `00000000` at `037d5190` where melonDS fetches
`e0031001`, at the same timestamp. Narrowing it down:

- the boot2 image *loads* correctly -- a read-back at `037d5190` right after
  `load_boot2` gives `e0031001`, and the window/slot maths all check out
  (ARM7 NWRAM-C covers `037B8000-037F8000`, the address lands in slot 2);
- boot2 never writes MBK at all, so it is not a remap by the guest;
- `SCFG_EXT` bit 25 stays set throughout, so NWRAM is never disabled;
- with `DS_WATCH` on that address, the *data* read returns the right value --
  only the instruction fetch reads zero.

The cause: a **WRAMCNT** write called `Bus::update_wram()` directly. On a DSi
the NWRAM windows sit *on top* of that region, and `update_wram` re-lays
`0x03000000-0x037FFFFF` as shared WRAM without re-applying them --
`update_nwram()` does both, in that order. boot2 runs *from* NWRAM, so its own
WRAMCNT write pulled the code out from under itself and the ARM7 fetched
zeros mid-routine. A direct-booted title sets WRAMCNT before anything maps
NWRAM, which is why this never showed before.

With that fixed the **ARM9 is byte-identical to melonDS for the entire first
frame of a NAND boot** (520 439 instructions, no divergence at all).

**Remaining ARM7 divergence**, a single localised difference at line 109115:
`LDRB r0,[0x0380FFC8]` reads **0x84** on melonDS and **0x00** here. Traced
with new polled watches on both sides (`DS_WATCH7=<arm7 wram offset>` here,
`TRACE_WATCH7=<offset>` in the harness -- polled per instruction, which sees
the byte change however it happened, unlike the `DS_WATCH` page trap that
only catches CPU accesses through the slow path):

- both emulators write **0x84** there at the same time (t=224412), from ARM7
  code at `037d85b8` -- identical;
- **we then zero it at t=324866** and melonDS never does.

The zeroing write is a `STMLTIA r1!,{r0,r2-r8}` memset loop at `037d8540`
sweeping `r1` up to `r12 = 0x03800D18`. As it passes `0x037FFFC8` that
address **aliases onto `arm7_wram[0xFFC8]`** through the ARM7-WRAM mirror
below `0x03800000` (WRAMCNT is 0, so the ARM7 has no shared WRAM and
`0x03000000-0x037FFFFF` mirrors its own 64 KB) -- the same physical byte as
`0x0380FFC8`. melonDS runs **the same loop with the same bounds** (its last
iteration has `r1 = r12 = 0x03800d18` at t=327416) and its byte survives.

**Resolved: WRAMCNT.** The reading above assumed WRAMCNT = 0 on both sides,
and it was only 0 on ours. melonDS's `DSi::Reset` calls `MapSharedWRAM(3)`
after `NDS::Reset` maps 0, so on a DSi the ARM7 owns all 32 KB of shared WRAM
and `0x037FFFC8` lands in `SharedWRAM[0x7FC8]`, never in the mirror.
`boot_dsi_nand` now sets WRAMCNT 3. This is the fourth bug of the same class
as EXMEMCNT and CP15: reset state a direct boot overwrites and real boot code
reads back (and check `DSi::Reset`, not only `NDS::Reset`).

#### NAND boot to the launcher (2026-09-12, evening)

**Result.** A real NAND boot with no cart attached runs boot2, the launcher,
the Health and Safety screen and -- after a tap -- the **DSi Launcher menu**.
At frame 1200 the top screen is pixel-identical to melonDS (the bottom differs
on its animated elements), and the CPU slice grid is identical to melonDS from
reset to the tap at frame ~600. Card-mode direct boot is still byte-identical
to melonDS over the 60-frame gate, all six DS scenes are unmoved over 600
frames, and the 22 unit tests pass. Commits `f8de6bf`, `50d4f87`, `e0b4f79`.

    cp dsi-binary/bios/dsinand.bin out/n.bin          # always a copy: boot writes to it
    dsperate-headless --direct --interp --dsi --touch 600:128,100:10 \
      --bios9 bios9.bin --bios7 bios7.bin --firmware dsifirmware.bin \
      --bios9i biosdsi9.bin --bios7i biosdsi7.bin --dsi-boot dsiboot.bin \
      --dsi-nand out/n.bin --dsi-nand-boot --frames 1300 --dump-frames out/o.frames

`--dsi` is required without a ROM (otherwise the machine is a DS and draws
nothing); a NAND boot no longer needs a cart. `--touch F:x,y[:N]` has the
same meaning as `trace_melonds --touch`.

**What it took, in the order the diffs found it.** Each item was the first
divergence from melonDS at the time (section 6 has the method).

*Machine and scheduler*

1. **WRAMCNT reset value** (above).
2. **Post-jump numC in the interpreter.** An LDM/POP that loads a Thumb pc
   charges its data cost after `JumpTo`, which leaves `R[15] = target + 2` in
   melonDS and `target + 4` in `jump()`; the `R[15] & 2` test inverts, so a
   word-aligned Thumb target is numC 0. Both JITs already did this; DS hashes
   unmoved.
3. **An IRQ raised by a DMA that ends mid-phase** is taken after the CPU's
   next instruction (melonDS resumes `Halt(2)` into `Execute`); the phase-start
   conversion had already passed.
4. **The ARM7 re-enters its DMA share while it moves and budget remains**
   (melonDS `while (ARM7Timestamp < target) RunNDMAs(1)`). The AES NDMA
   in/out ping-pong otherwise ran one pass per slice and put the ARM7 55 000
   cycles behind the ARM9 -- invisible in per-CPU traces, fatal for IPC.
5. **SCFG_CLK9 writes floor the ARM9 clock** to a system cycle every time
   (melonDS `SetScfgClock9` shifts `ARM9Timestamp` down and back even when the
   speed does not change), and the store that wrote it is priced from the
   rebuilt timing table (melonDS reads `DataCycles` after `BusWrite`).
6. **The launcher drops the ARM9 to 67 MHz** (SCFG_CLK9 `0x0184`, from ITCM,
   mid-slice). The scheduler used `shift9_` both as the clock conversion and
   as "is a DSi", so the switch turned every DSi interleave rule off and read
   quarter cycles as half. Now a `dsi_` flag gates the rules, the ARM9 is
   floored to system cycles at either speed, and a mid-slice switch rescales
   the slice (`Scheduler::set_clock9_shift`).
7. **`fire_due` spun forever** when an ARM9 soft (timer) event fell inside the
   ARM7's overshoot: one soft minimum was compared against the ARM7's limit
   while the pass fired ARM9 timers against `now_`. This was the frame-60
   "hang" of the cartless boot. Soft minima are now per CPU.
8. **Timer register access runs that CPU's soft timers first** (melonDS
   `RunTimers` in `TimerStart` / `TimerGetCounter`), so a control write sees
   an overflow that is already due.
9. **Halfword bulk DMA runs** were priced with the DS doubling (`<< 1`)
   instead of `shift9_` (2 at 134 MHz): 16-bit VRAM uploads cost half.

*Devices*

10. **DSP host interface** (`io/dsi_dsp.*`): melonDS `DSi_DSP` without a core.
    `PSTS` reads `0x0100`.
11. **SDIO host**: `SdHost` takes a controller number (reset `PortSelect`,
    IRQ2 lines, NDMA mode `0x29`, card presence `0xA0`, its own transfer
    event); instance 1 at `0x04004A00`.
12. **Atheros Wi-Fi module** on SDIO port 0 (`io/dsi_nwifi.*`, melonDS
    `DSi_NWifi`): SDIO F0/F1, the mailboxes, BMI/HTC/WMI answered instantly,
    the 1 ms timer. No network backend; a scan reports melonDS's built-in AP.
    Soft reset resets both ports.
13. **Cameras** (`io/dsi_camera.*`): the two Aptina sensors on I2C `0x78`/`0x7A`
    and the camera module at `0x04004200` (frame IRQ, scanline transfer, the
    two pixel buffers, NDMA `0x0B`). No image source: frames are black, as in
    melonDS's trace harness.
14. **SCFG reads** follow melonDS's per-width tables (a 32-bit ARM7 read of
    `SCFG_MC` had carried the cart insert delay in its top half).
15. **The DSi CODEC's SPI output latch** persists: an index byte, a write or
    a read of an unmodelled bank leaves the previous byte in SPIDATA.
16. **DSi touch coordinates** were stored as pixel `<< 8` -- colliding with the
    pen-changed bit 15 -- instead of `<< 4`, so the launcher saw every tap at
    x = 0. This is why Health and Safety ignored the tap.

**Save states** are `FORMAT_VERSION` 5 (SDIO transfer, Wi-Fi timer and camera
transfer events); the SD/MMC, SDIO, Wi-Fi and camera events rebind on load.

**Still open on this path.** Launching a title from the launcher (tap the
icon) or auto-launching one (`TLNC`); a cart-present NAND boot (the Shantae
boot's last known divergence, SPIDATA at frame 37, is plausibly one of fixes
15-16 but is not re-checked); rendered frames differ from melonDS from about
frame 25 even while the CPU grid matches (a 2D render/present difference not
yet looked at); the bottom-screen animation phase after the tap; the JIT and
the idle skip under DSi are unvalidated.

#### Unlaunch as a boot2 replacement (2026-09-12)

`dsi-binary/unlaunch/unlaunch.dsi` is a plain DSi multiboot SRL: unit code 03,
**no modcrypt**, no ARM9i/ARM7i sections, ARM9 0x02200000 (entry 0x02200800,
0xAD80) and ARM7 0x02380000 (entry 0x02380000, 0x205F8). Its strings confirm
it looks for **BOOTCODE.DSI** (fragment "ODE.DSI" at 0xBA47, with "TITLE",
"0003", "\CONTENT") and builds NAND title paths (`/content`, `/data`,
`/public.sav` at 0xB9E4).

Direct-booting it as a cart gets nowhere -- forced-white screen, 0 NAND reads
-- and **melonDS fails the same way** (uniform screen for 600 frames plus an
`undefined ARM9 instruction DE9302C3 @ 00004400`), because it is a boot2
replacement and expects boot2's entry state, not a cart's.

Run as boot2 instead (`--dsi-boot2 <srl>`, which implies `--dsi-nand-boot`)
it gets further -- 2 NAND block reads -- then stalls: the ARM9 sits in the
BIOS at 0xFFFF0110 and the ARM7 in the ARM7 BIOS delay loop at
0x00000170/0x174 (`SUBS r0,#1` / `BGT`), re-entered ~188 k times. The
EXMEMCNT reset value above was the prime suspect, since a boot2 replacement is
written against exactly the state real boot2 is entered in -- but with
EXMEMCNT, CP15, NWRAM and WRAMCNT all fixed the Unlaunch run is **unchanged**
(2 reads, both CPUs parked). melonDS cannot run Unlaunch either, so there is
no oracle; the real-boot2 route above is the one being pursued.

**BOOTCODE.DSI needs an SD card, which we do not have.** Phase 3 deliberately
left SD host port 0 absent (reads as "no card"); the NAND is port 1. Auto-
launching a title from the SD root means implementing port 0 over a FAT
image -- the `SyntheticNand`/FAT builder work, pointed at the SD slot.

### Phase 4 -- I2C, BPTWL, I2S/mic (~900 lines)

- I2C host at `0x04004500` with the BPTWL device: battery level (feed it the
  frontend's battery when we have one, else "charged"), volume/backlight
  registers, the power-button state machine only to the extent of "never
  pressed", warmboot flag register 0x70, `IRQ2_DSi_BPTWL`.
- `MICCNT/MICDATA` on I2S with the 16-entry FIFO and half-full IRQ; the
  existing SDL/ALSA capture feeds it. `IRQ2_DSi_MicExt`, NDMA mode 0x2C.
- Camera **stub**: I2C addresses 0x78/0x7A ack and return zeros, capture unit
  never signals a frame. Titles that require it show black; that is the stated
  limit.

Gate: the DSP-free oracle titles are playable end to end; a two-hour
frame-hash soak against melonDS on the real-NAND backer shows no drift.

#### Phase 4 status (2026-09-12)

- **I2C + BPTWL:** in since phase 1 (register file, boot flag, IRQ flags);
  the soft-reset request is logged, not acted on.
- **Cameras:** not a stub -- the launcher configures both sensors over I2C
  and polls the module, so melonDS's `DSi_Camera` was ported whole (NAND boot
  item 13). No frontend image source yet.
- **Mic / I2S (`MICCNT`, `MICDATA`, NDMA `0x2C`):** not started; no
  divergence has pointed at it so far.
- **Gate:** not yet run -- it needs a title launched through the NAND boot.

### Phase 5 -- DSP HLE (~1 500 lines) and the frontend

- Teak host interface registers (`DSP_PCFG/PSTS/PSEM/PMASK/PCLEAR`,
  `CMD/REP0..2`, PDATA DMA), `SCFG_RST` bit 0 gating, the ucode CRC32 table
  over NWRAM bank B, the three HLE ucodes ported from `DSP_HLE/`. AAC decode
  needs a decoder; melonDS pushes that to the frontend (`Platform_AAC.cpp`).
  Ship G.711 and graphics first, AAC when a decoder choice is made.
- Frontend: `[paths] bios9i`, `bios7i`, `dsi_nand` (ini only, per
  `settings.h`'s rule); the games picker gains DSiWare detection by header and
  a "(DSiWare)" tag. **Accept `.cia` directly**: the DSiWare CIAs people
  actually have carry the plain SRL as their one content, so a 40-line header
  walk (cert/ticket/TMD aligned to 0x40) turns into a `RomSource::map_file`
  at an offset -- the same trick the zip path uses. Encrypted-content CIAs
  (TMD content type bit 0) are refused with a clear message; a menu row for exporting/importing `.pub/.prv`; the
  `Emulation` page shows DSP state. Nothing else in the menu changes.
- RetroAchievements: DSiWare is `RC_CONSOLE_NINTENDO_DSI` (78) with its own
  memory map; the identify step already hashes the ROM bytes we have. The DS
  map's "DSi hole" assertion in `cheevos_memory.cpp` stays for DS games and a
  second table serves the DSi machine.

#### Phase 5 status (2026-09-12)

- **Host interface without a core** (`io/dsi_dsp.{h,cpp}`): PCFG, PSTS
  (write FIFO always empty, sticky semaphore bit), PSEM, PMASK, PCLEAR,
  CMD0-2, the PDATA read FIFO and its IRQs, `SCFG_RST` resetting the block,
  and melonDS's ARM7 32-bit write path into the page. With `TRACE_DSP=1` the
  melonDS harness shows the launcher never releases the DSP reset in 120
  frames, so this is all the NAND boot needs.
- **Not modelled, and logged once when reached:** the core being enabled
  (`SCFG_CLK9` bit 1, `SCFG_RST` released, PCFG bit 0 clear) -- from then on
  melonDS runs HLE ucodes or Teakra and schedules a 4096-cycle catch-up event
  that cuts slices -- and a PCFG bit-0 release, where melonDS would start a
  core. The G.711 oracle (KTUE) is the place to add them.
- **Frontend:** no DSi wiring yet (`[paths]`, picker, `.cia`, RA table).

## 5. Performance notes (why this can be fast on the RG DS)

- The ARM9 at 134 MHz doubles the guest instruction budget per frame. Our
  headroom on DS titles is 10-40 % depending on the scene, so **a DSiWare
  title that saturates its CPU will not run full speed on the A55 without the
  same campaign the DS titles got.** Most DSiWare is 2D and light; the ones
  to worry about are the 3D ones. Measure on the oracle set before promising.
- 16 MB main RAM is free for the page table (it already spans 4 GB). The cost
  tables grow by 12 MB of 4 KB pages in `cpu9_`/refill; negligible.
- NWRAM remaps happen at boot and rarely after; `PageTable::remap` plus a JIT
  range flush is the right cost model. Do not make MBK writes cheap at the
  expense of the read path.
- Per-sector AES-CTR on NAND reads is ~1 µs a sector in scalar C; titles read
  saves in KB. Not a concern; do not NEON it.
- The DSP, when present, is the risk: HLE is free, LLE is a core.

## 6. Verification

- **DS mode is bit-neutral**: `tools/all_scene_hashes.sh` on the six DS scenes
  before and after every phase, both builds `md5sum`ed; `state_roundtrip.sh`
  on `mlbis` and `gsdd-phase2`. This is the gate that lets the work land on
  `main` incrementally rather than as one merge.
- **DSi mode against melonDS**: `tools/compare_traces.py` with the melonDS
  DSi trace build, same BIOS dumps, real NAND backer, same title, from frame 0
  (direct boot in both). Trace divergence is expected only where melonDS
  itself says "TODO" (`0x02FFD7B0..0x02FFDC00` staging, wifi params) -- those
  we copy verbatim so the traces still agree.
- **Frame hashes** via `DS_FRAME_HASH=1` and the FIFO SHA-1 harness on the
  oracle titles, recorded as new `scenes/*.dsin` entries once inputs are
  captured.
- **Unit tests**: `aes` (vectors), `nand_synth` (build, then mount and read
  back with `tools/dsi_nand.py`, the independent Python FAT/crypto reader
  written against melonDS's derivation), `ndma`, `sdhost` (command sequences
  captured from the melonDS trace), `dsi_header`.
- Never benchmark or hash under a missing DSi BIOS; `load_bios` refuses DSi
  titles without both `bios7i`/`bios9i` rather than degrading.

**What actually found the NAND-boot bugs (2026-09-12).** Per-CPU instruction
traces were not enough: two emulators can agree on every instruction of each
CPU while interleaving them differently (the NDMA ping-pong put the ARM7 55 k
cycles behind with both traces identical). The loop that worked:

1. **Screenshots** of both (`--dump-frames` on both sides) to see how far each
   gets.
2. **Slice-grid diff**, about a minute and no traces: our `DS_DEBUG_SLICES=1`
   against the harness's `TRACE_SLICES=1` (`[slice] now` / `[fire]` lines in
   our units), comparing the slice-end column. Gate knobs on ours as above.
3. At the first split, **full traces** of that frame window on both sides
   with `TRACE_START_FRAME`, `TRACE_TIME_FINE=1` (the DSi ARM9 in raw core
   cycles; the normal stamp hides quarter-cycle cost differences) and both
   `DS_TRACE_NODEDUP=1`/`TRACE_NODEDUP=1`. The CPU whose first difference is
   earlier in time is the cause.
4. What the differences turned out to be: an unmodelled I/O page (a register
   reads 0 on one side), a register at the wrong width, an IRQ taken a slice
   early or late (read IE&IF in the handler), a cost off by a quarter cycle,
   or a stall of the wrong length (a timestamp gap on one side;
   `DS_DEBUG_DMA=2`).

Harness additions for this: `TRACE_SLICES`, `TRACE_TIME_FINE`, `TRACE_DSP`
(research `5aaa9f6`, `9d5c505`); ours: `TRACE_TIME_FINE`, `DS_WATCH7`, `t=`/pc
in `DS_IPC_LOG`, `--touch`.

## 7. Risks and open questions

1. **Test breadth.** We have three CIA titles and fifteen more in one NAND,
   all USA, all from one console. Enough to build against; not a compatibility
   survey. The DSP and camera questions in particular stay open until more
   titles are looked at.
2. **Synthetic NAND fidelity.** Titles use the SDK's FS library, which reads
   the FAT it finds; a mistake in our FAT16 builder or in the save-volume
   geometry shows as a title that "has no save data" or reformats. Mitigation:
   phase 3 lands on the real-NAND backer first, and the synthetic one must
   pass the same trace comparison up to the first save read.
3. **ConsoleID exposure.** Titles can read the ConsoleID through the AES
   key-slot path indirectly and some SDK code puts it in the save. A fixed
   synthetic ConsoleID means saves exported to a real console or melonDS NAND
   carry a foreign ID; the melonDS import path does not care. Note in docs.
4. **Half-dump DSi BIOSes.** Low-32 KB dumps boot in melonDS direct mode; if a
   title SWI reaches the upper half we cannot know until it does. Record the
   dump class in `bios_id` and log at first access to the padded region.
5. **Timing model at 2x clock.** Our per-page cost tables are calibrated on DS
   wait states; the DSi's main-RAM and NWRAM timings differ and melonDS's own
   are approximate (`UpdateVRAMTimings`, "TODO"). Exactness against melonDS
   is achievable, exactness against hardware is not claimed.
6. **Save states and NAND.** A real-NAND state cannot snapshot 240 MB. The
   dirty-sector journal is sound while the file underneath is not modified
   outside DSperate; refuse to load a state whose NAND identity (CID +
   ConsoleID + file size) differs, the way `bios_id` is checked today.
7. **DSP coverage.** Unknown until the library is surveyed; if a favourite
   title needs an unlisted ucode, that is the teakra decision.
8. **Licence.** teakra is MIT-style, fatfs BSD-style; both compatible if
   vendored. We plan to vendor neither in phases 1-4.
9. **The JIT under DSi (2026-09-12).** Every exactness result above is on the
   interpreter. The aarch64 build compiles the scheduler changes, and the
   A64/A32 post-jump numC already matched melonDS, but no DSi run has been
   checked under qemu -- and the device runs the JIT.
10. **Idle skip under DSi.** The gates run with `DS_IDLE_SKIP=0`; the default
    is on. The cartless boot's frame-60 hang looked like an idle-skip fault
    and was not, but the skip heuristics were built for DS scenes and are
    untested on the launcher.
11. **Render/present differences.** Rendered frames differ from melonDS from
    about frame 25 of the NAND boot even where the CPU interleave matches;
    the top screen of the launcher agrees, so this is likely a
    presentation or effect-state detail, but it is unexplained.
12. **Save-state size.** The Wi-Fi module's mailboxes (about 44 KB) and two
    32 KB camera MCU register files now travel in every DSi state.

## 8. Not in scope, and what would reopen it

Updated 2026-09-12: three rows moved in scope because the NAND boot needed them.

| feature | status | what remains |
|---------|--------|--------------|
| system menu / NAND boot | **in** -- boot2 shortcut ported, reaches the DSi Launcher | launching titles from it; full `SoftReset`; ticket ES crypto only if something needs it (nothing has) |
| DSi-mode retail carts | out: DS mode runs them today | the DSi cart-slot power machine (`SetScfgMC`), `SCFG_MC` swap |
| SD card slot | out: no DSiWare needs it (Unlaunch's `BOOTCODE.DSI` would) | a `FATStorage` equivalent on host folder or image, on SD host port 0 |
| cameras | **in** as hardware (the launcher needs them) | a frontend image source |
| DSi Wi-Fi (AR6002 SDIO) | **in** as hardware (SDIO host + NWifi) | a network backend / AP for actual connections |
| second card slot | out: nothing uses it | `NDSCartSlot2` |
| DSP cores (HLE ucodes, Teakra) | out for now: host interface only | the phase-5 HLE table and the catch-up event |
| mic / I2S | not started | `MICCNT/MICDATA`, NDMA `0x2C`, capture feed |
