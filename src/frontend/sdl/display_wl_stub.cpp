// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DmabufOut without Wayland (DSPERATE_WAYLAND=OFF): the build's SDL2 has no
// Wayland driver, so SDL_SysWMinfo carries no wl member and there is no
// compositor to hand buffers to. open() gives the answer the runtime checks
// would have given on such a device, and Display takes its other paths.
#include "display_wl.h"

#include <cstdio>

namespace ds::sdl {

bool DmabufOut::open(SDL_Window*, int, int, int) {
  std::fprintf(stderr, "dmabuf: built without Wayland support\n");
  return false;
}
void DmabufOut::close() {}
u32* DmabufOut::begin_frame() { return nullptr; }
void DmabufOut::end_frame() {}
void DmabufOut::on_release(void*, struct wl_buffer*) {}
bool DmabufOut::alloc_buf(Buf&) { return false; }
void DmabufOut::drop_buf(Buf&) {}

} // namespace ds::sdl
