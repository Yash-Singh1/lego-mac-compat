# Constructor shader dialect on desktop core contexts

The inherited `KHR-GL41.shaders30.glsl_constructors` group requests GLSL 1.30.
The fresh Apple GL4.1 baseline and the old GLMetal baseline both fail the same
324 positive cases at shader compilation. The 324 negative cases pass because
compilation fails, so those results do not prove the constructor checks ran.
For example, `bvec2_from_bool_mat2_fs` in Apple's `chunk-0063.qpa` uses
`#version 130` and `bvec2(false, mat2(1.0, 2.0, 3.0, 4.0))`.

The patch changes only a requested GLSL 1.30 dialect to GLSL 1.40 when the
actual context is desktop core 3.2 or newer. ES, compatibility contexts, older
contexts and other requested dialects retain their original templates. Both
shader stages receive the same dialect. Constructor expressions, component
order, expected images and positive/negative compilation checks are unchanged.

[GLSL 1.30 section 5.4.2](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.1.30.pdf)
and [GLSL 4.10 section 5.4.2](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.10.pdf)
permit vector construction from matrix components and mixed scalar/matrix
arguments. Matrix components are consumed in column-major order. Precision
qualifiers and default precision statements are legal desktop GLSL syntax under
GLSL 1.30 section 4.5, so removing those declarations would not repair a
constructor error.

[OpenGL 4.1 core section 1.7.1](https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf)
guarantees GLSL 4.10 support. GLSL 4.10 section 3.3 specifies an error for an
unsupported shader version and permits linking GLSL 1.40 stages with GLSL 4.10
stages. This patch addresses the observed native GLSL 1.30 rejection. It does
not claim that every core context must reject GLSL 1.30, or that the tested
constructors are invalid in GLSL 1.30.

The patch is retained in `glcts.sh` and passes forward/reverse applicability
checks. The changed translation unit passes a syntax-only compilation using
its existing Ninja compiler flags. GPU validation must rerun all 648 cases on
both providers with the repaired CTS binary. No pass verdict is substituted.
