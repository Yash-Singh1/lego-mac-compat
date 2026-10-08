# GLMetal

GLMetal is an OpenGL implementation written directly on Metal. It replaces
Apple's OpenGL.framework for games, with Apple's implementation as the
reference it is tested against. It exports the same symbols as
OpenGL.framework (1,275 `gl*` and 67 `CGL*` functions, listed in
`data/`), plus AGL and the NSOpenGL classes.

## Goals

1. Render what Apple's OpenGL renders, checked pixel by pixel.
2. Expose exactly Apple's profiles: legacy 2.1 (fixed function, ARB
   programs, GLSL 1.20) and core 3.2 to 4.1, with Apple's strings, limits,
   extensions and renderer info.
3. Work in any process: 32-bit games through the loader's GL dispatch, and
   64-bit (x86_64 or arm64) games through injection.
4. Be faster than Apple's GL-on-Metal layer by keeping per-draw CPU work low.

## Layout

| Path | Contents |
|---|---|
| `src/api/` | Entry points, errors, `glGet*`, the Apple profile |
| `src/state/` | Context state: objects, bindings, fixed-function state |
| `src/metal/` | Metal backend: resources, pipeline and sampler caches, command encoding, presentation |
| `src/shader/` | GLSL (glslang to SPIR-V to SPIRV-Cross MSL), ARB programs and fixed function to MSL |
| `src/platform/` | CGL, AGL, NSOpenGL, surfaces on CAMetalLayer |
| `tests/glcompare/` | Pixel comparison suite: every case runs on Apple's GL and on GLMetal |
| `tests/external/` | Fetch and build scripts for piglit and apitrace |
| `tools/` | Injection helpers, trace capture and replay comparison |

## Delivery

- **libGLMetal.dylib** is universal (x86_64, arm64). It exports the
  OpenGL.framework symbol set and `glmetal_get_proc_address`.
- **Embedding:** hosts load the library and call `glmetal_initialize`,
  `glmetal_get_proc_address`, and `glmetal_install_appkit_overrides`.
- **64-bit games:** `tools/glmetal-run <app>` injects it with
  `DYLD_INSERT_LIBRARIES`. The library interposes the CGL, AGL and `gl*`
  exports, and replaces NSOpenGLContext and NSOpenGLPixelFormat, because
  AppKit calls CGL from inside the shared cache. Apps with the hardened
  runtime drop `DYLD_*` variables; the helper reports that case.

## Shaders

GLSL is parsed by glslang, compiled to SPIR-V, and translated to MSL by
SPIRV-Cross. Both are built from pinned sources as universal static libraries
(`tools/build_deps.sh`), because Homebrew's copies are arm64 only. ARB vertex
and fragment programs and the fixed-function pipeline are generated as MSL
directly. Apple's GLSL quirks (implicit conversions it accepts, CR line
endings, `#version`-less shaders) are handled in a preprocessing step, each
with a glcompare case.

Vertex-only transform-feedback programs can retain binary64 values as raw
64-bit integers in Metal. This path supports exact uniform storage, feedback,
comparisons, rounding, bit packing, and common scalar/vector math. Integer
helpers implement addition, subtraction, multiplication, division, square
root, and fused multiply-add with binary64 rounding. All nine matrix shapes
support component multiplication and outer products; 2x2 matrices also support
determinant and inverse. The compiler checks SPIR-V operations before choosing
this path. Programs that need unsupported double conversions, matrix operations
or layouts, uniform initializers, or raster interfaces
still use the existing float lowering. The normal and feedback shader
variants must choose the same representation because they share uniforms.

Ordinary GLSL and fixed-function multiply-add expressions can contract to a
single rounding, matching Apple's shader compiler. This does not enable
general fast math. SPIRV-Cross keeps GLSL `precise` operations in separate
helpers that prevent contraction.

Float and shadow cube samplers bind a parallel 2D-array view of the selected
faces and mip range. Apple GPUs that expose the private Metal sampler control
`forceSeamsOnCubemapFiltering` use native filtering for nonseamless clamp-edge
cubes. The driver checks the device family, selectors, and a descriptor
roundtrip before using it. Other devices and wrap modes retain the array
emulation. Cube LOD calculations also distinguish Apple's clamp and repeat
paths. Sampler arrays and sampler arguments carry their descriptor slot so
the shader reads the right texture or sampler object's LOD state. Integer
cube samplers retain the existing native path.

The final raster vertex function masks disabled clip-distance outputs.
Capture passes retain the original values for transform feedback. Generated
variants are cached by shader source, entry point, and enabled-plane mask.

Aliased wide lines in the legacy fixed-function path replicate one-pixel
lines along the minor window axis. Temporary vertex batches are bounded.
Antialiased lines, multisample line rasterization, and programmable vertex
stages still use the existing line path.

User-plane clipping for fixed-function triangles with linear vertex
calculations runs before submission, using double-precision intersections
and bounded vertex batches. This reproduces Apple's interpolation after
clipping. Lighting, nonlinear texture generation, and fog retain native
clipping in the fixed-function path.

Eligible programmable triangles capture vertex-shader outputs, clip them in
a compute pass, and rasterize the resulting triangles. Linked SPIR-V supplies
the varying types and interpolation rules. Batches contain at most 512 input
triangles and use private GPU buffers without CPU readback. Transform feedback
keeps the original vertex outputs, and primitive queries count the input
triangles once. Unsupported interfaces and triangles crossing the eye plane
retain native clipping.

Compact vertex streams carry the original vertex-ID base separately from
their buffer offsets. User shaders add this base to the hardware ID; internal
capture indexing remains relative to the compact stream. This corrects affine
rebasing for nonzero draw starts and indexed draws. Transform-feedback and
geometry expansion still need separate mappings for nonaffine vertex IDs.

Linear RGBA8 and BGRA8 multisample resolves average samples in index order
using float32 shader arithmetic. Metal's hardware resolve rounds some exact
half-byte averages differently from Apple. Other formats retain hardware
resolve where available.

## Apple's frameworks

Apple's OpenGL.framework is the reference renderer for the test suite and a
source of data: strings, limits, extensions, pixel format and renderer
answers are captured from it (`tools/apple_dump.c`). Where
matching Apple exactly is easier by calling into it (for example, to
reproduce a GLSL compile log), GLMetal may load it at run time; it never
copies Apple binaries.

Context destruction submits pending work and drains its completion callbacks
before freeing the backend. It also clears surviving shared buffers' references
to that backend. Default textures belong to their context; named textures and
cube aliases are released when their last shared context closes.

## Testing

- **glcompare** runs each case in two processes, one per implementation,
  renders into an offscreen framebuffer, and compares the results pixel by
  pixel. Cases are plain C that calls GL directly; the runner binds those
  calls to whichever implementation it loaded. Each case sets a tolerance
  (default: exact) and the report shows both images and a diff.
- **piglit** runs through waffle's CGL platform against both implementations;
  the report lists tests whose results differ.
- **apitrace** captures a real game's GL stream on Apple's GL; the replay
  runs on both implementations and compares frames.

Run GPU suites sequentially on the development machine. The CTS runner and
glcompare already execute their cases in sequence; do not start several
suite processes at once. CPU-only shader translation and reference checks
can run independently.
Set `GLCOMPARE_CASE_DELAY_MS=50` when running glcompare on the shared machine
to leave idle time between cases after each context has finished and been
destroyed. The pause is outside the reported case timing.
For full CTS checks, use `GLCTS_CHUNK_SIZE=200 GLCTS_CHUNK_DELAY_MS=250`
to retire each test process after at most 200 cases and pause before the next.
`tests/probes/run_shader_clip_cpu.sh` checks clipping translation, vertex-ID
rebasing, and shader-cache roundtrips without executing GPU work. It compiles
an explicit list of generated Metal stages in a fresh temporary directory.

## Optional host resource configuration

The interfaces in `src/client.h` allow a host to provide a page-aligned shared
buffer allocator before initialization. Without it, GLMetal uses Metal allocation.
The callbacks must remain valid for the library's lifetime, including pooled
storage retained after context destruction. Callbacks must support concurrent
calls, including deallocation on Metal completion threads. Allocation
failure returns no storage instead of silently bypassing the host constraint.

A host may explicitly call `glmetal_prewarm_command_queues` after initialization
and before context creation to prepare reusable command queues. GLMetal performs
no command queue warmup by default. The host chooses queue and submission counts.
