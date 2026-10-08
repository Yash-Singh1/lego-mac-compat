# Binary32 vector normalization compiler regression

SPIRV-Cross emits `fast::normalize` explicitly for float vectors. That namespace
bypasses the backend's strict Metal math option. The cube precision diagnostic
shows direction differences of one or two ULPs before texture sampling; raw and
uniform-input modes are identical across providers.

The fragment-stage GLM compiler override evaluates its operand once through a float-vector
helper, computes explicit scalar squares and left-to-right sums with a local
`#pragma clang fp contract(off)`, then applies `rsqrt` and multiplies the vector.
The native Metal dot intrinsic accumulated differently at the recorded inputs;
removing only `fast::normalize` did not change that accumulation. Vertex and other nonfragment stages retain native `fast::normalize`. The native
vertex diagnostic differs from the fragment diagnostic at the same inputs.
Scalar, half and software binary64 normalization retain
their existing lowering. This follows the ordinary reciprocal-length form rather
than using an approximate fast vector intrinsic.

The CPU fixture compiles vec2, vec3 and vec4 normalization directly, through a helper,
in conditional branches and with a side-effecting function operand. It checks
normal and feedback-capture vertex MSL retain native lowering, while fragment
MSL calls the ordered helper. It checks that the operand function is not called
twice, and checks cache serialization. A scalar fixture retains `sign` lowering.
The script compiles all 40 generated Metal stages offline, without GPU execution
or a driver build. Three additional LLVM checks require no native dot, FMA
or contraction flags in the extracted vec2/3/4 helper kernels:

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_normalize_cpu.sh
```

The standalone `normalize_reference_cpu.c` Float32 host model using rounded dot, rounded reciprocal square root and
rounded component multiplication matches all three Apple captured directions:

| Pixel | Direction component bits |
| --- | --- |
|37,43|3efdc060,3d9f2be0,3f5d74b2|
|41,20|3f33b3fd,bd82856c,3f35983e|
|41,43|3f33b3fd,3d82856c,3f35983e|

This is evidence for the lowering, not a claim that host libm emulates Metal
for every input. The parent must validate cube images and broad shader cases on
both providers. GPU zero, overflow and subnormal normalization behavior should
also be checked before claiming full exceptional-input equivalence.

The bounded stage diagnostic measured 14 of 162 vertex components and 30 of
162 uniform-input fragment components with different bits across providers.
All strict input, capture-order and count checks passed. The ordered helper
produced exact matches for all eight original cube images, whose normalization
inputs come from interpolated varyings. These observations do not establish a
universal native fragment arithmetic model. No input-origin heuristic is used.
