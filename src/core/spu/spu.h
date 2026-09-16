// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <array>
#include <cstddef>

namespace ds { struct NDS; }
namespace ds::spu {

// Sound unit (ARM7 side, registers 0x04000400-0x0400051F).
//
// Sixteen channels (PCM8 / PCM16 / IMA-ADPCM on all, PSG square on 8-13,
// noise on 14-15), each stepping its own 16-bit timer at the SPU clock, a
// mixer producing one stereo sample every 2048 ARM9 cycles (32.768 kHz), and
// two capture units writing the mixer output back to ARM7 memory. Samples
// are fetched through a 32-byte per-channel FIFO like the hardware does
// (16-byte bursts, never from the ARM7 BIOS). The integer arithmetic and its
// truncation points follow melonDS so the output can be compared sample for
// sample.
//
// Output is interleaved s16 stereo at 32768 Hz in a ring the frontend
// drains; the oldest samples are overwritten when nobody drains it.
class Spu {
public:
  explicit Spu(NDS& nds) : nds_(nds) {}
  void reset();
  template <class S> void sync_state(S& s);   // after catch_up(); the output ring is dropped

  static bool owns_reg(u32 addr) { return addr >= 0x04000400 && addr < 0x04000520; }
  u32  read(u32 addr, u32 width);
  void write(u32 addr, u32 width, u32 value);

  void set_powcnt2(u16 v) { catch_up(); muted_ = !(v & 1); }

  // Mix every sample whose nominal time is <= `t`. Samples are produced in
  // batches (DS_SPU_BATCH, default 16) from one scheduler event, so anything
  // that observes or changes SPU state between events calls this first: the
  // register paths do, and the result is sample-for-sample what one event
  // per sample produced. Capture forces a batch of one, since it writes RAM.
  void run_to(u64 t) { while (mix_at_ <= t) { mix(); mix_at_ += mix_period_; } }
  // DSi SNDEXCNT (0x04004700, via Io::dsi_write): bit 15 I2S enable, 14 mute,
  // 13 selects 47.6 kHz output (only while disabled), bits 0-3 the NITRO/DSP
  // mix ratio. The DSi ignores SOUNDBIAS. melonDS DSi_I2S::WriteSndExCnt.
  void write_sndexcnt(u16 value, u16 mask);
  // What the mixer actually produces, from the clock and the mix period
  // rather than from the nominal name of the rate. The SPU emits one sample
  // every mix_period_ ARM9 cycles, so the DS runs at 67027964 / 2048 =
  // 32728.5 Hz -- *not* the 32768 its "32.768 kHz" name suggests, which is
  // 1207 ppm away. The DSi's high-rate mode is 67027964 / 1408 = 47605.1.
  //
  // The difference is small and it is not nothing: it is a fixed ratio error
  // on everything the frontend resamples, and before this was derived the
  // rate control sat at -1200 ppm for entire runs holding the queue against
  // it -- correcting our own constant while it was documented as the host's
  // crystal (docs/frame-pacing-scoping.md). Anything converting the stream
  // wants the exact one; SAMPLE_RATE is a name, good for asking a device for
  // a rate it will recognise and for sizing a mic buffer, not for a ratio.
  double output_rate_hz() const { return static_cast<double>(ARM9_CLOCK_HZ) / mix_period_; }
  u32  output_rate() const { return static_cast<u32>(output_rate_hz() + 0.5); }
  void set_apply_bias(bool on) { apply_bias_ = on; }
  void catch_up();

  // Output ring (stereo frames). `take` copies up to `max_frames` frames into
  // `dst` (2 * frames s16) and returns the count.
  size_t available() const { return (wr_ - rd_) & (RING_FRAMES - 1); }
  size_t take(s16* dst, size_t max_frames);
  void   drain() { rd_ = wr_; }
  // Frames the ring dropped because nobody took them for half a second: a
  // frontend that stopped draining, or a fast forward outrunning the ring.
  // Not saved in a state and not read by the core -- a statistic only.
  u64    ring_overruns() const { return overruns_; }

  // The nominal name of the DS rate, not the rate: the mixer produces
  // 32728.5 Hz (output_rate_hz()). Use this to ask a device for a rate it
  // will recognise, or to size a buffer; never as the input of a conversion.
  static constexpr u32 SAMPLE_RATE = 32768;
  static constexpr u32 MIX_PERIOD  = 2048;      // ARM9 cycles per output sample
  static constexpr u32 MIX_PERIOD_47K = 1408;   // the DSi's 47605 Hz (melonDS: 704 ARM7 cycles)
  static constexpr u32 TIMER_STEP  = 512;       // channel timer ticks per output sample (mix period / 4)

  // Exposed for the tests.
  static const u16 ADPCM_TABLE[89];
  static const s8  ADPCM_INDEX[8];

private:
  struct Channel {
    u32 cnt = 0, src = 0, loop = 0, len = 0;    // loop/len in bytes
    u16 timer_reload = 0;
    u32 timer = 0;
    s32 pos = 0;
    s16 cur = 0;
    u16 noise = 0;
    u8  volume = 0, vol_shift = 0, pan = 0;
    bool key_on = false;
    s32 adpcm_val = 0, adpcm_idx = 0, adpcm_val_loop = 0, adpcm_idx_loop = 0;
    u8  adpcm_byte = 0;
    u32 fifo[8] = {};
    u32 fifo_rd = 0, fifo_wr = 0, fifo_off = 0, fifo_level = 0;
    u32 format() const { return (cnt >> 29) & 3; }
    u32 repeat() const { return (cnt >> 27) & 3; }
  };
  struct Capture {
    u8  cnt = 0;
    u32 dst = 0, len = 4;
    u16 timer_reload = 0;
    u32 timer = 0, pos = 0;
    u32 fifo[4] = {};
    u32 fifo_rd = 0, fifo_wr = 0, fifo_off = 0, fifo_level = 0;
  };

  void set_cnt(Channel& c, u32 v);
  void start(Channel& c);
  void fifo_fill(Channel& c);
  template <typename T> T fifo_read(Channel& c);
  void next_pcm8(Channel& c);
  void next_pcm16(Channel& c);
  void next_adpcm(Channel& c);
  s32  run_channel(Channel& c, u32 n);

  void cap_set_cnt(Capture& cp, u8 v);
  void cap_flush(Capture& cp);
  template <typename T> void cap_write(Capture& cp, T v);
  void cap_run(Capture& cp, s32 sample);

  static void ev_mix(NDS& nds, u32);
  void mix();
  void push(s16 l, s16 r);

  NDS& nds_;
  std::array<Channel, 16> ch_;
  std::array<Capture, 2>  cap_;
  u16  cnt_ = 0;
  u16  bias_ = 0;
  u8   master_ = 0;
  bool muted_ = true;
  bool cap_warned_ = false;
  bool dbg_ = false;                     // DS_DEBUG_SPU: log control/key-on writes
  u64  mix_at_ = 0;                      // nominal time of the next sample
  u32  mix_period_ = MIX_PERIOD;         // MIX_PERIOD, or MIX_PERIOD_47K on a DSi with SNDEXCNT bit 13
  u32  timer_step_ = TIMER_STEP;         // mix_period_ / 4
  bool apply_bias_ = true;               // SOUNDBIAS: DS yes, DSi no
  u32  batch_ = 16;                      // samples per mix event (DS_SPU_BATCH)
  u32  cap_batch_ = 1;                   // ... while a capture runs (DS_SPU_CAP_BATCH; 1 = a sample per event, the conservative default)

  static constexpr size_t RING_FRAMES = 16384;   // half a second
  std::array<s16, RING_FRAMES * 2> ring_{};
  size_t rd_ = 0, wr_ = 0;
  u64    overruns_ = 0;                  // frames the ring overwrote unheard
};

} // namespace ds::spu
