// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Internet through the emulated access point: the NetDriver that carries the
// AP's Ethernet frames to and from a user-mode TCP/IP stack (vendored
// libslirp, slirp/README.md). The emulator owns a private subnet, the
// guest's TCP and UDP connections become ordinary host sockets, and nothing
// needs privileges or a second MAC on the host NIC -- which is what makes it
// the only internet transport that can work on a wlan0-only handheld.
//
// The DS sees a plain DHCP network: address 10.0.2.15, gateway 10.0.2.2,
// nameserver 10.0.2.3. Those are slirp's conventional numbers, and the DS's
// firmware AP slot is configured for DHCP, so the console is handed all of
// them without the player entering anything.
//
// Everything here runs on the emulation thread. NetDriver::recv is called
// from the ARM7's timeline (Wifi::ap_recv), so nothing in this class may
// block: the socket poll is always run with a zero timeout, and a frame that
// is not ready yet is simply not there this tick. libslirp is not
// thread-safe and this is the only thread that touches it.
//
// docs/wifi-scoping.md.
#pragma once

#include "core/io/wifi_transport.h"
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

struct Slirp;

namespace ds::net {

class SlirpDriver final : public io::NetDriver {
public:
  // Where the guest's DNS queries go. The DS asks whatever its AP slot names
  // as a nameserver, and DHCP names slirp's own 10.0.2.3, so this is applied
  // by rewriting queries addressed there -- it works whatever the firmware's
  // AP slot says, which a DHCP option alone would not.
  enum class Dns {
    Host,      // slirp answers 10.0.2.3 itself, forwarding to the host resolver
    Custom,    // rewritten to custom_addr: Wiimmfi, or whatever the player named
  };

  SlirpDriver();
  ~SlirpDriver() override;
  SlirpDriver(const SlirpDriver&) = delete;
  SlirpDriver& operator=(const SlirpDriver&) = delete;

  // dns_addr is an IPv4 address in host order; ignored unless dns is Custom.
  bool start(Dns dns = Dns::Host, u32 dns_addr = 0);
  void stop();
  bool ok() const { return slirp_ != nullptr; }
  const std::string& error() const { return err_; }

  // Drive the stack once, outside any guest frame traffic. Called once a
  // frame by the frontend so timers fire and sockets make progress even
  // while the game is not touching the radio.
  void process();

  // NetDriver
  int send(const u8* data, int len) override;
  int recv(u8* data) override;

  // Diagnostics (DS_VERBOSE): what has crossed the AP boundary.
  unsigned frames_out() const { return frames_out_; }
  unsigned frames_in() const { return frames_in_; }
  unsigned frames_dropped() const { return frames_dropped_; }   // rx queue was full
  unsigned dns_rewritten() const { return dns_rewritten_; }

  // The DNS the DS is handed by DHCP and therefore the address every query
  // it makes is sent to. 10.0.2.3, host order.
  static constexpr u32 kVirtualDns = 0x0A000203;

  // The DNS rewrite, as two pure functions on one Ethernet frame in place:
  // the destination on the way out, the source on the way back. Public
  // because the checksum arithmetic is what the tests are for -- a wrong
  // patch here is a query that silently goes nowhere.
  //
  // Each returns true if it matched and rewrote. A frame that is not an
  // IPv4 UDP datagram on port 53 from/to `from` is left alone.
  static bool rewrite_dns_dst(u8* frame, int len, u32 from, u32 to);
  static bool rewrite_dns_src(u8* frame, int len, u32 from, u32 to);

  // The largest Ethernet frame this hands back from recv(). NetDriver::recv
  // takes no buffer length -- the AP's buffer is 2048 and the 802.11 header
  // it wraps the frame in costs 40 -- so the MTU is chosen to stay under it.
  static constexpr std::size_t kMtu = 1500;
  static constexpr std::size_t kMaxFrame = kMtu + 14 + 4;

private:
  // The libslirp callbacks live in the .cpp: their signatures are spelled in
  // libslirp's own types, and that header stays out of this one.
  friend struct SlirpCallbacks;

  struct Timer {
    int id = 0;
    void* cb_opaque = nullptr;
    s64 expire_ms = -1;      // < 0: not armed
  };

  void pump();               // fill, poll(0), process -- the whole stack step
  void fire_due_timers();
  s64  now_ms() const;

  Slirp* slirp_ = nullptr;
  std::string err_;
  Dns dns_ = Dns::Host;
  u32 dns_addr_ = 0;                 // host order
  std::deque<std::vector<u8>> rx_;   // frames from the stack waiting for the guest
  // What add_poll gathered this step, in poll(2)'s terms but without
  // dragging <poll.h> into this header.
  struct PollFd { int fd; short events; short revents; };
  std::vector<PollFd> pollfds_;
  unsigned frames_out_ = 0, frames_in_ = 0, frames_dropped_ = 0, dns_rewritten_ = 0;
  std::deque<Timer> timers_;         // deque: slirp holds pointers to these
  s64 start_ns_ = 0;
};

} // namespace ds::net
