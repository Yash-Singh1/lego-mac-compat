# MW2 range flicker fix

September 26, 2026.

## Cause and correction

MW2's mark draw lists retain CPU index pointers after `R_EndMeshVerts` unlocks
and unmaps the index buffer. The compatibility bridge reused the unmapped staging
slot for unrelated vertex buffers. A later mark draw copied vertex records into
the dynamic index buffer, producing indices outside the declared vertex range.
The resulting triangles crossed the sky and terrain, even with a stationary
camera. Firing at the training range reproduces the fault after a fresh reload.

A hardware watchpoint caught the bad copy in `R_SetIndexData` at guest return
address `0x12cedf`. Watching its source caught the bullet-mark vertex generator
writing coordinates at `0x1e6fc2`. The source allocation belonged to the bridge's
reusable mapping pool. One captured draw had 402 indices, an advertised range of
0–6143, and actual indices near 65000. Its shader was
`lm_hsm_sun_dfog_tc0n0_sm3.hlsl`.

MW2 element buffers now own their low-address staging allocation until buffer
deletion. Unmapping keeps the bytes available to the pending CPU draw list.
Vertex buffers retain the reusable pool to avoid duplicating every static level
buffer in guest memory. Ownership follows the buffer's GL share group. The
change is scoped to MW2 profiles; MW1 retains its existing allocation policy.

No effects are suppressed. A test that called `glFinish` before every draw still
reproduced the fault, so full GPU synchronization was removed. The existing
index-upload optimization remains enabled.

## Startup alert lifetime

The replay also exposed premature destruction of the guest's
`SDLMessageBoxPresenter` when the hardware-change alert appeared. Guest-defined
Cocoa objects now join the guest autorelease scope, rather than the short-lived
native pool around one imported call. The lifetime self-test checks that the
object survives dispatch and disappears when the guest pool drains.

## Validation

The mapped-buffer self-test now unmaps an element buffer, maps and writes a
vertex buffer, and checks that the retained element pointer still contains its
indices. It also checks pointer stability on remapping the element buffer.

Renderer, mapped-buffer, profile, COD4 runtime and Objective-C proxy checks pass.
Initializer checks complete 45/45 for MW1 campaign, 49/49 for MW1 multiplayer, and
28/28 for MW2. The Objective-C lifetime check passes.

Isolated range replays use the user's copied save and graphics configuration,
including 1920x1200 and 2x antialiasing. The first corrected replay retained 45
frames after firing and during a second burst. Those captures show no large
triangles, diagonal lines or bright fans. The second replay captured 198 frames with the final build after two bursts,
including the stationary view between them. Sampled frames and the darkest sky
frames were visually inspected without finding the reported artifacts.

## Evidence

Files under `build/mw2-performance/` include:

- `Flicker-1790463347545265000/frame.ppm.000003077.png`, failing baseline.
- `bad-index-evidence/`, captured indices, vertices, shaders and draw state.
- `index-writer.log`, `source-writer.log`, and `source-alloc.log`, watchpoint evidence.
- `Fixed-1790465385108500000/contact-all.png`, first corrected replay.
- `Verified-1790465495106132000/`, final replay, capture scores and retained images.
- `fix-regressions.log`, `final-buffer-objc.log`, `final-lifetime.log`,
  `final-mw1mp.log`, and `final-mw2-init.log`, validation results.

The loader retains opt-in frame capture and shader dumping through
`LP32_AGL_CAPTURE_FRAME`, `LP32_AGL_CAPTURE_EVERY`,
`LP32_AGL_CAPTURE_SEQUENCE`, `LP32_AGL_CAPTURE_GATE`, and `LP32_DUMP_GLSL`.
Temporary pixel probes, buffer-write logging, and forced GPU synchronization
have been removed from the source.

The final executable was installed into `build/MW2-Compat.app` and the bundle
was signed and verified. Its Mach-O UUID matches the tested build,
`890C2AD4-CAA2-3243-91AF-B2CABA7FD561`. The installed MW1 campaign and multiplayer
executable hashes are unchanged. All isolated test game processes were stopped.

## Cave-room brightness, October 9, 2026

The reported cave-room effect switches the whole crate between brighter and
dimmer lighting. The user confirmed that it does not consist of black patches
or sharp edges moving across the crate. Muted private runs loaded a copy of
`autosave/af_caves-5.svg`, with one GPU workload at a time and a 30 FPS cap.
The original profile's file hashes remained unchanged.

Apple GL reproduced stationary brightness changes at this work light. Twelve
captures, separated by 60 frames, recorded mean crate-face RGB values from
67.3 to 113.9 and nearby floor values from 103.3 to 146.5. Their correlation
was 0.952. Five stationary GLMetal captures from another view of the same crate
also changed together with the floor, with correlation 0.906. GLMetal startup
was confirmed by its backend message and its loaded `libGLMetal.dylib` path.

These observations show that this brightness pulsing is not unique to GLMetal.
They suggest changing illumination from the work light. The views and game
times differ, so this is not a pixel-for-pixel provider comparison, and it does
not establish that the game intentionally animates this light or explain all
reported lighting defects. No lighting effect was suppressed.

The investigation independently reproduced and fixed a generic direct index
buffer lifetime bug in GLMetal. Its focused tests match Apple GL on arm64 and
x86_64. That fix is not established as the cause of the cave-room pulsing.
The current local compatibility apps and converter bundles include the rebuilt
driver. Published release archives retain their original contents.

Evidence is under `build/mw2-flicker-20261009/`: `lighting-observations.json`,
`apple-crate/4200.png` and `apple-crate/4860.png`, the retained stationary
`metal-crate-manual/` captures, and the buffer regression reports.
