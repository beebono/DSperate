// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/cheat/ar_engine.h"
#include "core/nds.h"
#include "core/state/state.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace ds::cheat {
namespace {

constexpr Cpu kCpu = Cpu::ARM7;   // the real cartridge runs off the ARM7's VBlank handler

u32 ror32(u32 v, u32 n) { n &= 31; return n ? ((v >> n) | (v << (32 - n))) : v; }

// FNV-1a over a code's words.
u32 code_id(const Code& c) {
  u32 h = 2166136261u;
  for (const u32 w : c.words) h = (h ^ w) * 16777619u;
  return h;
}

} // namespace

bool reads_extended_analog(const Code& c) {
  const std::vector<u32>& w = c.words;
  for (size_t pc = 0; pc + 1 < w.size();) {
    const u32 a = w[pc], b = w[pc + 1];
    pc += 2;
    if ((a >> 28) != 0xE) continue;
    const size_t end = std::min<u64>(pc + (static_cast<u64>(b) + 7) / 8 * 2, w.size());
    for (; pc < end; ++pc) {
      // ldrsh rd, [rn, #imm]
      if ((w[pc] & 0x0FF000F0) != 0x01D000F0) continue;
      const u32 imm = ((w[pc] >> 4) & 0xF0) | (w[pc] & 0xF);
      if (imm == 6 || imm == 8) return true;
    }
  }
  return false;
}

AnalogRequest analog_request(const std::vector<Code>& codes) {
  AnalogRequest r;
  for (const Code& c : codes) {
    if (!c.enabled || !c.analog) continue;
    ++r.schemes;
    r.nub = r.nub || c.analog_nub();
    r.extended = r.extended || reads_extended_analog(c);
    r.name = c.name.c_str();
  }
  return r;
}

const char* stop_name(Stop s) {
  switch (s) {
  case Stop::Ok:          return "ok";
  case Stop::BadOpcode:   return "bad opcode";
  case Stop::Truncated:   return "code ends mid-instruction";
  case Stop::Unsupported: return "unsupported opcode C4";
  case Stop::RunawayLoop: return "runaway loop";
  }
  return "?";
}

// `words` read two at a time: `a` carries the opcode in its top byte, `b` the
// operand. Skipped while the condition flag is clear, except opcodes that
// manipulate the flag itself.
Stop Engine::run_tracked(NDS& nds, const Code& code, size_t index) {
  mem::Bus& bus = nds.bus;
  const std::vector<u32>& w = code.words;
  const size_t n = w.size() & ~size_t{1};   // an odd tail cannot form a pair

  size_t pc = 0;
  u32 offset = 0, datareg = 0;
  u32 cond = 1, condstack = 0;
  size_t loopstart = 0;
  u32 loopcount = 0, loopcond = 1, loopcondstack = 0;
  u32 c5count = 0;
  u64 steps = 0;

  const auto read32 = [&](u32 addr) { return bus.dma_read32(kCpu, addr); };
  const auto read16 = [&](u32 addr) { return bus.dma_read16(kCpu, addr); };
  const auto read8  = [&](u32 addr) { return bus.dma_read8(kCpu, addr); };

  // Every write goes through these, so the engine can undo the code: the
  // first time this code touches a byte, the byte's current value joins the
  // journal (see Engine::revert), and the value it leaves there is noted.
  const bool track = index != SIZE_MAX && code.analog;
  const auto journaled = [&](u32 addr, u32 len, auto&& store) {
    if (!track) { store(); return; }
    for (u32 a = addr; a < addr + len; ++a) {
      auto [it, fresh] = originals_.try_emplace(a);
      if (fresh) it->second.value = bus.dma_read8(kCpu, a);
      std::vector<size_t>& writers = it->second.writers;
      if (std::find(writers.begin(), writers.end(), index) == writers.end()) writers.push_back(index);
    }
    store();
    for (u32 a = addr; a < addr + len; ++a) originals_[a].wrote = bus.dma_read8(kCpu, a);
  };
  // Aligned as the bus aligns them, or the journal keeps the wrong bytes.
  const auto write32 = [&](u32 addr, u32 v) { journaled(addr & ~3u, 4, [&] { bus.dma_write32(kCpu, addr, v); }); };
  const auto write16 = [&](u32 addr, u16 v) { journaled(addr & ~1u, 2, [&] { bus.dma_write16(kCpu, addr, v); }); };
  const auto write8  = [&](u32 addr, u8 v)  { journaled(addr, 1, [&] { bus.dma_write8(kCpu, addr, v); }); };

  while (pc + 1 < n) {
    if (++steps > MAX_STEPS) return Stop::RunawayLoop;
    const u32 a = w[pc], b = w[pc + 1];
    pc += 2;
    const u8 op = static_cast<u8>(a >> 24);
    const u8 hi = static_cast<u8>(op >> 4);

    // D0/D1/D2 and C5 run regardless (they restore the flag, end a loop, or
    // count); a skipped block-write must still step over its data.
    if ((op < 0xD0 && op != 0xC5) || op > 0xD2) {
      if (!cond) {
        if (hi == 0xE) {
          const u64 pairs = (static_cast<u64>(b) + 7) / 8;   // the data words that follow
          if (pc + pairs * 2 > n) return Stop::Truncated;
          pc += static_cast<size_t>(pairs * 2);
        }
        continue;
      }
    }

    // Address from `a`, or the offset register when `a`'s address field is zero.
    const auto cond_addr = [&] { const u32 addr = a & 0x0FFFFFFF; return addr ? addr : offset; };
    const auto push_cond = [&](bool taken) { condstack = (condstack << 1) | cond; cond = taken ? 1u : 0u; };

    switch (hi) {
    case 0x0: write32((a & 0x0FFFFFFF) + offset, b); break;
    case 0x1: write16((a & 0x0FFFFFFF) + offset, static_cast<u16>(b)); break;
    case 0x2: write8((a & 0x0FFFFFFF) + offset, static_cast<u8>(b)); break;

    case 0x3: push_cond(b >  read32(cond_addr())); break;
    case 0x4: push_cond(b <  read32(cond_addr())); break;
    case 0x5: push_cond(b == read32(cond_addr())); break;
    case 0x6: push_cond(b != read32(cond_addr())); break;

    // Masks the loaded halfword with ~b.h first, so a code can test a few bits.
    case 0x7: case 0x8: case 0x9: case 0xA: {
      const u16 val = static_cast<u16>(read16(cond_addr()) & ~static_cast<u16>(b >> 16));
      const u16 chk = static_cast<u16>(b);
      push_cond(hi == 0x7 ? chk >  val
              : hi == 0x8 ? chk <  val
              : hi == 0x9 ? chk == val
                          : chk != val);
      break;
    }

    case 0xB: offset = read32((a & 0x0FFFFFFF) + offset); break;

    case 0xC:
      switch (op) {
      case 0xC0:   // FOR 0..b, with the flag state to restore when it ends
        loopstart = pc;
        loopcount = b;
        loopcond = cond;
        loopcondstack = condstack;
        break;
      case 0xC2: return Stop::Unsupported;   // flashcard native-code extension
      case 0xC4: return Stop::Unsupported;   // self-modifying code
      case 0xC5:   // count++, then test it -- counted even while skipping
        ++c5count;
        if (!cond) break;
        push_cond((c5count & (b & 0xFFFF)) == (b >> 16));
        break;
      case 0xC6: write32(b, offset); break;
      default: return Stop::BadOpcode;
      }
      break;

    case 0xD:
      switch (op) {
      case 0xD0: cond = condstack & 1; condstack >>= 1; break;   // ENDIF
      case 0xD1:                                                 // NEXT
        if (loopcount > 0) { --loopcount; pc = loopstart; }
        else { cond = loopcond; condstack = loopcondstack; }
        break;
      case 0xD2:                                                 // NEXT, then reset everything
        if (loopcount > 0) { --loopcount; pc = loopstart; }
        else { offset = 0; datareg = 0; condstack = 0; cond = 1; }
        break;
      case 0xD3: offset = b; break;
      case 0xD4:                                                 // datareg <op>= b
        switch (a & 0xFF) {
        case 0x00: datareg += b; break;
        case 0x01: datareg |= b; break;
        case 0x02: datareg &= b; break;
        case 0x03: datareg ^= b; break;
        case 0x04: datareg = (b & 0xFF) > 31 ? 0 : datareg << (b & 0xFF); break;
        case 0x05: datareg = (b & 0xFF) > 31 ? 0 : datareg >> (b & 0xFF); break;
        case 0x06: datareg = ror32(datareg, b); break;
        case 0x07: datareg = (b & 0xFF) > 31 ? static_cast<u32>(static_cast<s32>(datareg) >> 31)
                                             : static_cast<u32>(static_cast<s32>(datareg) >> (b & 0xFF));
                   break;
        case 0x08: datareg *= b; break;
        default: return Stop::BadOpcode;
        }
        break;
      case 0xD5: datareg = b; break;
      case 0xD6: write32(b + offset, datareg); offset += 4; break;
      case 0xD7: write16(b + offset, static_cast<u16>(datareg)); offset += 2; break;
      case 0xD8: write8(b + offset, static_cast<u8>(datareg)); offset += 1; break;
      case 0xD9: datareg = read32(b + offset); break;
      case 0xDA: datareg = read16(b + offset); break;
      case 0xDB: datareg = read8(b + offset); break;
      case 0xDC: offset += b; break;
      default: return Stop::BadOpcode;
      }
      break;

    // Copy b bytes to a+offset. Packed two words per eight bytes; a trailing
    // 1-7 byte run still costs a whole pair.
    case 0xE: {
      const u64 pairs = (static_cast<u64>(b) + 7) / 8;
      if (pc + pairs * 2 > n) return Stop::Truncated;
      u32 dst = (a & 0x0FFFFFFF) + offset;
      u32 left = b;
      while (left >= 8) {
        write32(dst, w[pc]);     dst += 4;
        write32(dst, w[pc + 1]); dst += 4;
        pc += 2; left -= 8;
      }
      if (left > 0) {
        // Unpacked by hand, little-endian: the words are u32, host byte order
        // is not the code's business.
        const u8 bytes[8] = {
          static_cast<u8>(w[pc]), static_cast<u8>(w[pc] >> 8), static_cast<u8>(w[pc] >> 16), static_cast<u8>(w[pc] >> 24),
          static_cast<u8>(w[pc + 1]), static_cast<u8>(w[pc + 1] >> 8), static_cast<u8>(w[pc + 1] >> 16), static_cast<u8>(w[pc + 1] >> 24)};
        pc += 2;
        u32 at = 0;
        if (left >= 4) {
          write32(dst, static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
                                     (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24));
          dst += 4; at = 4; left -= 4;
        }
        while (left > 0) { write8(dst++, bytes[at++]); --left; }
      }
      break;
    }

    // Copy b bytes from the offset register to a (source is guest memory).
    case 0xF: {
      u32 src = offset, dst = a & 0x0FFFFFFF, left = b;
      for (; left >= 4; left -= 4, src += 4, dst += 4) write32(dst, read32(src));
      for (; left > 0; --left, ++src, ++dst) write8(dst, read8(src));
      break;
    }

    default: return Stop::BadOpcode;
    }
  }
  return pc == n ? Stop::Ok : Stop::Truncated;
}

Stop Engine::run_code(NDS& nds, const Code& code) { return run_tracked(nds, code, SIZE_MAX); }

void Engine::forget_writes() {
  originals_.clear();
  was_enabled_.clear();
  carried_.clear();
}

void Engine::save_journal(state::Writer& w) const {
  if (originals_.empty() && carried_.empty()) return;   // no cheat, no chunk: the state is unchanged
  std::vector<u32> addrs;
  addrs.reserve(originals_.size());
  for (const auto& entry : originals_) addrs.push_back(entry.first);
  std::sort(addrs.begin(), addrs.end());   // the same machine saves the same bytes
  std::vector<u8> values, wrote;
  std::map<size_t, std::vector<u32>> by_code;
  for (const u32 addr : addrs) {
    const Original& o = originals_.at(addr);
    values.push_back(o.value);
    wrote.push_back(o.wrote);
    for (const size_t i : o.writers) by_code[i].push_back(addr);
  }
  std::vector<Carried> writers = carried_;
  for (auto& [i, mine] : by_code) writers.push_back({code_id(codes[i]), std::move(mine)});

  w.begin("CHJR");
  w.vec(addrs);
  w.vec(values);
  w.vec(wrote);
  w.put(static_cast<u32>(writers.size()));
  for (const Carried& c : writers) {
    w.put(c.id);
    w.vec(c.addrs);
  }
  w.end();
}

void Engine::load_journal(state::Reader& r) {
  if (!r.begin("CHJR")) return;
  std::vector<u32> addrs;
  std::vector<u8> values, wrote;
  r.vec(addrs);
  r.vec(values);
  r.vec(wrote);
  if (addrs.size() != values.size() || addrs.size() != wrote.size()) { r.fail("cheat journal: addresses and values differ"); return; }
  for (size_t i = 0; i < addrs.size(); ++i) {
    Original& o = originals_[addrs[i]];
    o.value = values[i];
    o.wrote = wrote[i];
  }
  u32 n = 0;
  r.put(n);
  for (u32 i = 0; i < n && r.ok(); ++i) {
    Carried c;
    r.put(c.id);
    r.vec(c.addrs);
    carried_.push_back(std::move(c));
  }
  r.end();
}

// Only if the byte still holds what the code left there. If not, something
// has been loaded over it since, and the original belongs to what is gone.
void Engine::restore(NDS& nds, u32 addr, const Original& o) {
  if (nds.bus.dma_read8(kCpu, addr) == o.wrote) nds.bus.dma_write8(kCpu, addr, o.value);
}

// Writers still enabled keep their bytes; a disabled or vanished writer's
// bytes go back, unless an enabled writer shares them. Enabled first, so
// the sharing is known before anything is restored.
void Engine::settle_carried(NDS& nds) {
  std::vector<const Carried*> gone;
  for (const Carried& c : carried_) {
    size_t i = 0;
    while (i < codes.size() && !(codes[i].enabled && code_id(codes[i]) == c.id)) ++i;
    if (i == codes.size()) { gone.push_back(&c); continue; }
    for (const u32 addr : c.addrs) originals_[addr].writers.push_back(i);
  }
  for (const Carried* c : gone)
    for (const u32 addr : c->addrs) {
      auto it = originals_.find(addr);
      if (it == originals_.end() || !it->second.writers.empty()) continue;
      restore(nds, addr, it->second);
      originals_.erase(it);
    }
  carried_.clear();
}

void Engine::revert(NDS& nds, size_t index) {
  for (auto it = originals_.begin(); it != originals_.end();) {
    std::vector<size_t>& writers = it->second.writers;
    const auto mine = std::find(writers.begin(), writers.end(), index);
    if (mine == writers.end()) { ++it; continue; }
    writers.erase(mine);
    if (!writers.empty()) { ++it; continue; }
    restore(nds, it->first, it->second);
    it = originals_.erase(it);
  }
}

void Engine::run(NDS& nds) {
  if (!carried_.empty()) settle_carried(nds);
  if (codes.empty()) return;
  if (complained_.size() != codes.size()) complained_.assign(codes.size(), 0);
  if (was_enabled_.size() != codes.size()) was_enabled_.assign(codes.size(), 0);
  // Undo before running anything: a code switched off this frame leaves
  // the machine clean, and codes still on re-apply over the originals.
  for (size_t i = 0; i < codes.size(); ++i) {
    if (was_enabled_[i] && !codes[i].enabled) revert(nds, i);
    was_enabled_[i] = codes[i].enabled;
  }
  for (size_t i = 0; i < codes.size(); ++i) {
    const Code& c = codes[i];
    if (!c.enabled) continue;
    const Stop why = run_tracked(nds, c, i);
    // Say so once; it stops at the same place every frame after.
    if (why != Stop::Ok && !complained_[i]) {
      complained_[i] = 1;
      std::fprintf(stderr, "cheat: \"%s\" stopped: %s\n", c.name.c_str(), stop_name(why));
    }
  }
}

} // namespace ds::cheat
