// SPDX-License-Identifier: GPL-3.0-or-later
#include "mic_alsa.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#ifdef __linux__
#include <dlfcn.h>
#endif

namespace ds::sdl {

bool MicAlsa::open(u32 rate) {
#ifdef __linux__
  const char* dev = std::getenv("DS_MIC_DEV");
  if (dev && !*dev) return false;               // DS_MIC_DEV= (empty): SDL's path
  if (!dev) dev = "plughw:0,0";
  lib_ = dlopen("libasound.so.2", RTLD_NOW);
  if (!lib_) return false;
  auto sym = [&](const char* n) { return dlsym(lib_, n); };
  auto pcm_open = reinterpret_cast<int (*)(void**, const char*, int, int)>(sym("snd_pcm_open"));
  auto set_params = reinterpret_cast<int (*)(void*, int, int, unsigned, unsigned, int, unsigned)>(sym("snd_pcm_set_params"));
  auto strerr = reinterpret_cast<const char* (*)(int)>(sym("snd_strerror"));
  readi_ = reinterpret_cast<long (*)(void*, void*, unsigned long)>(sym("snd_pcm_readi"));
  recover_ = reinterpret_cast<int (*)(void*, int, int)>(sym("snd_pcm_recover"));
  close_ = reinterpret_cast<int (*)(void*)>(sym("snd_pcm_close"));
  if (!pcm_open || !set_params || !readi_ || !recover_ || !close_) { close(); return false; }
  // SND_PCM_STREAM_CAPTURE = 1, SND_PCM_NONBLOCK = 1; S16_LE = 2, RW_INTERLEAVED = 3.
  int err = pcm_open(&pcm_, dev, 1, 1);
  if (err < 0) { std::fprintf(stderr, "mic: alsa %s: %s\n", dev, strerr ? strerr(err) : "?"); pcm_ = nullptr; close(); return false; }
  err = set_params(pcm_, 2, 3, 1, rate, 1, 100000);   // mono, soft resample allowed, 100 ms buffer
  if (err < 0) { std::fprintf(stderr, "mic: alsa %s: %s\n", dev, strerr ? strerr(err) : "?"); close(); return false; }
  std::fprintf(stderr, "mic: alsa %s\n", dev);
  return true;
#else
  (void)rate;
  return false;
#endif
}

void MicAlsa::close() {
#ifdef __linux__
  if (pcm_ && close_) close_(pcm_);
  pcm_ = nullptr;
  if (lib_) { dlclose(lib_); lib_ = nullptr; }
#endif
}

void MicAlsa::capture(std::vector<s16>& out) {
  out.clear();
  if (!pcm_) return;
  s16 buf[1024];
  for (int rounds = 0; rounds < 8; ++rounds) {     // at most ~a quarter second: a backlog is dropped by the ring
    long n = readi_(pcm_, buf, 1024);
    if (n == -EAGAIN) break;
    if (n < 0) { if (recover_(pcm_, static_cast<int>(n), 1) < 0) break; continue; }
    if (n == 0) break;
    out.insert(out.end(), buf, buf + n);
  }
}

} // namespace ds::sdl
