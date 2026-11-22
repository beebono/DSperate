// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What melonDS's DSP HLE (DSP_HLE/) uses of melonDS's DSi class, so those
// sources stay as they are: the ARM9's bus, the DSP's view of NWRAM, one
// scheduled event, the microphone switch and the way back to the DSP block.
// Implemented in src/core/io/dsi_dsp.cpp.
#pragma once
#include <initializer_list>
#include "types.h"
#include "DSi_DSP.h"

namespace ds { struct NDS; }

namespace melonDS {

enum { Event_DSi_DSPHLE = 0 };
enum { Mic_DSi_DSP = 1 };
using EventFunc = void (*)(void* that, u32 param);
#define MakeEventThunk(cls, func) [](void* that, melonDS::u32 param) { static_cast<cls*>(that)->func(param); }

class DSi {
public:
  explicit DSi(ds::NDS& nds) : nds_(nds) {}

  u8  ARM9Read8(u32 addr);
  u16 ARM9Read16(u32 addr);
  u32 ARM9Read32(u32 addr);
  void ARM9Write8(u32 addr, u8 val);
  void ARM9Write16(u32 addr, u16 val);
  void ARM9Write32(u32 addr, u32 val);

  // [cpu][slot]; only the DSP's row (2) is filled, by refresh_nwram before each call in.
  u8* NWRAMMap_B[3][8] = {};
  u8* NWRAMMap_C[3][8] = {};
  void refresh_nwram();

  // One event, whose function and parameter ride in the scheduler's parameter.
  void RegisterEventFuncs(u32 id, void* that, std::initializer_list<EventFunc> funcs);
  void UnregisterEventFuncs(u32 id);
  void ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid, u32 param);
  void CancelEvent(u32 id);
  void fire(u32 packed);

  // The microphone is clocked into the DSP whenever I2S runs, so there is
  // nothing to switch here; a start does tell the frontend a title wants it.
  struct MicLine { DSi* dsi = nullptr; void Start(int); void Stop(int) {} } Mic;
  DSi_DSP DSP;

private:
  ds::NDS& nds_;
  void* that_ = nullptr;
  EventFunc funcs_[4] = {};
};

} // namespace melonDS
