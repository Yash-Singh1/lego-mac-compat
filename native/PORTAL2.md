# Portal 2 development

Portal 2 support is experimental. The supplied Mac application renders the
main menu, including its background video and text, and loads the opening
single-player relaxation room with its tutorial prompts. The game also exits
cleanly. Windowed and fullscreen startup render correctly; fullscreen has
also been checked after losing and regaining focus. The user reports that
the game launches and runs correctly. Sustained gameplay remains unverified.
The current work lives on the `portal2` branch in a separate checkout. No game
binaries or data are tracked by Git.

## Building and running

From `native/`:

```sh
make portal2-runtime
make GAME=portal2 SOURCE_APP="/Volumes/Storage/games/mac/Portal 2.app" bundle
open build/Portal2-Compat.app
```

`portal2-runtime` downloads Apple's 4.72 GB
[Lion archive](https://support.apple.com/en-us/106383), verifies its checksum,
and extracts only the i386 C++ runtime libraries. It never runs the installer
or package scripts. The archive is cached in `build/runtime-downloads` and
the resulting libraries in `build/guest-runtime`. For an existing download:

```sh
python3 tools/prepare_portal2_runtime.py --installer /path/to/InstallMacOSX.dmg
```

The bundle copies the game's approximately 11 GB of data to
`Contents/SharedSupport/Portal2`. The original application is only read.
Use `make GAME=portal2 promote-loader` to update the loader without copying
the game data again. A direct diagnostic launch accepts Source arguments:

```sh
arch -x86_64 build/Portal2-Compat.app/Contents/MacOS/Portal2Compat \
  -windowed -w 1280 -h 720 -novid
```

For fullscreen, choose **Options → Video → Display Mode → Full Screen**, then
**Apply**. Resolution is selected in the same menu. These are the game's
display controls; its fixed-size window does not expose macOS's green zoom
button. To override a saved windowed setting for one launch:

```sh
arch -x86_64 build/Portal2-Compat.app/Contents/MacOS/Portal2Compat -fullscreen -novid
```

The loader skips Portal 2's startup movies by supplying `-novid` on every
launch, including Finder and Dock launches. Their Bink audio uses legacy
Carbon Sound Manager calls that are not bridged (`NewSndCallBackUPP` is the
first); attempting to play them previously exited before reaching the menu.
In-game audio uses the existing AudioQueue bridge.

Launching normally uses the saved video settings. Avoid `-windowed` when
testing fullscreen. The development bundle is currently saved at fullscreen
1920×1200; that local preference is not part of the source or copied to other
installations.

Diagnostics are written to `~/Library/Logs/Portal2Compat/last-run.log`.
`LP32_TRACE_DYLD_INITIALIZERS=1` prints every guest library constructor.
`LP32_TRACE_DYLD_ERRORS=1` also prints failed library and symbol lookups,
including the engine's optional debug-library probes.
`LP32_TRACE_FONT=1` reports rejected ATSUI calls. `LP32_TRACE_DATES=1` traces
Cocoa date factories and guest autorelease pools.
`LP32_TRACE_EVENTS=1` traces Cocoa event delivery and keyboard callbacks.
`LP32_BACKGROUND_TEST=1` suppresses the compatibility layer's focus requests
during unattended tests and keeps its cursor released. Portal's inactive event
pump becomes nonblocking in this mode so background waiting does not distort
rendering measurements. `LP32_USE_SECONDARY_DISPLAY=1` selects the other monitor
when available; `LP32_DISPLAY_INDEX` selects a specific zero-based screen.

To play on the other display with a 120 FPS limit and an on-screen FPS counter,
quit the game first, then launch from `native/`:

```sh
LP32_USE_SECONDARY_DISPLAY=1 arch -x86_64 \
  build/Portal2-Compat.app/Contents/MacOS/Portal2Compat \
  -fullscreen +fps_max 120 +cl_showfps 1
```

The cap is an upper limit, independent of the display's refresh rate. This
selects the monitor without changing the saved rendering resolution. Add
`+load quick` to resume the latest quicksave. These launch options apply to
this session; a normal Finder launch continues to select the primary display.

`LP32_PORTAL_FRAME_STATS=1` reports presentation rate, mean/p50/p95/p99 frame
times, buffer-swap time and draw counts every 240 frames. `LP32_FRAME_STATS=1`
also measures imports, with more profiling overhead. For repeatable comparisons,
load the same quicksave, allow loading/shader compilation to finish, and keep
resolution and foreground/background mode the same. The diagnostic switches
`LP32_NO_BUFFER_CACHE=1` and `LP32_NO_CGL_CONTEXT_CACHE=1` restore the old lookup
paths for comparison. Unattended mode is for measurement; omit it when playing.

At the saved early chamber checkpoint, the September 5, 2026 comparison on
the secondary monitor measured about **15.6 FPS before and 42–43 FPS after**
the context/buffer caches, at unchanged 1920×1200 settings and about 710 draw
calls per frame. Typical 240-frame-window p95 times improved from about 66 ms
to 27 ms. Loading/shader-compilation windows were excluded and both runs used
the same unattended mode.

The second optimization pass retains a known buffer size across same-size
`glBufferData` orphaning. On success the size is unchanged; rejected parameters
leave the old store intact. A requested size change still invalidates the cache.
This removes the synchronous size query from streaming buffers each frame.
Two runs at the same checkpoint measured **54.9 and 52.7 FPS**, versus a new
baseline of **42.5 FPS**. Typical window p95 times fell from 27.3 ms to
21.6–23.5 ms. Rendering settings were unchanged, the cap was raised to 300
for measurement, and import profiling was off. Results and raw windows are
retained locally in `analysis/optimization2-results.json`.

Direct handlers for `memcmp`, `pthread_getspecific`, `pthread_setspecific`
and `pthread_self` were also tested. They reduced CPU dispatch work but
delivered about the same 54 FPS with worse p95 times (29.1–29.3 ms), so that
experiment was removed from the shipping loader. Its patch and TLS/memory
regression checks remain in `analysis/optimization2-direct-import-experiment.patch`.
These measurements describe this scene and machine, not every chamber. The
mean frame time at that stage was still above the 16.7 ms budget for 60 FPS.

### Mapped uploads and shader dispatch

The third pass replaces the `glBufferSubData` upload used by shadow mappings
with a native `glMapBuffer`/copy/explicit-flush/unmap sequence. The guest still
writes to its low-address staging allocation. The native mapping follows the
buffer's serialization and flush policies; it avoids the Metal-backed OpenGL
driver's repeated render-pass breaks and GPU copies for streaming writes.
Only validated mapped ranges use this path. Ordinary `glBufferSubData` calls
keep their existing behavior, and failed native mappings fall back to it.

Shader constant updates and program binding also use direct bridge handlers
for their core and ARB names, avoiding repeated import-name searches. Both
changes are Portal-only. `LP32_NO_MAPPED_UPLOAD=1` and `LP32_NO_FAST_GLSL=1`
disable them independently for same-binary comparisons.

At the same heavy checkpoint on September 5, the final comparison used
1920×1200 on the Retina display, a 300 FPS cap, 708–709 draws per frame, and
six complete 240-frame gameplay windows per run. The camera stayed still,
loading windows were excluded, import profiling was off, and background
waiting was disabled in every run. Roblox was temporarily suspended for these
three runs and resumed afterward. Desktop apps remained available.

| Configuration | FPS | Median of window p95 frame times |
| --- | ---: | ---: |
| Previous upload/dispatch paths, repaired x87 JIT | 51.7 | 31.5 ms |
| Mapped uploads/direct shader dispatch, repaired x87 JIT | 73.0 | 16.0 ms |
| Mapped uploads/direct shader dispatch, stock Rosetta | 72.3 | 15.3 ms |

The complete change improves this comparison by about **41%**, without changing
resolution or graphics quality. Earlier mapped-upload repeats measured
70.2–70.8 FPS while Roblox was running. Splitting uploads into 32/60 KiB chunks
reduced p95 times but stayed near 53 FPS; that experiment was removed. Disabling
the GL worker queue through Source's `-glmdisablemtgl -glmdisablemtgl2` options
regressed mapped uploads to 54.1 FPS, so normal threading is retained.

The GL buffer self-test covers partial and large uploads, preserved bytes,
explicit and implicit flushes, shared contexts, and buffer-size changes. An
offscreen pixel test checks queued draws during serialized overwrites,
nonoverlapping unsynchronized updates, and orphaning. Both the new upload path
and its fallback pass. The GLSL bridge's rendered-pixel test also passes.
Run the buffer test from `native/` with:

```sh
LP32_GL_BUFFER_SELFTEST=1 arch -x86_64 build/game_loader \
  build/Portal2-Compat.app/Contents/SharedSupport/Portal2.image
```

Raw windows and experiment notes are retained locally in
`analysis/upload-results.json` and `analysis/upload-*.log`. These measurements
apply to this checkpoint and machine; they do not imply a steady 120 FPS.

### Rosetta x87 investigation

The experimental [rosettax87_jit](https://github.com/Lifeisawful/rosettax87_jit)
hooks Rosetta's translator; its flags do not affect an ordinary Rosetta launch.
Revision `a44f1eff28591a90f4a8c7297134294811da06b5` was built under `analysis/`
on macOS 26.5.2, with Rosetta version `0x16f0240000000`.

The initial arithmetic failure was **register corruption during translation**,
not an incorrect implementation of addition. An x87 calculation stored directly
to memory produced `3.0`, but passing a double into a function translated for
the first time changed its bits to `0x0706050403020100`. Passing an ordinary
SSE constant reproduced it too. Preserving guest SIMD registers around the
translation hook fixed that failure. The decoder hook had a related issue,
which crashed AppKit's window-frame image conversion; it needs the same
protection. A further freestanding build issue made compiler-generated
`memset`/`bzero` implementations recurse indefinitely. Disabling those builtin
transformations removed the startup hang.

The complete fix is preserved in
[patches/rosettax87-preserve-simd.patch](patches/rosettax87-preserve-simd.patch),
with pinned build and launch instructions in [patches/README.md](patches/README.md).
The corrected runtime passes 11 upstream suites covering arithmetic, rounding,
control words, comparisons, conditional moves, constants and stack behavior.
Our four i386 dynamic-linker/ABI fixture combinations also pass through it.
A real 32-bit code-segment x87 probe and a cold-call SIMD preservation check
pass. [tests/test_rosettax87_cold.c](tests/test_rosettax87_cold.c) preserves the
minimal regression. Portal also loads and renders the heavy checkpoint with
the full decoder and JIT enabled.

On this M5 Pro, the upstream addition microbenchmarks improved by **29–61×**
with default optimization settings. That does not predict a game's overall
speedup: x87 is only part of the frame's work. Two stationary Portal runs at
the same heavy checkpoint measured **50.8 and 49.4 FPS**, versus **53.9 FPS**
for the initial stock run. A later stationary stock repeat measured **45.0 FPS**.
Settings stayed at 1920×1200 on the Retina display. Typical 240-frame-window p95
times were 30.0–31.8 ms with the JIT, 22.9 ms in the initial stock run and
30.5 ms in the later stock run. A comparison in which the user moved and fired
portals was excluded entirely. Roblox was also using about one CPU core during
the final stock repeat. Concurrent work was not controlled, so these measurements
**do not establish a net game speedup or regression**.
The game measurements use a 300 FPS cap, unattended mode, and no import profiling;
loading windows are excluded. CPU sampling happens after the measured windows.

Before the mapped-upload optimization, the JIT-run sample spent about 23% of
the main-thread samples waiting
for the OpenGL command queue during `glBufferSubData`, which identifies buffer
upload synchronization as a bottleneck. After addressing it, the controlled
comparison above measured 73.0 FPS with the repaired JIT and 72.3 FPS with stock
Rosetta. This small difference does not establish a useful additional x87 gain
for this scene. Normal app launches retain stock Rosetta; the patched JIT remains an
explicit experimental launch option. Precision/rounding tradeoff flags remain
disabled, and no system security settings were changed. Raw results and logs
are retained locally in `analysis/x87-results.json` and `analysis/x87-*.log`.

## Coexistence with LEGO

The engines can share the loader, memory allocator, mode-switch gateway and
OS bridges. Portal 2 adds a game profile and a guest dynamic linker; it does
not require removing the Pirates or Clone Wars profiles. Their executable
patches and controller layouts remain selected per title.

The important differences are:

- Portal 2's small universal i386 launcher opens Source libraries at runtime.
  `guest_dyld.c` maps these below the guest stacks, relocates and binds them,
  registers legacy Objective-C classes, and runs dependency constructors.
  Both classic relocations and compressed dyld information are supported.
- Guest library handles and symbols refer to i386 code. System calls cross
  the existing host bridge. Loaded libraries stay mapped because callbacks
  can retain code pointers after `dlclose`.
- Valve's Cocoa subclasses need guest instance storage and native callback
  adapters. The older Objective-C structure-return convention and Source's
  pthread argument convention are selected only for Portal 2.
- Cocoa windows expose stable guest handles for Source's remaining Carbon
  window calls. Showing, minimizing, restoring, focusing and invalidating the
  game window run on the AppKit thread. This prevents fullscreen focus changes
  from trapping on `ShowWindow`/`CollapseWindow`. `GetGlobalMouse` converts the
  native mouse position to Carbon's four-byte, top-left-origin `Point`.
- Portal 2's guest cursor visibility uses a balanced hide count, since the
  deprecated global query can disagree while another app has focus. Extra
  show requests cannot underflow that count. The bridge owns at most one
  native hide and applies capture/hiding only while Portal's game window is
  key and its application is active. AppKit focus loss releases the mouse
  immediately; background hide/capture requests only update guest intent,
  and background cursor warps are suppressed. Returning to Portal reapplies
  its current intent, so a menu stays uncaptured. Source retains control of
  pausing and menus. `LP32_TRACE_DISPLAY=1` reports focus, requested/effective
  cursor state, and suppressed background warps.
- Repeated `CGLGetCurrentContext` calls reuse the guest handle for the native
  context on that thread, avoiding temporary Cocoa objects and proxy-table
  searches on each call. Portal's buffer bridge tracks bindings per context,
  caches immutable share-group identity, and caches buffer sizes after querying
  them once. Buffer reallocation and
  deletion invalidate that state. This avoids draining Apple's threaded GL
  queue at every map, partial flush and unmap; uploads and rendering settings
  retain their existing behavior. This follows Apple's guidance to
  [keep copies of repeatedly queried GL state](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_designstrategies/opengl_designstrategies.html).
- The old C++ stream ABI needs more than the LEGO runtime's small string
  implementation. `LP32_GUEST_RUNTIME_DIR` can point to a private directory
  containing i386 `libstdc++.6.dylib` and `libc++abi.dylib`. The bundle also checks its own
  `compat-runtime` directory; `GUEST_RUNTIME_DIR` selects the directory copied
  by `make bundle` (default `build/guest-runtime`). Host `libSystem` and
  frameworks always remain bridged.
- Source's GLSL shader-object calls use native OpenGL shader IDs, with
  conversion of 32-bit string arrays. Legacy ATS/ATSUI font calls use CoreText
  for font selection, glyph metrics, shaping and bitmap drawing.
- AudioQueue output has low-memory buffer mirrors and native-to-guest
  callbacks. BSD sockets, page mappings, suspended threads and guest
  `setjmp`/`longjmp` support the engine's additional runtime requirements.

The loader currently targets the supplied `portal2_osx` image (entry `0x1cf0`,
image end `0x335c`), not every historical Portal 2 release. Dynamic unloading,
full dyld search semantics, arbitrary legacy Objective-C signatures and a
complete C++/OpenGL compatibility surface are not implemented.
Both windowed and fullscreen presentation use Cocoa. Display-wide gamma
adjustment is unavailable. AudioQueue disposal waits for native callbacks to
finish, including when the caller permits asynchronous disposal. Guest
`longjmp` cannot unwind across a native-to-guest callback boundary. Font
rendering has been checked in the menu and the first room's tutorial prompts.
Automated console input previously duplicated characters; the video-menu
selection check passes with valid window handles, but console input still
needs rechecking. Controllers, co-op and community content remain unverified.
Temporary dates follow guest autorelease-pool lifetimes, and
CFString Create/Release calls reclaim their handles; long sessions may still
expose limits in other retained Cocoa-object and thread registries.

## Checks

```sh
make test-macho-file test-guest-dyld
make test-gl-shader-bridge
make test-cursor-state
make GAME=portal2 test-portal2-loader
make test-arb-program-guard test-arb-sampler-usage
LP32_OBJC_PROXY_SELFTEST=1 arch -x86_64 build/game_loader
LP32_GL_BUFFER_SELFTEST=1 arch -x86_64 build/game_loader \
  build/Portal2-Compat.app/Contents/SharedSupport/Portal2.image
```

The standalone fixtures need no game data or historical SDK. They build tiny
original i386 libraries with Apple's linker and check both relocation
formats, function and data pointers, dependency initialization order,
one-time initialization, universal slices, missing symbols and rollback
after malformed-library rejection. They also exercise re-exports, weak
imports, the two Darwin `stat` layouts, 64-bit file offsets, both directory
entry layouts, reentrant directory reads, directory filter and sort callbacks,
deferred signal delivery and `iconv` pointer conversion. Wide-format checks
cover Unicode strings, 32-bit and 64-bit integers, floating-point values,
variadic widths, truncation, and the size of guest `%n` writes.
The legacy mouse-position check guards both sides of the four-byte output.
Cursor checks cover nested hide/show requests and excess show calls while
the fixture runs in the background. The cursor-state test uses a fake host
to check immediate release on focus loss, suppression of repeated background
capture/warp requests, restoration on return, menu transitions while inactive,
and retrying failed host releases without skipping cursor visibility cleanup.
Additional fixtures cover protected page allocation, time conversion, UDP
loopback, process pipes, CoreText glyph metrics and bitmap coverage, repeated
font-layout disposal, suspended threads, AudioQueue callbacks and reuse, and
guest stack recovery with signal masks. The Cocoa check covers point
conversion, repeated event delivery, and date lifetimes across nested pools.
The font fixture also checks thousands of CFString allocations and releases,
including overlapping references to the same native string.
The OpenGL test compiles and links
GLSL through the bridge and verifies a rendered pixel in an offscreen buffer.
The buffer test checks partial uploads, shared-context bindings and handles,
growth, shrinkage, same-size orphaning, rejected allocation parameters, and
binding invalidation on deletion.
The Portal 2 loader check maps the real
launcher and dependencies without executing game constructors or entering
the UI. Passing these checks is not evidence that the game is playable.
