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
| `DSi_SD.cpp/.h` | 1407 | two SD hosts (`0x04004800` SDMMC: SD card + NAND; `0x04004900` SDIO: wifi); the MMC card model |
| `DSi_NAND.cpp/.h` + `DSi_TMD.h` | 1671 | NAND image: nocash footer, FAT AES-CTR crypto, ES (ticket) crypto, fatfs mount, title import/export, TWLCFG/HWINFO |
| `DSi_I2C.cpp/.h` | 786 | I2C host + BPTWL power IC (+ cameras) |
| `DSi_I2S.cpp/.h` | 351 | MICCNT/MICDATA FIFO and **SNDEXCNT** |
| `DSi_SPI_TSC.cpp/.h` | 272 | DSi TSC/CODEC, DS-compat mode |
| `DSi_DSP.cpp/.h` + `DSP_HLE/` | 2973 | Teak host interface; HLE for AAC/G.711/graphics ucodes, LLE via teakra |
| `teakra/` | 13 734 | vendored Teak core |
| `fatfs/` + `sha1/` | ~23 000 | vendored FAT and SHA-1 |
| `DSi_Camera.cpp/.h`, `DSi_NWifi.cpp/.h` | 2841 | out of our scope |
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

No boot ROM, no boot2, no launcher. `DSi::LoadNAND`'s boot2 shortcut
(hard-coded boot2 key, BIOS fragments copied into ITCM, MBK values from NAND
offset 0x380) exists in melonDS to reach the system menu; we do not need it,
and the `FullBIOSBoot` path additionally needs the rare full 64 KB dumps.
Our entry is a DSi-flavoured `setup_direct_boot` matching melonDS's DSi
branch line for line; melonDS-with-the-same-title is then an instruction-trace
oracle from frame 0.

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

## 4. Phases

Each phase ends with the DS scenes bit-identical (`tools/all_scene_hashes.sh`)
and a `state_roundtrip.sh` pass. Phases 1..4 additionally end with a
melonDS-trace comparison on the DSiWare oracle titles (section 6). Line counts
are estimates from the melonDS equivalents scaled for our page-table bus.

### Phase 0 -- dumps and oracles (no code)

- Dump from a DSi: `bios7i.bin`, `bios9i.bin`, `nand.bin` with nocash footer
  (dsibiosdumper / the standard NAND dumpers write it), and the 128 KB DSi
  firmware melonDS wants for its own config. Extract two or three installed
  DSiWare titles as `.app` + `.tmd` (melonDS's TitleManager exports the app;
  NUS still serves `tmd` for the title ID at
  `nus.cdn.t.shop.nintendowifi.net/ccs/download/<idhi><idlo>/tmd`).
  **The research tree has none of this today** (`binary/real-bios` is DS
  only). This blocks every gate below.
- Build melonDS DSi mode with the trace instrumentation already used by
  `tools/compare_traces.py` (`dsperate-research/build/melonds-trace`), confirm
  the oracle titles run there from NAND, and record their first N frames.
- Pick the oracle set: one title with no save and no DSP (a simple game), one
  save-heavy title (public.sav in use from frame 1), one DSP title (AAC or
  G.711 -- Flipnote-class), one 3D title.

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
11. **TSC:** the DSi CODEC on SPI with `SetMode(0)` compatibility; direct boot
   leaves it in DS mode unless `AppFlags & 1`, so most titles never see the
   new register file. Port the bank-3 register model anyway (272 lines).

Gate: a no-save, no-DSP DSiWare title runs to its title screen with the
melonDS instruction trace matching, and nothing else's hashes moved.

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

### Phase 4 -- I2C, BPTWL, I2S/mic (~900 lines)

- I2C host at `0x04004600` with the BPTWL device: battery level (feed it the
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

### Phase 5 -- DSP HLE (~1 500 lines) and the frontend

- Teak host interface registers (`DSP_PCFG/PSTS/PSEM/PMASK/PCLEAR`,
  `CMD/REP0..2`, PDATA DMA), `SCFG_RST` bit 0 gating, the ucode CRC32 table
  over NWRAM bank B, the three HLE ucodes ported from `DSP_HLE/`. AAC decode
  needs a decoder; melonDS pushes that to the frontend (`Platform_AAC.cpp`).
  Ship G.711 and graphics first, AAC when a decoder choice is made.
- Frontend: `[paths] bios9i`, `bios7i`, `dsi_nand` (ini only, per
  `settings.h`'s rule); the games picker gains DSiWare detection by header and
  a "(DSiWare)" tag; a menu row for exporting/importing `.pub/.prv`; the
  `Emulation` page shows DSP state. Nothing else in the menu changes.
- RetroAchievements: DSiWare is `RC_CONSOLE_NINTENDO_DSI` (78) with its own
  memory map; the identify step already hashes the ROM bytes we have. The DS
  map's "DSi hole" assertion in `cheevos_memory.cpp` stays for DS games and a
  second table serves the DSi machine.

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
- **Unit tests**: `aes` (vectors), `nand_synth` (build, mount with a
  reference FAT parser, read back), `ndma`, `sdhost` (command sequences
  captured from the melonDS trace), `dsi_header`.
- Never benchmark or hash under a missing DSi BIOS; `load_bios` refuses DSi
  titles without both `bios7i`/`bios9i` rather than degrading.

## 7. Risks and open questions

1. **No test assets yet.** Everything above waits on a NAND + BIOS dump and
   two or three titles. Phase 0 is the real first step.
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

## 8. Not in scope, and what would reopen it

| feature | why not | what it would need |
|---------|---------|--------------------|
| system menu / NAND boot | needs full boot chain, launcher, ticket ES crypto with `bios7i[0x8308]` | boot2 shortcut port, fatfs-level NAND writes, full `SoftReset` |
| DSi-mode retail carts | DS mode runs them today | the DSi cart-slot power machine (`SetScfgMC`), `SCFG_MC` swap |
| SD card slot | no DSiWare needs it | a `FATStorage` equivalent on host folder or image |
| cameras | one or two titles | the Aptina register model + a frontend source |
| DSi Wi-Fi (AR6002 SDIO) | no networking in DSperate, and DS Wi-Fi is register-only too | SDIO host + 1 645 lines of NWifi + an AP |
| second card slot | nothing uses it | `NDSCartSlot2` |
