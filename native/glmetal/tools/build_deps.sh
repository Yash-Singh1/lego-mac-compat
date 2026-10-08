#!/bin/sh
# Fetches glslang and SPIRV-Cross at pinned releases and builds them as
# universal (x86_64 + arm64) static libraries under third_party/install.
# Homebrew's copies are arm64 only, and GLMetal also runs inside Rosetta
# processes.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
deps="$here/third_party"
install="$deps/install"
GLSLANG_TAG=16.6.0
SPIRV_CROSS_TAG=vulkan-sdk-1.4.357.0
mkdir -p "$deps"

fetch() {
    name=$1 url=$2 tag=$3
    if [ "$(cat "$deps/$name/.glmetal-tag" 2>/dev/null || true)" != "$tag" ]; then
        rm -rf "$deps/$name"
        git clone -q --depth 1 --branch "$tag" "$url" "$deps/$name"
        echo "$tag" > "$deps/$name/.glmetal-tag"
        rm -rf "$deps/build-$name"
    fi
}

fetch glslang https://github.com/KhronosGroup/glslang.git "$GLSLANG_TAG"
fetch SPIRV-Cross https://github.com/KhronosGroup/SPIRV-Cross.git "$SPIRV_CROSS_TAG"

common="-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64;arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
        -DCMAKE_INSTALL_PREFIX=$install -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON"

if [ ! -f "$install/lib/libglslang.a" ]; then
    # shellcheck disable=SC2086
    cmake -S "$deps/glslang" -B "$deps/build-glslang" -G Ninja $common -DENABLE_OPT=OFF \
        -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=ON -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF
    cmake --build "$deps/build-glslang"
    cmake --install "$deps/build-glslang"
fi

if [ ! -f "$install/lib/libspirv-cross-msl.a" ]; then
    # shellcheck disable=SC2086
    cmake -S "$deps/SPIRV-Cross" -B "$deps/build-SPIRV-Cross" -G Ninja $common \
        -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF \
        -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=ON -DSPIRV_CROSS_ENABLE_UTIL=OFF \
        -DSPIRV_CROSS_ENABLE_C_API=OFF
    cmake --build "$deps/build-SPIRV-Cross"
    cmake --install "$deps/build-SPIRV-Cross"
fi
echo "dependencies in $install"
