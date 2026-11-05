// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The headless --read-log (read_log.h): range lists, the load decoder, reads
// merged into runs per CPU and reading instruction, the frame window, and (in
// a census build) an interpreted load reaching it through the census hook with
// its PC while a store doesn't.
#include "core/cpu/cpu_mem.h"
#include "core/nds.h"
#include "frontend/headless/read_log.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace ds;
using headless::ReadLog;

namespace {

std::string slurp(const std::string& path) {
  std::string s;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
    std::fclose(f);
  }
  return s;
}

std::string tmp(const char* name) { return std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/" + name; }

void test_ranges() {
  std::vector<ReadLog::Range> r;
  CHECK(ReadLog::parse_ranges("02000000-0200FFFF,0x027E0000-0x027E3FFF", r));
  CHECK(r.size() == 2 && r[0].lo == 0x02000000 && r[0].hi == 0x0200FFFF && r[1].lo == 0x027E0000);
  CHECK(!ReadLog::parse_ranges("", r));
  CHECK(!ReadLog::parse_ranges("02000000", r));
  CHECK(!ReadLog::parse_ranges("02000010-02000000", r));
  CHECK(!ReadLog::parse_ranges("02000000-02000010;x", r));
}

void test_decoder() {
  // ARM
  CHECK(ReadLog::load_width(0xE5910000, false) == 4);   // ldr r0, [r1]
  CHECK(ReadLog::load_width(0xE5D10000, false) == 1);   // ldrb r0, [r1]
  CHECK(ReadLog::load_width(0xE5810000, false) == 0);   // str r0, [r1]
  CHECK(ReadLog::load_width(0xE7910002, false) == 4);   // ldr r0, [r1, r2]
  CHECK(ReadLog::load_width(0xE1D100B0, false) == 2);   // ldrh r0, [r1]
  CHECK(ReadLog::load_width(0xE1D100D0, false) == 1);   // ldrsb r0, [r1]
  CHECK(ReadLog::load_width(0xE1C100B0, false) == 0);   // strh r0, [r1]
  CHECK(ReadLog::load_width(0xE1C100D0, false) == 4);   // ldrd r0, [r1]
  CHECK(ReadLog::load_width(0xE1C100F0, false) == 0);   // strd r0, [r1]
  CHECK(ReadLog::load_width(0xE8BD8010, false) == 4);   // ldmia sp!, {r4, pc}
  CHECK(ReadLog::load_width(0xE92D4010, false) == 0);   // stmdb sp!, {r4, lr}
  CHECK(ReadLog::load_width(0xE1010092, false) == 4);   // swp r0, r2, [r1]
  CHECK(ReadLog::load_width(0xE0810002, false) == 0);   // add r0, r1, r2
  // Thumb
  CHECK(ReadLog::load_width(0x4801, true) == 4);        // ldr r0, [pc, #4]
  CHECK(ReadLog::load_width(0x5888, true) == 4);        // ldr r0, [r1, r2]
  CHECK(ReadLog::load_width(0x5688, true) == 1);        // ldrsb r0, [r1, r2]
  CHECK(ReadLog::load_width(0x5088, true) == 0);        // str r0, [r1, r2]
  CHECK(ReadLog::load_width(0x6808, true) == 4);        // ldr r0, [r1]
  CHECK(ReadLog::load_width(0x7808, true) == 1);        // ldrb r0, [r1]
  CHECK(ReadLog::load_width(0x6008, true) == 0);        // str r0, [r1]
  CHECK(ReadLog::load_width(0x8808, true) == 2);        // ldrh r0, [r1]
  CHECK(ReadLog::load_width(0x9801, true) == 4);        // ldr r0, [sp, #4]
  CHECK(ReadLog::load_width(0xBD10, true) == 4);        // pop {r4, pc}
  CHECK(ReadLog::load_width(0xB510, true) == 0);        // push {r4, lr}
  CHECK(ReadLog::load_width(0xC903, true) == 4);        // ldmia r1!, {r0, r1}
  CHECK(ReadLog::load_width(0x1888, true) == 0);        // adds r0, r1, r2
}

void test_runs_and_window() {
  if (!ReadLog::available()) return;   // start() needs the census hook
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  const std::string path = tmp("dsperate_read_log_test.tsv");
  {
    ReadLog log;
    CHECK(log.start(nds, path, {{0x02000000, 0x020000FF}}, 5, 7));
    ReadLog other;
    CHECK(!other.start(nds, path + ".2", {{0, 1}}, 0, 0));   // one log at a time
    nds.frame_count = 4; log.on_read(Cpu::ARM9, 0x02001000, 0x02000000, 4);   // before the window
    nds.frame_count = 5;
    log.on_read(Cpu::ARM9, 0x02001000, 0x02000010, 4);
    log.on_read(Cpu::ARM9, 0x02001000, 0x02000014, 4);        // same instruction, next word: one run
    log.on_read(Cpu::ARM9, 0x02001004, 0x02000010, 2);
    log.on_read(Cpu::ARM7, 0x02001000, 0x02000010, 1);        // the other CPU: its own line
    log.on_read(Cpu::ARM9, 0x02001000, 0x02000200, 4);        // outside the range
    nds.frame_count = 6; log.on_read(Cpu::ARM9, 0x02001000, 0x02000010, 4);
    nds.frame_count = 7; log.on_read(Cpu::ARM9, 0x02001000, 0x02000018, 4);   // after the window
    CHECK(log.finish());
  }
  const std::string s = slurp(path);
  CHECK(s.find("read\tarm9\t02001000\t02000010\t02000017\t12\t5\t6\n") != std::string::npos);
  CHECK(s.find("read\tarm9\t02001004\t02000010\t02000011\t2\t5\t5\n") != std::string::npos);
  CHECK(s.find("read\tarm7\t02001000\t02000010\t02000010\t1\t5\t5\n") != std::string::npos);
  CHECK(s.find("\t02000200\t") == std::string::npos && s.find("\t02000000\t") == std::string::npos);
  CHECK(s.find("\t02000018\t") == std::string::npos);
  std::remove(path.c_str());
}

// An interpreted load goes through data_cost and the census hook with the
// reading instruction's address (r15 is 8 ahead in ARM state); a store doesn't log.
void test_interpreter_load() {
  if (!ReadLog::available()) return;
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  const std::string path = tmp("dsperate_read_log_load.tsv");
  {
    ReadLog log;
    CHECK(log.start(nds, path, {{0x02000300, 0x02000303}}, 0, 0));
    CpuContext& cpu = nds.cpu(Cpu::ARM9);
    nds.bus.dma_write32(Cpu::ARM9, 0x02000100, 0xE5910000);   // ldr r0, [r1]
    nds.bus.dma_write32(Cpu::ARM9, 0x02000104, 0xE5810000);   // str r0, [r1]
    cpu.hot.regs[15] = 0x02000108;                            // executing 0x02000100
    mem_read32(cpu, 0x02000300);
    cpu.hot.regs[15] = 0x0200010C;                            // executing 0x02000104
    mem_write32(cpu, 0x02000300, 1);
  }
  const std::string s = slurp(path);
  CHECK(s.find("read\tarm9\t02000100\t02000300\t02000303\t4\t0\t0\n") != std::string::npos);
  CHECK(s.find("02000104") == std::string::npos);
  std::remove(path.c_str());
}

} // namespace

int main() {
  test_ranges();
  test_decoder();
  test_runs_and_window();
  test_interpreter_load();
  std::puts(ReadLog::available() ? "read_log: ok" : "read_log: ok (not a census build: the hook tests skipped)");
  return 0;
}
