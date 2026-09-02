// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Go/no-go for a KMS direct-scanout presentation tier, the KMSDRM counterpart
// of the Wayland dmabuf tier: can we allocate CMA dma-heap buffers, import
// them as DRM framebuffers, and page-flip them ourselves -- so the emulator
// writes scaled pixels straight into the scanout buffer and the "present" is
// one non-blocking ioctl instead of SDL's stretch-blit + texture upload +
// GLES draw + blocking swap?
//
// Raw DRM ioctls on purpose: the device has libdrm.so but no headers, and the
// UAPI structs are stable. Standalone, no libs:
//   aarch64-linux-gnu-g++ -O2 -o kms_scanout_probe tools/kms_scanout_probe.cpp
//
// Needs DRM master, so stop the compositor first (systemctl stop sway).
//   ./kms_scanout_probe --frames 300

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

struct dma_heap_allocation_data { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int fd_ = -1;
template <typename T> static int io(unsigned long req, T& arg) { return ioctl(fd_, req, &arg); }

struct Buf {
  int dmafd = -1; uint32_t handle = 0, fb = 0; uint32_t* px = nullptr; size_t bytes = 0;
};

static bool alloc_cma(Buf& b, uint32_t w, uint32_t h) {
  int heap = open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
  if (heap < 0) { perror("/dev/dma_heap/linux,cma"); return false; }
  dma_heap_allocation_data a = {};
  a.len = static_cast<uint64_t>(w) * h * 4;
  a.fd_flags = O_RDWR | O_CLOEXEC;
  int r = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a);
  close(heap);
  if (r < 0) { perror("CMA alloc"); return false; }
  b.dmafd = static_cast<int>(a.fd); b.bytes = a.len;
  void* m = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b.dmafd, 0);
  if (m == MAP_FAILED) { perror("mmap"); return false; }
  b.px = static_cast<uint32_t*>(m);
  memset(b.px, 0, b.bytes);

  drm_prime_handle ph = {}; ph.fd = b.dmafd;
  if (io(DRM_IOCTL_PRIME_FD_TO_HANDLE, ph) < 0) { perror("PRIME_FD_TO_HANDLE"); return false; }
  b.handle = ph.handle;

  drm_mode_fb_cmd2 f = {};
  f.width = w; f.height = h; f.pixel_format = 0x34325258;  // XR24
  f.handles[0] = b.handle; f.pitches[0] = w * 4;
  if (io(DRM_IOCTL_MODE_ADDFB2, f) < 0) { perror("ADDFB2"); return false; }
  b.fb = f.fb_id;
  return true;
}

int main(int argc, char** argv) {
  const char* card = "/dev/dri/card0";
  int frames = 300, max_outs = 8;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--outputs") && i + 1 < argc) max_outs = atoi(argv[++i]);
  }
  fd_ = open(card, O_RDWR | O_CLOEXEC);
  if (fd_ < 0) { perror(card); return 1; }
  if (ioctl(fd_, DRM_IOCTL_SET_MASTER, 0) < 0) { perror("SET_MASTER (is the compositor running?)"); return 1; }

  // Resources: connectors and CRTCs.
  drm_mode_card_res res = {};
  if (io(DRM_IOCTL_MODE_GETRESOURCES, res) < 0) { perror("GETRESOURCES"); return 1; }
  std::vector<uint32_t> conns(res.count_connectors), crtcs(res.count_crtcs), encs(res.count_encoders);
  res.connector_id_ptr = reinterpret_cast<uint64_t>(conns.data());
  res.crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs.data());
  res.encoder_id_ptr = reinterpret_cast<uint64_t>(encs.data());
  res.count_fbs = 0; res.fb_id_ptr = 0;
  if (io(DRM_IOCTL_MODE_GETRESOURCES, res) < 0) { perror("GETRESOURCES 2"); return 1; }
  printf("card %s: %u connectors, %u crtcs\n", card, res.count_connectors, res.count_crtcs);

  // Every connected connector with a mode: the board is dual-panel, and a
  // scanout tier has to drive both CRTCs from one process.
  struct Out { uint32_t conn, crtc; drm_mode_modeinfo mode; Buf buf[2]; };
  std::vector<Out> outs;
  for (uint32_t c : conns) {
    drm_mode_get_connector gc = {}; gc.connector_id = c;
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, gc) < 0) continue;
    std::vector<drm_mode_modeinfo> modes(gc.count_modes);
    std::vector<uint32_t> cencs(gc.count_encoders);
    gc.modes_ptr = reinterpret_cast<uint64_t>(modes.data());
    gc.encoders_ptr = reinterpret_cast<uint64_t>(cencs.data());
    gc.count_props = 0; gc.props_ptr = 0; gc.prop_values_ptr = 0;
    if (io(DRM_IOCTL_MODE_GETCONNECTOR, gc) < 0) continue;
    printf("  connector %u type %u status %u modes %u encoder %u\n", c, gc.connector_type, gc.connection, gc.count_modes, gc.encoder_id);
    if (gc.connection != 1 || gc.count_modes == 0) continue;
    Out o = {}; o.conn = c; o.mode = modes[0];
    drm_mode_get_encoder ge = {}; ge.encoder_id = gc.encoder_id ? gc.encoder_id : (gc.count_encoders ? cencs[0] : 0);
    if (ge.encoder_id && io(DRM_IOCTL_MODE_GETENCODER, ge) == 0 && ge.crtc_id) o.crtc = ge.crtc_id;
    if (!o.crtc) { for (uint32_t k : crtcs) { bool used = false; for (auto& u : outs) used |= u.crtc == k; if (!used) { o.crtc = k; break; } } }
    if (o.crtc) outs.push_back(o);
    if (outs.size() >= static_cast<size_t>(max_outs)) break;
  }
  if (outs.empty()) { fprintf(stderr, "no connected connector/crtc\n"); return 1; }
  for (Out& o : outs) {
    printf("using connector %u crtc %u mode %ux%u@%u\n", o.conn, o.crtc, o.mode.hdisplay, o.mode.vdisplay, o.mode.vrefresh);
    for (Buf& b : o.buf) if (!alloc_cma(b, o.mode.hdisplay, o.mode.vdisplay)) return 1;
    drm_mode_crtc sc = {};
    sc.crtc_id = o.crtc; sc.fb_id = o.buf[0].fb; sc.mode = o.mode; sc.mode_valid = 1;
    sc.set_connectors_ptr = reinterpret_cast<uint64_t>(&o.conn); sc.count_connectors = 1;
    if (io(DRM_IOCTL_MODE_SETCRTC, sc) < 0) { perror("SETCRTC"); return 1; }
  }
  printf("modeset ok on %zu output(s); scanning out CMA dmabufs\n", outs.size());

  // Per-frame: write the whole buffer (what the scale-out costs), submit a
  // non-blocking flip, and only then wait for the *previous* flip -- the way
  // the emulator would overlap it with the next frame's emulation.
  double t_write = 0, t_submit = 0, t_wait = 0;
  bool pending = false;
  int cur = 0;
  double worst_submit = 0;
  for (int f = 0; f < frames; ++f) {
    const uint32_t colour = 0x00202020u + static_cast<uint32_t>(f % 200) * 0x00010101u;
    double a = now_ms();
    for (Out& o : outs) {
      uint32_t* p = o.buf[cur].px;
      for (size_t i = 0; i < o.buf[cur].bytes / 4; ++i) p[i] = colour;
    }
    double bt = now_ms();
    for (Out& o : outs) {
      drm_mode_crtc_page_flip pf = {};
      pf.crtc_id = o.crtc; pf.fb_id = o.buf[cur].fb; pf.flags = DRM_MODE_PAGE_FLIP_EVENT;
      pf.user_data = static_cast<uint64_t>(f);
      if (io(DRM_IOCTL_MODE_PAGE_FLIP, pf) < 0) { perror("PAGE_FLIP"); return 1; }
    }
    double c = now_ms();
    // Wait for them (the buffers must be free before we write them again).
    for (size_t k = 0; k < outs.size(); ++k) {
      char ev[256];
      pollfd pfd = {fd_, POLLIN, 0};
      poll(&pfd, 1, 1000);
      ssize_t n = read(fd_, ev, sizeof ev);
      (void)n;
    }
    double d = now_ms();
    if (f >= 20) {   // warm-up
      t_write += bt - a; t_submit += c - bt; t_wait += d - c;
      if (c - bt > worst_submit) worst_submit = c - bt;
    }
    pending = true; (void)pending;
    cur ^= 1;
  }
  const double n = frames - 20;
  size_t total_bytes = 0; for (Out& o : outs) total_bytes += o.buf[0].bytes;
  printf("per frame over %d frames (%zu output(s), %.1f MB):\n", static_cast<int>(n), outs.size(), total_bytes / 1048576.0);
  printf("  CMA full-buffer write : %.3f ms\n", t_write / n);
  printf("  page-flip submit      : %.3f ms (max %.3f)\n", t_submit / n, worst_submit);
  printf("  wait for flip event   : %.3f ms\n", t_wait / n);
  printf("  total                 : %.3f ms\n", (t_write + t_submit + t_wait) / n);
  return 0;
}
