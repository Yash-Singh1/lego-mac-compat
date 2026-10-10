# GLMetal validation, October 9, 2026

The converter defaults to GLMetal again with the tested shader, command-stream,
and upload fixes. GLMetal contains generic renderer changes. Sound Manager
queue ownership and pause handling remain in the compatibility loader.

## Rendering

The complete offscreen suite ran sequentially on Apple GL and GLMetal with
100 ms between cases. GLMetal passed all 1,033 case assertions. Images matched
exactly in 1,027 cases. Four legacy wide-line cases differ by one green-channel
value at three pixels each. The older driver reproduces those same differences.
Two remaining cases test invalid floating-point texture swizzles: Apple accepts
the invalid settings and fails the assertions; GLMetal rejects them and passes.
These differences remain visible in the results.

The focused 150-case suite also passed and matched on arm64 and x86_64. Final
x86_64 checks covered 14 client-vertex cases, four pooled-upload cases, and four
ordered texture-query cases, with 22 exact matches. CPU shadow-border compiler
fixtures and all 20 generated offline Metal stages passed.

## Gameplay

An M5 Pro ran COD4's `airlift` scene at 1920 by 1200 with a private profile,
muted audio, and uncapped frames. Each observation covered 60 seconds after
gameplay began, using frame counters logged every 60 frames. There was one run
per condition, so the averages are approximate and do not establish results for
other missions or games.

| Provider | Average FPS | Recorded native shader waits |
| --- | ---: | ---: |
| Apple GL | 57.8 | Not instrumented |
| GLMetal, comment-salted sources | 77.7 | 615 ms during startup |
| GLMetal, warm cache and pooled uploads | 74.8 | 0 ms |

The latest run had no crash. A cutscene transition still took 335 ms, primarily
in guest event and movie handling. The comparable Apple transition took 424 ms.
This does not establish that all crashes or stutters are fixed. The previous
multi-second draw-time integer-input and alpha-test shader variants are removed.

Those observations used the background test path. Comment-only salting does
not establish cold downstream Metal pipelines, so the 77.7 FPS run must not
be treated as evidence that normal play is smooth with a cold cache.

## COD4 release and compiler scheduling investigation

The last published COD4 converter, `cod4-converter-v0.0.0-dev.3`, used Apple
OpenGL. Comparison with that tag found no removed COD4 profile patches, focus
policy or AGL presentation optimization. The pacing and append-ring changes
associated with MW2/MW3 were not active COD4 optimizations in that release.

The private COD4 `killhouse` reproduction used copied user settings, muted
audio, a real fullscreen window and one game process at a time. Fresh native
function entry names replaced comment-only salting. Native function compilation
used the same source and limits in both conditions; only scheduling priority
changed. Focus changed during these observations, so their FPS averages do
not establish a foreground performance ranking.

| Compiler priority | First gameplay frame | Pipeline builds in that frame | Native shader waits in that frame |
| --- | ---: | ---: | ---: |
| Utility | 11,304 ms | 2,723 ms | 7,690 ms |
| Utility, repeated | 11,054 ms | 2,835 ms | 7,408 ms |
| UserInitiated | 3,661 ms | 2,768 ms | 0 ms |
| UserInitiated, repeated | 3,926 ms | 2,996 ms | 0 ms |

Across recorded slow frames, native waits were 11,427 and 9,576 ms with Utility
priority, and 33 and 162 ms with UserInitiated. This supports raising the two
bounded prewarm queues and shader-to-pipeline notifications to UserInitiated.
The native shader queue still permits four jobs; the pipeline queue permits two.
It does not fix the remaining 2.7 to 3.0 seconds of first-use pipeline creation
or the separate cutscene and focus-handling stalls.

A bounded ready-pair retry experiment did not help this COD4 scene. ARB uploads
contained separate stages, so it produced no pipeline predictions. That
experiment is excluded from the final driver.

The final driver passed 147 fresh-identity rendering cases on each of arm64
and x86_64, with 294 exact Apple image matches. These include ARB lifetime and
alpha changes, integer vertex fetch, clip planes, depth clamp, sample shading
and shadow border comparison. Pipeline prediction CPU tests passed on both
architectures after removing the retry experiment.

## Audio converter lifetime

One Utility baseline run ended in native `AudioConverterNew`, where Caulk's
allocator trapped on zeroed free-block metadata. The earlier driver baseline
contains the same trap. The writer of that metadata is not established.

The converter bridge had a separate concrete race: it returned an unprotected
native converter while another thread could dispose it. Fill and Reset now
serialize per converter. Dispose retires the handle before waiting for active
work and rejects duplicate disposal. Callback self-disposal waits until the
outer Fill returns; recursive Fill or Reset rejects unsafe reentry. Independent
converters remain concurrent.

Deterministic PCM-only tests passed for blocked conversion callbacks, queued
Reset, concurrent disposal, independent converters and callback self-disposal,
alongside 16,384 variable-size resampling fills. They produce no device audio.
The lifetime fix alone does not prove the Caulk crash is eliminated.

With the final loader and normal shader caching, a 60-second `killhouse`
observation averaged 60.6 FPS with GLMetal and 54.8 FPS with Apple GL. Neither
run crashed. GLMetal's first gameplay frame was 825 ms, including 77 ms of
pipeline creation and no recorded native shader wait. The private profile,
resolution and loader were the same. Focus changed during the observations;
one run per provider does not establish overall superiority or stable frame
times. Audio playback stayed muted, so these observations do not establish
audible playback quality.

An offscreen CGL parameter probe also checked a suspected vsync mismatch.
Both Apple GL and GLMetal returned a default swap interval of 1 for legacy and
core contexts, with single and double buffering. Apple returned the same values
under arm64 and x86_64. Although the [archived Apple guide](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_contexts/opengl_contexts.html)
describes a default of 0, the current platform's measured default is 1. No
default-interval change was made.

## Audio and packaging

Muted native PCM tests cover decoding, ordered callbacks, gapless queueing,
completed-buffer reuse, teardown, and running playback followed by flush, pause,
and enqueue. Returned buffers remain owned by AudioQueue and are recycled.
Delayed completion jobs validate queue epochs before accessing buffer metadata.
Reset preserves the native queue's running state so a later pause reaches it.

All six installed compatibility apps passed signature checks after refresh.
The three converter apps were rebuilt. Packaging tests and relocated SP/MP
conversion checks passed for COD4, MW2, and MW3. Tests used copied profiles and
did not change the user's saves or audio settings.

Detailed local results are in `build/regression-20261009/`, including
`validation-summary.json`, full image comparisons, private gameplay logs, and
packaging results. These runtime artifacts are excluded from Git.

## First indexed draw after a flush

A direct element-buffer draw recorded its GPU read before `prepare_draw` opened
the new Metal command buffer. The read could inherit the previous submission
serial and look completed. An overlapping `glBufferSubData` or mapped write
then overwrote indices still needed by the queued draw. Read tracking now runs
after command-buffer creation, immediately before the indexed draw. Copied and
rewritten index streams retain their existing ownership.

The two new `core_buffer_index_read_after_flush` variants failed before the fix,
each losing a red triangle at 676 pixels. Both match Apple GL exactly after the
fix on arm64 and x86_64. The new ordinary and invariant multipass depth cases
also match on both architectures. A focused x86_64 run covered 26 buffer, depth,
client-index, index-type and primitive-restart cases with exact image matches.
These checks establish the buffer fix, not the cause of MW2 cave lighting.

Local evidence is under `build/mw2-flicker-20261009/`, including the before/after
index reports, `regression-*` reports and `multipass-final-arm64/`.
