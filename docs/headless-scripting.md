# Scripting the headless harness

`dsperate-headless` normally runs a fixed number of frames. With `--lua SCRIPT`
a Lua 5.4 script runs the machine instead: it owns the frame loop, reads and
writes guest memory between frames, holds buttons and the pen, saves and loads
states, and writes screenshots. That makes closed-loop tools possible (bots,
probes, route finders) that decide each frame from what they read.

## Building

Lua mode needs Lua 5.4's headers and library (`liblua5.4-dev` on Debian and
Ubuntu). CMake finds them with `find_package(Lua 5.4)`; without them the
harness is built as before and `--lua` exits with a message. `DSPERATE_LUA=OFF`
leaves it out on purpose.

## Running

    dsperate-headless --direct --lua probe.lua game.nds

The machine is set up as usual first (`--direct`, `--save`, `--cheats`,
`--load-state` and the rest apply), then the script runs; `--frames` and the
fixed loop's per-frame dumps don't. The run's exit code is the script's
(`ds.exit(n)`, 0 when the script returns, 1 on a Lua error). The script reads
its own settings from the environment (`os.getenv`) or files; the standard Lua
libraries are open.

## The `ds` table

| Call | What it does |
| --- | --- |
| `ds.frameadvance([n])` | Runs `n` frames (default 1) with the held buttons and pen. |
| `ds.framecount()` | The machine's frame count. |
| `ds.read8(addr [, cpu])`, `read16`, `read32` | A guest read as `cpu` sees the address: `"arm9"` (default) or `"arm7"`. TCM, I/O and every RAM are reachable; reads take no CPU time. |
| `ds.readrange(addr, len [, cpu])` | `len` bytes from `addr` as a string (use `string.byte` or `string.unpack`). |
| `ds.write8(addr, value [, cpu])`, `write16`, `write32` | A guest write; code it overwrites is retranslated, as with cheats. |
| `ds.setkeys(keys)` | The buttons held from the next frame on: a list (`{"a", "right"}`), a set (`{a = true}`), or a mask of the input log's bits (A 0, B 1, Select 2, Start 3, Right 4, Left 5, Up 6, Down 7, R 8, L 9, X 10, Y 11). `ds.setkeys({})` lets go. |
| `ds.touch(x, y)`, `ds.touch()` | Holds the pen at bottom-screen pixel `x, y`, or lifts it. |
| `ds.savestate(path)`, `ds.loadstate(path)` | A save state to or from a file: `true`, or `nil` and a message. Allowed between any two frames. |
| `ds.screenshot(path [, which])` | The last frame as a PNG: `"both"` (default, top over bottom, 256x384), `"top"` or `"bottom"`. `true`, or `nil` and a message. |
| `ds.exit([code])` | Ends the run with that exit code. |

Memory calls go through the same path as DMA and cheats (`Bus::dma_read*`,
`dma_write*`), so they work with the recompiler on.

## Example

```lua
-- Hold right for two seconds, then save a state and a screenshot.
ds.setkeys({"right"})
ds.frameadvance(120)
ds.setkeys({})
assert(ds.savestate("walked.dss"))
ds.screenshot("walked.png", "top")
print(string.format("frame %d, word at 0x02000000 = %08x", ds.framecount(), ds.read32(0x02000000)))
```

## Tests

`tests/lua_host_test.cpp` (ctest `lua_host`) runs scripts on a blank cartridge:
memory through both CPUs, keys, frames, state round trips, screenshots, exit
codes and errors.
