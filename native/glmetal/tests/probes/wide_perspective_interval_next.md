# Perspective and clipped wide-line interval trimming

The bounded identity/W1 wide-line fixes pass all 15 new boundary, tip, and
interval cases. Original diagonal, strip, loop, and viewport cases are exact.
Perspective and user-plane-clipped cases previously retained six extra pixels
each. The restricted trim-only extension below now makes both pixel-exact.

In the perspective fixture's fourth line, projected start X is approximately
8.727272885. Its half-open sample interval starts at pixel 9, so the extra
column at X8 lies outside it. For the clipped fixture's fourth line, the
user-plane intersection projects to X22.818227736. Its first sample is pixel
23, so the extra column at X22 likewise lies outside the interval.

Keep interval trimming separate from terminal supplements. A prospective
trim-only gate can retain the existing fixed-function, sample-count, query,
depth, fog, lighting, and texture restrictions while allowing finite matrices
and positive W. Do not broaden axis reconstruction, points, or reversed
replicas merely because the interval can be bounded.

FF uniform packing and `wide_line_draw` both construct the uploaded MVP with
`glm_mat4_multiply`. Bound GPU matrix-vector dot evaluation from those exact
uploaded coefficients. A conservative dot envelope includes gamma(7) times
the sum of absolute products, plus absolute subnormal error. Evaluate
perspective division only when the W interval is strictly positive. Apply
outward-rounded offset and viewport arithmetic bounds. Accept a scissor bound
only when snapping and `ceil(position - .5)` are constant over its interval.
The X/Y difference bounds must prove unchanged, nonaxis, non-45 major
classification. Every ambiguous case retains the existing path.

User-plane clipping additionally needs bounded eye-position dots and
`dot(float plane, eye)`, matching uploaded float plane coefficients. Reject
ambiguous distance signs and crossing denominators containing zero. Evaluate
`t = d0 / (d0 - d1)` and homogeneous endpoint interpolation with outward
rounding, intersect enabled planes, and initially reject eye/near-plane
crossings or unsupported clipping. FF computes clip distances before replica
offsets, so each user-plane intersection parameter is shared by replicas.

The remaining precision gap is rasterizer perspective division and viewport
setup. Shader IEEE bounds alone do not establish their hardware error bound.
A focused transformed-endpoint and raw-position diagnostic must validate an
error envelope before CPU certification becomes production behavior.

Relevant code is `src/vertex.c` in `wide_line_draw`, `wide_terminal_viewport`,
and `wide_major_scissor`; `src/ffgen.c` in `glm_ff_uniforms_pack` and generated
`ff_vertex`; and `tests/glcompare/cases_wide_lines.c`. Preserve all current
tolerances and expectations.

## Implemented trim-only slice

The raw-endpoint GPU probe found identical clip, eye and distance bits across
providers for all eight lines. Native had no samples outside the candidate
half-open major intervals; Metal had12 rows of extra samples. Applying the
candidate trim preserved native images exactly and removed the two original
perspective/clipped image residuals. This is measured support for the following
restricted implementation, not a general hardware-division theorem.

The new path requires identity modelview and either identity projection or a
symmetric axis-aligned perspective projection. Perspective XY scales must be
positive powers of two and W=-eyeZ. Cross-axis terms, asymmetric frustum offsets
and eye-plane shifts fall back. Shader XY and W evaluation is exact for normal
finite results under that structure; gamma(7) bounds the remaining Z/distance
dots. Subnormal products, possible intermediate overflow, uncertain clip signs and endpoints outside
any frustum plane fall back. At most one enabled user clip plane is supported.
Its t interval uses bounded distance differences/division and gamma(5) endpoint
interpolation. The source positions and all attributes remain unchanged.

Projected viewport bounds use a gamma(12) envelope around the measured setup.
This is an empirical conservative precision assumption for this restricted
structure, not a formally established native/Metal raster division bound.
Both XY quantization decisions must be constant across their envelopes, and
major-axis classification must exclude zero, axes and45-degree ambiguity.
The existing half-open scissor helper then trims each original replica draw;
it does not reconstruct geometry or synthesize new fragments. Existing scissor
flush/restore behavior is reused. Old terminal, reversal and axis gates remain
unchanged, including identity/W1 restrictions.

CPU checks extract the actual production helper. Eight measured lines are
eligible with expected first pixels; unsupported axis classification, asymmetric
transforms, multiple user planes, near and eye crossings fall back. Extreme
dot-product cancellation and subnormal products also fall back. UBSan passes.
The prior terminal CPU suite still checks33708 major/scissor intersections and
all original terminal/boundary cases successfully.

```sh
sh tests/probes/run_wide_projected_interval_cpu.sh
sh tests/probes/run_wide_terminal_cpu.sh
```

The production build passes all API checks in the 53-case wide-line group.
Its 49 pixel-exact images include both original perspective/clipped fixtures.
Only the four previously known reversal cases remain different, each at three
pixels by one byte. The six new strict projected interval cases are also
pixel-exact and check scissor restoration, following draws, repeated blending,
and stencil writes. Reports are in `build/wide-projected-trim-fixed/report`
and `build/wide-projected-strict/report`.
