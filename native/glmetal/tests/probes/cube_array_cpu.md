# Cube array compiler probes

Run `tests/probes/run_cube_array_cpu.sh`. It creates a fresh temporary artifact
directory and runs seven actual compiler/cache fixtures. It validates native
cube-array and hidden face-array resources, then compiles fourteen stages
offline as Metal2.3. No driver build or GPU execution occurs.

Fixtures cover implicit sampling, bias, explicit LOD, explicit gradients,
LOD query, sampler function arguments and dynamically indexed sampler arrays.
The generated helpers round and clamp the cube layer, select its six-face
region, and preserve native sampling for seamless or native seamful clamp
state. Integer and shadow cube arrays remain on their previous native paths.

Set `GLM_PROBE_KEEP=1` to retain generated sources and artifacts. Runtime
projection comparison is covered separately by
`core_cube_projection_diagnostic.*` in the pixel runner.
