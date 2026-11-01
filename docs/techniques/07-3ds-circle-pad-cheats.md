# 3DS Circle Pad cheats, without the 3DS

This document is unlike the others in this directory. Documents 00–06 describe
DraStic's techniques, reconstructed from its binary. This one describes a
feature DSperate implements itself: support for shocoman's Circle Pad (CPAD)
patches — the Action Replay codes that let a Nintendo DS game read the 3DS's
analog stick when it runs on that hardware under TWiLightMenu++/TWPatcher.
Here the same codes run on an emulator on ARM handhelds, and the player's
physical sticks take the Circle Pad's place.

The provenance is therefore different and is stated plainly. The patch format,
the ARM9 hook and every per-game offset are shocoman's, from the public
repository `Analog-Controls-for-NDS-Games-on-3DS` (Super Mario 64 DS plus
about seventy other titles under `patches_for_other_games/`). The RTCom
protocol description is Sono's, posted in the GBAtemp thread that accompanies
the patches. Everything about DSperate's side is from the source in this tree:
`src/core/input/`, `src/core/cheat/`, `src/frontend/sdl/input.*` and
`ds_emu_dac.py` in [ds-emu-dac](https://github.com/bmpriest/ds-emu-dac), which owns the
codes. Where the 3DS behaviour is described from
reading shocoman's ARM7/ARM11 code rather than from running it, §8 says so.

The one-sentence version: **a CPAD patch is three mechanisms stacked — a
transport, a relay and a game hook — and an emulator only needs the hook.**
The transport exists to move two bytes across a machine boundary that does
not exist here, so DSperate deletes it and writes the data itself.

---

## 1. The three layers on a 3DS

A DS game running on a 3DS has no path to the Circle Pad. The pad is read by
the ARM11; the game runs on the ARM9/ARM7 pair in DS mode, behind the TwlBg
process. The patch bridges that with parts that are worth telling apart,
because only one of them knows anything about the game.

### 1a. RTCom: a mailbox made of RTC registers

RTCom (Sono/Gericom) uses two free 1-byte legacy RTC registers as a mailbox
between ARM7 and ARM11: `REG_112` is the data byte, `REG_113` the
status/command byte. The vocabulary is small:

| Byte | Meaning |
|---|---|
| `0x00` | no command; RTCom released for this frame |
| `0x01` | ping (deliberately invalid; a present RTCom answers `0x82`) |
| `0x80` | ACK from ARM11 |
| `0x81` | ACK/next-byte from ARM7 when streaming |
| `0x82` | NAK/done from ARM11 (and the ping's answer) |
| `0x83` | keepalive from ARM7 |
| `0x41` | upload code: size in four `0x81` steps, then the bytes |
| `0x42` | make the uploaded region executable |
| `0x44` | call the uploaded code with one data byte |
| `0xFE` | synchronous release for this frame |

Each frame is a cycle of release (`0xFE`), re-request (`0x01`, answered
2–3 VBlanks later by RTC IRQ), work, and release again, with an autorelease
timeout if the ARM7 does not keep the channel alive with `0x83`. The
uploaded usercode has the signature `u8 usercode(u8 data, u8 iterator)`,
returns its byte in `r0`, and signals errors in `r4`. This is the whole
transport. It moves bytes; it knows nothing about sticks.

### 1b. The relay: TwlBg writes, ARM7 copies

The uploaded ARM11 code patches TwlBg at runtime. From then on, once per
frame TwlBg writes the pad state into legacy RTC registers on the NDS side
(`arm11_twlbg_patch.s`): Circle Pad X/Y into the Alarm-Time-2 register, and
— only in the New 3DS build — the C-Stick (nub) position and the ZL/ZR bits
into the Counter register. Injected ARM7 code then bit-bangs those registers
over the ordinary RTC port (`0x04000138`) and stores the result into shared
RAM at the fixed address `0x027FFDF0` (`RTCOM_DATA_OUTPUT` in `rtcom.cpp`):

| Address | Contents |
|---|---|
| `+0` (u16) | `CPadX \| CPadY<<8`, both signed bytes |
| `+4` (u32) | `ZLZR \| NubX<<8 \| NubY<<16`; ZL = bit 2, ZR = bit 1 of the low byte |

Two details in the relay matter later. The nub is rotated 45° before it is
stored — `new_x = sin45·(x−y)`, `new_y = sin45·(x+y)` with
`sin45 = 0xB50/0x1000` — because the C-Stick's axes sit diagonally. And in
the non-nub TwlBg build the `+4` word is written as zero: the data is not
merely unused, it is actively absent.

### 1c. The hook: the only game-specific part

The ARM9 half (`arm9_controls_hook.s`) is copied to a safe address
(`BASE_PATCH_ADDR`, `0x02004B00` for SM64DS) and a branch is inserted into
the game's input-update routine. Each frame it reads `0x027FFDF0`, and if
the stick is off centre it reproduces what the game does with its
touchscreen virtual joystick: speed is `sqrt(x²+y²)` normalised to
`0x1000` against `CPAD_MaxRadius = 0x69`, plus angle, sine and cosine from
the game's own sqrt/atan2 helpers, written into the game's controls struct
(speed at `+0x8`, sin `+0xA`, cos `+0xC`, angle `+0xE`, "stick active" flag
at `+0x14`). It then skips the touchscreen/D-pad update for that frame.
Centred, it falls through to the original instruction and the game behaves
exactly as unpatched. For SM64DS USA v1.0 (`ASME-AEA63749`) the per-ROM
values are:

| Symbol | Value |
|---|---|
| input-update inject address | `0x0202B324` (instruction replaced: `0xE7D01108`) |
| hook base | `0x02004B00` |
| controls struct | `0x02097594` |
| sqrt / get-angle helpers | `0x0203B898` / `0x02039684` |
| branch written at the inject address | `0xEAFF65FB` |

Every title and revision has its own row of these, tabulated in that game's
`generate_patch.py` upstream. That table — not the transport — is the asset
worth reusing.

---

## 2. What DSperate already had

Three pieces of delivery machinery predated this feature, and the design is
mostly "do not build a fourth":

- **The Action Replay engine** (`src/core/cheat/ar_engine.cpp`) runs enabled
  codes from the ARM7's VBlank IRQ, as a real cheat cartridge does
  (`CpuContext::check_irq`), and writes through the bus's DMA accessors, so a
  write over guest code invalidates JIT blocks via `store_code`. The opcodes
  the CPAD codes use — plain 32/16/8-bit writes, the `E` bulk copy, and the
  `5` conditional — are all supported; only the native-code `C2`/`C4`
  extensions are refused, and these codes never used them.
- **The database path**: `usrcheat.dat` (R4 format) is matched to the cart by
  game code and header checksum (`database.cpp`, JAMCRC over the 512-byte
  header), codes are toggled per game in the pause menu, and the enabled set
  persists by name in a per-game `.cheats` file.
- **The sticks themselves**: the MinUI/NextUI handhelds this targets expose
  both sticks and both triggers through the pak's SDL game-controller
  mappings, and the frontend already consumed SDL axis events — it just
  collapsed them to digital buttons past a deadzone, or drove the stylus.

What was missing was everything between "SDL reported an axis" and "guest
RAM at `0x027FFDF0` holds signed bytes".

---

## 3. The stripped cheat

Running the upstream codes unchanged is impossible, not just wasteful: they
open by performing the RTCom handshake against registers DSperate's RTC
model does not have (112/113, the Counter-extended read), uploading code to
an ARM11 that does not exist. The ARM7 half would spin on a status byte no
one will ever write.

So the codes are stripped to their ARM9 section. An upstream file is two
conditional sections; the second `D2000000` separates them. Everything from
that separator onward is kept — for SM64DS USA v1.0, 64 pairs (128 words,
the hook body itself `0x1E0` bytes) out of 309. `ds_emu_dac.py` (in
[ds-emu-dac](https://github.com/bmpriest/ds-emu-dac)) performs the strip for a whole upstream `action_replay_codes` folder and
refuses to emit a code that fails three checks: it starts with the install
guard, it contains an `E` bulk write, and — walking opcode positions,
stepping over `E` data, whose words are instructions that can look like
addresses — nothing writes ARM7 WRAM (`0x03000000–0x03FFFFFF`), which is
where the RTCom code used to live.

The guard deserves its line, because it settles the obvious performance
question. The first pair tests whether the word at the inject address is
still the original instruction (`0xE7D01108`); the hook body and branch are
written only while it is. DSperate runs cheat codes on every ARM7 VBlank
IRQ, so an unguarded `E` copy would rewrite 480 bytes through the JIT
invalidation path several times a frame. With the guard, the steady-state
cost of an installed patch is one conditional read per run and a walk over
skipped data. That measurement — by construction, and pinned by the
`test_analog_hook_sm64ds` test in `tests/cheat_test.cpp`, which asserts the
hook lands, the ARM7 sentinels survive, and a second run does not rewrite a
deliberately corrupted hook byte — is why there is no separate
apply-once patch engine. The hook's `InitialHook` prologue, which installs
the branch when physically patching a ROM, is dead bytes in this form; the
AR code writes the branch directly.

---

## 4. The analog channel (`src/core/input/`)

`input::Frame` carried buttons, pen position, lid and mic samples. It now
also carries two sticks in SDL range (`cpad_x/y`, `cstick_x/y`) and `zl`/`zr`
(`input_log.h`). The input log grew to DSIN version 3 — 24-byte records, the
ZL/ZR bits folded into the flags byte, the four stick axes appended as
`s16` — with versions 1 and 2 still reading back with the analog fields
zero. A replay therefore carries a patched session's sticks (§8 says what else it needs), and an old
replay cannot accidentally steer.

`input::apply()` writes the relay layout, once per frame, before the run:

- `+0`: each CPad axis divided to a signed byte (`v/256`, clamped).
- `+4`: ZL as bit 2 and ZR as bit 1 of the low byte, then the nub —
  **unrotated**. The relay's 45° rotation compensates the New 3DS
  C-Stick sensor being mounted diagonally in the shell; host sticks are
  axis-aligned, and applying the rotation anyway turned a pure right
  push into yaw-plus-pitch on device. (The first version replicated
  the rotation faithfully and was wrong for exactly that reason.)

The layout is not universal, a fact learned from a device report:
Metroid's own relay stores the nub as `s16` at `+6`/`+8` (ZL/ZR byte at
`+4`, gyro words at `+10`), so a packed write leaves its hook reading
the packed Y byte as its X and a never-written word as its Y — the
camera turned one way and never pitched. `NDS::analog_layout` selects which arrangement `apply()` writes. The two
cannot both be written, since they collide at `+6`, so the frontend reads it
from the enabled code: `cheat::reads_extended_analog` looks in the code's bulk
writes for an ARM `ldrsh` at offset 6 or 8, which is how an extended hook takes
the nub. The generic relay is packed; roughly a quarter of the
catalogue (64 of 229 ROMs) is extended, and the rule gives the same answer as
each game's relay source for every code bundled. It is a pattern and not a
proof, so `pad.analog_layout` (`auto`, the default, or `packed` or `extended`)
can overrule it.

The writes go through `bus.dma_write16/32` from the ARM9 side. They are
gated on `NDS::analog_patch`: with no patch, `apply()` writes nothing to
`0x027FFDF0` however hard the sticks are pushed, and an unpatched game sees
no behaviour change. The flag (and `analog_full`, §6) is deliberately not
part of a save state: the words are rewritten every frame before
`run_frame()`, like buttons and touch, so a state neither needs nor could
use a saved copy.

`tests/io_test.cpp`'s `test_analog_apply` pins the layout end to end:
`cpad (32767, −32768)` reads back as `0x807F`, a pure `+X` nub as
`(+127, 0)`, centred input clears both words, and the v3
log round-trips the fields.

---

## 5. Frontend capture (`src/frontend/sdl/input.*`)

SDL axis events arrive post-remap (the pak's mappings name the axes
positionally, and `pad.axis_*` remapping runs before this). `Input::axis()`
now also records the raw `Sint16` value of each stick and trigger, which
nothing previously retained — the stylus and stick-as-d-pad paths consume
directions and thresholds, not magnitudes.

`fill_analog()` converts a raw stick to the frame's fields, in `scale_stick`.
It flips Y, because SDL's up is negative and the hooks take the 3DS's, where up
is positive. It also applies a radial deadzone: below the deadzone the stick is centred; beyond
it, the remaining travel is rescaled to full range and the magnitude capped
at 1, so a full diagonal is full speed rather than √2 times it (the hook
divides by `CPAD_MaxRadius` and clamps its own way; feeding it √2 would
only saturate earlier on diagonals). ZL/ZR use the same past-¾-travel rule
the trigger bindings use (`max(deadzone, 24576)`), which is not a tuning
choice so much as a device fact: on the pre-rc11 H700 layout the triggers
are *buttons* (`b13`/`b14`) that SDL synthesises as 0/32767 axes, so one
threshold serves both physical forms.

Configuration is six keys, live on the pause menu's Analog page, under a link
to the game's analog cheats:

| Key | Values | Default |
|---|---|---|
| `pad.analog` | `off \| left \| right \| dual` | `off` |
| `pad.analog_deadzone` | SDL units, 0–30000 | `6000` |
| `pad.analog_sensitivity_left` / `_right` | percent, 10–300 | `100` |
| `pad.analog_invert_y` | bool | `false` |
| `pad.analog_layout` | `auto \| packed \| extended` | `auto` |

`dual` is the two-stick shape (left = CPad, right = nub); `left`/`right`
put the CPad on one stick for one-stick handhelds. A configure-time warning
fires if the chosen stick is also the stylus stick — both would read it —
because on these devices the same per-game file that enables analog is
expected to set `stylus_axis = none`.

Activation is decided in `Session::refresh_analog`, from the core's
`cheat::analog_request`, which headless replay uses too. It is called at game load and whenever the Cheats menu dirties the enabled set:
`NDS::analog_patch` is set only when `pad.analog` is not `off` *and* an
enabled, non-note cheat's name starts with `CPAD`. Either condition alone
does nothing — sticks configured for a game with no patch never write guest
RAM, and a patch enabled on a stick-less profile simply reads a centred
stick and falls through to the game's own input path.

---

## 6. The two variants, and the bug they exposed

Upstream ships most games twice: `CPAD` and `CPAD [with Nub stick, ZL and
ZR]`. The hook binary is identical in both. What differs between them lives
in the layers DSperate deleted — specifically, in the TwlBg build: plain
writes `+4` as zero (§1b), the nub build writes it live.

The first DSperate version wrote `+4` live whenever any CPAD cheat was on.
The result, reported from a device, was that both cheats gave the same
dual-stick and ZL/ZR behaviour — correctly diagnosed as an emulator bug,
not a cheat bug: the hook cannot tell where its data came from, so data
that should not exist turned the camera anyway. The test reproduced it
exactly: with the plain variant's flags, the word read back `0x5A5A04`
(rotated nub at full `+X`, ZL set) instead of zero.

The fix keeps the distinction where upstream keeps it, in the data:
`NDS::analog_full` is set by `refresh_analog` only when an enabled CPAD
cheat's name contains "Nub", and `apply_analog()` writes `+4` as zero while
it is clear. `test_analog_apply` asserts both halves. Games where upstream
offers only one variant are unaffected.

shocoman's 3DS-NDS Key Remapper codes do **not** carry over. The generator
emits a `C2000000` block, native code that patches a 22-entry table (3DS input
to NDS key mask) and reads the 3DS's own HID registers from the ARM11. The
engine refuses `C2`, and there is no ARM11 to run it on. The equivalent here is
the frontend's `[pad]` bindings.

Alternative schemes for one game are a sharper version of the same
trap. Two revisions of a hook (the locally provided Metroid scheme and
the current upstream one, say) install over the same game addresses;
the cheat engine runs every enabled code every frame, so with both on,
the second-installed hook's branches land in the middle of the first
hook's code and controls die entirely. Because cheat enablement
persists per game, the broken pair came back at every boot, including
"fresh" ones. Two fixes together close this. The generator ships all
of a ROM's schemes in one of the R4 format's exclusive categories,
which DSperate already implements end to end (the parser keeps only
the first enabled member, the pause menu treats members as radio
buttons), and `Session::load_enabled` collapses a persisted file that
names two members to the first. And the cheat engine now journals
every byte an analog code writes: when one is disabled, the bytes that
were solely its own are restored to what they held before the code
first wrote them, so disabling it completely uninstalls it —
branch sites get the game's original instructions back and the hook
body is erased. Switching schemes is therefore just enabling the
other one in the menu: the old hook is reverted and the new one
installs on the next frame, no reset needed. (`test_disable_reverts_writes`
in `tests/cheat_test.cpp` pins this, including the overlapping-writers
case.) The journal is forgotten on machine reset, which clears RAM. It is
not forgotten across a save state: see §6b.

### 6a. Metroid's two schemes are different control schemes

Both are named `[with Nub stick, ZL and ZR]`, and that name is what turns on
the second-stick data, but they do not offer the same controls. Disassembling
the stripped hooks for USA v1.1 gives:

| Input | `CPAD […] (upstream)` | `CPAD […]` (local) |
|---|---|---|
| Left stick | walk, Morph Ball | walk, Morph Ball |
| Right stick | aim | aim |
| D-pad | the game's own walking | up: Beam, down: Missile, left and right: previous and next special weapon |
| Select | the game's own | toggles the Scan Visor |
| X | the game's own | Morph Ball |
| ZR | toggles the visor | nothing: this hook never reads `+4` |
| ZL | nothing without a gyro | nothing |
| Gyro hotkeys | ZL + D-pad | A + D-pad |

The local code is the alternate scheme posted on page 16 of the GBAtemp thread,
and that post's control list agrees with the disassembly. The post gives a
second layout that adds ZL and ZR, but only by way of the Key Remapper, which is
a separate code and does not run here (§6). Its table, from the post's generator
link: 3DS R and ZR to NDS L, 3DS L to NDS B, 3DS Y and ZL to NDS R, X to X. The
game's own defaults make NDS L shoot, NDS R boost, B jump and X Morph Ball.

The hook bodies are shared almost line for line. The local one adds a second
entry at `0x020012EC` for the D-pad and Select, hooked from `0x02026C34` and
`0x02026C64`, and it overwrites eight game instructions with `mov r0, #0`:
four beside the walk hook (`0x02024B28`, `0x02024CBC`, `0x02024F18`,
`0x02025084`) and four beside the Morph Ball hook (`0x02021F50`, `0x02021FB4`,
`0x02022014`, `0x02022078`). Those writes have no matching logic in the hook,
and each was a call to `0x02028F0C`, the game's check for a bound action, with
the walk or Morph Ball bindings. That is how the D-pad comes to be free for
weapons. On USA v1.0, D-pad up walks under upstream and with no cheat, and does
nothing under the local scheme.

So the local scheme removes the digital movement path and puts nothing in its
place unless stick data arrives. Without stick data the
upstream scheme falls through to the D-pad; the local one has no fallback.
The game keeps running and Samus cannot move, which is what a failed feed looks
like on this scheme. `refresh_analog` logs the commonest cause, `pad.analog`
being off.

The local code's weapon-switch address, `0x0200C5FC`, is the same in all six
ROMs, although the hook sites move by region and revision. It may be low enough
to be unaffected, but this has not been checked against the Japanese or
European v1.0 binaries.

### 6b. Two ways a hook used to go wrong

**A cheat's enabled flag changes when it is toggled, but `refresh_analog` ran
only on Resume.** Reset Game and Load left the menu without it, so a new code
ran while `NDS::analog_patch` stayed false: the hook was in place and nothing
wrote `0x027FFDF0`. The category's own description told players to reset. Every
way out of the menu now goes through `apply_cheat_changes`.

**Save states hold installed hooks, and the journal did not.** Every pak profile
sets `autosave` and `autoload`, so a session normally resumes with last time's
hook already in RAM. `load_state` used to forget the journal, which meant:

- disabling the cheat uninstalled nothing, and with `analog_patch` now off the
  local scheme kept its `mov r0, #0` writes with no sticks behind them;
- switching scheme installed almost nothing. Both hooks begin with `0xE92D007F`,
  the only word their install guards test, so the new scheme took the old body
  for its own. Upstream to local still wrote the two local-only branches, and
  those landed inside the upstream body: `0x020012D0` is one instruction before
  a `pop {r1-r5}` with no matching push.

The journal now travels with the state as an optional `CHJR` chunk, keyed by a
hash of each code's words so a revised code does not inherit its predecessor's
hook, and the next `run()` keeps what enabled codes own and restores the rest.
Only analog codes are journaled: other cheats write game data the game goes on
changing, so putting an old value back would be wrong. A byte is restored only
if it still holds what the code wrote, so an overlay loaded over a site since
is left alone, and a hook's own variables stay behind in the free RAM its body
used. A game picked from the list has its `.ini` merged for the pad settings;
other keys in it wait for the next start. A state with no chunk
loads as before and has nothing to restore, so **a state saved by an older
build still carries its hook**. Use
Reset Game, or start the game fresh from the launcher, once. Hooks do not leave
such a state any other way.

---

## 7. Getting it onto a device

The pak (`minui-nintendo-dsperate-pak`) builds a pinned upstream tag, so the
DSperate change travels as `patches/0007-analog-controls.patch` until a tag
carries it — 0007 because 0004 belonged to a retired patch and the pak does not
reuse numbers. It applies after 0006 and is documented in the pak's TECHNICAL.md
as drop-on-upstream-absorption.

Everything else the pak contributes is data and seeding, no code paths:

- `config/usrcheat-analog.dat` — the generator's output: the whole catalogue,
  229 ROM revisions across 70 games. Tony Hawk Motion is left out, because its
  later sections are gated on a flag its ARM7 code sets.
- `config/games/ASME.ini` — the per-game override: `analog = dual`, both
  sticks off stylus/d-pad/face duty, L2 freed from the stylus tap and R2
  from held fast-forward, since both are ZL/ZR now.
- `paths.analog_cheats` names a database of analog patches apart from
  `usrcheat.dat`; the pak reads its bundled copy in place, so a new pak always
  brings its own. `merge_analog` adds it to the game's cheats for an exact
  checksum match only: `best_entry` falls back to the first revision, which is
  useful for a plain cheat and wrong for a hook at fixed addresses. Its codes
  replace any `CPAD` code from `usrcheat.dat`, which is what earlier paks
  seeded there.
- `dsp_seed_analog` seeds the per-game files next to `dsperate.ini`, **only when
  absent**, so an ini the player has edited is never overwritten.

The net on-device experience: first launch seeds the files; the player
enables `CPAD` (or the nub variant) under Options > Analog > Compatible Cheats
for SM64DS; from then on the sticks steer, with the virtual joystick the game
would have drawn staying hidden while the stick is active.

---

## 8. Limits, and what is not yet known

- **Disabling a hook mid-call.** `revert` runs from the ARM7's VBlank IRQ with
  no check that the ARM9 is outside the hook, so a toggle that lands inside it
  could return into erased code. A two-stage revert would close that, but a
  scheme switch needs the old body gone in the same run, or the new scheme's
  guard takes it for its own. It is left as a rare risk on a toggle.
- **Replays.** The input log carries the sticks but not how the session fed
  them. The SDL frontend replays under the live `pad.analog` settings; headless
  enables the channel from `--cheat`.

- **Catalogue depth.** The offsets are per-ROM, so unknown checksums get no
  patch (never a guessed address), and a file whose ARM9 parts cannot be
  separated from the relay is reported and skipped, never shipped silently
  broken. Most of the catalogue has had no device check.
- **Confirmation status.** SM64DS in both variants is confirmed working on
  device by user report — proportional movement, camera on the right stick
  and L2/R2 under the nub variant, none of that under the plain one.
  Nothing else in the catalogue is device-confirmed yet. The 3DS-side
  description in §1 is from reading shocoman's and Sono's sources, not from
  running RTCom; it is load-bearing only as evidence for what to delete.
- **Stick range.** The hooks scale against the 3DS's `CPAD_MaxRadius` of `0x69`
  (105) while `analog_byte` reaches 127. Metroid's walk hook does not clamp, so
  a full push asks for about 544 against its `MaxSpeed` of 450. Whether the game
  clamps that later has not been measured.
- **Out of scope, deliberately.** DSi mode (patch addresses assume the DS
  memory map); the Metroid gyroscope variant (no gyro source on these
  handhelds — the nub variant stands); and touchscreen synthesis as a
  generic fallback, which the existing `stylus_axis` already approximates
  and which is not the same control the games were patched for.

## Confidence

Claims about the 3DS side trace to the cited upstream sources (`rtcom.cpp`/
`rtcom.h`, `arm11_twlbg_patch.s`, `arm9_controls_hook.s`, Sono's protocol
post) and are stated as readings of that code. Claims about DSperate are
from this tree and its tests: `tests/cheat_test.cpp` (hook installation and
guard), `tests/io_test.cpp` (the `0x027FFDF0` layout, the unrotated nub and variant
gating), `tests/input_test.cpp` (capture, deadzone, diagonal clamp),
`test_state_carries_hooks` in `tests/cheat_test.cpp` (the real Metroid codes
across a state, both directions), the
pak's bats suite (seeding and no-clobber), and a patched-v3.0.5 build of
the shipped binary. The one claim resting on a user's device report rather
than an instrument is §8's "confirmed working"; no frame-time measurement
of a patched game on device exists yet, and the guard argument in §3
predicts — but does not measure — that it is unmeasurable.

