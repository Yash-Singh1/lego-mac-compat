# Unused double vertex output

```sh
GLM_PROBE_KEEP=1 sh tests/probes/run_fp64_unused_output_cpu.sh
```

Ordinary VS/FS shaders declaring a double VS output are checked with and without original transform feedback. Although FS does not consume the output, SPIRV-Cross originally emitted an illegal ulong raster output and offline Metal rejected the vertex function. The exact raster gate now rejects all double-containing VS output resources when FS is present. This diagnostic asserts whole-program fallback and matching original transform-feedback GL_FLOAT type/stride, then writes all generated stages for offline compilation. VS-only exact feedback remains independently covered by fp64_foundation_cpu.
