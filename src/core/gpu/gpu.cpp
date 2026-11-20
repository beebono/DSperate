// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include <chrono>
#include "core/state/state.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/gpu/split3d.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

// pow() for the gamma LUT below; pins a glibc symbol version for a low aarch64 runtime floor.
extern "C" double ds_pow_compat(double x, double y);

namespace ds::gpu {

// Knobs read once in the constructor.
namespace {
bool g_dbg_join = false, g_dbg_gpu = false, g_dbg_vramnz = false, g_dbg_skip = false;
bool g_no_lazy = false, g_no_lag = false;
int g_worker_delay_us = 0;
const char* g_dump_frame = nullptr;
void read_knobs() {
  static bool done = false;
  if (done) return;
  done = true;
  g_dbg_join = std::getenv("DS_DEBUG_JOIN") != nullptr;
  g_dbg_gpu = std::getenv("DS_DEBUG_GPU") != nullptr;
  g_dbg_vramnz = std::getenv("DS_DEBUG_VRAMNZ") != nullptr;
  g_dbg_skip = std::getenv("DS_DEBUG_SKIP") != nullptr;
  g_dump_frame = std::getenv("DS_DEBUG_DUMP_FRAME");
  g_no_lazy = std::getenv("DS_DEBUG_NOLAZY") != nullptr;   // every frame per-line (the lazy batch's reference)
  g_no_lag = std::getenv("DS_DEBUG_NOLAG") != nullptr;     // per-line lines never stay in flight past their HBlank
  // Each 2D worker job starts this many us late: widens any window in which the
  // emulation thread changes what an in-flight line reads (a race shows up every run).
  if (const char* d = std::getenv("DS_DEBUG_WDELAY")) g_worker_delay_us = std::atoi(d);
}
}  // namespace

static void ev_scanline(NDS& nds, u32) { nds.gpu.on_scanline_start(); }
static void ev_hblank(NDS& nds, u32)   { nds.gpu.on_hblank(); }
static void ev_fifo(NDS& nds, u32 x)   { nds.gpu.on_display_fifo(x); }

Gpu::Gpu(NDS& nds) : engine{Engine2D(nds, 0), Engine2D(nds, 1)}, nds_(nds) {
  read_knobs();
  split_ = thread_layout_on() || std::getenv("DS_2D_BWORKER");
  worker_.start(&Gpu::worker_job, this, split_ ? ThreadRole::EngineA : ThreadRole::Line, split_ ? "2d-engine-a" : "line-worker");
  if (split_) worker_b_.start(&Gpu::worker_b_job, this, ThreadRole::Line, "2d-engine-b");
  // DS_SPLIT3D: compose engine A without the 3D layer and merge it after
  // (core/gpu/split3d.h). 1: the CPU merge is the output; verify: the
  // one-pass line is, and every difference is reported; gpu: the split
  // alone, no merge (the picture lacks the 3D layer: for timing the split);
  // present: set_split3d_present(true) (GPU raster frames; headless resolves each frame on the CPU).
  stat_on_ = std::getenv("DS_SPLIT3D") || std::getenv("DS_SPLIT_STAT");
  cap_follow_ = true;
  cap_late_ok_ = !(std::getenv("DS_CAP_SYNC") && std::atoi(std::getenv("DS_CAP_SYNC")) != 0);
  split_cap_ = !(std::getenv("DS_SPLIT3D_NOCAP") && std::atoi(std::getenv("DS_SPLIT3D_NOCAP")) != 0);   // debug: captured images shown again stay as they are
  if (const char* e = std::getenv("DS_SPLIT3D")) {
    split3d_verify_ = !std::strcmp(e, "verify");
    split_req_ = !std::strcmp(e, "present");   // as the frontend asks for it (set_split3d_present); the caller resolves
    if (!split_req_) engine[0].set_split3d(split3d_verify_ ? Split3D::Verify : !std::strcmp(e, "gpu") ? Split3D::Gpu : *e != '0' ? Split3D::Cpu : Split3D::Off);
  }
}

Gpu::~Gpu() {
  late_join();
  const bool stat = std::getenv("DS_SPLIT3D") || std::getenv("DS_SPLIT_STAT");
  if (stat && (split_miss_cpu_ || split_miss_mixed_))
    std::fprintf(stderr, "split3d: 3D lines left to the 1x composite: %llu of a frame drawn on the CPU, %llu on a screen already showing a captured frame\n",
                 static_cast<unsigned long long>(split_miss_cpu_), static_cast<unsigned long long>(split_miss_mixed_));
  if (stat && (cap_late_frames_ || cap_untagged_.load())) std::fprintf(stderr, "split3d: %u capture frames did not wait for their 3D frame (late capture), %u written again with it; %u captured lines with 3D had no tag\n", cap_late_frames_, cap_late_filled_, cap_untagged_.load());
  if (stat && cap_frames_) std::fprintf(stderr, "split3d: %u capture frames, %u of them waiting for their 3D frame after a late capture was taken early (by a remap to textures or the ARM7 %u, a read %u, a DMA out %u)\n", cap_frames_, cap_backoff_frames_, cap_taken_remap_, cap_taken_read_, cap_taken_dma_);
  if (stat && split_scrolled_) std::fprintf(stderr, "split3d: %llu split lines had BG0 scrolled\n", static_cast<unsigned long long>(split_scrolled_));
  if (split_lines_ && stat) std::fprintf(stderr, "split3d: %llu lines split for the present, %llu frames merged on the CPU\n", static_cast<unsigned long long>(split_lines_), static_cast<unsigned long long>(split_resolved_));
  if (stat && split_cap_effect_) std::fprintf(stderr, "split3d: %llu captured 3D pixels were shown under a colour effect (left at 1x)\n", static_cast<unsigned long long>(split_cap_effect_));
  if (stat && (split_cap_merged_ || split_cap_shown_ || split_cap_changed_))
    std::fprintf(stderr, "split3d: %llu captured lines merged for the game; captured pixels shown again: %llu split (%llu away from where they were captured), %llu written to since, %llu of a second frame on a screen; %llu of %llu checked differed\n",
                 static_cast<unsigned long long>(split_cap_merged_), static_cast<unsigned long long>(split_cap_shown_), static_cast<unsigned long long>(split_cap_moved_), static_cast<unsigned long long>(split_cap_changed_),
                 static_cast<unsigned long long>(split_cap_other_), static_cast<unsigned long long>(split_cap_bad_), static_cast<unsigned long long>(split_cap_checked_));
  if (stat && (cap_copied_out_ || cap_copied_in_))
    std::fprintf(stderr, "split3d: captured pixels followed through main RAM: %llu out of a bank, %llu back into one\n", static_cast<unsigned long long>(cap_copied_out_ * 128), static_cast<unsigned long long>(cap_copied_in_ * 128));
  if (split3d_verify_) std::fprintf(stderr, "split3d: %llu pixels differed in %llu lines\n", static_cast<unsigned long long>(engine[0].split_mismatches()),
                                    static_cast<unsigned long long>(engine[0].split_verified()));
}

void Gpu::reset() {
  join_worker(); late_join();
  disarm_trap();
  line_ = 0; next_vcount_ = 0xFFFF; hblank_done_ = false;
  lazy_frame_ = false; per_line_[0] = per_line_[1] = false;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = false;
  frame_begun_ = false; screens_on_ = false;
  master_bright_g_[0] = master_bright_g_[1] = 0;
  capcnt_ = 0; capture_on_ = false;
  fifo_.fill(0); fifo_rd_ = fifo_wr_ = 0; fifo_line_.fill(0); run_fifo_ = false;
  for (Page& pg : pages_) for (auto& fb : pg.fb) fb.fill(0);
  page_ = front_ = 0;
  engine[0].reset(); engine[1].reset();
  set_powcnt(nds_.io.powcnt1);
  nds_.io.set_vcount(0);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.now() + HBLANK_START, ev_hblank);
}

// ---- registers --------------------------------------------------------------

u32 Gpu::reg_read(u32 addr, u32 width) {
  const u32 r = addr - 0x04000000;
  if (r >= 0x64 && r < 0x70) {
    auto rd16 = [&](u32 a) -> u32 {
      switch (a) {
      case 0x64: return capcnt_ & 0xFFFF;
      case 0x66: return capcnt_ >> 16;
      case 0x6C: return master_bright_g_[0];
      default: return 0;
      }
    };
    if (width == 32) return rd16(r) | (rd16(r + 2) << 16);
    if (width == 16) return rd16(r);
    return (rd16(r & ~1u) >> ((r & 1) * 8)) & 0xFF;
  }
  if (r >= 0x1064 && r < 0x1070) {
    const u32 v = (r & ~1u) == 0x106C ? master_bright_g_[1] : 0;
    return width == 8 ? (v >> ((r & 1) * 8)) & 0xFF : v;
  }
  return engine[r >= 0x1000].read(addr, width);
}

void Gpu::reg_write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  // MASTER_BRIGHT: guest copy here, render side via the journal.
  auto mb = [&](int e, u16 v) { master_bright_g_[e] = v; engine[e].master_bright_write(v); };
  if (r >= 0x64 && r < 0x70) {
    if (width == 32) {
      switch (r) {
      case 0x64: capcnt_ = value & 0xEF3F1F1F; return;
      case 0x68: fifo_[fifo_wr_] = value & 0xFFFF; fifo_[fifo_wr_ + 1] = value >> 16; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    if (width == 16) {
      switch (r) {
      case 0x64: capcnt_ = (capcnt_ & 0xFFFF0000) | (value & 0x1F1F); return;
      case 0x66: capcnt_ = (capcnt_ & 0x0000FFFF) | ((value & 0xEF3F) << 16); return;
      case 0x68: fifo_[fifo_wr_] = value; return;
      case 0x6A: fifo_[fifo_wr_ + 1] = value; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;   // the write pointer advances on the high half
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    switch (r) {
    case 0x64: capcnt_ = (capcnt_ & 0xFFFFFF00) | (value & 0x1F); return;
    case 0x65: capcnt_ = (capcnt_ & 0xFFFF00FF) | ((value & 0x1F) << 8); return;
    case 0x66: capcnt_ = (capcnt_ & 0xFF00FFFF) | ((value & 0x3F) << 16); return;
    case 0x67: capcnt_ = (capcnt_ & 0x00FFFFFF) | ((value & 0xEF) << 24); return;
    case 0x68: fifo_[fifo_wr_] = static_cast<u16>(value * 0x0101); return;
    case 0x6A: fifo_[fifo_wr_ + 1] = static_cast<u16>(value * 0x0101); return;
    case 0x6B: fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
    case 0x6C: mb(0, (master_bright_g_[0] & 0xFF00) | (value & 0x1F)); return;
    case 0x6D: mb(0, (master_bright_g_[0] & 0x00FF) | ((value & 0xC0) << 8)); return;
    default: return;
    }
  }
  if (r >= 0x1064 && r < 0x1070) {
    if (r == 0x106C && width >= 16) mb(1, value & 0xC01F);
    else if (r == 0x106C) mb(1, (master_bright_g_[1] & 0xFF00) | (value & 0x1F));
    else if (r == 0x106D) mb(1, (master_bright_g_[1] & 0x00FF) | ((value & 0xC0) << 8));
    return;
  }
  const int e = r >= 0x1000;
  engine[e].write(addr, width, value);
  // Engine A switching to VRAM display mid-frame starts reading an LCDC bank
  // the trap does not cover: render the rest of the frame per line.
  if (e == 0 && (r & 0xFFF) < 4 && trap_armed_ && !trap_lcdc_ && ((engine[0].read(0x04000000, 32) >> 16) & 3) == 2) fall_back_per_line(3);
}

void Gpu::set_powcnt(u16 value) {
  engine[0].powcnt_write(value);
  engine[1].powcnt_write(value);
  nds_.gpu3d.set_powcnt(value);
}

// ---- slow-path stores and the VRAM trap ---------------------------------------

// Guest bytes change now, engine copy via the journal. A store of the value already there is not journaled.
void Gpu::palette_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.palette.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].palette_written(off & 0x3FF, width, value);
}
void Gpu::oam_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.oam.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].oam_written(off & 0x3FF, width, value);
}

// A store into VRAM the engines can see, before it lands. Only ARM9 reaches
// the engine windows; the LCDC window matters only while it is displayed.
void Gpu::vram_store_trap(Cpu cpu, u32 addr) {
  if (!trap_armed_ || cpu != Cpu::ARM9) return;
  if (addr >= 0x06800000 && !trap_lcdc_) return;
  // A's deferred batch: a store reaching what it reads joins it, finishing
  // the frame and lifting the trap. B's windows only when B is deferred too.
  const u32 stale = snapshot_stale_ ? 3u : 0u;
  if (a_deferred_) {
    if ((reach_engines(addr) | stale) & (b_deferred_ ? 3u : 1u)) {
      const bool busy = !worker_.done() || (b_deferred_ && !worker_b_.done());
      if (busy) snap_hit_ = true;
      // On a snapshot the batch cannot see the store: the trap has nothing left to guard.
      if (snap_live_ && busy) { disarm_trap(); return; }
      prof::add(prof::C_2D_A_JOIN_STORES, 1); join_worker(JoinSite::Trap);
    }
    return;
  }
  // Charged to the engine its address reaches (both for anything unattributed).
  const u32 mask = reach_engines(addr) | stale;
  // Already per line everywhere it reaches: nothing to do but the lag-mode join below.
  if ((per_line_[0] || !(mask & 1)) && (per_line_[1] || !(mask & 2))) {
    // Lag mode: the store may land on a line engine B is still drawing.
    prof::add(prof::C_2D_LAG_STORES, 1);
    const u32 reach = reach_engines(addr) | stale;
    const bool b_joined = ((reach & 1) && inflight_[0]) || ((reach & 2) && inflight_[1]);
    if (b_joined) { prof::add(prof::C_2D_LAG_STORE_JOINS, 1); join_worker(JoinSite::Trap); }
    // Past either limit, drop lag and lift the trap unless the other engine is still
    // batching. Nothing may stay in flight once the trap is gone.
    const bool over = (b_joined && ++lag_trap_hits_ >= LAG_TRAP_LIMIT) || ++lag_trap_stores_ >= LAG_STORE_LIMIT;
    if (over && lag_frame_) {
      if (!b_joined) join_worker(JoinSite::Trap);
      lag_frame_ = false;
      arm_trap();   // the trap now guards only a batching engine's windows, if any
      prof::add(prof::C_2D_LAG_DROPPED, 1);
    }
    return;
  }
  prof::add(prof::C_2D_TRAP_HITS, 1);
  // One engine per-line with a lag line in flight, the other batching: the
  // catch-up below only joins if the batching engine has lines due, so a store
  // reaching the in-flight line waits for it here.
  if (inflight_[0] || inflight_[1]) {
    const u32 reach = reach_engines(addr) | stale;
    if (((reach & 1) && inflight_[0]) || ((reach & 2) && inflight_[1])) join_worker(JoinSite::Trap);
  }
  // The budget is per engine: one engine streaming tiles must not spend the other's.
  u32 burst_mask = 0;
  for (int e = 0; e < 2; ++e)
    if ((mask & (1u << e)) && !per_line_[e] && ++lazy_bursts_[e] < LAZY_BURST_LIMIT) burst_mask |= 1u << e;
  if (burst_mask) {
    catch_up(burst_mask);
    for (int e = 0; e < 2; ++e)
      if (burst_mask & (1u << e)) { per_line_[e] = true; burst_[e] = true; burst_left_[e] = LAZY_BURST_LINES; }
    arm_trap();   // a bursting engine's windows need no trap until its burst ends
    return;
  }
  fall_back_per_line(mask);
}

bool Gpu::vram_remap_begin(u32 moved_2d, const VramMap* next) {
  // A late capture's bank on its way to the texture units or the ARM7 (Etrian Odyssey draws its captured
  // screen as a texture): nothing there waits for late_fill, so the lines are written with their own 3D now.
  if (next && (late_[0].pending || late_[1].pending)) {
    u32 to = 0;
    for (u32 b = 0; b < next->texture.blocks(); ++b) to |= next->texture.mask[b];
    for (u32 b = 0; b < next->arm7.blocks(); ++b) to |= next->arm7.mask[b];
    if ((late_[0].pending && (to >> late_[0].bank & 1)) || (late_[1].pending && (to >> late_[1].bank & 1))) {
      join_worker(JoinSite::Remap);
      if (late_fill(render_next_[0] >= SCREEN_H)) { cap_stand_down(); ++cap_taken_remap_; }
    }
  }
  const bool trapped = vram_remap_begin_core(moved_2d, next);
  // The read trap held for a late capture, with nothing in flight: its saved entries would go stale under the
  // rebuild, as the in-flight one's would. Lifted now, back in its generic form after (vram_remap_end).
  if (read_trap_bank_ >= 0 && !read_trap_reapply_ && late_hold(read_trap_bank_) && !lines_in_flight()) {
    if (read_trap_generic_) nds_.bus.set_bank_read_trap(read_trap_bank_, false); else nds_.bus.set_lcdc_read_trap(read_trap_bank_, false);
    read_trap_generic_ = false; read_trap_reapply_ = true;
  }
  return trapped;
}

// `moved_2d`: engines whose read views this remap actually moves. CENSUS
// ONLY for now -- the catch-up below is still unconditional.
bool Gpu::vram_remap_begin_core(u32 moved_2d, const VramMap* next) {
  // LCDC (bit 2) is an engine A view only while A displays a VRAM bank or
  // a capture writes one; otherwise a bank entering or leaving it changes
  // nothing the 2D render reads, and the remap needs no catch-up or join
  // (the trap, if armed, is re-applied by the bus over the remapped pages).
  // Dragon Quest IX streams textures by flipping banks through LCDC hundreds
  // of times a frame.
  if (moved_2d & 4) {
    const bool lcdc_read = ((engine[0].dispcnt() >> 16) & 3) == 2 || capture_on_ || capture_render_;
    moved_2d = (moved_2d & 3) | (lcdc_read ? 1u : 0u);
  }
  if (!moved_2d) { prof::add(prof::C_VRAM_REMAP_2D_STILL, 1); return trap_armed_; }
  // A job in flight keeps rendering against its snapshot; the live map may
  // change under it as long as the write trap still covers every bank it
  // reads at the bank's new address: an engine window that is trapped, or
  // LCDC while LCDC is trapped. A bank leaving for the ARM7 side or an
  // untrapped window joins as before (the ARM7 is never trapped).
  static const bool snap_off = std::getenv("DS_SNAP") && std::getenv("DS_SNAP")[0] == '0';   // debugging: always join
  static const bool snap_capjoin = std::getenv("DS_SNAP_CAPJOIN") != nullptr;             // debugging: join under a capture
  if (!snap_off && next && lines_in_flight() && trap_armed_) {
    const VramMap& cur = nds_.bus.vram_map();
    auto banks_of = [](const VramView& v) { u32 m = 0; for (u32 b = 0; b < v.blocks(); ++b) m |= v.mask[b]; return m; };
    u32 old_banks = 0;
    if (inflight_[0]) old_banks |= banks_of(cur.abg) | banks_of(cur.aobj) | banks_of(cur.abg_extpal) | banks_of(cur.aobj_extpal) | cur.lcdc_mask;
    if (inflight_[1]) old_banks |= banks_of(cur.bbg) | banks_of(cur.bobj) | banks_of(cur.bbg_extpal) | banks_of(cur.bobj_extpal);
    const u32 na = banks_of(next->abg) | banks_of(next->aobj) | banks_of(next->abg_extpal) | banks_of(next->aobj_extpal);
    const u32 nb = banks_of(next->bbg) | banks_of(next->bobj) | banks_of(next->bbg_extpal) | banks_of(next->bobj_extpal);
    const u32 n7 = banks_of(next->arm7), ntex = banks_of(next->texture) | banks_of(next->texpal);
    bool covered = true;
    // A capture in flight is still writing its bank. The ARM9 is kept off it
    // by the read trap (re-applied over the new mapping after the remap); an
    // engine in flight reads its own snapshot, where the bank is still LCDC.
    // The ARM7, the texture units and an engine rendering inline against the
    // live map cannot be kept off it, so those cases join.
    if (capture_render_) {
      const u32 cap = 1u << ((capcnt_render_ >> 16) & 3);
      if (((n7 | ntex) & cap) || ((na & cap) && !inflight_[0]) || ((nb & cap) && !inflight_[1]) || snap_capjoin) covered = false;
    }
    for (u32 b = 0; b < 9 && covered; ++b) {
      const u32 bit = 1u << b;
      if (!(old_banks & bit)) continue;
      if (n7 & bit) covered = false;
      if ((na & bit) && !(trap_mask_ & 1)) covered = false;
      if ((nb & bit) && !(trap_mask_ & 2)) covered = false;
      if ((next->lcdc_mask & bit) && !trap_lcdc_) covered = false;
    }
    static const bool snap_log = std::getenv("DS_SNAP_LOG") != nullptr;
    if (snap_log) std::fprintf(stderr, "[snap] frame %llu line %u remap covered=%d cap=%d capbank=%u inflight=%d%d moved=%u lcdcnext=%02x\n", (unsigned long long)nds_.frame_count, line_, covered ? 1 : 0, capture_render_ ? 1 : 0, (capcnt_render_ >> 16) & 3, inflight_[0] ? 1 : 0, inflight_[1] ? 1 : 0, moved_2d, next->lcdc_mask);
    if (covered) {
      prof::add(prof::C_VRAM_REMAP_SNAPSHOT, 1);
      // An engine still in flight owes its cache reset at the join (its job
      // reads the snapshot); one that is not renders inline against the live
      // map and takes it now.
      for (int e = 0; e < 2; ++e) { if (inflight_[e]) remap_pending_[e] = true; else engine[e].vram_remapped(); }
      snapshot_stale_ = true;
      // The read trap's saved entries would go stale under the rebuild: lift it now, back after.
      if (read_trap_bank_ >= 0) {
        if (read_trap_generic_) nds_.bus.set_bank_read_trap(read_trap_bank_, false); else nds_.bus.set_lcdc_read_trap(read_trap_bank_, false);
        read_trap_generic_ = false; read_trap_reapply_ = true;
      }
      catch_up(3);   // lines still due below the frontier render now, against the map they were scanned with
      return false;  // the trap stays as it is; vram_remap_end re-applies it over the remapped pages
    }
  }
  if (prof::enabled) {
    prof::add(prof::C_VRAM_REMAP, 1);
    prof::add(moved_2d ? prof::C_VRAM_REMAP_2D_MOVED : prof::C_VRAM_REMAP_2D_STILL, 1);
    if (lines_in_flight()) {
      prof::add(prof::C_VRAM_REMAP_INFLIGHT, 1);
      // capture_on_ is cleared at VBlank before remaps land, so it says nothing here;
      // what matters is whether the in-flight job is rendering a capture (capture_render_).
      const bool alt = phase_period_ > 1, cap = capture_render_;
      if (alt) prof::add(prof::C_VRAM_REMAP_INFLIGHT_ALT, 1);
      if (cap) prof::add(prof::C_VRAM_REMAP_INFLIGHT_CAP, 1);
      if (!alt && !cap) prof::add(prof::C_VRAM_REMAP_INFLIGHT_CLEAN, 1);
    }
    const u32 f = frontier();
    if (f && ((render_next_[0] < f && render_next_[0] < SCREEN_H) ||
              (render_next_[1] < f && render_next_[1] < SCREEN_H)))
      prof::add(prof::C_VRAM_REMAP_PENDING, 1);
  }
  catch_up(3);
  join_worker(JoinSite::Remap);
  engine[0].vram_remapped(); engine[1].vram_remapped();
  // The trap stays armed across the remap: the bus re-applies it over the
  // pages it rewrites (the rest keep theirs), and vram_remap_end's arm_trap
  // only redoes the windows if the arming itself changed.
  return trap_armed_;
}
void Gpu::vram_remap_end(bool trapped) {
  if (trapped) arm_trap();
  // Kept armed across the remap (a job in flight on its snapshot); the bus
  // re-applied the trap over the pages it rewrote. The capture read trap
  // comes back in its generic form, over every page the bank backs now --
  // the guest reads it through the new mapping otherwise.
  if (read_trap_reapply_) {
    read_trap_reapply_ = false;
    if (read_trap_bank_ >= 0) { nds_.bus.set_bank_read_trap(read_trap_bank_, true); read_trap_generic_ = true; }
  }
}

void Gpu::capture_read_hit() {
  prof::add(prof::C_2D_A_JOIN_READS, 1);
  static const bool snap_log = std::getenv("DS_SNAP_LOG") != nullptr;
  if (snap_log) std::fprintf(stderr, "[snap] frame %llu line %u capture read hit\n", (unsigned long long)nds_.frame_count, line_);
  join_worker(JoinSite::Trap);
  // The lines of a late capture, read before the next frame's render has written them again: now.
  if ((late_[0].pending || late_[1].pending) && late_fill(render_next_[0] >= SCREEN_H)) { cap_stand_down(); ++cap_taken_read_; }
  release_read_trap(true);
}

void Gpu::release_read_trap(bool force) {
  if (read_trap_bank_ < 0) return;
  if (!force && late_hold(read_trap_bank_)) return;
  if (read_trap_generic_) nds_.bus.set_bank_read_trap(read_trap_bank_, false);
  else nds_.bus.set_lcdc_read_trap(read_trap_bank_, false);
  read_trap_bank_ = -1; read_trap_generic_ = false; read_trap_reapply_ = false;
}

void Gpu::take_snapshot(u32 engines) {
  const VramMap& live = nds_.bus.vram_map();
  if (!snap_mem_) {
    u32 total = 0; for (u32 sz : mem::Bus::VRAM_BANK_SIZES) total += sz;
    snap_mem_.reset(new u8[total]);
  }
  auto banks_of = [](const VramView& v) { u32 m = 0; const u32 n = v.blocks() ? v.blocks() : 1; for (u32 b = 0; b < n; ++b) m |= v.mask[b]; return m; };
  u32 m = 0;
  if (engines & 1) m |= banks_of(live.abg) | banks_of(live.aobj) | banks_of(live.abg_extpal) | banks_of(live.aobj_extpal);
  if (engines & 2) m |= banks_of(live.bbg) | banks_of(live.bobj) | banks_of(live.bbg_extpal) | banks_of(live.bobj_extpal);
  u8* p = snap_mem_.get();
  for (int i = 0; i < 9; ++i) {
    const u32 sz = mem::Bus::VRAM_BANK_SIZES[i];
    if (m & (1u << i)) { std::memcpy(p, live.bank(i), sz); snap_bank_[i] = p; } else snap_bank_[i] = live.bank(i);
    p += sz;
  }
}

void Gpu::snapshot_map(int k) {
  const VramMap& live = nds_.bus.vram_map();
  if (snap_live_) { job_map_[k] = live; job_map_[k].rebase(snap_bank_); job_map_gen_[k] = ~0u; return; }
  if (job_map_gen_[k] == live.generation()) return;
  job_map_[k] = live;
  job_map_gen_[k] = live.generation();
}

void Gpu::arm_trap() {
  // Windows that need it: a batching engine's (its stores must render the lines
  // before them first), and engine A's in a lag frame (its per-line line is in
  // flight). A per-line engine renders each line at its own HBlank.
  u32 m = lag_frame_ ? 1u : 0u;
  if (lazy_frame_) { if (!per_line_[0]) m |= 1; if (!per_line_[1]) m |= 2; }
  if (!m) { disarm_trap(); return; }
  // LCDC banks trapped when engine A displays one, or a capture writes one (A's reach).
  const bool lcdc = (m & 1) && (((engine[0].dispcnt() >> 16) & 3) == 2 || capture_on_);
  if (trap_armed_) {
    if (m == trap_mask_ && lcdc == trap_lcdc_) return;
    // Clear the old ranges first, or pages the new arming leaves out stay trapped.
    nds_.bus.set_vram_trap(false, trap_lcdc_, trap_mask_);
    prof::add(prof::C_2D_TRAP_NARROWED, 1);
  }
  trap_lcdc_ = lcdc; trap_mask_ = m;
  nds_.bus.set_vram_trap(true, trap_lcdc_, trap_mask_);
  trap_armed_ = true;
}
void Gpu::disarm_trap() {
  if (!trap_armed_) return;
  nds_.bus.set_vram_trap(false, trap_lcdc_, trap_mask_);
  trap_armed_ = false;
}


void Gpu::catch_up(u32 mask) {
  const u32 f = frontier();
  if (f == 0) return;
  const u32 last = (f < SCREEN_H ? f : SCREEN_H) - 1;
  const bool a = (mask & 1) && render_next_[0] <= last && render_next_[0] < SCREEN_H;
  const bool b = (mask & 2) && render_next_[1] <= last && render_next_[1] < SCREEN_H;
  if (!a && !b) return;
  render_ranges(a ? render_next_[0] : 1, a ? last : 0,
                b ? render_next_[1] : 1, b ? last : 0);
  join_worker(JoinSite::CatchUp);
}
void Gpu::fall_back_per_line(u32 mask) {
  catch_up(mask);
  for (int e = 0; e < 2; ++e) if (mask & (1u << e)) { per_line_[e] = true; burst_[e] = false; }
  arm_trap();   // guards what still batches (and a lag frame's in-flight line); lifted when nothing does
}

// ---- timing -----------------------------------------------------------------

void Gpu::on_hblank() {
  prof::Scope hook(prof::GPU_LINE);
  nds_.io.set_hblank(true);
  const bool frame_reset = line_ == 262;
  if (line_ < SCREEN_H) {
    // Rendered now per-line, or all together at the last one. Both engines
    // are always in the same mode.
    u32 f[2], l[2];
    for (int e = 0; e < 2; ++e) {
      const bool batch = lazy_frame_ && !per_line_[e];
      if (batch && line_ != SCREEN_H - 1) {
        f[e] = 1; l[e] = 0;   // nothing yet
        // But a piece of engine A's batch (early_a_), when the piece before is done: never a wait here.
        if (e == 0 && early_ok_ && lag_frame_ && line_ + 1u >= render_next_[0] + EARLY_A_LINES && trap_armed_ && (trap_mask_ & 1) && !a_deferred_ && worker_.done()) { f[0] = render_next_[0]; l[0] = line_; }
        continue;
      }
      f[e] = batch ? render_next_[e] : line_;
      l[e] = batch ? SCREEN_H - 1 : line_;
      if (!batch && render_next_[e] < line_) f[e] = render_next_[e];          // catch up anything skipped
    }
    // Draws account for themselves; subtracted from this hook's time. The
    // worker join in render_ranges has no scope of its own, so it's added back rather than subtracted.
    const u64 t_draw0 = prof::enabled ? prof::now_ns() : 0;
    const u64 join0 = prof::enabled ? join_wait_ns_ : 0;
    render_ranges(f[0], l[0], f[1], l[1]);
    if (prof::enabled)
      prof::add_timed(prof::GPU_LINE, (t_draw0 - prof::now_ns()) + (join_wait_ns_ - join0));   // wraps: a subtraction from the hook's own scope
    // End of a burst window: the lines after this one batch again, trapped.
    for (int e = 0; e < 2; ++e)
      if (burst_[e] && --burst_left_[e] == 0 && line_ < SCREEN_H - 1) {
        burst_[e] = false; per_line_[e] = false; render_next_[e] = line_ + 1; arm_trap();
      }
    hblank_done_ = true;
    nds_.dma.check(Cpu::ARM9, dma::MODE9_HBLANK);
  } else {
    hblank_done_ = true;
    engine[0].latch(Engine2D::L_PREDRAW, line_, frame_reset);
    engine[1].latch(Engine2D::L_PREDRAW, line_, frame_reset);
    if (line_ == 215) {
      // 3D flushed at VBlank rasterises now, ahead of the next frame's display
      // lines, so frameskip is decided here for begin_frame to latch.
      skip_next_ = skip_req_ && skippable();
      const u64 t_r0 = prof::enabled ? prof::now_ns() : 0;
      if (!skip_next_) nds_.gpu3d.render_frame(); else nds_.gpu3d.note_raster_skipped();
      if (prof::enabled) prof::add_timed(prof::GPU_LINE, t_r0 - prof::now_ns());   // wraps, as above
      if (probe_enabled_) async_probe_start();
    } else if (line_ == 262) {
      engine[0].latch(Engine2D::L_SPRITES, 0, false); engine[1].latch(Engine2D::L_SPRITES, 0, false);
      // An engine's skip flag resets at the join while its lines are in flight (finish_a, join_worker).
      if (!inflight_[0]) skipped_[0] = false;
      if (!inflight_[1]) skipped_[1] = false;
    }
    engine[0].latch(Engine2D::L_POSTDRAW, line_, frame_reset);
    engine[1].latch(Engine2D::L_POSTDRAW, line_, frame_reset);
  }
  nds_.sched.schedule(EventId::VBlank_Scanline, nds_.sched.event_time() + (CYCLES_PER_SCANLINE - HBLANK_START), ev_scanline);
}

// ---- async-raster probe (DS_ASYNC_PROBE=1, measurement only) ----------------
// Hashes texture/texture-palette VRAM at line 215 and at two join deadlines
// (line 0 next frame, line 192 swap): a change means an async raster worker
// would have read bytes the CPU was writing.
namespace {
u64 hash_view(const VramView& v) {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 b = 0; b < v.blocks(); ++b) {
    const u8* p = v.ptr[b];
    if (!p) { h = (h ^ 0x9e37) * 0x100000001b3ull; continue; }
    for (u32 o = 0; o < VramView::BLOCK; o += 8) {
      u64 w; std::memcpy(&w, p + o, 8);
      h = (h ^ w) * 0x100000001b3ull;
    }
  }
  return h;
}
u64 hash_tex_vram(const VramMap& vm) { return hash_view(vm.texture) * 31 + hash_view(vm.texpal); }
} // namespace

void Gpu::async_probe_start() {
  const VramMap& vm = nds_.bus.vram_map();
  probe_hash_ = hash_tex_vram(vm);
  probe_open_ = true;
  probe_vramcnt_at_l0_ = 0;
  prof::async_window = true;
  prof::add(prof::C_ASYNC_FRAMES, 1);
}

void Gpu::async_probe_check(bool at_line0) {
  if (!probe_open_) return;
  const bool dirty = hash_tex_vram(nds_.bus.vram_map()) != probe_hash_;
  if (at_line0) {
    if (dirty) prof::add(prof::C_ASYNC_DIRTY_L0, 1);
    probe_vramcnt_at_l0_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
    if (probe_vramcnt_at_l0_ != probe_vramcnt_base_) prof::add(prof::C_ASYNC_VRAMCNT_L0, 1);
    return;
  }
  if (dirty) prof::add(prof::C_ASYNC_DIRTY_SWAP, 1);
  probe_open_ = false;
  prof::async_window = false;
  probe_vramcnt_base_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
}

void Gpu::on_scanline_start() {
  prof::Scope hook(prof::GPU_LINE);
  nds_.io.set_hblank(false);
  if (next_vcount_ != 0xFFFF) { line_ = next_vcount_; next_vcount_ = 0xFFFF; }
  else line_ = static_cast<u16>((line_ + 1) % SCANLINES_PER_FRAME);
  hblank_done_ = false;

  // Display lines evaluate window edges in step_engine; the rest go through the latch.
  if (line_ >= SCREEN_H) { engine[0].latch(Engine2D::L_WINDOWS, line_, false); engine[1].latch(Engine2D::L_WINDOWS, line_, false); }
  if (line_ == 0) {
    // All display lines must be drawn before the frontend reads them and before begin_frame runs.
    { prof::Scope j(prof::JOIN0); join_worker(JoinSite::Line0); late_join(); }
    if (probe_enabled_) async_probe_check(true);
    { prof::Scope b(prof::BEGIN_FRAME); begin_frame(); }
    late_prefill();
    nds_.frame_ready = true;
  } else if (line_ == 192) {
    if (probe_enabled_) async_probe_check(false);
    nds_.io.set_vblank(true);
    fifo_rd_ = fifo_wr_ = 0;
    nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.dma.check(Cpu::ARM9, dma::MODE9_VBLANK);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_VBLANK);
    { prof::Scope v(prof::GX_VBLANK); nds_.gpu3d.vblank(); }
    if (capture_on_) { capcnt_ &= ~(1u << 31); capture_on_ = false; }
  } else if (line_ == 262) nds_.io.set_vblank(false);
  if (line_ >= 2 && line_ < 194) nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  else if (line_ == 194) nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  if (line_ < 192 && run_fifo_) nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 32 * 2, ev_fifo, 0);
  nds_.io.set_vcount(line_);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.event_time() + HBLANK_START, ev_hblank);
}

// Signature of only structural display choices (driving engine, display
// mode/bank, capture destination); per-frame changes like fades/scroll are excluded.
void Gpu::update_phase() {
  const u32 a = engine[0].dispcnt(), b = engine[1].dispcnt();
  const u32 sig = ((nds_.io.powcnt1 >> 15) & 1)
                | (((a >> 16) & 3) << 1) | (((a >> 18) & 3) << 3)
                | (((b >> 16) & 3) << 5)
                | ((capture_on_ ? 1u + ((capcnt_ >> 16) & 0xF) : 0u) << 7);
  for (u32 i = 0; i + 1 < PHASE_HISTORY; ++i) phase_sig_[i] = phase_sig_[i + 1];
  phase_sig_[PHASE_HISTORY - 1] = sig;
  if (phase_seen_ < PHASE_HISTORY) { ++phase_seen_; phase_period_ = 1; return; }
  // Smallest period that explains the whole window; none found means treat as 1.
  for (u32 p = 1; p <= PHASE_MAX; ++p) {
    bool ok = true;
    for (u32 i = p; i < PHASE_HISTORY && ok; ++i) ok = phase_sig_[i] == phase_sig_[i - p];
    if (ok) { phase_period_ = static_cast<u8>(p); return; }
  }
  phase_period_ = 1;
}

// The drawing page's lines that no frame has drawn since it became the drawing page, from the front page: what
// one buffer would still hold there. Between frames, nothing in flight.
void Gpu::patch_page() {
  if (front_ == page_) return;
  Page& d = pages_[page_];
  const Page& f = pages_[front_];
  const bool words = !d.split_fb[0].empty();
  for (int scr = 0; scr < 2; ++scr) {
    if (page_drawn_[scr][0] == ~u64{0} && page_drawn_[scr][1] == ~u64{0} && page_drawn_[scr][2] == ~u64{0}) continue;
    for (u32 y = 0; y < SCREEN_H; ++y) {
      if (page_drawn_[scr][y >> 6] >> (y & 63) & 1) continue;
      std::memcpy(d.fb[scr].data() + y * SCREEN_W, f.fb[scr].data() + y * SCREEN_W, SCREEN_W * sizeof(u32));
      if (words) {
        std::memcpy(d.split_fb[scr].data() + y * SCREEN_W, f.split_fb[scr].data() + y * SCREEN_W, SCREEN_W * sizeof(u32));
        std::memcpy(d.split_xy[scr].data() + y * SCREEN_W, f.split_xy[scr].data() + y * SCREEN_W, SCREEN_W * sizeof(u32));
      }
      d.split_mb[scr][y] = f.split_mb[scr][y]; d.split_reg[scr][y] = f.split_reg[scr][y];
    }
  }
}

void Gpu::hand_frame() {
  if (scaling() || lines_in_flight()) return;
  patch_page();
  front_ = page_; page_ ^= 1;
  static_assert(SCREEN_H == 192, "page_drawn_ holds 192 lines");
  for (auto& scr : page_drawn_) scr[0] = scr[1] = scr[2] = 0;
}

void Gpu::begin_frame() {
  frame_begun_ = true;
  // The frame just finished is what the readers get from here, taken with hand_frame or not: the page it was
  // drawn in, whole (the page handed before stays unwritten either way).
  if (front_ != page_ && !lines_in_flight()) { patch_page(); front_ = page_; }
  if (g_dbg_gpu)
    std::fprintf(stderr, "[gpu] frame %llu powcnt %04x dispcntA %08x dispcntB %08x mb %04x/%04x cap %08x vramcnt %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 static_cast<unsigned long long>(nds_.frame_count), nds_.io.powcnt1, engine[0].dispcnt(), engine[1].dispcnt(), master_bright_g_[0], master_bright_g_[1], capcnt_,
                 nds_.io.vramcnt[0], nds_.io.vramcnt[1], nds_.io.vramcnt[2], nds_.io.vramcnt[3], nds_.io.vramcnt[4], nds_.io.vramcnt[5], nds_.io.vramcnt[6], nds_.io.vramcnt[7], nds_.io.vramcnt[8]);
  if (g_dbg_vramnz) {
    std::fprintf(stderr, "[vram] frame %llu nz:", static_cast<unsigned long long>(nds_.frame_count));
    for (int i = 0; i < 9; ++i) { u32 n = 0; const u8* b = nds_.bus.vram_bank(i); for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) n += b[k] != 0; std::fprintf(stderr, " %c=%u", 'A' + i, n); }
    for (int i : {5, 7}) { const u8* b = nds_.bus.vram_bank(i); u32 lo = ~0u, hi = 0; for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) if (b[k]) { if (k < lo) lo = k; hi = k; } std::fprintf(stderr, " %c[%x..%x]", 'A' + i, lo, hi); }
    std::fputc('\n', stderr);
  }
  if (const char* f = g_dump_frame) {   // DS_DEBUG_DUMP_LINE=L: engine state and one rendered line
    if (nds_.frame_count == static_cast<u64>(std::atoi(f))) {
      const char* l = std::getenv("DS_DEBUG_DUMP_LINE"); const u32 line = l ? std::atoi(l) : 96;
      engine[0].debug_dump(line); engine[1].debug_dump(line);
    }
  }
  screens_on_ = nds_.io.powcnt1 & 1;
  // FIFO only needs clocking when something displays/captures from it, or a DMA channel awaits it.
  run_fifo_ = uses_fifo() || nds_.dma.in_mode(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
  if (capcnt_ & (1u << 31)) capture_on_ = true;
  if (capture_on_) capture_recent_ = CAPTURE_STICKY; else if (capture_recent_) --capture_recent_;
  // 3D frame this display frame reads, latched here since the raster moves on
  // at line 215 while the compositor may still be reading it.
  // A capture frame reads the 3D of this frame, never the one before; unless the capture is tagged whole and
  // shown from the frame's image (cap_late_, gpu.h), when the emulation does not wait for the raster.
  {
    const u32 un = cap_untagged_.load(std::memory_order_relaxed);
    if (capture_on_) cap_idle_ = 0; else if (++cap_idle_ > 4) cap_clean_ = 0;
    if (un != cap_untagged_seen_) { cap_untagged_seen_ = un; cap_clean_ = 0; } else if (capture_on_ && cap_clean_ < 255) ++cap_clean_;
    const bool tracked = split_req_ && !scaling() && !(colour_lut_ && colour_cpu_) && !raw_active_ && nds_.gpu3d.renderer().gpu_layers();
    if (capture_on_) { ++cap_frames_; if (cap_backoff_) ++cap_backoff_frames_; }
    if (cap_backoff_) --cap_backoff_;
    cap_late_ = capture_on_ && cap_late_ok_ && !cap_backoff_ && tracked && split_cap_ && nds_.gpu3d.renderer().gpu_scale() > 1 && ((capcnt_ >> 29) & 3) == 0 && cap_clean_ >= 3;
    late_at_ = -1; ++late_seq_;
    if (cap_late_) { for (int k = 0; k < 2; ++k) if (!late_[k].pending) late_at_ = k; cap_late_ = late_at_ >= 0; }
  }
  ref3d_ = nds_.gpu3d.frame_ref(!capture_on_ || cap_late_);
  // A frame the raster refused, drawn on the CPU: it has no record to wait for and no image to show the capture
  // from (its ref holds the picture itself). Captured as at 1x.
  if (cap_late_ && !ref3d_.gpu) { cap_late_ = false; late_at_ = -1; }
  if (cap_late_) ++cap_late_frames_;
  if (cap_late_) {
    LateCap& lc = late_[late_at_];
    ref3d_exact_ = ref3d_; ref3d_exact_.allow_lag = false; ref3d_exact_.out = nullptr;
    lc.pending = true; lc.frame = late_seq_; lc.ref = ref3d_; lc.bank = (capcnt_ >> 16) & 3; std::memset(lc.kind, 0, sizeof lc.kind);
    if (lc.u.empty()) { lc.u.resize(SCREEN_W * SCREEN_H); lc.w.resize(SCREEN_W * SCREEN_H); lc.px.resize(SCREEN_W * SCREEN_H); }
    lc.pre = false;
  }
  if (split_cur_.drawn) split_done_ = split_cur_;
  split_cur_ = SplitFrame{};
  split_cur_.track = split_req_ && !scaling() && !(colour_lut_ && colour_cpu_) && !raw_active_ && nds_.gpu3d.renderer().gpu_layers();
  split_cur_.want = split_cur_.track && ref3d_.gpu;
  if (split_cur_.track && cap_words_.empty()) {
    for (Page& pg : pages_) for (int k = 0; k < 2; ++k) { pg.split_fb[k].assign(SCREEN_W * SCREEN_H, 0); pg.split_xy[k].assign(SCREEN_W * SCREEN_H, 0); }
    cap_words_.assign(Engine2D::CAP_SLOTS * 65536, 0); cap_seg_.assign(Engine2D::CAP_SLOTS * 512, CapSeg{});
  }
  if (split_cur_.track != split_engine_) {   // step_engine sets engine A's split line by line while tracked
    split_engine_ = split_cur_.track;
    engine[0].set_cap_track(split_engine_ && split_cap_); engine[1].set_cap_track(split_engine_ && split_cap_);
    if (!split_engine_) { engine[0].set_split3d(Split3D::Off); std::fill(cap_seg_.begin(), cap_seg_.end(), CapSeg{}); for (CapCopy& c : cap_copy_) c.bytes = 0; }
  }
  update_phase();
  if (prof::enabled) {
    prof::add(prof::C_FRAMES_TOTAL, 1);
    if (phase_period_ > 1) prof::add(prof::C_FRAMES_PHASE_ALT, 1);
    if (capture_on_) prof::add(prof::C_FRAMES_CAPTURE, 1);
  }
  // Frameskip: what line 215 assumed, re-checked now the capture bit is known.
  skip_frame_ = skip_next_ && skippable();
  if (g_dbg_skip)
    std::fprintf(stderr, "[skip] frame %llu req %d raster %d capture %d/%u fifo %d period %u -> %s\n",
                 static_cast<unsigned long long>(nds_.frame_count), skip_req_ ? 1 : 0, skip_next_ ? 1 : 0,
                 capture_on_ ? 1 : 0, capture_recent_, run_fifo_ ? 1 : 0, phase_period_, skip_frame_ ? "skipped" : "drawn");
  render_next_[0] = render_next_[1] = 0;
  per_line_prev_[0] = per_line_[0]; per_line_prev_[1] = per_line_[1];
  per_line_[0] = per_line_[1] = false; frame_finished_ = false;
  // Per-engine futility: an engine that keeps ending its frames per line starts
  // them per line (bar a re-probe); the other engine keeps its batch.
  if (lazy_tried_)
    for (int e = 0; e < 2; ++e) eng_futile_[e] = per_line_prev_[e] ? eng_futile_[e] + 1 : 0;
  lazy_frame_ = lazy_enabled_ && !g_no_lazy && !run_fifo_;
  lazy_bursts_[0] = lazy_bursts_[1] = 0; burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0;
  if (lazy_frame_)
    for (int e = 0; e < 2; ++e) {
      if (eng_futile_[e] < LAZY_FUTILE_LIMIT) { eng_probe_in_[e] = LAZY_PROBE_PERIOD; continue; }
      if (--eng_probe_in_[e] == 0) { eng_probe_in_[e] = LAZY_PROBE_PERIOD; continue; }   // re-probe: try batching
      per_line_[e] = true;
      prof::add(e ? prof::C_2D_ENGINE_FUTILE_B : prof::C_2D_ENGINE_FUTILE_A, 1);
    }
  if (per_line_[0] && per_line_[1]) lazy_frame_ = false;   // both futile: nothing batches, nothing to trap for
  lazy_tried_ = lazy_frame_ || (lazy_enabled_ && !g_no_lazy && !run_fifo_);
  lag_frame_ = !run_fifo_ && !g_no_lag;
  lag_trap_hits_ = 0; lag_trap_stores_ = 0;
  {
    static const bool early_on = !(std::getenv("DS_2D_EARLY") && std::getenv("DS_2D_EARLY")[0] == '0');
    static const bool early_all = std::getenv("DS_2D_EARLY") && std::getenv("DS_2D_EARLY")[0] == '2';   // debug: every frame that may
    if (early_all) early_a_ = EARLY_A_HOLD;
    if (early_a_) --early_a_;
    early_ok_ = early_on && early_a_ && lazy_frame_ && lag_frame_ && !per_line_[0] && !scaling() && !skip_frame_ && (!ref3d_.gpu || !ref3d_.allow_lag);
  }
  if (snap_hit_) snap_want_ = SNAP_HOLD; else if (snap_want_) --snap_want_;
  snap_hit_ = false;
  arm_trap();
  if (lazy_frame_) prof::add(prof::C_2D_LAZY_FRAMES, 1);
  if (lag_frame_ && !lazy_frame_) prof::add(prof::C_2D_LAG_FRAMES, 1);
  if (!lazy_frame_) per_line_[0] = per_line_[1] = true;
}

// ---- main-memory display FIFO -----------------------------------------------

bool Gpu::uses_fifo() const {
  if (((engine[0].dispcnt() >> 16) & 3) == 3) return true;
  return (capcnt_ & (1 << 25)) && ((capcnt_ >> 29) & 3) != 0;
}

void Gpu::sample_fifo(u32 offset, u32 count) {
  for (u32 i = 0; i < count; ++i) { fifo_line_[offset + i] = fifo_[fifo_rd_]; fifo_rd_ = (fifo_rd_ + 1) & 0xF; }
}

// FIFO read out in 8-pixel steps starting ~3 pixels before the visible line
// (offset from the 8-pixel DMA grid). Each step requests the next DMA transfer (mode 4).
void Gpu::on_display_fifo(u32 x) {
  if (x > 0) { if (x == 8) sample_fifo(0, 5); else sample_fifo(x - 11, 8); }
  if (x < 256) {
    nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 6 * 8 * 2, ev_fifo, x + 8);
  } else sample_fifo(253, 3);
}

// ---- rendering and the output stage ------------------------------------------

// One engine's run of display lines on the worker; engine state is disjoint.
void Gpu::worker_job(void* self) {
  Gpu& g = *static_cast<Gpu*>(self);
  if (g.late_job_) { for (LateCap& lc : g.late_) if (lc.pending && lc.frame != g.late_job_seq_ && !lc.pre) g.late_lines(lc); return; }
  if (g_worker_delay_us) { const auto t = std::chrono::steady_clock::now() + std::chrono::microseconds(g_worker_delay_us); while (std::chrono::steady_clock::now() < t) {} }
  for (int e = 0; e < 2; ++e)
    for (u32 l = g.job_first_[e]; l <= g.job_last_[e]; ++l) g.step_engine(e, l);
  for (u32 i = 0; i < g.bscale_n_; ++i) {
    const StashedLine& st = g.bscale_[i];
    g.emit_scaled(st.screen, st.line, st.px);
  }
}

void Gpu::worker_b_job(void* self) {
  Gpu& g = *static_cast<Gpu*>(self);
  if (g.late_job_) { for (LateCap& lc : g.late_) if (lc.pending && lc.frame != g.late_job_seq_ && !lc.pre) g.late_lines(lc); return; }
  if (g_worker_delay_us) { const auto t = std::chrono::steady_clock::now() + std::chrono::microseconds(g_worker_delay_us); while (std::chrono::steady_clock::now() < t) {} }
  for (u32 l = g.bjob_first_; l <= g.bjob_last_; ++l) g.step_engine(1, l);
}

void Gpu::join_worker(JoinSite site) {
  if (!inflight_[0] && !inflight_[1] && !scale_inflight_) return;
  if (b_deferred_) worker_b_.wait(static_cast<int>(site));
  const bool dbg = g_dbg_join;
  if (dbg) std::fprintf(stderr, "[join] frame %llu line %u hblank %d a %u..%u b %u..%u deferred %d\n", (unsigned long long)nds_.frame_count, line_, hblank_done_ ? 1 : 0, job_first_[0], job_last_[0], job_first_[1], job_last_[1], a_deferred_ ? 1 : 0);
  if (a_deferred_ && site != JoinSite::Line0 && site != JoinSite::Other && !worker_.done()) early_a_ = EARLY_A_HOLD;
  { const u64 t0 = prof::now_ns(); worker_.wait(static_cast<int>(site));
    const u64 dt = prof::now_ns() - t0;
    join_wait_ns_ += dt; prof::add_ns(prof::W2D_JOIN, dt);
    if (prof::enabled) {
      prof::add(prof::C_W2D_JOIN_CALLS, 1);
      static constexpr prof::Counter kBySite[] = {
        prof::C_W2D_JOIN_NS_CATCHUP, prof::C_W2D_JOIN_NS_TRAP, prof::C_W2D_JOIN_NS_JOURNAL,
        prof::C_W2D_JOIN_NS_LINE0, prof::C_W2D_JOIN_NS_REMAP,
        prof::C_W2D_JOIN_NS_RPRE, prof::C_W2D_JOIN_NS_RPOST, prof::C_W2D_JOIN_NS_OTHER };
      prof::add(kBySite[static_cast<int>(site)], dt);
    } }
  inflight_[0] = inflight_[1] = false; scale_inflight_ = false; bscale_n_ = 0; early_inflight_ = false;
  for (int e = 0; e < 2; ++e) {
    engine[e].set_vram_override(nullptr);
    if (remap_pending_[e]) { engine[e].vram_remapped(); remap_pending_[e] = false; }
  }
  snapshot_stale_ = false; snap_live_ = false;
  if (b_deferred_) {   // as finish_a: writes the guest made meanwhile were journaled past the frame
    b_deferred_ = false;
    if (line_ == 0 || (line_ == 262 && hblank_done_)) skipped_[1] = false;
    engine[1].apply_pending();
    if (frame_finished_) engine[1].frame_done();
  }
  if (a_deferred_) { a_deferred_ = false; finish_a(); }
}

// Engine A's deferred batch has been joined: what render_ranges does at frame
// end, done now (read trap, then journal, then frame end).
void Gpu::finish_a() {
  release_read_trap();
  if (line_ == 0 || (line_ == 262 && hblank_done_)) skipped_[0] = false;
  engine[0].apply_pending();
  if (frame_finished_) { engine[0].frame_done(); disarm_trap(); }
}

void Gpu::debug_dump(FILE* f) {
  std::fprintf(f, "  gpu: line %u hblank_done %d render_next %u/%u lazy %d per_line %d/%d trap %d job a %u..%u b %u..%u inflight %d/%d deferred %d read_trap %d lag %d frame_ready %d\n", line_, hblank_done_ ? 1 : 0, render_next_[0], render_next_[1],
               lazy_frame_ ? 1 : 0, per_line_[0] ? 1 : 0, per_line_[1] ? 1 : 0, trap_armed_ ? 1 : 0, job_first_[0], job_last_[0], job_first_[1], job_last_[1], inflight_[0] ? 1 : 0, inflight_[1] ? 1 : 0, a_deferred_ ? 1 : 0, read_trap_bank_, lag_frame_ ? 1 : 0, nds_.frame_ready ? 1 : 0);
  worker_.debug_dump(f);
  if (split_) worker_b_.debug_dump(f);
  nds_.gpu3d.debug_dump(f);
}

// Render display lines of both engines: one hand-off to the worker, the other
// engine's run here. An empty range (first > last) means nothing due.
void Gpu::render_ranges(u32 af, u32 al, u32 bf, u32 bl) {
  const bool a_has = af <= al, b_has = bf <= bl;
  if (!a_has && !b_has) return;
  // A piece of engine A's batch in flight and engine B's line alone to draw: here, beside it (the engines'
  // state is disjoint, and nothing is handed that would snapshot the map).
  if (early_inflight_ && !a_has && bl < SCREEN_H - 1 && !scaling() && !inflight_[1] && !scale_inflight_) {
    prof::add(prof::C_2D_RANGE_B, 1);
    for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
    render_next_[1] = bl + 1;
    return;
  }
  const bool early_piece = a_has && al < SCREEN_H - 1 && early_ok_ && lazy_frame_ && !per_line_[0];
  join_worker(JoinSite::RangesPre);   // a job handed now snapshots the live map: nothing stale may still be in flight
  // The captures of the frames before, before anything of this range can show them. Line 0's sprites were
  // drawn at the end of the frame before (one line ahead): again, where they may show such a bank.
  if (late_fill()) { cap_backoff_len_ = CAP_BACKOFF_MIN; if (a_has && af == 0) engine[0].render_sprites(0); if (b_has && bf == 0) engine[1].render_sprites(0); }   // one that lasted to its own time: the stand-down starts short again
  capcnt_render_ = capcnt_; capture_render_ = capture_on_;
  late_render_ = cap_late_ ? late_at_ : -1;
  lcdc_mask_render_ = nds_.bus.vram_map().lcdc_mask;   // latched so the job doesn't consult a live map that may be rebuilding
  const bool dbg = g_dbg_join;
  auto hand = [&](bool a, bool b) {
    if (dbg) std::fprintf(stderr, "[hand] frame %llu line %u a %u..%u b %u..%u lag %d stash %u\n", (unsigned long long)nds_.frame_count, line_, a ? af : 1, a ? al : 0, b ? bf : 1, b ? bl : 0, lag_frame_ ? 1 : 0, bscale_n_);
    late_join();
    job_first_[0] = a ? af : 1; job_last_[0] = a ? al : 0;
    job_first_[1] = b ? bf : 1; job_last_[1] = b ? bl : 0;
    scale_inflight_ = bscale_n_ > 0;
    snapshot_map(0);
    if (a) engine[0].set_vram_override(&job_map_[0]);
    if (b) engine[1].set_vram_override(&job_map_[0]);
    worker_.dispatch();
    inflight_[0] = a; inflight_[1] = b;
    // A capture in flight writes an LCDC bank the guest may read before the join: trap it.
    if (a && capture_render_ && read_trap_bank_ < 0) {
      const int bank = static_cast<int>((capcnt_render_ >> 16) & 3);
      nds_.bus.set_lcdc_read_trap(bank, true);
      read_trap_bank_ = bank;
    }
  };
  const u32 a_len = a_has ? al - af + 1 : 0, b_len = b_has ? bl - bf + 1 : 0;
  const u32 last = a_has && b_has ? (al > bl ? al : bl) : a_has ? al : bl;
  bool a_handed = false, b_handed = false;
  // Read once: parked() can change under separate branches.
  const bool parked_at_decision = prof::enabled ? worker_.parked() : false;
  if (prof::enabled) {
    prof::add(prof::C_RR_CALLS, 1);
    prof::add(parked_at_decision ? prof::C_RR_PARKED : prof::C_RR_AWAKE, 1);
    if (b_has && b_len < 24 && !parked_at_decision) prof::add(prof::C_RR_SHORT_HANDED_AWAKE, 1);
    if (b_has && b_len < 24 && parked_at_decision) prof::add(prof::C_RR_SHORT_INLINE_PARKED, 1);
  }
  if (a_has && al == SCREEN_H - 1 && (a_len >= 24 || (!worker_.parked() && a_len >= b_len)) && trap_armed_ && (trap_mask_ & 1)) {
    // Run to the last display line: deferred join, engine B drawn here meanwhile.
    // Only under the trap over A's windows: the job stays in flight through
    // VBlank, when games stream VRAM. A per-line engine A outside a lag frame
    // (lag dropped mid-frame) has none, so its last line is drawn and joined now.
    prof::add(prof::C_RR_DEFER_A, 1);
    const bool b_split = split_ && b_has && bl == SCREEN_H - 1 && b_len >= 24 && trap_armed_ && (trap_mask_ & 2);
    static const bool snap_on = !(std::getenv("DS_2D_SNAP") && std::getenv("DS_2D_SNAP")[0] == '0');
    if (snap_on && snap_want_ && !capture_render_ && !trap_lcdc_) { take_snapshot(b_split ? 3u : 1u); snap_live_ = true; }
    hand(true, false);
    a_handed = true; a_deferred_ = true;
    if (b_split) {
      // Engine B's batch to its own worker, deferred to the same join: only a
      // batch run to the last line (everything journaled meanwhile is past the
      // frame) with the trap over the whole VRAM window (a lag frame's guards
      // only engine A's).
      bjob_first_ = bf; bjob_last_ = bl;
      snapshot_map(1);
      engine[1].set_vram_override(&job_map_[1]);
      worker_b_.dispatch();
      inflight_[1] = true; b_deferred_ = true; b_handed = true;
    }
  } else if (lag_frame_ && (a_has || (b_has && scaling()))) {
    // Lag: A's lines stay in flight until next HBlank unless this is the
    // last one. B is drawn here (its window streams per-line, joining lag on
    // every store), but its scaling is stashed with the job (bscale_).
    if (b_has && scaling()) {
      bscale_defer_ = true;
      for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
      bscale_defer_ = false;
      b_handed = true;                        // drawn, its scaling in flight
    }
    prof::add(prof::C_RR_LAG, 1);
    hand(a_has, false);
    a_handed = a_has;
    early_inflight_ = early_piece;
  } else if (b_has && (b_len >= 24 || !worker_.parked())) {
    // Short run against a parked worker drawn here: wake-up costs more than the lines do.
    prof::add(prof::C_RR_HAND_B, 1);
    hand(false, true);
    b_handed = true;
  } else {
    prof::add(prof::C_RR_NONE, 1);
  }
  if (a_has) prof::add(prof::C_2D_RANGE_A, 1);
  if (b_has) prof::add(prof::C_2D_RANGE_B, 1);
  if (prof::enabled) {
    prof::add(prof::C_RR_LINES_HANDED, (a_handed ? a_len : 0) + (b_handed ? b_len : 0));
    prof::add(prof::C_RR_LINES_INLINE, (a_has && !a_handed ? a_len : 0) + (b_has && !b_handed ? b_len : 0));
  }
  if (a_has && !a_handed) for (u32 x = af; x <= al; ++x) step_engine(0, x);
  if (b_has && !b_handed) for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
  // A's deferred batch stays in flight until line 0; a lagged run until the
  // next line. The last display line always joins.
  if (!a_deferred_) {
    // A B line left in flight needs B's windows trapped.
    if ((a_handed || b_handed || scale_inflight_) && lag_frame_ && last < SCREEN_H - 1 && !(b_handed && !(trap_armed_ && (trap_mask_ & 2)))) prof::add(prof::C_2D_LAG_LINES, 1);
    else join_worker(JoinSite::RangesPost);
  }
  if (a_has) render_next_[0] = al + 1;
  if (b_has) render_next_[1] = bl + 1;
  if (!frame_finished_ && render_next_[0] >= SCREEN_H && render_next_[1] >= SCREEN_H) {
    frame_finished_ = true;
    if (a_deferred_) { if (!b_deferred_) engine[1].frame_done(); }   // the deferred engines end (and lift traps) at the join
    else {
      join_worker(JoinSite::RangesPost);
      release_read_trap();
      engine[0].frame_done(); engine[1].frame_done(); disarm_trap();
    }
    // A late capture's bank stays read-trapped until late_fill (a job handed with the capture trapped it already).
    if (late_render_ >= 0 && capture_render_ && read_trap_bank_ < 0) {
      const int bank = static_cast<int>((capcnt_render_ >> 16) & 3);
      if (nds_.bus.vram_map().lcdc_mask & (1u << bank)) { nds_.bus.set_lcdc_read_trap(bank, true); read_trap_bank_ = bank; }
    }
  }
}

// One engine's display line, in hardware order: pre-scanline writes, window
// edges, the line's own writes, latches, the line, its output, next line's sprites.
void Gpu::step_engine(int e, u32 line) {
  Engine2D& en = engine[e];
  {
    prof::Scope jn(prof::JOURNAL);
    en.replay_to(line * 2);
    en.update_windows(line);
    en.replay_to(line * 2 + 1);
    en.pre_draw(line, false);
  }
  // Engine B on a hidden screen draws nothing; the line it comes back on re-renders its sprites.
  const bool draw = !skip_frame_ && !(e == 1 && !screen_visible_[en.screen()]);
  if (!draw) skipped_[e] = true; else if (skipped_[e]) { skipped_[e] = false; en.render_sprites(line); }
  // Reading the 3D line joins the raster bands: skip it when the raster never ran.
  // The 3D split for the present's merge: this line composes without BG0 and
  // never reads the record. Not on a screen already showing a capture's 3D
  // this frame (one 3D frame a screen).
  const int scr = en.screen();
  // A line engine A composes only to capture it (the screen shows a bank
  // or the FIFO meanwhile: Golden Sun: Dark Dawn draws all its 3D so) is
  // split as well, for the capture's tags.
  const u32 dmode = e == 0 ? (en.dispcnt() >> 16) & 3 : 1;
  const bool cap_comp = e == 0 && capture_render_ && !(capcnt_render_ & (1u << 24)) && ((capcnt_render_ >> 29) & 3) != 1;   // the capture takes the composed line
  const bool split = e == 0 && draw && split_cur_.want &&
                     (dmode == 1 ? screens_on_ && !(split_cur_.s[scr].any && !split_cur_.s[scr].live) : cap_comp);
  if (e == 0 && split_cur_.track) en.set_split3d(split ? Split3D::Gpu : Split3D::Off);
  if (e == 0 && draw) split_cur_.drawn = true;
  // Lines with the 3D layer on that are not split show the 1x record (DS_SPLIT_STAT): why.
  if (e == 0 && draw && split_cur_.track && !split && (en.dispcnt() & 0x108) == 0x108 && (dmode == 1 ? screens_on_ : cap_comp))
    ++(!split_cur_.want ? split_miss_cpu_ : split_miss_mixed_);
  // A split line reads the record only to capture it (below): the frame's own, waited for.
  // A late capture frame's line that is not split shows the 1x record itself: the frame's own, waited for.
  const bool exact3d = late_render_ >= 0 && draw && !split;
  if (e == 0 && (draw || capture_render_) && (!split || capture_render_)) { prof::Scope l3(prof::R3D_LINE); line3d_ = nds_.gpu3d.line(exact3d ? ref3d_exact_ : ref3d_, line); en.set_3d_line(line3d_); }
  if (draw) { en.render_line(line); output_engine(e, line); }
  // A late frame's line the engine could not split after all shows the 3D it was given, the frame before's: late capture stands down.
  if (e == 0 && draw && late_render_ >= 0 && split && !en.split_on() && (en.dispcnt() & 0x108) == 0x108) cap_untagged_.fetch_add(1, std::memory_order_relaxed);
  if (draw && split_cur_.track) {
    const bool live = split && en.split_on() && dmode == 1;
    if (live) {
      std::memcpy(wp().split_fb[scr].data() + line * SCREEN_W, en.split(), SCREEN_W * sizeof(u32));
      SplitScreen& ss = split_cur_.s[scr];
      if (!ss.any) { ss.any = ss.live = true; ss.ref = ref3d_; }
      ++split_lines_;
    } else if (split_cap_) split_capture_display(e, line);
    else std::memset(wp().split_fb[scr].data() + line * SCREEN_W, 0, SCREEN_W * sizeof(u32));
    const u16 reg = en.master_bright();
    const u32 mode = reg >> 14, f = std::min<u32>(reg & 0x1F, 16);   // kern::master_brightness as one multiply-add
    wp().split_reg[scr][line] = reg;
    // BG0's X offset on a line with the frame's own 3D: bits 20-28 (the merge reads the 3D pixel that far along).
    const u32 xpos = live ? nds_.gpu3d.layer_xpos(line) : 0;
    if (xpos) { split_cur_.s[scr].scrolled = true; ++split_scrolled_; }
    wp().split_mb[scr][line] = (mode == 1 ? (16 - f) | ((63 * f) << 8) : mode == 2 ? 16 - f : 16) | (xpos << 20);
  }
  if (e == 0 && capture_render_ && !skip_frame_) {
    DS_PROF(CAPTURE);
    // A split line's composite, for a capture that takes it (source A the
    // screen, not the 3D layer alone, and not source B alone): merged here,
    // as the present will. The capture is tagged with the line's split
    // words (a capture of the 3D layer alone: every pixel the layer's own).
    alignas(16) Pixel merged[SCREEN_W];
    static const std::array<u32, SCREEN_W> plain3d = [] { std::array<u32, SCREEN_W> a; a.fill(split3d::pack(0, split3d::PLAIN, 16, 0)); return a; }();
    const Pixel* comp = nullptr;
    const u32* words = nullptr;
    // A line with BG0 scrolled is captured untagged: its pixels are not where the frame's image has them.
    const bool cap_scrolled = nds_.gpu3d.layer_xpos(line) != 0;
    if (capcnt_render_ & (1u << 24)) { if (split_cur_.want && !cap_scrolled) words = plain3d.data(); }
    else if (split && en.split_on()) {
      if (!cap_scrolled) words = en.split();
      if (((capcnt_render_ >> 29) & 3) != 1) { kern::active::split_merge_line(en.split(), en.output(), line3d_, merged); comp = merged; ++split_cap_merged_; }
    }
    capture(line, comp, words);
    if (late_render_ >= 0 && words && ((capcnt_render_ >> 29) & 3) == 0) {   // what late_fill writes this line again from
      LateCap& lc = late_[late_render_];
      lc.cnt = capcnt_render_; lc.lcdc = lcdc_mask_render_;
      if (capcnt_render_ & (1u << 24)) lc.kind[line] = 2;
      else if (comp) {
        std::memcpy(lc.u.data() + line * SCREEN_W, en.output(), SCREEN_W * sizeof(Pixel));
        std::memcpy(lc.w.data() + line * SCREEN_W, en.split(), SCREEN_W * sizeof(u32));
        lc.kind[line] = 1;
      }
    }
  }
  // Sprites are rendered one line ahead of the backgrounds.
  if (draw && line < SCREEN_H - 1) {
    prof::Scope sc(prof::OBJ_DRAW, e == 0);
    en.render_sprites(line + 1);
  }
  { prof::Scope jn(prof::JOURNAL); en.post_draw(false); }
}

// The output stage for one engine's line: display mode, master brightness,
// 6->8 bit expansion, into the screen POWCNT1 bit 15 gives it (or scaled
// straight into the frontend's buffer).
void Gpu::output_engine(int e, u32 line) {
  prof::Scope sc(prof::OUTPUT, e == 0);
  const Engine2D& en = engine[e];
  const int screen = en.screen();
  const bool scaled = scaling();
  u32* dst = scaled ? line_out_[e].data() : wp().fb[screen].data() + line * SCREEN_W;
  if (!scaled) page_drawn_[screen][line >> 6] |= u64{1} << (line & 63);
  if (screens_on_) {
    if (e == 0) {
      const u32 mode = (en.dispcnt() >> 16) & 3;
      if (mode == 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else if (mode >= 2) output_a(line, dst);      // VRAM / FIFO display: expanded inside
      else { output_a(line, dst); expand_colours(dst); }
    } else {
      if ((en.dispcnt() >> 16) & 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else { output_b(dst); expand_colours(dst); }
    }
  } else { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF000000; }
  if (raw_active_ && raw_lines_[e] < SCREEN_H && (raw_lines_[e] != 0 || line == 0)) {
    std::memcpy(raw_fb_[screen].data() + line * SCREEN_W, dst, SCREEN_W * sizeof(u32));
    ++raw_lines_[e];
  }
  if (colour_lut_ && colour_cpu_ && screens_on_) kern::active::colour_correct(*colour_lut_, dst);   // the panel's black stays black when the LCDs are off
  if (scaled) {
    if (e == 1 && bscale_defer_ && bscale_n_ < SCREEN_H) {
      StashedLine& st = bscale_[bscale_n_++];
      st.line = line; st.screen = screen;
      std::memcpy(st.px, dst, sizeof st.px);
    } else emit_scaled(screen, line, dst);
  }
}

static inline bool row_kept(const Gpu::ScaleTarget& t, u32 y) { return y >= t.y_lo && y < t.y_hi; }
static inline u32* row_at(const Gpu::ScaleTarget& t, u32 y) { return t.px + (static_cast<size_t>(y) - t.y_lo) * t.pitch; }

// Mean of four 0xAARRGGBB pixels, per channel, alpha forced opaque.
static inline u32 mean4(u32 a, u32 b, u32 c, u32 d) {
  const u32 rb = (((a & 0xFF00FFu) + (b & 0xFF00FFu) + (c & 0xFF00FFu) + (d & 0xFF00FFu) + 0x020002u) >> 2) & 0xFF00FFu;
  const u32 g  = (((a & 0xFF00u) + (b & 0xFF00u) + (c & 0xFF00u) + (d & 0xFF00u) + 0x0200u) >> 2) & 0xFF00u;
  return 0xFF000000u | rb | g;
}
// The colour at least two of the four share (ties to the earlier pixel), or their mean when all differ.
static inline u32 mode4(u32 a, u32 b, u32 c, u32 d) {
  if (a == b || a == c || a == d) return a;
  if (b == c || b == d) return b;
  if (c == d) return c;
  return mean4(a, b, c, d);
}

// Rec.601-ish luma, 0..255*256.
static inline u32 luma(u32 c) { return ((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29; }
static inline u32 min4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l < bl) { best = p; bl = l; } }
  return best;
}
static inline u32 max4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l > bl) { best = p; bl = l; } }
  return best;
}
// Darkest/brightest of the four when it stands farther than `thr` from the
// block's mean luma (keeps a thin stroke while dithers/gradients average); mean otherwise.
static inline u32 extreme4(u32 a, u32 b, u32 c, u32 d, u32 thr) {
  const u32 lo = min4(a, b, c, d), hi = max4(a, b, c, d);
  const u32 m = (luma(a) + luma(b) + luma(c) + luma(d)) / 4;
  const u32 dlo = m - luma(lo), dhi = luma(hi) - m;
  if (dlo <= thr && dhi <= thr) return mean4(a, b, c, d);
  return dlo > dhi ? lo : dhi > dlo ? hi : mean4(a, b, c, d);
}

// sRGB <-> linear for the linear-light blend: 8-bit sRGB to 12-bit linear and back, built once.
namespace {
struct GammaLut {
  u16 to_lin[256];
  u8 from_lin[4096];
  GammaLut() {
    for (u32 i = 0; i < 256; ++i) {
      const double c = i / 255.0;
      const double l = c <= 0.04045 ? c / 12.92 : ds_pow_compat((c + 0.055) / 1.055, 2.4);
      to_lin[i] = static_cast<u16>(std::lround(l * 4095.0));
    }
    for (u32 i = 0; i < 4096; ++i) {
      const double l = i / 4095.0;
      const double c = l <= 0.0031308 ? l * 12.92 : 1.055 * ds_pow_compat(l, 1.0 / 2.4) - 0.055;
      from_lin[i] = static_cast<u8>(std::lround(c * 255.0));
    }
  }
};
const GammaLut& gamma_lut() { static const GammaLut lut; return lut; }

// Per-pixel weighted blend in linear light; w per pixel as blend_line_w.
void blend_line_linear(const u32* a, const u32* b, const u8* w, u32* out) {
  const GammaLut& g = gamma_lut();
  for (u32 i = 0; i < 256; ++i) {
    const u32 f = w[i];
    if (!f) { out[i] = a[i]; continue; }
    u32 r = 0xFF000000u;
    for (u32 sh = 0; sh < 24; sh += 8) {
      const u32 la = g.to_lin[(a[i] >> sh) & 255], lb = g.to_lin[(b[i] >> sh) & 255];
      r |= static_cast<u32>(g.from_lin[(la * (256 - f) + lb * f + 128) >> 8]) << sh;
    }
    out[i] = r;
  }
}
} // namespace

void Gpu::blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out) {
  alignas(16) u8 wt[SCREEN_W];
  std::memset(wt, static_cast<int>(w), sizeof wt);
  if (t.blend == 2) blend_line_linear(a, b, wt, out); else kern::active::blend_line_w(a, b, wt, out);
}

// One row with box-filter seams: the last pixel of a run that straddles two
// source pixels is their area-weighted blend.
void Gpu::emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst) {
  alignas(16) u32 next[SCREEN_W], seam[SCREEN_W];
  std::memcpy(next, src + 1, (SCREEN_W - 1) * sizeof(u32));
  next[SCREEN_W - 1] = src[SCREEN_W - 1];
  if (t.blend == 2) blend_line_linear(src, next, t.seam_w, seam); else kern::active::blend_line_w(src, next, t.seam_w, seam);
  kern::active::scale_row_straddle(src, seam, t.seam_w, t.xrun, dst);
}

bool Gpu::build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a) {
  a.cells = cells; a.cell_px = cell_px;
  a.first.assign(cells, 0); a.n.assign(cells, 0); a.w.assign(static_cast<size_t>(cells) * CELL_TAPS, 0);
  for (u32 i = 0; i < cells; ++i) {
    // Cell i covers source [i*src_n/cells, (i+1)*src_n/cells); tap weights are the overlap, 1/256 units.
    const u32 lo = i * src_n, hi = (i + 1) * src_n;         // in 1/cells units
    const u32 s0 = lo / cells, s1 = (hi + cells - 1) / cells; // taps [s0, s1)
    if (s1 - s0 > CELL_TAPS) return false;
    a.first[i] = static_cast<u16>(s0); a.n[i] = static_cast<u8>(s1 - s0);
    u32 sum = 0;
    for (u32 s = s0; s < s1; ++s) {
      const u32 olo = std::max(lo, s * cells), ohi = std::min(hi, (s + 1) * cells);
      u32 w = ((ohi - olo) * 256 + src_n / 2) / src_n;
      if (s + 1 == s1) w = 256 - sum;
      sum += w;
      a.w[static_cast<size_t>(i) * CELL_TAPS + (s - s0)] = static_cast<u16>(w);
    }
  }
  return true;
}

// One cell row: every cell's colour from its taps in the held lines, then the
// row of cells drawn as `cell_px` panel rows (the first the seam row when the
// grid is on) through the grid kernel with the cells' xrun.
void Gpu::emit_cells(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const CellMap& m = *t.cells;
  const u32 j = cell_row_[screen];
  if (j >= m.y.cells) return;
  const u32 l0 = m.y.first[j], ln = m.y.n[j];
  std::memcpy(cell_lines_[screen][line % CELL_TAPS], src, SCREEN_W * sizeof(u32));
  if (line + 1 < l0 + ln) return;
  auto held = [&](u32 v) { return cell_lines_[screen][(l0 + v) % CELL_TAPS]; };
  // The row is complete.
  alignas(16) u32 cells[SCREEN_W];
  const bool linear = t.blend == 2;
  const GammaLut& g = gamma_lut();
  const u16* wy = &m.y.w[static_cast<size_t>(j) * CELL_TAPS];
  for (u32 i = 0; i < m.x.cells; ++i) {
    const u32 s0 = m.x.first[i], sn = m.x.n[i];
    const u16* wx = &m.x.w[static_cast<size_t>(i) * CELL_TAPS];
    // Mean: the 2D box (weights in 1/65536), in sRGB or linear light.
    u32 acc[3] = {0, 0, 0};
    for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
      const u32 c = held(v)[s0 + u], wt = wx[u] * wy[v];
      for (u32 ch = 0; ch < 3; ++ch) { const u32 b = (c >> (8 * ch)) & 255; acc[ch] += (linear ? g.to_lin[b] : b) * wt; }
    }
    u32 mean = 0xFF000000u;
    for (u32 ch = 0; ch < 3; ++ch) {
      const u32 v = (acc[ch] + 32768) >> 16;
      mean |= static_cast<u32>(linear ? g.from_lin[std::min<u32>(v, 4095)] : v) << (8 * ch);
    }
    u32 out = mean;
    if (t.chunky != 2) {
      // Other modes use pixels >= half covered on both axes; largest-coverage pixel stands in for "top-left".
      u32 cand[CELL_TAPS * CELL_TAPS]; u32 nc = 0;
      u32 best = 0, bestw = 0;
      for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
        const u32 wt = wx[u] * wy[v];
        if (wt > bestw) { bestw = wt; best = held(v)[s0 + u]; }
        if (wx[u] >= 128 && wy[v] >= 128) cand[nc++] = held(v)[s0 + u];
      }
      if (nc == 0) cand[nc++] = best;
      switch (t.chunky) {
      case 1: out = best; break;
      case 3: {   // dominant: the most repeated candidate, ties to the earlier; the mean when all differ
        u32 bc = 0; out = mean;
        for (u32 a = 0; a < nc; ++a) { u32 cnt = 0; for (u32 b = 0; b < nc; ++b) cnt += cand[b] == cand[a]; if (cnt > bc && cnt >= 2) { bc = cnt; out = cand[a]; } }
        break;
      }
      case 4: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) < luma(out)) out = cand[a]; break; }
      case 5: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) > luma(out)) out = cand[a]; break; }
      default: {  // extreme: the darkest or brightest candidate, if farther than the threshold from the mean
        u32 lo = cand[0], hi = cand[0];
        for (u32 a = 1; a < nc; ++a) { if (luma(cand[a]) < luma(lo)) lo = cand[a]; if (luma(cand[a]) > luma(hi)) hi = cand[a]; }
        const u32 lm = luma(mean), dlo = lm > luma(lo) ? lm - luma(lo) : 0, dhi = luma(hi) > lm ? luma(hi) - lm : 0;
        out = (dlo <= t.chunky_thresh && dhi <= t.chunky_thresh) ? mean : dlo > dhi ? lo : dhi > dlo ? hi : mean;
      }
      }
    }
    cells[i] = out;
  }
  for (u32 i = m.x.cells; i < SCREEN_W; ++i) cells[i] = 0;
  // Draw: rows [j*P, (j+1)*P), the first the seam row.
  const u32 P = m.y.cell_px;
  const u32 y0 = j * P;
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  const bool grid = t.grid < 256;
  if (t.xrun[SCREEN_W] > SCALED_ROW_MAX) return;
  u32* row = row_scratch_[screen];
  if (grid) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, 1, false, row);
  else      kern::active::scale_row(cells, t.xrun, row);
  for (u32 y = y0 + (grid ? 1 : 0); y < y0 + P; ++y)
    if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
  if (grid && row_kept(t, y0)) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, 1, true, row_at(t, y0));
  cell_row_[screen] = j + 1;
}

void Gpu::scale_image(int screen, const u32* src) {
  if (!scale_[screen].px) return;
  for (u32 line = 0; line < SCREEN_H; ++line) emit_scaled(screen, line, src + line * SCREEN_W);
}

// Bilinear. Destination row y samples v = (y+0.5)*192/h - 0.5 in source
// lines; rows blending L-1 and L are written when L arrives. Rows above line
// 0's centre / below line 191's are that line alone (clamped). Source line L
// owns rows [ystart(L), ystart(L+1)); every row claimed exactly once.
void Gpu::emit_bilinear(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const u32 w = t.xrun[SCREEN_W];
  if (w > SCALED_ROW_MAX || w == 0 || t.h == 0) return;
  const u32 h = t.h;
  auto ystart = [h](u32 l) -> u32 {
    if (l == 0) return 0;
    const u32 k = ((2 * l - 1) * h + SCREEN_H - 1) / SCREEN_H;   // ceil((2L-1)h/192)
    return std::min(h, k / 2);
  };
  // Widen this line; previous line's row kept if it is line-1.
  const bool have_prev = line > 0 && lin_prev_line_[screen] + 1 == line;
  const u32 cur = have_prev ? lin_cur_[screen] ^ 1u : 0u;
  u32* const hcur = lin_row_[screen][cur];
  const u32* const hprev = lin_row_[screen][cur ^ 1u];
  kern::active::lerp_row_gather(src, t.lin_sx, t.lin_wx, w, hcur);
  lin_cur_[screen] = cur;
  lin_prev_line_[screen] = line;
  const size_t bytes = static_cast<size_t>(w) * sizeof(u32);
  auto out_row = [&](u32 y) -> u32* { return row_kept(t, y) ? row_at(t, y) : nullptr; };   // null: cropped away
  if (line == 0) {
    for (u32 y = 0; y < ystart(1); ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
    if (line + 1 < SCREEN_H) return;
  }
  if (have_prev) {
    const u32 y0 = ystart(line), y1 = ystart(line + 1);
    for (u32 y = y0; y < y1; ++y) {
      // Weight of this line: v - (line-1), in 1/256.
      const s32 wf = static_cast<s32>(((2 * y + 1) * SCREEN_H * 128) / h) - 128 - static_cast<s32>((line - 1) * 256);
      const u32 wy = static_cast<u32>(std::min(255, std::max(0, wf)));
      u32* const o = out_row(y);
      if (!o) continue;
      if (wy == 0) std::memcpy(o, hprev, bytes);
      else kern::active::lerp_rows(hprev, hcur, wy, w, o);
    }
  } else {
    // A gap in the lines (hidden span, or first line after reset): this line stands alone.
    for (u32 y = ystart(line); y < ystart(line + 1); ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
  }
  if (line + 1 == SCREEN_H)
    for (u32 y = ystart(SCREEN_H); y < h; ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
}

void Gpu::emit_scaled(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  if (t.bilinear && t.lin_sx && t.lin_wx) { emit_bilinear(screen, line, src); return; }
  if (t.chunky && t.cells) {
    if (line == 0) cell_row_[screen] = 0;
    emit_cells(screen, line, src);
    return;
  }
  u32 first = line, last = line;
  alignas(16) u32 block[SCREEN_W];
  if (t.chunky) {
    // Each 2x2 block is one cell (xrun merges pixel pairs; line pair merged
    // here). Top-left draws from the even line; other modes resolve on the odd one.
    if (t.chunky == 1) { if (line & 1) return; last = line + 1; }
    else {
      if (!(line & 1)) { std::memcpy(chunk_even_[screen], src, sizeof block); return; }
      first = line - 1;
      const u32* up = chunk_even_[screen];
      switch (t.chunky) {
      case 2:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mean4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 3:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mode4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 4:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = min4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 5:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = max4(up[s], up[s + 1], src[s], src[s + 1]); break;
      default: for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = extreme4(up[s], up[s + 1], src[s], src[s + 1], t.chunky_thresh); break;
      }
      src = block;
    }
  }
  const u32 y0 = (first * t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 y1 = ((last + 1) * t.h + SCREEN_H - 1) / SCREEN_H;
  if (y0 >= y1) return;                      // downscale: this line is dropped
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  // Rows built in cached scratch and copied out, never read back from the
  // target. A row wider than the scratch goes direct, which a cropped view cannot.
  const bool stage = t.xrun[SCREEN_W] <= SCALED_ROW_MAX;
  const bool cropped = t.y_lo != 0 || t.y_hi != t.h;
  if (!stage && cropped) return;
  u32* row = stage ? row_scratch_[screen] : row_at(t, y0);
  const bool blend = t.blend && t.seam_w;   // --seam blend: box-filter seams stand in for the grid
  if (blend && t.h >= SCREEN_H) {
    // Box-filter seams: a straddling panel pixel/row is an area-weighted
    // blend, others are nearest. The straddling row of this span is its last
    // and needs the next line, so it's written when that arrives. Upscales
    // only: a downscale drops lines and never returns for the row they left.
    const u32 hb = (last + 1) * t.h;                 // this span's lower boundary, in 1/192 rows
    const bool straddle_below = (hb % SCREEN_H) != 0 && last + 1 < SCREEN_H;
    const u32 ycrisp_end = straddle_below ? y1 - 1 : y1;
    if (ycrisp_end > y0) {
      emit_row_straddle(t, src, row);
      for (u32 y = stage ? y0 : y0 + 1; y < ycrisp_end; ++y)
        if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
    }
    // Row above straddles the previous line and this one: boundary falls
    // frac/192 down it, previous line owns that much, this line the rest.
    if (first > 0 && seam_prev_line_[screen] + 1 == first) {
      const u32 tb = first * t.h;                    // this span's upper boundary
      const u32 frac = tb % SCREEN_H;
      if (frac) {
        alignas(16) u32 mid[SCREEN_W];
        blend_rows(t, seam_prev_[screen], src, ((SCREEN_H - frac) * 256) / SCREEN_H, mid);   // weight of this line
        if (row_kept(t, y0 - 1)) emit_row_straddle(t, mid, row_at(t, y0 - 1));
      }
    }
    std::memcpy(seam_prev_[screen], src, sizeof seam_prev_[screen]);
    seam_prev_line_[screen] = last;
    return;
  }
  // Plain nearest: no grid, or seam blend on a view too small for its seams.
  if (t.grid >= 256 || blend) {
    kern::active::scale_row(src, t.xrun, row);
    for (u32 y = stage ? y0 : y0 + 1; y < y1; ++y)
      if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
    return;
  }
  // LCD grid: last output column/row of every source pixel's run/span is
  // dimmed, so each DS pixel shows a lit cell with a dark seam right/below.
  // A run of one pixel is left alone. Only runs the fractional scale widened
  // carry a seam (min_run = ceil(scale)); the seam leads its run. At exactly
  // 2x a full-strength seam per pixel reads as a wash, so a black seam goes on every other pixel instead
  // (4-pixel pitch); a dimmed one stays on every pixel.
  const u32 w = t.xrun[SCREEN_W];
  const u32 min_run = (w + SCREEN_W - 1) / SCREEN_W, min_rows = (t.h + SCREEN_H - 1) / SCREEN_H;
  const bool black = t.grid == 0;
  const u32 pitch_x = black && w == 2 * SCREEN_W ? 2 : 1, pitch_y = black && t.h == 2 * SCREEN_H ? 2 : 1;
  const bool seam = y1 - y0 >= std::max<u32>(2, min_rows) && first % pitch_y == 0;
  const u32 yfirst = seam ? y0 + 1 : y0;
  row = stage ? row_scratch_[screen] : row_at(t, yfirst);
  kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, pitch_x, false, row);
  for (u32 y = stage ? yfirst : yfirst + 1; y < y1; ++y)
    if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
  if (seam && row_kept(t, y0)) kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, pitch_x, true, row_at(t, y0));
}

void Gpu::output_a(u32 line, u32* dst) {
  const u32 dispcnt = engine[0].dispcnt();
  switch ((dispcnt >> 16) & 3) {
  case 0: for (u32 i = 0; i < 256; ++i) dst[i] = 0x3F3F3F; return;          // display off: white
  case 1: { const Pixel* src = engine[0].output(); for (u32 i = 0; i < 256; ++i) dst[i] = src[i]; break; }
  case 2: {                                                                 // VRAM display (LCDC bank)
    const u32 bank = (dispcnt >> 18) & 3;
    const VramMap& vm = nds_.bus.vram_map();   // bank pointers are remap-invariant
    static constexpr u16 kZeroLine[256] = {};
    const u16* src = (lcdc_mask_render_ & (1u << bank)) ? reinterpret_cast<const u16*>(vm.bank(bank)) + line * 256 : kZeroLine;
    kern::active::output_vram_line(src, engine[0].master_bright(), dst);
    return;
  }
  case 3: kern::active::output_vram_line(fifo_line_.data(), engine[0].master_bright(), dst); return;
  }
  apply_master_brightness(engine[0].master_bright(), dst);
}

void Gpu::output_b(u32* dst) {
  if (!((engine[1].dispcnt() >> 16) & 1)) { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF3F3F3F; return; }
  const Pixel* src = engine[1].output();
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  apply_master_brightness(engine[1].master_bright(), dst);
}

// 128 pixels of a bank as display capture left them: to tell them written to since.
static inline u32 cap_sum(const u16* p) {
  u32 a = 0, b = 0;
  for (u32 i = 0; i < 128; ++i) { a += p[i]; b += p[i] * (i + 1); }
  return a ^ (b * 0x9E3779B1u);
}

// Display capture: blends source A (engine A composite or the 3D layer) with
// source B (VRAM or the display FIFO) into an LCDC-mapped bank as BGR555.
void Gpu::capture(u32 line, const Pixel* composite, const u32* words) {
  const u32 cnt = capcnt_render_;
  const u32 size = (cnt >> 20) & 3;
  const u32 width = size == 0 ? 128 : 256, height = size == 0 ? 128 : 64 * size;
  if (line >= height) return;
  const u32 dst_bank = (cnt >> 16) & 3;
  // Bank pointers are remap-invariant so the live map is safe here; lcdc_mask is not (latched at hand-off).
  const VramMap& vm = nds_.bus.vram_map();
  const u32 lcdc = lcdc_mask_render_;
  if (!(lcdc & (1u << dst_bank))) return;
  u16* dst = reinterpret_cast<u16*>(vm.bank(dst_bank)) + (((((cnt >> 18) & 3) << 14) + line * width) & 0xFFFF);

  const Pixel* src_a = (cnt & (1 << 24)) ? line3d_ : composite ? composite : engine[0].output();
  const u16* src_b = nullptr;
  if (cnt & (1 << 25)) src_b = fifo_line_.data();
  else {
    const u32 dispcnt = engine[0].dispcnt();
    const u32 src_bank = (dispcnt >> 18) & 3;
    if (lcdc & (1u << src_bank)) {
      u32 off = line * 256;
      if (((dispcnt >> 16) & 3) != 2) off += ((cnt >> 26) & 3) << 14;
      src_b = reinterpret_cast<const u16*>(vm.bank(src_bank)) + (off & 0xFFFF);
    }
  }

  // The 3D split: what this line leaves in the bank, for when it is shown again.
  const u32 cap_idx = ((((cnt >> 18) & 3) << 14) + line * width) & 0xFFFF;
  const bool cap_track = split_cur_.track && split_cap_ && cap_idx + width <= 65536;
  // A line with 3D in what it captures and no tag for it: its bank pixels are all a later display has (cap_late_).
  if (!(cap_track && words && ((cnt >> 29) & 3) == 0) && ((cnt >> 29) & 3) != 1 && ((cnt & (1 << 24)) || (engine[0].dispcnt() & 0x108) == 0x108))
    cap_untagged_.fetch_add(1, std::memory_order_relaxed);
  u32 cap_uniform = 0;
  bool cap_each = false;
  if (cap_track && words && ((cnt >> 29) & 3) == 0) {   // source A alone
    // Not where the layer is hidden (op 0), nor where a 2D pixel stands in front unblended (FIXED, ka 0): the VRAM pixel is all there is.
    auto tag_of = [](u32 w) { return (w & (0x1Fu << split3d::KA_SHIFT | 3u << split3d::OP_SHIFT)) <= (split3d::FIXED << split3d::OP_SHIFT) ? 0u : w | split3d::Q15; };
    u32 diff = 0;
    for (u32 x = 0; x < width; ++x) diff |= words[x] ^ words[0];
    if (!diff) cap_uniform = tag_of(words[0]);   // one word for the line (a 3D scene with nothing over it): nothing per pixel to keep
    else {
      u32* const cw = cap_words_.data() + dst_bank * 65536 + cap_idx;
      u32 any = 0;
      for (u32 x = 0; x < width; ++x) { const u32 t = tag_of(words[x]); cw[x] = t; any |= t; }
      cap_each = any != 0;
    }
    if ((cap_uniform || cap_each) && cap_ref_[cap_ref_at_].gpu_gen != ref3d_.gpu_gen) {
      cap_ref_at_ = (cap_ref_at_ + 1) % 4; cap_ref_[cap_ref_at_] = ref3d_;
      // The last two captured frames keep their images while the game shows them, however many frames it draws meanwhile.
      const Renderer3D::FrameRef& before = cap_ref_[(cap_ref_at_ + 3) % 4];
      nds_.gpu3d.renderer().gpu_pin(ref3d_.gpu_gen, before.gpu ? before.gpu_gen : 0);
    }
  }
  switch ((cnt >> 29) & 3) {
  case 0:
    kern::active::capture_a15(src_a, width, dst);
    break;
  case 1:
    if (src_b) std::memcpy(dst, src_b, width * sizeof(u16));
    else std::memset(dst, 0, width * sizeof(u16));
    break;
  default: {
    u32 eva = cnt & 0x1F, evb = (cnt >> 8) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    // A missing B source (unmapped LCDC bank) reads as zero either way.
    static constexpr u16 kZeroLine[256] = {};
    kern::active::capture_blend(src_a, src_b ? src_b : kZeroLine, width, eva, evb, dst);
    break;
  }
  }
  if (cap_track)
    for (u32 k = 0; k < width; k += 128) {
      CapSeg& seg = cap_seg_[dst_bank * 512 + ((cap_idx + k) >> 7)];
      seg.gen = cap_uniform || cap_each ? ref3d_.gpu_gen : 0;
      seg.base = static_cast<u16>((cap_idx - line * width) & 0xFFFF); seg.width = static_cast<u16>(width);
      seg.uniform = cap_uniform; seg.each = cap_each;
      seg.sum = cap_sum(dst + k);
    }
}


void Gpu::set_colour_profile(ColourProfile p) {
  if (p == colour_) return;
  quiesce();   // the worker reads the tables
  if (p == ColourProfile::Off) colour_lut_.reset();
  else {
    auto lut = std::make_shared<kern::ColourLut>();
    build_colour_lut(p, *lut);
    colour_lut_ = std::move(lut);
  }
  colour_ = p;
}

void Gpu::set_colour_on_cpu(bool on) {
  if (on == colour_cpu_) return;
  quiesce();   // every line of a frame one way or the other
  colour_cpu_ = on;
}

void Gpu::request_raw_frame() {
  quiesce();   // no line of the frame before may land after this
  for (auto& f : raw_fb_) f.assign(SCREEN_W * SCREEN_H, 0xFF000000);
  raw_lines_[0] = raw_lines_[1] = 0;
  raw_active_ = true;
}

void Gpu::apply_master_brightness(u16 reg, u32* dst) { kern::active::master_brightness(reg, dst); }

// 6-bit RGB666 records -> 8-bit 0xAARRGGBB (top two bits replicated into the low two).
void Gpu::expand_colours(u32* dst) { kern::active::expand_colours(dst); }


// The late captures of frames before this one, written again with their own 3D frame (waited for): each line
// as capture() wrote it, where the bank still holds what that wrote. The worker is idle (render_ranges, quiesce).
bool Gpu::late_fill(bool current) {
  late_join();
  bool any = false;
  for (LateCap& lc : late_) {
    if (!lc.pending || (lc.frame == late_seq_ && !current)) continue;
    lc.pending = false;
    const u32 cnt = lc.cnt, size = (cnt >> 20) & 3;
    const u32 width = size == 0 ? 128 : 256, height = size == 0 ? 128 : 64 * size;
    const u32 dst_bank = (cnt >> 16) & 3;
    if (!(lc.lcdc & (1u << dst_bank))) continue;
    // The bank itself, wherever the game has mapped it since (to show it): bank pointers are remap-invariant.
    u16* const bank = reinterpret_cast<u16*>(nds_.bus.vram_map().bank(dst_bank));
    bool filled = false;
    for (u32 line = 0; line < std::min<u32>(height, SCREEN_H); ++line) {
      if (!lc.kind[line]) continue;
      const u32 cap_idx = ((((cnt >> 18) & 3) << 14) + line * width) & 0xFFFF;
      if (cap_idx + width > 65536) continue;
      u16* const dst = bank + cap_idx;
      // Still that capture's pixels: its tags, and nothing written over them.
      bool ours = true;
      for (u32 k = 0; k < width; k += 128) { const CapSeg& seg = cap_seg_[dst_bank * 512 + ((cap_idx + k) >> 7)]; ours = ours && seg.gen == lc.ref.gpu_gen && seg.sum == cap_sum(dst + k); }
      if (!ours) continue;
      if (!lc.pre) late_lines(lc);
      std::memcpy(dst, lc.px.data() + line * SCREEN_W, width * sizeof(u16));
      for (u32 k = 0; k < width; k += 128) cap_seg_[dst_bank * 512 + ((cap_idx + k) >> 7)].sum = cap_sum(dst + k);
      filled = true;
    }
    if (filled) { ++cap_late_filled_; any = true; }
  }
  if (read_trap_bank_ >= 0 && !lines_in_flight()) release_read_trap();   // held for a capture now written
  return any;
}

// A late capture's lines with its own 3D frame (waited for), each as capture() would have written it. On the
// emulation thread or the 2D worker: nothing here but the LateCap (all its lines drawn) and that frame's record.
void Gpu::late_lines(LateCap& lc) {
  const u32 size = (lc.cnt >> 20) & 3;
  const u32 width = size == 0 ? 128 : 256, height = size == 0 ? 128 : 64 * size;
  Renderer3D::FrameRef r = lc.ref;
  r.allow_lag = false; r.out = nullptr;   // that frame's own record
  nds_.gpu3d.renderer().sync_line(r, 0);
  alignas(16) Pixel merged[SCREEN_W];
  for (u32 line = 0; line < std::min<u32>(height, SCREEN_H); ++line) {
    if (!lc.kind[line]) continue;
    const Pixel* src = r.line(line);
    if (lc.kind[line] == 1) { kern::active::split_merge_line(lc.w.data() + line * SCREEN_W, lc.u.data() + line * SCREEN_W, src, merged); src = merged; }
    kern::active::capture_a15(src, width, lc.px.data() + line * SCREEN_W);
  }
  lc.pre = true;
}

// Line 0, after the join and begin_frame: the captures of the frames before go to the worker.
void Gpu::late_prefill() {
  static const bool on = !(std::getenv("DS_CAP_PRE") && std::getenv("DS_CAP_PRE")[0] == '0');
  if (!on || late_inflight_ || lines_in_flight() || !worker_.running()) return;
  bool due = false;
  for (const LateCap& lc : late_) due = due || (lc.pending && lc.frame != late_seq_ && !lc.pre);
  if (!due) return;
  late_job_seq_ = late_seq_; late_job_ = true; late_inflight_ = true;
  worker_.dispatch();
}

void Gpu::quiesce() {
  join_worker();
  late_fill();
  engine[0].apply_pending(); engine[1].apply_pending();
}

void Gpu::prepare_load() {
  join_worker(); late_join();
  release_read_trap();
  disarm_trap();
  lazy_frame_ = false; per_line_[0] = per_line_[1] = true;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = true;
  burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0; lag_frame_ = false;
  split_cur_ = split_done_ = SplitFrame{};
  for (CapCopy& c : cap_copy_) c.bytes = 0;
}

// The bank (a slot of Engine2D::CAP_SLOTS) and 16-bit pixel behind an ARM9 VRAM address that starts 128
// pixels lying in one bank.
bool Gpu::vram_pixels_at(u32 addr, u32& bank, u32& idx) const {
  const VramMap& vm = nds_.bus.vram_map();
  const u32 a = addr & 0x00FFFFFF;
  const u8* p;
  if (a >= 0x800000) {
    const u32 b = (a - 0x800000) >> 17;
    if (b > 3 || !(vm.lcdc_mask & (1u << b))) return false;
    p = vm.bank(static_cast<int>(b)) + (a & 0x1FFFF);
  } else {
    const VramView& v = a < 0x200000 ? vm.abg : a < 0x400000 ? vm.bbg : a < 0x600000 ? vm.aobj : vm.bobj;
    if (!v.size) return false;
    p = v.direct(a, 256);
    if (!p) return false;
  }
  for (u32 b = 0; b < Engine2D::CAP_SLOTS; ++b) {
    const u8* const base = vm.bank(Engine2D::cap_slot_bank(b));
    if (p < base || p >= base + Engine2D::cap_slot_size(b)) continue;
    const u32 off = static_cast<u32>(p - base);
    if (off & 0xFF) return false;
    bank = b; idx = off >> 1;
    return true;
  }
  return false;
}

void Gpu::dma_copied(u32 src, u32 dst, u32 bytes) {
  if (cap_seg_.empty() || !bytes || (bytes & 0xFF)) return;
  const VramMap& vm = nds_.bus.vram_map();
  const u32 n = bytes >> 8;
  if ((src >> 24) == 0x06 && (dst >> 24) == 0x02) {
    // Out of a bank: keep the tags of what is still as the capture wrote it.
    if (n > 4 * 512) return;
    u32 b0, i0;
    if (!vram_pixels_at(src, b0, i0)) return;
    // The capture's lines may still be on their way to the bank's tags.
    join_worker(JoinSite::Trap);
    // A late capture on its way out before the next frame's render: written with its own 3D frame now, and the
    // block copied again (Golden Sun: Dark Dawn moves every captured frame through main RAM).
    const bool refilled = (late_[0].pending || late_[1].pending) && late_fill(render_next_[0] >= SCREEN_H);
    if (refilled) { cap_stand_down(); ++cap_taken_dma_; }
    if (refilled && !nds_.dsi) {
      const u32 at = dst & (mem::Bus::MAIN_RAM_SIZE - 1);
      if (at + bytes <= mem::Bus::MAIN_RAM_SIZE)
        for (u32 k = 0; k < n; ++k) {
          u32 b, i;
          if (vram_pixels_at(src + (k << 8), b, i)) std::memcpy(nds_.bus.main_ram.get() + at + (k << 8), vm.bank(Engine2D::cap_slot_bank(b)) + i * 2, 256);
        }
    }
    CapCopy* rec = nullptr;
    for (CapCopy& c : cap_copy_) if (c.bytes && c.addr == dst) rec = &c;
    bool any = false;
    for (u32 k = 0; k < n && !any; ++k) {
      u32 b, i;
      any = vram_pixels_at(src + (k << 8), b, i) && cap_seg_[b * 512 + (i >> 7)].gen != 0;
    }
    if (!any) { if (rec) rec->bytes = 0; return; }
    if (!rec) { cap_copy_at_ ^= 1; rec = &cap_copy_[cap_copy_at_]; }
    rec->addr = dst; rec->bytes = bytes;
    rec->seg.assign(n, CapSeg{});
    bool each = false;
    for (u32 k = 0; k < n; ++k) {
      u32 b, i;
      if (!vram_pixels_at(src + (k << 8), b, i)) continue;
      const CapSeg& s = cap_seg_[b * 512 + (i >> 7)];
      if (!s.gen || cap_sum(reinterpret_cast<const u16*>(vm.bank(Engine2D::cap_slot_bank(b))) + i) != s.sum) continue;
      CapSeg& r = rec->seg[k];
      r = s; r.base = static_cast<u16>((i - s.base) & 0xFFFF);
      if (s.each) {
        if (!each) { each = true; rec->words.resize(static_cast<size_t>(n) * 128); }
        std::memcpy(rec->words.data() + k * 128, cap_words_.data() + b * 65536 + i, 128 * sizeof(u32));
      }
      ++cap_copied_out_;
    }
    return;
  }
  if ((src >> 24) != 0x02 || (dst >> 24) != 0x06) return;
  // Back into a bank: the destination is tagged as that capture's pixels,
  // if they are still what went out.
  for (const CapCopy& c : cap_copy_) {
    if (!c.bytes || src < c.addr || src - c.addr + bytes > c.bytes || ((src - c.addr) & 0xFF)) continue;
    const u32 k0 = (src - c.addr) >> 8;
    for (u32 k = 0; k < n; ++k) {
      u32 b, i;
      if (!vram_pixels_at(dst + (k << 8), b, i)) continue;
      const CapSeg& r = c.seg[k0 + k];
      CapSeg& d = cap_seg_[b * 512 + (i >> 7)];
      if (!r.gen || cap_sum(reinterpret_cast<const u16*>(vm.bank(Engine2D::cap_slot_bank(b))) + i) != r.sum) { d.gen = 0; continue; }
      d = r; d.base = static_cast<u16>((i - r.base) & 0xFFFF);
      if (r.each) std::memcpy(cap_words_.data() + b * 65536 + i, c.words.data() + (k0 + k) * 128, 128 * sizeof(u32));
      ++cap_copied_in_;
    }
    return;
  }
}

// A line not split for this frame's 3D: where it shows pixels of a captured
// image (a bitmap BG, bitmap sprites, VRAM display) as the capture wrote
// them, their words for the merge, with the frame that was captured.
void Gpu::split_capture_display(int e, u32 line) {
  const Engine2D& en = engine[e];
  const int scr = en.screen();
  SplitScreen& ss = split_cur_.s[scr];
  u32* const row = wp().split_fb[scr].data() + line * SCREEN_W;
  u32* const xy = wp().split_xy[scr].data() + line * SCREEN_W;
  alignas(16) u32 refs[SCREEN_W];
  const u32 mode = e == 0 ? (en.dispcnt() >> 16) & 3 : (en.dispcnt() >> 16) & 1;
  bool have = false, vram = false;
  if (screens_on_ && !ss.live) {
    if (mode == 1) have = en.cap_refs(refs);
    else if (e == 0 && mode == 2) {
      const u32 bank = (en.dispcnt() >> 18) & 3;
      if (lcdc_mask_render_ & (1u << bank)) {
        for (u32 x = 0; x < SCREEN_W; ++x) refs[x] = Engine2D::CAP_REF | (bank << 16) | (line * 256 + x);
        have = vram = true;
      }
    }
  }
  if (!have) { std::memset(row, 0, SCREEN_W * sizeof(u32)); return; }
  const VramMap& vm = nds_.bus.vram_map();
  const Pixel* const out = en.output();
  const u32 mreg = en.master_bright();
  const bool mb_on = ((mreg >> 14) == 1 || (mreg >> 14) == 2) && (mreg & 0x1F) != 0;
  auto frame_of = [&](u64 gen) -> const Renderer3D::FrameRef* {
    for (const Renderer3D::FrameRef& c : cap_ref_) if (c.gpu && c.gpu_gen == gen) return &c;
    return nullptr;   // captured too long ago
  };
  u32 shown = 0, moved = 0, changed = 0, other = 0;
  // The usual line: one capture's line shown where it was captured (a
  // full-screen image, whatever carries it). Then the words are the
  // capture's, less the pixels that are not on top or not shown as they are.
  {
    u32 x0 = 0;
    while (x0 < SCREEN_W && !refs[x0]) ++x0;
    if (x0 == SCREEN_W) { std::memset(row, 0, SCREEN_W * sizeof(u32)); return; }
    const u32 rb = refs[x0] - x0;   // the reference pixel 0 has, or would have
    u32 off = 0;
    for (u32 x = 0; x < SCREEN_W; ++x) off |= refs[x] ? refs[x] ^ (rb + x) : 0;
    const u32 b = (rb >> 16) & 7, idx = rb & 0xFFFF;
    const CapSeg& s0 = cap_seg_[b * 512 + (idx >> 7)];
    const CapSeg& s1 = cap_seg_[b * 512 + (((idx + 128) & 0xFFFF) >> 7)];
    if (!off && !mb_on && !(idx & 0xFF) && s0.gen && s0.gen == s1.gen && s0.base == s1.base && s0.width == 256 && s1.width == 256 &&
        s0.uniform == s1.uniform && s0.each == s1.each && ((idx - s0.base) & 0xFFFF) == line * 256 && (!ss.any || ss.ref.gpu_gen == s0.gen)) {
      const Renderer3D::FrameRef* const ref = ss.any ? &ss.ref : frame_of(s0.gen);
      const u16* const v = reinterpret_cast<const u16*>(vm.bank(Engine2D::cap_slot_bank(b))) + idx;
      if (!ref || cap_sum(v) != s0.sum || cap_sum(v + 128) != s1.sum) {   // captured too long ago, or written to since
        std::memset(row, 0, SCREEN_W * sizeof(u32));
        if (ref) split_cap_changed_ += SCREEN_W;
        return;
      }
      const u32* const cw = cap_words_.data() + b * 65536 + idx;
      const u32 uni = s0.uniform;
      u32 n = 0;
      if (vram && !s0.each) { for (u32 x = 0; x < SCREEN_W; ++x) row[x] = uni; n = SCREEN_W; }
      else if (vram) { for (u32 x = 0; x < SCREEN_W; ++x) { row[x] = cw[x]; n += cw[x] != 0; } }
      else if (!s0.each) { for (u32 x = 0; x < SCREEN_W; ++x) { const u32 w = refs[x] && (out[x] & 0x00FFFFFF) == kern::direct_colour(v[x]) ? uni : 0; row[x] = w; n += w != 0; } }
      else { for (u32 x = 0; x < SCREEN_W; ++x) { const u32 w = refs[x] && (out[x] & 0x00FFFFFF) == kern::direct_colour(v[x]) ? cw[x] : 0; row[x] = w; n += w != 0; } }
      if (!vram && stat_on_) for (u32 x = 0; x < SCREEN_W; ++x) split_cap_effect_ += refs[x] && (s0.each ? cw[x] : uni) && (out[x] & 0x00FFFFFF) != kern::direct_colour(v[x]);
      if (n && !ss.any) { ss.any = true; ss.live = false; ss.ref = *ref; }
      split_cap_shown_ += n;
      return;
    }
  }
  u32 memo = ~0u;
  bool memo_ok = false;
  for (u32 x = 0; x < SCREEN_W; ++x) {
    row[x] = 0;
    const u32 r = refs[x];
    if (!r) continue;
    const u32 b = (r >> 16) & 7, idx = r & 0xFFFF;
    const CapSeg& seg = cap_seg_[b * 512 + (idx >> 7)];
    if (!seg.gen) continue;
    u32 w = seg.each ? cap_words_[b * 65536 + idx] : seg.uniform;
    if (!w) continue;
    // Still the pixels the capture wrote, and this one shown as it is (no effect, nothing blended over it).
    const u16* const bank = reinterpret_cast<const u16*>(vm.bank(Engine2D::cap_slot_bank(b)));
    if (memo != b * 512 + (idx >> 7)) { memo = b * 512 + (idx >> 7); memo_ok = cap_sum(bank + (idx & ~127u)) == seg.sum; }
    if (!memo_ok) { ++changed; continue; }
    if (!vram && (out[x] & 0x00FFFFFF) != kern::direct_colour(bank[idx])) { ++split_cap_effect_; continue; }   // on top, but not shown as it is: a colour effect
    if (ss.any && ss.ref.gpu_gen != seg.gen) { ++other; continue; }   // one 3D frame a screen
    const u32 rel = (idx - seg.base) & 0xFFFF, cx = rel & (seg.width - 1u), cy = rel >> (seg.width == 256 ? 8 : 7);
    if (cy >= SCREEN_H) continue;
    if (!ss.any) {
      const Renderer3D::FrameRef* const ref = frame_of(seg.gen);
      if (!ref) continue;
      ss.any = true; ss.live = false; ss.ref = *ref;
    }
    if (split3d::op_of(w) == split3d::PLAIN && mb_on) w = split3d::pack(0, split3d::FIXED, 16, 0) | split3d::Q15;   // PLAIN skips master brightness
    if (cx != x || cy != line) { w |= split3d::XY; xy[x] = cx | (cy << 8); ss.coords = true; ++moved; }
    row[x] = w;
    ++shown;
  }
  split_cap_shown_ += shown; split_cap_moved_ += moved; split_cap_changed_ += changed; split_cap_other_ += other;
}

bool Gpu::split3d_screen(int screen, Split3DScreen* f) const {
  const SplitScreen& ss = split_done_.s[screen & 1];
  if (!ss.any) return false;
  f->words = fp().split_fb[screen & 1].data(); f->coords = ss.coords ? fp().split_xy[screen & 1].data() : nullptr; f->lines = fp().split_mb[screen & 1];
  f->gen = ss.ref.gpu_gen; f->live = ss.live; f->scrolled = ss.scrolled;
  return true;
}

void Gpu::merge_split3d(int screen, u32* dst) {
  screen &= 1;
  const SplitScreen& f = split_done_.s[screen];
  const u32* const fb = fp().fb[screen].data();
  if (dst != fb) std::memcpy(dst, fb, SCREEN_W * SCREEN_H * sizeof(u32));
  if (!f.any) return;
  Renderer3D::FrameRef r = f.ref;
  r.allow_lag = false; r.out = nullptr;   // that frame's own record, as the present samples it
  nds_.gpu3d.renderer().sync_line(r, 0);
  alignas(16) Pixel merged[SCREEN_W];
  alignas(16) u32 shown[SCREEN_W];
  for (u32 y = 0; y < SCREEN_H; ++y) {
    const u32* const w = fp().split_fb[screen].data() + y * SCREEN_W;
    const u32* const xy = fp().split_xy[screen].data() + y * SCREEN_W;
    const u32* const t = r.line(y);
    const s32 xofs = static_cast<s32>(fp().split_mb[screen][y] << 3) >> 23;   // BG0's X offset on this line (bits 20-28, signed)
    u64 on[4] = {};
    for (u32 x = 0; x < SCREEN_W; ++x) {
      merged[x] = 0;
      if (split3d::op_of(w[x]) == split3d::HIDDEN) continue;
      const u32 tx = static_cast<u32>(static_cast<s32>(x) + xofs);
      const u32 p = (w[x] & split3d::XY) ? r.line(std::min<u32>(xy[x] >> 8, SCREEN_H - 1))[xy[x] & 255] : tx < SCREEN_W ? t[tx] : 0;
      if (!((p >> 24) & 0x1F)) continue;
      merged[x] = split3d::merge_pixel(w[x], 0, p);
      on[x >> 6] |= u64{1} << (x & 63);
    }
    if (!(on[0] | on[1] | on[2] | on[3])) continue;
    kern::active::output_line(merged, fp().split_reg[screen][y], shown);   // as the output stage shows a composed line
    u32* const o = dst + y * SCREEN_W;
    for (u32 x = 0; x < SCREEN_W; ++x) if ((on[x >> 6] >> (x & 63)) & 1) o[x] = shown[x];
  }
}

bool Gpu::merge_split3d_scaled(int screen, u32 n, u32* dst) {
  screen &= 1;
  const SplitScreen& f = split_done_.s[screen];
  const u32* const fb = fp().fb[screen].data();
  const u32 W = SCREEN_W * n;
  const u32* fine = nullptr;
  if (f.any) {
    Renderer3D::FrameRef r = f.ref;
    r.allow_lag = false; r.out = nullptr;   // that frame's own record, as merge_split3d
    nds_.gpu3d.renderer().sync_line(r, 0);
    u32 sc = 0;
    fine = nds_.gpu3d.renderer().gpu_fine(r, &sc);
    if (!fine || sc != n) return false;
  }
  for (u32 y = 0; y < SCREEN_H * n; ++y)
    for (u32 x = 0; x < W; ++x) dst[y * W + x] = fb[(y / n) * SCREEN_W + x / n];
  if (!f.any) return true;
  alignas(16) Pixel merged[SCREEN_W];
  alignas(16) u32 shown[SCREEN_W];
  for (u32 y = 0; y < SCREEN_H; ++y) {
    const u32* const w = fp().split_fb[screen].data() + y * SCREEN_W;
    const u32* const xy = fp().split_xy[screen].data() + y * SCREEN_W;
    const s32 xofs = static_cast<s32>(fp().split_mb[screen][y] << 3) >> 23;   // BG0's X offset on this line, in DS pixels
    for (u32 sy = 0; sy < n; ++sy)
      for (u32 sx = 0; sx < n; ++sx) {
        u64 on[4] = {};
        for (u32 x = 0; x < SCREEN_W; ++x) {
          merged[x] = 0;
          if (split3d::op_of(w[x]) == split3d::HIDDEN) continue;
          u32 p;
          if (w[x] & split3d::XY) p = fine[(std::min<u32>(xy[x] >> 8, SCREEN_H - 1) * n + sy) * W + (xy[x] & 255) * n + sx];
          else {
            const u32 tx = static_cast<u32>(static_cast<s32>(x) + xofs);
            p = tx < SCREEN_W ? fine[(y * n + sy) * W + tx * n + sx] : 0;
          }
          if (!((p >> 24) & 0x1F)) continue;
          merged[x] = split3d::merge_pixel(w[x], 0, p);
          on[x >> 6] |= u64{1} << (x & 63);
        }
        if (!(on[0] | on[1] | on[2] | on[3])) continue;
        kern::active::output_line(merged, fp().split_reg[screen][y], shown);
        u32* const o = dst + (y * n + sy) * W + sx;
        for (u32 x = 0; x < SCREEN_W; ++x) if ((on[x >> 6] >> (x & 63)) & 1) o[x * n] = shown[x];
      }
  }
  return true;
}

void Gpu::resolve_split3d() {
  static const bool stat = std::getenv("DS_SPLIT3D") != nullptr;
  for (int s = 0; s < 2; ++s) {
    SplitScreen& ss = split_done_.s[s];
    if (!ss.any) continue;
    if (ss.live) { merge_split3d(s, fp().fb[s].data()); ++split_resolved_; }
    else if (stat) {   // a capture shown again: at 1x the merge must give the framebuffer's own pixels
      std::vector<u32> m(SCREEN_W * SCREEN_H);
      merge_split3d(s, m.data());
      for (u32 i = 0; i < SCREEN_W * SCREEN_H; ++i) {
        if (!fp().split_fb[s][i]) continue;
        ++split_cap_checked_;
        if (m[i] != fp().fb[s][i] && split_cap_bad_++ < 16)
          std::fprintf(stderr, "split3d: frame %llu screen %d pixel %u,%u: the capture's merge gives %06x, the screen shows %06x (word %08x)\n", static_cast<unsigned long long>(nds_.frame_count), s, i % SCREEN_W, i / SCREEN_W, m[i] & 0xFFFFFF, fp().fb[s][i] & 0xFFFFFF, fp().split_fb[s][i]);
      }
    }
    ss.any = false;
  }
}

template <class S> void Gpu::sync_state(S& s) {
  s.begin("GPU ");
  s.fields(line_, hblank_done_, frame_begun_, screens_on_, master_bright_g_, capcnt_, capture_on_, fifo_, fifo_rd_, fifo_wr_, fifo_line_, run_fifo_);
  s.fields(fp().fb);   // what the display shows until the next frame (and the thumbnail)
  s.end();
  engine[0].sync_state(s);
  engine[1].sync_state(s);
  if constexpr (S::reading) {
    nds_.sched.rebind(EventId::HBlank, ev_hblank);
    nds_.sched.rebind(EventId::VBlank_Scanline, ev_scanline);
    nds_.sched.rebind(EventId::DisplayFifo, ev_fifo);
  }
}
template void Gpu::sync_state<state::Writer>(state::Writer&);
template void Gpu::sync_state<state::Reader>(state::Reader&);

void Gpu::after_load() {
  // State was taken right after line 0's begin_frame(); its decisions depend
  // only on restored registers/DMA state, so retaking them re-arms the trap.
  if (frame_begun_ && line_ == 0 && !hblank_done_) begin_frame();
}

} // namespace ds::gpu
