# Core varying limits

GL4.1 core section 2.11.10 and table 6.50 require MAX_VARYING_COMPONENTS and MAX_VARYING_VECTORS. The latter equals the former divided by four. The minimum values are 60 and 15. The component token aliases the older MAX_VARYING_FLOATS token.

Official specification: https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf

GLMetal's desktop compiler uses glslang DefaultResources, with maxVaryingComponents=60 and maxVertexOutputComponents=64. Its core queries now report 60 and 15. Reporting 124 components would exceed that configured vertex-output capacity. Compatibility values remain 124 and 31. Transform-feedback record strides are reflected dynamically, so a 60-word record does not require a larger fixed capture allocation.

Apple silicon's core implementation rejects the component query while returning 31 vectors. The probe therefore stays outside the ordinary Apple-oracle image suite. It checks both valid queries, links an active 15-vec4 VS/FS interface, and verifies all 60 transform-feedback words.

Build the standalone case library without rebuilding the driver:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/varying_limits.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libvarying-limits.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libvarying-limits.dylib --out build/varying-limits
```

The probe is bounded to one point draw. Run it only when the centralized GPU lane is free.

CPU compiler regression:

```sh
sh tests/probes/run_varying_limits_cpu.sh
```

This parses compile-time assertions using the actual driver compiler, compiles ordinary and capture variants, and verifies a uniform initializer using gl_MaxVaryingVectors. Desktop core constants report 15 and 60. Legacy, ES and explicit compatibility sources retain their previous resource table.
