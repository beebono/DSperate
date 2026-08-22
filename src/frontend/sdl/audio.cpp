// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "core/nds.h"

#include <cstdio>

namespace ds::sdl {

bool Audio::open() {
  SDL_AudioSpec want{}, got{};
  want.freq = static_cast<int>(spu::Spu::SAMPLE_RATE);
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;      // queue granularity only; there is no callback
  want.callback = nullptr;
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);   // no changes allowed: SDL converts
  if (!dev_) { std::fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError()); return false; }
  frame_bytes_ = (spu::Spu::SAMPLE_RATE * 4) / 60;
  SDL_PauseAudioDevice(dev_, 0);
  std::fprintf(stderr, "audio: %d Hz, %d channels, %u-sample buffer\n", got.freq, got.channels, got.samples);
  return true;
}

void Audio::close() {
  if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
}

void Audio::push(NDS& nds) {
  s16 buf[2048 * 2];
  size_t n;
  while ((n = nds.spu.take(buf, 2048)) != 0) {
    if (dev_) SDL_QueueAudio(dev_, buf, static_cast<u32>(n * 4));
  }
}

double Audio::queued_frames() const {
  return dev_ ? static_cast<double>(SDL_GetQueuedAudioSize(dev_)) / frame_bytes_ : 0.0;
}

void Audio::pace() {
  if (!dev_) return;
  // Above the target the emulator is ahead of the speakers: wait. Below it the
  // machine cannot keep up and we let it run flat out. The wait is bounded so
  // an audio device that accepts samples but never plays them (a broken
  // PipeWire session, say) slows the emulator down instead of hanging it.
  const u32 limit = frame_bytes_ * TARGET_FRAMES;
  const Uint32 deadline = SDL_GetTicks() + 100;
  while (SDL_GetQueuedAudioSize(dev_) > limit) {
    if (SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) {
      if (SDL_GetQueuedAudioSize(dev_) > frame_bytes_ * STALLED_FRAMES) {
        if (!stalled_) std::fprintf(stderr, "audio: device is not consuming samples; dropping the backlog\n");
        stalled_ = true;
        SDL_ClearQueuedAudio(dev_);
      }
      return;
    }
    SDL_Delay(1);
  }
  stalled_ = false;
}

} // namespace ds::sdl
