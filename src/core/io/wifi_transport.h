// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What the Wi-Fi block talks to outside the machine (docs/wifi-scoping.md).
// Two interfaces, both optional: with neither set the radio behaves like a
// DS alone in a room -- frames go out to nobody and nothing ever arrives,
// which keeps every run deterministic.
//
// MpTransport carries raw DS frames (the 12-byte TX header plus the 802.11
// frame) between emulator instances for local wireless: PictoChat, Download
// Play, multi-cart. Its shape is melonDS's MPInterface, and a transport that
// speaks melonDS's LAN wire format makes a DSperate a peer of a melonDS.
//
// NetDriver carries Ethernet frames between the emulated access point and
// the host network (libslirp user-mode NAT is the one that works on a
// wlan0-only handheld).
//
// Both are polled from the ARM7's timeline on the emulation thread, so an
// implementation must never block in recv_*; only recv_replies (the MP host
// waiting for its clients' time slots) is allowed a bounded wait, and that
// wait is the one place a multiplayer session can stall the frame.
#pragma once

#include "core/types.h"

namespace ds::io {

class MpTransport {
public:
  virtual ~MpTransport() = default;
  virtual void begin() {}
  virtual void end() {}
  // Regular frames (LOC1-3 slots and beacons).
  virtual int  send_packet(const u8* data, int len, u64 timestamp) = 0;
  virtual int  recv_packet(u8* data, u64* timestamp) = 0;   // > 0: bytes; 0: nothing
  // The MP protocol: host CMD, client reply in its slot, host ack.
  virtual int  send_cmd(const u8* data, int len, u64 timestamp) = 0;
  virtual int  send_reply(const u8* data, int len, u64 timestamp, u16 aid) = 0;
  virtual int  send_ack(const u8* data, int len, u64 timestamp) = 0;
  virtual int  recv_host_packet(u8* data, u64* timestamp) = 0;   // < 0: the host is gone
  // The same without waiting: a host packet already here, or 0. A client
  // fetches early with this and still processes the frame at its own
  // timestamp; waiting only when its timeline has caught up with the host
  // let the host's next CMD sit unread for the whole run-ahead allowance,
  // and the two ends then slowed each other down until the game gave up.
  virtual int  peek_host_packet(u8* data, u64* timestamp) { (void)data; (void)timestamp; return 0; }
  // Gather the replies of the clients in aidmask into 15 x 1024-byte slots;
  // returns the mask of clients that answered.
  virtual u16  recv_replies(u8* data, u64 timestamp, u16 aidmask) = 0;
};

class NetDriver {
public:
  virtual ~NetDriver() = default;
  virtual int send(const u8* data, int len) = 0;
  virtual int recv(u8* data) = 0;   // > 0: bytes of one Ethernet frame; 0: nothing
};

} // namespace ds::io
