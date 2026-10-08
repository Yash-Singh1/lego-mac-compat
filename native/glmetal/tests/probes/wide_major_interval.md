# Wide-line major interval diagnostic

This standalone probe reproduces the original loop and viewport fixtures as
individual line segments. Optional `GLM_WIDE_MAJOR_SCISSOR=1` intersects each
segment with the sample centers in its snapped half-open major interval.
Positions and colors are unchanged. Each provider runs sequentially with the
normal 50 ms case delay.

The native split-segment images match the original loop and viewport images
exactly. Adding the interval scissor leaves those native images unchanged.
Before the production interval fix, GLMetal differs at two loop pixels and nine
viewport pixels. With the probe's interval scissor, both images are pixel-exact.
The loop's closing segment previously overwrote the preceding segment at two
samples. The viewport case included samples before its lower major endpoint.

Results are retained in `build/wide-major-interval`, with four provider/mode
subdirectories. The production change additionally checks viewport rounding
stability, retains its existing restricted state conditions, intersects the
application scissor with bounded arithmetic, and restores the prior state.
