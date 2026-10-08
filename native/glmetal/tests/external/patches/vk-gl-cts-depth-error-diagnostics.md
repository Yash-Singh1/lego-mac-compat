# Packed depth/stencil error diagnostics

The patch adds actual and expected error values and source line numbers to
`ValidateErrors` failures. It preserves each assertion, API call, and verdict.
It makes repeated mismatches attributable without changing test coverage.
