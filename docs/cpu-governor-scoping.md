# The CPU frequency governor, and why a frame limiter fights it

RG DS Plus (RK3566, four Cortex-A55 in one cpufreq policy, ROCKNIX), 2026-09-15.

ROCKNIX ships `ondemand` and its UI offers `performance` without defaulting to
it, so that is what a player gets. It costs, and it costs in the place that is
felt rather than the place that is measured: the median moves by 1-3 ms, the
missed frames multiply by 4-6 and the audio gaps by 20-30.

## What ondemand is doing

The tunables it ships with, which matter to all of the below:

    up_threshold 95   sampling_rate 8000 us   sampling_down_factor 1
    io_is_busy 0      ignore_nice_load 0      powersave_bias 0
    transition latency 171 us
    OPPs 408 600 816 1104 1416 1608 1800 1992 MHz

An emulator held to 59.8261 Hz is a duty-cycled load: a burst of work, then a
sleep to the next deadline. At 1992 MHz the SM64DS replay works 10 ms of every
16.7, so the busiest sample window ondemand can ever see is ~60 % -- far under
`up_threshold`, so the rule that jumps straight to the maximum OPP never fires
and the clock is set proportionally to load instead. Worse, an 8 ms window
against a 16.7 ms frame is not phase-locked to it: windows land wholly inside
the sleep, read as an idle machine, and step the clock *down*. The next frame
then starts at 600-1100 MHz and takes two or three times its budget. It is not
heard as a slow frame; it is heard as a gap in the sound.

The residency confirms it is dives and not a low average clock -- a third of
the run *is* at 1992 MHz:

    ondemand, SM64DS, 1800 frames:  408:0% 600:6% 816:6% 1104:7%
                                    1416:15% 1608:15% 1800:18% 1992:32%
    ~1700 transitions in 30 s, i.e. one every ~18 ms

## The measurements

SDL frontend, `--dual-window --cpu-oc`, 1800 frames with the first 600 dropped,
one binary, every pair run in both orders. `dry` is frames that found the audio
queue empty. Not thermal: 53-60 C throughout, against an 83 C trip.

| scene | governor / pacing | median | p99 | over budget /1200 | dry |
|---|---|---|---|---|---|
| sm64  | ondemand, sleep | 11.52-12.22 | 26.6-26.7 | 144-153 | 127-141 |
| sm64  | ondemand, busy  | 10.25-10.39 | 19.0-19.4 | 28-30   | 4-6    |
| sm64  | performance     | 10.08-10.09 | 19.0-19.4 | 26-30   | 3-7    |
| etody | ondemand, sleep | 9.06-9.16   | 17.9-31.6 | 75-139  | 86-104 |
| etody | ondemand, busy  | 6.12-6.47   | 15.0-16.0 | 0-8     | 4-13   |
| etody | performance     | 6.27-6.46   | 15.8-16.1 | 6       | 3      |
| dbori | ondemand, sleep | 14.37-14.38 | 33.8-54.4 | 87-145  | 70-124 |
| dbori | ondemand, busy  | 13.68-13.81 | 17.4-17.6 | 22-29   | 4-11   |
| dbori | performance     | 13.64-13.67 | 17.0-17.3 | 17-19   | 4-6    |
| nsmb  | ondemand, sleep | 16.97-18.31 | 53.3-78.2 | 635-688 | 343-392 |
| nsmb  | ondemand, busy  | 16.88-17.60 | 45.4-53.4 | 610-624 | 251-277 |
| nsmb  | performance     | 16.82-17.18 | 21.9-22.2 | 602-611 | 246-256 |

## What fixes it: pacing, not a governor tap

`emu.pacing` (`--pacing auto|sleep|busy`). `busy` holds the core to the
deadline instead of sleeping to it, so the machine looks as busy as it is.
Nothing writes to sysfs -- the only system state read is which governor is in
charge, and `auto` (the default) spins only where that governor is one that
decides by polling load (`ondemand`, `conservative`, `interactive`,
`powersave`). The frame rate is identical either way.

`powersave` counts as a poller only under a driver that means a fixed floor by
it: under `intel_pstate`/`amd-pstate` that name is the hardware picking its own
P-state from its own view of utilisation, it is the default on most desktops,
and it does not have this problem -- so the driver has to agree before anything
spins.

The spin must not run at the emulator's real-time priority: the kernel caps an
RT thread at `sched_rt_runtime_us` (95 % of every second here) and would
throttle the emulation along with the spin, and nothing at ordinary priority --
the sound daemon, the compositor -- would get the core at all. So the wait is
taken as `SCHED_OTHER` and the policy is put back for the frame.

**It does not cost battery.** Sampling `current_avg` over the last 16 s of
45 s runs, both orders, at 21-22 % charge:

    idle (nothing running)   567 mA   2093 mW
    ondemand, sleep          904 mA / 1008 mA
    ondemand, busy           862 mA /  784 mA

Holding a core is *cheaper* than letting the governor oscillate -- within a
noisy instrument (a gauge average, ~10 % spread inside a variant), but the
direction held in both orders. The oscillation's own cost is real: ~1700
regulator/PLL transitions a run, and the same work stretched over a longer
active period at a lower OPP.

## The limit of it, and what is still worth having

On a scene that is *already* over budget there is no gap to spin: a late frame
returns from the limiter without waiting at all. `nsmb` (over budget on half
its frames) keeps a p99 of 45-53 ms under ondemand + busy, against 22 ms on
performance. So pacing fixes the common case completely and a deficit scene
only partly.

For that case the answer is the governor, and the cheap one is **schedutil**,
which this kernel has: it is driven by the scheduler's own utilisation signal,
which decays across a sleep instead of resetting, so a duty-cycled load holds
its estimate. It measured indistinguishable from performance and keeps DVFS:

| governor | median | p99 | over budget | dry | at 1992 MHz |
|---|---|---|---|---|---|
| performance | 9.71-10.40 | 19.2-19.6 | 26-35 | 3-7 | 100 % |
| schedutil | 10.04-10.38 | 19.1-19.2 | 29-31 | 6-7 | 95 % |
| min_freq 1608 | 10.50-10.59 | 20.2-20.6 | 29-30 | 7 | 35 % |
| ondemand | 11.09-12.15 | 24.7-35.6 | 77-162 | 101-140 | 31-36 % |

One line in a ROCKNIX launcher (`start_dsperate.sh`) gets it, and it is the
right place for it: the emulator changing a system-global setting would outlive
a hard kill.

## Dead ends -- do not redo these

- **`up_threshold` 40.** Forces 89 % residency at 1992 MHz and the tail got
  *worse* (over budget 83-201 against 77-162). Average clock was never the
  problem; the dives remain, and each costs a 171 us transition plus up to a
  sample window at the low OPP.
- **`io_is_busy` 1.** No effect worth having (dry 58-110). This load does not
  wait on I/O.
- **`sampling_down_factor` 20.** Helps (dry 50-57) but does not close it, and
  it only makes the descent slower rather than stopping it.
- **`sampling_rate` 2000.** The kernel refuses anything under its minimum; the
  write fails and the value stays 8000.
- **A DDR devfreq governor.** There is none on this board -- only
  `fde60000.gpu`. The memory clock is fixed, so it is not part of this story
  (which would have mattered, the JIT being stall-bound).
- **uclamp** (a per-thread utilisation floor, which would have been the tidy
  fix under schedutil) is not compiled into this kernel: no
  `/proc/sys/kernel/sched_util_clamp_min`.

## A measurement trap this whole thing came out of

The figure that opened the session -- "13.26 ms ondemand against 7.34 ms
performance, nearly half the frame budget" -- was two different configurations,
not two governors: an SDL run against a headless `--timing-oc --cpu-uc` one
(`ab-base-sm64.log`: 7.97 ms). The real cost of the governor on that scene is
1.4-2.1 ms of median.

The reason no earlier benchmark ever caught this: **the headless frontend is
unthrottled**, so it runs at 100 % duty and every governor takes it to the
maximum OPP. A DVFS question can only be asked of the paced frontend.
