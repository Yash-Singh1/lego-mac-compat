#!/bin/sh
# Fetches Mesa at the revision in mesa.rev, applies patches/, and builds:
#   1. the build-time compilers (mesa_clc, kk_clc, vtn_bindgen2) for this Mac;
#   2. Zink, the DRI frontend and KosmicKrisp for the requested architecture.
#
# usage: build_mesa.sh <work-dir> <arch: x86_64|arm64>
# Requires Homebrew llvm, libclc, spirv-llvm-translator, spirv-tools,
# molten-vk (headers only), meson, ninja, glslang and python3 with mako,
# pyyaml and packaging.  Output: <work-dir>/build-<arch>/.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?work dir}
arch=${2:?arch}
revision=$(cat "$here/mesa.rev")
brew_prefix=$(brew --prefix)

mkdir -p "$work"
work=$(cd "$work" && pwd)
source="$work/mesa"
if [ ! -d "$source/.git" ]; then
    git init -q "$source"
    git -C "$source" remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
fi
stamp="$revision $(cat "$here"/patches/*.patch | shasum | cut -d' ' -f1)"
if [ "$(cat "$source/.lp32gl-stamp" 2>/dev/null || true)" != "$stamp" ]; then
    git -C "$source" fetch -q --depth 1 origin "$revision"
    git -C "$source" checkout -q --force "$revision"
    git -C "$source" clean -qfdx
    for patch in "$here"/patches/*.patch; do
        git -C "$source" apply "$patch"
    done
    echo "$stamp" > "$source/.lp32gl-stamp"
    rm -rf "$work/build-tools" "$work/build-$arch"
fi

export PATH="$brew_prefix/opt/llvm/bin:$PATH"
export CC=/usr/bin/clang CXX=/usr/bin/clang++ OBJC=/usr/bin/clang OBJCXX=/usr/bin/clang++

common="-Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers=zink
        -Dglx=disabled -Degl=enabled -Dgles1=disabled -Dgles2=disabled -Dopengl=true
        -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled
        -Dbuild-tests=false -Dmoltenvk-dir=$brew_prefix/opt/molten-vk
        -Dvulkan-loader-rpath=@loader_path"

tools="$work/build-tools"
if [ ! -f "$tools/build.ninja" ]; then
    # shellcheck disable=SC2086
    meson setup "$tools" "$source" --buildtype=release $common \
        -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled
fi
ninja -C "$tools" src/compiler/clc/mesa_clc src/kosmickrisp/clc/kk_clc \
    src/compiler/spirv/vtn_bindgen2

build="$work/build-$arch"
if [ "$arch" = "$(uname -m)" ]; then
    cross=""
else
    cross="--cross-file $here/cross-darwin-$arch.ini"
fi
if [ ! -f "$build/build.ninja" ]; then
    # shellcheck disable=SC2086
    PATH="$tools/src/compiler/clc:$tools/src/kosmickrisp/clc:$tools/src/compiler/spirv:$PATH" \
    meson setup "$build" "$source" $cross --buildtype=release -Db_ndebug=true $common \
        -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system
fi
PATH="$tools/src/compiler/clc:$tools/src/kosmickrisp/clc:$tools/src/compiler/spirv:$PATH" \
    ninja -C "$build"
