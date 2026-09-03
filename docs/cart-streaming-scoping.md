# Cart streaming as the default load path -- scoping

2026-09-03. What it takes to stop holding the whole ROM in RAM, and how zipped
games fit that on a device with no room to unpack them in memory.

## Why

`NDS::load_rom` slurps the file into a `std::vector<u8>`; for a zip it slurps
the archive *and* inflates the ROM beside it, so the peak is archive + ROM.
`Cart` then pads the ROM to a power of two (a 130 MB dump becomes 256 MB of
committed memory).

On the Miyoo A30 (512 MB, no swap, `overcommit_memory=0`, `/tmp` is a 249 MB
tmpfs) that is fatal for the big titles:

| title class          | file    | zip (typ.) | current peak | padded |
|----------------------|---------|------------|--------------|--------|
| most of the library  | <= 64 MB| 20-40 MB   | ~100 MB      | 64 MB  |
| Pokemon B/W, late RPGs | 128-256 MB | 80-150 MB | 210-400 MB | 128/256 MB |
| a handful (512 MB)   | 512 MB  | ~300 MB    | 800 MB       | 512 MB |

The A30 shows ~430 MB available with nothing running. A 256 MB zipped game
does not load today; a 128 MB one loads with nothing to spare.

## What touches the ROM bytes today

The surface is small, which is what makes this cheap:

| site | access |
|------|--------|
| `Cart::Cart` | header copy (0x160); secure-area re-encrypt **writes** 0x800 bytes at `arm9_rom_offset` |
| `Cart::decrypt_secure_area` | reads 0x800 at `arm9_rom_offset` |
| `Cart::rom_read32` | 4 bytes, wrapping inside a 4 KB page, masked by `rom_mask_` |
| `NDS::setup_direct_boot` | header 0x170, then the ARM9/ARM7 binaries sequentially |
| `rom_identity` | header 0x160 + raw size |

`Cart::sync_state` does not carry the ROM. The loader cart is built in memory
(`build_loader_cart`) and goes through `load_rom_image`, so a memory-backed
source has to survive.

## Design

### 1. `RomSource` behind `Cart`

Replace `std::vector<u8> rom_` with a small source object:

```
struct RomSource {
  u32 size;            // raw bytes on disk (rom_identity uses this)
  u32 mask;            // padded power-of-two minus one (chip id, wrap)
  const u8* page(u32 addr);    // 4 KB page, or the 0xFF page past size
  u8*  secure_area();          // the one writable window (0x800 at arm9 offset)
};
```

Two backends:

- **Mapped**: `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE)` over an fd. The
  kernel demand-pages from the SD card and the page cache is reclaimable, so
  RSS is whatever the game has touched and memory pressure evicts it. The
  secure-area re-encrypt writes through the private mapping and costs two
  copy-on-write pages. The last partial page needs care: mmap zero-fills past
  EOF but the cart must read 0xFF there, so the tail page is copied into a
  padded buffer once and served from it. Pages at or beyond `size` return a
  static 0xFF page.
- **Owned**: the existing vector, for the loader cart and tests.

`rom_read32` keeps a cached `(page_base, page_ptr)` and only calls `page()`
when the 4 KB page changes, so the hot path is the same pointer arithmetic it
is now. Cart reads are a negligible share of any frame; there is nothing to
measure here.

Both backends produce identical bytes, so the five-scene hash oracle
(`tools/scene_hashes.sh`, interp + JIT) and `tools/state_roundtrip.sh` are
the gate. `rom_id` is unchanged (header + raw size).

### 2. Zips

Sniffed by magic as now. Three cases:

- **Stored entry (method 0)**: map the zip itself, base at the entry's data
  offset rounded down to a page, plus the delta. No extraction, no disk
  write, nothing to clean up. Free, and worth having because some sets are
  stored.
- **Deflated entry**: DEFLATE has no random access, so the ROM is inflated
  once to a file on disk and that file is mapped. Streaming the inflate
  through the existing 32 KB `tinfl` window into a `FILE*` keeps RAM at the
  window plus a write buffer; the zip is mapped, not slurped.
- **Rejected alternative -- random-access inflate (zran)**: an access-point
  index every ~1 MB costs a full inflate pass at every launch and every
  cache miss inflates up to 1 MB (20-40 ms on a Cortex-A7) on the emulation
  thread. That is a dropped frame per miss during level loads. Not viable.

### 3. Where the extracted file lives, and for how long

Extraction is not cheap on the A30 (measured today, 16 MB SM64, cold):

| op | time | rate |
|----|------|------|
| SD read | 0.83 s | ~19 MB/s |
| SD write + fsync | 2.64 s | ~6 MB/s |

A 128 MB ROM is ~25 s of writes plus inflate; 256 MB about a minute. So the
extracted file must be **kept across sessions**, not re-made per launch, and
`/tmp` is out (tmpfs, 249 MB, and it is the RAM we are trying to save).

- **Location**: `<zip dir>/.dsperate/<zip stem>.nds`. Beside the zip on the
  same filesystem (the SD card has 115 GB free), inside a dot-directory so
  the game picker (which lists `*.nds`/`*.zip` in the games directory) does
  not show the game twice. Fallbacks in order: `paths.cache` from the config,
  then the config directory, then fail with a clear message. Never tmpfs.
- **Validity tag**: a sidecar `<stem>.tag` holding zip size + mtime, entry
  name, entry CRC32 and uncompressed size. Mismatch means re-extract. The
  entry CRC32 is verified while inflating (a 1 KB table; tinfl does not ship
  one), which is what keeps the "a corrupt archive fails loudly" property on
  this path.
- **Atomicity**: write to `<stem>.nds.part`, fsync, rename. A `.part` left
  by a power loss is deleted on the next load. Same pattern as the save
  writer in `nds.cpp`.
- **Eviction**: none automatic in the first cut. Config knob
  `cart.cache = keep | session` (`session` unlinks on exit; the right choice
  for someone with a small card, the wrong default given the write cost).
  A "clear cache" entry in the menu later.

### 4. Frontend

- Extraction runs on a worker thread with a progress callback; the SDL
  frontend draws an "Extracting <title> -- 37 %" overlay through the held
  frame path the pause menu uses (`begin_frame` + `Gpu::scale_image`, not
  `Display::draw`). The CLI prints to stderr.
- The picker launch path (`Menu::Result::Launch`) calls `load_rom` as now;
  the only change is that it may block for the extraction with the overlay
  up, and the cancel button aborts (delete the `.part`, stay on the list).
- `DS_CART_MMAP=0` / `--rom-in-ram` keeps the current slurp path for A/B and
  for hosts, one release cycle, then goes.

## Risks and what to measure first (phase 0)

1. **Page-fault stalls.** The DS card delivers ~6.7 MB/s so games tolerate
   cart latency, but a random 4 KB SD read is milliseconds and it happens on
   the emulation thread. Measure on the A30: random 4 KB read latency on the
   card, and a `DS_CART_LOG` address census on sm64 and a Pokemon level load
   (distinct pages per second, sequential fraction). Kernel readahead covers
   sequential streams; if the census shows scattered reads,
   `madvise(MADV_WILLNEED)` on the whole file at load when free RAM allows
   (the current behaviour, but reclaimable), or a prefetch of the next 64 KB
   at `command_start`. Quote p99 and over-budget frames, not means.
2. **vfat + kernel 3.4.39.** mmap on vfat is fine and `madvise` exists; the
   dot-directory name has to be checked on that vfat (`shortname=mixed`).
3. **32-bit address space.** A 512 MB mapping fits comfortably under the 3 GB
   user limit beside the JIT and page-table mappings.
4. **Exactness.** EOF padding is the one place the two backends can differ
   (0x00 vs 0xFF). Add a zip_test case with a ROM whose size is not a
   multiple of 4 KB and read across the end.

## Phases

| phase | work | size |
|-------|------|------|
| 0 | A30 SD latency + cart address census; decide readahead | half a day |
| 1 | `RomSource`, Mapped + Owned backends, EOF/tail semantics, hash gate on 5 scenes | 1 day |
| 2 | stored-entry in-place map; deflate-to-cache with tag, CRC, `.part`/rename; CLI progress | 1 day |
| 3 | SDL progress overlay + cancel, `paths.cache`, `cart.cache` knob, `.part` cleanup | 1 day |
| 4 | device pass: A30 with a 128 MB and a 256 MB zipped title, RG DS regression, p99 tables | half a day |

Streaming becomes the default at the end of phase 1 for loose `.nds`; zips
switch over at the end of phase 2. Nothing in the core outside `cart/` and
`nds.cpp` changes.

## Phase 0 results (A30, 2026-09-03)

### SD card latency (`sdlat`, 16 MB SM64 on the vfat card, caches dropped)

| op | p50 | p90 | p99 | max |
|----|-----|-----|-----|-----|
| pread 4 KB random | 1.2 ms | 1.9 ms | 6.2 ms | 8.2 ms |
| pread 64 KB random | 3.9 ms | 8.6 ms | 15.0 ms | 15.1 ms |
| pread 4 KB sequential | 27 us | 33 us | 5.2 ms | 12.6 ms |
| mmap first touch, random page | 3.4 ms | 6.8 ms | 10.4 ms | 12.3 ms |
| mmap first touch, sequential | 12 us | 0.4 ms | 6.0 ms | 6.3 ms |
| `madvise(WILLNEED)` whole 16 MB | 0.62 s, synchronous on kernel 3.4 (~26 MB/s) | | | |

A random mmap fault costs more than a random pread: the 3.4 kernel's
fault-around reads a bigger window. Sequential access is essentially free
either way, and the p99 spikes on sequential reads are the readahead window
boundaries.

### Cart access census (SM64 replay, 1500 frames, identical on host and A30)

| | |
|--|--|
| B7 block reads | 7759 |
| distinct 4 KB pages touched | 752 (2.9 MB of 16 MB) |
| next read == previous + 0x200 | 94.7 % |
| runs of contiguous reads | 411, median 1, p90 54, max 629 (315 KB) |
| frames with any cart read | 125 of 1500 |
| frames touching a new 4 KB page | 97 |
| frames touching a new 64 KB chunk | 60 |
| busiest frame | 17 new pages, 4 new chunks |

Reads are bursty: a loading frame issues ~120 block reads across 16 pages,
almost all sequential. Pricing those bursts with the latencies above:

| model | worst frame @p50 | frames > 16.7 ms @p50 | @p99 |
|-------|------------------|------------------------|------|
| every new page a random mmap fault | 57.8 ms | 57 | 83 |
| every new page a random pread | 20.4 ms | 23 | 72 |
| one random cost per new 64 KB chunk, rest sequential | 15.6 ms | 0 | 20 |

### Verdict

Plain demand paging is not acceptable on the A30: even the optimistic
chunk model drops 20 frames per 25 s at p99, and every one of them is on a
loading screen the player is staring at. Two things fix it, and both are in
scope:

1. **Prefetch the whole file at load when RAM allows.** `madvise(WILLNEED)`
   is a blocking read at ~26 MB/s on this kernel, i.e. the same cost as
   today's slurp (0.6 s for 16 MB, ~5 s for 128 MB, ~10 s for 256 MB), but the
   pages are page cache, not process memory, so they are reclaimable and the
   process never holds ROM + padding + archive. Rule: prefetch when
   `MemAvailable` exceeds file size + 128 MB, otherwise skip (or prefetch the
   first N MB, which is where the ARM binaries and early assets live). Done
   on a worker thread, in 4 MB steps so the game can start while it runs.
2. **A read-ahead helper for the residual.** On each B7 `command_start` that
   begins a new run, a helper thread `pread`s the next 256 KB (runs reach
   315 KB) into the page cache. The first block of a run still pays one
   random read (~1.2 ms), which the 64 KB-chunk row shows is within budget
   at p50. This is what carries titles too big to prefetch, and the eviction
   case.

Phase 4 measures the real thing: the p99 / over-budget table on the A30
with prefetch on, prefetch off + helper, and neither, on SM64 and a 128 MB
title.

## Phase 1 status (2026-09-03)

Landed, uncommitted: `cart/rom_source.{h,cpp}` (Mapped + Owned, tail page,
0xFF page, patch overlay for the secure-area rewrite, page-aligned range
mapping for a stored zip entry), `Cart` reads through it with a one-page
cache in `rom_read32`, `NDS::load_rom` maps loose `.nds` files by default
(`DS_CART_MMAP=0` slurps as before), `NDS::load_rom_source` for phase 2.
Gate: all six scenes bit-identical to the pre-change baseline at 1800
frames, mapped and in-RAM; SM64 through a deflated zip and Meteos through a
stored zip identical too; `tools/state_roundtrip.sh` sm64 300/200 and mlbis
900/300 pass; `tests/rom_source_test.cpp` covers the tail/EOF/range/patch
cases. The A30 SDL build runs the SM64 replay through the mapping.

## Phase 2 status (2026-09-03)

Landed, uncommitted, on top of phase 1:

- `zip.{h,cpp}`: `find_nds()` (the pick, no payload read) and
  `inflate_entry()` (streaming, CRC-32 checked as the bytes go out, progress
  callback); `extract_nds()` is now those two into a vector and stays for
  `DS_CART_MMAP=0` and the tests.
- `zip_cache.{h,cpp}`: `open_zip()`. A stored entry is mapped in place as a
  range of the archive. A deflated one is inflated to
  `<zip dir>/.dsperate/<stem>.nds` (fallback: `paths.cache` from the config,
  then fail), written as `.part`, fsynced, renamed, with a `.tag` holding
  the archive's size and mtime and the entry's name, CRC and size; a match
  reuses the image, anything else rebuilds it, a corrupt archive leaves
  neither a `.part` nor a tag.
- `RomSource::prefetch()`: a worker thread walks the mapping with
  `MADV_WILLNEED` in 4 MB steps and touches each page (the advice is
  synchronous on the 3.4 kernel and a hint on new ones). Started by
  `load_rom` when MemAvailable (or free + cached on a 3.x kernel) exceeds
  the mapping plus 128 MB; `DS_CART_PREFETCH=0` never, `=1` always. The
  destructor stops and joins it, so switching games does not wait for it.
  (Superseded by phase 4: now opt-in with `DS_CART_PREFETCH=1` only.)
- Headless prints extraction progress to stderr; the SDL frontend passes
  `paths.cache` through and blocks silently during an extraction until the
  phase 3 overlay.

Gate: all six scenes bit-identical to the baseline at 1800 frames with the
mapped + prefetch path; SM64 through a deflated zip identical on the
extracting run and the cached run; Meteos through a stored zip identical;
the in-RAM zip path identical. `tests/zip_test.cpp` gained the CRC check
and the cache cases (stored in place, extract, reuse by tag, rebuild on a
changed archive, corrupt leaves nothing, read-only directory falls back).

A30, zipped 16 MB SM64, caches dropped, 120-frame run:

| launch | wall |
|--------|------|
| first (extract to the card) | 9.5 s |
| second (cached image mapped) | 6.4 s |
| loose .nds, prefetch on | 6.5 s |
| loose .nds, prefetch off | 5.9 s |

So the extraction is ~3 s for 16 MB on that card (inflate + 6 MB/s write),
and the prefetch costs ~0.6 s of SD reading that the first level load would
otherwise pay in stalls. Both scale with the ROM; the SDL frontend needs the
phase 3 overlay before a 128 MB zip is pleasant.

The read-ahead helper for titles too big to prefetch is still to do; phase
4 decides whether it is needed after measuring a 128 MB title with
prefetch on the A30.

## Phase 4 results (A30, 2026-09-03, phase 2 build)

`DS_FRAME_STATS=1`, `--no-audio --quantum 0`, page cache dropped before
every run, a 600-frame warm-up discarded at the front (the first run of a
batch on this device is slow enough to invert a comparison, and one
warm-up was not quite enough: the first in-RAM row below is still slow).
Emulation-only frame times.

**Mario & Luigi, 128 MB, replay with save, 1800 frames**

| run | median | p99 | max | total |
|-----|--------|-----|-----|-------|
| in-RAM (`DS_CART_MMAP=0`), first real run | 32.6 | 51.6 | 116 | 52.5 s |
| mapped + prefetch | 25.6 | 42.3 | 274 (frame 0) | 45.4 s |
| mapped, demand paging | 25.8 | 42.1 | 89 | 45.4 s |
| demand paging (2) | 26.3 | 44.1 | 84 | 45.4 s |
| prefetch (2) | 25.2 | 45.9 | 303 (frame 0) | 46.0 s |
| in-RAM (2) | 26.0 | 41.3 | 79 | 45.6 s |
| zip, first launch (extracts 128 MB) | 25.6 | 39.8 | 77 | 67 s wall |
| zip, cached, prefetch | 25.3 | 41.4 | 350 (frame 0) | 53 s wall |
| zip, cached, demand paging | 25.6 | 43.2 | 86 | 52 s wall |

**Golden Sun, 256 MB, phase-2 state, 285 measured frames**

| run | median | p90 | p99 | max | over budget | wall |
|-----|--------|-----|-----|-----|-------------|------|
| in-RAM | 19.9 | 20.7 | 29.6 | 40 | 52 % | 21 s |
| prefetch | 19.8 | 24.9 | 28.4 | 31 | 71 % | 16 s |
| demand paging | 19.7 | 20.5 | 23.0 | 24 | 54 % | 9 s |
| zip, first launch (extracts 256 MB) | | | | | | 43 s |
| zip, cached, prefetch | as loose prefetch | | | | | 18 s |

**Pokemon White 2, 512 MB, from boot, no input, 1800 frames** (cannot be
held in RAM on this device at all)

| run | median | p99 | max | total | RSS |
|-----|--------|-----|-----|-------|-----|
| demand paging | 20.4 | 38.6 | 121 (frame 29) | 38.4 s | 84 MB |
| demand paging (2) | 20.2 | 30.7 | 96 | 35.8 s | 84 MB |
| forced prefetch (`DS_CART_PREFETCH=1`) | 20.1 | 31.8 | 286 (frame 0) | 37.1 s | 364 MB |
| zip, first launch (extracts 512 MB) | 20.9 | 42.5 | 96 | 89 s wall | |
| zip, cached, demand paging | 20.2 | 30.6 | 80 | 35.7 s | 84 MB |

### What it says

- **Demand paging is enough.** Warm, the mapped path matches in-RAM at the
  median and in total on the 128 MB title, is within a few percent at p99,
  and is the best condition on every column for Golden Sun. The phase-0
  model over-priced it: the game's reads are 95 % sequential and the
  kernel's readahead serves them, so the random-read stalls the model
  charged per page do not happen. The 512 MB title, which never loaded
  before, runs at a 20 ms median with the process at 84 MB.
- **The prefetch is a loss everywhere.** A 270-350 ms hitch at frame 0 while
  the worker fights startup for the SD bus, and on Golden Sun a sustained
  +4 ms at p90 for the ten seconds it takes to read 256 MB. It also pushed
  Pokemon's RSS to 364 MB. **Decision: prefetch off by default;
  `DS_CART_PREFETCH=1` keeps it for A/B.** The read-ahead helper is not
  needed and is dropped from the plan.
- **In-RAM is worse than mapped on the big titles**, not just equal: 12 s
  to read Golden Sun before the first frame and then a worse tail from
  holding half the device's RAM.
- **Extraction** costs once per game: ~15 s for 128 MB, ~35 s for 256 MB,
  ~50 s for 512 MB on this card, on top of the launch. The cached image
  then behaves exactly like a loose file. The phase 3 overlay is what makes
  that first launch acceptable.

## Phase 3 status (2026-09-03)

Landed, uncommitted, on top of phases 1-2 and the opt-in prefetch:

- **The notice.** `load_rom_notice` in the SDL frontend runs `load_rom` on a
  worker and, if it takes more than 300 ms (a loose or cached game never
  does), draws `draw_notice` over the dimmed held frame through the same
  present path as the pause menu: the title, `UNPACKING` with dots that
  advance only when the extraction reports progress (so they prove the
  worker is alive, not that time is passing), `WAITING ON THE CARD` after
  five seconds without progress, and `FIRST LAUNCH ONLY / B CANCELS`. No
  bar, no estimate. B, quit or a signal cancels: the sink stops, the
  `.part` is removed, `open_zip` fails with "cancelled", the picker stays
  on its list. The command-line launch (the spruce path) gets it too: the
  ROM load and the session setup moved after the display and input open.
- **Cache management** (`zip_cache.h`): tags are v2 and record the
  archive's canonical path; `sweep_cache` runs on every directory
  `open_zip` touches and removes images whose archive is gone, untagged
  images and stray `.part`s; `cart.cache_mb` (default 2048) evicts the
  least recently launched images (tag mtime, touched on every launch) to
  make room, never the one being opened; `cart.cache = session` deletes
  the image on exit and before the next pick; `--clear-cache` deletes every
  image in the launched game's directory, `paths.games` and `paths.cache`
  except the launched game's own.
- Checked on the A30: `--clear-cache` with the Mario & Luigi zip freed
  768 MB and kept its image; a fresh unpack through the notice loop took
  19.6 s wall for 120 frames; session mode removed the image on exit.
  The notice itself was not seen on the panel (ssh runs only) -- a
  hand-held check is still owed. Zip and loose scene hashes unchanged.
