// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds { struct NDS; }

namespace ds::cheat {

// Action Replay DS codes: pairs of 32-bit words, run once a frame from the
// ARM7's VBlank IRQ, which is where the real cartridge hooks itself. The
// opcode set is GBATEK's (writes, conditionals, a loop, a data register, two
// block copies); the interpreter follows melonDS's, which is the reference
// this was checked against.
//
// Writes go to the ARM7's address space through the bus's DMA accessors: the
// bytes are not the guest's, so they are not charged CPU cycles, but they do
// go through the page table and so invalidate JIT blocks like any other store.
struct Code {
  std::string name;
  bool enabled = false;
  std::vector<u32> words;   // an even number; an odd tail is ignored
};

// Why a code stopped, for the log and the tests. A code that simply ran off
// its end is Ok; everything else names something wrong with the code itself.
enum class Stop : u8 {
  Ok,          // ran to the end of the words
  BadOpcode,   // an opcode outside the set
  Truncated,   // an operand or a block ran past the end of the words
  Unsupported, // C4: the self-modifying opcode, which nothing is known to use
  RunawayLoop, // the iteration budget was exhausted (a code that never ends)
};
const char* stop_name(Stop s);

class Engine {
public:
  std::vector<Code> codes;

  // Runs every enabled code, in order. Safe to call with no codes.
  void run(NDS& nds);
  // One code, whatever its `enabled`. Returns why it stopped.
  Stop run_code(NDS& nds, const Code& code);

  // A code that loops for ever must not hang the emulator, so execution is
  // capped. The limit is per code per run and far above any real code: the
  // largest published ones are a few hundred word pairs with loops of a few
  // thousand iterations.
  static constexpr u64 MAX_STEPS = 1u << 20;

private:
  // Diagnostics are logged once per code, not once per frame: a bad code
  // fires every frame and would otherwise bury the log. Tracked by index and
  // reset whenever the list's length changes, so nothing here outlives the
  // vector it refers to.
  std::vector<u8> complained_;
};

} // namespace ds::cheat
