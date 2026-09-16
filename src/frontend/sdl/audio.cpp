// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"
#include "core/nds.h"

#include <algorithm>
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
  dev_samples_ = got.samples;
  // A DS frame, not 1/60 s: the console runs at 59.8261 Hz, and this number
  // is what a frame of queue depth means to the controller.
  frame_bytes_ = static_cast<u32>(static_cast<u64>(rate_) * 4 * CYCLES_PER_FRAME / ARM9_CLOCK_HZ);
  prev_l_ = prev_r_ = 0; phase_ = 0;
  stats_ = Stats{};
  SDL_PauseAudioDevice(dev_, 0);
  // The buffer is reported in frames of audio as well as samples: it is the
  // floor under any queue target, and "2048 samples" does not say that "2.55
  // DS frames" does. The input rate is not named here -- everything goes
  // through the resampler now, and a DSi title can move the SPU to 47605 Hz
  // mid-session, so the rate the sound is coming *from* is a live number and
  // belongs in the statistics line rather than in a message printed once.
  std::fprintf(stderr, "audio: %s driver, %d Hz, %d channels, %u-sample buffer (%.2f frames)\n",
               SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
               got.freq, got.channels, got.samples, device_buffer_frames());
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
  // The SPU's rate is not a constant: a DSi title can select 47.6 kHz
  // through SNDEXCNT mid-session. Re-reading it here costs nothing and
  // keeps the resampler honest when it changes. The exact rate, not the
  // nominal 32768 -- see Spu::output_rate_hz().
  in_rate_ = nds.spu.output_rate_hz();

  // Measure before queueing, so the depth is what the device has left to
  // play rather than what it has plus this frame.
  if (dev_) {
    const u32 queued = SDL_GetQueuedAudioSize(dev_);
    const double now = static_cast<double>(queued) / frame_bytes_;
    depth_ = depth_ < 0 ? now : depth_ + (now - depth_) * DRC_SMOOTH;
    if (now > target_frames_ + MAX_FRAMES) over_ = true;
    else if (now <= target_frames_) over_ = false;
    ++stats_.frames;
    if (queued == 0) ++stats_.dry;
    if (now < 0.5) ++stats_.under_half;
    if (now < 1.0) ++stats_.under_one;
    if (now < stats_.min_depth) stats_.min_depth = now;
    if (now > stats_.max_depth) stats_.max_depth = now;
  }

  // Fast forward outruns the speakers whatever the rate is, and a queue that
  // is not being consumed cannot be steered: both drop whole frames. Neither
  // teaches the controller anything, so its state is left where it is.
  if (over_ || (drop && (!dev_ || SDL_GetQueuedAudioSize(dev_) > frame_bytes_ * target_frames_))) {   // target_frames_ is fractional; the comparison promotes
    ++stats_.dropped;
    nds.spu.drain();
    return;
  }

  // Steer the queue back to the target. Positive trim plays out faster than
  // nominal, which drains a deep queue; negative fills a shallow one.
  if (dev_ && depth_ >= 0) {
    const double err = depth_ - target_frames_;
    trim_ = err * DRC_GAIN;
    if (trim_ > DRC_CLAMP) trim_ = DRC_CLAMP;
    else if (trim_ < -DRC_CLAMP) trim_ = -DRC_CLAMP;
  }

  s16 buf[2048 * 2];
  size_t n;
  while ((n = nds.spu.take(buf, 2048)) != 0) {
    if (!dev_) continue;
    // Always through the resampler, even when the rates match: the trim is
    // applied here, so a bypass would be a path with no rate control on it.
    // The step is input frames per output frame, 16.16 -- a larger step
    // emits fewer output frames from the same input, which is what draining
    // a deep queue means.
    const double ratio = in_rate_ / rate_ * speed_ * (1.0 + trim_);
    const u32 step = static_cast<u32>(ratio * 65536.0 + 0.5);
    out_.resize((static_cast<size_t>(n / ratio) + 2) * 2);
    size_t m = 0;
    for (size_t i = 0; i < n; ++i) {
      const s16 cl = buf[i * 2], cr = buf[i * 2 + 1];
      while (phase_ < 0x10000) {
        const u32 f = phase_;
        if ((m + 1) * 2 > out_.size()) out_.resize(out_.size() * 2);
        out_[m * 2]     = static_cast<s16>(prev_l_ + (((cl - prev_l_) * static_cast<s32>(f)) >> 16));
        out_[m * 2 + 1] = static_cast<s16>(prev_r_ + (((cr - prev_r_) * static_cast<s32>(f)) >> 16));
        ++m;
        phase_ += step;
      }
      phase_ -= 0x10000;
      prev_l_ = cl; prev_r_ = cr;
    }
    s16* out = out_.data();
    if (muted_) std::memset(out, 0, m * 4);
    else if (volume_ != 100) {
      // Linear in amplitude; the SPU's own master volume is the game's.
      const int g = volume_ * 256 / 100;
      for (size_t i = 0; i < m * 2; ++i) out[i] = static_cast<s16>((out[i] * g) >> 8);
    }
    SDL_QueueAudio(dev_, out, static_cast<u32>(m * 4));
  }
}

void Audio::set_speed(double factor) {
  speed_ = factor > 0.05 ? (factor < 20.0 ? factor : 20.0) : 0.05;
}

void Audio::set_buffer_ms(double ms) {
  if (!(ms > 0.0)) ms = DEFAULT_MS;      // unset, unparseable, or a NaN out of atof
  target_frames_ = std::clamp(ms, MIN_MS, MAX_MS) / FRAME_MS;
  depth_ = -1.0;      // the target moved: measure again rather than chase the old error
  trim_ = 0.0;
}

void Audio::set_volume(int percent) {
  volume_ = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

void Audio::pause(bool p) {
  if (!dev_) return;
  SDL_PauseAudioDevice(dev_, p ? 1 : 0);
  // The queue is gone, so what the controller had learned about it is too.
  if (p) { SDL_ClearQueuedAudio(dev_); over_ = false; depth_ = -1.0; trim_ = 0.0; }
}

double Audio::queued_frames() const {
  return dev_ ? static_cast<double>(SDL_GetQueuedAudioSize(dev_)) / frame_bytes_ : 0.0;
}

double Audio::device_buffer_frames() const {
  return frame_bytes_ ? static_cast<double>(dev_samples_) * 4 / frame_bytes_ : 0.0;
}

} // namespace ds::sdl
