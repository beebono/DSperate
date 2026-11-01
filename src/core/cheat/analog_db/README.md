# The built-in analog controls database

`usrcheat-analog.dat` is the analog controls (CPAD) catalogue DSperate falls
back on when no `usrcheat-analog.dat` is found on disk (`paths.analog_cheats`,
beside the ROM, then the config directory). It is linked in whole
(`cheat/analog_db.cpp`) and parsed like any other R4 cheat database. See
`docs/techniques/07-3ds-circle-pad-cheats.md` for what the codes do and how
DSperate feeds them.

## Whose work this is

The codes are **shocoman's** (posting as *shoco* on GBAtemp): the ARM9 hook
and every per-game offset come from
[Analog-Controls-for-NDS-Games-on-3DS](https://github.com/shocoman/Analog-Controls-for-NDS-Games-on-3DS)
and the thread
[Circle Pad patches for Super Mario 64 DS and other games](https://gbatemp.net/threads/circle-pad-patches-for-super-mario-64-ds-and-other-games-in-twilightmenu-with-twpatcher-and-rtcom.623267/).
The RTCom protocol the 3DS versions use is Sono's. The conversion for
emulators, which strips the 3DS-only parts, is bmpriest's
[ds-emu-dac](https://github.com/bmpriest/ds-emu-dac) (MIT).

**Licence.** shocoman's repository carries no licence, so this file is *not*
covered by DSperate's GPLv3. It is redistributed on the strength of how its
author chose to publish it: the codes are released as Action Replay codes for
players to paste into their own `usrcheat.dat`, the thread's first post offers
a ready-made `usrcheat.dat` itself, and that post's section "Several people
made their versions of DeadSkullzJr's cheat database and included my mods"
links six such third-party databases. A cheat database that ships these codes
with credit is the use the author describes. If shocoman asks for it to be
removed, delete this directory and `cheat/analog_db.cpp`; the on-disk lookup
keeps working without it.

## Rebuilding

From [ds-emu-dac](https://github.com/bmpriest/ds-emu-dac) at commit
`b873b73db0a4`, which vendors the upstream codes under `upstream/`:

    ./ds_emu_dac.py build metroid upstream --full AMHE --full AMHJ --full AMHP \
        --out usrcheat-analog.dat

| file | SHA-256 |
|---|---|
| `usrcheat-analog.dat` | `cf1a7920d9fa708ae97ac478228737f2b8d37b72a6e059c03651ae52708423c6` |

That rebuild has been checked to reproduce this file byte for byte: 114716
bytes, 229 ROM revisions of 207 game codes. Two files are skipped, both Tony
Hawk's Motion (`CTWE-88B86E61`, `CTWP-51C5963F`): their later sections are
gated on a flag the 3DS-side ARM7 code sets, so they cannot be separated from
the relay. `metroid/` adds the alternate Metroid Prime Hunters scheme
(GBAtemp thread, page 16) beside upstream's, in one exclusive category per ROM.
Regenerate only with a recorded commit, and update the table.
