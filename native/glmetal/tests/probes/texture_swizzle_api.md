# Texture swizzle API diagnostics

This standalone probe logs every scalar and four-component cube swizzle call
used by CTS `texture_swizzle.api_errors`, including the argument, actual error
and GL4.1 expected error. It also logs initial red swizzle queries for all nine
CTS targets. It preserves observations without assigning a conformance verdict.
There is no texture allocation or GPU draw. The probe waits 50ms before returning.

GL4.1 core §3.8.8, printed page221, lists valid TexParameter targets and requires
INVALID_ENUM for invalid enum-valued parameters. Section6.1.4, printed page320,
lists valid GetTexParameter targets. Both lists exclude multisample targets.
CTS initial_state nevertheless queries both multisample targets, which explains
its shared INVALID_ENUM failure. The first seven targets remain valid coverage.

Current GLMetal converts signed parameter values through Float32, then converts
to unsigned GLenum. A CPU check on Apple Silicon returns zero for Float32 -1
converted to unsigned. Thus the invalid swizzle argument -1 can become GL_ZERO.
The diagnostic determines whether this explains each provider's API failure;
existing CTS error messages do not identify the failing argument.

Primary source: [OpenGL4.1 core specification](https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf).
The older ARB_texture_swizzle extension specifies INVALID_OPERATION instead;
this probe follows the core4.1 INVALID_ENUM rule rather than extension wording.

Build and run sequentially from the repository root:

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/texture_swizzle_api.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libtexture-swizzle-api.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libtexture-swizzle-api.dylib --out build/texture-swizzle-api-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libtexture-swizzle-api.dylib --out build/texture-swizzle-api-glmetal
```

Preparation includes a C syntax check only. The parent controls GPU execution.

The native run rejected integer -1 setters with INVALID_OPERATION, but accepted
float -1 setters. GLMetal initially accepted all eight integer -1 calls; the
range-validation fix rejects them with the core INVALID_ENUM error. Permanent
state-preservation tests retain their strict assertions: native integer variants
pass and native float variants fail. This observation is not normalized away.
