// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/profile.h"

#include <cstdio>

namespace ds::prof {

bool enabled = false;
u64 ns[COUNT] = {};
const char* const names[COUNT] = {
  "cpu arm9", "cpu arm7", "dma", "gx geometry",
  "2d bg draw", "2d obj draw", "2d window", "2d select", "2d effects", "output", "capture",
  "3d clear", "3d spans", "3d final pass", "spu",
};

u64 count[C_COUNT] = {};
const char* const count_names[C_COUNT] = {"3d polygon lines", "3d span pixels", "3d resolved pixels"};

void report() {
  u64 total = 0;
  for (u64 v : ns) total += v;
  if (!total) return;
  for (u32 i = 0; i < C_COUNT; ++i)
    if (count[i]) std::fprintf(stderr, "[profile] %-20s %12llu\n", count_names[i], static_cast<unsigned long long>(count[i]));
  std::fprintf(stderr, "[profile] %-14s %9s %6s\n", "stage", "ms", "%");
  for (u32 i = 0; i < COUNT; ++i)
    if (ns[i]) std::fprintf(stderr, "[profile] %-14s %9.1f %5.1f%%\n", names[i], ns[i] / 1e6, 100.0 * ns[i] / total);
  std::fprintf(stderr, "[profile] %-14s %9.1f\n", "sum", total / 1e6);
}

} // namespace ds::prof
