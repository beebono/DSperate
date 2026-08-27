// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The per-frame host time report both frontends print at the end of a run.
// It lives here, and not in either main.cpp, so the two cannot drift: the
// whole point of the numbers is that a CLI replay and an SDL replay of the
// same scene can be put side by side.
//
// What goes in the series matters as much as the statistics. It must be
// emulation work only -- frame pacing (vsync in the present, or the audio
// queue) must be left out, or the tail stops measuring the emulator and
// starts measuring the display refresh, and every frame pins to 16.7 ms.
#pragma once

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ds {

// A DS frame is 1/59.8261 s. A frame that takes longer than that is one the
// emulator could not deliver in real time -- which is what a dropped frame
// and an audio underrun actually are. The mean hides these completely: a
// change can improve the mean while making the tail worse, and the tail is
// what is felt. Report both.
inline constexpr double kFrameBudgetMs = 1000.0 / 59.8261;

// `label` names the series: the default "frame" is the emulation-only slice
// described above. The SDL frontend also prints a "work" series (emulation +
// present, pacing still excluded) because a change can move cost between the
// two slices -- per-scanline scaling deliberately does -- and then neither
// slice alone can say what happened to the over-budget clusters. The work
// series is only meaningful with the present unblocked (--no-vsync);
// with vsync on its tail measures the display refresh, as above.
inline void frame_report(const std::vector<double>& frame_ms, const char* label = "frame") {
  if (frame_ms.empty()) return;
  std::vector<double> v = frame_ms;
  std::sort(v.begin(), v.end());
  double sum = 0; for (double x : v) sum += x;
  const size_t n = v.size();
  auto pct = [&](double p) { return v[std::min(n - 1, static_cast<size_t>(p * n))]; };
  size_t over = 0; for (double x : v) if (x > kFrameBudgetMs) ++over;
  std::fprintf(stderr, "%s ms: median %.3f mean %.3f p90 %.3f p99 %.3f max %.3f min %.3f total %.1f\n",
               label, v[n / 2], sum / n, pct(0.90), pct(0.99), v.back(), v.front(), sum);
  std::fprintf(stderr, "%s budget: %zu of %zu frames over %.3f ms (%.2f%%)\n",
               label, over, n, kFrameBudgetMs, 100.0 * static_cast<double>(over) / static_cast<double>(n));

  // Where the overruns are, not just how many. A count says the run stutters;
  // this says which frames to re-run with --dump-from/--dump-count and look
  // at. `frame_ms` is in run order, unlike the sorted copy above.
  if (!over) return;
  size_t worst_i = 0, bursts = 0, longest = 0, longest_at = 0, cur = 0, cur_at = 0;
  for (size_t i = 0; i < frame_ms.size(); ++i) {
    if (frame_ms[i] > frame_ms[worst_i]) worst_i = i;
    if (frame_ms[i] > kFrameBudgetMs) {
      if (cur == 0) { ++bursts; cur_at = i; }
      if (++cur > longest) { longest = cur; longest_at = cur_at; }
    } else cur = 0;
  }
  std::fprintf(stderr, "  worst frame #%zu at %.3f ms; %zu bursts, longest %zu frames from #%zu\n",
               worst_i, frame_ms[worst_i], bursts, longest, longest_at);
  // Ten windows over the run: a flat row is steady load, a spike is a
  // section that chugs and is worth dumping.
  constexpr size_t W = 10;
  const size_t span = (frame_ms.size() + W - 1) / W;
  std::fprintf(stderr, "  over-budget per %zu-frame window:", span);
  for (size_t w = 0; w < W; ++w) {
    size_t c = 0;
    for (size_t i = w * span; i < std::min(frame_ms.size(), (w + 1) * span); ++i)
      if (frame_ms[i] > kFrameBudgetMs) ++c;
    std::fprintf(stderr, " %zu", c);
  }
  std::fputc('\n', stderr);
}

}  // namespace ds
