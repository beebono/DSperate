// SPDX-License-Identifier: GPL-3.0-or-later
// The interface melonDS's teakra copy derives its Teakra class from
// (melonDS src/DSi_DSP.h, Copyright 2020 PoroCYon, 2016-2025 melonDS team;
// GPLv3), kept under its own name and path so the vendored sources in
// src/core/dsp/teakra need no change.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "types.h"
#include "Savestate.h"
#include "FIFO.h"

namespace ds { class DsiDsp; }

namespace melonDS {

class Savestate;
class DSi;

// The part of melonDS's DSi_DSP the HLE programs call: a reply or a semaphore
// for the ARM9. DsiDsp (src/core/io/dsi_dsp.cpp) is what answers.
class DSi_DSP {
public:
  ds::DsiDsp* host = nullptr;
  void IrqRep0();
  void IrqRep1();
  void IrqRep2();
  void IrqSem();
};

class DSPInterface {
public:
  virtual ~DSPInterface() {}

  virtual void Reset() = 0;
  virtual void DoSavestate(Savestate* file) = 0;
  virtual u32 GetID() = 0;

  // APBP data and semaphore
  virtual bool SendDataIsEmpty(std::uint8_t index) const = 0;
  virtual void SendData(std::uint8_t index, std::uint16_t value) = 0;
  virtual bool RecvDataIsReady(std::uint8_t index) const = 0;
  virtual std::uint16_t RecvData(std::uint8_t index) = 0;
  virtual void SetSemaphore(std::uint16_t value) = 0;
  virtual void ClearSemaphore(std::uint16_t value) = 0;
  virtual void MaskSemaphore(std::uint16_t value) = 0;
  virtual std::uint16_t GetSemaphore() const = 0;

  // DSP_PDATA/PADR transfers
  virtual std::uint16_t ProgramRead(std::uint32_t address) const = 0;
  virtual void ProgramWrite(std::uint32_t address, std::uint16_t value) = 0;
  virtual std::uint16_t DataReadA32(std::uint32_t address) const = 0;
  virtual void DataWriteA32(std::uint32_t address, std::uint16_t value) = 0;
  virtual std::uint16_t MMIORead(std::uint16_t address) = 0;
  virtual void MMIOWrite(std::uint16_t address, std::uint16_t value) = 0;
  virtual std::uint16_t DMAChan0GetSrcHigh() = 0;
  virtual std::uint16_t DMAChan0GetDstHigh() = 0;
  virtual std::uint16_t AHBMGetUnitSize(std::uint16_t i) const = 0;
  virtual std::uint16_t AHBMGetDirection(std::uint16_t i) const = 0;
  virtual std::uint16_t AHBMGetDmaChannel(std::uint16_t i) const = 0;
  virtual std::uint16_t AHBMRead16(std::uint32_t addr) = 0;
  virtual void AHBMWrite16(std::uint32_t addr, std::uint16_t value) = 0;
  virtual std::uint16_t AHBMRead32(std::uint32_t addr) = 0;
  virtual void AHBMWrite32(std::uint32_t addr, std::uint32_t value) = 0;

  virtual void Start() {}
  virtual void Run(unsigned cycle) { (void)cycle; }
  virtual void SampleClock(s16 output[2], s16 input) = 0;
};

} // namespace melonDS
