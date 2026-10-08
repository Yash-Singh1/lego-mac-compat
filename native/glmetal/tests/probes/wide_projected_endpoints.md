# Raw projected endpoint diagnostic

Build `wide_projected_endpoints.c` as a standalone library and select
`legacy_wide_projected_endpoints.*`. Two variants reproduce the original
perspective and user-plane-clipped wide-line fixtures. It does not alter
production sources or normal regression expectations.

A GLSL120 vertex shader evaluates the built-in modelview and MVP transforms
and the float eye-space clip-plane distance. A point draw places these raw
results in a one-pixel RGBA32F framebuffer. Fragment output merely copies the
chosen varying. All three float4 records are read and logged with exact bits
for each original endpoint. This measures a shader replica of FF transform
expressions, not direct capture of Apple's fixed-function vertex pipeline.
The distinction matters if Apple's FF pipeline uses different arithmetic.

After raw reads, the probe disables the program and draws the original
fixed-function lines. It logs projected endpoint coordinates, a candidate
snapped half-open interval, and each occupied minor row's first/last major
sample. Each individual raw draw and each line draw pauses for 50 ms.
There are 24 raw draws and four native line draws per variant.

Set `GLM_WIDE_ENDPOINT_TRIM=1` to intersect each native line with the candidate
interval. The candidate uses captured distances and CPU-double intersection
arithmetic. It is an experiment, not a proved production error bound. Native
positions and interpolation are unchanged. No analytic image assertions
claim that captured shader transforms reproduce Apple's FF arithmetic.

The default image contains the last original line. Read every per-row coverage
log when comparing providers. These diagnostics are intentionally standalone
because default provider images can differ. Raw capture and native coverage
should establish whether a trim-only interval classification is robust before
production eligibility expands to arbitrary transforms or clip planes.
