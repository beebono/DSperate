// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <vector>

namespace ds::sdl {

// Microphone straight from ALSA, bypassing the session's sound server. On
// the handhelds SDL's capture goes through PipeWire, whose only "source" is
// a monitor of the speaker: the game hears itself and nothing else. The
// codec's own capture PCM is free even while PipeWire holds playback, so it
// is opened directly (DS_MIC_DEV, default plughw:0,0 -- the plug layer does
// the mono/rate conversion). libasound is dlopen'd; without it, or without
// the device, open() fails and the SDL path is used.
class MicAlsa {
public:
  bool open(u32 rate, const char* device = nullptr);   // device: ALSA name; null/empty = DS_MIC_DEV or plughw:0,0
  void close();
  bool active() const { return pcm_ != nullptr; }
  // True when the capture PCM opened but rejected our parameters. The device
  // is there and busy or restricted; letting SDL open it too would only poke
  // the same codec a second time (on the handhelds the capture and playback
  // substreams share a DAI, and a second failed setup can leave the playback
  // stream not consuming), so the SDL fallback is skipped in that case.
  bool rejected() const { return rejected_; }
  void capture(std::vector<s16>& out);   // everything available, non-blocking

private:
  bool rejected_ = false;
  void* lib_ = nullptr;
  void* pcm_ = nullptr;
  long (*readi_)(void*, void*, unsigned long) = nullptr;
  int  (*recover_)(void*, int, int) = nullptr;
  int  (*close_)(void*) = nullptr;
};

} // namespace ds::sdl
