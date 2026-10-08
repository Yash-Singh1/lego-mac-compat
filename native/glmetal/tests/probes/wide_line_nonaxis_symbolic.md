# Nonaxis diamond boundary probe

`wide_line_nonaxis_symbolic.c` is a standalone diagnostic. It is excluded from
the normal regression library. Select `legacy_wide_line_nonaxis_symbolic.*`
when running its case library.

The eight variants place an endpoint at the upper and lower diamond edges and
the four tips of pixel 32,32. Each variant tests eight nonaxis approach octants
and three endpoint positions, on the edge and one 1/256 unit to either side.
It reads original and reversed native width-one coverage. Each pair pauses for
50 ms. There are 192 pairs and 384 draws across the complete probe.

Expected coverage uses exact rational coefficients for the formal endpoint
shift `(-epsilon^2, -epsilon)`. No finite epsilon changes submitted geometry.
The diagnostic logs every result and reports assertion failures after each
variant, so a failed model leaves all 24 comparisons available for analysis.
The `eligible` field reports original coverage zero and reversed coverage one.
The `duplicate` field reports coverage in both directions. Duplicate coverage
can be valid for endpoints outside the diamond and must not be supplemented.

Run the CPU checks with:

```sh
sh tests/probes/run_wide_symbolic_diamond_cpu.sh
```

The CPU check covers all 192 pairs. It proves the model's terminal classification
and reversal relationships, not that native rasterization implements the model.
Existing axis diagnostics have exceptions to this symbolic model, so this probe
deliberately excludes axis-aligned segments. Production coverage remains frozen
until native results establish which nonaxis boundary cases satisfy the rule.

## Native results

The Apple and GLMetal runs agree on all 192 observed pairs. The symbolic model
disagrees with nine exact-tip pairs, involving ten direction samples. The
four slanted-edge variants and every shifted endpoint agree with the model.

| Exact tip | Exceptional approach octants | Observed original,reversed | Model original,reversed |
| --- | --- | --- | --- |
| Upper | 0, 4 | 0,0 | 0,1 |
| Right | 1, 5, 6 | 0,1 | 0,0 |
| Right | 2 | 0,1 | 1,1 |
| Lower | 0, 4 | 0,1 | 0,0 |
| Left | 1 | 0,0 | 1,1 |

The logs are `/tmp/glmetal-wide-nonaxis-symbolic-apple.log` and
`/tmp/glmetal-wide-nonaxis-symbolic-test.log`. Results are under
`build/wide-nonaxis-symbolic/{apple,test}/results.tsv`. Each provider reports
four passing slanted-edge variants and four failing exact-tip variants.

These results reject a general upper-half boundary extension. In particular,
an upper tip can have no reversed coverage even though the formal predicate
classifies it as inside. The production extension therefore permits only upper
slanted edges, where both endpoint offsets from the sample center are nonzero.
It also excludes axis-aligned geometry and requires the opposite endpoint to
lie strictly outside the diamond. The existing candidate formula is unchanged.
All exact tips remain unsupported. Reproducing their native tie rule requires
additional evidence.

`run_wide_terminal_cpu.sh` extracts the actual production helper and checks
every one of these 192 native-probed pairs. Any accepted candidate must have no
forward coverage and reversed coverage under the formal model. The normal
`legacy_wide_boundary.*` regressions cover slanted upper and lower edges with
blending, stencil duplicate detection, and application scissor restoration.
