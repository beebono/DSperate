// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Go/no-go for compositor-compatible direct scanout: is a CMA dma-heap
// mapping fast enough to render into?
//
// The plan is to allocate the presentation buffer from /dev/dma_heap, mmap it,
// scale-and-blit both DS screens into it on the CPU, and hand the dmabuf to
// the compositor via zwp_linux_dmabuf so wlroots can scan it out directly.
// That only pays if writing into the mapping costs about what writing into
// ordinary memory costs -- dma-heap mappings are often uncached or
// write-combined, and if ours is several times slower the ~26% we hoped to
// win by not being composited is gone before we start.
//
// So this does the real per-frame work (clear the letterbox, nearest-neighbour
// scale 256x192 into each screen's rect) into a malloc'd buffer and into a CMA
// mapping, alternating rounds so thermal drift shows up as noise in both
// rather than as a win for whichever ran second.
//
// Standalone on purpose -- it must build and run on a board with nothing but a
// compiler:
//   aarch64-linux-gnu-g++ -O2 -o cma_blit_bench tools/cma_blit_bench.cpp
//
// Run it on the panel geometry the device actually uses; the layout changes
// the write volume by 4x on the portrait board:
//   ./cma_blit_bench --w 720 --h 1280 --layout vertical      # .25
//   ./cma_blit_bench --w 1280 --h 480 --layout horizontal    # .20, both panels
//
// --rot: the .25 scanout case. The output is scanned in panel orientation
// (720x1280 portrait) while the layout is landscape, so the scale-out must
// write rotated: a logical row becomes a destination *column*. Naive 4-byte
// column writes would be the worst pattern CMA could see; instead 8 logical
// rows are staged hot and stored as 8-wide (32-byte) blocks marching down
// the buffer -- the pattern the emulator's rotated scale-out would use. The
// same tiling runs against malloc and CMA:
//   ./cma_blit_bench --w 720 --h 1280 --rot                  # .25 scanout

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Declared here rather than included from <linux/dma-heap.h> so this builds
// against whatever sysroot is lying around.
struct dma_heap_allocation_data {
  uint64_t len;
  uint32_t fd;
  uint32_t fd_flags;
  uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

namespace {

constexpr int SCREEN_W = 256;
constexpr int SCREEN_H = 192;
constexpr int SCREENS = 2;

struct Rect { int x, y, w, h; };

double now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// Same arithmetic as Display::layout(): both screens stacked or side by side,
// aspect preserved, centred, non-integer scale.
void layout(int w, int h, bool across, Rect out[SCREENS]) {
  const int cols = across ? 2 : 1, rows = across ? 1 : 2;
  const double s = std::min(static_cast<double>(w) / (SCREEN_W * cols),
                            static_cast<double>(h) / (SCREEN_H * rows));
  const int dw = static_cast<int>(SCREEN_W * s), dh = static_cast<int>(SCREEN_H * s);
  const int x = (w - dw * cols) / 2, y = (h - dh * rows) / 2;
  for (int i = 0; i < SCREENS; ++i)
    out[i] = across ? Rect{x + dw * i, y, dw, dh} : Rect{x, y + dh * i, dw, dh};
}

// Nearest-neighbour scale of one 256x192 ARGB8888 screen into its rect.
// Destination writes are sequential within a row, which is the pattern that
// matters if the mapping turns out to be write-combined; the scattered access
// is on the source side, in ordinary cached memory.
void blit(uint32_t* dst, int pitch_px, const uint32_t* src, const Rect& r,
          const std::vector<int>& xmap) {
  for (int y = 0; y < r.h; ++y) {
    const uint32_t* s = src + static_cast<size_t>(y * SCREEN_H / r.h) * SCREEN_W;
    uint32_t* d = dst + static_cast<size_t>(r.y + y) * pitch_px + r.x;
    for (int x = 0; x < r.w; ++x) d[x] = s[xmap[x]];
  }
}

struct Stats { double mean, p50, p99; };

Stats stats(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (double x : v) sum += x;
  return Stats{sum / v.size(), v[v.size() / 2], v[static_cast<size_t>(v.size() * 0.99)]};
}

// One target buffer: where it came from, and how to reach its pixels.
struct Target {
  const char* name;
  uint32_t* px = nullptr;
  int dmabuf_fd = -1;
  void* map = nullptr;
  size_t bytes = 0;
};

bool alloc_malloc(Target& t, size_t bytes) {
  t.px = static_cast<uint32_t*>(std::aligned_alloc(4096, bytes));
  if (!t.px) { std::fprintf(stderr, "aligned_alloc(%zu) failed\n", bytes); return false; }
  std::memset(t.px, 0, bytes);   // fault it in, so the first round isn't paying for page faults
  t.bytes = bytes;
  return true;
}

bool alloc_cma(Target& t, size_t bytes, const char* heap) {
  int hfd = open(heap, O_RDWR | O_CLOEXEC);
  if (hfd < 0) { std::perror(heap); return false; }
  dma_heap_allocation_data d = {};
  d.len = bytes;
  d.fd_flags = O_RDWR | O_CLOEXEC;
  if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &d) < 0) {
    std::perror("DMA_HEAP_IOCTL_ALLOC");
    close(hfd);
    return false;
  }
  close(hfd);
  t.dmabuf_fd = static_cast<int>(d.fd);
  t.map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, t.dmabuf_fd, 0);
  if (t.map == MAP_FAILED) { std::perror("mmap dmabuf"); close(t.dmabuf_fd); t.dmabuf_fd = -1; return false; }
  t.px = static_cast<uint32_t*>(t.map);
  t.bytes = bytes;
  std::memset(t.px, 0, bytes);
  return true;
}

void release(Target& t) {
  if (t.map) munmap(t.map, t.bytes);
  else std::free(t.px);
  if (t.dmabuf_fd >= 0) close(t.dmabuf_fd);
  t.px = nullptr; t.map = nullptr; t.dmabuf_fd = -1;
}

struct Result { std::vector<double> clear, blit; };

#ifdef ROT_NEON
#include <arm_neon.h>
// The staging gather above is 16 scattered scalar loads per 64-byte store;
// this replaces it with an in-register transpose: 4x4 blocks of u32 are
// loaded as rows (sequential 16-byte vector loads within each staging row),
// transposed with TRN/zip pairs, and four transposed blocks concatenate into
// one 64-byte destination-row store.
static inline void trn4(uint32x4_t r[4]) {
  uint32x4x2_t a = vtrnq_u32(r[0], r[1]), b = vtrnq_u32(r[2], r[3]);
  r[0] = vcombine_u32(vget_low_u32(a.val[0]),  vget_low_u32(b.val[0]));
  r[1] = vcombine_u32(vget_low_u32(a.val[1]),  vget_low_u32(b.val[1]));
  r[2] = vcombine_u32(vget_high_u32(a.val[0]), vget_high_u32(b.val[0]));
  r[3] = vcombine_u32(vget_high_u32(a.val[1]), vget_high_u32(b.val[1]));
}
#endif

// Rotated (ccw) blit: logical landscape lw x lh = h x w scaled from the DS
// screens, written into the w x h portrait buffer as dst[y][x] =
// logical[lw-1-y][x]. Eight logical rows at a time are built in a hot
// staging tile, then stored transposed: for each logical x (a destination
// row), one 8-u32 contiguous block. Only the store pattern differs from the
// straight blit; the source work is identical.
void blit_rot(uint32_t* dst, int w, int h, const uint32_t* src, const Rect& r,
              const std::vector<int>& xmap) {
  // r is the screen's rect in the logical landscape (lw = h, lh = w).
  //
  // 16 logical rows per tile so every destination store is one full 64-byte
  // cache line (write-allocate never reads a half-written line back), and
  // the staging pitch is padded off the power of two so the transposed reads
  // do not alias into one cache set.
#ifndef ROT_T
#define ROT_T 16
#endif
  constexpr int T = ROT_T, PITCH = 2048 + 24;
  alignas(64) static uint32_t tile[T * PITCH];
  const int lw = h;
  for (int ly0 = 0; ly0 < r.h; ly0 += T) {
    const int rows = std::min(T, r.h - ly0);
    for (int t = 0; t < rows; ++t) {
      const uint32_t* s = src + static_cast<size_t>((ly0 + t) * SCREEN_H / r.h) * SCREEN_W;
      uint32_t* d = tile + t * PITCH;
      for (int x = 0; x < r.w; ++x) d[x] = s[xmap[x]];
    }
    if (rows < T)   // partial last tile: pad so the store loop stays full-width
      for (int t = rows; t < T; ++t) std::memcpy(tile + t * PITCH, tile + (rows - 1) * PITCH, static_cast<size_t>(r.w) * 4);
    // The rect's logical x span becomes a destination row span, its y span a
    // destination column span: one 64-byte store per destination row.
#ifdef ROT_NEON
    // 4 destination rows per step, each taking 4 transposed 4x4 blocks.
    for (int lx = 0; lx + 4 <= r.w; lx += 4) {
      uint32x4_t blk[4][4];   // blk[g] = staging rows 4g..4g+3 at columns lx..lx+3
      for (int g = 0; g < 4; ++g) {
        for (int i = 0; i < 4; ++i) blk[g][i] = vld1q_u32(tile + (4 * g + i) * PITCH + lx);
        trn4(blk[g]);
      }
      for (int i = 0; i < 4; ++i) {
        uint32_t* d = dst + static_cast<size_t>(lw - 1 - (r.x + lx + i)) * w + (r.y + ly0);
        vst1q_u32(d + 0,  blk[0][i]);
        vst1q_u32(d + 4,  blk[1][i]);
        vst1q_u32(d + 8,  blk[2][i]);
        vst1q_u32(d + 12, blk[3][i]);
      }
    }
    for (int lx = r.w & ~3; lx < r.w; ++lx) {
#else
    for (int lx = 0; lx < r.w; ++lx) {
#endif
      uint32_t* d = dst + static_cast<size_t>(lw - 1 - (r.x + lx)) * w + (r.y + ly0);
      const uint32_t* srcc = tile + lx;
#ifdef ROT_STNP
      // Gather into locals, then store the whole run non-temporally: the
      // destination is never read back, so skip the cache entirely.
      uint32_t buf[T];
      for (int t = 0; t < T; ++t) buf[t] = srcc[t * PITCH];
      for (int t = 0; t < T; t += 8) {
        __asm__ volatile("ldp q0, q1, [%1]\n\tstnp q0, q1, [%0]"
                         :: "r"(d + t), "r"(buf + t) : "q0", "q1", "memory");
      }
#else
      for (int t = 0; t < T; ++t) d[t] = srcc[t * PITCH];
#endif
    }
  }
}

void run(Target& t, int w, int h, const Rect rects[SCREENS],
         const std::vector<int> xmap[SCREENS], const uint32_t* const fb[SCREENS],
         int frames, bool sync, bool rot, Result& out) {
  const size_t bytes = static_cast<size_t>(w) * h * 4;
  for (int f = 0; f < frames; ++f) {
    if (sync && t.dmabuf_fd >= 0) {
      dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
      ioctl(t.dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s);
    }
    const double t0 = now_ms();
    std::memset(t.px, 0, bytes);            // what SDL_RenderClear costs us
    const double t1 = now_ms();
    for (int i = 0; i < SCREENS; ++i)
      if (rot) blit_rot(t.px, w, h, fb[i], rects[i], xmap[i]);
      else blit(t.px, w, fb[i], rects[i], xmap[i]);
    const double t2 = now_ms();
    if (sync && t.dmabuf_fd >= 0) {
      dma_buf_sync s = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE};
      ioctl(t.dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s);
    }
    out.clear.push_back(t1 - t0);
    out.blit.push_back(t2 - t1);
  }
}

void report(const char* name, const Result& r, size_t clear_bytes, size_t blit_bytes) {
  const Stats c = stats(r.clear), b = stats(r.blit);
  std::printf("  %-8s clear %6.3f ms (p50 %6.3f p99 %6.3f) %7.0f MB/s | "
              "blit %6.3f ms (p50 %6.3f p99 %6.3f) %7.0f MB/s | frame %6.3f ms\n",
              name, c.mean, c.p50, c.p99, clear_bytes / c.mean / 1048.576,
              b.mean, b.p50, b.p99, blit_bytes / b.mean / 1048.576,
              c.mean + b.mean);
}

} // namespace

int main(int argc, char** argv) {
  int w = 720, h = 1280, frames = 300, warmup = 60, rounds = 3;
  bool across = false, sync = false, rot = false;
  const char* heap = "/dev/dma_heap/linux,cma";

  for (int i = 1; i < argc; ++i) {
    auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
    if (!std::strcmp(argv[i], "--w")) w = std::atoi(next());
    else if (!std::strcmp(argv[i], "--h")) h = std::atoi(next());
    else if (!std::strcmp(argv[i], "--frames")) frames = std::atoi(next());
    else if (!std::strcmp(argv[i], "--warmup")) warmup = std::atoi(next());
    else if (!std::strcmp(argv[i], "--rounds")) rounds = std::atoi(next());
    else if (!std::strcmp(argv[i], "--layout")) across = !std::strcmp(next(), "horizontal");
    else if (!std::strcmp(argv[i], "--sync")) sync = true;
    else if (!std::strcmp(argv[i], "--rot")) { rot = true; across = true; }
    else if (!std::strcmp(argv[i], "--heap")) heap = next();
    else { std::fprintf(stderr, "usage: %s [--w N] [--h N] [--layout vertical|horizontal]\n"
                                "          [--frames N] [--warmup N] [--rounds N] [--sync] [--heap PATH]\n", argv[0]);
           return 2; }
  }

  Rect rects[SCREENS];
  // Rotated: the layout happens in the logical landscape (h x w), the buffer
  // stays portrait (w x h).
  layout(rot ? h : w, rot ? w : h, across, rects);

  std::vector<int> xmap[SCREENS];
  size_t blit_px = 0;
  for (int i = 0; i < SCREENS; ++i) {
    xmap[i].resize(rects[i].w);
    for (int x = 0; x < rects[i].w; ++x) xmap[i][x] = x * SCREEN_W / rects[i].w;
    blit_px += static_cast<size_t>(rects[i].w) * rects[i].h;
  }

  // Source framebuffers. Real content, not a flat fill -- a constant source
  // would let the memory system coalesce in ways a real frame never does.
  std::vector<uint32_t> src[SCREENS];
  const uint32_t* fb[SCREENS];
  uint32_t seed = 12345;
  for (int i = 0; i < SCREENS; ++i) {
    src[i].resize(SCREEN_W * SCREEN_H);
    for (auto& p : src[i]) { seed = seed * 1664525u + 1013904223u; p = 0xff000000u | (seed >> 8); }
    fb[i] = src[i].data();
  }

  const size_t bytes = static_cast<size_t>(w) * h * 4;
  const double scale = static_cast<double>(rects[0].w) / SCREEN_W;
  std::printf("%dx%d %s%s, scale x%.2f, %zu KiB buffer, %zu KiB blitted per frame\n",
              w, h, across ? "horizontal" : "vertical", rot ? " ROTATED (tiled ccw)" : "",
              scale, bytes / 1024, blit_px * 4 / 1024);
  std::printf("%d rounds of %d frames (%d discarded as warm-up), dma-buf sync %s\n\n",
              rounds, frames, warmup, sync ? "on" : "off");

  Target mem{"malloc"}, cma{"cma"};
  if (!alloc_malloc(mem, bytes)) return 1;
  const bool have_cma = alloc_cma(cma, bytes, heap);
  if (!have_cma)
    std::fprintf(stderr, "\n*** no CMA buffer -- malloc numbers only ***\n\n");

  // Alternate the two targets so DVFS or thermal drift over the run shows up
  // in both rather than as a win for whichever went last. The first frames of
  // a round are discarded outright: on these boards a cold first run is slow
  // enough to reverse a comparison.
  Result rm, rc;
  for (int r = 0; r < rounds; ++r) {
    Result warm;
    run(mem, w, h, rects, xmap, fb, warmup, sync, rot, warm);
    run(mem, w, h, rects, xmap, fb, frames, sync, rot, rm);
    if (have_cma) {
      Result warm2;
      run(cma, w, h, rects, xmap, fb, warmup, sync, rot, warm2);
      run(cma, w, h, rects, xmap, fb, frames, sync, rot, rc);
    }
  }

  const size_t blit_bytes = blit_px * 4;
  report("malloc", rm, bytes, blit_bytes);
  if (have_cma) {
    report("cma", rc, bytes, blit_bytes);
    const double fm = stats(rm.clear).mean + stats(rm.blit).mean;
    const double fc = stats(rc.clear).mean + stats(rc.blit).mean;
    std::printf("\n  cma / malloc = %.2fx per frame (%+.3f ms)\n", fc / fm, fc - fm);
    std::printf("  %s\n", fc / fm < 1.3
        ? "GO: writing into CMA costs about what ordinary memory costs."
        : "STOP: the mapping is too slow to render into; direct scanout cannot pay for this.");
  }

  // Keep the writes observable.
  volatile uint32_t sink = mem.px[bytes / 8] ^ (have_cma ? cma.px[bytes / 8] : 0);
  (void)sink;

  release(mem);
  if (have_cma) release(cma);
  return 0;
}
