# Border reconstruction diagnostic

This standalone probe stays outside the ordinary image suite. It renders one tiny 32x4 draw per variant into six RGBA32F attachments and logs float errors, rather than asserting that an unverified reconstruction formula matches the native sampler.

The attachments contain black-border sample, white-border sample, colored-border sample, existing scalar-weight reconstruction, componentwise mix and mix using sRGB-decoded coefficients. Core variants cover RGBA, RGB, RED, RG, swizzles with constant ONE/ZERO, SRGB_ALPHA and SRGB. Legacy variants cover ALPHA, LUMINANCE, LUMINANCE_ALPHA and INTENSITY. Each format has endpoint and fractional border colors.

Build the standalone diagnostic libraries:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/border_reconstruction.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libborder-core.dylib
xcrun clang -mmacosx-version-min=11.0 -DGLM_BORDER_LEGACY -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/border_reconstruction.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libborder-legacy.dylib
```

Run each provider sequentially through the centralized GPU lane. Substitute the legacy library for the legacy variants:

```sh
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libborder-core.dylib --out build/border-core-reference
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libborder-core.dylib --out build/border-core-test
```

For finite filter outputs, componentwise reconstruction uses each channel's own difference instead of the maximum rounded difference from another channel. It also preserves channels whose black and white endpoints coincide, such as format-provided alpha or constant swizzles. Coefficients must match the desired border after base-format conversion, clamping and swizzling. Fractional sRGB border interpretation requires the diagnostic result before changing production.

GL4.1 core section 3.8.11, printed page 229, converts border RGBA to the texture's internal format and clamps it to the stored component range. Section 3.8.17, printed pages 239–240, specifies sRGB conversion as part of filtering and leaves alpha unchanged. Official source: https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf
