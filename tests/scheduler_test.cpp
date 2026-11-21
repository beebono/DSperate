// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/nds.h"
#include "core/cpu/idle_loop.h"
#include <cstdlib>
#include <cstring>
#include "check.h"

using namespace ds;

static int fired = 0;
static u64 fired_at = 0;
static void on_event(NDS& nds, u32 param) { ++fired; fired_at = nds.sched.now(); CHECK(param == 7); }

int main() {
  setenv("DS_IDLE_CHECK", "1", 1);
  NDS nds;
  nds.sched.schedule(EventId::Dma, 1000, on_event, 7);
  nds.run_frame();
  CHECK(fired == 1);
  CHECK(fired_at >= 1000);
  CHECK(nds.sched.now() >= CYCLES_PER_FRAME);
  CHECK(nds.frame_count == 1);

  // A poll loop is idle only while its exit condition does not hold: the same
  // loop with the flag it waits for already set is about to leave.
  {
    CpuContext& a9 = nds.cpu(Cpu::ARM9);
    const u32 code[] = {0xE5901000u,    // ldr r1, [r0]
                        0xE3510000u,    // cmp r1, #0
                        0x0AFFFFFCu};   // beq loop
    u8* ram = nds.bus.main_ram.get();
    std::memcpy(ram, code, sizeof code);
    u32 flag = 0; std::memcpy(ram + 0x100, &flag, 4);
    a9.hot.cpsr = 0x1F;
    a9.hot.regs[0] = 0x02000100; a9.hot.regs[1] = 0;
    a9.hot.regs[15] = 0x02000004 + 8;           // stopped at the compare, Z set by the last lap
    a9.hot.cpsr |= 0x40000000u;
    const JitHot before = a9.hot;
    CHECK(cpu::in_idle_loop(a9));
    CHECK(!std::memcmp(&before, &a9.hot, sizeof before));   // the proof leaves the CPU as it was
    flag = 1; std::memcpy(ram + 0x100, &flag, 4);
    a9.hot.regs[15] = 0x02000000 + 8;           // the load has not seen it yet: one lap, then out
    CHECK(!cpu::in_idle_loop(a9));
    CHECK(cpu::idle_loop_last_reject() == cpu::IdleReject::Exit);
    CHECK(!std::memcmp(&before.regs, &a9.hot.regs, 15 * sizeof(u32)));
    flag = 0; std::memcpy(ram + 0x100, &flag, 4);
    CHECK(cpu::in_idle_loop(a9));
  }
  // The proof's own evaluator against the interpreter (DS_IDLE_CHECK=1, set
  // before the first query): random data-processing instructions and loads
  // in a loop body, every flag and carry state.
  {
    CpuContext& a9 = nds.cpu(Cpu::ARM9);
    u8* ram = nds.bus.main_ram.get();
    u32 seed = 0x2545F491u;
    const auto rnd = [&seed] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    u32 ran = 0;
    for (u32 trial = 0; trial < 20000; ++trial) {
      const u32 x = rnd();
      u32 dp = 0xE0000000u | (x & 0x03FFFFFFu);                    // AL, data processing
      if (!(dp & (1u << 25))) dp &= ~(1u << 4);                    // shift by immediate
      const u32 opcode = (dp >> 21) & 0xF;
      if (opcode >= 0x8 && opcode <= 0xB) dp |= 1u << 20;          // a compare sets flags
      dp = (dp & ~0x000FF00Fu) | ((rnd() % 8) << 16) | ((1 + rnd() % 7) << 12) | (dp & (1u << 25) ? dp & 0xF : rnd() % 8);
      const u32 ld[] = {0xE5908000u, 0xE5D08001u, 0xE1D080B2u};    // ldr r8,[r0] / ldrb r8,[r0,#1] / ldrh r8,[r0,#2]
      const u32 code[] = {dp, ld[trial % 3], 0xEAFFFFFCu};
      std::memcpy(ram + 0x400, code, sizeof code);
      u32 word = rnd(); std::memcpy(ram + 0x100, &word, 4);
      for (u32 r = 1; r < 8; ++r) a9.hot.regs[r] = (rnd() & 3) ? rnd() : rnd() & 0xFF;
      a9.hot.regs[0] = 0x02000100;
      a9.hot.cpsr = 0x1F | (rnd() & 0xF0000000u);
      a9.hot.regs[15] = 0x02000400 + 8;
      cpu::invalidate_idle_loops();
      cpu::in_idle_loop(a9);
      if (cpu::idle_loop_last_reject() == cpu::IdleReject::None || cpu::idle_loop_last_reject() == cpu::IdleReject::Exit) ++ran;
    }
    CHECK(ran > 5000);
    CHECK(cpu::idle_loop_stats().check_differs == 0);
  }
  std::puts("scheduler: ok");
  return 0;
}
