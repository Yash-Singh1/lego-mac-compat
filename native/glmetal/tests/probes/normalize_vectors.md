# Native vec2, vec3 and vec4 normalization bits

This standalone vertex and fragment diagnostic submits eighteen deterministic
vectors for each dimension. Inputs include the cube precision probe's directions
and neighbors, signed components, unequal magnitudes, small finite values,
and permutations that expose addition ordering. All active-dimensional inputs
are nonzero and have finite squared length.

The program captures the original vec4 input, normalized active components and
original vertex ID. It strictly checks input preservation, capture order and
primitive count. Output bits are logged beside a CPU scalar-product/ordered-sum
model. The model is diagnostic, not a substituted expected result. The vertex results and fragment results may differ even for identical inputs.
Three fragment variants render the same inputs into an eighteen-pixel RGBA32F
attachment and log their bits without asserting a CPU arithmetic model.

There are six tiny draws with an output buffer of 648 bytes. Each variant reads
completed feedback or pixels and waits 50 ms before returning. This is not registered in the
normal image comparison suite.

Build and run sequentially from the repository root:

```sh
xcrun clang -mmacosx-version-min=11.0 -ffp-contract=off -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/normalize_vectors.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libnormalize-vectors.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libnormalize-vectors.dylib --out build/normalize-vectors-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libnormalize-vectors.dylib --out build/normalize-vectors-glmetal
```

Preparation ran syntax checks only. The parent controls GPU execution.

The fragment-only helper run recorded 14 of 162 differing vertex components
and 30 of 162 differing fragment components. It had zero strict assertion
failures. Logs are `/tmp/glmetal-normalize-stages-apple.log` and
`/tmp/glmetal-normalize-stages-test.log`. Uniform-input fragment arithmetic
differs from the interpolated cube diagnostic, where all eight image cases
match exactly with the ordered helper. The probe exposes numeric differences
without declaring a universal native implementation or changing tolerances.
