# Clipped polygon anchor diagnostic

Build `clip_anchor_probe.c` as a standalone library and select
`core_clip_anchor_probe.*`. Five variants transform the entire original
triangle and both clip planes together: identity, quarter turns of 90,
180, and 270 degrees, and X reflection. Input vertex order and colour
attributes remain unchanged. The orthogonal transforms use exact signed
coordinate permutations rather than trigonometric approximations.

Each variant captures native post-VS values once and renders a native
reference. Four CPU-preclipped candidates anchor the polygon fan at maximum
Y, minimum Y, maximum X, and minimum X. Coordinates are evaluated after
conversion to uploaded binary32 positions and division by W. Primary
coordinate ties select the maximum other coordinate; logs report both
the chosen polygon index and the number of tied extrema. This deterministic
tie policy is a hypothesis, not an established native rule.

The default clipping arithmetic is binary64 split. Set
`GLM_CLIP_ANCHOR_ARITHMETIC` to 1 for binary32 split, 2 for fused binary32
interpolation, or 3 for reciprocal-t and split interpolation. This selects
one arithmetic mode per process to keep the draw count bounded. Captured
distances determine intersections, and the original fragment shader is
unchanged.

Logs include the native-relative difference score and first eight different
pixels for each anchor, and polygon output hexadecimal floats. There are
six draws per variant, 30 across all five variants. Every draw pauses for
50 ms. The final image is the native original, and diagnostic byte differences
do not assert an expected native anchor. Invalid values or GL errors still
fail. This probe remains outside the normal regression library.

The earlier input-order probes suggested a physical top-vertex fan origin
independent of original input order. Whole-geometry transforms distinguish
that screen-space hypothesis from choosing a particular source vertex.
Reflection also tests whether winding affects the rule. Do not change
production clipping until these controls establish an anchor and tie policy.

## Observed limits

In `/tmp/glmetal-clip-anchor-probe-apple.log`, minY candidates are exact for
all five transforms. MaxY is exact except for the rotated-90 geometry, where
its tied-extremum choice differs at four one-byte pixels. This does not
establish a universal minY rule. Earlier single-plane probes require the
opposite diagonal for the same triangle, and input cyclic rotations change
the correct numeric anchor index without changing the native image.

The GLMetal native baseline differs from Apple at two identity pixels,
four rotated-180 pixels, and four X-reflection pixels. Rotated-90 and
rotated-270 native baselines are already exact. Score comparisons are against
each provider's own native reference, so a candidate that matches Apple can
still report differences against GLMetal's current native clipping output.

One alternative is native sequential clipping and retriangulation after each
plane. The current production paths clip a whole polygon against all planes
and triangulate only at the end. Sequential triangle clipping can introduce
vertices at intersections between a later plane and an earlier fan diagonal.
That changes raster setup even when all continuous attributes are affine.

A CPU calculation from Apple's captured original values produces a four-vertex
polygon after plane zero and a four-vertex final polygon. Whole-polygon clipping
uses two final triangles. Retriangulating after plane zero then clipping each
triangle yields three final triangles and an additional vertex on plane one.
The earlier diagonal chooses that vertex near `(0.400000009, -0.006315853)`;
the alternative diagonal chooses it near `(0.400000003, -0.534951448)`.
These are materially different meshes, not just cyclic fan rotations.

A bounded next diagnostic can use identity and rotated-90 geometry, two
first-plane diagonals, two second-plane fan choices, and two plane orders.
Sixteen candidate draws plus two native captures and two final redraws keep
it to twenty draws. Preserve captured post-VS values and the original FS;
log all intermediate vertices and compare full images. Even exact candidate
images would support an algorithm only for the tested family. Production
remains unchanged until single-plane and multiple-plane behavior are explained
together.
