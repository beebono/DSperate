// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// DSperate's own DSi system font (io/dsi_font/README.md), linked in whole.
#include "core/io/dsi_nand_synth.h"

#include <cstring>

// The path comes from CMake (DSPERATE_DSI_FONT_FILE), absolute.
__asm__(
  "\t.section .rodata.dsperate_dsi_font,\"a\",%progbits\n"
  "\t.balign 4\n"
  "\t.globl dsperate_dsi_font_begin\n"
  "dsperate_dsi_font_begin:\n"
  "\t.incbin \"" DSPERATE_DSI_FONT_FILE "\"\n"
  "\t.globl dsperate_dsi_font_end\n"
  "dsperate_dsi_font_end:\n"
  "\t.previous\n");
extern "C" const ds::u8 dsperate_dsi_font_begin[];
extern "C" const ds::u8 dsperate_dsi_font_end[];

namespace ds::io {

std::vector<u8> builtin_dsi_font() {
  return std::vector<u8>(dsperate_dsi_font_begin, dsperate_dsi_font_end);
}

bool is_builtin_font_signature(const u8 sig[0x80]) {
  return dsperate_dsi_font_end - dsperate_dsi_font_begin >= 0xA0 && std::memcmp(sig, dsperate_dsi_font_begin, 0x80) == 0;
}

}  // namespace ds::io
