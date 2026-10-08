# Sample shading compiler probes

Run `tests/probes/run_sample_shading_cpu.sh`. It uses a fresh temporary directory,
compiles the actual driver compiler and cache code, and compiles twelve generated
stages offline as Metal2.3. It does not build the driver or run GPU work.
`GLM_PROBE_KEEP=1` retains the temporary artifacts.

Four fixtures cover implicit sample interpolation from SampleID and
SamplePosition, state-forced smooth/noperspective interpolation, and a shader
with only flat inputs. The checks preserve explicit centroid interpolation and
integer flat inputs, verify the dummy sample-ID parameter for flat-only input,
and roundtrip cached MSL. Original and patched fragment stages have independent
manifest entries. The state helper preserves raster position and renames the
selected entry to main0.
