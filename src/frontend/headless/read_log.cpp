// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/headless/read_log.h"

#include "core/cpu/cpu_mem.h"
#include "core/nds.h"
#include "core/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ds::headless {

namespace {

ReadLog* g_log = nullptr;
void (*g_prev)(bool, u32, bool) = nullptr;

} // namespace

// The census hook: every interpreter data access (loads and stores alike),
// from data_cost, while the accessing instruction runs.
void read_log_access(bool a9, u32 addr, bool) {
  ReadLog* log = g_log;
  if (!log || !log->nds_ || !log->in_window()) return;
  const Cpu which = a9 ? Cpu::ARM9 : Cpu::ARM7;
  if (!log->wanted(addr, 1)) return;
  CpuContext& cpu = log->nds_->cpu(which);
  const bool thumb = cpu.thumb();
  const u32 pc = cpu.hot.regs[15] - (thumb ? 4 : 8);   // as --trace prints it
  mem::Bus& bus = log->nds_->bus;
  const u32 instr = thumb ? bus.dma_read16(which, pc) : bus.dma_read32(which, pc);
  if (const u32 w = ReadLog::load_width(instr, thumb)) log->on_read(which, pc, addr, w);
}

u32 ReadLog::load_width(u32 i, bool thumb) {
  if (thumb) {
    i &= 0xFFFF;
    if ((i >> 11) == 0x09) return 4;                                   // LDR Rd, [PC, #imm]
    if ((i >> 12) == 0x5) {                                            // register offset
      static const u8 w[8] = {0, 0, 0, 1, 4, 2, 1, 2};                 // STR STRH STRB LDRSB LDR LDRH LDRB LDRSH
      return w[(i >> 9) & 7];
    }
    if ((i >> 13) == 0x3) return (i & 0x0800) ? ((i & 0x1000) ? 1 : 4) : 0;   // LDR/STR(B) #imm
    if ((i >> 12) == 0x8) return (i & 0x0800) ? 2 : 0;                 // LDRH/STRH #imm
    if ((i >> 12) == 0x9) return (i & 0x0800) ? 4 : 0;                 // LDR/STR [SP, #imm]
    if ((i & 0xF600) == 0xB400) return (i & 0x0800) ? 4 : 0;           // POP / PUSH
    if ((i >> 12) == 0xC) return (i & 0x0800) ? 4 : 0;                 // LDMIA / STMIA
    return 0;
  }
  const bool load = i & (1u << 20);
  switch ((i >> 25) & 7) {
  case 2: return load ? ((i & (1u << 22)) ? 1 : 4) : 0;                // LDR/STR(B) #imm
  case 3: return (i & 0x10) ? 0 : load ? ((i & (1u << 22)) ? 1 : 4) : 0;   // register offset (bit 4 set: media)
  case 4: return load ? 4 : 0;                                         // LDM / STM
  case 0:
    if ((i & 0x0FB00FF0) == 0x01000090) return (i & (1u << 22)) ? 1 : 4;   // SWP(B): its read
    if ((i & 0x90) == 0x90 && (i & 0x60)) {                            // halfword, signed, doubleword
      const u32 sh = (i >> 5) & 3;
      if (load) return sh == 2 ? 1 : 2;                                // LDRSB, LDRH / LDRSH
      return sh == 2 ? 4 : 0;                                          // LDRD (STRH, STRD store)
    }
    return 0;
  default: return 0;
  }
}

bool ReadLog::available() { return prof::census; }

bool ReadLog::parse_ranges(const char* spec, std::vector<Range>& out) {
  out.clear();
  const char* p = spec;
  while (*p) {
    char* end;
    const unsigned long lo = std::strtoul(p, &end, 16);
    if (end == p || *end != '-') return false;
    p = end + 1;
    const unsigned long hi = std::strtoul(p, &end, 16);
    if (end == p || hi < lo || hi > 0xFFFFFFFFul) return false;
    out.push_back({static_cast<u32>(lo), static_cast<u32>(hi)});
    p = end;
    if (*p == ',') ++p;
    else if (*p) return false;
  }
  return !out.empty();
}

bool ReadLog::start(NDS& nds, const std::string& path, std::vector<Range> ranges, u64 from, u64 to) {
  if (g_log || !available()) return false;
  nds_ = &nds; path_ = path; ranges_ = std::move(ranges); from_ = from; to_ = to;
  g_log = this; active_ = true;
  g_prev = g_census_access;
  g_census_access = read_log_access;
  return true;
}

bool ReadLog::in_window() const {
  const u64 f = nds_ ? nds_->frame_count : 0;
  return f >= from_ && (to_ == 0 || f < to_);
}

bool ReadLog::wanted(u32 addr, u32 bytes) const {
  for (const Range& r : ranges_) if (addr <= r.hi && addr + bytes - 1 >= r.lo) return true;
  return false;
}

void ReadLog::on_read(Cpu cpu, u32 pc, u32 addr, u32 bytes) {
  if (!wanted(addr, bytes) || !in_window()) return;
  const u64 f = nds_ ? nds_->frame_count : 0;
  for (u32 b = 0; b < bytes; ++b) {
    Count& c = reads_[cpu == Cpu::ARM7][(static_cast<u64>(pc) << 32) | (addr + b)];
    if (c.n++ == 0) c.first = f;
    c.last = f;
  }
}

bool ReadLog::finish() {
  if (!active_) return true;
  active_ = false;
  g_census_access = g_prev;
  g_log = nullptr;
  FILE* f = std::fopen(path_.c_str(), "w");
  if (!f) { std::fprintf(stderr, "read log: cannot write %s\n", path_.c_str()); return false; }
  std::fprintf(f, "# dsperate read log 1\n");
  std::fprintf(f, "# read\tcpu\tpc\tlo\thi\treads\tfirst frame\tlast frame\n");
  size_t pairs = 0;
  for (int c = 0; c < 2; ++c) {
    std::vector<std::pair<u64, Count>> all(reads_[c].begin(), reads_[c].end());
    pairs += all.size();
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // Neighbouring addresses read by one instruction make one run.
    for (size_t i = 0; i < all.size();) {
      const u32 pc = static_cast<u32>(all[i].first >> 32);
      u32 lo = static_cast<u32>(all[i].first), hi = lo;
      Count sum = all[i].second;
      size_t j = i + 1;
      for (; j < all.size() && static_cast<u32>(all[j].first >> 32) == pc && static_cast<u32>(all[j].first) == hi + 1; ++j) {
        hi = static_cast<u32>(all[j].first);
        sum.n += all[j].second.n;
        sum.first = std::min(sum.first, all[j].second.first);
        sum.last = std::max(sum.last, all[j].second.last);
      }
      std::fprintf(f, "read\t%s\t%08x\t%08x\t%08x\t%llu\t%llu\t%llu\n", c ? "arm7" : "arm9", pc, lo, hi,
                   static_cast<unsigned long long>(sum.n), static_cast<unsigned long long>(sum.first),
                   static_cast<unsigned long long>(sum.last));
      i = j;
    }
  }
  std::fclose(f);
  std::fprintf(stderr, "read log: %zu (pc, address) pairs written to %s\n", pairs, path_.c_str());
  return true;
}

} // namespace ds::headless
