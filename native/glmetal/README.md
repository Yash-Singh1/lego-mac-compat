# GLMetal

An OpenGL implementation (Apple's legacy 2.1 and core 4.1 profiles, CGL,
AGL and the NSOpenGL classes) written directly on Metal, tested pixel by
pixel against Apple's OpenGL. See [DESIGN.md](DESIGN.md).

## Build

```sh
tools/build_deps.sh      # glslang + SPIRV-Cross, universal static libs in third_party/
make lib                 # build/libGLMetal.dylib (x86_64 + arm64) and build/glmetal-compiler (arm64)
make check               # pixel comparison suite against Apple's OpenGL
```

`build/glmetal-compiler` must sit next to `libGLMetal.dylib`: processes
running under Rosetta compile shaders in it natively.

## Layout

- `src/` the implementation; `src/marshal/` the threaded command stream
- `tools/` code generators, dependency build, `apple_dump.c` (dumps Apple's
  implementation-dependent values into `data/apple-gl-reference.txt`)
- `data/` Apple's OpenGL/CGL export lists and reference values
- `tests/glcompare/` pixel suite; `tests/bench/` draw benchmark;
  `tests/window/` windowed smoke test
