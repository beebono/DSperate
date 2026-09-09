# RetroAchievements (Casual mode) with rcheevos -- scoping

2026-09-09. What it takes to put RetroAchievements into DSperate using
rcheevos, and why modelling against ROCKNIX on the RG DS makes the networking
a thin shim rather than a vendoring project.

Scope is deliberately **Casual only** (the mode RetroAchievements renamed from
"Softcore"). Hardcore is out: it is the half that has to police the emulator
(no save states, no cheats, no slowdown, no rewind, a reset on enable), and
that policing is a second project with a different risk profile. Everything
below is built so Casual works on its own and hardcore stays a later decision.

## Why this is mostly not an achievements problem

rcheevos is the achievement engine, and it is the part we do not have to
write. What it gives us:

| piece | what it does | our cost |
|-------|--------------|----------|
| `rcheevos/` | condition evaluation, triggers, leaderboards, rich presence | build it |
| `rapi/` | builds RA API request URLs/bodies, parses the JSON responses | build it |
| `rc_client.c` | the session: login, game identify/load, unlock queue, retry, events | build it |
| `rhash/hash_rom.c` | the canonical NDS game hash | build it, feed it bytes |
| `rc_consoles.h` | the DS memory map RA authors write against | one table lookup |

What it explicitly does **not** give us, and what we therefore own:

1. **An HTTP client.** `rc_client_create` takes a `server_call` callback; we
   supply the transport. rcheevos has no socket code at all.
2. **TLS.** The API hosts are `https://retroachievements.org` and
   `https://media.retroachievements.org` (`rc_api_common.c:12-15`). There are
   non-SSL host constants, but login sends a password, so plain HTTP is not an
   option.
3. **Any UI.** Login entry, an achievement list, unlock toasts, progress and
   challenge indicators are all ours.
4. **Image decoding.** Badges arrive as PNG. We have a PNG *writer* (miniz
   `tdefl`, used by the autosave thumbnail in `main.cpp:229`) and an inflate
   core, but no reader.

DSperate today has **zero networking** -- a grep for `curl`, `openssl`,
`mbedtls`, `socket(` or `netdb` across `src/` hits only Wayland protocol
headers' doc comments. This is the first network feature in the codebase.

## The five pieces

### 1. Memory map -- nearly free

RA authors DS achievements against three regions (`consoleinfo.c:737-752`,
`RC_CONSOLE_NINTENDO_DS = 18`):

| RA address | size | console address | DSperate |
|------------|------|-----------------|----------|
| `0x0000000` | 4 MB | `0x02000000` main RAM | `bus.main_ram` |
| `0x0400000` | 12 MB | -- | unused, DSi-only padding |
| `0x1000000` | 16 KB | `0x0E000000` pseudo (DTCM) | `bus.dtcm` |

Both are public `PageBuf` members on `Bus` (`mem/bus.h:100`), so
`read_memory` is a bounds check and a `memcpy` from a host pointer. It does
**not** go through `Bus::io_read` or the page tables, which matters twice: it
costs nothing, and it cannot perturb emulation (no write traps, no timing, no
VRAM remap side effects). Reads are exact by construction.

DTCM is a flat 16 KB buffer in DSperate, which is exactly the shape rcheevos
wants -- it deliberately exposes DTCM at a fake `0x0E000000` because the real
base is movable. No translation work.

This is the whole of the "emulator integration" in the traditional sense. It
is perhaps 40 lines.

### 2. The game hash -- one real trap

`rc_hash_nintendo_ds` (`hash_rom.c:386`) hashes the 0x160-byte header, then
the ARM9 and ARM7 binaries, then the icon, reading them **from the ROM file as
it sits on disk**.

The trap: DSperate does not leave those bytes alone. `Cart::Cart`
re-encrypts the secure area *in place*, writing 0x800 bytes at
`arm9_rom_offset` (documented in `cart-streaming-scoping.md`). The ARM9
binary the hash wants starts at `arm9_addr`, so a hash taken after `Cart`
construction can be wrong -- and a wrong hash means the game is silently not
recognised, which is the single most annoying failure mode to debug. **Hash
before `Cart` touches the source**, and gate it with a test.

rcheevos lets us replace file IO wholesale via `rc_hash_filereader_t`
(`rc_hash.h:45-52`). Pointing that at our `cart::RomSource` is the right
move and kills two problems at once:

- zipped ROMs hash correctly without inflating to a temp file (the A30 has no
  room for that -- see `cart-streaming-scoping.md`), and
- we hash the mmap'd pristine bytes rather than a mutated buffer.

With a custom filereader we can then build `rhash` with `RC_HASH_NO_DISC`,
`RC_HASH_NO_ENCRYPTED` and `RC_HASH_NO_ZIP`, dropping `hash_disc.c`,
`hash_encrypted.c`, `hash_zip.c`, `cdreader.c` and `aes.c`. What remains is
`hash.c`, `hash_rom.c`, `md5.c`.

### 3. Networking -- borrow it from the CFW

**Model against ROCKNIX on the RG DS.** It is the primary target and it ships
the entire stack already. Probed 2026-09-09 (ROCKNIX 20260909, kernel 7.0.2):

| what | there |
|------|-------|
| `libcurl.so.4` -> `libcurl.so.4.8.0` | curl 8.21.0, with nghttp2 (HTTP/2) |
| `libssl.so.3`, `libcrypto.so.3` | OpenSSL 3.6.3 |
| `/etc/ssl/certs/ca-certificates.crt` | real file, 227 KB |
| `/etc/pki/tls/certs/ca-bundle.crt` | -> `/run/rocknix/cacert.pem`, same bundle |

And it works end to end against the real hosts, from the device:

```
api:401 tls:0 t:0.270793        # retroachievements.org -- 401 is "no API key", i.e. we reached RA
media:200 verify:0              # media.retroachievements.org -- badge art reachable
```

`tls:0` is `ssl_verify_result` success, so the CFW's trust store validates RA's
chain with no help from us. That deletes three items from the original scope:
no vendored mbedTLS, no hand-written HTTP/1.1 client, no shipped CA bundle and
no refresh tooling for it. A 270 ms round trip is also a useful number for
later -- it is why the transport must not be on the emulation thread.

**Load libcurl with `dlopen`, do not link it.** This is already the house
pattern, and `wl_dyn.h` says why in its own header comment: libwayland is
resolved at runtime "so the binary carries no Wayland link dependency:
handhelds without a compositor ... must not fail to start over a missing
library". Exactly the same argument applies here, and more strongly --
achievements are optional, so a device without libcurl should lose the feature
and nothing else. Practical consequences:

- We `dlopen("libcurl.so.4")` -- the versioned soname, not the `libcurl.so`
  devel symlink, which only exists on ROCKNIX by luck and will not exist
  elsewhere.
- We need no curl package at build time. The handful of entry points we call
  (`curl_global_init`, `curl_easy_init/setopt/perform/getinfo/cleanup`,
  `curl_easy_strerror`, `curl_slist_append/free_all`) get declared in a small
  vendored header with the `CURLOPT_*` values we use. Those values are frozen
  ABI -- curl never renumbers them -- and a trimmed ~80-line header is more
  auditable than pulling in upstream `curl.h` and its includes.
- Let libcurl use its own compiled-in default CA location rather than setting
  `CURLOPT_CAINFO`. It is right on ROCKNIX, and it is right on any CFW that
  built its own curl. Keep a config override for the one that isn't.

**Where this degrades, and the cost of accepting that.** The two static
handhelds do not get achievements in v1, and it is worth being blunt that
this is a real limitation rather than an oversight:

| device | libcurl.so | CA bundle | our build | v1 |
|--------|-----------|-----------|-----------|-----|
| RG DS / ROCKNIX | **yes** | **yes** | dynamic | **works** |
| RG35XX SP | no | yes | `-static` | no |
| Miyoo A30 | no | none | `-static` | no |

Both secondary tiers link `-static` (`rg35xxsp-static-build`,
`miyoo-a30-build-and-display`), and `dlopen` from a fully static glibc binary
is unsupported -- so those builds could not use a CFW library even if one were
installed. Two later routes exist, both behind the same transport interface
and neither in v1: link libcurl statically into those two tiers, or shell out
to a `curl` binary (the A30 has one at `/mnt/SDCARD/spruce/bin/curl`). Putting
the transport behind an interface from the start is what keeps those cheap, so
do that even though v1 has exactly one backend.

**Threading.** HTTP runs on its own worker thread; nothing blocking goes near
the emulation thread -- see the 270 ms above. rcheevos' own locking is
`pthread_mutex` via `rc_compat.h`, so it tolerates a worker, but we queue
completions and invoke the rcheevos callbacks from the emulation thread inside
`rc_client_do_frame` anyway. That keeps `read_memory` single-threaded and
keeps achievement evaluation ordered against frames.

That thread is a **p99 risk, not a mean risk**, and this codebase has been
bitten by exactly this before: `rt-scheduling-closes-the-tail` found the frame
tail was preemption by unrelated threads, and the raster band workers cost the
JIT 16 % through shared-cache eviction. The network thread must be
`SCHED_OTHER` and nice'd below everything in `rt_thread.h`, and idle most of
the time. Note that libcurl with OpenSSL brings its own threads and memory
behaviour into our process, which is a second reason to measure rather than
reason about it.

### 4. UI

`menu.cpp` already has the pieces, which is unusually lucky:

- a page stack (`Menu::Page`) to hang an Achievements page off,
- a `TextEdit` page and `open_text_edit()` -- username and password entry
  exists,
- `draw_notice()` for a full-screen message,
- and a timed OSD pattern: `SLOT_OSD_FRAMES = 90` with `draw_label`
  (`main.cpp:613, 2480-2532`), which is precisely an unlock toast with the
  countdown already written.

So v1 needs: a login/account page, an achievement list page (rcheevos hands us
`rc_client_create_achievement_list`), and toasts on
`RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED` / `GAME_COMPLETED` /
`SERVER_ERROR` / `DISCONNECTED` / `RECONNECTED`.

Two things to be careful of, both already learned the hard way:

- The OSD/menu draws at panel resolution on the display engine's overlay layer
  (`a30-overlay-layer-menu`, `display_disp.h:126`), and on the disp and
  renderer tiers **the canvas has no CPU surface** -- the reason the options
  menu on `options-menu` branch is still unmerged. Toasts have to work on
  every display tier, so they follow the existing `slot_osd` paths
  (`main.cpp:2459-2532`), which already handle canvas / target / framebuffer
  separately. Do not invent a fourth path.
- Badge art means writing a PNG reader on top of our existing `tinfl`. The
  media host is reachable (200 above), so this is possible -- but it is real
  work for decoration. **v1 ships text-only toasts**; badges are a later item,
  and rcheevos is happy to never be asked for an image.

### 5. Persistence

- Credentials: RA issues a token on password login; store the **token**, never
  the password. A sidecar beside the config rather than `dsperate.ini` itself,
  following the `.ovr` precedent (`firmware-boot`). `Config::dir()` gives the
  location.
- Settings: `cheevos.enabled`, `cheevos.username`, `cheevos.toasts`,
  `cheevos.unofficial`, `cheevos.encore` go through the existing
  `SettingsHost` key/value interface (`settings.h:82`) -- no new mechanism.
- Save states: rcheevos offers `rc_client_serialize_progress_sized` /
  `rc_client_deserialize_progress_sized`. Save states are *allowed* in Casual,
  so this matters: without it, loading a state leaves achievement progress
  describing a world that no longer exists. States are chunked with a format
  version (`state/state.h:14-27`, `FORMAT_VERSION = 1`), so this is a new
  optional chunk -- old states load without it, and a state without the chunk
  calls `rc_client_reset`.

## Hardcore stays off, on purpose and explicitly

`rc_client` defaults to hardcore **on** (`rc_client.c:173`). Casual mode is
therefore not the absence of code, it is an explicit
`rc_client_set_hardcore_enabled(client, 0)` immediately after
`rc_client_create`, before any game loads. Getting this wrong would submit
hardcore unlocks from an emulator that permits save states and cheats, which
is the one failure here with consequences outside our own build -- it puts bad
data on other people's accounts. It deserves a test, not a comment.

## Non-technical blocker, worth resolving first

RetroAchievements gates which clients may talk to the API, and rcheevos
expects a client to identify itself (`rc_client_get_user_agent_clause`,
`rc_client.h:142`). Before building much of this, DSperate should be raised
with the RA team to confirm what an unregistered client may do -- in
particular whether Casual unlocks from a new client are accepted. If the
answer is "not until registered", that is a conversation to start now rather
than after the transport exists. I have not verified current RA policy here;
it needs asking, not guessing.

## Build shape

With the transport borrowed, rcheevos is the *only* thing vendored -- ~30 k
lines of C, of which we build most:

| `src/` | lines |
|--------|-------|
| `rcheevos/` | 10.7 k |
| `src/` (rc_client, rapi common, util, compat) | 9.4 k |
| `rhash/` | 6.0 k (we build ~half) |
| `rapi/` | 4.2 k |

Vendored the way miniz is (`src/core/cart/miniz/`, with a `README.md` saying
what was taken and what was compiled out) rather than as a submodule -- that
is this project's established pattern and it keeps the device builds
reproducible. The only other new source is our trimmed curl declarations and
the dlopen shim, modelled on `wl_dyn.{h,cpp}`.

Placement: **frontend, not core.** It needs config, UI, threads and sockets,
all of which live in `src/frontend/sdl/`. It only needs `NDS&` for
`bus.main_ram` and `bus.dtcm`. `dsperate_core` stays free of networking, and
the headless frontend stays unaffected.

One new CMake option, `DSPERATE_CHEEVOS` (OFF initially), and no new
`find_package` -- there is nothing to find, because nothing is linked. Compile
out what we do not use: `RC_DISABLE_LUA`, no `RC_CLIENT_SUPPORTS_RAINTEGRATION`
(Windows-only toolkit), no `RC_CLIENT_SUPPORTS_EXTERNAL`.

## Phases

Each phase is useful on its own and testable without the next.

1. **Hash and identify, offline-shaped.** Vendor rcheevos, build it, wire the
   `RomSource` filereader, compute the NDS hash for a ROM. Verify against
   known RA hashes for a handful of titles. No network yet. Proves the trap in
   §2 is handled and gets the build working.
2. **Memory reader plus a local runtime.** `read_memory` over
   `main_ram`/`dtcm`, `rc_client_do_frame` each frame, hardcore explicitly
   off. Drive it with a hand-written achievement set to prove triggers fire.
   **Measure here**: `rc_client_do_frame` cost on the A55 with a real set
   loaded, p99 and over-budget frames, before any networking exists.
3. **Transport.** The libcurl dlopen shim behind a transport interface, the
   worker thread, a queue draining on the emu thread. Login with password,
   token persisted, game identify and load. Still no UI beyond stderr. Small
   enough now that it is no longer the project's centre of gravity.
4. **UI.** Login page, achievement list, text toasts on the existing OSD
   paths, across display tiers on device.
5. **Save state integration** and the progress chunk.
6. **Device pass.** p99 and over-budget frames on the RG DS, PGO-vs-PGO in the
   SDL frontend per `pgo-frontend-ab-rule`, both run orders per
   `ab-run-order-bias`. Confirm the other two tiers still *build* and cleanly
   report the feature as unavailable.

Leaderboards and rich presence fall out of rcheevos almost free once 1-4 are
done and are worth doing; badge art, the static-tier transport and hardcore
are separate decisions.

## What I expect to go wrong

- **Hash mismatches** from the secure-area rewrite (§2) or from SuperCard
  headers, presenting as "game not found" rather than as an error. This is now
  the top risk, having displaced the TLS work.
- **CFW variability.** We are depending on someone else's library set, so a
  ROCKNIX update can move it -- and `rig-device-access` already records that a
  ROCKNIX update wipes `/storage/dsperate` and the BIOS dumps. The failure has
  to be a clean "achievements unavailable: libcurl.so.4 not found" on a log
  line, never a crash or a failure to start. `wl_dyn`'s sticky-failure
  approach is the model.
- **p99 on the device**, from the worker thread and from OpenSSL's own
  threading inside our process. Idle 99 % of the time is the saving grace, but
  it needs proving, not assuming.
- **Toasts on the disp/renderer tiers**, for the same reason the options menu
  is still on a branch.
