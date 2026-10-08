# Valid binary64 matrix inverse diagnostic

`inverse_singular_proof.py` proves the first CTS inverse_dmat3 matrix determinant
is exactly zero using rational arithmetic. Its output is retained in
`build/inverse-singular-proof.json`. Each row differs from the preceding row by
a constant, so the three rows are linearly dependent.

CTS gl4cGPUShaderFP64Tests.cpp Math::inverse near line12563 divides by that zero
determinant and requires alternating infinities. Input generation near line15014
uses an arithmetic sequence without an inverse singularity exclusion. Both
providers fail at this same input. GLSL4.10 §8.6 specifies undefined inverse
results for singular or poorly conditioned matrices. No CTS expectations change.
The diagnostic logger also reads feedback_data after unmapping near line15504;
its printed result must not be treated as reliable captured storage evidence.

Primary source: [GLSL4.10 specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.10.pdf).

The standalone probe tests3x3 and4x4 diagonal power-of-two matrices, a permutation
matrix, and a unit triangular matrix with one half-valued off-diagonal entry.
Their inverses have exact binary64 representations. It checks original input
capture, output type, exact expected inverse and A*inverse residual, and logs
all values before reporting failures. There are eight point draws, a256-byte
feedback allocation, and50ms pauses. It is deliberately outside the ordinary
image suite because current float fallback is expected to fail these checks.

The dense 3x3 case is A=L*L^T with unit-lower L[r,c]=(r-c)/8 below the diagonal.
The dense 4x4 case is a Hadamard matrix whose inverse is its transpose divided
by four. `inverse_dense_proof.py` independently verifies their exact rational
inverses and writes `build/inverse-dense-proof.json`. The norm products are
545015/262144 and4 respectively. Every dense cofactor is nonzero.

The residual bound is gamma(2n)*8, where gamma(k)=k*epsilon/(1-k*epsilon).
The factors account for n products and additions, with norm product at most4 across all cases. The dense rational proof verifies
this bound independently;8 remains conservative.
These particular exact operations should actually give zero residual; the
strict exact-output check remains in addition to the rounding bound.

```sh
xcrun clang -mmacosx-version-min=11.0 -Itests/glcompare -Ibuild/gen -dynamiclib tests/probes/fp64_inverse.c tests/glcompare/glc_util.c -Wl,-undefined,dynamic_lookup -o /tmp/libfp64-inverse.dylib
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libfp64-inverse.dylib --out build/fp64-inverse-apple
GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libfp64-inverse.dylib --out build/fp64-inverse-glmetal
```

Only CPU proof and syntax checks ran during preparation. Parent owns GPU runs.

The native GPU run passed both variants and all six matrices. The initial GLMetal
run failed both variants, reflected float matrices and produced incorrect output.
The same valid cases now live in `tests/glcompare/cases_fp64_inverse.c` for
permanent strict coverage after exact inverse support.

CPU compiler checks:

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_inverse_cpu.sh
```

Six direct, helper-function and side-effect-operand fixtures check binary64
feedback types and strides, cache serialization, and twelve offline Metal stages.
A host harness extracts the emitted integer arithmetic and cofactor helper bodies
from generated Metal and executes the same eight known-exact inverses under UBSan.
The 3x3/4x4 implementation uses software binary64 multiply, add, subtract and
divide throughout; nonsquare inverses and larger determinant operations retain
the existing eligibility rules.

Final GPU validation passed both dimensions and all eight matrices exactly on
Apple and GLMetal, with no GL error or runner crash. Report directory is
`build/fp64-inverse-final`.

The cofactor algorithm has a remaining extreme-scale limitation. A uniformly
scaled, well-conditioned matrix can overflow or underflow intermediate minors
or its determinant even when its inverse is finite. Diagonal matrices scaled by
1e200 or1e-200 illustrate this gap. Robust exponent scaling is future work;
these eight successful matrices do not establish arbitrary-exponent coverage.
