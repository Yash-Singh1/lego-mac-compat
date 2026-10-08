# Tessellation domain and output capture

Build `tess_patch_feedback.c` as a standalone case library and select
`driver_tess_patch_feedback`. It uses the original uniformless patch-output
shader from `core_tessellation_modes.patch_varyings`. The original TCS and TES
arithmetic are retained. Two TES assignments append diagnostic outputs for
`gl_TessCoord` and `gl_PrimitiveID`; the existing `gl_Position` and `ec` are
captured directly. There are no uniform-driven diagnostic branches.

One indexed draw emits two triangular patches with the original tessellation
levels. Transform feedback records three domain coordinates, four position
components, four colour components, and an integer patch ID. Records are
48 bytes. A bound 6 KiB range permits 128 records, followed by a 64-byte
sentinel guard outside the capture range. Generated and written primitive
queries must be nonzero and fit the buffer before any record readback.
Count mismatches remain failures, but bounded records are logged first.
Begin, draw, and End now each check GL errors immediately and fail strictly.

Logs include original emission order and records sorted by patch ID and
numeric domain coordinates. The coordinate, position, and colour fields use
raw hexadecimal binary32 bits. Compare coordinate multisets first because
providers can emit tessellated triangles in different orders. If domains
match, compare position and colour at the same patch/domain tuple. Duplicate
vertices can appear in several emitted triangles and should remain in the
multiset. No analytic expectation fixes tessellator emission order.

The native draw also renders the original fragment colour. It pauses for
50 ms before readback. The probe fails on invalid counts, overwritten guard,
invalid patch IDs, nonfinite values, or GL errors. Provider image differences
remain diagnostic evidence. This file is excluded from the normal library.

The first Apple run captured 27 generated/written triangles and 81 records,
but logged a software evaluation-program fallback. It therefore does not
establish the native hardware tessellation domain. GLMetal initially reported
zero counts; source inspection shows missing TES capture support. Begin
rejects programs without a VS capture function or GS, the compiler omits a
TES capture variant, and `draw_tessellation` ignores capture state. Generated
query accounting also uses input patch count as triangle vertex count.
Allowing Begin or changing counters alone cannot implement the missing
post-tessellation stream. The original probe checked counts before its final
GL error read, so its zero-error result did not prove Begin succeeded.

The strict rerun confirms GLMetal rejects Begin with `GL_INVALID_OPERATION`
`0x0502`. Apple's Begin, draw, and End each report zero error and its queries
report 27 generated and 27 written primitives. Apple still logs the software
evaluation-program fallback. This establishes a missing TES transform-feedback
API path in GLMetal; it does not establish that Apple's software domain equals
its hardware tessellator's domain. Keep the diagnostic strict. Do not implement
the API by merely accepting Begin or assigning query counts without capturing
the emitted post-tessellation primitive stream.
