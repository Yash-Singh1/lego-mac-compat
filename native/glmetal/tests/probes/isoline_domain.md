# Isoline domain and transform-feedback diagnostic

This standalone probe captures `gl_TessCoord` and `gl_PrimitiveID` from a
one-control-point isoline TES, with rasterization discarded. It is not registered
in the image comparison suite. The current unsupported GLMetal isoline path is
expected to fail compilation until the feature is implemented.

Each factor configuration runs as points and lines. Configurations cover factors
1, 2 and 3, independent line and segment factors, nonintegral equal/even/odd
spacing, fractional minimums, zero/negative/NaN relevant outer levels, maximum
factors and two input patches. If the advertised maximum is 64, two additional
cases test factor 65 clamping. Providers with a larger limit omit those cases so
the workload never exceeds 64 lines or 64 segments per patch.

The probe logs every captured coordinate and every captured segment. Assertions
check generated primitive counts, patch IDs, valid coordinates, line membership,
distinct endpoint counts and connectivity. Equal-spacing deviations from ideal mathematical positions are logged as
numeric errors, without an asserted numeric tolerance. Fractional-spacing
assertions do not prescribe the location of the two shorter segments. Sorting is confined to CPU
validation after feedback capture. No assertion depends on the driver's order
of patches, lines, points, segments or endpoint orientation.

[ARB_tessellation_shader section 2.X.2](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_tessellation_shader.txt)
defines factor clamping and rounding, patch discard, point mode and domain
coordinates. Its isoline section uses outer 0 for line count and outer 1 for
segments. Revision 19 corrected older reversed wording. Inner levels and the
remaining outer levels are irrelevant to isolines. The generator's primitive
order is implementation-dependent. The extension's tessellation invariance rule 8 requires `1.0 - x` to be
exact for each defined TessCoord component. The probe checks the complement
against binary64 subtraction and checks the two-operation roundtrip
`x == 1.0 - (1.0 - x)`. GLSL 4.10 section 7.1 only describes the coordinates;
the normative exact-complement requirement comes from tessellation invariance. The ARB extension does not specify
a numerical subdivision-accuracy bound, so this probe audits topology and
invariance and logs precision separately. It does not alter CTS assertions or
image comparison tolerances.

The feedback buffer is 256 KiB, at most 16384 records. Every case waits for its
feedback query and reads completed feedback before a 50 ms idle interval. Programs
are deleted between cases. There are 34 ordinary draws plus 4 upper-clamp draws
when the maximum is 64. No framebuffer or texture allocations are needed.

Build separately from the repository root, then run providers sequentially:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/isoline_domain.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libisoline-domain.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libisoline-domain.dylib --out build/isoline-domain-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libisoline-domain.dylib --out build/isoline-domain-glmetal
```

Only syntax checks have run during preparation. The parent controls GPU execution.
