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
