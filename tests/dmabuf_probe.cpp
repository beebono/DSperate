// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// dmabuf_probe [W H] [SOURCE...]: what each dma-buf source costs the scanout
// tiers, measured the way they use the buffers. Default sources: every
// /dev/dma_heap/*, udmabuf, drm (dumb buffers of the KMS driver); each is a
// DS_DMA_HEAP value and runs in its own child, since the allocator pins the
// first source that works. Per source, for a W x H x 4 panel buffer (default
// 1024x768, the RG DS Plus) and a 192 KB 1x frame (the RGA present's
// source):
//   alloc      the allocation itself, mmap, and the first write (page
//              faults, zeroing done by the kernel is already in alloc)
//   write      a warm memcpy of the frame into the mapping (the scanline
//              scaler's and RGA present's CPU writes)
//   sync       the DMA_BUF_IOCTL_SYNC pair around it (the cache flush)
//   read       the CPU reading the buffer back (PiP blend, overlay)
//   kms        PRIME import + ADDFB2 on /dev/dri/card0: can the display
//              controller scan it, and how long does the import take
//   pool       how many panel buffers the source gives before ENOMEM, up to
//              16 (the CMA pool's depth; unbounded sources stop at 16)
// Device-only, not a test.
#include "frontend/sdl/dmaheap.h"
#include "frontend/sdl/drm_uapi.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using ds::sdl::dmaheap::alloc;
using ds::sdl::dmaheap::sync_begin_write;
using ds::sdl::dmaheap::sync_end_write;
namespace drmu = ds::sdl::drmu;

namespace {

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Stats {
  std::vector<double> v;
  void add(double x) { v.push_back(x); }
  double pct(double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
  }
};

bool quiet_ok(int) { return true; }

struct Buf {
  int fd = -1; uint32_t* px = nullptr; size_t len = 0;
  ~Buf() { if (px) munmap(px, len); if (fd >= 0) close(fd); }
};

// alloc + mmap + first write, each timed; false if the source has nothing.
bool take(Buf& b, size_t len, double* t_alloc, double* t_map, double* t_first) {
  b.len = len;
  double t0 = now_ms();
  b.fd = alloc(len, quiet_ok, "dmabuf_probe");
  *t_alloc = now_ms() - t0;
  if (b.fd < 0) return false;
  t0 = now_ms();
  void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
  *t_map = now_ms() - t0;
  if (p == MAP_FAILED) { std::printf("  mmap: %s\n", std::strerror(errno)); return false; }
  b.px = static_cast<uint32_t*>(p);
  t0 = now_ms();
  sync_begin_write(b.fd);
  std::memset(b.px, 0, len);
  sync_end_write(b.fd);
  *t_first = now_ms() - t0;
  return true;
}

// PRIME import and a framebuffer on the KMS node: the scanout test. Needs no
// DRM master (ADDFB2 is allowed to any client), so it runs beside the compositor.
void kms(const Buf& b, uint32_t w, uint32_t h) {
  const int drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (drm < 0) { std::printf("  kms    : /dev/dri/card0: %s\n", std::strerror(errno)); return; }
  Stats imp, fb;
  const char* fail = nullptr;
  for (int i = 0; i < 20 && !fail; ++i) {
    double t0 = now_ms();
    drmu::prime_handle ph = {};
    ph.fd = b.fd;
    if (ioctl(drm, drmu::IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0) { fail = "PRIME_FD_TO_HANDLE"; break; }
    imp.add(now_ms() - t0);
    t0 = now_ms();
    drmu::mode_fb_cmd2 f = {};
    f.width = w; f.height = h; f.pixel_format = 0x34325258u;   // XR24
    f.handles[0] = ph.handle; f.pitches[0] = w * 4;
    const int r = ioctl(drm, drmu::IOCTL_MODE_ADDFB2, &f);
    const int err = errno;
    fb.add(now_ms() - t0);
    if (r == 0) { unsigned int id = f.fb_id; ioctl(drm, drmu::IOCTL_MODE_RMFB, &id); }
    drmu::gem_close gc = {}; gc.handle = ph.handle; ioctl(drm, drmu::IOCTL_GEM_CLOSE, &gc);
    if (r < 0) { errno = err; fail = "ADDFB2"; }
  }
  if (fail) std::printf("  kms    : %s: %s (the display cannot scan this source)\n", fail, std::strerror(errno));
  else std::printf("  kms    : import %.3f ms, ADDFB2 %.3f ms (median of 20); scanout OK\n", imp.pct(0.5), fb.pct(0.5));
  close(drm);
}

int child(uint32_t w, uint32_t h) {
  const size_t panel = static_cast<size_t>(w) * h * 4, frame = 256 * 192 * 4;
  std::printf("[%s]\n", std::getenv("DS_DMA_HEAP"));

  // alloc: 10 of each size, every one mapped and written once.
  for (size_t len : {panel, frame}) {
    Stats a, m, f;
    for (int i = 0; i < 10; ++i) {
      Buf b; double ta, tm, tf;
      if (!take(b, len, &ta, &tm, &tf)) { std::printf("  alloc  : none (%s)\n", std::strerror(errno)); return 1; }
      a.add(ta); m.add(tm); f.add(tf);
    }
    std::printf("  alloc  : %7zu KB: alloc %.3f ms (max %.3f), mmap %.3f, first write %.3f ms\n",
                len / 1024, a.pct(0.5), a.pct(1.0), m.pct(0.5), f.pct(0.5));
  }
  std::printf("  source : %s\n", ds::sdl::dmaheap::chosen());

  Buf b; double ta, tm, tf;
  if (!take(b, panel, &ta, &tm, &tf)) return 1;
  std::vector<uint32_t> pattern(panel / 4);
  uint32_t seed = 0x12345678u;
  for (uint32_t& p : pattern) { seed = seed * 1664525u + 1013904223u; p = 0xFF000000u | (seed >> 8); }

  // write + sync: as the scanline scaler does it (begin, write the frame, end).
  {
    Stats beg, cpy, end, raw;
    for (int i = 0; i < 60; ++i) {
      double t0 = now_ms();
      sync_begin_write(b.fd);
      const double t1 = now_ms();
      std::memcpy(b.px, pattern.data(), panel);
      const double t2 = now_ms();
      sync_end_write(b.fd);
      const double t3 = now_ms();
      beg.add(t1 - t0); cpy.add(t2 - t1); end.add(t3 - t2);
      t0 = now_ms();
      std::memcpy(b.px, pattern.data(), panel);   // the same write with no sync at all
      raw.add(now_ms() - t0);
    }
    std::printf("  write  : memcpy %.3f ms (%.0f MB/s) within sync; sync begin %.3f ms, end %.3f ms; memcpy unsynced %.3f ms\n",
                cpy.pct(0.5), static_cast<double>(panel) / 1048576.0 / (cpy.pct(0.5) / 1000.0), beg.pct(0.5), end.pct(0.5), raw.pct(0.5));
  }
  // read: the CPU over the whole buffer (an overlay or PiP blend reads what it covers).
  {
    Stats rd; volatile uint32_t sink = 0;
    for (int i = 0; i < 30; ++i) {
      const double t0 = now_ms();
      uint32_t acc = 0;
      for (size_t k = 0; k < panel / 4; k += 4) acc += b.px[k];
      sink = acc;
      rd.add(now_ms() - t0);
    }
    (void)sink;
    std::printf("  read   : every 4th word %.3f ms (%.0f MB/s effective over the buffer)\n", rd.pct(0.5), static_cast<double>(panel) / 1048576.0 / (rd.pct(0.5) / 1000.0));
  }
  kms(b, w, h);

  // pool: panel buffers until the source says no (16 at most: 48 MiB at 1024x768).
  {
    std::vector<int> fds;
    int err = 0;
    for (int i = 0; i < 16; ++i) {
      const int fd = alloc(panel, quiet_ok, "dmabuf_probe");
      if (fd < 0) { err = errno; break; }
      fds.push_back(fd);
    }
    if (fds.size() == 16) std::printf("  pool   : 16+ panel buffers (%zu MiB) without refusal\n", 16 * panel / 1048576);
    else std::printf("  pool   : %zu panel buffers (%zu MiB), then %s\n", fds.size(), fds.size() * panel / 1048576, std::strerror(err));
    for (int fd : fds) close(fd);
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int ai = 1;
  uint32_t w = 1024, h = 768;
  if (argc > 2 && std::atoi(argv[1]) > 0 && std::atoi(argv[2]) > 0) { w = static_cast<uint32_t>(std::atoi(argv[1])); h = static_cast<uint32_t>(std::atoi(argv[2])); ai = 3; }
  std::vector<std::string> sources(argv + ai, argv + argc);
  if (sources.empty()) {
    if (DIR* d = opendir("/dev/dma_heap")) {
      while (dirent* e = readdir(d)) if (e->d_name[0] != '.') sources.emplace_back(e->d_name);
      closedir(d);
      std::sort(sources.begin(), sources.end());
    }
    if (access("/dev/udmabuf", F_OK) == 0) sources.emplace_back("udmabuf");
    if (access("/dev/ion", F_OK) == 0) sources.emplace_back("ion");
    if (access("/dev/dri", F_OK) == 0) sources.emplace_back("drm");
  }
  std::printf("dmabuf_probe: %ux%u panel buffer (%zu KB), %zu sources\n", w, h, static_cast<size_t>(w) * h * 4 / 1024, sources.size());
  int rc = 0;
  for (const std::string& s : sources) {
    const pid_t pid = fork();
    if (pid == 0) { setenv("DS_DMA_HEAP", s.c_str(), 1); _exit(child(w, h)); }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) rc = 1;
  }
  return rc;
}
