# Patch varying precision diagnostic

Current core_tessellation_modes.patch_varyings has nine differing pixels, all
maximum one byte, in build/conformance-fragment-direct. Alpha matches at every
pixel. Patch0 alpha is0.625 and patch1 alpha0.85, so matching alpha supports
correct primitive IDs, levels and patch association. The RGB equations are
mathematically affine within each patch. They combine a patch average with
barycentrically interpolated control colors, followed by raster interpolation
of each generated triangle. Small domain-coordinate, TES contraction or raster
rounding differences can therefore produce the observed RGB-only residual.

This standalone five-variant diagnostic retains the original positions, colors,
patch indices, patch outputs and TES formulas in an RGBA32F attachment. It logs
all nine originally differing coordinates plus a center point. Variants compare:

- Original asymmetric levels and color equation.
- All levels1 to remove internal tessellation-domain vertices.
- Constant patch-average RGB to isolate patch data transfer.
- Shade, outer level and primitive ID encoded directly as RGB.
- Mixing each control color first, then weighting the mixed colors by domain
  coordinates. This is mathematically equivalent to the original mix but moves
  the rounding points.

The mode uniforms add shader branches, so the original mode is a diagnostic
analogue rather than a byte-identical compiler fixture. No output tolerance or
original image expectation changes. The fixture retains all TCS invocations'
identical patch writes as in the original test; it does not infer a race fix
from this small residual.

There are five draws of two patches each, a64x64 RGBA32F color attachment, and
50ms pauses after readback. Float images are blitted to the runner target.

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/tess_patch_precision.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libtess-patch-precision.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libtess-patch-precision.dylib --out build/tess-patch-precision-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libtess-patch-precision.dylib --out build/tess-patch-precision-glmetal
```

Only CPU syntax checks ran during preparation. Parent owns GPU execution.
