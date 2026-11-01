// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ds { struct NDS; }
namespace ds::state { class Writer; class Reader; }

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
  bool analog = false;       // a CPAD analog controls patch: journaled, and undone when disabled
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
  Stop run_code(NDS& nds, const Code& code);   // once, unjournaled
  // Nothing for run() to do, so the VBlank hook can skip it.
  bool idle() const { return codes.empty() && carried_.empty(); }

  // An analog code's writes are journaled per byte: the first time it writes
  // an address, the byte it held is kept until every code that wrote that
  // address has been disabled. When run() sees one go from enabled to
  // disabled, the bytes that were solely its own go back to those originals,
  // so its branch sites and hook body leave no trace, unless a byte no longer
  // holds what the code wrote: then an overlay or the hook's own variables
  // have moved on, and it is left alone. Other codes are not
  // journaled: what they write is game data the game goes on changing, and
  // putting an old value back would be wrong. Originals are read from guest
  // memory, so they are only meaningful for the machine state they were taken
  // in: forget them, without restoring, when the code list is replaced or the
  // machine resets.
  void forget_writes();

  // A save state holds guest RAM with every installed hook in it, so the
  // journal saying what those bytes were travels with it, keyed by each
  // code's words: the code list may differ when the state comes back, and a
  // revised code must not inherit its predecessor's hook. The next run()
  // keeps what enabled codes own and restores the rest. A state with no chunk
  // has nothing to restore. Without this, a hook saved in RAM outlives its
  // cheat, and an install guard can mistake another scheme's hook for its own.
  void save_journal(state::Writer& w) const;
  void load_journal(state::Reader& r);   // call after forget_writes()

  static constexpr u64 MAX_STEPS = 1u << 20;

private:
  Stop run_tracked(NDS& nds, const Code& code, size_t index);
  void revert(NDS& nds, size_t index);
  void settle_carried(NDS& nds);

  // Logged once per code, not once per frame, else a bad code buries the log.
  std::vector<u8> complained_;
  std::vector<char> was_enabled_;
  struct Original {
    u8 value = 0;                  // before any code wrote the byte
    u8 wrote = 0;                  // what the last write left there
    std::vector<size_t> writers;   // indices into `codes`; nearly always one
  };
  void restore(NDS& nds, u32 addr, const Original& o);
  std::unordered_map<u32, Original> originals_;
  // Writers a loaded state names, by a hash of their words, until the next
  // run() matches them against the enabled codes.
  struct Carried {
    u32 id = 0;
    std::vector<u32> addrs;
  };
  std::vector<Carried> carried_;
};

} // namespace ds::cheat
