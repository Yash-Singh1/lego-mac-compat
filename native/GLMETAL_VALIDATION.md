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
| GLMetal, cold native shader cache | 77.7 | 615 ms during startup |
| GLMetal, warm cache and pooled uploads | 74.8 | 0 ms |

The latest run had no crash. A cutscene transition still took 335 ms, primarily
in guest event and movie handling. The comparable Apple transition took 424 ms.
This does not establish that all crashes or stutters are fixed. The previous
multi-second draw-time integer-input and alpha-test shader variants are removed.

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
