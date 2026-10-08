# Cube sampling precision before byte conversion

`cube_precision.c` is a standalone diagnostic for the remaining one-byte
anisotropic cube differences. It uses the original six-level 32 px color/depth
cube patterns, a sampler object with anisotropy up to 8, clamp-edge wrapping,
and the original fullscreen vertex shader. Four variants cover color/shadow
sampling and seamless filtering enabled/disabled.

Five modes retain normalized interpolated directions, use raw directions,
use fixed raw uniform directions, use fixed normalized uniform directions,
or use fixed normalized directions with explicit uniform gradients. Fixed
inputs are computed once on the CPU from pixel centers. The explicit gradients
use normalized neighboring positions in a 2x2 pixel quad. They are diagnostic
inputs, not assumptions about the provider's derivative implementation. The
explicit-gradient mode scales the second channel gradients by the original
bias factor and retains the original third channel gradient scales.

Every draw writes five attachments:

- RGBA32F sampled result.
- RGBA32F direction, with comparison reference in alpha.
- RGBA32F x derivative, with interpolated u in alpha.
- RGBA32F y derivative, with interpolated v in alpha.
- RGBA8 containing the same sampled result.

Logs include hexadecimal float bits and values at pixels 37,43;41,20;41,43.
The first two modes draw once and read all three positions. Each uniform mode
draws once per selected position. This is 11 tiny draws per variant,44 total.
Readbacks complete before a 50 ms pause after each draw. The five attachments
occupy 272 KiB, excluding the modest cube mip chain. No output values or vendor
ordering assumptions are asserted. GL errors and framebuffer completeness
remain strict.

Interpret results in this order: compare directions, reference values and
derivatives; compare sampled float values; then compare RGBA8 conversion.
Uniform inputs remove shader arithmetic differences between providers.
Normalized/raw comparisons help expose sensitivity to cube projection and LOD.
Explicit gradients do not reproduce implicit anisotropic sampling by definition,
so a difference between those modes alone is not a driver defect.

Build separately, then run providers sequentially from the repository root:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/cube_precision.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libcube-precision.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libcube-precision.dylib --out build/cube-precision-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libcube-precision.dylib --out build/cube-precision-glmetal
```

Preparation ran C syntax checks only. The parent controls builds and GPU runs.
