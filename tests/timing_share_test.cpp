// SPDX-License-Identifier: GPL-3.0-or-later
// The ARM9 timing table with its open range as one shared page (Timing::shared_) against the same table
// with memory of its own throughout: every slot, refill entry and retime flag must match after each update.
#include "check.h"
#include "core/cpu/cpu.h"
#include "core/mem/timing.h"

#include <cstring>

using namespace ds;

namespace {
mem::Timing a, b;
CpuContext cpu{};
int step = 0;

void compare() {
  ++step;
  const size_t slots = mem::Timing::CPU9_BYTES, refill = mem::Timing::REFILL9_BYTES;
  const u8* ta = reinterpret_cast<const u8*>(a.cpu9());
  const u8* tb = reinterpret_cast<const u8*>(b.cpu9());
  if (std::memcmp(ta, tb, slots) != 0) { std::fprintf(stderr, "step %d: slots differ\n", step); std::exit(1); }
  if (std::memcmp(ta + slots, tb + slots, refill) != 0) {
    for (u32 p = 0; p < 0x100000; ++p) if (std::memcmp(ta + slots + p * 4, tb + slots + p * 4, 4) != 0) { std::fprintf(stderr, "step %d: refill entry of page %05x differs\n", step, p); break; }
    std::exit(1);
  }
  for (u32 p = 0; p < 0x100000; ++p) if (a.retime_flag(p) != b.retime_flag(p)) { std::fprintf(stderr, "step %d: retime flag of page %05x differs\n", step, p); std::exit(1); }
  CHECK(a.retime_pending() == b.retime_pending());
  a.retime_clear(); b.retime_clear();
}
void update(u32 start, u32 end) { a.update_cpu9(cpu, start, end); b.update_cpu9(cpu, start, end); compare(); }
void pu(u32 start, u32 end, u8 v) {   // pages
  std::memset(a.pu_map.get() + start, v, end - start);
  std::memset(b.pu_map.get() + start, v, end - start);
}
void clock(u32 shift) { a.clock9_shift = b.clock9_shift = shift; update(0, 0xFFFFFFFF); }
}  // namespace

int main() {
  b.unshare9();
  CHECK(!b.shared9());
  const bool sharing = a.shared9();   // false where the platform has no shared mapping: the test then compares like with like
  compare();
  update(0, 0xFFFFFFFF);
  // The DSi's clock switch, both ways, with the caches off and on.
  clock(2); clock(1);
  pu(0, 0x100000, 0x50); update(0, 0xFFFFFFFF);
  clock(2);
  pu(0, 0x100000, 0x00); update(0, 0xFFFFFFFF);
  pu(0x02000, 0x03000, 0x50); update(0x02000000, 0x03000000);
  clock(1); clock(2);
  // TCMs below the range, and ranges that end or start on its edges.
  cpu.itcm_size = 0x02000000; cpu.dtcm_base = 0x027E0000; cpu.dtcm_mask = 0xFFFFC000;
  update(0, 0xFFFFFFFF);
  update(0x08000000, 0x10000000);
  update(0xFC000000, 0xFFFFFFFF);
  update(0x0FFFF000, 0xFC001000);
  // A game region across the whole of it, uniform: still one page.
  pu(0x10000, 0xFC000, 0x10); update(0x10000000, 0xFC000000);   // data only: the code cost, and so every refill entry, as before
  pu(0x10000, 0x100000, 0x40); update(0, 0xFFFFFFFF);            // code: to the top, so that the page above the range agrees
  pu(0x10000, 0x100000, 0x00); update(0x10000000, 0xFFFFFFFF);
  CHECK(a.shared9() == sharing);   // nothing so far has made the range anything but one slot
  // The page above the range unlike it: the range's last refill entry is then its own.
  pu(0xFC000, 0xFC001, 0x40); update(0xFC000000, 0xFC001000);
  clock(1); clock(2);
  pu(0xFC000, 0xFC001, 0x00); update(0, 0xFFFFFFFF);
  // A region inside it: no longer uniform, and everything after must still match.
  pu(0x20000, 0x30000, 0x50); update(0x20000000, 0x30000000);
  CHECK(!a.shared9());
  clock(1); clock(2);
  pu(0x20000, 0x30000, 0x00); update(0, 0xFFFFFFFF);
  // reset() on both.
  a.reset(); b.reset(); compare();
  clock(1);
  std::printf("timing share: %d steps, shared at the start: %s\n", step, sharing ? "yes" : "no");
  return 0;
}
