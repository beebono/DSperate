// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The AAC decoder the DSi DSP's AAC program is run with: bare AAC-LC frames in,
// 1024 samples of PCM a frame out. A 441 Hz tone comes back as a 441 Hz tone.
#include "core/dsp/Platform.h"
#include "core/types.h"
#include "check.h"

#include <cstdlib>
#include <vector>

#include "aac_tone.inc"

using namespace ds;
namespace P = melonDS::Platform;

int main() {
  P::AACDecoder* dec = P::AAC_Init();
  CHECK(dec != nullptr);
  s16 pcm[1024];
  CHECK(!P::AAC_DecodeFrame(dec, kAacTone + 2, kAacTone[0] | kAacTone[1] << 8, pcm, sizeof pcm));   // not configured yet
  CHECK(P::AAC_Configure(dec, 44100, 1));
  std::vector<s16> all;
  const unsigned char* p = kAacTone;
  for (int f = 0; f < kAacToneFrames; ++f) {
    const int n = p[0] | p[1] << 8;
    CHECK(P::AAC_DecodeFrame(dec, p + 2, n, pcm, sizeof pcm));
    all.insert(all.end(), pcm, pcm + 1024);
    p += 2 + n;
  }
  // Past the encoder's lead-in: a steady tone, 100 samples a cycle.
  const size_t from = 4096;
  CHECK(all.size() >= from + 2048);
  s32 peak = 0; int crossings = 0;
  for (size_t i = from; i < from + 2000; ++i) {
    if (std::abs(all[i]) > peak) peak = std::abs(all[i]);
    if (all[i - 1] < 0 && all[i] >= 0) ++crossings;
  }
  CHECK(peak > 2000);
  CHECK(crossings >= 19 && crossings <= 21);
  // Rubbish is refused, and the decoder goes on.
  const unsigned char junk[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!P::AAC_DecodeFrame(dec, junk, sizeof junk, pcm, sizeof pcm));
  CHECK(P::AAC_Configure(dec, 32000, 2));
  P::AAC_DeInit(dec);
  std::puts("dsp_aac: ok");
  return 0;
}
