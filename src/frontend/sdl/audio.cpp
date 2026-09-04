// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "core/nds.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {

bool Audio::open(bool native_rate) {
  SDL_AudioSpec want{}, got{};
  want.freq = static_cast<int>(spu::Spu::SAMPLE_RATE);
  if (native_rate) {
    // The device's own rate, asked for explicitly: a daemon-backed device
    // accepts any rate and converts, so "allow a change" alone never
    // changes anything. SDL 2.24 can ask the default device; before that
    // 48 kHz is what every such daemon runs at.
    int freq = 48000;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    // Only the daemon backends implement the query; SDL 2.30's ALSA backend
    // crashes inside it (RG DS, 2026-09-04) rather than failing.
    const char* drv = SDL_GetCurrentAudioDriver();
    if (drv && (std::strcmp(drv, "pipewire") == 0 || std::strcmp(drv, "pulseaudio") == 0)) {
      SDL_AudioSpec def{};
      if (SDL_GetDefaultAudioInfo(nullptr, &def, 0) == 0 && def.freq > 0) freq = def.freq;
    }
#endif
    want.freq = freq;
  }
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 2048;      // queue granularity only (no callback); fewer, larger device writes
  want.callback = nullptr;
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!dev_) { std::fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError()); return false; }
  rate_ = got.freq > 0 ? static_cast<u32>(got.freq) : spu::Spu::SAMPLE_RATE;
  frame_bytes_ = (rate_ * 4) / 60;
  prev_l_ = prev_r_ = 0; phase_ = 0;
  SDL_PauseAudioDevice(dev_, 0);
  std::fprintf(stderr, "audio: %s driver, %d Hz, %d channels, %u-sample buffer%s\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
               got.freq, got.channels, got.samples, rate_ != spu::Spu::SAMPLE_RATE ? " (resampled from 32768 Hz here)" : "");
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
    s16* out = buf;
    size_t m = n;
    if (rate_ != spu::Spu::SAMPLE_RATE) {
      // Linear interpolation between consecutive input frames; the phase
      // advances by in/out per output frame, so the rates need share no
      // factor. Sized for any output rate up to 8x the input.
      const u32 step = static_cast<u32>((static_cast<u64>(spu::Spu::SAMPLE_RATE) << 16) / rate_);
      out_.resize((n * rate_ / spu::Spu::SAMPLE_RATE + 2) * 2);
      m = 0;
      for (size_t i = 0; i < n; ++i) {
        const s16 cl = buf[i * 2], cr = buf[i * 2 + 1];
        while (phase_ < 0x10000) {
          const u32 f = phase_;
          out_[m * 2]     = static_cast<s16>(prev_l_ + (((cl - prev_l_) * static_cast<s32>(f)) >> 16));
          out_[m * 2 + 1] = static_cast<s16>(prev_r_ + (((cr - prev_r_) * static_cast<s32>(f)) >> 16));
          ++m;
          phase_ += step;
        }
        phase_ -= 0x10000;
        prev_l_ = cl; prev_r_ = cr;
      }
      out = out_.data();
    }
    if (muted_) std::memset(out, 0, m * 4);
    else if (volume_ != 100) {
      // Linear in amplitude; the SPU's own master volume is the game's.
      const int g = volume_ * 256 / 100;
      for (size_t i = 0; i < m * 2; ++i) out[i] = static_cast<s16>((out[i] * g) >> 8);
    }
    SDL_QueueAudio(dev_, out, static_cast<u32>(m * 4));
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
  if (stalled_) {
    // A device in this state would otherwise cost the full probe below on
    // every single frame -- ~100 ms, which is the difference between 60 fps
    // and 10. Drop what has piled up and return at once; the caller paces on
    // the wall clock (Audio::stalled()) until a periodic probe finds the
    // device consuming again.
    if (!SDL_TICKS_PASSED(SDL_GetTicks(), stall_mark_ + STALL_RETRY_MS)) { SDL_ClearQueuedAudio(dev_); return; }
    // Probe time: let the queue stand and fall through. If the device is
    // still dead it takes a few frames to build the backlog again and one
    // 100 ms probe to re-latch -- ~2 % of the time, not all of it.
    stalled_ = false;
  }
  const Uint32 deadline = SDL_GetTicks() + 100;
  while (SDL_GetQueuedAudioSize(dev_) > limit) {
    if (SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) {
      if (SDL_GetQueuedAudioSize(dev_) > frame_bytes_ * STALLED_FRAMES) {
        if (!announced_) std::fprintf(stderr, "audio: device is not consuming samples; pacing on the clock instead\n");
        announced_ = true;
        stalled_ = true;
        stall_mark_ = SDL_GetTicks();
        SDL_ClearQueuedAudio(dev_);
      }
      return;
    }
    SDL_Delay(1);
  }
  stalled_ = false;
  announced_ = false;
}

} // namespace ds::sdl
