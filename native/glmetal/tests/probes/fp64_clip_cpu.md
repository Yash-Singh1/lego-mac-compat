# FP64 with post-vertex clipping

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_clip_cpu.sh
```

Four CPU fixtures combine a user clip plane with exact double or dvec4 UBO arrays in VS or FS. They check exact parent block reflection, internal capture reflection, pull-fragment reflection when applicable, and exact operations in generated child stages. Cache roundtrips preserve child shaders and layouts. Normal VS/FS, capture VS, compute kernel and pull VS/FS are compiled offline for each fixture, 24 Metal stages total. No GPU readback or submission is used.
