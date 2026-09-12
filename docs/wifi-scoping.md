# Wi-Fi emulation and passthrough -- scoping

2026-09-11. What it takes to make the DS radio do something: local wireless
between emulators (PictoChat, Download Play, multi-cart play), the emulated
access point that games reach the internet through, and where "passthrough"
is and is not a real option. Nothing here is implemented; the phases at the
end are the proposal.

## Where we are

Two states of the same code, one on each branch:

| branch | what the Wi-Fi block does |
|--------|---------------------------|
| `main` (`io.cpp` 753-900) | register file, 8 KB Wi-Fi RAM, BB/RF indirection, `W_RANDOM`, the transceiver power state machine (melonDS `UpdatePowerStatus`) with the 2048 us power-on event. `W_USCOUNT`/`W_USCOMPARE`/`W_CMDCOUNT` read 0. No timer, no frames, no IRQ 13/14/15. |
| `dsiware` (c0185a8) | adds melonDS's 8 us `USTimer`: `W_USCOUNT`/`W_USCOMPARE` live, the beacon counters, `W_PREBEACON`, `W_CMDCOUNT`, `W_CONTENTFREE`, IRQ 13/14/15, `WIFIWAITCNT`, `GPIO_WIFI`, and the `W_IE` write raising the IRQ. **Gated to DSi mode** (`wifi_update_power_on` returns early on a DS). A TX request logs "not modelled" once. |

The `dsiware` branch is the starting point: its timer is the spine every
frame path hangs off, and it is already in the DSI state chunk. The DS-only
gate was a caution, not a finding -- the doc records that the counter
readbacks moved no scene, and on a DS the timer only runs while a title has
cleared `W_POWER_US` bit 0 (reset value 1), so a game that never touches
the radio pays nothing.

The firmware side is already in place: `firmware_gen.cpp` writes a
configured AP slot ("DSperate-AP", open) at `0x3FA00`, and a real dump
carries whatever the user set in the DS settings menu, which we can boot
into (memory: firmware-boot). `POWCNT2` bit 1 gates the block, DMA mode 7
"Wi-Fi/GBA" exists in `dma.cpp` but nothing raises its trigger.

## Three different products called "Wi-Fi"

They share the frame engine and nothing else. Naming them keeps the phases
honest.

### A. Local wireless (Ni-Fi): DS to DS

PictoChat, Download Play, multi-cart Mario Kart. The host DS sends a CMD
frame, every client answers in its own time slot, the host acks. The frames
are 802.11 with Nintendo's own MP framing; no host radio can send them, so
"passthrough" does not exist for this product on any emulator. Peers are
other emulator instances reached over IP.

melonDS's transports for this, all behind `MPInterface` (82 lines):

| transport | what it is | for us |
|-----------|------------|--------|
| `LocalMP` (367) | shared-memory queues between instances in one process | no: a handheld runs one instance |
| `LAN` (1095) | ENet over UDP, discovery broadcast on 7063, game on 7064, a `MPPacketHeader` wire format | **yes** -- and speaking the same wire format makes an RG DS a peer of melonDS on a PC |
| `Netplay` (1088) | input-synced lockstep over ENet | no: a different project |

### B. Internet through an emulated access point

The game associates with an AP, gets DHCP, talks TCP/UDP to Nintendo's
servers. melonDS models this with `WifiAP` (419 lines: beacons every 128 ms,
probe/auth/assoc management frames, a data-frame to Ethernet rewrite) and
hands the Ethernet frames to a `NetDriver`:

| driver | mechanism | on our devices |
|--------|-----------|----------------|
| `Net_Slirp` (461 + vendored libslirp, 13.4 kLoC C with a 2-file glib shim) | user-mode NAT: the emulator owns a private subnet, TCP/UDP become host sockets, DNS is answered in-process (`HandleDNSFrame` resolves with the host resolver) | **yes**, works unprivileged on a wlan0-only box |
| `Net_PCap` (461) | bridge the frames onto a host NIC with a second MAC | no: needs CAP_NET_RAW, and a *Wi-Fi* host NIC drops frames from a foreign MAC (3-address 802.11), so it only ever worked over Ethernet |

So the one honest meaning of "passthrough" for DSperate is slirp: the
game's sockets ride the CFW's network. Nintendo WFC is gone; the live target
is Wiimmfi, reached by pointing the game's DNS at Wiimmfi's server (a
firmware AP setting the user can already make from the DS menu, or a config
default we apply in the DNS interceptor). Titles whose server check needs
Wiimmfi's patched ROM are a game-side matter and out of our hands.

### C. DSi Wi-Fi (AR6002 over SDIO)

`DSi_NWifi.cpp` (1645) plus the second `DSi_SD` host. Only DSi-mode titles
use it, it is a different chip with a different driver model, and the
DSiWare scoping already lists it out. It stays out; it would reuse the AP and
the net driver from B and nothing from A.

## The core port

What `Wifi.cpp` (2482 lines) has that we do not, by function group:

| group | melonDS functions | lines | notes |
|-------|-------------------|------:|-------|
| timer + IRQs | `ScheduleTimer` `USTimer` `MSTimer` `SetIRQ13/14/15` | ~250 | **on `dsiware` already**; drop the DS gate |
| TX | `TXSendFrame` `StartTX_LocN/Cmd/Beacon` `FireTX` `ProcessTX` `PreambleLen` | ~650 | slot state machine: preamble, duration by rate, the `W_TXBUSY`/`W_TXSTAT` bookkeeping, MP CMD/reply/ack sequencing with `W_CMD_COUNT` |
| RX | `StartRX` `FinishRX` `CheckRX` `MPClientReplyRX` | ~500 | RX ring in Wi-Fi RAM (`W_RXBUF_*`), the 16 frame filters, `RXTimestamp` delay, IRQ 0/6/7 |
| MP replies | `SendMPDefaultReply` `SendMPReply` `SendMPAck` `ReportMPReplyErrors` | ~150 | |
| RF/BB | `ChangeChannel` `RFTransfer_Type2/3` | ~60 | we have the register side; channel affects the AP filter only |
| register glue | `Read`/`Write` cases for `W_TXREQ_*`, `W_RXBUF_*`, `W_RXCNT`, `W_TXBUF_*` | ~200 | |

About 1 500 lines of port on top of the branch, plus `WifiAP` (419). The
port is line-for-line: we already track melonDS's model for this block and
the oracle harness (`trace_melonds`) makes a divergence a diff, not a
debugging session. Exactness rule for this project: **the machine is
deterministic given the packet sequence it receives**, so with the transport
stubbed to "no peer" every existing scene stays hash-identical, and a
title that uses the radio is traced against melonDS with its dummy net
driver.

Things the port has to fit into DSperate rather than copy:

- **Scheduler cost.** One event per 8 us is 2 083 events a frame while the
  radio is on. Our dispatch is cheap but it is not free; measure on the rig
  with a title sitting in a Wi-Fi menu before deciding whether `USTimer`
  needs a coarser "nothing armed" stride. The 5 bench scenes never enable
  the radio, so they cannot see it (memory: raster-is-hidden-on-sm64 is the
  same lesson).
- **Wi-Fi DMA.** Start mode 7 must fire on `W_RXCNT`/TX events. Check the
  DMA trigger path before assuming `dma.cpp`'s enum is wired.
- **Save states.** The new fields join the IO chunk (the `wifi_*` ones are
  there already); a state taken mid-session restores a radio with no peer,
  which is what a real DS does after sleep, and games handle it.
- **Replays.** A `.rec` with Wi-Fi traffic is not replayable unless packets
  are logged with timestamps. Out of scope; note it in the tool's usage.

## The transport layer (frontend)

New directory `src/net/`, mirroring `src/cheevos/` (that doc's "borrow from
the CFW" reasoning applies for dlopen vs vendor):

| piece | dependency | static tiers (A30, RG35XX SP) |
|-------|------------|-------------------------------|
| `MpTransport` interface + `NoPeer` | none | -- |
| `LanMp` (melonDS wire-compatible) | ENet (~10 kLoC C, MIT) | vendor it; it is small and has no deps |
| `NetDriver` + `SlirpDriver` | libslirp (vendored as melonDS does, with its glib shim) | vendor; builds static |
| DNS policy | in the slirp DNS interceptor | `wifi.dns` config: `host` / `wiimmfi` / an address |

**Threading is the real design question.** melonDS's MP host blocks in
`RecvReplies` for up to `RecvTimeout` (25 ms default) waiting for every
client's reply, and clients block on the host's CMD. Our ARM7 runs on the
audio-paced main thread (memory: spirit-tracks-audio-stutter), so a 25 ms
wait is over a frame of silence. That is inherent to Ni-Fi over IP, not a
bug to engineer away: the DS waits microseconds for a reply and an emulator
waits network round-trips. What we control:

- a socket thread that receives into a lock-free ring, so the core only
  ever polls memory and the only blocking point is the host's reply wait;
- the wait bounded by a config knob with a small default, and a menu status
  line that shows dropped replies, so a laggy session reads as lag and not
  as a hang;
- the audio pacer told to expect it (the pacer's banked debt, memory:
  frame-pair-absorption, absorbs a one-off; a session-long deficit needs
  the "pause the clock while waiting" mode melonDS effectively has).

Slirp has none of this: its poll runs on the socket thread, RX frames queue
for the AP, TX frames are handed off without waiting.

## Devices

All three handhelds have a `wlan0` and we reach them over it (memories:
rig-device-access, miyoo-a30-device-access, rg35xxsp-device-access), so LAN
multiplayer between two handhelds, or a handheld and melonDS on the dev
box, and slirp internet are both testable with what is on the shelf. The
ROCKNIX rig has libcurl and a resolver; the static tiers have a resolver
via the CFW's `resolv.conf`, which slirp's DNS path uses through
`getaddrinfo`.

## UI

Originally planned as its own page in the pattern of the RetroAchievements
pages: mode off / local (host, join with the discovery list) / internet; the
DNS choice; a status line (peers, dropped replies, AP association state).

**Built instead as one row** on the existing Emulation page -- see "The menu
row as built" below. A whole page turned out to be mostly unbuildable and
partly unnecessary: a pick row cannot render the dynamic discovery list, the
setting has to be restart-only anyway (the MAC is randomized pre-boot), and
the status a page would show is diagnostic rather than something a player
acts on, so it lives behind `DS_VERBOSE` with the rest of the startup
chatter. Internet mode and the DNS choice join the same row in phase 3.

## Phases

| phase | deliverable | proof | size |
|------:|-------------|-------|-----:|
| 0 | the `dsiware` timer on the DS too; Wi-Fi DMA trigger wired | 5 scenes hash-identical; rig cost with a Wi-Fi menu open | small |
| 1 | TX/RX engine + `WifiAP` + `NoPeer` transport | PictoChat boots to "no one nearby"; a WFC connection test associates and fails at DHCP; `trace_melonds` identical with melonDS's dummy net | ~1 900 lines |
| 2 | `LanMp` over ENet, melonDS wire format; socket thread; menu row (one row, not a page -- see below) | RG DS <-> melonDS PC in PictoChat, then Mario Kart DS multi-cart; two handhelds; Download Play of a demo | ~800 lines + ENet |
| 3 | `SlirpDriver` + DNS policy | Mario Kart DS on Wiimmfi from the rig; the A30 static build | ~500 lines + libslirp |
| -- | DSi NWifi | separate scoping when DSiWare needs it | out |
| -- | `LocalMP`, Netplay, pcap | out (reasons above) | out |

Phase 1 is where the port risk lives and it needs no network at all, so it
is the one to do first and alone. Phases 2 and 3 are independent of each
other.

## What I expect to go wrong

- **The reply wait vs the pacer.** The first multi-cart session will
  stutter, and the fix is a pacer mode, not a smaller timeout. Budget for
  it in phase 2.
- **Timer skew between peers.** melonDS's MP protocol carries a `Timestamp`
  in every packet and the client re-syncs `USTimestamp` to the host's. Our
  scheduler counts in ARM9 cycles (`ARM9_CLOCK_HZ`), melonDS's Wi-Fi in
  ARM7 cycles at 33 513 982; the branch already scales by 2. Get the unit
  wrong and a DSperate client never lines up with a melonDS host.
- **JIT and Wi-Fi RAM.** `0x04804000-0x04805FFF` is written by the ARM7
  and read by frames; it is IO in our page tables so the JIT never caches
  it, but a fast path for the ARM7 IO window would need to keep it that way.
- **Real firmware dumps.** A user's dump holds their real home AP with WPA
  settings the DS games cannot use anyway (DS games are WEP/open only);
  the emulated AP must answer for the SSID the firmware has, or the user
  must configure "DSperate-AP" from the DS menu. melonDS is strict
  ("melonAP"); answering any open-network probe is friendlier and costs a
  line.
- **Wiimmfi's rules.** Their DNS, their patched ROMs, their bans for
  modified clients: our job ends at delivering the packets the game sends.

## Phase 0 and 1 as built (2026-09-11, branch wifi-emu)

Branch `wifi-emu` off `dsiware` (its 8 us timer is the spine; see the
scoping above for why not `main`).

### Phase 0: the timer on the DS

`wifi_update_power_on` lost its DSi gate: the timer runs whenever POWCNT2
bit 1 is set and, on a DS, W_POWER_US bit 0 is clear (melonDS
`UpdatePowerOn`); the DS's one-shot 2048 us power-on event became the same
countdown the DSi used (`USUntilPowerOn`). `wifi_power_on_pending` stays as
a dead field so the IO chunk keeps its layout.

| check | result |
|-------|--------|
| mlbis / meteos / sm64 / etody / dbori, 1800 frames; GSDD state, 300 | hash-identical to the branch tip |
| timer forced on for sm64, 1200 frames, x86 host | 2.61 -> 2.70 s (+0.08 ms/frame), hashes identical |

None of the bench scenes enables the radio, so the cost line is a bound
from a forced-on build, not a measurement of a real session. On the A55 it
will be a few tenths of a millisecond per frame while a game is in a
Wi-Fi menu; a coarser stride when nothing is armed is the lever if that
ever shows.

Wi-Fi DMA (mode 7) needs no wiring: melonDS never triggers it either
(`DMA.cpp` only knows 0x13 for timing), and no title has been seen to
depend on it.

### Phase 1: the frame engine

`src/core/io/wifi.{h,cpp}` (1 320 + 191 lines) is the block as one class,
owned by `Io` (`nds.io.wifi`), with the register file, timer and power
model moved out of `io.cpp` to join the new TX/RX machinery and the
emulated access point. `wifi_transport.h` is the outside world: an
`MpTransport` (melonDS's `MPInterface` shape) and a `NetDriver`, both
optional and both null today. The port is function-for-function against
melonDS's `Wifi.cpp` / `WifiAP.cpp`, comments included where they explain a
hardware behaviour.

Two things were not a straight port:

- **RF chip type** was read from firmware byte 0x1C (the build year)
  instead of 0x40. The real dump on the shelf is type 2 and byte 0x1C
  happened not to be 3, so nothing had noticed.
- **Probe requests** are answered under the SSID they name, so a firmware
  configured for any open network finds "its" AP (melonDS answers only as
  "melonAP").

### Proof

**Scenes:** the five replays and the GSDD state stay hash-identical (none
of them enables the radio, so this proves the plumbing, not the engine).

**PictoChat**, firmware boot with a scripted tap (`tools/mkdsin.py`, new:
writes a `.dsin` from `F:x,y` touches and `kF:mask` key holds, the same
arguments `trace_melonds` takes): the room list scans channels 1/7/13, the
AP's channel-6 beacons arrive and are dropped by the channel check,
joining Chat Room A hosts it and transmits beacons from slot 4 (FC 0x0080,
88 bytes, channel 1). `DS_WIFI_LOG=1` shows all of it.

**Mario Kart DS against melonDS.** `DS_WIFI_TRACE=<file>` logs every
register access as `R|W addr value`; the harness's melonDS writes the same
under `TRACE_WIFI_REGS` (the hook patch in `dsperate-research/tools/melonds`
is regenerated). Direct boot, one script for both emulators
(`700:128,70 1150:68,143 1500:68,143 1850:188,143 2200:128,70 2900:150,155
3300:128,40`: Multiplayer, the first-run prompts, Multiplayer again, Create
Group, Normal):

| stage | accesses | result |
|-------|---------:|--------|
| boot + Multiplayer tap (1 100 frames) | 31 796 each | identical, line for line |
| lobby scanning (3 300 frames) | 75 283 each under the parity gate | identical except 13 IRQ15 handler blocks that land one channel hop later in melonDS |
| hosting a group (4 400 frames, 67 beacons) | 110 473 each | identical through access 105 410, then the guest writes different bytes into 0x288 and the timelines separate |

The IRQ15 blocks are the same 15 lines in both traces; only where they
fall relative to the guest's channel-hop code differs. Their inputs match
exactly (86 IRQ15 firings each, same W_USCOUNT / BEACONCOUNT / PREBEACON at
every one), and the first radio power-on already finds the two ARM7
timelines 1 504 cycles apart at boot, before any Wi-Fi activity. Both
residuals are the DS-mode ARM7 timing gap with melonDS, which predates
this work and is not the Wi-Fi block's; every Wi-Fi *read* before the
0x288 write is identical.

Parity gate used: `DS_IDLE_SKIP=0 DS_MELON_STM=1 DS_STORE_BUS=0`, lockstep
(no `--quantum 0`), the headless interpreter.

**Firmware-boot oracle: blocked.** The harness's melonDS boots the real
firmware into the first-boot wizard (its RTC reports power lost, and the
harness has no `--rtc-host`; the flag is DSperate's), so PictoChat cannot
be compared against it yet. Direct-boot titles are the oracle route until
the harness grows a power-good RTC with a fixed date on both sides.

### Not done, on purpose

- No transport is implemented: `nds.io.wifi.set_transport` /
  `set_net_driver` exist and nothing calls them. Phases 2 and 3 are the
  ENet LAN transport and libslirp, both of which vendor third-party code
  and want a decision on that first.
- Save states carry the engine (appended to the IO chunk); a mid-session
  state restores a radio with no peer, untested against a real session
  because there is none yet.

## Phase 2 as built (2026-09-11): local wireless over the LAN

`src/net/` is the transport library: ENet 1.3.18 vendored under
`enet/` (MIT; `enet/README.md`), and `lan_mp.{h,cpp}`, the `MpTransport`
that speaks melonDS's `net/LAN.cpp` protocol byte for byte -- the UDP
discovery beacon on 7063, ENet on 7064 with the control channel (client
init, player info, the 16-entry player list, connect/disconnect) and the
MP channel (24-byte `NIFI` header; CMD broadcast, reply to the last host,
ack), the 16 ms freshness window on queued frames and the 25 ms reply wait.
It runs on the emulation thread exactly as melonDS runs it: `process()`
once per frame, the recv calls poll, `recv_replies` waits. `DSPERATE_NET`
(default ON) builds it; both frontends take `--lan-host NAME`,
`--lan-join ADDR`, `--lan-name NAME`, and the headless one `--pace`
(implied by the LAN flags) to run at 59.83 frames a second, which the
protocol's wall-clock windows need.

### Proof

All PictoChat, firmware boot, scripted with `tools/mkdsin.py` (tap
PictoChat at frame 500; host joins Chat Room A at 1100, guest at 1500):

| host | guest | result |
|------|-------|--------|
| DSperate headless | DSperate headless (same box) | guest lists "Chat Room A 1/16", enters; both show "Now entering: Bill Nye" |
| melonDS (harness) | DSperate headless | same |
| DSperate headless | melonDS (harness) | same |
| DSperate headless (dev box) | **DSperate SDL on the RG DS**, over its Wi-Fi | guest received 2 668 CMD frames, replied to 532, acks back |

The melonDS end is `trace_melonds` with melonDS's own `LAN`, `MPInterface`
and `LocalMP` compiled in over our vendored ENet, `--lan-host` /
`--lan-join` / `--pace`, and a new `--rtc-ok` that clears the RTC's
power-lost bit so the firmware boots to its menu instead of the setup
wizard (this unblocks the firmware-boot oracle noted in phase 1).

### What the device run showed

The host's frame statistics on the dev box: worst frame 90 ms, 181
bursts. That is `recv_replies` waiting on the rig's reply over Wi-Fi, the
stall the scoping predicted. It lands on the *host*, once per CMD frame,
and PictoChat sends one CMD every few frames while idle. A DS-to-DS
session on two handhelds will feel it on both sides. The fix is the pacer
mode from the scoping (the clock pauses while the host waits), not a
smaller timeout; it is the next piece of phase 2, together with the menu
page (host / join with the discovery list / leave, and a status line).

### Rig note

`/usr/bin/dsperate` on the RG DS is no longer a bind mount of
`/storage/dsperate/dsperate` (different md5, no entry in /proc/mounts);
a pushed build has to be run from `/storage/dsperate/dsperate` by hand.

### --netplay: host or guest, decided on the LAN

`--netplay` on either frontend listens for hosts' discovery beacons for
2.5 s (a melonDS or DSperate host sends one a second on UDP 7063), joins
the first session heard with a free slot, and otherwise hosts one itself
(`LanMp::start_auto`). No addresses to type: two handhelds both started
with it pair up, whichever came first hosting. Verified on one box (the
second instance joined "First's game") and between the dev box and the
RG DS over Wi-Fi (the rig found "DevBox's game" by broadcast, joined, and
answered 593 CMD frames).

### Messages did not arrive: it was the MAC, not the pacer

A PictoChat message typed on the guest showed on the guest only. Not the
reply wait: the host's radio received every reply frame carrying it with
no failures, and the harness's melonDS failed the same way as host, as
guest, and with melonDS on both ends. Every console booted from one
firmware dump has the same MAC, and PictoChat tells users apart by MAC:
the host saw its own address on the message and dropped it (melonDS's
frontend randomizes the MAC per instance for this reason; the harness did
not). `NDS::set_wifi_mac_suffix` now gives every LAN instance a random
low three bytes, checksum fixed, before the boot. With that the host shows
both "Now entering" lines and the message, on one box and from the RG DS
over Wi-Fi ("Now entering: RGDS", the message, "Now leaving: RGDS").

The reply-wait cost is unchanged and still to do (51 ms worst frame on the
host in that run).

### Pacing, measured (2026-09-11)

The stall the scoping predicted is the MP host's reply wait: the guest
only answers a CMD frame when its emulation processes it, and a frontend
that runs a frame's emulation in a few milliseconds and sleeps the rest
answers up to a frame late. Two things were done and measured with
`lan: reply/host waits` (count, total, max, timeouts) and, on the device,
the audio-queue depth after each frame (the queue is the clock there; a
dip under one frame is the margin going, under zero a stutter):

1. **Sliced frames.** `NDS::run_frame_slice` runs a frame in 1 ms pieces
   (`Scheduler::run_until_or_frame`), and a frontend sleeps between the
   pieces towards the frame's deadline, so a CMD is answered within a
   slice. The headless frontend does this under `--pace`; the SDL one only
   with `DS_WIFI_SLICE=1`.
2. **A late-join race, fixed.** A peer learns another's radio is on only
   from the one-byte connect broadcast sent when it powers up, so a player
   joining a running session (the rig hosting PictoChat, the box joining
   12 s later) missed it and dropped out at the first CMD ("host gone").
   The host now tells a newcomer, and clients tell each other on link-up,
   with the same message. A DSperate joining a *melonDS* host late still
   hits melonDS's side of the race.

RG DS hosting PictoChat, dev box guest, 3000 frames, two runs each:

| mode | host waits total | max | audio queue < 1 frame | dry |
|------|-----------------:|----:|----------------------:|----:|
| plain pacer | 2.37 s | 9.8-11.0 ms | 5, 8 | never |
| sliced | 1.55-1.66 s | 8.7-18.8 ms | 37, 39 | never |

Dev box hosting, RG DS guest: the host's wait per CMD is one Wi-Fi round
trip (3-4 ms, RTT 3 ms by ping) in both modes; slicing cut the *guest's*
waiting for host frames from 4.3 s to 1.7 s.

So on this hardware the wait is ~0.8 ms a frame on a handheld host and
the plain pacer absorbs it; slicing trades audio margin for a shorter
worst wait and stays an experiment. The 51-100 ms worst frames seen on
the dev-box host earlier were the late-join race (waits timing out on a
client that had dropped out), not pacing.

### Download Play: where it stops (2026-09-11, open)

Mario Kart DS Simple-mode group, guest in the firmware's DS Download
Play. Reproduced headless on one box: the guest lists the group, confirms,
sits at "Downloading..." and the host drops it after a while. The
register trace (`DS_WIFI_TRACE`, now with `# CMD rx` / `# reply` markers)
shows a game-level exchange, not a transport one: the guest sends its
nickname in 4-word chunks as MP replies (`0107 "Bil"`, `0207 "l N"`), the
host answers the second with three 266-byte CMD frames, and from then on
the guest re-sends chunk 2 (364 times) while the host polls with an
unchanging acknowledgement and never streams the program.

It is not ours alone. `tools/melonds/localmp_pair` (new in the research
repo) runs two melonDS consoles over melonDS's own LocalMP, its canonical
local-wireless path, with the same scripts and the same trace markers:
melonDS's guest gets one chunk further (`0308 "ye"`, 9 564 times) and
stalls the same way, then falls back to "Looking for software". Over the
LAN transport every host/guest pairing (DSperate or melonDS on either
end) stalls too. The guest's replies alternate between data and empty in
both emulators, because the firmware arms the next reply slot only after
the following CMD has already arrived about half the time; re-sending
the last reply instead of an empty one (`DS_WIFI_REPLY_KEEP=1`) makes the
host poll ten times faster and changes nothing else, so that is not the
blocker either.

So this is a fidelity question in the MP model both emulators share
(melonDS has "implement CMD retries" in its history and the retry path
disabled as "causes instability"), to be taken up with a hardware-level
reading of the DL Play name/ack exchange. Not part of phase 2.

### Download Play, read against GBATEK (2026-09-11)

GBATEK's DS Download Play page (gbatek-ds-wifi-nintendo-ds-download-play)
gives the CMD, REPLY and ACK formats. With the host's outgoing CMD bodies
and incoming reply bodies traced (`# host CMD out` / `# host reply-in` in
`DS_WIFI_TRACE`; `DS_WIFI_TRACE_TIME=1` stamps every access with the
8 us timer), the DSperate-to-DSperate exchange reads as the spec has it:

| host CMD | client reply |
|----------|--------------|
| NameRequest (type 01) x N | Username snippets 0..4 (`0007`, `0107 "Bil"`, `0207 "l N"`, `0307 "ye"`, `0407`) |
| RSA frame (type 03, 0xE4 bytes) x 3 | first: stale snippet; then **RsaReply (type 08)** twice |
| Dummy (type 00) with `0008 0002` in the spare bytes, forever | RsaReply, forever |

So the host game receives the RsaReply against its RSA command and still
never sends a Data packet (type 04). melonDS's model does the same. The
wire is right up to that point; what Mario Kart's host inspects after the
RsaReply is the next question, and it is inside the game (its ARM9 side
of the DL library), to be watched with the CPU-side tools.

Two model details settled on the way, both kept:

- The firmware does not answer a CMD within "a few hundred clock cycles"
  as GBATEK suggests for the reply *contents*; in our emulation it reads
  the CMD within 200 us, then builds its next reply frame in full 1.4 ms
  later (median; after the ack, waiting on the ARM9). One CMD of reply
  lag is therefore inherent and the host tolerates it (the RSA retries).
- The reply's contents are now copied when its transmission ends, the
  latest moment the hardware could still be reading them, instead of at
  the end of the CMD as melonDS does (`mp_reply_pending_`). Which buffer
  answers stays decided at the CMD, as the REPLY1 -> REPLY2 move is.
  Latching the buffer itself late was tried and is wrong (replies vanish).

### Cut Off, and the session's real length (2026-09-11)

The host's "Touch Cut Off to begin racing" was tried while the guest was
in the racer list (it is, for ~300 frames after joining; the button is at
(147,166) on the host's bottom screen then). 70 frames after the tap the
host shows "Communication error. Press the A Button." -- because by then
the guest had already gone: the session lives only ~5 s of Wi-Fi time on
both emulators. Sequence, from the host's trace: NameRequests, username
snippets, the RSA frame three times, the RsaReply (in some runs; in others
the guest stops arming replies right after the RSA frames), then dummies
until the host gives the client up. The guest falls back to "Looking for
software" on its own about 1.5 s after the RSA exchange, so the 15-20 s
the user saw on the device is the host's patience, not the guest's.

An ARM7/ARM9 PC histogram of the guest in that phase shows no Wi-Fi wait
loop (idle ARM7, a plain ARM9 loop), so the firmware's DL client has
decided to abort rather than being stuck. What it checks is the open
question. Open leads, in order:

1. Does the real melonDS frontend (two instances, LocalMP) complete Mario
   Kart DS Download Play at all? Not buildable here (no Qt); a five-minute
   test on a machine with melonDS installed settles whether the model can
   do it. If it can, the harness differs from the frontend somewhere.
2. The client-side abort: trace the guest's ARM9 from the RSA frame to
   the deauth with `--trace` and find the branch that gives up (the
   firmware's DL client is in the dump; the reads it makes of the RX
   header words and the beacon's LCD-sync field are the candidates GBATEK
   points at).
3. A second Download Play host title, to separate Mario Kart's checks
   from the model's.

### Download Play: the first stage works (2026-09-11, late)

With the host's Cut Off tapped early (within ~40 frames of the guest's
sync, before the host sends its RSA frame), Mario Kart DS Download Play
now completes its first stage between two DSperate instances over the
LAN transport: 2 241 data packets (command 04, 0x1F8 bytes each), the
guest's DataReply counting up, the child program verified and booted
(the Nintendo logo on the guest), and the child re-associating for the
game's own second stage. Three transport findings made that possible:

- **Early fetch of host frames.** A client fetched host packets only when
  its timeline had reached `next_sync_`, so the host's next CMD sat unread
  for the whole run-ahead allowance; the host waited on the reply, both
  clocks slowed, and the two ended up timing out on each other (883
  bursts of 150 ms frames in the game-data stage). `peek_host_packet`
  (LanMp; CMD and ack frames only, a beacon at the queue head waits for
  the old path) lets the client pull them as they arrive and still process
  each at its own timestamp. Host waits went from 25 ms timeouts by the
  hundred to a 1.7 ms maximum.
- **The stale-frame window** (melonDS: 16 ms) is now 250 ms
  (`DS_LAN_STALE_MS`); it was a second way to drop the host's CMDs once a
  guest fell a frame behind.
- **Reply contents at transmission end** (the previous section) and the
  late-join connect race fix, both already in.

`--tap-after-sync F:x,y[:N[:R]]` on the headless host taps a point F
frames after the host syncs a client, R times 60 frames apart: the
reliable way to press Cut Off while the guest is in the racer list.

**Still open, with the reference on the other side of it:** Cut Off
*after* the host's RSA exchange. melonDS's LocalMP pair (`localmp_pair
--tap-after-sync 120` and 250) proceeds to data; ours does not: the host
keeps sending dummies with `0008 0002` and never a data packet, although
its trace shows the same RsaReply arriving against its RSA command as in
the runs that work. A second, older failure appears in some runs: the
guest never produces an RsaReply (empty replies from the RSA frame on).
Both are the game's DL library reacting to something in the timing the
LAN path produces and LocalMP does not; the next tool is a CPU-side
watch on the host's ARM9 after the RsaReply (what it compares), since
every wire-level quantity has now been matched.

### Download Play: the fault is the client clock, not the host (2026-09-12)

Two Opus reviews of melonDS's `Wifi.cpp` and `LocalMP.cpp`/`LAN.cpp`
against ours, then a decisive experiment, moved this from "somewhere in
Download Play" to a specific place.

**The decisive test.** A real melonDS guest (the trace harness, its own
`Wifi.cpp` over our vendored ENet) joins our headless host and completes
Download Play: 2 448 data commands, and the guest reaches the DS system
"Downloading... Please wait. Do not turn the power off." screen. So our
MP **host and the LAN transport are correct**. The remaining fault is in
our **client** Wi-Fi timing. Symmetrically, our host with our own guest
still fails about half the time.

**Two client fixes landed** (commit "wifi: MP client reliability"), both
toward melonDS:

- **Immediate reply send.** `send_mp_reply` now transmits the reply frame
  at CMD arrival (`tx_send_frame(slot, 5)` there), as melonDS does, rather
  than deferring the frame's contents to the reply slot's TX end. The
  deferral (added earlier to catch the firmware's in-place reply patching)
  made the client's reply timeline drift ~1 ms from the host's.
- **Reply-queue flush at CMD.** `LanMp::send_cmd` drops queued replies
  before sending, mirroring `LocalMP` re-basing its reply FIFO on every
  CMD. This, not the reply deferral, is the real cure for the host reading
  an RsaReply against the wrong command; the deferral was a symptom patch.

Also: peek is suppressed while the client's own reply is transmitting
(`W_TXBusy & 0x80`), and `DS_WIFI_NO_PEEK` disables the early fetch for
A/B.

**Measured, four runs each, late Cut Off (tap 120 frames after sync):**

| client config | data stage reached |
|---|---|
| immediate reply, peek on (default) | 2 / 4 |
| immediate reply, peek off | 0 / 4 |
| deferred reply, peek off | 1 / 2 |
| melonDS guest vs our host | reliable |

Immediate-reply + peek-on is the best config and is now the default; peek
off is worse, so the early fetch stays.

**The residual fault, and the fix.** Every success has the guest blocking
in lockstep with the host (10k-28k reply/host waits); every failure has it
decoupled (2.5k-6k waits, the guest running ahead). The DS MP protocol is
itself the shared clock: the host is the clock master, each command and
ack carries a timestamp, and the ack grants the client a bounded run-ahead
before it must stop and wait. Two independent console clocks stay locked
because the client never free-runs. Our client does not hold to that
ceiling: its emulated clock is driven by its own wall-clock pacer up to
`next_sync_`, and because host and guest do different work during a
transfer their wall clocks drift. When the guest's clock leads, it consumes
queued host commands early instead of blocking, and the DL library loses
its request/reply rhythm and reports a communication error. The reference
avoids this only because melonDS runs both instances as two threads of one
process sharing a start clock; our two separate processes over sockets are
looser, as two real consoles are.

The fix is a hard client ceiling: never let the guest's emulated clock pass
the last host frame's timestamp plus the granted run-ahead, and stall when
it would. That is the next step, and it is enforcing the protocol's own
clock, not adding a new network time sync.

### Download Play: solved -- two bugs, one symptom (2026-09-12)

Mario Kart DS Download Play now completes between two DSperate instances,
six runs of six, with the host's Cut Off tapped 120 frames after the guest
syncs (the human-timing case that used to fail). ~2428 data commands each,
no duplicate commands, the guest coupled to the host throughout.

Both bugs produced the same symptom -- the guest's ARM7 stops arming MP
replies at a random point, the host's next acks carry a client-failure bit,
and Mario Kart's download library reports a communication error -- which is
why fixing the first one only halved the failures.

**Bug 1: the ARM7 halted with an interrupt already pending.** `HALTCNT`
set `halted` unconditionally. melonDS re-tests the halt condition before
every instruction (`NDS::HaltInterrupted`, from `ARM::Execute`), so a CPU
that halts while `IE & IF` is nonzero never actually sleeps. Our only
un-halt is the IRQ *edge* in `Io::update_irq`, so the ARM7 slept through an
already-pending interrupt until some later, unrelated one woke it. Caught in
an ARM7 instruction trace of a failing guest: it halted 1.3 us after a
command, with a Wi-Fi interrupt pending, and did not wake for 1243 us --
through the ack and through the window in which it had to arm its next
reply. The command period is 1728 us.

This is a **general CPU-fidelity fix, not a Wi-Fi one**. Any title whose
ARM7 halts in a race with its own interrupt could lose up to a frame of
work. The five scene hashes are unchanged, so those scenes never hit the
race, but unexplained hitches elsewhere are worth re-checking against it.

**Bug 2: the early fetch could run mid-reception.** `Wifi::us_timer` could
take the `peek_host_packet` path in the same 8 us tick in which `start_rx()`
had just begun receiving a held frame: `com_status_` was clear when the
client block was entered and `start_rx()` sets it, so it has to be re-tested
in the guard. The fetch overwrote `rx_buffer_` -- the ack being received --
with the next command; the ack's reception then finished with the command's
bytes, so the ack was lost and the command was received twice, once early
and once at its own timestamp. The double reception rotated the reply latch
and ate the armed reply. melonDS's client fetches only at `next_sync`, after
its reply slot, and cannot do this. Visible in a guest trace as the *next*
command arriving 512 us early (the ack's slot) and again 1.2 ms later.

**The method that found bug 1**, worth keeping: a `# frame N` marker in the
Wi-Fi register trace plus `TRACE_END_FRAME` in the headless frontend let an
ARM7 instruction trace (`--trace`, `TRACE_TIME=1`, `--max`) be aligned to
the Wi-Fi trace by scheduler tick, so a command cycle that replied and the
first that did not can be diffed instruction by instruction. The scripts are
in the scratchpad (`a7_diff.py`, `cycle_profile.py`). Windowing matters: the
trace is ~25 MB per emulated frame.

**What this cost.** The ARM7 now does work it previously slept through, so
a guest under an active transfer is dearer: p90 frame time 16.65 -> 19.62 ms
and frames over budget 7 % -> 12-17 % on the dev box. That is correct
behaviour, not a regression to tune away, but it is a real cost and the RG DS
has less headroom. Non-Wi-Fi scenes are unaffected (hashes identical).

**Hypotheses that were wrong**, recorded so they are not chased again:
a client clock-ceiling / run-ahead bound (the guest was *behind* the host at
every failure, and every host frame was already held to its timestamp); the
CPU interleave (lockstep failed 3 of 3, the full parity gate 1 of 2); idle
skip; a missing or mis-valued Wi-Fi register (a melonDS guest and ours touch
exactly the same registers with the same values); and the transport (a real
melonDS guest completes against our host).

### The menu row as built (2026-09-11)

Local wireless is reachable from the pause menu as a single row, not a page:
**NETWORK FEATURES** at the foot of the Emulation page, `net.mode` in `[net]`,
with the values `off` / `auto` / `host` / `guest`.

`auto` is exactly what `--netplay` does: scan 2.5 s, join the first session
heard, host one if there is none. `host` and `guest` are that same scan with
the decision already made. `guest` needed a new argument on
`LanMp::start_auto` (`host_fallback`, default true): heard nobody and it gives
up rather than becoming the host, which is the whole point of choosing GUEST
on the second console -- otherwise two consoles both set to join would race,
and whichever scanned first would silently become the session.

A command-line flag still wins over the row, so `--lan-join ADDR` remains the
way to reach a host on a network that cannot broadcast to it, and the harness
invocations in this document are unaffected.

**Why the row is restart-only** (`FlagRestart`, so it says RESTART REQUIRED):
`NDS::set_wifi_mac_suffix` randomizes the low three bytes of the MAC in the
firmware image before the console boots. Two instances from one firmware dump
otherwise share a MAC, and PictoChat drops messages from its own MAC -- the
fault recorded under "Messages did not arrive" above. A row that started a
session mid-run would walk straight back into it.

**The player name** is `[user] nickname` unless `--lan-name` overrides it,
rather than a Wi-Fi setting of its own: the name a game shows for this console
and the name its peers see are the same thing to whoever is reading the
screen, and `user.nickname` is already a restart-only text row.

**Status is diagnostic, so it is gated.** The join line, the hosting/joined
line and the end-of-run reply-wait and audio-queue statistics are now `VLOG`
(`DS_VERBOSE=1`), alongside the existing `DS_WIFI_LOG=1` session log. Failures
-- a transport that would not start, a `net.mode` value that is not one of the
four, a build without `DSPERATE_NET` -- stay ungated, which is the frontend's
standing rule: the lines you need are the ones printed when something went
wrong. `Dep::Net` greys the row out with THIS BUILD HAS NO NETWORKING when
`DSPERATE_NET` is off, rather than offering a setting that does nothing.

What a page would have given and this does not: leaving a session without
quitting, and a live peer list. `LanMp::players()` and the `wait_*` counters
are there whenever that is worth building.

### The picker went up over Download Play's child boot (2026-09-12)

Reported from a real session: on the SDL frontend, continuing from the
Download Play menu into the downloaded program raised the loader cart's game
list, and the pause that comes with it cut the session's timing dead.

**Why it happens.** The picker's signal is `Gpu::screens_forced_white()` --
both engines' MASTER_BRIGHT driven to white -- and Download Play boots its
child program through that same fade. The original measurement that said
Download Play never forces white was taken while Download Play still
softlocked and could never reach a child boot, so it only ever saw the
Download Play *menus*, which indeed do not.

**Why nothing caught it.** The Download Play guest is a firmware boot, and a
firmware boot is exactly when the SDL frontend puts the loader cart in the
slot and arms `launcher`. Every Download Play run in this document was
headless, and the headless frontend has no picker at all.

**The gate.** `Wifi::mp_active()` (new, public: `is_mp_ || is_mp_client_`),
latched in the frontend as `mp_ever`. Once this console has been in an MP
exchange, the next forced white is the child program rather than a card
launch, and the picker stays down.

Two things make the latch the right instrument rather than a timer:

- It cannot be tested at the white frame itself. The host's Cut Off arrives
  as a deauth, which clears both MP flags (the `0x00C0` path in `Wifi`)
  before the child is verified and booted -- so by the time the fade lands
  there is no live session left to see.
- It needs no expiry, because **there is no way back from PictoChat or
  Download Play to the DS menu on hardware**: the player powers the console
  down. So "has associated at all" cannot produce a false positive against a
  later card launch within the same firmware run.

It is cleared on the firmware-reboot path (`nds.power_off` ->
`nds.reset()`), the one way the DS menu returns with the picker still armed.
The suppression is latched on `launch_latched` like the launch itself, since
the child's white holds indefinitely.

**Status:** builds clean, 21/21 tests pass. Not yet confirmed by a
two-instance SDL run -- that needs a scripted walk of the firmware menu into
Download Play against a Mario Kart host, which nothing here automates yet.
The real dumps for it are in `dsperate-research/binary/real-bios/`.

### The inexact speed knobs are off during a session (2026-09-12)

**Measured on real hardware, both ends:** a full Mario Kart DS Download Play
session from the dev box to the RG DS only held together once `fast_load` and
`cpu_oc` were turned off on the handheld. With either on, the timing stopped
matching and the session fell apart.

That is what should be expected rather than a surprise. A session couples the
two consoles' clocks: the guest holds every host frame until its timestamp and
has to answer inside its own reply slot (the pacing section above), so
anything that changes how long a console's work *appears* to take pulls the
pair apart. All three of the inexact knobs do exactly that -- `cpu_oc` prices
every recompiled memory access as a cached main-RAM load, `timing_oc` drops
the GX FIFO and geometry timing outright, and `fast_load` reads the card off
its clock.

So local wireless now forces all three off for the session, wherever they were
asked for -- `[emu]` in the config, `--cpu-oc` / `--timing-oc` / `--fast-load`
on the command line -- and says so on stderr, ungated, because it is
overriding something the player named. `timing_oc` was not one of the two
measured; it is included because it is the most timing-destructive of the
three and the same reasoning applies.

The menu says it too: `Dep::NetOff` greys CPU OC, TIMING OC and FAST LOAD with
NOT WITH NETWORK FEATURES ON while a session is up. The gate is a session
fact, not a config read -- `Host::net_on` -- so it covers `--netplay` and
`--lan-*` as well as `net.mode`.

Verified: `--netplay --cpu-oc` overrides the flag; `net.mode = host` with all
three on in the config overrides all three; `net.mode = off` with `cpu_oc =
true` says nothing and leaves it alone. 21/21 tests, with the gating asserted
in `test_network_features_row`.

## Phase 3 as built (2026-09-12): internet through the access point

The access point was already there -- `Wifi::ap_send` / `ap_recv` (the
`WifiAP` port) have done the 802.11 ⇄ Ethernet rewrite since phase 1, both
guarded by `if (net_)`. Phase 3 is what sits on the other side of that
pointer: a `NetDriver` over a user-mode TCP/IP stack, plus the DNS policy that
makes the result reach anything.

### libslirp, vendored

`src/net/slirp/`: libslirp v4.9.4 (BSD-3), `src/*.c` and `src/*.h` from the
tag unmodified, `libslirp-version.h` substituted by hand, built by
`src/net/CMakeLists.txt` as `dsperate_slirp` beside `dsperate_enet`.
Vendored for the same reason ENet is: the static handheld tiers cannot
dlopen and have no libslirp in their sysroots.

Upstream needs GLib; `slirp/glib/glib.{h,c}` is ours, about 25 functions over
libc -- allocation, a few string helpers, an append-only `GString`, the log
and assert macros, a PRNG. `g_shell_parse_argv` and `g_spawn_async_with_fds`
are stubs that fail: they serve `fork_exec`, which only runs for a `guestfwd`
with an `-exec` string, and we configure none.

**The trap, and it is worth remembering beyond this shim:** a GLib *function*
the shim misses is a link error, but a GLib *macro* it misses is not an error
at all -- it silently changes what libslirp compiles to, and differently per
word size. `GLIB_SIZEOF_VOID_P` went undefined at first. It selects
`cksum.c`'s accumulator loop and, in `ip.h`, whether `struct mbuf_ptr` is
padded to eight bytes so the overlay over the IP header lines up. The dev
box was fine. The A30 (armv7l) answered ARP and dropped every IP packet,
which is exactly the shape of a broken `ip_input` checksum: the ARP path does
not call `cksum` and the IP path does. So: **re-copies of libslirp are not
proven by a dev-box build. Run `test_slirp_driver` on the A30 toolchain too.**

### The driver

`src/net/slirp_driver.{h,cpp}`, ~300 lines. The DS is handed 10.0.2.15 by
DHCP, gateway 10.0.2.2, nameserver 10.0.2.3 -- slirp's conventional numbers,
the same ones melonDS uses -- and the generated firmware's AP slot is already
configured for DHCP (`firmware_gen.cpp` writes slot 1 as `DSperate-AP` with
zeroed address fields), so nothing has to be typed in. With a real dump the
player's own slots apply; the DS's Nintendo WFC setup writes them, and
firmware writes go to the sidecar, so it is done once.

Everything runs on the emulation thread. `NetDriver::recv` is called from the
ARM7's timeline, so **the socket poll is always zero-timeout** -- there is no
bounded wait here at all, unlike the MP host's reply wait. libslirp is not
thread-safe and this is the only thread that touches it. A dedicated net
thread with two locked queues is the fallback if a measurement ever asks for
one; a DS's real throughput did not.

### The DNS policy

`wifi.dns`: `host`, `wiimmfi` (the default) or an address. Wiimmfi is the
default because it is the only thing a DS can still reach -- Nintendo WFC was
switched off in 2014, so the host's own resolver, which is what a real DS
used, resolves the game's servers to nothing. Verified live 2026-09-12:
`dig @178.62.43.212 nas.nintendowifi.net` answers with Wiimmfi's own address,
which is the whole mechanism.

It is applied as a **rewrite, not a DHCP option**: every query the DS makes is
addressed to whatever its AP slot names, and DHCP names 10.0.2.3, so the
driver rewrites the destination of UDP/53 traffic bound there. That works
whatever the firmware slot says, which a DHCP option alone would not.

**Both directions matter.** The reply comes back from the real resolver's
address and its source must be rewritten back to 10.0.2.3, or the DS's socket
will not match it. Both rewrites patch the IP *and* UDP checksums
incrementally (RFC 1624) -- the addresses are inside the UDP checksum through
the pseudo-header, so patching only the IP header would leave every query to
be dropped by the first host that checks it. A zero UDP checksum means the
sender computed none and stays zero.

### The row, and exclusivity

`net.mode` gains a fifth value, INTERNET, and a `wifi.dns` row (`DNS`) hangs
off it with `Dep::NetInternet`. Being one pick row makes internet and local
wireless **mutually exclusive by construction**, which is the intent: one
radio, one use of it. The flags can still ask for both, so `--internet` with
any `--lan-*` / `--netplay` is refused outright.

Two things deliberately **not** inherited from local wireless:

- **The MAC is not randomized.** That exists so two instances off one firmware
  dump do not share a MAC; on a service that identifies a console, a new MAC
  every boot would be a new console every boot.
- **The inexact speed knobs are not forced off.** `Dep::NetOff` and the
  override both key on the local-wireless flags, and an internet session has
  no peer whose clock it must match.

### Proof

`tests/slirp_driver_test.cpp`, 9 cases, no traffic leaving the machine:

- the DNS rewrite out and back, each asserting both checksums still verify
  against an independent full recomputation, plus a **byte-for-byte round
  trip** (a patch that is merely self-consistent passes the first and fails
  this one);
- a zero UDP checksum stays zero; non-DNS, non-UDP, non-IPv4, wrong-port,
  later-fragment and truncated frames are left alone;
- ARP for the gateway, answered by slirp;
- the whole DHCP conversation -- DISCOVER, OFFER, REQUEST, ACK -- asserting
  the leased address and that the OFFER's option 6 really is 10.0.2.3, which
  is what the rewrite keys on.

Tiers: 22/22 on the dev box, 23/23 on the A30 (armv7l, glibc 2.23, under
qemu), and the aarch64 cross-build links. Exactness gate: sm64, 300 frames,
hash-identical to the pre-change build -- with no net driver attached nothing
in the machine moved, which is what `if (net_)` promises.

### Not proven here

A real session against Wiimmfi. That needs the rig and a Wiimmfi-patched ROM
(their server check is a game-side matter, per the scoping above), and it is
the acceptance test this phase is still waiting on. There is no trace oracle
for it either: the melonDS diff method retires at the AP boundary, because
what is on the other side is the real internet and is not deterministic.

### What a session takes away (2026-09-12)

A network session of either kind means something outside the emulator is
keeping time. Everything that lets the emulator set its own pace, or step
outside the timeline, goes away for the session -- keyed on `net_live`, which
is set once a transport has actually started, not from the flags, so a
session that failed to come up leaves the machine ordinary.

| what | how | why |
|------|-----|-----|
| save / load state, and the autosave on quit | hotkey refused with a line; the rows come off the pause menu | a state freezes this machine and not the one it is talking to, and nothing in a state file restores an ENet session or an open socket |
| fast forward, held and toggled | refused with a line; `emu.fast_forward` in the config forced off at startup | running ahead of the peer is the desync |
| frameskip | forced to 0 at startup, said on stderr; rows greyed `Dep::NetSession` | it exists to let a machine that is behind catch up by dropping presents, and under a session "behind" is fixed by running the frame |

The refusals are ungated on stderr, like the speed-knob override: the player
pressed a key, or named a setting, and nothing happened.

`Dep::NetSession` is deliberately wider than `Dep::NetOff`. NetOff greys the
three inexact speed knobs and is local-wireless-only, because it is about
matching one peer's clock. NetSession covers internet too, because a server
has timeouts whether or not there is a peer.

Startup `--load-state` and `emu.autoload` are **not** gated: they happen
before any transport is up, so there is no session to break. Loading into a
game and then joining a session is the player's business.

### The pause menu stops pausing

A console that goes quiet for the length of a menu visit has left the
session, so under `net_live` the pause menu is an overlay over a running
game rather than a stop. `set_paused` is not called; the menu is opened,
`display.set_page(true)` still goes on (it is what keeps the glyphs legible
on the display-engine tier), and the frontend keeps emulating.

Three things follow:

- **The menu is ticked once a frame** from the live path instead of from the
  idle loop. The tick was pulled out of the paused branch into `pump_menu`
  so both paths run the same code -- there is no second copy of the result
  switch.
- **The game is handed a frame with nothing pressed and no pen down** while
  the menu is up (the hinge is kept: it is a state of the console, not an
  input). The menu takes the buttons; the game sees a player with their
  hands off it, which is what is actually happening.
- **The menu is drawn over the live frame** at the four places the OSD
  already draws -- canvas and scratch, scaled and not -- undimmed, because
  the game is still going and the player may well be watching it.

The page says MENU rather than PAUSED there, and `Menu::set_network_session`
is the one call that does both that and hiding the state rows: they are the
same fact, so they are not two flags.

Verified on the dev box with injected key events, `--internet` against
Super Mario 64 DS: the FPS counter keeps reporting ~60 fps for the whole of
a four-second menu visit (the stopped machine prints nothing, the counter
living in the live path), a state hotkey pressed inside the menu answers
"state: not during a network session", the fast-forward hotkey likewise, and
a screenshot shows MENU with only OPTIONS / RESUME / QUIT over a game whose
pixels are still changing between captures. Control run without `--internet`:
the menu pauses as before, F5 saves, F9 fast-forwards.
