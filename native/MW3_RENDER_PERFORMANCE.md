# MW3 rendering performance, October 10, 2026

## Measured bottleneck

A read-only five-second sample of the running MW3 render thread recorded
2,880 samples. Index publication appeared in 1,179 samples, 40.9%, and
`glBufferData` in 1,170, 40.6%. New shared Metal buffer allocation appeared
in 769 samples, 26.7%; 611 samples, 21.2%, reached `IOGPUResourceCreate`.
These are nested categories, not independent costs to add together.
The GL command worker waited on its condition variable in 1,747 samples,
60.7%. The producer frequently spent time allocating buffers instead of
feeding the worker. This steady-state sample contained little shader
frontend compilation.

The compatibility bridge kept dynamic indices in a CPU shadow, then made
a separate `glBufferData` publication for every draw range. That avoided
expensive updates of the original large buffer, but each publication requested
separate Metal storage. GLMetal already had a storage pool; the live profile
shows allocation remained costly under this game's workload.

## Index arena

Replacement GL backends now append draw ranges to a 2 MB index arena. The
arena grows for a larger range and orphans on wrap or context change. Previously
submitted draws retain their original storage. Appends use at most 64 KB per
`glBufferSubData` command, allowing GLMetal to snapshot bytes inline instead
of allocating a staging Metal buffer. The bridge restores the game's original
element-buffer binding after each draw.

Identical consecutive publications can reuse their offset. A source generation
invalidates reuse after writes, aliases, orphaning, deletion or name reuse.
Context changes and destruction invalidate it too. The Apple path keeps its
existing publication method. `LP32_NO_INDEX_ARENA=1` disables the arena for
comparisons; `LP32_INDEX_STATS=1` reports publication and orphan counts.

## Generic sampler metadata

GLMetal now caches immutable sampler reflection in 32 bounded thread-local
plans. Each plan keys on the program, unique successful link serial and
reflection storage. Live texture units, sampler objects, texture formats,
mip views and depth metadata remain draw state. Collisions rebuild the plan;
programs with more than 128 sampler uniforms use the original scan.
Programs without border emulation skip an unnecessary shadow-sampler scan.
This changes no shader output or compiler cache identity.

The CPU probe replayed 32 captured MW3 programs with 660 uniforms and
160 sampler uniforms for 200,000 metadata iterations. Rosetta trials took
19.904 to 32.814 ms with the original scan and 5.184 to 9.134 ms with plans,
with identical checksums. Concurrent user activity contributes timing variation.
This is metadata processing time, not total texture binding or gameplay FPS.

## Validation and performance limits

`python3 native/tests/test_index_shadow_reuse.py` passed with the arena on
and off. It covers source writes, range/type/count changes, aliases, buffer
lifetimes, shared contexts, arena mutation, wrapping and upload chunk limits.
The native `LP32_GL_BUFFER_SELFTEST=1` also passed against the packaged driver.

`python3 native/tests/test_index_shadow_gpu.py` uses the production index
publication code with real GLMetal command-thread rendering. Each cohort
issues 131,072 measured draws in 256 offscreen frames. Pixel checks include
the frame whose queued draws straddle the 2 MB arena wrap. Both architectures
and both arena modes produced zero bad pixels and no GL errors.

| Architecture | Arena off, submission | Arena on, submission | Reduction |
| --- | ---: | ---: | ---: |
| Rosetta x86_64 | 352.563 ms | 212.107 ms | 39.8% |
| Native arm64 | 191.530 ms | 125.465 ms | 34.5% |

Completion times were 353.623 versus 217.744 ms on Rosetta and 192.286
versus 128.413 ms on arm64. The preceding cohort also passed all pixels,
with submission reductions of 39.5% and 38.6%. These are isolated index
publication workloads, not gameplay FPS claims. Their small draw ranges
exercise an already effective storage pool; real game ranges and GPU work differ.

Fifty focused comparisons against Apple GL passed, 25 per architecture.
They cover small indexed streams, sampler mutations, LOD and sampler arrays,
shadow references and ARB fragment programs. No comparison failed or crashed.
The disk shader cache was disabled for these comparisons.

Private muted checkpoint runs did not reliably leave the menu while another
MW3 instance was active. Menu timings are excluded from gameplay performance
results. Shader precompilation still helps first-use linking, but does not
remove index publication, asset streaming or every native pipeline creation.
This change addresses the measured index-allocation bottleneck; it does not
establish that all multi-second gameplay stalls are gone.

The normal `native/build/MW3-Compat.app` receives the new signed loader,
driver and compiler helper by atomic file replacement. An already running
game keeps its old mapped binaries. Quit and reopen the app to load the update.
Tests used copied profiles and muted background windows. User applications,
profile files and the game's assets were not changed by the tests.
