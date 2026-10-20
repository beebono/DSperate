// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core/types.h"

namespace ds::frontend {

// Resize a 256x192 frame to any positive size using nearest neighbours.
// Pitch is in u32s; no colour interpolation. Whole-number scales use NEON
// where available, while fractional scales also support shrinking insets.
void scale_nearest(const u32* src, u32* dst, u32 pitch, u32 width, u32 height);

} // namespace ds::frontend
