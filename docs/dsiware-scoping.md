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
| No-NAND fallback via Unlaunch | **in** | needs SD port 0 and synthesised boot data (2.2) |
| Title install from `.nds`/`.app`/`.cia` | **in** | temporary install into a working copy of the NAND (2.3) |
| Save export/import (`.pub`/`.prv`/`.bnr`) | **in** | melonDS TitleManager extensions |
| Microphone (I2S `MICCNT`/`MICDATA`) | **in** | fed by the existing SDL/ALSA capture |
| DSi Wi-Fi networking | **in** | Atheros module is modelled; attach it to the `wifi-emu` slirp backend |
| SD card slot (SD host port 0) | **in** | host folder or image; also carries Unlaunch's `BOOTCODE.DSI` |
| Frontend | **in** | `--dsi-mode` with the usual firmware boot options; ini paths; picker lists DSiWare only in DSi mode; `.cia` input |
| JIT + idle skip under DSi, on device | **in** | must work; **slower than DS titles is accepted for 2.0.0** |
| DSP HLE (G.711, graphics) | after 2.0.0 | one feature commit, together with camera images passed from the CLI |
| Camera image source | after 2.0.0 | the cameras exist as hardware; frames are black until then |
| DSi-enhanced retail carts in DSi mode | after 2.0.0 | they run in DS mode, as today |
| DSP LLE (Teakra), AAC ucode | undecided | only if a wanted title needs an unlisted ucode |
| Second card slot, DSi Shop, ticket/ES crypto | out | nothing needs them |

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
- The reverse-engineered launcher hand-off (`DS_DSI_HANDOFF=1`: indicator 3,
  `SCFG_EXT7` bit 18, the mount table at header word 0x1D4, 8 bytes at
  0x0380FFC4) gets the title onto the NAND. Shantae walks the directory and
  reads and writes `PUBLIC.SAV`, and then both emulators crash (melonDS: ARM9
  abort at 020D8444; ours: ARM7 on the SDK's `b .` IRQ handler at 037c7164).
  It is a card/NAND hybrid, not a launch. The earlier "proven end to end"
  claim was wrong.
- TLNC on top of a direct boot (`DS_DSI_HANDOFF=2`) is inert: only the boot
  ROM reads it.

Direct boot stays for two jobs: the phase-1 card-mode exactness gate
(section 5.1), and as the entry path Unlaunch-style boots build on.

### 2.2 Required files, and the Unlaunch fallback

Required: the DS `bios9.bin`/`bios7.bin`, `bios9i`/`bios7i` (half dumps are
enough: their low-32 KB CRCs match melonDS's; full-64 KB CRCs do not), the
DSi firmware (`dsifirmware.bin`), and `nand.bin` (nocash footer, which supplies
the eMMC CID and the console ID). The console ID seeds AES key slots 1 and 3,
so it is set **before** `aes.reset()`. The user's `nand.bin` is never written:
boots run on a working copy (today by hand, `cp` first).

`--dsi-boot <blob>` (0x154 bytes from `tools/dsi_nand.py bootblobs`: TWLCFG
0x128, HWINFO_N, HWINFO_S) is a **direct-boot** input only; a NAND boot reads
the console's own copies.

**Unlaunch fallback (no NAND).** `unlaunch.dsi` is a plain DSi SRL (no
modcrypt, no ARM9i/ARM7i) that replaces boot2 and runs `BOOTCODE.DSI` from
the SD root. `--dsi-boot2 <srl>` runs it in boot2's place. At `d30ed62` it did
2 NAND reads and then parked both CPUs in BIOS delay loops. That was before the
EXMEMCNT/CP15/NWRAM/WRAMCNT reset fixes and every launcher fix since, so
**it has not been re-tested at `e0b4f79`**. The only melonDS failure on
record (a uniform screen, and an undefined instruction at 0x00004400) came
from running it as a cart, which gets boot2's entry state wrong. **melonDS has
not been tried with Unlaunch as boot2.** Its `!FullBIOSBoot` path loads boot2
the same way `boot_dsi_nand` does, so getting `trace_melonds` to load the SRL
in boot2's place (the harness equivalent of `--dsi-boot2`) could make it the
oracle. Try that before debugging ours blind. To work without a NAND it needs:
1. SD host port 0 over a FAT image or host folder, carrying `BOOTCODE.DSI`
   (the same FAT builder as 2.3);
2. the three things `boot_dsi_nand` reads from the NAND synthesised instead:
   the boot info (0x220), the MBK mapping (0x380) and the eMMC CID;
3. whatever Unlaunch reads from `nand:/` itself (unknown until it runs).

### 2.3 Title install and saves

melonDS DS imports the title into the NAND before boot and removes it after
(`NANDMount::ImportTitle`). We do the same against the working copy:
`title/00030004/<id>/content/title.tmd` + `<ver>.app`, and
`data/public.sav`/`private.sav`/`banner.sav` sized from header 0x238/0x23C and
`AppFlags & 4`, each formatted as `CreateSaveFile` does. The NAND FAT is
AES-CTR encrypted (key from the console ID, `FATIV = bswap128(SHA1(CID))`), so
this host-side writer is where NAND crypto lives. The SD/MMC host passes
**raw** sectors, and the guest decrypts them in software; the AES engine is
not on this path.

Inputs:
- `.nds`/`.app`, with a TMD built from the header;
- `.cia`: the DSiWare CIAs people have carry the SRL as the only content;
  title-key-encrypted content is decrypted with 3DS common key 0.
  `dsperate-research/tools/melonds/cia_to_srl.py` is the reference, including
  the 520-byte DSi TMD (not the 3DS one).

Saves persist as `<saves>/<GAMECODE>.pub/.prv/.bnr`, extracted after a session
and imported at the next install, so they round-trip with melonDS's
TitleManager. Still to decide: whether the working copy is a full 240 MB copy
per session or a sector overlay over the read-only dump. The overlay is
smaller and also makes save states practical (2.5).

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
covered yet: NAND contents. A state must carry the working copy's dirty
sectors since install and refuse to load against a different NAND identity
(CID + console ID + size), the way `bios_id` is checked.

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
| Launcher hand-off experiments (`DS_DSI_HANDOFF=1/2`) | `nds_dsi.cpp` | diagnostic only; off by default |

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
   (13 scrolls launches Mighty Flip Champs instead; it also matches.) Still
   to do: the TLNC auto-launch on this boot, and the cart-present NAND boot
   (last diverged on SPIDATA at frame 37).
2. **Title installer + save export** (2.3), NAND working copy/overlay, `.cia`.
   Gate: an installed CIA title (Dr Mario KD9E, Plants vs Zombies KZLE) boots
   and saves; the exported `.pub` imports into melonDS with the same data.
3. **Mic / I2S**: `MICCNT`/`MICDATA`, 16-entry FIFO, half-full IRQ,
   `IRQ2_MicExt`, NDMA 0x2C. Oracle: Instrument Tuner (KTUE) uses the mic,
   though its DSP half waits for after 2.0.0.
4. **DSi Wi-Fi networking**: route the NWifi module's frames into the
   `wifi-emu` slirp backend.
5. **SD card slot** (port 0): FAT image or host folder.
6. **Unlaunch fallback**: add a boot2-replacement option to `trace_melonds`
   and see whether melonDS runs Unlaunch that way; re-run `--dsi-boot2` at
   current HEAD; then 2.2 items 1-3.
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

    cp dsi-binary/bios/dsinand.bin out/n.bin      # always a copy
    dsperate-headless --direct --interp --dsi --touch 600:128,100:10 \
      --bios9 bios9.bin --bios7 bios7.bin --firmware dsifirmware.bin \
      --bios9i biosdsi9.bin --bios7i biosdsi7.bin \
      --dsi-nand out/n.bin --dsi-nand-boot --frames 1300 --dump-frames out/o.frames
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
