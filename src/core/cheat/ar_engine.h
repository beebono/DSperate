// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds { struct NDS; }

namespace ds::cheat {

// Action Replay DS codes: pairs of 32-bit words, run once a frame from the
// ARM7's VBlank IRQ, where the real cartridge hooks itself. Writes go
// through the bus's DMA accessors (uncharged for CPU cycles, but still
// invalidate JIT blocks).
struct Code {
  std::string name;
  std::string description;   // often empty
  int group = -1;            // index into GameCheats::groups, -1 for none
  bool enabled = false;
  bool analog = false;       // a CPAD analog controls patch
  std::vector<u32> words;    // an even number; an odd tail is ignored

  bool is_note() const { return words.empty(); }
  // The variant that adds the second stick and ZL/ZR (see NDS::analog_full).
  bool analog_nub() const { return analog && name.find("Nub") != std::string::npos; }
};

// Whether an analog code's hook expects the extended layout at 0x027FFDF0
// (see NDS::AnalogLayout): such a hook takes the nub as signed halfwords at
// +6/+8, and a packed one takes the word at +4. Read from the ARM in its bulk
// writes; a pattern, so pad.analog_layout can still overrule it.
bool reads_extended_analog(const Code& c);

// What the enabled codes ask of the analog channel.
struct AnalogRequest {
  int schemes = 0;           // enabled analog codes; more than one will fight
  bool nub = false;          // one is the second-stick variant
  bool extended = false;     // one reads the extended layout
  const char* name = nullptr;
};
AnalogRequest analog_request(const std::vector<Code>& codes);

enum class Stop : u8 {
  Ok,
  BadOpcode,
  Truncated,   // an operand or a block ran past the end of the words
  Unsupported, // C2 (native code injection) or C4 (self-modifying code)
  RunawayLoop,
};
const char* stop_name(Stop s);

class Engine {
public:
  std::vector<Code> codes;

  void run(NDS& nds);
  Stop run_code(NDS& nds, const Code& code);

  static constexpr u64 MAX_STEPS = 1u << 20;

private:
  // Logged once per code, not once per frame, else a bad code buries the log.
  std::vector<u8> complained_;
};

} // namespace ds::cheat
