// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// Mali hardware counters for the raster probes (vk_probe, gles_probe), read
// through Arm's HWCPipe (MIT) from /dev/mali0. Built only when CMake is given
// DSPERATE_HWCPIPE_DIR (a HWCPipe checkout, e.g. ../toolchains/hwcpipe); at
// run time DS_PROBE_HWC=1 switches it on.
//
// One sample spans the whole timed loop, during which the probe submits
// each frame onto an idle GPU and waits for it, so the counts are the
// frames' own. Plain counts are printed per frame; derived ratios and
// rates (HWCPipe's float counters) as they come.
#include <cstdio>
#include <cstdlib>

#ifdef DSPERATE_HWCPIPE
#include <hwcpipe/counter_database.hpp>
#include <hwcpipe/gpu.hpp>
#include <hwcpipe/sampler.hpp>

#include <memory>
#include <string>
#include <vector>

class HwcProbe {
public:
  HwcProbe() {
    const char* e = std::getenv("DS_PROBE_HWC");
    if (!e || !std::atoi(e)) return;
    gpu_ = std::make_unique<hwcpipe::gpu>(0);
    if (!*gpu_) { std::fprintf(stderr, "hwc: no Mali GPU at /dev/mali0\n"); return; }
    hwcpipe::sampler_config cfg(*gpu_);
    for (hwcpipe_counter c : db_.counters_for_gpu(*gpu_)) if (!cfg.add_counter(c)) counters_.push_back(c);
    sampler_ = std::make_unique<hwcpipe::sampler<>>(cfg);
    if (const std::error_code ec = sampler_->start_sampling()) { std::fprintf(stderr, "hwc: %s\n", ec.message().c_str()); sampler_.reset(); }
  }
  ~HwcProbe() { if (sampler_) (void)sampler_->stop_sampling(); }
  // Around the timed loop: begin() resets the counts, end() reads them.
  void begin() { if (sampler_) (void)sampler_->sample_now(); }
  void end(int frames) {
    if (!sampler_) return;
    if (const std::error_code ec = sampler_->sample_now()) { std::fprintf(stderr, "hwc: %s\n", ec.message().c_str()); return; }
    std::printf("hwc (%zu counters, per frame over %d frames):\n", counters_.size(), frames);
    for (hwcpipe_counter c : counters_) {
      hwcpipe::counter_sample s; hwcpipe::counter_metadata m{};
      if (sampler_->get_counter_value(c, s) || db_.describe_counter(c, m)) continue;
      if (s.type == hwcpipe::counter_sample::type::uint64) std::printf("  %-60s %16.1f %s\n", m.name, static_cast<double>(s.value.uint64) / frames, m.units);
      else {
        // HWCPipe's derived counters: ratios (percent, per thread/pixel, averages) as they are, sums per frame.
        const std::string n = m.name, u = m.units;
        const bool ratio = u == "percent" || n.find(" per ") != std::string::npos || n.find("Average") == 0 || n.find("utilization") != std::string::npos || n.find("percentage") != std::string::npos;
        std::printf("  %-60s %16.4f %s%s\n", m.name, ratio ? s.value.float64 : s.value.float64 / frames, m.units, ratio ? " (ratio)" : "");
      }
    }
  }
private:
  hwcpipe::counter_database db_;
  std::unique_ptr<hwcpipe::gpu> gpu_;
  std::unique_ptr<hwcpipe::sampler<>> sampler_;
  std::vector<hwcpipe_counter> counters_;
};
#else
class HwcProbe {
public:
  HwcProbe() { const char* e = std::getenv("DS_PROBE_HWC"); if (e && std::atoi(e)) std::fprintf(stderr, "hwc: not built (configure with DSPERATE_HWCPIPE_DIR)\n"); }
  void begin() {}
  void end(int) {}
};
#endif
