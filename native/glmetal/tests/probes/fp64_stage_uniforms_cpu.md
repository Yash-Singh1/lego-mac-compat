# FP64 uniforms in generated stages

Run `GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_stage_uniforms_cpu.sh` for CPU compilation and offline Metal validation. The script does not load the driver or submit GPU work.

Nine fixtures use scalar, dvec4 and dmat2x3 uniform arrays in geometry, tessellation control and tessellation evaluation stages. They check the original program and generated stage reflection for exact double types and std140 array strides. All 45 generated Metal stages compile offline. A mixed-stage fixture adds four stages and verifies that an unsupported float-to-double conversion in the fragment shader selects the same float fallback in the geometry kernel.

Exact arithmetic and double uniform storage stay local to a stage. Interstage payloads remain 32-bit. Double inputs or outputs select whole-program fallback, except for the existing vertex-only transform feedback path. Generated capture, compute, evaluation and pull stages inherit the parent's choice. Shared uniform metadata checks reject a child that changes a user block's representation. A failed exact geometry or tessellation lowering recompiles the complete program through the existing fallback.

The GPU regressions in `cases_fp64_stage_uniforms.c` compare all four array elements against values above 2^53 and require a green rendered sample. Padding is poisoned, and the tests assert the original GL type, array stride, matrix stride, offset and block size. These checks distinguish actual binary64 execution from float conversion or metadata-only changes.

The existing fallback remains approximate. This work does not add exact double interstage payloads, row-major matrices, or unsupported conversion operations.
