#!/bin/sh
# Differential Khronos CTS (VK-GL-CTS glcts, KHR-GL* core modules): runs a
# case list on Apple's OpenGL and on GLMetal (injected) and compares the
# results; any difference not in glcts-known-diffs.txt fails.
#
#   tests/external/glcts.sh [build|apple|glmetal|compare|update|all]   (default: all)
#
# CASELIST: mustpass list name (default gl41-main). glcts's macOS port
# creates core 3.2/4.1 contexts only. patches/vk-gl-cts-state-reset.patch
# keeps its state reset from aborting on GL errors both implementations
# raise. Apple's results are reused only with the same test binary and case list.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ext=$root/third_party/external
out=$root/build/external
CTS_SHA=9335a4e4e455ed600cb7fa486999e3ed6c2a0dbf
src=$ext/VK-GL-CTS
bld=$ext/VK-GL-CTS-build
caselist=${CASELIST:-gl41-main}
list=$src/external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass/main/$caselist.txt
known=$here/glcts-known-diffs.txt
python=${PYTHON:-python3}
. "$here/headless.sh"

build() {
    if [ ! -d "$src/.git" ]; then
        mkdir -p "$ext"
        git clone -q https://github.com/KhronosGroup/VK-GL-CTS.git "$src"
    fi
    if [ "$(git -C "$src" rev-parse HEAD)" != "$CTS_SHA" ]; then
        git -C "$src" checkout -q -- .
        git -C "$src" checkout -q "$CTS_SHA" 2>/dev/null || { git -C "$src" fetch -q origin "$CTS_SHA"; git -C "$src" checkout -q "$CTS_SHA"; }
    fi
    for patch_name in vk-gl-cts-state-reset.patch vk-gl-cts-gl41-api-compat.patch \
        vk-gl-cts-gl41-tf-dialect.patch vk-gl-cts-gl41-multisample-allocation.patch \
        vk-gl-cts-packed-pixels-framebuffer-restore.patch gl41-fp64-cleanup.patch \
        vk-gl-cts-packed-depth-default-framebuffer.patch \
        vk-gl-cts-packed-depth-component-version.patch \
        vk-gl-cts-transform-feedback-exception-diagnostics.patch \
        vk-gl-cts-transform-feedback-mismatch-diagnostics.patch \
        vk-gl-cts-rgtc-volume-fallback.patch \
        vk-gl-cts-depth-error-diagnostics.patch \
        vk-gl-cts-fp64-default-framebuffer.patch \
        vk-gl-cts-gl41-constructor-dialect.patch; do
        patch_file=$here/patches/$patch_name
        if git -C "$src" apply --check "$patch_file" 2>/dev/null; then
            git -C "$src" apply "$patch_file"
        elif ! git -C "$src" apply --reverse --check "$patch_file" 2>/dev/null; then
            echo "CTS patch does not apply cleanly: $patch_name" >&2
            exit 1
        fi
    done
    [ -d "$src/external/glslang/src" ] || (cd "$src" && "$python" external/fetch_sources.py >/dev/null)
    build_shim
    cmake -S "$src" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=osx -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_EXE_LINKER_FLAGS="$(shim_link_flags)" >/dev/null
    ninja -C "$bld" -j2 glcts >/dev/null
}

apple_signature() {
    shasum -a 256 "$bld/external/openglcts/modules/glcts" "$list" "$shim"
}

apple_baseline_current() {
    [ -f "$out/glcts-$caselist-apple.tsv" ] &&
    [ -f "$out/glcts-$caselist-apple.signature" ] &&
    [ "$(cat "$out/glcts-$caselist-apple.signature")" = "$(apple_signature)" ]
}

run() { # run NAME [inject]
    mkdir -p "$out"
    build_shim
    require_headless "$bld/external/openglcts/modules/glcts"
    if [ "${2:-}" = inject ]; then
        "$python" "$here/cts_runner.py" "$bld/external/openglcts/modules" "$list" "$out/glcts-$caselist-$1.tsv" \
            DYLD_INSERT_LIBRARIES="$root/build/libGLMetalInject.dylib"
    else
        "$python" "$here/cts_runner.py" "$bld/external/openglcts/modules" "$list" "$out/glcts-$caselist-$1.tsv"
        apple_signature > "$out/glcts-$caselist-apple.signature"
    fi
}

case ${1:-all} in
build) build ;;
apple) run apple ;;
glmetal) run glmetal inject ;;
compare) "$python" "$here/diff_results.py" "$out/glcts-$caselist-apple.tsv" "$out/glcts-$caselist-glmetal.tsv" "$known" ;;
update) "$python" "$here/diff_results.py" "$out/glcts-$caselist-apple.tsv" "$out/glcts-$caselist-glmetal.tsv" "$known" --update ;;
all)
    build
    apple_baseline_current || run apple
    run glmetal inject
    "$python" "$here/diff_results.py" "$out/glcts-$caselist-apple.tsv" "$out/glcts-$caselist-glmetal.tsv" "$known"
    ;;
*) echo "usage: $0 [build|apple|glmetal|compare|update|all]" >&2; exit 2 ;;
esac
