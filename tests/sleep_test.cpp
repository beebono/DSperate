// System sleep must stop both CPUs, preserve device clocks and wake on ARM7 IRQ.
#include "core/nds.h"
#include "core/state/state.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

int main() {
	auto nds = std::make_unique<ds::NDS>();
	auto &io = nds->io;
	auto &a7 = nds->cpu(ds::Cpu::ARM7);
	auto &a9 = nds->cpu(ds::Cpu::ARM9);
	a7.halted = a9.halted = true;
	io.start_rtc_clock();
	io.rtc.datetime[6] = 0;
	io.cpu_io[1].ie = 1u << ds::io::IRQ_LID;
	io.cpu_io[1].ime = 0; // Waking does not require interrupt delivery to be enabled.

	ds::state::Writer awake;
	io.sync_state(awake);
	// Simulate the previous IO chunk, before sleep state was appended.
	auto legacy = awake.data();
	legacy.resize(legacy.size() - sizeof(bool) - sizeof(ds::u64));
	ds::u32 length = legacy.size() - 8;
	std::memcpy(legacy.data() + 4, &length, sizeof(length));

	for (unsigned cycle = 0; cycle < 3; cycle++) {
		io.set_lid(true);
		CHECK(!io.sleeping); // The game decides whether closing the lid means sleep.
		const auto running = nds->sched.now();
		nds->run_frame();
		CHECK(nds->sched.now() > running);
		a9.halted = false; // STOP must suspend ARM9 even when it is runnable.
		io.write(ds::Cpu::ARM7, 0x04000301, 8, 0xC0);
		CHECK(io.sleeping && a7.halted);
		const auto now = nds->sched.now();
		const auto pc9 = a9.hot.regs[15];
		const auto line = nds->gpu.line();
		const auto sec = io.rtc.datetime[6];
		nds->spu.drain();
		ds::u64 samples = 0;
		for (unsigned frame = 0; frame < 60; frame++) {
			nds->run_frame();
			CHECK(nds->sched.now() == now);
			CHECK(a9.hot.regs[15] == pc9 && nds->gpu.line() == line);
			std::vector<ds::s16> audio(nds->spu.available() * 2);
			samples += nds->spu.take(audio.data(), audio.size() / 2);
			for (auto sample : audio) CHECK(sample == 0);
		}
		CHECK(samples >= 32823 && samples <= 32824);
		CHECK(io.rtc.datetime[6] != sec);

		ds::state::Writer sleeping;
		io.sync_state(sleeping);
		io.set_lid(false);
		CHECK(!io.sleeping && !a7.halted);
		ds::state::Reader restore(sleeping.data().data(), sleeping.data().size());
		io.sync_state(restore);
		CHECK(io.sleeping);
		ds::state::Reader old(legacy.data(), legacy.size());
		io.sync_state(old);
		CHECK(!io.sleeping && io.sleep_frame_cycles == 0);
		a7.halted = a9.halted = true;
		io.cpu_io[1].if_ = 0;
	}

	io.write(ds::Cpu::ARM7, 0x04000301, 8, 0x80);
	CHECK(a7.halted && !io.sleeping);
	const auto before = nds->sched.now();
	nds->run_frame();
	CHECK(nds->sched.now() > before);
	io.cpu_io[1].if_ = 1u << ds::io::IRQ_LID;
	io.write(ds::Cpu::ARM7, 0x04000301, 8, 0xC0);
	CHECK(!io.sleeping); // A pending wake interrupt prevents entry into STOP.
	if (!failures) puts("System sleep, lid wake, silent audio pacing, RTC and state compatibility passed");
	return failures ? 1 : 0;
}
