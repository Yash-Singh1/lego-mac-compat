# Binary32 division CPU probes

Run `tests/probes/run_fp32_division_cpu.sh` from any directory. It requires the
repository's installed glslang/SPIRV-Cross libraries, clang++, Python3 and the
Xcode Metal compiler. It does not build the GL driver or execute GPU work.

The runner creates a fresh temporary directory and removes it on exit. Set
`GLM_PROBE_KEEP=1` to retain sources and compiled artifacts for inspection.

`fp32_reciprocal_cpu.cpp` tests the integer reciprocal-bit algorithm against
host binary32 division. It enumerates every significand for three exponent
boundaries with both signs and adds two million deterministic random bit
patterns. It checks 52,331,648 inputs, with 35,523,378 accepted normal reciprocal
comparisons. Zero, subnormal, NaN, infinity and nonnormal reciprocal cases are
rejected by the constant algorithm.

`fp32_division_compiler.cpp` reuses the actual compiler/cache setup in
`shader_clip_cpu.cpp`. Its four fixtures cover scalar/vector constant division,
precise constant multiplication, dynamic scalar division, and precise dynamic
vector division. It verifies generated operations, roundtrips cached stage
sources and generates eight stages. The runner checks an exact eight-entry
manifest and compiles every stage offline as Metal2.3. These eight stages are
separate from the clipping probe's nineteen stages.

The production dynamic helpers retain direct division outside the normal
finite reciprocal interval. CPU/offline checks establish compilation and bit
construction; provider readback regressions in `cases_float_runtime_division.c`
establish runtime behavior.
