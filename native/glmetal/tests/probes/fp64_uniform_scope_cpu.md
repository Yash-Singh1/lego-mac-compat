# Exact FP64 uniform scope

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_uniform_scope_cpu.sh
```

This CPU diagnostic compiles VS and FS independently with std140 arrays of double, dvec4 and dmat2. Literal and dynamic integer constructors separate layout from conversion support. All twelve fixtures now pass exact eligibility. An ordinary VS/FS program additionally checks GL_DOUBLE_VEC4 reflection and a 32-byte array stride.

The foundational slice fixes logical unpacking of physically padded uniform loads and encodes signed/unsigned 32-bit integer conversions directly as IEEE binary64 bits. The matrix-array slice preserves all nine column-major shapes, mixed UBO offsets, matrix/array strides and array value copies. Ordinary VS/FS exact support checks every present stage and retains whole-program fallback for unsupported operations or interfaces. Capture passes preserve the same storage choice.

Current limits remain deliberate: row-major double matrices, float/double conversion operations, double stage inputs, and raster programs containing any double-valued VS output select fallback. VS-only original transform feedback permits double outputs. Internal FP64 UBOs are now also preserved through GS and tessellation capture/kernel/pull stages with strictly 32-bit stage interfaces and child storage compatibility checks. Public compute execution is outside GL4.1.

The CTS max_uniform_components case tests VS, FS, GS, TCS and TES. Only integer verification results travel between stages, while each tested stage receives its own FP64 UBO. Its constructors use dynamically computed integers and cover double, dvec2/3/4 and all nine dmat shapes. All five stage paths now have exact uniform storage support. Focused GS/TCS/TES fixtures cover double, dvec4 and dmat2x3, while all nine matrix shapes are checked independently. Full CTS validation of arrays near the reported limits remains pending.

Detailed regressions and implementation evidence are in fp64_foundation_cpu.md, fp64_matrix_arrays_cpu.md, fp64_clip_cpu.md and fp64_unused_output_cpu.md. CTS source is gl4cGPUShaderFP64Tests.cpp, GPUShaderFP64Test2 prepareUniformVerification and prepareProgram.
