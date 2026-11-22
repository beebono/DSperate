#pragma once
#include <cstdio>
#include <cstdlib>

// DSperate: changed from upstream (as is timer.cpp/timer.h, for the timer
// prescaler; see the README beside the sources). An assertion here is a DSP
// program doing something the core does not implement (or cannot), and what a
// program does is the guest's choice: it must not take the emulator down.
// Upstream prints and calls std::abort(); this throws, and DsiDsp catches it
// and stops the DSP core alone (src/core/io/dsi_dsp.cpp).
struct TeakraFault {
    const char* expression;
    const char* file;
    int line;
};

[[noreturn]] inline void Assert(const char* expression, const char* file, int line) {
    throw TeakraFault{expression, file, line};
}

#define ASSERT(EXPRESSION) ((EXPRESSION) ? (void)0 : Assert(#EXPRESSION, __FILE__, __LINE__))
#define UNREACHABLE() Assert("UNREACHABLE", __FILE__, __LINE__)
