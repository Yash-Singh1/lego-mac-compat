# Completed transform-feedback stream counts

`feedback_stream_draw.c` checks generated and captured counts for two geometry
streams, then binds another feedback object and replays the completed streams.
The expected counts are 2 and 3, with 5 points replayed in total.

This strict probe runs separately from the Apple image comparison suite. On the
current Apple driver, capture reports 2 and 3 correctly but stream replay reports
only 2 total. Keep the assertions intact when comparing providers.

Build and run sequentially from the repository root:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/feedback_stream_draw.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libfeedback-stream-draw.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libfeedback-stream-draw.dylib --out build/feedback-stream-draw-fixed
```
