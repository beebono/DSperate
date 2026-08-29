// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

namespace ds::sdl {

// The host's lid, for handhelds with a real hinge (the RG DS: an evdev
// "gpio-keys-hall" device reporting SW_LID). SDL has no switch events, so
// the device is read directly. Closing the lid also suspends the host, so
// the sequence a game sees is: closed for the frames before the suspend
// (it goes to sleep), then open again after the resume (the lid IRQ wakes
// it) -- the same as a DS. Hosts without a switch get the second half from
// the suspend itself: CLOCK_BOOTTIME runs on through a suspend while
// CLOCK_MONOTONIC does not, so a jump between them pulses the lid closed
// for a few frames and open again.
class Lid {
public:
  void open();                 // find the switch; harmless when there is none
  void close();
  // Polls the switch and the suspend detector once a frame; returns true
  // when the state changed and stores it in `closed`.
  bool poll(bool& closed);
  bool has_switch() const { return fd_ >= 0; }

private:
  int  fd_ = -1;
  bool closed_ = false;
  int  pulse_ = 0;             // frames left of a suspend-detected close
  s64  skew_ns_ = 0;           // last boottime - monotonic
};

} // namespace ds::sdl
