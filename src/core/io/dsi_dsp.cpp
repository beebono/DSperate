// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/io/dsi_dsp.h"
#include "core/nds.h"
#include "core/io/io.h"
#include "core/state/state.h"
#include "core/cart/zip.h"
#include <cstdlib>
#include "core/dsp/Savestate.h"
#include "core/dsp/teakra/include/teakra/teakra.h"
#include "core/dsp/teakra/src/crash.h"
#include "core/dsp/DSi.h"
#include "core/dsp/Platform.h"
#include "core/dsp/DSP_HLE/AACUcode.h"
#include "core/dsp/DSP_HLE/G711Ucode.h"
#include "core/dsp/DSP_HLE/GraphicsUcode.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ds {

DsiDsp::DsiDsp(NDS& nds) : nds_(nds) {}
DsiDsp::~DsiDsp() { core_.reset(); }   // before the shim an HLE core unregisters from

// ---- what the HLE sources see of the machine (src/core/dsp/DSi.h) ----
// The ARM9's bus, for the DSP as a second master on it: the page table first
// (Bus::dma_*), as a DMA goes. Bus::read8 and its like are the I/O and
// unmapped-address path alone, and read RAM as nothing.
} // namespace ds
namespace melonDS {
u8  DSi::ARM9Read8(u32 a) { return nds_.bus.dma_read8(ds::Cpu::ARM9, a); }
u16 DSi::ARM9Read16(u32 a) { return nds_.bus.dma_read16(ds::Cpu::ARM9, a); }
u32 DSi::ARM9Read32(u32 a) { return nds_.bus.dma_read32(ds::Cpu::ARM9, a); }
void DSi::ARM9Write8(u32 a, u8 v) { nds_.bus.dma_write8(ds::Cpu::ARM9, a, v); }
void DSi::ARM9Write16(u32 a, u16 v) { nds_.bus.dma_write16(ds::Cpu::ARM9, a, v); }
void DSi::ARM9Write32(u32 a, u32 v) { nds_.bus.dma_write32(ds::Cpu::ARM9, a, v); }
void DSi::refresh_nwram() {
  for (u32 i = 0; i < 8; ++i) { NWRAMMap_B[2][i] = nds_.bus.nwram_dsp(1, i); NWRAMMap_C[2][i] = nds_.bus.nwram_dsp(2, i); }
}
void DSi::RegisterEventFuncs(u32, void* that, std::initializer_list<EventFunc> funcs) {
  that_ = that;
  u32 n = 0;
  for (EventFunc f : funcs) if (n < 4) funcs_[n++] = f;
}
void DSi::UnregisterEventFuncs(u32) { that_ = nullptr; for (EventFunc& f : funcs_) f = nullptr; }
// melonDS counts these delays in 33 MHz system cycles; the scheduler's unit is half that long.
void DSi::ScheduleEvent(u32, bool, s32 delay, u32 funcid, u32 param) {
  nds_.sched.schedule(ds::EventId::DsiDsp, nds_.sched.now() + static_cast<ds::u64>(delay < 1 ? 1 : delay) * 2, ds::DsiDsp::event,
                      (funcid & 3) << 24 | (param & 0xFFFFFF));
}
void DSi::CancelEvent(u32) { nds_.sched.cancel(ds::EventId::DsiDsp); }
void DSi::fire(u32 packed) { if (EventFunc f = funcs_[(packed >> 24) & 3]; f && that_) f(that_, packed & 0xFFFFFF); }
void DSi::MicLine::Start(int) { if (dsi) dsi->nds_.io.note_mic_wanted(); }
void DSi_DSP::IrqRep0() { host->irq_rep(0); }
void DSi_DSP::IrqRep1() { host->irq_rep(1); }
void DSi_DSP::IrqRep2() { host->irq_rep(2); }
void DSi_DSP::IrqSem() { host->irq_sem(); }
namespace Platform {
void Log(LogLevel, const char* fmt, ...) {
  static const bool on = std::getenv("DS_DSP_LOG") != nullptr;
  if (!on) return;
  va_list ap; va_start(ap, fmt);
  std::fputs("dsp: ", stderr); std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}
} // namespace Platform
} // namespace melonDS
namespace ds {

void DsiDsp::irq_rep(int i) { if (pcfg_ & (1u << (9 + i))) nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP); }
void DsiDsp::irq_sem() { psts_ |= 1u << 9; nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP); }
void DsiDsp::enter() { if (hle_ && shim_) shim_->refresh_nwram(); }

// The program the game has put in the DSP's memory, named by its CRC: the
// SDK's three, in the versions melonDS knows.
bool DsiDsp::make_hle(u32 crc) {
  using namespace melonDS::DSP_HLE;
  struct Known { u32 crc; int cls; int version; };
  static const Known known[] = {
    {0x7867C94B, UcodeBase::Class_AAC, -1},     // the DSi Sound app's
    {0x0CAFEF48, UcodeBase::Class_AAC, 0x00}, {0xEF5174AA, UcodeBase::Class_AAC, 0x01},
    {0x1D320185, UcodeBase::Class_AAC, 0x20}, {0xAE11D2FB, UcodeBase::Class_AAC, 0x40},
    {0xFAA1B612, UcodeBase::Class_G711, 0x00}, {0x7EEE19FE, UcodeBase::Class_G711, 0x10}, {0x6056C6FF, UcodeBase::Class_G711, 0x20},
    {0x2C281DAE, UcodeBase::Class_G711, 0x30}, {0x2A1D7F94, UcodeBase::Class_G711, 0x40}, {0x4EBEB519, UcodeBase::Class_G711, 0x50},
    {0xCD2A8B1B, UcodeBase::Class_Graphics, 0x00}, {0x7323B75B, UcodeBase::Class_Graphics, 0x10}, {0xBD4B63B6, UcodeBase::Class_Graphics, 0x11},
    {0x448BB6A2, UcodeBase::Class_Graphics, 0x20}, {0x63CAEC33, UcodeBase::Class_Graphics, 0x30}, {0x1451EB84, UcodeBase::Class_Graphics, 0x40},
    {0x2C974FC8, UcodeBase::Class_Graphics, 0x50},
  };
  for (const Known& k : known)
    if (k.crc == crc) { make_hle_id(static_cast<u32>(k.cls) << 16 | (static_cast<u32>(k.version) & 0xFFFF)); return hle_; }
  return false;
}

// By the ID a core gives itself (and a save state records): class << 16 | version.
void DsiDsp::make_hle_id(u32 id) {
  using namespace melonDS::DSP_HLE;
  if (!shim_) { shim_ = std::make_unique<melonDS::DSi>(nds_); shim_->DSP.host = this; shim_->Mic.dsi = shim_.get(); }
  core_.reset(); teakra_ = nullptr; hle_ = false;
  nds_.sched.cancel(EventId::DsiDsp);
  shim_->refresh_nwram();
  const int version = static_cast<s16>(id & 0xFFFF);
  switch (id >> 16) {
  case UcodeBase::Class_AAC: core_ = std::make_unique<AACUcode>(*shim_, version); break;
  case UcodeBase::Class_G711: core_ = std::make_unique<G711Ucode>(*shim_, version); break;
  case UcodeBase::Class_Graphics: core_ = std::make_unique<GraphicsUcode>(*shim_, version); break;
  default: return;
  }
  hle_ = true;
}

void DsiDsp::start_program() {
  u32 crc = 0;
  static const u8 none[0x8000] = {};
  for (u32 slot = 0; slot < 8; ++slot) { const u8* m = nds_.bus.nwram_dsp(1, slot); crc = cart::crc32_update(crc, m ? m : none, 0x8000); }
  static const bool lle_only = std::getenv("DS_DSP_LLE") != nullptr;   // DS_DSP_LLE=1: teakra for every program
  static const bool log = std::getenv("DS_DSP_LOG") != nullptr;        // DS_DSP_LOG=1: what is started, and the HLE's notes
  nds_started_ = true;
  if (!lle_only && make_hle(crc)) {
    if (log) std::fprintf(stderr, "dsp: program %08X: known, run at high level\n", crc);
    core_->Reset();
    core_->Start();
    return;
  }
  if (hle_ || !core_) make_core();
  nds_.dsi_dsp_started = true;   // what the frontends warn about: this one crawls
  std::fprintf(stderr, "dsp: program %08X is not one of the SDK's known ones: run on the DSP core itself, far slower than real time on a handheld\n", crc);
}

// PCFG bit 0 set again: the DSP is held. A high-level program is gone; teakra waits, reset.
void DsiDsp::stop_program() {
  nds_started_ = false;
  if (hle_) make_core();
  else if (core_) core_->Reset();
}

// The DSP sees NWRAM B as its program space and C as its data space, each in
// eight 32 KB slots that MBK hands to it or not.
u16 DsiDsp::shared_read(u32 addr) const {
  const u8* p = nds_.bus.nwram_dsp((addr & 0x40000) ? 2 : 1, (addr >> 15) & 7);
  if (!p) return 0;
  u16 v; std::memcpy(&v, p + (addr & 0x7FFF), 2);
  return v;
}
void DsiDsp::shared_write(u32 addr, u16 v) {
  u8* p = nds_.bus.nwram_dsp((addr & 0x40000) ? 2 : 1, (addr >> 15) & 7);
  if (p) std::memcpy(p + (addr & 0x7FFF), &v, 2);
}

void DsiDsp::make_core() {
  nds_.sched.cancel(EventId::DsiDsp);
  auto made = std::make_unique<Teakra::Teakra>();
  Teakra::Teakra& t = *made;
  teakra_ = made.get();
  hle_ = false;
  core_ = std::move(made);
  // A reply in one of the three ports, when PCFG asks to hear of it; a semaphore always.
  for (int i = 0; i < 3; ++i) t.SetRecvDataHandler(static_cast<std::uint8_t>(i), [this, i] { irq_rep(i); });
  t.SetSemaphoreHandler([this] { irq_sem(); });
  Teakra::SharedMemoryCallback sm;
  sm.read16 = [this](std::uint32_t a) { return shared_read(a); };
  sm.write16 = [this](std::uint32_t a, std::uint16_t v) { shared_write(a, v); };
  t.SetSharedMemoryCallback(sm);
  // The DSP masters the ARM9's bus through AHBM; at once, with no arbitration (as melonDS).
  Teakra::AHBMCallback ab;
  ab.read8 = [this](std::uint32_t a) { return nds_.bus.dma_read8(Cpu::ARM9, a); };
  ab.write8 = [this](std::uint32_t a, std::uint8_t v) { nds_.bus.dma_write8(Cpu::ARM9, a, v); };
  ab.read16 = [this](std::uint32_t a) { return nds_.bus.dma_read16(Cpu::ARM9, a); };
  ab.write16 = [this](std::uint32_t a, std::uint16_t v) { nds_.bus.dma_write16(Cpu::ARM9, a, v); };
  ab.read32 = [this](std::uint32_t a) { return nds_.bus.dma_read32(Cpu::ARM9, a); };
  ab.write32 = [this](std::uint32_t a, std::uint32_t v) { nds_.bus.dma_write32(Cpu::ARM9, a, v); };
  t.SetAHBMCallback(ab);
  // The microphone is clocked into it whenever I2S runs (sample_clock); enabling it tells the frontend a title wants one.
  t.SetMicEnableCallback([this](bool on) { if (on) nds_.io.note_mic_wanted(); });
}

void DsiDsp::reset() {
  padr_ = pcfg_ = psts_ = psem_ = pclear_ = 0;
  pmask_ = 0xFF;
  cmd_[0] = cmd_[1] = cmd_[2] = 0;
  pdata_dma_len_ = 0;
  rd_fifo_.clear();
  nds_started_ = false;
  if (nds_.dsi && (!core_ || hle_)) make_core();
  faulted_ = false;
  try { if (core_) core_->Reset(); } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); }
  stamp_ = nds_.sched.now();
  nds_.sched.cancel(EventId::DsiDsp);
}

void DsiDsp::fault(const char* what, const char* file, int line) {
  const char* base = std::strrchr(file, '/');
  std::fprintf(stderr, "dsp: the DSP program did something the core does not implement (%s, %s:%d); the DSP is stopped\n", what, base ? base + 1 : file, line);
  faulted_ = true;
  nds_.sched.cancel(EventId::DsiDsp);
}

void DsiDsp::set_rst_line(bool release) {
  rst_ = release;
  reset();
}

// The core runs while it has its clock (SCFG_CLK9 bit 1), is out of reset
// (SCFG_RST bit 0) and PCFG bit 0 does not hold it.
bool DsiDsp::core_enabled() const { return core_ && !hle_ && !faulted_ && (nds_.io.dsi.scfg_clock9 & (1u << 1)) && rst_ && !(pcfg_ & 1); }

// Scheduler time is in 67 MHz units; the DSP's clock is 134 MHz.
void DsiDsp::run(u64 until) {
  if (until <= stamp_) return;
  u64 cycles = (until - stamp_) * 2;
  stamp_ = until;
  if (!core_enabled()) return;
  try {
    while (cycles) { const unsigned n = cycles > 0x1000000 ? 0x1000000u : static_cast<unsigned>(cycles); core_->Run(n); cycles -= n; }
  } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); return; }
  // And again before long, whether or not the ARM9 looks: replies and sound are due on the DSP's own time.
  nds_.sched.schedule(EventId::DsiDsp, until + 2048, event, 0);
}
void DsiDsp::catch_up() { run(nds_.sched.now()); }
void DsiDsp::event(NDS& nds, u32 p) {
  DsiDsp& d = nds.io.dsp;
  if (d.hle_) { d.enter(); if (d.shim_) d.shim_->fire(p); }   // a high-level program's own timed step
  else d.catch_up();
}

void DsiDsp::sample_clock(s16 out[2], s16 mic) {
  out[0] = out[1] = 0;
  enter();
  try { if (core_ && !faulted_) core_->SampleClock(out, mic); } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); out[0] = out[1] = 0; }
}

template <class S> void DsiDsp::sync_state(S& s) {
  s.fields(rst_, padr_, pcfg_, psts_, psem_, pmask_, pclear_, cmd_, pdata_dma_len_,
           rd_fifo_.entries, rd_fifo_.occupied, rd_fifo_.rd, rd_fifo_.wr);
  if constexpr (S::reading) { rd_fifo_.rd &= 15; rd_fifo_.wr &= 15; if (rd_fifo_.occupied > 16) rd_fifo_.occupied = 16; }   // positions in the 16-entry FIFO
}
template void DsiDsp::sync_state<state::Writer>(state::Writer&);
template void DsiDsp::sync_state<state::Reader>(state::Reader&);

// The core's state is teakra's own flat block, kept whole.
void DsiDsp::sync_core(state::Writer& w) {
  std::vector<u8> block;
  if (core_) { melonDS::Savestate f(block); core_->DoSavestate(&f); }
  w.put(stamp_);
  w.put(faulted_);
  w.put(static_cast<u32>(core_ ? core_->GetID() : 0xFFFFFFFFu));   // which core: teakra's ID, or an HLE program's class and version
  w.put(nds_started_);
  w.put(static_cast<u32>(block.size()));
  w.blob(block.data(), block.size());
}
void DsiDsp::sync_core(state::Reader& r) {
  u32 n = 0;
  u32 id = 0xFFFFFFFFu;
  r.put(stamp_); r.put(faulted_); r.put(id); r.put(nds_started_); r.put(n);
  if (n > r.remaining()) { r.fail("DSP core state cut short"); return; }
  std::vector<u8> block(n);
  r.blob(block.data(), n);
  const bool armed = nds_.sched.armed(EventId::DsiDsp);   // an HLE program's pending step, as the state has it
  const u64 armed_at = armed ? nds_.sched.at(EventId::DsiDsp) : 0;
  const u32 armed_param = armed ? nds_.sched.param(EventId::DsiDsp) : 0;
  if (id == Teakra::ID || id == 0xFFFFFFFFu) { if (nds_.dsi && (!core_ || hle_)) make_core(); }
  else { make_hle_id(id); if (!hle_) { r.fail("DSP program in the state is not one this build knows"); return; } }
  if (hle_) {
    melonDS::Savestate f(block.data(), block.size());
    core_->DoSavestate(&f);
    if (f.Error) { r.fail("DSP program state is not this program's"); return; }
    if (armed) nds_.sched.schedule(EventId::DsiDsp, armed_at, event, armed_param);
    stamp_ = nds_.sched.now();
    return;
  }
  if (core_ && n) {
    melonDS::Savestate f(block.data(), block.size());
    core_->DoSavestate(&f);
    if (f.Error) { r.fail("DSP core state is not this core's"); return; }
  }
  stamp_ = nds_.sched.now();
  nds_.sched.cancel(EventId::DsiDsp);
  if (core_enabled()) nds_.sched.schedule(EventId::DsiDsp, stamp_ + 2048, event, 0);
}
void DsiDsp::core_absent_in_state() {
  faulted_ = false;
  nds_started_ = false;
  if (hle_ || !core_) { if (nds_.dsi) make_core(); }
  if (core_) core_->Reset();
  stamp_ = nds_.sched.now();
  nds_.sched.cancel(EventId::DsiDsp);
}

// Bit 9 (semaphore IRQ) is the only sticky bit; write FIFO always empty;
// bits 10-12 a reply waiting, 13-15 a command the DSP has not taken.
u16 DsiDsp::psts() const {
  u16 r = static_cast<u16>((psts_ & (1u << 9)) | (1u << 8));
  if (rd_fifo_.full()) r |= 1u << 5;
  if (!rd_fifo_.empty()) r |= (1u << 6) | (1u << 0);
  if (core_) {
    for (int i = 0; i < 3; ++i) {
      if (!core_->SendDataIsEmpty(static_cast<std::uint8_t>(i))) r |= static_cast<u16>(1u << (13 + i));
      if (core_->RecvDataIsReady(static_cast<std::uint8_t>(i))) r |= static_cast<u16>(1u << (10 + i));
    }
  }
  return r;
}

// PDATA reaches the space PCFG bits 12-14 name: data, MMIO, program, or the
// ARM9's bus through AHBM (when channel 0 is set up for that direction).
void DsiDsp::pdata_write(u16 v) {
  u32 addr = padr_;
  if (core_) {
    switch ((pcfg_ >> 12) & 7) {
    case 0: core_->DataWriteA32(addr | static_cast<u32>(core_->DMAChan0GetDstHigh()) << 16, v); break;
    case 1: core_->MMIOWrite(addr & 0x7FF, v); break;
    case 5: core_->ProgramWrite(addr | static_cast<u32>(core_->DMAChan0GetDstHigh()) << 16, v); break;
    case 7:
      addr |= static_cast<u32>(core_->DMAChan0GetDstHigh()) << 16;
      if (core_->AHBMGetDmaChannel(0) == 0 && core_->AHBMGetDirection(0) == 1) {
        switch (core_->AHBMGetUnitSize(0)) {
        case 0: nds_.bus.dma_write8(Cpu::ARM9, addr, static_cast<u8>(v)); break;
        case 1: core_->AHBMWrite16(addr, v); break;
        case 2: core_->AHBMWrite32(addr, v); break;
        default: break;
        }
      }
      break;
    default: return;
    }
  }
  if (pcfg_ & (1u << 1)) ++padr_;   // auto-increment
  nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP);   // write FIFO empty
}

u16 DsiDsp::pdata_dma_read() {
  u16 r = 0;
  u32 addr = padr_;
  if (core_) {
    switch ((pcfg_ >> 12) & 7) {
    case 0: r = core_->DataReadA32(addr | static_cast<u32>(core_->DMAChan0GetDstHigh()) << 16); break;
    case 1: r = core_->MMIORead(addr & 0x7FF); break;
    case 5: r = core_->ProgramRead(addr | static_cast<u32>(core_->DMAChan0GetDstHigh()) << 16); break;
    case 7:
      addr |= static_cast<u32>(core_->DMAChan0GetDstHigh()) << 16;
      if (core_->AHBMGetDmaChannel(0) == 0 && core_->AHBMGetDirection(0) == 0) {
        switch (core_->AHBMGetUnitSize(0)) {
        case 0: r = nds_.bus.dma_read8(Cpu::ARM9, addr); break;
        case 1: r = core_->AHBMRead16(addr); break;
        case 2: r = static_cast<u16>(core_->AHBMRead32(addr)); break;
        default: break;
        }
      }
      break;
    default: return r;
    }
  }
  if (pcfg_ & (1u << 1)) ++padr_;
  return r;
}

void DsiDsp::pdata_dma_fetch() {
  if (!pdata_dma_len_) return;
  rd_fifo_.write(pdata_dma_read());
  if (pdata_dma_len_ > 0) --pdata_dma_len_;
}

void DsiDsp::pdata_dma_start() {
  static const s32 lens[4] = {1, 8, 16, -1};
  pdata_dma_len_ = lens[(pcfg_ >> 2) & 3];
  const s32 amt = pdata_dma_len_ < 0 ? 16 : pdata_dma_len_;
  for (s32 i = 0; i < amt; ++i) pdata_dma_fetch();
  nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP);
}

u16 DsiDsp::pdata_read_mmio() {
  u16 ret = 0;
  if (!rd_fifo_.empty()) ret = rd_fifo_.read();
  if (pdata_dma_len_ != 0) {
    s32 left = 16 - static_cast<s32>(rd_fifo_.occupied);
    if (pdata_dma_len_ > 0 && pdata_dma_len_ < left) left = pdata_dma_len_;
    for (s32 i = 0; i < left; ++i) pdata_dma_fetch();
  }
  if (!rd_fifo_.empty() || rd_fifo_.full()) nds_.io.request_irq(Cpu::ARM9, io::IRQ_DSI_DSP);
  return ret;
}

u8 DsiDsp::read8_core(u32 addr) {
  enter();
  catch_up();
  switch (addr & 0x3F) {
  case 0x08: return static_cast<u8>(pcfg_);
  case 0x09: return static_cast<u8>(pcfg_ >> 8);
  case 0x0C: return static_cast<u8>(psts());
  case 0x0D: return static_cast<u8>(psts() >> 8);
  case 0x10: return static_cast<u8>(psem_);
  case 0x11: return static_cast<u8>(psem_ >> 8);
  case 0x14: return static_cast<u8>(pmask_);
  case 0x15: return static_cast<u8>(pmask_ >> 8);
  case 0x1C: return core_ ? static_cast<u8>(core_->GetSemaphore()) : 0;
  case 0x1D: return core_ ? static_cast<u8>(core_->GetSemaphore() >> 8) : 0;
  default: return 0;   // no 8-bit PDATA/PADR/PCLEAR reads
  }
}

u16 DsiDsp::read16_core(u32 addr) {
  enter();
  catch_up();
  switch (addr & 0x3E) {
  case 0x00: return pdata_read_mmio();
  case 0x08: return pcfg_;
  case 0x0C: return psts();
  case 0x10: return psem_;
  case 0x14: return pmask_;
  case 0x20: return cmd_[0];
  case 0x28: return cmd_[1];
  case 0x30: return cmd_[2];
  case 0x1C: return core_ ? core_->GetSemaphore() : 0;
  case 0x24: return core_ ? core_->RecvData(0) : 0;   // REP0-2: reading takes the reply
  case 0x2C: return core_ ? core_->RecvData(1) : 0;
  case 0x34: return core_ ? core_->RecvData(2) : 0;
  default: return 0;   // PADR/PCLEAR unreadable
  }
}

void DsiDsp::write8_core(u32 addr, u8 value) {
  enter();
  catch_up();
  switch (addr & 0x3F) {
  case 0x08: pcfg_ = static_cast<u16>((pcfg_ & 0xFF00) | value); break;   // byte writes skip the side effects
  case 0x09: pcfg_ = static_cast<u16>((pcfg_ & 0x00FF) | (value << 8)); break;
  default: break;
  }
  if (core_enabled()) nds_.sched.schedule(EventId::DsiDsp, nds_.sched.now() + 2048, event, 0);
}

void DsiDsp::write16_core(u32 addr, u16 value) {
  enter();
  catch_up();
  switch (addr & 0x3E) {
  case 0x00: pdata_write(value); break;
  case 0x04: padr_ = value; break;
  case 0x08:
    // Bit 0 falling starts the program in the DSP's memory; rising holds the DSP again.
    if ((pcfg_ & 1) && !(value & 1)) start_program();
    else if (!(pcfg_ & 1) && (value & 1)) stop_program();
    pcfg_ = value;
    if (pcfg_ & (1u << 4)) pdata_dma_start();
    else { pdata_dma_len_ = 0; rd_fifo_.clear(); }
    break;
  case 0x10: psem_ = value; if (core_) core_->SetSemaphore(value); break;
  case 0x14: pmask_ = value; if (core_) core_->MaskSemaphore(value); break;
  case 0x18:   // PCLEAR
    if (core_) core_->ClearSemaphore(value);
    if (!core_ || core_->GetSemaphore() == 0) psts_ &= static_cast<u16>(~(1u << 9));
    break;
  case 0x20: cmd_[0] = value; if (core_) core_->SendData(0, value); break;
  case 0x28: cmd_[1] = value; if (core_) core_->SendData(1, value); break;
  case 0x30: cmd_[2] = value; if (core_) core_->SendData(2, value); break;
  default: break;
  }
  // The write may have let the core go (or the clock and reset lines did, since the last look).
  if (core_enabled()) nds_.sched.schedule(EventId::DsiDsp, nds_.sched.now() + 2048, event, 0);
}

// The registers reach into the core, and the core may refuse (fault): the
// access then reads as nothing, and the DSP stays stopped.
u8 DsiDsp::read8(u32 addr) { try { return read8_core(addr); } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); return 0; } }
u16 DsiDsp::read16(u32 addr) { try { return read16_core(addr); } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); return 0; } }
void DsiDsp::write8(u32 addr, u8 value) { try { write8_core(addr, value); } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); } }
void DsiDsp::write16(u32 addr, u16 value) { try { write16_core(addr, value); } catch (const TeakraFault& f) { fault(f.expression, f.file, f.line); } }

} // namespace ds
