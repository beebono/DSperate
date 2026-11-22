// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What melonDS's DSP HLE (DSP_HLE/) asks of melonDS's Platform layer: a log,
// and an AAC decoder for the AAC program (the interface is melonDS's,
// src/Platform.h; implemented in aac_decoder.cpp).
#pragma once
#include "types.h"

namespace melonDS::Platform {

enum LogLevel { Debug, Info, Warn, Error };
// Shown only under DS_DSP_LOG=1: these are the HLE's own notes.
void Log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

struct AACDecoder;
AACDecoder* AAC_Init();                                    // null when the build has no decoder
void AAC_DeInit(AACDecoder* dec);
bool AAC_Configure(AACDecoder* dec, int frequency, int channels);
// One raw AAC frame (no ADTS/ADIF header) to 1024 samples of signed 16-bit PCM, interleaved.
bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen);

} // namespace melonDS::Platform
