# Triangle interpolation rotation diagnostic

The original quad has W=1 at every vertex and no flat shader inputs. Fan
conversion in vertex.c glm_triangulate emits B,C,A for the first triangle and
C,D,A for the second. That ordering is required for geometry-stage semantics,
but it can change the numerical origin of the rasterizer's attribute plane.
The helper is not evidence that ordinary rasterization must use that same order.

This standalone probe compares the original fan with explicit triangle lists
A,B,C/A,C,D and both cyclic rotations, for smooth and noperspective qualifiers.
It preserves the original position and color bits, sets clip Z=0 and W=1,
disables depth/clamp/blend, and reads RGBA32F. Smooth and noperspective must
represent the same mathematical plane here, but no exact cross-provider bit
assertion is imposed. It logs29 samples including all20 pixels that differed
in the original unblended quad and the separate blend case's differing pixels.
This isolates raster attribute arithmetic from depth clipping and UNORM/blend
conversion. It does not change image-suite tolerances or expected outputs.

There are eight tiny draws, one64x64 RGBA32F attachment per variant, and50ms
pauses after readback. The fixture restores the runner framebuffer and viewport.

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/triangle_interpolation.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libtriangle-interpolation.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libtriangle-interpolation.dylib --out build/triangle-interpolation-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libtriangle-interpolation.dylib --out build/triangle-interpolation-glmetal
```

Only syntax checks ran during preparation. Parent owns GPU execution.

The initial eight variants matched both providers at all116 logged components
per variant and across full blitted RGBA8 images. Thus cyclic rotation and the
smooth/noperspective distinction did not explain this W=1,z=0 residual.

Twelve added variants compare RGBA32F and RGBA8 color attachments, each with six
states: original vertex clip Z with native frustum clipping; original clip Z
with depth clamp and depth test disabled; clamp with implicit depth and LESS;
clamp with explicit clamped gl_FragCoord depth; clamp with explicit clamped
noperspective rawDepth; and z=0 with ordinary depth testing. A32F depth attachment
is present, cleared to1. No blending is enabled. These reproduce the original
vertex arithmetic and distinguish target quantization from depth-clamp/setup
and programmable depth wrappers. All variants retain full color images by
blitting to the runner target after numeric readback.

The expanded diagnostic has20 total draws,64x64 targets, and50ms pauses. GPU
execution remains owned by the parent; only CPU syntax checks ran for extension.

Expanded GPU results show all z=0 variants exact. The original varying Z differs
at all116 sampled Float32 components and20 image pixels, independent of RGBA32F
versus RGBA8 color storage. Within each provider, original Z with clipping,
clamp, depth testing and either explicit depth equation has the same sampled
colors. This rules out color-target quantization and depth writes for those
samples. Changed coverage is a separate effect.

Six additional RGBA32F variants preserve all original XY/color data and disable
depth testing. They enable depth clamp and use these clip-Z expressions:

- Fully inside varying Z,0.75-1.5*red.
- Constant inside Z,0.75.
- Constant outside Z,1.5.
- Reversed original Z,-1.5+3*red.
- Near-plane-only Z,-1.5+1.5*red.
- Far-plane-only Z,1.5*red.

They distinguish nonzero Z from actual near/far-plane crossings. The original20
variants are unchanged. Total workload is26 draws, with the same bounded targets,
29 samples and50ms pauses. No production changes are involved.
