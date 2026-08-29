// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "core/nds.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
  if (cap_) { SDL_CloseAudioDevice(cap_); cap_ = 0; }
}

bool Audio::open_capture() {
  if (SDL_GetNumAudioDevices(1) <= 0) { std::fprintf(stderr, "mic: no capture device\n"); return false; }
  SDL_AudioSpec want{}, got{};
  want.freq = static_cast<int>(spu::Spu::SAMPLE_RATE);
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  cap_ = SDL_OpenAudioDevice(nullptr, 1, &want, &got, 0);
  if (!cap_) { std::fprintf(stderr, "mic: %s (no microphone)\n", SDL_GetError()); return false; }
  SDL_PauseAudioDevice(cap_, 0);
  std::fprintf(stderr, "mic: %s\n", SDL_GetAudioDeviceName(0, 1) ? SDL_GetAudioDeviceName(0, 1) : "default");
  return true;
}

const std::vector<s16>& Audio::capture() {
  mic_.clear();
  if (!cap_) return mic_;
  const u32 per_frame = spu::Spu::SAMPLE_RATE / 60 + 1;
  u32 avail = SDL_GetQueuedAudioSize(cap_) / 2;
  if (avail > per_frame * 4) {                   // stale backlog (a stalled frame): keep the newest
    std::vector<s16> junk(avail - per_frame * 2);
    SDL_DequeueAudio(cap_, junk.data(), static_cast<u32>(junk.size() * 2));
    avail = per_frame * 2;
  }
  mic_.resize(avail);
  const u32 got = SDL_DequeueAudio(cap_, mic_.data(), avail * 2) / 2;
  mic_.resize(got);
  return mic_;
}

void Audio::push(NDS& nds, bool drop) {
  if (drop && (!dev_ || SDL_GetQueuedAudioSize(dev_) > frame_bytes_ * TARGET_FRAMES)) { nds.spu.drain(); return; }
  s16 buf[2048 * 2];
  size_t n;
  while ((n = nds.spu.take(buf, 2048)) != 0) {
    if (!dev_) continue;
    if (muted_) std::memset(buf, 0, n * 4);
    else if (volume_ != 100) {
      // Linear in amplitude; the SPU's own master volume is the game's.
      const int g = volume_ * 256 / 100;
      for (size_t i = 0; i < n * 2; ++i) buf[i] = static_cast<s16>((buf[i] * g) >> 8);
    }
    SDL_QueueAudio(dev_, buf, static_cast<u32>(n * 4));
  }
}

void Audio::set_volume(int percent) {
  volume_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

void Audio::pause(bool p) {
  if (!dev_) return;
  SDL_PauseAudioDevice(dev_, p ? 1 : 0);
  if (p) SDL_ClearQueuedAudio(dev_);
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
