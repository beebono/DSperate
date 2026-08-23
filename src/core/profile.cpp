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
const char* const count_names[C_COUNT] = {"3d polygon lines", "3d span pixels", "3d resolved pixels",
  "3d texel gathers (cached/direct)", "3d texel gathers (compressed)", "3d texel gathers (via views)",
  "3d texcache validated", "3d texcache decoded", "3d texcache bytes compared", "3d frames kept (no new swap)",
  "slices", "slices arm9 halted", "slices arm7 halted", "slices both halted", "slices with dma", "slices run to the deadline (both halted)",
  "2d lines rendered", "2d text bg lines", "2d affine bg lines", "2d extended bg lines", "2d 3d-layer lines", "2d lines with sprites", "2d lines with windows", "2d lines with colour effect", "2d lines where an effect can apply", "2d flat lines (no effect possible)", "2d plane selects",
  "2d lines: 0 layers", "2d lines: 1 layer", "2d lines: 2 layers", "2d lines: 3 layers", "2d lines: 4+ layers", "2d lines: 1 layer, fully opaque", "2d lines with obj pixels", "2d lines with 3d pixels", "2d lines with a window", "2d bg lines 16-colour text", "2d bg lines 256-colour text", "2d bg lines direct colour", "2d bg lines empty (transparent row)", "2d fast lines: backdrop only", "2d fast lines: one opaque layer", "2d full lines: effect mode live", "2d full lines: translucent 3d", "2d full lines: semi/bitmap sprites"};

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
