# Exact column-major FP64 matrix arrays

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_matrix_arrays_cpu.sh
```

The probe compiles all nine matrix shapes with mixed std140 members and arrays. It checks four operations per shape: whole-array copy with dynamic indexing, explicit column constructors, diagonal scalar constructors with local array assignment, and dynamic integer-to-double element constructors. Each checks original Double reflection, column stride, array stride, member offset and transform-feedback type/stride. Cache roundtrips retain metadata and generated sources. All 72 normal/capture Metal stages compile offline.

The lowering reuses column-major structs with ulong vector columns. Arrays use SPIRV-Cross's existing spvUnsafeArray value wrapper, or the ordinary array suffix when native arrays are required. This preserves whole-array copies and address-space indexing without changing IEEE bit representation. Three-row columns retain 32-byte storage stride.

A row-major UBO fixture verifies that unsupported row-major layout still selects the existing whole-program fallback. This slice does not enable FP64 stage inputs, raster double outputs, GS, tessellation or compute execution.

Nine registered core_fp64_matrix_arrays variants upload three matrices per shape with poisoned padding and values above 16777216. They verify all std140 query results, copy a whole matrix array, index dynamically, reconstruct from columns and compare against dynamically converted integer constructors. Original transform feedback must capture exact 64-bit matrix values and the comparison verdict. Workload is three points per variant; GPU execution is owned by the parent.
