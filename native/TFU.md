# Star Wars: The Force Unleashed

This branch is an unfinished port of the Aspyr 1.2 Mac release. It uses the
same i386 loader as Portal 2 and the LEGO games, with additional Carbon,
AGL, and movie compatibility work. A checked, in-memory instruction patch
fixes the renderer's display-ID sentinel; the source executable is unchanged.

## Input layout

Keep the original app and the complete Assets directory beside each other:

```text
Star Wars The Force Unleashed/
  Star Wars The Force Unleashed.app/
  Assets/
    FMV/
    LevelPacks/
    Mac/
    Scum/
    Shared/
    ...
```

The supplied installation has about 23 GiB of data, including the prologue,
main campaign and DLC level packs. Startup successfully finds and selects
its Assets directory. This does not yet verify every level or asset.

The tested executable is the plain i386 binary in the primary `.app`, with
SHA-256 `ea4b66dd751b7f7e3528e22b8fc6d34b01c32a0a5e73702fe06366f16bbc9a4c`.
The separately supplied `.bk.app` has a different executable layout and is
not supported by this profile.

## Build

From this checkout:

```sh
cd native
make tfu-runtime
make bundle
```

This branch defaults to `GAME=tfu` and the supplied installation path.
For another location:

```sh
make GAME=tfu SOURCE_APP="/path/to/Star Wars The Force Unleashed.app" \
  ASSETS_DIR="/path/to/Assets" bundle
```

`tfu-runtime` uses the same verified Apple runtime preparation as Portal 2.
It downloads Apple's Lion archive when the runtime cache is missing; it
does not install Lion or modify system libraries.

The output is `native/build/TFU-Compat.app`. The build copies the original
application and Assets into the output, so allow space for another complete
installation. It only reads the source installation. Building a bundle is
not evidence that startup or gameplay works.

The Portal 2 GUI converter remains specific to Portal 2.

## Progress and checks

- All 1,244 TFU initializers execute successfully.
- A fresh game reaches playable Kashyyyk as Vader. Background tests use
  isolated saves and preferences, with framebuffer captures of the level.
  The complete campaign has not been tested.
- High-detail materials preserve the D3D9 shader operations in Aspyr's GLSL
  translation: RSQ takes the absolute value of its argument, and CMP selects
  each component without arithmetic on the unused value. The old translation
  spread invalid values through normal maps and lighting, producing black
  blocks and unlit surfaces. A host-GPU pixel test reproduces the original
  black result and verifies the corrected color. A 1920×1200 full-detail
  Kashyyyk comparison reduced exact-black pixels from 495,449 to 158.
  Gameplay regression checks use Aspyr's string `LowDetail="0"` (high detail),
  rather than a plist Boolean, and check the captured materials as well as HUD.
  `LP32_KEEP_TFU_GLSL=1` disables the correction for comparisons;
  `LP32_DUMP_GLSL=/path/to/directory` records GLSL sources and compile/link
  diagnostics. Source game files are unchanged.
- DXT-compressed 3D particle textures are expanded to RGBA8 during upload.
  The current driver rejected TFU's 128×128×16 DXT5 explosion animation,
  leaving a zero-sized texture and drawing opaque black squares. The bridge
  preserves every animation slice, alpha, and mip level; interpolation and
  rendering remain on the GPU. The conversion is specific to TFU and runs
  only when textures load. Tests compare DXT1/3/5 decoding with the native
  2D decoder and cover partial blocks, mip uploads, pixel-unpack buffers,
  and restoration of unpack/client-storage state. A 35-second Kashyyyk
  capture sequence confirms the distant fireballs and fading smoke render
  correctly. `LP32_KEEP_TFU_COMPRESSED_VOLUME=1` disables the fallback for
  comparisons. No asset files are rewritten.
- TFU's pixel-upload buffers preserve the last texture row. Aspyr writes a
  complete image but flushes only `pitch * (height - 1)` bytes, relying on
  coherent legacy mappings. The staged bridge previously left that last row
  zero. In a 32×17 float deformation texture this moved 89 foliage vertices
  to the world origin, stretching 150 triangles across Kashyyyk. TFU's classic
  writable pixel-unpack maps now commit the complete mapped image on unmap;
  vertex/index buffers retain explicit range flushing. A GPU regression
  reproduces the missing row and verifies every float after texture upload.
  The rebuilt app's fresh-game capture shows normal foliage after camera
  movement and a Force attack; its GPU texture dump contains the final row.
- Aspyr's mesh loader reads bone indices through a write-only GL mapping.
  TFU maps preserve the buffer's existing contents instead of returning
  stale staging data, which previously overwrote the mesh loader's stack.
  A remapping regression test covers this behavior. The gameplay path also
  requires a 64-bit atomic-add adapter with i386 argument/return widths.
- Repeated vertex/index mappings reuse a committed CPU copy instead of
  reading the same data back from the GPU. These copies live outside the
  i386 heap, with a 256 MiB limit and a readback fallback. Explicitly flushed
  ranges and subdata writes update the copy; replacing/deleting a store
  invalidates it. Pixel-pack buffers are excluded because GPU writes must
  remain visible. Buffer tests cover those transitions and shared contexts.
  Small streaming writes reuse up to 16 staging allocations instead of
  copying the complete 2 MiB VBO for every lock. Reuse is limited to explicit
  write-only maps in the same context and is invalidated by external writes
  or store replacement; read maps still see the committed buffer contents.
  Carbon task-local storage and frequently called GLSL uniforms also use
  direct ABI adapters. At the same stationary Kashyyyk checkpoint, high
  detail and 1920×1200, the combined changes measured about 30 FPS versus
  9.4 FPS with those optimizations disabled on the M5 Pro. This is one
  scene, not a campaign-wide performance claim. Comparison overrides are
  `LP32_NO_CARBON_TLS_FAST`, `LP32_NO_BUFFER_CACHE`,
  `LP32_NO_TFU_BUFFER_SHADOW`, `LP32_NO_TFU_MAP_REUSE`, and `LP32_NO_FAST_GLSL`.
- Worker scheduling uses Carbon's actual semaphore timeout status. Returning
  a generic error made TFU treat busy workers as available and crash.
- The math bridge implements TFU's `cbrt` import, which previously stopped
  gameplay at guest return address `0x003c01ed`. The i386 regression reproduces
  the stop with the original argument (`-0x1.d91fecb4512ccp-131`) and verifies
  the cube root, signed zero, infinities, NaN, extreme magnitudes, and repeated
  x87 returns. An audit of TFU's math imports also found missing `coshf`,
  `sinhf`, `tanhf`, `frexpf`, `ldexpf`, and `modff`; these now have adapters and
  guest tests, including four-byte output-pointer bounds. The math fixtures
  can run without active displays using `test_guest_dyld.py --math-only`.
- XML callback strings survive until parser disposal, and the main image's
  character-classification table is bound to the i386 runtime layout. These
  fixes let material definitions and Lua startup scripts load correctly.
- The loader supports TFU's larger image, defined indirect stubs, and
  external relocations, with a synthetic Mach-O regression test.
- Carbon file references, event callbacks, native HIObject subclasses,
  event timers, display records and task storage have i386 integration
  tests in `test-guest-dyld`.
- Asynchronous fork reads and their i386 completion callbacks are tested,
  including a partial read at EOF. AudioConverter's old single-buffer API
  uses the supported native conversion API with a guest callback adapter;
  an i386 PCM conversion test checks actual sample values.
- Startup constructs the original license dialog, including its
  4,365-character license text. The user accepted it for this test; that
  choice is recorded only in the separate test preferences domains. Normal builds
  retain the game's first-launch license flow.
- Separate first-launch tests display the license, registration, and
  graphics-options dialogs. License text draws directly into the Carbon
  view, avoiding the unsupported AppKit Carbon-window wrapper. Control
  and drawing-context event parameters preserve native pointers behind
  32-bit handles; drawing-state and rectangle conversion have pixel tests.
- The movie adapter plays the startup logos and the complete 182-second
  opening story movie through AVFoundation, then reaches gameplay without
  a skip input. QuickTime's visual/audio characteristics (`eyes`/`ears`)
  resolve to the corresponding tracks and sample times. The legacy UI
  thread services main-loop work while waiting for the movie so AVFoundation
  can start its player. Visual-context reuse, frame timing, decoding, and
  audio-track muting have regression checks. Movie audio is muted during
  automated tests; a listening check is still needed.
- The OpenAL adapter passes a silent buffer/source/parameter test.
- AGL pixel formats and contexts use NSOpenGL. Legacy pixel buffers use
  framebuffer objects because native CGL pixel-buffer creation fails on
  the tested Mac. Offscreen rendering, GPU texture transfer and shared
  context tests pass on the M5 Pro.
- The game window uses a Cocoa presentation window with a hidden Carbon
  peer for legacy event targets. Fullscreen presents only the game surface,
  without a title bar, on the display selected in the options dialog. Aspyr's
  separate black backdrop remains hidden. Render resolution is independent
  of the desktop mode, so macOS display settings do not change.
- The monitor chooser's i386 drawing/hit/tracking callbacks are adapted to
  native Carbon. Its QuickDraw monitor diagram draws into the HIView's Quartz
  context. Background tests exercise opening, canceling and accepting it.
- The built-in Retina panel has display ID 1 on the tested Mac. Aspyr used 1
  as the initial previous-display sentinel, causing its first renderer to
  reuse a nonexistent mode list. The checked patch changes that sentinel
  to UINT32_MAX. Both attached displays reach the rendered main menu.
- GameController.framework feeds the game's built-in Xbox 360 adapter.
  Select **Xbox 360 Controller** in TFU's input options and connect an Xbox,
  DualShock 4, or DualSense supported by macOS. Cross/Circle/Square/Triangle
  correspond to A/B/X/Y; Options is Start, Share is Back. Sticks, analog
  triggers, the D-pad, and stick clicks retain the game's Xbox mappings.
  Button prompts remain Xbox prompts. One controller is supported; rumble
  is not implemented. Keyboard/mouse mode remains available.
- Controller input follows the workspace's actual foreground process. TFU's
  Carbon event loop can leave Cocoa's `NSApp.isActive` false even while the
  game owns foreground input; using that Cocoa flag discarded physical
  controller input at the main menu. Input releases when another app is in
  front, while the controller stays connected. GameController receives
  hardware events with background monitoring enabled so its separate Cocoa
  activation filter cannot suppress input before the bridge's foreground
  check. Virtual-pad tests bypass that framework filter, so fresh launches
  also need a physical-controller check. The bridge discovers
  connections again after unplugging/reconnecting. Guest ABI tests cover
  axes, triggers, buttons, state packet numbers, held controls across focus
  loss/return, and disconnection. Simulated
  gamepad tests start a new game, operate both sticks, and pause/resume.
  A physical DualShock 4 has been checked at the menus: D-pad, left-stick,
  and Cross input reached the guest, and Cross advanced to the difficulty
  screen; the user confirmed it works. Live traces also confirmed the foreground gate changing when
  switching applications while Cocoa's active flag remained stale. A full
  physical-controller gameplay pass is still outstanding.
- Legacy HID enumeration remains empty because the old i386 IOKit device
  interfaces have no adapter; exposing native devices caused a null-interface
  startup crash. Controller input uses the Xbox adapter instead. These
  changes are specific to TFU and leave the LEGO controller bridge intact.

Fullscreen focus/input behavior and the remainder of the campaign need testing.
Rectangular QuickDraw region bounds support the fullscreen startup path.
Some legacy APIs still require implementation, including AGL font display
lists and additional QuickTime track controls. Do not treat these startup
checks as verification of the full game.

```sh
make test-tfu-loader
make test-tfu-platform
python3 tests/test_large_image.py
make test-guest-dyld
python3 tests/test_guest_dyld.py --math-only
make test-gl-shader-bridge test-gl-volume-bridge
python3 tests/test_tfu_startup.py
python3 tests/test_tfu_gameplay.py
```

`LP32_HEADLESS=1` stops startup before window creation or activation and
returns status 77 when it reaches that boundary. Adding
`LP32_BUILD_UI_ONLY=1` permits constructing a hidden Carbon window but
still stops before showing or activating it. Platform self-tests create
no game window; the movie test mutes audio and the OpenAL test does not
start playback:

```sh
LP32_HEADLESS=1 \
LP32_MOVIE_SELFTEST="/absolute/path/to/Assets/FMV/FMV-AttractMode.mov" \
arch -x86_64 build/game_loader build/TFU-Compat.app/Contents/SharedSupport/TFU.image
```

`LP32_BACKGROUND_TEST=1` presents the test window behind other apps and keeps
the guest's application state active without capturing desktop input or
activating its window. This is a diagnostic override; normal app launches
still follow real focus changes. `LP32_AGL_CAPTURE_FRAME=/tmp/tfu.ppm` saves
the app's framebuffer every 300 swaps for rendering checks without capturing
the desktop.
`LP32_MUTE_AUDIO=1` mutes both OpenAL output and movie playback for that
process while retaining audio decoding and timing. It does not change system
volume or saved game options. Gameplay and startup test scripts enable it
automatically; ordinary app launches retain sound.
`LP32_TRACE_BUFFER_CACHE=1` reports mapping hits, readbacks, and CPU-copy
memory use; `LP32_TRACE_MOVIES=1` records movie progress and delivered frames.
`LP32_CARBON_CAPTURE_WINDOW=/tmp/dialog.png` renders a shown Carbon dialog
and its controls into an offscreen image for first-launch checks.
`test_tfu_startup.py` requires the game's license to have already been accepted.
It uses a temporary preferences domain and isolated saves, checks fullscreen
on each attached monitor and windowed startup, and records app framebuffers
and actual Cocoa window geometry in `build/test-tfu-startup`. It does not
inject mouse or keyboard input or activate the game. `LP32_TEST_LOADER` can
point to the bundled executable to test the packaged build.

## September 7 stutter investigation

The system-log window 2026-09-07 16:01:32–16:07:32 (local time) includes
movie teardown at 16:04:20.878, then display-query gaps of 5.511 seconds and
6.957 seconds. The latter spans 16:04:27.664–16:04:34.621. These are log-event
gaps, not recorded frame times. Switching to OBS at 16:04:30.739 happened
after the gap began; a movie transaction delayed 9.872 seconds ran immediately
afterward. That warning alone does not establish a movie deadlock.

An isolated Continue replay of the saved ship-corridor checkpoint reproduced
a 6.416-second first-render gap after the loading movie. Profiling attributed
2.469 seconds to shader-status queries, and a sample caught Apple's GLSL
compiler plus millions of guest text-processing calls during shader creation.
TFU now uses direct libc adapters for those text calls. With the same profiling
enabled, text adapters reduced the measured gap to 5.058 seconds. With movie
pacing also enabled, it measured 4.912 seconds, with the following gap falling
from 1.287 to 0.440 seconds; subsequent gameplay held about 30 FPS at 1280×800,
high detail. This is one checkpoint comparison. **First-use shader stutter
is reduced, not eliminated**, and the original incident has no captured stack.

Carbon symbol lookup now caches exports and missing symbols in bounded
thread-local storage. Event polling fell from roughly 94–97 microseconds per
call to 4 microseconds. Movie presentation is capped at 60 FPS; previously
the movie loop submitted repeated video images over 1,000 times per second.
AVPlayer remains responsible for media timing. Comparison overrides are
`LP32_NO_TFU_FAST_TEXT`, `LP32_NO_CARBON_SYMBOL_CACHE`, and
`LP32_NO_TFU_MOVIE_PACING`.

TFU's finite semaphore waits now honor milliseconds, negative microseconds,
and a single deadline across spurious wakeups. Previously every nonzero wait
was infinite. Tests cover both finite units, later signals, immediate polling,
infinite waits, and deletion. The recorded replay's long worker waits requested
infinity; the finite-timeout bug is a separate correctness fix.

Slow AGL frame intervals, buffer swaps, shader compilation and program links
now emit local timestamps and durations above 100 ms. Normal Finder output is
in `~/Library/Logs/TFUCompat/last-run.log`; `LP32_DIAGNOSTIC_LOG` selects a
different file so tests can preserve the live game's log. The new replay test
copies the save into an isolated profile, uses virtual input and muted audio,
and checks for a rendered gameplay HUD:

```sh
python3 tests/test_tfu_continue.py
```

Artifacts are under `build/test-tfu-continue/`. `LP32_CONTINUE_SECONDS` changes
the observation period; `LP32_CONTINUE_SAMPLE=1` samples the first-render stall.
This exercises the copied checkpoint, not every campaign level.

## September 7: window activation and Grip training

TFU drives Carbon's event loop while its visible game surface is a Cocoa
window. Previously, activation could leave `NSApp.isActive`, `isKeyWindow`
and `isMainWindow` false. The bridge now dispatches AppKit-defined window
events to NSApp from `ReceiveNextEvent`, leaving keyboard and mouse events
with Carbon. Carbon and Cocoa activation both restore the visible game
window; the hidden backdrop, minimized windows, modal dialogs and background
tests are excluded. The event pump is restricted to TFU's main thread and
guards against reentry.

`python3 tests/test_tfu_focus.py` briefly launches an isolated foreground app
and verifies active/key/main state in windowed and fullscreen modes. This
passes on this host, as do the background startup checks on both displays.
The packaged build also passed both foreground modes and survived arming
SIGQUIT without any trace environment variable. Run the foreground probe
with the desktop idle: a launch that receives only deactivation events from
macOS cannot verify activation and will time out.
An interactive Cmd+Tab/Spaces check remains useful: the foreground test
exercises the same activation path at startup, but does not synthesize the
OS app switcher.

The reported white rectangular saber in the completed Grip tutorial is
**not yet fixed or reliably reproduced**. Replays through Pause → Training
Room → Grip Tutorial, including 1920×1200 fullscreen, rendered a green blade.
The live session reported an unloadable 1D texture before entering training;
the clean replays did not, so this is a clue, not a confirmed shader diagnosis.
`LP32_CONTINUE_GRIP=1 python3 tests/test_tfu_continue.py` replays the room
from a copied checkpoint, with captures before and after swinging the saber.
`LP32_CONTINUE_FULLSCREEN=1` selects the fullscreen reproduction mode.
`LP32_CONTINUE_SETTINGS=1` includes the startup settings dialog; the test
accepts it through the existing internal dialog harness. Entry captures are
saved before Grip or saber input, and `LP32_CONTINUE_GRIP_ENTRY_TRACE=1`
arms a render trace at that point after verifying the signal handler exists.

The follow-up fullscreen settings-dialog replay and a second entry in the
same process still rendered a properly shaped green saber. Its GPU RGBA8
readback matched `LightSaberGreen.dds` exactly, byte for byte. Selecting the
campaign's red crystal did not change the green training replay. These are
clean comparisons, not a reproduction or fix of the reported white strips.
An isolated comparison with `LP32_KEEP_TFU_GLSL=1` produced a black room and
exited with SIGILL before the blade capture; it does not establish the cause
of the white-strip issue. Keep the existing GLSL correction enabled.

Live GL tracing is now installed on ordinary TFU launches, not only launches
with `LP32_TRACE_GL_RENDER` set. The old AGL path lacked this handler, so
SIGQUIT could terminate the game instead of arming a capture. On the updated
build, SIGQUIT arms a bounded render trace in `~/Library/Logs/TFUCompat/gl-render.log`;
active GLSL sources are saved in `gl-programs/glsl-<pid>-<capture>/`, and
1D sampler and binding state is included alongside 2D, 3D and cube textures.
Only use this diagnostic after the startup log confirms
`enabled SIGQUIT-triggered GL tracing for context presentation`.
Live TFU traces also save small RGBA8, RGBA16F and RGBA32F texture readbacks under
`gl-programs/data-<pid>-<capture>/`; `LP32_DUMP_GL_DATA` overrides that directory.
Dimensions are recorded in the matching `texture-content` trace entry, and
the `.bin` files contain raw RGBA components in OpenGL row order.
RGBA16F targets are converted to 32-bit floats on readback so non-finite
values remain visible to diagnostics. Readback
restores pixel-pack parameters and the previous pack buffer. Shader compile
and program-link failures now log on normal TFU launches as well as diagnostic
launches; successful-program listings remain opt-in.
The packaged fullscreen replay `checkpoint-2rlvstnu` survived a live SIGQUIT
capture without dump environment variables. Its saved RGBA8 saber pixels
matched the green DDS exactly, and the visible blade remained correctly shaped.

One packaged background replay (`checkpoint-ccy1gwl4`) stalled after all 515
GoldGuy movie frames: the guest rewound it twice, leaving it paused at zero
without the expected StopMovie transition. The harness timed out and cleaned
up its isolated process. This intermittent startup issue remains unresolved;
the immediate retry advanced through both movies normally. This follow-up
does not change movie playback behavior.

## Reflection and activation follow-up (September 7, evening)

The user's live black-patch scene was captured from PID 8463, swaps
37764–37768 (`build/reflection-live-8463.log`, 1,141 draw calls). The hangar
floor program 265 samples the 256×256 RGBA16F reflection target 34, produced
by blur program 137 from target 33. Its bound texture levels were complete.
The original capture did not read back RGBA16F; diagnostics now preserve
those targets as float components as well. A copied-checkpoint comparison
(`checkpoint-7r9tqh_k`, capture 2) contains valid, finite scene reflections
in both targets. That doorway comparison is not the user's failing position.
The reported black floor patches and white training saber remain unverified.

Captured material shaders also translate D3D9 LOG as `log2(abs(x))`, which
misses the specified finite `-FLT_MAX` result for zero. The TFU-only repair
now supplies that result for either signed zero. A GPU regression reads an
RGBA32F target and checks both zeros and finite positive/negative arguments.
This fixes a documented translation mismatch; it has not yet been shown to
resolve the user's black patches. The combat probe `checkpoint-0ss_f01y`
compiled the revised material shaders and reached the sphere-room encounter;
its sampled float targets had no NaNs or infinities. Manually walking into
combat depleted the character's health, so the harness's green-health-bar
assertion failed. This was not counted as a passing checkpoint test. `LP32_KEEP_TFU_GLSL_LOG=1` disables just
this correction for comparisons.

The revised focus path enumerates all app windows (including other Spaces),
marks the actual game surface MoveToActiveSpace, and responds to workspace
activation as well as AppKit/Carbon activation. Deferred restoration checks
that this process is still foreground before raising its window. Diagnostic
window reports now include foreground, onActiveSpace and collectionBehavior.
The isolated fullscreen probe `activation-aup0aypw` passed initial activation;
the user reported that Cmd+Tab away/back worked while this test app was open.
The automated CUA key injection did not produce a verifiable app switch.
`LP32_FOCUS_OBSERVE_SECONDS` leaves the isolated probe alive for manual checks,
and `LP32_FOCUS_MODE` selects fullscreen or windowed. The subsequent windowed probe
`activation-mnua1eo_` and stationary checkpoint replay `checkpoint-0jod7zwa`
both passed with the new loader. The GPU GLSL regression also passed. This source change needs
a new game launch; it does not modify the already running app.

## Confirmed reflection NaNs and SIGILL follow-up

The updated user session (PID 31724) still showed the black reflection.
Its capture `build/reflection-live-31724.log` proves the invalid values arise
before the blur/floor passes: texture 33 contains 155 pixels with NaN RGB
(465 components), and blurred texture 34 contains 449 (1,347 components).
Alpha remains finite. The unblurred NaNs occupy a narrow strip at x=145–153,
y=165–207 in the 256×256 reflection. This is not fixed by the LOG correction.
The exact producing draw is not yet established. `LP32_TRACE_FLOAT_TARGET=33`
is a new opt-in diagnostic that scans the small attachment after each draw
of an armed trace and reports the program and nonfinite count. It is disabled
on normal launches; simply walking through the area does not automatically
capture the bad pixels. Existing timing/failure logs remain automatic.

The same session exited with SIGILL at 22:26:32, approximately half a second
after presentation resumed on foreground activation. Launchd confirms the
signal, but no new .ips report or fault address was available. The earlier
CPU-resource report says no action was taken and is unrelated to termination.
The user confirmed Cmd+Tab return immediately preceded the crash. A copied
checkpoint replay with the updated diagnostics (`checkpoint-j26zk8vy`) passed;
the activation crash itself remains unresolved. Evidence and the
original executable/save copy are preserved under `build/crash-31724/`.

TFU now installs the existing fatal diagnostic handler for SIGILL as well as
SIGSEGV/SIGBUS. `tests/test_tfu_crash_diagnostics.py` executes a real illegal
instruction under Rosetta and validates the instruction/register report;
it also checks the existing SIGSEGV path. Both passed. This is diagnostic
coverage, not a fix for the exception. Other game profiles' signal setup is
unchanged.

## Late-evening Cmd+Tab regression

The production session PID 14568 still failed to raise its window. The
WindowServer log (`build/focus-14568-system.log`) confirms real Cmd+Tab
transitions selected TFU at 22:41:54, 22:42:24 and 22:42:34. This establishes
process activation, but does not establish Cocoa's active/key state.
The user confirmed the window stays hidden, rather than appearing without
input. The saber becomes visible during attacks, but remains logically
ignited and deflects even when invisible during movement. This is a rendering
report, not evidence of normal holstering. A new screenshot at 22:49:39 shows
the invisible blade and black reflection patches around the doorway and
console in the earlier room. A bounded capture was armed in live PID 14568
after verifying its SIGQUIT handler; its starting byte offset is saved in
`build/saber-reflection-14568-offset`.

The completed capture is `build/saber-reflection-live-14568.log`, with 1,098
draws across swaps 25124–25125. Readbacks are in the diagnostic directory's
`gl-programs/data-14568-1/`; counts are saved in
`build/saber-reflection-14568/summary.json`. Reflection texture 33 contains
970 pixels with NaN RGB, and texture 34 contains 4,139; all alpha values are
finite. The standing saber draw uses program 159, 40 indices, additive
ONE/ONE blending and valid finite vertex data. Its bound base texture 249
is a complete 64×64 RGBA8 image, but every one of its 16,384 bytes is zero.
This differs from the green DDS readbacks in successful Grip captures and
explains why this additive blade draw contributes no color. Where the
texture becomes empty is not yet established. The attack trail uses a
separate material/texture, so its visibility does not prove the standing
blade texture is intact. The user declined a manual crystal-change comparison;
the independent upload investigation below supersedes that diagnostic.

A fresh copied-checkpoint comparison (`checkpoint-xx8rkks3`, save hash
`a3ae3924aa1a1c299571d5057f5ef79544dad71c5b89f0f287dd914bc399e1a0`)
timed out before startup movie completion with a black captured frame;
it produced no gameplay comparison and is not counted as a pass. The test
cleaned up its own process and preference domain. The live process remained
running throughout.

The next build reconciles foreground ownership from the Carbon event pump
at most ten times per second, so restoration does not depend exclusively on
workspace notifications or main-queue blocks. It repairs inactive Cocoa
state only while the system already selects TFU, and can restore an
OS-hidden surface whose guest-visible flag remains set. A reentry guard
covers synchronous activation notifications. Guest-hidden/minimized windows,
modal dialogs and background tests remain excluded. Low-volume foreground
transition diagnostics are enabled on ordinary launches.

This focus change compiled successfully with warnings treated as errors and was
promoted to the app bundle; strict code-signature verification passed. The live
game was left running; the revised production Cmd+Tab behavior still needs
an interactive test after relaunch. The saber investigation continued below.

## Foreground-query correction (September 8)

The user reported that the September 7 Cmd+Tab revision still leaves the
window hidden. The earlier restore guard and 10 Hz reconciliation both used
`NSWorkspace.frontmostApplication`. A read-only observer that did not run the
AppKit loop demonstrated that this query remained at Chrome PID 27075 for
28 seconds after TFU Focus Test PID 92330 became foreground. During the same
interval `GetFrontProcess`/`GetProcessPID` reported PID 92330 correctly.
The observer source and output are `build/focus-query-probe.{m,log}`.

Restoration and reconciliation now query the Process Manager directly. They
still require this process to own foreground before raising a window. This
removes dependence on notification-backed foreground state; it does not yet
prove that stale state explains every reported Cmd+Tab failure. Background,
headless, modal, guest-hidden, backdrop and minimized-window exclusions are
unchanged. Normal logs now include window visibility and active-Space state
at foreground transitions. Isolated tests can additionally record timestamped
window snapshots with `LP32_CARBON_FOCUS_HISTORY` alongside their window report.

The focus regression now requires actual foreground ownership and membership
in the active Space as well as visible/key/main/Cocoa-active state. The CUA
app-directed Cmd+Tab attempts did not create an OS app-switch transition and
are not counted as interactive verification. A separate input method was
requested so an actual Cmd+Tab return can be tested.

The revised build compiled with warnings as errors. The stronger windowed
and fullscreen checks passed in `activation-eu2tmgdk`. The preceding
`activation-tsbjgw5g` windowed run failed: its history shows the OS selected
Obsidian PID 1620 during startup, before Cocoa activation completed, and the
game remained in the background. The successful retry does not turn that
interrupted run into a pass. The user subsequently requested packaging the
fix without further testing. The corrected foreground query is included in
`build/TFU-Compat.app`; actual Cmd+Tab verification remains outstanding.
The user's running process 68133 was not restarted, so the update applies
on the next launch.

## Cmd+Tab window-ordering failure: reproduced (September 8, 01:22)

The foreground-query change was insufficient. At the user's request, a real
HID Cmd+Tab probe reproduced the remaining failure without force-activating
the test app first. On return to PID 45579, WindowServer selected that process
but kept the other TFU process's window 75732 above the test window 75820.
The test window simultaneously reported visible, key, main, Cocoa-active,
foreground and on-active-Space. This is why the previous focus checks passed
despite the window remaining covered. Evidence is in
`build/focus-45579-cold-switch.log` and `activation-qoz3voo7/fullscreen.json`.

The restore path now follows `makeKeyAndOrderFront:` with
`orderFrontRegardless`, guarded by direct foreground ownership and the
existing background/modal/minimized exclusions. This forces the server-side
ordering even when Cocoa's activation and key-window state already appear
correct. It does not set an always-on-top window level.

`tests/tfu_cmd_tab_probe.m` sends real Cmd+Tab away and back after two- and
ten-second waits. It independently checks Process Manager foreground and the
frontmost overlapping normal window in WindowServer's on-screen list. It
does not call activation APIs to prime the target. Build with
`make GAME=tfu build/tfu_cmd_tab_probe`; `LP32_FOCUS_REAL_SWITCH=1` opts the
focus harness into this desktop-interacting check and requires authorization
to switch apps. Ordinary focus checks do not inject desktop input.

The pre-fix executable failed this regression in `activation-ygr0_c27`: the
second return selected PID 64097 while window 75732 from PID 21932 remained
above target window 75850. The same probe passed on the final build in
`activation-52dk6_bz`: two windowed and two fullscreen returns, including
ten seconds away, all restored the actual WindowServer front window.
`build/order-front-final-regression.log` records the pass; the per-mode
`*-cmd-tab.jsonl` files retain the independently measured screen order.

The first fixed two-mode run passed windowed switching but timed out on a
stale fullscreen startup report (`activation-p_cv1nl9`). Its window-activation
event arrived after the last snapshot. Reports now also observe key/main
window notifications, and the final two-mode run passed with those observers.
The foreground guard is checked again immediately before unconditional
ordering. The verified executable is packaged in `build/TFU-Compat.app`;
the existing user process 21932 was not restarted.

After the isolated tests, the old user process 21932 crashed on a foreground
return. The diagnostic handler captured SIGILL at `0x7ff819ef70ed`, an explicit
UD2 in caulk's `consolidating_free_map::maybe_create_free_node` (+119), in the
macOS audio allocator. This identifies the trap site, not the corruption's
origin. It is a separate unresolved crash and is not claimed fixed by the
window-ordering change. The session log and symbolication are preserved as
`build/focus-21932-crash.log` and `build/focus-21932-crash-symbol.txt`.
The isolated focus harness mutes audio, so its passes do not validate this
audio failure. Bundle signing completed successfully; its executable text
section matches the tested loader (SHA256
`6221bc72d7819310006abbe42791bd7fbbb293f063b6097f89b73b1934c2f379`).

## Intro black screen and focus follow-up (September 8, 01:38)

At the user's request, investigation used the already-open process 21939,
without launching another game replay. Its CUA screenshot was completely
black. Sampling showed the main thread servicing the movie wait loop and
the render thread continuing movie polling. Enabling movie diagnostics in
that process showed GoldGuy at time 0, rate 0, 515 delivered frames, ready
player/item status, and no decoder error. A retained earlier failed run
(`checkpoint-xx8rkks3`) shows the same state after repeated
`GoToBeginningOfMovie` calls, following one `StartMovie`.

Three bridge corrections address this path:

- Intel QuickTime `TimeRecord.value` uses low-word/high-word order, as defined
  by the SDK's little-endian `wide`. Get/SetMovieTime had those words reversed;
  the guest's timestamp reader takes the first word as the low value.
- Seeks expose the requested time immediately, retain the requested playing
  state across AVPlayer's automatic pause at EOF, and resume from MoviesTask
  only when the latest seek completes. Old completions cannot override Stop
  or Dispose. EOF notifications arriving after a rewind are ignored.
- The main-thread movie wait service now pumps AppKit activation events and
  reconciles window focus. Intro playback bypasses the normal Carbon
  ReceiveNextEvent hook, so its prior CFRunLoop-only service missed this path.

The headless movie self-test passed twice using one reused visual context,
covering actual decoding, Intel TimeRecord layout, EOF rewind, 32 consecutive
seeks, and Stop/Dispose while a seek is pending. See
`build/intro-movie-selftest.log`. The first attempted self-test omitted the
required image argument and exited before testing; the corrected command
passed. No additional game/window replay or Start-spam reproduction was run.
The new intro focus path and the reported button sequence therefore still
need end-to-end validation. The bundle is updated for the next launch; process
21939 remains on its original executable. Its diagnostic sample and debugger
transcript are `build/intro-21939-sample.txt` and `build/intro-21939-debug.log`.

## Invisible standing blade: reproduced and repaired

No crystal change from the user was needed. A replay of the retained save
`a3ae3924aa1a1c299571d5057f5ef79544dad71c5b89f0f287dd914bc399e1a0`
reproduced the missing blade after an attack (`checkpoint-yel41huo`). Its
64×64 base texture 249 was entirely zero. Upload provenance in
`checkpoint-dbkx1spi` and `checkpoint-1z2q6ihn` established that unpack buffer
134 contains the red DDS level-zero payload byte for byte, but the subsequent
BGRA/UNSIGNED_INT_8_8_8_8_REV upload leaves the texture zeroed. The GL call
reports no error; unpack settings and mapping state are valid. Standalone
native uploads do not reproduce the failure, so its deeper driver/context
cause is not established.

The TFU bridge now transfers these packed BGRA unpack-buffer uploads through
a temporary CPU source. It preserves the unpack binding and pixel-store
state, checks the source range/alignment, and leaves other formats—including
float deformation textures—on the existing path. This adds a CPU copy and
readback for affected uploads. `LP32_KEEP_TFU_PACKED_PBO=1` restores the
original path for comparisons. Changing only the packed type to byte format
did not fix the reproduction and was discarded.

With the same retained checkpoint and graphics-only settings, the original
path (`checkpoint-vt3h5f0m`) produced the invisible blade; CPU transfer
(`checkpoint-vrg8n3ml`) produced a visible red standing blade and an exact
decoded-DDS texture match. The default packaged build subsequently passed
full intro playback, Continue, and the new nonempty-blade check without
upload tracing or experimental flags (`checkpoint-k08vvp7i`). Its captured
texture also matches the red DDS exactly. A comparison and default-build
capture are saved under `build/saber-upload-fix/`.

`make GAME=tfu test-gl-texture-provenance` covers CPU/PBO pixel agreement,
packed BGRA conversion, nonzero source offsets, preservation of unusual pack
state and both buffer bindings, and rejection of invalid packed offsets.
The existing compressed-volume regression also passed. These tests do not
establish that the separate reflection NaNs or the historical white-strip
Grip report are fixed.

An additional Grip replay (`checkpoint-ub71i70u`) reached the room and its
post-attack screenshot shows a normal red standing blade. Its automated
texture assertion failed because the entry-time trace contained no blade
draw, before the later attack; this run is not counted as a texture-check
pass. The original room-entry white-strip report remains unverified.

For independent graphics replays, `LP32_CONTINUE_SAVE` selects a retained
save to copy, `LP32_CONTINUE_SABER_TRACE=1` attacks then captures the standing
blade, and `LP32_CONTINUE_EXPECT_BLADE=1` rejects an empty base texture.
`LP32_CONTINUE_SKIP_INTROS=1` omits only GoldGuy/Aspyr movies and only when
`LP32_BACKGROUND_TEST` is also set; graphics runs then omit the full-movie
assertion. Normal launches and ordinary regression runs retain playback.
This avoids relying on the separately unresolved intermittent intro rewind.
`LP32_TRACE_TFU_TEXTURE_UPLOADS=/absolute/directory` records the small packed
texture uploads and their PBO contents. `LP32_TRACE_TFU_TEXTURE_ERRORS=1`
also drains GL errors and is intended only for isolated diagnostic runs.

## Focus lifecycle follow-up (September 8, 02:34)

The prior ordering repair did not fix the user's running game. A real
Cmd+Tab selected PID 45074 while its window 76227 remained behind Finder
(WindowServer position 6). The game's Cocoa window reported key/main false
even as NSApp was active and its key-window getter returned a game window.
Repeated makeKeyAndOrderFront/orderFrontRegardless calls did not recover it.
Evidence: `build/live-45074-select-game.log`,
`build/live-45074-windows.json`, and `build/live-45074-focus-failure.log`.

The debugging inspection interrupted that session: executing an Objective-C
LLDB expression while stopped in 32-bit guest code produced Rosetta's
`unsupported privilege level: bf5d` abort. Do not evaluate host expressions
at a guest-code stop. The original broken state was consequently lost.

The revised TFU bridge queues synthetic Carbon activation/deactivation
events instead of invoking guest handlers inside AppKit's key-window and
application-activation callbacks. Superclass resignation now completes
before the guest notification. The custom AppKit pump and restoration path
also call updateWindows, which was missing when bypassing NSApplication.run.
Incomplete key/main restoration is logged once, followed by a recovery entry
only when the window actually becomes key/main again.

This corrects lifecycle/reentrancy hazards; **the exact trigger of PID
45074's stale state has not been established**. Several tests also passed
with the previous code, so their passing alone is not proof that this
specific user failure is eliminated.

Validation now includes actual OS Cmd+Tab, WindowServer ordering, Cocoa
key/main flags and key/main window identity, rapid 100/200 ms away intervals,
and Continue from a save copy at the user's display resolution. Six rapid
gameplay returns passed in windowed mode with audio and the real controller
(`activation-10fjg8md/windowed-cmd-tab.jsonl`). Six passed in fullscreen with
muted audio and the real controller (`activation-11gq6rmi/fullscreen-cmd-tab.jsonl`).
The original save and preferences were checked unchanged.

The first fullscreen validation exited at intro teardown, before app
switching, in caulk's tiered_allocator::deallocate at 0x7ff819ef6073.
That audio allocator failure remains unresolved and is not a focus-test pass.
See `activation-10fjg8md/fullscreen.log` and
`build/focus-lifecycle-checkpoint.log`. The updated bundle is
`build/TFU-Compat.app`; its executable is replaced atomically and re-signed.

The user subsequently reported that the LucasArts intro repeated without
input on first launch, then confirmed "ok fixed". A normal macOS bundle
launch with the current focus revision traced one GoldGuy playback (17.111 s,
514 frames) followed by one Aspyr playback (9.955 s, 299 frames), in
`build/test-tfu-focus/plain-3a7qh_zh/run.log`. No additional movie transport
change was made for that report; temporary extra seek diagnostics were
removed. The test copy was stopped and the current signed bundle retained.

## Reflection NRM correction and stale focus recovery (September 8, 03:10)

The live PID 59944 again records the unresolved focus mismatch: NSApp's
key/main pointers identify window 76683, but that window's key/main flags
are false after the restoration calls. Its foreground log also confirms
normal process switching. At investigation time macOS was locked (foreground
PID 175, loginwindow), so current-window capture and actual Cmd+Tab validation
could not proceed. The game was retained; no checkpoint navigation or restart
was performed. A SIGQUIT capture and target-33 float probe are armed for
future gameplay frames. The old process does not contain the new changes.

The saved reflection capture from PID 14568 contains zero vector attributes
in draws to target 33: program 384 has 32 consecutive zero values in each
of attributes 5 and 7, and program 327 has zero vectors in attributes 1/5
on two other meshes. See `build/reflection-nrm-evidence.json` for buffer
offsets and vertex indices. These materials use `normalize(vec3(...))`.
D3D9 NRM specifies a finite FLT_MAX multiplier for zero squared length;
GLSL normalization produced NaN RGB on this GPU for that input. The TFU-only
shader correction now preserves D3D9's zero-vector behavior, keeping the
ordinary normalize path for nonzero vectors. `LP32_KEEP_TFU_GLSL_NRM=1`
disables only this correction. Recompilation also avoids duplicating the
LOG/NRM helper definitions.

The floating-target GPU regression shows original zero-vector lighting as
`nan,nan,nan,1` and repaired lighting as `0.25,0.25,0.25,1`. Positive and
negative nonzero normals agree before and after. The initial x86_64 GPU run
also passed the existing RSQ/CMP/LOG, uniform and shader-handle tests
(`build/nrm-shader-test.log`). A follow-up run adding recompilation checks
stalled without output; it is not a pass. An ARM64 alternative failed its
low-address mapping requirement, then was killed on launch with a smaller
pagezero; it is not validation of the x86_64 runtime. This proves
the normalization defect and its correction; **it does not yet establish
that all of the pictured reflection patches are eliminated**. The first
NaN-producing draw in the live scene remains to be captured.

Focus restoration now detects the observed contradictory AppKit identities.
After 250 ms of persistence it orders the same window out and back in using
public APIs, retaining the view and rendering context, and explicitly makes
it main. This is attempted once per foreground visit, guarded by the actual
foreground PID. Reconciliation also checks main-window status. **This is a
candidate recovery, not a verified Cmd+Tab fix**; real switching could not
be exercised while the Mac was locked. No movie transport change accompanies
these changes. The candidate bundle was built and its signature verified;
build/promotion output is in `build/nrm-focus-promote.log`. The old signed
executable is retained as `build/TFUCompat-before-nrm-focus-recovery`.

### Unlocked verification

After unlock, the previous user process (59944) exited when its paused
window was raised, before gameplay resumed. Its signal handler reports
SIGILL at caulk address 0x7ff819ef6084, immediately following a controller
disconnect message. No live reflection frames were captured. Preserve
`build/live-59944-return-crash.log`; the native allocator crash remains
unresolved and is not claimed fixed by the shader/focus changes.

The x86_64 GPU regression, including repeated shader compilation, now passes
(`build/nrm-unlocked-test.log`). With a copy of the user's save and muted
audio, `activation-5po67zmb` passed two real fullscreen Cmd+Tab returns and
`activation-ilc58qxw` passed six rapid returns. Both check WindowServer
ordering and AppKit key/main identity.

The latter test reached the pictured control-room doorway. Black patches
beneath the console and doorway trim are absent in
`build/nrm-reflection-fixed.png`. Target 33 was scanned after 288 draws over
swaps 15493–15494; every probe reported zero nonfinite components
(`build/nrm-scene-validation.json`, full trace in the test's fullscreen.log).
This verifies the reported doorway with the NRM correction enabled.

The stale-focus condition was then deliberately recreated in that isolated
game at a native x86_64 event-pump breakpoint (CS 0x2b): calling the window's
public resignKeyWindow/resignMainWindow methods left NSApp's pointers
identifying it while both flags were false. The prior sequence of
makeKeyAndOrderFront, orderFrontRegardless and updateWindows did not repair
it (`build/focus-induced-normal-restore.json`). After detaching, the new
250-ms recovery logged `rebuilding stale window order` and `restore recovered`;
both flags and identities became correct (`build/focus-induced-recovery.json`).
This directly exercises the observed failure state, rather than relying
solely on app-switch cycles that already passed older builds. The trigger
that originally puts Carbon/AppKit into that state is still unknown.
Two additional real Cmd+Tab returns passed after that repair
(`build/focus-after-stale-recovery.jsonl`). Attaching LLDB interrupted the
Python harness's process ownership: its observation loop reported an exit,
while PID 24942 remained alive, reparented to launchd, and completed the
separate switch probe. Thus the overall harness observation is not a pass;
the recorded switch checks and recovery snapshots are the validation.
The isolated process was explicitly stopped after collecting them.

### Normal-bundle fullscreen ordering follow-up

Normal LaunchServices startup through the user's preferences dialog exposed
a second failure in PID 86972. The stale-identity recovery worked, but the
normal-level fullscreen window still remained below Steam/Alacritty/T3 in
WindowServer order. AppKit arrangeInFront, activation and explicit relative
window ordering did not resolve it. Temporary window-level experiments were
restored; no Core Animation or private AppKit calls were added to the bridge.
Preserve `build/normal-86972-order-failure.log` and the `normal-*-check.jsonl`
artifacts as failures, not passing focus checks.

Fullscreen presentation now uses NSFloatingWindowLevel only while the actual
Process Manager foreground PID is TFU. AppKit deactivation and the existing
foreground poll restore NSNormalWindowLevel when switching away; leaving
fullscreen restores it too. The test probe includes the fullscreen surface
in its WindowServer ordering check and requires its level to return to zero
while another application is selected.

The signed normal bundle, PID 70466/window 76999, passed two real Cmd+Tab
returns with normal audio and the user's startup preferences dialog.
`build/fullscreen-level-cmd-tab.jsonl` records TFU at the front and level 3
on return, and Steam at the front with TFU lowered to level 0 while away,
including a ten-second away interval. The naturally recurring key/main
mismatch also logged recovery on these transitions. The normal app was
left running. This validates the combined repair in the launch path that
failed the earlier isolated tests, but does not resolve the separate caulk
allocator crash after the earlier long background/lock interval.

Both saved float textures (33 and 34) are finite and contain nonzero scene
data in `build/nrm-scene-raw-capture`. Diagnostic files were moved out of the
bundle's Assets/1 directory, where the literal LP32_DUMP_GLSL=1 test setting
had placed them; the clean updated bundle's signature was verified again.

## Fullscreen intro and loading dimensions

The saved 1920×1200 render mode is independent of the desktop's Cocoa
point dimensions. On the internal 1728×1117 display, QuickDraw port bounds
previously returned 1728×1117 while the AGL backing surface was 1920×1200.
TFU's intro/loading path derived its viewport from those bounds, leaving
unused space at the right and bottom until gameplay supplied its own size.
Port bounds, visible-region bounds, and PixMap dimensions now consistently
use the selected fullscreen surface size; windowed and offscreen ports
retain their normal view bounds. No resolution preference is overwritten.

Before/after intro captures in `build/test-intro-resolution/` measure
1728×1117 and 1920×1200 respectively on the same 1728×1117 desktop. The
fixed frame fills the render surface. `test_tfu_startup.py` now asserts
that a fullscreen intro viewport equals the selected surface dimensions.

## Keyboard input during intro playback

September 8 keyboard/intro crash: PID 10960's SIGILL at
`0x7ff80d0718a4` resolves to `_dispatch_assert_queue_fail`. An isolated
keyboard/mouse launch under LLDB caught the same assertion on the guest
worker: `KeyTranslate` in `carbon_bridge32_dispatch` called
`TISGetInputSourceProperty` / `TSMGetInputSourceProperty`, which requires
the main queue. This was not an AVPlayer or shader failure.

The bridge now obtains and retains Unicode layout data and keyboard type
on the main thread before guest startup. Workers translate against a
mutex-protected retained snapshot. Input-source notifications invalidate
the snapshot; the main event pump refreshes it without making workers wait
for main-queue dispatch. `make GAME=tfu test-tfu-keyboard` checks all key
down/up codes from a worker during concurrent snapshot replacement,
including Return and Escape. It passed, as did six synthetic Return
pulses during the LucasArts logo. An isolated normal-audio launch played
both logos once and accepted native Return at the menu/attract transition.
Evidence is under `build/test-enter-intro/`, including the original
assertion backtrace. The movie transport and focus code were unchanged.

## Saves and settings

The original game's documented locations are:

- `~/Documents/Aspyr/Star Wars The Force Unleashed`
- `~/Library/Preferences/com.aspyr.SWTFU.plist`

The build does not delete or migrate either location. Startup tests use
`LP32_TFU_USER_DATA_ROOT` for temporary Documents/Preferences folders and
`LP32_TFU_PREFERENCES_ID` for a separate preferences domain. Those settings
are test overrides, not changes to the normal save location.

The existing `SWTFU.BIN` was copied into an isolated profile and successfully
reloaded through Continue. [TFU's manual](https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/32430/manuals/SW%20Manual%20-%20PC4.pdf?t=1741729267) describes autosaves at mission starts
and periodic checkpoints; completing the whole first level is not required.
Continue restores the last autosave, not necessarily the position where the
application was closed. The pause menu also offers Save Game.
