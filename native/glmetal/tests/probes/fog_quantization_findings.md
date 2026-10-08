# Fog interpolation quantization findings

This note analyzes the existing fog geometry, clipping, snap and snap-color
logs under `/tmp/glmetal-fog-*-{apple,test}.log`. No GPU run was repeated.
Machine-readable sample comparisons are in `build/fog-quantization-findings.json`.

The unsnapped quad differs at all60 logged samples between providers. Maximum
absolute differences are2.832e-6 for interpolated green and7.725e-5 for eye-space
fog distance. The current triangle diagonal and both cyclic rotations have the
same error. Changing the diagonal changes the error but does not remove it.
Thus this evidence does not support a vertex-order fix.

After explicit256-step round snapping, interpolated green and fog distance
match exactly across all60 samples. The derived exp factor still differs by up
to2.999999998e-8 at17 samples. The snap-color variant differs by up to6e-8 in
red and3e-8 in green/blue, so saying every Float32 result matches would be too
strong. These remaining differences occur after the interpolation inputs agree.

The native unsnapped input results are closest to256-step round snapping among
all tested grids. Their largest fog-distance difference is4.101e-5, compared
with about2.58e-4 for128-step round/floor,256-step floor and512-step round/floor.
GLMetal unsnapped differs from the same256-round input by1.0944e-4. Native is
closer to that quantized model, but is not identical to it.

A plausible mechanism is that the native rasterizer builds perspective attribute
planes from quantized post-divide screen positions, while Metal's triangle setup
or clipping uses different position precision. Green and fog distance share the
same geometric change, so this is broader than an exp approximation. Explicit
screen snapping makes the inputs exactly representable in both setups and
removes the large discrepancy.

There is a substantial confound. The original perspective quad crosses the
frustum's side planes. CPU preclipping, including X-only preclipping, also makes
interpolated green and fog distance identical across providers. Depth-only
preclipping leaves the original discrepancy. Quantizing XY before hardware
clipping changes the generated intersection positions as well as the final
raster-plane positions. Current evidence cannot distinguish clip-intersection
rounding from raster screen snapping or establish their order.

This does not justify a general production snap rule. The probe uses a64x64
viewport, no multisampling and one perspective projection. It does not establish
viewport-origin handling, tie rounding, negative coordinates, reversed viewports,
W<=0 triangles, sample locations, centroid/sample qualifiers, coverage invariance
or the native grid for other devices. Its floor(x*256+0.5) rule rounds ties toward
positive infinity, which has not been demonstrated as the hardware tie rule.
Snapping emitted clip XY could also change clipping, winding and coverage while
altering every varying. Native GL permits implementation-specific subpixel
precision; this diagnostic does not turn its observed grid into a normative rule.

The smallest useful future distinction would compare already-inside perspective
triangles to explicitly clipped triangles while preserving exact original
attributes, then audit position and fog interpolation separately. Existing runs
should not be repeated just to reconfirm this note. Production remains unchanged.
