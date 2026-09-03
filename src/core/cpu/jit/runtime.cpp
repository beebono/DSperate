// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Recompiler runtime, host-agnostic: code arena, the block cache, block
// linking, park-and-revive, the pre-translation worker, and self-modifying
// code tracking by host page. Everything that emits or patches host code is
// behind the `backend` interface in jit_internal.h (a64/, a32/): the stubs,
// the translator, the killed-block entry redirect and the link patch.
#include "core/cpu/jit/jit_internal.h"
#include "core/profile.h"
#include "core/sched/scheduler.h"
#include "core/cpu/cpu_cycles.h"
#include "core/cpu/interp/interp.h"
#include "core/cpu/interp/interp_internal.h"
#include "core/nds.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <string>
#include <cstring>
#include <unistd.h>
#include <cstdlib>
#include <sys/mman.h>
#include <algorithm>
#include <vector>

namespace ds::jit {

namespace {
// 32-bit hosts: the A32 backend reaches the stubs with `b`/`bl`, +-32 MB.
constexpr size_t ARENA_BYTES = sizeof(void*) >= 8 ? (64u << 20) : (32u << 20);
constexpr size_t BLOCK_MARGIN = 64u << 10;    // a block may emit up to this much

Runtime g_rt;
} // namespace
static void invalidate_blocks(const u8* host_page, std::vector<Block*> victims);
namespace {

// DS_JIT_CHURN=1: who invalidates what, and what gets retranslated. Printed at exit.
namespace churn {
struct Key { u32 pc; u8 dma, cpu; bool operator<(const Key& o) const { return pc != o.pc ? pc < o.pc : dma != o.dma ? dma < o.dma : cpu < o.cpu; } };
static std::map<Key, u64> writers;             // writer pc -> invalidations
static std::map<u32, u64> victims_by_page;     // guest page of a killed block -> kills
static std::map<u32, u64> retrans;             // guest pc -> translations
static std::map<u64, u64> trans_by_frame;      // frame -> translations (first-time + re)
static std::map<u64, u64> retrans_by_frame;    // frame -> retranslations only
static u64 inval = 0, killed = 0, trans = 0, frames_seen = 0, range_miss = 0, resets = 0;
static u64 retime_calls = 0, retime_killed = 0;   // ARM9 timing-table rebuilds and the blocks they killed
// Lead-time census: for each retranslation, guest time between the page's
// last invalidating write and the translate -- the window a pre-translator
// seeded at write time would have had. Buckets in ARM9 cycles
// (~1.12 M / frame): <0.1 ms, 0.1-1, 1-4, 4-16 (about a frame), 16-64, >64.
static std::map<u32, u64> page_last_write;   // guest 2 KB page -> sched.now() of last invalidation
static u64 lead_hist[6];
static u64 lead_n = 0, lead_sum = 0;
static bool on() { static const bool e = std::getenv("DS_JIT_CHURN") != nullptr; return e; }
static void report() {
  auto top = [](auto& m, const char* what, int n, auto print) {
    using K = std::remove_const_t<std::remove_reference_t<decltype(m.begin()->first)>>;
    std::vector<std::pair<u64, K>> v; for (auto& kv : m) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[churn] top %s (%zu distinct):\n", what, m.size());
    for (int i = 0; i < n && i < (int)v.size(); ++i) print(v[i].first, v[i].second);
  };
  auto pk = [](u64 n, const Key& k) { std::fprintf(stderr, "   %10llu  pc %08x %s arm%d\n", (unsigned long long)n, k.pc, k.dma ? "DMA" : "cpu", k.cpu); };
  auto pa = [](u64 n, u32 a) { std::fprintf(stderr, "   %10llu  %08x\n", (unsigned long long)n, a); };
  std::fprintf(stderr, "[churn] frames %llu arena resets %llu invalidations %llu blocks killed %llu translations %llu | code-page stores: silent %llu changed %llu, changed-but-no-block %llu\n",
               (unsigned long long)frames_seen, (unsigned long long)resets, (unsigned long long)inval, (unsigned long long)killed, (unsigned long long)trans,
               (unsigned long long)mem::code_store_stats.silent, (unsigned long long)mem::code_store_stats.changed, (unsigned long long)range_miss);
  std::fprintf(stderr, "[churn] retimes %llu (%.2f/frame) blocks killed by retimes %llu (%.1f/frame)\n",
               (unsigned long long)retime_calls, frames_seen ? static_cast<double>(retime_calls) / static_cast<double>(frames_seen) : 0.0,
               (unsigned long long)retime_killed, frames_seen ? static_cast<double>(retime_killed) / static_cast<double>(frames_seen) : 0.0);
  top(writers, "writers (pc of the store / DMA start)", 15, pk);
  top(victims_by_page, "invalidated guest pages (2 KB)", 15, pa);
  top(retrans, "retranslated block pcs", 20, pa);
  // Which frames the translation work lands in: a flat row is warm-up, a
  // spike is an overlay reload -- the frames to hold against the p99 table.
  auto pf = [](u64 n, u64 f) { std::fprintf(stderr, "   %10llu  frame %llu (%llu re)\n",
      (unsigned long long)n, (unsigned long long)f, (unsigned long long)retrans_by_frame[f]); };
  top(trans_by_frame, "translation frames", 20, pf);
  if (lead_n) {
    static const char* const lb[6] = {"<0.1ms", "0.1-1ms", "1-4ms", "4-16ms", "16-64ms", ">64ms"};
    std::fprintf(stderr, "[churn] retranslate lead time (page's last kill -> translate, upper bound), %llu samples, mean %.1f ms:\n",
                 (unsigned long long)lead_n, static_cast<double>(lead_sum) / static_cast<double>(lead_n) / 1120380.0 * 16.7);
    for (int i = 0; i < 6; ++i) std::fprintf(stderr, "   %-8s %10llu\n", lb[i], (unsigned long long)lead_hist[i]);
  }
}
}  // namespace churn

// ---- pre-translation worker (DS_JIT_PRETX=1) ---------------------------------------------
//
// Overlay loads hand the JIT ~1000 never-seen blocks in one frame (the mlbis
// p99 tail); translating them on the emulation thread is a 6-10 ms stall. The
// worker translates *statically reachable* code ahead of execution: every
// translated block's direct branch targets (Block::succ) are queued, the
// worker compiles them into the shared arena, and the emulation thread adopts
// the finished blocks on its next lookup miss. No guest state is speculated,
// so the guest cannot observe when a block was translated: exact by
// construction (gate: DS_FRAME_HASH).
//
// Concurrency model: ONE mutex (mu) covers the arena frontier (r.pos), the
// queues and the generation counter; the worker holds it for a whole
// translation (~10 us), the emulation thread only takes it on a lookup miss,
// to seed successors, or in reset/invalidate. The hot dispatch paths never
// touch it. Guest memory is written by the emulation thread only, so a block
// the worker built from bytes that then changed is caught at adoption by
// comparing the code bytes it read against what the guest holds now; timing-
// table rebuilds and arena resets bump `gen`, which orphans everything the
// worker built before them.
namespace pretx {
static bool on() { static const bool e = std::getenv("DS_JIT_PRETX") != nullptr && std::getenv("DS_JIT_DENSITY") == nullptr; return e; }
struct Job  { JitCpu* jc; u32 key; };
struct Done { JitCpu* jc; Block* b; u64 gen; u64 stamp; std::vector<u8> guest; };
// Deliberately leaked: the worker is detached and may be blocked on cv/mu when
// the process exits; running these destructors then hangs pthread_cond_destroy
// (measured: both threads futex-waiting inside pretx::cv after "ran N frames").
static std::mutex& mu = *new std::mutex;
static std::condition_variable& cv = *new std::condition_variable;
static std::deque<Job>& jobs = *new std::deque<Job>;
static std::vector<Done>& done = *new std::vector<Done>;
// Finished blocks wait here, INVISIBLE to the runtime, until the emulation
// thread actually misses on their key. Publishing them any earlier would tag
// their guest pages as code pages ahead of the baseline schedule, and the
// invalidation alerts from stores into those pages end slices at points the
// baseline would have run through -- a guest-visible interleave change
// (measured: mlbis frame 889 r12/r15 wobble, invalidations 6453 -> 6946).
// Installing exactly at the miss reproduces the baseline timeline.
static std::map<u64, Done>& staged = *new std::map<u64, Done>;         // (cpu << 32) | key -> finished block
static std::unordered_set<u64>& seen = *new std::unordered_set<u64>;   // (cpu << 32) | key: ever queued or translated by the main thread
static u64 gen = 0;
// True while the worker is emitting into its chunk without mu held. purge()
// waits it out after bumping gen: an arena reset reclaims the chunk's space,
// and the frontier must not be reused while the worker still writes there.
static std::atomic<bool> in_flight{false};
static u64 st_built = 0, st_adopted = 0, st_dropped = 0, st_skipped = 0;   // worker built / main adopted / stale-or-changed / not attempted
static void start();
static void seed(JitCpu& jc, const Block& b);
static Block* adopt(JitCpu& jc, u32 key);
// Takes mu when the worker exists, so the arena frontier and emitted-but-
// unpublished code cannot race it. A no-op (and no atomics) when off.
struct ArenaLock {
  bool locked;
  ArenaLock() : locked(on()) { if (locked) mu.lock(); }
  ~ArenaLock() { if (locked) mu.unlock(); }
};
// Everything the worker built or was about to build is orphaned. Call with
// mu NOT held (takes it). Used by arena resets and whole-CPU invalidations;
// per-block invalidations don't need it -- the byte compare at adoption
// rejects those.
static void purge() {
  if (!on()) return;
  std::lock_guard<std::mutex> lk(mu);
  ++gen;
  jobs.clear();
  for (Done& d : done) { delete d.b; ++st_dropped; }
  done.clear();
  for (auto& kv : staged) { delete kv.second.b; ++st_dropped; }
  staged.clear();
  seen.clear();
  while (in_flight.load(std::memory_order_acquire)) std::this_thread::yield();
}
} // namespace pretx

// ---- code page tracking ----------------------------------------------------------------

const u8* host_page_of(const u8* p) { return reinterpret_cast<const u8*>(reinterpret_cast<u64>(p) & ~u64{mem::PAGE_SIZE - 1}); }

bool code_query(const u8* host_page) { return g_rt.code_pages.count(host_page) != 0; }

void set_code_tag(const u8* host_page, bool on) {
  for (JitCpu& jc : g_rt.cpus)
    if (jc.ctx) jc.ctx->page_table.set_code_host(host_page, on);
}

void code_write_hook(u8* host, u32 len) {
  const u8* first = host_page_of(host);
  const u8* last = host_page_of(host + len - 1);
  invalidate_host_range(first, host, host + len - 1);
  if (last != first) invalidate_host_range(last, host, host + len - 1);
}

constexpr size_t PARKED_PER_KEY = 4;

void kill_block(JitCpu& jc, Block* b) {
  if (b->dead) return;
  b->dead = true;
  // Redirect the entry: anything linked to it lands in the dispatcher, which
  // misses (the LUT/map entries go below) and either revives a parked
  // translation whose guest bytes still match or retranslates.
  std::memcpy(b->entry_words, b->entry, backend::ENTRY_PATCH);
  {
    std::vector<Block*>& v = jc.parked[b->key];
    if (v.size() >= PARKED_PER_KEY) v.erase(v.begin());   // oldest out; it stays dead in the arena until the reset
    v.push_back(b);
  }
  backend::write_entry_redirect(b->entry, b->key, jc.dispatch);
  sync_icache(b->entry, backend::ENTRY_PATCH);
  const u32 idx = (b->key >> 1) & (LUT_SIZE - 1);
  if (static_cast<u32>(jc.lut[idx]) == b->key) jc.lut[idx] = LUT_EMPTY_KEY;
  jc.blocks.erase(b->key);
  g_rt.stats.blocks_invalidated++;
}

void remove_from_page_lists(Block* b) {
  for (u32 i = 0; i < b->npages; ++i) {
    auto it = g_rt.code_pages.find(b->host_pages[i]);
    if (it == g_rt.code_pages.end()) continue;
    auto& v = it->second;
    for (size_t k = 0; k < v.size(); ++k) if (v[k] == b) { v[k] = v.back(); v.pop_back(); break; }
    if (v.empty()) { g_rt.code_pages.erase(it); set_code_tag(b->host_pages[i], false); }
  }
}

JitCpu& jc_of(Block* b) { return g_rt.cpus[b->owner]; }

void reset_arena() {
  pretx::purge();   // the worker is idle and empty after this; the frontier below is ours
  if (churn::on()) ++churn::resets;
  Runtime& r = g_rt;
  for (JitCpu& jc : r.cpus) {
    for (Block* b : jc.all_blocks) if (!b->pooled) delete b;   // pooled ones go with block_pool below
    jc.all_blocks.clear();
    jc.blocks.clear();
    jc.parked.clear();
    if (jc.lut) for (u32 i = 0; i < LUT_SIZE; ++i) jc.lut[i] = LUT_EMPTY_KEY;
  }
  for (auto& kv : r.code_pages) set_code_tag(kv.first, false);
  r.code_pages.clear();
  r.block_pool.clear();
  r.pos = r.stubs_end;
  r.need_reset = false;
  r.stats.flushes++;
}

// The ARM9 timing table was rebuilt (PU / TCM / EXMEMCNT write). A block bakes
// only the code-fetch byte of its own pages and its static branch target's,
// and the N32 byte of its pc-relative literals' pages (Block::dep_*); every
// other cost is read from the live table. So only the blocks whose recorded
// pages had that byte change are killed -- the rest are still the translation
// the new table would produce. Parked translations and the pre-translation
// worker's output keep the old rule: dropped whole (they carry no
// dependency check on revival beyond the region stamp, which a PU write does
// not bump).
void on_timing_changed(CpuContext& cpu) {
  if (!cpu.jit) return;
  JitCpu& jc = *static_cast<JitCpu*>(cpu.jit);
  mem::Timing& t = cpu.nds->bus.timing();
  if (churn::on()) { static bool reg = (std::atexit(churn::report), true); (void)reg; }
  u64 killed = 0;
  if (g_rt.retime_all) {   // DS_JIT_RETIME_ALL: the old rule, for the census
    for (Block* b : jc.all_blocks) if (!b->dead) ++killed;
    t.retime_clear();
    invalidate_cpu(jc);
  } else {
    pretx::purge();
    if (!t.retime_pages().empty()) {
      for (Block* b : jc.all_blocks) {
        if (b->dead) continue;
        bool hit = b->dep_overflow;
        for (u32 i = 0; i < b->ndep && !hit; ++i) hit = (t.retime_flag(b->dep_page[i]) & b->dep_kind[i]) != 0;
        if (!hit) continue;
        remove_from_page_lists(b);
        kill_block(jc, b);
        ++killed;
      }
    }
    t.retime_clear();
    jc.parked.clear();
    if (killed) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
  }
  prof::add(prof::C_JIT_INVALIDATE_CPU, 1);
  prof::add(prof::C_JIT_INVALIDATE_CPU_KILLED, killed);
  if (churn::on()) { churn::retime_calls++; churn::retime_killed += killed; churn::frames_seen = cpu.nds->frame_count; }
}

} // namespace

Runtime& rt() { return g_rt; }

// ---- cache -----------------------------------------------------------------------------

void lut_insert(JitCpu& jc, Block* b) {
  const u32 idx = (b->key >> 1) & (LUT_SIZE - 1);
  jc.lut[idx] = (static_cast<u64>(b->entry - g_rt.arena) << 32) | b->key;
}

// DS_PERF_MAP=1: name each translated block for `perf` in /tmp/perf-<pid>.map,
// so profiles attribute samples to the guest address a block came from
// instead of one anonymous mapping. Blocks are named jit9_<pc>/jit7_<pc>
// (with a `t` suffix for Thumb); the arena is reused after a flush, so the
// same address can appear more than once and perf takes the last entry.
static FILE* perf_map_file() {
  static FILE* map = [] () -> FILE* {
    if (!std::getenv("DS_PERF_MAP")) return nullptr;
    char path[64];
    std::snprintf(path, sizeof path, "/tmp/perf-%d.map", static_cast<int>(getpid()));
    FILE* f = std::fopen(path, "w");
    if (f) std::setvbuf(f, nullptr, _IOLBF, 0);
    return f;
  }();
  return map;
}

static void perf_map_add(const Block* b, bool arm9) {
  FILE* map = perf_map_file();
  if (!map) return;
  std::fprintf(map, "%llx %x jit%c_%08x%s\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(b->entry)),
               b->size, arm9 ? '9' : '7', key_pc(b->key), key_thumb(b->key) ? "t" : "");
}

// The permanent stubs, named jit_stub_<name> (per-CPU ones jit_stub9_/jit_stub7_);
// each runs to the next stub's start.
static void perf_map_stubs(const Runtime& r) {
  FILE* map = perf_map_file();
  if (!map) return;
  std::vector<std::pair<const u8*, std::string>> v;
  auto add = [&](const void* p, const char* name) { if (p) v.emplace_back(static_cast<const u8*>(p), name); };
  add(reinterpret_cast<const void*>(r.enter), "jit_stub_enter"); add(r.enter_light, "jit_stub_enter_light");
  add(reinterpret_cast<const void*>(r.run_loop), "jit_stub_run_loop");
  add(r.exit_key, "jit_stub_exit_key"); add(r.exit_key_lit, "jit_stub_exit_key_lit"); add(r.exit_r15, "jit_stub_exit_r15");
  add(r.call_pure, "jit_stub_call_pure"); add(r.call_full, "jit_stub_call_full"); add(r.call2, "jit_stub_call2");
  add(r.poll, "jit_stub_poll"); add(r.flush_exit, "jit_stub_flush_exit");
  static const char* const sz[3] = {"8", "16", "32"};
  for (int i = 0; i < 3; ++i) { add(r.slow_load[i], (std::string("jit_stub_slow_load") + sz[i]).c_str()); add(r.slow_store[i], (std::string("jit_stub_slow_store") + sz[i]).c_str()); }
  add(r.merge_keep_cv, "jit_stub_merge_keep_cv"); add(r.merge_set_c, "jit_stub_merge_set_c");
  for (int c = 0; c < 2; ++c) {
    const JitCpu& jc = r.cpus[c];
    const std::string pre = c == 0 ? "jit_stub9_" : "jit_stub7_";
    add(jc.dispatch, (pre + "dispatch").c_str()); add(jc.link, (pre + "link").c_str()); add(jc.fallback, (pre + "fallback").c_str());
    add(jc.branch_indirect, (pre + "branch_indirect").c_str()); add(jc.branch_indirect_cdi, (pre + "branch_indirect_cdi").c_str());
  }
  std::sort(v.begin(), v.end());
  for (size_t i = 0; i < v.size(); ++i) {
    const u8* end = i + 1 < v.size() ? v[i + 1].first : r.arena + r.stubs_end;
    std::fprintf(map, "%llx %llx %s\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v[i].first)),
                 static_cast<unsigned long long>(end - v[i].first), v[i].second.c_str());
  }
}

// Publish a finished block: page registration, maps, LUT, stats. Emulation
// thread only -- these structures are unsynchronised, which is why the
// pre-translation worker hands its blocks here instead of doing this itself.
// Guest bytes [pc, pc + len) through the current mapping; an unmapped word
// reads as zero (a block never spans unmapped space: translation stops there).
static void copy_guest_bytes(JitCpu& jc, u32 pc, u8* dst, u32 len) {
  u32 done = 0;
  while (done < len) {
    const u32 a = pc + done;
    const u32 room = std::min<u32>(len - done, mem::PAGE_SIZE - (a & (mem::PAGE_SIZE - 1)));
    if (const u8* src = jc.ctx->page_table.read_ptr(a)) std::memcpy(dst + done, src, room);
    else std::memset(dst + done, 0, room);
    done += room;
  }
}
static bool guest_bytes_match(JitCpu& jc, const Block* b) {
  u8 cur[GUEST_COPY_MAX];
  copy_guest_bytes(jc, key_pc(b->key), cur, b->guest_copy_len);
  return std::memcmp(cur, b->guest_copy, b->guest_copy_len) == 0;
}

// Make `b` live: register the host pages its guest code sits on (through the
// current mapping), and enter it in the map and the LUT.
static void register_block(JitCpu& jc, Block* b) {
  Runtime& r = g_rt;
  const u32 pc = key_pc(b->key);
  const u8* p0 = jc.ctx->page_table.read_ptr(pc);
  const u8* p1 = jc.ctx->page_table.read_ptr(pc + b->guest_len - 1);
  b->npages = 0;
  b->host_lo = p0; b->host_hi = p1;
  if (p0) b->host_pages[b->npages++] = host_page_of(p0);
  if (p1 && host_page_of(p1) != (p0 ? host_page_of(p0) : nullptr)) b->host_pages[b->npages++] = host_page_of(p1);
  for (u32 i = 0; i < b->npages; ++i) {
    auto& v = r.code_pages[b->host_pages[i]];
    if (v.empty()) set_code_tag(b->host_pages[i], true);
    v.push_back(b);
  }
  jc.blocks[b->key] = b;
  lut_insert(jc, b);
}

// A killed translation of `key` whose guest bytes match again (a game that
// toggles a word between two values, the usual "patch the first instruction
// to enable a routine" idiom) is brought back: entry words restored, pages,
// map and LUT re-registered. Nothing about the translation itself changes,
// and nobody can be inside it -- a kill redirects its entry and raises the
// alert that leaves every running block at its next poll, and lookups happen
// between blocks. Only a same-stamp block may come back: translation bakes
// the timing tables in.
static Block* revive(JitCpu& jc, u32 key) {
  auto it = jc.parked.find(key);
  if (it == jc.parked.end()) return nullptr;
  std::vector<Block*>& v = it->second;
  const u64 stamp = jc.nds->bus.timing().stamp.load(std::memory_order_acquire);
  for (size_t k = v.size(); k-- > 0;) {
    Block* b = v[k];
    if (b->stamp != stamp || b->guest_len != b->guest_copy_len || !guest_bytes_match(jc, b)) continue;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(k));
    if (v.empty()) jc.parked.erase(it);
    std::memcpy(b->entry, b->entry_words, backend::ENTRY_PATCH);
    sync_icache(b->entry, backend::ENTRY_PATCH);
    b->dead = false;
    register_block(jc, b);
    g_rt.stats.blocks_revived++;
    return b;
  }
  return nullptr;
}

static void install(JitCpu& jc, Block* b) {
  Runtime& r = g_rt;
  if (r.debug) {   // DS_JIT_DEBUG: dump the block for `objdump -D -b binary (-m aarch64 / -m arm)`
    std::fprintf(stderr, "[jit] block %08x (%u bytes):", b->key, b->size);
    for (u32 i = 0; i < b->size; i += 4) { u32 w; std::memcpy(&w, b->entry + i, 4); std::fprintf(stderr, " %08x", w); }
    std::fputc('\n', stderr);
  }
  perf_map_add(b, jc.arm9);
  // What a revival will be checked against: the guest bytes as they are now
  // (page-aware, the block may straddle two host pages) and the timing stamp.
  b->stamp = jc.nds->bus.timing().stamp.load(std::memory_order_acquire);
  b->guest_copy_len = std::min<u32>(b->guest_len, GUEST_COPY_MAX);
  copy_guest_bytes(jc, key_pc(b->key), b->guest_copy, b->guest_copy_len);
  register_block(jc, b);
  jc.all_blocks.push_back(b);
  r.stats.blocks_translated++;
  r.stats.code_bytes += b->size;
  r.stats.hot_bytes += b->hot_size;
}

Block* translate(JitCpu& jc, u32 key) {
  DS_PROF(JIT_TX);   // nested inside the CPU9/CPU7 slice: an "of which" column
  Runtime& r = g_rt;
  if (churn::on()) {
    churn::trans++;
    const u64 f = jc.ctx->nds->frame_count;
    churn::trans_by_frame[f]++;
    if (churn::retrans[key_pc(key)]++ > 0) churn::retrans_by_frame[f]++;
    // Upper bound on pre-translation lead time: writes after the page's last
    // block died are invisible, so the true window is at most this.
    const auto pw = churn::page_last_write.find(key_pc(key) & ~(mem::PAGE_SIZE - 1));
    if (pw != churn::page_last_write.end()) {
      const u64 dt = jc.ctx->nds->sched.now() - pw->second;
      const u64 ms01 = 112038;    // ~0.1 ms of ARM9 cycles
      const int bucket = dt < ms01 ? 0 : dt < ms01 * 10 ? 1 : dt < ms01 * 40 ? 2 : dt < ms01 * 160 ? 3 : dt < ms01 * 640 ? 4 : 5;
      churn::lead_hist[bucket]++;
      churn::lead_n++; churn::lead_sum += dt;
    }
  }
  Block* b;
  {
    pretx::ArenaLock lk;
    if (r.pos + BLOCK_MARGIN > r.cap) return nullptr;
    b = &r.block_pool.emplace_back(Block{});
    b->key = key;
    b->owner = jc.arm9 ? 0 : 1;
    b->pooled = true;
    u32 size = 0;
    if (!backend::translate_block(jc, key, r.arena + r.pos, BLOCK_MARGIN, *b, size)) { r.block_pool.pop_back(); return nullptr; }
    b->entry = r.arena + r.pos;
    b->size = size;
    r.pos += (b->size + 15) & ~size_t{15};
  }
  sync_icache(b->entry, b->size);
  install(jc, b);
  pretx::seed(jc, *b);
  return b;
}

const u8* find_native(JitCpu& jc, u32 key) {
  auto it = jc.blocks.find(key);
  if (it != jc.blocks.end()) { lut_insert(jc, it->second); return it->second->entry; }
  if (Block* b = revive(jc, key)) return b->entry;
  if (Block* b = pretx::adopt(jc, key)) return b->entry;
  Block* b = translate(jc, key);
  return b ? b->entry : nullptr;
}

// ---- pre-translation worker body (state and contract above, by churn) ------------------------
namespace {
namespace pretx {

static void seed(JitCpu& jc, const Block& b) {
  if (!on() || !b.nsucc) return;
  std::lock_guard<std::mutex> lk(mu);
  bool queued = false;
  for (u8 i = 0; i < b.nsucc; ++i) {
    const u32 k = b.succ[i];
    if (jc.blocks.count(k)) continue;                       // emulation thread owns this map
    const u64 tag = (static_cast<u64>(jc.arm9 ? 0 : 1) << 32) | k;
    if (!seen.insert(tag).second) continue;
    jobs.push_back(Job{&jc, k});
    queued = true;
  }
  if (queued) cv.notify_one();
}

static void worker() {
  Runtime& r = g_rt;
  // The worker emits into its own arena chunk, reserved in one bump of r.pos,
  // so translate_block runs without mu held: the emulation thread's own
  // translations only ever contend with the (rare, microsecond) chunk
  // reserve, not with every worker block. A purge orphans the chunk; the
  // space comes back with the next arena reset.
  constexpr size_t CHUNK = 1u << 20;
  u8* chunk = nullptr;
  size_t used = 0, cap = 0;
  u64 chunk_gen = ~u64{0};
  for (;;) {
    Job j;
    u64 g, ts;
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [] { return !jobs.empty(); });
      j = jobs.front();
      jobs.pop_front();
      g = gen;
      ts = j.jc->nds->bus.timing().stamp.load(std::memory_order_acquire);   // before the table reads; see Timing::stamp
      if (g != chunk_gen || cap - used < BLOCK_MARGIN) {
        if (r.pos + CHUNK > r.cap) { ++st_skipped; continue; }   // full: the main thread will reset
        chunk = r.arena + r.pos;
        r.pos += CHUNK;
        used = 0; cap = CHUNK; chunk_gen = g;
      }
      in_flight.store(true, std::memory_order_release);
    }
    JitCpu& jc = *j.jc;
    // Copy the guest bytes BEFORE translating: the emulation thread may write
    // them at any point, and adoption compares the guest against this copy --
    // a block whose source moved under it never gets adopted. The copy stops
    // at the 2 KB page edge (one read_ptr mapping); a block that would cross
    // it is not pre-translated (rare -- blocks average a few instructions).
    const u32 pc = key_pc(j.key);
    const u8* src = jc.ctx->page_table.read_ptr(pc);
    if (!src) { in_flight.store(false, std::memory_order_release); ++st_skipped; continue; }
    u8 copy[1024];
    const u32 avail = std::min<u32>(sizeof copy, mem::PAGE_SIZE - (pc & (mem::PAGE_SIZE - 1)));
    std::memcpy(copy, src, avail);
    Block* b = new Block{};
    b->key = j.key;
    b->owner = jc.arm9 ? 0 : 1;
    u32 size = 0;
    if (!backend::translate_block(jc, j.key, chunk + used, BLOCK_MARGIN, *b, size) || b->guest_len > avail) {
      in_flight.store(false, std::memory_order_release);
      delete b; ++st_skipped; continue;
    }
    b->entry = chunk + used;
    b->size = size;
    used += (b->size + 15) & ~size_t{15};
    sync_icache(b->entry, b->size);     // dc cvau + ic ivau + dsb ish; the adopter issues the isb
    in_flight.store(false, std::memory_order_release);   // chunk writes done; cleared before mu (purge spins on it holding mu)
    {
      std::lock_guard<std::mutex> lk(mu);
      if (gen == g) {
        done.push_back(Done{&jc, b, g, ts, std::vector<u8>(copy, copy + b->guest_len)});
        ++st_built;
        // Chase this block's own successors so the frontier can run ahead of
        // execution instead of waiting for an adoption to seed it. `seen`
        // bounds the waste; a purge clears both.
        for (u8 i = 0; i < b->nsucc; ++i) {
          const u64 tag = (static_cast<u64>(jc.arm9 ? 0 : 1) << 32) | b->succ[i];
          if (seen.insert(tag).second) jobs.push_back(Job{&jc, b->succ[i]});
        }
      } else { delete b; ++st_dropped; }
    }
  }
}

// Emulation thread, on a lookup miss: hand over the worker's finished block
// for exactly this key, if it has one and it is still current. Everything
// else stays staged and invisible -- see the note at `staged`.
static Block* adopt(JitCpu& jc, u32 key) {
  if (!on()) return nullptr;
  Done d{};
  u64 cur;
  {
    std::lock_guard<std::mutex> lk(mu);
    for (Done& x : done) staged.emplace((static_cast<u64>(x.jc->arm9 ? 0 : 1) << 32) | x.b->key, std::move(x));
    done.clear();
    const auto it = staged.find((static_cast<u64>(jc.arm9 ? 0 : 1) << 32) | key);
    if (it == staged.end()) return nullptr;
    d = std::move(it->second);
    staged.erase(it);
    cur = gen;
  }
  const u8* src = jc.ctx->page_table.read_ptr(key_pc(key));
  if (d.gen != cur || d.stamp != jc.nds->bus.timing().stamp.load(std::memory_order_acquire) ||
      !src || std::memcmp(src, d.guest.data(), d.guest.size()) != 0) {
    delete d.b; ++st_dropped;
    return nullptr;
  }
  // DS_JIT_PRETX_VERIFY=1: re-translate the key fresh right now and diff it
  // against the staged block. Any mismatch means translation read state that
  // changed between build and adoption without failing the byte compare --
  // the exactness bug hunter.
  static const bool verify = std::getenv("DS_JIT_PRETX_VERIFY") != nullptr;
  if (verify) {
    Runtime& r = g_rt;
    std::lock_guard<std::mutex> lk(mu);
    if (r.pos + BLOCK_MARGIN <= r.cap) {
      Block tmp{};
      tmp.key = key; tmp.owner = jc.arm9 ? 0 : 1;
      u32 fsize = 0;   // frontier scratch; pos not advanced
      if (backend::translate_block(jc, key, r.arena + r.pos, BLOCK_MARGIN, tmp, fsize)) {
        if (tmp.guest_len != d.b->guest_len || fsize != d.b->size || tmp.hot_size != d.b->hot_size)
          std::fprintf(stderr, "[pretx] VERIFY shape mismatch key %08x: staged len/size/hot %u/%u/%u fresh %u/%u/%u frame %llu\n",
                       key, d.b->guest_len, d.b->size, d.b->hot_size, tmp.guest_len, fsize, tmp.hot_size,
                       (unsigned long long)jc.ctx->nds->frame_count);
        else {
          const u8* fresh = r.arena + r.pos;
          u32 diffs = 0;
          for (u32 i = 0; i < d.b->size; i += 4) {
            u32 a, f;
            std::memcpy(&a, d.b->entry + i, 4);
            std::memcpy(&f, fresh + i, 4);
            if (a == f) continue;
            // Relative branches to the fixed stubs legitimately differ with
            // the emission base; anything else is baked state that drifted.
            const u32 rel = backend::relative_branch_class(a);
            if (rel && rel == backend::relative_branch_class(f)) continue;
            if (++diffs <= 4)
              std::fprintf(stderr, "[pretx] VERIFY word mismatch key %08x +%u: staged %08x fresh %08x frame %llu\n",
                           key, i, a, f, (unsigned long long)jc.ctx->nds->frame_count);
          }
          if (diffs) {   // both blocks, for objdump -D -b binary (-m aarch64 / -m arm)
            std::fprintf(stderr, "[pretx] staged:");
            for (u32 i = 0; i < d.b->size && i < 256; i += 4) { u32 w; std::memcpy(&w, d.b->entry + i, 4); std::fprintf(stderr, " %08x", w); }
            std::fprintf(stderr, "\n[pretx] fresh: ");
            for (u32 i = 0; i < d.b->size && i < 256; i += 4) { u32 w; std::memcpy(&w, fresh + i, 4); std::fprintf(stderr, " %08x", w); }
            std::fputc('\n', stderr);
          }
        }
      }
    }
  }
  install(jc, d.b);
  asm volatile("isb" ::: "memory");   // this PE may fetch the adopted code next
  ++st_adopted;
  seed(jc, *d.b);
  return d.b;
}

static void start() {
  if (!on()) return;
  static const bool started = [] { std::thread(worker).detach(); return true; }();
  (void)started;
}

} // namespace pretx
} // namespace


// Does the block's code on `page` overlap the written bytes [lo, hi]? A block
// spans at most two pages, which need not be adjacent in host memory, so the
// test is per page: on its first page the code runs from host_lo to the page
// end (or host_hi if that is the only page), on its second from the page
// start to host_hi. An unmapped end (null) is treated as covering the page.
static bool block_touched(const Block* b, const u8* page, const u8* lo, const u8* hi) {
  if (!b->host_lo || !b->host_hi) return true;
  const u8* seg_lo; const u8* seg_hi;
  if (page == b->host_pages[0]) { seg_lo = b->host_lo; seg_hi = (b->npages == 1) ? b->host_hi : page + mem::PAGE_SIZE - 1; }
  else                          { seg_lo = page; seg_hi = b->host_hi; }
  return lo <= seg_hi && hi >= seg_lo;
}

// A store changed bytes [lo, hi] on `host_page` (both on that page): kill the
// blocks whose code they belong to, and only those -- data sharing a 2 KB page
// with code is the common case, not the exception (DraStic's third filter).
void invalidate_host_range(const u8* host_page, const u8* lo, const u8* hi) {
  auto it = g_rt.code_pages.find(host_page);
  if (it == g_rt.code_pages.end()) return;
  std::vector<Block*>& list = it->second;
  std::vector<Block*> victims;
  for (size_t k = 0; k < list.size();) {
    if (block_touched(list[k], host_page, lo, hi)) { victims.push_back(list[k]); list[k] = list.back(); list.pop_back(); }
    else ++k;
  }
  if (victims.empty()) { churn::range_miss++; return; }
  if (list.empty()) { g_rt.code_pages.erase(it); set_code_tag(host_page, false); }
  invalidate_blocks(host_page, std::move(victims));
}

void invalidate_host_page(const u8* host_page) {
  auto it = g_rt.code_pages.find(host_page);
  if (it == g_rt.code_pages.end()) return;
  std::vector<Block*> victims = std::move(it->second);
  g_rt.code_pages.erase(it);
  set_code_tag(host_page, false);
  invalidate_blocks(host_page, std::move(victims));
}

// Kill `victims`, all of which lived on `host_page` and have already been
// unlinked from that page's list.
static void invalidate_blocks(const u8* host_page, std::vector<Block*> victims) {
  if (churn::on()) {
    static bool reg = (std::atexit(churn::report), true); (void)reg;
    CpuContext* ctx = g_rt.cpus[0].ctx ? g_rt.cpus[0].ctx : g_rt.cpus[1].ctx;
    const CpuContext* run = ctx->nds->sched.running();
    const bool dma = ctx->nds->sched.in_dma();
    const u8 cpu = run ? (run->which == Cpu::ARM9 ? 9 : 7) : 0;
    const u32 pc = run ? run->hot.regs[15] : 0;
    churn::writers[churn::Key{pc, dma, cpu}]++;
    churn::inval++; churn::killed += victims.size(); churn::frames_seen = ctx->nds->frame_count;
    for (Block* b : victims) {
      churn::victims_by_page[key_pc(b->key) & ~(mem::PAGE_SIZE - 1)]++;
      churn::page_last_write[key_pc(b->key) & ~(mem::PAGE_SIZE - 1)] = ctx->nds->sched.now();
    }
  }
  for (Block* b : victims) {
    JitCpu& jc = jc_of(b);
    kill_block(jc, b);
    // The block may also be listed under its second page.
    for (u32 i = 0; i < b->npages; ++i) if (b->host_pages[i] != host_page) {
      auto jt = g_rt.code_pages.find(b->host_pages[i]);
      if (jt == g_rt.code_pages.end()) continue;
      auto& v = jt->second;
      for (size_t k = 0; k < v.size(); ++k) if (v[k] == b) { v[k] = v.back(); v.pop_back(); break; }
      if (v.empty()) { g_rt.code_pages.erase(jt); set_code_tag(b->host_pages[i], false); }
    }
  }
  for (JitCpu& jc : g_rt.cpus) if (jc.ctx) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
}

void invalidate_cpu(JitCpu& jc) {
  pretx::purge();   // timing/config changed: the worker's pending output is built on the old tables
  for (Block* b : jc.all_blocks) if (!b->dead) { remove_from_page_lists(b); kill_block(jc, b); }
  jc.blocks.clear();
  jc.parked.clear();   // built under the old tables: never revivable (the stamp check would refuse them anyway)
  if (jc.ctx) jc.ctx->hot.alerts |= ALERT_INVALIDATED;
}

// ---- helpers called from translated code -----------------------------------------------------

extern "C" u32 jit_h_fallback(CpuContext* cpu, u32 instr, u32 key) {
  // Execute one instruction through the interpreter with the interpreter's own
  // cycle accounting. r15 is set from the key (pipeline-adjusted).
  cpu->hot.regs[15] = key_r15(key);
  cpu->data_cycles = 0;
  cpu->jumped = false;
  if (cpu->which == Cpu::ARM9) prefetch_cost9(*cpu);
  else { cpu->code_cycles = key_pc(key) >> 15; cpu->code_region = key_pc(key) >> 24; }
  if (key_thumb(key)) {
    interp::exec_thumb(*cpu, static_cast<u16>(instr));
    if (!cpu->jumped) cpu->hot.regs[15] += 2;
  } else {
    interp::exec_arm(*cpu, instr);
    if (!cpu->jumped) cpu->hot.regs[15] += 4;
  }
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;   // the poll leaves; run() ends the slice
  g_rt.stats.instrs_fallback++;
  if (g_rt.hist) g_rt.fallback_hist[(static_cast<u64>(cpu->which) << 32) | key_pc(key)]++;
  if (g_rt.debug) std::fprintf(stderr, "[jit] fallback %08x %08x -> r15 %08x cpsr %08x budget %d halted %d jumped %d\n", key_pc(key), instr, cpu->hot.regs[15], cpu->hot.cpsr, cpu->hot.cycle_budget, cpu->halted, cpu->jumped);
  return cpu->jumped ? 1 : 0;
}

extern "C" const void* jit_h_lookup(CpuContext* cpu, u32 key) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu->jit);
  const u8* native = find_native(jc, key);
  if (g_rt.debug) std::fprintf(stderr, "[jit] lookup %08x -> %p (budget %d)\n", key, static_cast<const void*>(native), cpu->hot.cycle_budget);
  if (native) return native;
  cpu->hot.regs[15] = key_r15(key);
  return g_rt.flush_exit;   // arena full: leave; run() resets the arena
}

extern "C" const void* jit_h_link(CpuContext* cpu, u32 key, u8* patch_site) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu->jit);
  const u8* native = find_native(jc, key);
  if (g_rt.debug) std::fprintf(stderr, "[jit] link %08x -> %p at %p (budget %d)\n", key, static_cast<const void*>(native), static_cast<void*>(patch_site), cpu->hot.cycle_budget);
  if (!native) { cpu->hot.regs[15] = key_r15(key); return g_rt.flush_exit; }
  backend::patch_link(patch_site, native);   // `bl link` -> `b native`
  sync_icache(patch_site, 4);
  return native;
}

extern "C" void jit_h_cyclog(CpuContext* cpu, u32 instr, u32 key) {
  (void)instr;
  std::fprintf(stderr, "[cyc%d] %08x %d\n", cpu->which == Cpu::ARM9 ? 9 : 7, key_r15(key), cpu->hot.cycle_budget);
}

extern "C" void jit_h_trace(CpuContext* cpu, u32 instr, u32 key) {
  cpu->hot.regs[15] = key_r15(key);
  if (cpu->nds->trace) cpu->nds->trace(*cpu, instr, cpu->nds->trace_user);
}

// Loads and stores that left the inline page-table path: MMIO, unmapped
// space, read-only and code pages. Nearly all of them are MMIO (measured:
// 99 % across SM64DS, Mario & Luigi and Meteos, four in five of them loads),
// so those go straight to Io rather than through Bus, whose two frames only
// re-test the region this path has already established. Otherwise the same
// paths the interpreter's mem_read*/mem_write* take (cpu_mem.h), minus the
// cost, which the block charges from the timing table like every other access.
extern "C" u32 jit_h_ld8(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  if (u8* p = cpu->page_table.read_ptr(addr)) return *p;
  if ((addr & 0xFF000000) == 0x04000000) return cpu->nds->io.read(cpu->which, addr, 8);
  return cpu->nds->bus.read8(cpu->which, addr);
}
extern "C" u32 jit_h_ld16(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  addr &= ~1u;
  if (u8* p = cpu->page_table.read_ptr(addr)) { u16 v; std::memcpy(&v, p, 2); return v; }
  if ((addr & 0xFF000000) == 0x04000000) return cpu->nds->io.read(cpu->which, addr, 16);
  return cpu->nds->bus.read16(cpu->which, addr);
}
extern "C" u32 jit_h_ld32(CpuContext* cpu, u32 addr) {
  g_rt.stats.slow_accesses++;
  addr &= ~3u;
  if (u8* p = cpu->page_table.read_ptr(addr)) { u32 v; std::memcpy(&v, p, 4); return v; }
  if ((addr & 0xFF000000) == 0x04000000) return cpu->nds->io.read(cpu->which, addr, 32);
  return cpu->nds->bus.read32(cpu->which, addr);
}
extern "C" void jit_h_st8(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  bool code = false;
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { const u8 b8 = static_cast<u8>(v); if (code) mem::store_code(p, &b8, 1); else *p = b8; }
  else if ((addr & 0xFF000000) == 0x04000000) cpu->nds->io.write(cpu->which, addr, 8, v);
  else cpu->nds->bus.write8(cpu->which, addr, static_cast<u8>(v));
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}
extern "C" void jit_h_st16(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  addr &= ~1u;
  bool code = false;
  const u16 h = static_cast<u16>(v);
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &h, 2); else std::memcpy(p, &h, 2); }
  else if ((addr & 0xFF000000) == 0x04000000) cpu->nds->io.write(cpu->which, addr, 16, v);
  else cpu->nds->bus.write16(cpu->which, addr, h);
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}
extern "C" void jit_h_st32(CpuContext* cpu, u32 addr, u32 v) {
  g_rt.stats.slow_accesses++;
  addr &= ~3u;
  bool code = false;
  if (u8* p = cpu->page_table.write_ptr(addr, &code)) { if (code) mem::store_code(p, &v, 4); else std::memcpy(p, &v, 4); }
  else if ((addr & 0xFF000000) == 0x04000000) cpu->nds->io.write(cpu->which, addr, 32, v);
  else cpu->nds->bus.write32(cpu->which, addr, v);
  if (cpu->halted) cpu->hot.alerts |= ALERT_HALTED;
}

// ---- public API ---------------------------------------------------------------------------------

bool attach(NDS& nds, bool arm9, bool arm7) {
  Runtime& r = g_rt;
  if (!r.arena) {
    void* p = mmap(nullptr, ARENA_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { std::fprintf(stderr, "jit: cannot map code arena\n"); return false; }
    r.arena = static_cast<u8*>(p);
    r.cap = ARENA_BYTES;
    for (int c = 0; c < 2; ++c) {
      r.cpus[c].lut = reinterpret_cast<u64*>(r.arena + c * LUT_STRIDE);
      for (u32 i = 0; i < LUT_SIZE; ++i) r.cpus[c].lut[i] = LUT_EMPTY_KEY;
    }
    backend::emit_stubs(r);
    perf_map_stubs(r);
    r.strict = std::getenv("DS_JIT_STRICT") != nullptr;
    r.debug = std::getenv("DS_JIT_DEBUG") != nullptr;
    r.cyclog = std::getenv("DS_DEBUG_CYCLES") != nullptr;
    r.hist = std::getenv("DS_JIT_HIST") != nullptr;
    r.density = std::getenv("DS_JIT_DENSITY") != nullptr;
    r.fastcost = std::getenv("DS_JIT_FASTCOST") != nullptr;
    r.retime_all = std::getenv("DS_JIT_RETIME_ALL") != nullptr;
    r.nocsel  = std::getenv("DS_JIT_NOCSEL") != nullptr;
    r.nocost7 = std::getenv("DS_JIT_NOCOST7") != nullptr;
    if (const char* cp = std::getenv("DS_JIT_COSTPROBE")) r.costprobe = std::atoi(cp);
    if (const char* pp = std::getenv("DS_JIT_COSTPROBE_PART")) r.costprobe_part = std::atoi(pp);
    if (const char* mp = std::getenv("DS_JIT_MEMPROBE")) r.memprobe = std::atoi(mp);
    mem::PageTable::code_query = &code_query;
    mem::code_write_hook = &code_write_hook;
  }
  r.trace = nds.trace != nullptr;
  for (int c = 0; c < 2; ++c) {
    const bool on = c == 0 ? arm9 : arm7;
    if (!on) continue;
    JitCpu& jc = r.cpus[c];
    CpuContext& ctx = nds.cpu(c == 0 ? Cpu::ARM9 : Cpu::ARM7);
    jc.ctx = &ctx;
    jc.nds = &nds;
    jc.arm9 = c == 0;
    jc.hot.pt = ctx.page_table.raw();
    jc.hot.timing = c == 0 ? reinterpret_cast<const u8*>(ctx.timing9) : reinterpret_cast<const u8*>(ctx.timing7);
    jc.hot.arena = r.arena;
    // An overlay-heavy scene translates ~20 k blocks; growing these through a
    // burst rehashes/reallocates on the critical path of the burst frame.
    jc.blocks.reserve(1u << 15);
    jc.all_blocks.reserve(1u << 15);
    r.code_pages.reserve(1u << 13);
    ctx.jit = &jc;
    ctx.jit_timing_changed = &on_timing_changed;
    (c == 0 ? nds.run_arm9 : nds.run_arm7) = &run;
  }
  pretx::start();
  return true;
}

void detach(NDS& nds) {
  for (int c = 0; c < 2; ++c) {
    JitCpu& jc = g_rt.cpus[c];
    if (!jc.ctx) continue;
    invalidate_cpu(jc);
    jc.ctx->jit = nullptr;
    jc.ctx->jit_timing_changed = nullptr;
    (c == 0 ? nds.run_arm9 : nds.run_arm7) = &interp::run;
    jc.ctx = nullptr;
  }
}

void flush(CpuContext& cpu) { if (cpu.jit) invalidate_cpu(*static_cast<JitCpu*>(cpu.jit)); }
void flush_all() { reset_arena(); }
void set_trace(bool on) { if (g_rt.trace != on) { g_rt.trace = on; for (JitCpu& jc : g_rt.cpus) if (jc.ctx) invalidate_cpu(jc); } }
void set_cpu_oc(bool on) { if (g_rt.cpu_oc != on) { g_rt.cpu_oc = on; for (JitCpu& jc : g_rt.cpus) if (jc.ctx) invalidate_cpu(jc); } }
void set_strict(bool on) { if (g_rt.strict != on) { g_rt.strict = on; for (JitCpu& jc : g_rt.cpus) if (jc.ctx) invalidate_cpu(jc); } }
const Stats& stats() { return g_rt.stats; }

static double ex_total_pct(unsigned long long v, unsigned long long tot) {
  return tot ? 100.0 * static_cast<double>(v) / static_cast<double>(tot) : 0.0;
}

void density_reset() { for (DensitySlot& d : g_rt.density_slots) d.execs = 0; }

DensitySlot* density_new_slot() {
  Runtime& r = g_rt;
  if (!r.density) return nullptr;
  r.density_slots.emplace_back();
  return &r.density_slots.back();
}

void report(std::FILE* out) {
  const Stats& s = g_rt.stats;
  std::fprintf(out, "[jit] blocks %llu, inline instrs %llu, fallback executions %llu, slow accesses %llu, entries %llu, invalidated %llu, revived %llu, flushes %llu\n",
               (unsigned long long)s.blocks_translated, (unsigned long long)s.instrs_translated, (unsigned long long)s.instrs_fallback,
               (unsigned long long)s.slow_accesses, (unsigned long long)s.entries, (unsigned long long)s.blocks_invalidated, (unsigned long long)s.blocks_revived, (unsigned long long)s.flushes);
  if (pretx::on())
    std::fprintf(out, "[jit] pretx: built %llu adopted %llu dropped %llu skipped %llu\n",
                 (unsigned long long)pretx::st_built, (unsigned long long)pretx::st_adopted,
                 (unsigned long long)pretx::st_dropped, (unsigned long long)pretx::st_skipped);
  std::fprintf(out, "[jit] code %llu KB (hot %llu KB): %.1f bytes per guest instruction, %.1f hot\n", (unsigned long long)(s.code_bytes >> 10), (unsigned long long)(s.hot_bytes >> 10),
               s.instrs_translated ? static_cast<double>(s.code_bytes) / static_cast<double>(s.instrs_translated) : 0.0,
               s.instrs_translated ? static_cast<double>(s.hot_bytes) / static_cast<double>(s.instrs_translated) : 0.0);
  if (g_rt.density) {
    u64 ex = 0;
    long double hb = 0.0L, gi = 0.0L;
    for (const DensitySlot& d : g_rt.density_slots) {
      ex += d.execs;
      hb += static_cast<long double>(d.execs) * d.hot_bytes;
      gi += static_cast<long double>(d.execs) * d.guest_instrs;
    }
    std::fprintf(out, "[jit] density: %zu translations, %llu block entries, %.0Lf guest instrs executed, %.0Lf hot bytes fetched\n",
                 g_rt.density_slots.size(), (unsigned long long)ex, gi, hb);
    // The static figure has to be recomputed from the slots, not read from
    // Stats: stats.hot_bytes counts the counter code itself, which is ~24 bytes
    // on every block and would inflate the very baseline being compared against.
    // Same population, same fields -- the only difference is the weighting.
    long double shb = 0.0L, sgi = 0.0L;
    for (const DensitySlot& d : g_rt.density_slots) { shb += d.hot_bytes; sgi += d.guest_instrs; }
    std::fprintf(out, "[jit] density: %.2Lf hot bytes per guest instruction executed, against %.2Lf translated (static)\n",
                 gi > 0.0L ? hb / gi : 0.0L, sgi > 0.0L ? shb / sgi : 0.0L);
    // Block length, by translation and weighted by execution. The averages hide
    // the shape: what matters for dispatch cost is how many *executed* blocks
    // are short, not how many translated ones are.
    static const int lo[9] = {1, 2, 3, 4, 5, 9, 17, 33, 65};
    u64 tx[8] = {}, bex[8] = {};
    long double gx[8] = {};
    for (const DensitySlot& d : g_rt.density_slots) {
      int k = 0;
      while (k < 7 && static_cast<int>(d.guest_instrs) >= lo[k + 1]) ++k;
      tx[k]++; bex[k] += d.execs;
      gx[k] += static_cast<long double>(d.execs) * d.guest_instrs;
    }
    std::fprintf(out, "[jit] density: block length    translations        entries   %% entries   %% guest instrs\n");
    for (int k = 0; k < 8; ++k) {
      if (!tx[k] && !bex[k]) continue;
      char lab[16];
      if (lo[k + 1] - lo[k] == 1) std::snprintf(lab, sizeof lab, "%d", lo[k]);
      else std::snprintf(lab, sizeof lab, "%d-%d", lo[k], lo[k + 1] - 1);
      std::fprintf(out, "[jit]         %12s %14llu %14llu %10.2f %%     %10.2Lf %%\n", lab,
                   (unsigned long long)tx[k], (unsigned long long)bex[k],
                   ex_total_pct(bex[k], ex), gi > 0.0L ? 100.0L * gx[k] / gi : 0.0L);
    }
  }
  if (!g_rt.hist) return;
  std::vector<std::pair<u64, u64>> v(g_rt.fallback_hist.begin(), g_rt.fallback_hist.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::fprintf(out, "[jit] hottest fallbacks (cpu pc instr count):\n");
  for (size_t i = 0; i < v.size() && i < 16; ++i) {
    const Cpu c = static_cast<Cpu>(v[i].first >> 32);
    const u32 pc = static_cast<u32>(v[i].first);
    CpuContext& ctx = *g_rt.cpus[c == Cpu::ARM9 ? 0 : 1].ctx;
    u32 instr = 0;
    if (u8* p = ctx.page_table.read_ptr(pc)) std::memcpy(&instr, p, 4);
    std::fprintf(out, "[jit]   arm%d %08x %08x %llu\n", c == Cpu::ARM9 ? 9 : 7, pc, instr, (unsigned long long)v[i].second);
  }
}

const void* lookup(CpuContext& cpu) {
  JitCpu& jc = *static_cast<JitCpu*>(cpu.jit);
  Runtime& r = g_rt;
  for (;;) {
    if (r.need_reset) reset_arena();
    const bool thumb = cpu.thumb();
    const u32 key = make_key(cpu.hot.regs[15] - (thumb ? 4 : 8), thumb);
    const u64 lut = jc.lut[(key >> 1) & (LUT_SIZE - 1)];
    const u8* native = static_cast<u32>(lut) == key ? r.arena + (lut >> 32) : find_native(jc, key);
    if (!native) { reset_arena(); continue; }
    cpu.hot.alerts = 0;
    r.stats.entries++;
    return native;
  }
}

bool has_runtime() { return g_rt.arena != nullptr; }
void run_loop(void* scheduler) { g_rt.run_loop(scheduler); }

void run(CpuContext& cpu) {
  Runtime& r = g_rt;
  cpu.check_irq();
  if (cpu.halted) { cpu.hot.cycle_budget = -1; return; }
  if (cpu.step_limit) { interp::run(cpu); return; }
  while (cpu.hot.cycle_budget > 0) {
    r.enter(&cpu, lookup(cpu));
    if (cpu.halted) { cpu.budget_at_halt = cpu.hot.cycle_budget; cpu.hot.cycle_budget = -1; return; }
    if (cpu.hot.irq_pending) cpu.check_irq();
  }
}

} // namespace ds::jit
