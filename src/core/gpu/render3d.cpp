// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/render3d.h"
#include "core/state/state.h"

#include <type_traits>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include "core/gpu/kernels.h"
#include "core/gpu/gpu3d.h"
#include "core/gpu/vram_map.h"
#include "core/nds.h"
#include "core/profile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#if DSPERATE_NEON
#include <arm_neon.h>
#include "core/gpu/neon_compat.h"
#endif

namespace ds::gpu {

#if DSPERATE_NEON
// The A64-only NEON intrinsics the kernels use, in both spellings.
namespace compat = kern::compat;
#endif

// ---- interpolation --------------------------------------------------------------
//
// Attributes are not divided per pixel. A perspective-correct factor between
// the two endpoints is computed (9 fractional bits along Y, 8 along X) and the
// attribute is then interpolated linearly by that factor. When both W values
// are equal (with low bits clear) the factor is skipped and the interpolation
// is plainly linear, which is what 2D-style polygons want.

template <int dir>
void Renderer3D::Interp<dir>::setup(s32 x0_, s32 x1_, s32 w0, s32 w1, bool wbuf) {
  x0 = x0_; x1 = x1_; xdiff = x1_ - x0_; wbuffer = wbuf;
  xrecip_z = xdiff != 0 ? (1 << 22) / xdiff : 0;
  recip = xdiff >= 2 ? static_cast<u32>(((1ull << 32) + static_cast<u32>(xdiff) - 1) / static_cast<u32>(xdiff)) : 0;
  const u32 mask = dir ? 0x7E : 0x7F;
  linear = (w0 == w1) && !(w0 & mask) && !(w1 & mask);
  if (dir) { w0n = w0 >> 1; w0d = (w0 + ((w0 & ~w1) & 1)) >> 1; w1d = w1 >> 1; shift = 9; }
  else { w0n = w0; w0d = w0; w1d = w1; shift = 8; }
}

template <int dir>
void Renderer3D::Interp<dir>::set_x(s32 xv) {
  xv -= x0;
  x = xv;
  if (xdiff != 0 && (!linear || wbuffer)) {
    const u32 num = static_cast<u32>(xv * w0n) << shift;
    const u32 den = static_cast<u32>(xv * w0d) + static_cast<u32>((xdiff - xv) * w1d);
    yfactor = den == 0 ? 0 : num / den;
  }
}

template <int dir>
[[gnu::always_inline]] inline s32 Renderer3D::Interp<dir>::interpolate(s32 y0, s32 y1) const {
  if (xdiff == 0 || y0 == y1) return y0;
  if (!linear) {
    if (y0 < y1) return y0 + static_cast<s32>((static_cast<s64>(y1 - y0) * yfactor) >> shift);
    return y1 + static_cast<s32>((static_cast<s64>(y0 - y1) * ((1 << shift) - yfactor)) >> shift);
  }
  // Linear: d * f / xdiff with d < 2^17 (colours, texture coordinates) and
  // f <= xdiff < 2^9, so the product fits 32 bits; q = (n * ceil(2^32/xdiff))
  // >> 32 is the quotient or one too many, and one compare fixes it.
  const u32 d = static_cast<u32>(y0 < y1 ? y1 - y0 : y0 - y1), f = static_cast<u32>(y0 < y1 ? x : xdiff - x);
  u32 q;
  if (recip) {
    const u32 n = d * f;
    q = static_cast<u32>((static_cast<u64>(n) * recip) >> 32);
    if (q * static_cast<u32>(xdiff) > n) --q;
  } else if (xdiff == 1) q = d * f;
  else q = static_cast<u32>(static_cast<s64>(d) * f / xdiff);
  return (y0 < y1 ? y0 : y1) + static_cast<s32>(q);
}

template <int dir>
s32 Renderer3D::Interp<dir>::interpolate_z(s32 z0, s32 z1) const {
  if (xdiff == 0 || z0 == z1) return z0;
  if (wbuffer) {
    if (z0 < z1) return z0 + static_cast<s32>((static_cast<s64>(z1 - z0) * yfactor) >> shift);
    return z1 + static_cast<s32>((static_cast<s64>(z0 - z1) * ((1 << shift) - yfactor)) >> shift);
  }
  // Z-buffering interpolates linearly with a reciprocal of the span.
  s32 base, disp, factor;
  if (z0 < z1) { base = z0; disp = z1 - z0; factor = x; }
  else { base = z1; disp = z0 - z1; factor = xdiff - x; }
  if (dir) {
    int sh = 0;
    while (disp > 0x3FF) { disp >>= 1; ++sh; }
    return base + static_cast<s32>(((static_cast<s64>(disp) * factor * xrecip_z) >> 22) << sh);
  }
  disp >>= 9;
  return base + static_cast<s32>((static_cast<s64>(disp) * factor * xrecip_z) >> 13);
}

// ---- edges ------------------------------------------------------------------------

template <int side>
s32 Renderer3D::Slope<side>::setup_dummy(s32 x0_, bool wbuf) {
  dx = 0; x0 = x0_; xmin = x0_; xmax = x0_;
  increment = 0; xmajor = false;
  interp.setup(0, 0, 0, 0, wbuf);
  interp.set_x(0);
  xcov_incr = 0;
  return x0_;
}

// The slope has an 18-bit fraction and is computed as (x1-x0) * (1/ylen),
// not as a direct division, which is what the hardware does.
template <int side>
s32 Renderer3D::Slope<side>::setup(s32 x0_, s32 x1_, s32 y0, s32 y1, s32 w0, s32 w1, s32 y_, bool wbuf) {
  x0 = x0_; y = y_;
  if (x1_ > x0_) { xmin = x0_; xmax = x1_ - 1; negative = false; }
  else if (x1_ < x0_) { xmin = x1_; xmax = x0_ - 1; negative = true; }
  else { xmin = x0_; xmax = xmin; negative = false; }
  xlen = xmax + 1 - xmin;
  ylen = y1 - y0;
  if (ylen == 0) increment = 0;
  else if (ylen == xlen && xlen != 1) increment = 0x40000;
  else {
    const s32 yrecip = (1 << 18) / ylen;
    increment = (x1_ - x0_) * yrecip;
    if (increment < 0) increment = -increment;
  }
  xmajor = increment > 0x40000;
  if (side) {
    if (xmajor) dx = negative ? (0x20000 + 0x40000) : (increment - 0x20000);
    else if (increment != 0) dx = negative ? 0x40000 : 0;
    else dx = 0;
  } else {
    if (xmajor) dx = negative ? ((increment - 0x20000) + 0x40000) : 0x20000;
    else if (increment != 0) dx = negative ? 0x40000 : 0;
    else dx = 0;
  }
  dx += (y_ - y0) * increment;
  const s32 x = xval();
  const int interpoffset = (increment >= 0x40000) && (side ^ static_cast<int>(negative));
  interp.setup(y0 - interpoffset, y1 - interpoffset, w0, w1, wbuf);
  interp.set_x(y_);
  if (xmajor) xcov_incr = (ylen << 10) / xlen;
  return x;
}

template <int side>
s32 Renderer3D::Slope<side>::step() {
  dx += increment;
  ++y;
  const s32 x = xval();
  interp.set_x(y);
  return x;
}

template <int side>
s32 Renderer3D::Slope<side>::xval() const {
  s32 r = negative ? x0 - (dx >> 18) : x0 + (dx >> 18);
  if (r < xmin) r = xmin; else if (r > xmax) r = xmax;
  return r;
}

// Length of the edge's run on this scanline and its anti-aliasing coverage.
// X-major edges return the first pixel's coverage plus a per-pixel increment
// (flagged by bit 31); Y-major edges a single 5-bit coverage.
template <int side>
template <bool swapped>
void Renderer3D::Slope<side>::edge_params(bool aa, s32* length, s32* coverage) const {
  if (xmajor) {
    if (!swapped || side) {
      if (side ^ static_cast<int>(negative)) *length = (dx >> 18) - ((dx - increment) >> 18);
      else *length = ((dx + increment) >> 18) - (dx >> 18);
    }
    if (aa) {
      s32 startx = dx >> 18;
      if (negative) startx = xlen - startx;
      if (side) startx = startx - *length + 1;
      const s32 startcov = (((startx << 10) + 0x1FF) * ylen) / xlen;
      *coverage = static_cast<s32>(0x80000000u) | ((startcov & 0x3FF) << 12) | (xcov_incr & 0x3FF);
    } else *coverage = 0;
    if (swapped) *length = 1;
    return;
  }
  *length = 1;
  if (!aa) { *coverage = 0; return; }
  if (increment == 0) { *coverage = swapped ? 0 : 31; return; }
  s32 cov = ((dx >> 9) + (increment >> 10)) >> 4;
  if ((cov >> 5) != (dx >> 18)) cov = 31;
  cov &= 0x1F;
  if (swapped) { if (side ^ static_cast<int>(negative)) cov = 0x1F - cov; }
  else { if (!(side ^ static_cast<int>(negative))) cov = 0x1F - cov; }
  *coverage = cov;
}

// ---- pixel pipeline ---------------------------------------------------------------

// Fixed set of band workers. dispatch() only publishes the job and notifies, so
// every band runs on a pool thread and the emulation thread renders none of them
// -- it goes on emulating and pays only what it later blocks for in sync_line.
// A frame costs one broadcast and one barrier; the threads are created once and
// parked on a condition variable in between, never per frame.
//
// So Pool(n) is n threads *in addition to* the emulation thread, and
// DS_R3D_THREADS=N asks for N of them. N of 0 or 1 is a different shape
// entirely: band_count returns 1, render() takes the `maxb <= 1`
// path, and the raster runs inline on the emulation thread with no pool.
struct Renderer3D::Pool {
  explicit Pool(u32 n) {
    threads_.reserve(n);
    for (u32 i = 0; i < n; ++i) threads_.emplace_back([this, i] { loop(i); });
  }
  ~Pool() {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; generation_.fetch_add(1, std::memory_order_relaxed); }
    start_.notify_all();
    for (auto& t : threads_) t.join();
  }
  u32 workers() const { return static_cast<u32>(threads_.size()); }

  // Hand the bands to the workers and return. The caller (the emulation
  // thread) carries on and waits per band, at the line each band's output is
  // first read -- see Renderer3D::sync_line.
  // Returns the generation of this dispatch, which is what wait_bits takes.
  u64 dispatch(const std::function<void(u32)>& fn, u32 jobs, u32 bins) {
    u64 gen;
    {
      std::lock_guard<std::mutex> lk(m_);
      job_ = &fn; jobs_ = jobs; remaining_.store(static_cast<u32>(threads_.size()), std::memory_order_relaxed);
      done_bits_.store(0, std::memory_order_relaxed);
      nbins_ = bins;
      // Before the generation bump, not after: a worker that wakes on the new
      // generation must never see the previous frame's exhausted counter and
      // conclude there is nothing to do.
      next_bin_.store(0, std::memory_order_relaxed);
      gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    start_.notify_all();
    return gen;
  }

  // The next bin no worker has taken, or >= nbins when they are all claimed.
  // Bins are handed out in ascending Y so the top of the frame is always the
  // work in flight first -- that is the deadline order, since band b's output
  // is first read at display line bin_y_[b].
  u32 claim() { return next_bin_.fetch_add(1, std::memory_order_acq_rel); }
  u32 bins() const { return nbins_; }

  void mark_done(u32 bin) {
    { std::lock_guard<std::mutex> lk(m_); done_bits_.fetch_or(u64{1} << bin, std::memory_order_release); }
    done_.notify_all();
  }

  // Returns how long the caller was actually blocked, in nanoseconds.
  //
  // Most display lines find their bin already drawn, so the common case takes
  // no lock at all and costs one atomic load -- which is also what keeps the
  // clock reads off the fast path. The blocking case is the signal the thread
  // count is chosen from: it is the emulation thread standing still, waiting
  // for a strip of the frame it is about to composite.
  //
  // `gen` names the dispatch whose bins are meant: a wait for a generation
  // that has since been superseded returns at once, since a dispatch only
  // ever follows wait_idle on the one before it -- every bin of the old
  // generation is done by then. The compositor of frame N asks for frame N's
  // bands after the emulation thread has dispatched N+1 at line 215, and
  // without the generation check it would be waiting on N+1's done bits,
  // for ever if N+1 has fewer bins. Generation is read before the bits so
  // that a reset between the two is caught by the locked re-check.
  u64 wait_bits(u64 gen, u64 mask) {
    if (!mask) return 0;
    if (generation_.load(std::memory_order_acquire) != gen) return 0;
    if ((done_bits_.load(std::memory_order_acquire) & mask) == mask) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lk(m_);
      done_.wait(lk, [this, gen, mask] {
        return generation_.load(std::memory_order_relaxed) != gen || (done_bits_.load(std::memory_order_relaxed) & mask) == mask;
      });
    }
    return static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
  }

  // Every worker has returned from the job, not merely finished its bins.
  //
  // The two are no longer the same. A worker signals a bin from inside the
  // job (mark_done) so the compositor can read that strip while the rest of
  // the frame is still being drawn, which means the last bin can be marked
  // while its worker is still executing -- and the caller of sync_all goes on
  // to reassign job_fn_, destroying the std::function under it. Bin bits are
  // what a display line waits for; this is what the frame boundary waits for.
  u64 wait_idle() {
    if (remaining_.load(std::memory_order_acquire) == 0 && thieves_.load(std::memory_order_acquire) == 0) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lk(m_);
      done_.wait(lk, [this] { return remaining_.load(std::memory_order_relaxed) == 0 && thieves_.load(std::memory_order_relaxed) == 0; });
    }
    return static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
  }
  // A thief's claim: only if `gen` is still the dispatch in flight, atomically
  // with that check (dispatch resets the cursor under the lock), and counted
  // as in flight until thief_done so wait_idle covers it.
  u32 claim_if(u64 gen) {
    std::lock_guard<std::mutex> lk(m_);
    if (generation_.load(std::memory_order_relaxed) != gen) return ~0u;
    const u32 b = next_bin_.fetch_add(1, std::memory_order_acq_rel);
    if (b < nbins_) thieves_.fetch_add(1, std::memory_order_acq_rel);
    return b;
  }
  void thief_done(u32 bin) {
    { std::lock_guard<std::mutex> lk(m_); done_bits_.fetch_or(u64{1} << bin, std::memory_order_release); thieves_.fetch_sub(1, std::memory_order_acq_rel); }
    done_.notify_all();
  }
  bool done(u64 gen, u64 mask) const {
    return generation_.load(std::memory_order_acquire) != gen || (done_bits_.load(std::memory_order_acquire) & mask) == mask;
  }

private:
  void loop(u32 index) {
    u64 seen = 0;
    for (;;) {
      std::unique_lock<std::mutex> lk(m_);
      start_.wait(lk, [this, &seen] { return stop_ || generation_.load(std::memory_order_relaxed) != seen; });
      if (stop_) return;
      seen = generation_.load(std::memory_order_relaxed);
      const std::function<void(u32)>* fn = job_;
      const u32 jobs = jobs_;
      lk.unlock();
      if (fn && index < jobs) (*fn)(index);
      lk.lock();
      remaining_.fetch_sub(1, std::memory_order_release);
      done_.notify_all();
    }
  }
public:
  void debug_dump(FILE* f) {
    std::fprintf(f, "  band pool: workers %zu generation %llu jobs %u nbins %u next_bin %u remaining %u done_bits %016llx stop %d\n",
                 threads_.size(), (unsigned long long)generation_.load(std::memory_order_relaxed), jobs_, nbins_, next_bin_.load(std::memory_order_relaxed),
                 remaining_.load(std::memory_order_relaxed), (unsigned long long)done_bits_.load(std::memory_order_relaxed), stop_ ? 1 : 0);
  }
  std::vector<std::thread> threads_;
  std::mutex m_;
  std::condition_variable start_, done_;
  const std::function<void(u32)>* job_ = nullptr;
  std::atomic<u64> generation_{0};   // bumped under m_; read lock-free by wait_bits
  std::atomic<u64> done_bits_{0};
  std::atomic<u32> next_bin_{0};
  std::atomic<u32> thieves_{0};      // bins claimed by waiting threads and not yet done
  std::atomic<u32> remaining_{0};
  u32 jobs_ = 0, nbins_ = 0;
  bool stop_ = false;
};

Renderer3D::Renderer3D(NDS& nds) : nds_(nds) { out_dst_ = out_[0].data(); reset(); }
Renderer3D::~Renderer3D() = default;

void Renderer3D::reset() {
  color_.fill(0); depth_.fill(0); attr_.fill(0); out_[0].fill(0); out_[1].fill(0);
  stencil_.fill(0);
  prev_shadow_mask_.fill(false);
  display_ = 0;
  out_dst_ = out_[0].data();
  pending_bands_ = 0; gen_ = 0;
  for (auto& b : bands_) b->reset();
}

u8 Renderer3D::tex8(u32 addr) const {
  addr &= texv_->addr_mask();
  const u8* p = texv_->ptr[addr / VramView::BLOCK];
  return p ? p[addr & (VramView::BLOCK - 1)] : vm_->read8(*texv_, addr);
}
u16 Renderer3D::tex16(u32 addr) const {
  addr &= texv_->addr_mask();
  const u8* p = texv_->ptr[addr / VramView::BLOCK];
  if (p) { u16 v; std::memcpy(&v, p + (addr & (VramView::BLOCK - 1)), 2); return v; }
  return vm_->read16(*texv_, addr);
}
u16 Renderer3D::pal16(u32 addr) const {
  addr &= palv_->addr_mask();
  const u8* p = palv_->ptr[addr / VramView::BLOCK];
  if (p) { u16 v; std::memcpy(&v, p + (addr & (VramView::BLOCK - 1)), 2); return v; }
  return vm_->read16(*palv_, addr);
}

// ---- pixel pipeline ------------------------------------------------------------
//
// Span stage buffers: one entry per pixel of the current span, index 0 at
// screen x `x0` (the kernels round their counts up to 4: padded).

namespace {
inline u32 c15_to_18(u16 c, u32 shift) { u32 v = (shift == 0 ? (c << 1) : (c >> shift)) & 0x3E; if (v) ++v; return v; }
inline void rgb15_to_666(u16 c, u32& r, u32& g, u32& b) { r = c15_to_18(c, 0); g = c15_to_18(c, 4); b = c15_to_18(c, 9); }
}

void Renderer3D::expand_toon() {
  for (u32 i = 0; i < 32; ++i) {
    u32 r, g, b; rgb15_to_666(rs_->toon[i], r, g, b);
    toon6_[0][i] = static_cast<u8>(r); toon6_[1][i] = static_cast<u8>(g); toon6_[2][i] = static_cast<u8>(b);
  }
  for (u32 i = 0; i < 8; ++i) {
    u32 r, g, b; rgb15_to_666(rs_->edge[i], r, g, b);
    edge6_[0][i] = static_cast<u8>(r); edge6_[1][i] = static_cast<u8>(g); edge6_[2][i] = static_cast<u8>(b);
  }
}

// Texel at 12.4 coordinates (s, t): RGB555 colour, 5-bit alpha.
[[gnu::always_inline]] inline u32 Renderer3D::texture_sample(const Shade& sh, s32 s, s32 t, u32* alpha) const {
  u32 addr = sh.base;
  const s32 width = sh.width, height = sh.height;
  s >>= 4; t >>= 4;
  if (sh.srep) { if (sh.sflip && (s & width)) s = (width - 1) - (s & (width - 1)); else s &= width - 1; }
  else { if (s < 0) s = 0; else if (s >= width) s = width - 1; }
  if (sh.trep) { if (sh.tflip && (t & height)) t = (height - 1) - (t & (height - 1)); else t &= height - 1; }
  else { if (t < 0) t = 0; else if (t >= height) t = height - 1; }
  const u32 texpal = sh.texpal, alpha0 = sh.alpha0;
  // Cached texels already contain the decoded RGB555 colour and alpha.  Use
  // them in the scalar sampler too; otherwise non-NEON builds pay the full
  // dependent VRAM/palette lookup cost despite setup_shade having populated
  // the same cache for the vector gather path.
  if (sh.texels) {
    const u32 packed = sh.texels[static_cast<u32>(t * width + s)];
    *alpha = packed >> 16;
    return packed & 0xFFFF;
  }
  if (sh.tex_ptr) {
    // Texel and palette reads straight from host memory.
    const u8* tp = sh.tex_ptr; const u16* pp = sh.pal_ptr;
    const u32 off = static_cast<u32>(t * width + s);
    switch (sh.fmt) {
    case 1: { const u8 px = tp[off]; *alpha = ((px >> 3) & 0x1C) + (px >> 6); return pp[px & 0x1F]; }
    case 2: { const u8 px = (tp[off >> 2] >> ((s & 3) << 1)) & 3; *alpha = px == 0 ? alpha0 : 31; return pp[px]; }
    case 3: { u8 px = tp[off >> 1]; px = (s & 1) ? (px >> 4) : (px & 0xF); *alpha = px == 0 ? alpha0 : 31; return pp[px]; }
    case 4: { const u8 px = tp[off]; *alpha = px == 0 ? alpha0 : 31; return pp[px]; }
    case 6: { const u8 px = tp[off]; *alpha = px >> 3; return pp[px & 7]; }
    case 7: { u16 c; std::memcpy(&c, tp + off * 2, 2); *alpha = (c & 0x8000) ? 31 : 0; return c; }
    default: break;
    }
  }
  switch (sh.fmt) {
  case 1: {   // A3I5
    const u8 px = tex8(addr + (t * width + s));
    *alpha = ((px >> 3) & 0x1C) + (px >> 6);
    return pal16((texpal << 4) + ((px & 0x1F) << 1));
  }
  case 2: {   // 4 colours
    u8 px = tex8(addr + ((t * width + s) >> 2));
    px = (px >> ((s & 3) << 1)) & 3;
    *alpha = px == 0 ? alpha0 : 31;
    return pal16((texpal << 3) + (px << 1));
  }
  case 3: {   // 16 colours
    u8 px = tex8(addr + ((t * width + s) >> 1));
    px = (s & 1) ? (px >> 4) : (px & 0xF);
    *alpha = px == 0 ? alpha0 : 31;
    return pal16((texpal << 4) + (px << 1));
  }
  case 4: {   // 256 colours
    const u8 px = tex8(addr + (t * width + s));
    *alpha = px == 0 ? alpha0 : 31;
    return pal16((texpal << 4) + (px << 1));
  }
  case 5: {   // 4x4 compressed
    addr += ((t & 0x3FC) * (width >> 2)) + (s & 0x3FC) + (t & 3);
    addr &= 0x7FFFF;
    u32 slot1 = 0x20000 + ((addr & 0x1FFFC) >> 1);
    if (addr >= 0x40000) slot1 += 0x10000;
    u8 val;
    if (addr >= 0x20000 && addr < 0x40000) val = 0;      // texels can't live in slot 1
    else val = tex8(addr) >> (2 * (s & 3));
    const u16 palinfo = tex16(slot1);
    const u32 paloff = (palinfo & 0x3FFF) << 2;
    const u32 base = (texpal << 4) + paloff;
    auto mix = [&](u32 ma, u32 mb, u32 sh_) -> u16 {
      const u16 c0 = pal16(base), c1 = pal16(base + 2);
      const u32 r = ((c0 & 0x1F) * ma + (c1 & 0x1F) * mb) >> sh_;
      const u32 g = (((c0 & 0x3E0) * ma + (c1 & 0x3E0) * mb) >> sh_) & 0x3E0;
      const u32 b = (((c0 & 0x7C00) * ma + (c1 & 0x7C00) * mb) >> sh_) & 0x7C00;
      return static_cast<u16>(r | g | b);
    };
    *alpha = 31;
    switch (val & 3) {
    case 0: return pal16(base);
    case 1: return pal16(base + 2);
    case 2:
      if ((palinfo >> 14) == 1) return mix(1, 1, 1);
      if ((palinfo >> 14) == 3) return mix(5, 3, 3);
      return pal16(base + 4);
    default:
      if ((palinfo >> 14) == 2) return pal16(base + 6);
      if ((palinfo >> 14) == 3) return mix(3, 5, 3);
      *alpha = 0; return 0;
    }
  }
  case 6: {   // A5I3
    const u8 px = tex8(addr + (t * width + s));
    *alpha = px >> 3;
    return pal16((texpal << 4) + ((px & 7) << 1));
  }
  case 7: {   // direct colour
    const u16 c = tex16(addr + ((t * width + s) << 1));
    *alpha = (c & 0x8000) ? 31 : 0;
    return c;
  }
  default: *alpha = 0; return 0;
  }
}


// Depth test: 'less than' normally (mode 0); 'less or equal' for a
// front-facing pixel over an opaque back-facing one (mode 1); within a
// tolerance when the polygon asks for equal-depth testing (mode 2 Z-buffered
// ±0x200, mode 3 W-buffered ±0xFF).
template <int mode>
[[gnu::always_inline]] inline bool Renderer3D::depth_pass(u32 addr, s32 z, u32 dstattr) const {
  const s32 dstz = static_cast<s32>(depth_[addr]);
  if constexpr (mode == 0) return z < dstz;
  else if constexpr (mode == 1) return (dstattr & 0x00400010) == 0x00000010 ? z <= dstz : z < dstz;
  else if constexpr (mode == 2) return static_cast<u32>((dstz - z) + 0x200) <= 0x400;
  else return static_cast<u32>((dstz - z) + 0xFF) <= 0x1FE;
}
namespace {
inline int pick_depth_mode(const Polygon& p) {
  if (p.attr & (1 << 14)) return p.wbuffer ? 3 : 2;
  return p.facing ? 1 : 0;
}
}

[[gnu::always_inline]] inline u32 Renderer3D::alpha_blend(u32 dispcnt, u32 src, u32 dst, u32 alpha) {
  u32 dsta = dst >> 24;
  if (dsta == 0) return src;
  u32 r = src & 0x3F, g = (src >> 8) & 0x3F, b = (src >> 16) & 0x3F;
  if (dispcnt & (1 << 3)) {
    const u32 a1 = alpha + 1;
    r = ((r * a1) + ((dst & 0x3F) * (32 - a1))) >> 5;
    g = ((g * a1) + (((dst >> 8) & 0x3F) * (32 - a1))) >> 5;
    b = ((b * a1) + (((dst >> 16) & 0x3F) * (32 - a1))) >> 5;
  }
  if (alpha > dsta) dsta = alpha;
  return r | (g << 8) | (b << 16) | (dsta << 24);
}

template <bool textured>
[[gnu::always_inline]] inline u32 Renderer3D::shade_pixel(const Shade& sh, u32 vr, u32 vg, u32 vb, s32 s, s32 t) const {
  u32 r, g, b, a;
  const u32 blendmode = sh.blendmode;
  if (blendmode == 2) {
    if (sh.highlight) { vg = vr; vb = vr; }        // highlight: all components from red, toon colour added later
    else { u32 tr, tg, tb; rgb15_to_666(sh.toon[vr >> 1], tr, tg, tb); vr = tr; vg = tg; vb = tb; }
  }
  if constexpr (textured) {
    u32 talpha;
    const u16 tcolor = static_cast<u16>(texture_sample(sh, s, t, &talpha));
    u32 tr, tg, tb; rgb15_to_666(tcolor, tr, tg, tb);
    if (blendmode & 1) {   // decal
      if (talpha == 0) { r = vr; g = vg; b = vb; }
      else if (talpha == 31) { r = tr; g = tg; b = tb; }
      else {
        r = ((tr * talpha) + (vr * (31 - talpha))) >> 5;
        g = ((tg * talpha) + (vg * (31 - talpha))) >> 5;
        b = ((tb * talpha) + (vb * (31 - talpha))) >> 5;
      }
      a = sh.polyalpha;
    } else {               // modulate
      r = ((tr + 1) * (vr + 1) - 1) >> 6;
      g = ((tg + 1) * (vg + 1) - 1) >> 6;
      b = ((tb + 1) * (vb + 1) - 1) >> 6;
      a = ((talpha + 1) * (sh.polyalpha + 1) - 1) >> 5;
    }
  } else { r = vr; g = vg; b = vb; a = sh.polyalpha; }
  if (blendmode == 2 && sh.highlight) {
    u32 tr, tg, tb; rgb15_to_666(sh.toon[vr >> 1], tr, tg, tb);
    r += tr; g += tg; b += tb;
    if (r > 63) r = 63;
    if (g > 63) g = 63;
    if (b > 63) b = 63;
  }
  if (sh.wireframe) a = 31;
  return r | (g << 8) | (b << 16) | (a << 24);
}

[[gnu::always_inline]] inline void Renderer3D::plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow) {
  const u32 dstattr = attr_[addr];
  u32 attr = (polyattr & 0xE0F0) | ((polyattr >> 8) & 0xFF0000) | (1u << 22) | (dstattr & 0xFF001F0F);
  if (shadow) {
    // Shadows skip pixels of their own polygon id, opaque or translucent.
    if (dstattr & (1u << 22)) { if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return; }
    else if ((dstattr & 0x3F000000) == (polyattr & 0x3F000000)) return;
  } else if ((dstattr & 0x007F0000) == (attr & 0x007F0000)) return;   // equal translucent ids don't blend
  if (!(dstattr & (1u << 15))) attr &= ~(1u << 15);
  color = alpha_blend(dispcnt_, color, color_[addr], color >> 24);
  if (z != 0xFFFFFFFFu) depth_[addr] = z;
  color_[addr] = color;
  attr_[addr] = attr;
}

// ---- polygon setup ------------------------------------------------------------------

void Renderer3D::refresh_edge_state(Edge& e) const {
  const Polygon& p = *e.poly;
  e.vcl = &gx_->vertex(p.vtx[e.cur_vl]); e.vnl = &gx_->vertex(p.vtx[e.next_vl]);
  e.vcr = &gx_->vertex(p.vtx[e.cur_vr]); e.vnr = &gx_->vertex(p.vtx[e.next_vr]);
  e.wcl = p.w[e.cur_vl]; e.wnl = p.w[e.next_vl];
  e.wcr = p.w[e.cur_vr]; e.wnr = p.w[e.next_vr];
  e.zcl = p.z[e.cur_vl]; e.znl = p.z[e.next_vl];
  e.zcr = p.z[e.cur_vr]; e.znr = p.z[e.next_vr];
  e.nx_l = e.left.negative  || !e.left.xmajor;
  e.nx_r = e.right.negative || !e.right.xmajor;
  e.px_l = !e.left.negative  && e.left.xmajor;
  e.px_r = !e.right.negative && e.right.xmajor;
  e.lneg_xm = e.left.negative && e.left.xmajor;
  e.lxm = e.left.xmajor; e.rxm = e.right.xmajor;
  e.same_incr = e.left.increment == e.right.increment;
  e.l_incr0 = e.left.increment == 0;
  e.r_incr0 = e.right.increment == 0;
  e.next_sx_differ = e.vnl->sx != e.vnr->sx;
}

void Renderer3D::setup_left_edge(Edge& e, s32 y) const {
  const Polygon& p = *e.poly;
  while (y >= gx_->vertex(p.vtx[e.next_vl]).sy && e.cur_vl != p.vbot) {
    e.cur_vl = e.next_vl;
    if (p.facing) { e.next_vl = e.cur_vl + 1; if (e.next_vl >= p.nverts) e.next_vl = 0; }
    else { e.next_vl = e.cur_vl - 1; if (static_cast<s32>(e.next_vl) < 0) e.next_vl = p.nverts - 1; }
  }
  const Vertex &a = gx_->vertex(p.vtx[e.cur_vl]), &b = gx_->vertex(p.vtx[e.next_vl]);
  e.xl = e.left.setup(a.sx, b.sx, a.sy, b.sy, p.w[e.cur_vl], p.w[e.next_vl], y, p.wbuffer);
  refresh_edge_state(e);
}

void Renderer3D::setup_right_edge(Edge& e, s32 y) const {
  const Polygon& p = *e.poly;
  while (y >= gx_->vertex(p.vtx[e.next_vr]).sy && e.cur_vr != p.vbot) {
    e.cur_vr = e.next_vr;
    if (p.facing) { e.next_vr = e.cur_vr - 1; if (static_cast<s32>(e.next_vr) < 0) e.next_vr = p.nverts - 1; }
    else { e.next_vr = e.cur_vr + 1; if (e.next_vr >= p.nverts) e.next_vr = 0; }
  }
  const Vertex &a = gx_->vertex(p.vtx[e.cur_vr]), &b = gx_->vertex(p.vtx[e.next_vr]);
  e.xr = e.right.setup(a.sx, b.sx, a.sy, b.sy, p.w[e.cur_vr], p.w[e.next_vr], y, p.wbuffer);
  refresh_edge_state(e);
}

// Put a polygon's edge cursors back where setup_polygon leaves them: at its
// own top line, ready to be walked downwards.
//
// The chunk loop assumes an entering polygon's cursors still sit at ytop, and
// with one band per Renderer3D that held -- build_edges put them there and
// nothing had stepped them yet. With the frame cut into bins claimed by
// whichever worker is free, one instance renders several bins, and
// consecutive bins overlap by the line or two that final_pass needs either
// side of a boundary. A polygon whose ytop falls in that overlap is entered
// twice on the same instance, and the second entry would otherwise walk from
// wherever the first entry left the cursors.
void Renderer3D::rewind_edge(Edge& e) {
  const Polygon& p = *e.poly;
  const u32 n = p.nverts;
  u32 vtop = p.vtop, vbot = p.vbot;
  e.cur_vl = vtop; e.cur_vr = vtop;
  if (p.facing) {
    e.next_vl = e.cur_vl + 1; if (e.next_vl >= n) e.next_vl = 0;
    e.next_vr = e.cur_vr - 1; if (static_cast<s32>(e.next_vr) < 0) e.next_vr = n - 1;
  } else {
    e.next_vl = e.cur_vl - 1; if (static_cast<s32>(e.next_vl) < 0) e.next_vl = n - 1;
    e.next_vr = e.cur_vr + 1; if (e.next_vr >= n) e.next_vr = 0;
  }
  if (p.ybot == p.ytop) {
    // Flat polygon: a single span from the leftmost to the rightmost vertex.
    vtop = 0; vbot = 0;
    for (u32 i : {1u, n - 1}) {
      if (gx_->vertex(p.vtx[i]).sx < gx_->vertex(p.vtx[vtop]).sx) vtop = i;
      if (gx_->vertex(p.vtx[i]).sx > gx_->vertex(p.vtx[vbot]).sx) vbot = i;
    }
    e.cur_vl = vtop; e.next_vl = vtop; e.cur_vr = vbot; e.next_vr = vbot;
    e.xl = e.left.setup_dummy(gx_->vertex(p.vtx[e.cur_vl]).sx, p.wbuffer);
    e.xr = e.right.setup_dummy(gx_->vertex(p.vtx[e.cur_vr]).sx, p.wbuffer);
  } else {
    setup_left_edge(e, p.ytop);
    setup_right_edge(e, p.ytop);
  }
  refresh_edge_state(e);
}

void Renderer3D::setup_polygon(Edge& e, const Polygon& p) {
  const u32 n = p.nverts;
  u32 vtop = p.vtop, vbot = p.vbot;
  e.poly = &p;
  setup_shade(e.sh, p);
  e.cur_vl = vtop; e.cur_vr = vtop;
  if (p.facing) {
    e.next_vl = e.cur_vl + 1; if (e.next_vl >= n) e.next_vl = 0;
    e.next_vr = e.cur_vr - 1; if (static_cast<s32>(e.next_vr) < 0) e.next_vr = n - 1;
  } else {
    e.next_vl = e.cur_vl - 1; if (static_cast<s32>(e.next_vl) < 0) e.next_vl = n - 1;
    e.next_vr = e.cur_vr + 1; if (e.next_vr >= n) e.next_vr = 0;
  }
  if (p.ybot == p.ytop) {
    // Flat polygon: a single span from the leftmost to the rightmost vertex.
    vtop = 0; vbot = 0;
    for (u32 i : {1u, n - 1}) {
      if (gx_->vertex(p.vtx[i]).sx < gx_->vertex(p.vtx[vtop]).sx) vtop = i;
      if (gx_->vertex(p.vtx[i]).sx > gx_->vertex(p.vtx[vbot]).sx) vbot = i;
    }
    e.cur_vl = vtop; e.next_vl = vtop; e.cur_vr = vbot; e.next_vr = vbot;
    e.xl = e.left.setup_dummy(gx_->vertex(p.vtx[e.cur_vl]).sx, p.wbuffer);
    e.xr = e.right.setup_dummy(gx_->vertex(p.vtx[e.cur_vr]).sx, p.wbuffer);
  } else {
    setup_left_edge(e, p.ytop);
    setup_right_edge(e, p.ytop);
  }
  refresh_edge_state(e);
}

// ---- scanline rendering ------------------------------------------------------------

// Span stage: the perspective factor, depth and (optionally) the five
// attributes for screen pixels [xa, xb) of the span [xstart, xend], through
// the kernels (kern::active). This is Interp<0> (setup, set_x, interpolate,
// interpolate_z) evaluated for the whole span at once.
void Renderer3D::span_stage(SpanBuf& sb, s32 xstart, s32 xend, s32 xa, s32 xb, s32 wl, s32 wr, s32 zl, s32 zr, bool wbuffer,
                            const s32* al, const s32* ar, bool with_attrs, u32 off) const {
  // x0 is the screen x that maps to buffer index 0, so a span staged at batch
  // offset `off` sets it back by that much and every stage indexes correctly.
  sb.x0 = xa - static_cast<s32>(off);
  const u32 n = static_cast<u32>(xb - xa);
  const s32 xdiff = (xend + 1) - xstart;
  const s32 xv0 = xa - xstart;
  const bool linear = (wl == wr) && !(wl & 0x7F) && !(wr & 0x7F);
  // The factor feeds span_attr_persp (w-buffer depth) and the perspective
  // attribute kernels. A linear span only needs it for a varying w-buffer
  // depth; with z constant as well nothing reads it -- GSDD's title stages
  // 45 k such pixels a frame (census 2026-08-28).
  const bool use_factor = xdiff != 0 && (!linear || (wbuffer && zl != zr));
  if (use_factor) kern::active::span_factor(xv0, n, xdiff, wl, wl, wr, sb.fac + off);
  // A constant depth needs no reciprocal or per-pixel interpolation.  Flat
  // geometry is common in the DS scenes, and keeping this out of the span
  // kernel also makes the depth pre-pass a straight fill.
  if (xdiff == 0 || zl == zr) kern::active::span_z_const(zl, n, sb.z + off);
  else if (wbuffer) kern::active::span_attr_persp(zl, zr, sb.fac + off, n, sb.z + off);
  else kern::active::span_z_linear(zl, zr, xv0, n, xdiff, (1 << 22) / xdiff, sb.z + off);
  if (with_attrs) span_attrs(sb, xstart, xend, xa, xb, wl, wr, al, ar, false, false);
}

// Constant-attribute fills for span_attrs. These used to be three libc memset
// calls plus a scalar loop over sc/tc -- for the 60 % of SM64DS's polygon
// lines that are eight pixels or shorter, that was ~170 instructions of call
// overhead and loop to store five constants (qemu hotblocks, 2026-08-27).
// The buffers carry sixteen entries of slack past any staged span, so whole
// vectors are stored from `n` rounded up.
namespace {
[[gnu::always_inline]] inline void fill_rgb_const(u8* vr, u8* vg, u8* vb, u32 n, const s32* a) {
#if DSPERATE_NEON
  const uint8x16_t r = vdupq_n_u8(static_cast<u8>(static_cast<u32>(a[0]) >> 3));
  const uint8x16_t g = vdupq_n_u8(static_cast<u8>(static_cast<u32>(a[1]) >> 3));
  const uint8x16_t b = vdupq_n_u8(static_cast<u8>(static_cast<u32>(a[2]) >> 3));
  for (u32 i = 0; i < n; i += 16) { vst1q_u8(vr + i, r); vst1q_u8(vg + i, g); vst1q_u8(vb + i, b); }
#else
  const u32 fill = (n + 7) & ~7u;
  std::memset(vr, static_cast<u8>((static_cast<u32>(a[0]) >> 3) & 0xFF), fill);
  std::memset(vg, static_cast<u8>((static_cast<u32>(a[1]) >> 3) & 0xFF), fill);
  std::memset(vb, static_cast<u8>((static_cast<u32>(a[2]) >> 3) & 0xFF), fill);
#endif
}
[[gnu::always_inline]] inline void fill_st_const(s16* sc, s16* tc, u32 n, const s32* a) {
#if DSPERATE_NEON
  const int16x8_t s = vdupq_n_s16(static_cast<s16>(a[3])), t = vdupq_n_s16(static_cast<s16>(a[4]));
  for (u32 i = 0; i < n; i += 8) { vst1q_s16(sc + i, s); vst1q_s16(tc + i, t); }
#else
  for (u32 i = 0; i < n; ++i) { sc[i] = static_cast<s16>(a[3]); tc[i] = static_cast<s16>(a[4]); }
#endif
}
} // namespace

// The five attributes for screen pixels [ca, cb) of the span (a sub-range of
// the staged span, normally the depth pre-pass's candidate range).
void Renderer3D::span_attrs(SpanBuf& sb, s32 xstart, s32 xend, s32 ca, s32 cb, s32 wl, s32 wr, const s32* al, const s32* ar, bool attrs_constant, bool rgb_constant) const {
  const u32 off = static_cast<u32>(ca - sb.x0), n = static_cast<u32>(cb - ca);
  const s32 xdiff = (xend + 1) - xstart;
  const s32 xv0 = ca - xstart;

  // Match DraStic's constant-attribute polygon variant: when both ends of the
  // surviving range agree, no interpolation stage is needed at all.  Keep
  // the narrowed representations used by the pixel stages, rather than
  // materialising the old s32 planes just to truncate them again.
  if (attrs_constant || (al[0] == ar[0] && al[1] == ar[1] && al[2] == ar[2] &&
      al[3] == ar[3] && al[4] == ar[4])) {
    fill_rgb_const(sb.vr + off, sb.vg + off, sb.vb + off, n, al);
    fill_st_const(sb.sc + off, sb.tc + off, n, al);
    return;
  }
  const bool linear = (wl == wr) && !(wl & 0x7F) && !(wr & 0x7F);
  if (xdiff != 0 && !linear) {
    // A span whose colour endpoints agree -- most of them, and every span of
    // flat-shaded content -- interpolates nothing across r, g and b: fill the
    // three buffers with the constant and stage only s and t. This is
    // DraStic's set_buffer8 against interpolate_rgb, which it takes for 83 %
    // of SM64DS's spans and 100 % of Meteos's.
    if (rgb_constant || (al[0] == ar[0] && al[1] == ar[1] && al[2] == ar[2])) {
      prof::add(prof::C_SPAN_FLAT_RGB, 1);
      // The kernels write whole vectors past n; match that, the buffers carry
      // the slack for it.
      fill_rgb_const(sb.vr + off, sb.vg + off, sb.vb + off, n, al);
      kern::active::span_attrs2n(al, ar, sb.fac + off, n, sb.sc + off, sb.tc + off);
      return;
    }
    prof::add(prof::C_SPAN_LERP_RGB, 1);
    // One pass for all five, narrowed as it is stored.
    kern::active::span_attrs5n(al, ar, sb.fac + off, n, sb.vr + off, sb.vg + off, sb.vb + off, sb.sc + off, sb.tc + off);
    return;
  }
  if (xdiff == 0) {
    // Degenerate span: every pixel is the left endpoint.
    fill_rgb_const(sb.vr + off, sb.vg + off, sb.vb + off, n, al);
    fill_st_const(sb.sc + off, sb.tc + off, n, al);
    return;
  }
  // The linear span gets the same two-way split the perspective one above
  // does. It used to stage all five attributes as s32 into a 5 KB scratch
  // buffer and narrow them in a scalar loop, with a 64-bit reciprocal divide
  // per attribute -- and it never took the constant-colour shortcut at all,
  // although constant-W content (which is what makes a span linear) is
  // exactly where flat colour is most common. Meteos takes this branch for
  // every span it draws.
  if (rgb_constant || (al[0] == ar[0] && al[1] == ar[1] && al[2] == ar[2])) {
    prof::add(prof::C_SPAN_FLAT_RGB, 1);
    fill_rgb_const(sb.vr + off, sb.vg + off, sb.vb + off, n, al);
    kern::active::span_attrs2n_lin(al, ar, xv0, n, xdiff, sb.sc + off, sb.tc + off);
    return;
  }
  prof::add(prof::C_SPAN_LERP_RGB, 1);
  kern::active::span_attrs5n_lin(al, ar, xv0, n, xdiff, sb.vr + off, sb.vg + off, sb.vb + off, sb.sc + off, sb.tc + off);
}

void Renderer3D::render_shadow_mask_line(Edge& e, s32 y) {
  const Polygon& p = *e.poly;
  u32 polyalpha = (p.attr >> 16) & 0x1F;
  const bool wireframe = polyalpha == 0;

  const u32 srow = static_cast<u32>((y + 1) & (RING - 1));
  u8* const shadow_stencil = &stencil_[256 * srow];
  if (!prev_shadow_mask_[srow]) std::memset(shadow_stencil, 0, 256);
  prev_shadow_mask_[srow] = true;

  if (p.ytop != p.ybot) {
    if (y >= gx_->vertex(p.vtx[e.next_vl]).sy && e.cur_vl != p.vbot) setup_left_edge(e, y);
    if (y >= gx_->vertex(p.vtx[e.next_vr]).sy && e.cur_vr != p.vbot) setup_right_edge(e, y);
  }
  s32 xstart = e.xl, xend = e.xr;
  s32 wl = e.left.interp.interpolate(p.w[e.cur_vl], p.w[e.next_vl]);
  s32 wr = e.right.interp.interpolate(p.w[e.cur_vr], p.w[e.next_vr]);
  s32 zl = e.left.interp.interpolate_z(p.z[e.cur_vl], p.z[e.next_vl]);
  s32 zr = e.right.interp.interpolate_z(p.z[e.cur_vr], p.z[e.next_vr]);
  if (e.right.increment == 0 && (e.left.increment != 0 || xstart != xend) && xend != 0) --xend;

  bool l_fill, r_fill; s32 l_len, r_len, l_cov, r_cov;
  const bool always_fill = (dispcnt_ & ((1 << 4) | (1 << 5))) || (polyalpha < 31 && (dispcnt_ & (1 << 3))) || wireframe;
  // The stencil pass needs the edge lengths only, never the coverage.
  if (xstart > xend) {
    const Vertex &vlnext = gx_->vertex(p.vtx[e.next_vr]), &vrnext = gx_->vertex(p.vtx[e.next_vl]);
    e.right.edge_params<true>(false, &l_len, &l_cov);
    e.left.edge_params<true>(false, &r_len, &r_cov);
    std::swap(xstart, xend); std::swap(wl, wr); std::swap(zl, zr);
    if (always_fill) { l_fill = r_fill = true; }
    else {
      l_fill = (e.right.negative || !e.right.xmajor) || ((y == p.ybot - 1) && e.right.xmajor && (vlnext.sx != vrnext.sx));
      r_fill = (!e.left.negative && e.left.xmajor) || (!(e.left.negative && e.left.xmajor) && e.right.increment == 0) ||
               ((y == p.ybot - 1) && e.left.xmajor && (vlnext.sx != vrnext.sx));
    }
  } else {
    const Vertex &vlnext = gx_->vertex(p.vtx[e.next_vl]), &vrnext = gx_->vertex(p.vtx[e.next_vr]);
    e.left.edge_params<false>(false, &l_len, &l_cov);
    e.right.edge_params<false>(false, &r_len, &r_cov);
    if (always_fill) { l_fill = r_fill = true; }
    else {
      l_fill = ((e.left.negative || !e.left.xmajor) || ((y == p.ybot - 1) && e.left.xmajor && (vlnext.sx != vrnext.sx))) ||
               ((e.left.increment == e.right.increment) && (xstart + l_len == xend + 1));
      r_fill = (!e.right.negative && e.right.xmajor) || (e.right.increment == 0) ||
               ((y == p.ybot - 1) && e.right.xmajor && (vlnext.sx != vrnext.sx));
    }
  }

  if (wireframe) polyalpha = 31;
  if (polyalpha <= rs_->alpha_ref) { e.xl = e.left.step(); e.xr = e.right.step(); return; }

  int yedge = 0;
  if (y == p.ytop) yedge = 0x4; else if (y == p.ybot - 1) yedge = 0x8;
  s32 x = xstart;
  if (x < 0) x = 0;
  const s32 xa = x, xb = std::min(xend + 1, 256);
  SpanBuf sb;
  if (xb > xa) span_stage(sb, xstart, xend, xa, xb, wl, wr, zl, zr, p.wbuffer, nullptr, nullptr, false, 0);
  const int mode = pick_depth_mode(p);

  // Set stencil bits where the depth test fails; draw nothing. Bit 2 (the
  // pixel underneath) only ever steers writes to the under layer, which is
  // dead without AA -- see dispcnt_.
  const bool under = dispcnt_ & (1 << 4);
  const u32 row = row_of(y) + 1;
  auto stencil_span = [&](s32 xlimit) {
    for (; x < xlimit; ++x) {
      u32 addr = row + static_cast<u32>(x);
      const s32 z = sb.z[x - sb.x0];
      const u32 dstattr = attr_[addr];
      auto fails = [&](u32 a, u32 da) {
        switch (mode) {
        case 0: return !depth_pass<0>(a, z, da);
        case 1: return !depth_pass<1>(a, z, da);
        case 2: return !depth_pass<2>(a, z, da);
        default: return !depth_pass<3>(a, z, da);
        }
      };
      if (fails(addr, dstattr)) shadow_stencil[x] = 1;
      if (under && (dstattr & 0xF)) {
        addr += RSIZE;
        if (fails(addr, attr_[addr])) shadow_stencil[x] |= 2;
      }
    }
  };
  s32 xlimit = std::min({xstart + l_len, xend + 1, 256});
  if (!l_fill) x = xlimit; else stencil_span(xlimit);
  xlimit = std::min({xend - r_len + 1, xend + 1, 256});
  if (wireframe && !yedge) x = std::max(x, xlimit); else stencil_span(xlimit);
  xlimit = std::min(xend + 1, 256);
  if (r_fill) stencil_span(xlimit);

  e.xl = e.left.step();
  e.xr = e.right.step();
}

// Per-pixel resolve of screen pixels [xa, xb) of the current span: stencil,
// depth test (against the top pixel, then the one underneath), shading,
// alpha test and the opaque / translucent writes.
// part: 0 = left edge, 1 = inside, 2 = right edge.
template <int mode, bool textured, bool aa, bool shadow>
void Renderer3D::resolve_span(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov) {
  const u32 polyattr = sh.polyattr;
  const u8* stencil = &stencil_[256 * static_cast<u32>((y + 1) & (RING - 1))];
  const u32 row = row_of(y) + 1;
  const u8* ra = sb.vr; const u8* ga = sb.vg; const u8* ba = sb.vb;
  const s16* sa = sb.sc; const s16* ta = sb.tc;
  u32 resolved = 0;                // counted once after the loop, not per pixel
  for (s32 x = xa; x < xb; ++x) {
    const u32 i = static_cast<u32>(x - sb.x0);
    if (!sb.pass[i]) continue;    // neither the top pixel nor the one underneath can take it
    u32 addr = row + static_cast<u32>(x);
    u32 dstattr = attr_[addr];
    if (shadow) {
      const u8 st = stencil[x];
      if (!st) continue;
      if (!(st & 1)) { if (!aa) continue; addr += RSIZE; }   // under layer only: dead without AA
      if (!(st & 2)) dstattr &= ~0xFu;      // no shadow under anti-aliased edges
    }
    const s32 z = sb.z[i];
    // Failing against the top pixel, try the one underneath -- which only
    // exists with AA (dispcnt_); without it the pre-pass never named it.
    if (!depth_pass<mode>(addr, z, dstattr)) {
      if (!aa || !(dstattr & 0xF) || addr >= static_cast<u32>(RSIZE)) continue;
      addr += RSIZE;
      dstattr = attr_[addr];
      if (!depth_pass<mode>(addr, z, dstattr)) continue;
    }
    const u32 vr = ra[i], vg = ga[i], vb = ba[i];
    const s32 s = sa[i], t = ta[i];
    const u32 color = shade_pixel<textured>(sh, vr, vg, vb, s, t);
    ++resolved;
    const u32 alpha = color >> 24;
    if (alpha <= sh.alpha_ref) continue;
    if (alpha == 31) {
      u32 attr = polyattr | edge;
      bool push = false;
      if (aa) {
        if (part == 1) { if (attr & 0xF) { attr |= (0x1F << 8); push = true; } }
        else {
          s32 cov = part == 0 ? l_cov : r_cov;
          if (cov & static_cast<s32>(0x80000000u)) {
            if (part == 0) { cov = xcov >> 5; if (cov > 31) cov = 31; xcov += (l_cov & 0x3FF); }
            else { cov = 0x1F - (xcov >> 5); if (cov < 0) cov = 0; xcov += (r_cov & 0x3FF); }
          }
          attr |= (cov << 8);
          push = true;
        }
      }
      if (push && addr < static_cast<u32>(RSIZE)) {
        color_[addr + RSIZE] = color_[addr]; depth_[addr + RSIZE] = depth_[addr]; attr_[addr + RSIZE] = attr_[addr];
      }
      depth_[addr] = z; color_[addr] = color; attr_[addr] = attr;
    } else {
      const u32 zz = (sh.polyattr_z) ? static_cast<u32>(z) : 0xFFFFFFFFu;
      plot_translucent(addr, color, zz, polyattr, shadow);
      if (aa && (dstattr & 0xF) && addr < static_cast<u32>(RSIZE)) plot_translucent(addr + RSIZE, color, zz, polyattr, shadow);
    }
  }
  if (resolved) prof::add(prof::C_RESOLVED_PIXELS, resolved);
}

// The texture half of a Shade: format, size, VRAM addressing and the direct
// host pointers. Returns whether the polygon samples through the decoded
// cache, which is the one thing only the emulation thread may resolve
// (TextureCache::lookup mutates its map and frame stamps): render() runs this
// over the list once to record the pointers, and every worker -- job 0
// included, now that edge setup runs on its pool thread -- reads them back.
bool Renderer3D::texture_fields(Shade& sh, const Polygon& p) const {
  sh.fmt = (p.texparam >> 26) & 7;
  sh.textured = (dispcnt_ & 1) && sh.fmt != 0;
  sh.base = (p.texparam & 0xFFFF) << 3;
  sh.width = 8 << ((p.texparam >> 20) & 7);
  sh.height = 8 << ((p.texparam >> 23) & 7);
  sh.srep = p.texparam & (1 << 16); sh.sflip = p.texparam & (1 << 18);
  sh.trep = p.texparam & (1 << 17); sh.tflip = p.texparam & (1 << 19);
  sh.alpha0 = (p.texparam & (1 << 29)) ? 0 : 31;
  sh.texpal = p.texpal;
  // Direct pointers: every 16 KB block of the texture's range must be mapped
  // to one bank and follow the previous one in host memory.
  auto direct_range = [](const VramView& v, u32 addr, u32 len) -> const u8* {
    addr &= v.addr_mask();
    if (addr + len > v.size) return nullptr;
    const u8* p0 = v.ptr[addr / VramView::BLOCK];
    if (!p0) return nullptr;
    for (u32 b = addr / VramView::BLOCK + 1; b <= (addr + len - 1) / VramView::BLOCK; ++b)
      if (v.ptr[b] != p0 + (b - addr / VramView::BLOCK) * VramView::BLOCK) return nullptr;
    return p0 + (addr & (VramView::BLOCK - 1));
  };
  sh.tex_ptr = nullptr; sh.pal_ptr = nullptr; sh.texels = nullptr;
  sh.texv = texv_; sh.palv = palv_; sh.vm = vm_;
  if (sh.textured && sh.fmt != 5) {
    static const u32 bpp_num[8] = {0, 8, 2, 4, 8, 0, 8, 16};   // bits per texel
    const u32 bytes = (static_cast<u32>(sh.width * sh.height) * bpp_num[sh.fmt]) / 8;
    sh.tex_ptr = direct_range(*texv_, sh.base, bytes);
    const u32 pal_bytes = sh.fmt == 2 ? 8 : (sh.fmt == 3 ? 32 : (sh.fmt == 6 ? 16 : (sh.fmt == 1 ? 64 : 512)));
    const u32 pal_addr = sh.fmt == 2 ? (sh.texpal << 3) : (sh.texpal << 4);
    sh.pal_ptr = reinterpret_cast<const u16*>(direct_range(*palv_, pal_addr, pal_bytes));
  }
  // The decoded cache wins where sampling from VRAM is a chain of dependent
  // loads: always for the compressed format, and for any texture the direct
  // pointers cannot cover. A byte texel plus an L1-resident palette is
  // cheaper than a word from a four-times-larger decoded array, so the
  // other formats keep the direct path (measured: Mario & Luigi and Meteos
  // lost 0.5-1 % with every texture cached).
  return sh.textured && (sh.fmt == 5 || !sh.tex_ptr || !sh.pal_ptr);
}

void Renderer3D::setup_shade(Shade& sh, const Polygon& p) {
  sh.polyattr = p.attr & 0x3F008000;
  if (!p.facing) sh.polyattr |= (1 << 4);
  sh.polyattr_z = (p.attr & (1 << 11)) != 0;
  sh.polyalpha = (p.attr >> 16) & 0x1F;
  sh.wireframe = sh.polyalpha == 0;
  sh.shadow = p.shadow;
  sh.dispcnt = dispcnt_;   // AA bit as the raster honours it, not as written
  sh.alpha_ref = rs_->alpha_ref;
  sh.blendmode = (p.attr >> 4) & 3;
  sh.highlight = sh.dispcnt & (1 << 1);
  sh.toon = rs_->toon.data();
  const bool cached = texture_fields(sh, p);
  // DraStic decides constant attributes at polygon setup.  The endpoint values
  // are then identical on every scanline, so span_attrs can skip five repeated
  // comparisons (and take the fill path immediately).
  sh.attrs_constant = true;
  sh.rgb_constant = true;
  const Vertex& v0 = gx_->vertex(p.vtx[0]);
  for (u32 i = 1; i < p.nverts; ++i) {
    const Vertex& v = gx_->vertex(p.vtx[i]);
    for (int c = 0; c < 3; ++c) {
      if (v.fcol[c] != v0.fcol[c]) sh.rgb_constant = false;
    }
    if (v.tex[0] != v0.tex[0] || v.tex[1] != v0.tex[1]) sh.attrs_constant = false;
  }
  sh.attrs_constant = sh.rgb_constant && sh.attrs_constant;
  // The cache was resolved on the emulation thread (render()); every band
  // worker only reads the pointer that pass recorded.
  if (cached && texels_in_) sh.texels = setup_poly_ < texels_in_->size() ? (*texels_in_)[setup_poly_] : nullptr;
#if DSPERATE_NEON
  sh.gather4 = select_gather4(sh);
  sh.vec = !sh.shadow && !sh.wireframe;   // toon / highlight (blendmode 2) are vector stages in flush_batch
#else
  sh.gather4 = nullptr;
  sh.vec = false;
#endif
  sh.always_fill = (sh.dispcnt & ((1 << 4) | (1 << 5))) || (sh.polyalpha < 31 && (sh.dispcnt & (1 << 3))) || sh.wireframe;
  // Provably all-opaque batch: with the polygon's alpha at 31, decal writes
  // exactly that alpha for every pixel, and every texture format except
  // A3I5 / A5I3 carries only 0-or-31 texel alpha, which modulate preserves.
  // The alpha-0 lanes never reach the resolve -- span_shade narrows the pass
  // plane by the alpha test -- so the opaque resolve needs no alpha logic.
  sh.opaque = sh.polyalpha == 31 && (!sh.textured || (sh.blendmode & 1) || (sh.fmt != 1 && sh.fmt != 6));
  sh.mode = pick_depth_mode(p);
  sh.resolve = select_resolve(sh);
}


#if DSPERATE_NEON
namespace {
// 15-bit colour channel -> 6-bit (c15_to_18 on lanes): bits 1-5 of the field, +1 when non-zero.
inline uint32x4_t c15_to_18_4(uint32x4_t c, int shift) {
  uint32x4_t v = shift == 0 ? vshlq_n_u32(c, 1) : (shift == 4 ? vshrq_n_u32(c, 4) : vshrq_n_u32(c, 9));
  v = vandq_u32(v, vdupq_n_u32(0x3E));
  return vaddq_u32(v, vandq_u32(vtstq_u32(v, v), vdupq_n_u32(1)));
}
// Exclusive prefix count of set lanes, and the total.
inline uint32x4_t prefix_exclusive(uint32x4_t ones, u32* total) {
  const uint32x4_t zero = vdupq_n_u32(0);
  uint32x4_t s1 = vaddq_u32(ones, vextq_u32(zero, ones, 3));
  s1 = vaddq_u32(s1, vextq_u32(zero, s1, 2));          // inclusive scan
  *total = vgetq_lane_u32(s1, 3);
  return vsubq_u32(s1, ones);
}
inline uint32x4_t pack_colour(uint32x4_t r, uint32x4_t g, uint32x4_t b, uint32x4_t a) {
  return vorrq_u32(vorrq_u32(r, vshlq_n_u32(g, 8)), vorrq_u32(vshlq_n_u32(b, 16), vshlq_n_u32(a, 24)));
}
}

// Four texels for one (format, S wrap, T wrap) combination, chosen once per
// polygon (Shade::gather4): the wrap arithmetic and the texel decode are
// compile-time, the lanes travel through registers (umov out, fmov back)
// rather than a stack round trip, and there is no switch per block. Direct
// host pointers only; the compressed format and textures that are not
// host-contiguous take the per-lane sampler below.
namespace {
enum Wrap { CLAMP = 0, REPEAT = 1, FLIP = 2 };
// The compressed format keeps the colour table of the last 4x4 block it
// decoded in `scratch` (adjacent lanes nearly always share a block); the
// other formats ignore it.
struct Tex5Block { u32 key; u32 colour[4]; u32 alpha[4]; };
using Gather4Fn = void (*)(const Renderer3D::Shade&, const s16*, const s16*, uint32x4_t&, uint32x4_t&, Tex5Block*);

template <int wrap> inline int32x4_t wrap_lanes(int32x4_t v, int32x4_t size, int32x4_t size1) {
  if constexpr (wrap == REPEAT) return vandq_s32(v, size1);
  else if constexpr (wrap == FLIP) { const int32x4_t m = vandq_s32(v, size1); return vbslq_s32(vtstq_s32(v, size), vsubq_s32(size1, m), m); }
  else return vminq_s32(vmaxq_s32(v, vdupq_n_s32(0)), size1);
}

// One 4x4 block of the compressed format: its palette-info halfword lives
// in texture slot 1 at half the block's offset, and picks the palette base
// and one of four colour modes (GBATEK "Texture Format 5").
inline void tex5_decode_block(const Renderer3D::Shade& sh, u32 block, Tex5Block& b) {
  b.key = block;
  u32 slot1 = 0x20000 + ((block & 0x1FFFC) >> 1);
  if (block >= 0x40000) slot1 += 0x10000;
  const u16 palinfo = vram_fetch16(*sh.vm, *sh.texv, slot1);
  const u32 base = (sh.texpal << 4) + ((palinfo & 0x3FFF) << 2), mode = palinfo >> 14;
  const u32 c0 = vram_fetch16(*sh.vm, *sh.palv, base), c1 = vram_fetch16(*sh.vm, *sh.palv, base + 2);
  auto mix = [&](u32 ma, u32 mb, u32 shift) -> u32 {
    const u32 r = ((c0 & 0x1F) * ma + (c1 & 0x1F) * mb) >> shift;
    const u32 g = (((c0 & 0x3E0) * ma + (c1 & 0x3E0) * mb) >> shift) & 0x3E0;
    const u32 bl = (((c0 & 0x7C00) * ma + (c1 & 0x7C00) * mb) >> shift) & 0x7C00;
    return r | g | bl;
  };
  b.colour[0] = c0; b.colour[1] = c1;
  b.alpha[0] = b.alpha[1] = b.alpha[2] = 31;
  switch (mode) {
  case 0:  b.colour[2] = vram_fetch16(*sh.vm, *sh.palv, base + 4); b.colour[3] = 0; b.alpha[3] = 0; break;
  case 1:  b.colour[2] = mix(1, 1, 1);                              b.colour[3] = 0; b.alpha[3] = 0; break;
  case 2:  b.colour[2] = vram_fetch16(*sh.vm, *sh.palv, base + 4); b.colour[3] = vram_fetch16(*sh.vm, *sh.palv, base + 6); b.alpha[3] = 31; break;
  default: b.colour[2] = mix(5, 3, 3);                              b.colour[3] = mix(3, 5, 3); b.alpha[3] = 31; break;
  }
}

template <int fmt, int swrap, int twrap>
void gather4_impl(const Renderer3D::Shade& sh, const s16* sa, const s16* ta, uint32x4_t& colour, uint32x4_t& alpha, Tex5Block* scratch) {
  const int32x4_t w = vdupq_n_s32(sh.width), h = vdupq_n_s32(sh.height);
  const int32x4_t w1 = vdupq_n_s32(sh.width - 1), h1 = vdupq_n_s32(sh.height - 1);
  // (s16)coord >> 4, as the hardware truncates the interpolated coordinate.
  const int32x4_t sv = wrap_lanes<swrap>(vshrq_n_s32(vmovl_s16(vld1_s16(sa)), 4), w, w1);
  const int32x4_t tv = wrap_lanes<twrap>(vshrq_n_s32(vmovl_s16(vld1_s16(ta)), 4), h, h1);
  const uint32x4_t offv = vreinterpretq_u32_s32(vmlaq_s32(sv, tv, w));
  const u32 o0 = vgetq_lane_u32(offv, 0), o1 = vgetq_lane_u32(offv, 1), o2 = vgetq_lane_u32(offv, 2), o3 = vgetq_lane_u32(offv, 3);
  const u8* tp = sh.tex_ptr; const u16* pp = sh.pal_ptr;
  (void)o0; (void)o1; (void)o2; (void)o3; (void)tp; (void)pp; (void)scratch;
  u32 c0, c1, c2, c3, a0, a1, a2, a3;
  if constexpr (fmt == 1) {          // A3I5
    const u32 p0 = tp[o0], p1 = tp[o1], p2 = tp[o2], p3 = tp[o3];
    a0 = ((p0 >> 3) & 0x1C) + (p0 >> 6); a1 = ((p1 >> 3) & 0x1C) + (p1 >> 6); a2 = ((p2 >> 3) & 0x1C) + (p2 >> 6); a3 = ((p3 >> 3) & 0x1C) + (p3 >> 6);
    c0 = pp[p0 & 0x1F]; c1 = pp[p1 & 0x1F]; c2 = pp[p2 & 0x1F]; c3 = pp[p3 & 0x1F];
  } else if constexpr (fmt == 2) {   // 4 colours, 2 bits per texel
    const uint32x4_t sl = vreinterpretq_u32_s32(sv);
    const u32 alpha0 = sh.alpha0;
    const u32 p0 = (tp[o0 >> 2] >> ((vgetq_lane_u32(sl, 0) & 3) << 1)) & 3, p1 = (tp[o1 >> 2] >> ((vgetq_lane_u32(sl, 1) & 3) << 1)) & 3;
    const u32 p2 = (tp[o2 >> 2] >> ((vgetq_lane_u32(sl, 2) & 3) << 1)) & 3, p3 = (tp[o3 >> 2] >> ((vgetq_lane_u32(sl, 3) & 3) << 1)) & 3;
    a0 = p0 ? 31 : alpha0; a1 = p1 ? 31 : alpha0; a2 = p2 ? 31 : alpha0; a3 = p3 ? 31 : alpha0;
    c0 = pp[p0]; c1 = pp[p1]; c2 = pp[p2]; c3 = pp[p3];
  } else if constexpr (fmt == 3) {   // 16 colours, 4 bits per texel
    const uint32x4_t sl = vreinterpretq_u32_s32(sv);
    const u32 alpha0 = sh.alpha0;
    const u32 p0 = (tp[o0 >> 1] >> ((vgetq_lane_u32(sl, 0) & 1) << 2)) & 0xF, p1 = (tp[o1 >> 1] >> ((vgetq_lane_u32(sl, 1) & 1) << 2)) & 0xF;
    const u32 p2 = (tp[o2 >> 1] >> ((vgetq_lane_u32(sl, 2) & 1) << 2)) & 0xF, p3 = (tp[o3 >> 1] >> ((vgetq_lane_u32(sl, 3) & 1) << 2)) & 0xF;
    a0 = p0 ? 31 : alpha0; a1 = p1 ? 31 : alpha0; a2 = p2 ? 31 : alpha0; a3 = p3 ? 31 : alpha0;
    c0 = pp[p0]; c1 = pp[p1]; c2 = pp[p2]; c3 = pp[p3];
  } else if constexpr (fmt == 4) {   // 256 colours
    const u32 alpha0 = sh.alpha0;
    const u32 p0 = tp[o0], p1 = tp[o1], p2 = tp[o2], p3 = tp[o3];
    a0 = p0 ? 31 : alpha0; a1 = p1 ? 31 : alpha0; a2 = p2 ? 31 : alpha0; a3 = p3 ? 31 : alpha0;
    c0 = pp[p0]; c1 = pp[p1]; c2 = pp[p2]; c3 = pp[p3];
  } else if constexpr (fmt == 6) {   // A5I3
    const u32 p0 = tp[o0], p1 = tp[o1], p2 = tp[o2], p3 = tp[o3];
    a0 = p0 >> 3; a1 = p1 >> 3; a2 = p2 >> 3; a3 = p3 >> 3;
    c0 = pp[p0 & 7]; c1 = pp[p1 & 7]; c2 = pp[p2 & 7]; c3 = pp[p3 & 7];
  } else if constexpr (fmt == 5) {   // 4x4 compressed: per lane, block table cached
    alignas(16) u32 sl[4], tl[4];
    vst1q_u32(sl, vreinterpretq_u32_s32(sv)); vst1q_u32(tl, vreinterpretq_u32_s32(tv));
    u32 cs[4], as[4];
    for (int k = 0; k < 4; ++k) {
      const u32 ss = sl[k], tt = tl[k];
      const u32 addr = (sh.base + ((tt & 0x3FC) * (static_cast<u32>(sh.width) >> 2)) + (ss & 0x3FC) + (tt & 3)) & 0x7FFFF;
      const u32 block = addr & ~3u;
      if (scratch->key != block) tex5_decode_block(sh, block, *scratch);
      // Texels cannot live in slot 1: the hardware reads zero there.
      const u32 val = (addr >= 0x20000 && addr < 0x40000) ? 0 : ((vram_fetch8(*sh.vm, *sh.texv, addr) >> (2 * (ss & 3))) & 3);
      cs[k] = scratch->colour[val]; as[k] = scratch->alpha[val];
    }
    c0 = cs[0]; c1 = cs[1]; c2 = cs[2]; c3 = cs[3];
    a0 = as[0]; a1 = as[1]; a2 = as[2]; a3 = as[3];
  } else {                           // direct colour
    u16 t0, t1, t2, t3;
    std::memcpy(&t0, tp + o0 * 2, 2); std::memcpy(&t1, tp + o1 * 2, 2); std::memcpy(&t2, tp + o2 * 2, 2); std::memcpy(&t3, tp + o3 * 2, 2);
    a0 = (t0 >> 15) * 31; a1 = (t1 >> 15) * 31; a2 = (t2 >> 15) * 31; a3 = (t3 >> 15) * 31;
    c0 = t0; c1 = t1; c2 = t2; c3 = t3;
  }
  colour = vcombine_u32(vcreate_u32(c0 | (static_cast<u64>(c1) << 32)), vcreate_u32(c2 | (static_cast<u64>(c3) << 32)));
  alpha  = vcombine_u32(vcreate_u32(a0 | (static_cast<u64>(a1) << 32)), vcreate_u32(a2 | (static_cast<u64>(a3) << 32)));
}

// Four texels from the decoded cache: wrap on lanes, one independent load
// per lane, colour and alpha split from the word.
template <int swrap, int twrap>
void gather4_cached(const Renderer3D::Shade& sh, const s16* sa, const s16* ta, uint32x4_t& colour, uint32x4_t& alpha, Tex5Block*) {
  const int32x4_t w = vdupq_n_s32(sh.width), h = vdupq_n_s32(sh.height);
  const int32x4_t w1 = vdupq_n_s32(sh.width - 1), h1 = vdupq_n_s32(sh.height - 1);
  const int32x4_t sv = wrap_lanes<swrap>(vshrq_n_s32(vmovl_s16(vld1_s16(sa)), 4), w, w1);
  const int32x4_t tv = wrap_lanes<twrap>(vshrq_n_s32(vmovl_s16(vld1_s16(ta)), 4), h, h1);
  const uint32x4_t offv = vreinterpretq_u32_s32(vmlaq_s32(sv, tv, w));
  const u32* tp = sh.texels;
  const u32 v0 = tp[vgetq_lane_u32(offv, 0)], v1 = tp[vgetq_lane_u32(offv, 1)], v2 = tp[vgetq_lane_u32(offv, 2)], v3 = tp[vgetq_lane_u32(offv, 3)];
  const uint32x4_t v = vcombine_u32(vcreate_u32(v0 | (static_cast<u64>(v1) << 32)), vcreate_u32(v2 | (static_cast<u64>(v3) << 32)));
  colour = vandq_u32(v, vdupq_n_u32(0xFFFF));
  alpha = vshrq_n_u32(v, 16);
}
// A whole span's texels in one call: the four-lane bodies above in a loop,
// so the resolve loop neither makes an indirect call nor keeps the texture
// state live per four pixels. `n` is rounded up to four by the caller (the
// span buffers carry the slack).
using GatherNFn = void (*)(const Renderer3D::Shade&, const s16*, const s16*, u32, u32*, u32*);
template <int fmt, int swrap, int twrap>
void gatherN_impl(const Renderer3D::Shade& sh, const s16* sa, const s16* ta, u32 n, u32* col, u32* alp) {
  Tex5Block scratch{0xFFFFFFFFu, {}, {}};
  for (u32 i = 0; i < n; i += 4) {
    uint32x4_t c, a;
    gather4_impl<fmt, swrap, twrap>(sh, sa + i, ta + i, c, a, &scratch);
    vst1q_u32(col + i, c); vst1q_u32(alp + i, a);
  }
}
template <int swrap, int twrap>
void gatherN_cached(const Renderer3D::Shade& sh, const s16* sa, const s16* ta, u32 n, u32* col, u32* alp) {
  for (u32 i = 0; i < n; i += 4) {
    uint32x4_t c, a;
    gather4_cached<swrap, twrap>(sh, sa + i, ta + i, c, a, nullptr);
    vst1q_u32(col + i, c); vst1q_u32(alp + i, a);
  }
}
constexpr GatherNFn gatherN_cached_wraps(int swrap, int twrap) {
  constexpr GatherNFn t[3][3] = {
    {gatherN_cached<CLAMP, CLAMP>,  gatherN_cached<CLAMP, REPEAT>,  gatherN_cached<CLAMP, FLIP>},
    {gatherN_cached<REPEAT, CLAMP>, gatherN_cached<REPEAT, REPEAT>, gatherN_cached<REPEAT, FLIP>},
    {gatherN_cached<FLIP, CLAMP>,   gatherN_cached<FLIP, REPEAT>,   gatherN_cached<FLIP, FLIP>},
  };
  return t[swrap][twrap];
}
template <int fmt> constexpr GatherNFn gatherN_wraps(int swrap, int twrap) {
  constexpr GatherNFn t[3][3] = {
    {gatherN_impl<fmt, CLAMP, CLAMP>,  gatherN_impl<fmt, CLAMP, REPEAT>,  gatherN_impl<fmt, CLAMP, FLIP>},
    {gatherN_impl<fmt, REPEAT, CLAMP>, gatherN_impl<fmt, REPEAT, REPEAT>, gatherN_impl<fmt, REPEAT, FLIP>},
    {gatherN_impl<fmt, FLIP, CLAMP>,   gatherN_impl<fmt, FLIP, REPEAT>,   gatherN_impl<fmt, FLIP, FLIP>},
  };
  return t[swrap][twrap];
}
constexpr Gather4Fn gather4_cached_wraps(int swrap, int twrap) {
  constexpr Gather4Fn t[3][3] = {
    {gather4_cached<CLAMP, CLAMP>,  gather4_cached<CLAMP, REPEAT>,  gather4_cached<CLAMP, FLIP>},
    {gather4_cached<REPEAT, CLAMP>, gather4_cached<REPEAT, REPEAT>, gather4_cached<REPEAT, FLIP>},
    {gather4_cached<FLIP, CLAMP>,   gather4_cached<FLIP, REPEAT>,   gather4_cached<FLIP, FLIP>},
  };
  return t[swrap][twrap];
}

template <int fmt> constexpr Gather4Fn gather4_wraps(int swrap, int twrap) {
  constexpr Gather4Fn t[3][3] = {
    {gather4_impl<fmt, CLAMP, CLAMP>,  gather4_impl<fmt, CLAMP, REPEAT>,  gather4_impl<fmt, CLAMP, FLIP>},
    {gather4_impl<fmt, REPEAT, CLAMP>, gather4_impl<fmt, REPEAT, REPEAT>, gather4_impl<fmt, REPEAT, FLIP>},
    {gather4_impl<fmt, FLIP, CLAMP>,   gather4_impl<fmt, FLIP, REPEAT>,   gather4_impl<fmt, FLIP, FLIP>},
  };
  return t[swrap][twrap];
}
} // namespace

const void* Renderer3D::select_gather4(const Shade& sh) {
  const int sw = sh.srep ? (sh.sflip ? FLIP : REPEAT) : CLAMP, tw = sh.trep ? (sh.tflip ? FLIP : REPEAT) : CLAMP;
  if (sh.texels) return reinterpret_cast<const void*>(gatherN_cached_wraps(sw, tw));
  if (sh.fmt == 5) return reinterpret_cast<const void*>(gatherN_wraps<5>(sw, tw));
  if (!sh.tex_ptr || !sh.pal_ptr) return nullptr;
  switch (sh.fmt) {
  case 1: return reinterpret_cast<const void*>(gatherN_wraps<1>(sw, tw));
  case 2: return reinterpret_cast<const void*>(gatherN_wraps<2>(sw, tw));
  case 3: return reinterpret_cast<const void*>(gatherN_wraps<3>(sw, tw));
  case 4: return reinterpret_cast<const void*>(gatherN_wraps<4>(sw, tw));
  case 6: return reinterpret_cast<const void*>(gatherN_wraps<6>(sw, tw));
  case 7: return reinterpret_cast<const void*>(gatherN_wraps<7>(sw, tw));
  default: return nullptr;
  }
}

// Four texels through the per-lane sampler: the compressed format, or a
// texture / palette that is not host-contiguous.
inline void Renderer3D::texture_gather4(const Shade& sh, const s16* sa, const s16* ta, u32* colour, u32* alpha) const {
  for (int k = 0; k < 4; ++k) colour[k] = texture_sample(sh, sa[k], ta[k], &alpha[k]);
}

// One call per span instead of one per four pixels: the resolve loop then
// loads texels from the span buffer and keeps no texture state live.
void Renderer3D::span_texels(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const {
  const u32 off = static_cast<u32>(ca - sb.x0);
  // Two roundings, deliberately different. The gather is real work -- an
  // address computation, a wrap and a palette lookup per texel -- and steps
  // by four, so it runs over the span rounded up to *four*: rounding it to
  // sixteen fetched up to twelve texels nobody reads, and with a mean span
  // around ten pixels that was a third of the whole texture stage. The
  // repack below is byte-lane ALU over a buffer that is already there, so it
  // keeps its sixteen-wide step and the gap is filled in rather than fetched.
  const u32 n = static_cast<u32>(cb - ca);
  const u32 n4 = (n + 3) & ~3u;
  const u32 n16 = (n + 15) & ~15u;                         // the buffers carry sixteen entries of slack
  const s16* sa = sb.sc + off; const s16* ta = sb.tc + off;
  u32* col = sb.tcol + off; u32* alp = sb.talp + off;
  if (const GatherNFn g = reinterpret_cast<GatherNFn>(sh.gather4)) {
    g(sh, sa, ta, n4, col, alp);
    prof::add(sh.texels ? prof::C_TEX_FAST : (sh.fmt == 5 ? prof::C_TEX_SLOW_FMT5 : prof::C_TEX_FAST), n4);
  } else {
    prof::add(prof::C_TEX_SLOW_VIEWS, n4);
    for (u32 i = 0; i < n4; i += 4) texture_gather4(sh, sa + i, ta + i, col + i, alp + i);
  }
  // The shader reads whole sixteens; the tail past the gather is padding it
  // never looks at, but it has to be a defined value.
  const uint32x4_t zero = vdupq_n_u32(0);
  for (u32 i = n4; i < n16; i += 4) { vst1q_u32(col + i, zero); vst1q_u32(alp + i, zero); }
  // The RGB555 -> byte-plane repack used to happen here, into four more span
  // planes. It is now done inside span_shade, a vector at a time, so the
  // texels go gather -> registers -> shaded record without a round trip
  // through memory: it was 8 bytes a pixel written and 8 read back, on a
  // pipeline that measures memory-bound.
}

// A texel vector unpacked to the four 6-bit byte planes the shader wants:
// RGB555 in `col`, 5-bit alpha in `alp`, sixteen pixels at a time. Lives here
// rather than in a span plane because span_shade is its only consumer.
[[gnu::always_inline]] inline void texels16(const u32* col, const u32* alp, uint8x16_t out[4]) {
  const uint16x8_t m3e = vdupq_n_u16(0x3E), one16 = vdupq_n_u16(1);
  auto chan = [&](uint16x8_t c, int shift) {
    uint16x8_t v = shift == 0 ? vshlq_n_u16(c, 1) : (shift == 4 ? vshrq_n_u16(c, 4) : vshrq_n_u16(c, 9));
    v = vandq_u16(v, m3e);
    return vaddq_u16(v, vandq_u16(vtstq_u16(v, v), one16));
  };
  const uint16x8_t c0 = vcombine_u16(vmovn_u32(vld1q_u32(col)),     vmovn_u32(vld1q_u32(col + 4)));
  const uint16x8_t c1 = vcombine_u16(vmovn_u32(vld1q_u32(col + 8)), vmovn_u32(vld1q_u32(col + 12)));
  const uint16x8_t a0 = vcombine_u16(vmovn_u32(vld1q_u32(alp)),     vmovn_u32(vld1q_u32(alp + 4)));
  const uint16x8_t a1 = vcombine_u16(vmovn_u32(vld1q_u32(alp + 8)), vmovn_u32(vld1q_u32(alp + 12)));
  out[0] = vcombine_u8(vmovn_u16(chan(c0, 0)), vmovn_u16(chan(c1, 0)));
  out[1] = vcombine_u8(vmovn_u16(chan(c0, 4)), vmovn_u16(chan(c1, 4)));
  out[2] = vcombine_u8(vmovn_u16(chan(c0, 9)), vmovn_u16(chan(c1, 9)));
  out[3] = vcombine_u8(vmovn_u16(a0), vmovn_u16(a1));
}

// The span's shaded colours. Decal vs modulate is a property of the polygon,
// so it is decided here once rather than re-tested for every four pixels, and
// the resolve loop reads finished records.
//
// This pass also *narrows the pass plane by the alpha test*. The pre-pass can
// only mark depth candidates -- a pixel's alpha does not exist until the shade
// below computes it -- so the resolve used to load and mask every lane of a
// group before discovering that the alpha test killed all eight. Measured over
// 900 frames, that was 21 % of every group on etody, 18 % on sm64 and 41 % on
// meteos: groups where a lane passed depth and nothing drew. The alpha is
// already in a register here at the moment the record is stored, so folding
// the test in costs one compare and a read-modify-write of the pass byte per
// sixteen pixels, and the resolve's existing `pass` pre-test then skips those
// groups whole.
//
// It masks *both* pass bits, which is what the resolve does: alpha gates
// `m1 | m2`, so it kills the under-layer candidate exactly as it kills the top
// one. Only the vector path runs this (flush_batch calls it under sh.vec), so
// the shadow, wireframe and non-NEON resolves keep testing alpha themselves
// and stay byte-comparable.
template <bool textured>
void Renderer3D::span_shade(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const {
  const u32 off = static_cast<u32>(ca - sb.x0);
  const u32 n = (static_cast<u32>(cb - ca) + 15) & ~15u;
  const u8* ra = sb.vr + off; const u8* ga = sb.vg + off; const u8* ba = sb.vb + off;
  u32* out = sb.col + off;
  u8* pa = sb.pass + off;
  const uint8x16_t vpa = vdupq_n_u8(static_cast<u8>(sh.polyalpha));
  const uint8x16_t vref = vdupq_n_u8(static_cast<u8>(sh.alpha_ref));
  // Keep the alpha lanes and the pass byte together: one compare, one and.
  auto narrow = [&](u32 i, uint8x16_t a) __attribute__((always_inline)) {
    vst1q_u8(pa + i, vandq_u8(vld1q_u8(pa + i), vcgtq_u8(a, vref)));
  };
  // A pixel record is r | g << 8 | b << 16 | a << 24, so four byte planes
  // stored interleaved (st4) are the records themselves.
  if constexpr (!textured) {
    for (u32 i = 0; i < n; i += 16) {
      const uint8x16x4_t rec = {vld1q_u8(ra + i), vld1q_u8(ga + i), vld1q_u8(ba + i), vpa};
      vst4q_u8(reinterpret_cast<u8*>(out + i), rec);
    }
    return;
  }
  const u32* tca = sb.tcol + off; const u32* taca = sb.talp + off;
  const bool decal = sh.blendmode & 1;
  if (!decal) {
    // Modulate: ((t + 1) * (v + 1) - 1) >> 6 is t + v + t*v >> 6, which is an
    // add-long plus a multiply-accumulate-long in byte lanes (both channels
    // are 6-bit, so the product cannot leave 16 bits).
    for (u32 i = 0; i < n; i += 16) {
      uint8x16_t ch[4], tv[4];
      texels16(tca + i, taca + i, tv);
      const uint8x16_t vv[4] = {vld1q_u8(ra + i), vld1q_u8(ga + i), vld1q_u8(ba + i), vpa};
      for (int k = 0; k < 4; ++k) {
        uint16x8_t lo = vaddl_u8(vget_low_u8(tv[k]), vget_low_u8(vv[k]));
        uint16x8_t hi = compat::addl_high_u8(tv[k], vv[k]);
        lo = vmlal_u8(lo, vget_low_u8(tv[k]), vget_low_u8(vv[k]));
        hi = compat::mlal_high_u8(hi, tv[k], vv[k]);
        ch[k] = k == 3 ? vcombine_u8(vshrn_n_u16(lo, 5), vshrn_n_u16(hi, 5))
                       : vcombine_u8(vshrn_n_u16(lo, 6), vshrn_n_u16(hi, 6));
      }
      const uint8x16x4_t rec = {ch[0], ch[1], ch[2], ch[3]};
      vst4q_u8(reinterpret_cast<u8*>(out + i), rec);
      narrow(i, ch[3]);
    }
    return;
  }
  // Decal: (t * ta + v * (31 - ta)) >> 5, with the two ends taken whole.
  //
  // The record's alpha is the polygon's here, not the texel's, so the alpha
  // test is one decision for the whole batch: either every pixel survives it
  // and the pass plane is already right, or none does and the batch is dead.
  if (sh.polyalpha <= sh.alpha_ref) { std::memset(pa, 0, n); return; }
  const uint8x16_t v31 = vdupq_n_u8(31), v0 = vdupq_n_u8(0);
  for (u32 i = 0; i < n; i += 16) {
    uint8x16_t tx[4];
    texels16(tca + i, taca + i, tx);
    const uint8x16_t tal = tx[3];
    const uint8x16_t inv = vsubq_u8(v31, tal);
    const uint8x16_t at0 = vceqq_u8(tal, v0), at31 = vceqq_u8(tal, v31);
    uint8x16_t ch[4];
    const uint8x16_t tv[3] = {tx[0], tx[1], tx[2]};
    const uint8x16_t vv[3] = {vld1q_u8(ra + i), vld1q_u8(ga + i), vld1q_u8(ba + i)};
    for (int k = 0; k < 3; ++k) {
      uint16x8_t lo = vmull_u8(vget_low_u8(tv[k]), vget_low_u8(tal));
      uint16x8_t hi = compat::mull_high_u8(tv[k], tal);
      lo = vmlal_u8(lo, vget_low_u8(vv[k]), vget_low_u8(inv));
      hi = compat::mlal_high_u8(hi, vv[k], inv);
      const uint8x16_t m = vcombine_u8(vshrn_n_u16(lo, 5), vshrn_n_u16(hi, 5));
      ch[k] = vbslq_u8(at0, vv[k], vbslq_u8(at31, tv[k], m));
    }
    const uint8x16x4_t rec = {ch[0], ch[1], ch[2], vpa};
    vst4q_u8(reinterpret_cast<u8*>(out + i), rec);
  }
}

// Census (DS_PROFILE only): classify one eight-pixel group's kind codes and
// fold the result into the batch-level accumulators. Deliberately scalar and
// out of line -- it never runs in a measured build.
void Renderer3D::rk_census(u64 kinds, u64 p8) {
  prof::add(prof::C_RK_GROUPS, 1);
  if (!kinds) {
    prof::add(prof::C_RK_EMPTY, 1);
    prof::add((p8 & 0x0101010101010101ull) ? prof::C_RK_EMPTY_TOP : prof::C_RK_EMPTY_UNDER, 1);
    return;
  }
  u32 seen = 0, first = 0;
  bool mixed = false;
  for (u32 l = 0; l < 8; ++l) {
    const u32 k = static_cast<u32>((kinds >> (8 * l)) & 0xFF);
    if (!k) continue;
    seen |= k;
    if (!first) first = k; else if (k != first) mixed = true;
  }
  prof::add(mixed ? prof::C_RK_MIXED : prof::C_RK_UNIFORM, 1);
  // Every lane drawing and opaque: the store-only case. Counted for both
  // widths, so a four-lane group needs only its own four bytes set.
  if (kinds == 0x0101010101010101ull) { prof::add(prof::C_RK_FULL_OPAQUE, 1); prof::add(prof::C_RK_FULL_OPAQUE_PX, 8); }
  else if (kinds == 0x0000000001010101ull) { prof::add(prof::C_RK_FULL_OPAQUE, 1); prof::add(prof::C_RK_FULL_OPAQUE_PX, 4); }
  if (seen == 1) prof::add(prof::C_RK_OPAQUE, 1);
  else if (!(seen & 0x09)) prof::add(prof::C_RK_TRANS, 1);
  if (seen & 0x18) prof::add(prof::C_RK_UNDER, 1);
  rk_or_ |= seen;
  rk_mixed_ = rk_mixed_ || mixed;
  ++rk_groups_;
}

template <int mode, bool textured, bool aa, bool opq>
[[gnu::always_inline]] inline void Renderer3D::resolve_span_vec(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov) {
  // The profiling flag is read once per span, not once per eight-pixel
  // group: prof::add tests the global inside the innermost loop otherwise
  // (two loads and a branch per group, as stage_line already hoists).
  const bool pe = prof::enabled;
  u32 resolved_px = 0;
  const u32 polyattr = sh.polyattr;
  const uint32x4_t lane = {0, 1, 2, 3};
  const uint32x4_t v_alpha_ref = vdupq_n_u32(sh.alpha_ref), v31 = vdupq_n_u32(31), v0 = vdupq_n_u32(0);
  (void)v_alpha_ref;
  // Opaque attribute word for this part (coverage added per lane on the edge parts).
  u32 attr_base = polyattr | edge;
  bool push = false;
  s32 cov_sel = part == 0 ? l_cov : r_cov;
  bool cov_accum = false;
  if (aa) {
    if (part == 1) { if (attr_base & 0xF) { attr_base |= (0x1F << 8); push = true; } }
    else { push = true; cov_accum = cov_sel & static_cast<s32>(0x80000000u); if (!cov_accum) attr_base |= (cov_sel << 8); }
  }
  const s32 cov_step = cov_sel & 0x3FF;
  const bool blend_on = sh.dispcnt & (1 << 3);
  const u32 row0 = row_of(y) + 1;
  u8* const cb = reinterpret_cast<u8*>(color_.data());
  u8* const ab = reinterpret_cast<u8*>(attr_.data());

  // plot_translucent on eight lanes, in byte planes: one vld4 each for the
  // source and destination records and for the destination attributes, the
  // blend as multiply-long / multiply-accumulate-long in 16-bit lanes with a
  // narrowing shift, the attribute bytes rebuilt plane by plane, and one
  // vst4 back for each. DraStic's render_polygon_alpha_blend_c is this
  // shape; the 32-bit-lane form shifted every channel out of the word and
  // back for each of the two halves.
  //
  // Attribute of the translucent pixel: byte 0 = polygon bits 4-7 | dest
  // bits 0-3 (edge flags), byte 1 = polygon bits 13-15 (fog only where the
  // destination has it) | dest coverage bits 0-4, byte 2 = polygon id | the
  // translucent bit, byte 3 = the destination's.
  const uint8x8_t pa0 = vdup_n_u8(static_cast<u8>(polyattr & 0xF0));
  const uint8x8_t pa1 = vdup_n_u8(static_cast<u8>((polyattr >> 8) & 0xE0));
  const uint8x8_t pa2 = vdup_n_u8(static_cast<u8>((polyattr >> 24) | 0x40));
  const uint8x8_t pa2id = vand_u8(pa2, vdup_n_u8(0x7F));
  const uint8x8_t v7f8 = vdup_n_u8(0x7F), v0f8 = vdup_n_u8(0x0F), v1f8 = vdup_n_u8(0x1F), v3f8 = vdup_n_u8(0x3F);
  const uint8x8_t one8 = vdup_n_u8(1), v32_8 = vdup_n_u8(32), zero8 = vdup_n_u8(0);
  auto plot8 = [&](u32 base, const uint32x4_t* m, const uint8x8x4_t& src, const int32x4_t* z) __attribute__((always_inline)) {
    const uint8x8x4_t da = vld4_u8(ab + base * 4), dc = vld4_u8(cb + base * 4);
    uint8x8_t m8 = vmovn_u16(vcombine_u16(vmovn_u32(m[0]), vmovn_u32(m[1])));
    m8 = vbic_u8(m8, vceq_u8(vand_u8(da.val[2], v7f8), pa2id));   // equal translucent ids don't blend
    const uint8x8_t sa = src.val[3], dsta = dc.val[3];
    uint8x8_t r = src.val[0], g = src.val[1], b = src.val[2];
    if (blend_on) {
      const uint8x8_t a1 = vadd_u8(sa, one8), a2 = vsub_u8(v32_8, a1);
      r = vshrn_n_u16(vmlal_u8(vmull_u8(vand_u8(r, v3f8), a1), vand_u8(dc.val[0], v3f8), a2), 5);
      g = vshrn_n_u16(vmlal_u8(vmull_u8(vand_u8(g, v3f8), a1), vand_u8(dc.val[1], v3f8), a2), 5);
      b = vshrn_n_u16(vmlal_u8(vmull_u8(vand_u8(b, v3f8), a1), vand_u8(dc.val[2], v3f8), a2), 5);
    }
    const uint8x8_t none = vceq_u8(dsta, zero8);                  // nothing underneath: source as is
    uint8x8x4_t oc;
    oc.val[0] = vbsl_u8(m8, vbsl_u8(none, src.val[0], r), dc.val[0]);
    oc.val[1] = vbsl_u8(m8, vbsl_u8(none, src.val[1], g), dc.val[1]);
    oc.val[2] = vbsl_u8(m8, vbsl_u8(none, src.val[2], b), dc.val[2]);
    oc.val[3] = vbsl_u8(m8, vbsl_u8(none, sa, vmax_u8(sa, dsta)), dc.val[3]);
    vst4_u8(cb + base * 4, oc);
    uint8x8x4_t oa;
    oa.val[0] = vbsl_u8(m8, vorr_u8(pa0, vand_u8(da.val[0], v0f8)), da.val[0]);
    oa.val[1] = vbsl_u8(m8, vorr_u8(vand_u8(pa1, vorr_u8(da.val[1], v7f8)), vand_u8(da.val[1], v1f8)), da.val[1]);
    oa.val[2] = vbsl_u8(m8, pa2, da.val[2]);
    oa.val[3] = da.val[3];
    vst4_u8(ab + base * 4, oa);
    if (sh.polyattr_z) {
      const int16x8_t mw = vmovl_s8(vreinterpret_s8_u8(m8));
      const uint32x4_t mz[2] = {vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(mw))), vreinterpretq_u32_s32(compat::movl_high_s16(mw))};
      for (u32 k = 0; k < 2; ++k)
        vst1q_s32(reinterpret_cast<s32*>(&depth_[base + k * 4]), vbslq_s32(mz[k], z[k], vld1q_s32(reinterpret_cast<const s32*>(&depth_[base + k * 4]))));
    }
  };

  // Eight pixels a step, as two four-lane halves: the per-group scalar work
  // (the pass-byte test, the address arithmetic, the lane-kind reduction and
  // its branches) is paid once per eight pixels, and the group is the width
  // the byte-plane stages take (vld4_u8 over eight records). Lane masks are
  // tested through one 64-bit transfer of the narrowed lanes -- a cross-lane
  // reduction per test is what the in-order core stalls on.
  auto lanes8 = [](uint32x4_t a, uint32x4_t b) __attribute__((always_inline)) -> u64 {
    return vget_lane_u64(vreinterpret_u64_u8(vmovn_u16(vcombine_u16(vmovn_u32(a), vmovn_u32(b)))), 0);
  };
  constexpr u64 HALF[2] = {0x00000000FFFFFFFFull, 0xFFFFFFFF00000000ull};
  const bool untextured_passes = !textured && sh.polyalpha > sh.alpha_ref;
  if (!textured && !untextured_passes) return;   // alpha test fails for every pixel
  // A range of four pixels or fewer takes a single half: the three-part walk
  // hands the resolve the edge runs of a span separately, and those are one
  // to three pixels wide -- an eight-lane group there is twice the lane work
  // for nothing (etody's tail, device A/B 2026-08-30).
  auto step = [&](auto nh_c, s32 x, s32 rem) __attribute__((always_inline)) {
    constexpr u32 NH = decltype(nh_c)::value;
    const u32 i = static_cast<u32>(x - sb.x0);
    u64 p8; std::memcpy(&p8, sb.pass + i, 8);
    if (rem < 8) p8 &= (1ull << (8 * rem)) - 1;    // lanes past the end
    if (!(p8 & 0x0303030303030303ull)) return;
    // The pass bytes as lane vectors straight from memory: no general-register
    // to vector transfer, no per-lane shift to spread a word.
    const uint16x8_t p16 = vmovl_u8(vld1_u8(sb.pass + i));
    uint32x4_t pv[2] = {vmovl_u16(vget_low_u16(p16)), compat::movl_high_u16(p16)};
    if (rem < 8) {
      const uint32x4_t vr = vdupq_n_u32(static_cast<u32>(rem));
      pv[0] = vandq_u32(pv[0], vcltq_u32(lane, vr));
      pv[1] = vandq_u32(pv[1], vcltq_u32(vaddq_u32(lane, vdupq_n_u32(4)), vr));
    }
    const u32 addr = row0 + static_cast<u32>(x), under = addr + RSIZE;
    uint32x4_t m1[2] = {v0, v0}; int32x4_t z[2] = {vdupq_n_s32(0), vdupq_n_s32(0)};
    for (u32 k = 0; k < NH; ++k) { m1[k] = vtstq_u32(pv[k], vdupq_n_u32(1)); z[k] = vld1q_s32(sb.z + i + k * 4); }
    // Pre-pass bit 1: the top pixel fails the depth test but carries edge
    // flags, so the pixel underneath is a candidate. Its test is done here
    // (the pre-pass only looks at the top layer), and such lanes then take
    // the same opaque / translucent writes as the top lanes, at the under
    // address, without a push and without a second translucent plot -- the
    // two things the scalar path does differently for addr >= RSIZE.
    //
    // Two instantiations, not one body with a zero mask: the in-order core
    // pays every extra lane operation on the common path (a single body
    // measured 5 % slower on the span stage for 6 % fewer instructions), so
    // the no-under group must compile to exactly what it was.
    auto group = [&](auto two_c, const uint32x4_t* m2) __attribute__((always_inline)) {
      constexpr bool two = decltype(two_c)::value;
      uint32x4_t colour[2], mo[2] = {v0, v0}, mo1[2] = {v0, v0}, mt1[2] = {v0, v0}, mo2[2] = {v0, v0}, mt2[2] = {v0, v0}, dstattr[2], mb[2] = {v0, v0}, kv[2] = {v0, v0};
      resolved_px += NH * 4;
      for (u32 k = 0; k < NH; ++k) {
        colour[k] = vld1q_u32(sb.col + i + k * 4);
        dstattr[k] = vld1q_u32(&attr_[addr + k * 4]);
        if constexpr (opq) {
          // Every drawing lane is opaque by construction (Shade::opaque), so
          // the alpha test, the opaque/translucent split, the underneath
          // probe and the translucent plot below all compile out.
          mo1[k] = m1[k]; mo[k] = m1[k];
          kv[k] = vandq_u32(m1[k], vdupq_n_u32(1));
          if constexpr (two) {
            mo[k] = vorrq_u32(m1[k], m2[k]); mo2[k] = m2[k];
            kv[k] = vorrq_u32(kv[k], vandq_u32(m2[k], vdupq_n_u32(8)));
          }
        } else {
          const uint32x4_t a = vshrq_n_u32(colour[k], 24);
          uint32x4_t m = m1[k];
          if constexpr (two) m = vorrq_u32(m1[k], m2[k]);
          if constexpr (textured) m = vandq_u32(m, vcgtq_u32(a, v_alpha_ref));
          mo[k] = vandq_u32(m, vceqq_u32(a, v31));
          const uint32x4_t mt = vbicq_u32(m, mo[k]);
          mo1[k] = mo[k]; mt1[k] = mt; mo2[k] = v0; mt2[k] = v0;
          if constexpr (two) { mo1[k] = vandq_u32(mo[k], m1[k]); mt1[k] = vandq_u32(mt, m1[k]); mo2[k] = vandq_u32(mo[k], m2[k]); mt2[k] = vandq_u32(mt, m2[k]); }
          // bit 0 per lane: opaque, bit 1: translucent, bit 2: translucent with a pixel underneath;
          // bits 3 and 4: the same opaque / translucent, landing on the pixel underneath.
          // The under layer is dead without AA (dispcnt_), so bit 2 is off there.
          if constexpr (aa) mb[k] = vandq_u32(mt1[k], vtstq_u32(dstattr[k], vdupq_n_u32(0xF)));
          kv[k] = vorrq_u32(vorrq_u32(vandq_u32(mo1[k], vdupq_n_u32(1)), vandq_u32(mt1[k], vdupq_n_u32(2))), vandq_u32(mb[k], vdupq_n_u32(4)));
          if constexpr (two) kv[k] = vorrq_u32(kv[k], vorrq_u32(vandq_u32(mo2[k], vdupq_n_u32(8)), vandq_u32(mt2[k], vdupq_n_u32(16))));
        }
      }
      const u64 kinds = lanes8(kv[0], kv[1]);
      if (pe) rk_census(kinds, p8);
      if (!kinds) return;
      if (opq || (kinds & 0x0909090909090909ull)) {
        // One attribute word for both layers: coverage accumulates in x order
        // over every opaque pixel of the span, whichever layer it lands on.
        uint32x4_t attr[2] = {vdupq_n_u32(attr_base), vdupq_n_u32(attr_base)};
        if (aa && cov_accum) {
          u32 t0, t1 = 0;
          uint32x4_t pre[2];
          pre[0] = prefix_exclusive(vandq_u32(mo[0], vdupq_n_u32(1)), &t0);
          if constexpr (NH == 2) pre[1] = vaddq_u32(prefix_exclusive(vandq_u32(mo[1], vdupq_n_u32(1)), &t1), vdupq_n_u32(t0));
          for (u32 k = 0; k < NH; ++k) {
            const uint32x4_t xc = vshrq_n_u32(vmlaq_u32(vdupq_n_u32(static_cast<u32>(xcov)), pre[k], vdupq_n_u32(static_cast<u32>(cov_step))), 5);
            uint32x4_t cov;
            if (part == 0) cov = vminq_u32(xc, v31);
            else cov = vreinterpretq_u32_s32(vmaxq_s32(vsubq_s32(vdupq_n_s32(0x1F), vreinterpretq_s32_u32(xc)), vdupq_n_s32(0)));
            attr[k] = vorrq_u32(attr[k], vshlq_n_u32(cov, 8));
          }
          xcov += cov_step * static_cast<s32>(t0 + t1);
        }
        for (u32 k = 0; k < NH; ++k) {
          const u32 ak = addr + k * 4, uk = under + k * 4;
          if (kinds & (0x0101010101010101ull & HALF[k])) {
            const uint32x4_t dstcol = vld1q_u32(&color_[ak]);
            const int32x4_t dstz = vld1q_s32(reinterpret_cast<const s32*>(&depth_[ak]));
            if (push) {
              vst1q_u32(&color_[uk], vbslq_u32(mo1[k], dstcol, vld1q_u32(&color_[uk])));
              vst1q_s32(reinterpret_cast<s32*>(&depth_[uk]), vbslq_s32(mo1[k], dstz, vld1q_s32(reinterpret_cast<const s32*>(&depth_[uk]))));
              vst1q_u32(&attr_[uk], vbslq_u32(mo1[k], dstattr[k], vld1q_u32(&attr_[uk])));
            }
            vst1q_s32(reinterpret_cast<s32*>(&depth_[ak]), vbslq_s32(mo1[k], z[k], dstz));
            vst1q_u32(&color_[ak], vbslq_u32(mo1[k], colour[k], dstcol));
            vst1q_u32(&attr_[ak], vbslq_u32(mo1[k], attr[k], dstattr[k]));
          }
          if constexpr (two) {
            if (kinds & (0x0808080808080808ull & HALF[k])) {
              vst1q_s32(reinterpret_cast<s32*>(&depth_[uk]), vbslq_s32(mo2[k], z[k], vld1q_s32(reinterpret_cast<const s32*>(&depth_[uk]))));
              vst1q_u32(&color_[uk], vbslq_u32(mo2[k], colour[k], vld1q_u32(&color_[uk])));
              vst1q_u32(&attr_[uk], vbslq_u32(mo2[k], attr[k], vld1q_u32(&attr_[uk])));
            }
          }
        }
      }
      if constexpr (!opq) {
        if (kinds & 0x1212121212121212ull) {
          const uint8x8x4_t src = vld4_u8(reinterpret_cast<const u8*>(sb.col + i));
          if (kinds & 0x0202020202020202ull) {
            plot8(addr, mt1, src, z);
            if (kinds & 0x0404040404040404ull) plot8(under, mb, src, z);
          }
          if constexpr (two) { if (kinds & 0x1010101010101010ull) plot8(under, mt2, src, z); }
        }
      }
    };
    // Without AA the pre-pass never sets bit 1 (the under layer is dead), so
    // the two-layer group compiles out of that instantiation.
    if (aa && (p8 & 0x0202020202020202ull)) {
      uint32x4_t m2[2] = {v0, v0};
      for (u32 k = 0; k < NH; ++k) {
        const int32x4_t dz = vld1q_s32(reinterpret_cast<const s32*>(&depth_[under + k * 4]));
        uint32x4_t ok;
        if constexpr (mode == 0) ok = vcltq_s32(z[k], dz);
        else if constexpr (mode == 1) {
          const uint32x4_t back = vceqq_u32(vandq_u32(vld1q_u32(&attr_[under + k * 4]), vdupq_n_u32(0x00400010)), vdupq_n_u32(0x10));
          ok = vbslq_u32(back, vcleq_s32(z[k], dz), vcltq_s32(z[k], dz));
        } else if constexpr (mode == 2) ok = vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(dz, z[k]), vdupq_n_s32(0x200))), vdupq_n_u32(0x400));
        else ok = vcleq_u32(vreinterpretq_u32_s32(vaddq_s32(vsubq_s32(dz, z[k]), vdupq_n_s32(0xFF))), vdupq_n_u32(0x1FE));
        m2[k] = vandq_u32(vtstq_u32(pv[k], vdupq_n_u32(2)), ok);
      }
      group(std::true_type{}, m2);
    } else {
      if (!(p8 & 0x0101010101010101ull)) return;
      group(std::false_type{}, nullptr);
    }
  };
  for (s32 x = xa; x < xb;) {
    const s32 rem = xb - x;
    if (rem <= 4) { step(std::integral_constant<u32, 1>{}, x, rem); x += 4; }
    else { step(std::integral_constant<u32, 2>{}, x, rem); x += 8; }
  }
  if (pe) prof::add(prof::C_RESOLVED_PIXELS, resolved_px);
}
#endif
// Derive scanlines [y0, y1) of the polygon on `e` into lines_[].
//
// Everything here is a function of the polygon and y alone: the edge walk, the
// endpoint w/z and attribute interpolations, the swapped-edge handling, the
// edge lengths and coverages, and the fill rules. None of it reads the
// framebuffer, which is why it can all leave the per-scanline path.
//
// The interpolations are the fourteen Interp::interpolate calls that were
// ~69 instructions a scanline, and the edge setup another ~48 -- both now paid
// once per run instead of once per line. The loop also lets the constant
// polygon and Shade fields (wbuffer, always_fill, ybot, the vertex pointers)
// stay in registers across the whole run rather than being reloaded per line.
void Renderer3D::precompute_lines(Edge& e, s32 y0, s32 y1) {
  const Polygon& p = *e.poly;
  const Shade& sh = e.sh;
  const bool always_fill = sh.always_fill;
  const bool wireframe = sh.wireframe;
  const bool aa = sh.dispcnt & (1 << 4);   // coverage is only read by the AA resolve
  const bool flat = p.ytop == p.ybot;
  const s32 ybot1 = p.ybot - 1;
  const s32 ytop = p.ytop;

  for (s32 y = y0; y < y1; ++y) {
    LineSpan& ls = lines_[static_cast<u32>(y - y0)];
    if (!flat) {
      if (y >= gx_->vertex(p.vtx[e.next_vl]).sy && e.cur_vl != p.vbot) setup_left_edge(e, y);
      if (y >= gx_->vertex(p.vtx[e.next_vr]).sy && e.cur_vr != p.vbot) setup_right_edge(e, y);
    }
    s32 xstart = e.xl, xend = e.xr;
    s32 wl = e.left.interp.interpolate(e.wcl, e.wnl);
    s32 wr = e.right.interp.interpolate(e.wcr, e.wnr);
    s32 zl = e.left.interp.interpolate_z(e.zcl, e.znl);
    s32 zr = e.right.interp.interpolate_z(e.zcr, e.znr);
    // Right vertical edges are pushed one pixel left unless the span is a
    // single pixel at the screen's left edge.
    if (e.r_incr0 && (!e.l_incr0 || xstart != xend) && xend != 0) --xend;

    const Vertex *vlcur, *vlnext, *vrcur, *vrnext;
    const Interp<1>* istart; const Interp<1>* iend;
    bool l_fill, r_fill; s32 l_len, r_len, l_cov, r_cov;
    // Everything below that is not a function of y comes out of the Edge; only
    // the bottom-line test and the edge_params (which walk dx) are per scanline.
    const bool ybot_line = y == ybot1;
    const bool bottom_fill = ybot_line && e.next_sx_differ;
    if (xstart > xend) {
      // Swapped edges: the hardware walks them backwards, which breaks the
      // X-major edge lengths (and the AA on them) in a specific way.
      vlcur = e.vcr; vlnext = e.vnr;
      vrcur = e.vcl; vrnext = e.vnl;
      istart = &e.right.interp; iend = &e.left.interp;
      e.right.edge_params<true>(aa, &l_len, &l_cov);
      e.left.edge_params<true>(aa, &r_len, &r_cov);
      std::swap(xstart, xend); std::swap(wl, wr); std::swap(zl, zr);
      if (always_fill) { l_fill = r_fill = true; }
      else {
        l_fill = e.nx_r || (bottom_fill && e.rxm);
        r_fill = e.px_l || (!e.lneg_xm && e.r_incr0) || (bottom_fill && e.lxm);
      }
    } else {
      vlcur = e.vcl; vlnext = e.vnl;
      vrcur = e.vcr; vrnext = e.vnr;
      istart = &e.left.interp; iend = &e.right.interp;
      e.left.edge_params<false>(aa, &l_len, &l_cov);
      e.right.edge_params<false>(aa, &r_len, &r_cov);
      // Fill rules for opaque edges: left edges fill when their slope is <= 1,
      // right edges when > 1 or vertical; the bottom pixel of a negative
      // X-major edge fills next to a flat bottom; fully overlapping identical
      // edges fill. AA, edge marking, blended translucency or wireframe fill all.
      if (always_fill) { l_fill = r_fill = true; }
      else {
        l_fill = e.nx_l || (bottom_fill && e.lxm) ||
                 (e.same_incr && (xstart + l_len == xend + 1));
        r_fill = e.px_r || e.r_incr0 || (bottom_fill && e.rxm);
      }
    }

    int yedge = 0;
    if (y == ytop) yedge = 0x4; else if (ybot_line) yedge = 0x8;
    s32 x = xstart;
    if (x < 0) x = 0;

    ls.xstart = xstart; ls.xend = xend;
    ls.wl = wl; ls.wr = wr; ls.zl = zl; ls.zr = zr;
    ls.l_len = l_len; ls.r_len = r_len; ls.l_cov = l_cov; ls.r_cov = r_cov;
    ls.xa = x; ls.xb = std::min(xend + 1, 256);
    ls.yedge = yedge;
    ls.l_fill = l_fill; ls.r_fill = r_fill;
    ls.wf_skip = wireframe && !yedge;
    // Attributes at both ends of the span: r g b s t. Computed for every line
    // of the run, including the ones the depth pre-pass will go on to kill --
    // the per-scanline path skipped those, but the interpolants are live in
    // registers here and depth survival is 61-94 % across the five scenes, so
    // the branch costs more than the arithmetic it saves.
    ls.al[0] = istart->interpolate(vlcur->fcol[0], vlnext->fcol[0]);
    ls.al[1] = istart->interpolate(vlcur->fcol[1], vlnext->fcol[1]);
    ls.al[2] = istart->interpolate(vlcur->fcol[2], vlnext->fcol[2]);
    ls.al[3] = istart->interpolate(vlcur->tex[0], vlnext->tex[0]);
    ls.al[4] = istart->interpolate(vlcur->tex[1], vlnext->tex[1]);
    ls.ar[0] = iend->interpolate(vrcur->fcol[0], vrnext->fcol[0]);
    ls.ar[1] = iend->interpolate(vrcur->fcol[1], vrnext->fcol[1]);
    ls.ar[2] = iend->interpolate(vrcur->fcol[2], vrnext->fcol[2]);
    ls.ar[3] = iend->interpolate(vrcur->tex[0], vrnext->tex[0]);
    ls.ar[4] = iend->interpolate(vrcur->tex[1], vrnext->tex[1]);

    e.xl = e.left.step();
    e.xr = e.right.step();
  }
}

// Stage one precomputed scanline into the batch. What is left here is exactly
// the work the framebuffer reaches: the depth pre-pass against live depth_ and
// attr_, the attribute staging into the span buffer, and the batch job. The
// pixel stages and the resolve wait for flush_batch.
void Renderer3D::stage_line(Edge& e, s32 y, const LineSpan& ls) {
  const Polygon& p = *e.poly;
  const Shade& sh = e.sh;
  prev_shadow_mask_[static_cast<u32>((y + 1) & (RING - 1))] = false;

  const s32 xa = ls.xa, xb = ls.xb;
  // One load of the profiling flag for the whole line: the kernels between
  // the checks are opaque to the compiler, so every test re-read it (~50 insn/line).
  const bool pe = prof::enabled;
  const int mode = sh.mode;
  if (pe) { prof::add(prof::C_POLY_LINES, 1); prof::add(prof::C_SPAN_PIXELS, xb > xa ? static_cast<u64>(xb - xa) : 0); }
  if (pe) {
    const u32 len = xb > xa ? static_cast<u32>(xb - xa) : 0;
    const u32 b = len <= 4 ? 0 : len <= 8 ? 1 : len <= 16 ? 2 : len <= 32 ? 3 : len <= 64 ? 4 : len <= 128 ? 5 : 6;
    prof::add(static_cast<prof::Counter>(prof::C_SL0 + b), 1);
    prof::add(static_cast<prof::Counter>(prof::C_SLPX0 + b), len);
  }
  if (pe && prof::census_same_list) {   // work a content check would have skipped
    prof::add(prof::C_POLY_LINES_SAME, 1);
    prof::add(prof::C_SPAN_PIXELS_SAME, xb > xa ? static_cast<u64>(xb - xa) : 0);
  }
  // Depth first: the pre-pass finds the pixels the span can still write
  // (against the top pixel, or the one underneath where the top carries
  // edge flags); attributes are interpolated and the span resolved only
  // within that range, and an occluded span costs its depth stage alone.
  SpanBuf& sb = spanbuf_;
  const u32 off = batch_px_;
  s32 ca = xa, cb = xa;
  if (xb > xa) {
    span_stage(sb, ls.xstart, ls.xend, xa, xb, ls.wl, ls.wr, ls.zl, ls.zr, p.wbuffer, nullptr, nullptr, false, off);
    if (sh.shadow) {
      // Shadow polygons test against whichever pixel their stencil names; no pre-pass.
      std::memset(sb.pass + off, 1, static_cast<size_t>(xb - xa)); cb = xb;
    } else {
      const u32 row = row_of(y) + 1 + xa;
      const u32 r = kern::active::depth_candidates(mode, sb.z + off, &depth_[row], &attr_[row],
                                                   static_cast<u32>(xb - xa), sb.pass + off, (sh.dispcnt >> 4) & 1);
      if (r) { ca = xa + static_cast<s32>(r >> 16); cb = xa + static_cast<s32>(r & 0xFFFF); }
    }
  }
  if (pe) prof::add(cb > ca ? prof::C_SPAN_DRAWN : (xb > xa ? prof::C_SPAN_OCCLUDED : prof::C_SPAN_EMPTY), 1);
  if (cb <= ca) return;
  if (pe) {
    // Keyed on the reason, not sh.vec, so the census reads the same on a host build.
    const prof::Counter c = sh.shadow ? prof::C_RES_SHADOW_SPANS : sh.wireframe ? prof::C_RES_WIRE_SPANS
                          : sh.blendmode == 2 ? prof::C_RES_TOON_SPANS : prof::C_RES_VEC_SPANS;
    prof::add(c, 1); prof::add(static_cast<prof::Counter>(c + 1), static_cast<u64>(cb - ca));
  }
  span_attrs(sb, ls.xstart, ls.xend, ca, cb, ls.wl, ls.wr, ls.al, ls.ar, sh.attrs_constant, sh.rgb_constant);

  SpanJob& j = jobs_[njobs_++];
  j.y = y; j.ca = ca; j.cb = cb; j.off = off + static_cast<u32>(ca - xa);
  j.xdraw = xa; j.yedge = ls.yedge; j.l_cov = ls.l_cov; j.r_cov = ls.r_cov;
  j.lim0 = std::min({ls.xstart + ls.l_len, ls.xend + 1, 256});
  j.lim1 = std::min({ls.xend - ls.r_len + 1, ls.xend + 1, 256});
  j.lim2 = std::min(ls.xend + 1, 256);
  j.l_fill = ls.l_fill; j.r_fill = ls.r_fill; j.wf_skip = ls.wf_skip;

  batch_px_ += static_cast<u32>(xb - xa);
}

// The resolve kernel for a decoded Shade.
Renderer3D::ResolveFn Renderer3D::select_resolve(const Shade& sh) {
  static constexpr ResolveFn kResolve[4][2][2][2] = {
#define DS_R(m) {{{&Renderer3D::resolve_batch<m, false, false, false>, &Renderer3D::resolve_batch<m, false, false, true>},   \
                  {&Renderer3D::resolve_batch<m, false, true, false>,  &Renderer3D::resolve_batch<m, false, true, true>}},  \
                 {{&Renderer3D::resolve_batch<m, true, false, false>,  &Renderer3D::resolve_batch<m, true, false, true>},   \
                  {&Renderer3D::resolve_batch<m, true, true, false>,   &Renderer3D::resolve_batch<m, true, true, true>}}}
    DS_R(0), DS_R(1), DS_R(2), DS_R(3)
#undef DS_R
  };
#if DSPERATE_NEON
  static constexpr ResolveFn kResolveVec[4][2][2][2] = {
#define DS_V(m) {{{&Renderer3D::resolve_batch_vec<m, false, false, false>, &Renderer3D::resolve_batch_vec<m, false, false, true>},  \
                  {&Renderer3D::resolve_batch_vec<m, false, true, false>,  &Renderer3D::resolve_batch_vec<m, false, true, true>}},  \
                 {{&Renderer3D::resolve_batch_vec<m, true, false, false>,  &Renderer3D::resolve_batch_vec<m, true, false, true>},   \
                  {&Renderer3D::resolve_batch_vec<m, true, true, false>,   &Renderer3D::resolve_batch_vec<m, true, true, true>}}}
    DS_V(0), DS_V(1), DS_V(2), DS_V(3)
#undef DS_V
  };
  if (sh.vec) return kResolveVec[sh.mode][sh.textured][(sh.dispcnt >> 4) & 1][sh.opaque];
#endif
  return kResolve[sh.mode][sh.textured][(sh.dispcnt >> 4) & 1][sh.shadow];
}

// One span's three-part walk: the left edge run, the interior, the right edge
// run, each clipped to the range the depth pre-pass left alive. `range` draws
// one clipped run; the batch kernels pass their own body, which inlines here.
template <typename Range>
[[gnu::always_inline]] inline void Renderer3D::walk_span(const SpanJob& j, Range&& range) {
  // Put the origin back where this span's pixels are: buffer index for
  // screen x is j.off + (x - j.ca).
  spanbuf_.x0 = j.ca - static_cast<s32>(j.off);
  s32 x = j.xdraw, xcov = 0;
  auto draw_span = [&](s32 xlimit, int part, int edge) {
    if (x >= xlimit) return;
    const s32 lo = std::max(x, j.ca), hi = std::min(xlimit, j.cb);
    if (lo < hi) range(j.y, lo, hi, part, edge, j.l_cov, j.r_cov, xcov);
    x = xlimit;
  };
  if (j.l_cov & static_cast<s32>(0x80000000u)) { xcov = (j.l_cov >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; }
  if (!j.l_fill) x = j.lim0; else draw_span(j.lim0, 0, j.yedge | 0x1);
  if (j.wf_skip) x = std::max(x, j.lim1); else draw_span(j.lim1, 1, j.yedge);
  if (j.r_cov & static_cast<s32>(0x80000000u)) { xcov = (j.r_cov >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; }
  if (j.r_fill) draw_span(j.lim2, 2, j.yedge | 0x2);
}

// One call per batch instead of 2.4-2.9 per span. The range body inlines into
// the job loop, so the kernel's 272-byte frame, its twelve register-pair saves
// and every piece of setup that depends only on the Shade are paid once for
// the whole batch and lifted out of the loop by the compiler.
template <int mode, bool textured, bool aa, bool shadow>
void Renderer3D::resolve_batch(const Shade& sh, const SpanJob* jobs, u32 n) {
  const SpanBuf& sb = spanbuf_;
  for (u32 k = 0; k < n; ++k)
    walk_span(jobs[k], [&](s32 y, s32 lo, s32 hi, int part, int edge, s32 lc, s32 rc, s32& xc) {
      resolve_span<mode, textured, aa, shadow>(sh, sb, y, lo, hi, part, edge, lc, rc, xc);
    });
}

#if DSPERATE_NEON
template <int mode, bool textured, bool aa, bool opq>
void Renderer3D::resolve_batch_vec(const Shade& sh, const SpanJob* jobs, u32 n) {
  const SpanBuf& sb = spanbuf_;
  rk_or_ = 0; rk_mixed_ = false; rk_groups_ = 0;
  for (u32 k = 0; k < n; ++k)
    walk_span(jobs[k], [&](s32 y, s32 lo, s32 hi, int part, int edge, s32 lc, s32 rc, s32& xc) {
      resolve_span_vec<mode, textured, aa, opq>(sh, sb, y, lo, hi, part, edge, lc, rc, xc);
    });
  if (prof::enabled) {
    prof::add(prof::C_RK_BATCHES, 1);
    if (!rk_or_) prof::add(prof::C_RK_BATCH_EMPTY, 1);
    else {
      const bool one = !rk_mixed_ && (rk_or_ & (rk_or_ - 1)) == 0;
      prof::add(one ? prof::C_RK_BATCH_UNIFORM : prof::C_RK_BATCH_MIXED, 1);
      prof::add(prof::C_RK_BATCH_GRP, rk_groups_);
      prof::add(one ? prof::C_RK_BATCH_UNIFORM_GRP : prof::C_RK_BATCH_MIXED_GRP, rk_groups_);
      if (rk_or_ == 1) prof::add(prof::C_RK_BATCH_OPAQUE, 1);
    }
  }
}
#endif

// Run the pixel stages once over everything staged, then resolve span by span.
//
// This is the point of the batching: span_texels and span_shade are called
// once for up to BATCH_PX pixels instead of once per span, and our spans
// average 13-35 pixels (scenes/README.md). Their per-call setup -- the Shade
// fields, the gather function pointer, the blend-mode decision -- is paid once
// for the batch, which is what DraStic's flush does (docs/techniques/02 s3).
//
// Batching is safe across the spans of one polygon because they lie on
// distinct scanlines, so no span in the batch reads a pixel another one
// writes.
void Renderer3D::flush_batch(const Shade& sh) {
  if (!njobs_) { batch_px_ = 0; return; }
  prof::add(prof::C_BATCHES, 1); prof::add(prof::C_BATCH_SPANS, njobs_); prof::add(prof::C_BATCH_PX, batch_px_);
#if DSPERATE_NEON
  if (sh.vec) {
    SpanBuf& sb = spanbuf_;
    // One pass over the whole batch. x0 = 0 makes buffer index equal screen x
    // for the call, so [0, batch_px_) addresses everything staged.
    sb.x0 = 0;
    const u32 n16 = (batch_px_ + 15) & ~15u;
    // Toon: the vertex colour is replaced by toon[vr >> 1] before texturing
    // (shade_pixel), a 32-entry lookup on the red plane -- one table
    // instruction per plane. Highlight: all three planes take vr, and the
    // toon colour is added to the shaded result with a clamp at 63 (below).
    const bool toonmode = sh.blendmode == 2;
    if (toonmode) {
      if (!sh.highlight) {
        const uint8x16x2_t tr = {vld1q_u8(toon6_[0]), vld1q_u8(toon6_[0] + 16)};
        const uint8x16x2_t tg = {vld1q_u8(toon6_[1]), vld1q_u8(toon6_[1] + 16)};
        const uint8x16x2_t tb = {vld1q_u8(toon6_[2]), vld1q_u8(toon6_[2] + 16)};
        for (u32 i = 0; i < n16; i += 16) {
          const uint8x16_t idx = vshrq_n_u8(vld1q_u8(sb.vr + i), 1);
          vst1q_u8(sb.vr + i, compat::tbl2q_u8(tr, idx));
          vst1q_u8(sb.vg + i, compat::tbl2q_u8(tg, idx));
          vst1q_u8(sb.vb + i, compat::tbl2q_u8(tb, idx));
        }
      } else {
        for (u32 i = 0; i < n16; i += 16) { const uint8x16_t r = vld1q_u8(sb.vr + i); vst1q_u8(sb.vg + i, r); vst1q_u8(sb.vb + i, r); }
      }
    }
    if (sh.textured) { span_texels(sh, sb, 0, static_cast<s32>(batch_px_)); span_shade<true>(sh, sb, 0, static_cast<s32>(batch_px_)); }
    else span_shade<false>(sh, sb, 0, static_cast<s32>(batch_px_));
    if (toonmode && sh.highlight) {
      // vr still holds the vertex red (span_shade only reads the planes).
      const uint8x16x2_t tr = {vld1q_u8(toon6_[0]), vld1q_u8(toon6_[0] + 16)};
      const uint8x16x2_t tg = {vld1q_u8(toon6_[1]), vld1q_u8(toon6_[1] + 16)};
      const uint8x16x2_t tb = {vld1q_u8(toon6_[2]), vld1q_u8(toon6_[2] + 16)};
      const uint8x16_t v63 = vdupq_n_u8(63);
      for (u32 i = 0; i < n16; i += 16) {
        const uint8x16_t idx = vshrq_n_u8(vld1q_u8(sb.vr + i), 1);
        u8* rec = reinterpret_cast<u8*>(sb.col + i);
        uint8x16x4_t c = vld4q_u8(rec);
        c.val[0] = vminq_u8(vaddq_u8(c.val[0], compat::tbl2q_u8(tr, idx)), v63);
        c.val[1] = vminq_u8(vaddq_u8(c.val[1], compat::tbl2q_u8(tg, idx)), v63);
        c.val[2] = vminq_u8(vaddq_u8(c.val[2], compat::tbl2q_u8(tb, idx)), v63);
        vst4q_u8(rec, c);
      }
    }
  }
#endif
  (this->*sh.resolve)(sh, jobs_.data(), njobs_);
  njobs_ = 0; batch_px_ = 0;
}

// Rasterise lines [ya, yb) polygon at a time. The active set is merged once
// for the whole chunk (polygons entering on any of its lines), then each
// polygon in list order draws every line it covers inside the chunk.
//
// Per pixel this is the same order as line-at-a-time rendering: a pixel
// belongs to exactly one line, and the polygons still reach it in list order,
// which is what the translucent blend rules, the polygon-id stencil and the
// depth interactions depend on. The edge walk is if anything more natural
// this way -- Slope::step already advances one line at a time and the cursors
// live in the Edge, so a polygon's lines are still visited in order without
// gaps.
void Renderer3D::render_chunk(s32 ya, s32 yb) {
  for (s32 y = ya; y < yb; ++y) { clear_line(y); line_touched_[y] = false; prev_shadow_mask_[static_cast<u32>((y + 1) & (RING - 1))] = false; }
  // Merge in everything that starts anywhere in the chunk, keeping list order.
  //
  // order_ is sorted by ytop and only then by list index, so the slice for a
  // whole chunk spans several ytop buckets and is NOT in list order -- merging
  // it directly puts the active list out of order and changes which polygon
  // reaches a pixel first. (render_line could merge its slice directly because
  // a single bucket is one ytop and therefore already in list order.) Sort the
  // slice by index first; it is at most the polygon count and this runs once
  // per chunk, not once per line.
  {
    const u32 nin = bucket_[yb] - bucket_[ya];
    if (nin < 32) {
      // Small bins are cheaper to sort than to touch the complete membership
      // bitmap. This is the common case for sparse scenes.
      std::copy(&order_[bucket_[ya]], &order_[bucket_[ya]] + nin, enter_.begin());
      std::sort(enter_.begin(), enter_.begin() + nin);
    } else {
      // order_ is bucketed by ytop, not by submission order. For a busy
      // tile, mark its local entries and materialise them in edge/list order;
      // this is linear in the fixed 2048-polygon list and avoids the sort's
      // log(n) work on the hot bin boundary.
      for (u32 i = bucket_[ya]; i < bucket_[yb]; ++i) {
        const u32 id = order_[i];
        enter_bits_[id >> 6] |= u64{1} << (id & 63);
      }
      u32 out = 0;
      for (u32 id = 0; id < edge_count_; ++id) {
        const u64 bit = u64{1} << (id & 63);
        if (enter_bits_[id >> 6] & bit) {
          enter_[out++] = static_cast<u16>(id);
          enter_bits_[id >> 6] &= ~bit;
        }
      }
    }
    const u16* in = enter_.data();
    u32 a = 0, b = 0, n = 0;
    while (a < active_count_ || b < nin) {
      if (b >= nin || (a < active_count_ && active_[a] < in[b])) active_next_[n++] = active_[a++];
      else active_next_[n++] = in[b++];
    }
    std::swap(active_, active_next_);
    active_count_ = n;
  }
  u32 keep = 0;
  for (u32 k = 0; k < active_count_; ++k) {
    Edge& e = built_edge(active_[k]);
    const Polygon& p = *e.poly;
    const s32 lo = p.ytop > ya ? p.ytop : ya;
    const s32 hi = p.ybot < yb ? p.ybot : yb;
    // A polygon whose ytop is inside the chunk starts at its own top line;
    // one that began earlier carries the edge state it already has.
    //
    // One batch per polygon, flushed when it fills. Everything staged shares a
    // Shade, so the pixel stages run once for up to BATCH_PX pixels instead of
    // once per span. A polygon whose spans are too narrow for that to pay
    // never enters the batch at all -- stage_line resolves it directly and
    // jobs_ is not touched.
    prof::add(prof::C_CHUNK_ENTRIES, 1);
    // Flat polygons (ybot == ytop) draw their single line at ytop; every other
    // polygon draws [lo, hi). Either way the run is one contiguous stretch of
    // scanlines, which is what precompute_lines walks.
    const bool flat = p.ybot == p.ytop;
    const s32 ry0 = flat ? p.ytop : lo;
    const s32 ry1 = flat ? (p.ytop >= ya && p.ytop < yb ? p.ytop + 1 : p.ytop) : hi;
    if (p.shadow_mask) {
      // The stencil pass walks and steps the edges itself and stages nothing.
      for (s32 y = ry0; y < ry1; ++y) {
        line_touched_[y] = true;
        flush_batch(e.sh);
        render_shadow_mask_line(e, y);
      }
    } else if (ry1 > ry0) {
      precompute_lines(e, ry0, ry1);
      for (s32 y = ry0; y < ry1; ++y) {
        line_touched_[y] = true;
        // A span can be 256 pixels wide, so flush before staging one that might
        // not fit rather than after overrunning.
        if (batch_px_ >= BATCH_PX || njobs_ == jobs_.size()) flush_batch(e.sh);
        stage_line(e, y, lines_[static_cast<u32>(y - ry0)]);
        // Batching only pays where the pixel stages are worth amortising. An
        // untextured polygon's shading is a handful of NEON ops over a dozen
        // pixels, so those flush immediately.
        if (!e.sh.textured) flush_batch(e.sh);
      }
    }
    flush_batch(e.sh);
    if (p.ybot > yb) active_[keep++] = active_[k];
  }
  active_count_ = keep;
}

// ---- final pass -----------------------------------------------------------------------

u32 Renderer3D::fog_density(u32 addr) const {
  u32 z = depth_[addr];
  u32 id, frac;
  if (z < rs_->fog_offset) { id = 0; frac = 0; }
  else {
    // (Z - offset) >> 2 << shift: bits 0-16 fraction, 17+ table index. The
    // value can wrap in 32 bits for large shifts, as on hardware.
    z -= rs_->fog_offset;
    z = (z >> 2) << rs_->fog_shift;
    id = z >> 17;
    if (id >= 32) { id = 32; frac = 0; } else frac = z & 0x1FFFF;
  }
  u32 d = ((rs_->fog_density[id] * (0x20000 - frac)) + (rs_->fog_density[id + 1] * frac)) >> 17;
  if (d >= 127) d = 128;
  return d;
}

// The scalar passes: the specification the NEON final_pass is checked against
// (selftest_final_pass), and the whole of final_pass on non-NEON builds.
void Renderer3D::final_pass_ref(s32 y) {
  const u32 dispcnt = dispcnt_;
  // Edge marking and anti-aliasing act on polygon pixels (edge flags); fog
  // on the fog bit, which the clear can set too. A line nothing touched
  // needs none of it unless the clear carries fog.
  bool work = line_touched_[y];
  if (!work) {
    const bool clear_fog = (rs_->dispcnt & (1 << 14)) || (rs_->clear_attr1 & 0x8000);
    work = (dispcnt & (1 << 7)) && clear_fog;
  }
  if (!work) { std::memcpy(&out_dst_[y * 256], &color_[row_of(y) + 1], 256 * sizeof(u32)); return; }
  if (dispcnt & (1 << 5)) {
    // Edge marking on the topmost pixels, against the four neighbours.
    const u32 up = row_of(y - 1) + 1, dn = row_of(y + 1) + 1;
    for (int x = 0; x < 256; ++x) {
      const u32 addr = row_of(y) + 1 + x;
      const u32 attr = attr_[addr];
      if (!(attr & 0xF)) continue;
      const u32 id = attr >> 24;
      const u32 z = depth_[addr];
      if ((id != (attr_[addr - 1] >> 24) && z < depth_[addr - 1]) || (id != (attr_[addr + 1] >> 24) && z < depth_[addr + 1]) ||
          (id != (attr_[up + x] >> 24) && z < depth_[up + x]) || (id != (attr_[dn + x] >> 24) && z < depth_[dn + x])) {
        u32 r, g, b; rgb15_to_666(rs_->edge[id >> 3], r, g, b);
        color_[addr] = r | (g << 8) | (b << 16) | (color_[addr] & 0xFF000000);
        attr_[addr] = (attr & 0xFFFFE0FF) | 0x00001000;    // coverage broken for the AA pass
      }
    }
  }
  if (dispcnt & (1 << 7)) {
    // Fog on the top two pixels (the lower one feeds anti-aliasing).
    const bool fogcolor = !(dispcnt & (1 << 6));
    u32 fr, fg, fb; rgb15_to_666(static_cast<u16>(rs_->fog_color), fr, fg, fb);
    const u32 fa = (rs_->fog_color >> 16) & 0x1F;
    auto apply = [&](u32 addr) {
      const u32 d = fog_density(addr);
      const u32 c = color_[addr];
      u32 r = c & 0x3F, g = (c >> 8) & 0x3F, b = (c >> 16) & 0x3F, a = (c >> 24) & 0x1F;
      if (fogcolor) {
        r = ((fr * d) + (r * (128 - d))) >> 7;
        g = ((fg * d) + (g * (128 - d))) >> 7;
        b = ((fb * d) + (b * (128 - d))) >> 7;
      }
      a = ((fa * d) + (a * (128 - d))) >> 7;
      color_[addr] = r | (g << 8) | (b << 16) | (a << 24);
    };
    const bool under = dispcnt & (1 << 4);   // the lower pixel is only read by the AA blend
    for (int x = 0; x < 256; ++x) {
      u32 addr = row_of(y) + 1 + x;
      const u32 attr = attr_[addr];
      if (attr & (1 << 15)) apply(addr);
      if (!under || !(attr & 0xF)) continue;
      addr += RSIZE;
      if (attr_[addr] & (1 << 15)) apply(addr);
    }
  }
  if (dispcnt & (1 << 4)) {
    // Anti-aliasing: blend edge pixels with the pixel underneath by coverage.
    for (int x = 0; x < 256; ++x) {
      const u32 addr = row_of(y) + 1 + x;
      const u32 attr = attr_[addr];
      if (!(attr & 0xF)) continue;
      u32 cov = (attr >> 8) & 0x1F;
      if (cov == 0x1F) continue;
      if (cov == 0) { color_[addr] = color_[addr + RSIZE]; continue; }
      const u32 top = color_[addr], bot = color_[addr + RSIZE];
      u32 tr = top & 0x3F, tg = (top >> 8) & 0x3F, tb = (top >> 16) & 0x3F, ta = (top >> 24) & 0x1F;
      const u32 br = bot & 0x3F, bg = (bot >> 8) & 0x3F, bb = (bot >> 16) & 0x3F, ba = (bot >> 24) & 0x1F;
      ++cov;
      if (ba > 0) {
        tr = ((tr * cov) + (br * (32 - cov))) >> 5;
        tg = ((tg * cov) + (bg * (32 - cov))) >> 5;
        tb = ((tb * cov) + (bb * (32 - cov))) >> 5;
      }
      ta = ((ta * cov) + (ba * (32 - cov))) >> 5;
      color_[addr] = tr | (tg << 8) | (tb << 16) | (ta << 24);
    }
  }
  std::memcpy(&out_dst_[y * 256], &color_[row_of(y) + 1], 256 * sizeof(u32));
}

#if DSPERATE_NEON
void Renderer3D::final_pass(s32 y) {
  const u32 dispcnt = dispcnt_;
  // Edge marking and anti-aliasing act on polygon pixels (edge flags); fog
  // on the fog bit, which the clear can set too. A line nothing touched
  // needs none of it unless the clear carries fog.
  bool work = line_touched_[y];
  if (!work) {
    const bool clear_fog = (rs_->dispcnt & (1 << 14)) || (rs_->clear_attr1 & 0x8000);
    work = (dispcnt & (1 << 7)) && clear_fog;
  }
  if (!work) { std::memcpy(&out_dst_[y * 256], &color_[row_of(y) + 1], 256 * sizeof(u32)); return; }
  // The three passes as 16-pixel byte-plane kernels: a record's four bytes
  // (r g b a, and edge-flags / coverage+fog / translucent id / opaque id for
  // an attribute) are deinterleaved by one ld4 into planes, the maths is done
  // 16 lanes wide in bytes and shorts, and one st4 puts the records back.
  // This is DraStic's shape for the same passes (video_3d_edge_identify_*,
  // video_3d_fog_modulate_*, the AA resolve in video_3d_resolve_bin_*):
  // no per-pixel branch, no lane extraction, and the 32-bit records are
  // never shifted apart channel by channel. Each pass matches the scalar
  // loops below it (the non-NEON build) bit for bit.
  u8* const cb = reinterpret_cast<u8*>(color_.data());
  u8* const ab = reinterpret_cast<u8*>(attr_.data());
  const u32 base = row_of(y) + 1;
  if (dispcnt & (1 << 5)) {
    // Edge marking on the topmost pixels, against the four neighbours: a
    // pixel with edge flags whose polygon id differs from a neighbour's and
    // lies in front of it takes the edge colour of its id group and a
    // broken coverage. Only ids (never written here) and depths (never
    // written) are read from the neighbours, so the 16 lanes are
    // independent of each other and of the pixels already marked.
    const u32 up = row_of(y - 1) + 1, dn = row_of(y + 1) + 1;
    const uint8x16_t er = vld1q_u8(edge6_[0]), eg = vld1q_u8(edge6_[1]), ebl = vld1q_u8(edge6_[2]);
    for (u32 x = 0; x < 256; x += 16) {
      const u32 addr = base + x;
      uint8x16x4_t at = vld4q_u8(ab + addr * 4);
      const uint8x16_t edge = vtstq_u8(at.val[0], vdupq_n_u8(0xF));
      if (compat::maxv_u8(edge) == 0) continue;
      const uint8x16_t id = at.val[3];
      uint32x4_t z[4];
      for (u32 k = 0; k < 4; ++k) z[k] = vld1q_u32(&depth_[addr + k * 4]);
      // One neighbour: the id differs and z < neighbour z, as a byte mask.
      auto nb = [&](u32 naddr) -> uint8x16_t {
        const uint8x16_t nid = vld4q_u8(ab + naddr * 4).val[3];
        uint32x4_t lt[4];
        for (u32 k = 0; k < 4; ++k) lt[k] = vcltq_u32(z[k], vld1q_u32(&depth_[naddr + k * 4]));
        const uint8x16_t lt8 = vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(lt[0]), vmovn_u32(lt[1]))),
                                           vmovn_u16(vcombine_u16(vmovn_u32(lt[2]), vmovn_u32(lt[3]))));
        return vbicq_u8(lt8, vceqq_u8(id, nid));
      };
      uint8x16_t mark = vorrq_u8(vorrq_u8(nb(addr - 1), nb(addr + 1)), vorrq_u8(nb(up + x), nb(dn + x)));
      mark = vandq_u8(mark, edge);
      if (compat::maxv_u8(mark) == 0) continue;
      const uint8x16_t idx = vshrq_n_u8(id, 3);
      uint8x16x4_t c = vld4q_u8(cb + addr * 4);
      c.val[0] = vbslq_u8(mark, compat::tbl1q_u8(er, idx), c.val[0]);
      c.val[1] = vbslq_u8(mark, compat::tbl1q_u8(eg, idx), c.val[1]);
      c.val[2] = vbslq_u8(mark, compat::tbl1q_u8(ebl, idx), c.val[2]);
      vst4q_u8(cb + addr * 4, c);
      // attr = (attr & 0xFFFFE0FF) | 0x1000: byte 1 keeps bits 5-7, coverage 0x10.
      at.val[1] = vbslq_u8(mark, vorrq_u8(vandq_u8(at.val[1], vdupq_n_u8(0xE0)), vdupq_n_u8(0x10)), at.val[1]);
      vst4q_u8(ab + addr * 4, at);
    }
  }
  if (dispcnt & (1 << 7)) {
    // Fog on the top two pixels (the lower one feeds anti-aliasing).
    const bool fogcolor = !(dispcnt & (1 << 6));
    u32 fr, fg, fb; rgb15_to_666(static_cast<u16>(rs_->fog_color), fr, fg, fb);
    const u32 fa = (rs_->fog_color >> 16) & 0x1F;
    const uint8x8_t vfr = vdup_n_u8(static_cast<u8>(fr)), vfg = vdup_n_u8(static_cast<u8>(fg)), vfb = vdup_n_u8(static_cast<u8>(fb)), vfa = vdup_n_u8(static_cast<u8>(fa));
    // The 34-entry density table in a 64-byte lookup (indices 0..33 reach it).
    alignas(16) u8 dt[64] = {};
    std::memcpy(dt, rs_->fog_density.data(), 34);
    const uint8x16x4_t dens = {vld1q_u8(dt), vld1q_u8(dt + 16), vld1q_u8(dt + 32), vld1q_u8(dt + 48)};
    const uint32x4_t voff = vdupq_n_u32(rs_->fog_offset), v32 = vdupq_n_u32(32), vfrac = vdupq_n_u32(0x1FFFF);
    const int32x4_t vshift = vdupq_n_s32(static_cast<s32>(rs_->fog_shift));
    // fog_density on 16 lanes: table index and 17-bit fraction from the
    // depth, the two table entries by lookup, and the interpolation as
    // d0 + ((d1 - d0) * frac >> 17) with an arithmetic shift, which is the
    // scalar (d0 * (2^17 - frac) + d1 * frac) >> 17 exactly.
    auto density16 = [&](u32 addr) -> uint8x16_t {
      uint32x4_t id[4], frac[4];
      for (u32 k = 0; k < 4; ++k) {
        uint32x4_t zz = vld1q_u32(&depth_[addr + k * 4]);
        const uint32x4_t below = vcltq_u32(zz, voff);
        zz = vshlq_u32(vshrq_n_u32(vsubq_u32(zz, voff), 2), vshift);
        uint32x4_t i = vshrq_n_u32(zz, 17);
        const uint32x4_t ge32 = vcgeq_u32(i, v32);
        id[k] = vbicq_u32(vminq_u32(i, v32), below);
        frac[k] = vbicq_u32(vandq_u32(zz, vfrac), vorrq_u32(ge32, below));
      }
      const uint8x16_t i8 = vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(id[0]), vmovn_u32(id[1]))),
                                        vmovn_u16(vcombine_u16(vmovn_u32(id[2]), vmovn_u32(id[3]))));
      const uint8x16_t d0 = compat::tbl4q_u8(dens, i8), d1 = compat::tbl4q_u8(dens, vaddq_u8(i8, vdupq_n_u8(1)));
      const int16x8_t dlo = vreinterpretq_s16_u16(vsubl_u8(vget_low_u8(d1), vget_low_u8(d0)));
      const int16x8_t dhi = vreinterpretq_s16_u16(compat::subl_high_u8(d1, d0));
      const int32x4_t dd[4] = {vmovl_s16(vget_low_s16(dlo)), compat::movl_high_s16(dlo), vmovl_s16(vget_low_s16(dhi)), compat::movl_high_s16(dhi)};
      const uint16x8_t d0lo = vmovl_u8(vget_low_u8(d0)), d0hi = compat::movl_high_u8(d0);
      const int32x4_t b[4] = {vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(d0lo))), vreinterpretq_s32_u32(compat::movl_high_u16(d0lo)),
                              vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(d0hi))), vreinterpretq_s32_u32(compat::movl_high_u16(d0hi))};
      uint32x4_t d[4];
      for (u32 k = 0; k < 4; ++k) {
        int32x4_t v = vaddq_s32(b[k], vshrq_n_s32(vmulq_s32(dd[k], vreinterpretq_s32_u32(frac[k])), 17));
        const uint32x4_t sat = vcgeq_u32(vreinterpretq_u32_s32(v), vdupq_n_u32(127));
        d[k] = vbslq_u32(sat, vdupq_n_u32(128), vreinterpretq_u32_s32(v));
      }
      return vcombine_u8(vmovn_u16(vcombine_u16(vmovn_u32(d[0]), vmovn_u32(d[1]))),
                         vmovn_u16(vcombine_u16(vmovn_u32(d[2]), vmovn_u32(d[3]))));
    };
    // (f * d + c * (128 - d)) >> 7 on 16 lanes; d <= 128 so 128 - d is a byte.
    auto mix = [](uint8x16_t c, uint8x8_t f, uint8x16_t d, uint8x16_t inv) -> uint8x16_t {
      const uint16x8_t lo = vmlal_u8(vmull_u8(f, vget_low_u8(d)), vget_low_u8(c), vget_low_u8(inv));
      const uint16x8_t hi = vmlal_u8(vmull_u8(f, vget_high_u8(d)), vget_high_u8(c), vget_high_u8(inv));
      return vcombine_u8(vshrn_n_u16(lo, 7), vshrn_n_u16(hi, 7));
    };
    auto apply16 = [&](u32 addr, uint8x16_t m) {
      const uint8x16_t d = density16(addr), inv = vsubq_u8(vdupq_n_u8(128), d);
      uint8x16x4_t c = vld4q_u8(cb + addr * 4);
      if (fogcolor) {
        c.val[0] = vbslq_u8(m, mix(c.val[0], vfr, d, inv), c.val[0]);
        c.val[1] = vbslq_u8(m, mix(c.val[1], vfg, d, inv), c.val[1]);
        c.val[2] = vbslq_u8(m, mix(c.val[2], vfb, d, inv), c.val[2]);
      }
      c.val[3] = vbslq_u8(m, mix(vandq_u8(c.val[3], vdupq_n_u8(0x1F)), vfa, d, inv), c.val[3]);
      vst4q_u8(cb + addr * 4, c);
    };
    const bool under = dispcnt & (1 << 4);   // the lower pixel is only read by the AA blend
    for (u32 x = 0; x < 256; x += 16) {
      const u32 addr = base + x;
      const uint8x16x4_t at = vld4q_u8(ab + addr * 4);
      const uint8x16_t fog = vtstq_u8(at.val[1], vdupq_n_u8(0x80));
      if (compat::maxv_u8(fog)) apply16(addr, fog);
      if (!under) continue;
      const uint8x16_t edge = vtstq_u8(at.val[0], vdupq_n_u8(0xF));
      if (compat::maxv_u8(edge) == 0) continue;
      const u32 under = addr + RSIZE;
      const uint8x16_t ufog = vandq_u8(edge, vtstq_u8(vld4q_u8(ab + under * 4).val[1], vdupq_n_u8(0x80)));
      if (compat::maxv_u8(ufog)) apply16(under, ufog);
    }
  }
  if (dispcnt & (1 << 4)) {
    // Anti-aliasing: blend edge pixels with the pixel underneath by coverage.
    // Coverage 31 keeps the top pixel, 0 takes the one underneath whole;
    // otherwise (cov + 1) / 32 of the top over the rest of the bottom, the
    // colour only when something is underneath, the alpha always.
    const uint8x16_t v31 = vdupq_n_u8(31), v32 = vdupq_n_u8(32), v0 = vdupq_n_u8(0), a5 = vdupq_n_u8(0x1F);
    auto blend = [](uint8x16_t t, uint8x16_t b, uint8x16_t c1, uint8x16_t c2) -> uint8x16_t {
      const uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(t), vget_low_u8(c1)), vget_low_u8(b), vget_low_u8(c2));
      const uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(t), vget_high_u8(c1)), vget_high_u8(b), vget_high_u8(c2));
      return vcombine_u8(vshrn_n_u16(lo, 5), vshrn_n_u16(hi, 5));
    };
    for (u32 x = 0; x < 256; x += 16) {
      const u32 addr = base + x;
      const uint8x16x4_t at = vld4q_u8(ab + addr * 4);
      const uint8x16_t cov = vandq_u8(at.val[1], a5);
      const uint8x16_t m = vbicq_u8(vtstq_u8(at.val[0], vdupq_n_u8(0xF)), vceqq_u8(cov, v31));
      if (compat::maxv_u8(m) == 0) continue;
      const uint8x16x4_t top = vld4q_u8(cb + addr * 4), bot = vld4q_u8(cb + (addr + RSIZE) * 4);
      const uint8x16_t c1 = vaddq_u8(cov, vdupq_n_u8(1)), c2 = vsubq_u8(v32, c1);
      const uint8x16_t ta = vandq_u8(top.val[3], a5), ba = vandq_u8(bot.val[3], a5);
      const uint8x16_t under = vtstq_u8(ba, ba), whole = vceqq_u8(cov, v0);
      uint8x16x4_t o;
      for (u32 k = 0; k < 3; ++k) o.val[k] = vbslq_u8(under, blend(top.val[k], bot.val[k], c1, c2), top.val[k]);
      o.val[3] = blend(ta, ba, c1, c2);
      for (u32 k = 0; k < 4; ++k) o.val[k] = vbslq_u8(m, vbslq_u8(whole, bot.val[k], o.val[k]), top.val[k]);
      vst4q_u8(cb + addr * 4, o);
    }
  }
  std::memcpy(&out_dst_[y * 256], &color_[row_of(y) + 1], 256 * sizeof(u32));
}
#else
void Renderer3D::final_pass(s32 y) { final_pass_ref(y); }
#endif

// Randomised buffers through final_pass and final_pass_ref with every pass
// enabled (edge marking, fog with and without colour, anti-aliasing), the
// three planes compared afterwards. Returns the number of differing words.
u32 Renderer3D::selftest_final_pass(u32 seed, u32 dispcnt) {
  RenderState rs;
  rs.dispcnt = dispcnt;
  u32 st = seed * 2654435761u + 1;
  auto rnd = [&]() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
  for (auto& e : rs.edge) e = static_cast<u16>(rnd());
  for (auto& d : rs.fog_density) d = static_cast<u8>(rnd() & 0x7F);
  rs.fog_color = rnd() & 0x1F7FFF;
  rs.fog_offset = rnd() & 0x7FFF; rs.fog_shift = rnd() & 0xF;
  const RenderState* saved = rs_;
  const u32 saved_dispcnt = dispcnt_;
  rs_ = &rs; dispcnt_ = dispcnt;
  expand_toon();   // the edge colour planes come from rs_
  u32 diffs = 0;
  for (s32 y = 0; y < 8; ++y) {
    for (u32 i = 0; i < RSIZE * 2; ++i) {
      const u32 r = rnd();
      color_[i] = (r & 0x3F3F3F) | ((r >> 3) & 0x1F000000);
      depth_[i] = (rnd() >> 8) & ((seed & 2) ? 0xFFFFFF : 0xFFFF);
      // Edge flags on most lanes, coverage over the full range, fog bit half the time.
      const u32 a = rnd();
      attr_[i] = (a & 0x3F000000) | (a & 0x7F0000) | ((a >> 20) & 0x1F00) | ((a & 0x100) ? 0x8000 : 0) | (((a >> 4) & 7) ? (a & 0xF) : 0) | (a & 0x10);
    }
    line_touched_[y] = true;
    auto c0 = color_, d0 = depth_, a0 = attr_;
    std::vector<u32> o0(256 * 8), o1(256 * 8);   // final_pass writes out_dst_[y * 256 ..]
    out_dst_ = o0.data(); final_pass_ref(y);
    auto c1 = color_, a1 = attr_;
    color_ = c0; depth_ = d0; attr_ = a0;
    out_dst_ = o1.data(); final_pass(y);
    for (u32 i = 0; i < RSIZE * 2; ++i) {
      if (color_[i] != c1[i] && !diffs) fprintf(stderr, "final_pass y=%d colour[%u] (x=%d, %s) neon %08x ref %08x attr %08x depth %08x\n", y, i, static_cast<int>(i % W) - 1, i >= RSIZE ? "under" : "top", color_[i], c1[i], a0[i], d0[i]);
      if (attr_[i] != a1[i] && !diffs) fprintf(stderr, "final_pass y=%d attr[%u] neon %08x ref %08x\n", y, i, attr_[i], a1[i]);
      diffs += (color_[i] != c1[i]) + (attr_[i] != a1[i]);
    }
    for (u32 i = 0; i < 256; ++i) diffs += o0[y * 256 + i] != o1[y * 256 + i];
  }
  out_dst_ = out_[display_].data();
  rs_ = saved; dispcnt_ = saved_dispcnt;
  return diffs;
}

// The clear is done per line, just before the line is rendered (the final
// pass of line y-1 runs after line y, so the neighbours it reads are ready);
// the one-pixel border rows (y = -1 and 192) are written into the ring just
// before the final pass of the line that reads them. The lower pixel
// buffers (AA) are never cleared, as on the reference renderer.
void Renderer3D::clear_border(s32 y) {
  const u32 clearz = ((rs_->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
  const u32 polyid = rs_->clear_attr1 & 0x3F000000;
  const u32 row = row_of(y);
  // The border is a constant row. Bulk fills avoid three scalar stores per
  // pixel and keep the clear in the same cache-friendly shape as the tile
  // buffers used by the rasteriser.
  std::fill(color_.begin() + row, color_.begin() + row + W, 0u);
  std::fill(depth_.begin() + row, depth_.begin() + row + W, clearz);
  std::fill(attr_.begin() + row, attr_.begin() + row + W, polyid);
}

void Renderer3D::clear_line(s32 y) {
  const u32 clearz = ((rs_->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
  u32 polyid = rs_->clear_attr1 & 0x3F000000;
  const u32 row = row_of(y);
  color_[row] = 0; depth_[row] = clearz; attr_[row] = polyid;
  color_[row + 257] = 0; depth_[row + 257] = clearz; attr_[row + 257] = polyid;
  u32* color = &color_[row + 1]; u32* depth = &depth_[row + 1]; u32* attr = &attr_[row + 1];
  if (rs_->dispcnt & (1 << 14)) {
    // Clear image from texture slots 2 (colour) and 3 (depth), scrolled. The
    // source line is 512 bytes, 512-aligned, so it never crosses a 16 KB
    // block: when both slots map to one bank each, the line is two contiguous
    // runs (the wrap at column 256) through the kernels instead of two
    // view lookups per pixel. A slot mapped to several banks (or none) keeps
    // the per-pixel path, which reads through the view.
    const u8 yoff = static_cast<u8>(((rs_->clear_attr2 >> 24) & 0xFF) + y);
    const u32 xoff = (rs_->clear_attr2 >> 16) & 0xFF;
    const u8* crow = texv_->direct(0x40000 + (yoff << 9), 512);
    const u8* drow = texv_->direct(0x60000 + (yoff << 9), 512);
    if (crow && drow) {
      const u16* c16 = reinterpret_cast<const u16*>(crow);
      const u16* d16 = reinterpret_cast<const u16*>(drow);
      const u32 n0 = 256 - xoff;
      kern::active::clear_image_run(c16 + xoff, d16 + xoff, n0, polyid, color, depth, attr);
      if (xoff) kern::active::clear_image_run(c16, d16, xoff, polyid, color + n0, depth + n0, attr + n0);
    } else {
      u8 xo = static_cast<u8>(xoff);
      for (int x = 0; x < 256; ++x, ++xo) {
        const u16 v2 = tex16(0x40000 + (yoff << 9) + (xo << 1));
        const u16 v3 = tex16(0x60000 + (yoff << 9) + (xo << 1));
        u32 r, g, b; rgb15_to_666(v2, r, g, b);
        const u32 a = (v2 & 0x8000) ? 0x1F000000 : 0;
        color[x] = r | (g << 8) | (b << 16) | a;
        depth[x] = ((v3 & 0x7FFF) * 0x200) + 0x1FF;
        attr[x] = polyid | (v3 & 0x8000);
      }
    }
  } else {
    u32 r, g, b; rgb15_to_666(static_cast<u16>(rs_->clear_attr1), r, g, b);
    const u32 a = (rs_->clear_attr1 >> 16) & 0x1F;
    const u32 c = r | (g << 8) | (b << 16) | (a << 24);
    polyid |= (rs_->clear_attr1 & 0x8000);
    // Most frames use a solid clear. Keep this path bulk-oriented: unlike the
    // image clear above, every destination value is uniform and needs no per
    // pixel address or VRAM lookup.
    std::fill(color, color + 256, c);
    std::fill(depth, depth + 256, clearz);
    std::fill(attr, attr + 256, polyid);
  }
}

void Renderer3D::render(const Gpu3D& gx) {
  // Before anything: the previous frame's bands read the texture cache and
  // this object's state, and the identical-frame path below mutates the cache
  // even when it renders nothing.
  sync_all();
  gx_ = &gx;
  // The slowest band of the frame just synced, kept for two frames (the
  // band-count choice below). The slots are read again by the profile after
  // the dispatch, so they are left in place.
  // The sum over workers, not the slowest: the sum is the serial raster
  // cost and does not move when the worker count does, so the choice cannot
  // oscillate with its own effect.
  { u64 sum = 0; for (u32 w = 0; w < last_nb_ && w < 8; ++w) sum += band_ns_[w]; band_sum_ns_[1] = band_sum_ns_[0]; band_sum_ns_[0] = sum; }
  // DS_ABLATE bit 0: no rasterisation and no texture work at all. See the
  // comment on ablate() in gpu.cpp -- the picture is stale from here on.
  static const bool no_raster = [] { const char* e = std::getenv("DS_ABLATE"); return e && (std::atoi(e) & 1); }();
  if (no_raster) return;
  rs_frame_ = gx.render_state();
  rs_ = &rs_frame_;
  dispcnt_ = rs_->dispcnt & (aa_ ? ~0u : ~(1u << 4));
  expand_toon();
  vm_ = &nds_.bus.vram_map();
  texv_ = &vm_->texture;
  palv_ = &vm_->texpal;
  static const bool no_cache = std::getenv("DS_NO_TEXCACHE") != nullptr;   // A/B and debugging
  if (no_cache) texcache_.set_enabled(false);
  texcache_.begin_frame(nds_.frame_count);
  const Polygon* const* polys = gx.render_polygons();
  list_polys_ = polys; list_count_ = gx.render_polygon_count();
  // A frame with no new polygon list and the same render registers draws
  // the same picture as the last one — unless a texture or palette it reads
  // changed underneath. Validating every texture it uses through the cache
  // (a memcmp per texture, once) settles that, and when nothing had to be
  // re-decoded the previous colour buffer is kept; games that run their 3D
  // at 30 fps then cost half.
  if (gx.render_identical() && texcache_.enabled() && rendered_once_ && aa_ == aa_rendered_) {
    for (u32 i = 0; i < gx.render_polygon_count(); ++i) {
      const Polygon& p = *polys[i];
      const u32 fmt = (p.texparam >> 26) & 7;
      if (p.degenerate || !(rs_->dispcnt & 1) || fmt == 0) continue;
      texcache_.lookup(*vm_, fmt, (p.texparam & 0xFFFF) << 3, 8u << ((p.texparam >> 20) & 7), 8u << ((p.texparam >> 23) & 7),
                       p.texpal, (p.texparam & (1 << 29)) ? 0 : 31);
    }
    if (texcache_.decodes_this_frame() == 0) { prof::add(prof::C_R3D_FRAMES_KEPT, 1); return; }
  }
  rendered_once_ = true;
  aa_rendered_ = aa_;
  // What the emulation thread does itself is only what it must: resolve the
  // (non-thread-safe) texture cache to plain pointers, in list order, and
  // count the polygons that draw. Edge setup -- the per-polygon slopes,
  // interpolants and buckets, ~2048 Edge records at the worst -- moved off
  // it: job 0 now builds its own edges on its pool thread exactly as workers
  // 1..n always did, and the bins are cut from the polygon list directly.
  const u32 npoly = gx.render_polygon_count();
  poly_texels_.assign(npoly, nullptr);
  u32 live = 0;
  for (u32 i = 0; i < npoly; ++i) {
    const Polygon& p = *polys[i];
    if (p.degenerate) continue;
    ++live;
    Shade sh;
    if (texture_fields(sh, p))
      poly_texels_[i] = texcache_.lookup(*vm_, sh.fmt, sh.base, static_cast<u32>(sh.width), static_cast<u32>(sh.height), sh.texpal, sh.alpha0);
  }
  texels_in_ = &poly_texels_;

  u32 maxb = band_count(live);
  // A hot compositor thread (Gpu lag mode) takes the fourth core of the
  // handhelds: three band workers beside it measured +9 % on Golden Sun's
  // title, two measured -3 % in the mean but +18 % at p99 (RG DS,
  // 2026-09-04) -- the heavy-3D frames of that title need the third worker
  // (their raster is ~20 ms on two) while the light ones want the core for
  // the compositor. The recent raster cost decides: its two workloads
  // alternate frame by frame, so the previous frame of the same phase is
  // two back -- hence the max over the last two frames. The threshold is
  // what two workers can finish in the ~240 lines between the dispatch at
  // line 215 and the last display line. The pin stays at three for frames
  // whose compositing is batched and brief.
  if (maxb > 2 && !threads_forced() && nds_.gpu.lag_active()) {
    const u64 recent = band_sum_ns_[0] > band_sum_ns_[1] ? band_sum_ns_[0] : band_sum_ns_[1];
    static const u64 threshold = [] { const char* e = std::getenv("DS_R3D_LAG_NS"); return e ? static_cast<u64>(std::atoll(e)) : kLagBandThresholdNs; }();
    if (recent < threshold) maxb = 2;
  }
  // The buffer the display is not reading; it becomes the displayed one once
  // the frame is dispatched (its lines are then waited for per band).
  u32* const dst = out_[display_ ^ 1].data();
  if (maxb <= 1) { pending_bands_ = 0; wait_ns_.store(0, std::memory_order_relaxed); build_edges(); render_band(0, 192, dst); display_ ^= 1; return; }

  // The pool is always the maximum size and only `nb` of it is given work, so
  // ramping the thread count costs a dispatch flag rather than creating and
  // joining threads mid-scene.
  if (bands_.size() < maxb - 1) {
    while (bands_.size() < maxb - 1) bands_.push_back(std::make_unique<Renderer3D>(nds_));
  }
  for (auto& b : bands_) b->aa_ = aa_;   // the setting can change between frames
  // Never replaced once it is big enough: the compositor thread may be
  // waiting on it for the previous frame's bands (sync_line).
  if (!pool_ || pool_->workers() < maxb) pool_ = std::make_unique<Pool>(maxb);

  const u32 nb = (adapt_enabled() && !threads_forced()) ? adaptive_workers(maxb) : maxb;
  last_nb_ = nb;
  wait_ns_.store(0, std::memory_order_relaxed);   // consumed by adaptive_workers; start the next frame's tally

  // Bins are cut for the maximum, not for `nb`, so that a ramp changes one
  // thing at a time: the same strips are drawn either way, by more or fewer
  // threads. Cutting them for `nb` would re-slice the frame on every ramp.
  nbins_ = bin_count(maxb);
  compute_bins(nbins_, maxb);
  const Gpu3D& gxr = gx;
  // The job outlives this call now, so it is a member, and every worker runs
  // on a pool thread -- the emulation thread's job is to go on emulating.
  // Each worker takes bins until they run out, rather than owning one band,
  // so an uneven split costs the frame nothing: whoever is free next picks up
  // the next strip of the frame.
  job_fn_ = [this, &gxr, dst](u32 w) {
    const auto t0 = std::chrono::steady_clock::now();
    Renderer3D* r = this;
    if (w != 0) { r = bands_[w - 1].get(); r->prepare_worker(gxr, list_polys_, list_count_, &poly_texels_, &rs_frame_); }
    else build_edges();   // this instance's render state is already latched
    for (;;) {
      const u32 b = pool_->claim();
      if (b >= nbins_) break;
      const s32 y0 = bin_y_[b], y1 = bin_y_[b + 1];
      if (y0 < y1) r->render_band(y0, y1, dst);
      pool_->mark_done(b);
    }
    // Per worker now, not per bin: what a thread spent on the frame. Each
    // writes its own slot, so no synchronisation; read at the next frame's
    // dispatch (after sync_all) for the band-count choice above.
    if (w < 8) band_ns_[w] = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
  };
  pending_bands_ = nbins_;
  owner_ = std::this_thread::get_id();
  {
    // The next generation's slot: sync_all above guarantees no thief of the
    // generation two back is still reading it.
    DispatchCtx& c = ctx_[(gen_ + 1) & 1];
    c.gx = &gx; c.polys = list_polys_; c.npoly = list_count_; c.texels = poly_texels_; c.rs = rs_frame_;
    c.bin_y = bin_y_; c.nbins = nbins_; c.dst = dst; c.aa = aa_;
  }
  gen_ = pool_->dispatch(job_fn_, nb, nbins_);
  display_ ^= 1;
  if (!async_) sync_all();
  // The emulation thread waits for the slowest band, so that -- not the sum --
  // is what the 3D raster costs the frame. Both are recorded: the gap between
  // them is what balancing the bands could recover.
  if (prof::enabled) {
    u64 mx = 0, sum = 0;
    for (u32 i = 0; i < nb && i < 8; ++i) {
      if (i < 4) prof::add(static_cast<prof::Counter>(prof::C_BAND0_NS + i), band_ns_[i]);
      if (band_ns_[i] > mx) mx = band_ns_[i];
      sum += band_ns_[i];
    }
    prof::add(prof::C_BAND_MAX_NS, mx);
    prof::add(prof::C_BAND_SUM_NS, sum);
  }
}

// How many bins to cut the frame into for `workers` threads.
//
// More bins than workers is what lets a heavy strip be absorbed by threads
// that finish theirs early, and it does work: at twelve bins the three
// workers came out even (2400 / 2367 / 2357 ms against 1148 / 1863 / 2972)
// and the emulation thread's wait fell from 304 ms to 71 ms over 900 frames
// of sm64.
//
// It still loses. Every boundary duplicates two scanlines -- a bin rasterises
// from y0-1, because final_pass(y0) reads the line above, and through y1,
// because final_pass(y1-1) reads the line below -- and re-runs seed_active.
// At twelve bins that is 22 of 192 lines drawn twice, and the total raster
// work rose 19 % (5983 -> 7124 ms) to save 233 ms of waiting. Device totals
// over 1800 frames of sm64: 18801 ms at three bins, 18934 at six, 19655 at
// twelve, 20394 at twenty-four; meteos is flat to slightly worse.
//
// The reason the trade is bad here is that the raster is already
// asynchronous: the emulation thread waits for a band for 2.4 % of the frame,
// so balancing the workers has almost nothing to win, while the duplicated
// lines are paid in full and compete with the emulation thread for cores.
// DraStic bins into twelve fixed 16-line strips (video_3d_bin_polygons_1x,
// twelve polygon-index lists of stride 0x1004), but its bins are lists of
// polygons handed to a rasteriser that does not re-walk edges per bin, so it
// does not pay this. So the default is one bin per worker -- the machinery is
// here, and DS_R3D_BINS turns it up, if the boundary cost is ever removed.
u32 Renderer3D::bin_count(u32 workers) {
  static const int forced = [] {
    const char* e = std::getenv("DS_R3D_BINS");
    return e ? std::atoi(e) : -1;
  }();
  u32 n = forced > 0 ? static_cast<u32>(forced) : 8;
  if (n < workers) n = workers;
  if (n > MAX_BINS) n = MAX_BINS;
  if (n > 192) n = 192;
  return n;
}

// DS_R3D_SPLIT=stair|even|desc|taper: how the bins are sized (see compute_bins).
Renderer3D::Split Renderer3D::split_mode() {
  static const Split mode = [] {
    const char* e = std::getenv("DS_R3D_SPLIT");
    if (e) {
      if (!std::strcmp(e, "even")) return Split::Even;
      if (!std::strcmp(e, "desc")) return Split::Descending;
      if (!std::strcmp(e, "taper")) return Split::Taper;
      if (!std::strcmp(e, "stair")) return Split::Ascending;
    }
    // Taper is the default since the 3-worker pin: head bins keep the
    // deadline ramp while they can still all start at once, the tail stays
    // flat so no bin strands a worker. Re-measured 2026-08-31 (6 reps,
    // paired): GSDD phase 2 −2.6 % mean / −4.3 % p99 against Even, with
    // etody, sm64 and dbori flat. Descending is better still on etody
    // (−3 % mean) but costs GSDD +3 % mean / +9 % p99, and Ascending is the
    // deadline shape, right only while every bin starts at once — measured
    // at eight bins it cost etody two thirds of its over-budget frames
    // (188 against 64) and sm64 2 % of its total.
    return Split::Taper;
  }();
  return mode;
}

// Cut points for `nbins` bins.
//
// The cost model is the number of polygons covering each line: measured per
// band on the device, time tracked polygon-lines (1.38M / 1.55M / 2.55M for
// 5.35 / 8.11 / 6.42 s) and not span pixels, which were nearly equal across
// the bands. Every line also costs a clear and a final pass whatever covers
// it, hence the +1: a bin of empty lines is not free.
//
// Shares follow the deadlines rather than being equal. The raster is
// dispatched at line 215 and bin b's output is first read at display line
// bin_y_[b] of the next frame, so bin b has (263 - 215) + bin_y_[b] lines to
// finish in -- 48 for the first one. Sized equally, the first bin cannot make
// that and the emulation thread waits at line 0 having gained nothing: with
// equal cuts and three bins the workers evened out but the wait *rose*, 304
// ms to 760. The deadlines depend on the split, so it is solved once from the
// equal-work split and then refined.
//
// The polygons' line ranges are in the list itself (ytop / ybot from the
// geometry engine), so this is a difference array and a prefix sum over 192
// entries and needs no edge setup first.
void Renderer3D::compute_bins(u32 nbins, u32 workers) {
  std::array<s32, 194> delta{};
  const Polygon* const* polys = list_polys_;
  for (u32 i = 0; i < list_count_; ++i) {
    const Polygon& p = *polys[i];
    if (p.degenerate) continue;
    const s32 y0 = p.ytop < 0 ? 0 : p.ytop;
    const s32 y1 = p.ybot > 191 ? 191 : p.ybot;
    if (y0 > 191 || y1 < y0) continue;
    ++delta[y0]; --delta[y1 + 1];
  }
  std::array<u32, 193> cum{};   // cum[y] = cost of lines [0, y)
  s32 active = 0;
  for (s32 y = 0; y < 192; ++y) {
    active += delta[y];
    cum[y + 1] = cum[y] + static_cast<u32>(active) + 1;
  }
  const u32 total = cum[192];
  bin_y_[0] = 0;
  bin_y_[nbins] = 192;
  auto split_with = [&](const std::array<u32, MAX_BINS + 1>& weight, u32 wsum) {
    u32 acc = 0;
    for (u32 b = 1; b < nbins; ++b) {
      acc += weight[b - 1];
      const u32 target = static_cast<u32>((static_cast<u64>(total) * acc) / wsum);
      s32 y = bin_y_[b - 1];
      while (y < 192 && cum[static_cast<u32>(y)] < target) ++y;
      bin_y_[b] = y;
    }
  };
  std::array<u32, MAX_BINS + 1> weight{};
  u32 wsum = 0;
  for (u32 b = 0; b < nbins; ++b) { weight[b] = 1; ++wsum; }
  split_with(weight, wsum);                       // equal work, to get deadlines
  if (split_mode() == Split::Even) return;

  // The ramp shapes, refined twice because the deadlines depend on the split.
  //
  // Ascending is the deadline shape and it is only correct while every bin
  // *starts* at the same moment, which is true exactly when there is one bin
  // per worker. Beyond that bins are claimed in sequence, so a late bin does
  // not begin until a worker frees up and most of its runway is already gone
  // -- and Ascending hands it the largest share: at eight bins the last is
  // 4.5x the first (10 12 16 19 24 30 36 45), which is one worker still
  // grinding while the others have nothing left to claim. Descending is the
  // opposite bet, sized for when a bin starts rather than when it is due.
  for (int pass = 0; pass < 2; ++pass) {
    wsum = 0;
    for (u32 b = 0; b < nbins; ++b) {
      u32 w;
      switch (split_mode()) {
      case Split::Descending: w = 48 + static_cast<u32>(192 - bin_y_[b + 1]); break;
      // Ramp while the bins can still be started early -- one per worker --
      // then flat, so the head keeps the deadline taper that gets line 0 out
      // and the tail has no bin big enough to strand anyone.
      case Split::Taper: w = 48 + static_cast<u32>(bin_y_[b < workers ? b : workers - 1]); break;
      default: w = 48 + static_cast<u32>(bin_y_[b]); break;
      }
      weight[b] = w; wsum += w;
    }
    split_with(weight, wsum);
  }
}

void Renderer3D::debug_dump(FILE* f) {
  std::fprintf(f, "  raster: async %d pending_bands %u gen %llu nbins %u display %u\n", async_ ? 1 : 0, pending_bands_, (unsigned long long)gen_, nbins_, display_);
  if (pool_) pool_->debug_dump(f);
}

Renderer3D::FrameRef Renderer3D::frame_ref() const {
  FrameRef f;
  f.out = out_[display_].data();
  f.gen = gen_;
  f.nbins = pending_bands_;
  if (pending_bands_) f.bin_y = bin_y_;
  return f;
}

// Wait for the band that owns display line `y` of frame `f`, and no other:
// the bands are independent and each writes only its own output lines, so
// the compositor can read the top of the frame while the bottom is still
// being drawn. Reads nothing of this object that render() changes -- the
// frame's cut and generation travel in the ref -- so it can run on the
// compositor thread while the emulation thread dispatches the next frame.
void Renderer3D::sync_line(const FrameRef& f, s32 y) {
  if (!f.nbins || !pool_) return;
  u32 b = 0;
  while (b + 1 < f.nbins && y >= f.bin_y[b + 1]) ++b;
  if (steal_bins(f.gen, b)) return;
  DS_PROF(R3D_WAIT);
  const u64 ns = pool_->wait_bits(f.gen, u64{1} << b);
  if (ns) wait_ns_.fetch_add(ns, std::memory_order_relaxed);
}
// Wait for all of them: before the next frame's raster, and whenever
// something is about to change what the workers are reading (Bus::update_vram
// is the only way texture VRAM or its banking can move -- the texture and
// texture-palette views are never mapped into either CPU's address space).
void Renderer3D::sync_all() {
  // Unconditionally, not just while bins are outstanding: sync_line clears
  // pending_bands_ once it has waited for every bin, and a worker can still
  // be inside the job at that point -- it marks its last bin done from in
  // there. Waiting on an idle pool costs one uncontended lock.
  u64 ns = 0;
  if (pool_ && pending_bands_) steal_bins(gen_, nbins_ - 1);
  if (pool_) { DS_PROF(R3D_WAIT); ns = pool_->wait_idle(); if (ns) wait_ns_.fetch_add(ns, std::memory_order_relaxed); }
  if (!pending_bands_) return;
  if (ns) prof::add(prof::C_R3D_SYNC_ALL, 1);   // bands were still running: something waited for the whole raster
  pending_bands_ = 0;
}

int Renderer3D::steal_mode() {
  static const int m = [] { const char* e = std::getenv("DS_R3D_STEAL"); return e ? std::atoi(e) : 1; }();
  return m;
}

// Draw unclaimed bins on the calling thread until bin `upto` is done. Bins
// are handed out in ascending Y, so an unclaimed bin is never below one a
// worker still has to reach; whatever is claimed here is on the display's
// path anyway. Returns true if `upto` is done -- without having waited.
bool Renderer3D::steal_bins(u64 gen, u32 upto) {
  // The common line finds its band drawn: one atomic load, no thread-id
  // lookup -- the fast path is what it was before stealing.
  const u64 mask = u64{1} << upto;
  if (pool_->done(gen, mask)) return true;
  const int mode = steal_mode();
  if (mode == 0 || (mode == 1 && std::this_thread::get_id() != owner_)) return false;
  // A band of this thread's own for the duration; none free means two
  // thieves are already at it, and this one waits like before.
  StealBand* sb = nullptr;
  for (StealBand& c : steal_) if (!c.busy.exchange(true, std::memory_order_acq_rel)) { sb = &c; break; }
  if (!sb) return false;
  const DispatchCtx& cx = ctx_[gen & 1];
  bool result = false;
  for (;;) {
    if (pool_->done(gen, mask)) { result = true; break; }
    const u32 c = pool_->claim_if(gen);
    if (c >= cx.nbins) { result = pool_->done(gen, mask); break; }
    if (sb->gen != gen) {
      if (!sb->band) sb->band = std::make_unique<Renderer3D>(nds_);
      sb->band->aa_ = cx.aa;
      sb->band->prepare_worker(*cx.gx, cx.polys, cx.npoly, &cx.texels, &cx.rs);
      sb->gen = gen;
    }
    const s32 y0 = cx.bin_y[c], y1 = cx.bin_y[c + 1];
    if (y0 < y1) sb->band->render_band(y0, y1, cx.dst);
    pool_->thief_done(c);
    prof::add(prof::C_R3D_STOLEN, 1);
  }
  sb->busy.store(false, std::memory_order_release);
  return result;
}

// How many workers to run this frame, from how long the emulation thread
// spent blocked on the raster in the frames before it.
//
// A fixed count cannot be right for every scene, because what an extra raster
// thread is worth is exactly what the compositor is waiting for, and that
// differs by a factor of six between scenes. Measured over 900 frames with
// eight even bins: sm64 blocks in sync_line for 2.4 % of the frame, so a
// third worker buys almost nothing and mostly takes a core off emulation --
// it measures 19605 ms against 18963 at two. etody blocks for 15.8 %, and the
// third worker cuts the raster phase from 6156 ms to 4878 and the waiting
// from 3337 ms to 1286, worth 14100 ms against 14679. Same binary, same
// bins, opposite answers.
//
// Games move between the two regimes within a scene -- etody alternates 3D
// dungeons with 2D towns -- so this ramps quickly and falls back slowly. Two
// consecutive busy frames add a worker (a dungeon should not spend a second
// under-threaded); it takes two seconds of quiet to drop one, so a town does
// not give the thread back before the next corridor asks for it, and a scene
// flipping between the two does not thrash the pool.
u32 Renderer3D::adaptive_workers(u32 max_workers) {
  constexpr u64 kFrameNs = 16715000;            // one DS frame
  constexpr u32 kDownFrames = 120;              // ~2 s of quiet before giving a worker back
  // Tunable for the sweep that picked them; see the note below.
  static const int kShift = [] {
    const char* e = std::getenv("DS_R3D_ADAPT_SHIFT");
    return e ? std::atoi(e) : 6;                // averaging window, ~64 frames (~1 s)
  }();
  static const u64 kBusyNs = [] {
    const char* e = std::getenv("DS_R3D_ADAPT_BUSY");
    const int pct = e ? std::atoi(e) : 18;
    return kFrameNs * static_cast<u64>(pct) / 100;
  }();
  const u64 kIdleNs = kBusyNs / 2;

  if (max_workers < 2) return max_workers;
  if (workers_now_ == 0) { workers_now_ = 2; wait_ema_ = 0; }

  // A long average, not a run of frames.
  //
  // The first version counted consecutive frames over the threshold, which
  // measures the wrong thing: what separates the regimes is the *mean* time
  // blocked, while a burst of heavy frames happens in all of them. sm64's
  // worst burst is 122 frames, so a short attack trips on it and the slow
  // decay then latches the count high: measured, that put sm64 at three
  // workers almost permanently and cost 5 % (19287 ms against 18297 at two).
  //
  // The window has to be long for a second reason. Blocked time at two
  // workers, b=8 even, over 900 frames: etody 15.5 %, mlbis 4.1 %, sm64
  // 1.1 %, dbori 0.1 %. Only etody wants the extra thread and its 15.5 % is
  // *sustained*; mlbis's 4.1 % is episodic, and over a short window its
  // excursions cross any threshold etody needs. Averaging over about a second
  // separates sustained from episodic, which is the actual distinction.
  wait_ema_ += (static_cast<s64>(wait_ns_.load(std::memory_order_relaxed)) - wait_ema_) >> kShift;
  const u64 avg = wait_ema_ > 0 ? static_cast<u64>(wait_ema_) : 0;

  // Ramping up needs only the average to cross, so a dungeon is served within
  // the averaging window; giving a worker back needs the average low *and*
  // held there, so a town does not hand it over before the next corridor.
  if (avg > kBusyNs) {
    quiet_frames_ = 0;
    if (workers_now_ < max_workers) ++workers_now_;
  } else if (avg < kIdleNs) {
    if (++quiet_frames_ >= kDownFrames && workers_now_ > 2) { --workers_now_; quiet_frames_ = 0; }
  } else {
    quiet_frames_ = 0;
  }
  if (workers_now_ > max_workers) workers_now_ = max_workers;
  if (prof::enabled && workers_now_ <= 4) prof::add(static_cast<prof::Counter>(prof::C_R3D_W1 + workers_now_ - 1), 1);
  return workers_now_;
}

// DS_R3D_ADAPT=1 turns the controller on; it is off otherwise.
//
// SUPERSEDED, and left here only because the reasoning below is still the right
// reasoning about a 2<->3 ramp. band_count has pinned three workers since
// 2026-08-29, and the controller ramps between two and three -- so against the
// current default it can only ever take a worker away. Measured on .20, 1800
// frames, three interleaved reps against that default: etody mean -0.13 % but
// p99 +9.33 % and over-budget frames 16 -> 31; sm64 mean +2.57 %, p99 +4.23 %,
// over-budget 14 -> 24. etody at two workers is where that comes from -- 243
// over-budget frames against 16, p99 19.5 against 16.4.
//
// The measurement in the next paragraph was taken against a TWO-worker baseline
// and does not describe what enabling this knob does today. Do not re-test the
// controller without first un-pinning band_count: the two are not independent,
// and a sweep that varies one while the other is pinned measures the pin.
//
// It costs about half a percent of throughput on the scenes that do not need
// it -- small, but real rather than noise: mlbis, sm64 and dbori are slower
// in every rep of both machines, from brief ramps that do not pay for
// themselves. It buys 6 % of etody's total time and 78 % of its over-budget
// frames (257 -> 57), and the scenes paying the half percent do not pay it in
// dips: their over-budget counts do not move. Measured three reps on each of
// two RG DS boards, summed over the five scenes: -0.71 % and -0.72 %.
//
// Threshold 18 %, not 12. The means at two workers are etody 15.5 %, mlbis
// 4.1 %, sm64 1.1 %, dbori 0.1 %. A trigger near etody's own mean is close
// enough that mlbis's episodic peaks reach it; above it, the controller fires
// on sustained blocking and little else -- which is the distinction that
// predicts whether another worker pays, rather than a value threaded between
// two particular scenes.
bool Renderer3D::adapt_enabled() {
  static const bool on = [] {
    const char* e = std::getenv("DS_R3D_ADAPT");
    return e && std::atoi(e) != 0;    // off unless asked: band_count pins three workers
  }();
  return on;
}

bool Renderer3D::threads_forced() {
  static const bool on = std::getenv("DS_R3D_THREADS") != nullptr;
  return on;
}

// How many bands to split the frame into. DS_R3D_THREADS overrides the count
// (0 or 1 disables banding). Only near-empty frames stay on one thread:
// polygon *count* says nothing about raster cost (a skybox or a full-screen
// quad is a full frame of spans), and skipping banding below 24 polygons
// cost 12 % of the whole win on SM64DS.
static u32 g_band_cap = 0;
void Renderer3D::set_band_cap(u32 cap) { g_band_cap = cap; }

u32 Renderer3D::band_count(u32 polygons) {
  static const int forced = [] {
    const char* e = std::getenv("DS_R3D_THREADS");
    return e ? std::atoi(e) : -1;
  }();
  if (forced >= 0) return forced < 1 ? 1u : static_cast<u32>(forced);
  if (polygons < 2) return 1;
  if (g_band_cap) return g_band_cap;
  // Pinned at three since 2026-08-29 (RG DS knob sweep against the CPU cuts
  // of 2026-08-28: mlbis -6.8 %, etody -2.6 %, sm64 -1.4 %, meteos/dbori flat;
  // GSDD +5.8 %, whose main thread is the critical path). Before the pin a
  // third worker only paid where the emulation thread was blocked on the
  // raster (etody +4.3 %; sm64, mlbis, dbori lost 3-9 %), hence the 2<->3
  // controller, which stays behind DS_R3D_ADAPT=1.
  return 3;
}

// Set up a worker to render a band of the frame the coordinator has latched.
void Renderer3D::prepare_worker(const Gpu3D& gx, const Polygon* const* polys, u32 npoly, const std::vector<const u32*>* texels, const RenderState* rs) {
  gx_ = &gx;
  list_polys_ = polys; list_count_ = npoly;
  rs_ = rs;
  dispcnt_ = rs_->dispcnt & (aa_ ? ~0u : ~(1u << 4));
  expand_toon();
  vm_ = &nds_.bus.vram_map();
  texv_ = &vm_->texture;
  palv_ = &vm_->texpal;
  texels_in_ = texels;
  build_edges();
}

// List the live polygons and bucket them by top line. Every instance repeats
// this: the edge cursors are walked per line and so cannot be shared. The
// edges themselves are built on first use (built_edge).
void Renderer3D::build_edges() {
  const Polygon* const* polys = list_polys_;
  u32 n = 0;
  for (u32 i = 0; i < list_count_; ++i) {
    if (polys[i]->degenerate) continue;
    edges_[n].poly = polys[i];
    edge_list_[n++] = static_cast<u16>(i);
  }
  edge_built_.fill(0);
  edge_count_ = n;
  rendered_upto_ = 0;   // cursors are fresh at ytop again; nothing re-entered yet
  // Bucket the polygons by their top line (counting sort, list order kept).
  // A polygon above the screen starts at line 0; one below it is dropped.
  auto top_line = [&](const Polygon& p) { return p.ytop < 0 ? 0 : p.ytop; };
  bucket_.fill(0);
  for (u32 i = 0; i < n; ++i) { const s32 t = top_line(*edges_[i].poly); if (t < 192) ++bucket_[t + 1]; }
  for (int y = 0; y < 193; ++y) bucket_[y + 1] = static_cast<u16>(bucket_[y + 1] + bucket_[y]);
  std::array<u16, 194> fill = bucket_;
  for (u32 i = 0; i < n; ++i) { const s32 t = top_line(*edges_[i].poly); if (t < 192) order_[fill[t]++] = static_cast<u16>(i); }
}

// The active set as it stands at the *start* of line y: every polygon that
// began strictly above y and is still alive there, in list order (which the
// blending rules depend on). Polygons whose top line is y itself are merged
// in by render_line, exactly as in the sequential walk. Their edges are
// positioned directly at y, which Slope::setup supports.
void Renderer3D::seed_active(s32 y) {
  active_ = active_buf_[0].data();
  active_next_ = active_buf_[1].data();
  active_count_ = 0;
  if (y <= 0) return;
  for (u32 i = 0; i < edge_count_; ++i) {
    const Polygon& p = *edges_[i].poly;
    const s32 t = p.ytop < 0 ? 0 : p.ytop;
    if (t >= y || y >= p.ybot) continue;
    active_[active_count_++] = static_cast<u16>(i);
    Edge& e = built_edge(i);
    if (p.ytop != p.ybot) { setup_left_edge(e, y); setup_right_edge(e, y); }
  }
}

// Rasterise output lines [y0, y1) into dst. The final pass of a line reads
// the lines either side of it, so one extra line above is rasterised (and
// the border row stands in at the top and bottom of the screen).
void Renderer3D::render_band(s32 y0, s32 y1, u32* dst) {
  out_dst_ = dst;
  const s32 first = y0 > 0 ? y0 - 1 : 0;
  const s32 last = y1 + 1 < 192 ? y1 + 1 : 192;
  // Bins overlap by the line or two final_pass needs either side of a
  // boundary, so a polygon starting in the overlap is entered twice when the
  // same instance draws both bins. seed_active only re-seeds polygons that
  // began strictly above `first`; the ones starting inside the overlap are
  // merged by render_chunk, which expects their cursors still at ytop. Put
  // them back. order_ is bucketed by ytop, so this is exactly that slice.
  if (rendered_upto_ > first) {
    const s32 hi = rendered_upto_ < last ? rendered_upto_ : last;
    for (u32 i = bucket_[first]; i < bucket_[hi]; ++i)
      if (edge_is_built(order_[i])) rewind_edge(edges_[order_[i]]);   // an unbuilt edge is built at ytop when entered
  }
  if (last > rendered_upto_) rendered_upto_ = last;
  seed_active(first);
  if (y0 == 0) { DS_PROF(R3D_CLEAR); clear_border(-1); }
  // The stage timers are accumulated in locals and flushed once per band
  // rather than scoped per line: a scope per line is two clock reads per
  // stage per line per band, which is a measurable share of what it measures.
  const bool timing = prof::enabled;
  u64 spans_ns = 0, final_ns = 0;
  auto now = [] { return std::chrono::steady_clock::now(); };
  auto t = timing ? now() : std::chrono::steady_clock::time_point{};
  auto lap = [&](u64& into) { if (!timing) return; const auto n = now(); into += static_cast<u64>((n - t).count()); t = n; };

  // Rasterise a chunk at a time, then run the final pass over every line the
  // chunk completed. A line's final pass reads its two neighbours, so it lags
  // the raster by one line; CHUNK is two short of RING so the two lines still
  // waiting on it are never the ones the next chunk overwrites.
  s32 rasterised = first;   // lines [first, rasterised) are drawn
  s32 done = y0;            // next line still needing its final pass
  while (done < y1) {
    if (rasterised < 192) {
      // Stop at the last line this band can use. final_pass(y1-1) reads y1, so
      // y1 + 1 is the exclusive bound; rounding up to a whole CHUNK instead
      // rasterises lines that belong to the next band, which draws them again.
      // That overlap is proportional to CHUNK and was 18.8 % of all polygon
      // lines at CHUNK 14, 8.2 % at CHUNK 6 (sm64, 3 bands).
      const s32 want = y1 + 1 < 192 ? y1 + 1 : 192;
      s32 end = rasterised + CHUNK < 192 ? rasterised + CHUNK : 192;
      if (end > want) end = want;
      render_chunk(rasterised, end);
      rasterised = end;
      lap(spans_ns);
    }
    s32 upto = rasterised - 1;
    if (rasterised >= 192) { clear_border(192); upto = 192; lap(final_ns); }
    if (upto > y1) upto = y1;
    while (done < upto) { final_pass(done); ++done; }
    lap(final_ns);
  }
  prof::add_ns(prof::R3D_SPANS, spans_ns);
  prof::add_ns(prof::R3D_FINAL, final_ns);
}


template <class S> void Renderer3D::sync_output(S& s) {
  sync_all();
  if constexpr (S::reading) { reset(); texcache_.clear(); }
  s.begin("R3DO");
  s.fields(rendered_once_, out_[display_]);   // reset() above leaves display_ at 0 on a load
  s.end();
}
template void Renderer3D::sync_output<state::Writer>(state::Writer&);
template void Renderer3D::sync_output<state::Reader>(state::Reader&);

} // namespace ds::gpu
