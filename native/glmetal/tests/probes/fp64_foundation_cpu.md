# Exact FP64 array operands and integer conversion

Run the CPU/compiler and offline Metal checks without a driver build or GPU submission:

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_foundation_cpu.sh
```

The probe compiles twelve real VS-only transform-feedback programs. Four use std140 arrays of double, dvec2, dvec3 and dvec4 alongside float block members; eight convert signed or unsigned 32-bit scalar/vector arrays to doubles. Every case checks exact reflection, feedback stride and cache roundtrip, and compiles normal and capture MSL offline. Float-to-double conversion remains on the existing fallback and is checked separately.

The emitted scalar integer-conversion helper body is also compiled as host C++ under UndefinedBehaviorSanitizer. Its output is compared byte-for-byte against native binary64 conversion on 231202 inputs spanning zero, signed extrema, unsigned extrema, powers of two and their neighbors, every positive and negative 16-bit magnitude, and deterministic random values. Conversion constructs exponent and mantissa bits directly, with no intermediate float and no signed negation of INT_MIN.

Registered GPU regressions are in cases_fp64_uniform_arrays.c. They retain original GL_DOUBLE/dvec reflection and byte-exact captured values. The UBO cases poison padding, check std140 offsets/stride, load all four array elements dynamically and exercise abs plus equality on padded scalar operands. Conversion cases include 16777217, INT_MIN/MAX and UINT_MAX, which distinguish exact conversion from conversion through float. The GPU cases are intentionally bounded to four array points or eight conversion points; the parent runs them sequentially.

No raster stage eligibility or matrix-array support is changed by this slice. The older fp64_uniform_scope_cpu diagnostic now accepts integer conversion but still rejects matrix arrays; its ordinary VS/FS check now verifies exact GL_DOUBLE_VEC4 and a 32-byte stride after the separately owned stage expansion.
