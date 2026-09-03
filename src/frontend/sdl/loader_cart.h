// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::sdl {

// The loader cart, built in memory rather than read from a file: the card the
// DS menu draws when the frontend boots the firmware with no game, and whose
// launch raises the game list. tools/mkcart.py builds the same cart on disk --
// this is that script's build() ported, with the icon generated out of it into
// loader_icon.h -- so a BootMenu.nds made by hand still overrides this one and
// nothing downstream can tell the two apart.
//
// `title` is the first banner line and `subtitle` the second (empty for one
// line); both are UTF-8 and go into all six language slots. 128 KB.
std::vector<u8> build_loader_cart(const std::string& title, const std::string& subtitle);

} // namespace ds::sdl
