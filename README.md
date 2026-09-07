# DSperate (Nintendo DS Emulator)

AI DISCLAIMER: Agentic coding is used here. A lot. I am not smart enough to keep track
of all of this with the wet electric meat lump that sits inside my skull.

An attempt at a Nintendo DS emulator that reimplements DraStic JIT + NEON Rendering
techniques combined with modern niceties that melonDS provides like per-screen windows
and maybe overlaid shader support later on.

---

## Status

Commercial games boot and play. Everything below is in, verified against
melonDS (frame dumps, instruction traces) and measured on the Anbernic RG DS
(RK3566, four A55s) it is built for:

- **CPUs.** ARM946E-S and ARM7TDMI interpreters everywhere; on AArch64 an
  ARM → AArch64 recompiler for both ([src/core/cpu/jit/README.md](src/core/cpu/jit/README.md)),
  verified instruction by instruction against the interpreter. melonDS-grade
  cycle timing (per-page cost tables, TCM windows, ARM9 stores priced at bus
  cost), and an idle-loop detector that proves a poll loop side-effect-free
  and skips it to the next event.
- **System.** DMA (main-RAM burst timing, GXFIFO and display-FIFO modes),
  timers, IPC, SPI (touchscreen, firmware flash including page writes, power
  management and the microphone amplifier), RTC, the divider/sqrt unit,
  Wi-Fi register probing (no frames), and the two-CPU interleave either in
  128-cycle lockstep with melonDS or event-bound as DraStic does it.
- **Cartridge.** Retail Slot-1 carts: KEY1/KEY2 command protocol, direct boot,
  the save chip with melonDS's per-title save-type list (EEPROM and FLASH
  variants), and the IR carts' pass-through (Pokémon HG/SS, B/W, B2/W2).
  The GBA slot is an empty slot.
- **2D.** Both engines complete — every BG mode, sprites, windows, mosaic,
  blending, extended palettes, display capture, the main-memory display FIFO,
  master brightness — rendered lazily from a per-engine write journal in one
  batch per frame (exact by construction; a VRAM write trap catches the
  frame up when the picture could change under it), engine B on a worker
  thread, with NEON line kernels diffed against their portable references.
- **3D.** Geometry engine (command FIFO with cycle timing, matrix stacks,
  lighting, clipping, box/position/vector tests) and a software rasteriser
  with textures, toon/highlight shading, shadows, fog, edge marking and
  anti-aliasing that matches melonDS pixel for pixel. The raster runs
  asynchronously on three worker threads in horizontal bands, with a
  content-validated decoded-texture cache, batched span stages in NEON, and
  a skip for frames that resubmit the same geometry.
- **Sound.** The SPU mixes all sixteen channels (PCM8/16, ADPCM, PSG, noise),
  capture units and the output at 32768 Hz; the SDL frontend plays it and
  paces the emulator from the audio queue.
- **Sessions.** Battery saves, save states (ten slots, exact round trip),
  input recording and replay (a played scene becomes a benchmark), a real or
  fake microphone, and the lid/hinge.
- **Firmware boot.** With no game the console boots its own firmware: the DS
  menu, with the clock and calendar set from the host and the console's own
  settings editable from inside it. What the firmware writes to its flash is
  kept in a sidecar file, so the dump itself is never written to. Wi-Fi is
  register-level only, so PictoChat and Download Play do not work.
- **Frontend.** SDL2: INI config with per-game overrides, keyboard and
  controller remapping, hotkeys, fast forward, screenshots, per-scanline
  scaling straight into the presented buffer (nearest, bilinear, LCD grid,
  box-filter seams or chunky), zero-copy dmabuf presentation under Wayland
  with direct scanout when the compositor allows it, our own page flips under
  KMSDRM, the display engine's hardware scaler on Allwinner handhelds, a
  dual-window mode for dual-panel handhelds, and a blitted pause menu --
  save states, the slot list and Action Replay cheats as well as the 
  usual Resume and Quit functions.

[docs/techniques](docs/techniques) documents the DraStic techniques being
reimplemented, the measurements behind them, and a checklist of which are
done, equivalent, or deliberately not ported.

## Licence

GPLv3 — see [LICENSE](LICENSE). melonDS (GPLv3) is used as reference and, with
attribution, as a code source.

DSperate is a **clean-room** reimplementation of DraStic's *techniques*, documented
by studying its freely-distributable debug-symbol build — which its original
developer, Exophase, approved for distribution — and by measuring it on real
hardware. Those notes are in [docs/techniques](docs/techniques); everything in
them is reconstructible from that binary plus a test device. No DraStic code, in
any form, is in this tree.

## Building

Requires CMake ≥ 3.16, Ninja, and a C++17 compiler; SDL2 for the SDL frontend.

Running it also requires a DS BIOS pair and firmware image (`bios9.bin`,
`bios7.bin`, `firmware.bin`), dumped from your own console. **None are
provided by this repository**, and DSperate has only been tested against an
official dump: no open-source replacement BIOS or firmware has been tried,
and the direct-boot path, KEY1 key table, touchscreen calibration and
firmware settings all read the real ones.

    cmake --preset host && cmake --build --preset host && ctest --preset host
    ./build/host/src/frontend/headless/dsperate-headless --bios9 bios9.bin --bios7 bios7.bin \
        --firmware firmware.bin [--direct game.nds] --frames 60

Cross-building for ARM64 handhelds (needs `aarch64-linux-gnu-g++`; tests run under
`qemu-aarch64-static` if present):

    cmake --preset aarch64-cross && cmake --build --preset aarch64-cross
    (cd build/aarch64 && ctest)

The JIT and NEON kernels only build on AArch64 hosts; everywhere else you get the
interpreter and the portable C++ renderer. On AArch64 the recompiler is the
default for both CPUs (`--interp`, `--jit9`, `--jit7` select otherwise); it is
verified against the interpreter instruction by instruction
(`tests/jit_test.cpp`) and slice by slice on whole games. `DSPERATE_JIT`,
`DSPERATE_NEON`, `DSPERATE_TESTS`, `DSPERATE_HEADLESS` and `DSPERATE_SDL` are the
CMake switches. The only vendored third-party code is miniz's DEFLATE
decompressor, for reading zipped ROMs (`src/core/cart/miniz/`, MIT); the zip
container parsing is ours. There are no other dependencies beyond SDL2 for
the SDL frontend.

The two CPUs are interleaved either in 128-cycle lockstep with melonDS
(`--quantum 128`, the headless default — every frame dump and trace comparison
assumes it) or event-bound, each CPU running to the next scheduled event as
DraStic does (`--quantum 0`, the SDL frontend's default; `--lockstep` there
selects the former). Event-bound is a few percent faster at the cost of the
CPUs seeing each other's IPC writes and IRQs up to an event interval late.

## The headless frontend

`dsperate-headless` is the measurement harness: it boots the firmware (or a ROM with
`--direct`), runs `--frames N`, and is what every measurement and comparison
runs through.

    dsperate-headless --bios9 F --bios7 F --firmware F [--direct game.nds] [--frames N]
             [--stats-from N] [--interp | --jit9 | --jit7] [--quantum N] [--timing-oc] [--cpu-oc]
             [--no-aa] [--frameskip N] [--frameskip-capture] [--hide-screen top|bottom]
             [--save F] [--replay F]
             [--trace F [--max N]] [--dump-frames F [--dump-from N] [--dump-count N]]
             [--dump-audio F] [--save-state-at N:file] [--load-state F]
             [--cheats usrcheat.dat] [--list-cheats] [--cheat <name|#N>]
             [--rtc-host] [--firmware-override F]

`--hide-screen` skips the drawing of one screen's engine, as a single-screen
layout does in the frontend; that half of a frame dump goes stale.

`--rtc-host` and `--firmware-override` are for driving the firmware menu from
the harness and are off by default, because both break reproducibility: the
first seeds the clock from the wall, and the second lets a run change the
console's settings. Without them the harness is what every baseline assumes -- a
clock frozen at 2000-01-01 and a firmware image identical to the dump.

`--trace` writes per-CPU instruction traces (`<pc> <instr> <cpsr> r0..r14`,
one line per instruction, spin loops collapsed) for `tools/compare_traces.py`;
`--dump-frames` writes raw framebuffers for `tools/compare_frames.py`, and
`--dump-audio` the raw s16 stereo stream for `tools/compare_audio.py`. It
has no audio output. It does not pick up `<rom>.sav` automatically, so that a
stray `.sav` next to a ROM cannot silently move a frame baseline: give it
`--save file`, which loads read-only and is never written back.

## Playing

`dsperate` is the SDL2 frontend: direct boot, both screens, sound and
input. It is built when SDL2 is found (`-DDSPERATE_SDL=OFF` to skip it). The
ROM is optional -- without one it boots the firmware menu, see below.

A ROM may be a plain `.nds` or a `.zip` holding one. Zips are recognised by
their magic rather than their extension, so a launcher that hands over `.ZIP`
or an extensionless temporary file still works. If an archive holds more than
one `.nds`, the one whose game code is in the save-type database wins over one
that is not (homebrew, translations and hacks are not listed, so an archive of
those still loads), then the highest header revision, then archive order --
`DS_VERBOSE=1` prints which entry was taken. The image is hashed after
unpacking, so save states, `.sav` files and scene hashes are interchangeable
between a zipped and a loose copy of the same game. Only stored and deflated
entries: no zip64, no encryption, and no `.7z` or `.rar`, which would need real
dependencies. Unpacking a 64--256 MB ROM costs roughly 0.5--2 s at launch on an
A55, before the window appears.

    dsperate [game.nds|game.zip] [--bios9 bios9.bin --bios7 bios7.bin --firmware firmware.bin]
                 [--config F] [--write-config F]
                 [--scale N] [--fullscreen] [--layout L] [--screen top|bottom] [--pip-alpha X] [--dual-window]
                 [--integer-scale [under|over|off]]
                 [--linear] [--lcd-grid S] [--seam dark|blend|blend_linear]
                 [--chunky [M]] [--chunky-threshold N] [--chunky-cell C]
                 [--disp | --no-disp] [--fbdev | --no-fbdev] [--no-vsync]
                 [--no-audio] [--volume N] [--no-mic]
                 [--interp] [--lockstep | --quantum N] [--timing-oc] [--cpu-oc] [--fast-load]
                 [--aa | --no-aa] [--frameskip N] [--frameskip-mode adaptive|fixed] [--frameskip-capture]
                 [--frames N] [--stats-from N] [--record F | --replay F] [--rtc-host]
                 [--save F] [--load-state F] [--autosave-png F] [--clear-cache]

`dsperate --help` describes each one; every option has a key in the settings
file, and the command line overrides it. There is no renderer switch: the
frontend picks the fastest way onto the panel it can find (see "Display and
handhelds") and falls back to SDL's GPU renderer, then its software one, only
where none applies.

### Settings and controls

Settings live in `~/.config/dsperate/dsperate.ini` (`$XDG_CONFIG_HOME` is
honoured; `--config F` names another file), written with every key commented
out on the first run. Two files next to it override any of its keys for one
game: `games/<rom name>.ini` (the ROM's filename without `.nds`, the natural
one to write by hand) and `games/<GAMECODE>.ini` (the four-letter code printed
as `game: ... [XXXX]` at start, shared by every dump of that title); the
filename one wins, and is where a layout picked with the hotkey is
remembered. The command line overrides all of them. `[paths]` holds the BIOS/firmware so they need not
be passed every time, plus optional `saves` and `states` directories (default:
next to the ROM), `screenshots` (default: the states directory; the
autosave's PNG stays with its state), `cheats`, a `usrcheat.dat` database, and
`firmware_override`, where settings changed inside the firmware are kept
(default `<firmware>.ovr`). `[keys]` and `[pad]` remap the DS buttons to SDL key and
controller-button names (`x`, `Right Shift`, `dpup`, `+righttrigger`);
`[hotkeys]` and `[padhotkeys]` bind the frontend's actions, on the controller
usually as `mod+button` (or a chord, `mod+start+back` -- SDL calls Select
"back") with the pad's mode/home button as the modifier. The left stick works
the d-pad; the right stick moves a crosshair over the bottom screen and
clicking it touches (`stylus_axis` = `right|left|none` picks the stick,
`stylus_dpad` names a button that, held, turns the d-pad into the pen;
`stylus_button`, `stylus_speed` pixels per frame at full tilt, `stylus_size`;
it hides after `stylus_hide` idle frames). The touchscreen is driven by a finger or the mouse on the bottom
screen.

Defaults -- keyboard: arrows, `X`/`Z` = A/B, `S`/`A` = X/Y, `Q`/`W` = L/R,
Enter = Start, Right Shift = Select; `Escape` quits, `P` opens the pause menu, `Tab` held
fast-forwards, `F` toggles fullscreen, `F4`/`F10` cycle the screen layout forward/back, `F6` swaps
which screen is alone/large/dominant and `F8` moves the PiP inset (all three
remembered for the game), `F9` takes a screenshot (both screens, PNG, with a brief white flash that is not in the picture, in
`paths.screenshots`, the states directory by default), `-`/`=`/`0` are volume down/up/mute, `F5`/`F7` save/load
the state in the current slot and `F2`/`F3` change the slot, `L` closes and
opens the lid, `M` held is the fake microphone. The `fps` hotkey toggles an
on-screen frames-per-second counter (`FF` shows in the same corner while fast-forwarding, beside the number when both are on) and is unbound by default (`[video] fps =
true` starts it on). It and the save-state slot number are drawn in the corners
of the primary screen -- the one shown alone, large or dominant -- so they
follow `screen` and the screen-swap hotkey rather than sitting on a panel the
layout may not be showing. In `pip`, whichever of the two is in the corner the
inset occupies moves down its own edge instead of being buried under it. Controller, with Mode held:
Start+Select quits, Start opens the pause menu, right trigger fast-forwards, R/L save/load
the state, Right/Left change the slot, Select/X cycle the layout forward/back, Y swaps the screens;
the left stick's click is the microphone.

### Example configs

[`configs/`](configs/) holds three ready-made settings files: `default.ini`
is exactly what the first run writes (regenerated by
`tools/gen_example_configs.sh`, so it cannot drift), and `melonds.ini` /
`drastic.ini` set only the keys that make DSperate feel like those emulators'
defaults -- melonDS's 128-cycle lockstep, interpreter, filtering and
Shift+F1/F1 states; DraStic's toggled fast forward, unfiltered screens and
pad-centred hotkeys; `advdrastic.ini` is Knulli's "Advanced DraStic"
hotkey scheme (Function = the mode button). Copy one over `dsperate.ini` or pass it with `--config`;
`--write-config F` writes the default file anywhere.

### Microphone and lid

`M` (or the pad binding) held feeds noise at 80 % of full scale, for
blowing/shouting prompts on devices without a microphone. Otherwise the real
one is captured -- on Linux straight from ALSA (`mic_dev` / `DS_MIC_DEV`,
default `plughw:0,0`; the handhelds' PipeWire only offers a speaker monitor),
DC-blocked and noise-gated: `mic_gate` is the factor over the tracked floor
(default 5, 0 = off) and `mic_gain` scales what is left (default 0.25, set
against the RG DS and Mario & Luigi's mic-test meter). `--no-mic` leaves it
closed.

Closing the lid puts the game to sleep and opening it wakes it. A handheld
with a real hinge switch (evdev `SW_LID`) drives this itself, and a host
suspend/resume pulses it on hosts without one; `L` toggles it by hand.

### Saves and save states

Battery saves live next to the ROM as `<rom>.sav` (or under `[paths] saves`),
written a second after the game stops writing its save chip and again on
pause, lid close and exit -- a launcher's SIGTERM included.

Save states go to `<GAMECODE>.<slot>.dss` in the states directory (next to
the ROM, or `[paths] states`), ten slots. `F5`/`F7` and `F2`/`F3` reach them
without leaving the game; the **pause menu** (the `pause` hotkey -- `P`, or
Mode+Start on a controller) stops the machine and puts save, load, the slot
list, cheats, resume and quit on screen, which is what a handheld with no
keyboard needs. It draws over the held frame on the top screen, dims both to
show the machine is stopped, and is driven by the DS buttons themselves --
the guest cannot see them while it is up: up/down to move, left/right to
change the slot in place, A to choose, B to go back or resume; a held
direction repeats. Slots that already hold a state are marked, so a save says
what it would overwrite. Nothing runs behind it -- it is modal, not an
overlay -- and it redraws only when the picture would change.

A state is the whole machine at a frame boundary (~5.5 MB, uncompressed: RAM, VRAM, both CPUs, every
peripheral, the geometry engine's polygon RAM and the rasterised 3D frame)
and loads only with the same ROM; the battery save is written alongside it so
the two never disagree. The SDL frontend appends the screen layout (mode,
primary screen, PiP corner and sizes) after the machine's chunks, so a state
brings its view back with it; the headless build writes no such chunk and a
state without one keeps the current layout. The recompiler's translations are
dropped on load and rebuilt as the game runs. `src/core/state/state.h` describes the chunked
format; each subsystem lists its own fields in one `sync_state` that both
writes and reads, so a state saved straight after a load is byte-identical to
the one loaded -- `tools/state_roundtrip.sh <dsperate-headless> <scene> <N> <M>`
checks that, and that the frames after a load match the frames after the
save, on any recorded scene (the harness takes `--save-state-at N:file` and
`--load-state file`; `DS_STATE_DEBUG=1` prints the cycle-accounting state at
both points). Loading a state is refused during `--record` (the recording
could not replay past it) and `--replay` refuses to load or save states at
all.

`[emu] autosave = true` writes one state on quit to an unlisted eleventh slot,
`<states>/<GAMECODE>.auto.dss`. Nothing is written while playing, so it costs
no frame time; a Ctrl-C or a launcher's SIGTERM is covered, a `SIGKILL` is not.
Resume it with `--load-state` on that path -- the slot never shows in the pause
menu and the slot hotkeys never reach it, so it cannot be overwritten by hand.
It is skipped during a replay or a recording, like the save-state hotkey.

### Firmware boot

Started with no ROM -- or with one named `BootMenu.nds`, so a launcher that
only knows how to start games can reach it -- `dsperate` boots the
console's own firmware instead of a game. That is the DS menu: the clock and
calendar, the owner's nickname, the settings pages and the card panel. It needs
the real BIOS pair and firmware dump like everything else does.

A card goes in the slot either way, still under a firmware boot, so the menu
has a banner to draw and something to launch. It is built into the emulator --
nothing has to be shipped beside the binary -- and `[loader] title` and
`subtitle` set its two banner lines. A `BootMenu.nds` beside the config
replaces it entirely: `tools/mkcart.py` builds such a card with an icon and
title of your choosing, and its `--emit-header` is how the built-in one's icon
was generated. Neither needs anything from a commercial dump, and `[loader]
card = false` leaves the slot empty.

**Picking a game from the DS menu.** Point `[paths] games` at a directory and
launching that card raises a list of what is in it, over the white the launch
animation fades to; choosing one boots it. The list shows each file's
name without its extension -- the ROM header's own title reads
`ARTACADEMYRT` where the file reads `Art Academy` -- and it is the pause
menu's cheats page underneath, so it scrolls, pages with the shoulder buttons
and scrolls a name too long to fit.

The console's own launch animation is what raises it: the list goes up once
the animation has faded to white, read from MASTER_BRIGHT rather than from the
pixels, so the animation itself plays at full speed down the normal drawing
path. Launching the card is the only thing in the menu that forces both
screens white that way -- PictoChat, DS Download Play, the settings pages and
the shutdown they end in never do, and the white stretch of the firmware's own
boot is white pixels rather than a forced screen -- so nothing else raises the
list, and it does not matter whether the card was tapped or selected with the
D-pad and A.

The chosen game is direct-booted rather than firmware-booted a second time,
which is both faster and better looking -- the loader's fade covers the
transition, so there is no second Nintendo logo -- and it sidesteps the
firmware's own cartridge launch entirely. Saves, save states, screenshots and
cheats are all re-derived from the game that was picked, so nothing lands
under the loader's name.

The firmware runs under the recompiler's strict timing -- per-instruction
budget checks, in lockstep with the interpreter. Without them the boot has
been seen to fail on the RG DS, not every time, which is what a timing race
looks like; block-granularity overshoot is the suspect. The console is idle
enough in its own menu for the checks not to show, and they are dropped again
the moment a game is launched from the card, where they would. `DS_JIT_STRICT`
still forces them on for everything.

The clock is seeded from the host's local time and runs, so the menu shows
today's date. In the core the clock is off and frozen at 2000-01-01, because
the whole verification harness compares runs against each other and against
melonDS; only the frontends turn it on, and not under `--replay`, where a game
that reads the date would otherwise play differently every time the scene was
replayed.

**Changing the console's settings.** The settings pages work, so the nickname,
birthday, favourite colour, greeting and language can be set from inside the
firmware exactly as on hardware. The firmware saves them by writing its own
flash -- and those writes are kept in a **sidecar file**, `<firmware>.ovr`
(`[paths] firmware_override` to move it), which holds only the 256-byte pages
that changed and is applied over the image at load. `firmware.bin` itself is
never opened for writing: it is a dump of your console that you cannot
regenerate. Deleting the sidecar puts the console back to whatever the dump
says.

Leaving the settings pages, the firmware switches the console off. The
frontend takes that as its cue to write the sidecar out and start the console
again, so the settings you just changed are on disk and the menu comes back
with them applied -- which is what the hardware's power button would have done
next. A game is never rebooted this way.

The touchscreen calibration screen is cosmetic. The frontend reports pen
positions as plain screen pixels and normalises the stored calibration to
match, so a calibration run inside the firmware is accepted and then
normalised away rather than being allowed to aim the pen wrongly.

**PictoChat and Download Play do not work.** Wi-Fi is emulated at the register
level only -- enough for games to probe the hardware, with no frames, no
timers and no interrupts -- and both of those are Wi-Fi applications. They are
on the menu because the firmware puts them there, not because they are
supported. **Download Play in particular softlocks the menu**: it does not
fail and return, it hangs, and the only way out is to quit the emulator.
PictoChat will open its room list and go no further.

### Cheats

Action Replay codes, read from a `usrcheat.dat` database -- the format the
published DS cheat collections come in. Put one beside the ROM or in the
config directory (or name it with `[paths] cheats`) and the entry matching
the ROM's game code and header checksum is loaded, so a game with several
revisions gets the right one. Nothing is enabled by loading it.

The pause menu grows a **Cheats** page listing them under the database's own
headings, with the shoulder buttons paging through the long ones -- some
games have thousands. A is a toggle; a group the database marks as
alternatives (a difficulty, a character) allows only one at a time. What you
turn on is remembered per game, next to the save states.

Codes run once a frame from the ARM7's VBlank IRQ, which is where the real
cartridge hooks itself. `dsperate-headless` has `--cheats <file>`,
`--list-cheats` and `--cheat <name|#N>` for the same thing without a UI.


Fast forward (`Tab` held, Mode+right trigger, or the `fast_forward_toggle`
hotkey) drops the pacing -- `[emu] ff_speed = N` caps it at N times real time
-- and presents one frame in `ff_skip + 1` (default 3); every frame is still
emulated, so the run stays exact, and the audio queue keeps the newest frames
rather than falling behind.

Frameskip (`--frameskip N` / `[emu] frameskip`, 0 = off) drops the drawing of
up to N frames in a row, so at least one in N+1 is drawn. A skipped frame runs
the machine and the game exactly as usual -- the CPUs, the DMA, the geometry,
the 2D journals and latches -- and leaves out only what nothing else observes:
both engines' line rendering and output, the 3D rasterisation that feeds them,
and the scaling and present. The saved-state comparison above is bit-identical
with and without it on every recorded scene, apart from the framebuffers
themselves. A frame that display-captures or feeds the display FIFO is drawn
whatever the setting, since the game reads those pixels back (and the 3D raster
for a frame runs during the frame before it, so a few frames around a capture
are drawn too); a title that captures every frame -- Pokemon B/W's overworld,
Etrian Odyssey's dungeon view, Golden Sun -- therefore skips little or nothing
by default, and says so once on the console.

`--frameskip-capture` (`[emu] frameskip_capture`) skips those frames too. It is
inexact by construction: the capture write is skipped along with the drawing,
so the destination bank keeps the picture it last captured. DISPCAPCNT itself
behaves exactly as it does on a drawn frame; what differs from hardware is the
pixels, and only a game that reads them back with the CPU rather than
displaying them can tell.

Skipping and drawing both happen in whole *display periods*. Games drive the
two screens over several frames rather than one: Golden Sun toggles POWCNT1's
screen-swap bit every frame and renders one screen's 3D each time, capturing it
for the other screen to display next frame, and a game can alternate its
capture destination the same way. Drawing one frame in four there would draw
the same phase for ever -- every presented frame with one fresh screen and one
several frames old, and which one alternates, so the two screens look like they
are swapping. `Gpu::display_phase_period` watches the swap bit, both engines'
display modes and VRAM display banks, and the capture destination, and reports
the period of that sequence; the frontends then skip in blocks of it and draw a
block of it, and the limit counts those blocks rather than frames. So
`--frameskip 3` skips three frames in four on a period-1 title, and six in eight
on a period-2 title like Golden Sun -- the same ratio either way
(`DS_DEBUG_SKIP=1` prints the period, and the frontend says which it picked).

Only the last drawn frame of a block is presented. The first one has one screen
freshly rendered and the other still showing what it held before the skip --
Golden Sun renders one screen per frame and leaves the other to the capture the
next frame displays -- so presenting it flashes the stale screen. Drawing the
block through and presenting its last frame shows both screens of one moment. A
run of drawn frames longer than a period presents every frame, so an adaptive
run that stops skipping goes straight back to full rate.

`--frameskip-mode` picks the policy: `adaptive` (the default) skips only while
the emulator is running behind real time, measured as the milliseconds by which
the frames so far have run over their budget, and stops as soon as it has caught
up; `fixed` always skips N of every N+1 frames. `DS_DEBUG_SKIP=1` prints the
per-frame decision and why a frame was drawn anyway. The harness takes
`--frameskip N` too, in the fixed pattern, for measuring what the drawing costs.

### Display and handhelds

Layouts (`--layout` / `[video] layout`; `F4`/`F10` step through
`[video] layout_cycle`, by default all of them): `vertical`
(stacked) and `horizontal` (side by side; `--screen` picks which comes
first), `single` (one screen fills the window), `pip` (one fills it, the
other is an inset of `pip_scale` in `pip_corner` = `tl|tr|bl|br`),
`dominant_v` (stacked in DS order, both centred) and `dominant_h` (side by
side in DS order, bottoms aligned). By default (`dominant_ratio = auto`)
the dominant screen takes the largest whole number of panel pixels per DS
pixel that leaves the other at least `dominant_threshold`
(`--dominant-threshold`, default 0.25) of its size, and the other grows
into whatever room is left, up to the same size; the grid and chunky cells
then come out exact on the dominant screen. On a 640x480 panel `dominant_v`
gives a 2x top screen over a 128x96 bottom; on a 1280x720 window a 3x top
over 192x144 at the default threshold and a 2x top over 448x336 at 0.33,
and `dominant_h` 3x beside 512x384 either way. A number instead
(`--dominant-ratio 0.5`) fits the dominant screen to the width/height with
the other that fraction of its size. Every mode keeps
the 4:3 screen aspect. `--screen top|bottom` (`[video] screen`) is the
screen shown alone, large or dominant; `F6` swaps it.

`--integer-scale under|over` (`[video] integer_scale`) forces a whole number
of panel pixels per DS pixel on the full-size screens: `under` takes the
largest that fits and letterboxes, `over` the smallest that covers and crops.
The crop is centred on a stacked or side-by-side pair, so the edge between
the screens is kept; in dual window the top screen keeps its bottom row and
the bottom screen its top. The PiP inset and the dominant layouts' secondary
keep their ratio to the screen they belong to. The LCD grid and chunky cells
come out exact at a whole scale (one seam or cell per DS pixel, evenly).
Works on every tier, the display-engine scaler included, where `over` is a
source-window crop.

The core scales each scanline straight into the buffer that is presented, as
the line is produced (`DS_SCANLINE_SCALE=0/1` overrides the per-driver
default), and that buffer is a CMA dma-heap allocation the display hardware
can read directly (`DS_DMABUF=0` disables, `=1` requires). That buffer
comes from whichever allocator the kernel has: the heap named by
`DS_DMA_HEAP` (a `/dev/dma_heap/` name or path, `ion`, or `ion:<mask>`),
else every `/dev/dma_heap/*` contiguous-first (`linux,cma`, vendor
`cma-uncached`/`reserved`, then the system heaps), else `/dev/ion`; each
candidate is offered to the display and the first one it accepts is used
(the `buffers from` log line says which). The scaling
filters live on that path and cost the CPU, not a GPU: `--linear` is a
bilinear filter (two NEON passes per line; about 0.2 ms a frame at 2.5x on
an RG DS, free where the display engine scales, below); `--lcd-grid S`
dims one seam per DS pixel; `--seam blend` blends only the panel pixel that
straddles two DS pixels (sharp-shimmerless); `--chunky` draws 2x2 blocks as
one cell. `--linear` takes precedence over the other three. On the small
screens (the PiP inset, the dominant layouts' secondary) the full grid
(strength 1) needs at least 2x to show, a dimmed one is an overlay and
applies from 1x, and a view shown below 1x gets neither the grid nor
chunky. At exactly 2x a seam per DS pixel would leave one lit panel pixel
in four, a dim wash rather than a grid, so there the seam goes on every
other DS pixel (a 4-pixel pitch); every other scale seams each DS pixel. The largest screen chooses the chunky cell (`--chunky-cell`), and
the other one matches it in DS pixels rather than panel pixels, so a
half-size secondary gets cells half as many panel pixels across.

The presentation tiers, tried in order at start-up:

- **Display-engine scaler** (`--disp`, auto wherever `/dev/disp` answers:
  the Miyoo A30 and other Allwinner "disp 1.5" boards). The core draws a
  DS-resolution canvas, rotated for the panel, and the display engine scales
  it in hardware; no GL, no driver threads, ~1.4 ms a present.
- **fbdev** (`--fbdev`, auto when SDL's only driver is Mali-over-fbdev and
  `/dev/fb0` answers: the H700 handhelds under BaseOS): the scanline path
  straight into fb0's buffers.

- Under **Wayland**, submitted through `zwp_linux_dmabuf`. The compositor
  composites it zero-copy, or -- for a fullscreen opaque window on an
  untransformed output -- scans it out directly on a hardware plane.
- Under **KMSDRM**, page-flipped onto the panel's CRTC ourselves, borrowing
  SDL's DRM fd. SDL2 has no window framebuffer on that driver, so its
  "software" renderer is really a hidden GLES one: a scalar stretch blit, a
  full-screen upload into a streaming texture, a textured quad, and a
  blocking swap, 13.4 ms per frame on the RG DS. Flipping our own buffer is
  0.15 ms, and takes emulation + present from 17.9 ms a frame to 6.4.
- **SDL_Renderer**, only where none of the above applies (an X11 desktop,
  `DS_SCANLINE_SCALE=0`): the GPU renderer, or SDL's software one where no
  GPU renderer can be created. Not selectable; on the handhelds the scanline
  tiers measured faster than GLES because the GL driver's threads compete
  with the emulation and raster threads for four cores.

`--dual-window` opens one fullscreen window per video display with one DS
screen each, which is what a dual-panel handheld wants and what direct
scanout needs there; both panels are driven from the one process.

On a handheld with no desktop session, SDL uses its KMSDRM backend directly;
point `XDG_RUNTIME_DIR` at the PipeWire runtime directory or SDL's PulseAudio
backend spends about twenty seconds failing to connect before sound starts.

## Recording, replay and measuring

`--record scene.dsin` writes what you play, one 16-byte record per frame
(buttons, pen, lid and eight microphone samples -- enough for the games that
measure loudness; older 8-byte logs still replay), and `--replay scene.dsin`
plays it back (in the window, or headlessly with `dsperate --replay
scene.dsin`, which also takes `--dump-frames` and works under `perf`). The
emulator is deterministic given its inputs, so a replay reproduces the
session frame for frame as long as the ROM, BIOS and battery save are the
same as when it was recorded — a played scene becomes a benchmark.
[scenes/](scenes/) holds the recorded scenes used for every measurement in
the docs, with the saves they need; a recording made against a game that
writes a save (Mario & Luigi creates one on boot if it is missing) diverges
immediately without it. The SDL frontend picks up `<rom>.sav` automatically
except under `--replay`, where `--save` names the scene's save and nothing is
written back.

Both frontends print the same per-frame host-time report at the end of a
`--frames` run (`src/core/frame_report.h`): mean, median, p90, p99 and the
count and position of frames over the DS's 16.7 ms budget. Every change is
judged on the tail, not the mean.

## Diagnostics

Environment knobs the core reads; the ones that change timing or output are
for A/B runs, not for play.

- `DS_VERBOSE=1` (SDL) -- the startup lines (`game:`, `config:`, `cheats:`,
  `replay:`, the battery save) and the hotkey echoes (`paused`, `volume`,
  `layout`, `state slot`, `lid`). Off by default: on a handheld nobody reads
  stderr. Errors, refusals and the confirmations for anything written to disk
  (a state, a screenshot) are never gated -- those are the lines you want
  precisely when something went wrong.
- `DS_FRAME_STATS=1` (SDL) -- the per-frame timing statistics and the
  over-budget window histogram at exit. The headless frontend, whose job is
  measuring, always prints them. `DS_PROFILE=1` implies this.
- `DS_FPS=1` -- speed, per-stage times and audio buffer depth per second;
  with `--frames N` comparable between runs by frame index. The SDL frontend
  also has an on-screen counter -- `[video] fps` and the `fps` hotkey.
- `DS_PROFILE=1` (headless) -- wall time per stage, event counters and the slice
  census at exit; `DS_PROFILE_THREADS=1` per thread.
- `DS_FRAME_HASH=1` (headless) -- a digest of RAM and both CPUs' registers after
  every frame, and `DS_FRAME_DUMP=<frame>:<path>` writes that frame's RAM, so
  two builds can be diffed to the first frame their *state* differs --
  usually long before the first pixel does.
- `DS_JIT_DENSITY=1` (with `DS_PROFILE=1`) -- host bytes of translated code per
  guest instruction weighted by *execution* rather than by translation, plus the
  distribution of block length by entries. The `[jit] code ... bytes per guest
  instruction` line counts a block translated once and run a million times the
  same as one run once; this does not. Refused alongside `DS_JIT_PRETX`.
- `DS_IO_CENSUS=1` -- the hottest I/O registers by address and CPU, reads and
  writes listed separately, at exit. Reads are the point: a game polling a
  status bit costs a slow-path access per read and leaves no trace in any
  write log.
- `DS_WATCHDOG=<seconds>` (headless) -- aborts a run whose frame count stops
  advancing for that long, after printing the display-line and raster
  hand-off state.
- `--timing-oc` (or `[emu] timing_oc = true`) -- "Timing OC": DraStic's geometry
  model, opted into as a performance-accuracy trade. The GX FIFO has no level
  and never stalls the ARM9 or its DMA; commands execute in batches when the
  game observes the engine, when the ring fills and at VBlank; a swap takes
  effect at once; geometry commands cost no cycles and the per-command
  pipeline model is skipped. Golden Sun: Dark Dawn's title runs ~4 % faster
  with its frames bit-identical; Dragon Ball Origins' intro desynchronises.
  The untimed-DMA half of the old flag is gone: it measured worse in every
  combination (it hands the emulated ARM9 the cycles the DMA charged, and a
  saturated ARM9 spends them), and neither half was worth anything while the
  FIFO was still modelled.
- `DS_IDLE_SKIP=0|1|all` (or `[emu] idle_skip`) -- the idle-loop skip: `1`
  (default) skips only an ARM9 GXSTAT poll while a swap is pending; `all`
  skips every proven poll loop.
- `DS_JIT_CHURN=1` -- who invalidated translated code and which blocks were
  retranslated, at exit; `DS_JIT_STRICT=1` makes the recompiler test the
  budget before every instruction as the interpreter does, for lockstep
  comparison; `DS_PERF_MAP=1` names translated blocks for `perf`.
- `DS_R3D_THREADS=N` -- raster worker count (0/1 = single-threaded);
  `DS_2D_THREAD=0` keeps engine B on the emulation thread; `DS_2D_LAZY=0`
  renders the 2D engines per line through the same journal. All three must
  produce identical frames -- `tools/scene_hashes.sh` checks that over the
  recorded scenes.
- `DS_QUANTUM`, `DS_LCD_IRQ_DELAY`, `DS_STORE_BUS`, `DS_SPU_BATCH`,
  `DS_R3D_SKIPDUP`, `DS_AUXSPI_LOG`, `DS_MIC_LOG`, `DS_STATE_DEBUG` -- timing
  and logging knobs named where they are read.

## Tests and tools

`ctest` runs the unit tests in [tests/](tests/): the interpreter, the
recompiler against it (under `qemu-aarch64` when cross-built), the NEON
kernels against their portable references, the page table, scheduler, I/O,
SPU, 2D and 3D pipelines and the texture cache. [tools/](tools/) holds the
comparison scripts (`compare_frames.py`, `compare_traces.py`,
`compare_audio.py`), the exactness checks over the recorded scenes
(`scene_hashes.sh`, `state_roundtrip.sh`), `profile_categories.py` for `perf`
output, and `cma_blit_bench` for the dmabuf write-throughput question.
