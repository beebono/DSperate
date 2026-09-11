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

One menu page in the pattern of the RetroAchievements pages
(memory: retroachievements-scoping): mode off / local (host, join with the
discovery list) / internet; the DNS choice; a status line (peers, dropped
replies, AP association state). Config keys under `[wifi]` in
`configs/default.ini`.

## Phases

| phase | deliverable | proof | size |
|------:|-------------|-------|-----:|
| 0 | the `dsiware` timer on the DS too; Wi-Fi DMA trigger wired | 5 scenes hash-identical; rig cost with a Wi-Fi menu open | small |
| 1 | TX/RX engine + `WifiAP` + `NoPeer` transport | PictoChat boots to "no one nearby"; a WFC connection test associates and fails at DHCP; `trace_melonds` identical with melonDS's dummy net | ~1 900 lines |
| 2 | `LanMp` over ENet, melonDS wire format; socket thread; menu page | RG DS <-> melonDS PC in PictoChat, then Mario Kart DS multi-cart; two handhelds; Download Play of a demo | ~800 lines + ENet |
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
