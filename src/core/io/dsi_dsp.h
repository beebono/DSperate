// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSi's DSP: the host interface at 0x04004300 (ARM9) -- PDATA/PADR/PCFG/PSTS/PSEM/PMASK/PCLEAR,
// three CMD/REP port pairs, a 16-word PDATA read FIFO fed by the PCFG-started "DMA" -- and the
// TeakLite II core behind it, which is the vendored teakra running the program the game loads
// into NWRAM. The register side is adapted from melonDS's DSi_DSP (GPLv3, melonDS team); the
// write FIFO is always empty (PSTS bit 8), as there.
#pragma once
#include "core/types.h"

#include <memory>

namespace Teakra { class Teakra; }
namespace melonDS { class DSPInterface; class DSi; }

namespace ds {

struct NDS;
namespace state { class Writer; class Reader; }

class DsiDsp {
public:
  explicit DsiDsp(NDS& nds);
  ~DsiDsp();
  void reset();
  // The core. For the three programs Nintendo's SDK gave games (AAC, G711,
  // graphics), known by the CRC of the program memory when it is started, it
  // is melonDS's high-level rewrite of that program (src/core/dsp/DSP_HLE):
  // cheap. For anything else it is teakra (src/core/dsp/teakra), which runs
  // the DSP's own code out of NWRAM, caught up to the ARM9 at every register
  // access and from a periodic event: exact, and far too slow for a handheld
  // (DS_DSP_LLE=1 uses it for everything).
  bool running_lle() const { return !hle_ && nds_started_; }   // an unknown program is running on teakra
  void irq_rep(int i);                               // the HLE programs' replies and semaphore
  void irq_sem();
  static void event(NDS& nds, u32);                  // EventId::DsiDsp
  void sample_clock(s16 out[2], s16 mic);            // one I2S sample: the DSP's output, the microphone in
  void sync_core(state::Writer& w);                  // the core's own state: a chunk of its own (NDS)
  void sync_core(state::Reader& r);
  void core_absent_in_state();                       // a state from before the core: it starts from reset
  void set_rst_line(bool release);   // SCFG_RST bit 0; also resets the registers
  template <class S> void sync_state(S& s);

  // Offsets from 0x04004300 (the page mirrors every 0x40 bytes).
  u8   read8(u32 addr);
  u16  read16(u32 addr);
  u32  read32(u32 addr) { return read16(addr & 0x3C); }
  void write8(u32 addr, u8 value);
  void write16(u32 addr, u16 value);
  void write32(u32 addr, u32 value) { write16(addr & 0x3C, static_cast<u16>(value)); }

private:
  struct Fifo {
    u16 entries[16] = {};
    u32 occupied = 0, rd = 0, wr = 0;
    void clear() { occupied = rd = wr = 0; }
    bool empty() const { return occupied == 0; }
    bool full() const { return occupied >= 16; }
    void write(u16 v) { if (full()) return; entries[wr] = v; if (++wr >= 16) wr = 0; ++occupied; }
    u16 read() { const u16 v = entries[rd]; if (++rd >= 16) rd = 0; --occupied; return v; }
  };
  u16  psts() const;
  void pdata_write(u16 value);
  u16  pdata_dma_read();
  void pdata_dma_fetch();
  void pdata_dma_start();
  u16  pdata_read_mmio();
  bool core_enabled() const;
  void catch_up();
  void run(u64 until);
  void make_core();
  u16  shared_read(u32 addr) const;
  void shared_write(u32 addr, u16 v);

  NDS& nds_;
  bool rst_ = false;
  u16 padr_ = 0, pcfg_ = 0, psts_ = 0, psem_ = 0, pmask_ = 0xFF, pclear_ = 0;
  u16 cmd_[3] = {};
  s32 pdata_dma_len_ = 0;   // -1: until cancelled
  Fifo rd_fifo_;
  std::unique_ptr<melonDS::DSi> shim_;               // what the HLE sources see of the machine
  std::unique_ptr<melonDS::DSPInterface> core_;
  Teakra::Teakra* teakra_ = nullptr;                 // core_, when it is teakra
  bool hle_ = false;
  bool nds_started_ = false;
  void start_program();
  void stop_program();
  bool make_hle(u32 crc);
  void make_hle_id(u32 id);
  void enter();                                      // before any call into an HLE core
  u64 stamp_ = 0;   // scheduler time the core has been run to
  // The core met something it does not implement (teakra's assertions): it is
  // left stopped, as a DSP that has hung, until the block is next reset.
  bool faulted_ = false;
  void fault(const char* what, const char* file, int line);
  u8   read8_core(u32 addr);
  u16  read16_core(u32 addr);
  void write8_core(u32 addr, u8 value);
  void write16_core(u32 addr, u16 value);
};

} // namespace ds
