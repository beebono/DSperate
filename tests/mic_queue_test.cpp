// SPDX-License-Identifier: GPL-3.0-or-later
// The frontend's microphone queue: bursty capture in, one steady frame out.
// The capture side is a clock of its own (32768 Hz, delivering in fixed
// chunks), the frame side runs at the host's 60 Hz: the drift between them is
// what emptied the old queue every other frame.
#include "frontend/sdl/mic_queue.h"
#include "check.h"

#include <cstdio>
#include <vector>

namespace {

constexpr double PER_FRAME = 32768.0 * 1120380.0 / 67027964.0;   // samples in one emulated frame

struct Result { unsigned underruns; size_t min_take, max_take, max_level; size_t gaps; };

// `stall_at`: from that frame, nothing arrives for `stall_len` frames, then all of it at once.
Result run(size_t chunk, double frame_hz, int frames, int stall_at = -1, int stall_len = 0) {
  ds::sdl::MicQueue q(PER_FRAME);
  std::vector<ds::s16> in, out;
  double produced = 0; size_t sent = 0;
  ds::s16 next_in = 0, expect = 0; bool started = false;
  Result r{0, ~size_t(0), 0, 0, 0};
  for (int f = 0; f < frames; ++f) {
    produced += 32768.0 / frame_hz;
    in.clear();
    const bool stalled = f >= stall_at && f < stall_at + stall_len;
    while (!stalled && produced - static_cast<double>(sent) >= static_cast<double>(chunk)) {
      for (size_t k = 0; k < chunk; ++k) in.push_back(next_in++);
      sent += chunk;
    }
    q.frame(in.data(), in.size(), out);
    if (!started && !out.empty()) { started = true; expect = out[0]; }
    for (ds::s16 v : out) { if (v != expect) ++r.gaps; expect = static_cast<ds::s16>(v + 1); }
    if (f >= 120 && (stall_at < 0 || f < stall_at || f > stall_at + stall_len + 120)) {   // settled
      r.min_take = std::min(r.min_take, out.size());
      r.max_take = std::max(r.max_take, out.size());
      r.max_level = std::max(r.max_level, q.level());
    }
  }
  r.underruns = q.underruns();
  return r;
}

} // namespace

int main() {
  const size_t lo = static_cast<size_t>(PER_FRAME * (1 - ds::sdl::MicQueue::MAX_TRIM)) - 1;
  const size_t hi = static_cast<size_t>(PER_FRAME * (1 + ds::sdl::MicQueue::MAX_TRIM)) + 1;

  // ALSA on the RG DS Plus: 1024 samples every other frame. Host at 60 Hz, and at the DS's own 59.83 Hz.
  for (double hz : {60.0, 59.8261, 59.4, 60.5}) {
    const Result r = run(1024, hz, 60 * 300);
    std::printf("1024-sample chunks at %.2f Hz: takes %zu..%zu, level up to %zu, %u underruns, %zu gaps\n", hz, r.min_take, r.max_take, r.max_level, r.underruns, r.gaps);
    CHECK(r.underruns == 0);
    CHECK(r.gaps == 0);
    CHECK(r.min_take >= lo && r.max_take <= hi);
    CHECK(r.max_level < 1024 + 3 * PER_FRAME);   // latency stays bounded (~2.5 frames)
  }
  // SDL's capture: 512-sample callbacks.
  {
    const Result r = run(512, 60.0, 60 * 300);
    CHECK(r.underruns == 0 && r.gaps == 0 && r.min_take >= lo && r.max_take <= hi);
    CHECK(r.max_level < 512 + 3 * PER_FRAME);
  }
  // A host stall: dry for 10 frames, then the backlog at once. One underrun
  // while dry, the backlog past the reserve dropped, then steady again.
  {
    const Result r = run(1024, 60.0, 60 * 60, 600, 10);
    std::printf("stall: %u underruns, %zu gaps, level up to %zu after\n", r.underruns, r.gaps, r.max_level);
    CHECK(r.underruns == 1);
    CHECK(r.gaps <= 2);   // the dropped backlog
    CHECK(r.max_level < 1024 + 3 * PER_FRAME);
    CHECK(r.min_take >= lo && r.max_take <= hi);
  }
  // No capture at all: nothing comes out, ever.
  {
    ds::sdl::MicQueue q(PER_FRAME);
    std::vector<ds::s16> out(5);
    for (int f = 0; f < 100; ++f) { q.frame(nullptr, 0, out); CHECK(out.empty()); }
    CHECK(!q.primed());
  }
  std::puts("mic_queue: ok");
}
