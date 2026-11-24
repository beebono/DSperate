// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/mem/timing.h"
#include "core/profile.h"
#include "core/cpu/cpu.h"
#include "core/nds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ds::mem {

constexpr u32 CACHE_CODE = 3, CACHE_DATA = 3;   // cycles for a cached fetch/access (line fill approximation)

Timing::Timing()
    : pu_map(new u8[0x100000]), retime_flags_(new u8[0x100000]), bus9_(new u8[0x40000 * 8]), regions9_(new u8[0x40000]),
      tim7_(new u8[COST7_OFFSET + COST7_BYTES]), regions7_(new u8[0x20000]) {
  std::memset(retime_flags_.get(), 0, 0x100000);
  share9();
  reset();
}

void Timing::reset() {
  cost7_ready_ = false;   // set_region7 calls below rebuild once, at the end
  std::memset(pu_map.get(), 0, 0x100000);
  pu_memo = PuMemo{};
  retime_clear();
  set_region9(0x00000000, 0xFFFFFFFF, REGION_NONE, 32, 1, 1);
  set_region9(0xFFFF0000, 0xFFFFFFFF, REGION_BIOS, 32, 1, 1);
  set_region9(0x02000000, 0x03000000, REGION_MAIN_RAM, 16, 8, 1);
  set_region9(0x03000000, 0x04000000, REGION_WRAM, 32, 1, 1);
  set_region9(0x04000000, 0x05000000, REGION_IO, 32, 1, 1);
  set_region9(0x05000000, 0x06000000, REGION_PALETTE, 16, 1, 1);
  set_region9(0x06000000, 0x07000000, REGION_VRAM, 16, 1, 1);
  set_region9(0x07000000, 0x08000000, REGION_OAM, 32, 1, 1);
  set_region9(0x08000000, 0x0A000000, REGION_GBA_ROM, 16, 10, 6);
  set_region9(0x0A000000, 0x0B000000, REGION_GBA_RAM, 8, 10, 10);
  set_region7(0x00000000, 0xFFFFFFFF, REGION_NONE, 32, 1, 1);
  set_region7(0x00000000, 0x00010000, REGION_BIOS, 32, 1, 1);
  set_region7(0x02000000, 0x03000000, REGION_MAIN_RAM, 16, 8, 1);
  set_region7(0x03000000, 0x04000000, REGION_WRAM, 32, 1, 1);
  set_region7(0x04000000, 0x04800000, REGION_IO, 32, 1, 1);
  set_region7(0x04800000, 0x04808000, REGION_WIFI0, 32, 1, 1);
  set_region7(0x04808000, 0x04810000, REGION_WIFI1, 32, 1, 1);
  set_region7(0x06000000, 0x07000000, REGION_VRAM, 16, 1, 1);
  // CPU table: no PU yet -> nothing cached. One slot per 16 KB bus entry,
  // stored as a word (byte stores here were a visible share of boot time).
  for (u32 g = 0; g < 0x40000; ++g) {
    const u8* b = &bus9_[g * 8];
    u8 slot[8];
    const u32 sh = clock9_shift;
    slot[0] = static_cast<u8>(b[2] << sh); slot[1] = static_cast<u8>(b[0] << sh); slot[2] = static_cast<u8>(b[2] << sh); slot[3] = static_cast<u8>(b[3] << sh);
    slot[4] = slot[0]; slot[5] = slot[1]; slot[6] = slot[2]; slot[7] = slot[3];
    u64 w; std::memcpy(&w, slot, 8);
    if (shared_ && g * 4 >= SHARED_LO && g * 4 < SHARED_HI) {   // one region up there (REGION_NONE, above): one page of slots
      if (g * 4 == SHARED_LO) for (size_t k = 0; k < host_page_ / 8; ++k) std::memcpy(&cpu9_[(SHARED_LO + k) * 8], &w, 8);
      continue;
    }
    u64* c = reinterpret_cast<u64*>(&cpu9_[g * 4 * 8]);
    c[0] = w; c[1] = w; c[2] = w; c[3] = w;
  }
  if (shared_) {
    build_refill9(0, SHARED_LO);
    const u32 x = cpu9_[SHARED_LO * 8];   // never 0xFF: nothing is cacheable yet
    const u32 r = (x + x) | ((x + x) << 8) | ((x + x) << 16) | (x << 24);   // as build_refill9, every neighbour the same
    for (size_t k = 0; k < host_page_ / 4; ++k) std::memcpy(refill9_rw() + (SHARED_LO + k) * 4, &r, 4);
    build_refill9(SHARED_HI - 1, 0x100000);   // the range's last page too: through put_refill9, which checks it
  } else build_refill9(0, 0x100000);
  build_cost7();
  ++cpu9_version;
}

// Mirrors refill_cycles()/fetch_cost9() (cpu_cycles.h): branch fetch on a
// cacheable page (0xFF) is a line fill (3), sequential fetch is a hit (1)
// unless it starts a line (3); else costs the page's code byte.
void Timing::build_refill9(u32 first_page, u32 last_page) {
  auto X = [&](u32 p) { const u8 c = cpu9_[p * 8]; return c == 0xFF ? 3u : c; };
  auto Y = [&](u32 p) { const u8 c = cpu9_[p * 8]; return c == 0xFF ? 1u : c; };
  if (last_page > 0x100000) last_page = 0x100000;
  for (u32 p = first_page; p < last_page; ++p) {
    const u32 x = X(p), xn = p + 1 < 0x100000 ? X(p + 1) : x;
    const u32 r = (x + Y(p)) | ((x + x) << 8) | ((x + xn) << 16) | (x << 24);
    put_refill9(p, r);
  }
}

// A refill entry by itself: in the shared range it has to be the one every page there has.
void Timing::put_refill9(u32 page, u32 entry) {
  u8* at = refill9_rw() + page * 4;
  if (shared_ && page >= SHARED_LO && page < SHARED_HI) {
    u32 cur; std::memcpy(&cur, at, 4);
    if (cur == entry) return;
    unshare9();
  }
  std::memcpy(at, &entry, 4);
}

Timing::~Timing() {
#if defined(__linux__)
  if (mapped_) { munmap(cpu9_, CPU9_BYTES + REFILL9_BYTES); return; }
#endif
  delete[] cpu9_;
}

void Timing::share9() {
  shared_ = mapped_ = false;
#if defined(__linux__) && defined(SYS_memfd_create)
  static const bool want = [] { const char* e = std::getenv("DS_TIMING_SHARE"); return !e || e[0] != '0'; }();
  host_page_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t total = CPU9_BYTES + REFILL9_BYTES;
  void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base != MAP_FAILED) {
    cpu9_ = static_cast<u8*>(base); mapped_ = true;
    const bool fits = host_page_ >= 4096 && host_page_ <= 65536 && (host_page_ & (host_page_ - 1)) == 0;
    const int fd = want && fits ? static_cast<int>(syscall(SYS_memfd_create, "dsperate-timing", 1u /* MFD_CLOEXEC */)) : -1;
    if (fd >= 0) {
      bool ok = ftruncate(fd, static_cast<off_t>(2 * host_page_)) == 0;
      // Page 0 of the file under every host page of the slots, page 1 under every one of the refill entries.
      for (int t = 0; t < 2 && ok; ++t) {
        u8* lo = t == 0 ? cpu9_ + size_t{SHARED_LO} * 8 : refill9_rw() + size_t{SHARED_LO} * 4;
        u8* hi = t == 0 ? cpu9_ + size_t{SHARED_HI} * 8 : refill9_rw() + size_t{SHARED_HI} * 4;
        for (u8* p = lo; p < hi && ok; p += host_page_)
          ok = mmap(p, host_page_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(t * host_page_)) != MAP_FAILED;
      }
      close(fd);
      shared_ = true;
      if (!ok) unshare9();   // e.g. out of mappings: private memory over whatever was laid
    }
    return;
  }
#endif
  cpu9_ = new u8[CPU9_BYTES + REFILL9_BYTES];
}

// The shared range gets memory of its own, holding what it showed.
void Timing::unshare9() {
  if (!shared_) return;
  shared_ = false;
  if (std::getenv("DS_DEBUG_TIMING")) std::fprintf(stderr, "[timing] the open range gets its own memory\n");
#if defined(__linux__)
  for (int t = 0; t < 2; ++t) {
    const size_t unit = t == 0 ? 8 : 4;
    u8* lo = (t == 0 ? cpu9_ : refill9_rw()) + size_t{SHARED_LO} * unit;
    const size_t bytes = size_t{SHARED_HI - SHARED_LO} * unit;
    std::vector<u8> was(lo, lo + host_page_);
    if (mmap(lo, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
      std::fprintf(stderr, "timing: cannot give the ARM9 table its own memory\n");
      std::abort();
    }
    for (size_t o = 0; o < bytes; o += host_page_) std::memcpy(lo + o, was.data(), host_page_);
  }
#endif
}

// ARM7 data cost, evaluated once per (page, translate-time constants) instead
// of per access. Mirrors emit_charge_data_body's ARM7 arm in translate.cpp
// exactly, including signed maxima; the fuzzer checks the two agree.
void Timing::build_cost7() {
  // Distinct code-fetch costs the ARM7 region table produces; outside this
  // set keeps the inline model (nc7_index returns -1).
  u32 n = 0;
  for (u32 i = 0; i < NC7_SLOTS; ++i) nc7_values_[i] = 0xFFFFFFFFu;
  for (u32 p = 0; p < 0x20000 && n < NC7_SLOTS; ++p) {
    const u8* t = &bus7()[p * 4];
    const u32 nc_slots[2] = {0u, 2u};            // numC_nonseq7: n16 (Thumb) or n32 (ARM)
    for (u32 k : nc_slots) {
      bool seen = false;
      for (u32 i = 0; i < n; ++i) if (nc7_values_[i] == t[k]) seen = true;
      if (!seen && n < NC7_SLOTS) nc7_values_[n++] = t[k];
    }
  }

  if (std::getenv("DS_DEBUG_TIMING")) {
    std::fprintf(stderr, "[timing] arm7 nc slots:");
    for (u32 i = 0; i < NC7_SLOTS; ++i)
      if (nc7_values_[i] != 0xFFFFFFFFu) std::fprintf(stderr, " %u", nc7_values_[i]);
    std::fprintf(stderr, "\n");
  }

  build_cost7_range(0, 0x20000);
  cost7_ready_ = true;
}

// Fill [first_page, last_page). The 32-byte block depends only on the page's
// two bus bytes and whether it's main RAM, and consecutive pages nearly
// always agree, so it's evaluated once per change of input and copied
// otherwise.
void Timing::build_cost7_range(u32 first_page, u32 last_page) {
  auto smax = [](s32 a, s32 b) { return a > b ? a : b; };
  u8 block[COST7_STRIDE];
  u32 key = 0xFFFFFFFFu;   // (nd16, nd32, data_main) of `block`
  for (u32 p = first_page; p < last_page; ++p) {
    const u8* t = &bus7()[p * 4];
    const bool data_main = (p >> 9) == 2;        // (addr >> 24) == 2
    const u32 k = t[0] | (t[2] << 8) | (data_main ? 0x10000u : 0u);
    if (k != key) {
      key = k;
      for (u32 cm = 0; cm < 2; ++cm)
        for (u32 cdi = 0; cdi < 2; ++cdi)
          for (u32 ni = 0; ni < NC7_SLOTS; ++ni)
            for (u32 w = 0; w < 2; ++w) {
              const u32 v = nc7_values_[ni];
              if (v == 0xFFFFFFFFu) { block[cost7_offset(cm, cdi, ni, w)] = 0; continue; }
              const s32 nc = static_cast<s32>(v);
              const s32 nd = t[w ? 2 : 0];         // singles are never sequential
              s32 cost;
              if (data_main) {
                if (cm) cost = nd + nc;
                else {
                  const s32 ncx = nc + static_cast<s32>(cdi);
                  cost = smax(smax(ncx, nd), nd + ncx - 3);
                }
              } else {
                if (cm) {
                  const s32 ndp = nd + static_cast<s32>(cdi);
                  cost = smax(smax(nc, ndp), ndp + nc - 3);
                } else {
                  cost = nd + nc + static_cast<s32>(cdi);
                }
              }
              block[cost7_offset(cm, cdi, ni, w)] = static_cast<u8>(cost);
            }
    }
    std::memcpy(&cost7_rw()[p * COST7_STRIDE], block, COST7_STRIDE);
  }
}

void Timing::set_region9(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq) {
  const int n16 = nonseq, s16 = seq;
  const int n32 = bus_width == 16 ? n16 + s16 : n16, s32 = bus_width == 16 ? s16 + s16 : s16;
  const int cpu_n = (r == REGION_MAIN_RAM) ? 0 : 3;
  const u32 first = start >> 14, last = (end == 0xFFFFFFFF) ? 0x40000 : (end >> 14);
  for (u32 i = first; i < last; ++i) {
    u8* t = &bus9_[i * 8];
    t[0] = static_cast<u8>(n16 + cpu_n); t[1] = static_cast<u8>(s16); t[2] = static_cast<u8>(n32 + cpu_n); t[3] = static_cast<u8>(s32);
    t[4] = static_cast<u8>(n16); t[5] = static_cast<u8>(s16); t[6] = static_cast<u8>(n32); t[7] = static_cast<u8>(s32);
    regions9_[i] = r;
  }
  ++stamp;
}

void Timing::set_region7(u32 start, u32 end, Region r, int bus_width, int nonseq, int seq) {
  const int n16 = nonseq, s16 = seq;
  const int n32 = bus_width == 16 ? n16 + s16 : n16, s32 = bus_width == 16 ? s16 + s16 : s16;
  const u32 first = start >> 15, last = (end == 0xFFFFFFFF) ? 0x20000 : (end >> 15);
  for (u32 i = first; i < last; ++i) {
    u8* t = &bus7()[i * 4];
    t[0] = static_cast<u8>(n16); t[1] = static_cast<u8>(s16); t[2] = static_cast<u8>(n32); t[3] = static_cast<u8>(s32);
    regions7_[i] = r;
  }
  // Keep the precomputed cost table in step; a stale entry is a silent
  // timing divergence. A cost not in the nc slots falls back to the inline
  // model (nc7_index returns -1).
  if (cost7_ready_) build_cost7_range(first, last);
  ++stamp;
}

// One page's slot: depends on the 16 KB bus entry `b`, the page's two PU
// cacheability bits, and whether a TCM covers it.
// Stores ([5..7]): the ARM946E-S data cache doesn't allocate on write, so a
// store to a cacheable page that misses goes out at bus speed; only TCMs
// are free. DSi prices stores as cache hits instead (DSi PictoChat's boot
// heap needs it): `store_bus` false.
static u64 compose9(const u8* b, u8 pu, bool tcm_i, bool tcm_d, u32 sh, bool store_bus) {
  u8 c[8];
  c[0] = tcm_i ? 1 : (pu & 0x40) ? 0xFF : static_cast<u8>(b[2] << sh);
  if (tcm_i || tcm_d) { c[1] = 1; c[2] = 1; c[3] = 1; }
  else if (pu & 0x10) { c[1] = CACHE_DATA; c[2] = CACHE_DATA; c[3] = 1; }
  else { c[1] = static_cast<u8>(b[0] << sh); c[2] = static_cast<u8>(b[2] << sh); c[3] = static_cast<u8>(b[3] << sh); }
  c[4] = c[0];
  if (!(tcm_i || tcm_d) && (pu & 0x10) && store_bus) { c[5] = static_cast<u8>(b[0] << sh); c[6] = static_cast<u8>(b[2] << sh); c[7] = static_cast<u8>(b[3] << sh); }
  else { c[5] = c[1]; c[6] = c[2]; c[7] = c[3]; }
  u64 w; std::memcpy(&w, c, 8); return w;
}

// TCM windows are baked into the table (4 KB pages, both windows aligned to
// their size): an ITCM page costs 1 for code and data, a DTCM page 1 for
// data, so both engines cost an access with one table lookup.
void Timing::update_cpu9(const CpuContext& cpu, u32 start, u32 end, bool notify) {
  prof::add(prof::C_TIMING_UPDATE_CPU9, 1);
  static const bool debug = std::getenv("DS_DEBUG_TIMING") != nullptr;   // logs every rebuild
  if (debug) std::fprintf(stderr, "[timing] update_cpu9 %08x-%08x ctl %08x dtcm %08x itcm %08x pu %08x/%08x\n", start, end, cpu.cp15_control, cpu.cp15_dtcm, cpu.cp15_itcm, cpu.pu_data_cacheable, cpu.pu_code_cacheable);
  const u32 first = start >> 12, last = (end == 0xFFFFFFFF) ? 0x100000 : (end >> 12);
  const u32 lo = std::max(first, SHARED_LO), hi = std::min(last, SHARED_HI);
  if (shared_ && lo < hi && shared_range9(cpu, lo, hi)) {
    if (first < lo) rebuild_cpu9(cpu, first, lo);
    if (hi < last) rebuild_cpu9(cpu, hi, last);
    // The entries that read a neighbour across the range's two ends, now that both sides are as they will
    // be: the page below it, and its own last page, which put_refill9 takes out of sharing if the page
    // above makes that entry another.
    build_refill9(SHARED_LO - 1, SHARED_LO);
    build_refill9(SHARED_HI - 1, SHARED_HI);
  } else if (first < last) {
    rebuild_cpu9(cpu, first, last);
  }
  ++cpu9_version;
  if (notify) notify_cpu9(cpu);
}

// [first, last) of the shared range, while it is shared. True: done, as one page of slots. False: the range
// would not stay one slot throughout, so it has its own memory now and is rebuilt like the rest.
bool Timing::shared_range9(const CpuContext& cpu, u32 first, u32 last) {
  const u64 itcm_end = cpu.itcm_size >> 12;
  const u64 dtcm_lo = cpu.dtcm_mask ? (cpu.dtcm_base >> 12) : 0x100000, dtcm_hi = cpu.dtcm_mask ? ((static_cast<u64>(cpu.dtcm_base) + (~cpu.dtcm_mask + 1ull)) >> 12) : 0x100000;
  const u8* pu = pu_map.get() + first;
  const u32 g0 = first >> 2, g1 = (last - 1) >> 2;
  const bool uniform = itcm_end <= first && (dtcm_hi <= first || dtcm_lo >= last)
                    && std::memcmp(pu, pu + 1, last - first - 1) == 0
                    && std::memcmp(&bus9_[g0 * 8], &bus9_[(g0 + 1) * 8], size_t{g1 - g0} * 8) == 0;
  if (!uniform && std::getenv("DS_DEBUG_TIMING")) std::fprintf(stderr, "[timing] not uniform %05x-%05x: itcm_end %llx dtcm %llx-%llx pu %d bus %d\n", first, last, (unsigned long long)itcm_end, (unsigned long long)dtcm_lo, (unsigned long long)dtcm_hi, std::memcmp(pu, pu + 1, last - first - 1) == 0, std::memcmp(&bus9_[g0 * 8], &bus9_[(g0 + 1) * 8], size_t{g1 - g0} * 8) == 0);
  if (!uniform) { unshare9(); return false; }
  const u64 W = compose9(&bus9_[g0 * 8], pu[0], false, false, clock9_shift, !(cpu.nds && cpu.nds->dsi));
  u64 old_w; std::memcpy(&old_w, &cpu9_[SHARED_LO * 8], 8);
  if (W == old_w) return true;
  if (first != SHARED_LO || last != SHARED_HI) { unshare9(); return false; }   // part of it moved
  for (size_t k = 0; k < host_page_ / 8; ++k) std::memcpy(&cpu9_[(SHARED_LO + k) * 8], &W, 8);
  const u8 code = static_cast<u8>(W), data = static_cast<u8>(W >> 16), old_code = static_cast<u8>(old_w), old_data = static_cast<u8>(old_w >> 16);
  if (code != old_code) {
    const u32 x = code == 0xFF ? 3u : code, y = code == 0xFF ? 1u : code;
    const u32 R = static_cast<u8>(x + y) | (static_cast<u32>(static_cast<u8>(x + x)) << 8) | (static_cast<u32>(static_cast<u8>(x + x)) << 16) | (static_cast<u32>(static_cast<u8>(x)) << 24);
    for (size_t k = 0; k < host_page_ / 4; ++k) std::memcpy(refill9_rw() + (SHARED_LO + k) * 4, &R, 4);
  }
  // Every page flagged, none listed: the list is declared incomplete, and retime_clear wipes the lot.
  if (const u8 f = static_cast<u8>((code != old_code ? RETIME_CODE : 0) | (data != old_data ? RETIME_DATA : 0))) {
    u8* fl = retime_flags_.get();
    for (u32 p = SHARED_LO; p < SHARED_HI; ++p) fl[p] |= f;
    retime_overflow_ = true;
  }
  return true;
}

void Timing::rebuild_cpu9(const CpuContext& cpu, u32 first, u32 last) {
  const bool store_bus = !(cpu.nds && cpu.nds->dsi);
  const u32 sh = clock9_shift;
  const auto compose = [&](const u8* b, u8 pu, bool tcm_i, bool tcm_d) { return compose9(b, pu, tcm_i, tcm_d, sh, store_bus); };
  const auto bus_word = [&](u32 group) { u64 w; std::memcpy(&w, &bus9_[group * 8], 8); return w; };
  // Walk is by runs, not pages, since a full rebuild visits a million pages.
  // TCMs are two page intervals (sizes are powers of two, aligned); between
  // their edges a run with the same PU bits and bus entry shares one slot,
  // and whole 16-page blocks are skipped where nothing changed.
  const u64 itcm_end = cpu.itcm_size >> 12;
  const u64 dtcm_lo = cpu.dtcm_mask ? (cpu.dtcm_base >> 12) : 0x100000, dtcm_hi = cpu.dtcm_mask ? ((static_cast<u64>(cpu.dtcm_base) + (~cpu.dtcm_mask + 1ull)) >> 12) : 0x100000;
  const u8* pu_map_p = pu_map.get();
  u8* const refill = refill9_rw();
  u32 x_prev = 0, y_prev = 0; bool changed_prev = false;
  u32 i = first;
  while (i < last) {
    u32 seg_end = last;
    for (u64 e : {itcm_end, dtcm_lo, dtcm_hi}) if (e > i && e < seg_end) seg_end = static_cast<u32>(e);
    const bool ti = i < itcm_end, td = i >= dtcm_lo && i < dtcm_hi;
    while (i < seg_end) {
      const u8 pu = pu_map_p[i];
      const u64 bw = bus_word(i >> 2);
      const u64 W = compose(&bus9_[(i >> 2) * 8], pu, ti, td);
      const u8 code = static_cast<u8>(W), data = static_cast<u8>(W >> 16);
      const u32 x = code == 0xFF ? 3u : code, y = code == 0xFF ? 1u : code;
      const u32 R = static_cast<u8>(x + y) | (static_cast<u32>(static_cast<u8>(x + x)) << 8) | (static_cast<u32>(static_cast<u8>(x + x)) << 16) | (static_cast<u32>(static_cast<u8>(x)) << 24);   // page k's refill entry when page k+1 has the same slot
      // Run: pages with this PU byte and this bus entry.
      const u64 pu8 = 0x0101010101010101ull * pu;
      u32 j = i + 1;
      while (j < seg_end) {
        if ((j & 7) == 0 && j + 8 <= seg_end) { u64 v; std::memcpy(&v, pu_map_p + j, 8); if (v == pu8 && bus_word(j >> 2) == bw && bus_word((j >> 2) + 1) == bw) { j += 8; continue; } }
        if (pu_map_p[j] != pu || ((j & 3) == 0 && bus_word(j >> 2) != bw)) break;
        ++j;
      }
      for (; i < j; ++i) {
        u8* slot = &cpu9_[i * 8];
        u64 old_w; std::memcpy(&old_w, slot, 8);
        if (old_w == W && !changed_prev) {
          x_prev = x; y_prev = y;
          while ((i & 15) == 15 && i + 17 <= j) {
            u64 acc = 0;
            for (u32 k = 1; k <= 16; ++k) { u64 o; std::memcpy(&o, &cpu9_[(i + k) * 8], 8); acc |= o ^ W; }
            if (acc) break;
            i += 16;
          }
          continue;
        }
        if (old_w != W) std::memcpy(slot, &W, 8);
        const u8 old_code = static_cast<u8>(old_w), old_data = static_cast<u8>(old_w >> 16);
        // Refill entries written only where a branch cost moved: page i-1's
        // entry [2] needs this page's cost, so each page finishes one
        // iteration late.
        const bool changed = code != old_code;
        if (i > first && (changed || changed_prev)) {
          if (x_prev == x && y_prev == y) std::memcpy(refill + (i - 1) * 4, &R, 4);
          else { u8* r = refill + (i - 1) * 4; r[0] = static_cast<u8>(x_prev + y_prev); r[1] = static_cast<u8>(x_prev + x_prev); r[2] = static_cast<u8>(x_prev + x); r[3] = static_cast<u8>(x_prev); }
        }
        x_prev = x; y_prev = y; changed_prev = changed;
        // Only bytes a translation could have baked in count as a retime.
        const u8 f = static_cast<u8>((changed ? RETIME_CODE : 0) | (data != old_data ? RETIME_DATA : 0));
        if (f && (retime_flags_[i] & f) != f) {
          if (!retime_flags_[i]) { if (retime_list_.size() < RETIME_LIST_MAX) retime_list_.push_back(i); else retime_overflow_ = true; }
          retime_flags_[i] |= f;
        }
        // The pages after this one in the run that held the same old slot get the same answers to every
        // question above (the page before each now has this slot too, so its refill entry is R). A change
        // of the ARM9's clock moves all million pages, and through the general path that was ~27 ms.
        if (old_w != W) {
          u32 n = i + 1;
          for (; n < j; ++n) {
            u64 o; std::memcpy(&o, &cpu9_[n * 8], 8);
            if (o != old_w) break;
            std::memcpy(&cpu9_[n * 8], &W, 8);
            if (changed) std::memcpy(refill + (n - 1) * 4, &R, 4);
            if (f && (retime_flags_[n] & f) != f) {
              if (!retime_flags_[n]) { if (retime_list_.size() < RETIME_LIST_MAX) retime_list_.push_back(n); else retime_overflow_ = true; }
              retime_flags_[n] |= f;
            }
          }
          i = n - 1;
        }
      }
    }
  }
  // Pages at both edges of the range: the last one, and the one before
  // `first` (its entry [2] reads page `first`, which just changed).
  if (last > first && changed_prev) build_refill9(last - 1, last);
  if (first) build_refill9(first - 1, first);
}

void Timing::notify_cpu9(const CpuContext& cpu) {
  if (cpu.jit_timing_changed) cpu.jit_timing_changed(const_cast<CpuContext&>(cpu));
}

} // namespace ds::mem
