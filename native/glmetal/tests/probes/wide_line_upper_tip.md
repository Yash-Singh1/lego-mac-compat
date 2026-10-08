# Upper-tip traversal probe

Build `wide_line_upper_tip.c` as a standalone case library and select
`legacy_wide_line_upper_tip.*`. This is a diagnostic, excluded from normal
regressions. Production still excludes every exact diamond tip.

Five variants test a slope just steeper than 45 degrees, a steep slope, a
near-vertical nonaxis slope, a 1:6 slope, and the known x-major exception.
Each tests both signs of X and Y traversal, three integer translations,
and upper-tip endpoint offsets of -1/256, zero, +1/256. All endpoints remain
inside the 64-pixel viewport, so native frustum clipping does not interfere.

There are 36 original/reversed pairs per variant, 180 pairs overall. Each
pair pauses for 50 ms. The log includes start and endpoint coordinates,
target pixel, both observed coverage bits, and the formal symbolic model's
expected bits. Assertions report failures only after a variant logs every
comparison. The x-major control is expected to expose the already observed
failure of a universal symbolic model, and its failures remain visible.

The near-vertical slope is nonaxis by one exact 1/256 unit. This can reveal
whether the native tip rule changes close to vertical alignment. Integer
translations test whether a narrower rule depends only on local geometry.
No production eligibility change should follow until both providers agree
on the proposed family and its nearby controls.

Both native runs agree with the model on all 144 y-major pairs. The x-major
control retains six mismatched direction samples on each provider. Logs are
`/tmp/glmetal-wide-upper-tip-{apple,test}.log`; case results are under
`build/wide-upper-tip/{apple,test}/results.tsv`.

The production predicate now supports only nonaxis y-major upper tips, with
the opposite endpoint strictly outside the diamond. For upward traversal
ending at integral Y and half-integral X, the candidate is the preceding
half-integral Y center. Other candidate formulas and existing interior and
slanted-edge behavior remain unchanged. The actual-helper CPU check covers
all 144 native-probed pairs, and `legacy_wide_upper_tip.*` adds strict image
and stencil regressions. X-major tips, axes, and exact 45-degree ties remain
excluded.
