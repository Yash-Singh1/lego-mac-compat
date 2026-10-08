# Multiple-plane clipping precision

Build `clip_planes_precision.c` as a standalone case library and select
`core_clip_planes_precision`. This probe narrows the investigation to the
programmable fixture from `core_shader_clip.multiple_planes`. It does not
change production shaders or normal test tolerances.

One native draw captures the original post-VS position, nonlinear shade,
texture coordinate, and both clip distances through transform feedback.
It also renders the native clipped image. These exact captured binary32
outputs seed every CPU candidate, so host recomputation of the vertex shader
cannot explain candidate differences.

Candidates vary two plane orders, four intersection arithmetic modes, and
three cyclic polygon rotations. The arithmetic modes are binary64 split,
binary32 split, binary32 fused interpolation, and binary32 reciprocal times
numerator for the intersection parameter with split interpolation. Distances
for subsequent planes are interpolated from the captured outputs, matching
post-VS clipping rather than recomputing a plane from host coordinates.
Rotations zero and two retain a quadrilateral diagonal but change its fan
origin; rotation one switches the diagonal.

The unchanged original fragment shader renders every candidate through a
passthrough VS with clip distances disabled. Logs report differing pixel
counts, maximum byte difference, total byte difference, the first eight
different pixels, and polygon output bits as hexadecimal floats. Candidate
differences are diagnostic observations, not assertion failures. Invalid
capture values, polygon overflow, and GL errors still fail the probe.

Each variant has 24 candidate draws, one original capture draw, and one final
native redraw. Nine variants cover cyclic input order, swapped plane indices,
and one-plane controls. Each draw pauses for 50 ms. The final image is the original native
rendering. The standalone image can retain provider differences, so this
file remains outside the normal case library.

The earlier full-suite artifacts showed two one-byte core differences at
GL pixels 39,37 and 33,34, both alpha. The fixed-function multiple-plane
fixture had three differing pixels, including the same 39,37 alpha sample.
This probe separates fan/raster setup from plane-order and intersection
arithmetic before either production clipping path changes.

The first run found identical post-VS capture values across providers. All four
intersection arithmetic modes produced the same image scores for each fan
choice. Changing the quadrilateral diagonal, rather than arithmetic precision,
removed the two original alpha differences. Input-order and one-plane controls
show that no fixed polygon index explains all native choices. See
`build/clip-planes-order`, `build/clip-planes-single`, and the separate
`clip_anchor_probe.md` investigation. Production clipping remains unchanged.
