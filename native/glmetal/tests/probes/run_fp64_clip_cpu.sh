#!/bin/sh
# CPU-only: no driver library build and no GPU execution.
set -eu
probe_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
probe_work=$(mktemp -d "${TMPDIR:-/tmp}/glmetal-fp64-clip.XXXXXX")
cleanup() {
    if [ "${GLM_PROBE_KEEP:-0}" = 1 ]; then
        printf 'Artifacts retained: %s\n' "$probe_work"
    else
        rm -rf -- "$probe_work"
    fi
}
trap cleanup EXIT HUP INT TERM
cd "$probe_root"
clang++ -std=c++17 -Ithird_party/install/include -Ithird_party/glslang -Isrc -Ibuild/gen \
    tests/probes/fp64_clip_cpu.cpp \
    third_party/install/lib/libglslang.a \
    third_party/install/lib/libMachineIndependent.a \
    third_party/install/lib/libGenericCodeGen.a \
    third_party/install/lib/libOSDependent.a \
    third_party/install/lib/libSPIRV.a \
    third_party/install/lib/libglslang-default-resource-limits.a \
    third_party/install/lib/libspirv-cross-msl.a \
    third_party/install/lib/libspirv-cross-glsl.a \
    third_party/install/lib/libspirv-cross-core.a \
    -o "$probe_work/fp64_clip_cpu"
"$probe_work/fp64_clip_cpu" "$probe_work"

while IFS= read -r probe_stage; do
    xcrun metal -std=macos-metal2.3 -c "$probe_work/$probe_stage" -o "$probe_work/$probe_stage.air"
done < "$probe_work/manifest.txt"
