# Pixel pack image diagnostics

The normal comparison library registers only the four `core_pack_image_state.get3d_*` cases from `tests/glcompare/cases_pack_image_state.c`. They check the 3D `GetTexImage` image stride and skip offset, including untouched padding and guard bytes.

The same source retains the four 2D `ReadPixels` cases behind `GLM_PACK_READPIXELS_DIAGNOSTIC`. Build a separate library to run them explicitly. From the repository root:

```sh
xcrun clang -mmacosx-version-min=11.0 -DGLM_PACK_READPIXELS_DIAGNOSTIC \
  -Itests/glcompare -Ibuild/gen -dynamiclib \
  tests/glcompare/cases_pack_image_state.c \
  -Wl,-undefined,dynamic_lookup -o /tmp/libpack-readpixels-diagnostic.dylib
```

Run only with an exclusive GPU test lane, one provider at a time:

```sh
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple \
  --cases /tmp/libpack-readpixels-diagnostic.dylib --out /tmp/pack-readpixels-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider build/libGLMetal.dylib \
  --cases /tmp/libpack-readpixels-diagnostic.dylib --out /tmp/pack-readpixels-glmetal
```

The assertions remain strict. All cases log write offsets, values and raw bits before reporting a failure. Each uses a 512-byte padded payload with 32-byte guards on both ends.

The retained baseline is `build/pack-image-state-baseline`. Apple writes the two ReadPixels skip1 variants at the image skip offset, while GLMetal correctly uses the 2D offset. Apple passes all four 3D GetTexImage cases; the old GLMetal binary fails three. The standalone ReadPixels diagnostics preserve that reference discrepancy without making Apple a correctness oracle for it in the normal image suite.

The official [OpenGL4.1 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf), section6.1, printed page323, applies the additional PACK_IMAGE_HEIGHT and PACK_SKIP_IMAGES state to three-dimensional, two-dimensional array, and cube map array GetTexImage queries. The normal ReadPixels placement is described in section4.3.1, printed page285.
