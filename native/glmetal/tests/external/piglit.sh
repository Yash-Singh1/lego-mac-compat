#!/bin/sh
# Differential piglit: runs a piglit selection on Apple's OpenGL and on
# GLMetal (injected with libGLMetalInject.dylib) and compares the results;
# any difference not in piglit-known-diffs.txt fails.
#
#   tests/external/piglit.sh [build|apple|glmetal|compare|update|all]   (default: all)
#
# Tests never show a window or take focus: headless_shim.m (linked into
# every test executable, see headless.sh) keeps their GLUT windows
# transparent and the apps inactive; run refuses binaries without it.
#
# PIGLIT_TESTS: -t regex selection (default below); ARCH=x86_64 runs an
# x86_64 build under Rosetta. Sources and build live in third_party/external,
# results in build/external. Apple's results are reused once recorded
# (delete build/external/piglit-apple to record them again).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ext=$root/third_party/external
out=$root/build/external
arch=${ARCH:-arm64}
PIGLIT_SHA=cff53d1a1054e50b75348c8fd686d77947a26bc9
src=$ext/piglit
bld=$ext/piglit-build-$arch
known=$here/piglit-known-diffs.txt
tests=${PIGLIT_TESTS:-'spec@glsl-1\.10|spec@glsl-1\.20|spec@glsl-1\.30|spec@glsl-1\.40|spec@glsl-1\.50|spec@arb_vertex_program|spec@arb_fragment_program|spec@!opengl 1\.|spec@!opengl 2\.|spec@!opengl 3\.|spec@arb_occlusion_query|spec@arb_texture_rectangle|spec@ext_framebuffer_object|spec@arb_framebuffer_object'}
python=${PYTHON:-$(command -v python3)}  # needs numpy and mako

. "$here/headless.sh"

build() {
    if [ ! -d "$src/.git" ]; then
        mkdir -p "$ext"
        git clone -q https://gitlab.freedesktop.org/mesa/piglit.git "$src"
    fi
    git -C "$src" checkout -q "$PIGLIT_SHA" 2>/dev/null || { git -C "$src" fetch -q origin "$PIGLIT_SHA"; git -C "$src" checkout -q "$PIGLIT_SHA"; }
    build_shim
    cmake -S "$src" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_EXE_LINKER_FLAGS="$(shim_link_flags)" \
        -DPIGLIT_BUILD_GL_TESTS=ON -DPIGLIT_BUILD_GLX_TESTS=OFF -DPIGLIT_BUILD_EGL_TESTS=OFF \
        -DPIGLIT_BUILD_GLES1_TESTS=OFF -DPIGLIT_BUILD_GLES2_TESTS=OFF -DPIGLIT_BUILD_GLES3_TESTS=OFF \
        -DPIGLIT_BUILD_CL_TESTS=OFF -DPIGLIT_BUILD_VK_TESTS=OFF -DPIGLIT_USE_WAFFLE=OFF \
        -DPYTHON_EXECUTABLE="$python" -DPython3_EXECUTABLE="$python" >/dev/null
    ninja -C "$bld" >/dev/null
}

run() { # run NAME [inject]
    build_shim
    require_headless "$bld"/bin/*
    rm -rf "$out/piglit-$1"
    mkdir -p "$out"
    filter=$(printf '%s' "$tests" | tr '|' '\n' | sed 's/^/-t\n/')
    set -f
    old_ifs=$IFS
    IFS='
'
    if [ "${2:-}" = inject ]; then
        env PIGLIT_BUILD_DIR="$bld" PIGLIT_NO_FAST_SKIP=1 DYLD_INSERT_LIBRARIES="$root/build/libGLMetalInject.dylib" \
            "$python" "$src/piglit" run gpu $filter -j8 --timeout 120 "$out/piglit-$1" >/dev/null
    else
        env PIGLIT_BUILD_DIR="$bld" PIGLIT_NO_FAST_SKIP=1 \
            "$python" "$src/piglit" run gpu $filter -j8 --timeout 120 "$out/piglit-$1" >/dev/null
    fi
    IFS=$old_ifs
    set +f
}

case ${1:-all} in
build) build ;;
apple) run apple ;;
glmetal) run glmetal inject ;;
compare) "$python" "$here/diff_results.py" "$out/piglit-apple" "$out/piglit-glmetal" "$known" ;;
update) "$python" "$here/diff_results.py" "$out/piglit-apple" "$out/piglit-glmetal" "$known" --update ;;
all)
    build
    [ -d "$out/piglit-apple" ] || run apple
    run glmetal inject
    "$python" "$here/diff_results.py" "$out/piglit-apple" "$out/piglit-glmetal" "$known"
    ;;
*) echo "usage: $0 [build|apple|glmetal|compare|update|all]" >&2; exit 2 ;;
esac
