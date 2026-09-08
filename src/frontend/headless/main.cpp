// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Headless frontend: boots the BIOS/firmware (and optionally a ROM), runs N frames,
// optionally writing per-CPU instruction traces in the shared trace format:
//   <pc> <instr> <cpsr> r0 .. r14     (hex, one line per instruction)
// and/or dumping raw framebuffers (--dump-frames) for tools/compare_frames.py
// and the SPU output (--dump-audio, raw s16 stereo at 32768 Hz).
#include "core/nds.h"
#include "core/state/state.h"
#include "core/cpu/interp/interp.h"
#include "core/input/input_log.h"
#if DSPERATE_JIT
#include "core/cpu/jit/jit.h"
#endif
#include "core/profile.h"
#include "core/frame_report.h"
#include "core/cheat/database.h"

#include <cstdio>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>

namespace {


// DS_STATE_DEBUG=1: the per-CPU cycle-accounting state and the timing-table
// rows under each PC, printed where a state is written and after one is
// loaded, so a timing drift after a load can be attributed.
void dump_cpu_timing(ds::NDS& nds, const char* when) {
  if (!std::getenv("DS_STATE_DEBUG")) return;
  for (ds::Cpu w : {ds::Cpu::ARM9, ds::Cpu::ARM7}) {
    const ds::CpuContext& c = nds.cpu(w);
    const ds::u32 pc = c.hot.regs[15];
    std::fprintf(stderr, "[state %s] %s irq_pending %u ime %u ie %08x if %08x cpsr %08x\n", when, w == ds::Cpu::ARM9 ? "a9" : "a7", c.hot.irq_pending,
                 nds.io.cpu_io[w == ds::Cpu::ARM9 ? 0 : 1].ime, nds.io.cpu_io[w == ds::Cpu::ARM9 ? 0 : 1].ie, nds.io.cpu_io[w == ds::Cpu::ARM9 ? 0 : 1].if_, c.hot.cpsr);
    std::fprintf(stderr, "[state %s] %s pc %08x budget %d resid %d halted %d code_cycles %u data_cycles %u code_rgn %u data_rgn %u branch_fetch %d t9[",
                 when, w == ds::Cpu::ARM9 ? "a9" : "a7", pc, c.hot.cycle_budget, c.preempt_residual, c.halted, c.code_cycles, c.data_cycles, c.code_region, c.data_region, c.branch_fetch);
    for (int i = 0; i < 8; ++i) std::fprintf(stderr, "%s%u", i ? " " : "", c.timing9[pc >> 12][i]);
    std::fprintf(stderr, "] t7[");
    for (int i = 0; i < 4; ++i) std::fprintf(stderr, "%s%u", i ? " " : "", c.timing7[pc >> 15][i]);
    std::fprintf(stderr, "] itcm %u dtcm %08x/%08x\n", c.itcm_size, c.dtcm_base, c.dtcm_mask);
  }
  { ds::u64 h = 1469598103934665603ull; const ds::u32* l = nds.gpu3d.line(nds.gpu3d.frame_ref(), 0); for (int x = 0; x < 256; ++x) h = (h ^ l[x]) * 1099511628211ull;
    std::fprintf(stderr, "[state %s] 3d line0 hash %016llx\n", when, (unsigned long long)h); }
  std::fprintf(stderr, "[state %s] now %llu next %llu a7debt? gx stalled %d idle %d\n", when, (unsigned long long)nds.sched.now(), (unsigned long long)nds.sched.next_deadline(), nds.gpu3d.stalled(), nds.gpu3d.idle());
}

std::vector<ds::u8> slurp_file(const char* path) {
  std::vector<ds::u8> v;
  FILE* f = std::fopen(path, "rb");
  if (!f) return v;
  std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
  if (n > 0) { v.resize(static_cast<size_t>(n)); if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear(); }
  std::fclose(f);
  return v;
}
// Spin-loop collapse: a poll loop repeats its exact state every iteration until
// the polled value changes, so a line identical to one of the last 32 emitted
// lines is dropped. Both tracers apply the same rule, so traces stay comparable
// while shrinking by orders of magnitude.
struct TraceState {
  FILE* out[2] = {nullptr, nullptr};
  unsigned long long count[2] = {0, 0};
  unsigned long long executed[2] = {0, 0};
  unsigned long long max = 1'000'000;
  ds::u64 recent[2][32] = {};
  unsigned recent_pos[2] = {0, 0};
  bool pc_hist = false;                       // TRACE_PC_HIST=1: uncollapsed PC histogram on stderr at exit
  const ds::NDS* nds = nullptr;
  bool stamp = false;                         // TRACE_TIME=1: prefix each line with the scheduler time (not the shared format)
  std::unordered_map<ds::u32, unsigned long long> hist[2];
};

void trace_cb(ds::CpuContext& cpu, ds::u32 instr, void* user) {
  auto* t = static_cast<TraceState*>(user);
  const int i = static_cast<int>(cpu.which);
  t->executed[i]++;
  if (t->pc_hist) t->hist[i][cpu.hot.regs[15] - (cpu.thumb() ? 4 : 8)]++;
  if (t->count[i] >= t->max) return;
  const bool thumb = cpu.thumb();
  const ds::u32 pc = cpu.hot.regs[15] - (thumb ? 4 : 8);
  ds::u64 h = 1469598103934665603ull;
  auto mix = [&](ds::u32 v) { h ^= v; h *= 1099511628211ull; h ^= h >> 29; };
  mix(pc); mix(instr); mix(cpu.hot.cpsr);
  for (int r = 0; r < 15; ++r) mix(cpu.hot.regs[r]);
  for (int k = 0; k < 32; ++k) if (t->recent[i][k] == h) return;
  t->recent[i][t->recent_pos[i]++ & 31] = h;
  t->count[i]++;
  if (t->stamp) std::fprintf(t->out[i], "%llu ", (unsigned long long)t->nds->sched.now());
  std::fprintf(t->out[i], "%08x %08x %08x", pc, instr, cpu.hot.cpsr);
  for (int r = 0; r < 15; ++r) std::fprintf(t->out[i], " %08x", cpu.hot.regs[r]);
  std::fputc('\n', t->out[i]);
}
} // namespace

int main(int argc, char** argv) {
  const char *rom = nullptr, *bios9 = nullptr, *bios7 = nullptr, *fw = nullptr, *trace = nullptr, *dump = nullptr, *dump_audio = nullptr, *replay = nullptr, *save = nullptr;
  const char* load_state = nullptr; const char* save_state_path = nullptr; int save_state_at = -1;
  const char* hide_screen = nullptr;
  int frameskip = 0;    // --frameskip N: skip drawing N of every N+1 frames (fixed; the SDL frontend also has the adaptive mode)
  bool frameskip_capture = false;
  int stats_from = 0;   // --stats-from N: first frame counted in the timing statistics
  // A whole 1800-frame dump is ~708 MB, so a window can be selected: the
  // frame-budget report below names the frames worth looking at.
  int dump_from = 0, dump_count = 0;
  int frames = 60; bool direct = false;
#if DSPERATE_JIT
  bool jit9 = true, jit7 = true;
  long quantum = ds::LOCKSTEP_QUANTUM;   // the harness compares against melonDS: lockstep unless asked otherwise
#else
  bool jit9 = false, jit7 = false;
  long quantum = ds::LOCKSTEP_QUANTUM;
#endif
  TraceState ts;
  bool timing_oc = false;
  bool rtc_host = false;              // --rtc-host: free-running clock seeded from the wall
  const char* fw_override = nullptr;  // --firmware-override: sidecar of changed firmware pages
  bool no_aa = false;
  bool cpu_oc = false;
  bool frames_given = false;
  const char* cheat_db = nullptr;      // a usrcheat.dat to load this ROM's codes from
  bool list_cheats = false;
  std::vector<std::string> enable_cheats;   // names (or #index) to switch on
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
    auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
    if (arg("--frames")) { frames = std::atoi(argv[++i]); frames_given = true; }
    else if (arg("--bios9")) bios9 = argv[++i];
    else if (arg("--bios7")) bios7 = argv[++i];
    else if (arg("--firmware")) fw = argv[++i];
    else if (arg("--trace")) trace = argv[++i];
    else if (arg("--max")) ts.max = std::strtoull(argv[++i], nullptr, 0);
    else if (arg("--dump-frames")) dump = argv[++i];
    else if (arg("--dump-from")) dump_from = std::atoi(argv[++i]);    // first frame to dump
    else if (arg("--dump-count")) dump_count = std::atoi(argv[++i]);  // how many (0 = to the end)
    else if (arg("--dump-audio")) dump_audio = argv[++i];   // raw s16 stereo, 32768 Hz
    else if (arg("--replay")) replay = argv[++i];           // inputs recorded by dsperate-sdl --record; sets --frames to its length unless given
    else if (arg("--save")) save = argv[++i];               // battery save to start from; loaded read-only, never written back
    else if (arg("--cheats")) cheat_db = argv[++i];         // usrcheat.dat; the entry matching this ROM is loaded
    else if (flag("--list-cheats")) list_cheats = true;     // print them (with their index) and exit
    else if (arg("--cheat")) enable_cheats.push_back(argv[++i]);   // enable one by name, or by "#N" from --list-cheats
    else if (!std::strcmp(argv[i], "--direct")) direct = true;
    else if (!std::strcmp(argv[i], "--interp")) jit9 = jit7 = false;          // interpreter for both CPUs
    else if (arg("--quantum")) quantum = std::atol(argv[++i]);                // CPU interleave in ARM9 cycles; 0 = event-bound (the frontends' mode)
    else if (flag("--rtc-host")) rtc_host = true;                            // INEXACT by construction: runs stop being reproducible
    else if (arg("--firmware-override")) fw_override = argv[++i];            // load it, and write back what the firmware changed
    else if (flag("--timing-oc")) timing_oc = true;                          // no FIFO + untimed geometry (DraStic's model); see Gpu3D::set_timing_oc
    else if (flag("--no-aa")) no_aa = true;                                  // 3D anti-aliasing off (Renderer3D::set_aa); inexact, for measurement
    else if (flag("--cpu-oc")) cpu_oc = true;                                // INEXACT: JIT data accesses priced as main RAM at translate time; see jit::set_cpu_oc
    // Split A/B knobs: the bundled flag above is three separate changes.

    else if (!std::strcmp(argv[i], "--jit9")) { jit9 = true; jit7 = false; }  // recompile the ARM9 only
    else if (!std::strcmp(argv[i], "--jit7")) { jit9 = false; jit7 = true; }
    else if (arg("--load-state")) load_state = argv[++i];                   // restore a save state before running
    else if (arg("--frameskip")) frameskip = std::atoi(argv[++i]);          // skip drawing N of every N+1 frames (Gpu::set_frame_skip); a dump of a skipped frame is stale
    else if (flag("--frameskip-capture")) frameskip_capture = true;          // INEXACT: skip frames that display-capture too
    else if (arg("--hide-screen")) hide_screen = argv[++i];                 // top | bottom: the engine on it skips its drawing (Gpu::set_screen_visible); its half of the dump goes stale
    // Frames before N are run but left out of the statistics. A --load-state
    // starts cold: every translated block was dropped with the old run, the
    // texture cache is empty and the host caches hold the loader's data, so
    // the first frames are slow in a way the scene never is. Warm up, then
    // measure. Applies to the frame_ms/work_ms series, not to DS_PROFILE
    // counters, which accumulate from frame 0 either way.
    else if (arg("--stats-from")) stats_from = std::atoi(argv[++i]);
    else if (arg("--save-state-at")) { save_state_at = std::atoi(argv[++i]); save_state_path = std::strchr(argv[i], ':'); if (save_state_path) ++save_state_path; }   // N:path -- write after N frames (0 = at once)
    else rom = argv[i];
  }
  // A replay is recorded under direct boot (the SDL frontend has no other
  // mode), so replaying without --direct boots the firmware instead and the
  // inputs land in its setup wizard -- a run that looks fast because it draws
  // nothing. The frame count and cycle total are identical either way, so
  // there is no other sign it happened.
  if (replay && !direct)
    std::fprintf(stderr, "warning: --replay without --direct boots the firmware, not the ROM;"
                         " the replay will not reproduce the recorded session\n");
  std::fprintf(stderr, "DSperate 1.0.0 (%s%s)\n",
#if DSPERATE_JIT
              "jit",
#else
              "interp",
#endif
#if DSPERATE_NEON
              "+neon");
#else
              "");
#endif
  ds::NDS nds;
  if (frameskip_capture) nds.gpu.set_frameskip_capture(true);
  if (hide_screen) nds.gpu.set_screen_visible(!std::strcmp(hide_screen, "bottom") ? 1 : 0, false);
  {
    std::string err;
    if (!nds.load_bios(bios9 ? bios9 : "", bios7 ? bios7 : "", fw ? fw : "", {}, &err)) { std::fprintf(stderr, "bios: %s\n", err.c_str()); return 1; }
  }
  if (!nds.bios_native) std::fprintf(stderr, "note: --bios9/--bios7 %s; using the built-in FreeBIOS (direct boot only, timing is not Nintendo's)\n", bios9 ? "not found" : "not given");
  if (nds.firmware_synthetic) std::fprintf(stderr, "note: --firmware %s; using a generated firmware\n", fw ? "not found" : "not given");
  if (!direct && !nds.can_boot_firmware()) {
    // FreeBIOS's reset vector is an idle loop, so a firmware boot draws nothing.
    if (rom) { std::fprintf(stderr, "booting the firmware needs real BIOS and firmware dumps; pass --direct to run the ROM\n"); return 1; }
    std::fprintf(stderr, "warning: no ROM and no real dumps: nothing boots (FreeBIOS has no boot code)\n");
  }
  if (fw_override && !nds.firmware_synthetic) {
    std::string err;
    if (!nds.load_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware override: %s\n", err.c_str());
    else if (!err.empty()) std::fprintf(stderr, "firmware override: warning: %s\n", err.c_str());
  }
  nds.reset();
  // After reset(), which clears the RTC.
  if (rtc_host) nds.io.start_rtc_clock();
  // A zipped ROM may be inflated to disk on first use; say so, since on an
  // SD card that is seconds to a minute.
  nds.rom_progress = [](void*, ds::u64 done, ds::u64 total) {
    static ds::u64 last = ~0ull;
    const ds::u64 pct = total ? done * 100 / total : 100;
    if (pct / 10 != last / 10 || done == total) { std::fprintf(stderr, "\rzip: extracting %llu%%", static_cast<unsigned long long>(pct)); last = pct; }
    if (done == total) std::fputc('\n', stderr);
  };
  if (rom && !nds.load_rom(rom)) { std::fprintf(stderr, "could not read %s\n", rom); return 1; }
  nds.sched.set_quantum(quantum);
  nds.gpu3d.set_timing_oc(timing_oc);
  // Geometry worker + per-frame shape controller, with either inexact tier
  // (no-FIFO, or the FIFO kept with the cull priced by ratio under --cpu-oc).
  nds.gpu3d.set_geometry_worker(timing_oc || cpu_oc);   // DS_GX_THREAD: 0 never, 1 per-frame shape controller, 2 always
  nds.gpu3d.renderer().set_aa(!no_aa);

  if (rom && cheat_db) {
    ds::cheat::GameCheats found;
    std::string err;
    // From the loaded cart, so a zipped ROM is looked up by the game's header
    // rather than the archive's first 512 bytes.
    ds::u8 header[512] = {};
    if (nds.cart) nds.cart->rom_read(0, header, sizeof header);
    if (!ds::cheat::load_for_header(cheat_db, header, found, err)) {
      if (!err.empty()) { std::fprintf(stderr, "cheats: %s\n", err.c_str()); return 1; }
      std::fprintf(stderr, "cheats: this ROM is not in %s\n", cheat_db);
      if (list_cheats) return 0;
    } else {
      if (!err.empty()) std::fprintf(stderr, "cheats: %s\n", err.c_str());
      std::fprintf(stderr, "cheats: %s -- %zu codes in %zu groups\n",
                   found.name.c_str(), found.codes.size(), found.groups.size());
      if (list_cheats) {
        for (size_t i = 0; i < found.codes.size(); ++i) {
          const ds::cheat::Code& c = found.codes[i];
          const char* g = c.group >= 0 ? found.groups[static_cast<size_t>(c.group)].name.c_str() : "";
          std::printf("#%-5zu %-6s %-52s %s\n", i, c.is_note() ? "note" : "code", c.name.c_str(), g);
        }
        return 0;
      }
      nds.cheats.codes = std::move(found.codes);
      // Enabling by name matches the whole name; "#N" is the index the
      // listing printed, which is the way to reach one of the many codes
      // whose names are duplicated within a game.
      for (const std::string& want : enable_cheats) {
        bool hit = false;
        if (want.size() > 1 && want[0] == '#') {
          const size_t at = std::strtoul(want.c_str() + 1, nullptr, 10);
          if (at < nds.cheats.codes.size()) { nds.cheats.codes[at].enabled = true; hit = true; }
        } else {
          for (ds::cheat::Code& c : nds.cheats.codes) if (c.name == want) { c.enabled = true; hit = true; }
        }
        if (!hit) std::fprintf(stderr, "cheats: no code called \"%s\"\n", want.c_str());
      }
      size_t on = 0;
      for (const ds::cheat::Code& c : nds.cheats.codes) if (c.enabled) ++on;
      std::fprintf(stderr, "cheats: %zu enabled\n", on);
    }
  }
  if (rom && direct) nds.setup_direct_boot();
  // A recording made with a save present only replays if the save is there:
  // the game otherwise stops to create one. Loaded in the same place the SDL
  // frontend loads it, and never written back -- this is a harness.
  if (save && nds.cart && !nds.cart->sram().empty()) {
    if (FILE* f = std::fopen(save, "rb")) {
      std::vector<ds::u8>& sram = nds.cart->sram();
      const size_t n = std::fread(sram.data(), 1, sram.size(), f);
      std::fclose(f);
      std::fprintf(stderr, "save: loaded %zu bytes from %s\n", n, save);
    } else {
      std::fprintf(stderr, "save: cannot read %s\n", save);
      return 1;
    }
  }
#if DSPERATE_JIT
  if ((jit9 || jit7) && !ds::jit::attach(nds, jit9, jit7)) return 1;
  if ((jit9 || jit7) && cpu_oc) ds::jit::set_cpu_oc(true);
#else
  (void)jit9; (void)jit7; (void)cpu_oc;
#endif
  ds::prof::enabled = std::getenv("DS_PROFILE") != nullptr;
  if (const char* w = std::getenv("DS_WATCH")) nds.bus.enable_watch(static_cast<ds::u32>(std::strtoul(w, nullptr, 16)));
  if (trace) {
    ts.out[0] = std::fopen((std::string(trace) + ".arm9.trace").c_str(), "w");
    ts.out[1] = std::fopen((std::string(trace) + ".arm7.trace").c_str(), "w");
  }
  const char* per_frame = std::getenv("TRACE_PER_FRAME");
  unsigned long long last9 = 0, last7 = 0;
  FILE* dump_out = dump ? std::fopen(dump, "wb") : nullptr;
  if (dump && !dump_out) { std::fprintf(stderr, "could not open %s\n", dump); return 1; }
  ds::input::Log log;
  if (replay) {
    if (!log.open_read(replay)) { std::fprintf(stderr, "cannot read %s\n", replay); return 1; }
    if (!frames_given) frames = static_cast<int>(log.frames());
    std::fprintf(stderr, "replay: %u frames from %s\n", log.frames(), replay);
  }
  FILE* audio_out = dump_audio ? std::fopen(dump_audio, "wb") : nullptr;
  if (dump_audio && !audio_out) { std::fprintf(stderr, "could not open %s\n", dump_audio); return 1; }
  ts.pc_hist = std::getenv("TRACE_PC_HIST") != nullptr;
  ts.stamp = std::getenv("TRACE_TIME") != nullptr; ts.nds = &nds;
  const char* trace_start = std::getenv("TRACE_START_FRAME");   // suppress trace output before this frame
  const int trace_from = trace_start ? std::atoi(trace_start) : 0;
  // Per-frame host times. Whole-process wall clock on the device turned out to
  // spread 13 % run to run at a flat temperature, which buries any change
  // worth measuring; the median frame rejects the transient stalls that cause
  // it, and p90 still shows them if they matter.
  // DS_WATCHDOG=<seconds>: a frame that makes no progress for that long is a
  // hang; dump the display/raster hand-off state and abort, so the state is
  // in the log instead of needing a debugger on the stuck process.
  std::atomic<bool> wd_stop{false};
  std::thread wd;
  if (const char* w = std::getenv("DS_WATCHDOG")) {
    const int limit = std::atoi(w);
    wd = std::thread([&nds, &wd_stop, limit] {
      ds::u64 last = ~ds::u64{0}; int still = 0;
      while (!wd_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const ds::u64 fc = nds.frame_count;
        if (fc == last) { if (++still >= limit) {
          std::fprintf(stderr, "[watchdog] no progress for %d s at frame %llu:\n", limit, (unsigned long long)fc);
          nds.gpu.debug_dump(stderr); std::fflush(stderr); std::abort(); } }
        else { last = fc; still = 0; }
      }
    });
  }
  std::vector<double> frame_ms;
  frame_ms.reserve(static_cast<size_t>(frames));
  if (load_state) {
    std::vector<ds::u8> bytes = slurp_file(load_state);
    ds::state::Reader r(bytes.data(), bytes.size());
    std::string err;
    if (bytes.empty() || !nds.load_state(r, err)) { std::fprintf(stderr, "cannot load state %s: %s\n", load_state, bytes.empty() ? "unreadable" : err.c_str()); return 1; }
    std::fprintf(stderr, "state: loaded %s (frame %llu)\n", load_state, static_cast<unsigned long long>(nds.frame_count));
    dump_cpu_timing(nds, "load");
    // A replay continues from the state's frame, not from the log's start.
    if (log.reading()) { ds::input::Frame f; for (ds::u64 k = 0; k < nds.frame_count && log.read(f); ++k) {} }
  }
  auto write_state = [&](int after) {
    if (!save_state_path || save_state_at != after) return true;
    ds::state::Writer w; std::string err;
    if (!nds.save_state(w, err)) { std::fprintf(stderr, "cannot save state: %s\n", err.c_str()); return false; }
    dump_cpu_timing(nds, "save");
    FILE* f = std::fopen(save_state_path, "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", save_state_path); return false; }
    std::fwrite(w.data().data(), 1, w.data().size(), f); std::fclose(f);
    std::fprintf(stderr, "state: wrote %s after %d frames (%zu bytes)\n", save_state_path, after, w.data().size());
    return true;
  };
  if (!write_state(0)) return 1;
  const char* reboot_at = std::getenv("DS_REBOOT_AT");
  const int reboot_frame = reboot_at ? std::atoi(reboot_at) : -1;
  const char* reboot_rom = reboot_at ? std::strchr(reboot_at, ':') : nullptr;
  if (reboot_rom) ++reboot_rom;

  for (int i = 0; i < frames; ++i) {
    if (reboot_rom && i == reboot_frame) {
      nds.reset();
      if (!nds.load_rom(reboot_rom)) std::fprintf(stderr, "reboot: could not read %s\n", reboot_rom);
      else { nds.setup_direct_boot(); std::fprintf(stderr, "reboot: direct boot at frame %d\n", i); }
    }
    const auto t0 = std::chrono::steady_clock::now();
    // The recompiler's trace emission is armed from the first frame: turning
    // it on later drops every translated block at that frame, which changes
    // the run being traced. TRACE_START_FRAME only gates the callback.
#if DSPERATE_JIT
    if (trace && i == 0) ds::jit::set_trace(true);
#endif
    if (trace && i == trace_from) { nds.trace = trace_cb; nds.trace_user = &ts; }
    if (log.reading()) { ds::input::Frame in; if (log.read(in)) ds::input::apply(nds, in); }
    // The core takes the decision one frame ahead (the 3D raster for a frame
    // runs during the frame before it), so this asks for frame i + 1.
    if (frameskip > 0) {
      // Same rule as the SDL frontend: skip and draw in whole display periods
      // (Gpu::display_phase_period), the limit counting periods rather than
      // frames. Here as a fixed pattern.
      const int period = nds.gpu.display_phase_period();
      const int skip = frameskip * period;
      const int cycle = skip + period;
      nds.gpu.set_frame_skip(skip > 0 && static_cast<int>((i + 1) % cycle) < skip);
    }
    nds.run_frame();
    if (!write_state(i + 1)) return 1;
    if (static const bool fh = std::getenv("DS_FRAME_HASH") != nullptr; fh) {
      auto fnv = [](const ds::u8* p, size_t n, ds::u64 h) { for (size_t k = 0; k < n; ++k) h = (h ^ p[k]) * 1099511628211ull; return h; };
      ds::u64 h = 1469598103934665603ull;
      h = fnv(nds.bus.main_ram.get(), 4u << 20, h);
      h = fnv(nds.bus.shared_wram.get(), 32u << 10, h);
      h = fnv(nds.bus.arm7_wram.get(), 64u << 10, h);
      h = fnv(nds.bus.dtcm.get(), 16u << 10, h);
      const auto& a9 = nds.cpu(ds::Cpu::ARM9).hot; const auto& a7 = nds.cpu(ds::Cpu::ARM7).hot;
      std::fprintf(stderr, "[fh] %d mem %016llx a9", i, (unsigned long long)h);
      for (int r = 0; r < 16; ++r) std::fprintf(stderr, " %08x", a9.regs[r]);
      std::fprintf(stderr, " %08x a7", a9.cpsr);
      for (int r = 0; r < 16; ++r) std::fprintf(stderr, " %08x", a7.regs[r]);
      std::fprintf(stderr, " %08x\n", a7.cpsr);
      static const char* dumpf = std::getenv("DS_FRAME_DUMP");   // "<frame>:<path>": write main RAM + WRAM + DTCM after that frame
      if (dumpf && std::atoi(dumpf) == i) {
        FILE* f = std::fopen(std::strchr(dumpf, ':') + 1, "wb");
        std::fwrite(nds.bus.main_ram.get(), 1, 4u << 20, f); std::fwrite(nds.bus.shared_wram.get(), 1, 32u << 10, f);
        std::fwrite(nds.bus.arm7_wram.get(), 1, 64u << 10, f); std::fwrite(nds.bus.dtcm.get(), 1, 16u << 10, f); std::fclose(f);
      }
    }
    if (dump_out && i >= dump_from && (dump_count <= 0 || i < dump_from + dump_count)) {
      // raw 0xAARRGGBB, top screen then bottom, 256x192 each, one record per frame
      std::fwrite(nds.gpu.framebuffer(0), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
      std::fwrite(nds.gpu.framebuffer(1), 4, ds::SCREEN_W * ds::SCREEN_H, dump_out);
    }
    if (audio_out) {
      ds::s16 buf[2048 * 2]; size_t n;
      while ((n = nds.spu.take(buf, 2048)) != 0) std::fwrite(buf, 4, n, audio_out);
    } else nds.spu.drain();
#if DSPERATE_JIT
    if (i == stats_from && (jit9 || jit7)) ds::jit::density_reset();
#endif
    if (i >= stats_from)
      frame_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      // DS_FRAME_SERIES=<path>: the run-order series ("ms polygons raster_ns gx_ns" per line),
      // for the shape of a tail -- alternation, bursts -- rather than its size.
      static FILE* series = [] { const char* p = std::getenv("DS_FRAME_SERIES"); return p ? std::fopen(p, "w") : nullptr; }();
      // ms polygons raster_ns(serial, last synced frame) gx_worker_busy_ns
      if (series) std::fprintf(series, "%.3f %u %llu %llu\n", frame_ms.back(), nds.gpu3d.render_polygon_count(),
                               (unsigned long long)nds.gpu3d.last_raster_ns(), (unsigned long long)nds.gpu3d.take_worker_busy_ns());
    // The console has switched itself off. On a firmware boot that is the
    // firmware leaving its settings pages, with the pages it wrote already in
    // the image, so this is the moment to put them on disk -- and then to
    // start the console again, which is what the hardware's power button
    // would do next. A cart session is left alone: a game is not expected to
    // reach here, and quitting a benchmark on one stray write would be worse
    // than running on.
    if (nds.power_off) {
      std::fprintf(stderr, "power off at frame %d%s\n", i, nds.cart ? "" : "; saving settings and rebooting");
      if (!nds.cart) {
        if (fw_override) {
          std::string err;
          if (!nds.save_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware override: cannot save: %s\n", err.c_str());
        }
#if DSPERATE_JIT
        if (jit9 || jit7) ds::jit::flush_all();
#endif
        nds.reset();          // clears power_off, and re-seeds the clock if --rtc-host
      } else {
        nds.power_off = false;
      }
    }
    ds::prof::frame_mark();
    if (per_frame && trace) { std::fprintf(stderr, "frame %d arm9 %llu arm7 %llu\n", i, ts.executed[0] - last9, ts.executed[1] - last7); last9 = ts.executed[0]; last7 = ts.executed[1]; }
  }
  wd_stop.store(true); if (wd.joinable()) wd.join();
  if (fw_override && nds.firmware_override_dirty()) {
    std::string err;
    if (!nds.save_firmware_override(fw_override, err)) std::fprintf(stderr, "firmware override: cannot save: %s\n", err.c_str());
    else std::fprintf(stderr, "firmware override: saved %s\n", fw_override);
  }
  if (dump_out) std::fclose(dump_out);
  if (audio_out) std::fclose(audio_out);
  if (ts.pc_hist) {
    for (int c = 0; c < 2; ++c) {
      std::vector<std::pair<ds::u32, unsigned long long>> v(ts.hist[c].begin(), ts.hist[c].end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      std::fprintf(stderr, "arm%d hottest pcs:", c ? 7 : 9);
      for (size_t k = 0; k < v.size() && k < 12; ++k) std::fprintf(stderr, " %08x:%llu", v[k].first, v[k].second);
      std::fputc('\n', stderr);
    }
  }
  if (trace) { std::fclose(ts.out[0]); std::fclose(ts.out[1]);
    std::fprintf(stderr, "arm9: %llu lines (%llu instrs), arm7: %llu lines (%llu instrs), cap %llu lines each\n",
                 ts.count[0], ts.executed[0], ts.count[1], ts.executed[1], ts.max); }
  ds::prof::report();
#if DSPERATE_JIT
  if (ds::prof::enabled && (jit9 || jit7)) ds::jit::report(stderr);
#endif
  ds::interp::census_report(nds.frame_count);
  if (std::getenv("DS_STATE_DUMP")) {   // what each CPU is waiting on at the end of the run
    for (int c = 0; c < 2; ++c) {
      const ds::Cpu cpu = c == 0 ? ds::Cpu::ARM9 : ds::Cpu::ARM7;
      const auto& ci = nds.io.cpu_io[c];
      std::fprintf(stderr, "[state] %s pc %08x halted %d IME %x IE %08x IF %08x IE&IF %08x fifocnt %04x romctrl %08x auxspicnt %04x\n",
                   c == 0 ? "arm9" : "arm7", nds.cpu(cpu).hot.regs[15], nds.cpu(cpu).halted, ci.ime, ci.ie, ci.if_, ci.ie & ci.if_,
                   nds.io.read(cpu, 0x04000184, 16), nds.io.read(cpu, 0x040001A4, 32), nds.io.read(cpu, 0x040001A0, 16));
      for (int d = 0; d < 4; ++d) std::fprintf(stderr, "[state]   dma%d cnt %08x src %08x dst %08x\n", d, nds.io.read(cpu, 0x040000B8 + d * 12, 32), nds.io.read(cpu, 0x040000B0 + d * 12, 32), nds.io.read(cpu, 0x040000B4 + d * 12, 32));
    }
  }
  ds::frame_report(frame_ms);
  ds::prof::frame_breakdown(frame_ms);
  std::fprintf(stderr, "ran %llu frames, %llu cycles\n",
              static_cast<unsigned long long>(nds.frame_count),
              static_cast<unsigned long long>(nds.sched.now()));
  return 0;
}
