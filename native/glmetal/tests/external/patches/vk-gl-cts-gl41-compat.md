These patches repair CTS harness use of unsupported APIs and shader dialects in a GL4.1 core context. They do not change expected values or remove assertions.

`tests/external/glcts.sh build` applies these patches to the pinned CTS checkout before building:

- `vk-gl-cts-gl41-api-compat.patch` uses the GL4.0 subroutine stage name-length query instead of a GL4.3 program-interface query. It also queries the bound transform-feedback object's indexed buffer state instead of calling GL4.5 named-object getters.
- `vk-gl-cts-gl41-tf-dialect.patch` promotes GLSL1.30 templates to GLSL1.40 inside the transform-feedback program builder on GL3.2+ core contexts. Those contexts must support GLSL1.40 but may reject GLSL1.30. GL3.0 and compatibility contexts retain the original templates. All shader operations and test assertions remain unchanged.

- `vk-gl-cts-gl41-multisample-allocation.patch` selects the GL3.2 mutable multisample allocation API when immutable multisample storage is unavailable. Formats, sizes, sample counts and fixed-location requests are unchanged.
- `gl41-fp64-cleanup.patch` calls the image-unit cleanup API only when this test enabled its compute path. This prevents a null GL4.2 entry-point call in a GL4.1 context.
- `vk-gl-cts-packed-pixels-framebuffer-restore.patch` restores the caller's separate read and draw framebuffer bindings after temporary pixel readback.
- `vk-gl-cts-packed-depth-default-framebuffer.patch` initializes the packed depth/stencil fixture with the render context's actual default framebuffer. An offscreen context can use a nonzero framebuffer.
- `vk-gl-cts-packed-depth-component-version.patch` checks the pre-GL4.4 combined component-query result on GL4.1. The GL4.4 error assertion remains unchanged for newer contexts. See its companion document for the specification change.
- `vk-gl-cts-state-reset.patch` is the existing reset repair applied before the GL4.1 changes.

The CTS runner defaults to 200 cases per process and 250 milliseconds between processes. It retains each chunk's QPA log and attributes crashes to the case that actually started, even when CTS executes a different order than the input list. The `all` command refreshes the Apple baseline whenever the test binary, case list or headless library changes.

The pre-edit vendor files and their SHA256 checksums are retained locally under `build/probes/gl41-compat-originals`. CPU syntax checks used the existing Ninja target commands with output/dependency flags removed and `-fsyntax-only` added. Parent-controlled Apple and GLMetal runs determine the test results after these harness repairs; improvements should be reported separately from driver fixes.

`vk-gl-cts-rgtc-volume-fallback.patch` corrects the desktop TexImage3D validity
check for RGTC requests. Ordinary pixel uploads can use uncompressed fallback;
the target restrictions for encoded compressed uploads do not apply. See
[vk-gl-cts-rgtc-volume-fallback.md](vk-gl-cts-rgtc-volume-fallback.md) for the
specification and focused probes.
