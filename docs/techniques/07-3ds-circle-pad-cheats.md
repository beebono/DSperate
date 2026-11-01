# 3DS Circle Pad cheats, without the 3DS

This document is unlike the others in this directory. Documents 00–06 describe
DraStic's techniques, reconstructed from its binary. This one describes a
feature DSperate implements itself: support for shocoman's Circle Pad (CPAD)
patches. These are the Action Replay codes that let a Nintendo DS game read the
3DS's analog stick when it runs on that hardware under TWiLightMenu++/TWPatcher.
Here the same codes run on an emulator on ARM handhelds, and the player's
physical sticks take the Circle Pad's place.

The one-sentence version: **a CPAD patch is three mechanisms stacked — a
transport, a relay and a game hook — and an emulator only needs the hook.**
The transport exists to move two bytes across a machine boundary that does
not exist here, so DSperate deletes it and writes the data itself.

## Provenance

- **The codes are shocoman's.** That covers the patch format, the ARM9 hook and
  every per-game offset. They come from the public repository
  [`Analog-Controls-for-NDS-Games-on-3DS`](https://github.com/shocoman/Analog-Controls-for-NDS-Games-on-3DS)
  (Super Mario 64 DS plus about seventy other titles under
  `patches_for_other_games/`) and the
  [GBAtemp thread](https://gbatemp.net/threads/circle-pad-patches-for-super-mario-64-ds-and-other-games-in-twilightmenu-with-twpatcher-and-rtcom.623267/)
  that accompanies them, where shocoman posts as *shoco*.
- **The RTCom protocol description is Sono's**, posted in the same thread.
- **The conversion for emulators is bmpriest's**
  [ds-emu-dac](https://github.com/bmpriest/ds-emu-dac) (MIT). It owns the codes
  DSperate ships.
- **Everything about DSperate's side is from this tree:** `src/core/input/`,
  `src/core/cheat/` and `src/frontend/sdl/input.*`.
- **The 3DS side was read, not run.** §1 describes shocoman's ARM7/ARM11 code
  from its source, and §8 says so again.

**Licence.** shocoman's repository has no licence. The converted catalogue that
DSperate links in (`src/core/cheat/analog_db/`) is therefore not under the GPL.
It is redistributed because of how its author chose to publish it:

- The codes are published as Action Replay codes for players to paste into
  their own `usrcheat.dat`, and the thread's first post offers a ready-made
  database too.
- That post links six third-party cheat databases that include them, under the
  heading "Several people made their versions of DeadSkullzJr's cheat database
  and included my mods".

`analog_db/README.md` records this, together with the ds-emu-dac commit and
command that rebuild the file byte for byte.

---

## 1. The three layers on a 3DS

A DS game running on a 3DS has no path to the Circle Pad. The pad is read by
the ARM11; the game runs on the ARM9/ARM7 pair in DS mode, behind the TwlBg
process. The patch bridges that gap with three parts that are worth telling
apart, because only one of them knows anything about the game.

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

Each frame runs a cycle:

1. release (`0xFE`);
2. re-request (`0x01`), answered 2–3 VBlanks later by RTC IRQ;
3. work;
4. release again.

If the ARM7 does not keep the channel alive with `0x83`, it is released
automatically after a timeout. The uploaded usercode has the signature
`u8 usercode(u8 data, u8 iterator)`, returns its byte in `r0`, and signals
errors in `r4`. That is the whole transport. It moves bytes and knows nothing
about sticks.

### 1b. The relay: TwlBg writes, ARM7 copies

The uploaded ARM11 code patches TwlBg at runtime (`arm11_twlbg_patch.s`). From
then on, once per frame, TwlBg writes the pad state into legacy RTC registers on
the NDS side:

- Circle Pad X/Y into the Alarm-Time-2 register;
- in the New 3DS build only, the C-Stick (nub) position and the ZL/ZR bits into
  the Counter register.

Injected ARM7 code then bit-bangs those registers over the ordinary RTC port
(`0x04000138`). It stores the result into shared RAM at the fixed address
`0x027FFDF0` (`RTCOM_DATA_OUTPUT` in `rtcom.cpp`):

| Address | Contents |
|---|---|
| `+0` (u16) | `CPadX \| CPadY<<8`, both signed bytes |
| `+4` (u32) | `ZLZR \| NubX<<8 \| NubY<<16`; ZL = bit 2, ZR = bit 1 of the low byte |

Two details of the relay matter later:

- **The nub is rotated 45° before it is stored:** `new_x = sin45·(x−y)`,
  `new_y = sin45·(x+y)`, with `sin45 = 0xB50/0x1000`. The C-Stick's axes sit
  diagonally.
- **In the non-nub TwlBg build the `+4` word is written as zero.** The data is
  not merely unused there; it is actively absent.

### 1c. The hook: the only game-specific part

The ARM9 half (`arm9_controls_hook.s`) is copied to a safe address
(`BASE_PATCH_ADDR`, `0x02004B00` for SM64DS), and a branch to it is inserted
into the game's input-update routine. Each frame the hook reads `0x027FFDF0`.

**If the stick is off centre,** the hook reproduces what the game does with its
touchscreen virtual joystick:

- speed is `sqrt(x²+y²)`, normalised to `0x1000` against
  `CPAD_MaxRadius = 0x69`;
- angle, sine and cosine come from the game's own sqrt/atan2 helpers;
- all of it is written into the game's controls struct: speed at `+0x8`, sin
  `+0xA`, cos `+0xC`, angle `+0xE`, and a "stick active" flag at `+0x14`.

It then skips the touchscreen/D-pad update for that frame.

**If the stick is centred,** the hook falls through to the original
instruction, and the game behaves exactly as unpatched.

For SM64DS USA v1.0 (`ASME-AEA63749`) the per-ROM values are:

| Symbol | Value |
|---|---|
| input-update inject address | `0x0202B324` (instruction replaced: `0xE7D01108`) |
| hook base | `0x02004B00` |
| controls struct | `0x02097594` |
| sqrt / get-angle helpers | `0x0203B898` / `0x02039684` |
| branch written at the inject address | `0xEAFF65FB` |

Every title and revision has its own row of these values, tabulated in that
game's `generate_patch.py` upstream. That table, not the transport, is the asset
worth reusing.

---

## 2. What DSperate already had

Three pieces of delivery machinery predated this feature, and most of the
design amounts to "do not build a fourth":

- **The Action Replay engine** (`src/core/cheat/ar_engine.cpp`).
  - It runs enabled codes from the ARM7's VBlank IRQ (`CpuContext::check_irq`),
    as a real cheat cartridge does.
  - It writes through the bus's DMA accessors, so a write over guest code
    invalidates JIT blocks via `store_code`.
  - The opcodes the CPAD codes use are all supported: plain 32/16/8-bit writes,
    the `E` bulk copy, and the `5` conditional. Only the native-code `C2`/`C4`
    extensions are refused, and these codes never use them.
- **The database path.**
  - `usrcheat.dat` (R4 format) is matched to the cart by game code and header
    checksum (`database.cpp`, JAMCRC over the 512-byte header).
  - Codes are toggled per game in the pause menu.
  - The enabled set persists by name in a per-game `.cheats` file.
- **The sticks themselves.** SDL's game-controller layer reports both sticks and
  both triggers on the handhelds DSperate targets: the RG DS Plus under ROCKNIX,
  and the MinUI/NextUI devices through the pak's mappings. The frontend already
  consumed those axis events, but only reduced them to digital buttons past a
  deadzone, or used them to drive the stylus.

What was missing was everything between "SDL reported an axis" and "guest RAM
at `0x027FFDF0` holds signed bytes".

---

## 3. The stripped cheat

The upstream codes cannot run unchanged; it is not just a matter of waste. They
open with the RTCom handshake, against registers DSperate's RTC model does not
have (112/113, the Counter-extended read), and upload code to an ARM11 that does
not exist. The ARM7 half would spin forever on a status byte no one will write.

So the codes are stripped to their ARM9 section:

- **What is kept.** An upstream file has two conditional sections, separated by
  the second `D2000000`; everything from that separator onward is kept. For
  SM64DS USA v1.0 that is 64 pairs out of 309 (128 words; the hook body itself
  is `0x1E0` bytes).
- **The checks.** `ds_emu_dac.py` performs the strip for a whole upstream
  `action_replay_codes` folder. It refuses to emit a code unless it passes three
  checks:
  - it starts with the install guard;
  - it contains an `E` bulk write;
  - nothing in it writes ARM7 WRAM (`0x03000000–0x03FFFFFF`), which is where
    the RTCom code used to live. The checker walks opcode positions and steps
    over `E` data, because those data words are instructions that can look like
    addresses.

**The install guard settles the obvious performance question.** The first pair
tests whether the word at the inject address is still the original instruction
(`0xE7D01108`). The hook body and branch are written only while it is.

- DSperate runs cheat codes on every ARM7 VBlank IRQ. Without the guard, the `E`
  copy would rewrite 480 bytes through the JIT invalidation path several times a
  frame.
- With the guard, the steady-state cost of an installed patch is one conditional
  read per run plus a walk over skipped data.

This cost follows from how the codes are built; it has not been measured on a
device. It is pinned by `test_analog_hook_sm64ds` in `tests/cheat_test.cpp`,
which asserts three things:

- the hook lands;
- the ARM7 sentinels survive;
- a second run does not rewrite a deliberately corrupted hook byte.

This is why there is no separate apply-once patch engine. The hook's
`InitialHook` prologue, which installs the branch when a ROM is physically
patched, is dead bytes in this form: the AR code writes the branch directly.

---

## 4. The analog channel (`src/core/input/`)

`input::Frame` carried buttons, pen position, lid and mic samples. It now also
carries two sticks in SDL range (`cpad_x/y`, `cstick_x/y`) and `zl`/`zr`
(`input_log.h`).

The input log grew to DSIN version 3: 24-byte records, with the ZL/ZR bits
folded into the flags byte and the four stick axes appended as `s16`. Versions
1 and 2 still read back, with the analog fields zero. A replay therefore carries
a patched session's sticks (§8 says what else it needs), and an old replay
cannot accidentally steer.

`input::apply()` writes the relay layout once per frame, before the run:

- **`+0`:** each CPad axis divided down to a signed byte (`v/256`, clamped).
- **`+4`:** ZL as bit 2 and ZR as bit 1 of the low byte, then the nub,
  **unrotated**. The relay's 45° rotation compensates for the New 3DS C-Stick
  sensor being mounted diagonally in the shell. Host sticks are axis-aligned,
  and applying the rotation anyway turns a pure right push into yaw plus pitch.

**The layout is not universal.** Metroid Prime Hunters' own relay stores things
differently:

- the ZL/ZR byte at `+4`;
- the nub as `s16` at `+6`/`+8`;
- gyro words at `+10`.

Fed the packed layout, its hook reads the packed Y byte as its X and a
never-written word as its Y.

`NDS::analog_layout` selects which arrangement `apply()` writes. The two cannot
both be written, since they collide at `+6`, so the choice is read from the
enabled code:

- `cheat::reads_extended_analog` looks in the code's bulk writes for an ARM
  `ldrsh` at offset 6 or 8, which is how an extended hook takes the nub.
- The generic relay is packed. **64 of the catalogue's 229 ROMs are extended**,
  a count `test_builtin_analog` in `tests/cheat_db_test.cpp` pins against the
  built-in database.
- The rule gives the same answer as each game's relay source for every code
  bundled. It is a pattern, not a proof, so `pad.analog_layout` (`auto`, the
  default, or `packed` or `extended`) can overrule it.

The writes go through `bus.dma_write16/32` from the ARM9 side. They are gated on
`NDS::analog_patch`: with no patch, `apply()` writes nothing to `0x027FFDF0`
however hard the sticks are pushed, and an unpatched game sees no change in
behaviour.

`analog_patch` and `analog_full` (§6) are deliberately not part of a save state.
The words are rewritten every frame before `run_frame()`, like buttons and
touch, so a state neither needs nor could use a saved copy.

`tests/io_test.cpp`'s `test_analog_apply` pins the layout end to end:

- `cpad (32767, −32768)` reads back as `0x807F`;
- a pure `+X` nub reads back as `(+127, 0)`;
- centred input clears both words;
- the v3 log round-trips the fields.

---

## 5. Frontend capture (`src/frontend/sdl/input.*`)

SDL axis events arrive after remapping: the pak's mappings name the axes
positionally, and `pad.axis_*` remapping runs before this point.
`Input::axis()` now also records the raw `Sint16` value of each stick and
trigger. Nothing kept those values before: the stylus and stick-as-d-pad paths
consume directions and thresholds, not magnitudes.

`fill_analog()` converts a raw stick into the frame's fields, in `scale_stick`:

- **It flips Y.** SDL's up is negative; the hooks take the 3DS's, where up is
  positive.
- **It applies a radial deadzone.** Below the deadzone the stick reads as
  centred. Beyond it, the remaining travel is rescaled to full range and the
  magnitude capped at 1, so a full diagonal is full speed rather than √2 times
  it. (The hook divides by `CPAD_MaxRadius` and clamps in its own way; feeding
  it √2 would only make diagonals saturate earlier.)
- **ZL/ZR use the same past-¾-travel rule as the trigger bindings**
  (`max(deadzone, 24576)`). This is a device fact more than a tuning choice: on
  the pre-rc11 H700 layout the triggers are *buttons* (`b13`/`b14`) that SDL
  synthesises as 0/32767 axes, so one threshold serves both physical forms.

Configuration is six keys, live on the pause menu's Analog page, under a link
to the game's analog cheats:

| Key | Values | Default |
|---|---|---|
| `pad.analog` | `auto \| off \| left \| right \| dual` | `auto` |
| `pad.analog_deadzone` | SDL units, 0–30000 | `6000` |
| `pad.analog_sensitivity_left` / `_right` | percent, 10–300 | `100` |
| `pad.analog_invert_y` | bool | `false` |
| `pad.analog_layout` | `auto \| packed \| extended` | `auto` |

The `pad.analog` modes:

- `dual` is the two-stick shape: left = CPad, right = nub.
- `left`/`right` put the CPad on one stick, for one-stick handhelds.
- `auto` is `dual` on a pad that reports a right stick, and `left` otherwise.
  It is the default because the channel is already gated on the cheat (below),
  so the setting no longer has to be changed by hand before a patch works.

**Activation.** `Session::refresh_analog` decides it from the core's
`cheat::analog_request`, which headless replay uses too. It runs at game load
and whenever the Cheats menu changes the enabled set. `NDS::analog_patch` is set
only when both hold:

- `pad.analog` is not `off`;
- an enabled, non-note cheat's name starts with `CPAD`.

Neither condition alone does anything. Sticks configured for a game with no
patch never write guest RAM. A patch enabled on a stick-less profile reads a
centred stick and falls through to the game's own input path.

**While a patch runs, its sticks are its own.** By default the right stick also
moves the pen (`stylus_axis = right`) and the left stick also works the d-pad
(`stick_dpad = left`). Under a two-stick patch, Metroid's aim stick would then
drag the pen across the touch screen. So `Input::set_analog_patch`, called from
the frame loop with `NDS::analog_patch`, hands the sticks `pad.analog` feeds to
the patch:

- `stylus_axis`, `stick_dpad` and `stick_face` let go of those sticks, and
  directions they were holding are released.
- **Holding `pad.stylus_hold` lends the pen its stick back.** This is the button
  that already gave the pen the d-pad (it was called `stylus_dpad`, which still
  reads). Meanwhile the patch sees that stick as centred, so the two never both
  follow it. While `stylus_hold` is unbound, the pad modifier does the same, so
  the pen is never stranded; a bound `stylus_hold` keeps Guide for hotkeys.
- When the patch stops, the sticks return to their bindings on their next
  event.
- `test_analog_patch_claims_sticks` in `tests/input_test.cpp` pins each of
  these.

This replaces a configure-time warning that asked the player to set
`stylus_axis = none` in a per-game file.

**Per-game files.** A game picked from the list has its `.ini` merged for the
pad settings; other keys in it wait for the next start.

---

## 6. The two variants, and the bug they exposed

Upstream ships most games twice: `CPAD` and `CPAD [with Nub stick, ZL and ZR]`.
The hook binary is identical in both. The difference lies in the layers DSperate
deleted, specifically in the TwlBg build: the plain build writes `+4` as zero
(§1b), and the nub build writes it live.

**The data, not the hook, must keep them apart.** An early DSperate build wrote
`+4` live whenever any CPAD cheat was on, and on a device both variants gave the
same dual-stick and ZL/ZR behaviour. That was an emulator bug, not a cheat bug:
the hook cannot tell where its data came from, so data that should not exist
turned the camera anyway. With the plain variant's flags, the test read back
`0x5A5A04` (a rotated nub at full `+X`, ZL set) instead of zero.

So `refresh_analog` sets `NDS::analog_full` only when an enabled CPAD cheat's
name contains "Nub", and `apply_analog()` writes `+4` as zero while it is clear.
`test_analog_apply` asserts both halves. Games for which upstream offers only
one variant are unaffected.

**shocoman's 3DS-NDS Key Remapper codes do not carry over.**

- The generator emits a `C2000000` block: native code that patches a 22-entry
  table (3DS input to NDS key mask) and reads the 3DS's own HID registers from
  the ARM11.
- The engine refuses `C2`, and there is no ARM11 to run it on anyway.
- The equivalent here is the frontend's `[pad]` bindings.

**Alternative schemes for one game install over the same addresses.** One
example is Metroid's alternate scheme beside the current upstream one (§6a). The
cheat engine runs every enabled code every frame, so with both on, the
second-installed hook's branches land in the middle of the first hook's code
and controls stop working. Because cheat enablement persists per game, the
broken pair would come back at every boot. Three things prevent it:

- **One exclusive category per ROM.** The generator ships all of a ROM's schemes
  in one of the R4 format's exclusive categories. DSperate implements those end
  to end: the parser keeps only the first enabled member, and the pause menu
  treats members as radio buttons.
- **Persisted pairs collapse.** `Session::load_enabled` reduces a persisted file
  that names two members to the first.
- **Disabling a code uninstalls it.** The cheat engine journals every byte an
  analog code writes. When the code is disabled, each byte that was solely its
  own is restored to what it held before the code first wrote it. Branch sites
  get the game's original instructions back, and the hook body is erased.
  Switching schemes is therefore just enabling the other one in the menu: the
  old hook is reverted and the new one installs on the next frame, with no reset
  needed. `test_disable_reverts_writes` in `tests/cheat_test.cpp` pins this,
  including the case of overlapping writers.

The journal is forgotten on machine reset, which clears RAM. It is not
forgotten across a save state (§6b).

### 6a. Metroid's two schemes are different control schemes

Both are named `[with Nub stick, ZL and ZR]`, and that name is what turns on the
second-stick data, but they do not offer the same controls. Disassembling the
stripped hooks for USA v1.1 gives:

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
second layout that adds ZL and ZR, but only through the Key Remapper, a separate
code that does not run here (§6). Its table, from the post's generator link, is:

- 3DS R and ZR → NDS L;
- 3DS L → NDS B;
- 3DS Y and ZL → NDS R;
- X → X.

The game's own defaults make NDS L shoot, NDS R boost, B jump and X Morph Ball.

**The local scheme frees the D-pad by removing the game's movement calls.** The
two hook bodies are shared almost line for line, and the local one adds:

- a second entry at `0x020012EC` for the D-pad and Select, hooked from
  `0x02026C34` and `0x02026C64`;
- eight game instructions overwritten with `mov r0, #0`: four beside the walk
  hook (`0x02024B28`, `0x02024CBC`, `0x02024F18`, `0x02025084`) and four beside
  the Morph Ball hook (`0x02021F50`, `0x02021FB4`, `0x02022014`, `0x02022078`).

Those writes have no matching logic in the hook. Each was a call to
`0x02028F0C`, the game's check for a bound action, with the walk or Morph Ball
bindings. On USA v1.0, D-pad up walks under upstream and with no cheat, and does
nothing under the local scheme.

**So the local scheme has no fallback.** It removes the digital movement path
and puts nothing in its place unless stick data arrives; the upstream scheme
falls through to the D-pad. With no stick data the game keeps running and Samus
cannot move. That is what a failed feed looks like on this scheme, and
`refresh_analog` logs the commonest cause, `pad.analog = off`.

The local code's weapon-switch address, `0x0200C5FC`, is the same in all six
ROMs, although the hook sites move by region and revision. It may be low enough
to be unaffected, but this has not been checked against the Japanese or European
v1.0 binaries.

### 6b. Two ways a hook used to go wrong

**Leaving the menu by Reset or Load skipped the activation.** A cheat's enabled
flag changes when it is toggled, but `refresh_analog` ran only on Resume. Reset
Game and Load therefore left the menu with a new code running while
`NDS::analog_patch` stayed false: the hook was in place, and nothing wrote
`0x027FFDF0`. The category's own description told players to reset. Every way
out of the menu now goes through `apply_cheat_changes`.

**Save states held installed hooks, and the journal did not travel with them.**
Paks set `autosave` and `autoload`, so a session normally resumes with last
time's hook already in RAM. `load_state` used to forget the journal, which
meant:

- disabling the cheat uninstalled nothing. With `analog_patch` now off, the
  local scheme kept its `mov r0, #0` writes with no sticks behind them.
- switching scheme installed almost nothing. Both hooks begin with `0xE92D007F`,
  the only word their install guards test, so the new scheme took the old body
  for its own. Going from upstream to local still wrote the two local-only
  branches, and those landed inside the upstream body: `0x020012D0` is one
  instruction before a `pop {r1-r5}` with no matching push.

**The fix: the journal is saved in the state.**

- **It travels as an optional `CHJR` chunk.** The chunk is keyed by a hash of
  each code's words, so a revised code does not inherit its predecessor's hook.
  The next `run()` keeps what enabled codes own and restores the rest.
- **Only analog codes are journaled.** Other cheats write game data that the
  game goes on changing, so putting an old value back would be wrong.
- **A byte is restored only if it still holds what the code wrote.** An overlay
  loaded over a site since then is left alone, and a hook's own variables stay
  behind in the free RAM its body used.
- **A state with no chunk loads as before and has nothing to restore.** So a
  state saved by an older build still carries its hook: use Reset Game, or start
  the game fresh from the launcher, once. Hooks do not leave such a state any
  other way.

---

## 7. Where the codes come from

The catalogue ships inside DSperate. `src/core/cheat/analog_db/usrcheat-analog.dat`
is ds-emu-dac's output: 229 ROM revisions of 207 game codes, about seventy
titles. It is linked in with `.incbin` (`cheat/analog_db.cpp`), as the DSi fonts
are, and adds 115 KB to the binary.

Tony Hawk's Motion is left out. Its later sections are gated on a flag its ARM7
code sets, so they cannot be separated from the relay.

**`Session::open` looks for the game's analog codes in this order:**

1. `paths.analog_cheats` names the file itself; `none` turns analog codes off.
2. Otherwise, `usrcheat-analog.dat` beside the ROM, then in the config
   directory, the same places `usrcheat.dat` is looked for.
3. The built-in copy, for any game the file from steps 1–2 lacks. A newer
   catalogue on disk therefore extends the built-in one rather than hiding it.

**The analog database is kept apart from `usrcheat.dat`, and merged.**

- It stays a separate file, so a player's own `usrcheat.dat` stays theirs.
- `merge_analog` adds its entry to the game's cheats for an exact checksum match
  only. `best_entry` falls back to the first revision, which is useful for a
  plain cheat and wrong for a hook at fixed addresses.
- Its codes replace any `CPAD` code from `usrcheat.dat`, which is where earlier
  paks seeded them.

**Headless loads them the same way.** `--analog-cheats FILE|none` sets the file;
by default the built-in copy is used. The codes load whenever `--cheats`,
`--cheat` or `--list-cheats` is given, so `--cheat CPAD` alone enables SM64DS's
patch.

**On a device there is nothing to install.** The player enables `CPAD` (or the
nub variant) under Options > Analog > Compatible Cheats, and from then on the
sticks steer, with the game's virtual joystick staying hidden while the stick is
active.

**Packagings.** The NextUI pak (`minui-nintendo-dsperate-pak`) carried this
feature as `patches/0007-analog-controls.patch` until DSperate merged it, and
bundles its own `config/usrcheat-analog.dat`. After the merge, the patch can be
dropped. So can the stick lines of its per-game `ASME.ini` (`analog = dual`, and
the sticks off stylus/d-pad/face duty), since `pad.analog = auto` and the stick
ownership of §5 do that job.

The trigger lines still matter, though. Stick ownership covers sticks only, and
the pak binds L2 to the stylus tap and R2 to held fast-forward, which would fire
alongside ZL/ZR. DSperate's own defaults bind the triggers only under the
modifier, and ZL/ZR ignore the triggers while the modifier is held.

A bundled `.dat` remains useful only if it is newer than the built-in one. ROCKNIX needs nothing: its launcher passes the usual flags, and the
built-in catalogue answers.

**Updating the catalogue.** Rebuild with ds-emu-dac, replace the file, and
update the commit and SHA-256 in `analog_db/README.md`. `test_builtin_analog`
asserts the counts, so it will name what changed.

---

## 8. Limits, and what is not yet known

- **Disabling a hook mid-call.** `revert` runs from the ARM7's VBlank IRQ with
  no check that the ARM9 is outside the hook, so a toggle that lands inside it
  could return into erased code. A two-stage revert would close that gap, but a
  scheme switch needs the old body gone in the same run, or the new scheme's
  guard takes it for its own. It is left as a rare risk on a toggle.
- **Replays.** The input log carries the sticks but not how the session fed
  them. The SDL frontend replays under the live `pad.analog` settings; headless
  enables the channel from `--cheat`.
- **Recognised by name.** A code counts as analog when its name starts with
  `CPAD`, and as the nub variant when its name contains `Nub`. ds-emu-dac
  canonicalises both spellings upstream uses, and every real code in the
  built-in database passes the first test (`test_builtin_analog`). A hand-made
  code under another name would run as an ordinary cheat: not journaled, so
  disabling it would not uninstall its hook.
- **Catalogue depth.** The offsets are per-ROM, so an unknown checksum gets no
  patch, never a guessed address. A file whose ARM9 parts cannot be separated
  from the relay is reported and skipped, never shipped silently broken. Most
  of the catalogue has had no device check.
- **Confirmation status.** SM64DS is confirmed working on a device in both
  variants, by user report: proportional movement, plus camera on the right
  stick and L2/R2 under the nub variant only. The PR that added the feature
  also reports play on an RG35XXH in Metroid Prime Hunters, Star Fox Command and
  Animal Crossing. Nothing else in the catalogue is device-confirmed yet. The
  3DS-side description in §1 comes from reading shocoman's and Sono's sources,
  not from running RTCom; it is load-bearing only as evidence for what to
  delete.
- **Stick range.** The hooks scale against the 3DS's `CPAD_MaxRadius` of `0x69`
  (105), while `analog_byte` reaches 127. Metroid's walk hook does not clamp, so
  a full push asks for about 544 against its `MaxSpeed` of 450. Whether the game
  clamps that later has not been measured.
- **Out of scope, deliberately.**
  - DSi mode: the patch addresses assume the DS memory map.
  - The Metroid gyroscope variant: these handhelds have no gyro source, so the
    nub variant stands in.
  - Touchscreen synthesis as a generic fallback: the existing `stylus_axis`
    already approximates it, and it is not the control the games were patched
    for.

## Confidence

**3DS side.** Claims trace to the cited upstream sources (`rtcom.cpp`/`rtcom.h`,
`arm11_twlbg_patch.s`, `arm9_controls_hook.s`, Sono's protocol post) and are
stated as readings of that code.

**DSperate side.** Claims come from this tree and its tests:

- `tests/cheat_test.cpp`: hook installation and the guard, revert, and the real
  Metroid codes across a state in both directions (`test_state_carries_hooks`);
- `tests/io_test.cpp`: the `0x027FFDF0` layout, the unrotated nub and variant
  gating;
- `tests/input_test.cpp`: capture, deadzone, diagonal clamp, `auto`, and stick
  ownership;
- `tests/cheat_db_test.cpp`: the built-in catalogue's counts and its
  extended-layout share.

The provenance claim, that the built-in file is ds-emu-dac's output, was checked
by rebuilding it from the recorded commit and comparing hashes.

**What rests on reports, not instruments.** The device confirmations in §8 are
user reports. No frame-time measurement of a patched game on a device exists
yet; the guard argument in §3 predicts that the cost is too small to measure,
but does not measure it.
