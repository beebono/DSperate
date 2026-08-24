# Our profile against DraStic's, function by function

[techniques/00](techniques/00-method-and-measurements.md) compares the two
emulators by *subsystem bucket*. This compares them by *function*: for each
role in the pipeline, what does DraStic spend there, and what do we spend on
the function that plays the same part.

The question it answers is not "who is faster" — DraStic is, and by a lot. It
is **where is the shape of our profile different from theirs**, because a role
that costs us four times what it costs them is a design difference, and a role
that matches is one we should stop looking at.

## Method, and how much to trust it

Both profiles are `perf record -e cycles -F 999` on the RK3566, SM64DS.

- **DraStic**: `drastic-sym` (the unstripped build, exact symbols) under
  `--input-playback sm64`, 45 s, 29,009 samples, `SDL_VIDEODRIVER=dummy`.
- **Ours**: `dsperate.r3d0` under `--replay scenes/sm64.dsin --frames 1800`.

The two do **not** run the same input. DraStic cannot replay our `.dsin` and we
cannot replay its `.ir`, so this is two different SM64DS sessions. Durations
differ too.

The bigger correction is that **47.4 % of DraStic's samples are inside
libSDL2** — its frontend blits and scales in software even under the dummy
driver, and our CLI has no frontend at all. Comparing raw process shares would
be meaningless. So every number below is **renormalised to each emulator's own
code**: DraStic's own DSO is 39.9 % of its process, ours is 79.4 % of ours, and
each symbol is divided by its own emulator's figure.

| DSO | DraStic | ours |
|---|---|---|
| emulator's own code | 39.9 % | 79.4 % |
| libSDL2 | 47.4 % | — |
| translated guest code (`[JIT]`) | 5.7 % | 15.9 % |
| libc | 4.5 % | 3.3 % |
| kernel | 2.4 % | 1.3 % |

Given different scenes, different lengths and that renormalisation, **treat a
ratio below about 1.5× as noise**. Only the large ones below are claims.

One figure survives without renormalisation and is worth stating on its own:
excluding SDL from DraStic's process, its translated guest code is 10.9 % of
what remains against our 15.9 % — our recompiler output costs about **1.5×**
theirs, which is the same story
[jit-technique-audit.md](jit-technique-audit.md) tells from instruction counts.

## Role by role

Percentages are share of each emulator's own code.

| role | DraStic | | ours | | ratio |
|---|---|---|---|---|---|
| **span / polygon driver** | `render_polygon_flush_1x` | 2.6 % | `render_polygon_line` | 11.0 % | **4.3×** |
| **perspective setup** | `setup_perspective_steps_w_constant_asm` | 1.6 % | `span_factor` | 5.3 % | **3.4×** |
| **geometry submission** | `process_geometry_commands`, `geometry_flush_polygons` | 1.2 % | `Gpu3D::submit_polygon`, `fifo_read` | 3.5 % | **2.9×** |
| **palette / OAM change detection** | *(no such symbol)* | 0 % | `memcmp` | 1.6 % | **—** |
| attribute interpolation | `interpolate_uv_asm`, `setup_uv_interpolants_asm` | 3.8 % | `span_attrs5n`, `span_attrs2n`, `span_attrs` | 5.0 % | 1.3 × |
| depth compare | `depth_compare_less_than{,_constant}_asm` | 3.8 % | `depth_candidates` | 3.4 % | 0.9 × |
| scheduler / CPU handoff | `recompiler_cpu_next_action_arm9_to_arm7`, `recompiler_entry_direct` | 1.4 % | `Scheduler::slice_next` | 1.4 % | 1.0 × |
| texture cache lookup | `texture_cache_lookup` | 0.08 % | *(below 0.15 %)* | ~0.1 % | 1 × |
| texel fetch | `load_texels_paletted_asm` | 3.5 % | `gatherN_*` (all variants) | 1.9 % | **0.5×** |
| shade + blend + writeback | `alpha_combine`, `alpha_blend`, `writeback_*`, `modulate`, `combine_colors`, `load_depth_colors_id`, `load_depth` | 26.8 % | `resolve_span_vec` + `resolve_span` (all instantiations) | 16.4 % | **0.6×** |

## What this says

**Our per-pixel work is already better than theirs.** The shade/blend/writeback
bucket is the largest single thing DraStic does — 26.8 % of its own code across
nine kernels — and our fused `resolve_span_vec` does the same job for 16.4 %.
Texel fetch is half their cost. Depth compare, attribute interpolation, the
scheduler and the texture cache all match within noise.

That is the opposite of the assumption behind the technique documents. We have
been reading `techniques/02` as a list of things to catch up on, and at the
pixel level we are already ahead — our fusion beats their staging, even though
their staging is hand-written assembly and ours is intrinsics.

**Every place we are behind is a decision made too late.** The three large
ratios are all the same defect in different clothes:

- `render_polygon_line` is 4.3× `render_polygon_flush_1x` because theirs is a
  dispatcher and ours re-derives per-polygon facts on every scanline. §2 packs
  those into a descriptor word at bin time.
- `span_factor` is 3.4× their perspective setup because we compute a
  per-pixel factor array where they compute per-span *steps* and increment.
  Their hot variant is `_w_constant` — the specialised one — which is the same
  hoist again.
- Geometry submission is 2.9× theirs, and this one is not covered by any
  technique document. `techniques/00` puts DraStic's whole Video Geometry phase
  at 0.09 ms/frame and we have never examined ours.

**And one category we pay that they do not pay at all.** We spend 1.6 % of our
code in `memcmp`, from five sites in `engine2d.cpp` that compare 512-byte
palettes and the 1 KB OAM against saved copies to detect changes. DraStic's
profile has no comparable symbol anywhere — it evidently invalidates on the
write path instead, which it can do cheaply because its writes already go
through a tagged page table. This is the clearest "hot for us, absent for
them" in the whole comparison, and it is a 2D item, not a 3D one.

`PageTable::remap` belongs in the same family: DraStic's
`map_memory_page_from_memory_map` is 0.25 % of its code, and ours does not
appear on SM64DS but is 1.8 % on Etrian Odyssey.

## Where that leaves the ranking

Ordered by ratio rather than by our own frame share, which is the point of
doing this:

1. **The polygon driver** (4.3×) — biggest, and already the top item from the
   [scene census](../scenes/README.md).
2. **`span_factor`** (3.4×) — previously dismissed as a sub-1 % item measured
   against our own frame. Against DraStic it is one of the worst ratios we
   have, and it is a self-contained kernel.
3. **Geometry submission** (2.9×) — unexamined, undocumented, and the hottest
   thing in Dragon Ball after the driver.
4. **`memcmp` change detection** (∞) — a whole category of work to delete
   rather than optimise.

And a matching list of things to stop proposing: the pixel stages, the depth
pre-pass, the texture cache, the scheduler. We match or beat DraStic on all of
them.
