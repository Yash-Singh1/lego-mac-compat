# MW2 performance measurements

## Background pacing

While MW2 is not the active app, the loader limits it to `LP32_INACTIVE_FPS`
presents per second: 15 by default, and 0 turns the limit off. A game left
open in the background previously ran at full speed. One MW3 window left open
for two hours used about 3.2 cores and a large share of the GPU while MW2 was
being played. Scripted tests are exempt; with `LP32_TEST_INACTIVE_PACING=1`, a
background test measured 14 FPS. `bench.py` can now use a copy of the user's
profile (`--profile`, `--only-saves PREFIX`), run a custom key script with held
keys (`--keys "T:KEY[:HOLD],..."`), record for a fixed time (`--fixed`,
`--skip`) and sample at a set time (`--sample-at`).

## Fourth pass: mission loading and gamma

Mission loading means the time from confirming "Resume Game" until the
cutscene frame rate shows loading has finished. Two matched runs of each build
measured it at 10.19 and 10.13 seconds for the third-pass build, and 6.08 and
5.86 seconds now: about 41% less. `bench.py` reports this as `load_seconds`.
Frame-rate reports now come every 30 frames, so the end of loading is detected
more precisely. Earlier `resume_to_skip` values include about 2 seconds of
scripted menu input before the confirmation. Gameplay frame rate is unchanged.

Sampling the load showed that no thread was saturated. The fastfile loader
made about 20 million libc++ `std::string` calls, 930,000 `vsnprintf` calls,
and several hundred thousand `operator new`/`delete`, TLS and 64-bit division
calls. Most had no direct handler, so each call string-compared its name through
every bridge (future, stream, QuickTime, Carbon, ...) before reaching its
implementation. The retained changes are:

- Direct handlers for the hot narrow `std::string` members (`reserve`,
  `append`, `push_back`, copy/`__init` constructors, `assign`, `operator=`,
  `compare`). They share the chained implementations' helpers. A failure still
  reports the trapped import, through `compat_runtime32_trap_import`.
- Direct handlers for `vsnprintf`/`snprintf`/`sprintf` (the printf family now
  lives in `guest_printf_family`), `operator new`/`delete`, `malloc`/`calloc`/
  `free`, `bzero`, `strncmp`, 64-bit division, `mach_absolute_time`,
  `_lp32_tlv_get_addr` and common float/double math. The formatter no longer
  calls `getenv` on each call.
- MW2 single-player: two byte-checked patches remove the `Sleep(1)` that
  `Load_CreateMaterialPixelShader`/`VertexShader` performed after every 10 ms of
  shader creation, a yield meant for single-core machines.

`make test-runtime-files` now checks that each new direct handler matches the
chained result, including x87 float/double returns. The libc++ test compares
every direct string handler against the chained implementation.

The remaining load time is mostly Apple's GLSL compiler (`glCompileShader`,
~17% of the loader thread) and the game's own i386 code under Rosetta.

### Display gamma

MW2 set its brightness curve (`r_gamma`) with `CGSetDisplayTransferByTable`.
While the game was active, the curve darkened the whole display, including the
menu bar, the Dock and other windows. The earlier COD4 fix only restored the
display when the game lost focus. For MW2, the ramp is now never sent to the
display. It is applied to the game's own frame through a 256-entry lookup
table. Reads of the ramp still return the game's values.

MW2 finishes each frame by blitting its resolved scene to the window. The bridge
performs that blit into its own texture instead, draws the corrected image
offscreen and blits it to the window. Presenting a window whose last write was a
draw made `flushBuffer` wait for the GPU and cost about 3 FPS. A frame that does
not end with a full-window blit falls back to reading the window at present
time. The pass saves and restores all core-profile state it changes and costs
no measurable frame rate. Captures match the display ramp: mean brightness fell
from 111 to about 96, versus 98 predicted from the curve.
`LP32_DISPLAY_GAMMA=1` restores the display ramp. `make test-gamma-focus`
covers the table and formula conversions.

## Third pass

Sampling showed the game thread idle about 65% of the time, waiting on the
render (`Backend`) thread, so this pass targeted render-thread costs. Five
matched runs of the training scene, 15 to 35 seconds after the cutscene skip,
measured 38.3, 39.5 and 42.1 FPS for the previous build and 53.8, 51.3 and
53.5 FPS for this one: about 33% faster. Resume-to-skip loading stayed about
12 seconds for both builds. `build/mw2-performance/bench.py` runs these
measurements. With `--sample`, it also records a 10-second `sample` profile.

The retained changes are:

- Dynamic index buffers: each draw uploads only its own index range to a
  stream element buffer. Before, the whole 2 MB copy was republished after
  every lock, about 26 times per frame, uploading ~53 MB per frame to publish
  ~55 KB of changed indices. Uploads are now ~105 KB per frame.
- Redundant `glBindTexture` filtering. About 8,400 of 10,200 binds per frame
  named the texture already bound, and Apple's GL revalidated each texture at
  the next draw. Texture deletes, any `NSOpenGLContext` message and each
  present invalidate the cache. `LP32_NO_BIND_FILTER=1` disables it.
- Appends to the 1 MB dynamic vertex ring use an unsynchronized mapped write.
  They previously used `glBufferSubData`, which cost ~55 µs per call, even for
  128 bytes. Only bytes past the highest offset written since the last orphan
  qualify; mapped buffers keep the old path. `LP32_NO_APPEND_RING=1` disables it.
- A whole-buffer `GL_MAP_INVALIDATE_BUFFER_BIT` unmap replaces the buffer with
  `glBufferData`. Unmaps on the presenting context no longer call `glFlush`.
  Loading contexts still flush.
- `__dynamic_cast` caches its result per (vtable, complete-object vtable,
  source, target) and no longer calls `getenv` on every cast.

These changes apply to MW2 only, except the RTTI cache. Remaining render-thread
time is mostly Apple's per-draw GL cost (~3,200 draws per frame) and i386 game
code under Rosetta. There are ~80,000 guest-to-host import transitions per frame.
Main-thread GPU-sync polling (`R_SyncGpu` → `IDirect3DQuery9_Mac::GetData`)
still contends for the D3D device lock, costing the render thread about 6%.

The regression commands below passed. So did the GL buffer self-test for the
new slice semantics, and new RTTI tests for cached displacements. Firing replays,
including bursts during capture (`Final-*/darkest.png`), showed no bullet-mark
triangles. The darkest-sky score was 0.036–0.055, versus 0.071 for the
previously verified build.

## Second pass

The next pass compared the first-pass build against another optimized build at
the same settings. In the first matched gameplay runs, throughput increased from
28.8 to 38.7 FPS, about 34%. Loading improved less, from roughly 10 seconds to 9.
Logs and frame counts are in `build/mw2-performance/round2-measurements.json`.

The repeat test skipped the movie automatically when its frame timing indicated
that loading had finished, then checked captured frames to confirm entry into the
mission. It produced:

| Metric | First-pass build | Second-pass build |
| --- | --- | --- |
| Resume confirmation to cutscene skip | 10.37 s | 9.37 s |
| Training scene after the transition | 29.4 FPS | 37.6 FPS |

Frame-rate measurements use complete reporting intervals 15 to 35 seconds after
the skip, excluding the initial shader/draw warmup. This repeat showed about 28%
more frames per second and one second less loading. The earlier fixed-skip run
showed 34% more frames per second. Both runs kept resolution and effects unchanged.

The retained changes are:

- Run MW2's `strcmp` and `__tolower` directly in i386 code, avoiding a mode switch
  for each call. Comparisons read one byte at a time; case conversion uses a table
  populated from the host locale and refreshed after successful `setlocale` calls.
  MW1 keeps its existing libc import path. `LP32_NO_GUEST_LIBC=1` selects the old
  path for MW2 troubleshooting.
- Cache RTTI registry indices for frequently cast resource types. Each hit checks
  the live registry, and each cast still calculates virtual-base offsets from its
  object. The cache does not assume that different objects have identical offsets.
- Use direct import handlers for `pthread_getspecific` and `__dynamic_cast`,
  retaining the existing thread-local values and cast implementation.

Two experiments were rejected. Loading the bundled shader-preload lists by
aliasing their original bundle names caused a large loading-time regression.
Batching mapped-buffer flushes in the presenting context gave no clear frame-rate
gain. Neither change is installed; the original upload synchronization remains.

The new tests cover i386 string calls through imports and `dlsym`, NUL terminators
at protected page boundaries, unsigned-byte comparisons, case conversion and locale
refresh, thread-local isolation, and RTTI cache collisions and registry changes.
The full runtime, string, profile, RTTI and graphics tests passed, as did MW1
campaign/multiplayer and MW2 initializer checks.

## First-pass reference

Measured on 2026-09-26 with an Apple M5 Pro. Each launch used an isolated copy
of the same training-mission save and the user's graphics settings: 1920x1200,
2x antialiasing, shadows, depth of field and specular lighting enabled, VSync
disabled. The user's running game was suspended during measurement and resumed
afterward. Its settings and saves were not edited.

## Changes

- Build libc++ strings in place and grow their capacity geometrically. Shader
  translation previously copied the whole string twice on each append. Preserve
  overlapping inputs before moving or reallocating storage.
- Cache a direct handler for the frequently called libc++ string destructor.
- Use the existing typed core OpenGL handlers through the import cache. Keep
  mapped-buffer and framebuffer bookkeeping in the normal dispatcher.
- Replace two MW2 mutex polling loops with their existing blocking lock call.
  These patches require matching instruction bytes and only apply to MW2
  single-player. MW1 and MW2 multiplayer have no such patches.

## Measuring

| Metric | Before | After |
| --- | --- | --- |
| Training mission loading, latest matched launches | about 19 s | about 10 s |
| Opening training scene, 600 presented frames | 22.8 FPS | 25.4 FPS |

Earlier launches took about 26 seconds before and 12 seconds after. Loading
therefore varies with cache and scheduling state; these are local measurements,
not guaranteed times. The gameplay gain is modest. OpenGL buffer uploads and
driver waits remain a substantial cost in the sampled rendering thread.

`LP32_FRAME_INTERVAL=120` prints frame timing without instrumenting every import.
`LP32_FRAME_STATS=120 LP32_FRAME_STATS_IMPORTS=30` includes import costs, but its
per-call timing overhead is substantial during shader loading. Use separate runs
for profiling and elapsed-time comparisons.

The before/after test launches used `LP32_TRACE_GL_FRAMES=1` for timestamps every
300 frames and captured a framebuffer every 60 frames. Loading starts when the
resume confirmation is accepted and ends when the loading bar completes and
disappears. Escape at 65 seconds skips the movie in both builds. Gameplay timing
excludes that transition. This is a training-scene comparison, not a campaign-wide
benchmark or a cold disk-cache test.

Local scripts, logs, samples and captures are in `build/mw2-performance/`.
The retained `measurements.json` identifies the timed runs. The gameplay windows
were 82.812 to 109.178 seconds before, and 80.275 to 103.907 seconds after.

## Regression checks

```sh
make -C native GAME=cod4 test-rtti-bridge test-libcpp-bridge test-game-profile \
  test-gl-core-bridge test-gl-buffer test-cod4-runtime test-cod4-initializers
make -C native GAME=cod4mp test-cod4-initializers
make -C native GAME=mw2 test-cod4-initializers
```

All passed. The string tests cover long repeated appends, self-append across
reallocation, overlapping insert/erase, short and wide strings, and destructor
dispatch. Graphics tests cover both core calls and the mapped-buffer paths that
must retain their bookkeeping. MW1 campaign and multiplayer ran all 45 and 49
initializers; MW2 ran all 28. The installed MW1 apps were not updated.

Two exploratory launches trapped before the first frame while entering SDL's
startup alert code. Subsequent launches, including another launch with the same
scratch profile, succeeded. The cause of those two startup failures was not
established; they are excluded from the timing results. Full campaign progression
and every startup error dialog have not been tested.
