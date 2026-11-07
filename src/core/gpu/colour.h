// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/kernels.h"

#include <string>

// Colour correction profiles: another screen's colours, applied by the output
// stage (kern::colour_correct) to every shown line, so every route to the
// panel sees them. Display capture, save states and the core's own pixels
// before the output stage are untouched.
namespace ds::gpu {

enum class ColourProfile : u8 {
  Off,
  Nds,       // DS Phat: hunterk's nds-color as modified by Pokefan531 (public domain), as ROCKNIX's lcd1x-nds-color
  NdsLite,   // DS Lite: the ds_lite tone of josegonzalez's minui-nintendo-dsperate-pak, after kleinrein/rgds-core (GPL-3.0-or-later)
  Count,
};

// "off" | "nds" | "nds_lite"; false (and Off) for anything else.
bool parse_colour_profile(const std::string& v, ColourProfile& out);
const char* colour_profile_name(ColourProfile p);

// The profile's tables. `p` must not be Off.
void build_colour_lut(ColourProfile p, kern::ColourLut& out);

// The double-precision model the tables approximate (tests): 6-bit channels in, 8-bit out.
void colour_model(ColourProfile p, const u32 in6[3], u8 out[3]);

} // namespace ds::gpu
