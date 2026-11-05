// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The headless harness's read log (--read-log FILE): which instructions read
// which guest addresses, for finding the code that reads a data structure
// (a level, a table, a decompressed file). Every interpreter data load inside
// the --read-range windows is counted by (CPU, reading instruction's PC,
// address); at the end they're written as runs, one line per (CPU, PC, run).
//
// It listens on the core's existing per-access census hook
// (g_census_access, cpu_mem.h), so it needs a census build
// (-DDSPERATE_CENSUS=1) and the interpreter (--interp): the recompiler's
// inline loads never reach the hook. The hook sees loads and stores with the
// address only; the reading instruction (r15 less the pipeline) is decoded
// here to keep the loads and their widths. See docs/headless-scripting.md.
#pragma once
#include "core/types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ds { struct NDS; }
namespace ds::headless {

class ReadLog {
public:
  struct Range { u32 lo, hi; };   // inclusive
  // Parses "LO-HI[,LO-HI...]" (hex, 0x optional); false on a bad list.
  static bool parse_ranges(const char* spec, std::vector<Range>& out);
  // The bytes an instruction loads per access (1, 2 or 4), or 0 for one that
  // doesn't load (a store, or not a load/store at all).
  static u32 load_width(u32 instr, bool thumb);
  // Whether this build can log (a census build).
  static bool available();

  // Starts logging into `path` (written by finish). Frames [from, to) only;
  // to 0 means to the end. Takes the census hook; one log at a time.
  bool start(NDS& nds, const std::string& path, std::vector<Range> ranges, u64 from, u64 to);
  // Writes the file and gives the hook back. Safe to call twice.
  bool finish();
  ~ReadLog() { finish(); }

  // One load of `bytes` at `addr` by the instruction at `pc` (what the hook
  // calls once it has decoded the instruction; public for tests).
  void on_read(Cpu cpu, u32 pc, u32 addr, u32 bytes);

private:
  struct Count { u64 n = 0, first = 0, last = 0; };
  bool wanted(u32 addr, u32 bytes) const;
  bool in_window() const;
  friend void read_log_access(bool a9, u32 addr, bool seq);
  NDS* nds_ = nullptr;
  std::string path_;
  std::vector<Range> ranges_;
  u64 from_ = 0, to_ = 0;
  bool active_ = false;
  // Per CPU (ARM9, ARM7), by pc << 32 | address.
  std::unordered_map<u64, Count> reads_[2];
};

} // namespace ds::headless
