# RetroAchievements (Casual mode) with rcheevos -- scoping

2026-09-09. What it takes to put RetroAchievements into DSperate using
rcheevos, and why modelling against ROCKNIX on the RG DS makes the networking
a thin shim rather than a vendoring project.

Scope is deliberately **Casual only** (the mode RetroAchievements renamed from
"Softcore"). Hardcore is out: it is the half that has to police the emulator
(no save states, no cheats, no slowdown, no rewind, a reset on enable), and
that policing is a second project with a different risk profile. Everything
below is built so Casual works on its own and hardcore stays a later decision.

## Prior art

drastic-nano (GammaOS) added Casual RetroAchievements to DraStic and documented
it: `frameworks/base/cmds/drastic-nano/RETROACHIEVEMENTS.md` in
TheGammaSqueeze/GammaOSNextDistribution-14. It is the closest thing to a
reference integration for this console and it is worth reading before writing
any of phase 2 or 3. Where it has settled a question, this document says so
rather than re-deriving it; where we diverge (we are Casual-only, we hash from
our own bytes, we have save states to keep honest) that is called out too.

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

One rule to take from drastic-nano rather than discover: everything **not**
backed -- the 12 MB DSi-only padding above all -- reads as **zero**, and is left
for rc_client to mark the affected achievements unsupported at load. The
tempting alternative, serving whatever the DS has at that address, produces
false unlocks: a condition written against DSi RAM would be comparing against
unrelated live data instead of being disabled. Zero plus "unsupported" is the
honest answer.

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

**A second backend is proven, which matters for the static tiers.**
drastic-nano ships the subprocess route -- a worker thread that `fork`s and
`exec`s the device's `curl` binary (not `popen`), keeps the child pid, reads
body and status from its output, treats a transport failure as retryable, and
`SIGKILL`s an in-flight request if the game exits. So the fallback this document
lists below is not speculative; someone runs it in production against this same
server. It stays the fallback rather than the plan, because on ROCKNIX dlopen
avoids a process spawn per request and keeps the response in memory -- but it is
the obvious way to reach the A30 later, and the interface should be shaped so
that backend is a drop-in.

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

**Driving the runtime.** `rc_client_do_frame` is called **exactly once per
frame advance**, never batched and never twice. drastic-nano's notes are
explicit about why, and it is the kind of bug that would take a week to find:
rcheevos keeps a Delta (the previous frame's value) per memory reference, so
calling `do_frame` twice against one memory snapshot collapses Delta onto
Current and **single-frame edge triggers stop firing**. Achievements would
simply never unlock, with nothing in any log. When the emulator is paused, frame
evaluation stops and `rc_client_idle` runs at least once a second to keep the
session alive without evaluating frozen memory.

Worth noting our frame loop is in better shape for this than theirs: they
decoupled rendering from a free-running emulation thread and had to drive off
vblank, where `main.cpp`'s loop already advances exactly one frame per
iteration (`nds.run_frame()`, `main.cpp:2399`). One call site, one frame.

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
  following the `.ovr` precedent (`firmware-boot`); `Config::dir()` gives the
  location, and the file is mode **0600**, as drastic-nano's is. Their other
  lesson is worth copying: a stale token must fall back to a password login
  that re-establishes a fresh one, or a player whose token expires is locked out
  of their own account with no way back from inside the emulator.
- Settings: `cheevos.enabled`, `cheevos.username`, `cheevos.toasts`,
  `cheevos.unofficial`, `cheevos.encore` go through the existing
  `SettingsHost` key/value interface (`settings.h:82`) -- no new mechanism.
- Save states: rcheevos offers `rc_client_serialize_progress_sized` /
  `rc_client_deserialize_progress_sized`. Save states are *allowed* in Casual,
  so this matters: without it, loading a state leaves achievement progress
  describing a world that no longer exists. Note drastic-nano explicitly did
  **not** wire these -- it could afford not to, because its hardcore mode blocks
  state loading outright and its softcore simply wears the desync. A
  Casual-only emulator has no such escape, so this is one place we have to do
  more than the reference integration, not less. States are chunked with a format
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

## Client identity: not a blocker, but a hard requirement

An earlier revision of this document recorded a blocker here. It was wrong, and
the way it was wrong is worth keeping, because it is a trap anyone integrating
RetroAchievements will hit once.

A game lookup with curl's default User-Agent is refused outright:

```
$ curl -A "curl/8.21.0" "https://retroachievements.org/dorequest.php?r=gameid&m=<md5>"
{"Success":false,"Status":403,"Code":"unsupported_client",
 "Error":"This client is not supported.","GameID":0}
```

The same request, identifying itself honestly as what it is, succeeds:

```
$ curl -A "DSperate/0.1.0 rcheevos/12.4.0" "https://retroachievements.org/dorequest.php?r=gameid&m=<md5>"
{"Success":true,"GameID":14806}
```

So `unsupported_client` is the server refusing an **unrecognisable** client, not
an unapproved one. It is a well-formedness check on the User-Agent, and it
applies to every endpoint including the unauthenticated lookup above.

The requirement, which drastic-nano's notes state as a rule and we have now
confirmed from the outside, is a product token with **no spaces in the product
name**, numeric versioning so the server can negotiate, and an `rcheevos/<ver>`
clause. rcheevos builds the second half itself
(`rc_client_get_user_agent_clause`, `rc_client.h:142`); the product half is
ours, and `DSperate/<version>` is it. Two rules around it:

- It must always be **our** identity. Presenting another client's product token
  would clear the same 403, and that is circumventing an access control and
  misrepresenting the writer to a service whose data integrity depends on
  knowing who wrote. Not an option, ever, including "temporarily, for testing".
- It is a real interface, so it is set in one place and not composed ad hoc.

What registration *does* gate, per drastic-nano: "Hardcore credit requires the
`GammaOS-DrasticNano` identifier to be on the server's approved emulator list."
Hardcore credit -- which this project has scoped out. So **Casual is not
blocked**, and phases 3-6 are unblocked.

One honesty caveat: what is verified is identify-and-lookup, from outside, with
no account. Whether an unlock submission from an unregistered client is
accepted is untested -- it needs an account and a running session, i.e. phase 3.
Getting DSperate onto the approved list is still worth doing, and is the
prerequisite if hardcore is ever reconsidered.

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

Placement: **`src/cheevos/`, a library of its own.** Not the core, which must
stay free of a network transport, and which the headless frontend links; and
not loose files in `src/frontend/sdl/`, because `tests/` links frontend
translation units one at a time and a library is what makes that
straightforward. It depends on `dsperate_core` (for `cart::RomSource`, and
later `bus.main_ram` / `bus.dtcm`) and nothing depends on it but the SDL
frontend.

One new CMake option, `DSPERATE_CHEEVOS` (OFF until the feature is finished),
and no new `find_package` -- there is nothing to find, because nothing is
linked. With it off nothing references the library, so the default build is
what it was before any of this existed. Compile
out what we do not use: `RC_DISABLE_LUA`, no `RC_CLIENT_SUPPORTS_RAINTEGRATION`
(Windows-only toolkit), no `RC_CLIENT_SUPPORTS_EXTERNAL`.

## Phases

Each phase is useful on its own and testable without the next.

1. **Hash and identify, offline-shaped.** -- **DONE** 2026-09-09, see below.
2. **Memory reader plus a local runtime.** -- **DONE** 2026-09-09, see below.
3. **Session and transport.** -- **DONE** 2026-09-09, see below.
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

## Phase 1 as built

`src/cheevos/`: the vendored snapshot (`rcheevos/README.md` records the commit
and what is compiled), `cheevos_hash.cpp` (the `RomSource` filereader and
`ds::cheevos::rom_hash`), `tools/cheevos_hash.cpp` (prints the hash for the
files named on the command line) and `tests/cheevos_hash_test.cpp`.

The one core change is `RomSource::read_unpatched`, which reads past the
`patch()` overlay to the bytes the file holds *and returns how many it had*.
Both halves were needed, and neither is obvious:

- The overlay bypass is §2's trap. `RomSource::read` goes through `page()`,
  which serves the overlay first, and the secure-area rewrite lands on exactly
  the page where the ARM9 binary -- and therefore the hash -- begins.
- The short count is a second, quieter version of the same thing. rcheevos
  0-pads a truncated icon block, which it can only do if the reader reports a
  short read; `RomSource` otherwise serves 0xFF past the end of the image, and
  0xFF pads hash differently from 0x00 pads. Homebrew is where this shows up.

What it was checked against, given that RA's own database is unreachable (see
above):

- **An independent implementation.** A separate ~20-line Python implementation
  of the documented algorithm, written from the rule rather than from this
  code, agrees on **all 46 .nds files** in the local library.
- **Golden values in the test**, from that same independent implementation over
  a synthetic ROM, so the spec is pinned rather than the behaviour.
- **The trap, directly**: a test rewrites the secure area through `patch()`,
  confirms the rewrite is visible through `read()`, and confirms the hash does
  not move.
- **Zip and mmap equivalence**: one ROM raw, stored in a zip, and deflated in a
  zip all hash identically, as do an owned and a mapped source.
- **Exactness**: all five scenes bit-identical to the pre-change build over 200
  frames (`tools/scene_hashes.sh`), and the 18 tests pass.

- **RetroAchievements' own database**, once the User-Agent was right. Of six
  titles looked up through `r=gameid`, three came back with real game IDs
  (Sonic Rush 14806, The World Ends With You 4887, Phoenix Wright 12747) and
  three with `GameID:0`. That is the verification this document previously said
  was impossible. The lookup is an **exact** match against RA's hash table, so
  a non-zero game ID is proof the hash is byte-for-byte what RA holds; three
  independent exact matches do not happen by accident. The zeros are dumps RA
  has not hashed (a different revision or region), not failures -- the same code
  produced all six.

## Phase 2 as built

`src/cheevos/cheevos_memory.{h,cpp}` and `tests/cheevos_memory_test.cpp`, plus
`tools/cheevos_bench.cpp`. The vendored `src/rcheevos/` (the condition
evaluator) now compiles; `rc_client.c` and `rapi/` still do not.

`cheevos::Memory` is the window: `attach()`, `read()`, `supported()`, and the
two adapters rcheevos' entry points want. Three decisions in it are worth
knowing:

- **The region table is read from rcheevos at run time and checked**, not
  copied. `attach()` takes `rc_console_memory_regions(RC_CONSOLE_NINTENDO_DS)`
  and refuses to start if it is not the three regions with the sizes this code
  expects. If an upstream update ever gives the DSi hole real RAM, that is a
  loud failure rather than every achievement silently reading 12 MB off.
- **Unbacked reads return zero and report a short count.** The count is the
  signal: rcheevos reads "fewer bytes than asked" as "this address is not
  supported here" and disables the achievement, rather than evaluating it
  against whatever the DS left at that address.
- **Nothing goes through `Bus::io_read` or the page tables** -- just a bounds
  check and a `memcpy` from `bus.main_ram` / `bus.dtcm`. Emulation cannot tell a
  read happened: no write traps, no timing, no VRAM remap side effects.

### What it costs on the A55

Measured on the RG DS, `tools/cheevos_bench` (warm-up run discarded, per
`device-benchmark-warmup-reverses-results`), against a synthetic set whose
addresses are spread over the first 1 MB:

| set | mean | p50 | p99 | max | p99 as share of a 16.67 ms frame |
|-----|------|-----|-----|-----|------------------------------|
| 120 achievements x 6 conditions | 100.6 us | 96.2 us | 210.9 us | 482 us | **1.27 %** |
| 250 x 10 (pessimistic) | 380.3 us | 349.1 us | 755.1 us | 2113 us | 4.53 % |

The first row is the realistic one -- a large DS set is around a hundred
achievements -- and 0.60 % of a frame mean, 1.27 % p99, is affordable. Cost
scales roughly linearly in total conditions, so the second row is the shape of
the risk rather than a prediction.

Three caveats, because this is a synthetic set and it would be easy to read too
much into it:

- It is probably **harder than a real set**, not easier: the addresses are
  spread across 1 MB with no locality, where a real set clusters around the
  handful of structures a game keeps its state in. Cache behaviour favours the
  real one.
- The `max` column (482 us, 2.1 ms) is almost certainly scheduling noise rather
  than evaluation -- the benchmark runs at default priority on a live device --
  but it has not been separated out, so it is quoted rather than explained away.
- It does not include `rc_client`'s own per-frame bookkeeping, which phase 3
  adds on top, nor the network thread.

The one place to re-check is Golden Sun, which already runs over budget
(`compositor-thread-step1`): 1.3 % p99 is cheap in the abstract and less cheap
on a frame that is already late. Worth a look with a real set in phase 6.

### Rejected: evaluating on a presentation thread

Worth writing down, because it is the obvious idea and it comes back. The
thought is that the presenter already runs once per frame and is mostly idle
waiting on a vsync, so `do_frame`'s ~100 us could go there and off the
emulation thread. Three reasons not to, any one of them sufficient:

- **The thread is not there on the target.** A presenter thread is created only
  by `DispOut` (`display_disp.cpp:231`) and `FbdevOut`
  (`display_fbdev.cpp:110`) -- the A30 and H700 tiers. The DRM and Wayland
  tiers the RG DS runs present on the main thread. So it would be machinery
  built exclusively for the two `-static` tiers that cannot run achievements in
  the first place.
- **It is not one call per emulated frame.** The presenter takes the newest
  posted frame and drops the rest (`queued_ = pending_; pending_ = -1;`); that
  is the point of it, and `diag_flips_` / `diag_posts_` are counted separately
  because they diverge. Frameskip widens the gap. Driving the runtime from
  there means missed `do_frame` calls, which is exactly the hit-count and
  edge-trigger corruption `tests/cheevos_memory_test.cpp` pins down.
- **The memory would not be coherent.** Reading `main_ram` while the emulation
  thread writes it gives torn reads part-way through a structure, and a
  condition evaluated against a half-updated structure is how a false unlock
  happens -- the one failure mode here with consequences on other people's
  accounts.

The salvageable version is to hand a worker a *snapshot* rather than live
memory: at the frame boundary copy just the addresses the active set
references, which for a real set is a few hundred -- a 1-2 KB cache-friendly
copy, far cheaper than evaluating. That is defeated by pointer-following.
`RC_CONDITION_ADD_ADDRESS` and `num_indirect_conditions` are first-class in
rcheevos (`rc_runtime_types.h:149`) and real sets use them, so which addresses
get read depends on values read *during* evaluation and the set cannot be known
in advance.

So it stays on the emulation thread, at the frame boundary. What makes that an
easy trade is that the cost is **opt-in**: nothing is evaluated unless the
player is signed in and the game has a set, so 0.6 % mean / 1.27 % p99 is paid
by players who asked for it and by nobody else.

One warning for later: if the `compositor-thread` branch lands there *will* be
a real per-frame worker and this idea will look attractive again. Its lag mode
makes the one-call-per-frame problem worse, not better.

### Deliberately not done

The frontend is not wired up. `rc_client_do_frame` has nothing to drive until
there is a session and a set to evaluate, so a call in `main.cpp`'s loop now
would be dead code that still had to be kept correct; it lands in phase 3 with
the session, and the phases list above says so. What this phase proves is that
the window and the runtime are right and affordable, which is what phase 3
needs to build on.

## Phase 3 as built

`src/cheevos/cheevos_http.{h,cpp}` (the transport), `cheevos_client.{h,cpp}`
(the session and the credential sidecar), `tools/cheevos_session.cpp`,
`tests/cheevos_client_test.cpp`, a `[cheevos]` section in `config.cpp`, and
about forty lines in `main.cpp`. The whole of rcheevos we use now compiles:
`rc_client.c` and `rapi/` joined the runtime and the hash.

### The transport

`dlopen("libcurl.so.4")`, following `wl_dyn.h`, with nothing linked and no curl
package needed at build time -- the nine entry points and fourteen option
numbers we use are declared in `cheevos_http.cpp`. The option numbers are read
out of curl's own header and are safe to hard-code: curl assigns each a
permanent number and has never renumbered one, because every binary ever linked
against it would break.

Verified on the RG DS, against the live server:

```
cheevos: session up, transport libcurl (dlopen), user agent DSperate/1.13.1 rcheevos/12.4
cheevos: -> https://retroachievements.org/dorequest.php (POST)
cheevos: <- status 401, 123 bytes
  [problem] RetroAchievements sign-in failed -- Invalid user/password combination. Please try again.
```

That is a deliberately wrong password, and it is the useful test: the server's
*own* error message coming back means dlopen, TLS, ROCKNIX's trust store, the
User-Agent, rapi's request building, rapi's response parsing, the completion
hand-off and the message path all work. Everything bar a valid account.

A few details that are decisions rather than defaults: `CURLOPT_NOSIGNAL`,
because curl's alarm-based DNS timeout is not thread-safe and this runs on a
worker; `Expect:` cleared, so curl does not wait a second for a 100-continue
the server never sends; an 8 MB cap on a response body, since a confused reply
must not grow without limit on a 512 MB handheld; and `CURLOPT_CAINFO` left
*unset* so curl uses the store it was built against, with `DS_CHEEVOS_CAINFO`
as the escape hatch for a CFW that got it wrong.

### Threading

- `rc_client`, the memory reads and every rcheevos callback run on the
  emulation thread, and nowhere else. `rc_client_set_allow_background_memory_reads(c, 0)`
  makes that a promise rather than a convention.
- One worker thread does HTTP and touches neither `rc_client` nor guest memory.
- `frame()` drains completed requests -- invoking the rcheevos callbacks on the
  emulation thread -- and then calls `rc_client_do_frame`.

The worker drops itself to `SCHED_OTHER` and `nice(10)` on start. It inherits
whatever `emu.realtime` gave the process, and a thread that blocks on a socket
for 270 ms must not hold a real-time priority:
`rt-scheduling-closes-the-tail` found the frame tail was preemption by
unrelated threads, and this would be a new one.

### Casual mode, in one line

`rc_client_set_hardcore_enabled(c, 0)` immediately after `rc_client_create`,
because rc_client defaults it **on**. Nothing sets it back, and
`RC_CLIENT_EVENT_RESET` (which only hardcore raises) is logged and ignored.

### Unsupported addresses: already handled

Phase 2 left a note to wire `rc_runtime_validate_addresses`. It turns out not
to be needed, and the reason is worth recording. `rc_client` runs its own
`rc_client_validate_addresses` at game load (`rc_client.c:1346`) which, for
every memory reference in the set, does:

```c
if (memref->address > max_address ||
    client->callbacks.read_memory(memref->address, buffer, 1, client) == 0) {
  /* ... invalidate the achievements and leaderboards using it ... */
```

That is exactly the contract `Memory::read` was built to satisfy -- zero bytes
backed means "not supported here". So the decision in phase 2 to return a short
count rather than pretend is what makes unsupported achievements get disabled,
with no extra code at all.

### In the frontend

`cheevos.enabled` is off by default. When on: the session starts, the stored
token signs in, and the ROM is hashed once -- from `nds.cart->source()`, which
is only safe because `read_unpatched` reads past the secure-area rewrite `Cart`
has already done by then. `cheevos.frame()` is called exactly once per
`nds.run_frame()`, and `cheevos.idle()` on the paused path instead.

The set is *not* requested at boot. Signing in is asynchronous, so asking
immediately produced "Could not load achievements -- Login required", which
reads as a bug rather than as "you are not signed in". Instead a small
`cheevos_catch_up()` watches for the session reaching `SignedIn` and asks then
-- which is also the hook phase 4's menu needs, so signing in from the menu
loads the set for the game already running.

### Credentials

`<config>/cheevos.token`, mode 0600 from creation rather than chmod'ed after
(a token another user could read even briefly is a token to treat as leaked),
written to a temporary and renamed. Username and token only -- the password is
never stored. Tested: the round trip, the permissions, that a missing file is
not an error, that a half-written file is refused rather than used, and that a
newline in either field is refused rather than written.

### Importing the CFW's sign-in

Typing a password on a handheld with no keyboard is miserable, and on ROCKNIX
the player has usually signed in already -- EmulationStation's own
RetroAchievements sign-in writes to
`/storage/.config/system/configs/system.cfg`:

```
global.retroachievements.username=<name>
global.retroachievements.token=<16 chars>
global.retroachievements.password=<in clear text>
```

So `import_cfw_credentials()` reads that (and RetroArch's
`cheevos_username` / `cheevos_token`, which is the same thing in a different
shape, `key = "value"` rather than `key=value`). `cheevos.use_system_login`
controls it, default on, and ours wins whenever DSperate has a sign-in of its
own -- so signing in from our menu in phase 4 takes over from then on.

Two rules, both deliberate:

- **Only the token is read.** Those files keep the password in clear text
  beside it, and we do not want it: the token is all `rc_client` needs, it can
  be revoked on its own, and copying somebody's password into a second program
  is strictly worse than not. A test asserts the password never ends up in
  either field.
- **Their file is never written.** It is not ours, and a token we refreshed
  into it would be a change the CFW did not ask for.

### Verified end to end on the device

With that, the whole of phase 3 runs on the RG DS against the live server:

```
transport: libcurl (dlopen)
system login: /storage/.config/system/configs/system.cfg
  [info] Signed in to RetroAchievements -- <name>
hash:      3cd035c8692ec203a85b52d9e0c1938c
  [info] Sonic Rush -- achievements active
game:      14806  Sonic Rush
```

Sign-in from the CFW's token, our own hash, RetroAchievements' own game id, and
a real achievement set loaded and evaluating. A dump RA has not hashed (Spirit
Tracks, here) comes back as "No achievements for this game", which is the
message that has to exist or the feature looks broken.

### What a real set actually costs

The set above, measured on the RG DS over 2000 frames
(`tools/cheevos_session`):

| | mean | p50 | p99 | max | p99 share of a frame |
|---|---|---|---|---|---|
| Sonic Rush, real set | 54.0 us | 39.7 us | 150.2 us | 836 us | **0.90 %** |
| synthetic 120 x 6 (phase 2) | 100.6 us | 96.2 us | 210.9 us | 482 us | 1.27 % |

So the real set is *cheaper* than the synthetic estimate, which is what phase 2
guessed would happen and for the reason it gave: the synthetic addresses are
spread over 1 MB with no locality, where a real set clusters. The phase 2 number
stands as a pessimistic bound. Caveat on the real one: the console it measured
against was never booted, so conditions short-circuit differently than in play
-- it is indicative, not a final figure, and phase 6 should re-measure during
actual play.

### Still unverified

**Whether an unlock is credited.** Login and set loading are confirmed, and the
server served the set to DSperate without complaint, which is encouraging --
but an unlock needs a condition to actually fire, which needs somebody playing.
`--cheevos` turns the feature on for one run without touching the config, so
this is now a matter of playing a game rather than of writing anything.

## What I expect to go wrong

- **The User-Agent**, which is the one thing that silently disables the entire
  feature with a 403 and no other symptom. It belongs in one place, and the
  first thing phase 3 should log is the exact string it sent.
- **Delta collapse** from a stray second `rc_client_do_frame` in a frame --
  achievements that never unlock, with nothing in any log to say why.
- **Hash misses** against RA's real database for dumps they have not hashed
  (three of our six looked up as `GameID:0`). Not a bug, but it will be reported
  as one, so "this ROM is not one RetroAchievements knows" has to be a message
  the player can actually see.
- **Unlock submission from an unregistered client**, still untested: it needs a
  real account, not just a working transport.
- **Writing trigger expressions by hand**, which is how phase 2's tests are
  driven. In RetroAchievements' syntax an unprefixed `0x...` on the right of a
  comparison is a *16-bit memory read*, not a constant -- `0xH001004=0x2A` asks
  whether a byte equals the halfword at address 0x2A. A hex constant is `h2A`.
  It parses, it activates, and it silently never fires.
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
