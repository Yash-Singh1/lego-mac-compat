# Wide-line terminal coverage probe

This standalone file is excluded from the normal glcompare case library. Compile
it with the same optimized C flags as that library and run
`legacy_wide_line_terminal_predicate.*` sequentially on each provider. Create the
output directory's parents before starting the runner. The probe sleeps 50 ms
between its 128 comparisons per width variant.

It tests widths 2, 3, 4 and 6, x/y-major directions, positive/negative minor
slopes, forward/reverse vertex order, and endpoint fractions 0, .4, .5 and .6.
Each comparison reads the actual wide mask and the union of native width-one
replicas. A CPU predicate intersects the line with the open pixel diamond in
x+y/x-y coordinates. Supplement candidates are restricted to the terminal
major-axis column and the half-open center interval `[low, high)`.

The original closed-high interval falsely predicted exactly-half high endpoints.
The v2 Apple run has zero missing-mask prediction errors and zero overlap with
existing samples in all 512 comparisons. Missing samples total 80, 120, 160 and
240 for widths 2, 3, 4 and 6. CPU diamond disagreements with native narrow masks
occur away from the terminal column, so they do not establish a general native
rasterization model. `terminalDiamond` logs disagreements at the endpoint.

## Proposed production scope

Keep the existing fixed-function, aliased, non-MSAA wide-line gate in vertex.c.
Keep native narrow replicas for all current interior samples. Add only terminal
samples that pass the center-interval and diamond-exit exclusion predicates.
Do not reorder or move original object-space endpoints.

The current programmable user-clipping capture/pull path is not directly
available for generated fixed-function shaders. A bounded implementation needs:

- ffgen.c to factor the existing VS transform into a shared routine and emit an
  internal capture entry plus an explicit storage record for its post-VS values.
- A compute helper to consume the two captured endpoints of each replica,
  determine the terminal candidate, exclude native coverage, and interpolate
  post-VS color, fog, texcoords, clip distances and depth.
- A pull entry with the same fragment interface to render at most one one-pixel
  supplement per replica. Keep the original FF fragment and sampler bindings.
- Backend scratch bounded by the existing streamed batch limit. Internal capture
  and supplement draws must not change original transform-feedback/query counts.

Before production, resolve exact slope ties, zero-length segments, eye-plane
crossings, viewport/frustum/user clipping, stipple phase, and smooth/flat endpoint
interpolation. Confirm the exclusion predicate after actual VS projection and
native screen quantization. No blending or stencil sample may be written twice.
The current constant-color tests establish coverage only; they do not yet prove
post-VS interpolation at supplementary samples.

## Edge and interpolation results

`wide_line_edge_interpolation.c` adds twelve focused cases, with separate float
readbacks for primary color, explicit linear fog and a texture ramp. Width-one
and width-three runs are paced by 50 ms between each of six passes. Logs are
`/tmp/glmetal-line-edge-{apple,test}.log` in the recorded run.

The best primary-color model is:

1. Round projected endpoint window xy to a 1/256 grid.
2. Choose the dominant window axis, including x-major at an exact tie.
3. Compute `t = (sampleMajor - snappedStartMajor) / snappedMajorDelta`.
4. Perspective-correct using `t1 = (t/w1) / ((1-t)/w0 + t/w1)`.
5. Interpolate the original post-VS attributes with `t1`.

For Apple horizontal, vertical and exact45-degree cases, this model's mean red
error is approximately 3e-8 to 7e-8. Unsnappped major-axis interpolation has mean
error about 2.4e-5. Near45-degree cases fit snapped major-axis interpolation to
about 4e-8 to 9e-8, while orthogonal projection has error about 7.5e-4 to 8.6e-4.
Perspective W=1 to4 fits snapped major-axis interpolation to about 2e-8, versus
about 5e-5 without snapping. Metal native interiors fit the same model, with
some extra ULP differences. This supports preserving native interiors and using
the snapped-major-axis model for supplements. It does not prove bit-exact
hardware arithmetic for every attribute or arbitrary clipping.

Both providers generate no samples for the zero-length case. Apple width3 has
three extra terminal samples in horizontal/vertical, exact45, near45 and
reverse-perspective cases where Metal omits the terminal column. Forward
perspective and viewport/user-plane clipping cases have the same logged
coverage. Near-plane clipping again leaves three terminal samples absent on
Metal. Clipping needs the true post-clip endpoint and attributes before snapping;
using the original unbounded segment alone is insufficient.

Remaining implementation requirements include captured post-VS values, matching
clip endpoint construction, flat provoking values, even-width copy placement,
stipple/discard, depth values and query ownership. The tested interpolation
model is major-axis based, not Euclidean projection onto the segment. Initial
production should conservatively skip unsupported clipping and stipple cases
until their capture/coverage semantics are verified.

## Smaller CPU-only initial path

A CPU supplement can avoid new post-VS capture for a deliberately narrow FF
subset: unlit, no fog, all texturing disabled, no user clipping, positive finite
W, both endpoints inside the homogeneous frustum, and nonzero dominant-axis
length. Reject polygon-line conversion, stipple, smoothing, point sprites and
unsupported query/feedback accounting. Texture matrices alone are linear, but a
point has constant varying derivatives, so even explicit textured lines need a
broader derivative-preserving solution.

Clamp endpoint primary and secondary colors before interpolation, matching the
FF vertex `saturate` operations. Flat values have already been copied from the
provoking vertex by submit. Interpolate object position and these colors using
the snapped-major-axis perspective parameter. Recompute the interpolated CPU
MVP position and put the correction to the desired pixel center in fog.yz, the
existing internal wide-line clip-offset channel. The FF vertex already adds
this correction after its MVP transform. Point size can be internally fixed to
one for the wide-expansion shader variant; native lines ignore it.

Flush replicas through each supplemented line, render that line's points, then
continue later primitives. Appending all points after the entire batch changes
ordering at line intersections. A new internal query-count suppression flag is
needed unless the initial gate skips active primitive-generated queries and
feedback. Occlusion query accounting must include the added visible samples.
CPU/GPU MVP rounding, interpolated depth and sample placement still need a
standalone point-supplement test before production use.

## Initial implementation validation

The initial production subset now additionally requires identical finite primary
and secondary colors at both endpoints and disables depth testing. It rejects
instancing and programmable fragment/pipeline stages. A supplement copies the
original terminal endpoint, so no color interpolation or depth-model assumption
is needed. Only strictly open-diamond-contained endpoints qualify; exact boundary
cases and quantized major-axis changes retain the old path.

`run_wide_terminal_cpu.sh` extracts the actual helper from vertex.c into a fresh
temporary CPU program. The 512-combination assertions pass with 80/120/160/240
supplements, strict terminal diamond containment, half-open intervals and no
replica duplicates. `legacy_wide_terminal.*` adds eight registered GPU cases for
widths2/3/4/6, blending, stencil duplicate detection, primitive ordering and
point-size restoration. GPU validation is centralized by the parent agent.

The first CPU-only implementation further requires exact identity modelview and
projection matrices and original endpoint W=1. This removes differing CPU/GPU
MVP contraction and reciprocal evaluation from the proof. Each shifted endpoint
also has a binary32 viewport uncertainty interval derived from gamma(12), with
unit roundoff2^-24 and a magnitude that includes the replica offset. The count
covers offset division, clip addition and contracted or separate viewport
arithmetic. If either interval endpoint can round into a different1/256 cell,
that candidate stays on the original path. Integer-range guards precede pixel
coordinate conversion. CPU tests include extreme finite viewport coordinates,
half-step boundaries and width16 offsets on a1-pixel viewport.

## Axis reconstruction prototype

`wide_axis_interval.h` is a standalone proposal, not production code.
`run_wide_axis_interval_cpu.sh` builds it in a fresh temporary directory and
checks249,856 directed endpoint intervals, adjacent half-center boundaries,
zero/one-pixel intervals, integer limits and viewport fallbacks. For each valid
interval it checks all nondegenerate diamond half-spans1/256 through1/2. Native
zero-span minor-boundary ties require GPU evidence.

The helper starts at the first desired major center in traversal order and ends
0.75pixel beyond the last desired center. Reconstructed positions crossing a
viewport edge fall back. Set `GLM_WIDE_AXIS_RECONSTRUCT=1` for the separate
`wide_line_axis_boundaries.c` GPU diagnostic, which intentionally explores the
coverage model without adopting it in the driver.

## Reversed native interpolation diagnostic

Set `GLM_LINE_EDGE_REVERSE_WIDTH3=1` when running the standalone
`wide_line_edge_interpolation.c` case library to reverse only width-three
submission. Width-one runs remain unchanged. Each endpoint keeps its original
primary color, fog coordinate, texture coordinate and position. The log includes
`WideEdgeMode width3_reverse=1`; ordinary `WideEdge` sample records retain their
format for comparison with the original logs. The existing 50 ms pauses remain.

This first probe reverses the whole width-three draw. It does not yet apply a
scissor or draw an additional supplement. Compare the original terminal samples
against the reverse run to determine whether native interpolation supplies the
missing values. Do not use the reversed draw's coverage elsewhere as replacement
coverage for the original line.

If reversal supplies correct terminal values, a production supplement must
intersect a one-pixel scissor with the application's active scissor and viewport.
Render it after that line's original replicas and before subsequent primitives.
Restore scissor enable/box state and invalidate the backend's cached state.
The strict terminal predicate must prove the original replica does not touch
that sample, otherwise blending, stencil and occlusion counts would be repeated.
Keep the existing user-clip, perspective, depth, lighting, texture, stipple,
multisample and primitive-query exclusions until separately validated.

Flat shading needs colors already copied from the original provoking vertex;
reversing an unexpanded flat primitive changes its provoking vertex. Smooth
attributes must remain attached to their original endpoints. Native reversed
setup can round interpolation differently, so coverage alone is insufficient.
The float primary-color, fog and texture readbacks are the evidence for that
comparison. No CPU interpolation formula is substituted by this diagnostic.

Example after the parent builds the standalone library:

```sh
GLM_LINE_EDGE_REVERSE_WIDTH3=1 GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider apple --cases /tmp/libwide-line-edge-interpolation.dylib --out build/line-edge-reversed-apple
GLM_LINE_EDGE_REVERSE_WIDTH3=1 GLCOMPARE_CASE_DELAY_MS=50 build/glcompare --provider "$PWD/build/libGLMetal.dylib" --cases /tmp/libwide-line-edge-interpolation.dylib --out build/line-edge-reversed-glmetal
```

Axis reconstruction now quantizes original major endpoints before center bounds.
The production helper is included in the CPU runner's proof in addition to the
standalone copy. Reconstructed float positions must map stably to the intended
window cell, and every shifted minor endpoint must pass the viewport bound.

For varying finite colors, the added terminal path retains native interpolation:
a reversed replica is scissored to one proven missing pixel. The original
terminal endpoint must be strictly inside that diamond and the opposite
endpoint strictly outside, so reversal must exit it. App scissor intersection is
checked first; public scissor state and the state serial are restored after each
internal draw. Per-line flushing preserves ordering. The reversed replica uses
exactly the original Float32 minor-offset arithmetic. Constant colors retain the
point fast path and are required for axis reconstruction.

`legacy_wide_axis.*` adds ten strict axis regressions, including near-half values,
zero/one-pixel intervals and stencil coverage. `legacy_wide_reversal.*` adds eight
strict tests for varying colors, blending, stencil, application scissors,
restoration, primitive ordering and flat colors. GPU verification remains with
the parent and is required before claiming conformance success.
