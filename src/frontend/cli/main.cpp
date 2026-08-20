// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Headless CLI: runs N frames and exits. The SDL frontend comes later.
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  const char* rom = nullptr;
  int frames = 60;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
    else rom = argv[i];
  }
  std::printf("DSperate 0.0.1 (%s%s)\n",
#if DSPERATE_JIT
              "jit",
#else
              "interp",
#endif
#if DSPERATE_NEON
              "+neon");
#else
              "");
#endif
  ds::NDS nds;
  if (rom && !nds.load_rom(rom)) {
    std::fprintf(stderr, "could not read %s\n", rom);
    return 1;
  }
  for (int i = 0; i < frames; ++i) nds.run_frame();
  std::printf("ran %llu frames, %llu cycles\n",
              static_cast<unsigned long long>(nds.frame_count),
              static_cast<unsigned long long>(nds.sched.now()));
  return 0;
}
