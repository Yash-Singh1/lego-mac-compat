# Original vertex-ID mapping

Run `sh tests/probes/run_vertex_id_mapping_cpu.sh` for CPU-only validation. It compiles the actual driver compiler without rebuilding or executing the driver, validates 24 transformed SPIR-V modules with spirv-val, and compiles 25 Metal stages offline.

The GLSL140 and GLSL410 fixtures cover direct references, helper functions, global initializers, loops, conditional expressions and nested helper/loop combinations. Both normal and transform-feedback capture variants use the hidden buffer at slot30. Generated raw-ID stages opt out.

The transformation inserts an independent structured helper function. Original control-flow blocks, loop headers and Phi predecessors remain unchanged. The hidden table load occurs only in the enabled branch.

Runtime mapping currently covers VS-only transform-feedback expansion. The table follows original submission order and includes the compact stream's original vertex base. Raster replay permutations index the same records. Raw capture ordinals continue to determine transform-feedback destinations. GS and tessellation expansion retain their existing behavior.

GLMPoint contains six scalar words at byte offsets 0 through 20. Its upload reserves 32 bytes for std140 block alignment. ID tables follow existing float4 vertex records in stream buffer 30; checked sizes cannot exceed the device buffer limit, size_t range or uint32 word-address range. Ordinary GPU vertex fetch binds a harmless placeholder at 30 when that slot has no real attribute, and mapping remains disabled.
