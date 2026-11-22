// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The AAC decoder the DSP's high-level AAC program is run with (melonDS's
// Platform interface, src/core/dsp/Platform.h): FAAD2, vendored in
// src/core/dsp/faad2 as an AAC-LC-only build. Adapted from melonDS's
// frontend/qt_sdl/Platform_AAC.cpp (GPLv3, melonDS team).
//
// Code from FAAD2 is copyright (c) Nero AG, www.nero.com
#include "core/dsp/Platform.h"
#include "core/dsp/faad2/include/neaacdec.h"

namespace melonDS::Platform {

struct AACDecoder {
  bool inited = false;
  NeAACDecHandle handle = nullptr;
};

AACDecoder* AAC_Init() { return new AACDecoder(); }

void AAC_DeInit(AACDecoder* dec) {
  if (!dec) return;
  if (dec->handle) NeAACDecClose(dec->handle);
  delete dec;
}

bool AAC_Configure(AACDecoder* dec, int frequency, int channels) {
  if (!dec) return false;
  if (dec->handle) { NeAACDecClose(dec->handle); dec->handle = nullptr; dec->inited = false; }
  dec->handle = NeAACDecOpen();
  if (!dec->handle) return false;
  NeAACDecConfiguration* cfg = NeAACDecGetCurrentConfiguration(dec->handle);
  cfg->defObjectType = LC;
  cfg->outputFormat = FAAD_FMT_16BIT;
  if (!NeAACDecSetConfiguration(dec->handle, cfg)) return false;

  // An MP4 AudioSpecificConfig for AAC-LC at this rate and channel count: the
  // program hands over bare frames, and this is how the decoder is told the rest.
  static const int rates[9] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
  u8 rate_index = 3;   // 48000 where the rate is not one of these
  for (int i = 0; i < 9; ++i) if (frequency == rates[i]) { rate_index = static_cast<u8>(3 + i); break; }
  channels &= 0xF;
  u8 asc[5] = {static_cast<u8>(0x10 | (rate_index >> 1)), static_cast<u8>((rate_index << 7) | (channels << 3)), 0x56, 0xE5, 0x00};
  unsigned long rate_out = 0;
  unsigned char channels_out = 0;
  if (NeAACDecInit2(dec->handle, asc, sizeof asc, &rate_out, &channels_out) != 0) return false;
  dec->inited = true;
  return true;
}

bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen) {
  if (!dec || !dec->inited || inputlen <= 0) return false;
  NeAACDecFrameInfo info;
  NeAACDecDecode2(dec->handle, &info, static_cast<unsigned char*>(const_cast<void*>(input)), static_cast<unsigned long>(inputlen), &output,
                  static_cast<unsigned long>(outputlen));
  return !info.error && info.bytesconsumed == static_cast<unsigned long>(inputlen);
}

} // namespace melonDS::Platform
