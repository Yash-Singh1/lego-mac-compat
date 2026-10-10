# MW3 rendering performance, October 10, 2026

## Measured bottleneck

A read-only five-second sample of the running MW3 render thread recorded
2,880 samples. Index publication appeared in 1,179 samples, 40.9%, and
`glBufferData` in 1,170, 40.6%. New shared Metal buffer allocation appeared
in 769 samples, 26.7%; 611 samples, 21.2%, reached `IOGPUResourceCreate`.
These are nested categories, not independent costs to add together.
The GL command worker waited on its condition variable in 1,747 samples,
60.7%. The producer frequently spent time allocating buffers instead of
feeding the worker. This steady-state sample contained little shader
frontend compilation.

The compatibility bridge kept dynamic indices in a CPU shadow, then made
a separate `glBufferData` publication for every draw range. That avoided
expensive updates of the original large buffer, but each publication requested
separate Metal storage. GLMetal already had a storage pool; the live profile
shows allocation remained costly under this game's workload.

## Index arena

Replacement GL backends now append draw ranges to a 2 MB index arena. The
arena grows for a larger range and orphans on wrap or context change. Previously
submitted draws retain their original storage. Appends use at most 64 KB per
`glBufferSubData` command, allowing GLMetal to snapshot bytes inline instead
of allocating a staging Metal buffer. The bridge restores the game's original
element-buffer binding after each draw.

Identical consecutive publications can reuse their offset. A source generation
invalidates reuse after writes, aliases, orphaning, deletion or name reuse.
Context changes and destruction invalidate it too. The Apple path keeps its
existing publication method. `LP32_NO_INDEX_ARENA=1` disables the arena for
comparisons; `LP32_INDEX_STATS=1` reports publication and orphan counts.

## Generic sampler metadata

GLMetal now caches immutable sampler reflection in 32 bounded thread-local
plans. Each plan keys on the program, unique successful link serial and
reflection storage. Live texture units, sampler objects, texture formats,
mip views and depth metadata remain draw state. Collisions rebuild the plan;
programs with more than 128 sampler uniforms use the original scan.
Programs without border emulation skip an unnecessary shadow-sampler scan.
This changes no shader output or compiler cache identity.

The CPU probe replayed 32 captured MW3 programs with 660 uniforms and
160 sampler uniforms for 200,000 metadata iterations. Rosetta trials took
19.904 to 32.814 ms with the original scan and 5.184 to 9.134 ms with plans,
with identical checksums. Concurrent user activity contributes timing variation.
This is metadata processing time, not total texture binding or gameplay FPS.

## Validation and performance limits

`python3 native/tests/test_index_shadow_reuse.py` passed with the arena on
and off. It covers source writes, range/type/count changes, aliases, buffer
lifetimes, shared contexts, arena mutation, wrapping and upload chunk limits.
The native `LP32_GL_BUFFER_SELFTEST=1` also passed against the packaged driver.

`python3 native/tests/test_index_shadow_gpu.py` uses the production index
publication code with real GLMetal command-thread rendering. Each cohort
issues 131,072 measured draws in 256 offscreen frames. Pixel checks include
the frame whose queued draws straddle the 2 MB arena wrap. Both architectures
and both arena modes produced zero bad pixels and no GL errors.

| Architecture | Arena off, submission | Arena on, submission | Reduction |
| --- | ---: | ---: | ---: |
| Rosetta x86_64 | 352.563 ms | 212.107 ms | 39.8% |
| Native arm64 | 191.530 ms | 125.465 ms | 34.5% |

Completion times were 353.623 versus 217.744 ms on Rosetta and 192.286
versus 128.413 ms on arm64. The preceding cohort also passed all pixels,
with submission reductions of 39.5% and 38.6%. These are isolated index
publication workloads, not gameplay FPS claims. Their small draw ranges
exercise an already effective storage pool; real game ranges and GPU work differ.

Fifty focused comparisons against Apple GL passed, 25 per architecture.
They cover small indexed streams, sampler mutations, LOD and sampler arrays,
shadow references and ARB fragment programs. No comparison failed or crashed.
The disk shader cache was disabled for these comparisons.

Private muted checkpoint runs did not reliably leave the menu while another
MW3 instance was active. Menu timings are excluded from gameplay performance
results. Shader precompilation still helps first-use linking, but does not
remove index publication, asset streaming or every native pipeline creation.
This change addresses the measured index-allocation bottleneck; it does not
establish that all multi-second gameplay stalls are gone.

The normal `native/build/MW3-Compat.app` receives the new signed loader,
driver and compiler helper by atomic file replacement. An already running
game keeps its old mapped binaries. Quit and reopen the app to load the update.
Tests used copied profiles and muted background windows. User applications,
profile files and the game's assets were not changed by the tests.

## Remaining area-transition stalls

After the index-arena update, read-only live samples caught further native
pipeline creation during mission progression. One warning recorded an 848 ms
frame with 34 pipeline acquisitions taking 582 ms. GLSL/SPIRV-Cross translation
took 1 ms for that frame. Application-thread program compilation took 254 ms;
these categories can overlap and must not be added as independent costs.
Other recorded spikes included native function waits. A separate 385 ms frame
had no measured compiler or pipeline cost. The captures do not establish the
cause of every sustained 10 FPS interval reported by the player.

GLMetal previously discarded a prewarm request if no sufficiently confident
render recipe existed when its functions became ready. It now retains up to
128 skipped pairs for ten seconds and retries at most four matching pairs per
recipe observation. Demanded pairs leave that list. Full descriptor matching,
the two background compiler jobs, 32 outstanding predictions and 512 cached
pipelines are unchanged. Final shader variants or different render targets
still require their own exact pipelines. Shader output and compiler cache
identity are unchanged.

The compiler-only `pipeline_compile` probe now has a `deferred` mode. It loads
functions before learning recipes, then models preparation time before draws.
Two trials per architecture used 12 captured MW3 pairs, fresh Metal entry names
and a 10 ms gap. The baseline used the preceding prewarmer implementation with
the same probe. Neither cohort submitted GPU work.

| Architecture | Baseline first-draw pipeline waits | Updated waits | Updated reuse |
| --- | ---: | ---: | ---: |
| Native arm64 | 216.438 to 230.604 ms | 7.703 to 10.684 ms | 12 of 12 |
| Rosetta x86_64 | 222.420 to 223.884 ms | 8.720 to 13.920 ms | 12 of 12 |

Compilation moves into preparation time; it is not eliminated. This probe
demonstrates the missed-prewarm fix, not an equivalent gameplay FPS improvement.
The CPU mock also covers expiry, eviction, deduplication, demand removal,
the four-retry limit, queued/running demands and descriptor mismatches.

## Shared-loader upload staging

Non-presenting shared contexts submit texture uploads immediately. Reserving a
4 MB transient block for each tiny mip upload wasted staging capacity because
those submissions cannot share an arena. These uploads now use payload-sized
power-of-two pooled buffers. Their submission order is unchanged, and buffers
return to the pool only after the copying command buffer completes.

For 42 uploads covering six 64×64 RGBA mip chains, the size-class probe sums
240 KiB of new reservations versus 168 MiB with one old block per upload.
This is the reservation budget if all uploads remain in flight, not a measured
gameplay memory peak. Actual retention depends on GPU completion. Optional
`GLMETAL_UPLOAD_STAGING_STATS=1` logs reservations and pool reuse; it is disabled
by default. The live profiles do not prove staging caused the reported dip.

The new shared-context test checks 96 tiny mip/layer overwrites, client-byte
reuse and producer destruction before consumer reads. It and four existing
upload-staging cases match Apple GL on both architectures. Fourteen depth-clamp
and shader-variant comparisons also match on each architecture, giving 38
focused comparisons with no mismatches or crashes. The native buffer check
passes against the packaged driver.

## SDL presentation recording

MW2 and MW3 present through `NSOpenGLContext flushBuffer`. That path counted
swaps but never fed the detailed hitch recorder, leaving its log at the header.
It now records after the actual flush. Measured waits before presentation enter
the pacing category without changing their ordering; total frame intervals and
flush durations stay intact. Normal thresholds catch roughly 100 ms frames
that the separate 250 ms GLMetal warning threshold misses. Full import profiling
remains disabled by default.

The recorder CPU test passes. A private muted MW3 run produced a 41-frame
history through the actual SDL path, including draw, upload, runtime, work,
flush and pacing measurements. This was a startup/menu integration check,
not a reproduction of the player's sustained gameplay slowdown. Local samples,
probe results and comparison reports are under `build/mw3-area-20261010/` and
are excluded from Git. Reopening the normal updated app enables this recording
for the next mission progression capture.

## Buffer readback and completed queries, October 10

The next live MW3 session recorded a 1.09 second frame during a shader burst,
buffer mapping calls lasting 70 to 193 ms, and query getter calls lasting up to
85 ms. Caller disassembly shows a query availability check followed by a result
getter. These are individual call durations, not separate costs to add together.
Other 383 to 416 ms frames had no recorded draws. Live samples also caught engine
waiting, so these changes do not establish the cause of every loading stall.

CPU-authored buffer readback previously submitted and waited for all pending GPU
work. The command worker still applies earlier CPU uploads before readback, but
GLMetal now skips that GPU wait when the buffer has no recorded GPU writer.
Transform-feedback output retains the existing conservative submission and wait.
This also removes the unrelated GPU wait from bridge map-buffer staging reads.

Once a real query getter establishes completion, GLMetal retains the exact raw
64-bit result for that query generation. A subsequent availability or 32-bit
result getter returns it without another command-worker rendezvous. Signed and
unsigned saturation remain intact. Begin, delete, timestamp, indexed-query and
display-list paths invalidate the cache. The first availability check and all
necessary GPU completion waits remain unchanged.

The old hitch recorder allowed a startup presenter and the Backend presenter to
write the same frame counters. That made category totals and call attribution
unreliable after a thread handoff. Counters are now thread-local, and presentation
ownership has an atomic generation. Stale timestamps cannot reclaim ownership;
scopes crossing a handoff are discarded. Presentation history remains serialized,
and repeat sessions clear their history. The selected-call regression measured
roughly 25 to 27 ns per call, and nested scope recording roughly 37 to 38 ns.

CPU mocks pass for upload ordering, GPU writer waits, exact query values,
invalidation and error paths. AddressSanitizer query probes pass on both
architectures. Thirty-eight focused Apple GL comparisons pass, nineteen each on
arm64 and Rosetta. They cover buffer readback, first indexed draws after a flush,
existing query cases and completed-query reuse with changing zero/nonzero values.
There were no mismatches or crashes. Shader output and compiler identity are
unchanged. Evidence is under `build/mw3-followup-20261010/` and excluded from Git.

A muted private packaged MW3 startup run produced 41 recorded frames. Their
category sums stayed within frame work time. The native buffer check and signed
normal-app manifest checks passed. This was an integration check, not a replay
of the player's mission or a gameplay performance benchmark. The normal app
was refreshed atomically; the player's existing process stayed running.
