# Fixed-function cube shader probes

Run `tests/probes/run_ff_cube_cpu.sh`. It builds the actual C fixed-function
shader generator and reuses the compiler's shared cube helper text. It does not
build the GL driver or run GPU work. A fresh temporary directory contains four
independent shader libraries, each with vertex and fragment entries. All four
libraries compile offline as Metal2.3. `GLM_PROBE_KEEP=1` retains artifacts.

Fixtures cover ordinary cube coordinates, reflection texgen, cube units 0 and 7
mixed with a bordered 2D unit, and fixed-function fragment processing with all
legacy vertex outputs. Checks verify face texture aliases 64 and 71, fragment-only
LOD metadata buffer 25, and the shared cube sampling call. Uniform buffer 16 and
ordinary sampler slots remain unchanged.

GPU coverage is separate in `legacy_cube_sampling_state.*`. Its three variants
switch repeat/clamp/seamless state between draws, exercise mipmapped texture
environment bias, and combine two cube units. The original legacy reflection
case remains an independent regression.
