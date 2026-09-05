# Experimental Rosetta x87 runtime

`rosettax87-preserve-simd.patch` applies to
[Lifeisawful/rosettax87_jit](https://github.com/Lifeisawful/rosettax87_jit)
commit `a44f1eff28591a90f4a8c7297134294811da06b5`.
It is an optional dependency patch; normal Portal launches still use stock
Rosetta. No game binary is patched by this file.

On macOS 26.5.2, Rosetta version `0x16f0240000000`, our Release build exposed
three problems:

- The translation hook overwrote live guest SIMD registers. The first
  arithmetic check reported the bits `0x0706050403020100` instead of `3.0`.
  A cold call passing an ordinary SSE value reproduced the same corruption.
- The decoder hook also overwrote live SIMD state. Portal crashed in
  `vImageConvert_ChunkyToPlanar8` while AppKit drew its window frame.
  Disabling this hook removed that crash. Both hooks now preserve all
  128 bits of the SIMD registers, FPCR and FPSR at an assembly boundary.
- Compiler recognition of memory-clearing loops produced recursive
  `memset`/`bzero` implementations in the freestanding runtime. Disassembly
  and a process sample confirmed the resulting infinite loop. The relevant
  builtin transformations are disabled. `-fno-stack-check` and a `bzero`
  wrapper also accommodate the current Xcode toolchain.

The register-save cost occurs during translation, not on every execution
of an optimized x87 instruction. The patch does not enable fast rounding,
reduced precision, approximate reciprocal division or SIP-check bypasses.

## Reproduce

Run from `native/`, with Xcode command-line tools, CMake and Rosetta installed:

```sh
git clone https://github.com/Lifeisawful/rosettax87_jit build/rosettax87_jit
git -C build/rosettax87_jit checkout --detach a44f1eff28591a90f4a8c7297134294811da06b5
git -C build/rosettax87_jit apply ../../patches/rosettax87-preserve-simd.patch
cmake -S build/rosettax87_jit -B build/rosettax87_jit/build -DCMAKE_BUILD_TYPE=Release
cmake --build build/rosettax87_jit/build --target runtime_loader libRuntimeRosettax87 test_arith_x86_64 -j 4
xcrun clang -arch x86_64 -O0 tests/test_rosettax87_cold.c -o build/test_rosettax87_cold
arch -x86_64 build/test_rosettax87_cold
build/rosettax87_jit/build/bin/runtime_loader "$PWD/build/test_rosettax87_cold"
build/rosettax87_jit/build/bin/runtime_loader "$PWD/build/test_rosettax87_cold" sse
build/rosettax87_jit/build/bin/runtime_loader "$PWD/build/rosettax87_jit/build/bin/test_arith"
```

The loader uses macOS debugging authorization to inject the runtime into
the launched process. This investigation required no system security changes.
After quitting an existing Portal session, an experimental launch is:

```sh
LP32_DISPLAY_INDEX=1 build/rosettax87_jit/build/bin/runtime_loader \
  "$PWD/build/Portal2-Compat.app/Contents/MacOS/Portal2Compat" \
  -fullscreen +fps_max 120 +cl_showfps 1 +load quick
```

The local investigation checkout is under `analysis/rosettax87_jit`; the
commands above create an independent copy under the ignored build directory.
See [PORTAL2.md](../PORTAL2.md#rosetta-x87-investigation) for game measurements.
