// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The Action Replay interpreter: every opcode family, the condition stack,
// the loop, and the ways a malformed code has to fail rather than run away.
#include "core/nds.h"
#include "core/cheat/ar_engine.h"
#include "core/state/state.h"
#include "check.h"

#include <initializer_list>
#include <iterator>
#include <memory>
#include <vector>

using namespace ds;
using cheat::Stop;

namespace {

constexpr u32 RAM = 0x02000000;

u32 r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM7, a); }
u16 r16(NDS& nds, u32 a) { return nds.bus.dma_read16(Cpu::ARM7, a); }
u8  r8 (NDS& nds, u32 a) { return nds.bus.dma_read8 (Cpu::ARM7, a); }
void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM7, a, v); }

Stop run(NDS& nds, std::initializer_list<u32> words) {
  cheat::Engine e;
  cheat::Code c;
  c.name = "test";
  c.words.assign(words);
  return e.run_code(nds, c);
}

// The address field of a write opcode is 28 bits, so main RAM is named by
// its offset from 0x02000000 with the opcode in the top byte.
constexpr u32 at(u32 op, u32 addr) { return (op << 24) | (addr & 0x0FFFFFFF); }

void test_writes() {
  NDS nds;
  CHECK(run(nds, {at(0x00, 0x2000000), 0xCAFEBABE,
                  at(0x10, 0x2000010), 0x1234,
                  at(0x20, 0x2000020), 0x56}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x00) == 0xCAFEBABE);
  CHECK(r16(nds, RAM + 0x10) == 0x1234);
  CHECK(r8 (nds, RAM + 0x20) == 0x56);
  // Writes are relative to the offset register (D3 sets it).
  CHECK(run(nds, {0xD3000000, RAM + 0x100, at(0x00, 0x0000004), 0xAABBCCDD}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x104) == 0xAABBCCDD);
}

// Each 32-bit conditional, and the rule that a zero address field means the
// offset register rather than address zero.
void test_conditionals_32() {
  NDS nds;
  w32(nds, RAM, 100);
  const u32 store = at(0x00, 0x2000040);
  // b > mem, b < mem, b == mem, b != mem
  CHECK(run(nds, {at(0x30, 0x2000000), 200, store, 1, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);
  CHECK(run(nds, {at(0x30, 0x2000000), 50, store, 2, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);                       // not taken, no write
  CHECK(run(nds, {at(0x40, 0x2000000), 50, store, 3, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 3);
  CHECK(run(nds, {at(0x50, 0x2000000), 100, store, 4, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 4);
  CHECK(run(nds, {at(0x60, 0x2000000), 100, store, 5, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 4);                       // equal, so != is false
  // Address field zero: the offset register supplies the address, and is not
  // added to it.
  // The offset is cleared again before the store, which would otherwise add it.
  CHECK(run(nds, {0xD3000000, RAM, at(0x50, 0), 100, 0xD3000000, 0, store, 6, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 6);
}

// The 16-bit conditionals mask the loaded halfword with ~b.h before testing.
void test_conditionals_16() {
  NDS nds;
  w32(nds, RAM, 0x0000FF37);
  const u32 store = at(0x00, 0x2000040);
  // ~mask 0xFF00 leaves 0x0037; equal to 0x0037.
  CHECK(run(nds, {at(0x90, 0x2000000), 0xFF000037, store, 1, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);
  // Without the mask the halfword is 0xFF37, so the same test fails.
  CHECK(run(nds, {at(0x90, 0x2000000), 0x00000037, store, 2, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 1);
  CHECK(run(nds, {at(0xA0, 0x2000000), 0x00000037, store, 3, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 3);
  CHECK(run(nds, {at(0x70, 0x2000000), 0xFF000038, store, 4, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 4);                       // 0x38 > 0x37
  CHECK(run(nds, {at(0x80, 0x2000000), 0xFF000036, store, 5, 0xD0000000, 0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 5);                       // 0x36 < 0x37
}

// Nesting works the ordinary way as long as the outer condition holds: the
// inner one pushes, and its ENDIF restores the outer state.
void test_nested_conditions() {
  NDS nds;
  w32(nds, RAM, 1);
  w32(nds, RAM + 0x40, 0);
  CHECK(run(nds, {at(0x50, 0x2000000), 1,                 // outer: true
                  at(0x50, 0x2000000), 999,               // inner: false
                  at(0x00, 0x2000040), 7,                 // skipped
                  0xD0000000, 0,                          // ENDIF: back to the outer true
                  at(0x00, 0x2000044), 8,
                  0xD0000000, 0,
                  at(0x00, 0x2000048), 9}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x40) == 0);
  CHECK(r32(nds, RAM + 0x44) == 8);
  CHECK(r32(nds, RAM + 0x48) == 9);
}

// A conditional reached while the flag is clear is skipped outright, so it
// never pushes -- but ENDIF pops regardless. A code that nests inside a false
// condition therefore pops a frame that was never pushed and stays disabled
// for the rest of the run. That is what the hardware does and what melonDS
// does; it is pinned here because it looks like a bug when a code misbehaves.
void test_skipped_conditional_does_not_push() {
  NDS nds;
  w32(nds, RAM, 1);
  w32(nds, RAM + 0x50, 0);
  CHECK(run(nds, {at(0x50, 0x2000000), 999,               // false
                  at(0x50, 0x2000000), 1,                 // skipped, so not pushed
                  0xD0000000, 0,                          // pops the outer frame: true again
                  at(0x00, 0x2000050), 5,                 // so this one runs
                  0xD0000000, 0,                          // pops an empty stack: false
                  at(0x00, 0x2000054), 6}) == Stop::Ok);  // and this one does not
  CHECK(r32(nds, RAM + 0x50) == 5);
  CHECK(r32(nds, RAM + 0x54) == 0);
}

void test_loop() {
  NDS nds;
  w32(nds, RAM + 0x40, 0);
  // FOR 0..3 { datareg += 1 }  -- four passes in total (b, then b-1..0).
  CHECK(run(nds, {0xD5000000, 0,                          // datareg = 0
                  0xC0000000, 3,                          // FOR
                  0xD4000000, 1,                          // datareg += 1
                  0xD1000000, 0,                          // NEXT
                  0xD6000000, RAM + 0x40}) == Stop::Ok);  // u32[b] = datareg
  CHECK(r32(nds, RAM + 0x40) == 4);
}

// D2 ends a loop like D1 but then clears the offset, the data register and
// the condition stack.
void test_loop_flush() {
  NDS nds;
  CHECK(run(nds, {0xD3000000, RAM + 0x200,                // offset = ...
                  0xD5000000, 0x99,                       // datareg = 0x99
                  0xC0000000, 0,                          // FOR 0..0 (one pass)
                  0xD2000000, 0,                          // NEXT+FLUSH
                  at(0x00, 0x2000050), 1}) == Stop::Ok);
  // The write landed at 0x2000050, not 0x2000050 + old offset.
  CHECK(r32(nds, RAM + 0x50) == 1);
}

void test_data_register() {
  NDS nds;
  const u32 out = 0xD6000000, dst = RAM + 0x60;
  CHECK(run(nds, {0xD5000000, 0x0F0F0F0F, 0xD4000001, 0xF0F0F0F0, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0xFFFFFFFF);                     // OR
  CHECK(run(nds, {0xD5000000, 0xFF00FF00, 0xD4000002, 0x0FF00FF0, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x0F000F00);                     // AND
  CHECK(run(nds, {0xD5000000, 0xFFFF0000, 0xD4000003, 0xFF00FF00, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x00FFFF00);                     // XOR
  CHECK(run(nds, {0xD5000000, 1, 0xD4000004, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x10);                           // shift left
  CHECK(run(nds, {0xD5000000, 0x80, 0xD4000005, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x8);                            // shift right
  CHECK(run(nds, {0xD5000000, 0x00000001, 0xD4000006, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0x10000000);                     // rotate right
  CHECK(run(nds, {0xD5000000, 0xFFFFFFF0, 0xD4000007, 4, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0xFFFFFFFF);                     // arithmetic shift right
  CHECK(run(nds, {0xD5000000, 7, 0xD4000008, 6, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 42);                             // multiply
  // A shift of more than 31 clears the register; the arithmetic one saturates.
  CHECK(run(nds, {0xD5000000, 0xFF, 0xD4000004, 99, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0);
  CHECK(run(nds, {0xD5000000, 0x80000000, 0xD4000007, 99, out, dst}) == Stop::Ok);
  CHECK(r32(nds, dst) == 0xFFFFFFFF);
}

// The sized stores advance the offset by their own width; the loads do not.
void test_datareg_memory() {
  NDS nds;
  w32(nds, RAM + 0x80, 0x11223344);
  // Both the load and the store add the offset to their operand.
  CHECK(run(nds, {0xD3000000, 0x80, 0xD9000000, RAM, 0xD6000000, RAM + 0x90}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x110) == 0x11223344);             // 0x90 + the 0x80 offset
  CHECK(run(nds, {0xD3000000, 0, 0xDA000000, RAM + 0x80, 0xD6000000, RAM + 0x94}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x94) == 0x3344);                  // 16-bit load
  CHECK(run(nds, {0xD3000000, 0, 0xDB000000, RAM + 0x80, 0xD6000000, RAM + 0x98}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x98) == 0x44);                    // 8-bit load
  // Three stores from offset 0 land at +0, +4 and +6 as the offset advances.
  CHECK(run(nds, {0xD3000000, 0, 0xD5000000, 0xAABBCCDD,
                  0xD6000000, RAM + 0xA0,                 // 32-bit, offset += 4
                  0xD7000000, RAM + 0xA0,                 // 16-bit at +4, offset += 2
                  0xD8000000, RAM + 0xA0}) == Stop::Ok);  // 8-bit at +6
  CHECK(r32(nds, RAM + 0xA0) == 0xAABBCCDD);
  CHECK(r16(nds, RAM + 0xA4) == 0xCCDD);
  CHECK(r8 (nds, RAM + 0xA6) == 0xDD);
}

void test_offset_ops() {
  NDS nds;
  w32(nds, RAM + 0xB0, RAM + 0xC0);
  // B0: offset = u32[a + offset]
  CHECK(run(nds, {at(0xB0, 0x20000B0), 0, at(0x00, 0), 0x5A5A5A5A}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0xC0) == 0x5A5A5A5A);
  // DC adds to the offset; C6 stores it.
  CHECK(run(nds, {0xD3000000, 0x100, 0xDC000000, 0x23, 0xC6000000, RAM + 0xD0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0xD0) == 0x123);
}

// C5 counts every time it is reached, and tests the count against b.
void test_c5_counter() {
  NDS nds;
  w32(nds, RAM + 0xE0, 0);
  // Four passes; the write runs only when (count & 1) == 1, i.e. on 1 and 3.
  CHECK(run(nds, {0xD5000000, 0,
                  0xC0000000, 3,
                  0xC5000000, 0x00010001,
                  0xD4000000, 1,
                  0xD0000000, 0,
                  0xD1000000, 0,
                  0xD6000000, RAM + 0xE0}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0xE0) == 2);
}

void test_block_write() {
  NDS nds;
  // Eight bytes: two whole words, no tail.
  CHECK(run(nds, {at(0xE0, 0x2000100), 8, 0x11223344, 0x55667788}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x100) == 0x11223344);
  CHECK(r32(nds, RAM + 0x104) == 0x55667788);
  // Five bytes: a word and a byte, still costing a whole pair of data words.
  CHECK(run(nds, {at(0xE0, 0x2000110), 5, 0xAABBCCDD, 0x000000EE,
                  at(0x00, 0x2000120), 0x600D}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x110) == 0xAABBCCDD);
  CHECK(r8 (nds, RAM + 0x114) == 0xEE);
  CHECK(r32(nds, RAM + 0x120) == 0x600D);                 // the pair was stepped over exactly
  // Three bytes: no word, three bytes, one pair.
  CHECK(run(nds, {at(0xE0, 0x2000130), 3, 0x00332211, 0,
                  at(0x00, 0x2000140), 0xF00D}) == Stop::Ok);
  CHECK(r8(nds, RAM + 0x130) == 0x11);
  CHECK(r8(nds, RAM + 0x131) == 0x22);
  CHECK(r8(nds, RAM + 0x132) == 0x33);
  CHECK(r32(nds, RAM + 0x140) == 0xF00D);
}

// A skipped block write must step over its data, or the words are executed.
void test_block_write_skipped() {
  NDS nds;
  w32(nds, RAM, 1);
  w32(nds, RAM + 0x150, 0);
  CHECK(run(nds, {at(0x50, 0x2000000), 999,               // false
                  at(0xE0, 0x2000150), 8, 0xDEADBEEF, 0xDEADBEEF,
                  0xD0000000, 0,
                  at(0x00, 0x2000160), 0xBEEF}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x150) == 0);                      // the block did not run
  CHECK(r32(nds, RAM + 0x160) == 0xBEEF);                 // and the data was not executed
}

void test_block_copy() {
  NDS nds;
  w32(nds, RAM + 0x180, 0x01020304);
  w32(nds, RAM + 0x184, 0x05060708);
  CHECK(run(nds, {0xD3000000, RAM + 0x180, at(0xF0, 0x2000190), 8}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x190) == 0x01020304);
  CHECK(r32(nds, RAM + 0x194) == 0x05060708);
  // A length that is not a multiple of four finishes byte by byte.
  CHECK(run(nds, {0xD3000000, RAM + 0x180, at(0xF0, 0x20001A0), 6}) == Stop::Ok);
  CHECK(r32(nds, RAM + 0x1A0) == 0x01020304);
  CHECK(r16(nds, RAM + 0x1A4) == 0x0708);
}

// Malformed codes must stop, not run off the end or spin for ever.
void test_malformed() {
  NDS nds;
  // An opcode that does not exist.
  CHECK(run(nds, {0xC1000000, 0}) == Stop::BadOpcode);
  CHECK(run(nds, {0xDD000000, 0}) == Stop::BadOpcode);
  CHECK(run(nds, {0xD400000F, 0}) == Stop::BadOpcode);    // no such data op
  // C4 is refused rather than guessed at.
  CHECK(run(nds, {0xC4000000, 0}) == Stop::Unsupported);
  // A block write whose data is not there.
  CHECK(run(nds, {at(0xE0, 0x2000100), 64, 0, 0}) == Stop::Truncated);
  // The same, while skipping.
  w32(nds, RAM, 1);
  CHECK(run(nds, {at(0x50, 0x2000000), 999, at(0xE0, 0x2000100), 64, 0, 0}) == Stop::Truncated);
  // A loop with no end runs into the step cap instead of hanging.
  CHECK(run(nds, {0xC0000000, 0xFFFFFFFF, 0xD1000000, 0}) == Stop::RunawayLoop);
  // An odd trailing word cannot form a pair and is ignored.
  CHECK(run(nds, {at(0x00, 0x2000000), 0x1234, 0xDEADBEEF}) == Stop::Ok);
  CHECK(r32(nds, RAM) == 0x1234);
  // An empty code is fine.
  CHECK(run(nds, {}) == Stop::Ok);
}

// The engine runs enabled codes in order and leaves the disabled ones alone.
void test_engine_enable() {
  NDS nds;
  w32(nds, RAM + 0x1C0, 0);
  w32(nds, RAM + 0x1C4, 0);
  cheat::Engine e;
  // Named fields: Code gained a description and a group when the database
  // loader landed, and a positional list would silently follow it.
  cheat::Code on;  on.name = "on";   on.enabled = true;  on.words = {at(0x00, 0x20001C0), 1};
  cheat::Code off; off.name = "off"; off.enabled = false; off.words = {at(0x00, 0x20001C4), 1};
  e.codes.push_back(on);
  e.codes.push_back(off);
  e.run(nds);
  CHECK(r32(nds, RAM + 0x1C0) == 1);
  CHECK(r32(nds, RAM + 0x1C4) == 0);
  // Codes run every call, so a later frame sees the write again.
  w32(nds, RAM + 0x1C0, 0);
  e.run(nds);
  CHECK(r32(nds, RAM + 0x1C0) == 1);
}

// The hook: codes run when the ARM7 takes its VBlank IRQ, and only then.
// Every path that takes an IRQ -- interpreter, JIT and the scheduler's
// between-slices check -- goes through CpuContext::check_irq, so this covers
// all three.
void arm_vblank(NDS& nds) {
  CpuContext& arm7 = nds.cpu(Cpu::ARM7);
  arm7.hot.cpsr &= ~0x80u;                                   // IRQs unmasked
  nds.io.cpu_io[1].ime = 1;
  nds.io.cpu_io[1].ie = 1u << io::IRQ_VBLANK;
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_VBLANK, true);      // the GPU's own path
  CHECK(arm7.hot.irq_pending);
}

void test_hook_runs_on_arm7_vblank() {
  NDS nds;
  w32(nds, RAM + 0x200, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x2000200), 0x1234};
  nds.cheats.codes.push_back(c);

  arm_vblank(nds);
  nds.cpu(Cpu::ARM7).check_irq();
  CHECK(r32(nds, RAM + 0x200) == 0x1234);

  // A disabled code is not run by the hook either.
  w32(nds, RAM + 0x200, 0);
  nds.cheats.codes[0].enabled = false;
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_VBLANK, true);
  nds.cpu(Cpu::ARM7).hot.cpsr &= ~0x80u;
  nds.cpu(Cpu::ARM7).check_irq();
  CHECK(r32(nds, RAM + 0x200) == 0);
}

// The ARM9 takes VBlank too, and must not run the codes: on hardware the
// cartridge hooks the ARM7's handler, and running them twice a frame would
// double every "add one" style code.
void test_hook_ignores_arm9() {
  NDS nds;
  w32(nds, RAM + 0x204, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x2000204), 0x99};
  nds.cheats.codes.push_back(c);

  CpuContext& arm9 = nds.cpu(Cpu::ARM9);
  arm9.hot.cpsr &= ~0x80u;
  nds.io.cpu_io[0].ime = 1;
  nds.io.cpu_io[0].ie = 1u << io::IRQ_VBLANK;
  nds.io.set_irq_line(Cpu::ARM9, io::IRQ_VBLANK, true);
  arm9.check_irq();
  CHECK(r32(nds, RAM + 0x204) == 0);
}

// Another IRQ on the ARM7 is not the cheats' cue.
void test_hook_ignores_other_irqs() {
  NDS nds;
  w32(nds, RAM + 0x208, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x2000208), 0x77};
  nds.cheats.codes.push_back(c);

  CpuContext& arm7 = nds.cpu(Cpu::ARM7);
  arm7.hot.cpsr &= ~0x80u;
  nds.io.cpu_io[1].ime = 1;
  nds.io.cpu_io[1].ie = 1u << io::IRQ_HBLANK;
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_HBLANK, true);
  arm7.check_irq();
  CHECK(r32(nds, RAM + 0x208) == 0);
}

// The SM64DS analog-controls patch (shocoman/Analog-Controls-for-NDS-Games-on-3DS),
// stripped for an emulator: only the ARM9 section of
// "ASME-AEA63749 (USA v1.0).txt" -- the conditional hook install guarded by the
// original instruction at the input-update inject address (0x0202B324). The ARM7
// RTCom section is omitted entirely; the emulator feeds 0x027FFDF0 directly, so
// nothing may write the ARM7 RTC code block (0x03801D84) or erase the RTC-init
// call (0x037F837C). The guard also makes re-running the code every frame cheap:
// after the first install the E-block is stepped over, never rewritten.
void test_analog_hook_sm64ds() {
  static constexpr u32 kHookAddr = 0x0202B324;   // INPUT_UPDATE_INJECT_ADDRESS
  static constexpr u32 kBase = 0x02004B00;       // BASE_PATCH_ADDR
  static constexpr u32 kWords[] = {
  0x5202B324u, 0xE7D01108u, 0xE2004B00u, 0x000001E0u, 0xE92D400Fu, 0xE59F11C4u,
  0xE59F01CCu, 0xE5810000u, 0xE8BD400Fu, 0xE12FFF1Eu, 0xE92D423Fu, 0xE59F9198u,
  0xE5990004u, 0xE1A03E80u, 0xE1A03F23u, 0xE59F0180u, 0xE58F317Cu, 0xE0232000u,
  0xE0022003u, 0xE1866602u, 0xE1877603u, 0xE3A03000u, 0xE5990004u, 0xE1A01800u,
  0xE1A01C41u, 0xE351000Cu, 0xC3833002u, 0xE371000Cu, 0xB3833001u, 0xE1A03403u,
  0xE59F915Cu, 0xE1D900B4u, 0xE1800003u, 0xE1C900B4u, 0xE59F913Cu, 0xE1D940B0u,
  0xE1A00004u, 0xEB00002Cu, 0xE3540000u, 0x08BD423Fu, 0x07D01108u, 0x059F0138u,
  0x012FFF10u, 0xE1A05804u, 0xE1A04C04u, 0xE1A04C44u, 0xE1A05C45u, 0xE2655000u,
  0xE0C10494u, 0xE0E10595u, 0xE59F9104u, 0xE12FFF39u, 0xE1B00600u, 0xE59F10ECu,
  0xE59F90F0u, 0xE12FFF39u, 0xE3500A01u, 0xC3A00A01u, 0xE59F90ECu, 0xE1C900B8u,
  0xE1B00004u, 0xE1B01005u, 0xE59F90D8u, 0xE12FFF39u, 0xE59F90D4u, 0xE1C900BEu,
  0xE1A00604u, 0xE59F10B4u, 0xE59F90B8u, 0xE12FFF39u, 0xE59F90BCu, 0xE1C900BAu,
  0xE1A00605u, 0xE59F109Cu, 0xE59F90A0u, 0xE12FFF39u, 0xE59F90A4u, 0xE1C900BCu,
  0xE3A00001u, 0xE5C90014u, 0xE8BD423Fu, 0xE59F009Cu, 0xE12FFF10u, 0xE92D40FFu,
  0xE59F5064u, 0xE59F605Cu, 0xE59F4054u, 0xE5942000u, 0xE3500000u, 0x1A000006u,
  0xE1520005u, 0x1A00000Fu, 0xE59F2048u, 0xE2522001u, 0xE58F2040u, 0xD5846000u,
  0xEA00000Au, 0xE3A07014u, 0xE58F7030u, 0xE1520006u, 0x1A000006u, 0xE15400BCu,
  0xE2400004u, 0xE7943000u, 0xE5D31000u, 0xE3C11004u, 0xE5C31000u, 0xE5845000u,
  0xE8BD80FFu, 0x020F0DACu, 0xE3811004u, 0xE1A08008u, 0x00000000u, 0x00000000u,
  0x00000069u, 0x027FFDF0u, 0x01FFABE4u, 0x0203B898u, 0x02039684u, 0x02097594u,
  0x0202B324u, 0x0202B328u, 0x0202B634u, 0xEAFF65FBu, 0x0202B324u, 0xEAFF65FBu,
  0xD2000000u, 0x00000000u,
  };
  cheat::Engine e;
  cheat::Code c;
  c.name = "CPAD (stripped)";
  c.words.assign(std::begin(kWords), std::end(kWords));
  CHECK(!cheat::reads_extended_analog(c));

  { // Guard not met (no original instruction): installs nothing, still Ok.
    NDS nds;
    CHECK(e.run_code(nds, c) == Stop::Ok);
    CHECK(r32(nds, kBase) == 0);
    CHECK(r32(nds, kHookAddr) == 0);
  }

  NDS nds;
  w32(nds, kHookAddr, 0xE7D01108);                 // the instruction the hook replaces
  w32(nds, 0x03801D84, 0x5A5A5A5A);                // ARM7 RTC code block: must survive
  w32(nds, 0x037F837C, 0xEB002867);                // ARM7 RTC-init call: must survive
  CHECK(e.run_code(nds, c) == Stop::Ok);
  CHECK(r32(nds, kBase) == 0xE92D400F);            // first hook word
  CHECK(r32(nds, kBase + 0x1BC) == 0x027FFDF0);    // RTCom_Output the hook reads
  CHECK(r32(nds, kBase + 0x1CC) == 0x02097594);    // CONTROLS_STRUCT_ADDRESS (USA v1.0)
  CHECK(r32(nds, kHookAddr) == 0xEAFF65FB);        // branch into LoadValueFromStick
  CHECK(r32(nds, 0x03801D84) == 0x5A5A5A5A);
  CHECK(r32(nds, 0x037F837C) == 0xEB002867);

  // Second frame: the guard now sees the branch, so the hook is not rewritten.
  w32(nds, kBase + 8, 0xDEADBEEF);
  CHECK(e.run_code(nds, c) == Stop::Ok);
  CHECK(r32(nds, kBase + 8) == 0xDEADBEEF);
  CHECK(r32(nds, kHookAddr) == 0xEAFF65FB);
}

// A VBlank that is pending but not enabled is not the handler running, so it
// is not the cheats' cue either.
void test_hook_needs_ie_and_if() {
  NDS nds;
  w32(nds, RAM + 0x20C, 0);
  cheat::Code c;
  c.name = "hook";
  c.enabled = true;
  c.words = {at(0x00, 0x200020C), 0x55};
  nds.cheats.codes.push_back(c);

  CpuContext& arm7 = nds.cpu(Cpu::ARM7);
  arm7.hot.cpsr &= ~0x80u;
  nds.io.cpu_io[1].ime = 1;
  nds.io.cpu_io[1].ie = 1u << io::IRQ_HBLANK;      // HBlank is what is enabled
  nds.io.cpu_io[1].if_ = 1u << io::IRQ_VBLANK;     // VBlank is merely pending
  nds.io.set_irq_line(Cpu::ARM7, io::IRQ_HBLANK, true);
  arm7.check_irq();
  CHECK(r32(nds, RAM + 0x20C) == 0);
}

// Disabling a code undoes it: every byte the code wrote while enabled
// goes back to what it held before the code first wrote it, so a code
// that patches game code (branch sites, an installed hook body) leaves
// no trace once switched off -- and switching it back on installs it
// again. Bytes another still-enabled code also wrote are that code's
// business and survive.
// The two Metroid Prime Hunters schemes (USA v1.1) exactly as ds-emu-dac ships
// them. Both hooks start with the same word, the only thing their install
// guards test, and they sit at different offsets in the same 0x02001000 region.
const u32 kMetroidLocal[] = {0x62001000u,0xE92D007Fu,0xD0000000u,0x00000000u,0x62001000u,0xE92D007Fu,0xE2001000u,0x00000400u,0xE92D007Fu,0xE1A06004u,0xE28E1B05u,0xE59110D8u,0xE5911000u,0xE1510005u,0x18BD007Fu,0x11A01F81u,0x112FFF1Eu,0xE59F53C0u,0xE1D540B0u,0xE3540000u,0x08BD007Fu,0x01A01F81u,0x012FFF1Eu,0xE1A05804u,0xE1A04C04u,0xE1A04C44u,0xE1A05C45u,0xE2655000u,0xE59F3028u,0xE0000493u,0xE0010593u,0xE1A00640u,0xE1A01641u,0xE1C602BAu,0xE1C612BCu,0xE3A01001u,0xE5C61034u,0xE8BD007Fu,0xE28EE008u,0xE12FFF1Eu,0x000004F1u,0x00000000u,0x00000000u,0xE1A08000u,0xE1D10EB4u,0xE92D40FFu,0xE28E1EDEu,0xE5912000u,0xE5922000u,0xE3520000u,0x1A000043u,0xE5911004u,0xE5911000u,0xE151000Au,0x1A00003Fu,0xE59F7328u,0xE1D740FAu,0xE597500Cu,0xE1A06845u,0xE1A05805u,0xE1A05845u,0xE1D870B4u,0xE1D800B6u,0xE3170040u,0x13100001u,0x114F46B8u,0x114F56BAu,0x114F66BCu,0xE3170020u,0x13100001u,0xE5DF20F9u,0x12822001u,0x12022001u,0x15CF20EDu,0xE5DF20E8u,0xE3170080u,0x13100001u,0x11E02002u,0x15CF20D8u,0xE3520000u,0x03A04000u,0x03A06000u,0x0A00000Fu,0xE15F0BF0u,0xE0444000u,0xE1B044C4u,0xB2844001u,0xE15F0BFEu,0xE0455000u,0xE1B054C5u,0xB2855001u,0xE2655000u,0xE15F0DF0u,0xE0466000u,0xE1B064C6u,0xB2866001u,0xE5DF2091u,0xE3520001u,0x01A06005u,0xEB000015u,0xE0844001u,0xE0866000u,0xE59F3080u,0xE0040493u,0xE1B04644u,0xB2844001u,0xE1D802BAu,0xE1D812BCu,0xE0800006u,0xE0811004u,0xE1C802BAu,0xE1C812BCu,0xE3560000u,0x03540000u,0x0A000003u,0xE1D810B0u,0xE3510000u,0x03A01A01u,0x01C810B0u,0xE8BD40FFu,0xE28EE008u,0xE12FFF1Eu,0xE59F221Cu,0xE1D200F6u,0xE1D210F8u,0xE2611000u,0xE59F301Cu,0xE0000093u,0xE0010193u,0xE1B00640u,0xB2800001u,0xE1B01641u,0xB2811001u,0xE12FFF1Eu,0x00000000u,0x00000400u,0x00001F16u,0x02036B20u,0xE92D003Eu,0xE59F51D8u,0xE1D540B0u,0xE3540000u,0x0A000026u,0xE3500000u,0x028E2E5Du,0x128E2E1Eu,0xE5921000u,0xE5911000u,0xE3510000u,0x1A00001Fu,0xE5921004u,0xE5911000u,0xE151000Au,0x1A00001Bu,0xE3500000u,0x1A000012u,0xE59A04C4u,0xE3800A02u,0xE58A04C4u,0xE1A05804u,0xE1A04C04u,0xE1A04C44u,0xE1A05C45u,0xE59F3174u,0xE0000493u,0xE0010593u,0xE1A00640u,0xE1A01641u,0xE58F115Cu,0xE3500000u,0xB2600000u,0xB28EEE21u,0xA28EE07Cu,0xE8BD003Eu,0xE12FFF1Eu,0xE59F0140u,0xE3500000u,0xB2600000u,0xB28EEF77u,0xA28EE070u,0xE8BD003Eu,0xE12FFF1Eu,0xE8BD003Eu,0xE3500000u,0x059A136Cu,0x159A1370u,0xE59D0000u,0xE12FFF1Eu,0xE1A00E80u,0xE92D401Fu,0xE3A00B01u,0xEB00003Bu,0xE8BD401Fu,0x128EE038u,0xE12FFF1Eu,0xE3C12401u,0xE92D401Fu,0xE3A00002u,0xE3A01FE7u,0xE18A00B1u,0xE3A00040u,0xEB000031u,0x13A00000u,0x1B000028u,0xE3A00080u,0xEB00002Du,0x13A00002u,0x1B000024u,0xE3A00010u,0xEB000029u,0x0A000002u,0xE3E00000u,0xEB00000Cu,0xEB00001Eu,0xE3A00020u,0xEB000023u,0x0A000002u,0xE3A00001u,0xEB000006u,0xEB000018u,0xE3A00004u,0xEB00001Du,0x13A00000u,0x151F1160u,0x112FFF31u,0xE8BD801Fu,0xE92D401Eu,0xE1A03000u,0xE59F0078u,0xE7DA0000u,0xE59F1074u,0xE7DA1001u,0xE3A04000u,0xE3540008u,0x0A000008u,0xE2844001u,0xE0800003u,0xE2000007u,0xE3500000u,0x13500002u,0x0AFFFFF7u,0xE1A02031u,0xE3120001u,0x0AFFFFF4u,0xE8BD801Eu,0xE92D400Fu,0xE3A02000u,0xE1A01000u,0xE1A0000Au,0xE59F302Cu,0xE12FFF33u,0xE8BD800Fu,0xE92D4002u,0xE59F1020u,0xE19A10B1u,0xE1110000u,0xE8BD8002u,0x00000000u,0x00004492u,0x027FFDF0u,0x000004CEu,0x000004D2u,0x0200C5FCu,0x00000468u,0xD2000000u,0x00000000u,0x52021DE4u,0xE1A01F81u,0x02021DE4u,0xEBFF7C85u,0x02022078u,0xE3A00000u,0x02022014u,0xE3A00000u,0x02021F50u,0xE3A00000u,0x02021FB4u,0xE3A00000u,0xD2000000u,0x00000000u,0x52024B24u,0xE59D0000u,0x02024B20u,0xE3A00000u,0x02024B24u,0xEBFF71B7u,0x02024F10u,0xE3A00001u,0x02024F14u,0xEBFF70BBu,0x02024B28u,0xE3A00000u,0x02024CBCu,0xE3A00000u,0x02024F18u,0xE3A00000u,0x02025084u,0xE3A00000u,0xD2000000u,0x00000000u,0x52024314u,0xE1D10EB4u,0x02024314u,0xEBFF735Cu,0xD2000000u,0x00000000u,0x52026C34u,0xE3C12401u,0x02026C34u,0xEBFF69ACu,0x02026C64u,0xEBFF6999u,0xD2000000u,0x00000000u};
const u32 kMetroidUpstream[] = {0x62001000u,0xE92D007Fu,0xD0000000u,0x00000000u,0x62001000u,0xE92D007Fu,0xE2001000u,0x00000300u,0xE92D007Fu,0xE1A06004u,0xE28E1B05u,0xE59110D8u,0xE5911000u,0xE1510005u,0x18BD007Fu,0x11A01F81u,0x112FFF1Eu,0xE59F52D0u,0xE1D540B0u,0xE3540000u,0x08BD007Fu,0x01A01F81u,0x012FFF1Eu,0xE1A05804u,0xE1A04C04u,0xE1A04C44u,0xE1A05C45u,0xE2655000u,0xE59F3028u,0xE0000493u,0xE0010593u,0xE1A00640u,0xE1A01641u,0xE1C602BAu,0xE1C612BCu,0xE3A01001u,0xE5C61034u,0xE8BD007Fu,0xE28EE008u,0xE12FFF1Eu,0x000004F1u,0x00000000u,0x00000000u,0xE1A08000u,0xE1D10EB4u,0xE92D40FFu,0xE28E1EDEu,0xE5912000u,0xE5922000u,0xE3520000u,0x1A00004Cu,0xE5911004u,0xE5911000u,0xE151000Au,0x1A000048u,0xE59F7238u,0xE1D740FAu,0xE597500Cu,0xE1A06845u,0xE1A05805u,0xE1A05845u,0xE5D77004u,0xE5DF113Cu,0xE5CF7138u,0xE0211007u,0xE0077001u,0xE3170002u,0x13A00000u,0x159F1130u,0x112FFF31u,0xE1D870B4u,0xE5DF0118u,0xE3170040u,0x13100004u,0x114F48BCu,0x114F58BEu,0x114F69B0u,0xE3170020u,0x13100004u,0xE5DF20FAu,0x12822001u,0x12022001u,0x15CF20EEu,0xE5DF20E9u,0xE3170080u,0x13100004u,0x11E02002u,0x15CF20D9u,0xE3520000u,0x03A04000u,0x03A06000u,0x0A00000Fu,0xE15F0DF4u,0xE0444000u,0xE1B044C4u,0xB2844001u,0xE15F0EF2u,0xE0455000u,0xE1B054C5u,0xB2855001u,0xE2655000u,0xE15F0FF4u,0xE0466000u,0xE1B064C6u,0xB2866001u,0xE5DF2092u,0xE3520001u,0x01A06005u,0xEB000015u,0xE0844001u,0xE0866000u,0xE59F3080u,0xE0040493u,0xE1B04644u,0xB2844001u,0xE1D802BAu,0xE1D812BCu,0xE0800006u,0xE0811004u,0xE1C802BAu,0xE1C812BCu,0xE3560000u,0x03540000u,0x0A000003u,0xE1D810B0u,0xE3510000u,0x03A01A01u,0x01C810B0u,0xE8BD40FFu,0xE28EE008u,0xE12FFF1Eu,0xE59F2108u,0xE1D200F6u,0xE1D210F8u,0xE2611000u,0xE59F301Cu,0xE0000093u,0xE0010193u,0xE1B00640u,0xB2800001u,0xE1B01641u,0xB2811001u,0xE12FFF1Eu,0x00000000u,0x00000400u,0x00001F16u,0x02036B20u,0xE92D003Eu,0xE59F50C4u,0xE1D540B0u,0xE3540000u,0x0A000026u,0xE3500000u,0x028E2E5Du,0x128E2E1Eu,0xE5921000u,0xE5911000u,0xE3510000u,0x1A00001Fu,0xE5921004u,0xE5911000u,0xE151000Au,0x1A00001Bu,0xE3500000u,0x1A000012u,0xE59A04C4u,0xE3800A02u,0xE58A04C4u,0xE1A05804u,0xE1A04C04u,0xE1A04C44u,0xE1A05C45u,0xE59F3060u,0xE0000493u,0xE0010593u,0xE1A00640u,0xE1A01641u,0xE58F1048u,0xE3500000u,0xB2600000u,0xB28EEE21u,0xA28EE07Cu,0xE8BD003Eu,0xE12FFF1Eu,0xE59F002Cu,0xE3500000u,0xB2600000u,0xB28EEF77u,0xA28EE070u,0xE8BD003Eu,0xE12FFF1Eu,0xE8BD003Eu,0xE3500000u,0x059A136Cu,0x159A1370u,0xE59D0000u,0xE12FFF1Eu,0x00000000u,0x00004492u,0x027FFDF0u,0xD2000000u,0x00000000u,0x52021DE4u,0xE1A01F81u,0x02021DE4u,0xEBFF7C85u,0xD2000000u,0x00000000u,0x52024B24u,0xE59D0000u,0x02024B20u,0xE3A00000u,0x02024B24u,0xEBFF71C0u,0x02024F10u,0xE3A00001u,0x02024F14u,0xEBFF70C4u,0xD2000000u,0x00000000u,0x52024314u,0xE1D10EB4u,0x02024314u,0xEBFF735Cu,0xD2000000u,0x00000000u};

// What the patched instructions hold on the cartridge's code, as each code's
// guards expect them.
void seed_metroid(NDS& nds) {
  w32(nds, 0x02021DE4, 0xE1A01F81);
  w32(nds, 0x02024B24, 0xE59D0000);
  w32(nds, 0x02024314, 0xE1D10EB4);
  w32(nds, 0x02026C34, 0xE3C12401);
  w32(nds, 0x02026C64, 0xE1A00E80);
  w32(nds, 0x02024B28, 0x11111111);   // stands for the digital walk test the local scheme removes
}

void add_metroid(NDS& nds) {
  cheat::Code local, upstream;
  local.name = "local";
  local.analog = true;
  local.words.assign(std::begin(kMetroidLocal), std::end(kMetroidLocal));
  upstream.name = "upstream";
  upstream.analog = true;
  upstream.words.assign(std::begin(kMetroidUpstream), std::end(kMetroidUpstream));
  nds.cheats.codes.push_back(local);
  nds.cheats.codes.push_back(upstream);
}

// The journal chunk alone, through the same Writer and Reader a state uses,
// with guest RAM left as it was: exactly what a state brings back.
void state_round_trip(NDS& nds) {
  state::Writer w;
  nds.cheats.save_journal(w);
  state::Reader r(w.data().data(), w.data().size());
  nds.cheats.forget_writes();
  if (r.peek("CHJR")) nds.cheats.load_journal(r);
  CHECK(r.ok());
}

// Autosave and autoload are on in every pak profile, so a state normally
// brings RAM with the last session's hook already in it.
void test_state_carries_hooks() {
  { // The hook's cheat is off when the state comes back: it uninstalls.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    seed_metroid(nds);
    add_metroid(nds);
    nds.cheats.codes[0].enabled = true;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B28) == 0xE3A00000);
    state_round_trip(nds);
    nds.cheats.codes[0].enabled = false;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B28) == 0x11111111);
    CHECK(r32(nds, 0x02026C34) == 0xE3C12401);
    CHECK(r32(nds, 0x02026C64) == 0xE1A00E80);
    CHECK(r32(nds, 0x02001000) == 0);
  }
  { // Switching scheme across a state: the new hook replaces the old one
    // whole, so no branch lands inside a body of the other shape.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    seed_metroid(nds);
    add_metroid(nds);
    nds.cheats.codes[1].enabled = true;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x020012EC) == 0xE59D0000);      // upstream: inside its walk hook
    state_round_trip(nds);
    nds.cheats.codes[1].enabled = false;
    nds.cheats.codes[0].enabled = true;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02026C64) == 0xEBFF6999);
    CHECK(r32(nds, 0x020012EC) == 0xE3C12401);      // local: its entry, the replaced instruction
    CHECK(r32(nds, 0x020012D0) == 0xE1A00E80);
  }
  { // Still enabled: stays installed, and disabling later still reverts it.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    seed_metroid(nds);
    add_metroid(nds);
    nds.cheats.codes[1].enabled = true;
    nds.cheats.run(nds);
    state_round_trip(nds);
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B24) != 0xE59D0000);
    nds.cheats.codes[1].enabled = false;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B24) == 0xE59D0000);
    CHECK(r32(nds, 0x02001000) == 0);
  }
  { // A code no longer in the list: its bytes still go back.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    seed_metroid(nds);
    add_metroid(nds);
    nds.cheats.codes[0].enabled = true;
    nds.cheats.run(nds);
    state_round_trip(nds);
    nds.cheats.codes[0].name = "renamed";
    nds.cheats.codes[0].enabled = false;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B28) == 0x11111111);
    CHECK(r32(nds, 0x02001000) == 0);
  }
  { // Through a whole state, in the order the SDL frontend writes one: the
    // machine's chunks, the journal if any, then the frontend's own.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    std::vector<u8> rom(0x4000);
    CHECK(nds.load_rom_image(std::move(rom)));      // a state needs a cartridge
    nds.reset();
    nds.run_frame();                                // and a frame boundary
    seed_metroid(nds);
    add_metroid(nds);
    const auto frontend_chunks = [](state::Writer& w) {
      w.begin("VIEW"); w.put(u32{7}); w.end();
      w.begin("CHVO"); w.put(u32{9}); w.end();
    };
    std::string err;

    // No cheat has written, or a state from before the journal: no chunk,
    // and it must load, with the frontend's chunks still there to read.
    state::Writer bare;
    CHECK(nds.save_state(bare, err));
    frontend_chunks(bare);
    state::Reader plain(bare.data().data(), bare.data().size());
    CHECK(nds.load_state(plain, err));
    CHECK(plain.ok());
    CHECK(plain.peek("VIEW"));

    nds.cheats.codes[0].enabled = true;
    nds.cheats.run(nds);
    state::Writer full;
    CHECK(nds.save_state(full, err));
    frontend_chunks(full);
    CHECK(full.data().size() > bare.data().size());

    state::Reader with(full.data().data(), full.data().size());
    CHECK(nds.load_state(with, err));
    CHECK(with.peek("VIEW"));
    CHECK(with.begin("VIEW"));
    with.end();
    CHECK(with.begin("CHVO"));
    with.end();
    nds.cheats.codes[0].enabled = false;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B28) == 0x11111111);
    CHECK(r32(nds, 0x02001000) == 0);
  }
  { // Its code list gone: the hook still comes out, and the VBlank hook runs.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    seed_metroid(nds);
    add_metroid(nds);
    nds.cheats.codes[0].enabled = true;
    nds.cheats.run(nds);
    state_round_trip(nds);
    nds.cheats.codes.clear();
    CHECK(!nds.cheats.idle());
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x02024B28) == 0x11111111);
    CHECK(r32(nds, 0x02001000) == 0);
    CHECK(nds.cheats.idle());
  }
  { // A revised code under the same name does not inherit the old hook.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    seed_metroid(nds);
    add_metroid(nds);
    nds.cheats.codes[1].enabled = true;
    nds.cheats.run(nds);
    state_round_trip(nds);
    nds.cheats.codes[0].name = nds.cheats.codes[1].name;   // the local body, under upstream's name
    nds.cheats.codes[0].enabled = true;
    nds.cheats.codes[1].enabled = false;
    nds.cheats.run(nds);
    CHECK(r32(nds, 0x020012EC) == 0xE3C12401);      // the new body, whole
  }
}

// The stick layout read from a hook, on the real thing: Metroid's two schemes
// are extended and SM64DS is packed (test_analog_hook_sm64ds), and only a bulk
// write is code.
void test_layout_detection() {
  cheat::Code c;
  c.words.assign(std::begin(kMetroidLocal), std::end(kMetroidLocal));
  CHECK(cheat::reads_extended_analog(c));
  c.words.assign(std::begin(kMetroidUpstream), std::end(kMetroidUpstream));
  CHECK(cheat::reads_extended_analog(c));
  const u32 ldrsh_6 = 0xE1D200F6, ldrsh_8 = 0xE1D200F8, ldrsh_4 = 0xE1D200F4, ldrh_6 = 0xE1D800B6;
  c.words = {at(0xE0, 0x2001000), 8, ldrsh_6, 0};
  CHECK(cheat::reads_extended_analog(c));
  c.words = {at(0xE0, 0x2001000), 8, 0, ldrsh_8};
  CHECK(cheat::reads_extended_analog(c));
  c.words = {at(0xE0, 0x2001000), 8, ldrsh_4, 0};
  CHECK(!cheat::reads_extended_analog(c));
  c.words = {at(0xE0, 0x2001000), 8, ldrh_6, 0};    // unsigned: a game's struct, not the nub
  CHECK(!cheat::reads_extended_analog(c));
  c.words = {at(0x00, 0x2001000), ldrsh_6};         // a value being written, not code
  CHECK(!cheat::reads_extended_analog(c));
  c.words = {at(0xE0, 0x2001000), 64, ldrsh_6};     // truncated: stops at the end
  CHECK(cheat::reads_extended_analog(c));
}

void test_analog_request() {
  std::vector<cheat::Code> codes(3);
  codes[0].name = "Moon Jump"; codes[0].enabled = true; codes[0].words = {at(0xE0, 0x2001000), 8, 0xE1D200F6, 0};
  codes[1].name = "CPAD [with Nub stick]"; codes[1].analog = true; codes[1].words = {0, 0};
  codes[2].name = "CPAD"; codes[2].analog = true; codes[2].words = {at(0xE0, 0x2001000), 8, 0xE1D200F6, 0};
  CHECK(cheat::analog_request(codes).schemes == 0);     // an ordinary cheat is no request
  codes[2].enabled = true;
  cheat::AnalogRequest r = cheat::analog_request(codes);
  CHECK(r.schemes == 1 && r.extended && !r.nub);
  codes[1].enabled = true;
  r = cheat::analog_request(codes);
  CHECK(r.schemes == 2 && r.nub);
}

// Only analog codes are journaled. Anything else writes game data, which the
// game goes on changing: disabling a code just stops it writing.
void test_disable_leaves_other_codes_alone() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  w32(nds, RAM + 0x300, 100);
  cheat::Engine e;
  cheat::Code c; c.name = "max money"; c.enabled = true;
  c.words = {at(0x00, 0x2000300), 9999};
  e.codes.push_back(c);
  e.run(nds);
  CHECK(r32(nds, RAM + 0x300) == 9999);
  e.codes[0].enabled = false;
  e.run(nds);
  CHECK(r32(nds, RAM + 0x300) == 9999);
  state::Writer w;
  e.save_journal(w);
  CHECK(w.data().empty());
}

// A byte that no longer holds what the code wrote has been replaced since, by
// an overlay say, so its original belongs to what has gone.
void test_revert_leaves_changed_bytes() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  w32(nds, RAM + 0x400, 0x11111111);
  w32(nds, RAM + 0x404, 0x22222222);
  cheat::Engine e;
  cheat::Code c; c.name = "hook"; c.enabled = true; c.analog = true;
  c.words = {at(0x00, 0x2000400), 0xEA000000, at(0x00, 0x2000404), 0xEA000001};
  e.codes.push_back(c);
  e.run(nds);
  w32(nds, RAM + 0x400, 0x33333333);                // the overlay
  e.codes[0].enabled = false;
  e.run(nds);
  CHECK(r32(nds, RAM + 0x400) == 0x33333333);
  CHECK(r32(nds, RAM + 0x404) == 0x22222222);

  // And the same across a state: what the code wrote travels too.
  e.codes[0].enabled = true;
  w32(nds, RAM + 0x400, 0x11111111);
  e.run(nds);
  state::Writer w;
  e.save_journal(w);
  state::Reader r(w.data().data(), w.data().size());
  e.forget_writes();
  e.load_journal(r);
  CHECK(r.ok());
  w32(nds, RAM + 0x400, 0x33333333);
  e.codes[0].enabled = false;
  e.run(nds);
  CHECK(r32(nds, RAM + 0x400) == 0x33333333);
  CHECK(r32(nds, RAM + 0x404) == 0x22222222);
}

// The bus aligns a write down; the journal must keep those bytes, not the
// ones the address names.
void test_journal_follows_alignment() {
  auto p = std::make_unique<NDS>();
  NDS& nds = *p;
  w32(nds, RAM + 0x100, 0x11223344);
  w32(nds, RAM + 0x104, 0x55667788);
  cheat::Engine e;
  cheat::Code c; c.name = "misaligned"; c.enabled = true; c.analog = true;
  c.words = {at(0x00, 0x2000102), 0xCAFEBABE};
  e.codes.push_back(c);
  e.run(nds);
  CHECK(r32(nds, RAM + 0x100) == 0xCAFEBABE);
  w32(nds, RAM + 0x104, 0x99AABBCC);                // the game's, never the code's
  e.codes[0].enabled = false;
  e.run(nds);
  CHECK(r32(nds, RAM + 0x100) == 0x11223344);
  CHECK(r32(nds, RAM + 0x104) == 0x99AABBCC);
}

void test_disable_reverts_writes() {
  { // Plain writes, all three widths.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    w32(nds, RAM + 0x300, 0x11223344);
    cheat::Engine e;
    cheat::Code c; c.name = "w"; c.enabled = true; c.analog = true;
    c.words = {at(0x00, 0x2000300), 0xCAFEBABE,
               at(0x10, 0x2000310), 0x1234,
               at(0x20, 0x2000320), 0x56};
    e.codes.push_back(c);
    e.run(nds);
    CHECK(r32(nds, RAM + 0x300) == 0xCAFEBABE);
    e.codes[0].enabled = false;
    e.run(nds);
    CHECK(r32(nds, RAM + 0x300) == 0x11223344);
    CHECK(r16(nds, RAM + 0x310) == 0);
    CHECK(r8(nds, RAM + 0x320) == 0);
  }
  { // A guarded hook install (the CPAD shape): branch + E-block body.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    w32(nds, RAM + 0x400, 0xE5901000);              // the instruction the hook replaces
    cheat::Engine e;
    cheat::Code c; c.name = "hook"; c.enabled = true; c.analog = true;
    c.words = {at(0x60, 0x2001000), 0xE92D007F,     // install unless already installed
               at(0xE0, 0x2001000), 8, 0xE92D007F, 0xE12FFF1E,
               at(0x00, 0x2000400), 0xEA000000,     // branch into the hook
               0xD2000000, 0};
    e.codes.push_back(c);
    e.run(nds);
    CHECK(r32(nds, RAM + 0x1000) == 0xE92D007F);
    CHECK(r32(nds, RAM + 0x400) == 0xEA000000);
    e.run(nds);                                   // guard holds: no rewrite
    e.codes[0].enabled = false;
    e.run(nds);
    CHECK(r32(nds, RAM + 0x400) == 0xE5901000);   // original instruction back
    CHECK(r32(nds, RAM + 0x1000) == 0);           // hook body gone
    e.codes[0].enabled = true;
    e.run(nds);
    CHECK(r32(nds, RAM + 0x1000) == 0xE92D007F);  // and it installs again
    CHECK(r32(nds, RAM + 0x400) == 0xEA000000);
  }
  { // Overlap: disabling one writer must not clobber the other's byte.
    auto p = std::make_unique<NDS>();
    NDS& nds = *p;
    cheat::Engine e;
    cheat::Code a; a.name = "a"; a.enabled = true; a.analog = true; a.words = {at(0x00, 0x2000500), 1};
    cheat::Code b; b.name = "b"; b.enabled = true; b.analog = true; b.words = {at(0x00, 0x2000500), 2};
    e.codes.push_back(a);
    e.codes.push_back(b);
    e.run(nds);
    CHECK(r32(nds, RAM + 0x500) == 2);
    e.codes[0].enabled = false;
    e.run(nds);
    CHECK(r32(nds, RAM + 0x500) == 2);            // b still owns it
    e.codes[1].enabled = false;
    e.run(nds);
    CHECK(r32(nds, RAM + 0x500) == 0);            // last writer out: original back
  }
}

} // namespace

int main() {
  test_writes();
  test_conditionals_32();
  test_conditionals_16();
  test_nested_conditions();
  test_skipped_conditional_does_not_push();
  test_loop();
  test_loop_flush();
  test_data_register();
  test_datareg_memory();
  test_offset_ops();
  test_c5_counter();
  test_block_write();
  test_block_write_skipped();
  test_block_copy();
  test_malformed();
  test_engine_enable();
  test_hook_runs_on_arm7_vblank();
  test_hook_ignores_arm9();
  test_hook_ignores_other_irqs();
  test_hook_needs_ie_and_if();
  test_analog_hook_sm64ds();
  test_disable_reverts_writes();
  test_layout_detection();
  test_analog_request();
  test_disable_leaves_other_codes_alone();
  test_journal_follows_alignment();
  test_revert_leaves_changed_bytes();
  test_state_carries_hooks();
  std::printf("cheat: ok\n");
  return 0;
}
