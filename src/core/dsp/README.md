# The DSi's DSP

Three pieces of other projects' code live here, each kept as its authors wrote it so that it can be
compared with, and refreshed from, where it came from. `DsiDsp` (`src/core/io/dsi_dsp.cpp`) owns them
and picks between them: a DSP program known by its CRC runs on its high-level rewrite, anything else
on teakra.

## DSP_HLE: the SDK's three programs, at high level

`DSP_HLE/` and `FIFO.h` are melonDS's (GPLv3, melonDS team; `src/DSP_HLE` and `src/FIFO.h` at commit
497aa6cd, 2026-11-04), unchanged: rewrites of the AAC, G711 and graphics programs Nintendo's SDK gave
games. `DSi.h`, `DSi_DSP.h`, `Platform.h`, `Savestate.h` and `types.h` stand in for the melonDS
headers those sources include by relative path.

## faad2: the AAC decoder

`faad2/` is FAAD2 2.11.2 (GPL-2.0-or-later, `faad2/COPYING`; github.com/knik0/faad2), cut down to the
AAC-LC decoder: built with `LC_ONLY_DECODER` and `DISABLE_SBR`, and without the sources of SBR,
parametric stereo, DRM, the main/LTP/SSR profiles and the error-resilient tools. `aac_decoder.cpp`
drives it for the AAC program (the DSi Sound app's). Code from FAAD2 is copyright (c) Nero AG,
www.nero.com.

## teakra: the DSP core itself

`teakra/` is teakra, a TeakLite II / XpertTeak emulator by Weiyi Wang (MIT, `teakra/LICENSE`), as
melonDS carries it: `src/teakra` of melonDS at commit 497aa6cd (2026-11-04), which adds save states,
the I2S sample clock and the AHBM accessors to upstream teakra. Only the library's sources are here:
no disassembler, parser, tools or tests.

It is exact and slow: about 40 ms a frame on a desktop and 480 ms on an RK3566 while a program runs.
It is the fallback for programs that are not the SDK's (homebrew), and `DS_DSP_LLE=1` uses it for all.

Changes from melonDS's copy, each marked `DSperate:` in the source:

- `src/crash.h`: a failed assertion throws `TeakraFault` instead of calling `std::abort()`. What a DSP
  program does is the guest's choice; `DsiDsp` catches it and stops the DSP alone.
- `src/timer.cpp`, `src/timer.h`: the timer prescaler (TS: /1, /2, /4, /16), which upstream asserts
  is off. A prescaled timer is stepped every clock and never skipped over.
- `src/interpreter.h`: the per-cycle interrupt flags are looked at plainly, not with four atomic
  exchanges; the core runs on one thread here.
